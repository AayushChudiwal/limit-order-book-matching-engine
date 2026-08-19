# Phase 5 step 2 results: singleton/small-size level optimization

**N=20** SPY comparison for `OptimizedOrderBook`'s `LevelOrders`
(inline-1 + heap-overflow level storage, replacing
`std::deque<RestingOrder>`) against step 1's own N=10 baseline. Covers
the delta table, the frequency-drift check (now settled by a
near-perfectly clock-matched subset rather than by argument), what the
L1D refill counter (added between step 1 and step 2) actually shows,
and a fully-confirmed (not assumed) explanation for the
`order_storage_frees=3860`-vs-naive-expectation gap in the churn data.

**Why N=20**: the initial N=10 pass put `Add_real`'s regression at
-3.25% with a margin of only 1.09x over its noise threshold. Per
`docs/benchmark_methodology.md`'s "N=10 isn't always enough headroom"
rule, a margin that thin gets N=20 before it's called settled. Runs
11-20 were collected for that purpose; **the N=20 result confirms the
regression's direction and size** (-3.07%, 1.24x margin) and, as a
bonus this step didn't plan for, happened to land at a clock rate that
matches step 1's almost exactly -- which settles the frequency question
empirically instead of by inference. Both are below.

## Setup

All 20 `step2_spy_run_*.log` files: 4.180-4.394 GHz (P-core range,
confirmed), identical warmup (45,911 messages, 133 resting orders / 110
levels, 20,886/29,837 Adds used, 30,058 ops/variant, 226 rounds) --
verified identical across all 20, and the same as every prior run in
this series. The binary under test was confirmed current before running
(no recompilation needed), so it included
`OptimizedOrderBook`/`LevelOrders`, not a stale build.

**Run-level noise, disclosed not discarded**: runs 12, 19, and 20 each
read elevated across *multiple* variants simultaneously (run 12 on
every variant; runs 19-20 on Cancel and Replace) -- the same run-level
system-noise signature `docs/benchmark_methodology.md` documents for
the original baseline's run 5. One bracket carries a `[MIGRATED]` flag
(run 9's `Replace_shuffled`); step 1's own set has exactly one too (run
3). **All are included in every statistic below**, per this project's
standing rule against discarding an inconvenient run without a
principled reason. They are why several N=20 CoVs are *higher* than the
N=10 CoVs -- 10 more runs sampled more of the real noise floor, which
is the point of collecting them.

## The numbers

| Variant | step1 mean (c) | step2 N=20 mean (c) | step2 min | step2 max | step2 CoV% | delta % | threshold % | margin | verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Add_real | 251.38 | 259.10 | 249.96 | 278.72 | 2.80 | -3.07 | 2.48 | 1.24x | SIGNIFICANT (slower) |
| Add_widened | 533.72 | 404.09 | 389.44 | 421.20 | 2.08 | +24.29 | 1.55 | 15.68x | SIGNIFICANT |
| Cancel_traversal | 234.76 | 175.59 | 169.36 | 192.30 | 3.45 | +25.20 | 1.70 | 14.81x | SIGNIFICANT |
| Cancel_shuffled | 238.08 | 169.86 | 163.88 | 186.11 | 3.30 | +28.66 | 1.62 | 17.71x | SIGNIFICANT |
| Reduce_traversal | 80.00 | 76.06 | 74.30 | 78.20 | 1.22 | +4.92 | 0.95 | 5.20x | SIGNIFICANT |
| Reduce_shuffled | 84.22 | 82.51 | 81.51 | 85.11 | 1.05 | +2.03 | 0.99 | 2.04x | SIGNIFICANT |
| Replace_traversal | 638.91 | 533.97 | 523.24 | 559.54 | 1.90 | +16.42 | 1.56 | 10.55x | SIGNIFICANT |
| Replace_shuffled | 617.72 | 515.20 | 502.21 | 542.00 | 2.25 | +16.60 | 1.49 | 11.12x | SIGNIFICANT |

(`delta %` positive = step2 faster than step1. **Threshold note**: the
`0.894 * CoV%` shorthand in `docs/benchmark_methodology.md` is derived
for N=10-vs-N=10 with a shared variance estimate. This table is
N=10-vs-N=20 with visibly *unequal* variances, so it uses the
underlying formula that shorthand comes from rather than the shorthand
itself: `threshold = 2 * sqrt(s1²/10 + s2²/20)`, expressed as a
percentage of the step1 mean. That is the same 2-SE-of-the-difference
criterion, computed with each set's own observed spread instead of
assuming they match. Using the naive `0.894 * step1 CoV%` shorthand
here would have been *looser* on the noisier variants and would not
have credited N=20's tighter estimate on the quieter ones.)

Seven of eight variants are real wins, several very large (Cancel
+25-29%, Replace +16%, Add_widened +24%). Every delta moved by well
under a percentage point from its N=10 value -- the extra 10 runs
changed the *confidence*, not the *conclusion*, on any variant.

**`Add_real` is the one result that goes the other way**: a real,
measured *regression* (-3.07%), and -- unlike every other variant in
this table -- its margin over the noise threshold is thin (1.24x, up
from N=10's 1.09x). That's the number this step scrutinized hardest,
both for the frequency question and on its own mechanistic terms.
`Reduce_shuffled` (2.04x) is the only other variant not clearing by a
wide margin.

## The frequency gap: no longer an argument, now a measurement

The N=10 write-up had to *argue* that the ~1.5% clock gap between step
1 and step 2 wasn't manufacturing `Add_real`'s regression, using a
direction-of-bias inference. Runs 11-20 make that argument unnecessary,
because they happened to run hot enough to close the gap almost
entirely:

| Run set | N | mean clock | gap vs step1 |
|---|---:|---:|---:|
| step1 (baseline) | 10 | 4.3363 GHz | -- |
| step2 runs 1-10 | 10 | 4.2696 GHz | 1.54% |
| **step2 runs 11-20** | 10 | **4.3315 GHz** | **0.11%** |
| step2 runs 1-20 (all) | 20 | 4.3005 GHz | 0.82% |

Runs 11-20 sit **0.11% off step 1's mean clock** -- effectively
clock-matched, an order of magnitude tighter than the 1.54% gap the
original argument had to reason around. That converts the frequency
question from an inference into a direct test: **if the clock gap were
manufacturing `Add_real`'s regression, removing the gap should remove
the regression.**

It doesn't:

| Add_real comparison | clock gap | delta | threshold | margin |
|---|---:|---:|---:|---:|
| step2 runs 1-10 (original) | 1.54% | -3.25% | 2.99% | 1.09x |
| **step2 runs 11-20 (clock-matched)** | **0.11%** | **-2.89%** | 3.02% | 0.95x |
| step2 runs 1-20 (headline) | 0.82% | -3.07% | 2.48% | 1.24x |

The regression is still there, same sign, same rough size (-2.89% vs
-3.25%), with the clock gap reduced ~14x. A confound that survives the
removal of its own supposed cause is not that confound.

**Two direct corroborations, on top of that:**

1. **No within-set clock/cycles relationship at all.** Across all 20
   step2 runs, the correlation between a run's calibrated clock and its
   `Add_real` cycles/op is **r = +0.18** (t = 0.79, df = 18, p ≈ 0.44)
   -- statistically indistinguishable from zero. If the "clock rate
   biases cycle counts" mechanism were operating at any material
   strength across this 4.18-4.39 GHz range, 20 paired samples would
   show it. They don't. This is the same conclusion step 1 reached from
   a different direction (its far more extreme 52.6% Low-Power-Mode gap
   produced a *smaller* Add_real delta than a near-matched 1.58% gap
   did), now measured within a single clean run set rather than
   inferred across two.
2. **The confound's predicted direction didn't even materialize.**
   Runs 11-20 ran at a *higher* clock than runs 1-10 (4.3315 vs 4.2696
   GHz). The step-1 mechanism (a higher clock makes a fixed-nanosecond
   memory stall cost *more* cycles) predicts runs 11-20 should read
   *more* cycles on `Add_real`. They read slightly **fewer** (258.64 vs
   259.56). The effect is not merely too small to matter here -- it
   points the wrong way, which is what "swamped by other noise" looks
   like.

**On the clock-matched subset reading 0.95x (within noise):** taken
alone, runs 11-20 do not resolve `Add_real`'s regression as
significant. That is a *power* limitation, not counter-evidence, and it
would be dishonest to present it as either more or less than that. It's
N=10, and it contains all three of this set's run-level-noise outliers
(runs 12, 19, 20), which widens its threshold to 3.02% -- slightly
*looser* than the original N=10 set's 2.99% despite testing the same
effect. A ~3% effect against a ~3% threshold is precisely the situation
the N=20 rule exists to handle, which is why **the N=20 pooled
comparison (-3.07%, 1.24x) is the headline number**, not this subset.
The subset's job here is answering the frequency question -- for which
its clock-matching, not its sample size, is what matters -- and its
point estimate (-2.89%, same direction, same magnitude) is what does
that work.

**Conclusion**: the frequency gap does not explain `Add_real`'s
regression, and this is now shown rather than argued -- the regression
persists at essentially full size when the gap is closed to 0.11%, and
clock shows no measurable relationship to `Add_real` cycles across 20
runs. The N=20 margin remains thin (1.24x), so the *size* of the
regression should still be treated as approximate; its *direction and
existence* are settled, and its interpretation as a
measurement-boundary artefact (next section) is unchanged.

## What's actually driving Add_real's regression: a measurement-boundary artefact, not a real cost

**Stated plainly up front, because the mechanism below could otherwise
be misread as "inline-1 made adds slower":** it didn't. What actually
happened is that this specific benchmark draws a hard line between
"warmup" (untimed) and "the Add_real pass" (timed), and `LevelOrders`
moved some allocation cost from one side of that line to the other.
`Add_real`'s regression is substantially an artefact of where that
boundary happens to fall, not evidence that singleton-level storage
costs more in general -- a real, continuously-running order book has no
such boundary; every one of its adds is on the same side of it. See the
explanation below for the mechanism, and the "is this a real cost"
section after it for why the measured -3.07% overstates whatever real
effect (if any) survives outside this benchmark's own artificial
warmup/timed split.

This is the one place step 2's churn instrumentation earns its keep
beyond the sanity-check role it was built for. From
`bench/step2_churn_spy.csv`:

| Variant | level_creates | level_destroys | order_storage_allocs | order_storage_frees |
|---|---:|---:|---:|---:|
| Add_real | 240 | 84 | 205 | 82 |
| Add_widened | 2754 | 390 | 1151 | 174 |
| Cancel_traversal / shuffled | 0 | 24860 | 0 | 3860 |
| Reduce_traversal / shuffled | 0 | 5 | 0 | 1 |
| Replace_traversal | 21812 | 23526 | 3312 | 4503 |
| Replace_shuffled | 21398 | 22211 | 3131 | 3515 |

`Add_real` creates 240 new price levels over its 8,951-order pass, but
**205 of those same-pass operations also trigger the inline-to-overflow
allocation** -- an 85% ratio. That number is the mechanism: SPY's real
order flow clusters tightly around the live mid (that's the whole
reason `Add_real` vs `Add_widened` exists as a comparison -- see
`docs/benchmark_methodology.md`'s original ~2x finding), so a large
share of this pass's adds land at prices that *already* have exactly
one resting order -- either from warmup or from earlier in this same
pass. Under the OLD (step 1) design, that level's `std::deque` was
already allocated the moment its first order arrived, so a second order
joining it costs nothing extra. Under `LevelOrders`, that allocation is
*deferred* until the second order actually shows up -- and for a level
whose first order arrived during warmup (untimed, free) but whose
second order arrives during this timed `Add_real` pass, the allocation
cost that step 1's design paid for free gets paid here instead, inside
the measurement window.

`Add_widened` shows the same mechanism working in the *opposite*
direction: 2,754 new levels (11x more than `Add_real`, since 10x wider
price dispersion scatters into territory that's overwhelmingly untouched
by either warmup or the rest of this same pass), of which only 1,151
(42%, versus `Add_real`'s 85%) ever get a second order within this pass.
The large majority of `Add_widened`'s new levels stay single-order for
the whole pass and capture the full zero-allocation benefit, which is
why it's this step's second-largest win (+24.62%) despite creating far
more levels in absolute terms than `Add_real` does.

**This is a real, understood, and bounded tradeoff, not a bug**: the
total number of allocations a level pays over its full lifetime is
identical either way (exactly one, if it ever reaches a second order) --
`LevelOrders` only changes *when* that allocation happens, never *how
many*. It's a net win in aggregate (7 of 8 variants improved, several by
a lot), and the one case where it shows up as a same-pass regression is
exactly the case the mechanism predicts: tight, already-established
clustering where a level's first and second orders are likely to land on
opposite sides of the warmup/timed-pass boundary.

### Why -3.07% overstates whatever real cost survives outside this benchmark

The warmup/timed-pass split is an artefact of how this benchmark is
built (a cold book, warmed up untimed, then a separate timed slice), not
a property of a real, continuously-running order book. That split is
*exactly* what makes `Add_real`'s regression look larger than the
underlying algorithm actually costs:

- **Total allocation count, summed over a level's entire lifetime, is
  identical between step 1 and step 2 designs** -- exactly one, if the
  level ever reaches a second order; zero otherwise. `LevelOrders` never
  does *more* total allocation work than `std::deque` did; it only moves
  *when* a level's one allocation (if any) happens. A level that never
  gets a second order pays zero either way; a level that does pays
  exactly one either way. There is no lifetime sense in which step 2
  costs more.
- **In a real, continuously-running book, there is no warmup/timed
  seam for that one allocation to land on one side of.** Each level's
  first-order-then-second-order transition happens whenever it happens,
  spread naturally across the book's entire operating history -- some
  fraction of "second orders" will always be arriving at any given
  moment, the same way they always were under the old design, just
  without the old design's permanent, empty tax on every level that
  never needed it.
- **This specific benchmark concentrates 205 such transitions into one
  8,951-op slice immediately following warmup**, because that's
  precisely where a batch of already-established, still-single-order
  levels sit waiting for SPY's real subsequent order flow to touch them
  again. That's a sampling artefact of testing methodology (warm up
  first, measure second, on a real historical message sequence that
  wasn't generated to avoid this), not a property of the technique being
  measured.

None of this means `Add_real`'s -3.07% is wrong or should be discarded
-- it's a real, correctly-measured number *for this specific benchmark
construction*. It means the number shouldn't be read as "singleton-level
storage makes adds ~3% slower in general": a continuously-running book
would pay the same total allocation cost `LevelOrders` always pays
(strictly less than or equal to the old design's, never more), just
without this benchmark's artificial concentration of a chunk of it into
one measured window.

## Interpreting the L1D refill counter (new this step)

| Variant | l1d refills/op (N=20) | cycles/op (N=20) | refills as % of cycles | (N=10 refills) |
|---|---:|---:|---:|---:|
| Add_real | 2.379 | 259.1 | 0.92% | 2.377 |
| Add_widened | 2.877 | 404.1 | 0.71% | 2.879 |
| Cancel_traversal | 0.157 | 175.6 | 0.09% | 0.162 |
| Cancel_shuffled | 0.240 | 169.9 | 0.14% | 0.242 |
| Reduce_traversal | 0.172 | 76.1 | 0.23% | 0.152 |
| Reduce_shuffled | 0.159 | 82.5 | 0.19% | 0.146 |
| Replace_traversal | 0.690 | 534.0 | 0.13% | 0.685 |
| Replace_shuffled | 0.927 | 515.2 | 0.18% | 0.947 |

The N=20 refill rates are essentially unchanged from N=10 -- the two
Add variants, carrying by far the largest absolute rates, moved by
under 0.1% relative. The largest proportional movement is on Reduce
(0.152 -> 0.172, 0.146 -> 0.159), which is a ~10% shift on a number
already two orders of magnitude below Add's; that is noise on a
near-zero quantity, not signal, and it does not change the reading
below.

Add_real's ~2.38 refills/op against Cancel's ~0.16-0.24/op is roughly a
10-15x spread by this mean (a single run can read closer to the 25x
figure quoted going in, given each is its own noisy PMU sample -- the
20-run mean is the number to trust). Either way the direction and rough
scale is the same, and the mechanism is straightforward:

- **Cancel cycles the SAME 110 levels 226 times.** By the time this
  pass is underway, that small, fixed set of map nodes and hash buckets
  has been touched repeatedly enough to mostly live in a warm cache
  level -- consistent with `order_storage_allocs=0` for Cancel (no new
  memory is ever touched by construction, since Cancel never rests an
  order) and explains the low refill rate directly: there's very little
  *new* memory for Cancel to miss on.
- **Add_real touches a mix of brand-new tree nodes (240 level
  insertions into `std::map`) and brand-new heap allocations (205
  deferred `LevelOrders` overflow allocations)** -- both are exactly the
  kind of cold, non-contiguous, heap-scattered memory access that
  produces L1D misses. Add_widened's even higher creation volume (2,754
  levels) explains why it has the highest refill rate of any variant.

**This corroborates the inline-storage mechanism rather than
contradicting it.** The design's claim was never "eliminates cache
misses" -- it's "eliminates the *allocation* for a level that only ever
holds one order." The L1D data confirms exactly that split: where no
allocation happens (Cancel, Reduce -- both near-zero
`order_storage_allocs`), refill rates are low; where genuinely new
memory gets touched (Add_real's mix of new tree nodes and deferred
allocations, Add_widened's much larger share of genuinely-new
territory), refill rates track that new-memory-touching activity, not
the storage design's efficiency for the case it actually targets.

**Does it explain step 1's unresolved `Reduce_traversal` regression?**
No -- and this needs to be stated as a limitation, not glossed over: the
L1D counter didn't exist yet when that baseline-vs-step1 comparison was
made (`docs/phase5_step1_results.md`), so there is no L1D data for that
specific historical binary to check against. What step 2's own Reduce
numbers *do* show: `Reduce_traversal` and `Reduce_shuffled` have nearly
identical, low refill rates (0.172 vs 0.159/op at N=20, ~8% apart --
far tighter than Cancel's 0.157-vs-0.240 gap or any of the Add/Replace
spreads). That's consistent with, though doesn't newly prove, step 1's
conclusion that Reduce's small regression wasn't a memory-locality
effect in the first place (a memory-locality counter showing near-zero
signal for Reduce is what you'd expect either way, whether the original
cause was compute/branch-related as step 1 hypothesized or something
else entirely). The historical question remains open on its own terms;
this data neither confirms nor newly explains it, and it would be
overreaching to claim otherwise.

## Confirmed, not assumed: why Cancel shows `order_storage_frees=3860`

The naive expectation was `226 rounds x N overflow-mode levels`.
`docs/benchmark_methodology.md`'s own resting-order count (133 orders,
110 levels) suggests `N=23` (the surplus over one-per-level) as a first
guess -- neither `226*23=5198` nor any other simple guess from the
final-state order-count histogram lines up with 3860. Built a
standalone, sudo-free reproduction of the exact warmup + cycling logic
(no PMU involved, so no privilege needed) to settle this directly rather
than adjust the guess until it fit:

> **Reproducibility gap, noted during step 4:** that reproduction was a
> throwaway and was never committed, so unlike this project's other
> evidence tools (`fuzz_mutation_test`, `cache_line_probe`,
> `tick_granularity_scan`) the numbers below cannot currently be
> re-derived by anyone reading this. They were correct when taken -- they
> match the committed CSV exactly, which is the check that mattered --
> but "trust the transcript" is weaker than this project's usual
> standard. The fix is a `--churn-only` mode on `bench_matching_engine`
> that skips PMU setup entirely (churn counting needs no privilege; only
> the cycle counters do), which would make both step 2's and step 4's
> churn evidence runnable without root. Not done inside step 4's commit
> because it means restructuring validated PMU code, which deserves its
> own change and its own verification rather than riding along.

```
resting orders: 133
distinct levels: 110, overflow-mode (>=2 orders) levels: 17, max depth at one level: 4
Cancel_traversal: 225 rounds had exactly 17 frees, 1 round had exactly 35 frees
Cancel_shuffled:  225 rounds had exactly 17 frees, 1 round had exactly 35 frees
225*17 + 35 = 3860 -- exact match to both CSV totals.
```

**The mechanism**: `resting_after_warmup`'s *final* order-count-per-price
histogram says 17 levels currently hold 2+ orders -- and every round
from round 1 onward (rebuilt via `replenish_round`'s plain sequential
`AddLimitOrder` re-adds) faithfully reproduces exactly that: 17 frees,
every single round, no variation. **Round 0 is different because it
runs against `warmup_book` itself** -- copied with its *actual* internal
`LevelOrders` state from the real historical warmup replay, not
reconstructed from a clean final snapshot. `LevelOrders` deliberately
never downgrades `kOverflow` back to `kInline` (documented in
`optimized_order_book.hpp`'s class comment) on the reasoning that a
level's map entry gets erased the instant it's fully empty, so it never
needs to. That reasoning is correct for the "shrinks to zero" case --
but it missed a distinct case: **a level that peaks at 2+ orders during
warmup's real Add/Cancel/Reduce/Replace churn, then gets cancelled back
down to exactly 1 order (never zero), stays permanently in overflow
mode** -- one heap-allocated deque holding a single element, for the
rest of that instance's life, even though it now looks externally
identical to a level that was only ever touched once. Warmup's real
history left **18 such "stranded" levels** (35 observed in round 0,
minus the 17 that are genuinely still 2+, `35 - 17 = 18`) that the
final-snapshot histogram can't see, because it only counts *current*
order counts, not each level's peak.

**This is a real, bounded, and already-understood-in-kind cost, not a
new bug**: a stranded level is never worse off than step 1's design
(which always allocates from the first order regardless), it's simply a
case where step 2's optimization doesn't recover the full theoretical
benefit for a level whose depth spiked and receded before this
particular snapshot. 18 of 110 levels (~16%) carrying this in the
current warmed book is a real, quantifiable gap between "theoretical
best case" and "what a real, churny order book actually achieves."

## Does the stranded fraction grow without bound over a longer session?

The obvious worry, and the one worth answering with evidence rather than
a hand-wave: if every level that ever touches 2 orders stays stranded
forever, could the inline-1 optimization decay toward irrelevance by
mid-session on a full trading day, once far more cancel/replace activity
has had the chance to strand more levels?

**No -- bounded, for two structural reasons, both traceable to evidence
this project already collected independently of this question.**

**1. The absolute number of live levels a session can ever have stranded
is capped by a plateau this project already measured.**
`docs/benchmark_methodology.md`'s "Warmup and book depth" section found
that SPY's resting-order count **plateaus at 240 (NASDAQ) / 113 (PSX)
even after replaying the majority of each symbol's daily message
history** -- book depth does not grow with message count, on either
venue, for this symbol. Since a level can only be "live" (and therefore
only a candidate for being currently stranded) while it's actually
resting, and distinct price levels are always <= resting-order count,
the *absolute number* of levels that could possibly be stranded at any
one moment is capped by that same empirically-plateaued figure. A
worst-case where literally every live level were permanently stranded
would still top out around 240 levels on this symbol -- not a number
that grows as the session runs longer.

**2. The same evidence that produces that plateau is also exactly the
mechanism that un-strands levels.** A depth plateau (rather than
monotonic growth) is only possible if levels are regularly clearing back
to zero orders as often as new ones open -- if they weren't, resting
depth would accumulate over the session instead of holding steady. Every
time a level clears to zero, `OptimizedOrderBook` erases its map entry
(`PruneAndEmitLevelUpdate`, `MatchAgainst`) and any future order at that
price gets a brand-new, non-stranded `LevelOrders` instance -- this is
literally the un-stranding mechanism (see `LevelOrders`' class comment).
The same high cancel/requote rate that `docs/benchmark_methodology.md`
already identified as the *cause* of the depth plateau ("very high
cancel/requote rates mean most quote volume is fleeting, not resting
size") is what keeps clearing levels back to zero and resetting them to
a fresh, non-stranded state. Both the numerator (stranded levels) and
the denominator (live levels) are governed by the same churn dynamic, so
there's no structural reason for their ratio to trend toward 1 as the
session lengthens rather than settling into some steady-state fraction.

**When this reasoning would stop holding, and downgrading would become
worth the complexity**: this argument depends on the specific real-world
pattern this project's ITCH data actually shows -- heavy cancel/requote
churn, most quote volume fleeting rather than resting. A workload with
structurally different order flow (orders placed and mostly left resting
for the session, rarely cancelled -- a "post and hold" pattern rather
than SPY's high-frequency requoting) would see levels clear far less
often, weakening the un-stranding mechanism without weakening the rate
at which levels first touch overflow -- that's the condition under which
the stranded fraction could plausibly trend upward over a session rather
than stabilize. If a future symbol or venue's message mix shows that
pattern (low cancel share, high resting persistence -- checkable the
same way `docs/message_mix_psx_20190730.md` already characterizes
message-type mix per venue), that's the concrete trigger for revisiting
whether an explicit downgrade-on-drain-to-1 is worth its added
complexity. Not the case for anything measured in this project so far.

## Bottom line

- **Correctness**: unaffected by anything in this document --
  `RunDifferential<OrderBook, OptimizedOrderBook>`, the new
  `LevelOrders` unit tests, and mutation testing were already verified
  before this benchmark ran. Re-confirmed after the N=20 collection:
  full suite 140/140 passing.
- **Performance (N=20)**: 7 of 8 variants show large, well-margined
  wins (Cancel +25-29%, Replace +16%, Add_widened +24%, Reduce +2-5%).
  Every delta moved under a percentage point from its N=10 value --
  the extra 10 runs changed confidence, not conclusions.
  `Add_real` regresses -3.07%, measured correctly but substantially a
  **measurement-boundary artefact**, not a real per-operation cost:
  `LevelOrders` never does more total allocation work over a level's
  lifetime than `std::deque` did, it only moves *when* that one
  allocation (if any) happens -- this benchmark's own warmup/timed-pass
  seam is what concentrates 205 such transitions into one measured
  window, something a continuously-running book with no such seam would
  never do. Its margin improved from 1.09x to 1.24x, which is better
  but still thin: **treat the direction as settled and the exact
  magnitude as approximate.**
- **The clock confound is now closed empirically, not argued.** Runs
  11-20 landed 0.11% off step 1's mean clock (vs the original set's
  1.54% gap), and `Add_real`'s regression persisted at essentially full
  size (-2.89%). Clock shows no measurable relationship to `Add_real`
  cycles across all 20 runs (r = +0.18, p ~ 0.44), and the confound's
  predicted direction did not materialize -- the higher-clocked half
  read *fewer* cycles, not more. The N=10 write-up's direction-of-bias
  inference was correct, and no longer has to be relied on.
- **Threshold methodology**: this table's N=10-vs-N=20 comparison uses
  `2 * sqrt(s1²/10 + s2²/20)` rather than the `0.894 * CoV%` shorthand,
  which is derived specifically for the equal-N, equal-variance case
  neither of which holds here. Same criterion, correct arithmetic for
  the actual design.
- **L1D refills**: corroborate the inline-storage mechanism (low refill
  rate exactly where no allocation happens, high rate exactly where new
  tree nodes or deferred allocations occur) without retroactively
  explaining step 1's separate, still-open `Reduce_traversal` question
  -- that data doesn't exist for step 1's binary and this document
  doesn't pretend otherwise.
- **Churn mechanism**: confirmed exactly (not assumed) via a dedicated,
  sudo-free reproduction -- 17 genuinely-persistent overflow levels plus
  18 "stranded" ones inherited from warmup's real churn history, exactly
  accounting for all 3860 frees. Surfaced a real, previously undocumented
  edge case in `LevelOrders`' "never downgrade" design, worth carrying
  into step 3's own churn checks rather than rediscovering.
- **Stranded-overflow growth**: reasoned through and answered, not
  assumed -- bounded, not unbounded, over a longer session. Traceable to
  two structural facts: SPY's resting-order count is independently
  measured to plateau rather than grow with message count
  (`docs/benchmark_methodology.md`), which caps the absolute number of
  levels that could ever be stranded at once; and the same high
  cancel/requote churn that produces that plateau is exactly the
  mechanism that clears levels back to zero and un-strands them, so
  numerator and denominator move together rather than the ratio trending
  toward 1. Documented the concrete condition (a low-cancel,
  high-resting-persistence order-flow pattern) under which this stops
  holding and an explicit downgrade would become worth its complexity.

## Reproducing the N=20 numbers

```
for i in $(seq 1 20); do
  sudo ./build/bench_matching_engine \
    data/07302019.NASDAQ_ITCH50.partial bench/step2_spy_run_$i.csv SPY \
    | tee bench/step2_spy_run_$i.log
done
```

Every statistic in this document is computed from the committed
`bench/step2_spy_run_{1..20}.csv` (cycles, L1D refills) and
`bench/step2_spy_run_{1..20}.log` (per-run calibrated clock), against
`bench/step1_spy_run_{1..10}.csv` / `.log`. The churn counts come from
`bench/step2_churn_spy.csv`, produced by a separate
`-DLOB_TRACK_CHURN=ON` build (`build-churn/`) so the timed runs above
never carry that instrumentation's overhead.

Note that the run set is *not* reproducible cycle-for-cycle -- the
engine's replay is deterministic (identical warmup, op counts, and
churn every run, verified across all 20), but cycle counts and clock
calibration are physical measurements of this specific M4 under this
specific thermal/background state. Runs 12, 19, and 20 are the
concrete illustration: same deterministic work, measurably noisier
readings.
