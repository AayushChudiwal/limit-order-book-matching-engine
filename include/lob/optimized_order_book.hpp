#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

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

  private:
    struct RestingOrder {
        OrderId id;
        Quantity quantity;
    };

    // A price level's resting orders, FIFO order (front() = oldest = next
    // to trade). Small-size-optimized: real levels average ~1.2-1.3
    // orders on captured PSX/NASDAQ data (see
    // docs/benchmark_methodology.md's "resting orders in warmed book"
    // line) -- that number comes from a THIN book (SPY on a truncated
    // NASDAQ file), not a calibration that's assumed to hold at every
    // depth or on a full trading day; a deeper symbol or a complete file
    // would touch the overflow path below more often. That path is fully
    // correct at any depth, just without the inline benefit --
    // kInlineCapacity is the one constant to raise if a deeper real
    // workload changes the calibration.
    //
    // The first order at a level is stored INLINE (order_, no
    // allocation). A second order triggers exactly one heap allocation
    // (overflow_, a deque holding ALL orders at that level from then on).
    // This only needs to handle GROWING into overflow, never shrinking
    // back out of it: OrderBook always erases a level's map entry the
    // moment its order count reaches zero (PruneAndEmitLevelUpdate,
    // MatchAgainst) -- a LevelOrders instance is never reused after that
    // point, a fresh one is default-constructed (back at inline, no
    // allocation) the next time an order rests at that price. So an
    // instance's lifetime is monotonic: kEmpty -> kInline -> maybe
    // kOverflow, never back down.
    //
    // Known, BOUNDED (not unbounded, reasoned through and checked
    // against measured evidence, not just theorized -- see
    // docs/phase5_step2_results.md) consequence of that "never back
    // down" choice: a level that peaks at 2+ orders, then gets
    // cancelled back down to exactly 1 (never reaching 0), stays
    // "stranded" in overflow mode -- one heap-allocated deque holding a
    // single element -- until it eventually clears to zero and gets
    // erased (at which point a fresh, non-stranded instance takes over).
    // On the captured SPY warmup book, 18 of 110 levels (~16%) are
    // stranded this way. This does NOT grow without bound over a longer
    // session: SPY's resting-order count is independently measured to
    // plateau rather than grow with message count
    // (docs/benchmark_methodology.md), capping how many levels could
    // ever be simultaneously stranded, and the same high cancel/requote
    // churn that produces that plateau is exactly what clears levels
    // back to zero and un-strands them -- numerator and denominator move
    // together. Never worse than the old std::deque design (which always
    // allocates from the first order regardless) either way. Not fixed
    // here -- would be worth an explicit downgrade-on-drain-to-1 only if
    // a future symbol/venue's order flow shows low cancel share and high
    // resting persistence (weakening the un-stranding mechanism without
    // weakening the rate levels first touch overflow); documented so it
    // isn't rediscovered from scratch.
    //
    // Layout: mode_ (1 byte) + order_ (16 bytes: OrderId + Quantity,
    // each 8-byte) + overflow_ (8-byte pointer) fits well under the
    // measured 64-byte L1D refill granularity (docs/cache_hierarchy_m4.md)
    // even after alignment padding -- multiple levels' worth fit in one
    // refill during a sequential FullBook()/SumLevel() walk. The doc's
    // other measured number, 128-byte coherence line size (its
    // recommended false-sharing padding target on this chip, in
    // preference to the compiler's more conservative 256-byte
    // hardware_destructive_interference_size), is NOT applied here:
    // false-sharing padding matters for structures accessed
    // concurrently by different cores, and each LevelOrders lives inside
    // its own separately-heap-allocated std::map tree node, not in a
    // contiguous array multiple threads touch -- plus this engine is
    // single-threaded by design (see BookListener's comment). Revisit
    // if/when a concurrent consumer (e.g. a market-data publisher thread
    // reading levels while the matching thread writes them) exists, or
    // once step 4 (arena allocation) puts nodes in genuinely contiguous,
    // potentially cross-core-shared storage.
    class LevelOrders {
      public:
        LevelOrders() { bench::RecordLevelCreate(); }
        ~LevelOrders() {
            if (mode_ == Mode::kOverflow) {
                bench::RecordOrderStorageFree();
            }
            bench::RecordLevelDestroy();
        }

        LevelOrders(const LevelOrders& other) : mode_(other.mode_), order_(other.order_) {
            bench::RecordLevelCreate();
            if (other.mode_ == Mode::kOverflow) {
                overflow_ = std::make_unique<std::deque<RestingOrder>>(*other.overflow_);
                bench::RecordOrderStorageAlloc();
            }
        }
        LevelOrders& operator=(const LevelOrders& other) {
            if (this == &other) return *this;
            const bool had_overflow = mode_ == Mode::kOverflow;
            mode_ = other.mode_;
            order_ = other.order_;
            if (other.mode_ == Mode::kOverflow) {
                overflow_ = std::make_unique<std::deque<RestingOrder>>(*other.overflow_);
                if (!had_overflow) bench::RecordOrderStorageAlloc();
            } else {
                overflow_.reset();
                if (had_overflow) bench::RecordOrderStorageFree();
            }
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

            reference operator*() const { return in_overflow_ ? *underlying_ : *inline_ptr_; }
            pointer operator->() const { return in_overflow_ ? &*underlying_ : inline_ptr_; }
            iterator& operator++() {
                if (in_overflow_) {
                    ++underlying_;
                } else {
                    inline_ptr_ = nullptr;  // single-element inline range: after ++, at end
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
                return in_overflow_ ? underlying_ == rhs.underlying_ : inline_ptr_ == rhs.inline_ptr_;
            }
            bool operator!=(const iterator& rhs) const { return !(*this == rhs); }

          private:
            friend class LevelOrders;
            bool in_overflow_ = false;
            RestingOrder* inline_ptr_ = nullptr;
            std::deque<RestingOrder>::iterator underlying_{};
        };

        [[nodiscard]] bool empty() const {
            return mode_ == Mode::kOverflow ? overflow_->empty() : mode_ == Mode::kEmpty;
        }
        [[nodiscard]] std::size_t size() const {
            switch (mode_) {
                case Mode::kEmpty:
                    return 0;
                case Mode::kInline:
                    return 1;
                case Mode::kOverflow:
                    return overflow_->size();
            }
            return 0;  // unreachable; silences -Wreturn-type on some toolchains
        }

        // front()/pop_front() -- MatchAgainst's fill loop always trades
        // the oldest (FIFO) resting order first, same contract as
        // std::deque's.
        [[nodiscard]] RestingOrder& front() {
            return mode_ == Mode::kOverflow ? overflow_->front() : order_;
        }

        void pop_front() {
            if (mode_ == Mode::kOverflow) {
                overflow_->pop_front();
                // Deliberately not downgrading back to kInline/kEmpty
                // even if this drains the overflow queue to 0 or 1 -- see
                // the class comment: whoever owns this instance erases it
                // entirely the moment it's empty, so it never needs to
                // shrink back out of overflow within its own lifetime.
            } else {
                mode_ = Mode::kEmpty;  // was kInline; kEmpty->pop_front() is UB, matches deque
            }
        }

        void push_back(RestingOrder o) {
            switch (mode_) {
                case Mode::kEmpty:
                    order_ = o;
                    mode_ = Mode::kInline;
                    break;
                case Mode::kInline:
                    overflow_ = std::make_unique<std::deque<RestingOrder>>();
                    overflow_->push_back(order_);
                    overflow_->push_back(o);
                    mode_ = Mode::kOverflow;
                    bench::RecordOrderStorageAlloc();
                    break;
                case Mode::kOverflow:
                    overflow_->push_back(o);
                    break;
            }
        }

        iterator erase(iterator it) {
            if (mode_ == Mode::kOverflow) {
                iterator result;
                result.in_overflow_ = true;
                result.underlying_ = overflow_->erase(it.underlying_);
                return result;
            }
            mode_ = Mode::kEmpty;  // was kInline; erase on kEmpty is UB, matches deque
            iterator result;       // == end()
            result.in_overflow_ = false;
            result.inline_ptr_ = nullptr;
            return result;
        }

        // Const-callable, but returns the same mutating iterator type via
        // an internal const_cast rather than a parallel const_iterator --
        // this is a private, OrderBook-only type (not a public container),
        // and every const call site (SumLevel, FullBook) is read-only in
        // practice. See the class comment on OrderBook for the same
        // "obviously correct over defensively general" tradeoff.
        [[nodiscard]] iterator begin() const {
            return const_cast<LevelOrders*>(this)->begin_impl();
        }
        [[nodiscard]] iterator end() const { return const_cast<LevelOrders*>(this)->end_impl(); }

      private:
        iterator begin_impl() {
            iterator it;
            if (mode_ == Mode::kOverflow) {
                it.in_overflow_ = true;
                it.underlying_ = overflow_->begin();
            } else {
                it.in_overflow_ = false;
                it.inline_ptr_ = mode_ == Mode::kInline ? &order_ : nullptr;
            }
            return it;
        }
        iterator end_impl() {
            iterator it;
            if (mode_ == Mode::kOverflow) {
                it.in_overflow_ = true;
                it.underlying_ = overflow_->end();
            } else {
                it.in_overflow_ = false;
                it.inline_ptr_ = nullptr;
            }
            return it;
        }

        enum class Mode : std::uint8_t { kEmpty, kInline, kOverflow };
        Mode mode_ = Mode::kEmpty;
        RestingOrder order_{};
        std::unique_ptr<std::deque<RestingOrder>> overflow_;
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

    static Quantity SumLevel(const LevelQueue& level);
    static bool Crosses(Side aggressor_side, Price limit_price, Price level_price);
    static LevelQueue::iterator FindOrderInLevel(LevelQueue& level, OrderId id);

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

    BookListener& listener_;
};

}  // namespace lob
