#include <gtest/gtest.h>

#include "lob/fuzz/apply_op.hpp"
#include "lob/fuzz/generator.hpp"
#include "lob/fuzz/generator_profile.hpp"
#include "lob/fuzz/invariants.hpp"
#include "lob/order_book.hpp"

using namespace lob;
using namespace lob::fuzz;

namespace {

bool OpsEqual(const FuzzOp& a, const FuzzOp& b) {
    return a.kind == b.kind && a.order_id == b.order_id && a.replace_target == b.replace_target &&
           a.side == b.side && a.price == b.price && a.quantity == b.quantity;
}

}  // namespace

TEST(Generator, SameSeedProducesIdenticalOpsEveryField) {
    Generator gen_a(12345, DefaultProfile());
    Generator gen_b(12345, DefaultProfile());

    const auto result_a = gen_a.Generate(2000);
    const auto result_b = gen_b.Generate(2000);

    ASSERT_EQ(result_a.ops.size(), result_b.ops.size());
    for (std::size_t i = 0; i < result_a.ops.size(); ++i) {
        EXPECT_TRUE(OpsEqual(result_a.ops[i], result_b.ops[i])) << "diverged at op index " << i;
    }
}

TEST(Generator, DifferentSeedsProduceDifferentOps) {
    Generator gen_a(1, DefaultProfile());
    Generator gen_b(2, DefaultProfile());

    const auto result_a = gen_a.Generate(500);
    const auto result_b = gen_b.Generate(500);

    int differences = 0;
    for (std::size_t i = 0; i < result_a.ops.size(); ++i) {
        if (!OpsEqual(result_a.ops[i], result_b.ops[i])) {
            ++differences;
        }
    }
    EXPECT_GT(differences, 0);
}

// The generator's whole purpose in Phase 3 is exercising these specific
// pathological cases at nontrivial frequency. If any of the 12 required
// categories never fires, the generator has a coverage gap that would
// make the differential harness silently blind to whatever bug class
// that case exists to catch -- this must fail loudly, not pass with a
// shrug, exactly per the brief.
TEST(Generator, DefaultProfileFiresEveryRequiredPathologicalCaseAtNontrivialFrequency) {
    // 60000, not a rounder/smaller number: the rarest category
    // (market_into_empty_book -- the book stabilizes around
    // target_depth_per_side quickly, so a fully-empty side is a brief
    // window) measured 4-15 occurrences per run at 20000-60000 ops across
    // several seeds during authoring. 20000 put this test right at its
    // own pass/fail threshold; 60000 gives every category, including the
    // rarest, a comfortable multi-times-over margin instead of a
    // borderline assertion that could flip on unrelated changes.
    Generator gen(777, DefaultProfile());
    const auto result = gen.Generate(60000);

    const auto zero = result.coverage.ZeroCategories();
    EXPECT_TRUE(zero.empty()) << [&] {
        std::string msg = "generator failed to exercise: ";
        for (const auto& name : zero) {
            msg += name + " ";
        }
        return msg;
    }();

    // "Nontrivial frequency" -- not just >0, comfortably above it, so a
    // regression that makes a case rare-but-technically-nonzero still
    // gets caught.
    constexpr std::uint64_t kMinFires = 10;
    for (const auto& [name, count] : result.coverage.AsList()) {
        EXPECT_GE(count, kMinFires) << name << " fired only " << count << " times in 60000 ops";
    }
}

TEST(Generator, AllAddsProfileIsOverwhelminglyAddsButStillFallsBackForOtherKinds) {
    Generator gen(42, AllAddsProfile());
    const auto result = gen.Generate(5000);

    std::size_t add_count = 0;
    for (const auto& op : result.ops) {
        if (op.kind == FuzzOp::Kind::AddLimit || op.kind == FuzzOp::Kind::AddMarket) {
            ++add_count;
        }
    }
    // Overwhelmingly adds, but not necessarily 100%: pathological
    // injection (Cancel of a dead order, etc.) can still fire at its own
    // low rate independent of the profile's cancel_weight=0.
    EXPECT_GT(static_cast<double>(add_count) / static_cast<double>(result.ops.size()), 0.85);
}

TEST(Generator, ReplaceHeavyProfileProducesASubstantialReplaceFraction) {
    Generator gen(99, ReplaceHeavyProfile());
    const auto result = gen.Generate(5000);

    std::size_t replace_count = 0;
    for (const auto& op : result.ops) {
        if (op.kind == FuzzOp::Kind::Replace) {
            ++replace_count;
        }
    }
    EXPECT_GT(static_cast<double>(replace_count) / static_cast<double>(result.ops.size()), 0.25);
}

// "Orders must actually cross sometimes... state the achieved fill rate."
TEST(Generator, DefaultProfileAchievesANonTrivialFillRate) {
    Generator gen(2024, DefaultProfile());
    const auto result = gen.Generate(20000);

    EXPECT_GT(result.total_fill_events, 0u);
    EXPECT_GT(result.AchievedFillRate(), 0.02);  // at least ~2% of adds produce a fill
}

// Integration smoke test: a generated sequence, replayed through a FRESH
// engine (independent of the generator's own internal bookkeeping
// engine), must never violate a single-engine invariant at any point.
// This exercises the whole generator -> ApplyOp -> CheckInvariants
// pipeline together, not each piece in isolation.
TEST(Generator, GeneratedSequenceReplayedFreshNeverViolatesInvariants) {
    Generator gen(555, DefaultProfile());
    const auto result = gen.Generate(5000);

    FuzzListener listener;
    OrderBook book(listener);
    std::int64_t expected_total = 0;

    for (std::size_t i = 0; i < result.ops.size(); ++i) {
        const auto outcome = ApplyOp(book, listener, result.ops[i]);
        expected_total += outcome.quantity_delta;
        const auto violations = CheckInvariants(book, expected_total);
        ASSERT_TRUE(violations.empty())
            << "violation at op index " << i << ": " << (violations.empty() ? "" : violations[0]);
    }
}
