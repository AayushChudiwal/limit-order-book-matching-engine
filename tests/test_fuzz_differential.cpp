#include <gtest/gtest.h>

#include "lob/fuzz/differential.hpp"
#include "lob/fuzz/generator.hpp"
#include "lob/fuzz/generator_profile.hpp"
#include "lob/order_book.hpp"

using namespace lob;
using namespace lob::fuzz;

namespace {

// An engine that satisfies EngineUnderTest but rejects every single op
// unconditionally -- used only to prove RunDifferential actually detects
// and correctly localizes a divergence, as opposed to always silently
// returning ok=true. This is deliberately not a realistic bug (that's
// what the mutation-testing buggy variants are for); it's the simplest
// possible "obviously different" engine, so a test using it is a check on
// the HARNESS's detection mechanics, not a check on any particular bug
// class.
class RejectEverythingEngine {
  public:
    explicit RejectEverythingEngine(BookListener& listener) : listener_(listener) {}

    void AddLimitOrder(OrderId id, Side, Price, Quantity) {
        listener_.OnOrderRejected(id, RejectReason::InvalidQuantity);
    }
    void AddMarketOrder(OrderId id, Side, Quantity) {
        listener_.OnOrderRejected(id, RejectReason::InvalidQuantity);
    }
    void CancelOrder(OrderId id) { listener_.OnOrderRejected(id, RejectReason::UnknownOrderId); }
    void Replace(OrderId old_id, OrderId, Price, Quantity) {
        listener_.OnOrderRejected(old_id, RejectReason::UnknownOrderId);
    }
    void ReduceRestingQuantity(OrderId id, Quantity) {
        listener_.OnOrderRejected(id, RejectReason::UnknownOrderId);
    }

    std::optional<Price> BestBid() const { return std::nullopt; }
    std::optional<Price> BestAsk() const { return std::nullopt; }
    bool Empty() const { return true; }
    std::optional<Side> SideOf(OrderId) const { return std::nullopt; }
    std::vector<PriceLevel> TopLevels(Side, int) const { return {}; }
    std::vector<FullPriceLevel> FullBook(Side) const { return {}; }

  private:
    BookListener& listener_;
};

static_assert(EngineUnderTest<RejectEverythingEngine>);

}  // namespace

TEST(RunDifferential, ReferenceEngineAgainstItselfAlwaysAgrees) {
    Generator gen(31415, DefaultProfile());
    const auto generated = gen.Generate(10000);

    const auto result = RunDifferential<OrderBook, OrderBook>(generated.ops);

    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.divergence.has_value());
}

TEST(RunDifferential, AllThreeProfilesAgreeReferenceAgainstItself) {
    for (auto profile : {DefaultProfile(), AllAddsProfile(), ReplaceHeavyProfile()}) {
        Generator gen(2718, profile);
        const auto generated = gen.Generate(3000);
        const auto result = RunDifferential<OrderBook, OrderBook>(generated.ops);
        EXPECT_TRUE(result.ok) << "profile=" << profile.name;
    }
}

TEST(RunDifferential, DetectsAndLocalizesAnObviouslyBrokenEngine) {
    Generator gen(1, DefaultProfile());
    const auto generated = gen.Generate(100);

    const auto result = RunDifferential<OrderBook, RejectEverythingEngine>(generated.ops);

    ASSERT_FALSE(result.ok);
    ASSERT_TRUE(result.divergence.has_value());
    // The very first op the reference engine accepts and
    // RejectEverythingEngine doesn't is where this must localize -- for a
    // fresh id, that's op index 0 (an AddLimit or AddMarket).
    EXPECT_EQ(result.divergence->op_index, 0u);
    EXPECT_FALSE(result.divergence->description.empty());
}

TEST(RunDifferential, DivergenceCapturesBothEnginesFullBookState) {
    Generator gen(1, DefaultProfile());
    const auto generated = gen.Generate(100);

    const auto result = RunDifferential<OrderBook, RejectEverythingEngine>(generated.ops);

    ASSERT_TRUE(result.divergence.has_value());
    // Reference engine accepted the first op and should show it resting;
    // RejectEverythingEngine's book is always empty.
    const bool reference_has_something =
        !result.divergence->engine_a_bids.empty() || !result.divergence->engine_a_asks.empty();
    EXPECT_TRUE(reference_has_something);
    EXPECT_TRUE(result.divergence->engine_b_bids.empty());
    EXPECT_TRUE(result.divergence->engine_b_asks.empty());
}
