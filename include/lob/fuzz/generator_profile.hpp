#pragma once

#include <string>

namespace lob::fuzz {

struct GeneratorProfile {
    std::string name;

    // Base op-kind weights (normalized at generation time; needn't sum to
    // 1). These set the BASELINE mix -- actual per-step weights are
    // further adjusted by book-depth targeting (see Generator) so the
    // book self-stabilizes around target_depth_per_side instead of
    // drifting to empty or growing unbounded over a long run, an idea
    // adapted from exchange-core's TestOrdersGenerator.
    double add_limit_weight;
    double add_market_weight;
    double cancel_weight;
    double replace_weight;
    double reduce_quantity_weight;

    // Fraction of AddLimit orders deliberately priced to be immediately
    // marketable (crosses the current best opposite price) rather than
    // resting. The run summary reports the ACHIEVED fill rate this
    // produces, since the realized rate depends on book state, not just
    // this knob.
    double aggressive_fraction;

    // Target number of resting orders PER SIDE the depth-targeting
    // mechanism steers toward.
    int target_depth_per_side;

    // Probability, at each generation step, of deliberately steering
    // toward one of the required pathological cases instead of a normal
    // op (see Generator::GeneratePathological). One shared knob rather
    // than one per case: the requirement is "nontrivial frequency", not
    // fine per-case tuning, and a single knob is one fewer thing to get
    // wrong. Whichever case is chosen falls back to a normal op if its
    // precondition isn't currently satisfiable (e.g. "replace of a
    // partially-filled order" with no partially-filled order live yet).
    double pathological_rate;
};

// Ratios from docs/message_mix_psx_20190730.md: Add 45.162%, Cancel/
// Delete 45.903%, Replace 7.895% -- real NASDAQ PSX, full trading day,
// 2019-07-30. Renormalized over just these three categories (98.960% of
// total messages; Execute/Trade/Admin aren't generator-emittable op
// kinds -- see that doc for the source counts). The 45.162% Add share is
// further split 90/10 between AddLimit and AddMarket: ITCH has no direct
// "market order" wire message (a real market order shows up as the
// aggressor causing OTHER orders' Execute messages, never its own Add),
// so this split is a fuzzer design choice to keep AddMarketOrder's own
// pathological cases (empty-book, multi-level sweep) continuously
// exercised, not a measured ratio. ReduceQuantity similarly isn't part
// of the renormalization -- it's not client order flow the A/D/U mix
// describes -- and gets a small fixed slice on top for the same reason.
inline GeneratorProfile DefaultProfile() {
    return GeneratorProfile{
        .name = "default",
        .add_limit_weight = 0.4107,
        .add_market_weight = 0.0456,
        .cancel_weight = 0.4386,
        .replace_weight = 0.0798,
        .reduce_quantity_weight = 0.025,
        .aggressive_fraction = 0.15,
        .target_depth_per_side = 40,
        .pathological_rate = 0.08,
    };
}

// Almost entirely adds, deliberately: stresses new-price-level insertion,
// deep queues at a single level, and O(log M) tree growth in the
// reference engine's std::map -- the access pattern Phase 2's message-mix
// docs found real venues do NOT primarily exercise, but Phase 5's flat-
// array engine still needs to handle correctly when it does occur (a
// one-sided burst of adds with no cancels is a realistic opening-auction
// pattern even if it isn't the steady-state mix).
inline GeneratorProfile AllAddsProfile() {
    return GeneratorProfile{
        .name = "all_adds",
        .add_limit_weight = 0.92,
        .add_market_weight = 0.03,
        .cancel_weight = 0.0,
        .replace_weight = 0.0,
        .reduce_quantity_weight = 0.05,
        .aggressive_fraction = 0.10,
        // No cancels means depth-targeting has nothing to push back with;
        // a huge target keeps the (irrelevant, since cancel_weight is 0)
        // depth-pressure mechanism from ever engaging.
        .target_depth_per_side = 1'000'000,
        // Most pathological cases involve cancel/replace, so this profile
        // can't exercise most of them regardless of this knob -- kept
        // low rather than wasted.
        .pathological_rate = 0.03,
    };
}

// Stresses the Replace path specifically -- the message type explicitly
// named as where most real ITCH implementations have bugs (see
// OrderReplaceViaItchPattern in tests/test_reduce_and_side_of.cpp).
inline GeneratorProfile ReplaceHeavyProfile() {
    return GeneratorProfile{
        .name = "replace_heavy",
        .add_limit_weight = 0.30,
        .add_market_weight = 0.05,
        .cancel_weight = 0.15,
        .replace_weight = 0.45,
        .reduce_quantity_weight = 0.05,
        .aggressive_fraction = 0.15,
        .target_depth_per_side = 40,
        .pathological_rate = 0.12,
    };
}

}  // namespace lob::fuzz
