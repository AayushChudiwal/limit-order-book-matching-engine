#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

TEST(MarketOrderSweep, FillsCompletelyAgainstOneLevelBeforeMovingToTheNext) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{5});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{101}, Quantity{5});
    book.AddLimitOrder(OrderId{3}, Side::Sell, Price{102}, Quantity{5});

    book.AddMarketOrder(OrderId{4}, Side::Buy, Quantity{12});

    ASSERT_EQ(listener.fills.size(), 3u);
    EXPECT_EQ(listener.fills[0].price, Price{100});
    EXPECT_EQ(listener.fills[0].quantity, Quantity{5});
    EXPECT_EQ(listener.fills[1].price, Price{101});
    EXPECT_EQ(listener.fills[1].quantity, Quantity{5});
    EXPECT_EQ(listener.fills[2].price, Price{102});
    EXPECT_EQ(listener.fills[2].quantity, Quantity{2});

    // 2 shares remain resting at 102; a market order never rests itself.
    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(*book.BestAsk(), Price{102});
}

TEST(MarketOrderSweep, ExhaustingTheBookLeavesTheRemainderUnfilledAndUnrested) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{5});

    book.AddMarketOrder(OrderId{2}, Side::Buy, Quantity{50});

    ASSERT_EQ(listener.fills.size(), 1u);
    EXPECT_EQ(listener.fills[0].quantity, Quantity{5});
    EXPECT_TRUE(book.Empty());
    // Order 2 never appears as a resting order anywhere in the book.
    EXPECT_FALSE(book.BestBid().has_value());
}

TEST(MarketOrderSweep, SellMarketOrderSweepsTheBidSideDescendingByPrice) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{99}, Quantity{5});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{5});

    book.AddMarketOrder(OrderId{3}, Side::Sell, Quantity{7});

    ASSERT_EQ(listener.fills.size(), 2u);
    // Best bid (100) trades first even though it was added second.
    EXPECT_EQ(listener.fills[0].price, Price{100});
    EXPECT_EQ(listener.fills[0].quantity, Quantity{5});
    EXPECT_EQ(listener.fills[1].price, Price{99});
    EXPECT_EQ(listener.fills[1].quantity, Quantity{2});
}
