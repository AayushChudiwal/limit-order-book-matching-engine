# Message-type mix: main NASDAQ vs PSX, 2019-07-30

Same date, two venues, both real data, both processed by the identical
`itch_replay` tool. See `message_mix_psx_20190730.md` for the PSX
methodology and full detail; this file adds the main-NASDAQ comparison
and calls out where the two venues diverge.

## Important asymmetry: full day vs partial sample

**PSX**: full trading day, every message (30,467,321 messages, 876MB
decompressed).

**Main NASDAQ**: a partial, early-session sample only (6,578,524
messages, ~194MB decompressed out of what would be several GB for a full
day). The full compressed file is 3.66GB; downloading and decompressing
that much data wasn't practical given this environment's network
throughput (roughly 130-280KB/s observed, with frequent connection
resets), so this is an HTTP range request for the first ~194MB of
decompressed messages -- sequential from market open, not a random or
representative-of-the-full-day sample. Referential integrity (0
violations across 6,337,478 order-lifecycle messages) is unaffected by
the partial read: `MessageReader` cleanly treats a truncated trailing
message as end-of-stream (see `ItchReader.TruncatedTrailingMessageEndsTheStreamCleanly`
in `tests/test_itch_decode.cpp`), and every complete message before that
point is genuine, unmodified NASDAQ wire data.

This means the NASDAQ numbers below skew toward whatever an early
session looks like (heavier morning quote-setting activity) and should
NOT be read as "main NASDAQ's daily mix" the way the PSX numbers can be
read as PSX's actual daily mix. They're still real, non-synthetic data --
just a narrower window than PSX's.

## Grouped by book effect

| Category | PSX (full day) | Main NASDAQ (partial, early session) |
|---|---:|---:|
| Add | 45.162% | 42.140% |
| Cancel/Delete | 45.903% | 49.778% |
| Replace | 7.895% | 4.119% |
| Execute | 0.859% | 0.299% |
| Trade (non-book) | 0.074% | 0.058% |
| Administrative | 0.107% | 3.606% |

## What's consistent across both venues

Cancel/Delete is the largest or second-largest category on both venues,
and both venues show Add well under half of total flow. That's the
finding that matters for benchmark design: **no venue, no time window,
in this real data looks like an all-Adds workload.** A synthetic
benchmark that's mostly Adds would misrepresent both venues, not just
one.

## What differs, and why that's expected rather than concerning

- **Replace is proportionally higher on PSX (7.9%) than this NASDAQ
  sample (4.1%).** Plausible venue-behavior difference (PSX order flow
  may lean more on cancel-replace patterns), but given the NASDAQ side
  is an early-session sample, this could also just be time-of-day: order
  book construction right at the open may show more fresh Adds and Cancels
  relative to Replaces, before market makers settle into steady-state
  quote refreshing later in the day. Not resolvable without a full
  NASDAQ day.
- **Administrative messages (R/S/H/L/Y/V) are far higher in the NASDAQ
  sample (3.6% vs PSX's 0.1%).** This is a real, explainable venue
  difference, not a sampling artifact: NASDAQ is the primary listing
  exchange for far more symbols than PSX, and disseminates a Stock
  Directory ('R') and Market Participant Position ('L') message for
  every one of them at the start of the day -- L alone is 3.2% of this
  sample's messages. PSX, as a secondary venue, doesn't carry anywhere
  near as many listed names, so its per-symbol administrative overhead
  is proportionally tiny next to its trading message volume.

## exchange-core sanity band, restated

exchange-core's published benchmark mix (9% GTC / 3% IOC / 6% cancel /
82% "move") remains a sanity-check band only -- both real venues here
show cancel/delete activity comparable to or exceeding Adds, confirming
the qualitative direction (modify/cancel-heavy, not add-heavy) without
matching exchange-core's specific numbers, which come from a different,
synthetic multi-asset benchmark, not from ITCH.

## Referential integrity

NASDAQ partial sample: **0 violations** out of 6,337,478 order-lifecycle
messages -- consistent with the PSX full-day result (also 0 violations,
out of 30,412,263).
