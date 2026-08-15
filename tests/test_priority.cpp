#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

TEST(Priority, FifoWithinAPriceLevel) {
    RecordingListener listener;
    OrderBook book(listener);

    // Two resting sells at the same price, in this arrival order.
    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});

    // A buy for the combined size fills order 1 before order 2, purely
    // because it arrived first -- not because of size or price.
    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{100}, Quantity{20});

    ASSERT_EQ(listener.fills.size(), 2u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{1});
    EXPECT_EQ(listener.fills[1].resting_id, OrderId{2});
}

TEST(Priority, BetterPriceLevelMatchesBeforeAnEarlierButWorsePriceLevel) {
    RecordingListener listener;
    OrderBook book(listener);

    // Order 1 (worse price for a buyer) arrives before order 2 (better,
    // cheaper price), but price priority must still win.
    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{101}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});

    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{101}, Quantity{10});

    ASSERT_EQ(listener.fills.size(), 1u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{2});
    EXPECT_EQ(listener.fills[0].price, Price{100});
    // Order 1 at the worse price is untouched.
    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(*book.BestAsk(), Price{101});
}

TEST(Priority, BestBidAndAskTrackTopOfBookAcrossMultipleLevels) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{99}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{3}, Side::Sell, Price{102}, Quantity{10});
    book.AddLimitOrder(OrderId{4}, Side::Sell, Price{101}, Quantity{10});

    ASSERT_TRUE(book.BestBid().has_value());
    EXPECT_EQ(*book.BestBid(), Price{100});
    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(*book.BestAsk(), Price{101});
}
