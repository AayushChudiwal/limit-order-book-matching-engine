#include "lob/optimized_order_book.hpp"

#include <algorithm>
#include <cassert>

namespace lob {

namespace {
constexpr Side Opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }
}  // namespace

Quantity OptimizedOrderBook::SumLevel(const LevelQueue& level) const {
    // O(1) now: LevelOrders maintains this incrementally. Debug builds
    // still re-derive and assert it inside Total() -- see LevelOrders.
    return level.Total(order_arena_);
}

bool OptimizedOrderBook::Crosses(Side aggressor_side, Price limit_price, Price level_price) {
    return aggressor_side == Side::Buy ? limit_price.ticks >= level_price.ticks
                                       : limit_price.ticks <= level_price.ticks;
}

OptimizedOrderBook::LevelQueue* OptimizedOrderBook::LevelFor(Side side, Price price) {
    return side == Side::Buy ? FindLevel(bids_, price) : FindLevel(asks_, price);
}

OptimizedOrderBook::LevelQueue::iterator OptimizedOrderBook::FindOrderInLevel(LevelQueue& level,
                                                                              OrderId id) {
    return std::find_if(level.begin(order_arena_), level.end(order_arena_),
                        [id](const RestingOrder& o) { return o.id == id; });
}

void OptimizedOrderBook::PruneAndEmitLevelUpdate(Side side, Price price) {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        assert(it != bids_.end() &&
               "level must still exist immediately after removing an order from it");
        if (it->second.empty()) {
            // Release BEFORE erase: the level's destructor cannot free
            // its arena chunks (see LevelOrders' class comment), so this
            // is the only thing standing between an erase and a leak.
            it->second.Release(order_arena_);
            bids_.erase(it);
            listener_.OnBookUpdate(side, price, Quantity{0});
        } else {
            listener_.OnBookUpdate(side, price, SumLevel(it->second));
        }
    } else {
        auto it = asks_.find(price);
        assert(it != asks_.end() &&
               "level must still exist immediately after removing an order from it");
        if (it->second.empty()) {
            it->second.Release(order_arena_);
            asks_.erase(it);
            listener_.OnBookUpdate(side, price, Quantity{0});
        } else {
            listener_.OnBookUpdate(side, price, SumLevel(it->second));
        }
    }
}

void OptimizedOrderBook::RestOrder(OrderId id, Side side, Price price, Quantity quantity) {
    LevelQueue& level = side == Side::Buy ? bids_[price] : asks_[price];
    level.push_back(order_arena_, RestingOrder{id, quantity});
    locations_[id] = Location{side, price};
    listener_.OnOrderAccepted(id);
    listener_.OnBookUpdate(side, price, SumLevel(level));
}

Quantity OptimizedOrderBook::RemoveFromLevel(Side side, Price price, OrderId id) {
    LevelQueue* level = LevelFor(side, price);
    assert(level != nullptr && "locations_ points at a level that doesn't exist");
    auto it = FindOrderInLevel(*level, id);
    assert(it != level->end(order_arena_) &&
           "locations_ points at an order that isn't in its level");
    const Quantity quantity = it->quantity;
    level->erase(order_arena_, it);
    return quantity;
}

template <typename OppositeMap>
Quantity OptimizedOrderBook::MatchAgainst(OppositeMap& opposite, OrderId aggressor_id,
                                          Side aggressor_side, std::optional<Price> limit_price,
                                          Quantity remaining) {
    while (remaining.units > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();
        const Price level_price = level_it->first;
        if (limit_price && !Crosses(aggressor_side, *limit_price, level_price)) {
            break;
        }

        LevelQueue& queue = level_it->second;
        while (remaining.units > 0 && !queue.empty()) {
            RestingOrder& resting = queue.front(order_arena_);
            const Quantity traded{std::min(remaining.units, resting.quantity.units)};

            listener_.OnFill(Fill{aggressor_id, resting.id, aggressor_side, level_price, traded});
            remaining -= traded;
            resting.quantity -= traded;
            queue.AdjustTotal(-traded.units);

            if (resting.quantity.units == 0) {
                locations_.erase(resting.id);
                queue.pop_front(order_arena_);
            }
        }

        const bool level_now_empty = queue.empty();
        listener_.OnBookUpdate(Opposite(aggressor_side), level_price,
                               level_now_empty ? Quantity{0} : SumLevel(queue));
        if (level_now_empty) {
            // Same ownership rule as PruneAndEmitLevelUpdate: chunks must
            // be released before the level is erased.
            level_it->second.Release(order_arena_);
            opposite.erase(level_it);
        }
    }
    return remaining;
}

void OptimizedOrderBook::AddLimitOrder(OrderId id, Side side, Price price, Quantity quantity) {
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

void OptimizedOrderBook::AddMarketOrder(OrderId id, Side side, Quantity quantity) {
    if (locations_.contains(id)) {
        listener_.OnOrderRejected(id, RejectReason::DuplicateOrderId);
        return;
    }
    if (quantity.units <= 0) {
        listener_.OnOrderRejected(id, RejectReason::InvalidQuantity);
        return;
    }

    if (side == Side::Buy) {
        MatchAgainst(asks_, id, side, std::nullopt, quantity);
    } else {
        MatchAgainst(bids_, id, side, std::nullopt, quantity);
    }
}

void OptimizedOrderBook::CancelOrder(OrderId id) {
    auto it = locations_.find(id);
    if (it == locations_.end()) {
        listener_.OnOrderRejected(id, RejectReason::UnknownOrderId);
        return;
    }
    const Location loc = it->second;
    const Quantity removed_quantity = RemoveFromLevel(loc.side, loc.price, id);
    locations_.erase(it);

    listener_.OnOrderCancelled(id, removed_quantity);
    PruneAndEmitLevelUpdate(loc.side, loc.price);
}

void OptimizedOrderBook::Replace(OrderId old_id, OrderId new_id, Price new_price,
                                 Quantity new_quantity) {
    const auto side = SideOf(old_id);
    if (!side.has_value()) {
        return;
    }
    CancelOrder(old_id);
    AddLimitOrder(new_id, *side, new_price, new_quantity);
}

void OptimizedOrderBook::ModifyOrder(OrderId id, Price new_price, Quantity new_quantity) {
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
    auto order_it = FindOrderInLevel(*level, id);
    assert(order_it != level->end(order_arena_));

    const bool same_price = new_price == loc.price;
    const bool quantity_reduced_or_equal = new_quantity.units <= order_it->quantity.units;

    if (same_price && quantity_reduced_or_equal) {
        level->AdjustTotal(new_quantity.units - order_it->quantity.units);
        order_it->quantity = new_quantity;
        listener_.OnOrderModified(id, new_quantity);
        listener_.OnBookUpdate(loc.side, loc.price, SumLevel(*level));
        return;
    }

    level->erase(order_arena_, order_it);
    locations_.erase(it);
    PruneAndEmitLevelUpdate(loc.side, loc.price);

    AddLimitOrder(id, loc.side, new_price, new_quantity);
}

void OptimizedOrderBook::ReduceRestingQuantity(OrderId id, Quantity amount) {
    auto it = locations_.find(id);
    if (it == locations_.end()) {
        listener_.OnOrderRejected(id, RejectReason::UnknownOrderId);
        return;
    }
    if (amount.units <= 0) {
        listener_.OnOrderRejected(id, RejectReason::InvalidQuantity);
        return;
    }
    const Location loc = it->second;
    LevelQueue* level = LevelFor(loc.side, loc.price);
    assert(level != nullptr);
    auto order_it = FindOrderInLevel(*level, id);
    assert(order_it != level->end(order_arena_));

    if (order_it->quantity.units < amount.units) {
        listener_.OnOrderRejected(id, RejectReason::InsufficientQuantity);
        return;
    }

    order_it->quantity -= amount;
    level->AdjustTotal(-amount.units);
    if (order_it->quantity.units == 0) {
        level->erase(order_arena_, order_it);
        locations_.erase(it);
    }
    PruneAndEmitLevelUpdate(loc.side, loc.price);
}

std::optional<Price> OptimizedOrderBook::BestBid() const {
    return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
}

std::optional<Price> OptimizedOrderBook::BestAsk() const {
    return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
}

std::optional<Side> OptimizedOrderBook::SideOf(OrderId id) const {
    auto it = locations_.find(id);
    return it == locations_.end() ? std::nullopt : std::optional<Side>(it->second.side);
}

std::vector<PriceLevel> OptimizedOrderBook::TopLevels(Side side, int depth) const {
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

std::vector<FullPriceLevel> OptimizedOrderBook::FullBook(Side side) const {
    std::vector<FullPriceLevel> levels;
    auto append = [&levels, this](const auto& level_map) {
        for (const auto& [price, queue] : level_map) {
            FullPriceLevel level{price, {}};
            level.orders.reserve(queue.size());
            for (auto it = queue.begin(order_arena_); it != queue.end(order_arena_); ++it) {
                level.orders.push_back({it->id, it->quantity});
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
