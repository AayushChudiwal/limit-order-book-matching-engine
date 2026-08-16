#include "lob/bench/pmu.hpp"

#include <dlfcn.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace lob::bench {

namespace {

// ---- Private API surface -----------------------------------------------
// Function names and the class-mask constants below are verified against
// this machine's actual shared-cache exports (`dyld_info -exports` on
// /System/Library/PrivateFrameworks/kperf.framework and
// kperfdata.framework) before writing this file -- not reconstructed
// from memory of a years-old blog post. Struct internals for kpep_db /
// kpep_config / kpep_event are never touched directly (only opaque
// pointers are passed between calls), so this file doesn't depend on
// guessing their layout.

constexpr std::uint32_t kKpcClassFixedMask = 1u << 0;
constexpr std::uint32_t kKpcClassConfigurableMask = 1u << 1;

struct kpep_db;
struct kpep_config;
struct kpep_event;

using kpc_set_counting_t = int (*)(std::uint32_t);
using kpc_set_thread_counting_t = int (*)(std::uint32_t);
using kpc_get_config_t = int (*)(std::uint32_t, std::uint64_t*);
using kpc_set_config_t = int (*)(std::uint32_t, std::uint64_t*);
using kpc_get_counter_count_t = std::uint32_t (*)(std::uint32_t);
using kpc_get_thread_counters_t = int (*)(std::uint32_t, std::uint32_t, std::uint64_t*);
using kpc_force_all_ctrs_set_t = int (*)(int);

using kpep_db_create_t = int (*)(const char*, kpep_db**);
using kpep_db_free_t = void (*)(kpep_db*);
using kpep_db_event_t = int (*)(kpep_db*, const char*, kpep_event**);
using kpep_config_create_t = int (*)(kpep_db*, kpep_config**);
using kpep_config_free_t = void (*)(kpep_config*);
using kpep_config_add_event_t = int (*)(kpep_config*, kpep_event**, std::uint32_t, std::uint32_t*);
using kpep_config_force_counters_t = int (*)(kpep_config*);
using kpep_config_kpc_t = int (*)(kpep_config*, std::uint64_t*, std::size_t);
using kpep_config_kpc_count_t = int (*)(kpep_config*, std::size_t*);
using kpep_config_kpc_map_t = int (*)(kpep_config*, std::size_t*, std::size_t);

struct KpcApi {
    kpc_set_counting_t set_counting = nullptr;
    kpc_set_thread_counting_t set_thread_counting = nullptr;
    kpc_get_config_t get_config = nullptr;
    kpc_set_config_t set_config = nullptr;
    kpc_get_counter_count_t get_counter_count = nullptr;
    kpc_get_thread_counters_t get_thread_counters = nullptr;
    kpc_force_all_ctrs_set_t force_all_ctrs_set = nullptr;

    kpep_db_create_t db_create = nullptr;
    kpep_db_free_t db_free = nullptr;
    kpep_db_event_t db_event = nullptr;
    kpep_config_create_t config_create = nullptr;
    kpep_config_free_t config_free = nullptr;
    kpep_config_add_event_t config_add_event = nullptr;
    kpep_config_force_counters_t config_force_counters = nullptr;
    kpep_config_kpc_t config_kpc = nullptr;
    kpep_config_kpc_count_t config_kpc_count = nullptr;
    kpep_config_kpc_map_t config_kpc_map = nullptr;
};

template <typename F>
F Resolve(void* handle, const char* name) {
    void* sym = dlsym(handle, name);
    if (sym == nullptr) {
        throw std::runtime_error(std::string("PmuCounters: missing symbol ") + name +
                                 " -- private API surface changed on this OS version");
    }
    return reinterpret_cast<F>(sym);
}

KpcApi LoadApi() {
    void* kperf = dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_NOW);
    if (kperf == nullptr) {
        throw std::runtime_error(std::string("PmuCounters: failed to dlopen kperf.framework: ") +
                                 dlerror());
    }
    void* kperfdata =
        dlopen("/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata", RTLD_NOW);
    if (kperfdata == nullptr) {
        throw std::runtime_error(
            std::string("PmuCounters: failed to dlopen kperfdata.framework: ") + dlerror());
    }

    KpcApi api;
    api.set_counting = Resolve<kpc_set_counting_t>(kperf, "kpc_set_counting");
    api.set_thread_counting = Resolve<kpc_set_thread_counting_t>(kperf, "kpc_set_thread_counting");
    api.get_config = Resolve<kpc_get_config_t>(kperf, "kpc_get_config");
    api.set_config = Resolve<kpc_set_config_t>(kperf, "kpc_set_config");
    api.get_counter_count = Resolve<kpc_get_counter_count_t>(kperf, "kpc_get_counter_count");
    api.get_thread_counters = Resolve<kpc_get_thread_counters_t>(kperf, "kpc_get_thread_counters");
    api.force_all_ctrs_set = Resolve<kpc_force_all_ctrs_set_t>(kperf, "kpc_force_all_ctrs_set");

    api.db_create = Resolve<kpep_db_create_t>(kperfdata, "kpep_db_create");
    api.db_free = Resolve<kpep_db_free_t>(kperfdata, "kpep_db_free");
    api.db_event = Resolve<kpep_db_event_t>(kperfdata, "kpep_db_event");
    api.config_create = Resolve<kpep_config_create_t>(kperfdata, "kpep_config_create");
    api.config_free = Resolve<kpep_config_free_t>(kperfdata, "kpep_config_free");
    api.config_add_event = Resolve<kpep_config_add_event_t>(kperfdata, "kpep_config_add_event");
    api.config_force_counters =
        Resolve<kpep_config_force_counters_t>(kperfdata, "kpep_config_force_counters");
    api.config_kpc = Resolve<kpep_config_kpc_t>(kperfdata, "kpep_config_kpc");
    api.config_kpc_count = Resolve<kpep_config_kpc_count_t>(kperfdata, "kpep_config_kpc_count");
    api.config_kpc_map = Resolve<kpep_config_kpc_map_t>(kperfdata, "kpep_config_kpc_map");
    return api;
}

const KpcApi& Api() {
    static const KpcApi api = LoadApi();
    return api;
}

}  // namespace

PmuCounters::PmuCounters() {
    const KpcApi& api = Api();

    if (geteuid() != 0) {
        throw std::runtime_error(
            "PmuCounters: not running as root -- kpc_set_counting/kpc_set_thread_counting "
            "require root. Re-run this binary under sudo.");
    }

    // kpc_cpu_string() (the private-API way to get this) empirically
    // failed on this M4 during validation (see docs/pmu_validation.md) --
    // machdep.cpu.brand_string is a public, documented sysctl and reports
    // the same information, so use that instead rather than debugging an
    // undocumented function this code doesn't otherwise depend on.
    char cpu_buf[128] = {};
    std::size_t cpu_buf_len = sizeof(cpu_buf);
    if (sysctlbyname("machdep.cpu.brand_string", cpu_buf, &cpu_buf_len, nullptr, 0) == 0) {
        cpu_string_ = cpu_buf;
    } else {
        cpu_string_ = "(unknown -- machdep.cpu.brand_string sysctl failed)";
    }

    kpep_db* db = nullptr;
    if (api.db_create(nullptr, &db) != 0 || db == nullptr) {
        throw std::runtime_error(
            "PmuCounters: kpep_db_create failed -- no event database for this CPU");
    }

    kpep_config* cfg = nullptr;
    if (api.config_create(db, &cfg) != 0 || cfg == nullptr) {
        api.db_free(db);
        throw std::runtime_error("PmuCounters: kpep_config_create failed");
    }

    if (api.config_force_counters(cfg) != 0) {
        api.config_free(cfg);
        api.db_free(db);
        throw std::runtime_error("PmuCounters: kpep_config_force_counters failed");
    }

    // Fixed counters (cycles, instructions) need no event lookup -- they
    // are dedicated hardware counters on every generation. Configurable
    // counters (branch mispredicts, L1D cache refills) are added by NAME
    // through the CPU's own event database and are best-effort: if the
    // name isn't present (a chip generation genuinely lacking that
    // event) or there's no free configurable counter slot, that specific
    // counter is simply left unavailable rather than failing construction
    // entirely -- cycles/instructions are still useful on their own.
    auto try_add_configurable_event = [&](const char* name) -> bool {
        kpep_event* ev = nullptr;
        if (api.db_event(db, name, &ev) != 0 || ev == nullptr) {
            return false;
        }
        std::uint32_t err = 0;
        return api.config_add_event(cfg, &ev, 0, &err) == 0;
    };

    // ARM_BR_MIS_PRED is deliberately NOT attempted: tools/pmu_validate.cpp
    // measured 11 mispredicts out of 1,000,000 genuinely data-dependent
    // (unpredictable) branches on this M4 -- essentially zero, which is
    // not what a real branch-mispredict counter reports for random
    // branches. Whatever this event counts on this chip/OS combination,
    // it isn't branch mispredicts, so it is never configured and
    // IsBranchMispredictsAvailable() always returns false here. See
    // docs/pmu_validation.md for the raw numbers. Re-enabling this on a
    // future OS/chip revision is a one-line change:
    // try_add_configurable_event("ARM_BR_MIS_PRED").
    branch_mispredicts_available_ = false;
    l1d_cache_refills_available_ = try_add_configurable_event("ARM_L1D_CACHE_REFILL");

    std::size_t kpc_count = 0;
    if (api.config_kpc_count(cfg, &kpc_count) != 0) {
        api.config_free(cfg);
        api.db_free(db);
        throw std::runtime_error("PmuCounters: kpep_config_kpc_count failed");
    }

    std::vector<std::uint64_t> kpc_config(kpc_count, 0);
    if (api.config_kpc(cfg, kpc_config.data(), kpc_config.size() * sizeof(std::uint64_t)) != 0) {
        api.config_free(cfg);
        api.db_free(db);
        throw std::runtime_error("PmuCounters: kpep_config_kpc failed");
    }

    // Where each added configurable event landed in the counter buffer --
    // NOT necessarily insertion order, hence asking the API rather than
    // assuming.
    std::vector<std::size_t> kpc_map(kpc_count, 0);
    if (api.config_kpc_map(cfg, kpc_map.data(), kpc_map.size() * sizeof(std::size_t)) != 0) {
        api.config_free(cfg);
        api.db_free(db);
        throw std::runtime_error("PmuCounters: kpep_config_kpc_map failed");
    }

    const std::uint32_t fixed_count = api.get_counter_count(kKpcClassFixedMask);
    counter_buf_size_ =
        fixed_count + static_cast<std::size_t>(api.get_counter_count(kKpcClassConfigurableMask));
    read_buf_.assign(counter_buf_size_, 0);

    // Configurable events were added in this order: branch mispredicts
    // (if available), then L1D cache refills (if available) -- kpc_map[i]
    // gives the counter-buffer slot for the i-th configured event, which
    // this walk assigns in that same order.
    std::size_t next_map_index = 0;
    if (branch_mispredicts_available_ && next_map_index < kpc_map.size()) {
        branch_mispredicts_index_ = static_cast<int>(fixed_count + kpc_map[next_map_index]);
        ++next_map_index;
    }
    if (l1d_cache_refills_available_ && next_map_index < kpc_map.size()) {
        l1d_cache_refills_index_ = static_cast<int>(fixed_count + kpc_map[next_map_index]);
        ++next_map_index;
    }

    if (api.force_all_ctrs_set(1) != 0) {
        api.config_free(cfg);
        api.db_free(db);
        throw std::runtime_error("PmuCounters: kpc_force_all_ctrs_set failed");
    }

    if (api.set_config(kKpcClassConfigurableMask, kpc_config.data()) != 0) {
        api.config_free(cfg);
        api.db_free(db);
        throw std::runtime_error("PmuCounters: kpc_set_config failed");
    }

    api.config_free(cfg);
    api.db_free(db);

    const std::uint32_t classes = kKpcClassFixedMask | kKpcClassConfigurableMask;
    if (api.set_counting(classes) != 0) {
        throw std::runtime_error(
            "PmuCounters: kpc_set_counting failed (root required -- see constructor check above; "
            "this failing anyway usually means SIP or a security policy is blocking PMU access)");
    }
    if (api.set_thread_counting(classes) != 0) {
        throw std::runtime_error("PmuCounters: kpc_set_thread_counting failed");
    }
}

PmuCounters::~PmuCounters() {
    // Deliberately leaves system-wide counting enabled rather than
    // tearing it down: kpc_set_counting(0) here would also be racing any
    // OTHER process that started counting concurrently (e.g. Instruments)
    // and stop theirs. Benchmark processes are short-lived; this is the
    // same tradeoff every kpc-based tool in the wild makes.
}

PmuSnapshot PmuCounters::Read() const {
    // read_buf_ is sized once in the constructor and reused here -- no
    // per-call allocation. See its declaration in pmu.hpp for why that
    // matters.
    if (Api().get_thread_counters(0, static_cast<std::uint32_t>(read_buf_.size()),
                                  read_buf_.data()) != 0) {
        throw std::runtime_error("PmuCounters::Read: kpc_get_thread_counters failed");
    }

    PmuSnapshot snap;
    snap.cycles = read_buf_.size() > 0 ? read_buf_[0] : 0;
    snap.instructions = read_buf_.size() > 1 ? read_buf_[1] : 0;
    if (branch_mispredicts_index_ >= 0 &&
        static_cast<std::size_t>(branch_mispredicts_index_) < read_buf_.size()) {
        snap.branch_mispredicts = read_buf_[static_cast<std::size_t>(branch_mispredicts_index_)];
    }
    if (l1d_cache_refills_index_ >= 0 &&
        static_cast<std::size_t>(l1d_cache_refills_index_) < read_buf_.size()) {
        snap.l1d_cache_refills = read_buf_[static_cast<std::size_t>(l1d_cache_refills_index_)];
    }
    return snap;
}

}  // namespace lob::bench
