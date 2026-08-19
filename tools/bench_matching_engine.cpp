// bench_matching_engine: measures OptimizedOrderBook operation cost per op
// type (Add, Cancel, Reduce, Replace) against real PSX SPY data.
//
// Targets lob::OptimizedOrderBook (include/lob/optimized_order_book.hpp),
// not the reference lob::OrderBook -- Phase 5 step 1's numbers
// (docs/benchmark_methodology.md, docs/phase5_step1_results.md) were
// captured against OrderBook itself, back when it was still the only
// engine; step 2 onward forked into OptimizedOrderBook (see that file's
// class comment), so this tool follows the fork. Continuing an N-run
// series across that fork is valid -- OptimizedOrderBook started as an
// exact behavioral copy of OrderBook post-step-1 -- but don't compare
// numbers from before the fork against a DIFFERENT build of this tool
// that still targeted OrderBook without checking both sides used the
// same target.
//
// History (see docs/benchmark_methodology.md for the full account): a
// per-op mach_absolute_time() design was tried and rejected -- at this
// engine's real operation size (tens of ns), the 41.7ns/24MHz timer tick
// quantizes every reported number to an exact multiple of itself,
// destroying resolution (Add's p50 WAS one tick). A per-op PMU design was
// tried before that and also rejected -- kpc_get_thread_counters() costs
// ~2688 cycles (~628ns) median, ~87% pure instrumentation overhead
// against a ~3090-cycle op, and batching to amortize it introduced a bias
// that varied BY OP TYPE.
//
// This version uses the PMU cycle counter exactly ONCE per (op-type,
// variant) benchmark, bracketing a tight, type-PURE, back-to-back loop of
// real engine calls -- no interleaving of other op types inside the
// timed window, no per-op reads, no batching decision. That gives a
// precise MEAN cycles/op per variant; it does not give a per-op tail
// distribution -- see docs/benchmark_methodology.md for why that's not
// measurable on this platform at this operation size, stated plainly
// rather than papered over with a quantized histogram.
//
// Also reads ARM_L1D_CACHE_REFILL (same bracket, same before/after
// snapshot as cycles/instructions -- no extra PMU reads) into
// l1d_cache_refills / mean_l1d_refills_per_op. Validated TRUSTED in
// docs/pmu_validation.md (branch mispredicts was NOT and is never
// configured, see PmuCounters). Labelling caveat, stated once here
// rather than at every callsite: this is ARM's L1D_CACHE_REFILL event,
// which on most Arm PMU implementations counts linefill traffic more
// broadly than only CPU-demand accesses (a prefetch that fills a line
// but is never subsequently touched can also increment it) -- this
// repo's validation confirmed the counter responds correctly to real
// locality pressure (a small buffer that fits L1D vs a large one that
// doesn't), not that it exclusively attributes refills to demand
// accesses on this specific chip. Treat "L1D cache refills" as the
// accurate label; do not read it as "L1D demand misses" without further
// validation. Empty in the CSV (not zero) if this chip/OS doesn't
// expose the event -- see the startup warning.
//
// Each pass starts from a FRESH COPY of the same warmed-up book (built
// once from a real, untimed replay of SPY's early message history), so
// all 8 variants below start from an identical, realistic state:
//   - Add_real:    a real, unmodified, contiguous slice of later SPY Add
//                  messages (tight price clustering around the real
//                  mid -- the favorable case for a flat price array).
//   - Add_widened: the SAME slice, with each price's distance from the
//                  live mid scaled by kWidenMultiplier -- a synthetic
//                  stress variant bounding how much of any future flat-
//                  array win depends on that tight clustering.
//   - {Cancel,Reduce,Replace}_traversal: driven against orders resting
//                  in the warmed book, in FullBook()'s natural
//                  price-then-arrival order -- best-case cache locality,
//                  labeled as such because it's exactly the property a
//                  flat array / arena allocator (Phase 5) improves, so
//                  this variant alone would overstate their benefit.
//   - {Cancel,Reduce,Replace}_shuffled: the SAME resting orders, order
//                  randomized with a fixed, committed seed -- the
//                  PRIMARY number for each of these three op types.
//   - Replace's new price is sampled from the empirical distribution of
//                  REAL SPY Replace price deltas (extracted from the
//                  file, not invented), so the aggressive-vs-passive
//                  split is what the real data determined, not a knob.
//
// Cancel/Reduce/Replace are driven against real resting orders rather
// than replaying the literal historical cancel/reduce/replace sequence
// -- stated plainly: this is a deliberate, defensible tradeoff (a real
// historical subsequence would reference orders skipped elsewhere and
// mostly no-op), not an oversight.
//
// Must run under sudo (PmuCounters requires root).
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lob/arena.hpp"
#include "lob/bench/churn.hpp"
#include "lob/bench/frequency.hpp"
#include "lob/bench/pmu.hpp"
#include "lob/bench/scheduling.hpp"
#include "lob/itch/mapped_file.hpp"
#include "lob/itch/messages.hpp"
#include "lob/itch/reader.hpp"
#include "lob/optimized_order_book.hpp"

using lob::bench::CalibrateGigahertz;
using lob::bench::CurrentCpuNumber;
using lob::bench::PmuCounters;
using lob::bench::SetInteractiveQos;

namespace {

// A fixed, committed seed -- NOT re-rolled per run -- for every source of
// randomness below (resting-order shuffles, Replace delta sampling, new
// synthetic ids). Reproducibility of the workload itself is what makes
// cross-run spread (the N>=10 baseline collection) meaningful: if the
// workload changed between runs, run-to-run spread would conflate
// machine noise with workload variance.
constexpr std::uint64_t kFixedSeed = 0xB0BACAFEULL;
constexpr double kWidenMultiplier = 10.0;
constexpr std::uint64_t kSyntheticIdBase = 1'000'000'000'000ULL;

// mt19937_64's own raw 64-bit output is portable across libc++/libstdc++
// (established in Phase 3's fuzz generator); std::shuffle and
// std::uniform_int_distribution are NOT guaranteed to be, since their
// algorithms are implementation-defined even given identical input
// stream. Both uses below (shuffling, delta sampling) go through raw
// rng() calls only, for the same reproducibility reason Phase 3 settled
// on.
void DeterministicShuffle(std::vector<std::size_t>& indices, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    for (std::size_t i = indices.size(); i > 1; --i) {
        const std::size_t j = static_cast<std::size_t>(rng() % i);
        std::swap(indices[i - 1], indices[j]);
    }
}

class NullListener : public lob::BookListener {
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

struct AddRecord {
    lob::Side side;
    lob::Price price;
    lob::Quantity qty;
};

struct RestingOrder {
    lob::OrderId id;
    lob::Side side;
    lob::Price price;
    lob::Quantity qty;
};

std::vector<RestingOrder> CollectRestingOrders(const lob::OptimizedOrderBook& book) {
    std::vector<RestingOrder> out;
    for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
        for (const auto& level : book.FullBook(side)) {
            for (const auto& o : level.orders) {
                out.push_back(RestingOrder{o.id, side, level.price, o.quantity});
            }
        }
    }
    return out;
}

std::size_t CountLevels(const lob::OptimizedOrderBook& book) {
    std::size_t n = 0;
    for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
        n += book.FullBook(side).size();
    }
    return n;
}

struct PassResult {
    std::string label;
    std::uint64_t ops = 0;
    std::uint64_t cycles = 0;
    std::uint64_t instructions = 0;
    // ARM_L1D_CACHE_REFILL, validated TRUSTED in docs/pmu_validation.md
    // (large-vs-small-buffer sanity check). nullopt if this chip/OS
    // didn't expose the event (PmuCounters::IsL1dCacheRefillsAvailable()
    // was false) -- every variant in one run shares the same
    // availability, so this is either set for all of them or none.
    std::optional<std::uint64_t> l1d_cache_refills;
    // Always present (never optional) -- ReadChurn() returns an
    // all-zero snapshot when LOB_TRACK_CHURN wasn't compiled into the
    // linked lob_optimized_engine, same shape either way. See
    // include/lob/bench/churn.hpp: a LOB_TRACK_CHURN=OFF build (the
    // default, used for every trusted cycle number this project cites)
    // costs nothing extra reading this -- the counters themselves
    // compile to nothing, so the delta is always {0,0,0,0}. A
    // LOB_TRACK_CHURN=ON build's churn_* fields here are meaningful;
    // its cycle/instructions/l1d fields in the SAME run are NOT --
    // counting churn on every level/order-storage event adds real
    // overhead to the exact code paths being cycle-timed. Run twice,
    // once per build config, never cite cycles from an ON build.
    lob::bench::ChurnSnapshot churn_delta;
    bool migrated = false;
};

template <typename Fn>
PassResult TimePass(const PmuCounters& pmu, std::string label, std::uint64_t op_count,
                    Fn&& run_all) {
    if (op_count == 0) {
        return PassResult{std::move(label), 0, 0, 0, std::nullopt, {}, false};
    }
    const std::size_t cpu_before = CurrentCpuNumber();
    const auto before = pmu.Read();
    const auto churn_before = lob::bench::ReadChurn();
    run_all();
    const auto after = pmu.Read();
    const auto churn_after = lob::bench::ReadChurn();
    const std::size_t cpu_after = CurrentCpuNumber();
    std::optional<std::uint64_t> l1d_refills;
    if (before.l1d_cache_refills && after.l1d_cache_refills) {
        l1d_refills = *after.l1d_cache_refills - *before.l1d_cache_refills;
    }
    const lob::bench::ChurnSnapshot churn_delta{
        churn_after.level_creates - churn_before.level_creates,
        churn_after.level_destroys - churn_before.level_destroys,
        churn_after.order_storage_allocs - churn_before.order_storage_allocs,
        churn_after.order_storage_frees - churn_before.order_storage_frees};
    return PassResult{std::move(label), op_count, after.cycles - before.cycles,
                      after.instructions - before.instructions, l1d_refills, churn_delta,
                      cpu_after != cpu_before};
}

// Runs `apply_round` (one full pass over a fixed-size pool) repeatedly,
// with `replenish_round` (untimed) restoring the pool to its starting
// state between rounds, until at least kMinCyclingOps total ops have been
// timed. No real resting-order pool observed on this venue reaches
// anywhere near "tens of thousands" on its own (deepest candidate found
// across the top 150 most-active NASDAQ symbols was 432) -- cycling a
// smaller real pool is how this benchmark still gets a statistically
// usable op count instead of trusting a mean built from a few hundred
// samples. Only `apply_round` is timed; `replenish_round` (rebuilding the
// pool back to its start-of-round state) is not -- same separation
// low-latency-matching-engine's benchmark harness uses ("resolve targets
// ... before timing; RunOp() contains only the matching operation").
template <typename ApplyRoundFn, typename ReplenishRoundFn>
PassResult TimeCyclingPass(const PmuCounters& pmu, std::string label, std::size_t pool_size,
                           std::uint64_t min_total_ops, ApplyRoundFn&& apply_round,
                           ReplenishRoundFn&& replenish_round) {
    if (pool_size == 0) {
        return PassResult{std::move(label), 0, 0, 0, std::nullopt, {}, false};
    }
    const std::size_t rounds =
        (min_total_ops + pool_size - 1) / pool_size;  // ceil(min_total_ops / pool_size)

    std::uint64_t total_ops = 0;
    std::uint64_t total_cycles = 0;
    std::uint64_t total_instructions = 0;
    std::optional<std::uint64_t> total_l1d_refills;
    lob::bench::ChurnSnapshot total_churn{};
    bool migrated = false;
    for (std::size_t r = 0; r < rounds; ++r) {
        if (r > 0) {
            replenish_round();  // untimed: restore the pool to round-0 state
        }
        const std::size_t cpu_before = CurrentCpuNumber();
        const auto before = pmu.Read();
        const auto churn_before = lob::bench::ReadChurn();
        apply_round();
        const auto after = pmu.Read();
        const auto churn_after = lob::bench::ReadChurn();
        const std::size_t cpu_after = CurrentCpuNumber();
        total_ops += pool_size;
        total_cycles += after.cycles - before.cycles;
        total_instructions += after.instructions - before.instructions;
        if (before.l1d_cache_refills && after.l1d_cache_refills) {
            total_l1d_refills = total_l1d_refills.value_or(0) +
                                (*after.l1d_cache_refills - *before.l1d_cache_refills);
        }
        total_churn.level_creates += churn_after.level_creates - churn_before.level_creates;
        total_churn.level_destroys += churn_after.level_destroys - churn_before.level_destroys;
        total_churn.order_storage_allocs +=
            churn_after.order_storage_allocs - churn_before.order_storage_allocs;
        total_churn.order_storage_frees +=
            churn_after.order_storage_frees - churn_before.order_storage_frees;
        migrated = migrated || (cpu_after != cpu_before);
    }
    return PassResult{std::move(label),   total_ops,  total_cycles, total_instructions,
                      total_l1d_refills, total_churn, migrated};
}

// Builds the CSV row as an explicit vector of fields rather than a chain
// of `<<` with a hand-counted number of literal commas for the
// "unavailable" cases -- an earlier version of this function got that
// count wrong for the l1d-unavailable branch (one comma short of the
// two empty fields it needed), undetected because L1D refills happen to
// be available on every machine this tool has actually run on so far.
// Explicit fields + a single join makes that whole class of bug
// impossible to reintroduce silently.
void PrintAndRecord(const PassResult& r, double ghz, std::ofstream& csv) {
    if (r.ops == 0) {
        std::printf("  %-18s SKIPPED (no ops available)\n", r.label.c_str());
        return;
    }
    const double mean_cycles = static_cast<double>(r.cycles) / static_cast<double>(r.ops);
    const double mean_ns = mean_cycles / ghz;
    const double mean_storage_allocs =
        static_cast<double>(r.churn_delta.order_storage_allocs) / static_cast<double>(r.ops);

    std::string l1d_print = "l1d_refills=n/a";
    std::string l1d_refills_field;
    std::string mean_l1d_refills_field;
    if (r.l1d_cache_refills) {
        const double mean_l1d_refills =
            static_cast<double>(*r.l1d_cache_refills) / static_cast<double>(r.ops);
        char print_buf[48];
        std::snprintf(print_buf, sizeof(print_buf), "l1d_refills=%6.3f/op", mean_l1d_refills);
        l1d_print = print_buf;
        l1d_refills_field = std::to_string(*r.l1d_cache_refills);
        char mean_buf[32];
        std::snprintf(mean_buf, sizeof(mean_buf), "%.6f", mean_l1d_refills);
        mean_l1d_refills_field = mean_buf;
    }

    std::printf(
        "  %-18s ops=%8llu  mean=%8.1f cycles (%6.1f ns)  %-22s  storage_allocs=%6.4f/op  %s\n",
        r.label.c_str(), static_cast<unsigned long long>(r.ops), mean_cycles, mean_ns,
        l1d_print.c_str(), mean_storage_allocs, r.migrated ? "[MIGRATED]" : "");

    const std::vector<std::string> fields = {
        r.label,
        std::to_string(r.ops),
        std::to_string(r.cycles),
        std::to_string(r.instructions),
        std::to_string(mean_cycles),
        std::to_string(mean_ns),
        l1d_refills_field,       // empty field if unavailable
        mean_l1d_refills_field,  // empty field if unavailable
        std::to_string(r.churn_delta.level_creates),
        std::to_string(r.churn_delta.level_destroys),
        std::to_string(r.churn_delta.order_storage_allocs),
        std::to_string(r.churn_delta.order_storage_frees),
        std::to_string(mean_storage_allocs),
        std::to_string(r.migrated ? 1 : 0),
    };
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) csv << ',';
        csv << fields[i];
    }
    csv << '\n';
}

struct Args {
    std::string itch_path;
    std::string out_csv;
    std::string requested_symbol;
};

int Run(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: bench_matching_engine <itch-file> <out.csv> [symbol]\n"
                     "  Must run under sudo (PMU access requires root).\n");
        return 1;
    }
    Args args;
    args.itch_path = argv[1];
    args.out_csv = argv[2];
    args.requested_symbol = argc >= 4 ? argv[3] : "";

    if (!SetInteractiveQos()) {
        std::fprintf(stderr,
                     "warning: pthread_set_qos_class_self_np failed -- continuing without the "
                     "P-core scheduling hint\n");
    }

    PmuCounters pmu;
    const double ghz = CalibrateGigahertz(pmu);
    std::printf("=== bench_matching_engine ===\n");
    std::printf("CPU: %s\n", pmu.CpuString().c_str());
    std::printf("calibrated frequency this run: %.3f GHz\n", ghz);
    // Printed every run, never inferred. Generation-tag validation costs
    // a branch on every arena dereference, so a number produced with it
    // ON is not comparable to this project's published baselines -- and
    // the tags default to ON everywhere else precisely because they are
    // wanted for tests and CI. Making the build's choice visible in the
    // run log is what stops a tagged binary from quietly contributing a
    // "slow" result to a benchmark table. Configure a benchmark build
    // with -DLOB_ARENA_GENERATION_TAGS=OFF.
    std::printf("arena generation tags: %s%s\n",
                LOB_ARENA_GENERATION_TAGS ? "ON" : "OFF",
                LOB_ARENA_GENERATION_TAGS
                    ? "  *** WARNING: not a benchmark-shaped build, numbers not comparable ***"
                    : "");

    lob::itch::MappedFile file(args.itch_path);
    const auto data = file.data();

    // --- Pass 1: find the target symbol (most Add-order activity, same
    // auto-pick rule as itch_replay), whole file. ---
    std::unordered_map<std::uint16_t, std::string> locate_to_symbol;
    std::unordered_map<std::uint16_t, std::uint64_t> add_activity;
    {
        lob::itch::MessageReader reader(data);
        while (const auto msg = reader.Next()) {
            if (msg->type == 'R') {
                const auto m = lob::itch::DecodeStockDirectory(msg->body);
                locate_to_symbol[m.stock_locate] = Trim(m.stock);
            } else if (msg->type == 'A' || msg->type == 'F') {
                const auto m = lob::itch::DecodeAddOrder(msg->body);
                ++add_activity[m.stock_locate];
            }
        }
    }
    std::uint16_t target_locate = 0;
    std::string target_symbol;
    if (!args.requested_symbol.empty()) {
        bool found = false;
        for (const auto& [locate, sym] : locate_to_symbol) {
            if (sym == args.requested_symbol) {
                target_locate = locate;
                target_symbol = sym;
                found = true;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr, "symbol \"%s\" not found in this file's Stock Directory\n",
                         args.requested_symbol.c_str());
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
    std::printf("target symbol: %s (stock_locate=%u)\n", target_symbol.c_str(),
                static_cast<unsigned>(target_locate));

    // --- Pass 2: single scan of the target symbol's full message
    // history. Builds: (a) warmup_book, replayed untimed -- the starting
    // state every variant below copies; (b) real_adds, every Add message
    // for this symbol across the WHOLE file, for the Add_real/Add_widened
    // slice; (c) replace_deltas, the empirical price-delta distribution
    // from every real Replace this symbol saw, via a lightweight
    // order_ref->price map maintained alongside (not the OrderBook
    // itself -- this doesn't need matching, just "what price did this id
    // last have").
    //
    // Warmup stops once the book's resting-order count reaches
    // kTargetRestingDepth, NOT after a fixed message count: a fixed
    // 20000-message cutoff was tried first and left only 33 resting
    // orders for a symbol this liquid -- SPY's ~45%/46% Add/Cancel-Delete
    // split means most quotes are cancelled again quickly (real HFT
    // requoting behavior), so raw message count is a poor proxy for
    // resting depth. 33 samples for Cancel/Reduce/Replace is far too
    // small to trust a mean over -- one expensive outlier dominates it.
    // Checked periodically (not every message) since it costs an O(depth)
    // FullBook() walk; capped so a symbol that never reaches the target
    // still terminates warmup with a report of what was actually
    // achieved, rather than looping indefinitely.
    //
    // kTargetRestingDepth turned out to be unreachable for every real
    // candidate checked (see docs/benchmark_methodology.md) -- meaning
    // warmup would otherwise run to EOF and consume every real Add
    // message, leaving NONE for the Add_real/Add_widened slice (this
    // happened on the first NASDAQ run: both variants reported "no ops
    // available"). kAddReserveFraction fixes it: warmup also stops once
    // it has consumed enough of the symbol's total real Adds (known from
    // Pass 1's add_activity count) that the required reserve is used up,
    // regardless of whether the depth target was reached. ---
    constexpr std::size_t kTargetRestingDepth = 3000;
    constexpr std::uint64_t kWarmupCheckInterval = 1000;
    constexpr std::uint64_t kWarmupHardCap = 400000;
    constexpr double kAddReserveFraction = 0.3;
    constexpr std::size_t kAddReserveMin = 200;
    constexpr std::size_t kAddSliceMax = 150000;

    const std::uint64_t total_target_adds = add_activity[target_locate];
    const std::size_t add_reserve = std::max<std::size_t>(
        kAddReserveMin,
        std::min<std::size_t>(kAddSliceMax,
                              static_cast<std::size_t>(total_target_adds * kAddReserveFraction)));

    NullListener listener;
    lob::OptimizedOrderBook warmup_book(listener);
    std::unordered_map<std::uint64_t, std::int64_t> order_price;
    std::vector<std::int64_t> replace_deltas;
    std::vector<AddRecord> real_adds;
    std::uint64_t seen = 0;
    std::uint64_t replace_missing_side = 0;
    std::size_t adds_in_warmup = 0;
    bool warmup_active = true;
    std::uint64_t warmup_end_seen = 0;
    std::size_t warmup_end_depth = 0;

    auto maybe_end_warmup = [&]() {
        if (!warmup_active) return;
        const bool reserve_exhausted =
            total_target_adds > add_reserve && adds_in_warmup >= total_target_adds - add_reserve;
        if (!reserve_exhausted && seen % kWarmupCheckInterval != 0 && seen < kWarmupHardCap) return;
        const std::size_t depth = CollectRestingOrders(warmup_book).size();
        if (depth >= kTargetRestingDepth || seen >= kWarmupHardCap || reserve_exhausted) {
            warmup_active = false;
            warmup_end_seen = seen;
            warmup_end_depth = depth;
        }
    };

    lob::itch::MessageReader reader(data);
    while (const auto msg = reader.Next()) {
        switch (msg->type) {
            case 'A':
            case 'F': {
                const auto m = lob::itch::DecodeAddOrder(msg->body);
                if (m.stock_locate != target_locate) continue;
                ++seen;
                const lob::Side side = m.side == 'B' ? lob::Side::Buy : lob::Side::Sell;
                const lob::Price price{static_cast<std::int64_t>(m.price_ticks)};
                const lob::Quantity qty{static_cast<std::int64_t>(m.shares)};
                order_price[m.order_ref] = price.ticks;
                real_adds.push_back(AddRecord{side, price, qty});
                if (warmup_active) {
                    ++adds_in_warmup;
                    warmup_book.AddLimitOrder(lob::OrderId{m.order_ref}, side, price, qty);
                }
                break;
            }
            case 'E': {
                const auto m = lob::itch::DecodeOrderExecuted(msg->body);
                if (m.stock_locate != target_locate) continue;
                ++seen;
                if (warmup_active) {
                    warmup_book.ReduceRestingQuantity(
                        lob::OrderId{m.order_ref},
                        lob::Quantity{static_cast<std::int64_t>(m.executed_shares)});
                }
                break;
            }
            case 'C': {
                const auto m = lob::itch::DecodeOrderExecutedWithPrice(msg->body);
                if (m.stock_locate != target_locate) continue;
                ++seen;
                if (warmup_active) {
                    warmup_book.ReduceRestingQuantity(
                        lob::OrderId{m.order_ref},
                        lob::Quantity{static_cast<std::int64_t>(m.executed_shares)});
                }
                break;
            }
            case 'X': {
                const auto m = lob::itch::DecodeOrderCancel(msg->body);
                if (m.stock_locate != target_locate) continue;
                ++seen;
                if (warmup_active) {
                    warmup_book.ReduceRestingQuantity(
                        lob::OrderId{m.order_ref},
                        lob::Quantity{static_cast<std::int64_t>(m.canceled_shares)});
                }
                break;
            }
            case 'D': {
                const auto m = lob::itch::DecodeOrderDelete(msg->body);
                if (m.stock_locate != target_locate) continue;
                ++seen;
                order_price.erase(m.order_ref);
                if (warmup_active) {
                    warmup_book.CancelOrder(lob::OrderId{m.order_ref});
                }
                break;
            }
            case 'U': {
                const auto m = lob::itch::DecodeOrderReplace(msg->body);
                if (m.stock_locate != target_locate) continue;
                ++seen;
                const auto old_price_it = order_price.find(m.original_order_ref);
                if (old_price_it != order_price.end()) {
                    replace_deltas.push_back(static_cast<std::int64_t>(m.price_ticks) -
                                             old_price_it->second);
                    order_price.erase(old_price_it);
                    order_price[m.new_order_ref] = static_cast<std::int64_t>(m.price_ticks);
                }
                if (warmup_active) {
                    if (!warmup_book.SideOf(lob::OrderId{m.original_order_ref}).has_value()) {
                        ++replace_missing_side;
                    } else {
                        warmup_book.Replace(lob::OrderId{m.original_order_ref},
                                            lob::OrderId{m.new_order_ref},
                                            lob::Price{static_cast<std::int64_t>(m.price_ticks)},
                                            lob::Quantity{static_cast<std::int64_t>(m.shares)});
                    }
                }
                break;
            }
            default:
                continue;
        }
        maybe_end_warmup();
    }
    if (warmup_active) {
        // Reached EOF before the target depth or the hard cap -- record
        // whatever was actually achieved rather than leaving these at 0.
        warmup_end_seen = seen;
        warmup_end_depth = CollectRestingOrders(warmup_book).size();
    }

    const bool depth_reached = warmup_end_depth >= kTargetRestingDepth;
    const char* stop_reason = depth_reached                       ? "depth target reached"
                              : warmup_end_seen >= kWarmupHardCap ? "hard message cap"
                              : adds_in_warmup + add_reserve >= total_target_adds
                                  ? "Add reserve for the timed slice exhausted"
                                  : "reached EOF for this symbol";
    std::printf(
        "warmup: %llu messages replayed, resting depth reached %zu (target %zu%s) -- stopped "
        "due to: %s, (%llu Replace with unknown original side, skipped)\n",
        static_cast<unsigned long long>(warmup_end_seen), warmup_end_depth, kTargetRestingDepth,
        depth_reached ? "" : ", NOT REACHED", stop_reason,
        static_cast<unsigned long long>(replace_missing_side));
    std::printf("Adds used in warmup: %zu of %llu total (reserve for Add slice: %zu)\n",
                adds_in_warmup, static_cast<unsigned long long>(total_target_adds), add_reserve);
    std::printf("real_adds collected (whole file, this symbol): %zu\n", real_adds.size());
    std::printf("real Replace price deltas collected (whole file, this symbol): %zu\n",
                replace_deltas.size());

    const auto resting_after_warmup = CollectRestingOrders(warmup_book);
    const std::size_t level_count = CountLevels(warmup_book);
    std::printf("resting orders in warmed book: %zu across %zu price levels (%.2f orders/level)\n",
                resting_after_warmup.size(), level_count,
                level_count > 0 ? static_cast<double>(resting_after_warmup.size()) /
                                      static_cast<double>(level_count)
                                : 0.0);
    if (resting_after_warmup.size() < 50) {
        std::fprintf(stderr,
                     "bench_matching_engine: WARNING -- only %zu resting orders. Cancel/Reduce/"
                     "Replace means below are cycling this tiny pool repeatedly; treat them with "
                     "more caution than Add_real/Add_widened.\n",
                     resting_after_warmup.size());
    }

    if (replace_deltas.empty()) {
        std::fprintf(stderr,
                     "bench_matching_engine: no real Replace messages found for this symbol -- "
                     "cannot sample a price-delta distribution, aborting\n");
        return 1;
    }

    if (!pmu.IsL1dCacheRefillsAvailable()) {
        std::fprintf(stderr,
                     "warning: ARM_L1D_CACHE_REFILL not available on this chip/OS -- "
                     "l1d_cache_refills columns will be empty\n");
    }
#if defined(LOB_TRACK_CHURN) && LOB_TRACK_CHURN
    std::fprintf(stderr,
                 "NOTE: built with LOB_TRACK_CHURN=ON -- churn_* columns in this run are "
                 "meaningful, but cycles/instructions/l1d_cache_refills are NOT: counting "
                 "every level/order-storage event adds real overhead to the exact code paths "
                 "being cycle-timed. Never cite cycle numbers from this build; re-run with "
                 "LOB_TRACK_CHURN=OFF (the default) for those.\n");
#endif

    std::ofstream csv(args.out_csv);
    csv << "variant,ops,cycles,instructions,mean_cycles_per_op,mean_ns_per_op,l1d_cache_refills,"
           "mean_l1d_refills_per_op,level_creates,level_destroys,order_storage_allocs,"
           "order_storage_frees,mean_storage_allocs_per_op,migrated\n";

    std::printf("\n--- results (mean cycles/op, single aggregate bracket per variant) ---\n");

    // --- Add_real: a real, unmodified, contiguous slice of later Add
    // messages for this symbol -- the messages actually used to build
    // warmup_book are excluded by starting the slice after adds_in_warmup
    // (tracked during the scan above). kAddReserveFraction (see above)
    // guarantees warmup stopped early enough to leave a slice here even
    // when the depth target was never reached. ---
    const std::size_t add_slice_begin = adds_in_warmup;
    const std::size_t add_slice_end = std::min(real_adds.size(), add_slice_begin + kAddSliceMax);

    {
        lob::OptimizedOrderBook pass_book = warmup_book;
        auto result = TimePass(pmu, "Add_real", add_slice_end - add_slice_begin, [&]() {
            for (std::size_t i = add_slice_begin; i < add_slice_end; ++i) {
                const auto& a = real_adds[i];
                pass_book.AddLimitOrder(lob::OrderId{kSyntheticIdBase + i}, a.side, a.price, a.qty);
            }
        });
        PrintAndRecord(result, ghz, csv);
    }

    // --- Add_widened: same slice, each price's distance from the pass's
    // OWN live mid scaled by kWidenMultiplier -- synthetic, labeled as
    // such, bounding how much of any future gain depends on tight real-
    // world clustering. ---
    {
        lob::OptimizedOrderBook pass_book = warmup_book;
        auto result = TimePass(pmu, "Add_widened", add_slice_end - add_slice_begin, [&]() {
            for (std::size_t i = add_slice_begin; i < add_slice_end; ++i) {
                const auto& a = real_adds[i];
                const auto bid = pass_book.BestBid();
                const auto ask = pass_book.BestAsk();
                lob::Price price = a.price;
                if (bid.has_value() && ask.has_value()) {
                    const double mid =
                        (static_cast<double>(bid->ticks) + static_cast<double>(ask->ticks)) / 2.0;
                    const double widened =
                        mid + (static_cast<double>(a.price.ticks) - mid) * kWidenMultiplier;
                    price = lob::Price{static_cast<std::int64_t>(widened)};
                }
                pass_book.AddLimitOrder(lob::OrderId{kSyntheticIdBase + i}, a.side, price, a.qty);
            }
        });
        PrintAndRecord(result, ghz, csv);
    }

    // --- Cancel/Reduce/Replace: traversal order (best-case locality,
    // secondary) and shuffled order (primary), against the same real
    // resting-order pool -- CYCLED (see TimeCyclingPass) to reach
    // kMinCyclingOps total, since no real single-snapshot resting pool on
    // this venue gets anywhere near that on its own (see the comment on
    // TimeCyclingPass). ---
    constexpr std::uint64_t kMinCyclingOps = 30000;

    std::vector<std::size_t> traversal_idx(resting_after_warmup.size());
    for (std::size_t i = 0; i < traversal_idx.size(); ++i) traversal_idx[i] = i;
    std::vector<std::size_t> shuffled_idx = traversal_idx;
    DeterministicShuffle(shuffled_idx, kFixedSeed);

    auto print_cycling_info = [&](const char* label, std::size_t pool_size) {
        const std::uint64_t rounds =
            pool_size == 0 ? 0 : (kMinCyclingOps + pool_size - 1) / pool_size;
        std::printf("  [%-18s pool=%zu ops/round x %llu rounds]\n", label, pool_size,
                    static_cast<unsigned long long>(rounds));
    };

    auto run_cancel = [&](const char* label, const std::vector<std::size_t>& order) {
        lob::OptimizedOrderBook pass_book = warmup_book;
        print_cycling_info(label, order.size());
        auto result = TimeCyclingPass(
            pmu, label, order.size(), kMinCyclingOps,
            [&]() {
                for (const auto idx : order) pass_book.CancelOrder(resting_after_warmup[idx].id);
            },
            [&]() {
                // Untimed: re-add each cancelled order back under its
                // ORIGINAL id/price/qty so the next round is identical to
                // this one.
                for (const auto idx : order) {
                    const auto& o = resting_after_warmup[idx];
                    pass_book.AddLimitOrder(o.id, o.side, o.price, o.qty);
                }
            });
        PrintAndRecord(result, ghz, csv);
    };
    run_cancel("Cancel_traversal", traversal_idx);
    run_cancel("Cancel_shuffled", shuffled_idx);

    // Reduce by half the resting quantity (minimum 1) -- an arbitrary but
    // stated choice; unlike Replace's price delta, no specific real-data
    // requirement was set for the reduce amount, and the op's cost is
    // driven by which order/level is touched, not by the exact amount
    // subtracted. Replenish uses ModifyOrder (Phase 1's own primitive,
    // not Replace) to restore the ORIGINAL quantity before each round --
    // the order never fully empties (max(1, qty/2) never reaches zero),
    // so it's always found.
    auto run_reduce = [&](const char* label, const std::vector<std::size_t>& order) {
        lob::OptimizedOrderBook pass_book = warmup_book;
        print_cycling_info(label, order.size());
        auto result = TimeCyclingPass(
            pmu, label, order.size(), kMinCyclingOps,
            [&]() {
                for (const auto idx : order) {
                    const auto& o = resting_after_warmup[idx];
                    const std::int64_t amount = std::max<std::int64_t>(1, o.qty.units / 2);
                    pass_book.ReduceRestingQuantity(o.id, lob::Quantity{amount});
                }
            },
            [&]() {
                for (const auto idx : order) {
                    const auto& o = resting_after_warmup[idx];
                    pass_book.ModifyOrder(o.id, o.price, o.qty);
                }
            });
        PrintAndRecord(result, ghz, csv);
    };
    run_reduce("Reduce_traversal", traversal_idx);
    run_reduce("Reduce_shuffled", shuffled_idx);

    // Replace: new price = resting price + a delta sampled (raw
    // mt19937_64, no std:: distribution -- see kFixedSeed's comment)
    // from the REAL empirical distribution of this symbol's Replace
    // price deltas -- a FRESH delta each round, continuing the same RNG
    // stream, so every round is a real (not repeated) sample. New id is
    // synthetic each round; quantity is left unchanged -- only price was
    // flagged as the distribution that must come from real data.
    // Replenish tracks each pool slot's CURRENT id (it changes every
    // round) and resets it to the ORIGINAL id/price via cancel + re-add.
    auto run_replace = [&](const char* label, const std::vector<std::size_t>& order) {
        lob::OptimizedOrderBook pass_book = warmup_book;
        print_cycling_info(label, order.size());
        std::mt19937_64 delta_rng(kFixedSeed);
        std::uint64_t next_synthetic_id = kSyntheticIdBase + real_adds.size() + 1;
        std::vector<lob::OrderId> current_id(resting_after_warmup.size());
        for (std::size_t i = 0; i < resting_after_warmup.size(); ++i) {
            current_id[i] = resting_after_warmup[i].id;
        }
        auto result = TimeCyclingPass(
            pmu, label, order.size(), kMinCyclingOps,
            [&]() {
                for (const auto idx : order) {
                    const auto& o = resting_after_warmup[idx];
                    const std::int64_t delta = replace_deltas[delta_rng() % replace_deltas.size()];
                    const lob::Price new_price{o.price.ticks + delta};
                    const lob::OrderId new_id{next_synthetic_id++};
                    pass_book.Replace(current_id[idx], new_id, new_price, o.qty);
                    current_id[idx] = new_id;
                }
            },
            [&]() {
                for (const auto idx : order) {
                    const auto& o = resting_after_warmup[idx];
                    pass_book.CancelOrder(current_id[idx]);
                    pass_book.AddLimitOrder(o.id, o.side, o.price, o.qty);
                    current_id[idx] = o.id;
                }
            });
        PrintAndRecord(result, ghz, csv);
    };
    run_replace("Replace_traversal", traversal_idx);
    run_replace("Replace_shuffled", shuffled_idx);

    std::printf("\nwrote %s\n", args.out_csv.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bench_matching_engine: %s\n", e.what());
        return 1;
    }
}
