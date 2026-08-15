#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

TEST(SideOf, ReturnsTheSideOfARestingOrderAndNulloptOtherwise) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{110}, Quantity{10});

    ASSERT_TRUE(book.SideOf(OrderId{1}).has_value());
    EXPECT_EQ(*book.SideOf(OrderId{1}), Side::Buy);
    ASSERT_TRUE(book.SideOf(OrderId{2}).has_value());
    EXPECT_EQ(*book.SideOf(OrderId{2}), Side::Sell);
    EXPECT_FALSE(book.SideOf(OrderId{404}).has_value());
}

TEST(ReduceRestingQuantity, PartialReductionLeavesTheOrderRestingWithNoFillEmitted) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{50});
    book.ReduceRestingQuantity(OrderId{1}, Quantity{20});

    // No Fill: this models a feed reporting its own already-resolved
    // execution or cancellation, not a match this engine computed.
    EXPECT_TRUE(listener.fills.empty());
    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(*book.BestAsk(), Price{100});
}

TEST(ReduceRestingQuantity, ReducingToZeroRemovesTheOrder) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{50});
    book.ReduceRestingQuantity(OrderId{1}, Quantity{50});

    EXPECT_TRUE(listener.fills.empty());
    EXPECT_TRUE(book.Empty());
}

TEST(ReduceRestingQuantity, OfAnUnknownOrderIdIsRejectedCleanly) {
    RecordingListener listener;
    OrderBook book(listener);

    book.ReduceRestingQuantity(OrderId{404}, Quantity{10});

    ASSERT_EQ(listener.rejected.size(), 1u);
    EXPECT_EQ(listener.rejected[0].second, RejectReason::UnknownOrderId);
}

TEST(ReduceRestingQuantity, ReducingByMoreThanRemainsIsRejectedNotAppliedPartially) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{50});
    book.ReduceRestingQuantity(OrderId{1}, Quantity{999});

    ASSERT_EQ(listener.rejected.size(), 1u);
    EXPECT_EQ(listener.rejected[0].second, RejectReason::InsufficientQuantity);
    // The order must be untouched -- still resting at its original size,
    // not silently clamped to zero.
    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(*book.BestAsk(), Price{100});
}

// This is the exact pattern the ITCH replay adapter uses for an Order
// Replace message: look up the original's side, cancel the original
// entirely, and add the replacement fresh under its new reference number.
// Phase 1's own OrderBook::ModifyOrder is deliberately NOT used here --
// it preserves queue priority on a same-price quantity decrease, which is
// correct for an order-management-style "modify in place" API but is not
// what ITCH's wire-level Order Replace means: Replace always assigns a
// brand new day-unique reference number, so it always starts over at the
// back of the queue, even when the new quantity is smaller than the old
// one. Getting this distinction backwards is called out by name as a
// common real-world ITCH parsing bug.
TEST(OrderReplaceViaItchPattern, AlwaysLosesQueuePriorityEvenOnAQuantityDecrease) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});

    // "Replace" order 1 with a smaller quantity at the same price, under a
    // new reference number -- the ITCH pattern, not ModifyOrder.
    const auto side = book.SideOf(OrderId{1});
    ASSERT_TRUE(side.has_value());
    book.CancelOrder(OrderId{1});
    book.AddLimitOrder(OrderId{3}, *side, Price{100}, Quantity{5});

    book.AddLimitOrder(OrderId{4}, Side::Buy, Price{100}, Quantity{15});

    // Order 2 (untouched) trades first: the replacement forfeited
    // priority despite its smaller size, unlike Phase 1's ModifyOrder,
    // which would have kept order 1/3 ahead of order 2 in this scenario
    // (see Modify.QuantityDecreaseAtSamePriceKeepsQueuePriority).
    ASSERT_EQ(listener.fills.size(), 2u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{2});
    EXPECT_EQ(listener.fills[0].quantity, Quantity{10});
    EXPECT_EQ(listener.fills[1].resting_id, OrderId{3});
    EXPECT_EQ(listener.fills[1].quantity, Quantity{5});
}
