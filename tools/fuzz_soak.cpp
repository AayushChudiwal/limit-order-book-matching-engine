// fuzz_soak: the long, randomized-seed CI job, in two complementary modes.
//
// ---------------------------------------------------------------------
// Why two modes, and why neither replaces the other
// ---------------------------------------------------------------------
// MODE A (invariant + determinism) asserts a single engine's structural
// invariants after every op (CheckInvariants: book never crossed, levels
// strictly ordered with none empty, no id resting twice, resting total
// matching ApplyOp's predicted deltas), plus "the same seed regenerates
// bit-identical ops".
//
// MODE B (differential) runs OrderBook and OptimizedOrderBook over the
// same op stream and compares observable behaviour after every op.
//
// These catch DIFFERENT failures and neither subsumes the other:
//
//   - A differential test is blind to a bug both engines share.
//     OptimizedOrderBook was forked from OrderBook (see
//     docs/phase5_plan.md), so shared-ancestry bugs are a live category,
//     not a theoretical one -- both engines would agree, confidently,
//     and be wrong together. Only mode A's invariants catch that.
//   - Mode A is blind to divergence that violates no invariant. Two
//     engines can each keep a perfectly well-formed book and still
//     disagree about which one is correct (a fill assigned to the wrong
//     resting order at the same price, say -- FIFO priority is not an
//     invariant of a single book's shape). Only mode B catches that.
//
// ---------------------------------------------------------------------
// Mode A is parameterized over the engine, deliberately
// ---------------------------------------------------------------------
// This tool previously hardcoded the reference OrderBook -- written when
// Phase 5's optimized engine did not exist yet. That made the soak test
// FROZEN CODE: docs/phase5_plan.md commits to "OrderBook itself needs no
// further changes for the rest of Phase 5", while every remaining step
// mutates OptimizedOrderBook. The long randomized sweep was therefore
// exercising the one engine that was not changing.
//
// So mode A now takes the engine as a parameter, and CI runs it against
// OptimizedOrderBook as the primary target. The reference engine still
// gets a smaller nightly sweep of its own (invariant-reference), because
// it is the ground truth every mode-B comparison is measured against: if
// OrderBook itself violated an invariant, every differential result
// silently becomes meaningless and nothing else would flag it.
//
// ---------------------------------------------------------------------
// Cost note (see docs/benchmark_methodology.md, "Fuzz sweep cost")
// ---------------------------------------------------------------------
// Runtime is strongly SUPERLINEAR in op_count -- roughly O(ops^1.7),
// because CheckInvariants runs after every op and its cost scales with
// current book size. 15x the ops costs ~100x the time. Budget sweeps by
// widening seed_count at a modest op_count, not by deepening op_count:
// for memory-safety work especially, a use-after-free is triggered by
// hitting the right SHAPE of sequence, not by running longer.
//
// Usage: fuzz_soak <mode> [seed_offset] [seed_count] [op_count]
//   mode: invariant-optimized | invariant-reference | differential
// Defaults: offset=0, count=50, ops=50000.
// CI passes a seed_offset derived from the run number so each scheduled
// run explores a fresh range while staying reproducible given the offset.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lob/fuzz/apply_op.hpp"
#include "lob/fuzz/differential.hpp"
#include "lob/fuzz/generator.hpp"
#include "lob/fuzz/generator_profile.hpp"
#include "lob/fuzz/invariants.hpp"
#include "lob/optimized_order_book.hpp"
#include "lob/order_book.hpp"

using namespace lob;
using namespace lob::fuzz;

namespace {

bool CheckDeterminism(std::uint64_t seed, const GeneratorProfile& profile, int op_count) {
    Generator gen_a(seed, profile);
    Generator gen_b(seed, profile);
    const auto result_a = gen_a.Generate(op_count);
    const auto result_b = gen_b.Generate(op_count);

    if (result_a.ops.size() != result_b.ops.size()) {
        return false;
    }
    for (std::size_t i = 0; i < result_a.ops.size(); ++i) {
        const auto& x = result_a.ops[i];
        const auto& y = result_b.ops[i];
        if (x.kind != y.kind || x.order_id != y.order_id || x.replace_target != y.replace_target ||
            x.side != y.side || x.price != y.price || x.quantity != y.quantity) {
            return false;
        }
    }
    return true;
}

// Returns the op index and violation description of the first invariant
// break found, or nullopt if the whole run is clean. Generic over the
// engine: see the header comment on why this must NOT be pinned to the
// reference engine.
template <EngineUnderTest Engine>
std::optional<std::pair<std::size_t, std::string>> CheckPropertyInvariants(
    const std::vector<FuzzOp>& ops) {
    FuzzListener listener;
    Engine book(listener);
    std::int64_t expected_total = 0;

    for (std::size_t i = 0; i < ops.size(); ++i) {
        const auto outcome = ApplyOp(book, listener, ops[i]);
        expected_total += outcome.quantity_delta;
        const auto violations = CheckInvariants(book, expected_total);
        if (!violations.empty()) {
            return std::make_pair(i, violations[0]);
        }
    }
    return std::nullopt;
}

// The generator-coverage assertion ("every op category got exercised")
// is a claim about the generator AT VOLUME, not about any individual
// run, so it is only meaningful once a run is long enough to plausibly
// reach every category. Measured on seeds 0-39, all three profiles,
// counting seeds that report at least one zero category:
//
//     2,000 ops -> 24/40 gaps    8,000 ops ->  4/40 gaps
//     3,000 ops -> 12/40 gaps   10,000 ops ->  3/40 gaps
//     5,000 ops ->  8/40 gaps   20,000 ops ->  0/40 gaps
//                               30,000 ops ->  0/40 gaps
//
// So a short sweep that ran this check would fail constantly on
// perfectly healthy code -- at 2,000 ops it would flag 60% of seeds.
// That is not a generator bug; a 2,000-op walk simply may not reach
// (say) market_into_empty_book. Gate the check rather than either
// weakening it or restricting every sweep to 20k+ ops: the short
// per-push sweeps exist for engine correctness and memory safety, and
// the nightly sweeps that DO run at 30k still enforce it in full.
constexpr int kCoverageCheckMinOps = 20000;

enum class Mode { kInvariantOptimized, kInvariantReference, kDifferential };

std::optional<Mode> ParseMode(const char* s) {
    if (std::strcmp(s, "invariant-optimized") == 0) return Mode::kInvariantOptimized;
    if (std::strcmp(s, "invariant-reference") == 0) return Mode::kInvariantReference;
    if (std::strcmp(s, "differential") == 0) return Mode::kDifferential;
    return std::nullopt;
}

const char* ModeName(Mode m) {
    switch (m) {
        case Mode::kInvariantOptimized:
            return "invariant-optimized (OptimizedOrderBook)";
        case Mode::kInvariantReference:
            return "invariant-reference (OrderBook, ground-truth guard)";
        case Mode::kDifferential:
            return "differential (OrderBook vs OptimizedOrderBook)";
    }
    return "?";
}

// One (profile, seed) combination. Returns true on failure.
bool RunOne(Mode mode, const GeneratorProfile& profile, std::uint64_t seed, int op_count) {
    bool failed = false;

    // Determinism and generator coverage are properties of the GENERATOR,
    // not of any engine, so they belong to the invariant modes only --
    // running them again under differential would be duplicated work.
    if (mode != Mode::kDifferential) {
        if (!CheckDeterminism(seed, profile, 1000)) {
            std::printf(
                "FAIL: profile=%s seed=%llu is NOT deterministic (same seed produced "
                "different ops on two independent runs)\n",
                profile.name.c_str(), static_cast<unsigned long long>(seed));
            return true;
        }
    }

    Generator gen(seed, profile);
    const auto generated = gen.Generate(op_count);

    if (mode != Mode::kDifferential && profile.name == "default" &&
        op_count >= kCoverageCheckMinOps) {
        const auto zero = generated.coverage.ZeroCategories();
        if (!zero.empty()) {
            std::printf("FAIL: profile=default seed=%llu generator coverage gap:",
                        static_cast<unsigned long long>(seed));
            for (const auto& name : zero) {
                std::printf(" %s", name.c_str());
            }
            std::printf("\n");
            failed = true;
        }
    }

    switch (mode) {
        case Mode::kInvariantOptimized: {
            if (const auto v = CheckPropertyInvariants<OptimizedOrderBook>(generated.ops)) {
                std::printf(
                    "FAIL: profile=%s seed=%llu OptimizedOrderBook invariant violated at op "
                    "%zu: %s\n",
                    profile.name.c_str(), static_cast<unsigned long long>(seed), v->first,
                    v->second.c_str());
                failed = true;
            }
            break;
        }
        case Mode::kInvariantReference: {
            if (const auto v = CheckPropertyInvariants<OrderBook>(generated.ops)) {
                std::printf(
                    "FAIL: profile=%s seed=%llu OrderBook (REFERENCE) invariant violated at op "
                    "%zu: %s\n"
                    "      NOTE: a reference-engine violation invalidates every differential\n"
                    "      result too -- mode B compares against this engine.\n",
                    profile.name.c_str(), static_cast<unsigned long long>(seed), v->first,
                    v->second.c_str());
                failed = true;
            }
            break;
        }
        case Mode::kDifferential: {
            const auto result = RunDifferential<OrderBook, OptimizedOrderBook>(generated.ops);
            if (result.divergence.has_value()) {
                std::printf("FAIL: profile=%s seed=%llu divergence at op %zu: %s\n",
                            profile.name.c_str(), static_cast<unsigned long long>(seed),
                            result.divergence->op_index,
                            result.divergence->description.c_str());
                failed = true;
            }
            break;
        }
    }
    return failed;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <mode> [seed_offset] [seed_count] [op_count]\n"
                     "  mode: invariant-optimized | invariant-reference | differential\n",
                     argv[0]);
        return 2;
    }
    const auto mode = ParseMode(argv[1]);
    if (!mode) {
        std::fprintf(stderr, "unknown mode '%s' (expected invariant-optimized, "
                             "invariant-reference, or differential)\n",
                     argv[1]);
        return 2;
    }

    const std::uint64_t seed_offset = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;
    const int seed_count = argc > 3 ? std::atoi(argv[3]) : 50;
    const int op_count = argc > 4 ? std::atoi(argv[4]) : 50000;

    std::printf("fuzz_soak [%s]: seeds [%llu, %llu), %d ops each, all three profiles\n",
                ModeName(*mode), static_cast<unsigned long long>(seed_offset),
                static_cast<unsigned long long>(seed_offset) + seed_count, op_count);
    if (*mode != Mode::kDifferential) {
        std::printf("generator-coverage assertion: %s (needs op_count >= %d; see kCoverageCheckMinOps)\n",
                    op_count >= kCoverageCheckMinOps ? "ENFORCED" : "SKIPPED -- run too short",
                    kCoverageCheckMinOps);
    }

    bool any_failure = false;
    int checked = 0;

    for (auto profile : {DefaultProfile(), AllAddsProfile(), ReplaceHeavyProfile()}) {
        for (int i = 0; i < seed_count; ++i) {
            const std::uint64_t seed = seed_offset + static_cast<std::uint64_t>(i);
            ++checked;
            if (RunOne(*mode, profile, seed, op_count)) {
                any_failure = true;
            }
        }
    }

    std::printf("\n[%s] checked %d (profile, seed) combinations. %s\n", ModeName(*mode), checked,
                any_failure ? "FAILURES FOUND -- see above. Shrink and commit as a regression test."
                            : "All clean.");
    return any_failure ? 1 : 0;
}
