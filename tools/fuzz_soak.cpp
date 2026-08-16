// fuzz_soak: the long, randomized-seed CI job. Phase 5's optimized engine
// doesn't exist yet, so there's no second real engine to differentially
// compare against on a schedule -- what this DOES check, at much higher
// seed and op-count volume than the fast per-push job, is exactly what
// the original Phase 3 plan calls for even without a second engine:
// property-based invariants (via CheckInvariants) asserted continuously
// on the reference engine across a long random walk, and "replay of the
// same seed is bit-identical" (regenerating and comparing).
//
// Usage: fuzz_soak [seed_offset] [seed_count] [op_count]
// Defaults: offset=0, count=50, ops=50000. CI passes a seed_offset
// derived from the run number so each scheduled run explores a fresh
// seed range while staying fully reproducible given the same offset.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "lob/fuzz/apply_op.hpp"
#include "lob/fuzz/generator.hpp"
#include "lob/fuzz/generator_profile.hpp"
#include "lob/fuzz/invariants.hpp"
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
// break found, or nullopt if the whole run is clean.
std::optional<std::pair<std::size_t, std::string>> CheckPropertyInvariants(
    const std::vector<FuzzOp>& ops) {
    FuzzListener listener;
    OrderBook book(listener);
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

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t seed_offset = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 0;
    const int seed_count = argc > 2 ? std::atoi(argv[2]) : 50;
    const int op_count = argc > 3 ? std::atoi(argv[3]) : 50000;

    std::printf("fuzz_soak: seeds [%llu, %llu), %d ops each, all three profiles\n",
                static_cast<unsigned long long>(seed_offset),
                static_cast<unsigned long long>(seed_offset) + seed_count, op_count);

    bool any_failure = false;
    int checked = 0;

    for (auto profile : {DefaultProfile(), AllAddsProfile(), ReplaceHeavyProfile()}) {
        for (int i = 0; i < seed_count; ++i) {
            const std::uint64_t seed = seed_offset + static_cast<std::uint64_t>(i);
            ++checked;

            if (!CheckDeterminism(seed, profile, 1000)) {
                std::printf(
                    "FAIL: profile=%s seed=%llu is NOT deterministic (same seed produced "
                    "different ops on two independent runs)\n",
                    profile.name.c_str(), static_cast<unsigned long long>(seed));
                any_failure = true;
                continue;
            }

            Generator gen(seed, profile);
            const auto generated = gen.Generate(op_count);
            const auto zero = generated.coverage.ZeroCategories();
            if (profile.name == "default" && !zero.empty()) {
                std::printf("FAIL: profile=default seed=%llu generator coverage gap:",
                            static_cast<unsigned long long>(seed));
                for (const auto& name : zero) {
                    std::printf(" %s", name.c_str());
                }
                std::printf("\n");
                any_failure = true;
            }

            if (const auto violation = CheckPropertyInvariants(generated.ops)) {
                std::printf("FAIL: profile=%s seed=%llu invariant violated at op %zu: %s\n",
                            profile.name.c_str(), static_cast<unsigned long long>(seed),
                            violation->first, violation->second.c_str());
                any_failure = true;
            }
        }
    }

    std::printf("\nChecked %d (profile, seed) combinations. %s\n", checked,
                any_failure ? "FAILURES FOUND -- see above. Shrink and commit as a regression test."
                            : "All clean.");
    return any_failure ? 1 : 0;
}
