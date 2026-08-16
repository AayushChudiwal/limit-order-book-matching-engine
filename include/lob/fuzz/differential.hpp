#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "lob/fuzz/apply_op.hpp"
#include "lob/fuzz/engine_under_test.hpp"
#include "lob/fuzz/invariants.hpp"
#include "lob/fuzz/op.hpp"
#include "lob/order_book.hpp"

namespace lob::fuzz {

struct Divergence {
    std::size_t op_index;
    FuzzOp op;
    std::string description;  // human-readable summary of exactly what differed

    // Both engines' full state AFTER the diverging op -- "dump ... both
    // book states, and the minimal diff" from the brief. Captured here
    // rather than left for the caller to re-derive, since re-running to
    // the divergence point a second time to capture it would need the
    // exact same op sequence anyway.
    std::vector<FullPriceLevel> engine_a_bids;
    std::vector<FullPriceLevel> engine_a_asks;
    std::vector<FullPriceLevel> engine_b_bids;
    std::vector<FullPriceLevel> engine_b_asks;
};

struct DifferentialResult {
    bool ok = true;
    std::optional<Divergence> divergence;
};

namespace detail {

inline std::string DescribeFill(const Fill& f) {
    std::ostringstream os;
    os << "{aggressor=" << f.aggressor_id.value << " resting=" << f.resting_id.value
       << " side=" << (f.aggressor_side == Side::Buy ? "Buy" : "Sell") << " price=" << f.price.ticks
       << " qty=" << f.quantity.units << "}";
    return os.str();
}

inline bool FillsEqual(const Fill& a, const Fill& b) {
    return a.aggressor_id == b.aggressor_id && a.resting_id == b.resting_id &&
           a.aggressor_side == b.aggressor_side && a.price == b.price && a.quantity == b.quantity;
}

inline std::string DescribeFullBook(const std::vector<FullPriceLevel>& levels) {
    std::ostringstream os;
    for (const auto& level : levels) {
        os << "[" << level.price.ticks << ": ";
        for (const auto& o : level.orders) {
            os << o.id.value << "x" << o.quantity.units << " ";
        }
        os << "] ";
    }
    return os.str();
}

inline bool FullBookEqual(const std::vector<FullPriceLevel>& a,
                          const std::vector<FullPriceLevel>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].price != b[i].price || a[i].orders.size() != b[i].orders.size()) {
            return false;
        }
        for (std::size_t j = 0; j < a[i].orders.size(); ++j) {
            if (a[i].orders[j].id != b[i].orders[j].id ||
                a[i].orders[j].quantity != b[i].orders[j].quantity) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace detail

// Runs an identical op sequence against two engines, comparing after
// EVERY op -- not at the end, which would hide bugs that cancel out.
// Compares: the fill sequence each op produced (price, qty, maker/taker
// ids, and ORDER -- a FIFO-vs-LIFO bug produces the same fills in a
// different order, invisible to a set comparison), full book state on
// both sides (every level, every order, in queue order -- see
// OrderBook::FullBook's doc comment for why TopLevels() alone isn't
// enough), and reject outcomes (same op must be accepted/rejected
// identically by both). Also runs CheckInvariants on each engine
// independently every step, so a bug that makes both engines wrong in
// exactly the same way -- which a pure A-vs-B diff alone would miss --
// still gets caught, as long as it's a property CheckInvariants covers
// (see the Phase 5 readiness doc for what none of this covers: memory
// safety, UB, allocator/hash-map bugs that don't corrupt observable
// output on a given run).
template <EngineUnderTest EngineA, EngineUnderTest EngineB>
DifferentialResult RunDifferential(std::span<const FuzzOp> ops) {
    FuzzListener listener_a;
    FuzzListener listener_b;
    EngineA engine_a(listener_a);
    EngineB engine_b(listener_b);

    std::int64_t expected_total_a = 0;
    std::int64_t expected_total_b = 0;

    auto make_divergence = [&](std::size_t index, std::string description) {
        return Divergence{index,
                          ops[index],
                          std::move(description),
                          engine_a.FullBook(Side::Buy),
                          engine_a.FullBook(Side::Sell),
                          engine_b.FullBook(Side::Buy),
                          engine_b.FullBook(Side::Sell)};
    };

    for (std::size_t i = 0; i < ops.size(); ++i) {
        const FuzzOp& op = ops[i];

        const std::size_t fills_before_a = listener_a.fills.size();
        const std::size_t accepted_before_a = listener_a.accepted.size();
        const std::size_t rejected_before_a = listener_a.rejected.size();
        const auto outcome_a = ApplyOp(engine_a, listener_a, op);
        expected_total_a += outcome_a.quantity_delta;

        const std::size_t fills_before_b = listener_b.fills.size();
        const std::size_t accepted_before_b = listener_b.accepted.size();
        const std::size_t rejected_before_b = listener_b.rejected.size();
        const auto outcome_b = ApplyOp(engine_b, listener_b, op);
        expected_total_b += outcome_b.quantity_delta;

        // --- fill sequence, this op only ---
        const std::size_t new_fills_a = listener_a.fills.size() - fills_before_a;
        const std::size_t new_fills_b = listener_b.fills.size() - fills_before_b;
        if (new_fills_a != new_fills_b) {
            return {false, make_divergence(i, "fill count differs: A produced " +
                                                  std::to_string(new_fills_a) + ", B produced " +
                                                  std::to_string(new_fills_b))};
        }
        for (std::size_t k = 0; k < new_fills_a; ++k) {
            const auto& fa = listener_a.fills[fills_before_a + k];
            const auto& fb = listener_b.fills[fills_before_b + k];
            if (!detail::FillsEqual(fa, fb)) {
                return {false, make_divergence(i, "fill #" + std::to_string(k) +
                                                      " differs: A=" + detail::DescribeFill(fa) +
                                                      " B=" + detail::DescribeFill(fb))};
            }
        }

        // --- accept/reject outcome, this op only ---
        const bool accepted_a = listener_a.accepted.size() > accepted_before_a;
        const bool accepted_b = listener_b.accepted.size() > accepted_before_b;
        const bool rejected_a = listener_a.rejected.size() > rejected_before_a;
        const bool rejected_b = listener_b.rejected.size() > rejected_before_b;
        if (accepted_a != accepted_b) {
            return {false,
                    make_divergence(i, std::string("acceptance differs: A ") +
                                           (accepted_a ? "accepted" : "did not accept") + ", B " +
                                           (accepted_b ? "accepted" : "did not accept"))};
        }
        if (rejected_a != rejected_b) {
            return {false,
                    make_divergence(i, std::string("rejection differs: A ") +
                                           (rejected_a ? "rejected" : "did not reject") + ", B " +
                                           (rejected_b ? "rejected" : "did not reject"))};
        }
        if (rejected_a && rejected_b &&
            listener_a.rejected.back().second != listener_b.rejected.back().second) {
            return {
                false,
                make_divergence(
                    i, "reject reason differs: A=" +
                           std::to_string(static_cast<int>(listener_a.rejected.back().second)) +
                           " B=" +
                           std::to_string(static_cast<int>(listener_b.rejected.back().second)))};
        }

        // --- full book state, both sides ---
        const auto bids_a = engine_a.FullBook(Side::Buy);
        const auto bids_b = engine_b.FullBook(Side::Buy);
        if (!detail::FullBookEqual(bids_a, bids_b)) {
            return {false,
                    make_divergence(i, "bid side differs: A=" + detail::DescribeFullBook(bids_a) +
                                           " B=" + detail::DescribeFullBook(bids_b))};
        }
        const auto asks_a = engine_a.FullBook(Side::Sell);
        const auto asks_b = engine_b.FullBook(Side::Sell);
        if (!detail::FullBookEqual(asks_a, asks_b)) {
            return {false,
                    make_divergence(i, "ask side differs: A=" + detail::DescribeFullBook(asks_a) +
                                           " B=" + detail::DescribeFullBook(asks_b))};
        }

        // --- best bid/ask, compared directly -- NOT implied by the
        // FullBook comparison above. An engine keeping a separate
        // best-price CACHE (a real, plausible optimization -- see
        // BuggyOrderBook's own comment on why it has one) that goes stale
        // reports a wrong BestBid()/BestAsk() while FullBook(), reading
        // the underlying levels directly, still looks perfectly correct.
        // This exact gap shipped in an earlier version of this function:
        // BestBidCacheNotInvalidated went uncaught across 5 seeds and
        // 3000 ops each until this check was added -- discovered by the
        // mutation-testing verification this file exists to pass, not by
        // inspection. Left in as a reminder in the commit history rather
        // than a silent fix.
        if (engine_a.BestBid() != engine_b.BestBid()) {
            return {
                false,
                make_divergence(
                    i,
                    "best bid differs: A=" +
                        (engine_a.BestBid() ? std::to_string(engine_a.BestBid()->ticks) : "none") +
                        " B=" +
                        (engine_b.BestBid() ? std::to_string(engine_b.BestBid()->ticks) : "none"))};
        }
        if (engine_a.BestAsk() != engine_b.BestAsk()) {
            return {
                false,
                make_divergence(
                    i,
                    "best ask differs: A=" +
                        (engine_a.BestAsk() ? std::to_string(engine_a.BestAsk()->ticks) : "none") +
                        " B=" +
                        (engine_b.BestAsk() ? std::to_string(engine_b.BestAsk()->ticks) : "none"))};
        }

        // --- single-engine invariants, independently on each ---
        if (const auto v = CheckInvariants(engine_a, expected_total_a); !v.empty()) {
            return {false, make_divergence(i, "engine A invariant violated: " + v[0])};
        }
        if (const auto v = CheckInvariants(engine_b, expected_total_b); !v.empty()) {
            return {false, make_divergence(i, "engine B invariant violated: " + v[0])};
        }
    }

    return {true, std::nullopt};
}

}  // namespace lob::fuzz
