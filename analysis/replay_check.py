# the engine trade rules written a 2nd time from the spec only (not the c++), replayed on a trace and compared trade for trade -> bars file numbers are inputs, NOT checked

import csv
import math
import sys

# the constants of the specification
CAPITAL = 10000.0  # dollars into leg A per trade
ENTRY_Z = 2.0
MAX_ENTRY_Z = 3.5
EXIT_Z = 0.0
STOP_Z = 3.5
TIMEOUT_HALF_LIVES = 5.0
COST_BPS = 0.0005  # 5 bp of notional per execution, 4 executions a round trip
HALF_LIFE_MIN, HALF_LIFE_MAX = 0.5, 30.0
WARM_UP = 750
BETA_MIN, BETA_MAX = 0.25, 4.0

TOLERANCE = 1e-9  # absolute for values up to 1, relative above that

BAR_NUMBERS = [
    "openA",
    "openB",
    "closeA",
    "closeB",
    "lnA",
    "lnB",
    "alpha",
    "beta",
    "z",
    "zMean",
    "zSd",
    "johansenOK",
    "gateMeanReverting",
    "gateHalfLife",
    "frozenHalfLife",
]  # the columns the rules use
TRADE_EXACT = [
    "signalBar",
    "entryBar",
    "exitBar",
    "direction",
    "exitReason",
]  # must match exactly
TRADE_NUMBERS = [
    "zEntry",
    "zExit",
    "sharesA",
    "sharesB",
    "entryPriceA",
    "entryPriceB",
    "exitPriceA",
    "exitPriceB",
    "pnl",
    "cost",
]  # must match within TOLERANCE


# ------------------------------------------------------------------ reading the trace files


def numberOrNone(text):
    # a blank cell means "not available on this bar"
    text = text.strip()
    return None if text == "" else float(text)


def loadBars(prefix):
    bars = []
    with open(prefix + "_bars.csv", newline="") as file:
        for row in csv.DictReader(file):
            bar = {name: numberOrNone(row[name]) for name in BAR_NUMBERS}
            bar["bar"] = int(row["bar"])
            bar["date"] = row["date"]  # type: ignore
            bars.append(bar)
    for i in range(1, len(bars)):
        if bars[i]["bar"] != bars[i - 1]["bar"] + 1:
            # "the previous bar" must be the previous row, otherwise next-bar fills mean something else
            raise ValueError(
                f"bars are not consecutive: {bars[i - 1]['bar']} is followed by {bars[i]['bar']}"
            )
    return bars


def loadTrades(prefix):
    trades = []
    with open(prefix + "_trades.csv", newline="") as file:
        for row in csv.DictReader(file):
            trade = {name: float(row[name]) for name in TRADE_NUMBERS}
            for name in ("signalBar", "entryBar", "exitBar", "direction"):
                trade[name] = int(row[name])
            trade["exitReason"] = row["exitReason"].strip()  # type: ignore
            trades.append(trade)
    return trades


# ------------------------------------------------------------------ the trade rules (from the specification)


def isOne(value):
    return value is not None and value == 1.0


def entryAllowed(bar):
    # every condition must hold. a blank value fails its condition
    z, beta, halfLife = bar["z"], bar["beta"], bar["gateHalfLife"]
    if not (
        ENTRY_Z < abs(z) < MAX_ENTRY_Z
    ):  # past 3.5 -> more likeyl a break than a bargain
        return False
    if bar["bar"] - 1 < WARM_UP:  # first bar allowed to signal is 751
        return False
    if not isOne(bar["johansenOK"]):
        return False
    if (
        not isOne(bar["gateMeanReverting"])
        or halfLife is None
        or not (HALF_LIFE_MIN <= halfLife <= HALF_LIFE_MAX)
    ):
        return False
    if beta is None or not (BETA_MIN <= beta <= BETA_MAX):
        return False
    return True


def makeEntryOrder(bar):
    # everything the trade needs later is frozen on the signal bar -> hedge, z-score scale, share counts, timeout
    z, beta, halfLife = bar["z"], bar["beta"], bar["frozenHalfLife"]
    timeoutDays = 0  # 0 -> no timeout
    if halfLife is not None and halfLife >= 0:
        timeoutDays = max(
            1, math.ceil(TIMEOUT_HALF_LIVES * halfLife)
        )  # round up -> never cut short, at least 1 bar
    return {
        "signalBar": bar["bar"],
        "direction": 1 if z < 0 else -1,  # +1 = long A / short B
        "zEntry": z,
        "alpha0": bar["alpha"],
        "beta0": beta,
        "mean0": bar["zMean"],
        "sd0": bar["zSd"],
        "timeoutDays": timeoutDays,
        "sharesA": CAPITAL
        / bar["closeA"],  # sized on the signal bar close, not the fill price
        "sharesB": beta * CAPITAL / bar["closeB"],
    }


def exitSignal(trade, bar):
    # todays spread measured with the hedge and the z-score scale of the signal bar. needs no  z from today
    spread = bar["lnA"] - (trade["alpha0"] + trade["beta0"] * bar["lnB"])
    zHold = (spread - trade["mean0"]) / trade["sd0"]
    direction = trade["direction"]
    target = (direction == 1 and zHold >= -EXIT_Z) or (
        direction == -1 and zHold <= EXIT_Z
    )  # come home -> back to 0 from the entry side
    stop = (direction == 1 and zHold < -STOP_Z) or (
        direction == -1 and zHold > STOP_Z
    )  # stretched past 3.5 on the entry side
    timeout = (
        not target
        and not stop
        and trade["timeoutDays"] > 0
        and bar["bar"] - trade["signalBar"] >= trade["timeoutDays"]
    )  # counted from the signal bar, not the fill
    if stop:
        return "stop", zHold
    if target:
        return "target", zHold
    if timeout:
        return "timeout", zHold
    return None, zHold


def openFillPrices(bar):
    # an order from the previous bar fills at this open. one open missing -> BOTH legs use this close
    if bar["openA"] is None or bar["openB"] is None:
        return bar["closeA"], bar["closeB"]
    return bar["openA"], bar["openB"]


def closeTrade(trade, bar, priceA, priceB, reason, zExit):
    # fills in the exit side of a trade -> P&L of the two legs and the cost of all four executions
    sharesA, sharesB = trade["sharesA"], trade["sharesB"]
    entryA, entryB = trade["entryPriceA"], trade["entryPriceB"]
    trade["exitBar"] = bar["bar"]
    trade["exitPriceA"], trade["exitPriceB"] = priceA, priceB
    trade["zExit"] = zExit
    trade["exitReason"] = reason
    trade["pnl"] = trade["direction"] * (
        sharesA * (priceA - entryA) - sharesB * (priceB - entryB)
    )  # -1 flips both legs (short A, long B)
    trade["cost"] = COST_BPS * (
        sharesA * entryA + sharesB * entryB + sharesA * priceA + sharesB * priceB
    )
    return trade


def replay(bars, fill):
    # walks the bars once. returns the closed trades in the order they closed
    atOpen = fill == "open"
    trades = []
    position = None  # the open trade, None when flat
    pendingEntry = None  # entry order waiting for the next open (only with --fill open)
    pendingExit = (
        None  # (reason, zExit) waiting for the next open (only with --fill open)
    )
    coolOff = False  # after a stop or timeout -> no entry until abs(z) is back under 2

    for bar in bars:
        # step 1 -> the order created on the previous bar fills at this bars open
        if pendingEntry is not None:
            position = pendingEntry
            position["entryBar"] = bar["bar"]
            position["entryPriceA"], position["entryPriceB"] = openFillPrices(bar)
            pendingEntry = None
        elif pendingExit is not None:
            priceA, priceB = openFillPrices(bar)
            reason, zExit = pendingExit
            trades.append(closeTrade(position, bar, priceA, priceB, reason, zExit))
            coolOff = coolOff or reason in (
                "stop",
                "timeout",
            )  # a target exit starts no cool-off
            position = None
            pendingExit = None

        # step 2 -> decisions at this bars close
        if position is not None:
            # a) in a position (also one opened a moment ago in step 1) -> look for an exit
            reason, zHold = exitSignal(position, bar)
            if reason is not None:
                if atOpen:
                    pendingExit = (reason, zHold)
                else:
                    trades.append(
                        closeTrade(
                            position, bar, bar["closeA"], bar["closeB"], reason, zHold
                        )
                    )
                    coolOff = coolOff or reason in ("stop", "timeout")
                    position = None
        elif pendingEntry is None and pendingExit is None and bar["z"] is not None:
            # b) flat with nothing waiting, and todays z exists -> cool-off bookkeeping or a new entry
            if coolOff:
                if abs(bar["z"]) < ENTRY_Z:
                    coolOff = False  # back inside the band -> entries allowed again from the next bar
            elif entryAllowed(bar):
                order = makeEntryOrder(bar)
                if atOpen:
                    pendingEntry = order  # fills in step 1 of the next bar
                else:
                    position = order  # same-close -> filled right away at this close
                    position["entryBar"] = bar["bar"]
                    position["entryPriceA"], position["entryPriceB"] = (
                        bar["closeA"],
                        bar["closeB"],
                    )
    # end of data -> nothing is force-closed, an open position or a waiting order makes no trade row
    return trades


# ------------------------------------------------------------------ comparing the two trade lists


def numberDifference(mine, theirs):
    # absolute difference for values up to 1, reltaive above
    return abs(mine - theirs) / max(1.0, abs(mine), abs(theirs))


def describeTrade(trade):
    return (
        f"signal {trade['signalBar']} entry {trade['entryBar']} exit {trade['exitBar']} "
        f"direction {trade['direction']} {trade['exitReason']}"
    )


def compareTrades(mine, theirs):
    # returns (identical, first mismatch as text, largest numeric difference over the trades compared)
    firstMismatch = None
    largest = 0.0
    for i in range(min(len(mine), len(theirs))):
        replayTrade, engineTrade = mine[i], theirs[i]
        wrongFields = [
            name for name in TRADE_EXACT if replayTrade[name] != engineTrade[name]
        ]
        if wrongFields:
            # a different bar, direction or reason -> the lists have diverged, so stop. numbers of 2 different trades would only add noise
            if firstMismatch is None:
                firstMismatch = (
                    f"trade {i + 1}: {', '.join(wrongFields)} differ -> replay: {describeTrade(replayTrade)} | "
                    f"engine: {describeTrade(engineTrade)}"
                )
            break
        for name in TRADE_NUMBERS:
            difference = numberDifference(replayTrade[name], engineTrade[name])
            if difference != difference:
                difference = float("inf")  # a nan on either side counts as a mismatch
            largest = max(largest, difference)
            if difference > TOLERANCE and firstMismatch is None:
                firstMismatch = (
                    f"trade {i + 1} (signal bar {engineTrade['signalBar']}): {name} replay={replayTrade[name]!r} "
                    f"engine={engineTrade[name]!r}"
                )
    if firstMismatch is None and len(mine) != len(
        theirs
    ):  # every shared trade agrees but one list is longer
        longer, who = (
            (mine, "replay") if len(mine) > len(theirs) else (theirs, "engine")
        )
        extra = longer[
            min(len(mine), len(theirs))
        ]  # first trade past the end of the shorter list
        firstMismatch = (
            f"trade count replay={len(mine)} engine={len(theirs)} -> first trade only the {who} has: "
            f"{describeTrade(extra)}"
        )
    return firstMismatch is None, firstMismatch, largest


# ------------------------------------------------------------------ command line


def parseArguments(argv):
    # -> list of (prefix, fill). --fill sticks to every prefix after it, with none given the name decides
    runs = []
    fill = "auto"
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--fill" or arg.startswith("--fill="):
            if arg == "--fill":
                i += 1
                value = (
                    argv[i] if i < len(argv) else ""
                )  # --fill as the last word -> empty, refused below
            else:
                value = arg[len("--fill=") :]
            if value not in ("open", "close", "auto"):
                sys.exit(f"--fill needs open, close or auto, got '{value}'")
            fill = value
        elif arg.startswith("--"):
            sys.exit(f"unknown option {arg}")
        else:
            for ending in ("_bars.csv", "_trades.csv"):  # allow the file name itself
                if arg.endswith(ending):
                    arg = arg[: -len(ending)]
            inferred = (
                "close" if arg.endswith("_close") else "open"
            )  # open is the engines default
            runs.append((arg, inferred if fill == "auto" else fill))
        i += 1
    return runs


def main():
    runs = parseArguments(sys.argv[1:])
    if not runs:
        sys.exit(
            "usage: replay_check.py [--fill open|close] <tracePrefix> [<tracePrefix> ...]"
        )

    failed, unreadable = 0, 0
    for prefix, fill in runs:
        try:
            bars = loadBars(prefix)
            engineTrades = loadTrades(prefix)
        except (OSError, KeyError, ValueError) as problem:
            print(f"ERROR {prefix} (fill {fill}): cannot read the trace -> {problem}")
            unreadable += 1
            continue
        try:
            replayTrades = replay(bars, fill)
        except (TypeError, ZeroDivisionError) as problem:
            # a price, log price or frozen value the rules need is blank or zero -> the specification does not cover it
            print(
                f"ERROR {prefix} (fill {fill}): the rules needed a value the bars file does not have -> {problem}"
            )
            unreadable += 1
            continue
        identical, firstMismatch, largest = compareTrades(replayTrades, engineTrades)
        line = (
            f"{'OK   ' if identical else 'FAIL '} {prefix} (fill {fill}): {len(bars)} bars, "
            f"engine {len(engineTrades)} trades, replay {len(replayTrades)} trades, "
            f"largest numeric difference {largest:.3g}"
        )
        if not identical:
            line += f" -> first mismatch: {firstMismatch}"
            failed += 1
        print(line)

    if unreadable:
        sys.exit(2)  # a trace cuold not be read or replayed
    if failed:
        sys.exit(1)  # a run did not match


if __name__ == "__main__":
    main()
