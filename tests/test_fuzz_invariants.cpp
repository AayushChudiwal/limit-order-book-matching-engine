#include <gtest/gtest.h>

#include <algorithm>

#include "lob/fuzz/apply_op.hpp"
#include "lob/fuzz/invariants.hpp"
#include "lob/order_book.hpp"

using namespace lob;
using namespace lob::fuzz;

namespace {

// A minimal stand-in satisfying EngineUnderTest with hard-coded query
// results -- used only to test CheckInvariants' DETECTION logic against
// deliberately invalid states no real, publicly-driven OrderBook could
// ever reach through its own API. Mutators are no-ops: this test file
// never calls them, only sets state directly via the public fields below.
class FakeEngine {
  public:
    void AddLimitOrder(OrderId, Side, Price, Quantity) {}
    void AddMarketOrder(OrderId, Side, Quantity) {}
    void CancelOrder(OrderId) {}
    void Replace(OrderId, OrderId, Price, Quantity) {}
    void ReduceRestingQuantity(OrderId, Quantity) {}

    std::optional<Price> BestBid() const { return best_bid; }
    std::optional<Price> BestAsk() const { return best_ask; }
    bool Empty() const { return bids.empty() && asks.empty(); }
    std::optional<Side> SideOf(OrderId) const { return std::nullopt; }
    std::vector<PriceLevel> TopLevels(Side, int) const { return {}; }
    std::vector<FullPriceLevel> FullBook(Side side) const {
        return side == Side::Buy ? bids : asks;
    }

    std::optional<Price> best_bid;
    std::optional<Price> best_ask;
    std::vector<FullPriceLevel> bids;
    std::vector<FullPriceLevel> asks;
};

static_assert(EngineUnderTest<FakeEngine>);

}  // namespace

TEST(CheckInvariants, CleanEmptyBookHasNoViolations) {
    FakeEngine engine;
    const auto violations = CheckInvariants(engine, 0);
    EXPECT_TRUE(violations.empty());
}

TEST(CheckInvariants, CleanPopulatedBookHasNoViolations) {
    FakeEngine engine;
    engine.best_bid = Price{100};
    engine.best_ask = Price{101};
    engine.bids = {{Price{100}, {{OrderId{1}, Quantity{10}}}}};
    engine.asks = {{Price{101}, {{OrderId{2}, Quantity{20}}}}};

    const auto violations = CheckInvariants(engine, 30);
    EXPECT_TRUE(violations.empty());
}

TEST(CheckInvariants, DetectsACrossedBook) {
    FakeEngine engine;
    engine.best_bid = Price{101};
    engine.best_ask = Price{100};  // crossed: bid >= ask
    engine.bids = {{Price{101}, {{OrderId{1}, Quantity{10}}}}};
    engine.asks = {{Price{100}, {{OrderId{2}, Quantity{10}}}}};

    const auto violations = CheckInvariants(engine, 20);
    ASSERT_FALSE(violations.empty());
    EXPECT_NE(violations[0].find("crossed"), std::string::npos);
}

TEST(CheckInvariants, DetectsBidLevelsOutOfOrder) {
    FakeEngine engine;
    // Bids must be strictly descending; this is ascending.
    engine.bids = {{Price{100}, {{OrderId{1}, Quantity{10}}}},
                   {Price{101}, {{OrderId{2}, Quantity{10}}}}};

    const auto violations = CheckInvariants(engine, 20);
    const bool found = std::any_of(violations.begin(), violations.end(), [](const auto& v) {
        return v.find("not strictly ordered") != std::string::npos;
    });
    EXPECT_TRUE(found);
}

TEST(CheckInvariants, DetectsAnEmptyPriceLevel) {
    FakeEngine engine;
    engine.bids = {{Price{100}, {}}};  // a level with no orders should never exist

    const auto violations = CheckInvariants(engine, 0);
    const bool found = std::any_of(violations.begin(), violations.end(), [](const auto& v) {
        return v.find("empty price level") != std::string::npos;
    });
    EXPECT_TRUE(found);
}

TEST(CheckInvariants, DetectsANonPositiveRestingQuantity) {
    FakeEngine engine;
    engine.bids = {{Price{100}, {{OrderId{1}, Quantity{0}}}}};

    const auto violations = CheckInvariants(engine, 0);
    const bool found = std::any_of(violations.begin(), violations.end(), [](const auto& v) {
        return v.find("non-positive resting quantity") != std::string::npos;
    });
    EXPECT_TRUE(found);
}

TEST(CheckInvariants, DetectsTheSameOrderIdRestingOnBothSides) {
    FakeEngine engine;
    engine.bids = {{Price{100}, {{OrderId{1}, Quantity{10}}}}};
    engine.asks = {{Price{101}, {{OrderId{1}, Quantity{10}}}}};  // same id, other side

    const auto violations = CheckInvariants(engine, 20);
    const bool found = std::any_of(violations.begin(), violations.end(), [](const auto& v) {
        return v.find("rests more than once") != std::string::npos;
    });
    EXPECT_TRUE(found);
}

TEST(CheckInvariants, DetectsAConservationMismatch) {
    FakeEngine engine;
    engine.bids = {{Price{100}, {{OrderId{1}, Quantity{10}}}}};

    const auto violations = CheckInvariants(engine, 999);  // expected doesn't match observed 10
    const bool found = std::any_of(violations.begin(), violations.end(), [](const auto& v) {
        return v.find("not conserved") != std::string::npos;
    });
    EXPECT_TRUE(found);
}

// Integration check: driving a REAL OrderBook through ApplyOp and tracking
// the running expected total via quantity_delta should never trip the
// conservation invariant, across a realistic mixed sequence.
TEST(CheckInvariants, RealEngineDrivenThroughApplyOpStaysConservedAcrossAMixedSequence) {
    FuzzListener listener;
    OrderBook book(listener);
    std::int64_t expected_total = 0;

    const std::vector<FuzzOp> ops = {
        {FuzzOp::Kind::AddLimit, OrderId{1}, {}, Side::Buy, Price{100}, Quantity{10}},
        {FuzzOp::Kind::AddLimit, OrderId{2}, {}, Side::Sell, Price{102}, Quantity{15}},
        {FuzzOp::Kind::AddLimit, OrderId{3}, {}, Side::Buy, Price{102}, Quantity{20}},  // crosses,
                                                                                        // partial
        {FuzzOp::Kind::Cancel, OrderId{1}, {}, Side::Buy, Price{0}, Quantity{0}},
        {FuzzOp::Kind::Replace, OrderId{4}, OrderId{3}, Side::Buy, Price{101}, Quantity{5}},
        {FuzzOp::Kind::AddMarket, OrderId{5}, {}, Side::Sell, Price{0}, Quantity{3}},
        {FuzzOp::Kind::ReduceQuantity, {}, OrderId{4}, Side::Buy, Price{0}, Quantity{2}},
    };

    for (const auto& op : ops) {
        const auto outcome = ApplyOp(book, listener, op);
        expected_total += outcome.quantity_delta;
        const auto violations = CheckInvariants(book, expected_total);
        EXPECT_TRUE(violations.empty())
            << "op kind=" << static_cast<int>(op.kind)
            << " violations[0]=" << (violations.empty() ? "" : violations[0]);
    }
}
