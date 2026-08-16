#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "lob/fuzz/fuzz_listener.hpp"
#include "lob/fuzz/generator_profile.hpp"
#include "lob/fuzz/id_pool.hpp"
#include "lob/fuzz/op.hpp"
#include "lob/order_book.hpp"

namespace lob::fuzz {

// Counts of how many times each REQUIRED pathological case actually
// FIRED during a generation run -- not how many times it was attempted
// (a case whose precondition was never satisfiable falls back to a
// normal op and doesn't count). "If any is zero, the generator is
// broken -- fail loudly rather than passing silently" is the entire
// point of tracking this separately from just generating a big pile of
// ops and hoping they're representative.
struct CoverageSummary {
    std::uint64_t cancel_of_fully_executed = 0;
    std::uint64_t cancel_of_already_cancelled = 0;
    std::uint64_t replace_same_price = 0;
    std::uint64_t replace_across_spread = 0;
    std::uint64_t replace_of_partially_filled = 0;
    std::uint64_t replace_of_nonexistent = 0;
    std::uint64_t zero_or_negative_quantity = 0;
    std::uint64_t quantity_larger_than_resting = 0;
    std::uint64_t market_into_empty_book = 0;
    std::uint64_t market_sweep_exhausts_side = 0;
    std::uint64_t deep_queue_add = 0;
    std::uint64_t duplicate_order_id = 0;

    [[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> AsList() const {
        return {
            {"cancel_of_fully_executed", cancel_of_fully_executed},
            {"cancel_of_already_cancelled", cancel_of_already_cancelled},
            {"replace_same_price", replace_same_price},
            {"replace_across_spread", replace_across_spread},
            {"replace_of_partially_filled", replace_of_partially_filled},
            {"replace_of_nonexistent", replace_of_nonexistent},
            {"zero_or_negative_quantity", zero_or_negative_quantity},
            {"quantity_larger_than_resting", quantity_larger_than_resting},
            {"market_into_empty_book", market_into_empty_book},
            {"market_sweep_exhausts_side", market_sweep_exhausts_side},
            {"deep_queue_add", deep_queue_add},
            {"duplicate_order_id", duplicate_order_id},
        };
    }

    [[nodiscard]] std::vector<std::string> ZeroCategories() const {
        std::vector<std::string> zero;
        for (const auto& [name, count] : AsList()) {
            if (count == 0) {
                zero.push_back(name);
            }
        }
        return zero;
    }
};

struct GenerationResult {
    std::vector<FuzzOp> ops;
    CoverageSummary coverage;
    std::uint64_t total_fill_events = 0;
    std::int64_t total_filled_quantity = 0;
    std::uint64_t adds_attempted = 0;  // AddLimit + AddMarket count

    // Fraction of AddLimit/AddMarket ops that produced at least one fill
    // -- reported rather than assumed, since the realized rate depends on
    // book state built up during generation, not just the profile's
    // aggressive_fraction knob.
    [[nodiscard]] double AchievedFillRate() const {
        return adds_attempted == 0
                   ? 0.0
                   : static_cast<double>(total_fill_events) / static_cast<double>(adds_attempted);
    }
};

// Generates a seeded, reproducible sequence of FuzzOps. Internally drives
// its own throwaway reference-engine OrderBook as it generates, observing
// real fills/cancels/rejects through FuzzListener to make informed
// choices -- which ids are live vs dead-via-execution vs dead-via-cancel
// vs currently-partially-filled, what the current best bid/ask and
// per-level depths are -- rather than generating blind and hoping the
// sequence happens to be realistic. The generated sequence is then
// replayed by the differential harness against FRESH engine instances;
// generation and comparison are deliberately separate passes.
//
// Uses raw std::mt19937_64 output (rng_() % N), never
// std::uniform_int_distribution/uniform_real_distribution: the standard
// only guarantees an ENGINE's output sequence is identical for a given
// seed across conforming implementations, not that a DISTRIBUTION
// consumes that output identically -- "a seed alone must reproduce a run
// exactly" would be at the mercy of whichever standard library happens
// to be compiling (libc++ here, libstdc++ on CI's ubuntu-latest) if any
// distribution object were used. Same reasoning as IdPool avoiding
// unordered_set.
class Generator {
  public:
    Generator(std::uint64_t seed, GeneratorProfile profile);

    GenerationResult Generate(int op_count);

  private:
    struct LiveOrderInfo {
        Side side;
        Price price;
        Quantity quantity;
    };

    FuzzOp GenerateOneOp();
    FuzzOp GenerateNormalOp();
    FuzzOp GeneratePathologicalOp();

    std::optional<FuzzOp> TryCancelOfFullyExecuted();
    std::optional<FuzzOp> TryCancelOfAlreadyCancelled();
    std::optional<FuzzOp> TryReplaceSamePrice();
    std::optional<FuzzOp> TryReplaceAcrossSpread();
    std::optional<FuzzOp> TryReplaceOfPartiallyFilled();
    std::optional<FuzzOp> TryReplaceOfNonexistent();
    std::optional<FuzzOp> TryZeroOrNegativeQuantity();
    std::optional<FuzzOp> TryQuantityLargerThanResting();
    std::optional<FuzzOp> TryMarketIntoEmptyBook();
    std::optional<FuzzOp> TryMarketSweepExhaustsSide();
    std::optional<FuzzOp> TryDeepQueueAdd();
    std::optional<FuzzOp> TryDuplicateOrderId();

    FuzzOp GenerateCancel();
    FuzzOp GenerateReplace();
    FuzzOp GenerateReduceQuantity();

    [[nodiscard]] std::optional<LiveOrderInfo> FindLive(OrderId id) const;
    [[nodiscard]] std::int64_t TotalQuantity(Side side) const;
    [[nodiscard]] OrderId RandomLiveId();
    [[nodiscard]] Side RandomSide();
    [[nodiscard]] Quantity RandomQuantity();
    [[nodiscard]] Price ClusteredRestingPrice(Side side);
    [[nodiscard]] Price AggressivePrice(Side side);
    [[nodiscard]] std::int64_t ClusteredOffset(std::int64_t max_deviation);
    [[nodiscard]] double Uniform01();
    OrderId NextId();

    void RecordOutcome(const FuzzOp& op, std::size_t fills_before, std::size_t accepted_before,
                       std::size_t cancelled_before, std::size_t rejected_before);

    std::mt19937_64 rng_;
    GeneratorProfile profile_;

    FuzzListener listener_;
    OrderBook book_{listener_};

    std::uint64_t next_id_ = 1;
    std::int64_t mid_price_ = 100'000;

    IdPool live_buy_;
    IdPool live_sell_;
    IdPool dead_via_execution_;
    IdPool dead_via_cancel_;
    IdPool partially_filled_;

    CoverageSummary coverage_;
    std::uint64_t total_fill_events_ = 0;
    std::int64_t total_filled_quantity_ = 0;
    std::uint64_t adds_attempted_ = 0;
};

}  // namespace lob::fuzz
