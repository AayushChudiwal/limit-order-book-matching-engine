#include <gtest/gtest.h>

#include "lob/itch/integrity_checker.hpp"

using lob::itch::OrderReferenceIntegrityChecker;

TEST(IntegrityChecker, CleanAddExecuteCancelDeleteSequenceHasNoViolations) {
    OrderReferenceIntegrityChecker checker;

    checker.OnAdd(1, 100, 0);
    checker.OnExecuted(1, 40, 'E', 100);  // 60 left
    checker.OnCanceled(1, 20, 200);       // 40 left
    checker.OnDeleted(1, 300);            // removed

    EXPECT_TRUE(checker.Violations().empty());
    EXPECT_EQ(checker.LiveOrderCount(), 0u);
}

TEST(IntegrityChecker, ExecutingTheFullRemainingSizeRemovesTheOrder) {
    OrderReferenceIntegrityChecker checker;

    checker.OnAdd(1, 100, 0);
    checker.OnExecuted(1, 100, 'E', 100);

    EXPECT_TRUE(checker.Violations().empty());
    EXPECT_EQ(checker.LiveOrderCount(), 0u);
}

TEST(IntegrityChecker, ExecutedAgainstAnUnknownReferenceIsAViolation) {
    OrderReferenceIntegrityChecker checker;

    checker.OnExecuted(999, 10, 'E', 42);

    ASSERT_EQ(checker.Violations().size(), 1u);
    EXPECT_EQ(checker.Violations()[0].order_ref, 999u);
    EXPECT_EQ(checker.Violations()[0].message_type, 'E');
    EXPECT_EQ(checker.Violations()[0].file_offset, 42u);
}

TEST(IntegrityChecker, CancelingMoreSharesThanRemainIsAViolation) {
    OrderReferenceIntegrityChecker checker;

    checker.OnAdd(1, 100, 0);
    checker.OnCanceled(1, 150, 50);

    ASSERT_EQ(checker.Violations().size(), 1u);
    EXPECT_EQ(checker.Violations()[0].message_type, 'X');
    EXPECT_EQ(checker.Violations()[0].file_offset, 50u);
    // The order must be untouched by the rejected reduction, not partially
    // applied -- still live with its original size.
    EXPECT_EQ(checker.LiveOrderCount(), 1u);
}

TEST(IntegrityChecker, DuplicateAddOfALiveReferenceIsAViolation) {
    OrderReferenceIntegrityChecker checker;

    checker.OnAdd(1, 100, 0);
    checker.OnAdd(1, 50, 10);

    ASSERT_EQ(checker.Violations().size(), 1u);
    EXPECT_EQ(checker.Violations()[0].message_type, 'A');
    EXPECT_EQ(checker.LiveOrderCount(), 1u);
}

TEST(IntegrityChecker, DeletingAnUnknownReferenceIsAViolation) {
    OrderReferenceIntegrityChecker checker;

    checker.OnDeleted(404, 7);

    ASSERT_EQ(checker.Violations().size(), 1u);
    EXPECT_EQ(checker.Violations()[0].message_type, 'D');
    EXPECT_EQ(checker.Violations()[0].order_ref, 404u);
}

TEST(IntegrityChecker, CleanReplaceRemovesTheOriginalAndRegistersTheNewReference) {
    OrderReferenceIntegrityChecker checker;

    checker.OnAdd(1, 100, 0);
    checker.OnReplaced(1, 2, 80, 10);

    EXPECT_TRUE(checker.Violations().empty());
    EXPECT_EQ(checker.LiveOrderCount(), 1u);

    // The original reference no longer exists -- a later message against
    // it must be flagged.
    checker.OnDeleted(1, 20);
    ASSERT_EQ(checker.Violations().size(), 1u);
    EXPECT_EQ(checker.Violations()[0].order_ref, 1u);

    // The new reference is live and usable.
    checker.OnDeleted(2, 30);
    EXPECT_EQ(checker.Violations().size(), 1u);  // no new violation added
    EXPECT_EQ(checker.LiveOrderCount(), 0u);
}

TEST(IntegrityChecker, ReplacingAnUnknownOriginalReferenceIsAViolationButStillRegistersTheNew) {
    OrderReferenceIntegrityChecker checker;

    checker.OnReplaced(999, 2, 80, 10);

    ASSERT_EQ(checker.Violations().size(), 1u);
    EXPECT_EQ(checker.Violations()[0].order_ref, 999u);
    // The new reference should still be tracked -- a single bad Replace
    // shouldn't cause a cascade of spurious violations for every message
    // that legitimately references the new id afterward.
    EXPECT_EQ(checker.LiveOrderCount(), 1u);
    checker.OnDeleted(2, 20);
    EXPECT_EQ(checker.Violations().size(), 1u);
}

TEST(IntegrityChecker, ReplacingOntoAnAlreadyLiveNewReferenceIsAViolation) {
    OrderReferenceIntegrityChecker checker;

    checker.OnAdd(1, 100, 0);
    checker.OnAdd(2, 50, 10);  // reference 2 already live independently

    checker.OnReplaced(1, 2, 80, 20);

    ASSERT_EQ(checker.Violations().size(), 1u);
    EXPECT_EQ(checker.Violations()[0].order_ref, 2u);
    EXPECT_EQ(checker.Violations()[0].message_type, 'U');
}

TEST(IntegrityChecker, TracksLiveOrderCountAcrossManyIndependentReferences) {
    OrderReferenceIntegrityChecker checker;

    checker.OnAdd(1, 100, 0);
    checker.OnAdd(2, 200, 1);
    checker.OnAdd(3, 300, 2);
    EXPECT_EQ(checker.LiveOrderCount(), 3u);

    checker.OnDeleted(2, 3);
    EXPECT_EQ(checker.LiveOrderCount(), 2u);

    checker.OnExecuted(1, 100, 'E', 4);  // fully executed -- removed
    EXPECT_EQ(checker.LiveOrderCount(), 1u);

    EXPECT_TRUE(checker.Violations().empty());
}
