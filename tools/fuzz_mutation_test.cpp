// fuzz_mutation_test: the evidence that Phase 3's differential harness
// actually catches bugs, not just a claim that it would. For each of
// BuggyOrderBook's seven injected bugs, runs the harness across many
// seeds and reports: was it caught, at what op index, and how small does
// it shrink to. This is what proves Phase 5's green differential runs
// will mean something.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "lob/fuzz/buggy_order_book.hpp"
#include "lob/fuzz/differential.hpp"
#include "lob/fuzz/generator.hpp"
#include "lob/fuzz/generator_profile.hpp"
#include "lob/fuzz/shrinker.hpp"
#include "lob/order_book.hpp"

using namespace lob;
using namespace lob::fuzz;

namespace {

constexpr int kOpCount = 20000;
constexpr std::uint64_t kSeedCount = 20;

struct BugResult {
    std::string name;
    int caught_count = 0;
    int total_seeds = 0;
    std::vector<std::size_t> detection_indices;  // only for caught runs
    std::vector<std::size_t> shrunk_sizes;       // only for caught runs
    double total_shrink_ms = 0.0;
};

template <BugKind Kind>
BugResult RunAllSeeds(const std::string& name) {
    BugResult result;
    result.name = name;
    result.total_seeds = static_cast<int>(kSeedCount);

    for (std::uint64_t seed = 1; seed <= kSeedCount; ++seed) {
        Generator gen(seed, DefaultProfile());
        const auto generated = gen.Generate(kOpCount);

        const auto initial = RunDifferential<OrderBook, BuggyOrderBookVariant<Kind>>(generated.ops);
        if (initial.ok) {
            continue;  // not caught this seed
        }
        ++result.caught_count;
        result.detection_indices.push_back(initial.divergence->op_index);

        const StillFailsPredicate still_fails = [](std::span<const FuzzOp> ops) {
            return !RunDifferential<OrderBook, BuggyOrderBookVariant<Kind>>(ops).ok;
        };
        const auto t0 = std::chrono::steady_clock::now();
        const auto shrunk = Shrink(generated.ops, still_fails);
        const auto t1 = std::chrono::steady_clock::now();
        result.total_shrink_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        result.shrunk_sizes.push_back(shrunk.size());
    }
    return result;
}

std::size_t Median(std::vector<std::size_t> v) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void PrintResult(const BugResult& r) {
    std::printf("%-35s caught %2d/%2d seeds", r.name.c_str(), r.caught_count, r.total_seeds);
    if (r.caught_count > 0) {
        const auto min_idx =
            *std::min_element(r.detection_indices.begin(), r.detection_indices.end());
        const auto max_idx =
            *std::max_element(r.detection_indices.begin(), r.detection_indices.end());
        const auto min_shrunk = *std::min_element(r.shrunk_sizes.begin(), r.shrunk_sizes.end());
        const auto max_shrunk = *std::max_element(r.shrunk_sizes.begin(), r.shrunk_sizes.end());
        std::printf(" | detected at op index: min=%zu median=%zu max=%zu", min_idx,
                    Median(r.detection_indices), max_idx);
        std::printf(" | shrunk size: min=%zu median=%zu max=%zu", min_shrunk,
                    Median(r.shrunk_sizes), max_shrunk);
        std::printf(" | avg shrink time: %.1fms", r.total_shrink_ms / r.caught_count);
    } else {
        std::printf(" | *** NEVER CAUGHT -- GENERATOR GAP, NOT A HARNESS PASS ***");
    }
    std::printf("\n");
}

}  // namespace

int main() {
    std::printf("Mutation-testing report: %d ops/seed, %llu seeds/bug, DefaultProfile\n\n",
                kOpCount, static_cast<unsigned long long>(kSeedCount));

    const auto results = {
        RunAllSeeds<BugKind::CancelUnlinksWrongNode>("CancelUnlinksWrongNode"),
        RunAllSeeds<BugKind::ReplaceKeepsQueuePriority>("ReplaceKeepsQueuePriority"),
        RunAllSeeds<BugKind::PartialFillLeavesStaleQuantity>("PartialFillLeavesStaleQuantity"),
        RunAllSeeds<BugKind::BestBidCacheNotInvalidated>("BestBidCacheNotInvalidated"),
        RunAllSeeds<BugKind::FifoViolatedLifoInstead>("FifoViolatedLifoInstead"),
        RunAllSeeds<BugKind::OffByOneSkipsLastLevelInSweep>("OffByOneSkipsLastLevelInSweep"),
        RunAllSeeds<BugKind::PriceLevelNotRemovedOnLastCancel>("PriceLevelNotRemovedOnLastCancel"),
    };

    for (const auto& r : results) {
        PrintResult(r);
    }

    const bool all_fully_caught =
        std::all_of(results.begin(), results.end(),
                    [](const BugResult& r) { return r.caught_count == r.total_seeds; });
    std::printf("\n%s\n", all_fully_caught
                              ? "All 7 bugs caught in every seed tested."
                              : "AT LEAST ONE BUG WAS MISSED IN AT LEAST ONE SEED -- see above.");

    return all_fully_caught ? 0 : 1;
}
