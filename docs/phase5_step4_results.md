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

## Headline result: N=20, warm machine, both changes in

All 20 runs verified before any number was read: `arena generation tags:
OFF` present in every log, `LOB_TRACK_CHURN` off, identical warmup
(45,911 messages, 133 resting orders across 110 levels) in all 20. One
`[MIGRATED]` bracket, included rather than discarded, per this project's
standing rule against dropping inconvenient runs.

Compared against step 2's committed N=20 baseline. Threshold is
`2 * sqrt(s1^2/n1 + s2^2/n2)` as a percentage of the step 2 mean -- the
full SE-of-the-difference formula, not the `0.894 * CoV%` shorthand,
which is only valid for equal-N with a shared variance estimate (see
`docs/benchmark_methodology.md`).

| Variant | step 2 N=20 | step 4 N=20 | min | max | CoV% | delta % | threshold | margin |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Add_real | 259.10 | 180.73 | 175.50 | 194.52 | 2.60 | **+30.25** | 1.49 | 20.26x |
| Add_widened | 404.09 | 281.24 | 269.97 | 299.32 | 3.17 | **+30.40** | 1.35 | 22.45x |
| Cancel_traversal | 175.59 | 165.77 | 162.48 | 172.28 | 1.31 | +5.60 | 1.64 | 3.42x |
| Cancel_shuffled | 169.86 | 158.02 | 155.79 | 161.50 | 0.89 | +6.97 | 1.52 | 4.58x |
| Reduce_traversal | 76.06 | 72.94 | 70.80 | 75.00 | 1.49 | +4.10 | 0.84 | 4.88x |
| Reduce_shuffled | 82.51 | 77.80 | 76.09 | 80.07 | 1.31 | +5.71 | 0.72 | 7.90x |
| Replace_traversal | 533.97 | 497.66 | 490.23 | 516.60 | 1.31 | +6.80 | 1.01 | 6.73x |
| Replace_shuffled | 515.20 | 482.42 | 477.21 | 490.84 | 0.72 | +6.36 | 1.05 | 6.06x |

**Eight of eight variants improve, all significantly, none by a thin
margin.** The smallest is Cancel_traversal at 3.42x; every other variant
clears by 4.5x or more. This is the first step in Phase 5 with no
regression anywhere.

### Did the incremental sum fix Add_real? Yes, decisively.

`Add_real` was the headline question, and the answer is unambiguous:

| | Add_real vs step 2 |
|---|---:|
| arena alone (cold set, intermediate) | **-15.80%** |
| arena + incremental sum (this set) | **+30.25%** |

A 46-point swing on the variant the fix was aimed at. The regression is
not merely absorbed; `Add_real` went from step 4's worst variant to
tying `Add_widened` for its best. That is what the depth-trend table
predicted: `Add_real`'s adds land on already-occupied levels, so it was
paying the multiplied O(n) walk hardest and had the most to gain from
removing it.

### Did Reduce's regressions survive? No -- they were the same cause.

| Variant | arena alone (cold) | arena + incremental sum |
|---|---:|---:|
| Reduce_traversal | -3.41% (2.29x) | **+4.10%** (4.88x) |
| Reduce_shuffled | -2.71% (2.31x) | **+5.71%** (7.90x) |

Both flipped from significant regressions to significant improvements.
Worth being precise about the cause, because "clock artefact" was the
tempting explanation and it is **not** the right one: the clock gap
moved in the direction that *flatters* step 4, so it was making those
regressions look smaller, not creating them. What actually removed them
was the incremental sum. Reduce calls `OnBookUpdate` after every
operation and allocates nothing at all
(`order_storage_allocs = 0`) -- so it got none of the arena's allocation
benefit and paid the full cost of the fattened walk. Once the walk is
gone, the arena's remaining effect on Reduce is the locality improvement
(L1D refills -28.9% / -24.4%), and that shows up as a real gain.

### Clock: gap now 0.47%, and run 1 changes nothing

| Run set | N | mean clock | gap vs step 2 |
|---|---:|---:|---:|
| step 2 (baseline) | 20 | 4.3005 GHz | -- |
| step 4, all 20 | 20 | 4.2802 GHz | 0.47% |
| step 4, excluding run 1 | 19 | 4.2892 GHz | 0.26% |
| step 4 arena-only cold set | 10 | 4.1709 GHz | 3.01% |

**Run 1 (4.110 GHz) is a genuine cold-start outlier** -- runs 2-20 sit
in a tight 4.243-4.344 GHz band, comfortably inside the P-core range,
while run 1 sits 0.13 GHz below all of them.

**Excluding it changes no classification, and barely moves any number.**
Every delta shifts by at most 0.08 percentage points (Add_widened
+30.40% -> +30.31%, the largest move); every margin stays within 0.3x;
all eight verdicts are identical. It is reported both ways above and
**included** in the headline table, on the same principle that keeps the
`[MIGRATED]` bracket in: a run is not dropped because it is
inconvenient, and here it demonstrably is not.

The 0.47% residual gap still points the same way it always does -- it
flatters step 4 slightly. At this size, against margins of 3.4x-22x, it
cannot account for any of these results. It is also a quarter of the
cold set's 3.01% gap, which is why that set stays marked unpublishable
and this one is the result.

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

## What the L1D counter shows: the arena's thesis, confirmed

This is the most durable result in step 4. Cycle numbers depend on clock
state, thermal conditions and which of the two changes you are looking
at; a cache-miss counter dropping by 80% does not.

| Variant | step 2 N=20 | step 4 N=20 | change |
|---|---:|---:|---:|
| Cancel_traversal | 0.157 | 0.030 | **-80.9%** |
| Replace_shuffled | 0.927 | 0.269 | **-71.0%** |
| Replace_traversal | 0.690 | 0.285 | -58.7% |
| Cancel_shuffled | 0.240 | 0.114 | -52.5% |
| Add_real | 2.379 | 1.245 | -47.6% |
| Reduce_traversal | 0.172 | 0.122 | -28.9% |
| Reduce_shuffled | 0.159 | 0.120 | -24.4% |
| Add_widened | 2.877 | 2.564 | -10.9% |

**Every variant improved**, and the largest drops land exactly where the
arena predicts they should. Cancel and Replace churn price levels
hardest -- `bench/step2_churn_spy.csv` shows Replace performing ~1.5
level lifecycle events per op and Cancel ~0.83 -- so they recycle the
same arena slots over and over instead of touching fresh `malloc`
memory each time. Reused slots stay warm; fresh allocations did not.
That was the entire premise of arena allocation, and this is the counter
that was added to test it.

`Add_widened`'s -10.9% is the smallest improvement, which also fits: it
scatters into genuinely new price territory, so a large share of its
misses are on brand-new `std::map` tree nodes that the arena does not
touch at all. The arena can only help with memory it owns.

### Attribution between the two changes

The arena-only cold set (see "Intermediate data" below) gives the
split. Its refill numbers are usable
even though its cycle numbers are not -- the clock confound acts on
cycles, not on cache-miss counts:

| Variant | step 2 | arena only | + incremental sum |
|---|---:|---:|---:|
| Cancel_traversal | 0.157 | 0.038 | 0.030 |
| Add_real | 2.379 | 1.916 | 1.245 |
| Reduce_traversal | 0.172 | 0.163 | 0.122 |
| Reduce_shuffled | 0.159 | 0.195 | 0.120 |

**The arena delivered most of the Cancel/Replace locality win on its
own** (0.157 -> 0.038 for Cancel_traversal, essentially all of it).
**The incremental sum delivered most of the Add and Reduce win**
(`Add_real` 1.916 -> 1.245; Reduce barely moved under the arena alone,
then dropped ~25-29%). That is the expected division: removing an O(n)
walk stops touching the cache lines that walk was reading, which helps
precisely the variants that walk without allocating.

`Reduce_shuffled`'s arena-only reading (0.159 -> 0.195, +23%) was the one
apparent regression in the cold set. It resolves to 0.120 here, a 24.4%
improvement. On a base two orders of magnitude below Add's rate and
~0.2% of the variant's cycles either way, that intermediate reading was
noise on a near-zero quantity, and treating it as a finding would have
been over-reading the instrument.

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

## Churn: level lifecycle unchanged, and why storage_allocs jumped 4.4x

The standing rule requires level create/destroy counts to be identical to
the prior step's baseline, since a change there would mean the step
altered price-level *lifecycle* rather than storage, and the timing
comparison would no longer be apples-to-apples.

**They are identical -- all eight variants, both counters, exactly.**

| Variant | level_creates | level_destroys | storage_allocs | storage_frees |
|---|---|---|---|---|
| Add_real | 240 = 240 | 84 = 84 | 205 -> **908** | 82 -> 288 |
| Add_widened | 2754 = 2754 | 390 = 390 | 1151 -> **1388** | 174 -> 220 |
| Cancel (both) | 0 = 0 | 24860 = 24860 | 0 = 0 | 3860 = 3860 |
| Reduce (both) | 0 = 0 | 5 = 5 | 0 = 0 | 1 = 1 |
| Replace_traversal | 21812 = 21812 | 23526 = 23526 | 3312 = 3312 | 4503 = 4503 |
| Replace_shuffled | 21398 = 21398 | 22211 = 22211 | 3131 = 3131 | 3515 = 3515 |

`Add_real`'s `order_storage_allocs` rose 205 -> 908 (4.43x), which is
large enough to demand an explanation rather than a shrug -- allocation
rate was the mechanism behind step 2's own `Add_real` story.

**The counter is measuring a different unit, not more work.** Step 2
incremented it once per level that ever reached two orders (the single
`make_unique<deque>`), no matter how deep that level later grew. Step 4
increments it once per *chunk*, and a chunk holds 8 orders. Measured,
not derived -- chunks per level as a function of order count:

| orders at one level | 1 | 2 | 8 | 9 | 17 | 26 | 81 |
|---|---:|---:|---:|---:|---:|---:|---:|
| chunks held | 0 | 1 | 1 | 2 | 3 | 4 | 11 |

So chunks = `ceil(n/8)` for n >= 2, and 0 while the level is still
inline. (The obvious guess, `ceil((n-1)/8)`, is wrong -- the first chunk
absorbs the previously-inline order alongside the new one. Checked
rather than assumed.)

That predicts exactly the pattern observed:

- **Cancel, Reduce and Replace are byte-identical** because they cycle a
  fixed pool -- 133 orders across 110 levels, ~1.2 orders/level. No level
  ever approaches 8 orders, so one chunk per overflow level equals one
  deque per overflow level.
- **The two Add passes diverge** because they only add, never remove, so
  levels accumulate depth throughout the pass.
- **`Add_real` diverges most (4.43x) and `Add_widened` least (1.21x)**,
  and that ordering is itself the clustering signal: SPY's real adds
  concentrate on a few prices, driving those levels deep; `Add_widened`
  scatters 10x wider so its levels stay shallow. The same clustering that
  made `Add_real` regress under the O(n) walk is what makes its chunk
  count high.

**Three reasons this is not a cost regression:**

1. `Add_real` got **30.25% faster** while allocating 4.43x more times. If
   those acquisitions were expensive that could not happen -- an arena
   allocation is a free-list pop, not a `malloc`.
2. Its L1D refills fell **47.6%** over the same change. More
   acquisitions, far fewer cache misses -- exactly what slot reuse
   predicts.
3. **Step 2's counter undercounted its own design.** It saw only the
   `make_unique`. A `std::deque` holding 81 `RestingOrder`s also
   allocates its internal map array and several element blocks, none of
   which were ever counted. So the honest comparison is not "205 mallocs
   vs 908 arena pops" -- step 2's true allocation count was higher than
   205 by an amount this instrumentation was never positioned to see.

One caveat worth stating: the Add passes accumulate orders without ever
removing them, which is an artefact of how the benchmark is built rather
than a property of a real book (which cancels constantly -- Cancel's
24,860 level destroys are the evidence). Real levels do not march to 81
orders deep. The chunk counts above are therefore an upper bound on what
a live book would show, in the same way step 2's warmup/timed seam
inflated its own `Add_real` number.

## Bottom line

- **Correctness**: 171/171 tests in three configurations (Release
  tagged, Release untagged, asserts + ASan/UBSan untagged). Differential
  and invariant sweeps clean at 40 seeds x 20,000 ops. Level lifecycle
  counts identical to step 2, so the timing comparison is
  apples-to-apples per the standing rule.
- **Performance**: eight of eight variants improve significantly, the
  first Phase 5 step with no regression anywhere. Add +30%, Replace
  +6.4-6.8%, Cancel +5.6-7.0%, Reduce +4.1-5.7%. Smallest margin 3.42x.
- **`Add_real`'s regression is gone**: -15.80% with the arena alone,
  +30.25% once the O(n) walk it exposed was removed. A 46-point swing on
  the variant the fix targeted.
- **Reduce's regressions were the walk, not the clock.** They flipped to
  +4.10%/+5.71%. The clock gap was making them look *smaller* than they
  were, not creating them -- an important distinction, since "clock
  artefact" was the tempting reading.
- **L1D refills improved on every variant**, up to -80.9% on
  Cancel_traversal. This is the arena's thesis confirmed in the counter
  built to test it, and it is the result least sensitive to measurement
  conditions.
- **Clock hygiene**: 0.47% gap against step 2 (0.26% excluding run 1's
  cold start), versus the 3.01% gap that made the intermediate set
  unpublishable. Run 1 is reported, retained, and shown to change no
  classification.
- **`storage_allocs` +4.4x on `Add_real` is a unit change, not a cost
  change** -- chunks hold 8 orders where a deque was counted once per
  level. Verified by measuring chunks-per-level directly. The variant
  allocating 4.4x more often also got 30% faster and missed cache 47.6%
  less.

### What this step deliberately did not do

- **The `std::map` nodes are still individually heap-allocated.** Step 4
  arenas order storage only. Step 6 replaces that map wholesale with a
  flat price array, so arena-backing its nodes now would have optimized
  a structure already scheduled for deletion -- and generation tags mean
  nothing where `std::map` owns the dereference.
- **Generation tags are compiled out of the benchmark build.** Every
  number here comes from a binary with `LOB_ARENA_GENERATION_TAGS=OFF`,
  confirmed in all 20 run headers. The tagged build is a different
  program, which is why the untagged ASan nightly remains the gate.
- **`OrderChunk::kCapacity = 8` is calibrated to this data**, where the
  deepest observed real level holds 4 orders. A venue or symbol with
  structurally deeper books would chain more chunks per level; the code
  is correct at any depth, but the constant is a measured choice, not a
  universal one.


## Reproducing this

```
# Benchmark build -- generation tags MUST be off, or the numbers are not
# comparable to step 2's. The run header prints the setting and flags a
# tagged build as not-comparable.
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DLOB_ARENA_GENERATION_TAGS=OFF
cmake --build build-bench -j
for i in $(seq 1 20); do
  sudo ./build-bench/bench_matching_engine \
    data/07302019.NASDAQ_ITCH50.partial bench/step4_spy_run_$i.csv SPY \
    | tee bench/step4_spy_run_$i.log
done

# Churn diagnostic -- separate tree, since LOB_TRACK_CHURN adds real
# overhead to the exact paths being cycle-timed. Its cycle columns are
# not citable and the tool says so in its own output.
cmake -S . -B build-churn -DCMAKE_BUILD_TYPE=Release -DLOB_TRACK_CHURN=ON \
      -DLOB_ARENA_GENERATION_TAGS=OFF
cmake --build build-churn --target bench_matching_engine -j
sudo ./build-churn/bench_matching_engine \
  data/07302019.NASDAQ_ITCH50.partial bench/step4_churn_spy.csv SPY

# Attribution table (no sudo needed) -- see the header of the tool for
# the three-way build recipe and the interleaving requirement.
cmake --build build-bench --target add_depth_probe -j
./build-bench/add_depth_probe
```

Every number in this document comes from committed files:
`bench/step4_spy_run_{1..20}.csv` / `.log` for the headline table,
`bench/step4_churn_spy.csv` for churn,
`bench/step4_arena_only_cold_run_{1..10}.*` for the intermediate
attribution set, and `bench/step2_spy_run_{1..20}.*` for the baseline.

**Check the run header before reading any number.** A stale binary
produced an entire churn comparison during this step's work that matched
step 2 perfectly -- because it *was* step 2's binary. The tell was the
missing `arena generation tags:` line, which was noticed only after the
table had been read as a clean result. Verify `arena generation tags:
OFF`, the absence of the `LOB_TRACK_CHURN=ON` warning, identical warmup
lines, and a P-core clock (~4.3-4.5 GHz) first.
