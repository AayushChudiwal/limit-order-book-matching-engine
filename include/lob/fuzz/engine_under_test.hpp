#pragma once

#include <concepts>
#include <optional>
#include <vector>

#include "lob/order_book.hpp"

namespace lob::fuzz {

// Anything the differential harness can run must expose this surface --
// the same public interface OrderBook has. A C++20 concept rather than a
// virtual base class, deliberately: this must eventually run against
// Phase 5's optimized engine, which must not carry virtual dispatch
// overhead on its hot path (see order_book.hpp's own comment on why
// BookListener stays ordinary virtual dispatch only because it isn't
// hot-path -- the engine itself must stay concrete). A concept is checked
// at compile time and costs nothing at runtime: the harness is a
// template, and any two concrete types satisfying this shape can be
// diffed against each other with no adapter layer in between.
template <typename E>
concept EngineUnderTest = requires(E engine, const E const_engine, OrderId id, Side side,
                                   Price price, Quantity quantity, int depth) {
    { engine.AddLimitOrder(id, side, price, quantity) } -> std::same_as<void>;
    { engine.AddMarketOrder(id, side, quantity) } -> std::same_as<void>;
    { engine.CancelOrder(id) } -> std::same_as<void>;
    { engine.ReduceRestingQuantity(id, quantity) } -> std::same_as<void>;
    { const_engine.BestBid() } -> std::same_as<std::optional<Price>>;
    { const_engine.BestAsk() } -> std::same_as<std::optional<Price>>;
    { const_engine.Empty() } -> std::same_as<bool>;
    { const_engine.SideOf(id) } -> std::same_as<std::optional<Side>>;
    { const_engine.TopLevels(side, depth) } -> std::same_as<std::vector<PriceLevel>>;
    { const_engine.FullBook(side) } -> std::same_as<std::vector<FullPriceLevel>>;
};

static_assert(EngineUnderTest<OrderBook>,
              "the reference engine itself must satisfy the concept the fuzzer is built on");

}  // namespace lob::fuzz
