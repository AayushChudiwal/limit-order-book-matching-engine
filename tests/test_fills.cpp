#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

TEST(Fills, PartialFillLeavesRestingOrderWithReducedQuantity) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{50});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{20});

    ASSERT_EQ(listener.fills.size(), 1u);
    EXPECT_EQ(listener.fills[0].aggressor_id, OrderId{2});
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{1});
    EXPECT_EQ(listener.fills[0].price, Price{100});
    EXPECT_EQ(listener.fills[0].quantity, Quantity{20});

    // Resting order 1 still has 30 left and 100 remains the best ask.
    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(*book.BestAsk(), Price{100});
    // The taker was fully filled and should not have rested.
    EXPECT_FALSE(book.BestBid().has_value());
}

TEST(Fills, FullFillOfRestingOrderRemovesItFromTheBook) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{50});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{50});

    ASSERT_EQ(listener.fills.size(), 1u);
    EXPECT_EQ(listener.fills[0].quantity, Quantity{50});
    EXPECT_FALSE(book.BestAsk().has_value());
    EXPECT_FALSE(book.BestBid().has_value());
}

TEST(Fills, IncomingOrderLargerThanRestingOrderPartiallyFillsAndRests) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{20});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{50});

    ASSERT_EQ(listener.fills.size(), 1u);
    EXPECT_EQ(listener.fills[0].quantity, Quantity{20});
    // The remaining 30 shares of order 2 should now rest as the best bid.
    ASSERT_TRUE(book.BestBid().has_value());
    EXPECT_EQ(*book.BestBid(), Price{100});
}

TEST(Fills, RejectsDuplicateOrderId) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{101}, Quantity{10});

    ASSERT_EQ(listener.rejected.size(), 1u);
    EXPECT_EQ(listener.rejected[0].second, RejectReason::DuplicateOrderId);
}

TEST(Fills, RejectsNonPositiveQuantityAndPrice) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{0});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{0}, Quantity{10});

    ASSERT_EQ(listener.rejected.size(), 2u);
    EXPECT_EQ(listener.rejected[0].second, RejectReason::InvalidQuantity);
    EXPECT_EQ(listener.rejected[1].second, RejectReason::InvalidPrice);
}
