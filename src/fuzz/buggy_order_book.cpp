#include "lob/fuzz/buggy_order_book.hpp"

#include <algorithm>
#include <cassert>

namespace lob::fuzz {

Quantity BuggyOrderBook::SumLevel(const LevelQueue& level) {
    Quantity total{0};
    for (const auto& order : level) {
        total += order.quantity;
    }
    return total;
}

bool BuggyOrderBook::Crosses(Side aggressor_side, Price limit_price, Price level_price) {
    return aggressor_side == Side::Buy ? limit_price.ticks >= level_price.ticks
                                       : limit_price.ticks <= level_price.ticks;
}

BuggyOrderBook::LevelQueue* BuggyOrderBook::LevelFor(Side side, Price price) {
    return side == Side::Buy ? FindLevel(bids_, price) : FindLevel(asks_, price);
}

void BuggyOrderBook::RefreshBestCache() {
    best_bid_cache_ = bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
    best_ask_cache_ = asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
}

void BuggyOrderBook::RestOrder(OrderId id, Side side, Price price, Quantity quantity) {
    LevelQueue& level = side == Side::Buy ? bids_[price] : asks_[price];
    level.push_back(RestingOrder{id, quantity});
    locations_[id] = Location{side, price};
    listener_.OnOrderAccepted(id);
    RefreshBestCache();
}

Quantity BuggyOrderBook::RemoveFromLevel(Side side, Price price, OrderId id) {
    LevelQueue* level = LevelFor(side, price);
    assert(level != nullptr);

    if (bug_ == BugKind::CancelUnlinksWrongNode) {
        // BUG: always unlinks the FRONT of the level's queue, regardless
        // of which id was actually requested. Correct only by coincidence
        // when the requested id happens to already be at the front (a
        // single-order level, or the oldest order at that level).
        const Quantity q = level->front().quantity;
        level->pop_front();
        return q;
    }

    auto it = std::find_if(level->begin(), level->end(),
                           [id](const RestingOrder& o) { return o.id == id; });
    assert(it != level->end());
    const Quantity quantity = it->quantity;
    level->erase(it);
    return quantity;
}

template <typename OppositeMap>
Quantity BuggyOrderBook::MatchAgainst(OppositeMap& opposite, OrderId aggressor_id,
                                      Side aggressor_side, std::optional<Price> limit_price,
                                      Quantity remaining) {
    while (remaining.units > 0 &&
           (bug_ == BugKind::OffByOneSkipsLastLevelInSweep ? opposite.size() > 1
                                                           : !opposite.empty())) {
        auto level_it = opposite.begin();
        const Price level_price = level_it->first;
        if (limit_price && !Crosses(aggressor_side, *limit_price, level_price)) {
            break;
        }

        LevelQueue& queue = level_it->second;
        const bool lifo = (bug_ == BugKind::FifoViolatedLifoInstead);

        while (remaining.units > 0 && !queue.empty()) {
            RestingOrder& resting = lifo ? queue.back() : queue.front();
            const Quantity traded{std::min(remaining.units, resting.quantity.units)};

            listener_.OnFill(Fill{aggressor_id, resting.id, aggressor_side, level_price, traded});
            remaining -= traded;

            const bool fully_consumed = (traded.units == resting.quantity.units);
            if (!(bug_ == BugKind::PartialFillLeavesStaleQuantity && !fully_consumed)) {
                // BUG (when active and this fill is partial): skip this
                // update entirely, leaving the resting order's quantity
                // stale -- too high -- for every subsequent read. The
                // Fill event reported above is still correct; only the
                // book's own bookkeeping silently drifts.
                resting.quantity -= traded;
            }

            if (resting.quantity.units == 0) {
                locations_.erase(resting.id);
                if (lifo) {
                    queue.pop_back();
                } else {
                    queue.pop_front();
                }
            }
        }

        if (queue.empty()) {
            opposite.erase(level_it);
        }
    }
    return remaining;
}

void BuggyOrderBook::AddLimitOrder(OrderId id, Side side, Price price, Quantity quantity) {
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
    } else {
        RefreshBestCache();
    }
}

void BuggyOrderBook::AddMarketOrder(OrderId id, Side side, Quantity quantity) {
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
    RefreshBestCache();
}

void BuggyOrderBook::CancelOrder(OrderId id) {
    auto it = locations_.find(id);
    if (it == locations_.end()) {
        listener_.OnOrderRejected(id, RejectReason::UnknownOrderId);
        return;
    }
    const Location loc = it->second;
    const Quantity removed_quantity = RemoveFromLevel(loc.side, loc.price, id);
    locations_.erase(it);

    LevelQueue* level = LevelFor(loc.side, loc.price);
    const bool level_now_empty = (level != nullptr && level->empty());
    if (level_now_empty && bug_ != BugKind::PriceLevelNotRemovedOnLastCancel) {
        if (loc.side == Side::Buy) {
            bids_.erase(loc.price);
        } else {
            asks_.erase(loc.price);
        }
    }

    listener_.OnOrderCancelled(id, removed_quantity);

    if (level_now_empty && bug_ == BugKind::BestBidCacheNotInvalidated) {
        // BUG: the level that held the best price on this side just
        // emptied (and, in every OTHER bug variant, was just erased from
        // the map), but the cache is never told -- best_bid_cache_/
        // best_ask_cache_ keeps pointing at a price that no longer has
        // any resting orders (or, combined with
        // PriceLevelNotRemovedOnLastCancel specifically, wouldn't even be
        // the actual best anymore once a real next-best level exists).
        return;
    }
    RefreshBestCache();
}

void BuggyOrderBook::Replace(OrderId old_id, OrderId new_id, Price new_price,
                             Quantity new_quantity) {
    auto it = locations_.find(old_id);
    if (it == locations_.end()) {
        return;  // mirrors the real adapter pattern: nothing to replace, nothing happens
    }
    const Location loc = it->second;

    if (bug_ == BugKind::ReplaceKeepsQueuePriority && new_price.ticks == loc.price.ticks) {
        // BUG: at an unchanged price, mutate the existing node's id and
        // quantity IN PLACE instead of cancelling and re-resting fresh --
        // keeps its position in the FIFO queue instead of forfeiting it.
        // Real ITCH Order Replace semantics never do this: a brand-new
        // day-unique reference number always starts at the back, even
        // when the price hasn't moved (see OrderReplaceViaItchPattern in
        // tests/test_reduce_and_side_of.cpp). A price CHANGE still falls
        // through to the correct path below even in this buggy variant --
        // there's no queue to stay in at a level that didn't exist yet.
        LevelQueue* level = LevelFor(loc.side, loc.price);
        assert(level != nullptr);
        auto order_it = std::find_if(level->begin(), level->end(),
                                     [old_id](const RestingOrder& o) { return o.id == old_id; });
        assert(order_it != level->end());
        locations_.erase(it);
        order_it->id = new_id;
        order_it->quantity = new_quantity;
        locations_[new_id] = Location{loc.side, loc.price};
        return;
    }

    CancelOrder(old_id);
    AddLimitOrder(new_id, loc.side, new_price, new_quantity);
}

void BuggyOrderBook::ReduceRestingQuantity(OrderId id, Quantity amount) {
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
    auto order_it = std::find_if(level->begin(), level->end(),
                                 [id](const RestingOrder& o) { return o.id == id; });
    assert(order_it != level->end());

    if (order_it->quantity.units < amount.units) {
        listener_.OnOrderRejected(id, RejectReason::InsufficientQuantity);
        return;
    }

    order_it->quantity -= amount;
    if (order_it->quantity.units == 0) {
        level->erase(order_it);
        locations_.erase(it);
        if (level->empty()) {
            if (loc.side == Side::Buy) {
                bids_.erase(loc.price);
            } else {
                asks_.erase(loc.price);
            }
        }
    }
    RefreshBestCache();
}

std::optional<Side> BuggyOrderBook::SideOf(OrderId id) const {
    auto it = locations_.find(id);
    return it == locations_.end() ? std::nullopt : std::optional<Side>(it->second.side);
}

std::vector<PriceLevel> BuggyOrderBook::TopLevels(Side side, int depth) const {
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

std::vector<FullPriceLevel> BuggyOrderBook::FullBook(Side side) const {
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

}  // namespace lob::fuzz
