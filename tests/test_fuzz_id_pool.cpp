#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "lob/fuzz/id_pool.hpp"

using namespace lob;
using namespace lob::fuzz;

TEST(IdPool, StartsEmpty) {
    IdPool pool;
    EXPECT_TRUE(pool.Empty());
    EXPECT_EQ(pool.Size(), 0u);
}

TEST(IdPool, AddThenContainsAndAt) {
    IdPool pool;
    pool.Add(OrderId{1});
    pool.Add(OrderId{2});
    pool.Add(OrderId{3});

    EXPECT_EQ(pool.Size(), 3u);
    EXPECT_TRUE(pool.Contains(OrderId{1}));
    EXPECT_TRUE(pool.Contains(OrderId{2}));
    EXPECT_TRUE(pool.Contains(OrderId{3}));
    EXPECT_FALSE(pool.Contains(OrderId{4}));
}

TEST(IdPool, RemoveMiddleElementKeepsTheOtherTwoAccessible) {
    IdPool pool;
    pool.Add(OrderId{1});
    pool.Add(OrderId{2});
    pool.Add(OrderId{3});

    pool.Remove(OrderId{2});

    EXPECT_EQ(pool.Size(), 2u);
    EXPECT_FALSE(pool.Contains(OrderId{2}));
    ASSERT_TRUE(pool.Contains(OrderId{1}));
    ASSERT_TRUE(pool.Contains(OrderId{3}));

    // After swap-remove, At(0) and At(1) must be exactly {1, 3} in some
    // order -- no stale/duplicate/missing entries.
    std::vector<std::uint64_t> remaining = {pool.At(0).value, pool.At(1).value};
    std::sort(remaining.begin(), remaining.end());
    EXPECT_EQ(remaining, (std::vector<std::uint64_t>{1, 3}));
}

TEST(IdPool, RemovingAnAbsentIdIsANoOp) {
    IdPool pool;
    pool.Add(OrderId{1});

    pool.Remove(OrderId{999});

    EXPECT_EQ(pool.Size(), 1u);
    EXPECT_TRUE(pool.Contains(OrderId{1}));
}

TEST(IdPool, RemoveTheLastElementLeavesAnEmptyValidPool) {
    IdPool pool;
    pool.Add(OrderId{1});
    pool.Remove(OrderId{1});

    EXPECT_TRUE(pool.Empty());
    EXPECT_FALSE(pool.Contains(OrderId{1}));
}

TEST(IdPool, AddRemoveAddCycleStaysConsistentWithAGroundTruthSet) {
    // Cross-check against std::set (whose iteration IS well-defined and
    // sorted, unlike unordered_set) across a longer add/remove sequence,
    // rather than hand-tracing swap-remove indices by eye.
    IdPool pool;
    std::vector<bool> present(200, false);

    auto reference_members = [&] {
        std::vector<std::uint64_t> v;
        for (std::uint64_t i = 0; i < present.size(); ++i) {
            if (present[i]) {
                v.push_back(i);
            }
        }
        return v;
    };

    auto pool_members = [&] {
        std::vector<std::uint64_t> v;
        for (std::size_t i = 0; i < pool.Size(); ++i) {
            v.push_back(pool.At(i).value);
        }
        std::sort(v.begin(), v.end());
        return v;
    };

    for (std::uint64_t i = 0; i < 200; ++i) {
        if (i % 3 != 0) {
            pool.Add(OrderId{i});
            present[i] = true;
        }
    }
    EXPECT_EQ(pool_members(), reference_members());

    for (std::uint64_t i = 0; i < 200; i += 5) {
        pool.Remove(OrderId{i});
        present[i] = false;
    }
    EXPECT_EQ(pool_members(), reference_members());
    EXPECT_EQ(pool.Size(), reference_members().size());
}
