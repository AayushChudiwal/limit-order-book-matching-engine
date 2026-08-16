# Benchmark methodology: reference-engine baseline

This is the record of how `bench_matching_engine` arrived at its current
design, what the noise floor is, and what Phase 5 can and can't honestly
claim against this baseline. Three different measurement designs were
tried and rejected before this one; that history is kept here rather than
silently erased, because the reasons they failed are exactly the
reasons the current design looks the way it does.

## Platform

MacBook Air, Apple M4 (4 P-cores + 6 E-cores), macOS. `bench_matching_engine`
must run under `sudo` (the PMU cycle counter requires root -- see
`docs/pmu_validation.md`). Apple Silicon exposes no thread-affinity API
(no `isolcpus`/`sched_setaffinity` equivalent) and no controllable or
reliably-readable target CPU frequency:

- **No core pinning.** `QOS_CLASS_USER_INTERACTIVE` is set as a
  scheduling hint (biases toward P-cores) but the OS can still migrate
  the thread. Every timed pass records whether the CPU core index
  (`pthread_cpu_number_np`) changed between the start and end of its
  bracket and reports `[MIGRATED]` if so -- across all 10 baseline runs
  and both symbols checked, this fired exactly once (`Replace_traversal`
  on one TVIX run), so contamination from migration appears rare on this
  workload, not absent.
- **No frequency control.** Each run calibrates its own GHz fresh (races
  the cycle counter against `mach_absolute_time` over a busy loop -- see
  `lob::bench::CalibrateGigahertz`) rather than trusting a cached value.
  Across the 10 baseline runs this ranged 4.376-4.439 GHz (mean 4.406,
  ~1.4% spread) -- real thermal/DVFS variance, part of what the reported
  noise floor below is actually measuring.

## The measurement instrument: three designs, two rejected

**Design 1: per-op `mach_absolute_time()`.** Rejected. At this engine's
real operation size (tens of ns), the 41.7ns/24MHz timer tick quantizes
every reported number to an exact multiple of itself -- Add's measured
p50 was *one tick*. Not a resolution problem at the margins; the
histogram was unusable.

**Design 2: per-op PMU reads.** Rejected. `PmuCounters::Read()` costs
~2688 cycles (~628ns) median on this M4 (dominated by
`kpc_get_thread_counters()`, a kernel round-trip -- confirmed by fixing
an unrelated per-call heap allocation in `Read()` and finding it barely
moved the number) against a ~3090-cycle measured op -- ~87% pure
instrumentation overhead. Batching ops together to amortize this made it
worse in a different way: achieved batch size varied *by op type*
(1.63 for Add, 1.52 for Reduce, 1.22 for Replace, since real ITCH streams
don't run long stretches of one message type), meaning the overhead
contribution wasn't just large, it was inconsistent across the exact
categories being compared -- disqualifying for a measurement meant to
support Phase 5 before/after comparisons.

Direct EL0 PMC register reads (bypassing `kpc_get_thread_counters()`
entirely, the obvious fix if it worked) were investigated and found not
to work on stock macOS: Apple's PMC registers are not EL0-accessible
without a **patched kernel** (confirmed via the PacmanPatcher project),
which requires disabling SIP and is out of scope for what `sudo`
authorizes here. Even under a patched kernel, the control register "may
be repeatedly overwritten by a kernel process... typically won't persist
longer than ~100µs" -- unstable even in the best case. `kpc_get_thread_counters()`
going through the kernel is not a workaround-able inefficiency on this
platform.

**Design 3 (current): PMU aggregate, ONE bracket per (op-type, variant).**
`PmuCounters::Read()` is called exactly twice per benchmarked variant --
once before, once after a tight, type-pure, back-to-back loop of real
engine calls. No interleaving of other op types inside the timed window,
no per-op reads, no batching decision. This gives a precise **mean
cycles/op**, not a per-op tail distribution. That's a real limitation,
stated plainly: **per-op tail latency is not measurable on this platform
at this operation size.** Neither available instrument supports it --
`mach_absolute_time` because its granularity exceeds the operation size,
PMU because its own read cost exceeds the operation size. A future
optimized (Phase 5) engine with larger fixed per-op overhead, or a
platform with a cheaper cycle-read path, could reopen this; this baseline
doesn't have it, and no percentile numbers are published pretending
otherwise.

## Warmup and book depth

Original plan: warm up a book with a fixed message count, then use
`FullBook()`'s currently-resting orders as the pool for Cancel/Reduce/Replace.
A fixed 20,000-message cutoff left only **33 resting orders** for SPY on
PSX -- far too few to trust a mean over. Retuning to stop warmup once
resting depth reached a target (3000) instead of a fixed message count
didn't fix it: a scan across the top 150 most-active symbols on NASDAQ
(no PMU needed, pure book-building, see the depth-check methodology
below) found the **deepest candidate anywhere in that pool reaches only
432** resting orders (TVIX). SPY plateaus at 240 (NASDAQ) / 113 (PSX)
even after replaying the majority of each symbol's daily message history.
This is consistent across two different venues and dozens of symbols, so
it reads as a genuine property of 2019-era lit-market order flow --
very high cancel/requote rates mean most quote volume is fleeting, not
resting size -- rather than a bug or a single-venue artifact. (It can't
be fully separated from the NASDAQ file's truncation -- see below -- but
PSX's SPY history is a complete day and shows the same order-of-magnitude
plateau.)

Given that, no realistic warmup will ever produce "thousands" of resting
orders to draw a percentile from. The design changed to: **cycle** a
smaller real pool -- repeat [untimed re-add/reset to restore the pool] →
[timed batch over the full pool] for enough rounds to reach 30,000 total
ops. Only the batch itself is timed; replenishing between rounds
(re-adding cancelled orders, resetting reduced quantities via
`ModifyOrder`, resetting replaced orders' id/price via cancel+re-add) is
not -- the same separation the `low-latency-matching-engine` benchmark
harness uses for its own resting-order pool ("resolve targets ... before
timing; `RunOp()` contains only the matching operation being measured" --
found via a deliberate inspiration search of other open-source LOB
benchmark harnesses, not copied, adapted to this project's own
warmup/depth constraints).

Warmup itself stops at the FIRST of: target depth (3000) reached, a
400,000-message hard cap, or **30% of the symbol's total real Add
messages reserved** for the untimed `Add_real`/`Add_widened` slice
(whichever fires first). That reserve exists because the first NASDAQ
run hit an interaction bug: since depth 3000 is unreachable, warmup
without a reserve just runs to EOF and consumes every real Add message,
leaving nothing for the Add slice (`Add_real`/`Add_widened` both reported
"no ops available"). Every printed run states explicitly which condition
stopped warmup.

## Correctness dataset vs performance dataset

**PSX (`data/20190730.PSX_ITCH_50`), unchanged, complete day**: stays
the correctness dataset -- referential-integrity checking, the
independent-implementation cross-validation, and the message-mix
documentation from Phase 2 are all unaffected by anything in this
document.

**NASDAQ (`data/07302019.NASDAQ_ITCH50.partial`)** is the performance
dataset used for this baseline. It is **not a complete day** -- the
download died mid-transfer (DNS resolution failures against
`emi.nasdaq.com` after three partial-range retry attempts; see
`data/nasdaq_range_download.log`), landing at 247MB of the compressed
367MB total, roughly the first ~67% of the trading day. A full
re-download was attempted but is NASDAQ-throttled to the point of being
impractical to wait on synchronously (~9 hour estimate at observed
throughput); if it completes, this baseline should be redone against the
complete day as a refinement. This baseline stands on its own with that
limitation stated, not hidden.

## Symbol selection

**SPY (stock_locate 7397)**, primary: continuity with the correctness
dataset's symbol, and the 2nd-deepest candidate in the top-150-by-Add-activity
scan (240 resting / 191 levels on NASDAQ).

**TVIX (stock_locate 7968)**, cross-check: the single deepest real
candidate found (432 resting / 273 levels) -- chosen by resting depth,
not message/Add count, since the two don't correlate (MT, SAP, UL had
far more Add activity than SPY but shallower final depth). A single run
against TVIX (not a full N=10 spread) confirms SPY's numbers aren't a
one-off: every op type lands within the same order of magnitude
(Cancel ~257-264c, Reduce ~85-88c, Replace ~769-772c on TVIX vs
Cancel ~245-262c, Reduce ~78-85c, Replace ~671-703c on SPY across the 10
baseline runs).

## Workload per variant

Each variant starts from a **fresh copy** of the same warmed-up book (a
real, untimed replay of the target symbol's early message history), so
all variants share an identical starting state:

- **`Add_real`**: a real, unmodified, contiguous slice of later Add
  messages for this symbol (up to 150,000, or whatever the 30% reserve
  provides if smaller) -- tight price clustering around the real mid,
  the favorable case for a flat price array.
- **`Add_widened`**: the SAME slice, with each order's distance from the
  pass's own live mid scaled by 10x -- synthetic, labeled as such,
  bounding how much of any future flat-array win depends on that tight
  real-world clustering.
- **`{Cancel,Reduce,Replace}_traversal`**: driven against orders resting
  in the warmed book, in `FullBook()`'s natural price-then-arrival
  order -- best-case cache locality, labeled as such because it's
  exactly the property a flat array / arena allocator (Phase 5) is meant
  to improve.
- **`{Cancel,Reduce,Replace}_shuffled`**: the SAME resting orders, order
  randomized with a fixed, committed seed (`0xB0BACAFE`) -- via a
  hand-rolled Fisher-Yates over raw `mt19937_64` output, not
  `std::shuffle`/`std::uniform_int_distribution`, whose algorithms are
  implementation-defined even given an identical seed (the same
  portability reasoning Phase 3's fuzz generator settled on).
- **Replace's new price** is `resting_price + delta`, where `delta` is
  sampled from the empirical distribution of this symbol's REAL Replace
  price deltas extracted from the file (via a lightweight
  order_ref→price map tracked alongside the warmup replay, not
  invented) -- so the aggressive-vs-passive split (does the new price
  cross the spread?) is what the real data determined, not a chosen
  knob.

**Stated plainly**: Cancel/Reduce/Replace are driven against real
resting orders rather than replaying the literal historical
cancel/reduce/replace sequence. A real historical subsequence doesn't
work standalone here -- a cancel references an order that must exist,
and skipping the interleaved Adds needed to isolate cancels would make
most of them no-ops against missing orders. This is a deliberate,
defensible tradeoff, not an oversight.

## Noise floor: 10 runs, SPY on NASDAQ

All 10 runs replayed **identical setup** (verified in each run's log,
not assumed): 45,911 warmup messages, depth reached 133 / 110 levels,
20,886 of 29,837 total Adds used in warmup, 8,951 reserved for the Add
slice, 30,058 total ops per Cancel/Reduce/Replace variant (226 cycling
rounds). The spread below is real run-to-run measurement noise, not
workload drift.

Minimum reliably detectable improvement assumes Phase 5 collects its own
N=10 runs with similar variance, compared against this baseline's N=10:
SE of the difference ≈ stdev·√(2/10), threshold ≈ 2·SE ≈ 0.894·CoV%.

| Variant | mean (cycles) | min | max | CoV | min. detectable improvement |
|---|---:|---:|---:|---:|---:|
| Add_real | 313.6 | 304.5 | 328.4 | 2.37% | 2.12% |
| Add_widened | 608.7 | 589.3 | 633.1 | 2.31% | 2.07% |
| Cancel_traversal | 254.9 | 240.6 | 295.2 | 5.94% | 5.32% |
| Cancel_shuffled | 258.0 | 245.0 | 296.2 | 5.59% | 5.00% |
| Reduce_traversal | 78.3 | 77.2 | 80.6 | 1.30% | 1.16% |
| Reduce_shuffled | 83.6 | 82.1 | 85.5 | 1.39% | 1.24% |
| Replace_traversal | 703.5 | 689.6 | 794.6 | 4.58% | 4.10% |
| Replace_shuffled | 681.8 | 670.5 | 753.5 | 3.73% | 3.34% |

**Cancel and Replace carry the most noise** (~4-6% CoV) — a Phase 5
improvement below roughly 5% on these specific variants would be at real
risk of being indistinguishable from run-to-run variance, and shouldn't
be claimed without its own N≥10 comparison. Add and Reduce are
noticeably tighter (~1-2% CoV) and can resolve smaller genuine
improvements.

One run (run 5) shows elevated cycles across *multiple* variants
simultaneously (Cancel_traversal 295.1c vs a ~255c typical, Replace both
variants ~13% above typical) without tripping the `[MIGRATED]` flag on
those specific brackets -- consistent with some run-level system noise
(background activity, thermal state) the coarse per-round core-check
doesn't catch. It's included in the statistics above, not excluded,
since discarding an inconvenient run without a principled reason would
understate the real noise floor.

## Add_real vs Add_widened: does it survive the noise?

Yes, clearly. The ~2x gap survives run-to-run variance with room to
spare -- this is the most interesting result in the project so far, and
it's real, not an artifact:

| run | Add_widened / Add_real |
|---|---:|
| 1 | 1.925 |
| 2 | 1.927 |
| 3 | 2.026 |
| 4 | 1.925 |
| 5 | 1.928 |
| 6 | 1.911 |
| 7 | 1.972 |
| 8 | 1.890 |
| 9 | 1.991 |
| 10 | 1.919 |

Ratio ranges 1.89-2.03 (mean ~1.94x) across all 10 runs -- a ~7% relative
spread in the ratio itself, against each side's own ~2.3% individual
CoV. Widening the real order flow's price dispersion 10x around the
live mid costs roughly double, on the unoptimized reference engine. How
much of that gap survives Phase 5's flat array is exactly the question
this variant exists to answer later.

## Traversal vs shuffled: nuanced, not simply inconclusive

The direction of the traversal-vs-shuffled gap is **consistent across
all 10 runs for all three op types** (sign never flips) -- unlikely by
chance alone (p ≈ 0.001 for 10/10 agreement under a fair-coin null). But
the *magnitude* differs meaningfully by op type, and for one op type it's
the opposite of what "traversal = better locality = faster" predicts:

- **Reduce**: traversal consistently ~4.6-8.6% faster than shuffled (10/10
  runs) -- a real, moderate effect, above this variant's own ~1.3% CoV.
- **Cancel**: traversal consistently faster, but only by ~0.04-2.64%
  (10/10 runs) -- directionally real (paired within-run comparisons
  cancel common noise like run 5's system-wide elevation, which is why
  the sign holds even though the *absolute* per-variant CoV is ~5-6%),
  but small enough that it sits at or below the noise floor for an
  independent-run comparison.
- **Replace**: traversal is consistently ~2.4-5.5% *slower* than
  shuffled (10/10 runs) -- reversed. Not yet explained; a plausible
  factor is that Replace's new price comes from a real sampled delta
  decorrelated from the input traversal order, so starting in
  price-sorted order buys little once the operation itself scatters
  the result across the tree. Flagged for investigation, not asserted.

Overall: on this **unoptimized reference engine** (`std::map` + `deque`,
pointer-chasing regardless of visit order), traversal-order vs
shuffled-order shows only a small-to-moderate difference. That's the
useful baseline reading -- there isn't much cache locality here to lose
in the first place. If Phase 5's flat array / arena allocator shows a
*much larger* traversal-vs-shuffled gap than this baseline did, that's
direct evidence the optimization created real, newly-exploitable
locality, not just an artifact of how the benchmark is built.

## Reproducing this

```
sudo ./build/bench_matching_engine data/07302019.NASDAQ_ITCH50.partial bench/baseline_spy.csv SPY
sudo ./build/bench_matching_engine data/07302019.NASDAQ_ITCH50.partial bench/baseline_tvix.csv TVIX
```

Both are fully deterministic given the fixed seed and the (fixed, given
the same file) real data -- confirmed by the 10 SPY runs replaying
identical warmup/op counts every time. Run-to-run differences are pure
measurement noise, captured in the table above.
