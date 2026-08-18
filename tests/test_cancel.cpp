#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "test_listener.hpp"

using namespace lob;
using lob::testutil::RecordingListener;

namespace {
// Phase 5 step 1 moved CancelOrder's empty-level erase to run after
// OnOrderCancelled fires instead of before (see PruneAndEmitLevelUpdate).
// No listener in this repo queries book state from inside a callback today,
// so nothing currently observes the difference -- but the public API
// (BestBid/BestAsk/TopLevels/FullBook) doesn't stop a listener from holding
// a pointer back to its own book and doing exactly that. This listener does,
// so the transient state is pinned down by a test rather than left as an
// unverified claim in a commit message.
class BookQueryingListener : public BookListener {
  public:
    void SetBook(OrderBook* book) { book_ = book; }

    void OnFill(const Fill&) override {}
    void OnOrderAccepted(OrderId) override {}
    void OnOrderCancelled(OrderId, Quantity) override {
        best_ask_during_callback = book_->BestAsk();
    }
    void OnOrderModified(OrderId, Quantity) override {}
    void OnOrderRejected(OrderId, RejectReason) override {}
    void OnBookUpdate(Side, Price, Quantity) override {}

    std::optional<Price> best_ask_during_callback;

  private:
    OrderBook* book_ = nullptr;
};
}  // namespace

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

TEST(Cancel, DuringOnOrderCancelledTheJustEmptiedLevelIsTransientlyStillQueryable) {
    BookQueryingListener listener;
    OrderBook book(listener);
    listener.SetBook(&book);

    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    book.CancelOrder(OrderId{1});  // the only order at this level

    // Mid-callback, the level hasn't been pruned yet (PruneAndEmitLevelUpdate
    // runs after OnOrderCancelled), so a listener querying the book sees a
    // stale, already-empty price level rather than no level at all. This
    // matches the pre-existing OnBookUpdate contract elsewhere in the engine
    // (see MatchAgainst, which has always emitted before erasing) rather than
    // introducing a new kind of inconsistency -- but it IS a genuine change
    // from what CancelOrder itself used to guarantee, so it's pinned here
    // rather than left implicit.
    ASSERT_TRUE(listener.best_ask_during_callback.has_value());
    EXPECT_EQ(*listener.best_ask_during_callback, Price{100});

    // Once CancelOrder returns, the level is gone as expected.
    EXPECT_FALSE(book.BestAsk().has_value());
}
