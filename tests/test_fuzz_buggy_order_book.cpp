#include <gtest/gtest.h>

#include "lob/fuzz/buggy_order_book.hpp"
#include "lob/fuzz/differential.hpp"
#include "lob/fuzz/generator.hpp"
#include "lob/fuzz/generator_profile.hpp"
#include "lob/order_book.hpp"

using namespace lob;
using namespace lob::fuzz;

// BugKind::None must behave identically to OrderBook -- this is the
// baseline that makes the other six tests trustworthy. If BuggyOrderBook's
// mirrored "correct path" itself diverged from the reference for reasons
// unrelated to any of the seven named bugs, a "caught it!" result on one
// of the other six wouldn't prove the harness catches THAT bug -- it
// might just be catching an implementation slip in the mirror.
TEST(BuggyOrderBook, NoneMatchesReferenceEngineAcrossAllThreeProfiles) {
    for (auto profile : {DefaultProfile(), AllAddsProfile(), ReplaceHeavyProfile()}) {
        for (std::uint64_t seed : {1ull, 2ull, 3ull}) {
            Generator gen(seed, profile);
            const auto generated = gen.Generate(3000);
            const auto result =
                RunDifferential<OrderBook, BuggyOrderBookVariant<BugKind::None>>(generated.ops);
            EXPECT_TRUE(result.ok)
                << "profile=" << profile.name << " seed=" << seed
                << (result.divergence ? " desc=" + result.divergence->description : "");
        }
    }
}

// Each of the seven bugs, at a seed/op-count combination confirmed during
// authoring to catch it quickly (see docs/fuzz_mutation_testing.md for the
// full detection-speed table across many seeds) -- these are the
// permanent regression locks: if any of these ever stops failing, the
// differential harness has lost the ability to catch that bug class, and
// this test suite must say so loudly, not silently pass.
template <BugKind Kind>
void ExpectCaught(std::uint64_t seed, int op_count) {
    Generator gen(seed, DefaultProfile());
    const auto generated = gen.Generate(op_count);
    const auto result = RunDifferential<OrderBook, BuggyOrderBookVariant<Kind>>(generated.ops);
    ASSERT_FALSE(result.ok) << "harness failed to catch this bug within " << op_count << " ops";
    ASSERT_TRUE(result.divergence.has_value());
    EXPECT_FALSE(result.divergence->description.empty());
}

TEST(BuggyOrderBook, CancelUnlinksWrongNodeIsCaught) {
    ExpectCaught<BugKind::CancelUnlinksWrongNode>(1, 3000);
}

TEST(BuggyOrderBook, ReplaceKeepsQueuePriorityIsCaught) {
    ExpectCaught<BugKind::ReplaceKeepsQueuePriority>(1, 3000);
}

TEST(BuggyOrderBook, PartialFillLeavesStaleQuantityIsCaught) {
    ExpectCaught<BugKind::PartialFillLeavesStaleQuantity>(1, 3000);
}

TEST(BuggyOrderBook, BestBidCacheNotInvalidatedIsCaught) {
    // This bug went UNCAUGHT the first time this test was written -- not
    // because the injected bug was wrong, but because RunDifferential
    // never compared BestBid()/BestAsk() directly, only FullBook() (which
    // reads the underlying levels, not the separate cache the bug lives
    // in). Fixed in differential.hpp; see that file's comment at the
    // comparison this test depends on.
    ExpectCaught<BugKind::BestBidCacheNotInvalidated>(1, 3000);
}

TEST(BuggyOrderBook, FifoViolatedLifoInsteadIsCaught) {
    ExpectCaught<BugKind::FifoViolatedLifoInstead>(1, 3000);
}

TEST(BuggyOrderBook, OffByOneSkipsLastLevelInSweepIsCaught) {
    ExpectCaught<BugKind::OffByOneSkipsLastLevelInSweep>(1, 3000);
}

TEST(BuggyOrderBook, PriceLevelNotRemovedOnLastCancelIsCaught) {
    ExpectCaught<BugKind::PriceLevelNotRemovedOnLastCancel>(1, 3000);
}
