# Phase 5 step 4 results: arena allocation, and the O(n) walk it exposed

**Step 4 landed as TWO changes, and this document reports them
separately wherever the evidence allows.** That is deliberate: the net
number alone would be misleading in both directions.

1. **`4b0e71e` -- arena-allocated order storage.** `LevelOrders`'
   overflow becomes fixed-capacity chunks taken from an arena the book
   owns, addressed by generation-tagged handles, instead of a
   `std::unique_ptr<std::deque<RestingOrder>>`.
2. **`e261bbb` -- incremental level totals.** `SumLevel` stops walking a
   level's orders on every mutating operation and reads a maintained
   total in O(1).

The second is not scope creep bolted on afterwards. The arena benchmark
regressed `Add_real` by 15.8%, and the investigation into *why* found
that the engine had been recomputing each level's aggregate quantity on
every add, cancel, reduce and fill since long before step 4. The arena
did not create that cost -- it multiplied it, by making each step of the
walk more expensive. Fixing the walk was the durable repair; making the
iterator merely cheaper would have left an O(depth) term sitting in the
hot path.

## The attribution: one curve explains both results

This is the central evidence of step 4 and the reason the two changes
can be reported apart despite being measured together.

Sudo-free wall-clock probe (`tools/add_depth_probe.cpp`, committed so
this table stays checkable), 110 price levels
matching the warmed SPY book, 9,000 timed adds, **interleaved**
A/B/C over 7 passes with each binary's minimum taken. Interleaving is
load-bearing here, not a detail -- see `docs/benchmark_methodology.md`,
"Measurement confounds this project has actually hit".

| orders already at the target level | step 2 (deque) | step 4 arena alone | step 4 + incremental sum |
|---:|---:|---:|---:|
| 0 | 44.80 ns | 65.66 ns (+47%) | **20.44 ns (-54%)** |
| 1 | 44.93 ns | 66.08 ns (+47%) | 19.92 ns (-56%) |
| 2 | 44.58 ns | 66.94 ns (+50%) | 19.73 ns (-56%) |
| 4 | 45.79 ns | 69.44 ns (+52%) | 19.43 ns (-58%) |
| 8 | 47.35 ns | 74.02 ns (+56%) | 19.36 ns (-59%) |

**Read the trend down each column, not just the values across a row:**

- **step 2 grows with depth** (44.80 -> 47.35). Expected: `SumLevel`
  walks the level, so cost scales with how many orders are on it. A
  cheap per-element iterator keeps the slope shallow.
- **step 4's arena grows FASTER** (65.66 -> 74.02). Same O(n) walk, but
  each element now costs more to reach: the chunk iterator carries an
  arena pointer plus an 8-byte handle and resolves `arena.Get()` per
  step, where step 2 walked a `std::deque` directly. Depth multiplies
  the penalty instead of merely adding to it.
- **with the incremental sum the curve goes FLAT** (20.44 -> 19.36,
  drifting slightly *down*). That flatness is the signature of the walk
  disappearing: an O(1) read cannot scale with depth. It is the clearest
  single piece of evidence in this step.

**One curve explains both of the benchmark's contradictory-looking
results.** `Add_real` clusters onto prices that already hold an order
(85% of its adds, per `docs/phase5_step2_results.md`'s churn analysis),
so its adds land on deeper levels and pay the multiplied walk -- it
regressed. `Add_widened` scatters 10x wider into territory that is
mostly untouched, so its levels stay shallow and it collects the arena's
allocation win without the walk penalty -- it gained 23%. Same code,
opposite outcomes, one mechanism.

### Confirmed as compute, not memory, before the mechanism was proposed

The hypothesis above was checked against the PMU counters before being
written down, because a plausible story about a performance result is
exactly the kind of thing that is easy to believe and hard to falsify
afterwards.

| Variant | instr/op step 2 | instr/op step 4 arena | delta | IPC step 2 | IPC step 4 |
|---|---:|---:|---:|---:|---:|
| Add_real | 889.5 | 1056.9 | **+18.8%** | 3.43 | 3.52 |
| Add_widened | 1042.3 | 777.7 | -25.4% | 2.58 | 2.50 |
| Cancel_traversal | 847.1 | 792.1 | -6.5% | 4.82 | 4.85 |
| Reduce_traversal | 296.0 | 303.6 | +2.6% | 3.89 | 3.86 |
| Replace_traversal | 1628.5 | 1484.2 | -8.9% | 3.05 | 2.96 |

`Add_real`'s cycles moved +15.8% against instructions +18.8%, with IPC
essentially unchanged. Cycles track instruction count across every
variant. So the regression was **more work executed**, not more stalling
-- which also rules out the obvious alternative explanation, since
`Add_real`'s L1D refills went *down* 19.5% over the same change. A
memory-locality story cannot explain a regression whose cache-miss
counter improved.

## Intermediate data: the arena alone (NOT PUBLISHABLE)

Reported only to attribute which change did what. **These numbers must
not be cited as step 4's result**, for two independent reasons:

1. **The run set fails this project's own cleanliness rule.**
   `docs/phase5_plan.md` requires a confirmed P-core clock (~4.3-4.5 GHz
   on this M4). These 10 runs averaged **4.171 GHz**, range 4.011-4.236,
   with a monotonic thermal ramp from run 1 upward -- the machine was
   cold. Every run in the set is below the band.
2. **It measures a state that was never committed as final**, since the
   incremental-sum fix landed immediately afterwards.

Preserved as `bench/step4_arena_only_cold_run_{1..10}.csv` / `.log`
rather than `step4_spy_run_*`, because the latter names were reused by
the warm N=20 collection -- these files are the only copy of the
arena-only state and the doc would otherwise cite numbers nothing in the
repo can produce.

N=10 against step 2's committed N=20 baseline:

| Variant | step 2 N=20 | arena-only N=10 | delta | threshold | margin |
|---|---:|---:|---:|---:|---:|
| Add_real | 259.10 | 300.04 | **-15.80%** | 1.60 | 9.85x |
| Add_widened | 404.09 | 311.16 | +23.00% | 1.09 | 21.19x |
| Cancel_traversal | 175.59 | 163.37 | +6.96% | 2.08 | 3.35x |
| Cancel_shuffled | 169.86 | 158.46 | +6.71% | 1.66 | 4.04x |
| Reduce_traversal | 76.06 | 78.66 | -3.41% | 1.49 | 2.29x |
| Reduce_shuffled | 82.51 | 84.75 | -2.71% | 1.17 | 2.31x |
| Replace_traversal | 533.97 | 501.92 | +6.00% | 1.05 | 5.71x |
| Replace_shuffled | 515.20 | 487.60 | +5.36% | 1.15 | 4.67x |

**Which way the 3.01% clock gap cuts.** step 2's baseline averaged
4.3005 GHz against this set's 4.1709 -- a 3.01% gap, twice the 1.54%
step 2 spent a section justifying. Per the mechanism established in
`docs/phase5_step1_results.md`, a lower clock makes a fixed-nanosecond
memory stall cost fewer cycles, so the slower-clocked side looks
artificially faster. That means:

- the **wins here are inflated** and some of them may not survive a
  clock-matched comparison, and
- the **regressions are understated**. `Add_real`'s -15.80% is a
  **floor, not a ceiling** -- at matched clock it would be worse.

That direction is why the regression was taken seriously rather than
attributed to the confound: a bias pushing toward "looks faster" cannot
manufacture a result showing "slower".

## What the L1D counter shows, and why it survives the clock problem

The refill numbers are the most robust result in the intermediate set,
because a 3% clock difference cannot manufacture a change of this size
in a cache-miss counter:

| Variant | step 2 refills/op | arena-only refills/op | change |
|---|---:|---:|---:|
| Cancel_traversal | 0.157 | 0.038 | **-75.6%** |
| Replace_shuffled | 0.927 | 0.354 | -61.8% |
| Replace_traversal | 0.690 | 0.305 | -55.9% |
| Cancel_shuffled | 0.240 | 0.116 | -51.5% |
| Add_real | 2.379 | 1.916 | -19.5% |
| Add_widened | 2.877 | 2.615 | -9.1% |
| Reduce_traversal | 0.172 | 0.163 | -5.2% |
| Reduce_shuffled | 0.159 | 0.195 | +23.2% |

This is the arena's actual thesis, confirmed in the counter that was
added to test it. Reused arena slots stay warm; fresh `malloc`
allocations did not. The largest drops are exactly where the reuse rate
is highest -- Cancel and Replace churn levels hardest
(`bench/step2_churn_spy.csv`: Replace does ~1.5 level lifecycle events
per op, Cancel ~0.83), so they recycle the same slots repeatedly instead
of touching new memory each time.

`Reduce_shuffled`'s +23.2% is the one increase, on a near-zero base
(0.159 -> 0.195 refills/op, ~0.2% of its cycles either way). Both Reduce
variants sit two orders of magnitude below Add's rate because Reduce
allocates nothing at all (`order_storage_allocs = 0`); at that scale the
counter is measuring noise, not a locality change, and it is not
evidence of anything.

## Still to come

- **N=20 headline comparison** on a warm machine, with both changes in
  place, against step 2's N=20 baseline. N=20 rather than N=10 because
  the intermediate set put Reduce at 2.29x/2.31x margins and Replace's
  thresholds near 1.05 -- thin enough that `docs/benchmark_methodology.md`'s
  "N=10 isn't always enough headroom" rule applies.
- **Churn comparison** against `bench/step2_churn_spy.csv`, confirming
  level create/destroy counts are unchanged (the standing rule) and
  reporting what happened to order-storage acquisition counts.
- **Untagged ASan nightly** green on the commit being landed, per
  `docs/phase5_plan.md`'s standing requirement for ownership-changing
  changes.
