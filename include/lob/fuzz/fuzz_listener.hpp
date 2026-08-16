#pragma once

#include <utility>
#include <vector>

#include "lob/listener.hpp"

namespace lob::fuzz {

// Records every listener callback verbatim, like tests/test_listener.hpp's
// RecordingListener -- this is that same idea, living in the library
// rather than the test tree because ApplyOp (below) and the differential
// harness both need it outside of GoogleTest. Kept as a plain field-by-
// field twin rather than shared with the test one: that one is
// test-tree-only by design (tests/ isn't part of any installed or linked
// library target), and duplicating five lines per field is cheaper than
// restructuring the test tree to export it.
class FuzzListener : public BookListener {
  public:
    void OnFill(const Fill& fill) override { fills.push_back(fill); }
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
    void OnBookUpdate(Side, Price, Quantity) override {}

    std::vector<Fill> fills;
    std::vector<OrderId> accepted;
    std::vector<std::pair<OrderId, Quantity>> cancelled;
    std::vector<std::pair<OrderId, Quantity>> modified;
    std::vector<std::pair<OrderId, RejectReason>> rejected;
};

}  // namespace lob::fuzz
