# Phase 5 step 1 results: correctness confirmed, performance benchmark invalid

This is the required N=10 baseline comparison for commit `e2e0422`
("Phase 5 step 1: hot-path hygiene -- eliminate redundant level lookups"),
plus the two questions left open by that commit: whether the
`OnOrderCancelled`/prune reordering is truly unobservable, and what
mechanism explains each variant's delta.

**Headline: the 10-run comparison below cannot be trusted as evidence of
a performance change. It was measured on a different CPU clock domain
than the Phase 4 baseline it's being compared against.** Correctness is
fully confirmed independent of that problem. Performance is not yet
validly measured. Both are stated plainly below rather than one being
allowed to imply the other.

## The benchmark is confounded, not just noisy

Every one of the 10 `step1_spy_run_*.log` files reports **`calibrated
frequency this run: 2.087-2.088 GHz`**. The Phase 4 baseline's 10 runs
(`docs/benchmark_methodology.md`) ranged **4.376-4.439 GHz**. That's not
run-to-run variance -- the baseline's own 10 runs varied by ~1.4%
(real thermal/DVFS drift, as documented there); the step1 runs vary by
**0.05%** across all 10 of them, which is far too tight to be thermal
noise and is instead the signature of every run landing on the same
clock domain throughout.

This machine, checked directly:

```
$ pmset -g batt
Now drawing from 'Battery Power'
 -InternalBattery-0; 25%; discharging
$ pmset -g | grep lowpowermode
 lowpowermode         1
```

On battery, with **Low Power Mode on**. `docs/cache_hierarchy_m4.md`
already documents that this project's `SetInteractiveQos()` sets
`QOS_CLASS_USER_INTERACTIVE` specifically to bias scheduling onto
P-cores, and that this is a *hint*, not a guarantee. Low Power Mode is
known to cap or exclude P-cores regardless of that hint. `2.088 GHz` is
far below this M4's P-core range (the baseline's own 4.4 GHz) and
consistent with an E-core clock. `bench_matching_engine` calls
`SetInteractiveQos()` and logs whether it *failed* (it didn't -- no
warning line appears in any step1 log) but has no check on whether the
resulting calibrated frequency is actually in the expected P-core band,
so this ran to completion without surfacing the problem.

**Why this specifically corrupts a cross-session cycles/op comparison,
not just wall-clock time:** the PMU counts real elapsed cycles, so a
cycle is still a cycle regardless of clock speed -- but memory-latency
stalls (a `std::map` node's cache miss, a hash bucket lookup) are fixed
in *nanoseconds*, not cycles. At a lower clock, that same fixed-ns
stall consumes fewer cycles. So any operation whose cost is
memory-latency-dominated will show *artificially fewer* cycles/op when
measured at 2.1 GHz than at 4.4 GHz, independent of any code change --
while an operation whose cost is compute-dominated (loop iterations,
arithmetic) won't shift nearly as much, since its cost scales with
instruction count, not wall-clock stall time. That is exactly the
pattern in the raw numbers below: the two variants with the least
memory-chasing (`Reduce_*`, a short deque scan and a subtraction) show
~0% delta, while `Add_real`, `Cancel_*`, and `Replace_*` (all dominated
by `std::map`/`unordered_map` traversal) show the largest deltas. That
correlation is what a clock-domain confound predicts; it is also
consistent with (but does not distinguish from) the code-level
hygiene win the commit intended. The two can't be separated with this
data.

## The raw numbers, reported as provisional

Setup was verified identical across all 10 step1 runs from each log
(same as the baseline's own methodology requires): 45,911 warmup
messages, resting depth 133 / 110 levels, 20,886 of 29,837 Adds used in
warmup (8,951 reserved for the Add slice), 30,058 ops per
Cancel/Reduce/Replace variant (226 cycling rounds). Only the clock
domain differs from the baseline session -- the workload itself is a
clean match.

| Variant | step1 mean (c) | min | max | CoV% | baseline mean (c) | delta % | min-detectable % | verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| Add_real | 262.79 | 254.40 | 273.62 | 2.26 | 313.6 | 16.20 | 2.12 | outside noise threshold, **but confound cannot be excluded** |
| Add_widened | 579.90 | 557.91 | 600.04 | 2.48 | 608.7 | 4.73 | 2.07 | outside noise threshold, **but confound cannot be excluded** |
| Cancel_traversal | 248.36 | 233.80 | 257.70 | 2.84 | 254.9 | 2.57 | 5.31 | within noise |
| Cancel_shuffled | 253.82 | 234.42 | 265.07 | 3.44 | 258.0 | 1.62 | 5.00 | within noise |
| Reduce_traversal | 79.44 | 77.00 | 82.82 | 2.54 | 78.3 | -1.45 | 1.16 | outside noise threshold (slightly **slower**) |
| Reduce_shuffled | 83.90 | 82.82 | 85.03 | 0.98 | 83.6 | -0.36 | 1.24 | within noise |
| Replace_traversal | 638.11 | 629.62 | 644.46 | 0.72 | 703.5 | 9.30 | 4.09 | outside noise threshold, **but confound cannot be excluded** |
| Replace_shuffled | 614.38 | 604.95 | 620.99 | 0.81 | 681.8 | 9.89 | 3.33 | outside noise threshold, **but confound cannot be excluded** |

(`delta %` = `(baseline_mean - step1_mean) / baseline_mean * 100`;
positive = step1 faster. `min-detectable %` = `0.894 * baseline CoV%`,
per the Phase 4 methodology's own N=10-vs-N=10 threshold.)

Every variant that clears the noise threshold is one dominated by
map/hash traversal -- exactly the variants the clock-domain confound
would inflate. Nothing here should be read as "Add_real got 16%
faster." `Reduce_traversal`'s small apparent regression (-1.45%, just
outside its own tight 1.16% threshold) is also flagged rather than
waved off -- see the mechanism section below for why near-zero-to-slightly-negative
is exactly what the code change predicts for this variant regardless of
clock domain.

## What to do before Phase 5 step 2

1. Re-run the same `for i in $(seq 1 10); do sudo ./build/bench_matching_engine ...; done`
   loop on AC power with Low Power Mode off, and confirm each log's
   calibrated frequency lands in the ~4.3-4.5 GHz band (matching the
   baseline) before trusting any delta.
2. Consider adding a sanity check to `bench_matching_engine` (or
   `CalibrateGigahertz`'s caller) that warns or aborts if the calibrated
   frequency falls well outside the expected P-core band -- this
   confound produced a complete, clean-looking, fully-consistent 10-run
   CSV set with no error, warning, or `[MIGRATED]` flag anywhere. Nothing
   in the current instrumentation would have caught this without manually
   reading the calibration line in each log.
3. Until then, this step's performance claim is **null**, not negative
   and not positive.

## Mechanism attribution (code-level, holds regardless of the clock-domain question)

The *ranking* between variants is explainable from the diff itself, and
this part doesn't depend on resolving the confound above -- it's about
which of the three changes touches which code path, not about the
absolute magnitude.

**`Add_real`/`Add_widened` -- only `RestOrder`'s held-reference change
applies.** `AddLimitOrder` calls `RestOrder` when it doesn't fully
match, and `RestOrder` is the *only* one of the three step-1 changes
that touches this path -- `PruneAndEmitLevelUpdate` is Cancel/Reduce/
price-changing-Modify only, and `FindOrderInLevel` is Remove/Modify/
Reduce only, neither reachable from a fresh Add. Before: `bids_[price]`
(one tree traversal, insert-or-find) then `EmitLevelUpdate` calling
`LevelFor` (`bids_.find(price)`, a **second, fully redundant** tree
traversal for the same key). After: one traversal, level reference held
and reused. That's a 2-to-1 reduction in `std::map` traversals on every
single Add that rests -- the largest structural cut of any of the three
changes, on the path with the fewest ops per call, so it should show the
largest proportional effect. It does, in both this data and (pending
re-verification) presumably a clean re-run.

**`Cancel_traversal`/`Cancel_shuffled` -- `PruneAndEmitLevelUpdate`
collapses 3 map finds into 2.** Old `CancelOrder`: `RemoveFromLevel`'s
`LevelFor` (find #1) + `EraseLevelIfEmpty`'s own find (find #2) +
`EmitLevelUpdate`'s `LevelFor` (find #3) = 3 finds. New: `LevelFor`
(find #1) + `PruneAndEmitLevelUpdate` (find #2) = 2 finds -- a 33%
cut, smaller than Add's 50% cut, and further diluted by everything else
Cancel does (a `locations_` erase, a deque erase via `FindOrderInLevel`,
two listener calls) that this change doesn't touch. Consistent with
the smallest resolvable win of the three memory-bound variants -- and
in this data it doesn't even clear its own noise threshold.

**`Reduce_traversal`/`Reduce_shuffled` -- no change in map-find count on
the common path.** This is the one that needed tracing carefully because
it's not obvious from the summary in the commit message. Old
`ReduceRestingQuantity`: `LevelFor` (find #1, always) +
conditionally `EraseLevelIfEmpty` (find #2, **only if the reduce
exhausts the order to zero**) + `EmitLevelUpdate` (find #3, always).
New: `LevelFor` (find #1, always) + `PruneAndEmitLevelUpdate` (find #2,
always). **In the common case where a Reduce doesn't fully exhaust the
order** (a partial reduce, which is most of what this variant drives),
old had exactly 2 finds (`LevelFor` + `EmitLevelUpdate`) and new still
has exactly 2 (`LevelFor` + `PruneAndEmitLevelUpdate`) -- zero
reduction. Only the minority case (reduce-to-zero) goes from 3 finds to
2. This is precisely why Reduce shows ~0% (and a slightly negative
delta on `_traversal`, well within what noise plus
`PruneAndEmitLevelUpdate`'s extra branch/assert overhead on the
non-exhausting path could produce) while every other variant shows a
measurable-or-larger shift: it's the one variant where the "redundant
lookup" being eliminated wasn't actually being paid on the common path
before.

**`Replace_traversal`/`Replace_shuffled` -- compounds both of the above,
via composition.** `Replace(old_id, new_id, ...)` is implemented as
exactly `CancelOrder(old_id)` then `AddLimitOrder(new_id, ...)`. So it
inherits `PruneAndEmitLevelUpdate`'s modest Cancel-side win on the first
half, and `RestOrder`'s larger Add-side win on the second half whenever
the replace doesn't cross the spread and rests (the common case, per
`docs/benchmark_methodology.md`'s note on Replace's real-delta
distribution). This is why Replace shows the second-largest delta of
any variant, larger than Cancel alone and smaller than Add alone,
without needing its own explanation. A back-of-envelope check:
baseline Add_real (313.6c) minus step1 Add_real (262.8c) is ~50.8c
saved; baseline Cancel_traversal (254.9c) minus step1 (248.4c) is ~6.5c
saved; summed, ~57.3c, in the same ballpark as Replace_traversal's
observed ~65c saved (703.5 -> 638.1) -- not exact (Replace's internal
structure isn't literally "one Cancel plus one Add" once matching
against the opposite side is involved), but the right order of
magnitude to support composition as the mechanism rather than something
Replace-specific.

**One important caveat on all of the above:** this reasoning explains
the *relative ranking* between variants, which is a fair thing to trust
even from a clock-confounded dataset, since all 8 variants in a given
run share the same clock domain and the same confound direction. It
does *not* by itself validate the *magnitude* of any individual delta
against the baseline -- that still requires the re-run described above.

## The `OnOrderCancelled`/prune reordering: verified observable, not just theoretically

The commit message called this reordering "unobservable... erasing an
empty map node never calls a `BookListener` method." That's true for
every listener that exists in this repo today (`RecordingListener`,
`FuzzListener` -- both just append to vectors, neither queries book
state). It is **not** true in general: nothing in `BookListener`'s
interface or `OrderBook`'s public API stops a listener from holding its
own pointer back to the same `OrderBook` and calling
`BestBid()`/`BestAsk()`/`TopLevels()`/`FullBook()` synchronously from
inside a callback.

Added `tests/test_cancel.cpp`'s
`Cancel.DuringOnOrderCancelledTheJustEmptiedLevelIsTransientlyStillQueryable`,
using a new `BookQueryingListener` that does exactly that. Confirmed:
cancelling the only order at a price level, and querying `BestAsk()`
from inside `OnOrderCancelled`, returns that (now-empty) price level --
it hasn't been pruned yet, because the erase now happens inside
`PruneAndEmitLevelUpdate`, which runs *after* `OnOrderCancelled`
returns. Once `CancelOrder` itself returns, `BestAsk()` correctly
reports empty. 129/129 tests pass including this one.

Two things temper how alarming this is:

1. **No listener in this codebase does this today**, so nothing is
   currently broken by it.
2. **This exact emit-before-erase pattern already existed elsewhere**
   in the engine before this commit -- `MatchAgainst` (the fill-matching
   loop) has always called `listener_.OnBookUpdate(...)` and *then*
   erased the level if it emptied out from a fill, never the other way
   around. So Phase 5 step 1 didn't invent a new kind of transient
   inconsistency; it made `CancelOrder` consistent with a contract that
   already existed on the fill path. It's a genuine change from what
   `CancelOrder` *itself* used to guarantee, which is exactly why it's
   worth documenting rather than leaving as an unstated coincidence.

Documented the contract directly on `BookListener::OnOrderCancelled` in
`include/lob/listener.hpp`, pointing at the pinned-down test, so a
future listener implementation that does want to query book state
synchronously knows the rule instead of discovering it by accident.

## Bottom line

- **Correctness**: confirmed. 129/129 tests (128 existing + 1 new
  regression test locking down the reordering behavior), no change to
  mutation-testing or fuzz results claimed by this doc (unchanged from
  the original commit's own verification).
- **Performance**: not validly measured yet. The 10-run comparison in
  this document was captured on battery power with Low Power Mode on,
  landing on a ~2.1 GHz clock domain against a ~4.4 GHz baseline, and
  cannot distinguish a real code-driven win from a clock-domain
  artifact that would spuriously favor exactly the memory-bound
  variants this change touches. Needs a re-run under matched power
  conditions before any percentage from this step is cited as a result.
- **The `OnOrderCancelled` reordering**: real and observable under a
  specific, currently-hypothetical listener shape (one that queries the
  book from inside a callback), not exercised by anything in this repo
  today, and consistent with a contract (`OnBookUpdate` may fire before
  an empty level is pruned) that already existed elsewhere in the
  engine. Pinned down with a test and documented on the interface
  rather than left as an unverified claim.
