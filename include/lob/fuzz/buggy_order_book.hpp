#pragma once

#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/fuzz/engine_under_test.hpp"
#include "lob/listener.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob::fuzz {

// Seven realistic, specific bugs a from-scratch matching engine
// implementation (e.g. Phase 5's optimized engine) could plausibly have.
// Each is injected at exactly one point, isolated from the other six --
// this is what proves the differential harness can catch each bug CLASS,
// not just "this particular buggy file".
enum class BugKind {
    None,  // no injected bug -- BuggyOrderBook(listener, None) must behave
           // identically to OrderBook; used as a baseline sanity check that
           // this class's mirrored logic isn't ITSELF subtly wrong before
           // trusting any of the other six as a fair test of the harness.
    CancelUnlinksWrongNode,
    ReplaceKeepsQueuePriority,
    PartialFillLeavesStaleQuantity,
    BestBidCacheNotInvalidated,
    FifoViolatedLifoInstead,
    OffByOneSkipsLastLevelInSweep,
    PriceLevelNotRemovedOnLastCancel,
};

// A second, independent implementation of the SAME price-time-priority
// matching logic as OrderBook, deliberately NOT sharing any code with
// it -- each of the seven BugKind values injects one specific, realistic
// deviation from correct behaviour at one specific point, everything else
// mirroring the reference engine's algorithm. This exists purely to prove
// Phase 3's differential harness (RunDifferential + CheckInvariants +
// the shrinker) actually catches these bug classes BEFORE Phase 5's real
// optimized engine exists to test it against -- see
// docs/fuzz_mutation_testing.md for the results.
//
// Maintains its own best-bid/best-ask CACHE (best_bid_cache_/
// best_ask_cache_) rather than deriving it live from bids_.begin()/
// asks_.begin() the way OrderBook does -- a plausible real optimization
// (avoid a tree lookup on every BestBid() call), and the mechanism
// BestBidCacheNotInvalidated's bug lives in.
class BuggyOrderBook {
  public:
    BuggyOrderBook(BookListener& listener, BugKind bug) : listener_(listener), bug_(bug) {}

    void AddLimitOrder(OrderId id, Side side, Price price, Quantity quantity);
    void AddMarketOrder(OrderId id, Side side, Quantity quantity);
    void CancelOrder(OrderId id);
    void Replace(OrderId old_id, OrderId new_id, Price new_price, Quantity new_quantity);
    void ReduceRestingQuantity(OrderId id, Quantity amount);

    [[nodiscard]] std::optional<Price> BestBid() const { return best_bid_cache_; }
    [[nodiscard]] std::optional<Price> BestAsk() const { return best_ask_cache_; }
    [[nodiscard]] bool Empty() const { return bids_.empty() && asks_.empty(); }
    [[nodiscard]] std::optional<Side> SideOf(OrderId id) const;
    [[nodiscard]] std::vector<PriceLevel> TopLevels(Side side, int depth) const;
    [[nodiscard]] std::vector<FullPriceLevel> FullBook(Side side) const;

  private:
    struct RestingOrder {
        OrderId id;
        Quantity quantity;
    };
    using LevelQueue = std::deque<RestingOrder>;
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

    LevelQueue* LevelFor(Side side, Price price);
    void RefreshBestCache();
    void RestOrder(OrderId id, Side side, Price price, Quantity quantity);
    Quantity RemoveFromLevel(Side side, Price price, OrderId id);

    template <typename OppositeMap>
    Quantity MatchAgainst(OppositeMap& opposite, OrderId aggressor_id, Side aggressor_side,
                          std::optional<Price> limit_price, Quantity remaining);

    std::map<Price, LevelQueue, std::greater<Price>> bids_;
    std::map<Price, LevelQueue, std::less<Price>> asks_;
    std::unordered_map<OrderId, Location> locations_;

    std::optional<Price> best_bid_cache_;
    std::optional<Price> best_ask_cache_;

    BookListener& listener_;
    BugKind bug_;
};

static_assert(EngineUnderTest<BuggyOrderBook>);

// RunDifferential constructs its two engines as EngineA(listener) --
// single-argument -- but BuggyOrderBook needs a BugKind alongside the
// listener to know which bug to be. This is a thin forwarding wrapper
// that bakes the BugKind in as a template parameter instead, giving each
// bug its own distinct, single-argument-constructible type (e.g.
// BuggyOrderBookVariant<BugKind::CancelUnlinksWrongNode>) usable directly
// as RunDifferential's EngineB. BuggyOrderBook itself stays
// runtime-parameterized rather than templated on BugKind throughout,
// since a runtime bug_ is what the mutation-testing CLI tool needs
// (selecting a bug from an argv string at startup).
template <BugKind Kind>
class BuggyOrderBookVariant {
  public:
    explicit BuggyOrderBookVariant(BookListener& listener) : impl_(listener, Kind) {}

    void AddLimitOrder(OrderId id, Side side, Price price, Quantity quantity) {
        impl_.AddLimitOrder(id, side, price, quantity);
    }
    void AddMarketOrder(OrderId id, Side side, Quantity quantity) {
        impl_.AddMarketOrder(id, side, quantity);
    }
    void CancelOrder(OrderId id) { impl_.CancelOrder(id); }
    void Replace(OrderId old_id, OrderId new_id, Price new_price, Quantity new_quantity) {
        impl_.Replace(old_id, new_id, new_price, new_quantity);
    }
    void ReduceRestingQuantity(OrderId id, Quantity amount) {
        impl_.ReduceRestingQuantity(id, amount);
    }

    [[nodiscard]] std::optional<Price> BestBid() const { return impl_.BestBid(); }
    [[nodiscard]] std::optional<Price> BestAsk() const { return impl_.BestAsk(); }
    [[nodiscard]] bool Empty() const { return impl_.Empty(); }
    [[nodiscard]] std::optional<Side> SideOf(OrderId id) const { return impl_.SideOf(id); }
    [[nodiscard]] std::vector<PriceLevel> TopLevels(Side side, int depth) const {
        return impl_.TopLevels(side, depth);
    }
    [[nodiscard]] std::vector<FullPriceLevel> FullBook(Side side) const {
        return impl_.FullBook(side);
    }

  private:
    BuggyOrderBook impl_;
};

}  // namespace lob::fuzz
