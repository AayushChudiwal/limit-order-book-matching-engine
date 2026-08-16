# Cache hierarchy on this M4: three numbers, not one

Phase 5's struct-layout decisions (node packing, false-sharing padding,
intrusive-node sizing) all depend on "the cache line size" -- except
there isn't one number, there are (at least) three, and conflating them
is exactly the mistake flagged in the original Phase 5 prompt (which
assumed a single 64-byte x86-style line throughout). This document
records what was actually measured on this machine, not what secondary
sources claimed about a different M-series chip.

## The three numbers

| Quantity | Value on this M4 | Source |
|---|---:|---|
| L1D refill granularity | **64 bytes** | measured, `cache_line_probe`, this doc |
| OS-reported / coherence line size | **128 bytes** | `sysctl hw.cachelinesize`, measured |
| `std::hardware_destructive_interference_size` | **256 bytes** | this toolchain (Apple clang 21, arm64-apple-darwin25), measured |

**These serve different purposes and none of them is "wrong":**

- **64 bytes (refill granularity)** governs how many order-book nodes
  fit in a single L1D fill unit -- pack a hot struct to fit within 64
  bytes (or a clean fraction/multiple of it) to control how many misses
  a sequential scan costs.
- **128 bytes (coherence line size)** is what actually matters for
  false-sharing avoidance: two variables 64 bytes apart still share a
  128-byte coherence unit and will still ping-pong under concurrent
  access from different cores, even though a *single-threaded* refill
  only needed 64 bytes to satisfy the same access.
- **256 bytes (`hardware_destructive_interference_size`)** is the
  compiler vendor's own, more conservative padding recommendation --
  larger than the measured 128-byte coherence unit. Plausible reason
  (not confirmed, offered as a hypothesis): the DMP/prefetcher (see
  below) can pull in an adjacent line on top of the coherence unit
  itself, effectively widening the zone two variables need to avoid
  sharing beyond the raw coherence granularity. Whether or not that's
  the exact mechanism, using 256 is safe (it's a superset of the
  measured 128) at the cost of more padding overhead than strictly
  necessary. **Recommendation: use 128 for false-sharing padding on this
  chip, per direct measurement, and treat 256 as available but
  over-conservative unless a specific measurement says otherwise for
  a particular structure.**

## How 64 bytes was actually measured

`tools/cache_line_probe.cpp`: two loads, at `base` and `base+offset`,
issued through `volatile` pointers so the compiler can't fuse or
eliminate either one. If both addresses land in the same cache line, the
pair costs one L1D refill (measured via the already-validated
`ARM_L1D_CACHE_REFILL` counter, see `docs/pmu_validation.md`); if they
cross a line boundary, it costs two. Sweeping `offset` finds exactly
where that transition happens. Every offset tested is a multiple of 8
(reading `uint64_t`s) -- an earlier version of this tool tested 60 and
124, which are NOT 8-byte-aligned, and 124 produced a spurious reading;
removed once identified as a misaligned-read artifact, not a real
line-size signal.

**The first version of this tool produced a self-contradictory result**:
same-line pairs measured ~0.5 refills instead of the expected ~1.0,
while different-line pairs measured correctly (~2.0, no deflation). A
uniform code-level bug (e.g. an off-by-2 divisor) would have deflated
*every* offset equally, not just the same-line case -- so the design was
split into three isolated variants to find the actual cause rather than
assume one:

1. **Sequential stride, independent loads** (the original design): fixed
   1024-byte stride between trials, no dependency between the two loads
   within a trial.
2. **Sequential stride, DEPENDENT loads**: same stride, but the second
   load's address is computed from the first load's value (a genuine
   pointer-chasing dependency, `addr2 = base + offset + (v1 & 0)` --
   `v1` is guaranteed 0 by construction, so the address is always
   architecturally correct, but the CPU can't know that without waiting
   for `v1`). Tests whether the deflation comes from the CPU issuing two
   *independent* loads to the same line close enough together that the
   pair gets coalesced or undercounted.
3. **Randomized trial positions, independent loads**: same independent
   (non-serialized) loads as variant 1, but trial addresses are drawn at
   random (fixed seed, raw `mt19937_64` output, no
   `std::shuffle`/`uniform_int_distribution` -- same portability
   reasoning as `bench_matching_engine.cpp`) instead of a fixed stride.
   Tests whether the regular 1024-byte inter-trial stride itself is
   being recognized and prefetched by the hardware stride prefetcher.

**Results**: variants 2 and 3 -- methodologically unrelated to each
other (one removes the parallelism, the other removes the predictable
stride) -- converge tightly and independently on the same clean answer:
~1.00 refills/pair below 64 bytes, ~2.00 at and above 64 bytes, with the
transition exactly at offset 64 in both. Variant 1 (the flawed original)
stays noisy and deflated (0.32-0.57) below 64 bytes but is *also*
correct (~2.0, no deflation) at and above 64 bytes. Since two
methodologically-independent clean measurements agree, and the only
disagreement comes from the construction already suspected of being
flawed, **64 bytes is the trustworthy answer** -- not a three-way
compromise.

**What variant 1's flaw actually was, and what it wasn't**: an earlier
run of variant 1 also showed a dip to ~1.1 at offsets 256 and 384. That
dip did NOT reproduce once a separate bug was fixed -- the very first
version of this tool reset its read cursor to 0 for every offset in the
sweep, meaning every offset's "fresh" 20MB region was actually the exact
same 20MB region every previous offset had just read. Once every offset
was given its own genuinely fresh, never-reused buffer region, the
256/384 dip disappeared even in variant 1. That specific dip is
therefore attributed to the cursor-reuse bug, not to a genuine
prefetcher interaction at those specific offsets -- worth stating
plainly rather than asserting a mechanism that didn't survive a cleaner
measurement.

The remaining variant-1-only deflation (same-line, regular stride,
independent loads) is most plausibly a coalescing or prefetch-timing
interaction specific to that combination -- two independent, back-to-
back, regularly-strided loads to the same line being handled differently
by the load/miss pipeline than either a serialized pair or an
irregularly-strided one. This is offered as a plausible mechanism, not a
proven one; it doesn't change the design-relevant answer (64 bytes),
since the two clean, mutually-independent variants already establish it.

## The P-core/E-core sysctl trap

`sysctl hw.l1dcachesize` / `hw.l1icachesize` / `hw.l2cachesize` report
**65536 (64KB) / 131072 (128KB) / 4194304 (4MB)** on this machine.
Cross-referencing publicly available specs for M4: these numbers match
the **E-core** L1D/L1I/L2 sizes, not the P-core figures (commonly cited
as 128KB L1D / 192KB L1I / a much larger shared L2, on the order of
16-32MB depending on M4 tier). `hw.cachelinesize` (128) does not show
this split -- only the cache *size* sysctls do.

This matters directly for this project: `lob::bench::SetInteractiveQos()`
sets `QOS_CLASS_USER_INTERACTIVE` specifically to bias execution onto
P-cores. **Any code that reads these sysctls to auto-tune a
cache-blocking parameter (e.g. "process N items per block so the
working set fits in L1D") would size that block using E-core capacity
numbers while actually running on a P-core with roughly double the real
L1D and many times the real L2 -- underutilizing available cache by a
factor of 2-8x.** If Phase 5 ever wants to auto-tune a blocking factor
from a cache size, it needs a hardcoded, measured constant for the
P-core case, not a naive `sysctl` read.

## Prefetcher, briefly

Apple Silicon's DMP (data memory-dependent prefetcher) is not a plain
stride prefetcher -- it follows loaded *values* as candidate addresses,
speculatively prefetching through pointer chains (this is also the
mechanism behind the GoFetch side-channel published against M1/M2/M3).
Practical implication for Phase 5, stated as an open question rather
than a settled one: since this prefetcher is specifically built to
mitigate pointer-chasing latency, the measured benefit of replacing a
pointer-chasing tree with a flat array **on this chip specifically**
could be smaller than the same change would produce on a platform
without an equivalent mechanism. This is exactly why the flat array is
ordered *after* several other Phase 5 changes in the commit plan, and
why the traversal-vs-shuffled instrumentation built for the Replace
anomaly (see the Phase 5 research report) will be reused to check
whether the flat array actually creates a locality gap this baseline
mostly lacks, rather than assuming it will from x86-tuned intuition.

## Reproducing this

```
sudo ./build/cache_line_probe
```
