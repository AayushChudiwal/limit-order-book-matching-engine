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

## The eight steps, in order

1. **Hot-path hygiene** (done, commit `e2e0422`, results in
   `docs/phase5_step1_results.md`). Redundant `std::map` lookup
   elimination in the reference engine itself -- no data structure
   changes.
2. **Singleton/small-size level optimization** (in progress). Real
   levels average ~1.2-1.3 orders on captured PSX/NASDAQ data
   (`docs/benchmark_methodology.md`) -- `LevelOrders` stores the first
   order inline (no heap allocation) and only allocates once a second
   order arrives at the same price. See
   `include/lob/optimized_order_book.hpp`'s class comment for the full
   design and its cache-layout reasoning.
3. **Bitset + find-first-set for best bid/ask.** Replacing (or
   augmenting) the `std::map`'s own `begin()`-is-best-price property
   with a bitset over the tick range, so best-bid/best-ask becomes an
   FFS instruction instead of a tree descent.
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
6. **Flat price array.** Replacing the `std::map<Price, LevelQueue>`
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
