//the trading loop -> one pass over z, opens and shuts pair trades and marks the daily  P&L
#pragma once
#include <string>
#include <vector>
#include "DataLoader.h"
#include "Kalman.h"
#include "Filters.h"

struct TradeParams {
    double capital = 10000.0; //dollars into leg A  per trade, B gets beta times that
    double entryZ = 2.0;
    double exitZ = 0.0;
    // 2 ways a trade can fail -> diverging (price keeps moving against us) hits the stop, stalilng hits the timeout
    double stopZ = 3.5; //widening to 6 was tried -> xom/cvx got much worse (2022 energy shock), v/ma a little better, so 3.5 stayed
    double maxEntryZ = 3.5; //refuse entries already this stretched -> a jump that big is more likely a break than a bargain
    double timeoutHalfLives = 5.0; //exit after this many half-lives -> (1/2)^5 ~ 3% of the expected stertch left. 0 = timeout off
    double costBps = 0.0005; //a fraction not bps (0.0005 = 5bps) -> charged on the notional at entry and at exit
    double commissionPerExecution = 0.0; //dollars per order -> 4 orders a round trip (2 legs in, 2 out)
    bool frozenExit = true; // exits on the spread FROZEN at entry. false -> live kalman z,  which drifts to 0 as the filter re-fits even when prices never revert
};

struct PairTrade {
    std::string entryDate, exitDate;
    int direction; //+1 = long spread (long A short B), -1 = short spread
    double zEntry, zExit; //entry = live z, exit = the z the exit rules judged (frozen if frozenExit)
    double pnl; //dollars -> per positon
    double entryNotional; // gross moeny deployed at entry (A + B)
    int holdingDays; //trading days (not calender)
    std::string exitReason; //"target", "stop", "timeout" or "end" (data ran out)
    double cost; //total friciton paid on round trip
    double netPnl; //pnl - cost
};

std::vector<PairTrade> runTradingLoop(const std::vector<AlignedBar>& bars,
            const KalmanOutput& kalman, const std::vector<double>& z, //z[i] belongs to  bars[i+1], kalman starts on day 2
            const std::vector<double>& zMean, const std::vector<double>& zSd, //the yardstick behind each z
            const std::vector<double>& halfLife, //half-life known each day -> sets each trades timeout at entry
            const std::vector<GateDay>& gates, const TradeParams& params,
            std::vector<double>* dailyPnlOut = nullptr); //optional daily p&L out
