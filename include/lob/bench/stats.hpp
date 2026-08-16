#pragma once

#include <cstddef>
#include <vector>

namespace lob::bench {

// Percentiles over a set of per-operation cycle costs (already normalized
// to "cycles per op" by the caller -- see bench_matching_engine.cpp's
// batching, where a batched sample's total cycles are divided by its
// batch size before landing here). Nearest-rank on a sorted copy:
// deterministic, no interpolation choice to justify in review.
struct Percentiles {
    double min = 0;
    double p50 = 0;
    double p99 = 0;
    double p999 = 0;
    double p9999 = 0;
    double max = 0;
    std::size_t count = 0;
};

// Does not mutate `samples`.
[[nodiscard]] Percentiles ComputePercentiles(const std::vector<double>& samples);

}  // namespace lob::bench
