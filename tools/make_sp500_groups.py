#builds the groups file (group,ticker) for tools/make_pairs.py, S&P 500 grouped by GICS sub-industry plus ctl_ control groups that are reported but never traded (membership is as of the snapshot date, so earlier years carry a survivorship bias)

import argparse
import csv
import os
import re
import sys

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SNAPSHOT = os.path.join(PROJECT, "tools", "sp500_constituents.csv")
DEFAULT_OUT = os.path.join(PROJECT, "tools", "sp500_groups.csv")
MEMBERS = os.path.join(PROJECT, "tools", "sp500_members.csv")

#controls that are not in the constituents table -> funds that track the same index / the same metal
ETF_CONTROLS = [
    ("ctl_sp500_etfs", ["spy", "ivv", "voo"]),
    ("ctl_gold_etfs", ["gld", "iau"]),
]


def stooqTicker(symbol):
    #stooq writes share classes with a dash -> BRK.B becomes brk-b, BF.B becomes bf-b
    return symbol.strip().lower().replace(".", "-")


def slug(text):
    #"Oil & Gas Exploration & Production" -> "oil_gas_exploration_production" (no commas, no spaces)
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


def stripWiki(cell):
    #"[[3M]]" -> "3M", "[[Alphabet Inc.|Alphabet]] (Class A)" -> "Alphabet (Class A)", drops refs and templates
    cell = re.sub(r"<ref[^>]*>.*?</ref>|<ref[^>]*/>", "", cell)
    cell = re.sub(r"\[\[(?:[^\]|]*\|)?([^\]]*)\]\]", r"\1", cell)
    cell = re.sub(r"\{\{[^{}]*\}\}", "", cell)
    return cell.strip()


def parseWikitext(path):
    #-> [(symbol, security, sector, subIndustry, dateAdded)] from the table with id="constituents"
    text = open(path, encoding="utf-8").read()
    start = text.index('id="constituents"')
    table = text[start:text.index("\n|}", start)]
    rows = []
    for block in table.split("\n|-")[2:]:
        #[0] is the table line and [1] the header row (skipped). a cell starts a new line with | or ||, but 2 cells can share a line via ||
        cells = []
        for line in re.split(r"\n\|\|?", "\n" + block.strip()):
            cells.extend(c.strip() for c in line.split("||"))
        cells = [c for c in cells if c]
        if len(cells) < 6:
            continue
        symbol = re.search(r"\{\{[^|{}]*\|([^|{}]+)\}\}", cells[0])
        if not symbol:
            continue
        rows.append((symbol.group(1).strip(), stripWiki(cells[1]), stripWiki(cells[2]), stripWiki(cells[3]),
                     stripWiki(cells[5])))
    return rows


def readSnapshot(path):
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(row for row in f if not row.startswith("#"))
        return [(r["symbol"], r["security"], r["sector"], r["subIndustry"], r["dateAdded"]) for r in reader]


def writeSnapshot(path, rows, retrieved):
    with open(path, "w", newline="", encoding="utf-8") as f:
        f.write("# S&P 500 constituents with GICS sector and sub-industry.\n")
        f.write("# source: https://en.wikipedia.org/wiki/List_of_S%26P_500_companies (CC BY-SA 4.0), "
                f"retrieved {retrieved}.\n")
        f.write("# written by tools/make_sp500_groups.py --from-wikitext. do not edit by hand.\n")
        out = csv.writer(f, lineterminator="\n")
        out.writerow(["symbol", "security", "sector", "subIndustry", "dateAdded"])
        out.writerows(rows)


def companyKey(security):
    #"Alphabet Inc. (Class A)" and "Alphabet Inc. (Class C)" -> the same key, so share classes can be spotted
    return re.sub(r"\s*\(class [a-z]\)\s*", "", security.lower()).strip()


def main():
    parser = argparse.ArgumentParser(description="S&P 500 GICS sub-industry groups for tools/make_pairs.py")
    parser.add_argument("--from-wikitext", default="", help="rebuild the snapshot from a saved copy of the page's wikitext")
    parser.add_argument("--retrieved", default="", help="date the wikitext was downloaded (written into the snapshot)")
    parser.add_argument("--out", default=DEFAULT_OUT, help="default tools/sp500_groups.csv")
    args = parser.parse_args()

    if args.from_wikitext:
        if not args.retrieved:
            sys.exit("--from-wikitext needs --retrieved YYYY-MM-DD")
        rows = parseWikitext(args.from_wikitext)
        if len(rows) < 450: #too few rows parsed -> layout changed
            sys.exit(f"only {len(rows)} rows parsed -> the page layout changed, not overwriting the snapshot")
        writeSnapshot(SNAPSHOT, rows, args.retrieved)
        print(f"snapshot: {len(rows)} constituents -> {os.path.relpath(SNAPSHOT, PROJECT)}")
    if not os.path.exists(SNAPSHOT):
        sys.exit(f"no snapshot at {SNAPSHOT} -> run once with --from-wikitext")
    rows = readSnapshot(SNAPSHOT)

    #share classes -> first class listed stays in the sub-industry group, every class goes in a control group
    byCompany = {}
    for symbol, security, sector, sub, added in rows:
        byCompany.setdefault(companyKey(security), []).append(symbol)
    shareClasses = {k: v for k, v in byCompany.items() if len(v) > 1}
    dropped = {s for symbols in shareClasses.values() for s in symbols[1:]}

    groups = {} #sub-industry slug -> tickers in table order
    for symbol, security, sector, sub, added in rows:
        if symbol in dropped:
            continue
        groups.setdefault(slug(sub), []).append(stooqTicker(symbol))

    with open(args.out, "w", newline="") as f:
        f.write("group,ticker\n")
        f.write("# written by tools/make_sp500_groups.py from tools/sp500_constituents.csv. do not edit by hand.\n")
        f.write("# one group per GICS sub-industry. groups starting with ctl_ are data-quality controls: reported, never traded.\n")
        for group in sorted(groups):
            for ticker in groups[group]:
                f.write(f"{group},{ticker}\n")
        for company, symbols in sorted(shareClasses.items()):
            for symbol in symbols:
                f.write(f"ctl_shareclass_{slug(company)},{stooqTicker(symbol)}\n")
        for group, tickers in ETF_CONTROLS:
            for ticker in tickers:
                f.write(f"{group},{ticker}\n")

    #ticker -> index entry date + sector, for the point-in-time universe (eligible only from the date it joined)  and the 2008 short-sale ban on financials
    with open(MEMBERS, "w", newline="") as f:
        f.write("ticker,dateAdded,sector\n")
        for symbol, security, sector, sub, added in rows:
            if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", added):
                sys.exit(f"{symbol}: date added is not a date ({added!r}) -> the page layout changed")
            f.write(f"{stooqTicker(symbol)},{added},{sector}\n")

    sizes = sorted((len(t) for t in groups.values()), reverse=True)
    pairs = sum(n * (n - 1) // 2 for n in sizes) #n choose 2 per group
    singles = sum(1 for n in sizes if n == 1)
    print(f"{len(rows)} constituents -> {len(groups)} sub-industry groups ({singles} with a single company, so no pair)")
    print(f"  largest groups: {sizes[:8]}   pairs if every ticker has data: {pairs}")
    print(f"  share classes moved to controls: " + ", ".join("/".join(v) for v in shareClasses.values()))
    before2008 = sum(1 for r in rows if r[4] < "2008-01-01")
    holdout = sum(1 for r in rows if r[4] >= "2023-01-01")
    print(f"  index entry dates: {before2008} joined before 2008, {len(rows) - before2008 - holdout} in 2008-2022, {holdout} in 2023 or later")
    print(f"  -> {os.path.relpath(args.out, PROJECT)}, {os.path.relpath(MEMBERS, PROJECT)}")


if __name__ == "__main__":
    main()
