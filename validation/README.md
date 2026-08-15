# Engine-correctness cross-validation

The original plan for engine-correctness validation was to diff this
engine's reconstructed book against [LOBSTER](https://lobsterdata.com)'s
free sample reconstructions. That's no longer possible: LOBSTER's site was
rebuilt at some point after 2024 under a new operator
("LOBSTER DATA TECHNOLOGY & SCIENCE LTD") and every data path now requires
an account -- a paid download portal, a pay-per-use student tier, an
academic-conference trial, or a book-purchase-verification flow. There is
no free, no-registration sample left. (Verified directly by loading the
site rather than assumed from stale documentation.)

This directory replaces that plan with **cross-implementation agreement**:
an independent open-source ITCH 5.0 reconstructor,
[martinobdl/ITCH](https://github.com/martinobdl/ITCH) (C++, [Zenodo DOI
10.5281/zenodo.5209267](https://doi.org/10.5281/zenodo.5209267)), run on
the *exact same* real PSX file this repo already validated referential
integrity against (`20190730.PSX_ITCH_50`, see
`docs/message_mix_psx_20190730.md`), diffed row-by-row against this
engine's own reconstruction of the same file.

This is a materially stronger check than LOBSTER would have been: LOBSTER's
sample is a different venue, different date, different file entirely
(NASDAQ main tape, 2012-06-21). A match there would only prove the *engine
logic* generalizes to another dataset. A row-for-row match against a
second implementation parsing the *same bytes* directly tests whether this
engine's interpretation of the ITCH 5.0 spec -- which fields mean what,
which messages affect book state, how Replace/Cancel/Delete/Execute reduce
a resting order -- agrees with an independent implementation's
interpretation of the identical wire data. Two independently-written
engines being internally self-consistent (which is all Phase 3's
differential fuzzer checks) says nothing about whether they're both
*wrong* in the same way; this does.

## Method

1. Pick symbols spanning a liquidity range from the real PSX file's own
   Add-order activity counts (a liquidity proxy): SPY (rank 1, most
   active), IYW (rank 51, mid), UGAZ (rank 101, lower but still
   substantial).
2. Run this engine's `itch_snapshot_dump` for each symbol -- one row per
   book-affecting message, raw integer tick prices.
3. Run martinobdl/ITCH's `BookConstructor` for the same symbols against
   the same file -- same row-per-message shape, decimal dollar prices.
4. `diff_books.py` parses both, converting the reference tool's decimal
   prices to exact integer ticks via string manipulation (never float
   multiplication, which would risk introducing the exact rounding error
   this comparison exists to catch), and compares row by row.
5. Cheap extra reconciliation: sum this engine's reconstructed executed +
   traded shares per symbol and sanity-check against each symbol's
   published *consolidated* daily volume (Yahoo Finance) for the same
   date -- PSX is one of many venues a US-listed symbol trades on, so its
   share should be a small, plausible fraction of the consolidated total,
   never anywhere close to exceeding it.

## Reproducing

martinobdl/ITCH is **not vendored into this repo** -- it's a separate
project with its own license and its own build. Clone and build it
yourself:

```sh
git clone https://github.com/martinobdl/ITCH /path/to/ITCH
cd /path/to/ITCH
make
```

Then, instead of its bash wrapper (which does its own gzip decompression
and an online stock-locate lookup this repo doesn't need, since the file
is already decompressed), call the underlying binary directly:

```sh
mkdir -p /path/to/out/book /path/to/out/messages
/path/to/ITCH/bin/BookConstructor \
    data/20190730.PSX_ITCH_50 /path/to/out/book/ /path/to/out/messages/ 10 SPY
```

This produces `20190730.PSX_ITCH_50_SPY_book_10.csv` and
`..._SPY_message.csv` in those directories. Then, from this repo:

```sh
cmake --build build --target itch_snapshot_dump
./build/itch_snapshot_dump data/20190730.PSX_ITCH_50 SPY > /tmp/mine_spy.csv

python3 validation/diff_books.py \
    /tmp/mine_spy.csv \
    /path/to/out/book/20190730.PSX_ITCH_50_SPY_book_10.csv \
    /path/to/out/messages/20190730.PSX_ITCH_50_SPY_message.csv
```

## Results

All three symbols, full trading day, top 10 levels both sides:

| Symbol | Rows compared | Result |
|---|---:|---|
| SPY  | 697,986 | exact match |
| IYW  | 94,297  | exact match |
| UGAZ | 50,510  | exact match |

Total: **842,793 book snapshots, zero divergence**, across a 700x
liquidity range (SPY's row count vs UGAZ's).

Getting to a clean diff surfaced one real, fully-explained discrepancy
worth recording rather than hiding: the two tools initially disagreed on
row *count* (SPY: 698,087 reference rows vs 697,986 here). The cause
wasn't a book-state bug -- martinobdl/ITCH logs a book-snapshot row for
every message touching the symbol, including type `P` (Trade,
non-displayed), which the ITCH spec explicitly states "do[es] not affect
the book." This engine correctly excludes `P` from book reconstruction
(see `messages.hpp`), so it never emits a row for one. The reference
tool's `P` rows are exact duplicates of the row before them (no book
column changes) -- confirmed by cross-referencing its message CSV, which
showed exactly 101 `P` messages for SPY, matching the row-count gap
exactly. `diff_books.py` drops those rows before comparing when given the
reference tool's message CSV.

### Volume reconciliation

| Symbol | PSX-reconstructed shares (this engine) | Consolidated daily volume (Yahoo Finance, 2019-07-30) | PSX share |
|---|---:|---:|---:|
| SPY  | 1,529,775 | 45,849,000 | 3.34% |
| IYW  | 887       | 214,800    | 0.41% |
| UGAZ | 51,546    | 946,749    | 5.44% |

All three are small, plausible fractions of consolidated volume --
consistent with PSX being one of many venues a listed symbol trades on,
never anywhere near exceeding the consolidated total (which would be an
immediate correctness red flag).
