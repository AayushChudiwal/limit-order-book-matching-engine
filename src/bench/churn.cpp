#include "lob/bench/churn.hpp"

namespace lob::bench {

#if defined(LOB_TRACK_CHURN) && LOB_TRACK_CHURN

namespace {
std::uint64_t g_level_creates = 0;
std::uint64_t g_level_destroys = 0;
std::uint64_t g_order_storage_allocs = 0;
std::uint64_t g_order_storage_frees = 0;
}  // namespace

void RecordLevelCreate() { ++g_level_creates; }
void RecordLevelDestroy() { ++g_level_destroys; }
void RecordOrderStorageAlloc() { ++g_order_storage_allocs; }
void RecordOrderStorageFree() { ++g_order_storage_frees; }

ChurnSnapshot ReadChurn() {
    return ChurnSnapshot{g_level_creates, g_level_destroys, g_order_storage_allocs,
                         g_order_storage_frees};
}

#else

ChurnSnapshot ReadChurn() { return ChurnSnapshot{}; }

#endif

}  // namespace lob::bench
