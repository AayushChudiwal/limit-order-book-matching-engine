#pragma once

#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/listener.hpp"
#include "lob/types.hpp"

namespace lob {

struct PriceLevel {
    Price price;
    Quantity total_quantity;
};

// Deliberately naive, obviously-correct reference implementation.
//
// Price levels live in a std::map (a balanced tree) so that price ordering
// -- and therefore "what is the best bid/ask" -- falls directly out of
// begin()/end() with no separate bookkeeping to keep in sync. Within a
// level, orders sit in a std::deque in arrival order, so FIFO price-time
// priority is just "match from the front, append at the back".
//
// This is the textbook approach (see WK Selph's "How to Build a Fast Limit
// Order Book"): O(log M) to touch a new price level, O(1) for everything
// else at an existing level, where M is the number of distinct price
// levels. It is not the fast engine -- cancelling an order in the middle of
// a deque is an O(k) linear scan, and every add/cancel touches a tree node
// with pointer-chasing and no cache locality. That is the point: this
// engine exists to be a ground truth that Phase 3's differential fuzzer
// checks an optimized engine against, not to be benchmarked and thrown
// away. It stays in the repo permanently.
class OrderBook {
  public:
    explicit OrderBook(BookListener& listener) : listener_(listener) {}

    // Adds a new limit order. It first matches immediately against the
    // opposite side while price allows (this also covers a "marketable"
    // limit order that crosses the spread and sweeps one or more levels);
    // any unfilled remainder rests in the book at its own limit price.
    void AddLimitOrder(OrderId id, Side side, Price price, Quantity quantity);

    // Adds a market order: matches immediately against the opposite side
    // regardless of price until fully filled or the book is exhausted.
    // Never rests -- see the comment in the .cpp for why.
    void AddMarketOrder(OrderId id, Side side, Quantity quantity);

    // Cancels the full remaining quantity of a resting order.
    void CancelOrder(OrderId id);

    // Modifies a resting order's price and/or quantity. See the .cpp for
    // the priority rules this enforces.
    void ModifyOrder(OrderId id, Price new_price, Quantity new_quantity);

    // Reduces a resting order's quantity by an amount this engine did not
    // itself compute, removing it entirely if that reaches zero -- and,
    // unlike AddLimitOrder/AddMarketOrder, never emits OnFill. A Fill in
    // this engine's model means "an aggressor order this engine matched
    // against a resting one"; a feed that reports its own already-resolved
    // executions (ITCH's Order Executed messages, or its Order Cancel
    // messages, which are mechanically the same reduce-or-remove operation
    // under a different label -- see lob/itch/messages.hpp) never tells you
    // who the aggressor was, so there is no Fill to construct. This is why
    // Phase 2's ITCH replay can't reuse the live-matching path for
    // someone else's order: it can only apply the state transition the
    // feed already computed.
    void ReduceRestingQuantity(OrderId id, Quantity amount);

    [[nodiscard]] std::optional<Price> BestBid() const;
    [[nodiscard]] std::optional<Price> BestAsk() const;
    [[nodiscard]] bool Empty() const { return bids_.empty() && asks_.empty(); }

    // The best `depth` price levels on one side, best-first, each with its
    // aggregated resting quantity. Fewer than `depth` entries come back if
    // the side doesn't have that many distinct price levels. Exists purely
    // for external validation (Phase 2's cross-implementation diff against
    // an independent ITCH reconstructor) -- nothing inside this engine
    // needs more than BestBid/BestAsk, so this is a read-only reporting
    // query, not a hot-path primitive.
    [[nodiscard]] std::vector<PriceLevel> TopLevels(Side side, int depth) const;

    // Which side a resting order is on. ITCH's Order Replace message omits
    // side (the spec is explicit: it can't change, so consumers must
    // remember it from the original Add) -- this is that memory, exposed
    // as a read-only query rather than duplicated into a second tracking
    // map in the replay adapter, since the engine already has to know it
    // internally to place the order in the first place.
    [[nodiscard]] std::optional<Side> SideOf(OrderId id) const;

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
    void EraseLevelIfEmpty(Side side, Price price);
    void EmitLevelUpdate(Side side, Price price);
    void RestOrder(OrderId id, Side side, Price price, Quantity quantity);
    Quantity RemoveFromLevel(Side side, Price price, OrderId id);

    // Shared by AddLimitOrder (limit_price set) and AddMarketOrder
    // (limit_price = nullopt) so the sweep-multiple-levels logic exists in
    // exactly one place -- two independent copies of this loop, one per
    // order type, is how real order books grow a subtle divergence bug
    // between their market- and limit-order matching paths.
    template <typename OppositeMap>
    Quantity MatchAgainst(OppositeMap& opposite, OrderId aggressor_id, Side aggressor_side,
                          std::optional<Price> limit_price, Quantity remaining);

    std::map<Price, LevelQueue, std::greater<Price>> bids_;  // best bid = begin()
    std::map<Price, LevelQueue, std::less<Price>> asks_;     // best ask = begin()
    std::unordered_map<OrderId, Location> locations_;        // order id -> where it rests

    BookListener& listener_;
};

}  // namespace lob
