#!/usr/bin/env python3
"""Diffs this engine's top-10 book reconstruction against an independent
ITCH implementation's reconstruction of the same real file, row by row.

Usage:
    python3 diff_books.py <mine.csv> <reference.csv> [reference_messages.csv]

`mine.csv` is itch_snapshot_dump's output (raw integer tick prices).
`reference.csv` is martinobdl/ITCH's BookConstructor book output (decimal
dollar prices) -- see README.md in this directory for how to obtain and
run it. The optional third argument is that same tool's message CSV,
row-aligned with its book CSV by construction; passing it lets this script
drop rows caused by 'P' (Trade) messages, which the ITCH spec says never
affect the book, before comparing -- otherwise a message type one tool
logs a redundant snapshot for and the other correctly skips looks like a
row-count mismatch instead of the expected, spec-consistent difference it
actually is.

Only the Python standard library is used -- no dependency to install.
"""
import csv
import sys


def parse_ticks(price_str):
    """Convert a decimal price string like '300.19' to exact integer ticks
    (4 implied decimals, matching ITCH's own Price(4) wire format) via
    string manipulation -- never float multiplication, which risks
    introducing the exact rounding error this comparison exists to catch.
    """
    if price_str == "" or price_str is None:
        return None
    if "." in price_str:
        whole, frac = price_str.split(".")
    else:
        whole, frac = price_str, ""
    frac = (frac + "0000")[:4]
    sign = -1 if whole.startswith("-") else 1
    whole = whole.lstrip("-")
    return sign * (int(whole) * 10000 + int(frac))


def parse_int(s):
    return None if s == "" else int(s)


def load_reference_message_types(path):
    with open(path, newline="") as f:
        return [row["type"] for row in csv.DictReader(f)]


def load_reference_rows(path, skip_types=frozenset(), message_types=None):
    """message_types must be the row-aligned type list from the reference
    tool's own message CSV (its docs guarantee message row k <-> book row
    k). Rows whose message type is in skip_types are dropped -- e.g. 'P'
    (Trade), which the ITCH spec itself says never affects the book, so a
    row logged for it is a no-op duplicate of the row before it, not a
    genuine book-state entry to compare against.
    """
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for idx, row in enumerate(reader):
            if message_types is not None and message_types[idx] in skip_types:
                continue
            levels = []
            for i in range(1, 11):
                bp = parse_ticks(row[f"{i}_bid_price"])
                bv = parse_int(row[f"{i}_bid_vol"])
                ap = parse_ticks(row[f"{i}_ask_price"])
                av = parse_int(row[f"{i}_ask_vol"])
                levels.append((bp, bv, ap, av))
            rows.append((int(row["time"]), levels))
    return rows


def load_mine_rows(path):
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            levels = []
            for i in range(1, 11):
                bp = parse_int(row[f"{i}_bid_price"])
                bv = parse_int(row[f"{i}_bid_vol"])
                ap = parse_int(row[f"{i}_ask_price"])
                av = parse_int(row[f"{i}_ask_vol"])
                levels.append((bp, bv, ap, av))
            rows.append((int(row["time"]), levels))
    return rows


def main():
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <mine.csv> <reference.csv> [reference_messages.csv]")
        sys.exit(1)

    mine_path, ref_path = sys.argv[1], sys.argv[2]
    ref_message_path = sys.argv[3] if len(sys.argv) > 3 else None

    mine = load_mine_rows(mine_path)
    if ref_message_path:
        types = load_reference_message_types(ref_message_path)
        ref = load_reference_rows(ref_path, skip_types={"P"}, message_types=types)
        print(
            f"(dropped {types.count('P')} reference rows for 'P' Trade messages -- "
            f"non-book-affecting per the ITCH spec)"
        )
    else:
        ref = load_reference_rows(ref_path)

    print(f"mine: {len(mine)} rows, reference: {len(ref)} rows")

    n = min(len(mine), len(ref))
    first_mismatch = None
    for i in range(n):
        if mine[i] != ref[i]:
            first_mismatch = i
            break

    if first_mismatch is None:
        print(f"first {n} rows match exactly.")
    else:
        i = first_mismatch
        print(f"FIRST MISMATCH at row {i} (0-indexed):")
        print(f"  mine:      time={mine[i][0]} levels={mine[i][1]}")
        print(f"  reference: time={ref[i][0]} levels={ref[i][1]}")
        print("  context (2 rows before):")
        for j in range(max(0, i - 2), i):
            print(f"    [{j}] mine:      time={mine[j][0]} levels={mine[j][1]}")
            print(f"    [{j}] reference: time={ref[j][0]} levels={ref[j][1]}")

    if len(mine) != len(ref):
        print(f"\nROW COUNT MISMATCH: mine={len(mine)} reference={len(ref)} diff={len(ref)-len(mine)}")
        if first_mismatch is None:
            # Rows agree up to the shorter length -- show what follows in
            # the longer file.
            longer_name, longer = ("reference", ref) if len(ref) > len(mine) else ("mine", mine)
            print(f"  extra rows appear only in {longer_name}, starting at row {n}:")
            for j in range(n, min(n + 5, len(longer))):
                print(f"    [{j}] time={longer[j][0]} levels={longer[j][1]}")
        sys.exit(2)


if __name__ == "__main__":
    main()
