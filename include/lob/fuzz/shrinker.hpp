#pragma once

#include <functional>
#include <span>
#include <vector>

#include "lob/fuzz/op.hpp"

namespace lob::fuzz {

// A failing op sequence can reproduce with a single, minimal repro
// worth reading -- or it can be a 10,000-op needle in a haystack. This
// finds the former from the latter.
using StillFailsPredicate = std::function<bool(std::span<const FuzzOp>)>;

// Delta-debugging (ddmin) minimization, following the standard algorithm
// precisely (see e.g. debuggingbook.org's DeltaDebugger): given an op
// sequence `ops` for which `still_fails(ops)` is true, repeatedly tries
// removing chunks of shrinking size, keeping any removal that still
// fails and restarting at coarser granularity, halving the chunk size
// only when no removal at the current granularity succeeds. Terminates
// at a 1-minimal result: removing any single remaining op makes it pass.
//
// Decoupled from any specific engine pair via `still_fails` -- a
// predicate over an op sequence -- so the SAME shrinker works whether the
// failure came from RunDifferential<A, B>, a single-engine invariant
// check, or anything else that can be reduced to "does this subsequence
// still reproduce".
//
// Ops are removed freely, including ones other ops reference by id (a
// Cancel/Replace whose target Add got removed). This is safe, not just
// convenient: every engine already rejects an unknown id cleanly (see
// RejectReason::UnknownOrderId), so an "orphaned" reference after
// shrinking just becomes another reject-path exercise, never UB or a
// crash the shrinker would need to guard against.
//
// Precondition: still_fails(ops) must already be true. Not assert()'d --
// a caller passing an already-passing sequence is a caller bug, and this
// function will simply (and harmlessly) return it unchanged rather than
// silently fabricate a "shrink" of nothing.
std::vector<FuzzOp> Shrink(std::vector<FuzzOp> ops, const StillFailsPredicate& still_fails);

}  // namespace lob::fuzz
