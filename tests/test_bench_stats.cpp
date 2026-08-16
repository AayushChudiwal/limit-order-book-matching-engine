#include <gtest/gtest.h>

#include <vector>

#include "lob/bench/stats.hpp"

using namespace lob::bench;

TEST(Stats, EmptyInputReturnsZeroedResultWithZeroCount) {
    const auto p = ComputePercentiles({});
    EXPECT_EQ(p.count, 0u);
    EXPECT_EQ(p.min, 0);
    EXPECT_EQ(p.max, 0);
}

TEST(Stats, SingleSampleIsEveryPercentile) {
    const auto p = ComputePercentiles({42.0});
    EXPECT_EQ(p.count, 1u);
    EXPECT_DOUBLE_EQ(p.min, 42.0);
    EXPECT_DOUBLE_EQ(p.max, 42.0);
    EXPECT_DOUBLE_EQ(p.p50, 42.0);
    EXPECT_DOUBLE_EQ(p.p99, 42.0);
    EXPECT_DOUBLE_EQ(p.p999, 42.0);
    EXPECT_DOUBLE_EQ(p.p9999, 42.0);
}

TEST(Stats, UnsortedInputDoesNotMutateCallerAndSortsInternally) {
    const std::vector<double> samples = {5.0, 1.0, 4.0, 2.0, 3.0};
    const auto p = ComputePercentiles(samples);
    EXPECT_EQ(samples[0], 5.0);  // caller's vector untouched
    EXPECT_DOUBLE_EQ(p.min, 1.0);
    EXPECT_DOUBLE_EQ(p.max, 5.0);
}

TEST(Stats, NearestRankOnOneToHundred) {
    std::vector<double> samples;
    for (int i = 1; i <= 100; ++i) {
        samples.push_back(static_cast<double>(i));
    }
    const auto p = ComputePercentiles(samples);
    EXPECT_EQ(p.count, 100u);
    EXPECT_DOUBLE_EQ(p.min, 1.0);
    EXPECT_DOUBLE_EQ(p.max, 100.0);
    // nearest-rank: index = floor(fraction * (n-1)), 0-based into sorted data
    EXPECT_DOUBLE_EQ(p.p50, samples[static_cast<std::size_t>(0.50 * 99)]);
    EXPECT_DOUBLE_EQ(p.p99, samples[static_cast<std::size_t>(0.99 * 99)]);
}

TEST(Stats, RareTailOutliersShowInP9999ButNotInLowerPercentiles) {
    // 9990 normal + 10 outliers (0.1%) out of 10000 samples. Nearest-rank
    // indices here are 4999 (p50), 9899 (p99), 9989 (p999) -- all still
    // inside the 9990 normal values (indices 0..9989) -- and 9998 (p9999),
    // which falls inside the outlier block (indices 9990..9999). This is
    // exactly the scenario a P-core/E-core migration tail is expected to
    // produce: invisible to p50/p99/p999, visible only at p9999.
    std::vector<double> samples(9990, 100.0);
    samples.insert(samples.end(), 10, 1'000'000.0);
    const auto p = ComputePercentiles(samples);
    EXPECT_EQ(p.count, 10000u);
    EXPECT_DOUBLE_EQ(p.p50, 100.0);
    EXPECT_DOUBLE_EQ(p.p99, 100.0);
    EXPECT_DOUBLE_EQ(p.p999, 100.0);
    EXPECT_DOUBLE_EQ(p.p9999, 1'000'000.0);
    EXPECT_DOUBLE_EQ(p.max, 1'000'000.0);
}
