#include "lob/order_book.hpp"

#include <algorithm>
#include <cassert>

namespace lob {

namespace {
constexpr Side Opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }
}  // namespace

Quantity OrderBook::SumLevel(const LevelQueue& level) {
    Quantity total{0};
    for (const auto& order : level) {
        total += order.quantity;
    }
    return total;
}

bool OrderBook::Crosses(Side aggressor_side, Price limit_price, Price level_price) {
    // A buy crosses a resting ask if it's willing to pay at least the ask's
    // price; a sell crosses a resting bid if it's willing to accept at most
    // the bid's price.
    return aggressor_side == Side::Buy ? limit_price.ticks >= level_price.ticks
                                       : limit_price.ticks <= level_price.ticks;
}

OrderBook::LevelQueue* OrderBook::LevelFor(Side side, Price price) {
    return side == Side::Buy ? FindLevel(bids_, price) : FindLevel(asks_, price);
}

void OrderBook::EraseLevelIfEmpty(Side side, Price price) {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it != bids_.end() && it->second.empty()) {
            bids_.erase(it);
        }
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end() && it->second.empty()) {
            asks_.erase(it);
        }
    }
}

void OrderBook::EmitLevelUpdate(Side side, Price price) {
    const LevelQueue* level = LevelFor(side, price);
    const Quantity total = level ? SumLevel(*level) : Quantity{0};
    listener_.OnBookUpdate(side, price, total);
}

void OrderBook::RestOrder(OrderId id, Side side, Price price, Quantity quantity) {
    LevelQueue& level = side == Side::Buy ? bids_[price] : asks_[price];
    level.push_back(RestingOrder{id, quantity});
    locations_[id] = Location{side, price};
    listener_.OnOrderAccepted(id);
    EmitLevelUpdate(side, price);
}

Quantity OrderBook::RemoveFromLevel(Side side, Price price, OrderId id) {
    LevelQueue* level = LevelFor(side, price);
    // These two asserts protect an internal invariant (locations_ must
    // always agree with what's actually sitting in the level deques), not a
    // bad-input condition -- unknown order ids from a caller are handled
    // above via OnOrderRejected. If either of these fires, it's an engine
    // bug, which is exactly what assert (not an exception) is for.
    assert(level != nullptr && "locations_ points at a level that doesn't exist");
    auto it = std::find_if(level->begin(), level->end(),
                           [id](const RestingOrder& o) { return o.id == id; });
    assert(it != level->end() && "locations_ points at an order that isn't in its level");
    const Quantity quantity = it->quantity;
    level->erase(it);
    return quantity;
}

template <typename OppositeMap>
Quantity OrderBook::MatchAgainst(OppositeMap& opposite, OrderId aggressor_id, Side aggressor_side,
                                 std::optional<Price> limit_price, Quantity remaining) {
    while (remaining.units > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();
        const Price level_price = level_it->first;
        if (limit_price && !Crosses(aggressor_side, *limit_price, level_price)) {
            break;
        }

        LevelQueue& queue = level_it->second;
        while (remaining.units > 0 && !queue.empty()) {
            RestingOrder& resting = queue.front();
            const Quantity traded{std::min(remaining.units, resting.quantity.units)};

            listener_.OnFill(Fill{aggressor_id, resting.id, aggressor_side, level_price, traded});
            remaining -= traded;
            resting.quantity -= traded;

            if (resting.quantity.units == 0) {
                locations_.erase(resting.id);
                queue.pop_front();
            }
        }

        // Read emptiness before erasing the map node -- queue is a
        // reference into level_it->second, which erase() below invalidates.
        const bool level_now_empty = queue.empty();
        listener_.OnBookUpdate(Opposite(aggressor_side), level_price,
                               level_now_empty ? Quantity{0} : SumLevel(queue));
        if (level_now_empty) {
            opposite.erase(level_it);
        }
    }
    return remaining;
}

void OrderBook::AddLimitOrder(OrderId id, Side side, Price price, Quantity quantity) {
    if (locations_.contains(id)) {
        listener_.OnOrderRejected(id, RejectReason::DuplicateOrderId);
        return;
    }
    if (quantity.units <= 0) {
        listener_.OnOrderRejected(id, RejectReason::InvalidQuantity);
        return;
    }
    if (price.ticks <= 0) {
        listener_.OnOrderRejected(id, RejectReason::InvalidPrice);
        return;
    }

    const Quantity remaining = side == Side::Buy ? MatchAgainst(asks_, id, side, price, quantity)
                                                 : MatchAgainst(bids_, id, side, price, quantity);

    if (remaining.units > 0) {
        RestOrder(id, side, price, remaining);
    }
}

void OrderBook::AddMarketOrder(OrderId id, Side side, Quantity quantity) {
    if (locations_.contains(id)) {
        listener_.OnOrderRejected(id, RejectReason::DuplicateOrderId);
        return;
    }
    if (quantity.units <= 0) {
        listener_.OnOrderRejected(id, RejectReason::InvalidQuantity);
        return;
    }

    // Market orders match with no price limit and never rest: whatever is
    // left once the book is exhausted is simply not filled. A real venue's
    // IOC/FOK semantics (an explicit "unfilled remainder cancelled" event)
    // are a Phase 6 order-type extension; "sweep to completion or silently
    // drop the remainder" is the correct minimal behaviour for a reference
    // engine and is sufficient for this phase's fill-accounting tests,
    // which check filled quantity via the Fill events rather than any
    // separate unfilled-remainder callback.
    if (side == Side::Buy) {
        MatchAgainst(asks_, id, side, std::nullopt, quantity);
    } else {
        MatchAgainst(bids_, id, side, std::nullopt, quantity);
    }
}

void OrderBook::CancelOrder(OrderId id) {
    auto it = locations_.find(id);
    if (it == locations_.end()) {
        listener_.OnOrderRejected(id, RejectReason::UnknownOrderId);
        return;
    }
    const Location loc = it->second;
    const Quantity removed_quantity = RemoveFromLevel(loc.side, loc.price, id);
    locations_.erase(it);
    EraseLevelIfEmpty(loc.side, loc.price);

    listener_.OnOrderCancelled(id, removed_quantity);
    EmitLevelUpdate(loc.side, loc.price);
}

void OrderBook::ModifyOrder(OrderId id, Price new_price, Quantity new_quantity) {
    auto it = locations_.find(id);
    if (it == locations_.end()) {
        listener_.OnOrderRejected(id, RejectReason::UnknownOrderId);
        return;
    }
    if (new_quantity.units <= 0) {
        listener_.OnOrderRejected(id, RejectReason::InvalidQuantity);
        return;
    }

    const Location loc = it->second;
    LevelQueue* level = LevelFor(loc.side, loc.price);
    assert(level != nullptr);
    auto order_it = std::find_if(level->begin(), level->end(),
                                 [id](const RestingOrder& o) { return o.id == id; });
    assert(order_it != level->end());

    const bool same_price = new_price == loc.price;
    const bool quantity_reduced_or_equal = new_quantity.units <= order_it->quantity.units;

    if (same_price && quantity_reduced_or_equal) {
        // Design decision: a quantity DECREASE at an unchanged price keeps
        // queue position. This mirrors real venue behaviour (e.g. NASDAQ
        // INET rules) -- trimming your size shouldn't cost you the same
        // priority as jumping the price or growing your size would. The
        // alternative, treating every modify as an unconditional
        // cancel/replace, is simpler to implement but wrong: real venues
        // don't do that, and it would fail the "modify keeps priority"
        // correctness test by construction rather than by accident.
        order_it->quantity = new_quantity;
        listener_.OnOrderModified(id, new_quantity);
        EmitLevelUpdate(loc.side, loc.price);
        return;
    }

    // A price change, or a quantity increase, forfeits queue position. Pull
    // the order out and resubmit it through AddLimitOrder rather than
    // hand-writing a second re-insertion path -- that also means a modify
    // that happens to become marketable (the new price now crosses the
    // opposite touch) matches immediately instead of silently resting
    // through a crossed book, for free, because it runs through exactly
    // the same matching code a fresh order would. From the listener's
    // perspective this surfaces as a book update at the old price (or a
    // reduced/removed level) followed by a fresh OnOrderAccepted at the new
    // price -- deliberately not a distinct "OnOrderModified" event, since
    // OnOrderAccepted correctly communicates "this order now has a new
    // queue position", which OnOrderModified (reserved for the
    // priority-preserving case above) does not.
    level->erase(order_it);
    locations_.erase(it);
    EraseLevelIfEmpty(loc.side, loc.price);
    EmitLevelUpdate(loc.side, loc.price);

    AddLimitOrder(id, loc.side, new_price, new_quantity);
}

void OrderBook::ReduceRestingQuantity(OrderId id, Quantity amount) {
    auto it = locations_.find(id);
    if (it == locations_.end()) {
        listener_.OnOrderRejected(id, RejectReason::UnknownOrderId);
        return;
    }
    const Location loc = it->second;
    LevelQueue* level = LevelFor(loc.side, loc.price);
    assert(level != nullptr);
    auto order_it = std::find_if(level->begin(), level->end(),
                                 [id](const RestingOrder& o) { return o.id == id; });
    assert(order_it != level->end());

    if (order_it->quantity.units < amount.units) {
        // A real feed reducing an order below zero would mean this
        // engine's reconstruction has already diverged from the feed's --
        // report it rather than let the quantity go negative and corrupt
        // every aggregate level total from here on.
        listener_.OnOrderRejected(id, RejectReason::InsufficientQuantity);
        return;
    }

    order_it->quantity -= amount;
    if (order_it->quantity.units == 0) {
        level->erase(order_it);
        locations_.erase(it);
        EraseLevelIfEmpty(loc.side, loc.price);
    }
    EmitLevelUpdate(loc.side, loc.price);
}

std::optional<Price> OrderBook::BestBid() const {
    return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
}

std::optional<Price> OrderBook::BestAsk() const {
    return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
}

std::optional<Side> OrderBook::SideOf(OrderId id) const {
    auto it = locations_.find(id);
    return it == locations_.end() ? std::nullopt : std::optional<Side>(it->second.side);
}

std::vector<PriceLevel> OrderBook::TopLevels(Side side, int depth) const {
    std::vector<PriceLevel> levels;
    levels.reserve(static_cast<std::size_t>(depth));
    if (side == Side::Buy) {
        for (auto it = bids_.begin(); it != bids_.end() && static_cast<int>(levels.size()) < depth;
             ++it) {
            levels.push_back({it->first, SumLevel(it->second)});
        }
    } else {
        for (auto it = asks_.begin(); it != asks_.end() && static_cast<int>(levels.size()) < depth;
             ++it) {
            levels.push_back({it->first, SumLevel(it->second)});
        }
    }
    return levels;
}

std::vector<FullPriceLevel> OrderBook::FullBook(Side side) const {
    std::vector<FullPriceLevel> levels;
    auto append = [&levels](const auto& level_map) {
        for (const auto& [price, queue] : level_map) {
            FullPriceLevel level{price, {}};
            level.orders.reserve(queue.size());
            for (const auto& order : queue) {
                level.orders.push_back({order.id, order.quantity});
            }
            levels.push_back(std::move(level));
        }
    };
    if (side == Side::Buy) {
        append(bids_);
    } else {
        append(asks_);
    }
    return levels;
}

}  // namespace lob
