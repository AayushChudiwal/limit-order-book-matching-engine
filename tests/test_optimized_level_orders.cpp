#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

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

// ---------------------------------------------------------------------
// Step 4: arena ownership.
//
// LevelOrders holds arena handles and CANNOT free its own chunks -- its
// destructor has no arena reference. Every level erase site must call
// Release() first. These tests assert that discipline directly, because
// nothing else can: an unreleased chunk is a logical leak inside the
// arena's free list, not a malloc leak, so ASan's leak checker never
// sees it and a differential comparison never diverges because of it.
// ---------------------------------------------------------------------

TEST(OptimizedArenaOwnership, EmptyBookHoldsNoChunks) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);
    EXPECT_EQ(book.LiveOrderChunks(), 0u);
}

TEST(OptimizedArenaOwnership, ASingleOrderNeverTakesAChunk) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{10});

    EXPECT_EQ(book.LiveOrderChunks(), 0u)
        << "the inline slot must still cover the one-order case -- that is step 2's whole win";
}

TEST(OptimizedArenaOwnership, SecondOrderTakesExactlyOneChunk) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{10});

    EXPECT_EQ(book.LiveOrderChunks(), 1u);
}

TEST(OptimizedArenaOwnership, CancellingEveryOrderReleasesEveryChunk) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    for (int i = 1; i <= 30; ++i) {
        book.AddLimitOrder(OrderId{static_cast<std::uint64_t>(i)}, Side::Buy, Price{100},
                           Quantity{10});
    }
    ASSERT_GT(book.LiveOrderChunks(), 0u) << "test needs the overflow path to be exercised";

    for (int i = 1; i <= 30; ++i) {
        book.CancelOrder(OrderId{static_cast<std::uint64_t>(i)});
    }

    EXPECT_TRUE(book.Empty());
    EXPECT_EQ(book.LiveOrderChunks(), 0u) << "cancel path must Release before erasing the level";
}

TEST(OptimizedArenaOwnership, MatchingAwayEveryOrderReleasesEveryChunk) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    for (int i = 1; i <= 30; ++i) {
        book.AddLimitOrder(OrderId{static_cast<std::uint64_t>(i)}, Side::Sell, Price{100},
                           Quantity{10});
    }
    ASSERT_GT(book.LiveOrderChunks(), 0u);

    // One aggressor large enough to consume the whole level.
    book.AddLimitOrder(OrderId{999}, Side::Buy, Price{100}, Quantity{300});

    EXPECT_TRUE(book.Empty());
    EXPECT_EQ(book.LiveOrderChunks(), 0u) << "MatchAgainst must Release before erasing the level";
}

TEST(OptimizedArenaOwnership, ChunksSpanMoreThanOneChunkAndStillFullyRelease) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    // Comfortably past OrderChunk::kCapacity (8) so the chain is several
    // chunks long, exercising the link/unlink paths rather than just the
    // single-chunk case the real SPY data reaches.
    constexpr int kOrders = 50;
    for (int i = 1; i <= kOrders; ++i) {
        book.AddLimitOrder(OrderId{static_cast<std::uint64_t>(i)}, Side::Buy, Price{100},
                           Quantity{10});
    }
    EXPECT_GE(book.LiveOrderChunks(), 5u) << "50 orders must chain multiple 8-slot chunks";

    // Cancel from the MIDDLE outward, so chunks empty out of order and
    // the unlink-a-non-head-chunk path actually runs.
    for (int i = 25; i <= kOrders; ++i) {
        book.CancelOrder(OrderId{static_cast<std::uint64_t>(i)});
    }
    for (int i = 1; i < 25; ++i) {
        book.CancelOrder(OrderId{static_cast<std::uint64_t>(i)});
    }

    EXPECT_TRUE(book.Empty());
    EXPECT_EQ(book.LiveOrderChunks(), 0u);
}

TEST(OptimizedArenaOwnership, RepeatedFillAndDrainCyclesDoNotAccumulateChunks) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    std::size_t peak = 0;
    std::uint64_t next_id = 1;
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 12; ++i) {
            book.AddLimitOrder(OrderId{next_id++}, Side::Buy, Price{100}, Quantity{10});
        }
        peak = std::max(peak, book.LiveOrderChunks());
        // Drain the level entirely via an aggressor.
        book.AddLimitOrder(OrderId{next_id++}, Side::Sell, Price{100}, Quantity{120});
        ASSERT_TRUE(book.Empty()) << "round " << round;
        ASSERT_EQ(book.LiveOrderChunks(), 0u) << "round " << round;
    }

    EXPECT_GT(peak, 0u) << "test needs the overflow path";
}

TEST(OptimizedArenaOwnership, CopiedBookHasIndependentArenaStorage) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);
    for (int i = 1; i <= 5; ++i) {
        book.AddLimitOrder(OrderId{static_cast<std::uint64_t>(i)}, Side::Buy, Price{100},
                           Quantity{10});
    }
    ASSERT_EQ(book.LiveOrderChunks(), 1u);

    // This is exactly what bench_matching_engine does before every timed
    // variant (`pass_book = warmup_book`), so it is not a hypothetical.
    RecordingListener copy_listener;
    OptimizedOrderBook copy = book;
    ASSERT_EQ(copy.LiveOrderChunks(), 1u);

    const auto before = book.FullBook(Side::Buy);

    // Drain the copy completely; the original must be untouched.
    for (int i = 1; i <= 5; ++i) {
        copy.CancelOrder(OrderId{static_cast<std::uint64_t>(i)});
    }
    EXPECT_TRUE(copy.Empty());
    EXPECT_EQ(copy.LiveOrderChunks(), 0u);

    EXPECT_FALSE(book.Empty()) << "the copy must not have shared the original's chunks";
    EXPECT_EQ(book.LiveOrderChunks(), 1u);
    const auto after = book.FullBook(Side::Buy);
    ASSERT_EQ(after.size(), before.size());
    ASSERT_EQ(after[0].orders.size(), 5u);
    for (std::size_t i = 0; i < after[0].orders.size(); ++i) {
        EXPECT_EQ(after[0].orders[i].id, before[0].orders[i].id);
    }
}

// Chunk count must stay proportional to a level's DEPTH, not to the
// number of operations that have flowed through it.
//
// Honest scope note: this does NOT fail if push_back's compaction is
// removed -- verified by disabling it and re-running. Chunk count is
// bounded either way (only the head chunk is pop_front'ed, so a
// multi-chunk level has tail.head == 0 and compaction is a no-op; a
// single-chunk level just oscillates 1<->2 chunks as the drained one is
// freed). It is kept because the bound it asserts is real and easy for a
// future change to break -- e.g. dropping the "free a chunk once it
// drains" branch in pop_front would make this grow without limit while
// every output-comparison test still passed.
TEST(OptimizedArenaOwnership, ChunkCountTracksDepthNotOperationCount) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    // Drain via MATCHING, not cancelling: cancel goes through erase(),
    // which shifts survivors down and never lets head drift, so only the
    // fill path in MatchAgainst exercises pop_front at all.
    constexpr int kResident = 6;
    constexpr int kQty = 10;
    std::uint64_t next_id = 1;
    for (int i = 0; i < kResident; ++i) {
        book.AddLimitOrder(OrderId{next_id++}, Side::Sell, Price{100}, Quantity{kQty});
    }

    std::size_t peak_chunks = book.LiveOrderChunks();
    for (int i = 0; i < 500; ++i) {
        // Aggressor consumes exactly the front resting order -> pop_front.
        book.AddLimitOrder(OrderId{next_id++}, Side::Buy, Price{100}, Quantity{kQty});
        book.AddLimitOrder(OrderId{next_id++}, Side::Sell, Price{100}, Quantity{kQty});
        peak_chunks = std::max(peak_chunks, book.LiveOrderChunks());
    }

    EXPECT_LE(peak_chunks, 2u) << "1000 ops through a 6-deep level must not accumulate chunks";
    EXPECT_EQ(book.FullBook(Side::Sell).at(0).orders.size(), static_cast<std::size_t>(kResident));
}

TEST(OptimizedArenaOwnership, CompactionPreservesFifoOrder) {
    // Compaction moves live elements inside a chunk. If it got the copy
    // direction or bounds wrong, order would scramble -- and price-time
    // priority is the one thing this engine must never get wrong.
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    std::uint64_t next_id = 1;
    for (int i = 0; i < 5; ++i) {
        book.AddLimitOrder(OrderId{next_id++}, Side::Sell, Price{100}, Quantity{10});
    }
    // Drain some from the front, then push more -- forcing compaction.
    for (int i = 0; i < 3; ++i) {
        book.CancelOrder(OrderId{static_cast<std::uint64_t>(i + 1)});
    }
    for (int i = 0; i < 5; ++i) {
        book.AddLimitOrder(OrderId{next_id++}, Side::Sell, Price{100}, Quantity{10});
    }

    const auto levels = book.FullBook(Side::Sell);
    ASSERT_EQ(levels.size(), 1u);
    const auto& orders = levels[0].orders;
    ASSERT_EQ(orders.size(), 7u);
    // Surviving originals (4, 5) first, then the five new ones in order.
    const std::uint64_t expected[] = {4, 5, 6, 7, 8, 9, 10};
    for (std::size_t i = 0; i < orders.size(); ++i) {
        EXPECT_EQ(orders[i].id, OrderId{expected[i]}) << "FIFO position " << i;
    }
}

// ---------------------------------------------------------------------
// Incremental level totals.
//
// SumLevel used to walk a level's orders on every mutating operation,
// because OnBookUpdate reports the level's aggregate resting quantity
// after every change. It is now maintained incrementally and read in
// O(1). LevelOrders::Total() re-derives and asserts it in debug builds,
// but that assert only protects builds with NDEBUG off -- these tests
// check the OBSERVABLE totals, so a drift bug is caught in Release too.
//
// This matters more than a normal correctness test because
// RunDifferential does not compare OnBookUpdate payloads and
// CheckInvariants derives its totals by walking FullBook -- so neither
// would notice a wrong emitted total.
// ---------------------------------------------------------------------

namespace {
// Last total reported for a side/price, or -1 if never reported.
std::int64_t LastTotal(const RecordingListener& l, Side side, Price price) {
    std::int64_t found = -1;
    for (const auto& u : l.book_updates) {
        if (u.side == side && u.price == price) found = u.total_quantity.units;
    }
    return found;
}
}  // namespace

TEST(OptimizedLevelTotals, AddsAccumulate) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{10});
    EXPECT_EQ(LastTotal(listener, Side::Buy, Price{100}), 10);
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{25});
    EXPECT_EQ(LastTotal(listener, Side::Buy, Price{100}), 35);
    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{100}, Quantity{5});
    EXPECT_EQ(LastTotal(listener, Side::Buy, Price{100}), 40);
}

TEST(OptimizedLevelTotals, CancelSubtractsTheCancelledOrderOnly) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);
    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{10});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{25});
    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{100}, Quantity{5});

    book.CancelOrder(OrderId{2});  // from the middle
    EXPECT_EQ(LastTotal(listener, Side::Buy, Price{100}), 15);
    book.CancelOrder(OrderId{1});  // the front
    EXPECT_EQ(LastTotal(listener, Side::Buy, Price{100}), 5);
    book.CancelOrder(OrderId{3});  // the last one -> level gone
    EXPECT_EQ(LastTotal(listener, Side::Buy, Price{100}), 0);
}

TEST(OptimizedLevelTotals, PartialFillReducesTheTotalByTheTradedAmount) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);
    book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{30});
    book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{20});
    ASSERT_EQ(LastTotal(listener, Side::Sell, Price{100}), 50);

    // Consumes all of #1 and part of #2.
    book.AddLimitOrder(OrderId{3}, Side::Buy, Price{100}, Quantity{40});
    EXPECT_EQ(LastTotal(listener, Side::Sell, Price{100}), 10);
}

TEST(OptimizedLevelTotals, ReduceRestingQuantityAdjustsTheTotal) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);
    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{30});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{20});

    book.ReduceRestingQuantity(OrderId{1}, Quantity{12});
    EXPECT_EQ(LastTotal(listener, Side::Buy, Price{100}), 38);

    book.ReduceRestingQuantity(OrderId{1}, Quantity{18});  // to zero -> removed
    EXPECT_EQ(LastTotal(listener, Side::Buy, Price{100}), 20);
}

TEST(OptimizedLevelTotals, ModifyInPlaceAdjustsTheTotal) {
    RecordingListener listener;
    OptimizedOrderBook book(listener);
    book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{30});
    book.AddLimitOrder(OrderId{2}, Side::Buy, Price{100}, Quantity{20});

    // Same price, reduced quantity -> in-place path, keeps queue position.
    book.ModifyOrder(OrderId{1}, Price{100}, Quantity{5});
    EXPECT_EQ(LastTotal(listener, Side::Buy, Price{100}), 25);
}

TEST(OptimizedLevelTotals, TotalsStayCorrectAcrossDeepChurn) {
    // Long mixed sequence spanning several chunks, so compaction,
    // chunk unlink and the in-place mutation paths all run. The
    // expected total is tracked independently here.
    RecordingListener listener;
    OptimizedOrderBook book(listener);

    std::int64_t expected = 0;
    std::uint64_t next_id = 1;
    std::vector<std::pair<std::uint64_t, std::int64_t>> live;

    for (int i = 0; i < 40; ++i) {
        const std::int64_t q = 1 + (i % 7);
        book.AddLimitOrder(OrderId{next_id}, Side::Buy, Price{100}, Quantity{q});
        live.emplace_back(next_id, q);
        ++next_id;
        expected += q;
        ASSERT_EQ(LastTotal(listener, Side::Buy, Price{100}), expected) << "after add " << i;
    }

    // Cancel every third, from the middle outward.
    for (std::size_t i = live.size(); i-- > 0;) {
        if (i % 3 != 0) continue;
        book.CancelOrder(OrderId{live[i].first});
        expected -= live[i].second;
        ASSERT_EQ(LastTotal(listener, Side::Buy, Price{100}), expected) << "after cancel " << i;
    }

    EXPECT_GT(expected, 0);
    EXPECT_EQ(book.FullBook(Side::Buy).at(0).price, Price{100});
}
