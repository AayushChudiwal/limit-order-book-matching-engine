#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

TEST(TopLevels, EmptyBookReturnsNoLevels) {
    RecordingListener listener;
    OrderBook book(listener);

    EXPECT_TRUE(book.TopLevels(Side::Buy, 10).empty());
    EXPECT_TRUE(book.TopLevels(Side::Sell, 10).empty());
}

TEST(TopLevels, ReturnsFewerThanDepthWhenNotEnoughLevelsExist) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{99}, Quantity{20});

    const auto levels = book.TopLevels(Side::Buy, 10);
    ASSERT_EQ(levels.size(), 2u);
    EXPECT_EQ(levels[0].price, Price{100});
    EXPECT_EQ(levels[1].price, Price{99});
}

TEST(TopLevels, BidsAreBestFirstDescendingAndAsksAreBestFirstAscending) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{98}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{99}, Quantity{10});

    book.AddLimitOrder(OrderId{4}, Side::Sell, Price{105}, Quantity{10});
    book.AddLimitOrder(OrderId{5}, Side::Sell, Price{103}, Quantity{10});
    book.AddLimitOrder(OrderId{6}, Side::Sell, Price{104}, Quantity{10});

    const auto bids = book.TopLevels(Side::Buy, 10);
    ASSERT_EQ(bids.size(), 3u);
    EXPECT_EQ(bids[0].price, Price{100});
    EXPECT_EQ(bids[1].price, Price{99});
    EXPECT_EQ(bids[2].price, Price{98});

    const auto asks = book.TopLevels(Side::Sell, 10);
    ASSERT_EQ(asks.size(), 3u);
    EXPECT_EQ(asks[0].price, Price{103});
    EXPECT_EQ(asks[1].price, Price{104});
    EXPECT_EQ(asks[2].price, Price{105});
}

TEST(TopLevels, AggregatesQuantityAcrossMultipleOrdersAtTheSameLevel) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{15});
    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{100}, Quantity{5});

    const auto bids = book.TopLevels(Side::Buy, 10);
    ASSERT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids[0].total_quantity, Quantity{30});
}

TEST(TopLevels, TruncatesToTheRequestedDepth) {
    RecordingListener listener;
    OrderBook book(listener);

    for (int i = 0; i < 15; ++i) {
        book.AddLimitOrder(OrderId{static_cast<std::uint64_t>(i + 1)}, Side::Buy, Price{100 - i},
                           Quantity{10});
    }

    const auto bids = book.TopLevels(Side::Buy, 10);
    ASSERT_EQ(bids.size(), 10u);
    EXPECT_EQ(bids[0].price, Price{100});
    EXPECT_EQ(bids[9].price, Price{91});
}
