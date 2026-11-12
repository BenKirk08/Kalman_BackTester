#data QA only -> old close / bulk close should be one constant, a step in the ratio = a missing dividend adjustment (reads 2023+ rows, logged in HOLDOUT_UNSEAL.log, D3 D22)

import csv
import datetime
import os

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OLD = os.path.join(PROJECT, "Data", "Input")
BULK = os.path.join(PROJECT, "Data", "Input", "stooq")
PAIRS = [("xom", "xom"), ("cvx", "cvx"), ("cola", "ko"), ("pepsi", "pep"), ("v", "v"), ("ma", "ma"),
         ("aapl", "aapl"), ("ccl", "ccl"), ("cuk", "cuk")] #(old file name, bulk ticker)
STEP = 5e-4 #ratio change bigger than 0.05% between 2 days = a step (rounding noise ~1e-5)


def closes(path):
    with open(path, newline="") as f:
        return {r["Date"]: float(r["Close"]) for r in csv.DictReader(f)}


def main():
    with open(os.path.join(PROJECT, "HOLDOUT_UNSEAL.log"), "a") as log:
        log.write(f"{datetime.datetime.now():%Y-%m-%d %H:%M:%S}  data QA only  tools/check_dividend_adjustment.py "
                  f"(old/bulk close ratios of {len(PAIRS) - 1} tickers, no strategy)\n")
    print(f"{'old file':<10}{'bulk':<6}{'common days':>12}{'old-only':>9}{'steps <2023':>12}{'steps 2023+':>12}"
          f"{'ratio drift 2023+':>19}   first steps (date: ratio change)")
    for oldName, ticker in PAIRS:
        oldPath = os.path.join(OLD, f"{oldName}_Data.csv")
        bulkPath = os.path.join(BULK, f"{ticker}_Data.csv")
        if not os.path.exists(bulkPath):
            print(f"{oldName:<10}{ticker:<6}  not in the bulk data")
            continue
        old, bulk = closes(oldPath), closes(bulkPath)
        days = sorted(d for d in old if d in bulk)
        #old file days the bulk file should cover but lacks
        oldOnly = sum(1 for d in old if d not in bulk and d >= min(bulk))
        ratio = [old[d] / bulk[d] for d in days]
        #day i vs day i-1 so the step is dated on the day it first shows
        steps = [(days[i], ratio[i] / ratio[i - 1] - 1.0) for i in range(1, len(days))
                 if abs(ratio[i] / ratio[i - 1] - 1.0) > STEP]
        before = [s for s in steps if s[0] < "2023-01-01"]
        after = [s for s in steps if s[0] >= "2023-01-01"]
        #dates are ISO strings so string compare is date compare
        first2023 = next((i for i, d in enumerate(days) if d >= "2023-01-01"), len(days) - 1)
        #drift measured from the last ratio before 2023 to the end
        drift = ratio[-1] / ratio[first2023 - 1] - 1.0
        shown = ", ".join(f"{d}: {c:+.2%}" for d, c in after[:4])
        print(f"{oldName:<10}{ticker:<6}{len(days):>12}{oldOnly:>9}{len(before):>12}{len(after):>12}{drift:>18.2%}   {shown}")
    print("\nsteps = days where old/bulk close changes by more than 0.05%. a clean pair of files has none.")
    print("ratio drift = how far the two files move apart over 2023 to the end, i.e. the dividends one of them is missing.")


if __name__ == "__main__":
    main()
