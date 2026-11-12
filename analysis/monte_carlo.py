# monte carlo tests for the pairs backtester (1-3 on single pairs, 4-6 on the walk-forward portfolio) -> the strategy only ever exists once (in c++), python only resamples what c++ produced

import argparse
import os
import shutil
import subprocess
import sys

import numpy as np
import pandas as pd

PROJECT = os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))
)  # analysis/ -> project folder
OUT_FOLDER = os.path.join(
    "Data", "Output", "montecarlo"
)  # the c++ export writes its runs here, the results are saved here too
TRADING_DAYS = 252.0
MIN_TRADES = 5  # fewer trades than this  -> a sharpe or a resample means nothing. one rule used by tests 1, 2, 3 and 5

# the runs c++ exports (same names as runExport in main.cpp) -> 2 controls so the tests themselves can be checked
RUNS = [
    ("v_ma", "real"),
    ("xom_cvx", "real"),
    ("cuk_ccl", "real"),
    ("cola_pepsi", "real"),
    ("syn_fast", "control: edge built in"),
    ("syn_indep", "control: no edge"),
]


# ------------------------------------------------------------------ c++ plumbing


def runBinary(binary, args):
    # run the c++ program from the project folder -> its data paths are relative
    completed = subprocess.run(
        [binary] + args, cwd=PROJECT, capture_output=True, text=True
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"c++ failed ({' '.join(args)}):\n{completed.stdout}{completed.stderr}"
        )
    return completed.stdout


def loadRun(name):
    prefix = os.path.join(OUT_FOLDER, name)
    daily = pd.read_csv(prefix + "_daily.csv")
    trades = pd.read_csv(prefix + "_trades.csv")
    params = pd.read_csv(prefix + "_params.csv").iloc[
        0
    ]  # 1 row -> the costs used + the c++ answers
    return daily, trades, params


# ------------------------------------------------------------------ shared maths (same rules as the c++)


def tradePnl(
    closeA,
    closeB,
    betaPrior,
    entryIndex,
    exitIndex,
    direction,
    capital,
    costBps,
    commission,
):
    # same sums as the c++ trading loop -> legs frozen at entry, A gets capital, B gets beta * capital
    entryA, entryB = (
        closeA[entryIndex],
        closeB[entryIndex],
    )  # no +1 offset here -> the export already wrote row i with the prices of bars[i+1]
    exitA, exitB = closeA[exitIndex], closeB[exitIndex]
    sharesA = capital / entryA
    sharesB = (
        betaPrior[entryIndex] * capital / entryB
    )  # B leg = beta times the A leg, in dollars
    pnl = direction * (
        sharesA * (exitA - entryA) - sharesB * (exitB - entryB)
    )  # long A short B, flipped for short spread
    entryNotional = (
        sharesA * entryA + sharesB * entryB
    )  # gross dollars deployed at entry (A + B)
    exitNotional = (
        sharesA * exitA + sharesB * exitB
    )  # same shares at the exit prices. costBps is a fraction not bps (0.0005 = 5bps) -> charged on entry + exit notional
    cost = (
        costBps * (entryNotional + exitNotional) + 4.0 * commission
    )  # 4 executions per round trip
    return pnl, cost, pnl - cost


def liveWindow(dailyPnl):
    # first to last day with any P&L -> same window c++ metrics measures sharpe over
    nonZero = np.flatnonzero(dailyPnl != 0.0)
    if len(nonZero) == 0:
        return None
    return dailyPnl[nonZero[0] : nonZero[-1] + 1]  # +1 -> the last live day is included


def sharpeOf(dailyPnl, capitalBase):
    # annualised -> mean/sd of daily returns * sqrt(252). sd uses n-1 like the c++
    returns = dailyPnl / capitalBase
    sd = returns.std(ddof=1)
    return (
        0.0 if sd == 0 else returns.mean() / sd * np.sqrt(TRADING_DAYS)
    )  # flat series (sd 0) -> 0, same guard as  the c++


def maxDrawdownOf(dailyPnl):
    # biggest drop below the running high, high starts at 0 -> same walk as the c++
    equity = np.cumsum(dailyPnl)
    peak = np.maximum(0.0, np.maximum.accumulate(equity))
    return float((peak - equity).max())


# ------------------------------------------------------------------ check python rebuilds the c++ exactly


def verifyRun(name, daily, trades, params):
    # nothing below is trusted ulness python gets the same numbers as c++ from the exported data -> if not, the simulations would be testing a different strategy
    problems = []
    dailyPnl = daily["dailyPnl"].to_numpy()

    if len(trades) == 0:
        return problems  # nothing to check (and nothing to resample)

    # 1. the daily series must add up to the trade list (pnlCheck)
    if (
        abs(dailyPnl.sum() - params["netPnl"]) > 0.01
    ):  # to the cent, same limit as the c++  check
        problems.append(
            f"daily P&L sums to {dailyPnl.sum():.4f} but c++ net is {params['netPnl']:.4f}"
        )

    # 2. sharpe and max drawdown rebuilt from the daily series, capital base (mean entry notional) from the trade list
    capitalBase = trades["entryNotional"].mean()
    window = liveWindow(dailyPnl)
    if abs(capitalBase - params["avgExposure"]) > 1e-6:
        problems.append(f"capital base {capitalBase} vs c++ {params['avgExposure']}")
    if abs(sharpeOf(window, capitalBase) - params["sharpe"]) > 1e-9:
        problems.append(
            f"sharpe {sharpeOf(window, capitalBase)} vs c++ {params['sharpe']}"
        )
    if abs(maxDrawdownOf(window) - params["maxDrawdown"]) > 1e-6:
        problems.append(
            f"max drawdown {maxDrawdownOf(window)} vs c++ {params['maxDrawdown']}"
        )

    # 3. every trades P&L and cost rebuilt from the exported prices -> to the cent (really to ~1e-9)
    dateToIndex = {date: i for i, date in enumerate(daily["date"])}
    entryIndex = (
        trades["entryDate"].map(dateToIndex).to_numpy()
    )  # dates -> row numbers of the daily file
    exitIndex = trades["exitDate"].map(dateToIndex).to_numpy()
    pnl, cost, net = tradePnl(
        daily["closeA"].to_numpy(),
        daily["closeB"].to_numpy(),
        daily["betaPrior"].to_numpy(),
        entryIndex,
        exitIndex,
        trades["direction"].to_numpy(),
        params["capital"],
        params["costBps"],
        params["commissionPerExecution"],
    )
    worst = max(
        np.abs(pnl - trades["pnl"]).max(),
        np.abs(cost - trades["cost"]).max(),
        np.abs(net - trades["netPnl"]).max(),
    )
    if worst > 0.01:
        problems.append(f"trade P&L rebuild off by up to ${worst:.6f}")
    if (
        exitIndex - entryIndex != trades["holdingDays"].to_numpy()
    ).any():  # holding days = exit row - entry row (trading days)
        problems.append("holding days dont match entry/exit dates")
    return problems


# ------------------------------------------------------------------ test 1 -> block bootstrap


def blockBootstrap(window, capitalBase, blockLength, nResamples, rng):
    # resample the daily P&L in BLOCKS of days, not single days -> volatility clusters and trades last several days, so single days would make the range too narrow
    nDays = len(window)
    nBlocks = int(
        np.ceil(nDays / blockLength)
    )  # enough blocks to cover the window, the  extra days get trimmed
    sharpes, nets, drawdowns = [], [], []
    for start in range(0, nResamples, 500):  # chunks of 500 -> keeps memory small
        chunkSize = min(500, nResamples - start)
        blockStarts = rng.integers(
            0, nDays, size=(chunkSize, nBlocks)
        )  # one random start day per block -> chunkSize x nBlocks of them
        indices = (
            (blockStarts[:, :, None] + np.arange(blockLength)[None, None, :]) % nDays
        )  # circular -> a block running off the end wraps to the start, so every day is equalyl likely to be picked
        sample = window[
            indices.reshape(chunkSize, -1)[:, :nDays]
        ]  # glue the blocks together, trim to the real length

        returns = sample / capitalBase
        sd = returns.std(axis=1, ddof=1)  # axis 1 = days -> one sd per resample
        sharpes.append(
            np.where(sd > 0, returns.mean(axis=1) / np.where(sd > 0, sd, 1.0), 0.0)
            * np.sqrt(TRADING_DAYS)
        )  # sd 0 -> sharpe 0. the inner where only stops the divide by 0
        nets.append(sample.sum(axis=1))
        equity = np.cumsum(sample, axis=1)
        peak = np.maximum(
            0.0, np.maximum.accumulate(equity, axis=1)
        )  # same drawdown walk as maxDrawdownOf, one row per resample
        drawdowns.append((peak - equity).max(axis=1))
    return np.concatenate(sharpes), np.concatenate(nets), np.concatenate(drawdowns)


def runBootstrap(daily, trades, blockLength, nResamples, rng):
    # test 1 -> how uncertain are the sharpe, net P&L and max drawdown -> the real value of each + its 95% range over the resamples
    window = liveWindow(daily["dailyPnl"].to_numpy())
    capitalBase = trades[
        "entryNotional"
    ].mean()  # same capital base the c++ sharpe uses
    sharpes, nets, drawdowns = blockBootstrap(
        window, capitalBase, blockLength, nResamples, rng
    )
    return {
        "sharpe": sharpeOf(window, capitalBase),
        "sharpeLo": np.percentile(sharpes, 2.5),
        "sharpeHi": np.percentile(
            sharpes, 97.5
        ),  # 2.5 to 97.5 percentile -> the 95% range
        "probSharpeAbove0": (sharpes > 0).mean(),
        "net": window.sum(),  # type: ignore
        "netLo": np.percentile(nets, 2.5),
        "netHi": np.percentile(nets, 97.5),
        "maxDD": maxDrawdownOf(window),
        "maxDDLo": np.percentile(drawdowns, 2.5),
        "maxDDHi": np.percentile(drawdowns, 97.5),
    }


# ------------------------------------------------------------------ test 2 -> random entry null test


def placeRandomTrades(holds, allowedDays, nDays, rng):
    # drop the same holding periods onto random allowed days, never overlapping (one position at a time, like c++)
    occupied = np.zeros(nDays, dtype=bool)
    entries, exits, skipped = [], [], 0
    for hold in sorted(holds, reverse=True):  # longest first -> they are hardest to fit
        for _ in range(
            200
        ):  # a trade that cant be fitted in 200 tries is skipped and counted
            entryDay = allowedDays[rng.integers(len(allowedDays))]
            exitDay = entryDay + hold
            if (
                exitDay > nDays - 1 or occupied[entryDay : exitDay + 1].any()
            ):  # runs past the last day or overlaps a placed trade -> try again
                continue
            occupied[entryDay : exitDay + 1] = (
                True  # inclusive -> no entering on another trades exit day, same as c++
            )
            entries.append(entryDay)
            exits.append(exitDay)
            break
        else:  # for-else -> only reached when none of the tries fitted
            skipped += 1
    return np.array(entries, dtype=int), np.array(exits, dtype=int), skipped


def runNullTest(daily, trades, params, nSimulations, rng):
    # null hypothesis -> the z-score has NO skill. same holding periods, beta sizing, costs and gate-allowed days, but entry days AND directions picked at random
    closeA, closeB = daily["closeA"].to_numpy(), daily["closeB"].to_numpy()
    betaPrior = daily["betaPrior"].to_numpy()
    allowedDays = np.flatnonzero(
        daily["canEnter"].to_numpy() == 1
    )  # canEnter = 1 -> a day the gates + trade window let c++ open a trade
    holds = trades["holdingDays"].to_numpy()
    nDays = len(closeA)

    realNet = trades["netPnl"].sum()
    nullNets = np.empty(nSimulations)
    skippedTotal = 0
    for sim in range(nSimulations):
        entries, exits, skipped = placeRandomTrades(holds, allowedDays, nDays, rng)
        skippedTotal += skipped
        directions = rng.choice(
            [-1, 1], size=len(entries)
        )  # coin flip per trade -> long or short spread
        _, _, net = tradePnl(
            closeA,
            closeB,
            betaPrior,
            entries,
            exits,
            directions,
            params["capital"],
            params["costBps"],
            params["commissionPerExecution"],
        )
        nullNets[sim] = net.sum()

    # +1 on top and bottom -> the real result counts as one of the possibilities, so p can never be exactly 0
    pValue = (np.sum(nullNets >= realNet) + 1) / (nSimulations + 1)
    return {
        "realNet": realNet,
        "nullMean": nullNets.mean(),
        "nullLo": np.percentile(nullNets, 2.5),
        "nullHi": np.percentile(nullNets, 97.5),
        "pValue": pValue,
        "skippedRate": skippedTotal
        / (
            nSimulations * max(len(holds), 1)
        ),  # share of the trades it tried to place that never fitted
        "expectedNullMean": -trades["cost"].mean()
        * len(holds),  # random direction -> gross ~0, so roughly -costs
    }


# ------------------------------------------------------------------ test 3 -> synthetic paths

SYNTHETIC_DATES = np.arange(
    np.datetime64("2016-07-05"), np.datetime64("2026-07-03"), dtype="datetime64[D]"
)  # arange stops BEFORE the end date -> last day is 2026-07-02
SYNTHETIC_DATES = SYNTHETIC_DATES[
    np.is_busday(SYNTHETIC_DATES)
]  # weekdays, same calendar as the synthetic test pairs
SPREAD_SD = 0.015  # every half-life gets the SAME long run spread size -> only the speed changes between groups


def makeSyntheticPaths(halfLife, nPaths, rng):
    # nPaths pairs at once -> lnB random walk (1.5% daily vol), lnA = lnB + spread, spread an OU with this half-life
    nDays = len(SYNTHETIC_DATES)
    lnB = np.log(50.0) + np.cumsum(
        0.0003 + 0.015 * rng.standard_normal((nPaths, nDays)), axis=1
    )  # B starts near 50 -> 0.03% drift + 1.5% vol a day (in logs)
    spread = np.zeros((nPaths, nDays))
    if (
        halfLife is None
    ):  # halfLife None -> spread is a random walk (no reversion at all) -> the no-edge reference
        spread = np.cumsum(
            0.006 * rng.standard_normal((nPaths, nDays)), axis=1
        )  # 0.6% steps a day (log spread), nothing pulls them back
    else:
        theta = 1.0 - 0.5 ** (1.0 / halfLife)  # share of the gap closed per day
        shock = SPREAD_SD * np.sqrt(
            1.0 - (1.0 - theta) ** 2
        )  # daily shock that  keeps the long run sd at SPREAD_SD
        spread[:, 0] = SPREAD_SD * rng.standard_normal(
            nPaths
        )  # start already "in equilibrium"
        epsilon = rng.standard_normal((nPaths, nDays))
        for t in range(1, nDays):
            spread[:, t] = (1.0 - theta) * spread[:, t - 1] + shock * epsilon[
                :, t
            ]  # AR(1) -> keep (1 - theta) of yesterdays gap, add todays shock
    lnA = (
        np.log(40.0) - np.log(50.0) + lnB + spread
    )  # beta  = 1, alpha = ln(40/50) -> A sits near 40 when B is 50
    return np.exp(lnA), np.exp(lnB), spread


def writePriceFile(path, close):
    # the loaders need every price column (the legacy one volume too) -> the strategy only uses close, so open/high/low just repeat it
    lines = ["Date,Open,High,Low,Close,Volume"]
    for date, price in zip(SYNTHETIC_DATES, close):
        lines.append(f"{date},{price:.4f},{price:.4f},{price:.4f},{price:.4f},1000000")
    with open(path, "w") as file:
        file.write("\n".join(lines) + "\n")


def fittedHalfLife(spread):
    # OU slope by least squares -> same fit as fitOU in Signals.cpp. used to check the generator itself
    x, y = (
        spread[:-1],
        np.diff(spread),
    )  # x = yesterdays spread, y = the change to today
    slope = np.polyfit(x, y, 1)[0]
    return (
        -np.log(2.0) / np.log(1.0 + slope) if -1.0 < slope < 0.0 else np.nan
    )  # half-life = -ln(2) / ln(1 + slope). slope outside (-1, 0) -> NaN, left out of the median


def runSynthetic(binary, halfLives, nPaths, rng):
    # test 3 -> how does the edge depend on how fast the spread reverts (half-life) -> nPaths simulated pairs per half-life, each backtested by the c++ batch mode
    tempFolder = os.path.join(OUT_FOLDER, "paths_tmp")
    rows, generatorChecks = [], []
    for halfLife in halfLives:
        label = "none" if halfLife is None else str(halfLife)
        os.makedirs(
            tempFolder, exist_ok=True
        )  # remade for every group -> its deleted at the end of each one
        closeA, closeB, spread = makeSyntheticPaths(halfLife, nPaths, rng)
        if halfLife is not None:
            generatorChecks.append(
                (
                    halfLife,
                    np.nanmedian([fittedHalfLife(pathSpread) for pathSpread in spread]),
                )
            )  # median fitted half-life across the paths ->  should land near the one built in

        listPath = os.path.join(tempFolder, "pairs.csv")
        with open(listPath, "w") as file:  # the pair list the c++ batch mode reads
            file.write("label,fileA,fileB\n")
            for pathIndex in range(nPaths):
                fileA = os.path.join(tempFolder, f"h{label}_p{pathIndex}_a.csv")
                fileB = os.path.join(tempFolder, f"h{label}_p{pathIndex}_b.csv")
                writePriceFile(fileA, closeA[pathIndex])
                writePriceFile(fileB, closeB[pathIndex])
                file.write(f"h{label}_p{pathIndex},{fileA},{fileB}\n")
        resultsPath = os.path.join(tempFolder, "results.csv")
        runBinary(
            binary, ["batch", listPath, resultsPath]
        )  # c++ backtests every pair in the list -> 1 results row per pair
        results = pd.read_csv(resultsPath)
        results["halfLife"] = label
        rows.append(results)
        shutil.rmtree(tempFolder)  # ~25MB of price files per group -> not worth keeping
        print(f"  half-life {label:>4}: {nPaths} paths done")
    return pd.concat(rows, ignore_index=True), generatorChecks


def summariseSynthetic(allResults, halfLives):
    summary = []
    for halfLife in halfLives:
        label = "none" if halfLife is None else str(halfLife)
        group = allResults[allResults["halfLife"] == label]
        traded = group[
            group["trades"] >= MIN_TRADES
        ]  # sharpe from 2 or 3 trades is noise -> same rule as tests 1 + 2
        summary.append(
            {
                "halfLife": label,
                "paths": len(group),
                "pathsTraded": len(traded),
                "avgTrades": group["trades"].mean(),
                "cointegratedShare": group[
                    "cointegrated"
                ].mean(),  # 0/1 per path (johansen on the whole path) -> the mean is the share
                "netMean": group[
                    "netPnl"
                ].mean(),  # net P&L over ALL the paths, sharpe only over the traded ones
                "netLo": group["netPnl"].quantile(0.025),
                "netHi": group["netPnl"].quantile(0.975),
                "sharpeMean": traded["sharpe"].mean() if len(traded) else np.nan,
                "sharpeLo": traded["sharpe"].quantile(0.025) if len(traded) else np.nan,
                "sharpeHi": traded["sharpe"].quantile(0.975) if len(traded) else np.nan,
                "probSharpeAbove0": (traded["sharpe"] > 0).mean()
                if len(traded)
                else np.nan,
            }
        )
    return pd.DataFrame(summary)


def plotSynthetic(summary, path):
    import matplotlib

    matplotlib.use("Agg")  # no window -> just write the png
    import matplotlib.pyplot as plt

    labels = summary["halfLife"].tolist()
    x = np.arange(len(labels))
    figure, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    for panel, column, title in [
        (axes[0], "sharpe", f"Sharpe ratio (paths with {MIN_TRADES}+ trades)"),
        (axes[1], "net", "Net P&L per path ($), all paths"),
    ]:
        mean, low, high = (
            summary[column + "Mean"],
            summary[column + "Lo"],
            summary[column + "Hi"],
        )  # summary columns -> sharpeMean / sharpeLo / sharpeHi, same for net
        panel.errorbar(
            x, mean, yerr=[mean - low, high - mean], fmt="o", capsize=5, color="#1f5fa8"
        )  # yerr wants the distances down and up from the mean
        panel.axhline(0, color="grey", linewidth=0.8)
        panel.set_xticks(x)
        panel.set_xticklabels(labels)
        panel.set_xlabel("spread half-life (days)   ['none' = no reversion]")
        panel.set_title(title)
    figure.suptitle(
        "Edge vs half-life: mean and 95% range across synthetic paths (final design, 10 years each)"
    )
    figure.tight_layout()
    figure.savefig(path, dpi=130)
    plt.close(figure)


# ------------------------------------------------------------------ portfolio mode (tests 4-6)

# arm name in the c++ files -> its short code (D18), in the order test 4 prints the arms. tests 4 + 5 -> every statistic was fixed before any result existed (DECISIONS.md D20)
ARM_NAMES = {
    "kalman": "K",
    "static": "S",
    "kalman_static_legs": "KS",
    "rolling_ols": "SR",
    "static_train_z": "ST",
}
# the comparisons fixed in advance (D18, D21) -> the headline pair, the 2 halves of its decomposition, "is it the filter or just updating every day", then textbook z vs rolling z
DIFFERENCES = [
    ("kalman", "static"),
    ("kalman", "kalman_static_legs"),
    ("kalman_static_legs", "static"),
    ("kalman", "rolling_ols"),
    ("static_train_z", "static"),
]


def portfolioSharpe(returns):
    # the ONE sharpe (D20) -> daily net P&L / fixed capital, every day of the period, x sqrt(252), no risk-free rate
    sd = returns.std(ddof=1)  # ddof 1 -> n-1 (sample sd), same as the c++
    return 0.0 if sd == 0 else returns.mean() / sd * np.sqrt(TRADING_DAYS)


def loSE(sharpe, nDays):
    # lo (2002), IID  returns -> the daily sharpe has variance (1 + SR_daily^2 / 2)  / days, annualised below. same formula as portfolioMetrics in Research.cpp
    return np.sqrt(
        (1.0 + 0.5 * sharpe * sharpe / TRADING_DAYS) / (nDays / TRADING_DAYS)
    )


def loadPortfolio(prefix):
    daily = pd.read_csv(prefix + "_daily.csv")
    trades = pd.read_csv(
        prefix + "_trades.csv", keep_default_na=False
    )  # blanks stay "" and not NaN (the exit date of a trade still open)
    summary = pd.read_csv(prefix + "_summary.csv")
    tradeDays = pd.read_csv(
        prefix + "_tradepnl.csv"
    )  # gross P&L of each trade on each day it was open
    market = (
        pd.read_csv(prefix + "_market.csv")
        if os.path.exists(prefix + "_market.csv")
        else None
    )  # no market file -> test 6 is skipped
    return daily, trades, summary, tradeDays, market


def verifyPortfolio(daily, trades, summary, tradeDays):
    # same rule as the single-pair tests -> nothing is trusted unless python rebuilds the c++ numbers from the files
    problems = []
    capital = float(summary["capital"].iloc[0])
    for _, row in summary.iterrows():
        armDaily = daily[
            (daily["arm"] == row["arm"])
            & (daily["date"] >= row["from"])
            & (daily["date"] <= row["until"])
        ]  # dates are yyyy-mm-dd text -> they compare in date order, both ends included
        returns = armDaily["pnl"].to_numpy() / capital
        if len(returns) != row["days"]:
            problems.append(
                f"{row['arm']} {row['period']}: {len(returns)} days vs c++ {row['days']}"
            )
        elif (
            abs(portfolioSharpe(returns) - row["sharpe"]) > 1e-9
        ):  # elif -> only the first problem of a row is reported
            problems.append(
                f"{row['arm']} {row['period']}: sharpe {portfolioSharpe(returns)} vs c++ {row['sharpe']}"
            )
        elif abs(armDaily["pnl"].sum() - row["pnl"]) > 1e-6:
            problems.append(
                f"{row['arm']} {row['period']}: P&L {armDaily['pnl'].sum()} vs c++ {row['pnl']}"
            )
    for arm, armDaily in daily.groupby("arm"):
        armTrades = trades[trades["arm"] == arm]
        # every dollar of daily P&L must belong to a trade -> gross from the per-trade daily file, minus costs and borrow
        gross = tradeDays[tradeDays["arm"] == arm]["pnl"].sum()
        if abs(gross - armTrades["grossPnl"].astype(float).sum()) > 1e-6:
            problems.append(
                f"{arm}: per-trade daily P&L sums to {gross} but the trade list says {armTrades['grossPnl'].astype(float).sum()}"
            )
        if abs(armDaily["pnl"].sum() - armTrades["netPnl"].astype(float).sum()) > 1e-6:
            problems.append(
                f"{arm}: daily P&L {armDaily['pnl'].sum()} vs trades {armTrades['netPnl'].astype(float).sum()}"
            )
        if (
            abs(
                armDaily["pnl"].sum()
                - (gross - armDaily["cost"].sum() - armDaily["borrow"].sum())
            )
            > 1e-6
        ):
            problems.append(f"{arm}: daily P&L is not gross - costs - borrow")
    return problems


def blockIndices(nDays, blockLength, chunkSize, rng):
    # cricular block bootstrap -> blocks of consecutive days, a block running off the end wraps to the start
    nBlocks = int(np.ceil(nDays / blockLength))
    starts = rng.integers(0, nDays, size=(chunkSize, nBlocks))
    indices = (
        (starts[:, :, None] + np.arange(blockLength)[None, None, :]) % nDays
    )  # every start + 0, 1 .. blockLength - 1 -> chunkSize x nBlocks x blockLength day numbers, % nDays does the wrap
    return indices.reshape(chunkSize, -1)[
        :, :nDays
    ]  # chunkSize x nDays, the extra days of the last block trimmed off


def pairedBootstrap(returnsByArm, blockLength, nResamples, rng):
    # every arm is resampled with the SAME days -> the arms stay lined  up, so the difference between 2 arms is resampled correctly (same pairs on the same days -> strongly correlated)
    arms = list(returnsByArm)
    stack = np.vstack([returnsByArm[arm] for arm in arms])  # arms x days
    nDays = stack.shape[1]
    sharpesByArm = {arm: [] for arm in arms}
    for start in range(0, nResamples, 250):  # chunks -> keeps memory small
        chunkSize = min(250, nResamples - start)
        indices = blockIndices(nDays, blockLength, chunkSize, rng)
        sample = stack[:, indices]  # arms x chunkSize x days
        mean = sample.mean(axis=2)  # axis 2 = days -> one value per arm per resample
        sd = sample.std(axis=2, ddof=1)
        sharpe = np.where(sd > 0, mean / np.where(sd > 0, sd, 1.0), 0.0) * np.sqrt(
            TRADING_DAYS
        )  # portfolioSharpe for every arm and resample at once (sd 0 -> 0, same guard)
        for k, arm in enumerate(arms):
            sharpesByArm[arm].append(sharpe[k])
    return {
        arm: np.concatenate(chunks) for arm, chunks in sharpesByArm.items()
    }  # arm -> its nResamples sharpes, resample i used the same days in every arm


PERIOD_NAMES = {
    "development": "development",
    "design": "design windows",
    "validation": "validation",  # period key in the c++ summary -> the label printed here
    "holdout": "HOLD-OUT",
    "oos": "out of sample",
    "full": "full record",
}


def periodsOf(dates, summary):
    # (name, mask) for each period the c++ reported for this run, in its order (PREFIX_summary.csv) -> development (split into design + validation when the design was chosen on its first part), then the hold-out and the totals once unsealed
    periods = []
    firstArmRows = summary[
        summary["arm"] == summary["arm"].iloc[0]
    ]  # every arm has the same periods -> read them off the first arm
    for _, row in firstArmRows.iterrows():
        mask = (dates >= row["from"]) & (dates <= row["until"])
        name = f"{PERIOD_NAMES.get(row['period'], row['period'])} {row['from'][:4]}-{row['until'][:4]}"  # first 4 characters = the year -> e.g. development 2008-2022
        periods.append((name, mask))
    return periods


def runPortfolioBootstrap(daily, summary, capital, blockLengths, nResamples, seed):
    # test 4 -> 95% interval on each arms sharpe, and on the difference between 2 arms from the same resampled days
    wide = daily.pivot(
        index="date", columns="arm", values="pnl"
    ).sort_index()  # one row per date, one column per arm
    dates = wide.index.to_numpy()
    arms = [
        arm for arm in ARM_NAMES if arm in wide.columns
    ]  # ARM_NAMES order, only the arms this run has
    rows, differenceRows = [], []
    for periodName, mask in periodsOf(dates, summary):
        returnsByArm = {arm: wide[arm].to_numpy()[mask] / capital for arm in arms}
        nDays = int(mask.sum())
        for blockLength in blockLengths:
            # one stream per (period, block length) -> changing one never changes anothers numbers
            rng = np.random.default_rng(
                [seed, 4, len(rows), blockLength]
            )  # key = seed, test no, result rows so far (grows every pass, so it differs per period), block length
            bootSharpes = pairedBootstrap(returnsByArm, blockLength, nResamples, rng)
            for arm in arms:
                real = portfolioSharpe(returnsByArm[arm])
                rows.append(
                    {
                        "period": periodName,
                        "block": blockLength,
                        "arm": arm,
                        "days": nDays,
                        "sharpe": real,
                        "loSE": loSE(real, nDays),
                        "bootSE": bootSharpes[arm].std(
                            ddof=1
                        ),  # loSE = the IID formula, bootSE = sd of the resampled sharpes
                        "lo": np.percentile(bootSharpes[arm], 2.5),
                        "hi": np.percentile(bootSharpes[arm], 97.5),
                        "probAbove0": (bootSharpes[arm] > 0).mean(),
                    }
                )
            for arm, other in DIFFERENCES:
                if arm not in arms or other not in arms:
                    continue  # an arm this run doesnt have -> no difference to take
                bootDifference = (
                    bootSharpes[arm] - bootSharpes[other]
                )  # resample i has the same days in both arms -> a paired difference
                real = portfolioSharpe(returnsByArm[arm]) - portfolioSharpe(
                    returnsByArm[other]
                )
                differenceRows.append(
                    {
                        "period": periodName,
                        "block": blockLength,
                        "a": arm,
                        "b": other,
                        "difference": real,
                        "lo": np.percentile(bootDifference, 2.5),
                        "hi": np.percentile(bootDifference, 97.5),
                        "probAbove0": (bootDifference > 0).mean(),
                        "correlation": np.corrcoef(
                            returnsByArm[arm], returnsByArm[other]
                        )[0, 1],
                    }
                )  # [0, 1] -> off the diagonla of the 2 x 2 matrix = correlation of the 2 arms real daily returns (not resampled)
    return pd.DataFrame(rows), pd.DataFrame(differenceRows)


def runClusterNull(daily, trades, tradeDays, summary, capital, nSimulations, seed):
    # null hypothesis -> the DIRECTION of the trades carries no information. every trade keeps its pair, dates, size and costs -> the books clustering stays the real one, only the sign of the gross P&L is random (whole clusters at a time)
    from scipy import sparse  # only test 5 needs scipy

    rows = []
    for arm, armDaily in daily.groupby("arm"):
        armDaily = armDaily.sort_values("date")
        dates = armDaily["date"].to_numpy()
        dayIndex = {date: i for i, date in enumerate(dates)}
        armTrades = trades[trades["arm"] == arm]
        if len(armTrades) < MIN_TRADES:
            continue  # too few trades to test, same rule as tests 1-3
        week = pd.to_datetime(armTrades["entryDate"]).dt.strftime(
            "%G-%V"
        )  # ISO year-week
        clusterKey = (
            armTrades["group"] + "|" + week
        ).to_numpy()  # cluster  = same sub-industry AND same entry week -> those trades flip tgoether (not independent bets, e.g. one stocks jump opens many same-way trades)
        clusterIds, clusterOf = np.unique(
            clusterKey, return_inverse=True
        )  # clusterOf -> the cluster no of each trade
        clusterOfTrade = dict(zip(armTrades["id"].to_numpy(), clusterOf))

        armTradeDays = tradeDays[tradeDays["arm"] == arm]
        clusterMatrix = sparse.csr_matrix(
            (
                armTradeDays["pnl"].to_numpy(),
                (
                    armTradeDays["date"].map(dayIndex).to_numpy(),
                    armTradeDays["id"].map(clusterOfTrade).to_numpy(),
                ),
            ),
            shape=(len(dates), len(clusterIds)),
        )  # days x clusters -> gross P&L of each cluster each day (trades sharing a cell are summed)
        fixedCosts = (
            armDaily["cost"].to_numpy() + armDaily["borrow"].to_numpy()
        )  # paid whatveer the direction
        realPnl = armDaily["pnl"].to_numpy()
        rebuilt = (
            np.asarray(clusterMatrix.sum(axis=1)).ravel() - fixedCosts
        )  # every sign +1 has to give back the real daily P&L
        if np.abs(rebuilt - realPnl).max() > 1e-6:
            raise RuntimeError(
                f"{arm}: the cluster matrix does not rebuild the daily P&L"
            )

        for periodName, mask in periodsOf(dates, summary):
            rng = np.random.default_rng(
                [seed, 5, len(rows)]
            )  # own stream for every (arm, period)
            realSharpe = portfolioSharpe(realPnl[mask] / capital)
            periodMatrix = clusterMatrix[mask]
            periodFixedCosts = fixedCosts[mask]
            nullSharpes = []
            for start in range(
                0, nSimulations, 500
            ):  # chunks of 500 -> keeps memory small. the signs are drawn  a chunk at a time, so another chunk size gives other (equally random) books
                chunkSize = min(500, nSimulations - start)
                signs = rng.choice(
                    [-1.0, 1.0], size=(len(clusterIds), chunkSize)
                )  # +1 or -1 per cluster per random book, 50/50
                book = (
                    (periodMatrix @ signs - periodFixedCosts[:, None]) / capital
                )  # days x chunkSize -> flipped gross minus the real costs, over capital
                sd = book.std(axis=0, ddof=1)
                nullSharpes.append(
                    np.where(sd > 0, book.mean(axis=0) / np.where(sd > 0, sd, 1.0), 0.0)
                    * np.sqrt(TRADING_DAYS)
                )  # the statistic -> the portfolio sharpe of each random book
            nullSharpes = np.concatenate(nullSharpes)
            rows.append(
                {
                    "period": periodName,
                    "arm": arm,
                    "trades": len(armTrades),
                    "clusters": len(clusterIds),
                    "sharpe": realSharpe,
                    "nullMean": nullSharpes.mean(),
                    "nullLo": np.percentile(nullSharpes, 2.5),
                    "nullHi": np.percentile(nullSharpes, 97.5),
                    "pValue": (np.sum(nullSharpes >= realSharpe) + 1)
                    / (nSimulations + 1),
                }
            )  # p = (1 + random books at least as good as the real one) / (nSimulations + 1)
    return pd.DataFrame(rows)


def runMarketExposure(daily, market, summary, capital):
    # a pairs book should not need the market to go up -> beta of the daily return to SPYs, and the sharpe after taking that exposure out
    spy = market.set_index("date")["spy"]
    spyReturns = spy.pct_change()  # sipmle daily returns. the market file starts 1 day early -> the first test day has one too
    rows = []
    for arm, armDaily in daily.groupby("arm"):
        armDaily = armDaily.sort_values("date").set_index("date")
        returns = armDaily["pnl"] / capital
        both = pd.concat(
            [returns, spyReturns], axis=1, keys=["r", "spy"]
        ).dropna()  # only days with both a book return and a SPY return
        for periodName, mask in periodsOf(both.index.to_numpy(), summary):
            x, y = (
                both["spy"].to_numpy()[mask],
                both["r"].to_numpy()[mask],
            )  # x = SPY, y = the book
            beta = np.cov(y, x, ddof=1)[0, 1] / x.var(
                ddof=1
            )  # OLS slope of the book on SPY = cov  / var
            rows.append(
                {
                    "period": periodName,
                    "arm": arm,
                    "beta": beta,
                    "correlation": np.corrcoef(x, y)[0, 1],
                    "sharpe": portfolioSharpe(y),
                    "sharpeHedged": portfolioSharpe(y - beta * x),
                }
            )  # hedged = return - beta x SPY return
    return pd.DataFrame(rows)


def mainPortfolio(args):
    prefix = args.portfolio
    daily, trades, summary, tradeDays, market = loadPortfolio(prefix)
    capital = float(
        summary["capital"].iloc[0]
    )  # the fixed capital, same on every summary row
    armLabel = lambda arm: (
        f"{ARM_NAMES.get(arm, arm):<3}{arm}"
    )  # short label padded to 3, then the full arm name

    print(
        "=== check: python rebuilds the c++ portfolio numbers from the exported files ==="
    )
    problems = verifyPortfolio(daily, trades, summary, tradeDays)
    if problems:
        sys.exit(
            "python does not match c++ -> stopping, results would not be trustworthy:\n  "
            + "\n  ".join(problems)
        )
    print(
        f"  OK ({summary['arm'].nunique()} arms: sharpe, P&L and day count per period, and daily P&L = trades = gross - costs - borrow)"
    )

    blockLengths = (
        [args.pblock, 21, 126]
        if args.pblock not in (21, 126)
        else [args.pblock] + [block for block in (21, 63, 126) if block != args.pblock]
    )  # main block first,  then 2 more for the sensitivity lines (21 + 126, or 63 in place of the one already used)
    print(
        f"\n=== test 4: block bootstrap of the portfolio's daily returns ({args.boot} resamples, {args.pblock}-day blocks) ==="
    )
    bootTable, differenceTable = runPortfolioBootstrap(
        daily, summary, capital, blockLengths, args.boot, args.seed
    )
    bootTable.to_csv(prefix + "_mc_bootstrap.csv", index=False)
    differenceTable.to_csv(prefix + "_mc_differences.csv", index=False)
    mainRows = bootTable[bootTable["block"] == args.pblock]
    for period in mainRows["period"].unique():
        print(f"{period}:")
        print(
            f"  {'arm':<24} {'sharpe':>7} {'95% interval':>17} {'boot SE':>8} {'Lo SE':>6} {'P(>0)':>6}"
        )
        for _, row in mainRows[mainRows["period"] == period].iterrows():
            print(
                f"  {armLabel(row['arm']):<24} {row['sharpe']:7.2f} [{row['lo']:6.2f}, {row['hi']:6.2f}] {row['bootSE']:8.2f} {row['loSE']:6.2f} {row['probAbove0']:6.0%}"
            )
    print("differences between arms (same resampled days for both):")
    print(
        f"  {'period':<26} {'a minus b':<12} {'difference':>10} {'95% interval':>17} {'P(>0)':>6} {'corr':>5}"
    )
    for _, row in differenceTable[differenceTable["block"] == args.pblock].iterrows():
        name = f"{ARM_NAMES[row['a']]} - {ARM_NAMES[row['b']]}"
        print(
            f"  {row['period']:<26} {name:<12} {row['difference']:10.2f} [{row['lo']:6.2f}, {row['hi']:6.2f}] {row['probAbove0']:6.0%} {row['correlation']:5.2f}"
        )
    otherBlocks = [block for block in blockLengths if block != args.pblock]
    lastPeriod = bootTable["period"].iloc[
        -1
    ]  # the last period the run has (the full record once unsealed)
    print(
        f"block length sensitivity, {lastPeriod}, interval for each arm ({', '.join(str(block) for block in otherBlocks)}-day blocks):"
    )
    for _, row in bootTable[
        (bootTable["period"] == lastPeriod) & (bootTable["block"] != args.pblock)
    ].iterrows():
        print(
            f"  {armLabel(row['arm']):<24} {int(row['block']):>4}-day blocks  [{row['lo']:6.2f}, {row['hi']:6.2f}]"
        )

    print(
        f"\n=== test 5: cluster sign-flip null test ({args.pnull} random books per arm) ==="
    )
    print(
        "every trade keeps its pair, dates, size and costs -> only the direction is random, flipped for a whole"
    )
    print(
        "sub-industry-week at a time. p = share of random books with a sharpe at least as high as the real one"
    )
    nullTable = runClusterNull(
        daily, trades, tradeDays, summary, capital, args.pnull, args.seed
    )
    nullTable.to_csv(prefix + "_mc_null.csv", index=False)
    print(
        f"  {'period':<26} {'arm':<24} {'trades':>7} {'clusters':>8} {'sharpe':>7} {'random: mean':>13} {'95% range':>17} {'p-value':>8}"
    )
    for _, row in nullTable.iterrows():
        print(
            f"  {row['period']:<26} {armLabel(row['arm']):<24} {row['trades']:7d} {row['clusters']:8d} {row['sharpe']:7.2f} {row['nullMean']:13.2f}"
            f" [{row['nullLo']:6.2f}, {row['nullHi']:6.2f}] {row['pValue']:8.4f}"
        )

    if market is not None:
        print(
            "\n=== test 6: market exposure (beta to SPY, and the sharpe with that exposure hedged out) ==="
        )
        exposure = runMarketExposure(daily, market, summary, capital)
        exposure.to_csv(prefix + "_mc_market.csv", index=False)
        print(
            f"  {'period':<26} {'arm':<24} {'beta':>7} {'corr':>6} {'sharpe':>7} {'hedged':>7}"
        )
        for _, row in exposure.iterrows():
            print(
                f"  {row['period']:<26} {armLabel(row['arm']):<24} {row['beta']:7.3f} {row['correlation']:6.2f} {row['sharpe']:7.2f} {row['sharpeHedged']:7.2f}"
            )
    print(
        f"\nsaved: {prefix}_mc_bootstrap.csv, _mc_differences.csv, _mc_null.csv"
        + (", _mc_market.csv" if market is not None else "")
    )


# ------------------------------------------------------------------ main


def main():
    parser = argparse.ArgumentParser(
        description="monte carlo tests for the pairs backtester"
    )
    parser.add_argument(
        "--binary",
        default=os.path.join("build", "BackTester"),
        help="path to the built c++ program",
    )
    parser.add_argument(
        "--boot", type=int, default=10000, help="bootstrap resamples (test 1)"
    )
    parser.add_argument(
        "--block", type=int, default=20, help="bootstrap block length in days (test 1)"
    )
    parser.add_argument(
        "--null", type=int, default=5000, help="random strategies (test 2)"
    )
    parser.add_argument(
        "--paths", type=int, default=100, help="synthetic pairs per half-life (test 3)"
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=20261009,
        help="fixed seed -> every number is reproducible",
    )
    parser.add_argument(
        "--old-data",
        action="store_true",
        help="tests 1-3 on the original hand-downloaded files (missing dividend adjustments from 2023)",
    )
    parser.add_argument(
        "--unseal-holdout",
        action="store_true",
        help="tests 1-3: let the export read bulk-data prices after 2022-12-31 (the binary logs it)",
    )
    parser.add_argument(
        "--portfolio",
        default="",
        help="output prefix of a 'BackTester walkforward' run -> tests 4-6 instead of 1-3",
    )
    parser.add_argument(
        "--pblock",
        type=int,
        default=63,
        help="portfolio bootstrap block length in days (test 4)",
    )
    parser.add_argument(
        "--pnull", type=int, default=10000, help="random books per arm (test 5)"
    )
    args = parser.parse_args()

    os.chdir(PROJECT)  # every path is relative to the project folder, same as the c++
    if args.portfolio:
        mainPortfolio(args)
        return
    binary = os.path.abspath(
        args.binary
    )  # resolved AFTER the chdir ->  a relative path is taken from the project folder
    if not os.path.exists(binary):
        sys.exit(
            f"cant find the c++ program at {binary} -> build it first: cmake -B build && cmake --build build"
        )

    # separate random streams per test -> changing one tests size doesnt change another tests numbers
    rngBootstrap = np.random.default_rng([args.seed, 1])
    rngNull = np.random.default_rng([args.seed, 2])
    rngSynthetic = np.random.default_rng([args.seed, 3])

    print("=== exporting runs from c++ ===")
    exportArgs = (
        ["export"]
        + (["--old-data"] if args.old_data else [])
        + (["--unseal-holdout"] if args.unseal_holdout else [])
    )
    print(runBinary(binary, exportArgs), end="")

    print("\n=== check: python rebuilds every c++ number from the exported data ===")
    loaded = {}
    for name, kind in RUNS:
        daily, trades, params = loadRun(name)
        problems = verifyRun(name, daily, trades, params)
        if problems:
            sys.exit(
                f"{name}: python does not match c++ -> stopping, results would not be trustworthy:\n  "
                + "\n  ".join(problems)
            )
        print(
            f"  {name:11s} OK ({len(trades)} trades: P&L, costs, sharpe, drawdown all match)"
            f"   data {daily['date'].iloc[0]} to {daily['date'].iloc[-1]}"
        )
        loaded[name] = (daily, trades, params, kind)

    print(
        f"\n=== test 1: block bootstrap ({args.boot} resamples, {args.block}-day blocks), trading from 2020 to the end of each run's data ==="
    )
    print(
        f"{'run':11s} {'kind':24s} {'sharpe':>7s} {'95% range':>17s} {'P(>0)':>6s} {'net $':>8s} {'95% range':>17s} {'maxDD $':>8s} {'95% range':>15s}"
    )
    bootRows = []
    for name, (daily, trades, params, kind) in loaded.items():
        if len(trades) < MIN_TRADES:
            print(
                f"{name:11s} {kind:24s} skipped -> only {len(trades)} trades, too few to resample"
            )
            continue
        bootResult = runBootstrap(daily, trades, args.block, args.boot, rngBootstrap)
        bootRows.append({"run": name, **bootResult})
        print(
            f"{name:11s} {kind:24s} {bootResult['sharpe']:7.2f} [{bootResult['sharpeLo']:6.2f}, {bootResult['sharpeHi']:6.2f}] {bootResult['probSharpeAbove0']:6.0%}"
            f" {bootResult['net']:8.0f} [{bootResult['netLo']:6.0f}, {bootResult['netHi']:6.0f}] {bootResult['maxDD']:8.0f} [{bootResult['maxDDLo']:5.0f}, {bootResult['maxDDHi']:5.0f}]"
        )
    pd.DataFrame(bootRows).to_csv(
        os.path.join(OUT_FOLDER, "test1_bootstrap.csv"), index=False
    )

    print(
        f"\n=== test 2: random entry null test ({args.null} random strategies per run) ==="
    )
    print(
        "same trade count, holding periods, sizing, costs and gate-allowed days -> only WHEN and WHICH WAY are random"
    )
    print(
        f"{'run':11s} {'kind':24s} {'real net $':>10s} {'random: mean':>13s} {'95% range':>18s} {'p-value':>8s}  verdict"
    )
    nullRows = []
    for name, (daily, trades, params, kind) in loaded.items():
        if len(trades) < MIN_TRADES:
            print(
                f"{name:11s} {kind:24s} skipped -> only {len(trades)} trades, too few to test"
            )
            continue
        nullResult = runNullTest(daily, trades, params, args.null, rngNull)
        nullRows.append({"run": name, **nullResult})
        verdict = (
            "beats random timing"
            if nullResult["pValue"] < 0.05
            else "no better than random"
        )  # real P&L in the top 5% of the random strategies -> the timing is doing something
        print(
            f"{name:11s} {kind:24s} {nullResult['realNet']:10.0f} {nullResult['nullMean']:13.0f} [{nullResult['nullLo']:7.0f}, {nullResult['nullHi']:7.0f}] {nullResult['pValue']:8.3f}  {verdict}"
        )
    pd.DataFrame(nullRows).to_csv(
        os.path.join(OUT_FOLDER, "test2_null.csv"), index=False
    )

    halfLives = [2, 5, 10, 20, 40, None]  # trading days. None = no reversion at all
    print(
        f"\n=== test 3: synthetic paths ({args.paths} pairs per half-life, final design, full 10 years) ==="
    )
    allResults, generatorChecks = runSynthetic(
        binary, halfLives, args.paths, rngSynthetic
    )
    print(
        "  generator check (half-life built -> median fitted): "
        + ", ".join(f"{built} -> {fitted:.1f}" for built, fitted in generatorChecks)
    )
    summary = summariseSynthetic(allResults, halfLives)
    allResults.to_csv(os.path.join(OUT_FOLDER, "test3_all_paths.csv"), index=False)
    summary.to_csv(os.path.join(OUT_FOLDER, "test3_edge_vs_halflife.csv"), index=False)
    print(
        f"\n{'half-life':>9s} {str(MIN_TRADES) + '+ trd':>7s} {'trades':>7s} {'coint%':>7s} {'net $ mean':>11s} {'95% range':>19s} {'sharpe':>7s} {'95% range':>17s} {'P(>0)':>6s}"
    )
    for _, row in summary.iterrows():
        print(
            f"{row['halfLife']:>9s} {row['pathsTraded']:4d}/{row['paths']:<3d} {row['avgTrades']:6.1f} {row['cointegratedShare']:7.0%}"
            f" {row['netMean']:11.0f} [{row['netLo']:7.0f}, {row['netHi']:7.0f}] {row['sharpeMean']:7.2f}"
            f" [{row['sharpeLo']:6.2f}, {row['sharpeHi']:6.2f}] {row['probSharpeAbove0']:6.0%}"
        )
    plotPath = os.path.join(OUT_FOLDER, "test3_edge_vs_halflife.png")
    plotSynthetic(summary, plotPath)
    print(
        f"\nsaved: {OUT_FOLDER}/ (test1_bootstrap.csv, test2_null.csv, test3_*.csv, test3_edge_vs_halflife.png)"
    )


if __name__ == "__main__":
    main()
