#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

TEST(Modify, QuantityDecreaseAtSamePriceKeepsQueuePriority) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});

    // Order 1 trims its size but keeps the same price.
    book.ModifyOrder(OrderId{1}, Price{100}, Quantity{5});

    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{100}, Quantity{10});

    // If priority were lost, order 2 (unchanged) would trade first. It
    // must not: order 1 is still ahead of order 2.
    ASSERT_EQ(listener.fills.size(), 2u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{1});
    EXPECT_EQ(listener.fills[0].quantity, Quantity{5});
    EXPECT_EQ(listener.fills[1].resting_id, OrderId{2});
    EXPECT_EQ(listener.fills[1].quantity, Quantity{5});
}

TEST(Modify, QuantityIncreaseAtSamePriceLosesQueuePriority) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});

    // Order 1 grows -- this must send it to the back of the queue.
    book.ModifyOrder(OrderId{1}, Price{100}, Quantity{20});

    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{100}, Quantity{10});

    // Order 2 now trades first, since order 1 forfeited its position.
    ASSERT_EQ(listener.fills.size(), 1u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{2});
}

TEST(Modify, PriceChangeLosesQueuePriorityEvenWithASmallerQuantity) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{101}, Quantity{10});

    // Even though the new quantity is smaller, changing price must lose
    // priority -- it's now a different price level entirely.
    book.ModifyOrder(OrderId{2}, Price{100}, Quantity{5});

    // Big enough to sweep both order 1 (10) and order 2 (5) at price 100.
    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{100}, Quantity{15});

    ASSERT_EQ(listener.fills.size(), 2u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{1});
    EXPECT_EQ(listener.fills[1].resting_id, OrderId{2});
}

TEST(Modify, OfAnUnknownOrderIdIsRejectedCleanly) {
    RecordingListener listener;
    OrderBook book(listener);

    book.ModifyOrder(OrderId{404}, Price{100}, Quantity{10});

    ASSERT_EQ(listener.rejected.size(), 1u);
    EXPECT_EQ(listener.rejected[0].second, RejectReason::UnknownOrderId);
}

TEST(Modify, ThatBecomesMarketableExecutesImmediately) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{95}, Quantity{10});

    // Order 2 raises its bid to a price that now crosses the book.
    book.ModifyOrder(OrderId{2}, Price{101}, Quantity{10});

    ASSERT_EQ(listener.fills.size(), 1u);
    EXPECT_EQ(listener.fills[0].aggressor_id, OrderId{2});
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{1});
    EXPECT_TRUE(book.Empty());
}
