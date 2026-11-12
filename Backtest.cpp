#include "Backtest.h"
#include "Cointegration.h"
#include "DataLoader.h"
#include "Evaluation.h"
#include "Filters.h"
#include "Kalman.h"
#include "Signals.h"
#include "Trading.h"
#include "Metrics.h"
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>
#include <iomanip>

//if specified start/end date -> shut gate days outside
static void applyTradeWindow(std::vector<GateDay>& gates, 
    const std::vector<std::string>& dates, 
    const std::string& from, const std::string& until) {
    for (size_t i = 0; i < gates.size(); i++) {
        if (!from.empty() && dates[i] < from) gates[i].canEnter = false;
        if (!until.empty() && dates[i] > until) gates[i].canEnter = false;
    }
}
// "data/input/v_Data.csv" -> "v" for table rows being readable. not static -> walk forward uses it too
std::string shortName(const std::string& path) {
    size_t slashPos = path.find_last_of('/');
    std::string name = (slashPos == std::string::npos) ? path : path.substr(slashPos + 1);
    size_t suffixPos = name.find("_Data");
    if (suffixPos != std::string::npos) name = name.substr(0, suffixPos);
    return name;
}


std::vector<AlignedBar> loadAlignedBars(const std::string& fileA, const std::string& fileB, const BacktestConfig& config) {
    std::vector<Bar> barsA = loadCSV(fileA);
    std::vector<Bar> barsB = loadCSV(fileB);
    std::vector<AlignedBar> bars = alignBars(barsA, barsB);
    if (!config.dataEnd.empty()) { // dates are sorted -> cut everything after dataEnd off the end
        while (!bars.empty() && bars.back().date > config.dataEnd) bars.pop_back();
    }
    if (!config.dataStart.empty()) { //and evreything before dataStart off the front
        size_t first = 0;
        while (first < bars.size() && bars[first].date < config.dataStart) first++;
        bars.erase(bars.begin(), bars.begin() + (std::ptrdiff_t)first);
    }
    if (bars.size() < 2) throw std::runtime_error("fewer than 2 common days: " + fileA + " / " + fileB);
    return bars;
}

BacktestResult runBacktest(const std::string &fileA, const std::string &fileB,
    const BacktestConfig& config) {
    BacktestResult result;
    result.nameA = shortName(fileA); result.nameB = shortName(fileB);

    // loading
    std::vector<AlignedBar> bars = loadAlignedBars(fileA, fileB, config);
    result.bars = bars; // kept for exportRun -> ptyhon needs the exact prices the trades used

    //kalman, z-scores and gates are causal -> day t only uses data up to day t, so they run over everything
    result.kalman = runKalman(bars, config.kalman);
    result.z = rollingZScore(result.kalman.spread, config.zWindow, &result.zMean, &result.zSd);
    //half life of the FROZEN spread (what exits judge), window borrowed from gates ouWindow (250 days ~1 year)
    result.halfLife = rollingFrozenHalfLife(bars, result.kalman, config.gates.ouWindow);

    //johansen + full OU fit are NOT causal -> must stop at tradeUntil or a train verdict peeks ahead
    size_t diagnosticEnd = result.kalman.spread.size();
    if (!config.tradeUntil.empty()) {
        while (diagnosticEnd > 0 && result.kalman.dates[diagnosticEnd - 1] > config.tradeUntil) diagnosticEnd--; // walk back past the cutoff
    }

    //spread[i] belongs to bars[i+1] -> spread [0, diagEnd) uses bars [0, diagEnd] so slice needs diagEnd + 1 bars
    std::vector<AlignedBar> diagnosticBars(bars.begin(), bars.begin() + (std::ptrdiff_t)diagnosticEnd + 1);
    result.johansen = johansenTest(diagnosticBars);
    result.ou = fitOU(result.kalman.spread, 0, diagnosticEnd);

    //applying  gates
    GateParams gateParams = config.gates;
    gateParams.minHalfLife = config.minHalfLife;
    gateParams.maxHalfLife = config.maxHalfLife;
    result.gates = computeGates(bars, result.kalman, gateParams);
    if (!config.useGates) result.gates = openGates(result.gates);
    applyTradeWindow(result.gates, result.kalman.dates, config.tradeFrom, config.tradeUntil);

    //applying trades
    result.trades = runTradingLoop(bars, result.kalman, result.z, result.zMean, result.zSd, result.halfLife, result.gates, config.trade, &result.dailyPnl);
    result.metrics = computeMetrics(result.trades, result.dailyPnl);
    result.verdict = judgePair(result.johansen, result.ou, result.trades, config.minHalfLife, config.maxHalfLife);
    return result;
}

void exportRun(const BacktestResult& result, const BacktestConfig& config, const std::string& prefix) {
    //ofstream wont create directories itself
    std::filesystem::path folder = std::filesystem::path(prefix).parent_path();
    if (!folder.empty()) std::filesystem::create_directories(folder);

    // 17 significant digits -> a double survives the round trip to text exactly
    const int FULL_DIGITS = 17;

    //1. one row per day (spread index i). prices are bars[i+1] -> same offset the trading loop uses
    std::ofstream dailyFile(prefix + "_daily.csv");
    if (!dailyFile) throw std::runtime_error("couldnt write " + prefix + "_daily.csv");
    dailyFile << "index,date,closeA,closeB,betaPrior,canEnter,dailyPnl\n" << std::setprecision(FULL_DIGITS);
    for (size_t i = 0; i < result.kalman.spread.size(); i++) {
        dailyFile << i << "," << result.kalman.dates[i] << ","
              << result.bars[i + 1].closeA << "," << result.bars[i + 1].closeB << ","
              << result.kalman.betaPrior[i] << "," << (result.gates[i].canEnter ? 1 : 0) << "," //canEnter -> days a trade was allowed to open
              << result.dailyPnl[i] << "\n";
    }

    //2. one row per trade -> python checks it can rebuild every one of these from the daily prices
    std::ofstream tradesFile(prefix + "_trades.csv");
    if (!tradesFile) throw std::runtime_error("couldnt write " + prefix + "_trades.csv");
    tradesFile << "entryDate,exitDate,direction,zEntry,zExit,pnl,cost,netPnl,entryNotional,holdingDays,exitReason\n"
           << std::setprecision(FULL_DIGITS);
    for (const PairTrade& trade : result.trades) {
        tradesFile << trade.entryDate << "," << trade.exitDate << "," << trade.direction << "," << trade.zEntry << "," << trade.zExit << ","
               << trade.pnl << "," << trade.cost << "," << trade.netPnl << "," << trade.entryNotional << ","
               << trade.holdingDays << "," << trade.exitReason << "\n";
    }

    //3. costs used + the c++ answers -> python must match these before any simualtion result is trusted
    std::ofstream paramsFile(prefix + "_params.csv");
    if (!paramsFile) throw std::runtime_error("couldnt write " + prefix + "_params.csv");
    paramsFile << "capital,costBps,commissionPerExecution,trades,netPnl,sharpe,maxDrawdown,avgExposure\n"
           << std::setprecision(FULL_DIGITS)
           << config.trade.capital << "," << config.trade.costBps << "," << config.trade.commissionPerExecution << ","
           << result.metrics.trades << "," << result.metrics.netPnl << "," << result.metrics.sharpe << "," << result.metrics.maxDrawdown << ","
           << result.metrics.avgExposure << "\n";
}

void outputBacktest(const BacktestResult& result, const std::string& run) {
    std::cout << "\n============ " << result.nameA << " / " << result.nameB;
    if (!run.empty()) std::cout << "   (" << run << ")"; //which run this is -> train / test / full etc
    std::cout << " ============\n";
    // filter, z-scores and gates alwasy run over all the data -> only trades are windowed, so later dates are expected
    if (!run.empty()) std::cout << "(filter + z-scores use all dates, trades only happen inside the run's window)\n";
    outputJohansenResults(result.johansen);
    outputKalmanResults(result.kalman);
    outputOUResults(result.ou);
    outputZScoreResults(result.z, result.kalman);
    outputGateStats(result.gates);
    //no separate before/after cost printout -> metrics already shows gross, costs and net
    outputPairVerdict(result.verdict, result.ou);
    outputMetrics(result.metrics);
    outputNISResults(result.kalman, 100, result.kalman.spread.size());
}

void outputSummaryHeader() {
    std::cout << std::left << std::setw(12) << "pair"
                           << std::setw(16) << "run"
              << std::right << std::setw(8) << "trades"
                            << std::setw(8) << "win%"
                            << std::setw(7) << "stops"
                            << std::setw(11) << "gross"
                            << std::setw(11) << "cost"
                            << std::setw(11) << "net"
              << "   tradable?\n" << std::string(94, '-') << "\n";
}

void outputSummaryRow(const BacktestResult &result, const std::string& run) {
    double gross = 0, cost = 0, net = 0;
    size_t wins = 0, stops = 0;
    for (const PairTrade& trade : result.trades) {
        gross += trade.pnl; cost += trade.cost; net += trade.netPnl;
        if (trade.netPnl > 0) wins++;
        if (trade.exitReason == "stop") stops++;
    }
    double winRate = result.trades.empty() ? 0.0 : 100.0 * (double)wins / (double)result.trades.size();

    std::cout << std::left << std::setw(12) << (result.nameA + "/" + result.nameB)
              << std::setw(16) << run //which run this row is -> without it the rows for one pair look identical
              << std::right << std::fixed
              << std::setw(8) << result.trades.size()
              << std::setw(8) << std::setprecision(1) << winRate
              << std::setw(7) << stops
              << std::setw(11) << std::setprecision(0) << gross
              << std::setw(11) << cost
              << std::setw(11) << net
              << "    " << (result.verdict.tradable ? "yes" : "no")
    << "\n";
    std::cout << std::defaultfloat << std::setprecision(6);   
}