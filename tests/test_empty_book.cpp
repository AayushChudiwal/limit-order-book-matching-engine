#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

TEST(EmptyBook, StartsEmptyWithNoBestBidOrAsk) {
    RecordingListener listener;
    OrderBook book(listener);

    EXPECT_TRUE(book.Empty());
    EXPECT_FALSE(book.BestBid().has_value());
    EXPECT_FALSE(book.BestAsk().has_value());
}

TEST(EmptyBook, AddingIntoAnEmptyBookJustRestsWithNoFills) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{10});

    EXPECT_TRUE(listener.fills.empty());
    ASSERT_TRUE(book.BestBid().has_value());
    EXPECT_EQ(*book.BestBid(), Price{100});
}

TEST(EmptyBook, MarketOrderIntoAnEmptyBookFillsNothingAndDoesNotCrash) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddMarketOrder(OrderId{1}, Side::Buy, Quantity{10});

    EXPECT_TRUE(listener.fills.empty());
    EXPECT_TRUE(book.Empty());
}

TEST(EmptyBook, CancelOnAnEmptyBookIsRejectedNotUndefinedBehaviour) {
    RecordingListener listener;
    OrderBook book(listener);

    book.CancelOrder(OrderId{1});

    ASSERT_EQ(listener.rejected.size(), 1u);
    EXPECT_EQ(listener.rejected[0].second, RejectReason::UnknownOrderId);
}
