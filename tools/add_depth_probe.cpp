// add_depth_probe: how does the cost of an add scale with how many
// orders already sit at the target price level?
//
// This is the evidence tool behind docs/phase5_step4_results.md's
// attribution table -- the one that explains why step 4's arena
// regressed Add_real by 15.8% while improving Add_widened by 23%, and
// why making level totals incremental fixed it. Committed rather than
// left as a throwaway, because a conclusion whose supporting measurement
// cannot be re-run is weaker than this project's usual standard (see
// docs/phase5_step2_results.md's own note about exactly that mistake).
//
// WHY IT MEASURES DEPTH SPECIFICALLY
// OnBookUpdate reports a level's aggregate resting quantity after every
// change. If that total is RECOMPUTED by walking the level, an add costs
// O(orders-at-that-level) and the per-element iteration cost is
// multiplied by depth. If it is MAINTAINED incrementally, the cost is
// flat in depth. Sweeping depth therefore distinguishes the two designs
// directly, rather than inferring it from an aggregate benchmark number.
//
// NO PMU, SO NO SUDO. Wall clock only. Absolute numbers are NOT
// comparable to bench_matching_engine's cycle counts; the shape of the
// curve, and the ratio between binaries, is the point.
//
// REPRODUCING THE THREE-WAY COMPARISON. The table in the results doc
// compares three engine versions. Build this file against each:
//
//   # current HEAD
//   c++ -std=c++20 -O2 -DNDEBUG -DLOB_ARENA_GENERATION_TAGS=0 -Iinclude \
//       tools/add_depth_probe.cpp src/optimized_order_book.cpp \
//       src/bench/churn.cpp -o /tmp/probe_head
//
//   # a historical engine (e.g. step 2, before the arena)
//   mkdir -p /tmp/old/lob && git show 98b1997:include/lob/optimized_order_book.hpp \
//       > /tmp/old/lob/optimized_order_book.hpp
//   git show 98b1997:src/optimized_order_book.cpp > /tmp/old/optimized_order_book.cpp
//   cp -r include/lob/bench include/lob/listener.hpp include/lob/types.hpp \
//       include/lob/order_book.hpp /tmp/old/lob/
//   c++ -std=c++20 -O2 -DNDEBUG -I/tmp/old -Iinclude \
//       tools/add_depth_probe.cpp /tmp/old/optimized_order_book.cpp \
//       src/bench/churn.cpp -o /tmp/probe_step2
//
// INTERLEAVE THE BINARIES. Do not run one to completion and then the
// other: on this machine the SAME binary doing identical work measured
// 125.75 ns/add and 51.09 ns/add ten minutes apart. Run them
// alternately for several passes and take each one's minimum, so
// machine drift lands on all candidates instead of whichever ran during
// a bad patch. See docs/benchmark_methodology.md, "Measurement
// confounds this project has actually hit".
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "lob/listener.hpp"
#include "lob/optimized_order_book.hpp"

using namespace lob;

namespace {

struct NullListener : BookListener {
    void OnOrderAccepted(OrderId) override {}
    void OnOrderCancelled(OrderId, Quantity) override {}
    void OnOrderModified(OrderId, Quantity) override {}
    void OnOrderRejected(OrderId, RejectReason) override {}
    void OnFill(const Fill&) override {}
    // Summing here keeps the reported total observably used, so the
    // OnBookUpdate payload (the whole point of this measurement) cannot
    // be optimized away.
    void OnBookUpdate(Side, Price, Quantity q) override { sink += q.units; }
    std::int64_t sink = 0;
};

// depth_per_level: orders resting at each price BEFORE timing starts.
double TimeAdds(int levels, int depth_per_level, int timed_adds) {
    NullListener listener;
    OptimizedOrderBook book(listener);

    std::uint64_t id = 1;
    // Untimed pre-population. Prices spaced so nothing ever crosses.
    for (int level = 0; level < levels; ++level) {
        for (int d = 0; d < depth_per_level; ++d) {
            book.AddLimitOrder(OrderId{id++}, Side::Buy, Price{1000 + level * 10}, Quantity{100});
        }
    }

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < timed_adds; ++i) {
        book.AddLimitOrder(OrderId{id++}, Side::Buy, Price{1000 + (i % levels) * 10},
                           Quantity{100});
    }
    const auto end = std::chrono::steady_clock::now();

    if (listener.sink < 0) std::printf("unreachable\n");  // keep the work alive
    return std::chrono::duration<double, std::nano>(end - start).count() / timed_adds;
}

}  // namespace

int main() {
    // 110 levels matches the warmed SPY book this project benchmarks
    // against; 9,000 adds matches Add_real's 8,951-op slice, so the
    // level depths reached here resemble the real pass rather than
    // growing without bound.
    constexpr int kLevels = 110;
    constexpr int kAdds = 9000;
    constexpr int kReps = 5;

    std::printf("add_depth_probe: %d levels, %d timed adds, min of %d reps\n", kLevels, kAdds,
                kReps);
    std::printf("%-44s %10s\n", "orders already resting at the target level", "ns/add");
    for (int depth : {0, 1, 2, 4, 8}) {
        double best = 1e18;
        for (int rep = 0; rep < kReps; ++rep) {
            best = std::min(best, TimeAdds(kLevels, depth, kAdds));
        }
        std::printf("%-44d %10.2f\n", depth, best);
    }
    std::printf(
        "\nFlat across depth => the level total is O(1). Rising => it is\n"
        "being recomputed by walking the level, and the slope is the\n"
        "per-element iteration cost.\n");
    return 0;
}
