#scores the D26 design candidates from their PREFIX_summary.csv files -> score = average of kalman and static sharpe, never the difference, so the arms gap is not optimised

import csv
import os
import sys

MARGIN = 0.20 #a candidate replaces H0 only if its score is at least this much higher (fixed in D26)


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else "."
    rows = []
    for fileName in sorted(os.listdir(folder)):
        if not fileName.endswith("_summary.csv"):
            continue
        name = fileName[:-len("_summary.csv")]
        sharpe = {}
        with open(os.path.join(folder, fileName), newline="") as f:
            for r in csv.DictReader(f):
                if r["period"] == "development":
                    #sharpe, standard error, window start, window end
                    sharpe[r["arm"]] = (float(r["sharpe"]), float(r["sharpeSE"]), r["from"], r["until"])
        if "kalman" in sharpe and "static" in sharpe:
            rows.append((name, sharpe["kalman"], sharpe["static"]))
    if not rows:
        sys.exit(f"no *_summary.csv files in {folder}")
    #h0 is the yardstcik, none if its file is missing
    base = next((0.5 * (k[0] + s[0]) for name, k, s in rows if name == "h0"), None)
    print(f"design candidates, test windows {rows[0][1][2]} to {rows[0][1][3]} (sharpe, Lo standard error in brackets)")
    print(f"  {'candidate':<24} {'kalman':>14} {'static':>14} {'score':>7} {'vs H0':>7}")
    best = None
    for name, k, s in rows:
        #average of the two arms
        score = 0.5 * (k[0] + s[0])
        versus = "" if base is None or name == "h0" else f"{score - base:+7.2f}"
        print(f"  {name:<24} {k[0]:6.2f} ({k[1]:.2f})  {s[0]:6.2f} ({s[1]:.2f}) {score:7.2f} {versus:>7}")
        #h0 is the baseline so never the best candidate
        if name != "h0" and (best is None or score > best[1]):
            best = (name, score)
    if base is not None and best is not None:
        if best[1] - base >= MARGIN:
            print(f"\nbest candidate: {best[0]} (score {best[1]:.2f}, {best[1] - base:+.2f} against H0) -> it replaces H0")
        else:
            print(f"\nbest candidate: {best[0]} (score {best[1]:.2f}, {best[1] - base:+.2f} against H0): under the "
                  f"{MARGIN:.2f} margin -> H0 stays")


if __name__ == "__main__":
    main()
