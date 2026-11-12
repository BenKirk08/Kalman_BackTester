#compares the output files of two walkforward runs (daily, trades, screen) -> used for truncation, price unit and hold-out checks, exit code 1 on any mismatch

import argparse
import csv
import sys


def load(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def close(a, b, tol):
    if a == b:
        return True
    try:
        x, y = float(a), float(b)
    except ValueError:
        return False
    #relative tolerance, absolute for values under 1
    return abs(x - y) <= tol * max(1.0, abs(x), abs(y))


def compare(name, rowsA, rowsB, keyCols, tol, skipCols=()):
    #rows matched by key columns so row order doesnt matter
    a = {tuple(r[c] for c in keyCols): r for r in rowsA}
    b = {tuple(r[c] for c in keyCols): r for r in rowsB}
    missing = [k for k in a if k not in b]
    extra = [k for k in b if k not in a]
    worst, different = 0.0, 0
    for k in a:
        if k not in b:
            continue
        for col, va in a[k].items():
            if col in skipCols:
                continue
            vb = b[k][col]
            if not close(va, vb, tol):
                different += 1
                if different <= 3:
                    print(f"    {name} {k} {col}: {va} vs {vb}")
            else:
                #text columns  have no difference to measure
                try:
                    worst = max(worst, abs(float(va) - float(vb)))
                except ValueError:
                    pass
    ok = not missing and not extra and different == 0
    print(f"  {name:<8} {len(a):>7} rows vs {len(b):>7}: {'identical' if ok else 'DIFFERENT'}"
          f" (only in A: {len(missing)}, only in B: {len(extra)}, values differing: {different},"
          f" largest accepted difference {worst:.1e})")
    for k in (missing + extra)[:3]:
        print(f"    unmatched: {k}")
    return ok


def main():
    parser = argparse.ArgumentParser(description="compare two walkforward runs")
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--until", default="", help="compare only what is dated on or before this (YYYY-MM-DD)")
    parser.add_argument("--tol", type=float, default=1e-9)
    parser.add_argument("--ignore-size", action="store_true",
                        help="price-unit check: share counts differ by the unit, so skip the scale/sigma-free columns only")
    args = parser.parse_args()
    until = args.until

    #one failed file set fails the whole run
    ok = True
    daily = [load(p + "_daily.csv") for p in (args.a, args.b)]
    if until:
        daily = [[r for r in rows if r["date"] <= until] for rows in daily]
    ok &= compare("daily", daily[0], daily[1], ("arm", "date"), args.tol)

    trades = [load(p + "_trades.csv") for p in (args.a, args.b)]
    if until:
        #a trade still open at the cut is unfinished business in the short run -> compare closed trades only
        trades = [[r for r in rows if r["exitDate"] and r["exitDate"] <= until] for rows in trades]
    else:
        trades = [[r for r in rows] for rows in trades]
    ok &= compare("trades", trades[0], trades[1], ("arm", "pair", "signalDate"), args.tol)

    screens = [load(p + "_screen.csv") for p in (args.a, args.b)]
    if until:
        screens = [[r for r in rows if r["windowFrom"] <= until] for rows in screens]
    #alpha is the intercept in price units of the run -> it legitimately shifts with the price unit
    skip = ("alpha",) if args.ignore_size else ()
    ok &= compare("screen", screens[0], screens[1], ("pair", "windowFrom"), args.tol, skip)

    print("RESULT: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
