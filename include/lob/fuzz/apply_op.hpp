#pragma once

#include <cstdint>
#include <optional>

#include "lob/fuzz/engine_under_test.hpp"
#include "lob/fuzz/fuzz_listener.hpp"
#include "lob/fuzz/op.hpp"

namespace lob::fuzz {

struct OpOutcome {
    // Signed change in total resting quantity across BOTH sides combined,
    // derived from what the listener actually recorded during this call --
    // not from the op's declared intent -- so it's correct whether the op
    // was accepted, rejected, partially filled, or (Replace) silently a
    // no-op because its target no longer exists.
    std::int64_t quantity_delta = 0;
};

namespace detail {

// Every filled unit leaves the resting side it matched against and never
// enters the aggressor's own resting side (whether the aggressor is a
// limit order's unfilled remainder or -- always -- a market order): a
// pure removal from the combined total, on top of whatever this op's own
// remainder contributes. AddMarketOrder never rests by design (see its
// own doc comment), so only a resting AddLimitOrder contributes a rested
// amount.
template <EngineUnderTest Engine>
std::int64_t ApplyAddLikeAndMeasure(Engine& engine, FuzzListener& listener, OrderId id, Side side,
                                    std::optional<Price> price, Quantity quantity) {
    const std::size_t fills_before = listener.fills.size();
    const std::size_t accepted_before = listener.accepted.size();

    if (price.has_value()) {
        engine.AddLimitOrder(id, side, *price, quantity);
    } else {
        engine.AddMarketOrder(id, side, quantity);
    }

    std::int64_t filled_this_op = 0;
    for (std::size_t i = fills_before; i < listener.fills.size(); ++i) {
        filled_this_op += listener.fills[i].quantity.units;
    }

    // OnOrderAccepted only fires when something actually rests (see
    // RestOrder) -- an exact-fill AddLimitOrder is accepted but rests
    // nothing, so checking the listener (rather than "not rejected")
    // correctly yields rested=0 for that case too.
    const bool accepted_now = listener.accepted.size() > accepted_before;
    const std::int64_t rested =
        (price.has_value() && accepted_now) ? (quantity.units - filled_this_op) : 0;

    return rested - filled_this_op;
}

}  // namespace detail

// Applies one generated op to one engine, performing exactly the calls a
// real adapter would -- Replace through Engine::Replace() (ITCH
// semantics: new id, always forfeits priority; never OrderBook::
// ModifyOrder -- see OrderReplaceViaItchPattern in
// tests/test_reduce_and_side_of.cpp for why) -- and returns the
// resulting change in total resting quantity across both sides combined.
//
// This is the ONE place that knows how a FuzzOp maps onto engine calls:
// the generator uses it to drive its internal bookkeeping engine, and the
// differential harness uses it to drive both engines under comparison.
// Two independent call sites re-deriving "how do I apply a Replace" is
// exactly the kind of divergence risk this whole codebase's comments keep
// warning about -- so there's only one.
template <EngineUnderTest Engine>
OpOutcome ApplyOp(Engine& engine, FuzzListener& listener, const FuzzOp& op) {
    switch (op.kind) {
        case FuzzOp::Kind::AddLimit:
            return {detail::ApplyAddLikeAndMeasure(engine, listener, op.order_id, op.side, op.price,
                                                   op.quantity)};

        case FuzzOp::Kind::AddMarket:
            return {detail::ApplyAddLikeAndMeasure(engine, listener, op.order_id, op.side,
                                                   std::nullopt, op.quantity)};

        case FuzzOp::Kind::Cancel: {
            const std::size_t cancelled_before = listener.cancelled.size();
            engine.CancelOrder(op.order_id);
            if (listener.cancelled.size() > cancelled_before) {
                return {-listener.cancelled.back().second.units};
            }
            return {0};
        }

        case FuzzOp::Kind::Replace: {
            // Engine::Replace() is itself a no-op (no listener events at
            // all) when replace_target doesn't currently rest -- exactly
            // the "replace of an order that no longer exists" pathological
            // case -- so all three before-counts naturally yield delta=0
            // in that case without a separate branch here.
            const std::size_t cancelled_before = listener.cancelled.size();
            const std::size_t fills_before = listener.fills.size();
            const std::size_t accepted_before = listener.accepted.size();

            engine.Replace(op.replace_target, op.order_id, op.price, op.quantity);

            std::int64_t delta = 0;
            if (listener.cancelled.size() > cancelled_before) {
                delta -= listener.cancelled.back().second.units;
            }
            std::int64_t filled_this_op = 0;
            for (std::size_t i = fills_before; i < listener.fills.size(); ++i) {
                filled_this_op += listener.fills[i].quantity.units;
            }
            const bool accepted_now = listener.accepted.size() > accepted_before;
            const std::int64_t rested = accepted_now ? (op.quantity.units - filled_this_op) : 0;
            delta += rested - filled_this_op;
            return {delta};
        }

        case FuzzOp::Kind::ReduceQuantity: {
            // ReduceRestingQuantity has no partial-success path: it either
            // reduces by exactly the requested amount or is rejected
            // outright (unknown id, non-positive amount, or amount
            // exceeding what remains -- see order_book.cpp). No rejection
            // means the full requested amount left the book.
            const std::size_t rejected_before = listener.rejected.size();
            engine.ReduceRestingQuantity(op.replace_target, op.quantity);
            const bool rejected_now = listener.rejected.size() > rejected_before;
            return {rejected_now ? 0 : -op.quantity.units};
        }
    }
    return {0};
}

}  // namespace lob::fuzz
