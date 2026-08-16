#include "lob/bench/stats.hpp"

#include <algorithm>

namespace lob::bench {

namespace {

double NearestRank(const std::vector<double>& sorted, double fraction) {
    const auto rank = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[rank];
}

}  // namespace

Percentiles ComputePercentiles(const std::vector<double>& samples) {
    Percentiles result;
    result.count = samples.size();
    if (samples.empty()) {
        return result;
    }

    std::vector<double> sorted(samples);
    std::sort(sorted.begin(), sorted.end());

    result.min = sorted.front();
    result.max = sorted.back();
    result.p50 = NearestRank(sorted, 0.50);
    result.p99 = NearestRank(sorted, 0.99);
    result.p999 = NearestRank(sorted, 0.999);
    result.p9999 = NearestRank(sorted, 0.9999);
    return result;
}

}  // namespace lob::bench
