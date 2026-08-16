#include <gtest/gtest.h>

#include <algorithm>

#include "lob/fuzz/buggy_order_book.hpp"
#include "lob/fuzz/differential.hpp"
#include "lob/fuzz/generator.hpp"
#include "lob/fuzz/generator_profile.hpp"
#include "lob/fuzz/shrinker.hpp"
#include "lob/order_book.hpp"

using namespace lob;
using namespace lob::fuzz;

namespace {

std::vector<FuzzOp> MakeMarkerOps(int total, int marker_stride) {
    std::vector<FuzzOp> ops;
    ops.reserve(static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i) {
        const bool is_marker = (i % marker_stride == 0);
        ops.push_back(FuzzOp{FuzzOp::Kind::AddLimit,
                             OrderId{static_cast<std::uint64_t>(i + 1)},
                             {},
                             Side::Buy,
                             Price{100},
                             Quantity{is_marker ? 999 : 1}});
    }
    return ops;
}

std::size_t CountMarkers(std::span<const FuzzOp> ops) {
    return static_cast<std::size_t>(std::count_if(
        ops.begin(), ops.end(), [](const FuzzOp& op) { return op.quantity.units == 999; }));
}

}  // namespace

// Pure mechanics, decoupled from the matching engine entirely: a
// synthetic predicate ("at least 3 marker ops present") with a known,
// checkable answer. This tests ddmin's own correctness -- does it
// actually reduce, does the result still satisfy the predicate --
// independently of whether it also works well on real engine bugs
// (the tests below).
TEST(Shrink, ReducesASyntheticFailureToNearItsKnownMinimum) {
    const auto original = MakeMarkerOps(100, 10);  // 10 marker ops among 100
    ASSERT_GE(CountMarkers(original), 3u);

    const auto still_fails = [](std::span<const FuzzOp> ops) { return CountMarkers(ops) >= 3; };
    ASSERT_TRUE(still_fails(original));

    const auto shrunk = Shrink(original, still_fails);

    EXPECT_TRUE(still_fails(shrunk)) << "shrinker returned a result that no longer fails";
    EXPECT_GE(CountMarkers(shrunk), 3u);
    // Not necessarily the global minimum (ddmin is 1-minimal, not
    // globally minimal), but should land very close to it for a problem
    // this cleanly separable.
    EXPECT_LE(shrunk.size(), 6u) << "expected close to the 3 markers needed, got " << shrunk.size();
    EXPECT_LT(shrunk.size(), original.size());
}

TEST(Shrink, ReturnsAnAlready1MinimalSequenceUnchanged) {
    std::vector<FuzzOp> ops = {
        FuzzOp{FuzzOp::Kind::AddLimit, OrderId{1}, {}, Side::Buy, Price{100}, Quantity{999}},
        FuzzOp{FuzzOp::Kind::AddLimit, OrderId{2}, {}, Side::Buy, Price{100}, Quantity{999}},
        FuzzOp{FuzzOp::Kind::AddLimit, OrderId{3}, {}, Side::Buy, Price{100}, Quantity{999}},
    };
    const auto still_fails = [](std::span<const FuzzOp> o) { return CountMarkers(o) >= 3; };

    const auto shrunk = Shrink(ops, still_fails);

    EXPECT_EQ(shrunk.size(), 3u);
}

namespace {

template <BugKind Kind>
std::vector<FuzzOp> ShrinkFailureFor(std::uint64_t seed, int op_count) {
    Generator gen(seed, DefaultProfile());
    const auto generated = gen.Generate(op_count);

    const StillFailsPredicate still_fails = [](std::span<const FuzzOp> ops) {
        return !RunDifferential<OrderBook, BuggyOrderBookVariant<Kind>>(ops).ok;
    };

    return Shrink(generated.ops, still_fails);
}

}  // namespace

// The shrinker is only as trustworthy as its demonstrated ability to
// reduce a REAL failure -- not a synthetic one, and not just "it compiles
// and returns something". Each of these takes a large (20000-op) real
// generated failure and confirms the shrunk result is (a) dramatically
// smaller and (b) still genuinely reproduces the bug when re-run
// independently, not merely assumed correct because Shrink() only ever
// keeps complements that already passed still_fails().
TEST(Shrink, ActuallyReducesARealCancelUnlinksWrongNodeFailure) {
    const auto shrunk = ShrinkFailureFor<BugKind::CancelUnlinksWrongNode>(1, 20000);

    ASSERT_FALSE(shrunk.empty());
    EXPECT_LT(shrunk.size(), 200u) << "shrunk to " << shrunk.size() << " ops from 20000";
    const auto result =
        RunDifferential<OrderBook, BuggyOrderBookVariant<BugKind::CancelUnlinksWrongNode>>(shrunk);
    EXPECT_FALSE(result.ok) << "shrunk repro no longer reproduces the failure";
}

TEST(Shrink, ActuallyReducesARealFifoViolatedLifoInsteadFailure) {
    const auto shrunk = ShrinkFailureFor<BugKind::FifoViolatedLifoInstead>(1, 20000);

    ASSERT_FALSE(shrunk.empty());
    EXPECT_LT(shrunk.size(), 200u) << "shrunk to " << shrunk.size() << " ops from 20000";
    const auto result =
        RunDifferential<OrderBook, BuggyOrderBookVariant<BugKind::FifoViolatedLifoInstead>>(shrunk);
    EXPECT_FALSE(result.ok) << "shrunk repro no longer reproduces the failure";
}

TEST(Shrink, ActuallyReducesARealBestBidCacheNotInvalidatedFailure) {
    // The trickiest of the seven to shrink to something meaningful: the
    // bug only manifests on a cancel that empties the best price level,
    // which itself depends on prior book state built up by earlier ops --
    // a naive shrink could plausibly get stuck needing most of the
    // sequence just to construct that precondition.
    const auto shrunk = ShrinkFailureFor<BugKind::BestBidCacheNotInvalidated>(1, 20000);

    ASSERT_FALSE(shrunk.empty());
    EXPECT_LT(shrunk.size(), 1000u) << "shrunk to " << shrunk.size() << " ops from 20000";
    const auto result =
        RunDifferential<OrderBook, BuggyOrderBookVariant<BugKind::BestBidCacheNotInvalidated>>(
            shrunk);
    EXPECT_FALSE(result.ok) << "shrunk repro no longer reproduces the failure";
}
