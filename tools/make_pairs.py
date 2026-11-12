#pair list for  batch mode from groups of tickers, every pair inside a group and none across -> 500 tickers all paired would be ~125k pairs and ~6k would pass a 5% test by chance

import argparse
import csv
import os
import sys

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) #tools/ -> project folder
STOOQ = os.path.join(PROJECT, "Data", "Input", "stooq")


def readGroups(path):
    #-> dict of group to tickers in file order. order matters, the first ticker of a pair is A in ln A = a + b*ln B
    groups = {}
    with open(path, newline="") as f:
        reader = csv.DictReader(row for row in f if row.strip() and not row.lstrip().startswith("#")) #skips blank and # lines
        if not reader.fieldnames or not {"group", "ticker"} <= {h.strip().lower() for h in reader.fieldnames}:
            sys.exit(f"{path} needs a header with group,ticker columns")
        for row in reader:
            row = {k.strip().lower(): (v or "").strip() for k, v in row.items() if k}
            g, t = row["group"], row["ticker"].lower()
            if g and t and t not in groups.setdefault(g, []):
                groups[g].append(t)
    return groups


def main():
    parser = argparse.ArgumentParser(description="pair list for BackTester batch mode, every pair within each group")
    parser.add_argument("groups", help="csv with group,ticker columns")
    parser.add_argument("--manifest", default=os.path.join(STOOQ, "manifest.csv"),
                        help="manifest from import_stooq.py (default Data/Input/stooq/manifest.csv)")
    parser.add_argument("--out", default=os.path.join(STOOQ, "pairs.csv"), help="default Data/Input/stooq/pairs.csv")
    args = parser.parse_args()

    if not os.path.exists(args.manifest):
        sys.exit(f"no manifest at {args.manifest} -> run tools/import_stooq.py first")
    with open(args.manifest, newline="") as f:
        files = {row["ticker"]: row["file"] for row in csv.DictReader(f)}

    pairs, done, missing = [], set(), {}
    for group, tickers in readGroups(args.groups).items():
        have = [t for t in tickers if t in files]
        for t in tickers:
            if t not in files:
                missing.setdefault(t, group)
        made = 0
        for i in range(len(have)):
            for j in range(i + 1, len(have)): #j > i -> each unordered pair once
                a, b = have[i], have[j]
                if frozenset((a, b)) in done:
                    continue #same pair already listed from another group
                done.add(frozenset((a, b)))
                pairs.append((f"{a}_{b}", files[a], files[b], group))
                made += 1
        print(f"  {group:<20} {len(have):>4} tickers -> {made:>6} pairs")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="") as f:
        out = csv.writer(f, lineterminator="\n")
        out.writerow(["label", "fileA", "fileB", "group"])
        out.writerows(pairs)

    shown = os.path.relpath(args.out, PROJECT)
    print(f"\n{len(pairs)} pairs -> {args.out if shown.startswith('..') else shown}")
    if pairs:
        print(f"  at a 5% test, about {0.05 * len(pairs):,.1f} of these would show as cointegrated by chance alone "
              "(if none of them really were)")
    if missing:
        print(f"  not in the manifest, so left out ({len(missing)}): {', '.join(sorted(missing))}")
        print("  -> not in the zip, or skipped by import_stooq.py's checks (its output says which)")


if __name__ == "__main__":
    main()
