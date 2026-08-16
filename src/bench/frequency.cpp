#include "lob/bench/frequency.hpp"

#include <mach/mach_time.h>

#include <cstdint>

namespace lob::bench {

double CalibrateGigahertz(const PmuCounters& pmu) {
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);

    // Not a fixed-instruction-count loop like pmu_validate.cpp's tests --
    // this only needs to burn CPU-bound wall-clock time, not prove an
    // exact instruction count. `sink` is volatile so the store on every
    // iteration is an observable side effect the compiler cannot elide or
    // batch away, which keeps the loop from being optimized down to
    // fewer iterations than actually run.
    constexpr std::uint64_t kIterations = 2'000'000'000ull;
    volatile std::uint64_t sink = 0;

    const auto before = pmu.Read();
    const std::uint64_t wall_before = mach_absolute_time();
    for (std::uint64_t i = 0; i < kIterations; ++i) {
        sink = sink + i;
    }
    const std::uint64_t wall_after = mach_absolute_time();
    const auto after = pmu.Read();

    const std::uint64_t cycles = after.cycles - before.cycles;
    const double wall_ns = static_cast<double>(wall_after - wall_before) *
                           static_cast<double>(timebase.numer) /
                           static_cast<double>(timebase.denom);
    // GHz (10^9 cycles/sec) and cycles/ns are the same number -- 1 ns is
    // 10^-9 sec, so cycles/sec / 10^9 == cycles/ns == GHz.
    return static_cast<double>(cycles) / wall_ns;
}

}  // namespace lob::bench
