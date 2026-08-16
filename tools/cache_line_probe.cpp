// cache_line_probe: measures the ACTUAL L1D refill granularity on this
// machine via the dependent-two-load-at-varying-offset technique -- the
// same method a published measurement of the Apple M4 Pro used (see
// docs/cache_hierarchy_m4.md for the source and full writeup). Everything
// about struct layout, false-sharing padding, and intrusive-node sizing
// in Phase 5 depends on this number; it should be measured on THIS chip,
// not assumed from secondary sources about a different M-series model.
//
// Method: two loads, at `base` and `base+offset`, issued via `volatile`
// pointers (so the compiler cannot fuse or eliminate either access). If
// both addresses fall in the same cache line, the pair is expected to
// cost ONE L1D refill; if they cross a line boundary, TWO. Sweeping
// `offset` finds where that transition happens.
//
// This file runs THREE variants, not one, because the first version's
// same-line case measured ~0.5 refills/pair instead of the expected
// ~1.0, while the different-line case measured correctly (~2.0) --
// ruling out a uniform code-level bug (that would deflate every offset
// equally) and pointing at a same-line-specific hardware/measurement
// effect. Rather than assert an explanation, each candidate cause gets
// its own isolated test:
//   - "sequential" -- the original design: fixed 1024-byte stride
//     between trials, two INDEPENDENT (unordered) loads per trial.
//   - "dependent"  -- same stride, but the second load's ADDRESS is
//     computed from the first load's VALUE (a genuine pointer-chasing
//     dependency), forcing strict serialization between the two loads.
//     Tests whether the deflation comes from the CPU issuing both loads
//     in the same cycle and coalescing/undercounting a shared-line pair.
//   - "randomized" -- offsets tested at RANDOM (fixed-seed, no
//     std::shuffle/uniform_int_distribution -- see kSeed's comment)
//     buffer positions instead of a fixed stride. Tests whether the
//     regular 1024-byte inter-trial stride is itself being recognized
//     and prefetched by the hardware stride prefetcher, which would
//     affect the sequential variant but not this one.
// Comparing the three isolates which mechanism (if any) explains both
// the same-line deflation and the offset-256/384 dip reported in
// docs/cache_hierarchy_m4.md.
//
// Every offset tested is a multiple of 8 -- an earlier version of this
// tool included 60 and 124, which are NOT 8-byte-aligned, and produced
// an anomalous reading at 124 that was very likely a genuine misaligned-
// load artifact (an 8-byte read at a non-8-byte-aligned address can
// straddle a line boundary unpredictably), not a real line-size signal.
// Removed rather than explained away.
//
// Must run under sudo (PmuCounters requires root, same as pmu_validate).
#include <sys/sysctl.h>

#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include "lob/bench/pmu.hpp"

using lob::bench::PmuCounters;

namespace {

// Fixed seed, raw mt19937_64 output only (no std::shuffle/
// uniform_int_distribution) -- same portability reasoning as
// bench_matching_engine.cpp: those algorithms are implementation-defined
// even given an identical seed.
constexpr std::uint64_t kSeed = 0xB0BACAFEULL;

void PrintSysctlU64(const char* name) {
    std::uint64_t value = 0;
    std::size_t len = sizeof(value);
    if (sysctlbyname(name, &value, &len, nullptr, 0) == 0) {
        std::printf("  %-24s %llu\n", name, static_cast<unsigned long long>(value));
    } else {
        std::printf("  %-24s (sysctl failed)\n", name);
    }
}

// Two independent (unordered) loads -- no dependency between them, so an
// out-of-order core is free to issue both in the same cycle.
[[gnu::noinline]] std::uint64_t TwoLoadsIndependent(const std::uint8_t* base, std::size_t offset) {
    const volatile std::uint64_t* p1 = reinterpret_cast<const volatile std::uint64_t*>(base);
    const volatile std::uint64_t* p2 =
        reinterpret_cast<const volatile std::uint64_t*>(base + offset);
    return *p1 ^ *p2;
}

// Same two addresses, but load2's address is computed FROM load1's
// value, forcing genuine serialization (classic pointer-chasing
// dependency). `*p1` is guaranteed 0 by construction (the buffer is
// zero-filled), so `base + offset + (v1 & 0)` always equals
// `base + offset` architecturally -- the CPU still can't know that
// without waiting for v1, so it can't issue the second load early.
[[gnu::noinline]] std::uint64_t TwoLoadsDependent(const std::uint8_t* base, std::size_t offset) {
    const volatile std::uint64_t* p1 = reinterpret_cast<const volatile std::uint64_t*>(base);
    const std::uint64_t v1 = *p1;
    const std::uint8_t* addr2 = base + offset + (v1 & 0u);
    const volatile std::uint64_t* p2 = reinterpret_cast<const volatile std::uint64_t*>(addr2);
    return v1 ^ *p2;
}

struct SweepResult {
    std::size_t offset;
    double refills_per_pair;
};

template <typename TwoLoadFn>
double MeasureRefillsPerPair(const PmuCounters& pmu, const std::uint8_t* base, std::size_t offset,
                             std::size_t trials, TwoLoadFn&& two_load) {
    volatile std::uint64_t sink = 0;
    const auto before = pmu.Read();
    for (std::size_t t = 0; t < trials; ++t) {
        sink ^= two_load(base + t * 1024, offset);
    }
    const auto after = pmu.Read();
    if (sink == 0xFFFFFFFFFFFFFFFFULL) std::printf("");  // keep sink live
    const std::uint64_t refills = *after.l1d_cache_refills - *before.l1d_cache_refills;
    return static_cast<double>(refills) / static_cast<double>(trials);
}

// Randomized variant: trial positions drawn from a large pool of
// 64-byte-aligned slots, WITH replacement (the pool is large enough --
// see kSlotPoolSize -- that collision probability across kRandomTrials
// draws is negligible: ~trials^2/(2*pool) expected collisions). No fixed
// inter-trial stride at all, so a stride prefetcher has nothing regular
// to lock onto.
double MeasureRefillsPerPairRandomized(const PmuCounters& pmu, const std::uint8_t* base,
                                       std::size_t pool_size, std::size_t offset,
                                       std::size_t trials, std::mt19937_64& rng) {
    const std::size_t slot_count = (pool_size - offset - 8) / 64;
    volatile std::uint64_t sink = 0;
    const auto before = pmu.Read();
    for (std::size_t t = 0; t < trials; ++t) {
        const std::size_t slot = rng() % slot_count;
        sink ^= TwoLoadsIndependent(base + slot * 64, offset);
    }
    const auto after = pmu.Read();
    if (sink == 0xFFFFFFFFFFFFFFFFULL) std::printf("");
    const std::uint64_t refills = *after.l1d_cache_refills - *before.l1d_cache_refills;
    return static_cast<double>(refills) / static_cast<double>(trials);
}

void PrintSweep(const char* label, const std::vector<SweepResult>& results) {
    std::printf("\n--- %s ---\n%-8s %-16s\n", label, "offset", "refills/pair");
    // Report only the FIRST offset after which every remaining result
    // stays >= 1.5 -- a solitary jump that later drops back down (the
    // 256/384 dip in the original run) is an anomaly to flag, not a
    // sustained transition, so it's reported separately, not as
    // "transition detected".
    std::size_t sustained_transition_idx = results.size();
    for (std::size_t i = 0; i < results.size(); ++i) {
        bool sustained = true;
        for (std::size_t j = i; j < results.size(); ++j) {
            if (results[j].refills_per_pair < 1.5) {
                sustained = false;
                break;
            }
        }
        if (sustained) {
            sustained_transition_idx = i;
            break;
        }
    }
    for (std::size_t i = 0; i < results.size(); ++i) {
        std::printf("%-8zu %-16.4f", results[i].offset, results[i].refills_per_pair);
        if (i == sustained_transition_idx) {
            std::printf("  <-- first SUSTAINED transition (every later offset stays >= 1.5)");
        } else if (results[i].refills_per_pair >= 1.5 && i < sustained_transition_idx) {
            std::printf("  <-- ANOMALY: crosses 1.5 but does not stay elevated");
        } else if (i > sustained_transition_idx && results[i].refills_per_pair < 1.5) {
            std::printf("  <-- ANOMALY: drops back below 1.5 after the sustained transition");
        }
        std::printf("\n");
    }
}

int Run() {
    std::printf("=== cache_line_probe ===\n\n");

    PmuCounters pmu;
    std::printf("CPU: %s\n", pmu.CpuString().c_str());
    if (!pmu.IsL1dCacheRefillsAvailable()) {
        std::fprintf(stderr,
                     "cache_line_probe: ARM_L1D_CACHE_REFILL is not available on this run -- "
                     "cannot measure refill granularity without it. See docs/pmu_validation.md.\n");
        return 1;
    }

    const std::size_t offsets[] = {0,   8,   16,  24,  32,  40,  48,  56,  64,  72,  80,  96,
                                   104, 112, 120, 128, 136, 144, 160, 192, 224, 256, 384, 512};
    const std::size_t num_offsets = sizeof(offsets) / sizeof(offsets[0]);
    constexpr std::size_t kTrials = 20000;
    constexpr std::size_t kMaxOffset = 512;
    constexpr std::size_t kPerOffsetRegion = kTrials * 1024 + kMaxOffset + 64;  // ~20.5MB

    // --- Variant 1: sequential stride, independent loads (the original
    // design). Each offset gets its OWN, never-reused region of a large
    // buffer -- the first version of this tool reset the cursor to 0 for
    // every offset, so every offset re-read the exact same ~20MB region
    // instead of fresh memory each time. Fixed here. ---
    {
        std::vector<std::uint8_t> buffer(kPerOffsetRegion * num_offsets, 0);
        std::vector<SweepResult> results;
        for (std::size_t i = 0; i < num_offsets; ++i) {
            const std::uint8_t* region = buffer.data() + i * kPerOffsetRegion;
            const double refills =
                MeasureRefillsPerPair(pmu, region, offsets[i], kTrials, TwoLoadsIndependent);
            results.push_back(SweepResult{offsets[i], refills});
        }
        PrintSweep("sequential stride, independent loads", results);
    }

    // --- Variant 2: sequential stride, DEPENDENT loads -- tests whether
    // the same-line deflation comes from the CPU issuing both
    // independent loads in the same cycle and undercounting a shared-
    // line pair. Only a few representative offsets (full sweep isn't
    // needed to test this specific hypothesis). ---
    {
        const std::size_t dep_offsets[] = {0, 16, 32, 56, 64, 96, 128};
        const std::size_t n = sizeof(dep_offsets) / sizeof(dep_offsets[0]);
        std::vector<std::uint8_t> buffer(kPerOffsetRegion * n, 0);
        std::vector<SweepResult> results;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t* region = buffer.data() + i * kPerOffsetRegion;
            const double refills =
                MeasureRefillsPerPair(pmu, region, dep_offsets[i], kTrials, TwoLoadsDependent);
            results.push_back(SweepResult{dep_offsets[i], refills});
        }
        PrintSweep("sequential stride, DEPENDENT (serialized) loads", results);
    }

    // --- Variant 3: randomized trial positions -- tests whether the
    // hardware stride prefetcher, recognizing the regular 1024-byte
    // inter-trial stride used in variants 1/2, is itself responsible for
    // the low-offset deflation or the offset-256/384 dip. No fixed
    // stride here at all. ---
    {
        constexpr std::size_t kPoolSize = 256ull * 1024 * 1024;  // far exceeds L1D/L2
        constexpr std::size_t kRandomTrials = 8000;
        std::vector<std::uint8_t> buffer(kPoolSize, 0);
        std::mt19937_64 rng(kSeed);
        std::vector<SweepResult> results;
        for (std::size_t i = 0; i < num_offsets; ++i) {
            const double refills = MeasureRefillsPerPairRandomized(pmu, buffer.data(), kPoolSize,
                                                                   offsets[i], kRandomTrials, rng);
            results.push_back(SweepResult{offsets[i], refills});
        }
        PrintSweep("randomized trial positions, independent loads", results);
    }

    std::printf("\n--- cross-checks (compile-time constant / sysctl, no measurement) ---\n");
#ifdef __cpp_lib_hardware_interference_size
    std::printf("  std::hardware_destructive_interference_size  %zu\n",
                std::hardware_destructive_interference_size);
    std::printf("  std::hardware_constructive_interference_size %zu\n",
                std::hardware_constructive_interference_size);
#else
    std::printf("  __cpp_lib_hardware_interference_size not defined by this toolchain\n");
#endif
    PrintSysctlU64("hw.cachelinesize");
    PrintSysctlU64("hw.l1dcachesize");
    PrintSysctlU64("hw.l1icachesize");
    PrintSysctlU64("hw.l2cachesize");
    return 0;
}

}  // namespace

int main() {
    try {
        return Run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cache_line_probe: %s\n", e.what());
        return 1;
    }
}
