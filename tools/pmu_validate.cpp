// pmu_validate: empirically validates lob::bench::PmuCounters on THIS
// machine before any benchmark result is allowed to depend on it. Prints
// raw numbers for every check -- this is meant to be read, not just
// trusted because it exited 0.
//
// Must run under sudo (root is required for kpc_set_counting /
// kpc_set_thread_counting).
#include <mach/mach_time.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "lob/bench/pmu.hpp"

using lob::bench::PmuCounters;
using lob::bench::PmuSnapshot;

namespace {

// A loop with a STATICALLY KNOWN instruction count: one MOV, then N
// iterations of (SUBS + B.NE), each iteration always executing both
// instructions regardless of whether the branch is taken. Hand-written
// in inline asm specifically so no compiler optimization (unrolling,
// vectorization, loop-count-elision) can change the instruction count
// out from under this test -- the entire point is having a ground truth
// to check the counter against.
[[gnu::noinline]] void KnownInstructionCountLoop(std::uint64_t n) {
    asm volatile(
        "mov x0, %[n]\n"
        "1:\n"
        "subs x0, x0, #1\n"
        "b.ne 1b\n"
        :
        : [n] "r"(n)
        : "x0", "cc");
}

constexpr std::uint64_t ExpectedInstructionsFor(std::uint64_t n) {
    return 2 * n + 1;  // n * (SUBS + B.NE) + the initial MOV
}

double PercentError(double measured, double expected) {
    return 100.0 * (measured - expected) / expected;
}

void PrintSnapshotDelta(const char* label, const PmuSnapshot& before, const PmuSnapshot& after) {
    const std::uint64_t cycles = after.cycles - before.cycles;
    const std::uint64_t instructions = after.instructions - before.instructions;
    std::printf(
        "  %-28s cycles=%llu instructions=%llu ipc=%.3f\n", label,
        static_cast<unsigned long long>(cycles), static_cast<unsigned long long>(instructions),
        instructions > 0 ? static_cast<double>(instructions) / static_cast<double>(cycles) : 0.0);
}

int Run() {
    std::printf("=== PMU validation ===\n\n");

    PmuCounters pmu;
    std::printf("CPU string reported by kpc_cpu_string(): %s\n", pmu.CpuString().c_str());
    std::printf("Branch mispredicts configurable counter available: %s\n",
                pmu.IsBranchMispredictsAvailable() ? "yes" : "no");
    std::printf("L1D cache refills configurable counter available:  %s\n\n",
                pmu.IsL1dCacheRefillsAvailable() ? "yes" : "no");

    bool cycles_trustworthy = true;
    bool branch_mispredicts_trustworthy = pmu.IsBranchMispredictsAvailable();
    bool l1d_cache_refills_trustworthy = pmu.IsL1dCacheRefillsAvailable();

    // --- Test 1: known instruction count -------------------------------
    std::printf("--- Test 1: known instruction count ---\n");
    {
        constexpr std::uint64_t kN = 10'000'000ull;
        const std::uint64_t expected = ExpectedInstructionsFor(kN);

        KnownInstructionCountLoop(1000);  // warm up icache/branch predictor state
        const auto before = pmu.Read();
        KnownInstructionCountLoop(kN);
        const auto after = pmu.Read();
        const std::uint64_t measured = after.instructions - before.instructions;

        const double err =
            PercentError(static_cast<double>(measured), static_cast<double>(expected));
        std::printf("  n=%llu expected_instructions=%llu measured_instructions=%llu error=%.2f%%\n",
                    static_cast<unsigned long long>(kN), static_cast<unsigned long long>(expected),
                    static_cast<unsigned long long>(measured), err);
        if (std::abs(err) > 5.0) {
            std::printf("  FAIL: >5%% error -- instructions-retired counter is not trustworthy\n");
            cycles_trustworthy =
                false;  // if instructions is this wrong, don't trust the setup at all
        } else {
            std::printf("  PASS\n");
        }
    }
    std::printf("\n");

    // --- Test 2: linearity (double the work -> ~double the cycles) -----
    std::printf("--- Test 2: linearity (double work => ~double cycles) ---\n");
    {
        constexpr std::uint64_t kN = 20'000'000ull;
        KnownInstructionCountLoop(1000);
        const auto b1 = pmu.Read();
        KnownInstructionCountLoop(kN);
        const auto a1 = pmu.Read();
        const std::uint64_t cycles_1x = a1.cycles - b1.cycles;

        const auto b2 = pmu.Read();
        KnownInstructionCountLoop(kN * 2);
        const auto a2 = pmu.Read();
        const std::uint64_t cycles_2x = a2.cycles - b2.cycles;

        const double ratio = static_cast<double>(cycles_2x) / static_cast<double>(cycles_1x);
        std::printf("  1x cycles=%llu  2x cycles=%llu  ratio=%.3f (expect ~2.0)\n",
                    static_cast<unsigned long long>(cycles_1x),
                    static_cast<unsigned long long>(cycles_2x), ratio);
        if (ratio < 1.8 || ratio > 2.2) {
            std::printf("  FAIL: ratio outside [1.8, 2.2] -- cycles counter is not trustworthy\n");
            cycles_trustworthy = false;
        } else {
            std::printf("  PASS\n");
        }
    }
    std::printf("\n");

    // --- Test 3: IPC plausibility ---------------------------------------
    std::printf("--- Test 3: IPC plausibility (expect roughly 1-6 on this core) ---\n");
    {
        constexpr std::uint64_t kN = 20'000'000ull;
        KnownInstructionCountLoop(1000);
        const auto before = pmu.Read();
        KnownInstructionCountLoop(kN);
        const auto after = pmu.Read();
        const std::uint64_t cycles = after.cycles - before.cycles;
        const std::uint64_t instructions = after.instructions - before.instructions;
        const double ipc = static_cast<double>(instructions) / static_cast<double>(cycles);
        std::printf("  cycles=%llu instructions=%llu ipc=%.3f\n",
                    static_cast<unsigned long long>(cycles),
                    static_cast<unsigned long long>(instructions), ipc);
        if (ipc < 0.1 || ipc > 8.0) {
            std::printf("  FAIL: IPC wildly implausible -- wrong event code or broken setup\n");
            cycles_trustworthy = false;
        } else {
            std::printf(
                "  PASS (this loop is a tight SUBS/B.NE dependency chain, so IPC well under "
                "1.0 is actually expected here -- the plausibility bound is to catch an "
                "event code that's wrong by orders of magnitude, not to demand high IPC "
                "from a deliberately serial loop)\n");
        }
    }
    std::printf("\n");

    // --- Test 4: implied frequency vs mach_absolute_time, LONG run -----
    std::printf("--- Test 4: implied frequency vs mach_absolute_time (long run) ---\n");
    {
        mach_timebase_info_data_t timebase{};
        mach_timebase_info(&timebase);

        constexpr std::uint64_t kN =
            2'000'000'000ull;  // long enough that 41ns wall-clock granularity is irrelevant
        const auto before = pmu.Read();
        const std::uint64_t wall_before = mach_absolute_time();
        KnownInstructionCountLoop(kN);
        const std::uint64_t wall_after = mach_absolute_time();
        const auto after = pmu.Read();

        const std::uint64_t cycles = after.cycles - before.cycles;
        const double wall_ns = static_cast<double>(wall_after - wall_before) *
                               static_cast<double>(timebase.numer) /
                               static_cast<double>(timebase.denom);
        const double wall_seconds = wall_ns / 1e9;
        const double implied_ghz = (static_cast<double>(cycles) / wall_seconds) / 1e9;

        std::printf("  cycles=%llu wall_time=%.4fs implied_frequency=%.3f GHz\n",
                    static_cast<unsigned long long>(cycles), wall_seconds, implied_ghz);
        // Apple M4: P-cores are publicly documented in the 4.0-4.5GHz
        // range, E-cores substantially lower (~2.6-2.9GHz). QOS is not set
        // in this validation binary specifically so this number also
        // tells us which kind of core the scheduler happened to run this
        // on -- see the QOS_CLASS_USER_INTERACTIVE note in the benchmark
        // harness itself.
        if (implied_ghz < 0.5 || implied_ghz > 6.0) {
            std::printf(
                "  FAIL: implied frequency outside a plausible range for any Apple Silicon core "
                "-- cycles counter is not trustworthy\n");
            cycles_trustworthy = false;
        } else {
            std::printf("  PASS (plausible for %s)\n",
                        implied_ghz > 3.2 ? "a P-core" : "an E-core, or a P-core that throttled");
        }
    }
    std::printf("\n");

    // --- Configurable counter check: branch mispredicts -----------------
    if (pmu.IsBranchMispredictsAvailable()) {
        std::printf("--- Branch mispredicts sanity check ---\n");
        std::mt19937_64 rng(42);
        std::vector<int> random_bits(1'000'000);
        for (auto& b : random_bits) {
            b = static_cast<int>(rng() & 1);
        }
        volatile std::uint64_t sink = 0;

        const auto before = pmu.Read();
        for (int b : random_bits) {
            // Genuinely data-dependent, unpredictable branch.
            if (b != 0) {
                sink += 1;
            } else {
                sink += 2;
            }
        }
        const auto after = pmu.Read();
        PrintSnapshotDelta("random (unpredictable) branches", before, after);
        const std::uint64_t mispredicts =
            after.branch_mispredicts && before.branch_mispredicts
                ? *after.branch_mispredicts - *before.branch_mispredicts
                : 0;
        std::printf("  branch_mispredicts=%llu out of %zu random branches\n",
                    static_cast<unsigned long long>(mispredicts), random_bits.size());
        // A real branch predictor gets truly-random data-dependent
        // branches wrong a substantial fraction of the time (order tens
        // of percent) -- near-zero here would mean the event isn't
        // actually counting mispredicts.
        if (mispredicts < random_bits.size() / 20) {
            std::printf(
                "  FAIL: mispredict count implausibly low for random branches -- "
                "ARM_BR_MIS_PRED does not look trustworthy on this chip\n");
            branch_mispredicts_trustworthy = false;
        } else {
            std::printf("  PASS\n");
        }
        std::printf("\n");
    }

    // --- Configurable counter check: L1D cache refills -------------------
    if (pmu.IsL1dCacheRefillsAvailable()) {
        std::printf("--- L1D cache refills sanity check ---\n");
        constexpr std::size_t kSmall = 1024;             // 8KB, fits comfortably in L1D
        constexpr std::size_t kLarge = 8 * 1024 * 1024;  // 64MB, far exceeds L1D
        std::vector<std::uint64_t> small_buf(kSmall, 1);
        std::vector<std::uint64_t> large_buf(kLarge, 1);
        volatile std::uint64_t sink = 0;

        // Warm up, then measure repeated full-buffer sweeps for each.
        for (int rep = 0; rep < 3; ++rep) {
            for (auto v : small_buf) {
                sink += v;
            }
        }
        const auto b_small = pmu.Read();
        for (int rep = 0; rep < 50; ++rep) {
            for (auto v : small_buf) {
                sink += v;
            }
        }
        const auto a_small = pmu.Read();

        for (int rep = 0; rep < 3; ++rep) {
            for (auto v : large_buf) {
                sink += v;
            }
        }
        const auto b_large = pmu.Read();
        for (int rep = 0; rep < 50; ++rep) {
            for (auto v : large_buf) {
                sink += v;
            }
        }
        const auto a_large = pmu.Read();

        const std::uint64_t refills_small =
            a_small.l1d_cache_refills && b_small.l1d_cache_refills
                ? *a_small.l1d_cache_refills - *b_small.l1d_cache_refills
                : 0;
        const std::uint64_t refills_large =
            a_large.l1d_cache_refills && b_large.l1d_cache_refills
                ? *a_large.l1d_cache_refills - *b_large.l1d_cache_refills
                : 0;
        const std::uint64_t elements_small = static_cast<std::uint64_t>(kSmall) * 50;
        const std::uint64_t elements_large = static_cast<std::uint64_t>(kLarge) * 50;

        std::printf(
            "  small buffer (8KB, fits L1D):  refills=%llu over %llu accesses "
            "(%.5f refills/access)\n",
            static_cast<unsigned long long>(refills_small),
            static_cast<unsigned long long>(elements_small),
            static_cast<double>(refills_small) / static_cast<double>(elements_small));
        std::printf(
            "  large buffer (64MB, exceeds L1D): refills=%llu over %llu accesses "
            "(%.5f refills/access)\n",
            static_cast<unsigned long long>(refills_large),
            static_cast<unsigned long long>(elements_large),
            static_cast<double>(refills_large) / static_cast<double>(elements_large));
        const double small_rate =
            static_cast<double>(refills_small) / static_cast<double>(elements_small);
        const double large_rate =
            static_cast<double>(refills_large) / static_cast<double>(elements_large);
        // The large buffer must show meaningfully MORE refills per access
        // than the small one -- if it doesn't, the event isn't actually
        // distinguishing cache-resident from cache-missing accesses.
        if (large_rate < small_rate * 3.0) {
            std::printf(
                "  FAIL: large-buffer refill rate not meaningfully higher than small-buffer -- "
                "ARM_L1D_CACHE_REFILL does not look trustworthy on this chip\n");
            l1d_cache_refills_trustworthy = false;
        } else {
            std::printf("  PASS (large buffer shows %.1fx the refill rate of the small one)\n",
                        large_rate / small_rate);
        }
        std::printf("\n");
        if (sink == 0) {
            std::printf("");  // keep sink live, never actually zero here
        }
    }

    // --- Verdict ----------------------------------------------------------
    std::printf("=== Verdict ===\n");
    std::printf("cycles:               %s\n", cycles_trustworthy ? "TRUSTED" : "NOT TRUSTED");
    std::printf("instructions:         %s\n", cycles_trustworthy ? "TRUSTED" : "NOT TRUSTED");
    std::printf("branch_mispredicts:   %s\n",
                !pmu.IsBranchMispredictsAvailable() ? "UNAVAILABLE (event not configured)"
                : branch_mispredicts_trustworthy    ? "TRUSTED"
                                                    : "NOT TRUSTED -- failed sanity check");
    std::printf("l1d_cache_refills:    %s\n",
                !pmu.IsL1dCacheRefillsAvailable() ? "UNAVAILABLE (event not configured)"
                : l1d_cache_refills_trustworthy   ? "TRUSTED"
                                                  : "NOT TRUSTED -- failed sanity check");

    return cycles_trustworthy ? 0 : 1;
}

}  // namespace

int main() {
    try {
        return Run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pmu_validate: %s\n", e.what());
        return 1;
    }
}
