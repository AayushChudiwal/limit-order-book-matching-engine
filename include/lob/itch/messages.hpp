#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "lob/itch/decode.hpp"

namespace lob::itch {

// Field offsets below are transcribed directly from the NASDAQ
// TotalView-ITCH 5.0 specification (03/06/2015), section 4. Every message
// shares a five-field header -- Message Type(1), Stock Locate(2), Tracking
// Number(2), Timestamp(6) -- so only the type-specific tail differs
// message to message; these decoders only pull the fields Phase 2 actually
// needs (book reconstruction + referential-integrity checking), not every
// field the spec defines. Notably, Tracking Number is never read: it's an
// internal NASDAQ routing field with no bearing on book state.
//
// price_ticks fields are the raw wire integer, unconverted -- per Phase
// 1's Price type, "ticks" already means "this engine's fixed-point unit",
// and ITCH's Price(4) (an integer with 4 implied decimal places) maps onto
// that with zero conversion. That equivalence is exactly why Phase 1
// chose an integer-ticks Price type in the first place.

inline std::string_view ToStringView(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Shared by 'A' (Add Order -- No MPID Attribution, 36-byte body) and 'F'
// (Add Order With MPID Attribution, 40-byte body): both carry the same
// fields at the same offsets through Price; 'F' just has 4 extra
// attribution bytes afterward that book reconstruction doesn't need.
struct AddOrder {
    std::uint16_t stock_locate;
    std::uint64_t timestamp_ns;
    std::uint64_t order_ref;
    char side;  // 'B' or 'S'
    std::uint32_t shares;
    std::string_view stock;  // 8 bytes, space-padded
    std::uint32_t price_ticks;
};

inline AddOrder DecodeAddOrder(std::span<const std::byte> body) {
    AddOrder m;
    m.stock_locate = ReadU16(body.data() + 1);
    m.timestamp_ns = ReadU48(body.data() + 5);
    m.order_ref = ReadU64(body.data() + 11);
    m.side = static_cast<char>(std::to_integer<std::uint8_t>(body[19]));
    m.shares = ReadU32(body.data() + 20);
    m.stock = ToStringView(body.subspan(24, 8));
    m.price_ticks = ReadU32(body.data() + 32);
    return m;
}

// 'E' Order Executed (23-byte body).
struct OrderExecuted {
    std::uint16_t stock_locate;
    std::uint64_t timestamp_ns;
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
};

inline OrderExecuted DecodeOrderExecuted(std::span<const std::byte> body) {
    OrderExecuted m;
    m.stock_locate = ReadU16(body.data() + 1);
    m.timestamp_ns = ReadU48(body.data() + 5);
    m.order_ref = ReadU64(body.data() + 11);
    m.executed_shares = ReadU32(body.data() + 19);
    return m;
}

// 'C' Order Executed With Price (36-byte body). Book-state effect is
// identical to a plain Order Executed -- reduce the resting order's
// quantity by executed_shares -- so this intentionally shares the same
// shape rather than adding fields (printable, execution price) that book
// reconstruction has no use for; they matter for time-and-sales, which is
// out of scope here.
using OrderExecutedWithPrice = OrderExecuted;

inline OrderExecutedWithPrice DecodeOrderExecutedWithPrice(std::span<const std::byte> body) {
    OrderExecutedWithPrice m;
    m.stock_locate = ReadU16(body.data() + 1);
    m.timestamp_ns = ReadU48(body.data() + 5);
    m.order_ref = ReadU64(body.data() + 11);
    m.executed_shares = ReadU32(body.data() + 19);
    return m;
}

// 'X' Order Cancel -- a PARTIAL reduction of display size (19-byte body).
// Distinct from 'D' Order Delete, which removes the order entirely.
struct OrderCancel {
    std::uint16_t stock_locate;
    std::uint64_t timestamp_ns;
    std::uint64_t order_ref;
    std::uint32_t canceled_shares;
};

inline OrderCancel DecodeOrderCancel(std::span<const std::byte> body) {
    OrderCancel m;
    m.stock_locate = ReadU16(body.data() + 1);
    m.timestamp_ns = ReadU48(body.data() + 5);
    m.order_ref = ReadU64(body.data() + 11);
    m.canceled_shares = ReadU32(body.data() + 19);
    return m;
}

// 'D' Order Delete -- full removal (15-byte body).
struct OrderDelete {
    std::uint16_t stock_locate;
    std::uint64_t timestamp_ns;
    std::uint64_t order_ref;
};

inline OrderDelete DecodeOrderDelete(std::span<const std::byte> body) {
    OrderDelete m;
    m.stock_locate = ReadU16(body.data() + 1);
    m.timestamp_ns = ReadU48(body.data() + 5);
    m.order_ref = ReadU64(body.data() + 11);
    return m;
}

// 'U' Order Replace (35-byte body). Per the spec, side/stock/attribution
// aren't included -- they can't change, so consumers must remember them
// from the original Add. new_order_ref is a BRAND NEW day-unique
// reference, unconditionally: this is why Order Replace always forfeits
// queue priority, even when the new quantity is smaller than the old one
// (unlike Phase 1's OrderBook::ModifyOrder, which preserves priority on a
// same-price quantity decrease -- that rule describes a different,
// order-management-level "modify in place" semantics that simply isn't
// what appears on the wire here). See the ITCH replay adapter for how
// this gets applied.
struct OrderReplace {
    std::uint16_t stock_locate;
    std::uint64_t timestamp_ns;
    std::uint64_t original_order_ref;
    std::uint64_t new_order_ref;
    std::uint32_t shares;
    std::uint32_t price_ticks;
};

inline OrderReplace DecodeOrderReplace(std::span<const std::byte> body) {
    OrderReplace m;
    m.stock_locate = ReadU16(body.data() + 1);
    m.timestamp_ns = ReadU48(body.data() + 5);
    m.original_order_ref = ReadU64(body.data() + 11);
    m.new_order_ref = ReadU64(body.data() + 19);
    m.shares = ReadU32(body.data() + 27);
    m.price_ticks = ReadU32(body.data() + 31);
    return m;
}

// 'R' Stock Directory (39-byte body). Only Stock Locate and the ticker are
// read -- everything else (market category, financial status, round lot
// size, ...) is display metadata with no effect on book reconstruction.
struct StockDirectory {
    std::uint16_t stock_locate;
    std::string_view stock;  // 8 bytes, space-padded
};

inline StockDirectory DecodeStockDirectory(std::span<const std::byte> body) {
    StockDirectory m;
    m.stock_locate = ReadU16(body.data() + 1);
    m.stock = ToStringView(body.subspan(11, 8));
    return m;
}

}  // namespace lob::itch
