#include <gtest/gtest.h>

#include "lob/fuzz/apply_op.hpp"
#include "lob/order_book.hpp"

using namespace lob;
using namespace lob::fuzz;

namespace {

// The ground truth ApplyOp's returned delta is checked against: actual
// total resting quantity, summed directly from FullBook, independent of
// however ApplyOp computed its answer. Testing the delta formula against
// a formula re-derived in the test risks reproducing the same mistake
// twice; testing it against the engine's own observable state doesn't.
std::int64_t TotalRestingBothSides(const OrderBook& book) {
    std::int64_t total = 0;
    for (const auto& level : book.FullBook(Side::Buy)) {
        for (const auto& order : level.orders) {
            total += order.quantity.units;
        }
    }
    for (const auto& level : book.FullBook(Side::Sell)) {
        for (const auto& order : level.orders) {
            total += order.quantity.units;
        }
    }
    return total;
}

struct Fixture {
    FuzzListener listener;
    OrderBook book{listener};
};

}  // namespace

TEST(ApplyOp, AddLimitThatFullyRestsMatchesObservedDelta) {
    Fixture f;
    FuzzOp op{FuzzOp::Kind::AddLimit, OrderId{1}, {}, Side::Buy, Price{100}, Quantity{50}};

    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after - before, outcome.quantity_delta);
    EXPECT_EQ(outcome.quantity_delta, 50);
}

TEST(ApplyOp, AddLimitThatPartiallyFillsMatchesObservedDelta) {
    Fixture f;
    f.book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{20});

    FuzzOp op{FuzzOp::Kind::AddLimit, OrderId{2}, {}, Side::Buy, Price{100}, Quantity{50}};
    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after - before, outcome.quantity_delta);
    // 20 filled (leaves the book), 30 rests: net -20 + 30 = +10... but the
    // resting SELL is fully consumed (-20) and 30 rests fresh (+30): +10.
    EXPECT_EQ(outcome.quantity_delta, 10);
}

TEST(ApplyOp, AddLimitThatExactlyFillsWithNoRemainderMatchesObservedDelta) {
    Fixture f;
    f.book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{20});

    FuzzOp op{FuzzOp::Kind::AddLimit, OrderId{2}, {}, Side::Buy, Price{100}, Quantity{20}};
    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after - before, outcome.quantity_delta);
    EXPECT_EQ(outcome.quantity_delta, -20);  // resting order consumed, nothing rests
    EXPECT_TRUE(f.book.Empty());
}

TEST(ApplyOp, RejectedAddLimitIsAZeroDelta) {
    Fixture f;
    f.book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{10});

    // Duplicate id -- rejected.
    FuzzOp op{FuzzOp::Kind::AddLimit, OrderId{1}, {}, Side::Buy, Price{100}, Quantity{10}};
    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after, before);
    EXPECT_EQ(outcome.quantity_delta, 0);
}

TEST(ApplyOp, AddMarketPartialSweepMatchesObservedDelta) {
    Fixture f;
    f.book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
    f.book.AddLimitOrder(OrderId{2}, Side::Sell, Price{101}, Quantity{10});

    FuzzOp op{FuzzOp::Kind::AddMarket, OrderId{3}, {}, Side::Buy, Price{0}, Quantity{15}};
    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after - before, outcome.quantity_delta);
    EXPECT_EQ(outcome.quantity_delta, -15);  // 15 filled, market order never rests
}

TEST(ApplyOp, AddMarketIntoEmptyBookIsAZeroDelta) {
    Fixture f;
    FuzzOp op{FuzzOp::Kind::AddMarket, OrderId{1}, {}, Side::Buy, Price{0}, Quantity{100}};

    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after, before);
    EXPECT_EQ(outcome.quantity_delta, 0);
    EXPECT_TRUE(f.listener.fills.empty());
}

TEST(ApplyOp, CancelOfALiveOrderMatchesObservedDelta) {
    Fixture f;
    f.book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{30});

    FuzzOp op{FuzzOp::Kind::Cancel, OrderId{1}, {}, Side::Buy, Price{0}, Quantity{0}};
    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after - before, outcome.quantity_delta);
    EXPECT_EQ(outcome.quantity_delta, -30);
}

TEST(ApplyOp, CancelOfADeadOrUnknownOrderIsAZeroDelta) {
    Fixture f;
    FuzzOp op{FuzzOp::Kind::Cancel, OrderId{404}, {}, Side::Buy, Price{0}, Quantity{0}};

    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after, before);
    EXPECT_EQ(outcome.quantity_delta, 0);
}

TEST(ApplyOp, CleanReplaceMatchesObservedDelta) {
    Fixture f;
    f.book.AddLimitOrder(OrderId{1}, Side::Buy, Price{100}, Quantity{20});

    FuzzOp op{FuzzOp::Kind::Replace, OrderId{2}, OrderId{1}, Side::Buy, Price{100}, Quantity{35}};
    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after - before, outcome.quantity_delta);
    EXPECT_EQ(outcome.quantity_delta, 15);  // -20 (old gone) + 35 (new rests) = +15
    EXPECT_FALSE(f.book.SideOf(OrderId{1}).has_value());
    EXPECT_TRUE(f.book.SideOf(OrderId{2}).has_value());
}

TEST(ApplyOp, ReplaceAcrossTheSpreadBecomesAggressiveAndMatchesObservedDelta) {
    Fixture f;
    f.book.AddLimitOrder(OrderId{1}, Side::Buy, Price{95}, Quantity{20});
    f.book.AddLimitOrder(OrderId{2}, Side::Sell, Price{100}, Quantity{10});

    // Replace order 1 to a price that now crosses the resting ask.
    FuzzOp op{FuzzOp::Kind::Replace, OrderId{3}, OrderId{1}, Side::Buy, Price{101}, Quantity{20}};
    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after - before, outcome.quantity_delta);
    ASSERT_EQ(f.listener.fills.size(), 1u);
    EXPECT_EQ(f.listener.fills[0].quantity, Quantity{10});
}

TEST(ApplyOp, ReplaceOfANonexistentOrderIsAZeroDeltaNoOpNotAnAdd) {
    Fixture f;
    FuzzOp op{FuzzOp::Kind::Replace, OrderId{2}, OrderId{404}, Side::Buy, Price{100}, Quantity{10}};

    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after, before);
    EXPECT_EQ(outcome.quantity_delta, 0);
    // Confirms this is a genuine no-op, not "add anyway": id 2 must not
    // have been created.
    EXPECT_FALSE(f.book.SideOf(OrderId{2}).has_value());
}

TEST(ApplyOp, ReduceQuantitySuccessMatchesObservedDelta) {
    Fixture f;
    f.book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{50});

    FuzzOp op{FuzzOp::Kind::ReduceQuantity, {}, OrderId{1}, Side::Buy, Price{0}, Quantity{15}};
    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after - before, outcome.quantity_delta);
    EXPECT_EQ(outcome.quantity_delta, -15);
}

TEST(ApplyOp, ReduceQuantityLargerThanRestingIsRejectedWithZeroDelta) {
    Fixture f;
    f.book.AddLimitOrder(OrderId{1}, Side::Sell, Price{100}, Quantity{10});

    FuzzOp op{FuzzOp::Kind::ReduceQuantity, {}, OrderId{1}, Side::Buy, Price{0}, Quantity{999}};
    const auto before = TotalRestingBothSides(f.book);
    const auto outcome = ApplyOp(f.book, f.listener, op);
    const auto after = TotalRestingBothSides(f.book);

    EXPECT_EQ(after, before);
    EXPECT_EQ(outcome.quantity_delta, 0);
    ASSERT_EQ(f.listener.rejected.size(), 1u);
    EXPECT_EQ(f.listener.rejected[0].second, RejectReason::InsufficientQuantity);
}
