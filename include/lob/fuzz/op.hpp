#pragma once

#include <cstdint>

#include "lob/types.hpp"

namespace lob::fuzz {

// A single engine operation the generator can emit and the differential
// harness replays. Flat rather than a std::variant of distinct per-kind
// structs: five op kinds with mostly-overlapping fields is clearer as one
// struct with a kind tag than as a variant every consumer has to visit,
// and this gets generated and replayed by the million -- it's not on any
// measured hot path where the unused fields per kind would matter.
struct FuzzOp {
    enum class Kind : std::uint8_t {
        AddLimit,
        AddMarket,
        Cancel,
        // ITCH Order Replace semantics: a brand-new id, always forfeiting
        // queue priority -- deliberately NOT Phase 1's OrderBook::ModifyOrder,
        // whose same-price quantity-decrease case preserves priority. See
        // lob/itch/messages.hpp's OrderReplace comment and
        // OrderReplaceViaItchPattern in tests/test_reduce_and_side_of.cpp.
        Replace,
        // Not client order flow -- an external-execution-report primitive
        // (see OrderBook::ReduceRestingQuantity's doc comment) that Phase 2's
        // ITCH replay depends on and Phase 5's optimized engine must
        // replicate exactly, so it stays in the fuzzer's vocabulary at low
        // frequency for continuous coverage even though it isn't part of
        // the PSX-derived message-mix ratios the other four kinds are.
        ReduceQuantity,
    };

    Kind kind;

    // AddLimit/AddMarket: the new order's id.
    // Cancel: the order to cancel.
    // Replace: the NEW id to assign.
    // ReduceQuantity: unused (see replace_target).
    OrderId order_id;

    // Replace: the id of the order being replaced.
    // ReduceQuantity: the id of the order to reduce.
    // Otherwise unused.
    OrderId replace_target;

    // AddLimit/AddMarket only.
    Side side;

    // AddLimit/Replace only.
    Price price;

    // AddLimit/AddMarket/Replace: the order's quantity.
    // ReduceQuantity: the amount to reduce by.
    Quantity quantity;
};

}  // namespace lob::fuzz
