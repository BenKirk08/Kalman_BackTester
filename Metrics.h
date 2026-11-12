//stats for one run -> tallies from the trade list, returns, sharpe and drawdown from the daily P&L
#pragma once
#include <cstddef>
#include <vector>
#include "Trading.h"

struct Metrics {
    //trade tallies -> win = NET pnl above 0, everything else a loss
    size_t trades = 0, wins = 0, losses = 0, stops = 0;
    size_t timeouts = 0; //exits where the reversion never came in time -> stalled, not diverged
    double winRate = 0; //% of trades with NET positvie
    double avgWin, avgLoss; //dollars of NET pnl, avgLoss  is never positive
    double profitFactor = 0; //total winnings / total losses
    double avgHoldingDays = 0; //average days holding trade before sell

    //dollars summed over all trades
    double grossPnl = 0, costs = 0, netPnl = 0;
    double avgExposure = 0; //mean gross dollars deployed per trade

    //daily series level -> returns are daily P&L / avgExposure
    size_t liveDays = 0, daysInMarket = 0; //first to last nonzero  P&L day, sum of holding days
    double timeInMarket = 0; //%
    double annualReturn = 0; //%
    double annualVol = 0; //%
    double sharpe = 0; //exccess over zero annualised
    double maxDrawdown = 0; //money (postive/absolute)
    double maxDrawdownPct = 0; //% of avgExposure
    double calmar = 0; // annnualReturn / maxDrawdownPct
    double pnlCheck = 0; // check -> where sum of dailyPnl - netPnl approx 0
};

Metrics computeMetrics(const std::vector<PairTrade>& trades,
                        const std::vector<double>& dailyPnl);
void outputMetrics(const Metrics& metrics);