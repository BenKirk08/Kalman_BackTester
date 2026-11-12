#converts the stooq bulk daily zip (or the folder it unzips to) into one <ticker>_Data.csv per ticker for DataLoader.cpp, keeping only tickers that pass every check so no bad file stops a batch run

import argparse
import contextlib
import csv
import datetime
import io
import math
import os
import re
import statistics
import sys
import zipfile

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) #tools/ -> project folder
DEFAULT_OUT = os.path.join(PROJECT, "Data", "Input", "stooq")
NEEDED = ["<DATE>", "<OPEN>", "<HIGH>", "<LOW>", "<CLOSE>", "<VOL>"] #column names stooq uses, in our output order
RECENT_DAYS = 252 #price + liquidity checks use the last year only -> oledr prices are scaled down by dividend adjusting


def tickerFromMember(name):
    #data/daily/us/nasdaq stocks/1/aapl.us.txt -> aapl. anything outside a-z 0-9 . - becomes _ so the name is a plian filename
    base = os.path.basename(name).lower()
    base = base[:-4] if base.endswith(".txt") else base
    base = base[:-3] if base.endswith(".us") else base
    clean = re.sub(r"[^a-z0-9.\-]", "_", base).lstrip(".")
    return clean if re.search(r"[a-z0-9]", clean) else None


def marketFromMember(name):
    #the folder says what it is -> "nyse etfs" is etf, "nasdaq stocks" is stock
    for part in name.lower().split("/")[:-1]:
        if "etf" in part:
            return "etf", part
        if "stock" in part:
            return "stock", part
    return "other", os.path.dirname(name)


@contextlib.contextmanager
def openSource(path):
    #-> [(name,  opener)] for every  .txt price file, from the zip or the folder it unzips to. names use /  so marketFromMember reads both the same way
    if os.path.isdir(path):
        items = []
        for root, dirs, files in os.walk(path): #doesnt follow folder symlinks
            dirs.sort()
            for fn in sorted(files):
                full = os.path.join(root, fn)
                if fn.lower().endswith(".txt") and not os.path.islink(full):
                    rel = os.path.relpath(full, path).replace(os.sep, "/")
                    items.append((rel, lambda full=full: open(full, "rb")))
        yield items
    elif zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as zf:
            yield [(m.filename, lambda m=m: zf.open(m)) for m in zf.infolist()
                   if not m.is_dir() and m.filename.lower().endswith(".txt")]
    else:
        sys.exit(f"not a zip file or folder: {path}")


def readTickerList(path):
    #one ticker per line, or any csv with a "ticker" column (so a groups file for make_pairs.py works here too)
    with open(path, newline="") as f:
        rows = [r for r in csv.reader(f) if r and r[0].strip() and not r[0].strip().startswith("#")]
    if not rows:
        return set()
    header = [h.strip().lower() for h in rows[0]]
    if "ticker" in header:
        col = header.index("ticker")
        return {r[col].strip().lower() for r in rows[1:] if len(r) > col and r[col].strip()}
    return {r[0].strip().lower() for r in rows}


def parseMember(raw, start, end):
    #-> (rows, droppedRows, problem). rows are (date, open, high, low, close, volume) inside [start, end]
    text = io.TextIOWrapper(raw, encoding="utf-8", errors="replace", newline="")
    reader = csv.reader(text)
    header = next(reader, None)
    if not header:
        return [], 0, "empty file"
    header = [h.strip().upper() for h in header]
    if any(n not in header for n in NEEDED):
        return [], 0, "unexpected columns"
    idx = [header.index(n) for n in NEEDED] #column  positions in output order
    perCol = header.index("<PER>") if "<PER>" in header else None

    rows, dropped, last = [], 0, ""
    for rec in reader:
        if not rec:
            continue
        try:
            if perCol is not None and rec[perCol].strip().upper() != "D":
                return [], 0, "not daily data"
            d = rec[idx[0]].strip()
            if len(d) != 8 or not d.isdigit():
                raise ValueError("bad date")
            date = f"{d[:4]}-{d[4:6]}-{d[6:]}" #stooq writes YYYYMMDD
            o, h, l, c = (float(rec[i]) for i in idx[1:5])
            vol = rec[idx[5]].strip()
            v = float(vol) if vol else 0.0
        except (ValueError, IndexError):
            dropped += 1
            continue
        if (start and date < start) or (end and date > end):
            continue
        if not all(math.isfinite(x) and x > 0 for x in (o, h, l, c)) or not math.isfinite(v) or v < 0:
            dropped += 1 #ln(close) needs close > 0, and a broken open/high/low would fail stod in the loader
            continue
        if date <= last:
            dropped += 1 #duplicate or out of order -> the two pointer merge in alignBars needs strictly increasing dates
            continue
        rows.append((date, o, h, l, c, int(round(v)))) #volume as a whole number, stoll would read 9.7193e+07 as 9
        last = date
    return rows, dropped, None


def writePriceFile(path, rows):
    #repr -> shortest text that reads back as the exact same double, so no precision is lost
    with open(path, "w", newline="") as f:
        f.write("Date,Open,High,Low,Close,Volume\n")
        for date, o, h, l, c, v in rows:
            f.write(f"{date},{o!r},{h!r},{l!r},{c!r},{v}\n")


def projectPath(path):
    #pair lists are read by the c++ binary from the project folder -> relative paths there, absolute anywhere else
    rel = os.path.relpath(path, PROJECT)
    return path if rel.startswith("..") else rel


def main():
    parser = argparse.ArgumentParser(description="convert stooq's bulk daily zip into BackTester price files")
    parser.add_argument("source", help="d_us_txt.zip, or the folder it unzips to")
    parser.add_argument("--out", default=DEFAULT_OUT, help="output folder (default Data/Input/stooq)")
    parser.add_argument("--tickers", default="", help="comma separated tickers to import, e.g. xom,cvx,ko,pep")
    parser.add_argument("--tickers-file", default="", help="file of tickers (one per line, or a csv with a ticker column)")
    parser.add_argument("--markets", choices=["all", "stocks", "etfs"], default="all")
    parser.add_argument("--start", default="2016-01-01", help="first date kept, YYYY-MM-DD ('' for all history)")
    parser.add_argument("--end", default="", help="last date kept, YYYY-MM-DD (default: everything)")
    parser.add_argument("--min-rows", type=int, default=1000,
                        help="min trading days kept (johansen gate needs 750 before it can open)")
    parser.add_argument("--min-price", type=float, default=5.0, help="min median close over the last year")
    parser.add_argument("--min-dollar-volume", type=float, default=1e6,
                        help="min median daily close*volume over the last year")
    parser.add_argument("--max-stale-days", type=int, default=30,
                        help="drop tickers whose data ends more than this many days before the newest date in the zip")
    args = parser.parse_args()

    for d in (args.start, args.end):
        if d and not re.fullmatch(r"\d{4}-\d{2}-\d{2}", d):
            sys.exit(f"dates must be YYYY-MM-DD, got {d!r}")
    wanted = {t.strip().lower() for t in args.tickers.split(",") if t.strip()}
    if args.tickers_file:
        wanted |= readTickerList(args.tickers_file)
    wantMarket = {"all": None, "stocks": "stock", "etfs": "etf"}[args.markets]

    os.makedirs(args.out, exist_ok=True)

    written, skipped, seen, inZip = [], {}, {}, set()
    def skip(ticker, reason):
        skipped.setdefault(reason, []).append(ticker)

    with openSource(args.source) as members:
        if not members:
            sys.exit(f"no .txt price files in {args.source} -> is it stooq's ascii download?")
        print(f"{len(members)} price files in {os.path.basename(os.path.normpath(args.source))}")

        for n, (name, opener) in enumerate(members, 1):
            ticker = tickerFromMember(name)
            market, folder = marketFromMember(name)
            if ticker is None or (wanted and ticker not in wanted):
                continue
            inZip.add(ticker)
            if wantMarket and market != wantMarket:
                continue
            if ticker in seen:
                skip(ticker, f"duplicate ticker (kept {seen[ticker]})")
                continue
            seen[ticker] = folder

            with opener() as raw:
                rows, dropped, problem = parseMember(raw, args.start, args.end)
            if problem:
                skip(ticker, problem)
                continue
            if len(rows) < args.min_rows:
                skip(ticker, f"fewer than {args.min_rows} rows")
                continue
            recent = rows[-RECENT_DAYS:] #last year of bars, medians so one odd day cant decide a ticker
            medClose = statistics.median(r[4] for r in recent)
            medDollarVol = statistics.median(r[4] * r[5] for r in recent)
            if medClose < args.min_price:
                skip(ticker, f"median price under {args.min_price:g}")
                continue
            if medDollarVol < args.min_dollar_volume:
                skip(ticker, f"median dollar volume under {args.min_dollar_volume:,.0f}")
                continue

            path = os.path.join(args.out, f"{ticker}_Data.csv")
            writePriceFile(path, rows)
            written.append({"_path": path, "ticker": ticker, "market": market, "folder": folder, "file": projectPath(path),
                            "rows": len(rows), "first": rows[0][0], "last": rows[-1][0], "droppedRows": dropped,
                            "medianClose": round(medClose, 4), "medianDollarVolume": round(medDollarVol)})
            if n % 1000 == 0:
                print(f"  {n}/{len(members)} scanned, {len(written)} written")

    #still trading? -> compare each last date to the newest date in the download. failed files from this run are removed again
    if written:
        newest = max(w["last"] for w in written)
        cutoff = (datetime.date.fromisoformat(newest) - datetime.timedelta(days=args.max_stale_days)).isoformat()
        kept = []
        for w in written:
            if w["last"] < cutoff:
                os.remove(w["_path"])
                skip(w["ticker"], f"stopped trading before {cutoff}")
            else:
                kept.append(w)
        written = kept

    manifestPath = os.path.join(args.out, "manifest.csv")
    with open(manifestPath, "w", newline="") as f:
        cols = ["ticker", "market", "folder", "file", "rows", "first", "last", "droppedRows",
                "medianClose", "medianDollarVolume"]
        out = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore") #_path is only for this script
        out.writeheader()
        out.writerows(sorted(written, key=lambda w: w["ticker"]))

    print(f"\nwrote {len(written)} tickers -> {projectPath(args.out)}/  (list: {projectPath(manifestPath)})")
    if written:
        print(f"  dates {min(w['first'] for w in written)} to {max(w['last'] for w in written)}, "
              f"{sum(w['market'] == 'stock' for w in written)} stocks, {sum(w['market'] == 'etf' for w in written)} etfs")
        bad = sum(w["droppedRows"] for w in written)
        if bad:
            print(f"  {bad} bad rows dropped across all files (bad numbers, close <= 0, duplicate dates)")
    for reason, tickers in sorted(skipped.items(), key=lambda kv: -len(kv[1])):
        shown = ", ".join(sorted(tickers)[:8]) + (" ..." if len(tickers) > 8 else "")
        print(f"  skipped {len(tickers):>5}: {reason}  ({shown})")
    if wanted:
        missing = sorted(wanted - inZip)
        if missing:
            print(f"  not in the download ({len(missing)}): {', '.join(missing)}")


if __name__ == "__main__":
    main()
