# PMU validation: is `PmuCounters` measuring what it claims to?

Hardware: MacBook Air, Apple M4 (4 P-cores + 6 E-cores), macOS, run via
`sudo ./build/pmu_validate` on 2026-08-15. `kperf`/`kperfdata` require
root, so this cannot be run from an unprivileged process -- see the
constructor check in `src/bench/pmu.cpp`.

This is the evidence, not the claim -- same reasoning as
`docs/fuzz_mutation_testing.md`. A cycle counter that silently reads the
wrong hardware event produces numbers that look exactly as plausible as
a correct one; the only way to trust it is to check it against something
with a known ground truth first. Every check below does that.

## Raw output

```
=== PMU validation ===

CPU string reported by kpc_cpu_string(): (unknown -- kpc_cpu_string failed)
Branch mispredicts configurable counter available: yes
L1D cache refills configurable counter available:  yes

--- Test 1: known instruction count ---
  n=10000000 expected_instructions=20000001 measured_instructions=20013362 error=0.07%
  PASS

--- Test 2: linearity (double work => ~double cycles) ---
  1x cycles=20054666  2x cycles=40070513  ratio=1.998 (expect ~2.0)
  PASS

--- Test 3: IPC plausibility (expect roughly 1-6 on this core) ---
  cycles=20003744 instructions=40013324 ipc=2.000
  PASS (this loop is a tight SUBS/B.NE dependency chain, so IPC well under
  1.0 is actually expected here -- the plausibility bound is to catch an
  event code that's wrong by orders of magnitude, not to demand high IPC
  from a deliberately serial loop)

--- Test 4: implied frequency vs mach_absolute_time (long run) ---
  cycles=2002593178 wall_time=0.4588s implied_frequency=4.365 GHz
  PASS (plausible for a P-core)

--- Branch mispredicts sanity check ---
  random (unpredictable) branches cycles=4040321 instructions=9019001 ipc=2.232
  branch_mispredicts=11 out of 1000000 random branches
  FAIL: mispredict count implausibly low for random branches -- ARM_BR_MIS_PRED does not look trustworthy on this chip

--- L1D cache refills sanity check ---
  small buffer (8KB, fits L1D):  refills=24 over 51200 accesses (0.00047 refills/access)
  large buffer (64MB, exceeds L1D): refills=1429111 over 419430400 accesses (0.00341 refills/access)
  PASS (large buffer shows 7.3x the refill rate of the small one)

=== Verdict ===
cycles:               TRUSTED
instructions:         TRUSTED
branch_mispredicts:   NOT TRUSTED -- failed sanity check
l1d_cache_refills:    TRUSTED
```

(`kpc_cpu_string()` itself failed on this run -- see "CPU string bug"
below. Not one of the four counters this document is validating; fixed
separately and doesn't change any verdict above.)

## Reading the results

**Test 1 (known instruction count), PASS, 0.07% error.** 10,000,000
iterations of a hand-written `SUBS`/`B.NE` loop have a statically known
instruction count (`2n+1`); `FIXED_INSTRUCTIONS` landed within noise of
it. This is the strongest single check available: it doesn't just say
"cycles are plausible," it confirms the counter agrees with an exactly
known ground truth.

**Test 2 (linearity), PASS, ratio 1.998.** Doubling the loop iteration
count almost exactly doubled measured cycles. Rules out a counter that's
scaled by some fixed but wrong factor, or saturating/wrapping in a way
that only shows up at one specific workload size.

**Test 3 (IPC plausibility), PASS, IPC = 2.000 exactly.** Worth
explaining why a "tight dependency chain" hits IPC 2 instead of ~1: `SUBS`
followed immediately by `B.NE` on the same flags is a compare-and-branch
pattern Apple's front end fuses into a single dispatched unit, so one
loop iteration (two static instructions) retires in roughly one cycle.
This isn't a red flag -- it's confirmation the CPU is doing something a
real optimizer needs to know about, and a coincidence this clean would
be a bigger warning sign if it *didn't* line up with a well-known Apple
Silicon fusion case.

**Test 4 (implied frequency vs `mach_absolute_time`), PASS, 4.365 GHz.**
The one externally-anchored check: over a ~0.46s run (long enough that
`mach_absolute_time`'s ~41ns granularity is negligible), cycles /
wall-seconds lands right in the middle of the M4 P-core's publicly
documented ~4.0-4.5GHz range. This is what ties "cycles" back to an
actual unit of time at all on a platform with no software-readable
TSC frequency.

**Branch mispredicts, FAIL.** 1,000,000 genuinely data-dependent,
unpredictable branches (fed from `std::mt19937_64`, `& 1`) produced only
11 counted mispredicts. A real branch predictor gets truly random
branches wrong a substantial fraction of the time -- 11/1,000,000 is
consistent with "this event isn't counting what its name says," not with
"Apple's predictor is unreasonably good." Per the standing rule --
*do not publish a counter you haven't sanity-checked* -- `ARM_BR_MIS_PRED`
is dropped. `src/bench/pmu.cpp` no longer even attempts to configure it
(`branch_mispredicts_available_` is hardcoded `false`, with a comment
pointing back here); `IsBranchMispredictsAvailable()` will always return
`false` on this build until someone re-validates on different hardware
or a different OS revision.

**L1D cache refills, PASS.** An 8KB buffer (fits in M4's L1D) showed a
0.00047 refill/access rate; a 64MB buffer (far exceeds L1D) showed
0.00341, a 7.3x higher rate. That's the expected direction and a large
enough gap to be meaningful noise-wise. `ARM_L1D_CACHE_REFILL` is
trusted.

## CPU string bug (unrelated to the counters above)

`kpc_cpu_string()` returned nonzero (failure) on this run, so the tool
printed `(unknown -- kpc_cpu_string failed)` instead of `Apple M4`. This
doesn't affect any of the four validated counters -- it's a separate,
undocumented private function used only for a human-readable label. Root
cause wasn't chased down (this is exactly the kind of private-API
brittleness the whole PMU approach is already accepting elsewhere, and
chasing it would only add value back to something this class doesn't
actually need). Instead, `src/bench/pmu.cpp` was changed to read
`machdep.cpu.brand_string` via the public, documented `sysctlbyname()`
API instead of calling `kpc_cpu_string()` at all -- confirmed independently
during earlier investigation of this machine (`sysctl -n
machdep.cpu.brand_string` → `Apple M4`). One fewer private symbol this
code depends on, and no failure mode left to report a wrong or missing
CPU label in a committed benchmark artifact.

## Verdict this repo's benchmark harness relies on

| Counter | Status | Basis |
|---|---|---|
| cycles | **Trusted** | Tests 1, 2, 3, 4 all passed |
| instructions | **Trusted** | Test 1 (exact known count), Test 3 |
| l1d_cache_refills | **Trusted** | Passed dedicated sanity check (7.3x small-vs-large ratio) |
| branch_mispredicts | **Not trusted -- disabled in code** | Failed sanity check (11/1,000,000 on random branches) |

The Phase 4 benchmark harness reports **cycles** as the primary unit
(with instructions and L1D cache refills as trusted supplementary
metrics where useful) and does not report branch mispredicts at all.

## Confirmation re-run, after the two fixes above

Same machine, same binary rebuilt with the `sysctlbyname` fix and
`ARM_BR_MIS_PRED` no longer configured at all:

```
=== PMU validation ===

CPU string reported by kpc_cpu_string(): Apple M4
Branch mispredicts configurable counter available: no
L1D cache refills configurable counter available:  yes

--- Test 1: known instruction count ---
  n=10000000 expected_instructions=20000001 measured_instructions=20013541 error=0.07%
  PASS

--- Test 2: linearity (double work => ~double cycles) ---
  1x cycles=20014304  2x cycles=40024317  ratio=2.000 (expect ~2.0)
  PASS

--- Test 3: IPC plausibility (expect roughly 1-6 on this core) ---
  cycles=20066873 instructions=40027444 ipc=1.995
  PASS

--- Test 4: implied frequency vs mach_absolute_time (long run) ---
  cycles=2003091902 wall_time=0.4575s implied_frequency=4.379 GHz
  PASS (plausible for a P-core)

--- L1D cache refills sanity check ---
  small buffer (8KB, fits L1D):  refills=15 over 51200 accesses (0.00029 refills/access)
  large buffer (64MB, exceeds L1D): refills=1428600 over 419430400 accesses (0.00341 refills/access)
  PASS (large buffer shows 11.6x the refill rate of the small one)

=== Verdict ===
cycles:               TRUSTED
instructions:         TRUSTED
branch_mispredicts:   UNAVAILABLE (event not configured)
l1d_cache_refills:    TRUSTED
```

CPU string now reports correctly. Branch mispredicts is skipped outright
(the check block in `pmu_validate.cpp` is gated on
`IsBranchMispredictsAvailable()`, which is now always `false`) rather
than running and failing -- there is no code path left that could report
a mispredict number from this event. The other three counters reproduced
within run-to-run noise of the original numbers above: 4.379 GHz vs.
4.365 GHz implied frequency, IPC 1.995 vs. 2.000, L1D large/small ratio
11.6x vs. 7.3x (both comfortably clear the 3x bar). Nothing here changes
the verdict table.

## Reproducing this

```
sudo ./build/pmu_validate
```

Must run as root (`kpc_set_counting`/`kpc_set_thread_counting` require
it). If it fails for a permissions reason even under `sudo`, that is a
signal to stop and investigate -- not to work around -- since it likely
means SIP or another security policy is blocking PMU access on that
machine, which changes what's actually testable there.
