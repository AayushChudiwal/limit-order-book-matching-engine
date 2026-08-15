// itch_replay: replays a decompressed NASDAQ TotalView-ITCH 5.0 file
// through the Phase 1 reference engine and reports:
//   - the message-type mix across the WHOLE file (every symbol) -- this is
//     the real, non-synthetic benchmark workload Phase 4/5 will measure
//     optimizations against, not an assumed distribution.
//   - referential-integrity results from OrderReferenceIntegrityChecker,
//     also whole-file -- the primary correctness check for the raw parsing
//     path (see integrity_checker.hpp).
//   - a single-symbol book reconstruction (auto-picked by Add-order
//     activity as a liquidity proxy, or given explicitly) run through the
//     actual OrderBook engine, reporting phantom fills (should be zero --
//     see ReduceRestingQuantity's doc comment for why an Add should never
//     legitimately cross the book during replay) and unexpected rejects.
//
// This tool operates on an already gzip -d'd file; it deliberately doesn't
// handle .gz itself, keeping decompression a separate, inspectable step
// rather than folding a second concern into ITCH parsing.
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>

#include "lob/itch/integrity_checker.hpp"
#include "lob/itch/mapped_file.hpp"
#include "lob/itch/messages.hpp"
#include "lob/itch/reader.hpp"
#include "lob/order_book.hpp"

namespace {

class ReplayListener : public lob::BookListener {
  public:
    void OnFill(const lob::Fill&) override { ++fills; }
    void OnOrderAccepted(lob::OrderId) override {}
    void OnOrderCancelled(lob::OrderId, lob::Quantity) override {}
    void OnOrderModified(lob::OrderId, lob::Quantity) override {}
    void OnOrderRejected(lob::OrderId, lob::RejectReason reason) override {
        ++rejects;
        ++reject_counts[reason];
    }
    void OnBookUpdate(lob::Side, lob::Price, lob::Quantity) override {}

    std::uint64_t fills = 0;
    std::uint64_t rejects = 0;
    std::map<lob::RejectReason, std::uint64_t> reject_counts;
};

std::string Trim(std::string_view padded) {
    std::string s(padded);
    const auto last = s.find_last_not_of(' ');
    s.erase(last == std::string::npos ? 0 : last + 1);
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: itch_replay <path-to-decompressed-itch-file> [target-symbol]\n";
        return 1;
    }
    const std::string path = argv[1];
    const std::string requested_symbol = argc >= 3 ? argv[2] : "";

    lob::itch::MappedFile file(path);
    const auto data = file.data();

    // --- Pass 1: whole-file, all symbols. Message-type mix, referential
    // integrity, and per-symbol Add activity (to auto-pick a liquid
    // symbol if none was requested). ---
    std::map<char, std::uint64_t> type_counts;
    std::unordered_map<std::uint16_t, std::string> locate_to_symbol;
    std::unordered_map<std::uint16_t, std::uint64_t> add_activity;
    lob::itch::OrderReferenceIntegrityChecker integrity;
    std::uint64_t total_messages = 0;
    std::uint64_t lifecycle_message_count = 0;

    {
        lob::itch::MessageReader reader(data);
        while (const auto msg = reader.Next()) {
            ++total_messages;
            ++type_counts[msg->type];

            switch (msg->type) {
                case 'R': {
                    const auto m = lob::itch::DecodeStockDirectory(msg->body);
                    locate_to_symbol[m.stock_locate] = Trim(m.stock);
                    break;
                }
                case 'A':
                case 'F': {
                    const auto m = lob::itch::DecodeAddOrder(msg->body);
                    integrity.OnAdd(m.order_ref, m.shares, msg->file_offset);
                    ++add_activity[m.stock_locate];
                    ++lifecycle_message_count;
                    break;
                }
                case 'E': {
                    const auto m = lob::itch::DecodeOrderExecuted(msg->body);
                    integrity.OnExecuted(m.order_ref, m.executed_shares, 'E', msg->file_offset);
                    ++lifecycle_message_count;
                    break;
                }
                case 'C': {
                    const auto m = lob::itch::DecodeOrderExecutedWithPrice(msg->body);
                    integrity.OnExecuted(m.order_ref, m.executed_shares, 'C', msg->file_offset);
                    ++lifecycle_message_count;
                    break;
                }
                case 'X': {
                    const auto m = lob::itch::DecodeOrderCancel(msg->body);
                    integrity.OnCanceled(m.order_ref, m.canceled_shares, msg->file_offset);
                    ++lifecycle_message_count;
                    break;
                }
                case 'D': {
                    const auto m = lob::itch::DecodeOrderDelete(msg->body);
                    integrity.OnDeleted(m.order_ref, msg->file_offset);
                    ++lifecycle_message_count;
                    break;
                }
                case 'U': {
                    const auto m = lob::itch::DecodeOrderReplace(msg->body);
                    integrity.OnReplaced(m.original_order_ref, m.new_order_ref, m.shares,
                                         msg->file_offset);
                    ++lifecycle_message_count;
                    break;
                }
                default:
                    break;
            }
        }
    }

    // Resolve the target symbol: an explicit request, or whichever locate
    // code has the most Add-order activity -- symbols with the most
    // resting-order churn are the ones actually trading.
    std::uint16_t target_locate = 0;
    std::string target_symbol;
    if (!requested_symbol.empty()) {
        bool found = false;
        for (const auto& [locate, sym] : locate_to_symbol) {
            if (sym == requested_symbol) {
                target_locate = locate;
                target_symbol = sym;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "symbol \"" << requested_symbol
                      << "\" not found in this file's Stock Directory\n";
            return 1;
        }
    } else {
        std::uint64_t best = 0;
        for (const auto& [locate, count] : add_activity) {
            if (count > best) {
                best = count;
                target_locate = locate;
            }
        }
        auto it = locate_to_symbol.find(target_locate);
        target_symbol = it != locate_to_symbol.end() ? it->second : "?";
    }

    // --- Pass 2: replay only the target symbol's messages through the
    // real OrderBook engine to reconstruct its book. ---
    ReplayListener listener;
    lob::OrderBook book(listener);
    std::uint64_t target_messages = 0;
    std::uint64_t replace_missing_side = 0;

    {
        lob::itch::MessageReader reader(data);
        while (const auto msg = reader.Next()) {
            switch (msg->type) {
                case 'A':
                case 'F': {
                    const auto m = lob::itch::DecodeAddOrder(msg->body);
                    if (m.stock_locate != target_locate) {
                        break;
                    }
                    ++target_messages;
                    const lob::Side side = m.side == 'B' ? lob::Side::Buy : lob::Side::Sell;
                    book.AddLimitOrder(lob::OrderId{m.order_ref}, side,
                                       lob::Price{static_cast<std::int64_t>(m.price_ticks)},
                                       lob::Quantity{static_cast<std::int64_t>(m.shares)});
                    break;
                }
                case 'E': {
                    const auto m = lob::itch::DecodeOrderExecuted(msg->body);
                    if (m.stock_locate != target_locate) {
                        break;
                    }
                    ++target_messages;
                    book.ReduceRestingQuantity(
                        lob::OrderId{m.order_ref},
                        lob::Quantity{static_cast<std::int64_t>(m.executed_shares)});
                    break;
                }
                case 'C': {
                    const auto m = lob::itch::DecodeOrderExecutedWithPrice(msg->body);
                    if (m.stock_locate != target_locate) {
                        break;
                    }
                    ++target_messages;
                    book.ReduceRestingQuantity(
                        lob::OrderId{m.order_ref},
                        lob::Quantity{static_cast<std::int64_t>(m.executed_shares)});
                    break;
                }
                case 'X': {
                    const auto m = lob::itch::DecodeOrderCancel(msg->body);
                    if (m.stock_locate != target_locate) {
                        break;
                    }
                    ++target_messages;
                    book.ReduceRestingQuantity(
                        lob::OrderId{m.order_ref},
                        lob::Quantity{static_cast<std::int64_t>(m.canceled_shares)});
                    break;
                }
                case 'D': {
                    const auto m = lob::itch::DecodeOrderDelete(msg->body);
                    if (m.stock_locate != target_locate) {
                        break;
                    }
                    ++target_messages;
                    book.CancelOrder(lob::OrderId{m.order_ref});
                    break;
                }
                case 'U': {
                    const auto m = lob::itch::DecodeOrderReplace(msg->body);
                    if (m.stock_locate != target_locate) {
                        break;
                    }
                    ++target_messages;
                    // Side isn't on the wire for a Replace -- it has to
                    // come from the original order (see SideOf()'s doc
                    // comment). CancelOrder + AddLimitOrder under the new
                    // reference, NOT ModifyOrder: see
                    // OrderReplaceViaItchPattern in
                    // test_reduce_and_side_of.cpp for why.
                    const auto side = book.SideOf(lob::OrderId{m.original_order_ref});
                    if (!side.has_value()) {
                        ++replace_missing_side;
                        break;
                    }
                    book.CancelOrder(lob::OrderId{m.original_order_ref});
                    book.AddLimitOrder(lob::OrderId{m.new_order_ref}, *side,
                                       lob::Price{static_cast<std::int64_t>(m.price_ticks)},
                                       lob::Quantity{static_cast<std::int64_t>(m.shares)});
                    break;
                }
                default:
                    break;
            }
        }
    }

    // --- report ---
    std::cout << "file: " << path << "\n";
    std::cout << "total messages: " << total_messages << "\n\n";

    std::cout << "message-type mix (whole file, all symbols):\n";
    for (const auto& [type, count] : type_counts) {
        const double pct = 100.0 * static_cast<double>(count) / static_cast<double>(total_messages);
        std::cout << "  " << type << "  " << std::setw(12) << count << "  " << std::fixed
                  << std::setprecision(3) << pct << "%\n";
    }
    std::cout << "\n";

    std::cout << "referential integrity: " << integrity.Violations().size()
              << " violation(s) out of " << lifecycle_message_count
              << " order-lifecycle messages (A/F/E/C/X/D/U) checked, whole file\n";
    const auto& violations = integrity.Violations();
    for (std::size_t i = 0; i < violations.size() && i < 10; ++i) {
        std::cout << "  offset=" << violations[i].file_offset
                  << " type=" << violations[i].message_type << " ref=" << violations[i].order_ref
                  << " reason=\"" << violations[i].reason << "\"\n";
    }
    if (violations.size() > 10) {
        std::cout << "  ... and " << (violations.size() - 10) << " more\n";
    }
    std::cout << "\n";

    std::cout << "target symbol: " << target_symbol << " (stock_locate=" << target_locate << ")\n";
    std::cout << "  messages replayed for this symbol: " << target_messages << "\n";
    std::cout << "  phantom fills during replay, should be 0: " << listener.fills << "\n";
    std::cout << "  unexpected engine rejects, should be 0: " << listener.rejects << "\n";
    std::cout << "  Order Replace with unknown original side, should be 0: " << replace_missing_side
              << "\n";
    std::cout << "  final reconstructed best bid: "
              << (book.BestBid() ? std::to_string(book.BestBid()->ticks) : "none") << "\n";
    std::cout << "  final reconstructed best ask: "
              << (book.BestAsk() ? std::to_string(book.BestAsk()->ticks) : "none") << "\n";

    const bool clean = violations.empty() && listener.fills == 0 && replace_missing_side == 0;
    return clean ? 0 : 2;
}
