#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

TEST(Cancel, RemovesARestingOrderSoItNoLongerMatches) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.CancelOrder(OrderId{1});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{10});

    EXPECT_TRUE(listener.fills.empty());
    EXPECT_FALSE(book.BestAsk().has_value());
    EXPECT_TRUE(book.BestBid().has_value());
}

TEST(Cancel, OfUnknownOrderIdIsRejectedCleanlyAndDoesNotCrash) {
    RecordingListener listener;
    OrderBook book(listener);

    book.CancelOrder(OrderId{999});

    ASSERT_EQ(listener.rejected.size(), 1u);
    EXPECT_EQ(listener.rejected[0].first, OrderId{999});
    EXPECT_EQ(listener.rejected[0].second, RejectReason::UnknownOrderId);
}

TEST(Cancel, EmptiesThePriceLevelOnlyWhenTheLastOrderAtItIsCancelled) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{20});

    book.CancelOrder(OrderId{1});
    // The level should still exist -- order 2 is still resting there.
    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(*book.BestAsk(), Price{100});

    book.CancelOrder(OrderId{2});
    EXPECT_FALSE(book.BestAsk().has_value());
}

TEST(Cancel, FromTheMiddleOfAQueuePreservesTheOrderOfTheRemainingOrders) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{3}, Side::Sell, Price{100}, Quantity{10});

    book.CancelOrder(OrderId{2});  // remove the middle order

    book.AddLimitOrder(OrderId{4}, Side::Buy, Price{100}, Quantity{20});

    ASSERT_EQ(listener.fills.size(), 2u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{1});
    EXPECT_EQ(listener.fills[1].resting_id, OrderId{3});
}
