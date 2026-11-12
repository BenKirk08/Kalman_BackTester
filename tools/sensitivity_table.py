#prints the pre-registered configuration and its sensitivities side by side -> only shows whether the SIGN and rouhg size survive, the paired bootstrap is in analysis/monte_carlo.py

import csv
import os
import sys

ORDER = ["h0", "fill_same_close", "cost_10bp", "cost_20bp", "borrow_0", "no_point_in_time", "kalman_raw_prices",
         "rebase_10", "rebase_1000", "legs_swapped", "no_halflife_gate", "equal_notional", "equal_risk", "no_caps",
         "windows_6_months"]
WHAT = {
    "h0": "base design",
    "fill_same_close": "fills at the signal's own close (optimistic)",
    "cost_10bp": "cost 10 bp per execution",
    "cost_20bp": "cost 20 bp per execution",
    "borrow_0": "no borrow charge",
    "no_point_in_time": "today's index members in every year",
    "kalman_raw_prices": "kalman on raw log prices (no rebasing)",
    "rebase_10": "prices rebased to 10",
    "rebase_1000": "prices rebased to 1000",
    "legs_swapped": "A and B swapped",
    "no_halflife_gate": "half-life gate off",
    "equal_notional": "equal notional instead of equal risk",
    "equal_risk": "equal risk instead of equal notional",
    "no_caps": "no exposure caps",
    "windows_6_months": "6-month windows",
}


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else "."
    rows = {}
    for name in ORDER:
        path = os.path.join(folder, name + "_summary.csv")
        if not os.path.exists(path):
            continue
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                #one row per run,  period and arm
                rows[(name, r["period"], r["arm"])] = r
    #only periods that at least one run has
    periods = [p for p in ("development", "design", "validation", "holdout", "oos", "full") if any(k[1] == p for k in rows)]
    for period in periods:
        print(f"\n{period}:")
        print(f"  {'run':<44} {'kalman':>14} {'static':>14} {'K - S':>7}")
        for name in ORDER:
            k, s = rows.get((name, period, "kalman")), rows.get((name, period, "static"))
            if not k or not s:
                continue
            #K - S column is kalman minus static
            ks, ss = float(k["sharpe"]), float(s["sharpe"])
            print(f"  {WHAT[name]:<44} {ks:6.2f} ({float(k['sharpeSE']):.2f})  {ss:6.2f} ({float(s['sharpeSE']):.2f}) {ks - ss:7.2f}")
    print("\nsharpe (Lo standard error). each run changes one thing in the base design (the first row).")


if __name__ == "__main__":
    main()
