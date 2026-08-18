#include <gtest/gtest.h>

#include "lob/optimized_order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

// LevelOrders (the inline-1 + heap-overflow small-size-optimized level
// container, see optimized_order_book.hpp) is private to
// OptimizedOrderBook -- these tests exercise it only through the public
// engine API, the same way test_cancel.cpp/test_priority.cpp etc. test
// OrderBook. Focused specifically on the inline<->overflow boundary,
// since that's the new code step 2 introduced; everything else is
// already covered by porting the same op sequences the reference
// engine's own tests use (and by the differential fuzz tests in
// test_fuzz_differential_optimized.cpp).

TEST(OptimizedLevelOrders, SingleOrderAtALevelStaysInlineAndBehavesNormally) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(*book.BestAsk(), Price{100});

    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{10});
    ASSERT_EQ(listener.fills.size(), 1u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{1});
    EXPECT_FALSE(book.BestAsk().has_value());
}

TEST(OptimizedLevelOrders, SecondOrderAtALevelTriggersOverflowAndPreservesFifo) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});  // triggers overflow

    // FIFO: order 1 (first in) must fill before order 2, regardless of
    // internal inline/overflow representation.
    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{100}, Quantity{15});
    ASSERT_EQ(listener.fills.size(), 2u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{1});
    EXPECT_EQ(listener.fills[0].quantity, Quantity{10});
    EXPECT_EQ(listener.fills[1].resting_id, OrderId{2});
    EXPECT_EQ(listener.fills[1].quantity, Quantity{5});
}

TEST(OptimizedLevelOrders, ThirdAndLaterOrdersAppendInOverflowPreservingFifo) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{3}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{4}, Side::Sell, Price{100}, Quantity{10});

    book.AddLimitOrder(OrderId{5}, Side::Buy, Price{100}, Quantity{40});
    ASSERT_EQ(listener.fills.size(), 4u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{1});
    EXPECT_EQ(listener.fills[1].resting_id, OrderId{2});
    EXPECT_EQ(listener.fills[2].resting_id, OrderId{3});
    EXPECT_EQ(listener.fills[3].resting_id, OrderId{4});
}

TEST(OptimizedLevelOrders, CancellingTheMiddleOrderInOverflowPreservesRemainingOrder) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{3}, Side::Sell, Price{100}, Quantity{10});

    book.CancelOrder(OrderId{2});  // remove the middle order while in overflow mode

    book.AddLimitOrder(OrderId{4}, Side::Buy, Price{100}, Quantity{20});
    ASSERT_EQ(listener.fills.size(), 2u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{1});
    EXPECT_EQ(listener.fills[1].resting_id, OrderId{3});
}

TEST(OptimizedLevelOrders, CancellingTheFrontOrderInOverflowAdvancesToTheNextOne) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});

    book.CancelOrder(OrderId{1});  // cancel the front while overflow-backed

    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{100}, Quantity{10});
    ASSERT_EQ(listener.fills.size(), 1u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{2});
}

TEST(OptimizedLevelOrders, DrainingOverflowToEmptyThenReRestingStartsFreshInline) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    // Grow into overflow, then fully drain it -- the level's map entry
    // gets erased (PruneAndEmitLevelUpdate). A LevelOrders instance
    // never needs to shrink back out of overflow because of exactly this:
    // the next order at this price gets a BRAND NEW, freshly
    // default-constructed (inline-mode) LevelOrders, not the old one.
    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});
    book.CancelOrder(OrderId{1});
    book.CancelOrder(OrderId{2});
    EXPECT_FALSE(book.BestAsk().has_value());

    // Re-resting at the same price must behave identically to a level
    // that was never touched -- this is the observable proof the fresh
    // instance starts inline again, not left in some stale overflow state.
    book.AddLimitOrder(OrderId{3}, Side::Sell, Price{100}, Quantity{5});
    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(*book.BestAsk(), Price{100});
    book.AddLimitOrder(OrderId{4}, Side::Buy, Price{100}, Quantity{5});
    ASSERT_EQ(listener.fills.size(), 1u);
    EXPECT_EQ(listener.fills[0].resting_id, OrderId{3});
}

TEST(OptimizedLevelOrders, CopyingTheBookDeepCopiesOverflowStorageIndependently) {
    RecordingListener listener_a;
    OptimizedOrderBook original(listener_a);
    original.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    original.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{20});  // now overflow

    OptimizedOrderBook copy = original;  // exercises LevelOrders' copy ctor's overflow_ deep copy

    // Mutating the copy must not affect the original's overflow storage,
    // and vice versa -- if the copy constructor had shared the
    // unique_ptr<deque<...>> instead of deep-copying it (a use-after-free
    // / double-free waiting to happen, and definitely a correctness bug
    // either way), this would show up as the wrong order filling.
    copy.CancelOrder(OrderId{1});
    original.AddLimitOrder(OrderId{3}, Side::Buy, Price{100}, Quantity{10});
    ASSERT_EQ(listener_a.fills.size(), 1u);
    EXPECT_EQ(listener_a.fills[0].resting_id, OrderId{1})
        << "original's order 1 must still be resting -- cancelling it on the COPY must not "
           "have touched the original's storage";

    // Sanity: the copy's own state is independently correct too (order 1
    // was cancelled on the copy, so only order 2 remains resting on it).
    copy.CancelOrder(OrderId{2});
    EXPECT_FALSE(copy.BestAsk().has_value());
}
