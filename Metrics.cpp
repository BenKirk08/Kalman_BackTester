#include "Metrics.h"
#include "Trading.h"
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <iomanip>

static const double TRADING_DAYS = 252.0; //trading days in a year

Metrics computeMetrics(const std::vector<PairTrade>& trades, 
    const std::vector<double> &dailyPnl) {
    Metrics metrics {};
    metrics.trades = trades.size();
    double sumWins = 0, sumLosses = 0, sumHold = 0, sumExposure = 0;
    for (const PairTrade& trade : trades) {
        metrics.grossPnl += trade.pnl;
        metrics.costs += trade.cost;
        metrics.netPnl += trade.netPnl;
        sumHold += trade.holdingDays;
        sumExposure += trade.entryNotional;
        metrics.daysInMarket += (size_t)trade.holdingDays; //entry day not counted -> same days that get a daily mark

        if (trade.netPnl > 0) { metrics.wins++; sumWins += trade.netPnl; } // a trade netting exactly 0 counts as a loss
        else { metrics.losses++; sumLosses += trade.netPnl; }
        if (trade.exitReason == "stop") metrics.stops++;
        if (trade.exitReason == "timeout") metrics.timeouts++;
    }

    if (metrics.trades > 0) {
        metrics.winRate = 100.0 * (double)metrics.wins / (double)metrics.trades;
        metrics.avgHoldingDays = sumHold / (double)metrics.trades;
        metrics.avgExposure = sumExposure / (double)metrics.trades;
        metrics.avgWin = metrics.wins ? sumWins /(double)metrics.wins : 0.0; //with divide 0 check
        metrics.avgLoss = metrics.losses ? sumLosses / (double)metrics.losses : 0.0;
        //>1 = profitable
        metrics.profitFactor = (sumLosses != 0.0) ? sumWins / std::abs(sumLosses) : 0.0; // losses sum to 0 (e.g. no losers) -> 0, not infinity
    }

    //only measuring stratagy on live days -> ie start to end
    size_t first = dailyPnl.size(), last = 0;
    bool any = false;
    for (size_t i = 0; i < dailyPnl.size(); i++) {
        if (dailyPnl[i] != 0.0) {
            if (!any) { first = i; any = true; } //entry days are 0 -> the span starts after the first entry
            last = i; 
        }
    }

    if (any && metrics.avgExposure > 0.0) {
        metrics.liveDays = last - first + 1; //both ends count
        const double capitalBase = metrics.avgExposure; // denomenator for returns

        double sum = 0, sumSq = 0;
        double equity = 0, peak = 0; //peak starts at 0 -> a loss from the very first day already counts as drawdown
        for (size_t i = first; i <= last; i++) {
            double dailyReturn = dailyPnl[i] / capitalBase; //daily return as a fraction of capital
            sum += dailyReturn;
            sumSq += dailyReturn * dailyReturn;

            equity += dailyPnl[i]; //cumulative P&L = the equity curve
            if (equity > peak) peak = equity;
            double drawdown = peak - equity; //how far below peak we are
            if (drawdown > metrics.maxDrawdown) metrics.maxDrawdown = drawdown;
        }
        
        const double n = (double)metrics.liveDays;
        const double mean = sum / n;
        //sample variance (n-1) straigth from the 2 sums ->  (sum x^2 - n.mean^2) / (n-1), 0 for a single day
        const double variance = (n > 1.0) ? (sumSq - n * mean * mean) / (n - 1.0) : 0.0;
        const double sd = std::sqrt(variance > 0.0 ? variance : 0.0); //rounding can leae it a hair below 0

        metrics.annualReturn = 100.0 * mean * TRADING_DAYS; //returns scale w/ t
        metrics.annualVol = 100.0 * sd * std::sqrt(TRADING_DAYS); //vol scales w/ sqrt t
        metrics.sharpe = (sd > 0.0) ? (mean / sd) * std::sqrt(TRADING_DAYS) : 0.0;
        metrics.maxDrawdownPct = 100.0 * metrics.maxDrawdown / capitalBase;
        metrics.calmar = (metrics.maxDrawdownPct > 0.0) ? metrics.annualReturn / metrics.maxDrawdownPct : 0.0;
        metrics.timeInMarket = 100.0 * (double)metrics.daysInMarket / n; //%

        // self check that the daily series must reconcile w/the trade list
        double sumDaily = 0;
        for (double pnl : dailyPnl) sumDaily += pnl; // whole series is fine -> days outside  the live  span are 0
        metrics.pnlCheck = sumDaily - metrics.netPnl;
    }
    return metrics;
}

void outputMetrics(const Metrics& metrics) {
    std::cout << "\n<------Metrics------>\n" << std::fixed;
    std::cout << std::setprecision(2)
              << "trades " << metrics.trades << "   win rate " << metrics.winRate << "%"
              << "   stops " << metrics.stops
              << "   timeouts " << metrics.timeouts
              << "   avg hold " << metrics.avgHoldingDays << " days\n"
              << "avg win $" << metrics.avgWin << "   avg loss $" << metrics.avgLoss
              << "   profit factor " << metrics.profitFactor << "\n"
              << "gross $" << metrics.grossPnl << "   costs $" << metrics.costs
              << "   net $" << metrics.netPnl << "\n"
              << "capital base (avg gross exposure) $" << metrics.avgExposure
              << "   time in market " << metrics.timeInMarket << "%\n"
              << "annual return " << metrics.annualReturn << "%"
              << "   annual vol " << metrics.annualVol << "%\n"
              << "SHARPE " << std::setprecision(3) << metrics.sharpe
              << "   max drawdown $" << std::setprecision(2) << metrics.maxDrawdown
              << " (" << metrics.maxDrawdownPct << "% of capital)"
              << "   calmar " << std::setprecision(2) << metrics.calmar << "\n";

    //pnlCheck must be ~0 -> if not, the daily series and the trade list disagree
    if (std::abs(metrics.pnlCheck) > 0.01)
        std::cout << "!! P&L RECONCILIATION FAILED: " << metrics.pnlCheck << "\n";

    //sticky formatting -> put cout back. everything printed after (e.g. NIS) used to get rounded to 2dp, 0.334 showed as 0.33
    std::cout << std::defaultfloat << std::setprecision(6);
}