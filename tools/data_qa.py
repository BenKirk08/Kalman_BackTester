#price-only data checks on the research universe (DECISIONS.md D25) -> missing sessions vs SPY, split-like jumps, one-bar spikes, stale runs, large moves

import argparse
import csv
import math
import os

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CUTOFF = "2022-12-31" #the hold-out starts the day after (DECISIONS.md D1). not configurable on purpose
SPLIT_RATIOS = [2.0, 3.0, 4.0, 5.0, 10.0, 1.5]
SPLIT_TOLERANCE = 0.03 #how close the log return has to be to the log of a  split ratio
BIG_MOVE = 0.25
SPIKE = 0.3 #a one-bar move this big that reverses the next bar is treated as a bad print


badOpens = {} #path -> opens the engine would refuse (not positive, or outside the low-high of the day) -> see loadSeries


def readPrices(path):
    #-> [(date, close)] up to the cut-off
    rows = []
    bad = 0
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r["Date"] > CUTOFF:
                break
            rows.append((r["Date"], float(r["Close"])))
            o, h, l = float(r["Open"]), float(r["High"]), float(r["Low"])
            #1e-6 slack so rounding in the csv doesnt flag an open sitting exactly on the low or high
            if not (o > 0.0 and o >= l * (1.0 - 1e-6) and o <= h * (1.0 + 1e-6)):
                bad += 1
    badOpens[path] = bad
    return rows


def main():
    parser = argparse.ArgumentParser(description="data checks on the research universe (pre-2023 bars only)")
    parser.add_argument("--pairs", default=os.path.join(PROJECT, "Data", "Input", "stooq", "pairs.csv"))
    parser.add_argument("--calendar", default=os.path.join(PROJECT, "Data", "Input", "stooq", "spy_Data.csv"),
                        help="the ticker whose sessions define the trading calendar (default SPY)")
    args = parser.parse_args()

    files = {}
    with open(args.pairs, newline="") as f:
        for r in csv.DictReader(f):
            for col in ("fileA", "fileB"):
                path = r[col] if os.path.isabs(r[col]) else os.path.join(PROJECT, r[col])
                files[os.path.basename(path).replace("_Data.csv", "")] = path
    calendar = [d for d, _ in readPrices(args.calendar)]

    missingReport, splitLike, staleRuns, spikes = [], [], [], []
    bigMoves = 0
    for ticker in sorted(files):
        rows = readPrices(files[ticker])
        if len(rows) < 2:
            continue
        dates = {d for d, _ in rows}
        sessions = [d for d in calendar if d >= rows[0][0]]
        missing = [d for d in sessions if d not in dates]
        share = len(missing) / max(len(sessions), 1)
        if missing:
            missingReport.append((share, ticker, len(missing), len(sessions), missing[:3]))

        run = 1
        for i in range(1, len(rows)):
            ret = math.log(rows[i][1] / rows[i - 1][1]) #daily log return
            if abs(ret) > BIG_MOVE:
                bigMoves += 1
            for ratio in SPLIT_RATIOS:
                if abs(abs(ret) - math.log(ratio)) <= SPLIT_TOLERANCE:
                    #reversed within 5 sessions? then it was a real spike, not a missing adjustment
                    later = rows[min(i + 5, len(rows) - 1)][1]
                    #later is 5 bars on (or the last bar), still there if over half the jump rmeains
                    stillThere = abs(math.log(later / rows[i - 1][1])) > 0.5 * abs(ret)
                    if stillThere:
                        splitLike.append((ticker, rows[i][0], ret, ratio if ret > 0 else 1.0 / ratio))
                    break
            #one-bar spike -> a big move that the very next bar takes back almost exactly -> a bad print, not a price
            if abs(ret) > SPIKE and i + 1 < len(rows):
                nextRet = math.log(rows[i + 1][1] / rows[i][1])
                if abs(ret + nextRet) < 0.15 * abs(ret): #2 moves cancel to within 15% of the first
                    spikes.append((ticker, rows[i][0], ret, rows[i - 1][1], rows[i][1], rows[i + 1][1]))
            if rows[i][1] == rows[i - 1][1]:
                run += 1
            else:
                if run >= 5: #run covers bars i-run to i-1
                    staleRuns.append((run, ticker, rows[i - run][0], rows[i - 1][0]))
                run = 1
        if run >= 5: #run still  open at the end of the data
            staleRuns.append((run, ticker, rows[len(rows) - run][0], rows[-1][0]))

    print(f"{len(files)} tickers, bars up to {CUTOFF} only, calendar = {os.path.basename(args.calendar)} ({len(calendar)} sessions)")
    over = [m for m in missingReport if m[0] > 0.01]
    print(f"\nmissing sessions: {len(missingReport)} tickers miss at least one, {len(over)} miss more than 1% (these are dropped)")
    for share, ticker, n, total, first in sorted(missingReport, reverse=True)[:15]:
        print(f"  {ticker:<8} {n:>4} of {total} ({share:.2%})  first: {', '.join(first)}")
    print(f"\nsplit-like jumps (within {SPLIT_TOLERANCE} of a clean ratio, not reversed in 5 sessions): {len(splitLike)}")
    for ticker, date, ret, ratio in splitLike:
        print(f"  {ticker:<8} {date}  log return {ret:+.3f}  (looks like x{ratio:.3g})")
    print(f"\none-bar spikes (abs log return above {SPIKE}, taken back by the next bar): {len(spikes)}")
    for ticker, date, ret, before, bad, after in spikes:
        print(f"  {ticker:<8} {date}  log return {ret:+.3f}  closes {before:g} -> {bad:g} -> {after:g}")
    long = [s for s in staleRuns if s[0] >= 10]
    print(f"\nstale prices: {len(staleRuns)} runs of 5+ identical closes, {len(long)} of 10+ (history cut after these)")
    for run, ticker, first, last in sorted(staleRuns, reverse=True)[:15]:
        print(f"  {ticker:<8} {run} closes  {first} to {last}")
    print(f"\nlarge moves (abs log return above {BIG_MOVE}): {bigMoves} ticker-days (counted only)")
    worst = sorted(((badOpens[files[t]], t) for t in files if badOpens.get(files[t])), reverse=True)
    print(f"unusable opens (outside the day's low-high): {sum(n for n, _ in worst)} bars in {len(worst)} tickers"
          f" -> a next-open fill on such a bar uses the bar's close" + (f". most: {', '.join(f'{t} {n}' for n, t in worst[:6])}" if worst else ""))


if __name__ == "__main__":
    main()
