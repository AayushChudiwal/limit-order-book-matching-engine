#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

// "Self-crossing" here means a marketable limit order -- one whose price
// crosses the spread against the resting book -- not a single participant
// trading with themselves (that's self-trade prevention, a Phase 6 order-
// type extension). A marketable limit order must execute immediately
// against the book rather than incorrectly resting behind orders it could
// already trade with.

TEST(SelfCrossing, MarketableLimitOrderExecutesInsteadOfResting) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    // Buy priced above the best ask -- immediately marketable.
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{105}, Quantity{10});

    ASSERT_EQ(listener.fills.size(), 1u);
    // Trades at the resting order's price, not the aggressor's limit.
    EXPECT_EQ(listener.fills[0].price, Price{100});
    EXPECT_FALSE(book.BestBid().has_value());
    EXPECT_FALSE(book.BestAsk().has_value());
}

TEST(SelfCrossing, UnfilledRemainderOfAMarketableLimitOrderRestsAtItsOwnLimitPrice) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{5});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{105}, Quantity{10});

    ASSERT_EQ(listener.fills.size(), 1u);
    EXPECT_EQ(listener.fills[0].quantity, Quantity{5});
    // The remaining 5 shares rest at order 2's own limit price (105), not
    // at the price it traded at.
    ASSERT_TRUE(book.BestBid().has_value());
    EXPECT_EQ(*book.BestBid(), Price{105});
}

TEST(SelfCrossing, SweepsMultipleLevelsButStopsAtItsLimitPrice) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{5});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{101}, Quantity{5});
    book.AddLimitOrder(OrderId{3}, Side::Sell, Price{102}, Quantity{5});

    // A buy limit at 101 should sweep 100 and 101 but must not touch 102.
    book.AddLimitOrder(OrderId{4}, Side::Buy, Price{101}, Quantity{15});

    ASSERT_EQ(listener.fills.size(), 2u);
    EXPECT_EQ(listener.fills[0].price, Price{100});
    EXPECT_EQ(listener.fills[1].price, Price{101});
    // 5 shares of the buy remain unfilled and rest at 101.
    ASSERT_TRUE(book.BestBid().has_value());
    EXPECT_EQ(*book.BestBid(), Price{101});
    // The level at 102 is completely untouched.
    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(*book.BestAsk(), Price{102});
}
