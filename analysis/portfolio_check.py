#independent check of a walk-forward run -> rebuilds fills, sizes, limits and the P&L day by day from the raw prices and compares with what the c++ wrote

import argparse
import bisect
import csv
import math
import os
import sys
from collections import defaultdict

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UNIT = 10000.0 #engine fixed unit on leg A (TradeParams.capital)
TOLERANCE = 1e-6 #dollars
SIGMA_WINDOW = 60 #daily changes the sizing volatility is taken over


def loadPrices(path, startDate, endDate):
    #-> (dates, {date -> (open, close)}). startDate = a data fix cutting the early history (tools/data_fixes.csv)
    dates, prices = [], {}
    with open(path, newline="") as file:
        for row in csv.DictReader(file):
            if row["Date"] > endDate: #endDate = runs last day -> never read past it (hold-out seal)
                break
            if row["Date"] < startDate:
                continue
            openPrice, high, low, close = float(row["Open"]), float(row["High"]), float(row["Low"]), float(row["Close"])
            #open must be positive  and inside the days low-high range (1e-6 relaitve slack)
            usable = openPrice > 0.0 and openPrice >= low * (1.0 - 1e-6) and openPrice <= high * (1.0 + 1e-6)
            dates.append(row["Date"])
            prices[row["Date"]] = (openPrice if usable else None, close) #unusable open -> None, fills fall back to the close
    return dates, prices


def main():
    parser = argparse.ArgumentParser(description="rebuild the portfolio's accounting from the raw prices, day by day")
    parser.add_argument("prefix")
    parser.add_argument("--pairs", default=os.path.join("Data", "Input", "stooq", "pairs.csv"))
    parser.add_argument("--fixes", default=os.path.join("tools", "data_fixes.csv"), help="'none' if the run used none")
    parser.add_argument("--fill", choices=["open", "close"], default="open")
    parser.add_argument("--cost-bp", type=float, default=5.0)
    parser.add_argument("--borrow-bp", type=float, default=50.0)
    parser.add_argument("--capital", type=float, default=1e6)
    parser.add_argument("--sizing", choices=["risk", "gross"], default="risk")
    parser.add_argument("--risk", type=float, default=0.0001, help="daily volatility each trade is sized to (share of capital)")
    parser.add_argument("--gross-per-trade", type=float, default=0.02, help="equal-notional sizing: gross per trade (share of capital)")
    parser.add_argument("--no-caps", action="store_true", help="the run had the exposure limits off (--no-caps)")
    parser.add_argument("--max-trade", type=float, default=0.04)
    parser.add_argument("--max-ticker", type=float, default=0.10)
    parser.add_argument("--max-book", type=float, default=2.0)
    parser.add_argument("--swapped", action="store_true", help="the run had A and B swapped (--swap)")
    args = parser.parse_args()
    os.chdir(PROJECT)
    costRate, borrowRate, capital = args.cost_bp * 1e-4, args.borrow_bp * 1e-4, args.capital #bp -> fraction, borrow is per year
    nextOpen = args.fill == "open" #else same-close -> fillde on the signal bar  close

    startDates = {}
    if args.fixes != "none":
        with open(args.fixes, newline="") as file:
            for row in csv.DictReader(line for line in file if not line.startswith("#")): #skip the # comment lines
                if row["action"] == "start":
                    startDates[row["ticker"]] = row["date"]

    pairFiles = {}
    with open(args.pairs, newline="") as file:
        for row in csv.DictReader(file):
            pairFiles[row["label"]] = (row["fileB"], row["fileA"]) if args.swapped else (row["fileA"], row["fileB"]) #(leg A file, leg B file)
    tickerOf = lambda path: os.path.basename(path).replace("_Data.csv", "") #.../X_Data.csv -> X

    #what the c++  wrote
    with open(args.prefix + "_trades.csv", newline="") as file:
        trades = list(csv.DictReader(file))
    daily = defaultdict(dict) #arm ->  date -> row
    lastDay = ""
    with open(args.prefix + "_daily.csv", newline="") as file:
        for row in csv.DictReader(file):
            daily[row["arm"]][row["date"]] = row
            lastDay = max(lastDay, row["date"]) #ISO dates -> string max is the ltaest day
    theirTradeDays = {}
    with open(args.prefix + "_tradepnl.csv", newline="") as file:
        for row in csv.DictReader(file):
            theirTradeDays[(row["arm"], row["id"], row["date"])] = float(row["pnl"]) #(arm, trade id, date) -> P&L that day

    priceCache, calendarCache = {}, {}
    def pricesOf(path):
        if path not in priceCache:
            priceCache[path] = loadPrices(path, startDates.get(tickerOf(path), ""), lastDay) #never past the runs last day
        return priceCache[path]
    def pairCalendar(label):
        #the pairs own bars -> days both tickers have a price
        if label not in calendarCache:
            datesA, _ = pricesOf(pairFiles[label][0])
            _, pricesB = pricesOf(pairFiles[label][1])
            commonDays = [day for day in datesA if day in pricesB]
            calendarCache[label] = (commonDays, {day: i for i, day in enumerate(commonDays)}) #(days, day -> its index)
        return calendarCache[label]

    problems = defaultdict(list)
    worst = defaultdict(float) #(arm, what) -> largest abs difference seen
    checked = defaultdict(int) #arm -> trades rebuilt
    mine = defaultdict(lambda: defaultdict(lambda: [0.0, 0.0, 0.0])) #arm -> date -> [gross,  cost, borrow]
    openSpans = defaultdict(list) #arm -> [(first day open at the close, first day no longer open, entry notional)]
    reservations = defaultdict(list) #arm -> [(reserved from, released on, tickerA, tickerB, notionalA, notionalB)]
    myTradeDays = set()

    def note(arm, what, difference, detail):
        worst[(arm, what)] = max(worst[(arm, what)], abs(difference))
        if abs(difference) > TOLERANCE and len(problems[arm]) < 6: #first 6 problems per arm are enough
            problems[arm].append(f"{what}: off by {difference:.6g} -> {detail}")

    for trade in trades:
        arm, label = trade["arm"], trade["pair"]
        fileA, fileB = pairFiles[label]
        _, pricesA = pricesOf(fileA)
        _, pricesB = pricesOf(fileB)
        commonDays, index = pairCalendar(label)
        signalDate, entryDate, exitDate = trade["signalDate"], trade["entryDate"], trade["exitDate"]
        direction, scale, beta = int(trade["direction"]), float(trade["scale"]), float(trade["beta"])
        detail = f"{arm} {label} signal {signalDate}"
        closeA = lambda day: pricesA[day][1] #[0] = open, [1] = close
        closeB = lambda day: pricesB[day][1]

        #next-open -> filled on the pairs next bar after the signal. same-close -> on the signal bar
        expectedEntry = commonDays[index[signalDate] + 1] if nextOpen else signalDate
        if entryDate != expectedEntry:
            problems[arm].append(f"entry date {entryDate}, expected {expectedEntry} -> {detail}")

        def fillPrice(day, atClose):
            #both legs at the open, unless either open is unusable -> both at that days close
            (openA, dayCloseA), (openB, dayCloseB) = pricesA[day], pricesB[day]
            if atClose or openA is None or openB is None:
                return dayCloseA, dayCloseB
            return openA, openB

        #sizing volatility -> sample sd of the 60 daily changes of ln A - beta ln B up to  and including the signal bar
        signalIndex = index[signalDate]
        changes = [math.log(closeA(commonDays[k]) / closeA(commonDays[k - 1])) - beta * math.log(closeB(commonDays[k]) / closeB(commonDays[k - 1]))
                   for k in range(signalIndex - SIGMA_WINDOW + 1, signalIndex + 1)] #change k = bar k-1 -> bar k, last one ends ON the signal bar
        mean = sum(changes) / SIGMA_WINDOW
        sigma = math.sqrt(sum((change - mean) ** 2 for change in changes) / (SIGMA_WINDOW - 1)) #n-1 -> sample sd
        if abs(sigma - float(trade["sigma"])) > 1e-9 * sigma: #relative check
            problems[arm].append(f"sigma {trade['sigma']}, recomputed {sigma} -> {detail}")

        #size rule -> equal risk (or equal notional), shrunk to the per-trade cap
        if args.sizing == "risk":
            wantedScale = args.risk * capital / (UNIT * sigma) #daily dollar sd approx scale x UNIT x sigma = risk x capital
        else:
            wantedScale = args.gross_per_trade * capital / (UNIT * (1.0 + abs(beta))) #gross = scale x UNIT x (1 + abs(beta))
        if not args.no_caps and wantedScale * UNIT * (1.0 + abs(beta)) > args.max_trade * capital:
            wantedScale = args.max_trade * capital / (UNIT * (1.0 + abs(beta)))
        note(arm, "size", (wantedScale - scale) * UNIT, detail) #compared in dollars on leg A

        #sizes are fixed on the signal days closes
        sharesA = scale * UNIT / closeA(signalDate)
        sharesB = scale * beta * UNIT / closeB(signalDate) #hedge ratio x the leg A dollars
        entryA, entryB = fillPrice(entryDate, not nextOpen)
        entryNotional = sharesA * entryA + sharesB * entryB #gross at  the fill prices, not the signal closes
        note(arm, "entry notional", entryNotional - float(trade["gross"]), detail)
        shortNotional = sharesB * entryB if direction > 0 else sharesA * entryA #long spread = long A, short B
        legPnl = lambda toA, fromA, toB, fromB: direction * (sharesA * (toA - fromA) - sharesB * (toB - fromB))

        #counted against the limits -> the notionals at the signal days closes, from approval to the exit fill (still open -> never released)
        reservations[arm].append((signalDate, exitDate if exitDate else "9999-99-99", tickerOf(fileA), tickerOf(fileB),
                                  scale * UNIT, scale * beta * UNIT))

        #walk the trade day by day on the pairs own bars
        entryIndex = index[entryDate]
        #"end" = the pairs prices stopped -> closed at its LAST close, with the exit booked on the later day the engine gave up waiting for a bar
        stranded = bool(exitDate) and trade["exitReason"] == "end"
        exitAtClose = (not nextOpen) or stranded
        exitBar = commonDays[bisect.bisect_right(commonDays, exitDate) - 1] if stranded else exitDate #the bar the exit price comes from (stranded -> last bar on or before exitDate)
        exitIndex = index[exitBar] if exitDate else len(commonDays) #its index (past the last bar if still open)
        pnlByDay = {} #date -> gross P&L booked that day
        if nextOpen:
            pnlByDay[entryDate] = legPnl(closeA(entryDate), entryA, closeB(entryDate), entryB) #fill price -> that days close
        for k in range(entryIndex + 1, exitIndex):
            day, previousDay = commonDays[k], commonDays[k - 1]
            pnlByDay[day] = legPnl(closeA(day), closeA(previousDay), closeB(day), closeB(previousDay)) #close -> close
        costByDay = {entryDate: costRate * entryNotional}
        if exitDate:
            exitA, exitB = fillPrice(exitBar, exitAtClose)
            if exitIndex > entryIndex: #a stranded position can end on its own entry bar -> the entry-day mark  already reached that close
                previousDay = commonDays[exitIndex - 1]
                pnlByDay[exitBar] = pnlByDay.get(exitBar, 0.0) + legPnl(exitA, closeA(previousDay), exitB, closeB(previousDay)) #last close -> exit fill
            costByDay[exitDate] = costByDay.get(exitDate, 0.0) + costRate * (sharesA * exitA + sharesB * exitB) #on exitDate, the later day when stranded
            if int(trade["holdingDays"]) != exitIndex - entryIndex: #counted in pair bars
                problems[arm].append(f"holding days {trade['holdingDays']}, expected {exitIndex - entryIndex} -> {detail}")
        #open at the closes from the entry bar to the bar before the exit fill. stranded -> also open at its last close (closed just after it)
        heldCloses = [commonDays[k] for k in range(entryIndex, exitIndex + 1 if stranded else exitIndex)]

        for day, pnl in pnlByDay.items():
            theirs = theirTradeDays.get((arm, trade["id"], day), 0.0)
            note(arm, "trade-day P&L", pnl - theirs, f"{detail} on {day}")
            myTradeDays.add((arm, trade["id"], day))
            mine[arm][day][0] += pnl
        for day, dayCost in costByDay.items():
            mine[arm][day][1] += dayCost
        for day in heldCloses:
            mine[arm][day][2] += borrowRate / 252.0 * shortNotional #yeraly rate / 252 on the short leg entry notional, each close held
        gross = sum(pnlByDay.values())
        note(arm, "gross P&L", gross - float(trade["grossPnl"]), detail)
        note(arm, "cost", sum(costByDay.values()) - float(trade["cost"]), detail)
        note(arm, "borrow", borrowRate / 252.0 * shortNotional * len(heldCloses) - float(trade["borrow"]), detail)
        note(arm, "net P&L", gross - sum(costByDay.values()) - borrowRate / 252.0 * shortNotional * len(heldCloses) - float(trade["netPnl"]), detail)
        openSpans[arm].append((entryDate, exitDate if exitDate else "9999-99-99", entryNotional))
        checked[arm] += 1

    #a trade-day the c++ wrote that no trade of mine proudced
    for key, pnl in theirTradeDays.items():
        if key not in myTradeDays and abs(pnl) > TOLERANCE and len(problems[key[0]]) < 6:
            problems[key[0]].append(f"per-trade file has ${pnl:.4f} for trade {key[1]} on {key[2]} that the prices do not give")

    #every day of the daily file -> P&L, cost, borrow, positions open, gross exposure
    dayWorst = defaultdict(float)
    for arm, rows in daily.items():
        if arm not in checked:
            continue
        calendar = sorted(rows)
        openCount = [0] * (len(calendar) + 1) #difference arrays, +1 slot for spans running past the last day
        openGross = [0.0] * (len(calendar) + 1)
        for first, gone, notional in openSpans[arm]:
            i, j = bisect.bisect_left(calendar, first), bisect.bisect_left(calendar, gone)
            openCount[i] += 1; openCount[j] -= 1
            openGross[i] += notional; openGross[j] -= notional
        count, gross = 0, 0.0
        for i, day in enumerate(calendar):
            count += openCount[i] #running sum -> open at this close
            gross += openGross[i]
            dayGross, dayCost, dayBorrow = mine[arm].get(day, (0.0, 0.0, 0.0))
            row = rows[day]
            for what, difference in (("day P&L", dayGross - dayCost - dayBorrow - float(row["pnl"])), ("day cost", dayCost - float(row["cost"])),
                               ("day borrow", dayBorrow - float(row["borrow"])), ("day gross exposure", gross - float(row["gross"])),
                               ("day open positions", count - int(row["open"]))):
                dayWorst[arm] = max(dayWorst[arm], abs(difference))
                if abs(difference) > TOLERANCE and len(problems[arm]) < 6:
                    problems[arm].append(f"{what} on {day}: off by {difference:.6g}")

    #limits -> replay approvals and releases in date order
    limits = {}
    for arm, items in reservations.items():
        events = []
        for start, end, tickerA, tickerB, notionalA, notionalB in items:
            events.append((start, 1, tickerA, tickerB, notionalA, notionalB)) #reserved when approved, at the signal days close
            events.append((end, 0, tickerA, tickerB, notionalA, notionalB)) #released when the exit fills (before that days approvals)
        events.sort(key=lambda event: (event[0], event[1])) #same day -> release (0) sorts before approval (1)
        book, perTicker, peakBook, peakTicker = 0.0, defaultdict(float), 0.0, 0.0
        for _, isOpen, tickerA, tickerB, notionalA, notionalB in events:
            sign = 1.0 if isOpen else -1.0
            book += sign * (notionalA + notionalB)
            perTicker[tickerA] += sign * notionalA
            perTicker[tickerB] += sign * notionalB
            peakBook = max(peakBook, book)
            peakTicker = max(peakTicker, perTicker[tickerA], perTicker[tickerB]) #only the  2 tickers just touched can set  a new peak
        limits[arm] = (peakBook / capital, peakTicker / capital)
        if not args.no_caps:
            if peakBook > args.max_book * capital * (1 + 1e-9): #tiny slack for float sums
                problems[arm].append(f"book gross peaked at {peakBook / capital:.1%} of capital, limit {args.max_book:.0%}")
            if peakTicker > args.max_ticker * capital * (1 + 1e-9):
                problems[arm].append(f"a ticker's gross peaked at {peakTicker / capital:.1%} of capital, limit {args.max_ticker:.0%}")

    ok = True
    for arm in sorted(checked):
        largest = max((value for (armName, _), value in worst.items() if armName == arm), default=0.0) #over every per-trade check
        peakBook, peakTicker = limits[arm]
        status = "OK  " if not problems[arm] else "FAIL"
        print(f"{status} {arm:<20} {checked[arm]:>6} trades and {len(daily[arm])} days rebuilt from the price files: largest "
              f"difference per trade ${largest:.1e}, per day ${dayWorst[arm]:.1e}. peak reserved gross: book {peakBook:.1%}, "
              f"one ticker {peakTicker:.1%} of capital")
        for line in problems[arm]:
            print("     " + line)
            ok = False
    print("RESULT: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
