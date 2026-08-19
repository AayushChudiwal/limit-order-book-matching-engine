#pragma once

#include <cassert>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/arena.hpp"
#include "lob/bench/churn.hpp"
#include "lob/listener.hpp"
#include "lob/order_book.hpp"  // PriceLevel, RestingOrderView, FullPriceLevel -- shared value types
#include "lob/types.hpp"

namespace lob {

// Phase 5's optimized engine. Forked from OrderBook (include/lob/order_book.hpp)
// at the state that file was in after "Phase 5 step 1: hot-path hygiene"
// (commit e2e0422) -- OrderBook's own class comment is explicit that it
// "stays in the repo permanently" as the naive, obviously-correct
// reference the differential fuzzer checks an optimized engine against,
// and docs/phase5_plan.md's step 2 onward (singleton/small-size levels,
// arena allocation, flat price array, intrusive lists, ...) are real
// data-structure changes, not the "no data structure changes" hygiene
// step 1 already applied directly to the reference. Splitting here means
// OrderBook never needs another line changed for the rest of Phase 5,
// and every step from here on is checked against it via
// RunDifferential<OrderBook, OptimizedOrderBook> (see
// lob::fuzz::EngineUnderTest, tests/test_fuzz_differential_optimized.cpp).
//
// Step 1's hygiene (PruneAndEmitLevelUpdate, FindOrderInLevel, RestOrder
// emitting from its held level reference) is carried over unchanged --
// it's already verified behavior-preserving and there's no reason to
// either lose it or re-derive it here.
//
// Step 2 (this file, first cut): LevelQueue is no longer
// std::deque<RestingOrder>. See LevelOrders below.
class OptimizedOrderBook {
  public:
    explicit OptimizedOrderBook(BookListener& listener) : listener_(listener) {}

    void AddLimitOrder(OrderId id, Side side, Price price, Quantity quantity);
    void AddMarketOrder(OrderId id, Side side, Quantity quantity);
    void CancelOrder(OrderId id);
    void ModifyOrder(OrderId id, Price new_price, Quantity new_quantity);
    void Replace(OrderId old_id, OrderId new_id, Price new_price, Quantity new_quantity);
    void ReduceRestingQuantity(OrderId id, Quantity amount);

    [[nodiscard]] std::optional<Price> BestBid() const;
    [[nodiscard]] std::optional<Price> BestAsk() const;
    [[nodiscard]] bool Empty() const { return bids_.empty() && asks_.empty(); }
    [[nodiscard]] std::vector<PriceLevel> TopLevels(Side side, int depth) const;
    [[nodiscard]] std::vector<FullPriceLevel> FullBook(Side side) const;
    [[nodiscard]] std::optional<Side> SideOf(OrderId id) const;

    // Number of order-storage chunks the arena currently has live.
    //
    // Exposed deliberately, not as a test backdoor: "every chunk this
    // book took, it gave back" is THE invariant of step 4's explicit
    // ownership design (LevelOrders cannot free its own chunks -- see its
    // class comment), and it is otherwise unobservable. A chunk that is
    // never Released is a logical leak INSIDE the arena, not a malloc
    // leak, so neither ASan's leak checker nor a behavioural differential
    // can see it. This is the only way to assert it.
    [[nodiscard]] std::size_t LiveOrderChunks() const { return order_arena_.live_count(); }

  private:
    struct RestingOrder {
        OrderId id;
        Quantity quantity;
    };

    // A fixed-capacity run of resting orders, allocated from the book's
    // arena. Chunks chain (next) so a level of any depth is representable
    // while the common case stays a single chunk.
    //
    // kCapacity = 8 is calibrated, not arbitrary: the captured SPY warmup
    // book's deepest level holds 4 orders (docs/phase5_step2_results.md's
    // churn reproduction), and LevelOrders already stores the first order
    // inline, so one chunk covers every level observed in this data with
    // room to spare. Chaining exists for correctness at depths this data
    // does not reach, not because it is expected to run often.
    struct OrderChunk {
        static constexpr std::uint8_t kCapacity = 8;

        RestingOrder orders[kCapacity];
        std::uint8_t head = 0;   // index of the oldest live order
        std::uint8_t count = 0;  // live orders occupy [head, head + count)
        ArenaHandle next{};      // next chunk in FIFO order, invalid at the tail
    };
    using OrderArena = Arena<OrderChunk>;

    // A price level's resting orders, FIFO order (front() = oldest = next
    // to trade). Small-size-optimized: real levels average ~1.2-1.3
    // orders on captured PSX/NASDAQ data (see
    // docs/benchmark_methodology.md's "resting orders in warmed book"
    // line) -- that number comes from a THIN book (SPY on a truncated
    // NASDAQ file), not a calibration that's assumed to hold at every
    // depth or on a full trading day; a deeper symbol or a complete file
    // would touch the overflow path below more often. That path is fully
    // correct at any depth, just without the inline benefit.
    //
    // The first order at a level is stored INLINE (order_, no allocation
    // of any kind). A second order moves the level into kOverflow, which
    // takes one OrderChunk from the book's arena and holds ALL orders at
    // that level from then on.
    //
    // STEP 4 CHANGED WHERE THAT CHUNK COMES FROM, AND WHAT OWNS IT.
    // Step 2's version held a std::unique_ptr<std::deque<RestingOrder>>:
    // RAII, self-freeing, one malloc for the deque plus the deque's own
    // internal map/block allocations on top. This version holds arena
    // handles instead, which has two consequences worth stating plainly
    // because both are load-bearing:
    //
    // 1. LevelOrders NO LONGER OWNS ITS STORAGE, and cannot. It has no
    //    arena reference (deliberately -- see the layout note below), so
    //    its destructor CANNOT free its chunks. Freeing is explicit:
    //    Release(arena) must be called before a level is erased from
    //    bids_/asks_. Both erase sites do this (PruneAndEmitLevelUpdate,
    //    MatchAgainst). Miss one and chunks leak; do it twice and a
    //    later Allocate() hands the same slot to someone else while a
    //    stale handle still points at it.
    //
    //    That is exactly the "arena allocator reusing a node that's still
    //    referenced" hazard docs/phase5_readiness.md names as invisible
    //    to a behavioural differential test. It is introduced knowingly,
    //    with two mitigations built for it first: generation-tagged
    //    handles (a stale deref is a detected error, not a silent read of
    //    recycled memory -- see include/lob/arena.hpp) and the untagged
    //    ASan nightly that gates arena commits.
    //
    // 2. COPYING A LevelOrders COPIES HANDLES, NOT CHUNKS. That is
    //    correct for the only copy this engine actually performs -- a
    //    whole-book copy, where OptimizedOrderBook copies its arena
    //    alongside its maps and slot indices are preserved, so every
    //    copied handle lands on the copy's own chunk (Arena's copy
    //    constructor documents why indices make this work with no fixup).
    //    It would NOT be correct to copy a single LevelOrders within one
    //    book: the two would alias the same chunks and the second
    //    Release() would be a double free. Nothing does that today
    //    (std::map's operator[] default-constructs, erase destroys, and
    //    only whole-map copy copies values), and this comment exists so
    //    that stays true on purpose rather than by luck.
    //
    // size_ is maintained inline precisely so empty() and size() need no
    // arena argument -- they are called from const, arena-less contexts
    // and from the hot prune path, and threading an arena through them
    // would have spread the arena parameter across most of the class for
    // no benefit.
    //
    // Layout: mode_ (1 byte) + order_ (16 bytes) + two 8-byte handles +
    // size_ (4 bytes) still fits inside the measured 64-byte L1D refill
    // granularity (docs/cache_hierarchy_m4.md) after padding. Note this
    // is LARGER than step 2's version (which had a single 8-byte
    // pointer); the trade is that the chunk it points to is arena memory
    // reused across levels rather than a fresh malloc each time. Whether
    // that trade pays is the measurement this step exists to make, not
    // something assumed here.
    //
    // Deliberately does NOT store a reference/pointer to the arena: that
    // would grow every level by 8 more bytes to hold the same value
    // repeated across every level in the book, and would make LevelOrders
    // non-trivially-copyable in a way the whole-book copy would then have
    // to fix up. Passing the arena per call keeps the stored state to
    // pure indices.
    class LevelOrders {
      public:
        LevelOrders() { bench::RecordLevelCreate(); }
        ~LevelOrders() {
            // No chunk freeing here -- see consequence (1) in the class
            // comment. Release(arena) is the owner's job.
            bench::RecordLevelDestroy();
        }

        LevelOrders(const LevelOrders& other)
            : mode_(other.mode_),
              order_(other.order_),
              head_chunk_(other.head_chunk_),
              tail_chunk_(other.tail_chunk_),
              size_(other.size_),
              total_units_(other.total_units_) {
            bench::RecordLevelCreate();
        }
        LevelOrders& operator=(const LevelOrders& other) {
            if (this == &other) return *this;
            mode_ = other.mode_;
            order_ = other.order_;
            head_chunk_ = other.head_chunk_;
            tail_chunk_ = other.tail_chunk_;
            size_ = other.size_;
            total_units_ = other.total_units_;
            return *this;
        }
        LevelOrders(LevelOrders&&) = default;
        LevelOrders& operator=(LevelOrders&&) = default;

        class iterator {
          public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = RestingOrder;
            using difference_type = std::ptrdiff_t;
            using pointer = RestingOrder*;
            using reference = RestingOrder&;

            reference operator*() const {
                return in_overflow_ ? arena_->Get(chunk_).orders[offset_] : *inline_ptr_;
            }
            pointer operator->() const { return &**this; }

            iterator& operator++() {
                if (!in_overflow_) {
                    inline_ptr_ = nullptr;  // single-element inline range: after ++, at end
                    return *this;
                }
                const OrderChunk& chunk = arena_->Get(chunk_);
                ++offset_;
                if (offset_ >= static_cast<std::uint8_t>(chunk.head + chunk.count)) {
                    chunk_ = chunk.next;
                    offset_ = chunk_.valid() ? arena_->Get(chunk_).head : 0;
                }
                return *this;
            }
            iterator operator++(int) {
                iterator tmp = *this;
                ++(*this);
                return tmp;
            }

            bool operator==(const iterator& rhs) const {
                if (in_overflow_ != rhs.in_overflow_) return false;
                if (!in_overflow_) return inline_ptr_ == rhs.inline_ptr_;
                // end() is an invalid chunk handle; offset is meaningless there.
                if (chunk_ != rhs.chunk_) return false;
                return !chunk_.valid() || offset_ == rhs.offset_;
            }
            bool operator!=(const iterator& rhs) const { return !(*this == rhs); }

          private:
            friend class LevelOrders;
            bool in_overflow_ = false;
            RestingOrder* inline_ptr_ = nullptr;
            OrderArena* arena_ = nullptr;
            ArenaHandle chunk_{};
            std::uint8_t offset_ = 0;
        };

        // Arena-free queries -- see the size_ note in the class comment.
        [[nodiscard]] bool empty() const { return size_ == 0; }
        [[nodiscard]] std::size_t size() const { return size_; }

        // Aggregate resting quantity at this level, maintained
        // INCREMENTALLY rather than recomputed.
        //
        // Every mutation point knows its own delta, so the total is O(1)
        // to read instead of O(orders-in-level) to walk. That matters
        // because OnBookUpdate fires after every add/cancel/reduce/fill
        // (see BookListener), so the walk was on the hot path of every
        // mutating operation -- cheap at this data's 1.2 orders/level,
        // linear in depth on a deeper book, and multiplied by whatever
        // the per-element iteration costs.
        //
        // THE RISK THIS CREATES, AND WHY THE ASSERT IS NOT OPTIONAL:
        // a maintained total can silently DRIFT from reality if any
        // mutation site forgets to adjust it. RunDifferential does not
        // compare OnBookUpdate payloads (it compares fills, accept/reject,
        // BestBid/BestAsk and FullBook), and CheckInvariants derives its
        // resting total from FullBook by walking orders -- so neither
        // would notice a wrong total_units_. That is the same shape of
        // blind spot docs/phase5_readiness.md describes for arena
        // use-after-free: correct-looking output, wrong program.
        //
        // So the debug build re-derives the sum and asserts they agree.
        // O(n) in debug, O(1) in Release -- the tests, CI and the
        // sanitizer jobs all run the checked form (ci.yml passes
        // -UNDEBUG deliberately), the benchmark runs the fast one.
        [[nodiscard]] Quantity Total([[maybe_unused]] const OrderArena& arena) const {
#ifndef NDEBUG
            std::int64_t walked = 0;
            for (auto it = begin(arena); it != end(arena); ++it) {
                walked += it->quantity.units;
            }
            assert(walked == total_units_ &&
                   "level total drifted from the sum of its orders -- a mutation site failed to "
                   "adjust it");
#endif
            return Quantity{total_units_};
        }

        // For mutations that change an order's quantity IN PLACE through
        // a reference handed out by front()/an iterator, where this class
        // never sees the write: partial fills in MatchAgainst,
        // ModifyOrder's same-price reduce, ReduceRestingQuantity.
        void AdjustTotal(std::int64_t delta) { total_units_ += delta; }

        // front()/pop_front() -- MatchAgainst's fill loop always trades
        // the oldest (FIFO) resting order first, same contract as
        // std::deque's.
        [[nodiscard]] RestingOrder& front(OrderArena& arena) {
            if (mode_ != Mode::kOverflow) return order_;
            OrderChunk& chunk = arena.Get(head_chunk_);
            return chunk.orders[chunk.head];
        }

        void pop_front(OrderArena& arena) {
            if (mode_ != Mode::kOverflow) {
                mode_ = Mode::kEmpty;  // was kInline; kEmpty->pop_front() is UB, matches deque
                size_ = 0;
                total_units_ = 0;
                return;
            }
            OrderChunk& chunk = arena.Get(head_chunk_);
            total_units_ -= chunk.orders[chunk.head].quantity.units;
            ++chunk.head;
            --chunk.count;
            --size_;
            if (chunk.count == 0) {
                const ArenaHandle next = chunk.next;
                arena.Free(head_chunk_);
                bench::RecordOrderStorageFree();
                head_chunk_ = next;
                if (!next.valid()) tail_chunk_ = ArenaHandle{};
            }
        }

        void push_back(OrderArena& arena, RestingOrder order) {
            switch (mode_) {
                case Mode::kEmpty:
                    order_ = order;
                    mode_ = Mode::kInline;
                    size_ = 1;
                    total_units_ = order.quantity.units;
                    break;
                case Mode::kInline: {
                    const ArenaHandle handle = arena.Allocate();
                    bench::RecordOrderStorageAlloc();
                    OrderChunk& chunk = arena.Get(handle);
                    chunk.head = 0;
                    chunk.count = 2;
                    chunk.orders[0] = order_;
                    chunk.orders[1] = order;
                    chunk.next = ArenaHandle{};
                    head_chunk_ = handle;
                    tail_chunk_ = handle;
                    mode_ = Mode::kOverflow;
                    size_ = 2;
                    total_units_ += order.quantity.units;
                    break;
                }
                case Mode::kOverflow: {
                    OrderChunk& tail = arena.Get(tail_chunk_);
                    // COMPACT BEFORE DECIDING WE ARE FULL. head only
                    // ever advances (pop_front never rewinds it), so a
                    // chunk whose live run has drifted to the end of the
                    // array reports no room while still holding free
                    // slots at the front. Sliding the live run back to 0
                    // reclaims them.
                    //
                    // Measured effect, honestly: ~3% on the fuzz harness
                    // (10.75s vs 11.08s, 1 seed x 30k ops, differential
                    // mode, idle machine). Worth keeping -- it is a
                    // handful of moves on a path that would otherwise
                    // allocate -- but it is a small optimization, NOT a
                    // fix for unbounded growth. Without it the chunk
                    // count still stays bounded: only the head chunk is
                    // ever pop_front'ed, so a multi-chunk level always
                    // has tail.head == 0 and this is a no-op, and a
                    // single-chunk level merely oscillates between one
                    // and two chunks as the old one drains and is freed.
                    //
                    // (An earlier revision of this comment claimed >13x.
                    // That reading was taken while two sanitizer jobs
                    // were saturating the machine and was an artifact of
                    // the measurement, not of this code -- the same
                    // class of confound docs/phase5_step1_results.md
                    // documents for the CPU-frequency case. Re-measured
                    // on an idle machine before being written down.)
                    if (tail.head > 0 &&
                        static_cast<std::size_t>(tail.head) + tail.count >= OrderChunk::kCapacity) {
                        for (std::uint8_t i = 0; i < tail.count; ++i) {
                            tail.orders[i] = tail.orders[tail.head + i];
                        }
                        tail.head = 0;
                    }
                    if (static_cast<std::size_t>(tail.head) + tail.count < OrderChunk::kCapacity) {
                        tail.orders[tail.head + tail.count] = order;
                        ++tail.count;
                    } else {
                        const ArenaHandle handle = arena.Allocate();
                        bench::RecordOrderStorageAlloc();
                        // NOTE: `tail` above may DANGLE here -- Allocate()
                        // can grow the arena's slot vector, which
                        // reallocates. Handles survive that; references do
                        // not (see include/lob/arena.hpp). Re-Get both.
                        OrderChunk& fresh = arena.Get(handle);
                        fresh.head = 0;
                        fresh.count = 1;
                        fresh.orders[0] = order;
                        fresh.next = ArenaHandle{};
                        arena.Get(tail_chunk_).next = handle;
                        tail_chunk_ = handle;
                    }
                    ++size_;
                    total_units_ += order.quantity.units;
                    break;
                }
            }
        }

        iterator erase(OrderArena& arena, iterator it) {
            if (mode_ != Mode::kOverflow) {
                mode_ = Mode::kEmpty;  // was kInline; erase on kEmpty is UB, matches deque
                size_ = 0;
                total_units_ = 0;
                return end(arena);
            }

            OrderChunk& chunk = arena.Get(it.chunk_);
            total_units_ -= chunk.orders[it.offset_].quantity.units;
            const std::uint8_t last = static_cast<std::uint8_t>(chunk.head + chunk.count - 1);
            for (std::uint8_t i = it.offset_; i < last; ++i) {
                chunk.orders[i] = chunk.orders[i + 1];
            }
            --chunk.count;
            --size_;

            iterator result = it;
            if (chunk.count == 0) {
                const ArenaHandle next = chunk.next;
                if (head_chunk_ == it.chunk_) {
                    head_chunk_ = next;
                    if (!next.valid()) tail_chunk_ = ArenaHandle{};
                } else {
                    ArenaHandle prev = head_chunk_;
                    while (arena.Get(prev).next != it.chunk_) {
                        prev = arena.Get(prev).next;
                    }
                    arena.Get(prev).next = next;
                    if (!next.valid()) tail_chunk_ = prev;
                }
                arena.Free(it.chunk_);
                bench::RecordOrderStorageFree();
                result.chunk_ = next;
                result.offset_ = next.valid() ? arena.Get(next).head : 0;
            } else if (it.offset_ >= static_cast<std::uint8_t>(chunk.head + chunk.count)) {
                // Erased the chunk's last element: advance to the next chunk.
                result.chunk_ = chunk.next;
                result.offset_ = result.chunk_.valid() ? arena.Get(result.chunk_).head : 0;
            }
            return result;
        }

        // Free every chunk this level owns and reset to empty. MUST be
        // called before the level is erased from bids_/asks_ -- the
        // destructor cannot do it. See consequence (1) in the class
        // comment.
        void Release(OrderArena& arena) {
            ArenaHandle handle = head_chunk_;
            while (handle.valid()) {
                const ArenaHandle next = arena.Get(handle).next;
                arena.Free(handle);
                bench::RecordOrderStorageFree();
                handle = next;
            }
            head_chunk_ = ArenaHandle{};
            tail_chunk_ = ArenaHandle{};
            mode_ = Mode::kEmpty;
            size_ = 0;
            total_units_ = 0;
        }

        // Const-callable, but returns the same mutating iterator type via
        // an internal const_cast rather than a parallel const_iterator --
        // this is a private, OrderBook-only type (not a public container),
        // and every const call site (SumLevel, FullBook) is read-only in
        // practice. See the class comment on OrderBook for the same
        // "obviously correct over defensively general" tradeoff.
        [[nodiscard]] iterator begin(const OrderArena& arena) const {
            return const_cast<LevelOrders*>(this)->begin_impl(const_cast<OrderArena&>(arena));
        }
        [[nodiscard]] iterator end(const OrderArena& arena) const {
            return const_cast<LevelOrders*>(this)->end_impl(const_cast<OrderArena&>(arena));
        }

      private:
        iterator begin_impl(OrderArena& arena) {
            iterator it;
            it.arena_ = &arena;
            if (mode_ == Mode::kOverflow) {
                it.in_overflow_ = true;
                it.chunk_ = head_chunk_;
                it.offset_ = head_chunk_.valid() ? arena.Get(head_chunk_).head : 0;
            } else {
                it.in_overflow_ = false;
                it.inline_ptr_ = mode_ == Mode::kInline ? &order_ : nullptr;
            }
            return it;
        }
        iterator end_impl(OrderArena& arena) {
            iterator it;
            it.arena_ = &arena;
            it.in_overflow_ = mode_ == Mode::kOverflow;
            it.inline_ptr_ = nullptr;
            it.chunk_ = ArenaHandle{};
            it.offset_ = 0;
            return it;
        }

        enum class Mode : std::uint8_t { kEmpty, kInline, kOverflow };
        Mode mode_ = Mode::kEmpty;
        RestingOrder order_{};
        ArenaHandle head_chunk_{};
        ArenaHandle tail_chunk_{};
        std::uint32_t size_ = 0;
        std::int64_t total_units_ = 0;
    };
    using LevelQueue = LevelOrders;

    struct Location {
        Side side;
        Price price;
    };

    template <typename LevelMap>
    static LevelQueue* FindLevel(LevelMap& levels, Price price) {
        auto it = levels.find(price);
        return it == levels.end() ? nullptr : &it->second;
    }

    Quantity SumLevel(const LevelQueue& level) const;
    static bool Crosses(Side aggressor_side, Price limit_price, Price level_price);
    LevelQueue::iterator FindOrderInLevel(LevelQueue& level, OrderId id);

    LevelQueue* LevelFor(Side side, Price price);
    void PruneAndEmitLevelUpdate(Side side, Price price);
    void RestOrder(OrderId id, Side side, Price price, Quantity quantity);
    Quantity RemoveFromLevel(Side side, Price price, OrderId id);

    template <typename OppositeMap>
    Quantity MatchAgainst(OppositeMap& opposite, OrderId aggressor_id, Side aggressor_side,
                          std::optional<Price> limit_price, Quantity remaining);

    std::map<Price, LevelQueue, std::greater<Price>> bids_;
    std::map<Price, LevelQueue, std::less<Price>> asks_;
    std::unordered_map<OrderId, Location> locations_;

    // Backs every level's overflow storage. Copied wholesale by the
    // implicit copy constructor alongside bids_/asks_, which is what
    // keeps the handles those maps hold valid in the copy.
    OrderArena order_arena_;

    BookListener& listener_;
};

}  // namespace lob
