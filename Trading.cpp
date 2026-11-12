#include "Trading.h"
#include "DataLoader.h"
#include "Filters.h"
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <vector>
#include <iostream>

std::vector<PairTrade> runTradingLoop(const std::vector<AlignedBar>& bars,
        const KalmanOutput& kalman, const std::vector<double>& z,
        const std::vector<double>& zMean, const std::vector<double>& zSd,
        const std::vector<double>& halfLife,
        const std::vector<GateDay>& gates, const TradeParams& params, std::vector<double>* dailyPnlOut) {
    std::vector<PairTrade> trades;
    //one daily mark to market per z index -> 0 on flat days and entry days
    if (dailyPnlOut) dailyPnlOut->assign(z.size(), 0.0);

    int state = 0; //0 = flat, +1 = long spread, -1 = short spread
    bool coolOff = false; //after a stop or timeout -> no re-entry until  z back into band
    double sharesA = 0, sharesB = 0;
    size_t entryIndex = 0; // z index of the entry day -> its bar is bars[entryIndex + 1]
    double alpha0 = 0, beta0 = 0, mean0 = 0, sd0 = 0; //frozen at entry alongside the legs -> the filter and rolling mean/sd move on, the shares held dont
    double zHold = 0; //the z the exit rules judge each day while holding
    size_t timeoutDays = 0; //frozen at entry -> how long this trade gets to revert. 0  = no timeout for this trade
    PairTrade openTrade{};

    for (size_t i = 0; i < z.size(); i++) {
        if (std::isnan(z[i])) continue; //dont even try to enter until z-scores
        const AlignedBar& bar = bars[i+1]; // z[i] describes bar[i+1] !!!!!! as kalman only satrts on 2nd day
        
        if (state == 0) {
            if (coolOff) { //if was outside stop then now back in
                if (std::abs(z[i]) < params.entryZ) coolOff = false;
                continue; //no entry on the release day either -> z is inside the band then so it couldnt enter anyway
            }
            if (!gates[i].canEnter) continue; //gates filter out unwanted days
            if (std::abs(z[i]) > params.entryZ && std::abs(z[i]) < params.maxEntryZ) { //incase massive jump, prevent  going in when already abd
                state = (z[i] < 0) ? +1 : -1; //cheap -> long spread, rich -> short spread
                
                // gross exposure = capital (A) +  beta.capital (B) = (1+beta).capital
                sharesA = params.capital / bar.closeA;
                sharesB = kalman.betaPrior[i] * params.capital / bar.closeB; //beta times the dollars of A -> long  spread P&L ~ capital * (dlnA - beta.dlnB)
                openTrade = {};
                openTrade.entryDate = bar.date;
                openTrade.direction = state;
                openTrade.zEntry = z[i];
                entryIndex = i;

                // at entry the frozen spread IS the innovation (same alpha, beta, prices) -> frozen z = live z today, they split from tomorrow
                alpha0 = kalman.alphaPrior[i];
                beta0 = kalman.betaPrior[i];
                mean0 = zMean[i];
                sd0 = zSd[i]; //never 0 here -> z[i] passed the entry band so it was finite, which needs sd > 0
                zHold = z[i]; //in case data ends on the entry day -> the end block still has a sensible zExit

                //timeout frozen at entry from the half-life konwn today (trailing window, so no look ahead)
                double halfLifeNow = halfLife[i];
                if (params.timeoutHalfLives > 0.0 && !std::isnan(halfLifeNow) && halfLifeNow >= 0.0) {
                    timeoutDays = (size_t)std::ceil(params.timeoutHalfLives * halfLifeNow); //round up -> never cut short
                    if (timeoutDays < 1) timeoutDays = 1; //white nise (hl 0) -> 1 day, cant exit before the next bar anyway
                } else {
                    //timeouts off, NaN (window not full yet) or -1 (no pull back measured) -> no timeout, target and stop only
                    timeoutDays = 0;
                }
            }
        } else {
            //mark against yesterdays close as bars[i] is yersterdays as bar == bar[i+1]. entry days get 0 (here not reached)
            if (dailyPnlOut) (*dailyPnlOut)[i] = state * (sharesA * (bar.closeA - bars[i].closeA)
                            - sharesB * (bar.closeB - bars[i].closeB));

            //which z do the exits judge?
            if (params.frozenExit) {
                //todays prices through the ENTRY relationship -> P&L ~ state * capital * (change in frozenSpread), so z home = money home
                double frozenSpread = bar.lnCloseA - (alpha0 + beta0 * bar.lnCloseB);
                zHold = (frozenSpread - mean0) / sd0;
            } else {
                //old way -> live innovation z, drifts back to 0 as the filter absorbs the mispricing whether or not prices revert
                zHold = z[i];
            }

            // target = z back withni +- of normal, stop = stretched past stopZ against us (loss)
            bool target = (state == +1 && zHold >= -params.exitZ) ||
                          (state == -1 && zHold <= params.exitZ);
            bool stop = (state == +1 && zHold < -params.stopZ) ||
                        (state == -1 && zHold > params.stopZ);
            //timeout ONLY when neither of the others fired today -> a real target or stop always wins the label
            bool timeout = !target && !stop && timeoutDays > 0 && (i - entryIndex) >= timeoutDays;

            if (target || stop || timeout) {
                const AlignedBar& entryBar = bars[entryIndex + 1]; //for P&L (offset remeber)
                openTrade.pnl = state * (sharesA * (bar.closeA - entryBar.closeA) // state flips the sign -> long spread earns when A rises and B falls
                                    - sharesB * (bar.closeB - entryBar.closeB));
                
                //frictino -> notional moved at entry + exit & taxed at costBps per executuion
                double entryNotional = sharesA * entryBar.closeA + sharesB * entryBar.closeB;
                double exitNotional = sharesA * bar.closeA + sharesB * bar.closeB;
                openTrade.cost = params.costBps * (entryNotional + exitNotional) + 4.0 * params.commissionPerExecution; //4 for round trip
                openTrade.netPnl = openTrade.pnl - openTrade.cost;
                openTrade.entryNotional = entryNotional; //for metrics capital base
                if (dailyPnlOut) (*dailyPnlOut)[i] -= openTrade.cost; //cahrge friction on the day of exit

                openTrade.exitDate = bar.date;
                openTrade.zExit = zHold; //the z that actually triggered the exit
                openTrade.holdingDays = (int)(i - entryIndex); // entry day not counted
                openTrade.exitReason = stop ? "stop" : (target ? "target" : "timeout");
                trades.push_back(openTrade);
                state = 0;
                if (stop || timeout) coolOff = true; //both mean the reversion bet failed -> wait for normal before re-entering
            }
        }
    }
    //ended while holding -> close at the last  bar
    if (state != 0) {
        const AlignedBar& lastBar = bars.back(); //same bar as the last z index (z has 1 less entry than bars)
        const AlignedBar& entryBar = bars[entryIndex + 1];
        openTrade.pnl = state * (sharesA * (lastBar.closeA - entryBar.closeA)
                            - sharesB * (lastBar.closeB - entryBar.closeB));

        double entryNotional = sharesA * entryBar.closeA + sharesB * entryBar.closeB;
        double exitNotional = sharesA * lastBar.closeA + sharesB * lastBar.closeB;
        openTrade.cost = params.costBps * (entryNotional + exitNotional) + 4.0 * params.commissionPerExecution;
        openTrade.netPnl = openTrade.pnl - openTrade.cost;
        openTrade.entryNotional = entryNotional;
        if (dailyPnlOut) (*dailyPnlOut)[z.size() - 1] -= openTrade.cost; //exit cost lands on the last day like a normal exit

        openTrade.exitDate = lastBar.date;
        openTrade.zExit = zHold; //last z the exit rules saw -> matches whichever exit mode is on
        openTrade.holdingDays = (int)(z.size() - 1 - entryIndex); //last  z index - entry index -> same  count as a normal exit on that day
        openTrade.exitReason = "end";
        trades.push_back(openTrade);
    }

    return trades;
}