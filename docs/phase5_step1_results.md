# Phase 5 step 1 results: correctness confirmed, performance measured and mostly explained

This is the required N=10 baseline comparison for commit `e2e0422`
("Phase 5 step 1: hot-path hygiene -- eliminate redundant level lookups"),
covering the same three questions as before, now against a valid
measurement: the delta table against the Phase 4 baseline, mechanism
attribution for each variant, and the `OnOrderCancelled`/prune
reordering question. An earlier version of this document was written
against a run captured on battery power with Low Power Mode on
(calibrated ~2.088 GHz, an E-core-range clock); that data has been
superseded by the corrected run below and is not cited for any
magnitude claim.

## The re-run is valid

All 10 corrected `step1_spy_run_*.log` files report calibrated
frequencies in **4.296-4.421 GHz**, matching the Phase 4 baseline's own
4.376-4.439 GHz range (P-core, no throttling). Setup is identical to
both the original step1 attempt and the baseline: 45,911 warmup
messages, resting depth 133 / 110 levels, 20,886 of 29,837 Adds used in
warmup (8,951 reserved for the Add slice), 30,058 ops per
Cancel/Reduce/Replace variant (226 cycling rounds), verified from every
log rather than assumed.

**Residual clock gap, checked rather than ignored:** the corrected
run's mean frequency is 4.336 GHz against the baseline's 4.406 GHz
mean -- a **1.58% residual gap**, step1 still very slightly the lower
clock of the two. Given the confound mechanism identified in the
invalidated run (lower clock reduces the cycle cost of fixed-ns memory
stalls), this residual gap could in principle still nudge memory-bound
variants toward looking slightly faster than they truly are. Three
reasons this doesn't change any conclusion here:

1. **The direction test.** The original (invalid, ~53%-lower-clock)
   run measured Add_real's delta at 16.20%. This corrected run, at only
   a 1.58% lower clock than baseline, measures Add_real's delta at
   **19.84% -- larger, not smaller.** If clock-domain difference were
   the dominant driver of Add's delta, the far more extreme clock drop
   should have produced the larger number, not the smaller one. It
   didn't. That's evidence the code-level win is the real effect and
   the E-core run, if anything, *understated* it (plausibly because
   E-cores also carry smaller L1/L2 caches per
   `docs/cache_hierarchy_m4.md`, adding extra cache-miss overhead that
   partially offset the lookup-elimination benefit -- a different
   confound working in the opposite direction, not a clean single-variable
   comparison either way).
2. **The margin test.** Every variant classified SIGNIFICANT below
   clears its own noise threshold by at least 1.49x, most by far more
   (Add_real at 9.36x, Add_widened at 5.96x). A 1.58% clock gap,
   even under the (already-contradicted) assumption that it inflates
   deltas linearly, cannot plausibly account for a variant losing
   33%+ of its measured delta -- the amount needed to push the closest
   case (Cancel_traversal, 1.49x) back into its noise band.
3. **The direction-of-bias test on the one regression.** If the
   residual 1.58% gap biases anything, it biases *toward* step1 looking
   faster (lower clock = fewer cycles for the same fixed-ns memory
   stall). `Reduce_traversal` is the one variant that measures *slower*
   in step1 (-2.17%). A bias that would make step1 look artificially
   faster cannot be the explanation for a variant that instead measures
   artificially slower -- if anything this makes that regression more
   credible, not less.

**Conclusion: the ~1-2% residual clock gap is not large enough to move
any variant's classification, and where it could theoretically bias a
result, it argues against rather than for the two most interesting
findings below (Add's large win, Reduce's small regression).**

## The numbers

| Variant | step1 mean (c) | min | max | CoV% | baseline mean (c) | delta % | min-detectable % | margin (x threshold) | verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Add_real | 251.38 | 245.25 | 272.43 | 3.34 | 313.6 | 19.84 | 2.12 | 9.36x | SIGNIFICANT |
| Add_widened | 533.72 | 505.09 | 548.36 | 2.18 | 608.7 | 12.32 | 2.07 | 5.96x | SIGNIFICANT |
| Cancel_traversal | 234.76 | 228.97 | 243.05 | 1.98 | 254.9 | 7.90 | 5.31 | 1.49x | SIGNIFICANT |
| Cancel_shuffled | 238.08 | 231.51 | 244.62 | 1.95 | 258.0 | 7.72 | 5.00 | 1.54x | SIGNIFICANT |
| Reduce_traversal | 80.00 | 78.23 | 81.30 | 1.25 | 78.3 | -2.17 | 1.16 | 1.87x | SIGNIFICANT (slower) |
| Reduce_shuffled | 84.22 | 82.47 | 85.89 | 1.39 | 83.6 | -0.75 | 1.24 | 0.60x | WITHIN NOISE |
| Replace_traversal | 638.91 | 626.05 | 664.81 | 2.19 | 703.5 | 9.18 | 4.09 | 2.24x | SIGNIFICANT |
| Replace_shuffled | 617.72 | 606.85 | 636.25 | 1.95 | 681.8 | 9.40 | 3.33 | 2.82x | SIGNIFICANT |

(`delta %` = `(baseline_mean - step1_mean) / baseline_mean * 100`,
positive = step1 faster; `min-detectable %` = `0.894 * baseline CoV%`
per the Phase 4 methodology's own N=10-vs-N=10 threshold; `margin` =
`|delta%| / min-detectable%`, how many multiples of the noise floor the
result clears.)

Six of eight variants are real, measurable wins, ranging from Cancel's
modest ~7.7-7.9% to Add_real's ~19.8%. `Reduce_shuffled` is genuinely
within noise. `Reduce_traversal` is real but in the wrong direction --
addressed below, since it's the one result that doesn't fit the
mechanism story cleanly.

## Mechanism attribution, checked against the corrected data

**`Add_real`/`Add_widened` -- confirmed, and larger than first
measured.** `RestOrder` is the only one of the three step-1 changes
reachable from a resting Add, and it cuts the map traversal count on
that path from 2 (`bids_[price]` insert-or-find, then a second,
fully redundant `LevelFor` inside the old `EmitLevelUpdate`) to 1
(the held reference is reused directly). The largest structural cut of
the three changes, on the path with the fewest total ops -- correctly
predicted to be the largest proportional win, and at 19.84% /
12.32% it's the largest of any variant by a wide margin.

**`Cancel_traversal`/`Cancel_shuffled` -- confirmed, in the predicted
direction and now clearly outside noise.** `PruneAndEmitLevelUpdate`
collapses `CancelOrder`'s 3 map finds (`RemoveFromLevel`'s `LevelFor` +
`EraseLevelIfEmpty`'s own find + `EmitLevelUpdate`'s `LevelFor`) to 2.
Smaller cut than Add's (33% vs 50% of finds removed), diluted further
by everything else Cancel does that this change doesn't touch
(`locations_` erase, a deque erase). Lands at ~7.7-7.9% -- smaller
than Add as predicted, and (unlike in the invalidated run) clearly
outside its own noise band this time.

**`Replace_traversal`/`Replace_shuffled` -- confirmed, consistent with
composition.** `Replace = CancelOrder(old_id) + AddLimitOrder(new_id, ...)`,
so it inherits `PruneAndEmitLevelUpdate`'s Cancel-side win plus
`RestOrder`'s larger Add-side win whenever the replace doesn't cross
and rests. Lands at ~9.2-9.4%, between Cancel's and Add's own deltas,
as the composition predicts.

**`Reduce_traversal`/`Reduce_shuffled` -- partially falsified, and
worth being precise about which part.** The original claim was: in the
common (non-exhausting) case, `ReduceRestingQuantity` does exactly 2 map
finds both before and after this commit (`LevelFor` + `EmitLevelUpdate`'s
own `LevelFor`, old; `LevelFor` + `PruneAndEmitLevelUpdate`'s own
`.find()`, new) -- so the find-count mechanism predicts ~0% delta. That
part is **re-verified correct**: both old and new code do exactly two
independent `.find(price)` calls on the non-exhausting path -- confirmed
by re-reading both versions side by side, not just by the earlier
summary. The find-count mechanism also correctly predicts Reduce should
show *by far* the smallest movement of any variant, which is true both
times this was measured (an order of magnitude below Add/Cancel/Replace
either way).

**What it got wrong: "should show ~0% delta" is too strong a claim.**
`Reduce_traversal` measures -2.17% here (and -1.45% in the invalidated
run -- same direction both times, so this isn't run-to-run noise).
Instructions/op (not just cycles) confirm something real changed:
step1 executes ~1.17-1.18% *more* instructions per op than baseline,
almost identically for both `Reduce_traversal` and `Reduce_shuffled`
(302.5 -> 306.1 instr/op traversal, 303.7 -> 307.2 shuffled). Checked
two hypotheses for where those extra instructions come from:

- *Lost inlining of `FindOrderInLevel`* (the shared function replacing
  three duplicated `std::find_if` call sites) -- **ruled out.** Built
  the pre-step-1 commit (`d8ca725`) in an isolated worktree and
  disassembled `ReduceRestingQuantity` from both binaries
  (`objdump -d --demangle`, Release/`-O3`/NDEBUG on both). No `bl`
  (branch-with-link, i.e. a real out-of-line call) to `FindOrderInLevel`
  exists in either version's hot path -- it's still fully inlined,
  same as the old triplicated lambda was.
- *Static code size* -- also doesn't explain it on its own: the new
  compiled `ReduceRestingQuantity` is actually **smaller** (193 vs 206
  static instructions) than the old one, which on its face argues the
  opposite direction from the measured +1.17% *dynamic* instructions/op.
  Static size and dynamic per-call execution count aren't the same
  quantity once branch/loop paths differ, so this isn't a real
  contradiction -- but it does mean a whole-function size diff can't
  settle where the extra dynamic instructions come from.

**Where this was left:** the extra ~3.5 instructions/op most likely
come from how `PruneAndEmitLevelUpdate`'s branch structure (an
explicit `it->second.empty()` check plus the buy/sell dispatch) got
laid out differently than the old `EmitLevelUpdate` + conditional
`EraseLevelIfEmpty`, but pinning that precisely -- and explaining why
cycles diverge more sharply for `_traversal` (IPC drops 3.863->3.826)
than for `_shuffled` (IPC actually ticks up 3.635->3.648) despite both
getting nearly the same instruction-count increase -- would need
branch-misprediction and cache-miss counters this harness doesn't
collect (a limitation `docs/benchmark_methodology.md` already states:
this PMU setup gives mean cycles/op, not a decomposition into stall
categories). Not chased further given the honest ceiling on what two
counters (cycles, instructions) can resolve, and given the absolute
size of the effect (~1.7-2 cycles/op) is small on its own terms.

**Net correction to the mechanism story:** the find-count reasoning
correctly predicts *relative magnitude* across all four op types --
Reduce is and should be an order of magnitude less affected than
Add/Cancel/Replace, and it is. It does not correctly predict that
Reduce should be a *wash*. There's a small, real, reproducible net cost
(not a benefit) on this one variant that the step 1 hygiene pass
introduced as a side effect, most likely in how
`PruneAndEmitLevelUpdate` compiles relative to the old
two-function split, not in anything algorithmic. Whether this ~2-cycle
regression is worth chasing further (e.g. trying an alternate branch
layout, or accepting it as noise-adjacent) is a step 2 call, not
something this document resolves.

## The `OnOrderCancelled`/prune reordering: still verified observable

Unchanged from the prior investigation, independent of the benchmark
correction above. `tests/test_cancel.cpp`'s
`Cancel.DuringOnOrderCancelledTheJustEmptiedLevelIsTransientlyStillQueryable`
uses a `BookQueryingListener` holding its own pointer back to the
`OrderBook`, and confirms that cancelling the only order at a price
level and querying `BestAsk()` from inside `OnOrderCancelled` returns
that (now-empty) level -- it isn't pruned until `PruneAndEmitLevelUpdate`
runs immediately after. No listener in this repo does this today
(`RecordingListener`, `FuzzListener` only record events), and the same
emit-before-erase pattern already existed on the `MatchAgainst` fill
path before this commit, so `CancelOrder` is now consistent with a
contract that already existed elsewhere in the engine rather than
introducing a new kind of transience -- but it's a genuine change from
what `CancelOrder` itself used to guarantee, so it's documented
directly on `BookListener::OnOrderCancelled` in
`include/lob/listener.hpp` and pinned by the test. 129/129 tests pass.

## Bottom line

- **Correctness**: confirmed. 129/129 tests, unchanged from the
  original commit's own verification (mutation testing 140/140, fuzz
  clean).
- **Performance**: now validly measured. Add_real +19.8%, Add_widened
  +12.3%, Cancel +7.7-7.9%, Replace +9.2-9.4% -- all real, all clear
  their noise thresholds, all mechanistically explained by which of the
  three step-1 changes reaches which code path. Cancel's margin (~1.5x
  its threshold) is the thinnest of the four wins -- see
  `docs/benchmark_methodology.md`'s new "N=10 isn't always enough
  headroom" section, which sets N=20 as the bar for any future commit
  whose headline claim is Cancel-sized or thinner. `Reduce_shuffled` is
  a wash (within noise, as predicted). `Reduce_traversal` is a small,
  real, reproducible regression
  (-2.17%) that the find-count mechanism didn't predict and that
  disassembly narrowed down to "not lost inlining" without fully
  resolving -- flagged honestly rather than folded into the win story.
- **The residual ~1.6% clock gap** between this run and the baseline
  is checked, not assumed away: it's too small to move any variant's
  classification, and in the two cases where it matters most (Add's
  large win, Reduce's regression) the direction of any residual bias
  argues against, not for, the effect actually observed.
- **The `OnOrderCancelled` reordering**: real and observable under a
  specific, currently-hypothetical listener shape, not exercised by
  anything in this repo today, consistent with a pre-existing contract
  elsewhere in the engine. Pinned down with a test, documented on the
  interface.

Phase 5 step 1 is ready to be called done: hygiene change, correctness
verified, performance validly measured and net positive across 6 of 8
variants, with the one regression documented rather than hidden.
