#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace lob::bench {

// Raw, cumulative (not delta) PMU counter reads for the CURRENT THREAD.
// Callers take two snapshots around a region of interest and subtract.
struct PmuSnapshot {
    std::uint64_t cycles = 0;
    std::uint64_t instructions = 0;
    std::optional<std::uint64_t> branch_mispredicts;
    std::optional<std::uint64_t> l1d_cache_refills;
};

// Wraps Apple's private kperf.framework (KPC: enable/read hardware
// counters) and kperfdata.framework (KPEP: the per-CPU event name
// database at /usr/share/kpep/, e.g. as4.plist for this M4). Deliberately
// does NOT hardcode M1-era event codes (the CPMU_* magic numbers from
// Lemire/ibireme's original 2021 writeup): those were reverse-engineered
// specifically for the M1 and are not guaranteed to mean the same thing,
// or exist at all, on later chips. Instead this looks up events BY NAME
// ("ARM_BR_MIS_PRED", "ARM_L1D_CACHE_REFILL") through kpep_db_event(),
// which resolves against whatever /usr/share/kpep/<chip>.plist matches
// the CPU actually running -- Apple's own database is the
// chip-specific-correctness authority, not a number copied from a blog
// post. FIXED_CYCLES and FIXED_INSTRUCTIONS are dedicated fixed-function
// counters present on every generation and always attempted first;
// configurable-counter events (branch mispredicts, L1D cache refills)
// are best-effort -- see IsBranchMispredictsAvailable() /
// IsL1dCacheRefillsAvailable() -- and this class does NOT claim they are
// trustworthy on its own. That claim only comes from
// tools/pmu_validate.cpp actually running on the target hardware; see
// docs/pmu_validation.md for the recorded result this repo's benchmark
// numbers rely on. On this M4, validation found ARM_BR_MIS_PRED does not
// actually count branch mispredicts (near-zero on genuinely
// unpredictable branches), so this class never configures it --
// IsBranchMispredictsAvailable() always returns false here. cycles,
// instructions, and ARM_L1D_CACHE_REFILL all passed validation.
//
// Requires root (kpc_set_counting/kpc_set_thread_counting fail
// otherwise) -- the constructor throws std::runtime_error with a message
// naming this if it isn't running as root, rather than silently
// producing zeros.
class PmuCounters {
  public:
    PmuCounters();
    ~PmuCounters();

    PmuCounters(const PmuCounters&) = delete;
    PmuCounters& operator=(const PmuCounters&) = delete;

    [[nodiscard]] PmuSnapshot Read() const;

    [[nodiscard]] bool IsBranchMispredictsAvailable() const {
        return branch_mispredicts_available_;
    }
    [[nodiscard]] bool IsL1dCacheRefillsAvailable() const { return l1d_cache_refills_available_; }

    // The exact CPU string kpc_cpu_string() reports (e.g. "Apple M4") --
    // included in every committed benchmark artifact so results are never
    // silently misattributed to the wrong chip.
    [[nodiscard]] const std::string& CpuString() const { return cpu_string_; }

  private:
    bool branch_mispredicts_available_ = false;
    bool l1d_cache_refills_available_ = false;
    std::string cpu_string_;

    // Index into the kpc_get_thread_counters() buffer for each
    // configurable event actually configured, or -1 if not configured.
    // Fixed counters are always buffer indices [0]=cycles,
    // [1]=instructions on Apple Silicon.
    int branch_mispredicts_index_ = -1;
    int l1d_cache_refills_index_ = -1;
    std::size_t counter_buf_size_ = 0;
};

}  // namespace lob::bench
