// Permanent regression tests: each of these op sequences is the ACTUAL
// shrunk output of Shrink() run against a real generated failure (seed=1,
// 20000 ops, DefaultProfile) for one of the seven injected bugs in
// BuggyOrderBook -- captured verbatim, not hand-written or regenerated
// from the seed. See docs/fuzz_mutation_testing.md for the full
// detection-speed and shrink-size table these came from.
//
// Committing the literal minimized op sequence (rather than re-running
// the generator+shrinker here every time) means these stay fast,
// human-readable at a glance, and immune to ever silently stopping being
// minimal if the generator's internals change later.
#include <gtest/gtest.h>

#include "lob/fuzz/buggy_order_book.hpp"
#include "lob/fuzz/differential.hpp"
#include "lob/order_book.hpp"

using namespace lob;
using namespace lob::fuzz;

TEST(ShrunkRegression, CancelUnlinksWrongNode) {
    // Two orders at the same price; cancelling the SECOND one must not
    // remove the first instead.
    const std::vector<FuzzOp> ops = {
        {FuzzOp::Kind::AddLimit, OrderId{1}, OrderId{0}, Side::Buy, Price{100030}, Quantity{26}},
        {FuzzOp::Kind::AddLimit, OrderId{2}, OrderId{0}, Side::Buy, Price{100030}, Quantity{39}},
        {FuzzOp::Kind::Cancel, OrderId{2}, OrderId{0}, Side::Buy, Price{0}, Quantity{0}},
    };
    const auto result =
        RunDifferential<OrderBook, BuggyOrderBookVariant<BugKind::CancelUnlinksWrongNode>>(ops);
    ASSERT_FALSE(result.ok);
}

TEST(ShrunkRegression, ReplaceKeepsQueuePriority) {
    // A single order, replaced at the SAME price with a smaller quantity
    // -- must still forfeit queue priority (see
    // OrderReplaceViaItchPattern in test_reduce_and_side_of.cpp).
    const std::vector<FuzzOp> ops = {
        {FuzzOp::Kind::AddLimit, OrderId{1}, OrderId{0}, Side::Buy, Price{100029}, Quantity{320}},
        {FuzzOp::Kind::Replace, OrderId{2}, OrderId{1}, Side::Buy, Price{100029}, Quantity{315}},
    };
    const auto result =
        RunDifferential<OrderBook, BuggyOrderBookVariant<BugKind::ReplaceKeepsQueuePriority>>(ops);
    ASSERT_FALSE(result.ok);
}

TEST(ShrunkRegression, PartialFillLeavesStaleQuantity) {
    // A resting sell partially filled by a smaller incoming buy -- the
    // resting order's remaining quantity must reflect the fill.
    const std::vector<FuzzOp> ops = {
        {FuzzOp::Kind::AddLimit, OrderId{1}, OrderId{0}, Side::Sell, Price{100030}, Quantity{128}},
        {FuzzOp::Kind::AddLimit, OrderId{2}, OrderId{0}, Side::Buy, Price{100030}, Quantity{44}},
    };
    const auto result =
        RunDifferential<OrderBook, BuggyOrderBookVariant<BugKind::PartialFillLeavesStaleQuantity>>(
            ops);
    ASSERT_FALSE(result.ok);
}

TEST(ShrunkRegression, BestBidCacheNotInvalidated) {
    // A single resting order, then cancelled -- BestAsk() must go back to
    // none, not stay stuck pointing at the now-empty level.
    const std::vector<FuzzOp> ops = {
        {FuzzOp::Kind::AddLimit, OrderId{1}, OrderId{0}, Side::Sell, Price{100033}, Quantity{204}},
        {FuzzOp::Kind::Cancel, OrderId{1}, OrderId{0}, Side::Buy, Price{0}, Quantity{0}},
    };
    const auto result =
        RunDifferential<OrderBook, BuggyOrderBookVariant<BugKind::BestBidCacheNotInvalidated>>(ops);
    ASSERT_FALSE(result.ok);
}

TEST(ShrunkRegression, FifoViolatedLifoInstead) {
    // Two resting buys at the same price in this arrival order; an
    // incoming market sell must match the FIRST one (order 1), not the
    // second.
    const std::vector<FuzzOp> ops = {
        {FuzzOp::Kind::AddLimit, OrderId{1}, OrderId{0}, Side::Buy, Price{100030}, Quantity{332}},
        {FuzzOp::Kind::AddLimit, OrderId{2}, OrderId{0}, Side::Buy, Price{100030}, Quantity{309}},
        {FuzzOp::Kind::AddMarket, OrderId{3}, OrderId{0}, Side::Sell, Price{0}, Quantity{45}},
    };
    const auto result =
        RunDifferential<OrderBook, BuggyOrderBookVariant<BugKind::FifoViolatedLifoInstead>>(ops);
    ASSERT_FALSE(result.ok);
}

TEST(ShrunkRegression, OffByOneSkipsLastLevelInSweep) {
    // A single resting sell; an incoming marketable buy at a crossing
    // price must still sweep it -- even though, from the aggressor's
    // point of view, it's the only ("last") level on the book.
    const std::vector<FuzzOp> ops = {
        {FuzzOp::Kind::AddLimit, OrderId{1}, OrderId{0}, Side::Sell, Price{100033}, Quantity{426}},
        {FuzzOp::Kind::AddLimit, OrderId{2}, OrderId{0}, Side::Buy, Price{100034}, Quantity{261}},
    };
    const auto result =
        RunDifferential<OrderBook, BuggyOrderBookVariant<BugKind::OffByOneSkipsLastLevelInSweep>>(
            ops);
    ASSERT_FALSE(result.ok);
}

TEST(ShrunkRegression, PriceLevelNotRemovedOnLastCancel) {
    // A single resting order, then cancelled -- the price level itself
    // must disappear from the book, not linger empty.
    const std::vector<FuzzOp> ops = {
        {FuzzOp::Kind::AddLimit, OrderId{1}, OrderId{0}, Side::Sell, Price{100050}, Quantity{204}},
        {FuzzOp::Kind::Cancel, OrderId{1}, OrderId{0}, Side::Buy, Price{0}, Quantity{0}},
    };
    const auto result =
        RunDifferential<OrderBook,
                        BuggyOrderBookVariant<BugKind::PriceLevelNotRemovedOnLastCancel>>(ops);
    ASSERT_FALSE(result.ok);
}
