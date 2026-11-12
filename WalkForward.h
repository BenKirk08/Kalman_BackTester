#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "Backtest.h"

//walk forward -> re-estimate Q, R, entry/exit z & z window each fold from ONLY the train years, then trade the next year

//small grids on purpose -> every extra setting tried is another chance to find one that only looked good by luck
struct ParamGrid {
    std::vector<double> Q = {1e-7, 3e-7, 1e-6, 3e-6, 1e-5};
    std::vector<double> R = {1e-6, 3e-6, 1e-5, 3e-5, 1e-4, 3e-4}; //only searched when R is fixed -> adaptive R estimates itself
    std::vector<double> entryZ = {2.0, 1.5, 2.5}; // defaults first -> on a tie the default wins
    std::vector<double> exitZ = {0.0, 0.5};
    std::vector<size_t> zWindow = {60, 40, 90};
};

struct WalkForwardParams {
    int firstTestYear = 2020; //first year traded out of sample
    int lastTestYear = 2026;
    int trainYears = 3; //train window  length before each test year. 0 -> anchored (train from the very start)
    bool refit = true; //true -> re-estimate every fold. false -> keep base settings (the control to compare against)
    size_t minTrainTrades = 8; //fewer train trades than this -> train sharpe is noise so dont trust  it
    size_t likelihoodBurnIn = 100; //skip the first days in the likelihood -> P still settling from its made up start
};

// what was cosen and what happened for one fold (one test year)
struct FoldResult {
    int testYear = 0;
    double Q = 0, R = 0; //R = starting R if adaptive
    double entryZ = 0, exitZ = 0;
    size_t zWindow = 0;
    double trainSharpe = 0; //sharpe of the chosen settings on the train years (what the search maximised)
    size_t trainTrades = 0;
    bool traded = false; // false -> no setting earned a positive train  sharpe with enough trades so stood aside
    std::vector<PairTrade> testTrades; //out of sample trades only
    double testNet = 0;
};

struct WalkForwardResult {
    std::string nameA, nameB;
    bool refit = true;
    bool adaptiveR = false;
    std::vector<FoldResult> folds;
    std::vector<PairTrade> oosTrades; // every fold test trades joined -> the only honest performance record
    std::vector<double> oosDailyPnl; // every fold test daily P&L summed onto one timeline
    Metrics oosMetrics;
};

WalkForwardResult runWalkForward(const std::string& fileA, const std::string& fileB,
                                 const BacktestConfig& base, const WalkForwardParams& params,
                                 const ParamGrid& grid = ParamGrid());
void outputWalkForward(const WalkForwardResult& result);
