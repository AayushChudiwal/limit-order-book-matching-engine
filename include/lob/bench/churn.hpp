#pragma once

#include <cstdint>

namespace lob::bench {

// Counts price-level and order-storage lifecycle events inside
// OptimizedOrderBook's LevelOrders (see include/lob/optimized_order_book.hpp),
// mirroring PmuCounters' before/after snapshot pattern -- callers take two
// ReadChurn() snapshots around a region and subtract.
//
// Gated entirely behind the LOB_TRACK_CHURN compile definition (CMake
// option LOB_TRACK_CHURN, default OFF): when off, RecordLevelCreate() /
// RecordLevelDestroy() / RecordOrderStorageAlloc() / RecordOrderStorageFree()
// are empty inline functions that the optimizer removes completely, and
// ReadChurn() always returns a zeroed snapshot. This exists so the SAME
// LevelOrders code compiled for the trusted N=10/N=20 PMU cycle
// comparisons (docs/benchmark_methodology.md) carries zero overhead from
// this instrumentation -- churn counting is a separate, deliberately-
// opted-into diagnostic build, not something silently riding along in
// every cycle number this project cites. See docs/phase5_plan.md's
// standing rules for why that separation matters.
//
// Plain (non-atomic) counters: OrderBook/OptimizedOrderBook are
// single-threaded by design (see BookListener's own comment on why
// virtual dispatch was an acceptable choice for the same reason), so
// there is no concurrent-increment case to guard against here.
struct ChurnSnapshot {
    std::uint64_t level_creates = 0;
    std::uint64_t level_destroys = 0;
    std::uint64_t order_storage_allocs = 0;
    std::uint64_t order_storage_frees = 0;
};

[[nodiscard]] ChurnSnapshot ReadChurn();

#if defined(LOB_TRACK_CHURN) && LOB_TRACK_CHURN
void RecordLevelCreate();
void RecordLevelDestroy();
void RecordOrderStorageAlloc();
void RecordOrderStorageFree();
#else
inline void RecordLevelCreate() {}
inline void RecordLevelDestroy() {}
inline void RecordOrderStorageAlloc() {}
inline void RecordOrderStorageFree() {}
#endif

}  // namespace lob::bench
