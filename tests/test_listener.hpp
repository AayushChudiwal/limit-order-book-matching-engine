#pragma once

#include <utility>
#include <vector>

#include "lob/listener.hpp"

namespace lob::testutil {

struct RecordedFill {
    OrderId aggressor_id;
    OrderId resting_id;
    Side aggressor_side;
    Price price;
    Quantity quantity;
};

struct RecordedBookUpdate {
    Side side;
    Price price;
    Quantity total_quantity;
};

// Records every listener callback verbatim so tests can assert on exact
// event sequences (order of fills, which levels updated, what got
// rejected) instead of only on final book state.
class RecordingListener : public BookListener {
  public:
    void OnFill(const Fill& fill) override {
        fills.push_back(
            {fill.aggressor_id, fill.resting_id, fill.aggressor_side, fill.price, fill.quantity});
    }
    void OnOrderAccepted(OrderId id) override { accepted.push_back(id); }
    void OnOrderCancelled(OrderId id, Quantity remaining) override {
        cancelled.emplace_back(id, remaining);
    }
    void OnOrderModified(OrderId id, Quantity new_quantity) override {
        modified.emplace_back(id, new_quantity);
    }
    void OnOrderRejected(OrderId id, RejectReason reason) override {
        rejected.emplace_back(id, reason);
    }
    void OnBookUpdate(Side side, Price price, Quantity total) override {
        book_updates.push_back({side, price, total});
    }

    std::vector<RecordedFill> fills;
    std::vector<OrderId> accepted;
    std::vector<std::pair<OrderId, Quantity>> cancelled;
    std::vector<std::pair<OrderId, Quantity>> modified;
    std::vector<std::pair<OrderId, RejectReason>> rejected;
    std::vector<RecordedBookUpdate> book_updates;
};

}  // namespace lob::testutil
