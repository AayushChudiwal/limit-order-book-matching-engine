# Phase 5 plan: optimizing the matching engine

This roadmap existed only in conversation until now -- committing it here
so the ordering and its reasoning, and the standing rules every step
follows, are checkable against the actual commits rather than living
only in chat history.

## Architecture: two engines, not one progressively-mutated one

`include/lob/order_book.hpp`'s `OrderBook` is, by its own class comment,
"deliberately naive, obviously-correct... not the fast engine... exists
to be a ground truth that Phase 3's differential fuzzer checks an
optimized engine against... stays in the repo permanently." Step 1
("hot-path hygiene," commit `e2e0422`) modified it directly -- redundant
map lookups don't change the class's naive, obviously-correct character,
so that stayed within the spirit of "reference engine," even though it
technically touched the file.

Step 2 onward doesn't have that property: a hand-rolled small-size-
optimized container, later an arena allocator, dense internal IDs, a
flat price array, intrusive lists -- these are exactly the kind of
complexity the reference engine's own comment says it should never
carry. So step 2 forks: `include/lob/optimized_order_book.hpp` /
`src/optimized_order_book.cpp` define `OptimizedOrderBook`, starting as
an exact behavioral copy of `OrderBook` as it stood after step 1.
`OrderBook` itself needs no further changes for the rest of Phase 5.
Every step from here on is checked against it via
`RunDifferential<OrderBook, OptimizedOrderBook>` (`lob::fuzz::EngineUnderTest`,
already built generically for exactly this in Phase 3 -- see
`tests/test_fuzz_differential_optimized.cpp`), and `bench_matching_engine`
targets `OptimizedOrderBook` from step 2 onward.

## The steps, in order

Originally eight; step 3 was removed after measurement (see "Step 3:
removed, and why"), leaving seven. The numbering is deliberately left
alone -- steps 4-8 keep their original numbers.

1. **Hot-path hygiene** (done, commit `e2e0422`, results in
   `docs/phase5_step1_results.md`). Redundant `std::map` lookup
   elimination in the reference engine itself -- no data structure
   changes.
2. **Singleton/small-size level optimization** (done, commit `443dfd8`,
   results in `docs/phase5_step2_results.md` -- N=20, 7 of 8 variants
   improved, `Add_real`'s -3.07% regression measured and explained as a
   benchmark measurement-boundary artefact). Real
   levels average ~1.2-1.3 orders on captured PSX/NASDAQ data
   (`docs/benchmark_methodology.md`) -- `LevelOrders` stores the first
   order inline (no heap allocation) and only allocates once a second
   order arrives at the same price. See
   `include/lob/optimized_order_book.hpp`'s class comment for the full
   design and its cache-layout reasoning.
3. ~~**Bitset + find-first-set for best bid/ask.**~~ **REMOVED as a
   standalone step** -- its stated rationale was factually wrong. Folded
   into step 6, where it costs almost nothing. See "Step 3: removed, and
   why" below for the measurements. Steps are NOT renumbered: step 4 is
   still called step 4 everywhere in this repo's history and docs, and
   renumbering would silently invalidate every existing cross-reference.
4. **Arena allocation.** Replacing per-node heap allocation (both for
   price levels and, depending how step 2 lands, order storage) with a
   pre-allocated arena -- the first step where `docs/phase5_readiness.md`'s
   "arena allocator reusing a node still referenced" concern becomes
   real and ASan on the fuzz corpus stops being optional.
5. **Dense internal IDs at the arena boundary, with an open-addressing
   map for external-ref translation.** External order IDs (real,
   caller-supplied, sparse `OrderId`s) get translated to dense internal
   indices at the arena boundary, so the arena itself can be indexed
   directly rather than pointer-chased.
6. **Flat price array** (now also carries the former step 3's bitset).
   Replacing the `std::map<Price, LevelQueue>`
   tree with a flat, tick-indexed array once steps 2-5 establish the
   node/storage layout it will hold. Ordered this late deliberately --
   see `docs/cache_hierarchy_m4.md`'s prefetcher section: Apple
   Silicon's DMP already mitigates some pointer-chasing latency a flat
   array would otherwise win back, and the traversal-vs-shuffled
   instrumentation from the Phase 4 baseline (the unexplained
   Replace-traversal-slower-than-shuffled result) gets reused here to
   check whether the flat array actually creates a locality gap this
   baseline mostly lacks, rather than assuming it will.
7. **Intrusive lists.** Referenced in step 1's own code comments
   (`FindOrderInLevel`'s "giving the scan itself O(1) structure is
   Phase 5's later intrusive-list step, not hygiene") -- turns the
   O(depth) linear scan within a level into O(1) unlink, now that steps
   2-5 have established what a level's storage actually looks like.
8. **Prefetch hints.** Last, and explicitly informed by whatever step
   6's flat-array measurement actually finds about this chip's DMP --
   hand-placed prefetches only where the data says the hardware isn't
   already covering the access pattern.

**Ordering rationale**: highest-impact-per-risk first, and calibrated to
the *measured* shape of this data rather than a generic "deep book"
assumption. Singleton/small-size levels is step 2 (not, say, the flat
array) specifically because the ~1.2-1.3 orders/level finding is
already measured and already tells you where the win is; arena
allocation and dense IDs are ordered before the flat array because the
flat array's own design (what goes in each slot) depends on what steps
2-5 decide a level/order's storage looks like, not the other way
around; the flat array is ordered before intrusive lists and prefetch
hints because those two are refinements *of* whatever the flat array
turns out to need, not independent changes.

## Step 3: removed, and why

Recorded here in full rather than just deleted, because a step removed
*with evidence* is a more useful artifact than a step completed on a
false premise. This section is the evidence.

### The premise was wrong

Step 3 originally read: "so best-bid/best-ask becomes an FFS
instruction instead of a **tree descent**." That phrasing came from the
general LOB literature, where the structure being replaced is typically
a balanced tree whose minimum genuinely costs a descent. It was never
checked against `std::map` on libc++, which is what this engine
actually uses.

**`std::map::begin()` is not a tree descent.** libc++'s `__tree` caches
the leftmost node in its header, so `begin()` is a pointer read.
Measured, not assumed -- same-shape `std::map<Price, ...>`, timing
`m.begin()->first`:

| map size | 10 | 110 | 1,000 | 10,000 | 100,000 |
|---|---:|---:|---:|---:|---:|
| ns/call | 3.117 | 2.615 | 2.312 | 2.371 | 1.396 |

Flat across four orders of magnitude (the mild *downward* drift is
loop/cache noise, not scaling). At this book's measured 110 live levels
that is ~11 cycles. **There is no O(log n) here to remove**, so the
step's entire stated mechanism does not exist.

### Almost nothing in the benchmark even calls it

- `Add_widened` is the ONLY timed variant that calls
  `BestBid()`/`BestAsk()` (`tools/bench_matching_engine.cpp:694-695`) --
  and it is the *synthetic* variant, where those calls are the
  harness's own price-widening arithmetic, not engine hot path.
  "Winning" there would be optimizing the scaffolding.
- `Add_real` and `Replace` touch `opposite.begin()` once per op inside
  `MatchAgainst` (`src/optimized_order_book.cpp:82`), then break on the
  non-crossing check. ~11 of 259 / 534 cycles.
- **`Cancel` and `Reduce` never touch best-price at all** -- and Cancel
  is step 2's single biggest win (+25-29%).

### The maintenance cost is larger than the win, on the variants that matter

A bitset must be updated on every price-level create/destroy. Those
counts are already measured in `bench/step2_churn_spy.csv`. At ~9 cycles
per update (load word, mask, store, plus index arithmetic), against the
N=20 cycle counts and thresholds in `docs/phase5_step2_results.md`:

| Variant | bit-ops/op | added cycles | as % of cycles | threshold % | |
|---|---:|---:|---:|---:|---|
| Cancel_traversal | 0.827 | +7.4 | 4.24% | 1.70% | **regression** |
| Cancel_shuffled | 0.827 | +7.4 | 4.38% | 1.62% | **regression** |
| Replace_traversal | 1.508 | +13.6 | 2.54% | 1.56% | **regression** |
| Replace_shuffled | 1.451 | +13.1 | 2.53% | 1.49% | **regression** |
| Add_widened | 0.351 | +3.2 | 0.78% | 1.55% | below noise |
| Add_real | 0.036 | +0.3 | 0.13% | 2.48% | below noise |
| Reduce (both) | 0.000 | 0 | 0.00% | ~0.97% | below noise |

So the expected outcome of shipping step 3 standalone was: a few percent
on one synthetic variant, nothing measurable anywhere else on the read
side, and a **significant regression on Cancel and Replace** -- undoing
a meaningful share of step 2's largest wins to buy a lookup that was
already a pointer read.

### Why folding it into step 6 is the right home

Step 6 removes the `std::map` entirely in favour of a flat tick-indexed
array. At that point level insert/erase are already array writes, so
maintaining an occupancy bitset alongside them is largely absorbed into
work the step is doing anyway -- and the bitset stops being a redundant
index over a tree that already knows its own minimum, and becomes the
thing that *gives* the flat array an ordered-traversal capability it
otherwise lacks. The cost/benefit inverts precisely because the map is
gone.

### Two findings step 6 must not inherit naively

Both came out of sizing the bitset and are recorded here so step 6
starts from measurement rather than from the SPY-shaped intuition that
produced step 3:

1. **Penny granularity is symbol-specific, NOT venue-wide.** SPY's
   prices are 100% penny multiples (0 sub-penny in 31,435 NASDAQ and
   353,212 PSX priced messages), which makes penny-indexing look safe
   and shrinks the bitset ~100x. It does not generalize. Scanning every
   symbol's Add/Replace prices in both captured files:

   | venue | symbols | priced msgs | sub-penny | symbols w/ any |
   |---|---:|---:|---:|---:|
   | NASDAQ 07/30/2019 | 5,805 | 3,043,132 | 28,292 (0.93%) | 175 |
   | PSX 07/30/2019 | 6,768 | 16,165,067 | 36,717 (0.23%) | 272 |

   The offenders are exactly what Reg NMS Rule 612 predicts -- low-priced
   stocks, where sub-penny quoting is legal below $1.00: KTOV
   ($0.51-$3.14) 36.9% sub-penny across 53,990 messages, MYSZ
   ($0.01-$1.87) **96.3%**, IGLD 98.3%, MTFB 98.2%, TGB 100%. A flat
   array that indexes by penny would collapse distinct price levels onto
   one slot for these symbols. Index by raw tick, or derive the
   increment per symbol and verify it -- never assume 100.

   The existing differential fuzzer covers this specific risk already:
   `src/fuzz/generator.cpp` builds prices as `mid_price_ +/- magnitude`
   in raw ticks, so it naturally emits non-multiples of 100.

2. **`199999.9900` (1,999,999,900 ticks) is a sentinel, not a price.**
   2,625 of NASDAQ's 5,805 symbols report it as their maximum; PSX's
   file has zero. Any min/max-derived array sizing that swallows it
   spans ~2 billion ticks. It must be excluded explicitly.

### The readiness-doc gap this also moves

`docs/phase5_readiness.md` lists "flat array tick-band drift/rebasing"
as caught only "partially, by chance," needing a dedicated price-drift
generator profile. That gap was filed against the flat array. Since the
bitset now arrives with it, the profile is a step-6 prerequisite, not a
later refinement.

## Standing rules, every step

- **One commit per technique.** Each step lands as its own commit (or
  small commit sequence), not bundled with the next step's work.
- **N>=10 baseline after every commit**, comparing against the
  immediately-preceding step's own N>=10 baseline (not just the
  original Phase 4 number) -- report mean/min/max/CoV per variant and
  the delta against each variant's min-detectable-improvement threshold
  (`docs/benchmark_methodology.md`'s `0.894 * CoV%` formula).
  **N>=20 instead of N=10 if the commit's headline claim is Cancel-sized
  or thinner** -- see `docs/benchmark_methodology.md`'s "N=10 isn't
  always enough headroom" section, added after step 1's Cancel result
  cleared its threshold by only ~1.5x.
- **Within-noise results are reported as within-noise**, not folded
  into a win narrative because the sign happened to be positive.
- **Verify the run is actually clean before trusting it**: confirmed
  P-core clock (~4.3-4.5 GHz on this M4, not an accidental Low-Power-Mode/
  E-core session -- see `docs/phase5_step1_results.md`'s account of
  exactly this happening once already), identical warmup/op-count setup
  across all runs in the set.
- **`RunDifferential<OrderBook, OptimizedOrderBook>` must pass** after
  every step, at the volume the step's change plausibly needs to be
  exercised at (e.g. step 2's inline/overflow transition specifically
  needed `AllAddsProfile`'s one-sided-burst behavior, not just the
  default mix, to be genuinely tested -- see
  `tests/test_fuzz_differential_optimized.cpp`).
- **Mutation testing re-run** after every step, confirmed unchanged
  (140/140, 7/7 bugs x 20/20 seeds) -- it exercises the reference
  engine, so a step that only touches `OptimizedOrderBook` shouldn't
  move this number, and confirming that is the check, not an assumption.
- **Churn instrumentation before/after**, wherever a step changes
  allocation behavior (see `include/lob/bench/churn.hpp`): level
  create/destroy counts must stay identical to the prior step's own
  baseline (a change there would mean the step altered price-level
  *lifecycle*, not just storage -- and the timing comparison for that
  step would no longer be apples-to-apples with the step before it,
  since Phase 4's own data already showed level create/destroy ordering
  is a real, measurable-cost-sensitive thing, not a bookkeeping detail);
  order-storage allocation/free counts are the actual signal a step
  claims to move.
