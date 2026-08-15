#pragma once

#include "lob/types.hpp"

namespace lob {

enum class RejectReason : std::uint8_t {
    UnknownOrderId,
    DuplicateOrderId,
    InvalidQuantity,
    InvalidPrice,
};

struct Fill {
    OrderId aggressor_id;
    OrderId resting_id;
    Side aggressor_side;  // side of the order that triggered this match
    Price price;          // always the resting order's price: price-time
                          // priority means the resting side sets the trade
                          // price, never the aggressor's requested price
    Quantity quantity;
};

// The engine reports every state change through this interface rather than
// returning results synchronously, because a real feed handler has to emit
// the same events (fills, book deltas, rejects) to multiple independent
// consumers -- a market data publisher, a risk system, an audit log -- and
// baking a single return type into AddLimitOrder() would force all of that
// through one channel.
//
// This stays ordinary virtual dispatch for the reference engine. Phase 1's
// goal is an obviously correct engine, and virtual calls are not on any hot
// path yet -- removing them here would be optimising before Phase 4 has a
// benchmark to prove it matters. Phase 5 removes virtual dispatch from the
// *optimized* engine's inner loop once that benchmark exists.
class BookListener {
  public:
    virtual ~BookListener() = default;

    virtual void OnFill(const Fill& fill) = 0;
    virtual void OnOrderAccepted(OrderId id) = 0;
    virtual void OnOrderCancelled(OrderId id, Quantity remaining_quantity) = 0;
    virtual void OnOrderModified(OrderId id, Quantity new_quantity) = 0;
    virtual void OnOrderRejected(OrderId id, RejectReason reason) = 0;

    // Aggregated resting quantity at a price level after some change.
    // total_quantity.units == 0 means the level no longer exists.
    virtual void OnBookUpdate(Side side, Price price, Quantity total_quantity) = 0;
};

}  // namespace lob
