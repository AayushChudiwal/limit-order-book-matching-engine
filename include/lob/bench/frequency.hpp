#pragma once

#include "lob/bench/pmu.hpp"

namespace lob::bench {

// Empirically measures THIS run's cycles-per-second by racing the cycle
// counter against mach_absolute_time over a busy loop lasting a few
// hundred ms (long enough that mach_absolute_time's ~41ns granularity is
// negligible) -- the same cross-check tools/pmu_validate.cpp used to
// validate the cycles counter in the first place (docs/pmu_validation.md
// measured ~4.38GHz this way). Run fresh per invocation rather than
// hardcoded from that one prior measurement: Apple Silicon exposes no way
// to fix or even read a target frequency, and thermal state can shift it
// run to run -- see docs/benchmark_methodology.md.
[[nodiscard]] double CalibrateGigahertz(const PmuCounters& pmu);

}  // namespace lob::bench
