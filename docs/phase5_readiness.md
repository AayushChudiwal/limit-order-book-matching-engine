# Phase 5 readiness: what this harness will and will not catch

Requested directly, answered directly. This is not a hedge -- it's the
list of what to add before trusting a green Phase 5 run on any of these
specific points.

## The four questions, answered

### A flat price array mishandling prices drifting outside the tick band?

**Partially, and not reliably as built today.** `Generator`'s price walk
(`ClusteredRestingPrice`/`AggressivePrice`, mean-reverting around
`mid_price_`) is deliberately tuned for realistic *depth and queueing*
behavior -- clustering tightly so multi-level sweeps and queue priority
actually get exercised (see `docs/message_mix_psx_20190730.md` for why
that mattered). It is not tuned to *drift far* from its starting point,
and a flat array's tick-band boundary is a Phase 5 implementation detail
that doesn't exist yet -- there's no band size to aim the generator at.

If the price walk happens to wander far enough by chance over a very
long run, `RunDifferential` WOULD catch a resulting divergence (the
comparison logic doesn't care why two engines disagree). But "might
happen by chance over a long run" is not the same as "reliably
exercised," and mutation testing didn't include a bug of this shape to
measure against.

**What Phase 5 needs:** once the flat array's band size and rebasing
strategy exist, add a fourth generator profile (or a `Generator`
constructor knob) that deliberately walks `mid_price_` in one direction
by an amount comparable to the band size, forcing rebasing to actually
happen repeatedly within a single run. Until that profile exists, a
band-boundary bug is a **blind spot**, not a covered case.

### An arena allocator reusing a node that's still referenced?

**No.** This is a memory-safety bug, not a logic bug, and nothing in
this harness inspects memory safety. A use-after-free from a
prematurely-freed-and-reused arena slot can easily produce output that
still *looks* plausible on a given run (the reused memory might
coincidentally hold data that doesn't crash a comparison), which is
exactly the failure mode a pure output-comparison harness cannot
distinguish from "correct." `RunDifferential` and `CheckInvariants` both
operate purely on what the engine *reports* through its public interface
-- they have no way to observe that the memory backing that report is
no longer validly owned.

**What Phase 5 needs:** run the SAME fuzz corpus (the generator, the
differential harness, ideally the full `fuzz_soak`-scale sweep) with the
optimized engine built under ASan, the same way `ci.yml`'s `sanitizers`
job already does for the reference engine. ASan will catch a
use-after-free or heap-buffer-overflow from the arena regardless of
whether the corrupted read happens to still produce plausible-looking
output. This is not optional infrastructure to add later -- it is the
specific tool for this specific gap.

> **UPDATE (step 4, arena allocation).** Both halves of this were built,
> and the second one changes what the first is *for*. Read the two
> together, because the original "ASan is the only tool" framing above is
> now only true of one specific build.
>
> **1. The sanitized sweep exists.** `ci.yml`'s `fuzz-sanitized` job
> (per-push, 40 seeds x 2,000 ops, both modes) and
> `fuzz-nightly.yml`'s `sanitized` job (120 seeds x 10,000 ops, both
> modes) run the real fuzz corpus against `OptimizedOrderBook` under
> ASan+UBSan. First full nightly: 2,670 (profile, seed) combinations,
> all clean.
>
> **2. Arena handles carry generation tags, so UAF is no longer
> invisible to the behavioural harness.** A handle is (index,
> generation); freeing a slot bumps its generation; dereferencing
> validates. A stale handle is therefore a detected error *inside the
> engine*, not a silent read of recycled memory -- which means
> `RunDifferential`, `CheckInvariants`, and the plain unit tests all
> catch it, on every push, in seconds. That is a large improvement in
> feedback latency over "wait for a 95-minute nightly to happen to draw
> the right seed shape".
>
> **What this does NOT mean is that the ASan nightly is now redundant.**
> The tags are behind `LOB_ARENA_GENERATION_TAGS`, ON for
> debug/sanitizer/CI builds and **OFF for the benchmark build**, so the
> published cycle numbers never pay for them. The untagged build is
> therefore a genuinely different program from the one CI exercises --
> and it is the one this project publishes performance claims about. It
> deserves independent verification that does not depend on the
> mechanism being compiled out.
>
> So the roles are now: **generation tags are the fast catcher for the
> tagged build; the ASan nightly is the backstop for the untagged,
> release-shaped build.** Neither covers the other's build. The standing
> requirement in `docs/phase5_plan.md` -- untagged ASan nightly green on
> the commit before an ownership-changing change counts as landed --
> stands for exactly that reason.

### A custom hash map with a bug only at high load factor or on collision-heavy key patterns?

**Unreliably, as the generator is tuned today.** `Generator`'s order ids
are a simple monotonically-increasing counter (`next_id_`), and
`target_depth_per_side` (40 by default) keeps the number of
*simultaneously live* orders modest -- the generator was tuned for
realistic depth and queue behavior, not for stressing a hash table's
load factor or engineering key collisions against whatever hash function
Phase 5's custom map uses. A long-running `fuzz_soak` sweep will
naturally churn through many total ids over time (dead ones get
recycled out of the live pools), which gives *some* incidental coverage
of hash map insert/erase churn, but nothing here deliberately drives the
map toward its worst case, and nothing here knows what "worst case"
means for a hash function that doesn't exist yet.

**What Phase 5 needs:** targeted unit tests written directly against the
hash map, independent of the fuzzer entirely -- specifically constructed
key sequences at high load factor, and (if the hash function's
collision behavior is knowable in advance) adversarial collision-prone
keys. This is a case where the general-purpose behavioral fuzzer is
structurally the wrong tool: it fuzzes the *engine's* behavior through
its public interface, not a specific internal data structure's
worst-case access pattern.

### Memory errors that produce correct output on this run but are still UB?

**No**, for the same reason as the arena question -- this is the general
case the arena answer is a specific instance of. A differential/
invariant harness built entirely on observable output can only ever
prove "the output was correct this run." UB that happens not to
corrupt anything observable *this run* (a benign-looking
uninitialized read, a technically-out-of-bounds access that lands in
already-allocated memory, signed overflow that happens not to matter
for these particular values) is invisible by construction -- there is no
"output was wrong" for the harness to notice, because there wasn't one
this time.

**What Phase 5 needs:** the same answer as the arena question (ASan/
UBSan on the fuzz corpus), plus treating a long soak run under
sanitizers as load-bearing verification in its own right, not just an
extension of the differential comparison. UBSan in particular catches
classes of bug (signed overflow, misaligned access, invalid enum
values) that produce a "correct-looking" result on most inputs and
only occasionally on the specific bit patterns that expose them --
exactly why volume matters here, not just presence of the sanitizer
flag.

## Summary table

| Phase 5 concern | Caught by this harness as built today? | What closes the gap |
|---|---|---|
| Flat array tick-band drift/rebasing | Partially, by chance | A dedicated price-drift generator profile once the band size is known |
| Arena allocator UAF | **Now yes, in tagged builds** -- generation-tagged handles make a stale deref a detected engine error, caught by the ordinary differential/invariant/unit tests. Originally: no. | Generation tags (`LOB_ARENA_GENERATION_TAGS`, ON in CI) **plus** the ASan nightly as backstop for the untagged benchmark build |
| Custom hash map at high load factor / adversarial keys | Unreliably | Targeted unit tests against the hash map directly |
| UB with no observable corruption this run | No | ASan/UBSan on the fuzz corpus, run at volume -- built, see `fuzz-nightly.yml` |

## What this harness IS proven to do well

Worth stating plainly alongside the gaps, from `docs/fuzz_mutation_testing.md`:
seven realistic, independently-plausible matching-engine logic bugs,
caught reliably (140/140 across 20 seeds each), fast (median detection
within tens of ops), with the shrinker reliably reducing any of them to
a 2-4 op repro a human can read in seconds. That is genuinely what a
behavioral differential harness is built to do, and it does it well. The
four gaps above are not a criticism of the approach -- they're the
honest boundary of what "compare two engines' observable output" can
ever cover, no matter how thorough the comparison is within that
boundary.

Phase 5, concretely, should ship with:
1. This harness, unchanged in kind, run against the real optimized
   engine (this is the "later" the original Phase 3 plan deferred).
2. ASan+UBSan CI on the optimized engine's own test suite, mirroring
   Phase 1's `sanitizers` job.
3. The fuzz corpus (generator + differential harness) ALSO run under
   ASan+UBSan, not just plain -- this is the one item on this list that
   is genuinely new work, not a copy of an existing pattern, since
   Phase 1/2's sanitizer jobs run hand-written tests, not a generator-
   driven volume sweep.
4. Targeted unit tests for the custom hash map's high-load-factor and
   collision behavior, independent of the fuzzer.
5. Once the flat array's tick-band size is a known constant: a
   dedicated price-drift generator profile exercising rebasing
   specifically, not left to chance.
