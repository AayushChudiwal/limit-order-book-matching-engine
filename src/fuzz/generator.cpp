#include "lob/fuzz/generator.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "lob/fuzz/apply_op.hpp"

namespace lob::fuzz {

Generator::Generator(std::uint64_t seed, GeneratorProfile profile)
    : rng_(seed), profile_(std::move(profile)) {}

double Generator::Uniform01() {
    // See the class doc comment: raw engine output, never
    // uniform_real_distribution, for cross-platform reproducibility.
    return static_cast<double>(rng_() % 1'000'000ULL) / 1'000'000.0;
}

OrderId Generator::NextId() { return OrderId{next_id_++}; }

Side Generator::RandomSide() { return (rng_() % 2 == 0) ? Side::Buy : Side::Sell; }

Quantity Generator::RandomQuantity() {
    // Mostly small, occasionally a large burst -- stresses sweeps and
    // deep levels without every order being enormous.
    if (rng_() % 20 == 0) {
        return Quantity{500 + static_cast<std::int64_t>(rng_() % 4500)};
    }
    return Quantity{1 + static_cast<std::int64_t>(rng_() % 500)};
}

std::int64_t Generator::ClusteredOffset(std::int64_t max_deviation) {
    // Sum of a few small uniform draws, scaled by a squared-uniform per-
    // call deviation, approximates a cluster near zero without a real
    // Gaussian sampler -- the same idea exchange-core's
    // TestOrdersGenerator uses for resting-order price offsets, adapted
    // (not copied) here.
    const double dev_fraction = Uniform01();
    const auto dev = 1 + static_cast<std::int64_t>(dev_fraction * dev_fraction *
                                                   static_cast<double>(max_deviation));
    std::int64_t sum = 0;
    constexpr int kSamples = 4;
    for (int i = 0; i < kSamples; ++i) {
        sum += static_cast<std::int64_t>(rng_() % static_cast<std::uint64_t>(dev + 1));
    }
    return sum / kSamples * 2 - dev;
}

Price Generator::ClusteredRestingPrice(Side side) {
    const std::int64_t magnitude = 1 + std::abs(ClusteredOffset(50));
    return Price{side == Side::Buy ? mid_price_ - magnitude : mid_price_ + magnitude};
}

Price Generator::AggressivePrice(Side side) {
    const auto opposite_best = side == Side::Buy ? book_.BestAsk() : book_.BestBid();
    if (!opposite_best.has_value()) {
        return ClusteredRestingPrice(side);  // nothing to cross -- fall back to resting
    }
    const std::int64_t offset = std::abs(ClusteredOffset(10));
    return Price{side == Side::Buy ? opposite_best->ticks + offset : opposite_best->ticks - offset};
}

std::optional<Generator::LiveOrderInfo> Generator::FindLive(OrderId id) const {
    for (const Side side : {Side::Buy, Side::Sell}) {
        for (const auto& level : book_.FullBook(side)) {
            for (const auto& order : level.orders) {
                if (order.id == id) {
                    return LiveOrderInfo{side, level.price, order.quantity};
                }
            }
        }
    }
    return std::nullopt;
}

std::int64_t Generator::TotalQuantity(Side side) const {
    std::int64_t total = 0;
    for (const auto& level : book_.FullBook(side)) {
        for (const auto& order : level.orders) {
            total += order.quantity.units;
        }
    }
    return total;
}

OrderId Generator::RandomLiveId() {
    const std::size_t buy_size = live_buy_.Size();
    const std::size_t total = buy_size + live_sell_.Size();
    const std::size_t idx = static_cast<std::size_t>(rng_() % total);
    return idx < buy_size ? live_buy_.At(idx) : live_sell_.At(idx - buy_size);
}

FuzzOp Generator::GenerateCancel() {
    return FuzzOp{FuzzOp::Kind::Cancel, RandomLiveId(), {}, Side::Buy, Price{0}, Quantity{0}};
}

FuzzOp Generator::GenerateReplace() {
    const OrderId target = RandomLiveId();
    const auto info = FindLive(target);
    const bool aggressive = Uniform01() < profile_.aggressive_fraction;
    const Price new_price =
        aggressive ? AggressivePrice(info->side) : ClusteredRestingPrice(info->side);
    return FuzzOp{FuzzOp::Kind::Replace, NextId(), target, info->side, new_price, RandomQuantity()};
}

FuzzOp Generator::GenerateReduceQuantity() {
    const OrderId target = RandomLiveId();
    const auto info = FindLive(target);
    const std::int64_t current = info->quantity.units;
    const auto amount = 1 + static_cast<std::int64_t>(rng_() % static_cast<std::uint64_t>(current));
    return FuzzOp{FuzzOp::Kind::ReduceQuantity, {}, target, Side::Buy, Price{0}, Quantity{amount}};
}

FuzzOp Generator::GenerateNormalOp() {
    const double live_total = static_cast<double>(live_buy_.Size() + live_sell_.Size());
    const double target_total = static_cast<double>(profile_.target_depth_per_side) * 2.0;
    const double pressure =
        std::clamp(target_total > 0 ? live_total / target_total : 1.0, 0.2, 3.0);

    double w_add_limit = profile_.add_limit_weight * std::clamp(2.0 - pressure, 0.3, 2.0);
    const double w_add_market = profile_.add_market_weight;
    double w_cancel = profile_.cancel_weight * std::clamp(pressure, 0.3, 2.0);
    double w_replace = profile_.replace_weight;
    double w_reduce = profile_.reduce_quantity_weight;

    // Nothing live to Cancel/Replace/Reduce against -- force an Add
    // rather than picking an op kind with no valid target.
    if (live_buy_.Empty() && live_sell_.Empty()) {
        w_cancel = 0.0;
        w_replace = 0.0;
        w_reduce = 0.0;
        w_add_limit = std::max(w_add_limit, 0.01);
    }

    const double total = w_add_limit + w_add_market + w_cancel + w_replace + w_reduce;
    double pick = Uniform01() * total;

    if ((pick -= w_add_limit) < 0) {
        const Side side = RandomSide();
        const bool aggressive = Uniform01() < profile_.aggressive_fraction;
        return FuzzOp{FuzzOp::Kind::AddLimit,
                      NextId(),
                      {},
                      side,
                      aggressive ? AggressivePrice(side) : ClusteredRestingPrice(side),
                      RandomQuantity()};
    }
    if ((pick -= w_add_market) < 0) {
        return FuzzOp{FuzzOp::Kind::AddMarket, NextId(), {}, RandomSide(), Price{0},
                      RandomQuantity()};
    }
    if ((pick -= w_cancel) < 0) {
        return GenerateCancel();
    }
    if ((pick -= w_replace) < 0) {
        return GenerateReplace();
    }
    return GenerateReduceQuantity();
}

std::optional<FuzzOp> Generator::TryCancelOfFullyExecuted() {
    if (dead_via_execution_.Empty()) {
        return std::nullopt;
    }
    const OrderId id = dead_via_execution_.At(rng_() % dead_via_execution_.Size());
    ++coverage_.cancel_of_fully_executed;
    return FuzzOp{FuzzOp::Kind::Cancel, id, {}, Side::Buy, Price{0}, Quantity{0}};
}

std::optional<FuzzOp> Generator::TryCancelOfAlreadyCancelled() {
    if (dead_via_cancel_.Empty()) {
        return std::nullopt;
    }
    const OrderId id = dead_via_cancel_.At(rng_() % dead_via_cancel_.Size());
    ++coverage_.cancel_of_already_cancelled;
    return FuzzOp{FuzzOp::Kind::Cancel, id, {}, Side::Buy, Price{0}, Quantity{0}};
}

std::optional<FuzzOp> Generator::TryReplaceSamePrice() {
    if (live_buy_.Empty() && live_sell_.Empty()) {
        return std::nullopt;
    }
    const OrderId target = RandomLiveId();
    const auto info = FindLive(target);
    if (!info) {
        return std::nullopt;
    }
    // Biased toward a smaller quantity: same price + smaller size is the
    // case that must still lose priority via Replace (contrast Phase 1's
    // ModifyOrder, which would keep it) -- see
    // OrderReplaceViaItchPattern.
    const auto new_qty = std::max<std::int64_t>(
        1, info->quantity.units - static_cast<std::int64_t>(
                                      rng_() % static_cast<std::uint64_t>(info->quantity.units)));
    ++coverage_.replace_same_price;
    return FuzzOp{FuzzOp::Kind::Replace, NextId(),         target, info->side,
                  info->price,           Quantity{new_qty}};
}

std::optional<FuzzOp> Generator::TryReplaceAcrossSpread() {
    if (live_buy_.Empty() && live_sell_.Empty()) {
        return std::nullopt;
    }
    const OrderId target = RandomLiveId();
    const auto info = FindLive(target);
    if (!info) {
        return std::nullopt;
    }
    const auto opposite_best = info->side == Side::Buy ? book_.BestAsk() : book_.BestBid();
    if (!opposite_best.has_value()) {
        return std::nullopt;  // nothing on the other side to cross
    }
    const std::int64_t offset = std::abs(ClusteredOffset(10));
    const Price new_price{info->side == Side::Buy ? opposite_best->ticks + offset
                                                  : opposite_best->ticks - offset};
    ++coverage_.replace_across_spread;
    return FuzzOp{FuzzOp::Kind::Replace, NextId(), target, info->side, new_price, RandomQuantity()};
}

std::optional<FuzzOp> Generator::TryReplaceOfPartiallyFilled() {
    if (partially_filled_.Empty()) {
        return std::nullopt;
    }
    const OrderId target = partially_filled_.At(rng_() % partially_filled_.Size());
    const auto info = FindLive(target);
    if (!info) {
        return std::nullopt;
    }
    ++coverage_.replace_of_partially_filled;
    const bool aggressive = Uniform01() < profile_.aggressive_fraction;
    const Price new_price =
        aggressive ? AggressivePrice(info->side) : ClusteredRestingPrice(info->side);
    return FuzzOp{FuzzOp::Kind::Replace, NextId(), target, info->side, new_price, RandomQuantity()};
}

std::optional<FuzzOp> Generator::TryReplaceOfNonexistent() {
    OrderId target{0};
    if (!dead_via_execution_.Empty() && (rng_() % 2 == 0)) {
        target = dead_via_execution_.At(rng_() % dead_via_execution_.Size());
    } else if (!dead_via_cancel_.Empty()) {
        target = dead_via_cancel_.At(rng_() % dead_via_cancel_.Size());
    } else if (!dead_via_execution_.Empty()) {
        target = dead_via_execution_.At(rng_() % dead_via_execution_.Size());
    } else {
        // Nothing has died yet (early in a run) -- fabricate an id far
        // outside the range NextId() will ever issue, so this case still
        // fires even before anything has died.
        target = OrderId{next_id_ + 1'000'000'000ULL};
    }
    ++coverage_.replace_of_nonexistent;
    return FuzzOp{
        FuzzOp::Kind::Replace, NextId(), target, Side::Buy, ClusteredRestingPrice(Side::Buy),
        RandomQuantity()};
}

std::optional<FuzzOp> Generator::TryZeroOrNegativeQuantity() {
    const Quantity bad_qty{(rng_() % 2 == 0) ? 0 : -(static_cast<std::int64_t>(rng_() % 10) + 1)};
    ++coverage_.zero_or_negative_quantity;
    if (!(live_buy_.Empty() && live_sell_.Empty()) && rng_() % 2 == 0) {
        return FuzzOp{
            FuzzOp::Kind::ReduceQuantity, {}, RandomLiveId(), Side::Buy, Price{0}, bad_qty};
    }
    return FuzzOp{FuzzOp::Kind::AddLimit,           NextId(), {}, RandomSide(),
                  ClusteredRestingPrice(Side::Buy), bad_qty};
}

std::optional<FuzzOp> Generator::TryQuantityLargerThanResting() {
    if (live_buy_.Empty() && live_sell_.Empty()) {
        return std::nullopt;
    }
    const OrderId target = RandomLiveId();
    const auto info = FindLive(target);
    if (!info) {
        return std::nullopt;
    }
    ++coverage_.quantity_larger_than_resting;
    const Quantity too_much{info->quantity.units + 1 + static_cast<std::int64_t>(rng_() % 1000)};
    return FuzzOp{FuzzOp::Kind::ReduceQuantity, {}, target, Side::Buy, Price{0}, too_much};
}

std::optional<FuzzOp> Generator::TryMarketIntoEmptyBook() {
    const bool buy_side_starves = live_sell_.Empty();  // a Buy market order needs resting asks
    const bool sell_side_starves = live_buy_.Empty();
    if (!buy_side_starves && !sell_side_starves) {
        return std::nullopt;
    }
    const Side side = buy_side_starves ? Side::Buy : Side::Sell;
    ++coverage_.market_into_empty_book;
    return FuzzOp{FuzzOp::Kind::AddMarket, NextId(), {}, side, Price{0}, RandomQuantity()};
}

std::optional<FuzzOp> Generator::TryMarketSweepExhaustsSide() {
    const std::int64_t buy_total = TotalQuantity(Side::Buy);
    const std::int64_t sell_total = TotalQuantity(Side::Sell);

    Side target_side;
    std::int64_t target_total;
    if (buy_total > 0 && (sell_total == 0 || buy_total <= sell_total)) {
        target_side = Side::Buy;
        target_total = buy_total;
    } else if (sell_total > 0) {
        target_side = Side::Sell;
        target_total = sell_total;
    } else {
        return std::nullopt;  // both sides empty -- that's the empty-book case, not this one
    }

    const Side aggressor_side = target_side == Side::Buy ? Side::Sell : Side::Buy;
    ++coverage_.market_sweep_exhausts_side;
    return FuzzOp{FuzzOp::Kind::AddMarket,
                  NextId(),
                  {},
                  aggressor_side,
                  Price{0},
                  Quantity{target_total + 1 + static_cast<std::int64_t>(rng_() % 100)}};
}

std::optional<FuzzOp> Generator::TryDeepQueueAdd() {
    constexpr std::size_t kDeepThreshold = 5;
    for (const Side side : {Side::Buy, Side::Sell}) {
        for (const auto& level : book_.FullBook(side)) {
            if (level.orders.size() >= kDeepThreshold) {
                ++coverage_.deep_queue_add;
                return FuzzOp{FuzzOp::Kind::AddLimit, NextId(), {}, side, level.price,
                              RandomQuantity()};
            }
        }
    }
    return std::nullopt;
}

std::optional<FuzzOp> Generator::TryDuplicateOrderId() {
    if (live_buy_.Empty() && live_sell_.Empty()) {
        return std::nullopt;
    }
    const OrderId existing = RandomLiveId();
    ++coverage_.duplicate_order_id;
    const Side side = RandomSide();
    return FuzzOp{FuzzOp::Kind::AddLimit,      existing,        {}, side,
                  ClusteredRestingPrice(side), RandomQuantity()};
}

FuzzOp Generator::GeneratePathologicalOp() {
    using Try = std::optional<FuzzOp> (Generator::*)();
    static constexpr std::array<Try, 12> kTries = {
        &Generator::TryCancelOfFullyExecuted,
        &Generator::TryCancelOfAlreadyCancelled,
        &Generator::TryReplaceSamePrice,
        &Generator::TryReplaceAcrossSpread,
        &Generator::TryReplaceOfPartiallyFilled,
        &Generator::TryReplaceOfNonexistent,
        &Generator::TryZeroOrNegativeQuantity,
        &Generator::TryQuantityLargerThanResting,
        &Generator::TryMarketIntoEmptyBook,
        &Generator::TryMarketSweepExhaustsSide,
        &Generator::TryDeepQueueAdd,
        &Generator::TryDuplicateOrderId,
    };
    const auto chosen = kTries[rng_() % kTries.size()];
    if (auto op = (this->*chosen)()) {
        return *op;
    }
    return GenerateNormalOp();
}

FuzzOp Generator::GenerateOneOp() {
    if (Uniform01() < profile_.pathological_rate) {
        return GeneratePathologicalOp();
    }
    return GenerateNormalOp();
}

void Generator::RecordOutcome(const FuzzOp& op, std::size_t fills_before,
                              std::size_t accepted_before, std::size_t cancelled_before,
                              std::size_t rejected_before) {
    for (std::size_t i = fills_before; i < listener_.fills.size(); ++i) {
        const auto& fill = listener_.fills[i];
        ++total_fill_events_;
        total_filled_quantity_ += fill.quantity.units;
        mid_price_ += (fill.price.ticks - mid_price_) / 20;
        mid_price_ = std::max<std::int64_t>(mid_price_, 1000);

        const OrderId maker = fill.resting_id;
        const bool still_live = book_.SideOf(maker).has_value();
        if (still_live) {
            if (!partially_filled_.Contains(maker)) {
                partially_filled_.Add(maker);
            }
        } else {
            live_buy_.Remove(maker);
            live_sell_.Remove(maker);
            partially_filled_.Remove(maker);
            if (!dead_via_execution_.Contains(maker)) {
                dead_via_execution_.Add(maker);
            }
        }
    }

    for (std::size_t i = accepted_before; i < listener_.accepted.size(); ++i) {
        const OrderId id = listener_.accepted[i];
        const auto side = book_.SideOf(id);
        if (side.has_value()) {
            (*side == Side::Buy ? live_buy_ : live_sell_).Add(id);
        }
    }

    for (std::size_t i = cancelled_before; i < listener_.cancelled.size(); ++i) {
        const OrderId id = listener_.cancelled[i].first;
        live_buy_.Remove(id);
        live_sell_.Remove(id);
        partially_filled_.Remove(id);
        if (!dead_via_cancel_.Contains(id)) {
            dead_via_cancel_.Add(id);
        }
    }

    // ReduceQuantity has no per-order listener callback of its own (only
    // OnBookUpdate, a level aggregate) -- infer what happened from
    // pre/post liveness and whether it was rejected.
    if (op.kind == FuzzOp::Kind::ReduceQuantity) {
        const bool rejected_now = listener_.rejected.size() > rejected_before;
        if (!rejected_now && !book_.SideOf(op.replace_target).has_value()) {
            live_buy_.Remove(op.replace_target);
            live_sell_.Remove(op.replace_target);
            partially_filled_.Remove(op.replace_target);
            if (!dead_via_cancel_.Contains(op.replace_target)) {
                dead_via_cancel_.Add(op.replace_target);
            }
        }
    }
}

GenerationResult Generator::Generate(int op_count) {
    GenerationResult result;
    result.ops.reserve(static_cast<std::size_t>(op_count));

    for (int i = 0; i < op_count; ++i) {
        const FuzzOp op = GenerateOneOp();

        const std::size_t fills_before = listener_.fills.size();
        const std::size_t accepted_before = listener_.accepted.size();
        const std::size_t cancelled_before = listener_.cancelled.size();
        const std::size_t rejected_before = listener_.rejected.size();

        ApplyOp(book_, listener_, op);
        RecordOutcome(op, fills_before, accepted_before, cancelled_before, rejected_before);

        if (op.kind == FuzzOp::Kind::AddLimit || op.kind == FuzzOp::Kind::AddMarket) {
            ++adds_attempted_;
        }

        result.ops.push_back(op);
    }

    result.coverage = coverage_;
    result.total_fill_events = total_fill_events_;
    result.total_filled_quantity = total_filled_quantity_;
    result.adds_attempted = adds_attempted_;
    return result;
}

}  // namespace lob::fuzz
