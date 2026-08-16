#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "lob/fuzz/engine_under_test.hpp"

namespace lob::fuzz {

// Structural invariants checked against a SINGLE engine's current state --
// no second engine required. This is what still finds bugs before Phase 5
// gives the differential harness a real second engine to diff against
// (see the Phase 5 readiness doc): book never crossed, price levels
// strictly ordered with no empty levels, no order id resting twice or on
// both sides, every resting quantity positive, and total resting quantity
// matching the running total ApplyOp's deltas predict. Called after every
// op, on every engine, in every fuzz mode.
template <EngineUnderTest Engine>
std::vector<std::string> CheckInvariants(const Engine& engine,
                                         std::int64_t expected_total_resting) {
    std::vector<std::string> violations;

    const auto best_bid = engine.BestBid();
    const auto best_ask = engine.BestAsk();
    if (best_bid.has_value() && best_ask.has_value() && best_bid->ticks >= best_ask->ticks) {
        violations.push_back("book crossed: best_bid=" + std::to_string(best_bid->ticks) +
                             " >= best_ask=" + std::to_string(best_ask->ticks));
    }

    std::unordered_set<std::uint64_t> seen_ids;
    std::int64_t observed_total = 0;

    auto check_side = [&](Side side, const char* side_name) {
        const auto levels = engine.FullBook(side);
        std::optional<std::int64_t> previous_price;
        for (const auto& level : levels) {
            if (level.orders.empty()) {
                violations.push_back(std::string("empty price level left in book on ") + side_name +
                                     " at price " + std::to_string(level.price.ticks));
            }
            if (previous_price.has_value()) {
                const bool correctly_ordered = side == Side::Buy
                                                   ? (level.price.ticks < *previous_price)
                                                   : (level.price.ticks > *previous_price);
                if (!correctly_ordered) {
                    violations.push_back(
                        std::string(side_name) +
                        " price levels not strictly ordered: " + std::to_string(*previous_price) +
                        " then " + std::to_string(level.price.ticks));
                }
            }
            previous_price = level.price.ticks;

            for (const auto& order : level.orders) {
                if (order.quantity.units <= 0) {
                    violations.push_back("non-positive resting quantity for order " +
                                         std::to_string(order.id.value) + ": " +
                                         std::to_string(order.quantity.units));
                }
                if (!seen_ids.insert(order.id.value).second) {
                    violations.push_back("order id " + std::to_string(order.id.value) +
                                         " rests more than once (duplicate level entry or "
                                         "present on both sides)");
                }
                observed_total += order.quantity.units;
            }
        }
    };
    check_side(Side::Buy, "bid");
    check_side(Side::Sell, "ask");

    if (observed_total != expected_total_resting) {
        violations.push_back(
            "total resting quantity not conserved: observed=" + std::to_string(observed_total) +
            " expected=" + std::to_string(expected_total_resting));
    }

    return violations;
}

}  // namespace lob::fuzz
