// itch_snapshot_dump: replays one symbol's messages through the reference
// engine and prints a top-10 book snapshot after every message, in the
// same row-per-message shape as martinobdl/ITCH's BookConstructor output
// (time, 1_bid_price, 1_bid_vol, 1_ask_price, 1_ask_vol, ..., 10_ask_vol)
// -- one CSV row per book-affecting message, blank fields where a level
// doesn't exist. This is the "mine" side of Phase 2's engine-correctness
// cross-validation: a separate script diffs this output against an
// independent ITCH implementation's reconstruction of the exact same
// file, row for row.
//
// Deliberately prints raw integer ticks for price, not decimal dollars --
// converting a decimal string like "300.19" back to an exact tick count
// via floating point risks introducing the very rounding error this
// comparison exists to catch. The diff script parses the reference tool's
// decimal strings as exact digit strings instead, so no float conversion
// happens on either side of the comparison.
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>

#include "lob/itch/mapped_file.hpp"
#include "lob/itch/messages.hpp"
#include "lob/itch/reader.hpp"
#include "lob/order_book.hpp"

namespace {

class SilentListener : public lob::BookListener {
  public:
    void OnFill(const lob::Fill&) override {}
    void OnOrderAccepted(lob::OrderId) override {}
    void OnOrderCancelled(lob::OrderId, lob::Quantity) override {}
    void OnOrderModified(lob::OrderId, lob::Quantity) override {}
    void OnOrderRejected(lob::OrderId, lob::RejectReason) override {}
    void OnBookUpdate(lob::Side, lob::Price, lob::Quantity) override {}
};

std::string Trim(std::string_view padded) {
    std::string s(padded);
    const auto last = s.find_last_not_of(' ');
    s.erase(last == std::string::npos ? 0 : last + 1);
    return s;
}

// Interleaves bid and ask levels per row position -- 1_bid_price,1_bid_vol,
// 1_ask_price,1_ask_vol,2_bid_price,... -- matching martinobdl/ITCH's
// column order exactly, since the diff script compares by column index.
void PrintRow(const std::vector<lob::PriceLevel>& bids, const std::vector<lob::PriceLevel>& asks,
              int depth) {
    for (int i = 0; i < depth; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        std::cout << ",";
        if (idx < bids.size()) {
            std::cout << bids[idx].price.ticks;
        }
        std::cout << ",";
        if (idx < bids.size()) {
            std::cout << bids[idx].total_quantity.units;
        }
        std::cout << ",";
        if (idx < asks.size()) {
            std::cout << asks[idx].price.ticks;
        }
        std::cout << ",";
        if (idx < asks.size()) {
            std::cout << asks[idx].total_quantity.units;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: itch_snapshot_dump <path-to-decompressed-itch-file> <symbol>\n";
        return 1;
    }
    const std::string path = argv[1];
    const std::string requested_symbol = argv[2];

    lob::itch::MappedFile file(path);
    const auto data = file.data();

    // Resolve the target symbol's stock_locate from the file's own Stock
    // Directory ('R') messages -- same lookup itch_replay.cpp does.
    std::uint16_t target_locate = 0;
    bool found = false;
    {
        lob::itch::MessageReader reader(data);
        while (const auto msg = reader.Next()) {
            if (msg->type != 'R') {
                continue;
            }
            const auto m = lob::itch::DecodeStockDirectory(msg->body);
            if (Trim(m.stock) == requested_symbol) {
                target_locate = m.stock_locate;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        std::cerr << "symbol \"" << requested_symbol
                  << "\" not found in this file's Stock Directory\n";
        return 1;
    }

    SilentListener listener;
    lob::OrderBook book(listener);

    std::cout << "time";
    for (int i = 1; i <= 10; ++i) {
        std::cout << "," << i << "_bid_price," << i << "_bid_vol," << i << "_ask_price," << i
                  << "_ask_vol";
    }
    std::cout << "\n";

    lob::itch::MessageReader reader(data);
    while (const auto msg = reader.Next()) {
        std::uint64_t timestamp_ns = 0;
        bool touched = true;

        switch (msg->type) {
            case 'A':
            case 'F': {
                const auto m = lob::itch::DecodeAddOrder(msg->body);
                if (m.stock_locate != target_locate) {
                    touched = false;
                    break;
                }
                timestamp_ns = m.timestamp_ns;
                const lob::Side side = m.side == 'B' ? lob::Side::Buy : lob::Side::Sell;
                book.AddLimitOrder(lob::OrderId{m.order_ref}, side,
                                   lob::Price{static_cast<std::int64_t>(m.price_ticks)},
                                   lob::Quantity{static_cast<std::int64_t>(m.shares)});
                break;
            }
            case 'E': {
                const auto m = lob::itch::DecodeOrderExecuted(msg->body);
                if (m.stock_locate != target_locate) {
                    touched = false;
                    break;
                }
                timestamp_ns = m.timestamp_ns;
                book.ReduceRestingQuantity(
                    lob::OrderId{m.order_ref},
                    lob::Quantity{static_cast<std::int64_t>(m.executed_shares)});
                break;
            }
            case 'C': {
                const auto m = lob::itch::DecodeOrderExecutedWithPrice(msg->body);
                if (m.stock_locate != target_locate) {
                    touched = false;
                    break;
                }
                timestamp_ns = m.timestamp_ns;
                book.ReduceRestingQuantity(
                    lob::OrderId{m.order_ref},
                    lob::Quantity{static_cast<std::int64_t>(m.executed_shares)});
                break;
            }
            case 'X': {
                const auto m = lob::itch::DecodeOrderCancel(msg->body);
                if (m.stock_locate != target_locate) {
                    touched = false;
                    break;
                }
                timestamp_ns = m.timestamp_ns;
                book.ReduceRestingQuantity(
                    lob::OrderId{m.order_ref},
                    lob::Quantity{static_cast<std::int64_t>(m.canceled_shares)});
                break;
            }
            case 'D': {
                const auto m = lob::itch::DecodeOrderDelete(msg->body);
                if (m.stock_locate != target_locate) {
                    touched = false;
                    break;
                }
                timestamp_ns = m.timestamp_ns;
                book.CancelOrder(lob::OrderId{m.order_ref});
                break;
            }
            case 'U': {
                const auto m = lob::itch::DecodeOrderReplace(msg->body);
                if (m.stock_locate != target_locate) {
                    touched = false;
                    break;
                }
                timestamp_ns = m.timestamp_ns;
                const auto side = book.SideOf(lob::OrderId{m.original_order_ref});
                if (side.has_value()) {
                    book.CancelOrder(lob::OrderId{m.original_order_ref});
                    book.AddLimitOrder(lob::OrderId{m.new_order_ref}, *side,
                                       lob::Price{static_cast<std::int64_t>(m.price_ticks)},
                                       lob::Quantity{static_cast<std::int64_t>(m.shares)});
                }
                break;
            }
            default:
                touched = false;
                break;
        }

        if (!touched) {
            continue;
        }

        std::cout << timestamp_ns;
        PrintRow(book.TopLevels(lob::Side::Buy, 10), book.TopLevels(lob::Side::Sell, 10), 10);
        std::cout << "\n";
    }

    return 0;
}
