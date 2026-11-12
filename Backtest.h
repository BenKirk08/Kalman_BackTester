#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "Cointegration.h"
#include "Kalman.h"
#include "Signals.h"
#include "Trading.h"
#include "Filters.h"
#include "Evaluation.h"
#include "Metrics.h"

//one pair backtest end to end -> config in, result + report out
struct BacktestConfig {
    //kalman
    KalmanParams kalman;

    //signal
    size_t zWindow = 60;

    //UO tradability band (used for both verdict and rolling)
    double minHalfLife = 0.5; 
    double maxHalfLife = 30.0; 

    //components
    bool useGates = true;
    GateParams gates;
    TradeParams trade;

    //used to restrict trading to a specific window
    std::string tradeFrom = ""; //empty -> from the start
    std::string tradeUntil = ""; //empty -> to the end

    //drop bars dated after this (empty -> keep all). tradeUntil only stops ENTRIES, dataEnd removes the prices -> dev run cant see hold-out (DECISIONS.md D2)
    std::string dataEnd = "";
    //drop bars dated before this (empty -> keep all). bulk download goes back  to 2005, original files start 2016-07-05 -> used to line them  up
    std::string dataStart = "";
};

struct BacktestResult {
    std::string nameA, nameB;
    std::vector<AlignedBar> bars; //the aligned prices this run traded on -> kept so exportRun can write them out
    JohansenResult johansen;
    KalmanOutput kalman;
    OUResult ou;
    Metrics metrics;
    std::vector<double> dailyPnl;
    std::vector<double> z;
    std::vector<double> zMean, zSd; //the yardstick behind each z -> trading freezes the entry days pair
    std::vector<double> halfLife; //frozen spread half-life known each day -> each trades timeout is set from it at entry
    std::vector<GateDay> gates;
    std::vector<PairTrade> trades;
    PairVerdict verdict;
};

BacktestResult runBacktest(const std::string& fileA, const std::string& fileB,
            const BacktestConfig& config);

std::string shortName(const std::string& path); //"Data/Input/v_Data.csv" -> "v"

// loads both files, aligns by date, applies dataStart / dataEnd -> runBacktest and walk-forward share it so see the same bars
std::vector<AlignedBar> loadAlignedBars(const std::string& fileA, const std::string& fileB, const BacktestConfig& config);

// writes a run to 3 csvs (_daily, _trades, _params) so python (analysis/monte_carlo.py) can resample it & check it matches the c++ metrics
void exportRun(const BacktestResult& result, const BacktestConfig& config, const std::string& prefix);

void outputBacktest(const BacktestResult& result, const std::string& run = ""); //full report. run = label e.g. "train 2016-21"
void outputSummaryHeader(); //header
void outputSummaryRow(const BacktestResult& result, const std::string& run = ""); //one line. run = label for the row