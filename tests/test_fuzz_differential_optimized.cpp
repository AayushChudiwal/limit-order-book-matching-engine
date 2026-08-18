#include <gtest/gtest.h>

#include "lob/fuzz/differential.hpp"
#include "lob/fuzz/generator.hpp"
#include "lob/fuzz/generator_profile.hpp"
#include "lob/optimized_order_book.hpp"
#include "lob/order_book.hpp"

using namespace lob;
using namespace lob::fuzz;

static_assert(EngineUnderTest<OptimizedOrderBook>,
              "OptimizedOrderBook must satisfy the same interface the fuzzer checks OrderBook "
              "against -- this is what lets RunDifferential<OrderBook, OptimizedOrderBook> exist "
              "at all");

// Verifies Phase 5 step 2 (LevelOrders replacing std::deque<RestingOrder>
// as the reference engine's LevelQueue, see optimized_order_book.hpp)
// against the naive reference engine -- the same differential mechanism
// test_fuzz_differential.cpp uses to check the reference against itself
// and against a deliberately-broken engine, now checking the FORK point
// itself. A step 2 divergence here means LevelOrders' inline/overflow
// transition logic disagrees with std::deque somewhere the smaller,
// hand-written unit tests in test_optimized_level_orders.cpp didn't
// think to cover.
TEST(RunDifferentialOptimized, AgreesWithReferenceOnDefaultProfile) {
    Generator gen(31415, DefaultProfile());
    const auto generated = gen.Generate(10000);

    const auto result = RunDifferential<OrderBook, OptimizedOrderBook>(generated.ops);

    EXPECT_TRUE(result.ok) << (result.divergence ? result.divergence->description : "");
}

TEST(RunDifferentialOptimized, AgreesWithReferenceOnAllThreeProfiles) {
    for (auto profile : {DefaultProfile(), AllAddsProfile(), ReplaceHeavyProfile()}) {
        Generator gen(2718, profile);
        const auto generated = gen.Generate(5000);
        const auto result = RunDifferential<OrderBook, OptimizedOrderBook>(generated.ops);
        EXPECT_TRUE(result.ok) << "profile=" << profile.name
                               << (result.divergence ? " " + result.divergence->description : "");
    }
}

// AllAddsProfile deliberately drives one-sided bursts of adds -- exactly
// what pushes many levels past LevelOrders' inline-1 capacity into its
// heap-overflow path, since a deep queue at one price means many orders
// stacking at the SAME level rather than spreading across levels. This
// is the profile most likely to catch an inline/overflow transition bug
// that a shallower, more cancel-heavy mix wouldn't reach.
TEST(RunDifferentialOptimized, AgreesWithReferenceUnderDeepSingleLevelQueues) {
    Generator gen(99, AllAddsProfile());
    const auto generated = gen.Generate(20000);
    const auto result = RunDifferential<OrderBook, OptimizedOrderBook>(generated.ops);
    EXPECT_TRUE(result.ok) << (result.divergence ? result.divergence->description : "");
}

// Several independent seeds at moderate volume -- a single seed passing
// only says LevelOrders is correct for that specific op sequence's
// happen-to-be inline/overflow transition points, not in general.
TEST(RunDifferentialOptimized, AgreesWithReferenceAcrossMultipleSeeds) {
    for (std::uint64_t seed : {1ULL, 7ULL, 42ULL, 1337ULL, 999999ULL}) {
        Generator gen(seed, DefaultProfile());
        const auto generated = gen.Generate(4000);
        const auto result = RunDifferential<OrderBook, OptimizedOrderBook>(generated.ops);
        EXPECT_TRUE(result.ok) << "seed=" << seed
                               << (result.divergence ? " " + result.divergence->description : "");
    }
}
