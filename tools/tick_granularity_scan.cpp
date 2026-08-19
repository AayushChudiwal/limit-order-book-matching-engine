// Venue-wide tick-granularity scan: for EVERY symbol in an ITCH file,
// how many Add/Replace prices are multiples of 100 ticks (a penny) and
// how many are not.
//
// Why this is a committed tool and not a one-off script: the former
// step 3 (bitset + FFS for best bid/ask) was sized partly on the
// observation that SPY's prices are all penny multiples, which would
// let a price-indexed structure shrink ~100x. That observation is true
// for SPY and FALSE in general -- Reg NMS Rule 612 permits sub-penny
// quoting below $1.00, and this scan finds 175 NASDAQ / 272 PSX symbols
// using it, some (MYSZ 96.3%, IGLD 98.3%, TGB 100%) almost exclusively.
// Step 6's flat price array must not inherit the SPY-shaped assumption,
// so the check that disproves it lives next to the conclusion rather
// than in a chat log. See docs/phase5_plan.md, "Step 3: removed, and
// why".
//
// Also reports the 199999.9900 sentinel (1,999,999,900 ticks), which is
// not a real price and which any min/max-derived array sizing must
// exclude -- on its own it spans ~2 billion ticks.
//
// Usage: tick_granularity_scan <decompressed-itch-file>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "lob/itch/mapped_file.hpp"
#include "lob/itch/messages.hpp"
#include "lob/itch/reader.hpp"

namespace {

struct SymbolStats {
    std::string name;
    std::uint64_t total = 0;
    std::uint64_t sub_penny = 0;   // price % 100 != 0
    std::uint32_t min_price = 0xFFFFFFFFu;
    std::uint32_t max_price = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch-file>\n", argv[0]);
        return 2;
    }

    lob::itch::MappedFile file(argv[1]);
    lob::itch::MessageReader reader(file.data());

    std::map<std::uint16_t, SymbolStats> stats;

    while (auto msg = reader.Next()) {
        switch (msg->type) {
            case 'R': {
                const auto m = lob::itch::DecodeStockDirectory(msg->body);
                std::string s(m.stock);
                while (!s.empty() && s.back() == ' ') s.pop_back();
                stats[m.stock_locate].name = s;
                break;
            }
            case 'A':
            case 'F': {
                const auto m = lob::itch::DecodeAddOrder(msg->body);
                auto& st = stats[m.stock_locate];
                ++st.total;
                if (m.price_ticks % 100 != 0) ++st.sub_penny;
                st.min_price = std::min(st.min_price, m.price_ticks);
                st.max_price = std::max(st.max_price, m.price_ticks);
                break;
            }
            case 'U': {
                const auto m = lob::itch::DecodeOrderReplace(msg->body);
                auto& st = stats[m.stock_locate];
                ++st.total;
                if (m.price_ticks % 100 != 0) ++st.sub_penny;
                st.min_price = std::min(st.min_price, m.price_ticks);
                st.max_price = std::max(st.max_price, m.price_ticks);
                break;
            }
            default:
                break;
        }
    }

    // ITCH uses 199999.9900 (1999999900 ticks) as a "no meaningful limit"
    // sentinel. It is not a real tradable price, and any min/max-based
    // array sizing that swallows it spans ~2 billion ticks.
    constexpr std::uint32_t kSentinel = 1999999900u;
    std::uint64_t sentinel_symbols = 0;
    for (auto& [locate, st] : stats) {
        if (st.max_price == kSentinel) ++sentinel_symbols;
    }

    std::vector<const SymbolStats*> all;
    std::uint64_t grand_total = 0, grand_sub = 0;
    std::uint64_t symbols_with_sub = 0, symbols_seen = 0;
    for (auto& [locate, st] : stats) {
        if (st.total == 0) continue;
        all.push_back(&st);
        ++symbols_seen;
        grand_total += st.total;
        grand_sub += st.sub_penny;
        if (st.sub_penny > 0) ++symbols_with_sub;
    }

    std::printf("symbols with priced messages: %llu\n",
                static_cast<unsigned long long>(symbols_seen));
    std::printf("total priced messages:        %llu\n",
                static_cast<unsigned long long>(grand_total));
    std::printf("sub-penny (price %% 100 != 0): %llu  (%.4f%%)\n",
                static_cast<unsigned long long>(grand_sub),
                grand_total ? 100.0 * static_cast<double>(grand_sub) /
                                  static_cast<double>(grand_total)
                            : 0.0);
    std::printf("symbols with ANY sub-penny:   %llu of %llu\n",
                static_cast<unsigned long long>(symbols_with_sub),
                static_cast<unsigned long long>(symbols_seen));
    std::printf("symbols whose max price is the 199999.99 sentinel: %llu\n",
                static_cast<unsigned long long>(sentinel_symbols));

    // Named symbols of interest, printed explicitly rather than relying
    // on them showing up in a top-N list.
    for (const char* want : {"SPY", "KTOV", "MYSZ", "IGLD"}) {
        for (auto& [locate, st] : stats) {
            if (st.total > 0 && st.name == want) {
                std::printf("  %-6s min=%.4f max=%.4f  priced=%llu  sub-penny=%llu (%.3f%%)\n",
                            st.name.c_str(), st.min_price / 10000.0, st.max_price / 10000.0,
                            static_cast<unsigned long long>(st.total),
                            static_cast<unsigned long long>(st.sub_penny),
                            100.0 * static_cast<double>(st.sub_penny) /
                                static_cast<double>(st.total));
            }
        }
    }
    std::printf("\n");

    // The cheapest symbols -- where Reg NMS Rule 612 permits sub-penny
    // quoting (under $1.00) and where a penny-indexing assumption would
    // break first if it breaks at all.
    std::sort(all.begin(), all.end(), [](const SymbolStats* a, const SymbolStats* b) {
        return a->min_price < b->min_price;
    });
    std::printf("--- 20 lowest-priced symbols (min Add/Replace price) ---\n");
    std::printf("%-10s %12s %12s %12s %14s %10s\n", "symbol", "min($)", "max($)", "priced msgs",
                "sub-penny", "sub-penny%");
    for (std::size_t i = 0; i < all.size() && i < 20; ++i) {
        const auto* st = all[i];
        std::printf("%-10s %12.4f %12.4f %12llu %14llu %9.3f%%\n", st->name.c_str(),
                    st->min_price / 10000.0, st->max_price / 10000.0,
                    static_cast<unsigned long long>(st->total),
                    static_cast<unsigned long long>(st->sub_penny),
                    100.0 * static_cast<double>(st->sub_penny) / static_cast<double>(st->total));
    }

    std::printf("\n--- symbols with the MOST sub-penny prices ---\n");
    std::sort(all.begin(), all.end(), [](const SymbolStats* a, const SymbolStats* b) {
        return a->sub_penny > b->sub_penny;
    });
    std::printf("%-10s %12s %12s %12s %14s %10s\n", "symbol", "min($)", "max($)", "priced msgs",
                "sub-penny", "sub-penny%");
    for (std::size_t i = 0; i < all.size() && i < 15; ++i) {
        const auto* st = all[i];
        if (st->sub_penny == 0) break;
        std::printf("%-10s %12.4f %12.4f %12llu %14llu %9.3f%%\n", st->name.c_str(),
                    st->min_price / 10000.0, st->max_price / 10000.0,
                    static_cast<unsigned long long>(st->total),
                    static_cast<unsigned long long>(st->sub_penny),
                    100.0 * static_cast<double>(st->sub_penny) / static_cast<double>(st->total));
    }
    return 0;
}
