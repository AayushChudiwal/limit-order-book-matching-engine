# Phase 5 step 2 results: singleton/small-size level optimization

N=10 SPY comparison for `OptimizedOrderBook`'s `LevelOrders` (inline-1 +
heap-overflow level storage, replacing `std::deque<RestingOrder>`)
against step 1's own N=10 baseline. Covers the delta table, the
frequency-drift check, what the L1D refill counter (added between step 1
and step 2) actually shows, and a fully-confirmed (not assumed)
explanation for the `order_storage_frees=3860`-vs-naive-expectation gap
in the churn data.

## Setup

All 10 `step2_spy_run_*.log` files: 4.180-4.324 GHz (P-core range,
confirmed), identical warmup (45,911 messages, 133 resting orders / 110
levels, 20,886/29,837 Adds used, 30,058 ops/variant, 226 rounds) --
same as every prior run in this series. The binary under test was
confirmed current before running (no recompilation needed), so it
included `OptimizedOrderBook`/`LevelOrders`, not a stale build.

## The numbers

| Variant | step1 mean (c) | step2 mean (c) | step2 min | step2 max | step2 CoV% | delta % | min-detectable % | margin | verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Add_real | 251.38 | 259.56 | 252.35 | 272.20 | 2.34 | -3.25 | 2.99 | 1.09x | SIGNIFICANT (slower) |
| Add_widened | 533.72 | 402.30 | 389.44 | 421.20 | 2.40 | +24.62 | 1.95 | 12.62x | SIGNIFICANT |
| Cancel_traversal | 234.76 | 174.27 | 169.36 | 179.03 | 1.69 | +25.76 | 1.77 | 14.56x | SIGNIFICANT |
| Cancel_shuffled | 238.08 | 167.66 | 163.88 | 169.66 | 1.18 | +29.58 | 1.74 | 17.01x | SIGNIFICANT |
| Reduce_traversal | 80.00 | 75.78 | 74.93 | 76.90 | 0.89 | +5.28 | 1.12 | 4.73x | SIGNIFICANT |
| Reduce_shuffled | 84.22 | 82.26 | 81.59 | 83.14 | 0.55 | +2.33 | 1.24 | 1.88x | SIGNIFICANT |
| Replace_traversal | 638.91 | 532.37 | 525.64 | 549.84 | 1.33 | +16.68 | 1.96 | 8.52x | SIGNIFICANT |
| Replace_shuffled | 617.72 | 514.28 | 502.21 | 542.00 | 2.15 | +16.74 | 1.75 | 9.59x | SIGNIFICANT |

(`delta %` positive = step2 faster than step1; threshold is `0.894 *
step1's own CoV%`, this step's immediately-preceding baseline, per
`docs/phase5_plan.md`'s standing rule. Every variant clears its
threshold -- nothing lands within noise this step, which is itself
worth double-checking rather than taking at face value, see below.)

Seven of eight variants are real wins, several very large (Cancel
+26-30%, Replace +17%, Add_widened +25%). **`Add_real` is the one
result that goes the other way**: a real, measured *regression*
(-3.25%), and -- unlike every other variant in this table -- its margin
over the noise threshold is uncomfortably thin (1.09x). That's the one
number in this step that needed the most scrutiny, both for the
frequency question and on its own mechanistic terms.

## The ~1.5% frequency gap: checked, same argument structure as step 1

Precise numbers, not the range endpoints: step1's 10 runs average
**4.336 GHz**; step2's 10 runs average **4.270 GHz** -- a **1.54% gap**
(the range-endpoint comparison the runs were reported with, ~4.30 vs
~4.18-4.32, reads closer to ~2-3%, but the mean-to-mean figure is the
one that actually drives an N=10-vs-N=10 comparison, since that's what
the per-run cycle counts average against).

Three checks, same structure as the step 1 confound analysis:

1. **Direction test.** The step 1 investigation already established
   that clock-gap magnitude does *not* reliably predict delta magnitude
   in this system: the far more extreme 52.6% gap (2.088 GHz vs
   4.406 GHz, the invalidated Low-Power-Mode run) produced a *smaller*
   Add_real delta (16.20%) than the near-matched 1.58% gap did once
   corrected (19.84%) -- the opposite of what a simple "lower clock
   inflates the win" model predicts. That same skepticism carries
   forward here: there's no established basis for treating a 1.5%
   gap as a reliable predictor of bias direction or size in this
   dataset, only as something to rule out on its own terms per variant.
2. **Margin test.** Six of eight variants clear their threshold by
   4.7x-17x -- no plausible few-percentage-point clock effect closes
   that gap. `Reduce_shuffled` (1.88x) and `Add_real` (1.09x) are the
   two worth real scrutiny; `Add_real` especially, since its margin is
   barely above 1x and its delta (-3.25%) is the same order of
   magnitude as the clock gap itself (1.54%). This is the one case
   this step where "probably not enough to matter" needed checking
   rather than asserting.
3. **Direction-of-bias test.** step2 ran at the *lower* mean clock
   (4.270 vs 4.336 GHz). Per the confound mechanism established in step
   1 (a lower clock reduces the cycle cost of a fixed-nanosecond memory
   stall), any residual bias from this gap would make step2 look
   *artificially faster* than it truly is -- i.e. it would push
   deltas in the *positive* direction. `Add_real`'s delta is
   **negative** (a regression). A bias that pushes toward "step2 looks
   faster" cannot be manufacturing a result that instead shows step2
   *slower* -- if anything, the true regression is very slightly
   understated by this gap, not created by it. This is the same
   argument that made step 1's `Reduce_traversal` regression more
   credible, not less, and it applies identically here.

**Conclusion**: the frequency gap does not explain `Add_real`'s
regression -- if it has any effect at all, it argues the regression is
slightly larger than measured, not smaller or spurious. `Add_real`'s
thin margin means this result should get an N=20 re-check before being
treated as fully settled (extending the same standing rule step 1
established for Cancel-sized claims to any result this close to 1x),
but the direction and rough size are not a clock artifact.

## What's actually driving Add_real's regression: the churn data explains it directly

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

## Interpreting the L1D refill counter (new this step)

| Variant | l1d refills/op | cycles/op | refills as % of cycles |
|---|---:|---:|---:|
| Add_real | 2.377 | 259.6 | 0.92% |
| Add_widened | 2.879 | 402.3 | 0.72% |
| Cancel_traversal | 0.162 | 174.3 | 0.09% |
| Cancel_shuffled | 0.242 | 167.7 | 0.14% |
| Reduce_traversal | 0.152 | 75.8 | 0.20% |
| Reduce_shuffled | 0.146 | 82.3 | 0.18% |
| Replace_traversal | 0.685 | 532.4 | 0.13% |
| Replace_shuffled | 0.947 | 514.3 | 0.18% |

Add_real's ~2.38 refills/op against Cancel's ~0.16-0.24/op is roughly a
10-15x spread by this N=10 mean (a single run can read closer to the
25x figure quoted going in, given each is its own noisy PMU sample --
the 10-run mean is the number to trust). Either way the direction and
rough scale is the same, and the mechanism is straightforward:

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
identical, low refill rates (0.152 vs 0.146/op, ~4% apart -- far
tighter than Cancel's 0.162-vs-0.242 gap or any of the Add/Replace
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
best case" and "what a real, churny order book actually achieves" --
worth keeping in mind for step 3 onward (a book that runs longer, or a
symbol with heavier quote-churn, would likely accumulate more stranded
levels over time, though never more than would have been allocated
under step 1's design anyway). Not treated as something to fix in this
step; noted here so a future step doesn't rediscover it from scratch.

## Bottom line

- **Correctness**: unaffected by anything in this document --
  `RunDifferential<OrderBook, OptimizedOrderBook>`, the new
  `LevelOrders` unit tests, and mutation testing were already verified
  before this benchmark ran (see the prior turn's diff review).
- **Performance**: 7 of 8 variants show large, well-margined wins
  (Cancel +26-30%, Replace +17%, Add_widened +25%, Reduce +2-5%).
  `Add_real` regresses -3.25%, mechanistically explained by the churn
  data (deferred allocation cost landing inside this pass's measurement
  window rather than during warmup) and not attributable to the ~1.5%
  clock gap, which if anything argues the regression is understated
  rather than manufactured. Recommend an N=20 re-check on `Add_real`
  specifically before calling it fully settled, given its thin (1.09x)
  margin.
- **L1D refills**: corroborate the inline-storage mechanism (low refill
  rate exactly where no allocation happens, high rate exactly where new
  tree nodes or deferred allocations occur) without retroactively
  explaining step 1's separate, still-open `Reduce_traversal` question
  -- that data doesn't exist for step 1's binary and this document
  doesn't pretend otherwise.
- **Churn mechanism**: confirmed exactly (not assumed) via a dedicated,
  sudo-free reproduction -- 17 genuinely-persistent overflow levels plus
  18 "stranded" ones inherited from warmup's real churn history, exactly
  accounting for all 3860 frees. Surfaced a real, bounded, previously
  undocumented edge case in `LevelOrders`' "never downgrade" design,
  worth carrying into step 3's own churn checks rather than rediscovering.
