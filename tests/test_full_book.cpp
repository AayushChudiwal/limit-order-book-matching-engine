#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

TEST(FullBook, EmptyBookReturnsNoLevels) {
    RecordingListener listener;
    OrderBook book(listener);

    EXPECT_TRUE(book.FullBook(Side::Buy).empty());
    EXPECT_TRUE(book.FullBook(Side::Sell).empty());
}

TEST(FullBook, ReturnsEveryLevelNotJustTopN) {
    RecordingListener listener;
    OrderBook book(listener);

    for (int i = 0; i < 25; ++i) {
        book.AddLimitOrder(OrderId{static_cast<std::uint64_t>(i + 1)}, Side::Buy, Price{100 - i},
                           Quantity{10});
    }

    const auto levels = book.FullBook(Side::Buy);
    ASSERT_EQ(levels.size(), 25u);
    EXPECT_EQ(levels[0].price, Price{100});
    EXPECT_EQ(levels[24].price, Price{76});
}

TEST(FullBook, OrdersWithinALevelAreInFifoArrivalOrder) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{20});
    book.AddLimitOrder(OrderId{3}, Side::Sell, Price{100}, Quantity{30});

    const auto levels = book.FullBook(Side::Sell);
    ASSERT_EQ(levels.size(), 1u);
    ASSERT_EQ(levels[0].orders.size(), 3u);
    EXPECT_EQ(levels[0].orders[0].id, OrderId{1});
    EXPECT_EQ(levels[0].orders[0].quantity, Quantity{10});
    EXPECT_EQ(levels[0].orders[1].id, OrderId{2});
    EXPECT_EQ(levels[0].orders[2].id, OrderId{3});
}

TEST(FullBook, ReflectsCancelsAndPartialFillsAccurately) {
    RecordingListener listener;
    OrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{20});
    book.AddLimitOrder(OrderId{3}, Side::Sell, Price{100}, Quantity{30});

    book.CancelOrder(OrderId{2});
    book.AddLimitOrder(OrderId{4}, Side::Buy, Price{100}, Quantity{5});  // partially fills order 1

    const auto levels = book.FullBook(Side::Sell);
    ASSERT_EQ(levels.size(), 1u);
    ASSERT_EQ(levels[0].orders.size(), 2u);
    EXPECT_EQ(levels[0].orders[0].id, OrderId{1});
    EXPECT_EQ(levels[0].orders[0].quantity, Quantity{5});  // 10 - 5
    EXPECT_EQ(levels[0].orders[1].id, OrderId{3});
}
