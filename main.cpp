// entry point -> the original report + every command line mode (export, facts, batch, dump, verify, bench, walkforward, trace)
#include "Backtest.h"
#include "WalkForward.h"
#include "Holdout.h"
#include "Market.h"
#include "Verify.h"
#include "Bench.h"
#include "Research.h"
#include "Engine.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <cmath>
#include <algorithm>

//hand-downloaded files miss dividend adjustments from 2023 (tools/check_dividend_adjustment.py) -> the report reads the stooq bulk download, "--old-data" puts the old files back
struct ReportFiles {
    std::string cuk = "Data/Input/cuk_Data.csv", ccl = "Data/Input/ccl_Data.csv"; //old files only
    std::string xom, cvx, cola, pepsi, visa, mastercard;
    std::string dataStart; //first date used from the bulk files ("" for the old files -> they start thre anyway)
};
static ReportFiles reportFiles(bool oldData) {
    ReportFiles files;
    if (oldData) {
        files.xom = "Data/Input/xom_Data.csv";
        files.cvx = "Data/Input/cvx_Data.csv";
        files.cola = "Data/Input/cola_Data.csv";
        files.pepsi = "Data/Input/pepsi_Data.csv";
        files.visa = "Data/Input/v_Data.csv";
        files.mastercard = "Data/Input/ma_Data.csv";
    } else {
        files.xom = "Data/Input/stooq/xom_Data.csv";
        files.cvx = "Data/Input/stooq/cvx_Data.csv";
        files.cola = "Data/Input/stooq/ko_Data.csv"; //bulk files are named by ticker -> ko, pep
        files.pepsi = "Data/Input/stooq/pep_Data.csv";
        files.visa = "Data/Input/stooq/v_Data.csv";
        files.mastercard = "Data/Input/stooq/ma_Data.csv";
        files.dataStart = "2016-07-05"; // the old files first day -> old and corrected runs start toegther
    }
    return files;
}

//the normal report (no argument) -> single runs, walk forward, then the exit design table
static int runAll(bool oldData) {
    const ReportFiles files = reportFiles(oldData);
    const std::string xomFile = files.xom, cvxFile = files.cvx, visaFile = files.visa,
                      mastercardFile = files.mastercard;
    // old files are exemtp from the seal, the bulk ones are NOT -> warn that the report is cut short
    if (!oldData && !holdoutUnsealed()) {
        std::cout << "NOTE: the hold-out is sealed, so this report only sees prices up to "
                  << HOLDOUT_LAST_DEV_DATE_TEXT
                  << ". the README tables (2016-2026) need: ./build/BackTester --unseal-holdout\n\n";
    }
    std::cout << "=== single runs: train = 2016-2021, test = 2022-2026 (out of sample) ===\n";
    outputSummaryHeader();
    BacktestConfig config;
    config.dataStart = files.dataStart;

    BacktestConfig train = config;
    train.tradeUntil = "2021-12-31"; //no entries after this. johansen + OU verdict stops here too -> no peeking ahead

    BacktestConfig test = config;
    test.tradeFrom = "2022-01-01"; //no entries before this -> filter and gates have still run from the first bar

    // same 3 windows for both pairs -> in-sample, out-of-sample,  full
    outputSummaryRow(runBacktest(xomFile, cvxFile, train), "train 2016-21");
    outputSummaryRow(runBacktest(xomFile, cvxFile, test), "test 2022-26");
    outputSummaryRow(runBacktest(xomFile, cvxFile, config), "full 2016-26");
    outputSummaryRow(runBacktest(visaFile, mastercardFile, train), "train 2016-21");
    outputSummaryRow(runBacktest(visaFile, mastercardFile, test), "test 2022-26");
    outputSummaryRow(runBacktest(visaFile, mastercardFile, config), "full 2016-26");

    BacktestConfig adaptive = config; //same as full sample but R adapts daily
    adaptive.kalman.adaptiveR = true;
    outputSummaryRow(runBacktest(xomFile, cvxFile, adaptive), "full, adaptive R");
    outputSummaryRow(runBacktest(visaFile, mastercardFile, adaptive), "full, adaptive R");

    outputBacktest(runBacktest(visaFile, mastercardFile, train), "train 2016-21"); //the one full report

    //control = the same folds traded with fixed settings -> the only difference is the re-fitting itself
    WalkForwardParams control;
    control.refit = false;
    WalkForwardParams refit; //defaults -> 3 year rolling train, test years 2020-2026

    //"adaptive" from above is reused -> with adaptive R the likelihood only searches Q, R estimates itself online
    for (const auto& pair : {std::pair<std::string, std::string>{visaFile, mastercardFile}, {xomFile, cvxFile}}) {
        outputWalkForward(runWalkForward(pair.first, pair.second, config, control)); //fixed settings
        outputWalkForward(runWalkForward(pair.first, pair.second, config, refit)); //re-fit Q, R + trading settings
        outputWalkForward(runWalkForward(pair.first, pair.second, adaptive, refit)); //re-fit Q + trading settings
    }

    //exit design sensitivity -> the README table comes from here. walk forward  control so ONLY the exit rule and the stop change
    std::cout << "\n=== exit design sensitivity (walk forward control, out of sample 2020-2026) ===\n"
              << "frozen = exits judged on the spread the trade entered on (final design), live-z = old exits\n"
              << "pair      exits    stopZ  trades     net   sharpe   maxDD\n";
    for (const auto& pair : {std::pair<std::string, std::string>{visaFile, mastercardFile}, {xomFile, cvxFile}}) {
        for (bool frozen : {true, false}) { // true = final design, false = the old live-z exits
            for (double stopZ : {3.5, 6.0}) { //3.5 = the default stop, 6 = the wide one that was tried
                BacktestConfig variant = config;
                variant.trade.frozenExit = frozen;
                variant.trade.stopZ = stopZ;
                const Metrics metrics = runWalkForward(pair.first, pair.second, variant, control).oosMetrics;
                std::cout << std::left << std::setw(10) << (shortName(pair.first) + "/" + shortName(pair.second))
                          << std::setw(9) << (frozen ? "frozen" : "live-z") << std::setw(7) << stopZ << std::right
                          << std::fixed << std::setw(6) << metrics.trades << std::setprecision(0) << std::setw(8)
                          << metrics.netPnl << std::setprecision(2) << std::setw(9) << metrics.sharpe
                          << std::setprecision(0) << std::setw(8) << metrics.maxDrawdown << "\n"
                          << std::defaultfloat << std::setprecision(6); //sticky formatting -> reset every row
            }
        }
    }

    return 0;
}

//"BackTester facts" -> the single numbers the README findings quote for the original pairs. one command, so none of them has to be taken on trust
static int runFacts(bool oldData) {
    const ReportFiles files = reportFiles(oldData);
    struct Pair {
        std::string name, fileA, fileB;
        bool bulk; //true -> read from the bulk download, so dataStart applies
    };
    std::vector<Pair> pairs = {
        {"KO/PEP", files.cola, files.pepsi, !oldData},
        {"XOM/CVX", files.xom, files.cvx, !oldData},
        {"V/MA", files.visa, files.mastercard, !oldData},
        {"CUK/CCL", files.cuk, files.ccl, false}, //old files only -> CUK delisted in May 2026, not in the bulk download
    };
    // the same-index control. only exists in the bulk download
    if (!oldData) pairs.push_back({"SPY/IVV", "Data/Input/stooq/spy_Data.csv", "Data/Input/stooq/ivv_Data.csv", true});

    std::cout << "pair      days   trace  critical  cointegrated  gate open   NIS fixed R  NIS adaptive R"
                 "  innov. half-life (fixed -> adaptive)  frozen half-life (median)  most extreme z\n";
    for (const Pair& pair : pairs) {
        BacktestConfig config;
        if (pair.bulk) config.dataStart = files.dataStart;
        const BacktestResult fixedRun = runBacktest(pair.fileA, pair.fileB, config);
        BacktestConfig adaptiveConfig = config;
        adaptiveConfig.kalman.adaptiveR = true; // same pair again, ONLY the R rule differs
        const BacktestResult adaptiveRun = runBacktest(pair.fileA, pair.fileB, adaptiveConfig);

        size_t gateDays = 0, openDays = 0;
        for (const GateDay& gate : fixedRun.gates) {
            if (!gate.hasEnoughHistory) continue; //warm-up days are left out of the share
            gateDays++;
            if (gate.johansenOK) openDays++; //johansen gate ONLY, the half-life gate is not counted
        }
        //half-life of the spread a trade would freeze, on every day it can be measured
        std::vector<double> frozenHalfLives;
        for (double halfLife : fixedRun.halfLife)
            if (!std::isnan(halfLife) && halfLife > 0.0) frozenHalfLives.push_back(halfLife);
        std::sort(frozenHalfLives.begin(), frozenHalfLives.end()); //sorted -> the midde one is the median
        double extremeZ = 0;
        std::string extremeZDate;
        for (size_t i = 0; i < fixedRun.z.size(); i++) {
            //NaN z (e.g. z window not full yet) is skipped. biggest |z| wins but the sign is kept
            if (!std::isnan(fixedRun.z[i]) && std::abs(fixedRun.z[i]) > std::abs(extremeZ)) {
                extremeZ = fixedRun.z[i];
                extremeZDate = fixedRun.kalman.dates[i]; //z and kalman.dates share an index
            }
        }
        //NIS skips the first 100 days. median = sorted[size / 2] -> the upper middle one when the count is even
        std::cout << std::left << std::setw(9) << pair.name << std::right << std::fixed << std::setw(5)
                  << fixedRun.bars.size() << std::setprecision(2) << std::setw(8) << fixedRun.johansen.trace0
                  << std::setw(10) << fixedRun.johansen.criticalValue << std::setw(14)
                  << (fixedRun.johansen.isCointegrated ? "yes" : "no") << std::setprecision(0) << std::setw(10)
                  << (gateDays ? 100.0 * (double)openDays / (double)gateDays : 0.0) << "%" << std::setprecision(3)
                  << std::setw(14) << computeNIS(fixedRun.kalman, 100, fixedRun.kalman.spread.size()) << std::setw(16)
                  << computeNIS(adaptiveRun.kalman, 100, adaptiveRun.kalman.spread.size()) << std::setprecision(2)
                  << std::setw(22) << fixedRun.ou.halfLife << " -> " << adaptiveRun.ou.halfLife << std::setprecision(1)
                  << std::setw(22) << (frozenHalfLives.empty() ? 0.0 : frozenHalfLives[frozenHalfLives.size() / 2])
                  << std::setprecision(1) << std::setw(14) << extremeZ << " on " << extremeZDate << "\n"
                  << std::defaultfloat << std::setprecision(6); //sticky formatting -> put cout back
    }
    std::cout << "\ngate open = share of days (after the 750-day warm-up) on which the rolling johansen gate is open.\n"
              << "NIS = mean of e^2/S from day 100 on, should be ~1. half-lives in trading days.\n";

    //finding 3 -> with the old live-z exits the stop hadrly ever fires, with frozen exits it does its job
    std::cout
        << "\nexits in the walk-forward control (2020-2026, stop 3.5): how many of each kind, and how many of them made money before costs\n"
        << "pair      exits     trades  targets (gross > 0)   stops (gross > 0)   timeouts\n";
    WalkForwardParams control;
    control.refit = false; //the same folds traded with fixed settings
    for (const Pair& pair : pairs) {
        if (pair.name != "V/MA" && pair.name != "XOM/CVX") continue; // same 2 pairs as the exit design table
        for (bool frozen : {true, false}) {
            BacktestConfig config;
            if (pair.bulk) config.dataStart = files.dataStart;
            config.trade.frozenExit = frozen; //stopZ stays at its default 3.5
            const WalkForwardResult walkForward = runWalkForward(pair.fileA, pair.fileB, config, control);
            size_t targets = 0, targetWins = 0, stops = 0, stopWins = 0, timeouts = 0;
            for (const PairTrade& trade : walkForward.oosTrades) {
                if (trade.exitReason == "target") {
                    targets++;
                    if (trade.pnl > 0) targetWins++; //pnl is before costs (netPnl is after)
                } else if (trade.exitReason == "stop") {
                    stops++;
                    if (trade.pnl > 0) stopWins++;
                } else if (trade.exitReason == "timeout") timeouts++; //an "end" exit lands in no column, ONLY in trades
            }
            std::cout << std::left << std::setw(10) << pair.name << std::setw(9) << (frozen ? "frozen" : "live-z")
                      << std::right << std::setw(7) << walkForward.oosTrades.size() << std::setw(9) << targets << " ("
                      << targetWins << ")" << std::setw(15) << stops << " (" << stopWins << ")" << std::setw(15)
                      << timeouts << "\n";
        }
    }
    return 0;
}

//"BackTester export" -> writes the runs the monte carlo resamples (tests 1 + 2) into Data/Output/montecarlo/
static int runExport(bool oldData) {
    const ReportFiles files = reportFiles(oldData);
    const std::string xomFile = files.xom, cvxFile = files.cvx, visaFile = files.visa,
                      mastercardFile = files.mastercard;
    const std::string colaFile = files.cola, pepsiFile = files.pepsi, cukFile = files.cuk, cclFile = files.ccl;
    const std::string folder = "Data/Output/montecarlo/";
    BacktestConfig outOfSample;
    outOfSample.tradeFrom = "2020-01-01"; //same out of sample years as the walk forward, as one continuous run
    outOfSample.dataStart = files.dataStart;

    //no-edge control -> independent random walks with the gates OFF so it actually trades. the null test must say "no skill" here
    BacktestConfig noGates = outOfSample;
    noGates.useGates = false;

    struct ExportJob {
        std::string name, fileA, fileB;
        BacktestConfig config;
    };
    const std::vector<ExportJob> jobs = {
        {"v_ma", visaFile, mastercardFile, outOfSample},
        {"xom_cvx", xomFile, cvxFile, outOfSample},
        {"cuk_ccl", cukFile, cclFile, outOfSample}, //always the old files
        {"cola_pepsi", colaFile, pepsiFile, outOfSample},
        {"syn_fast", "Data/Input/synthetic/syn_fast_a_Data.csv", "Data/Input/synthetic/syn_fast_b_Data.csv",
         outOfSample}, //edge built in
        {"syn_indep", "Data/Input/synthetic/syn_indep_a_Data.csv", "Data/Input/synthetic/syn_indep_b_Data.csv",
         noGates}, //no edge
    };
    for (const ExportJob& job : jobs) {
        BacktestResult result = runBacktest(job.fileA, job.fileB, job.config);
        exportRun(result, job.config, folder + job.name); //name is a file prefix -> _daily, _trades, _params csv
        std::cout << "exported " << job.name << " -> " << result.trades.size() << " trades\n";
    }
    return 0;
}

//"BackTester batch <pairList.csv> <results.csv> --legacy" -> the original pipeline over every pair in the list, one results row per pair
static int runBatch(const std::string& listFile, const std::string& resultsFile) {
    std::ifstream list(listFile);
    if (!list) throw std::runtime_error("couldnt open pair list: " + listFile);
    std::ofstream results(resultsFile);
    if (!results) throw std::runtime_error("couldnt write results: " + resultsFile);

    //17 digits -> a double survives the trip to text and back exatcly
    results << "label,trades,netPnl,sharpe,maxDrawdown,trace0,cointegrated,ouHalfLife\n" << std::setprecision(17);
    BacktestConfig config; //final design, full sample
    std::string line;
    std::getline(list, line); //skip header
    size_t done = 0;
    while (std::getline(list, line)) {
        if (line.empty()) continue;
        std::stringstream lineStream(line);
        std::string label, fileA, fileB; //list columns -> label,fileA,fileB
        std::getline(lineStream, label, ',');
        std::getline(lineStream, fileA, ',');
        std::getline(lineStream, fileB, ',');

        BacktestResult result = runBacktest(fileA, fileB, config);
        results << label << "," << result.metrics.trades << "," << result.metrics.netPnl << "," << result.metrics.sharpe
                << "," << result.metrics.maxDrawdown << "," << result.johansen.trace0 << ","
                << (result.johansen.isCointegrated ? 1 : 0) << "," << result.ou.halfLife << "\n";
        done++;
    }
    std::cout << "batch: " << done << " pairs -> " << resultsFile << "\n";
    return 0;
}

//"BackTester dump <pairList.csv> <out.csv> [--end YYYY-MM-DD]" -> regression fingerprint of the LEGACY pipeline. every trade of every pair to 17 significant digits + a digest of the daily P&L
static int runDump(const std::string& listFile, const std::string& outFile, const std::string& dataEnd) {
    std::ifstream list(listFile);
    if (!list) throw std::runtime_error("couldnt open pair list: " + listFile);
    std::ofstream out(outFile);
    if (!out) throw std::runtime_error("couldnt write: " + outFile);
    out << std::setprecision(17);

    BacktestConfig config; //final design, full sample
    config.dataEnd = dataEnd; //"" keeps every bar. a date -> later bars are dropped before anytihng is computed
    std::string line;
    std::getline(list, line); //skip header
    size_t done = 0, failed = 0;
    while (std::getline(list, line)) {
        if (line.empty()) continue;
        std::stringstream lineStream(line);
        std::string label, fileA, fileB;
        std::getline(lineStream, label, ',');
        std::getline(lineStream, fileA, ',');
        std::getline(lineStream, fileB, ',');
        try {
            BacktestResult result = runBacktest(fileA, fileB, config);
            //P row -> one per pair. bars, trades, then 3 sums of the daily P&L (plain, absolute, day weighted)
            double sum = 0, sumAbs = 0, weighted = 0;
            for (size_t i = 0; i < result.dailyPnl.size(); i++) {
                sum += result.dailyPnl[i];
                sumAbs += std::abs(result.dailyPnl[i]);
                //weight i + 1 so day 0 counts too -> this sum changes if the same P&L lands on a different day
                weighted += result.dailyPnl[i] * (double)(i + 1);
            }
            out << "P," << label << "," << result.bars.size() << "," << result.trades.size() << "," << sum << ","
                << sumAbs << "," << weighted << "\n";
            //T row -> one per trade
            for (const PairTrade& trade : result.trades) {
                out << "T," << label << "," << trade.entryDate << "," << trade.exitDate << "," << trade.direction << ","
                    << trade.zEntry << "," << trade.zExit << "," << trade.pnl << "," << trade.cost << ","
                    << trade.netPnl << "," << trade.entryNotional << "," << trade.holdingDays << "," << trade.exitReason
                    << "\n";
            }
        } catch (const std::exception& error) {
            out << "E," << label << "," << error.what() << "\n"; //one bad pair must not stop the other 1,400
            failed++;
        }
        done++; //counts the  pairs that threw too
    }
    std::cout << "dump: " << done << " pairs (" << failed << " threw) -> " << outFile << "\n";
    return 0;
}

//what the trace flags can change
struct TraceOptions {
    FillModel fill = FillModel::NextOpen; // --fill close -> SameClose
    double rebase = 100.0; //--rebase X -> both legs are rebased to this before any model sees them (D13)
};

// reads --fill and --rebase off the trace command line
static TraceOptions parseTraceOptions(int argc, char* argv[]) {
    TraceOptions options;
    for (int i = 5; i < argc; i++) { //argv[2], [3], [4] are the 2 files and the prefix -> flags start at 5
        const std::string arg = argv[i];
        // i + 1 < argc -> a flag with no value after it is ignored. ++i steps over the  value
        if (arg == "--fill" && i + 1 < argc)
            options.fill = std::string(argv[++i]) == "close" ? FillModel::SameClose : FillModel::NextOpen;
        else if (arg == "--rebase" && i + 1 < argc) options.rebase = std::stod(argv[++i]);
    }
    return options;
}

//the traced run itself -> one pair through the engine, every bar it saw kept in rows
static std::vector<EngineTrade> runTraceEngine(const PairData& data, const TraceOptions& options,
                                               std::vector<TraceRow>& rows) {
    const BacktestConfig baseConfig{};
    EngineConfig engineConfig = legacyEngineConfig(baseConfig);
    engineConfig.fill = options.fill;
    engineConfig.rebase = options.rebase;
    engineConfig.forceCloseAtEnd = false; //whatever is open at the last bar stays open -> no "end" exits
    engineConfig.legacyNanSkip = false; //a bar with a 0/0 z-score still marks an open position
    PairEngine engine(engineConfig);
    const int signal = engine.addKalmanSignal(baseConfig.kalman, baseConfig.zWindow);
    StrategyConfig strategyConfig = legacyStrategyConfig(baseConfig);
    strategyConfig.minBeta = 0.25; //hedge-ratio guard -> no entry unless the hedge ratio is inside [0.25, 4] (D14)
    strategyConfig.maxBeta = 4.0;
    engine.addRun(signal, strategyConfig);
    engine.traceSignal(signal, &rows);
    for (size_t t = 0; t < data.n; t++) {
        engine.onBar(barAt(data, t));
        if (engine.candidateCount()) engine.approveAll(); //one pair, no book to refues an entry -> approve them all
    }
    engine.finish();
    return std::move(engine.trades); //the engine is done -> hand its trade list over without a copy
}

//the bars file -> one line per bar with everything the strategy saw on it
static void writeTraceBars(const std::string& prefix, const PairData& data, const std::vector<TraceRow>& rows) {
    std::ofstream barsFile(prefix + "_bars.csv");
    if (!barsFile) throw std::runtime_error("couldnt write " + prefix + "_bars.csv");
    //blank = not available on that bar (window not full yet, or an unusable open)
    barsFile << "bar,date,openA,openB,closeA,closeB,lnA,lnB,alpha,beta,spread,z,zMean,zSd,johansenOK,gateMeanReverting,"
                "gateHalfLife,frozenHalfLife,sigma\n"
             << std::setprecision(17);
    for (const TraceRow& row : rows) {
        barsFile << row.bar << "," << dateToString(row.date) << ",";
        //x == x fails ONLY for NaN -> an unusable open is written as a blank
        if (data.openA[row.bar] == data.openA[row.bar]) barsFile << data.openA[row.bar];
        barsFile << ",";
        if (data.openB[row.bar] == data.openB[row.bar]) barsFile << data.openB[row.bar];
        barsFile << "," << data.closeA[row.bar] << "," << data.closeB[row.bar] << "," << row.lnA << "," << row.lnB
                 << "," << row.alpha << "," << row.beta << "," << row.spread << ",";
        if (row.hasZ) barsFile << row.z << "," << row.zMean << "," << row.zSd;
        else barsFile << ",,"; //3 blank fields (z, zMean, zSd) -> the 2 commas keep the columns lined up
        barsFile << "," << (row.johansenOK ? 1 : 0) << ",";
        if (row.hasGate) barsFile << (row.gateMeanReverting ? 1 : 0) << "," << row.gateHalfLife;
        else barsFile << ","; //2 blank gate fields
        barsFile << ",";
        if (row.hasFrozenHalfLife) barsFile << row.frozenHalfLife;
        barsFile << "," << row.sigma << "\n";
    }
}

//the trades file -> one line per trade the traced run made
static void writeTraceTrades(const std::string& prefix, const std::vector<EngineTrade>& engineTrades) {
    std::ofstream tradesFile(prefix + "_trades.csv");
    if (!tradesFile) throw std::runtime_error("couldnt write " + prefix + "_trades.csv");
    //signalBar = bar whose close triggered the entry. entryBar and exitBar = bars the 2 orders were FILLED on
    tradesFile
        << "signalBar,entryBar,exitBar,direction,zEntry,zExit,sharesA,sharesB,entryPriceA,entryPriceB,exitPriceA,"
           "exitPriceB,pnl,cost,exitReason\n"
        << std::setprecision(17);
    for (const EngineTrade& trade : engineTrades) {
        tradesFile << trade.signalBar << "," << trade.entryBar << "," << trade.exitBar << "," << trade.direction << ","
                   << trade.zEntry << "," << trade.zExit << "," << trade.sharesA << "," << trade.sharesB << ","
                   << trade.entryPriceA << "," << trade.entryPriceB << "," << trade.exitPriceA << ","
                   << trade.exitPriceB << "," << trade.pnl << "," << trade.cost << "," << exitReasonName(trade.reason)
                   << "\n";
    }
}

//"BackTester trace <fileA> <fileB> <outPrefix> [--fill open|close] [--rebase X]" -> analysis/replay_check.py replays the trade rules from the bars file with its own implementation and must arrive at the same trades
static int runTrace(int argc, char* argv[]) {
    if (argc < 5)
        throw std::runtime_error(
            "usage: BackTester trace <fileA> <fileB> <outPrefix> [--fill open|close] [--rebase X]");
    const TraceOptions options = parseTraceOptions(argc, argv);
    const Series seriesA = loadSeries(argv[2]), seriesB = loadSeries(argv[3]);
    PairData data;
    alignPair(seriesA, seriesB, data); //data points into the 2 series -> they must outlive it

    std::vector<TraceRow> rows;
    const std::vector<EngineTrade> trades = runTraceEngine(data, options, rows);

    const std::string prefix = argv[4];
    writeTraceBars(prefix, data, rows);
    writeTraceTrades(prefix, trades);
    std::cout << "trace: " << rows.size() << " bars, " << trades.size() << " trades -> " << prefix
              << "_bars.csv, _trades.csv\n";
    return 0;
}

//takes a flag out of the arguments wherever it sits -> true if it was there
static bool stripFlag(int& argc, char* argv[], const std::string& flag) {
    bool found = false;
    int kept = 0;
    for (int i = 0; i < argc; i++) {
        if (i > 0 && std::string(argv[i]) == flag) { //i > 0 -> argv[0] is the program itself, never a flag
            found = true;
            continue;
        }
        argv[kept++] = argv[i]; //kept arguments slide down over the gap
    }
    argc = kept;
    return found;
}

//strips --unseal-holdout and, if it was there, opens the hold-out with the full command line on record
static void unsealIfAsked(int& argc, char* argv[]) {
    //joined BEFORE the flag is stripepd -> the log gets the whole command line
    std::string commandLine;
    for (int i = 0; i < argc; i++) commandLine += (i ? " " : "") + std::string(argv[i]);
    const bool unseal = stripFlag(argc, argv, "--unseal-holdout");
    //the pair list is the first file argument of the modes that take one -> fingerprinted in the log
    const std::string pairList = (argc > 2 && std::string(argv[2]).find(".csv") != std::string::npos) ? argv[2] : "";
    if (unseal) unsealHoldout(commandLine, pairList);
}

//batch mode -> its flags, then the engine batch or the legacy one
static int runBatchCommand(int argc, char* argv[]) {
    if (argc < 4)
        throw std::runtime_error("usage: BackTester batch <pairList.csv> <results.csv> [--threads N] [--legacy]");
    unsigned threads = 0; //0 -> one per core
    bool legacy = false;
    for (int i = 4; i < argc; i++) { //argv[2] and [3] are the list and the results file -> flags start at 4
        const std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) threads = (unsigned)std::stoul(argv[++i]);
        else if (arg == "--legacy") legacy = true;
    }
    //the event engine across threads by default. --legacy -> the original one-pair-at-a-time pipeline
    return legacy ? runBatch(argv[2], argv[3]) : runBatchEngine(argv[2], argv[3], threads);
}

//dump mode -> its one flag, then the legacy fingerprint
static int runDumpCommand(int argc, char* argv[]) {
    if (argc < 4) throw std::runtime_error("usage: BackTester dump <pairList.csv> <out.csv> [--end YYYY-MM-DD]");
    std::string dataEnd; //"" unless --end gives a date
    for (int i = 4; i + 1 < argc; i++) //stops 1 short -> an --end with no date after it is ignored
        if (std::string(argv[i]) == "--end") dataEnd = argv[i + 1];
    return runDump(argv[2], argv[3], dataEnd);
}

//verify mode -> its flags, then the engine checked against the legacy pipeline
static int runVerifyCommand(int argc, char* argv[]) {
    if (argc < 3)
        throw std::runtime_error(
            "usage: BackTester verify <pairList.csv> [--all] [--threads N] [--wf-years FIRST LAST]");
    bool all = false; //--all -> also the walk-forward, static hedge and variant checks
    unsigned threads = 0;
    int firstYear = 2020, lastYear = 2026; //the legacy walk-forwards own test years
    for (int i = 3; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--all") all = true;
        else if (arg == "--threads" && i + 1 < argc) threads = (unsigned)std::stoul(argv[++i]);
        else if (arg == "--wf-years" && i + 2 < argc) { //takes 2 values -> FIRST then LAST
            firstYear = std::stoi(argv[i + 1]);
            lastYear = std::stoi(argv[i + 2]);
            i += 2; //step over both years
        }
    }
    return runVerify(argv[2], all, threads, firstYear, lastYear);
}

// bench mode -> its flags, then the timing runs
static int runBenchCommand(int argc, char* argv[]) {
    if (argc < 3)
        throw std::runtime_error("usage: BackTester bench <pairList.csv> [--threads N] [--runs R] [--legacy]");
    unsigned threads = 0;
    int runs = 9; //odd -> the median is one real timing, not the average of 2
    bool legacy = false; //true -> the legacy pipeline is timed too, for the speed-up
    for (int i = 3; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) threads = (unsigned)std::stoul(argv[++i]);
        else if (arg == "--runs" && i + 1 < argc) runs = std::stoi(argv[++i]);
        else if (arg == "--legacy") legacy = true;
    }
    return runBench(argv[2], threads, runs, legacy);
}

int main(int argc, char* argv[]) {
    try {
        //--unseal-holdout is the ONLY way to read bulk-data prices after 2022-12-31. taken out of the argument list here so no mode has to know about it (DECISIONS.md D22)
        unsealIfAsked(argc, argv);
        //--old-data anywhere -> the original report / export / facts on the hand-downloaded files (the regression baseline)
        const bool oldData = stripFlag(argc, argv, "--old-data");
        const std::string mode = (argc > 1) ? argv[1] : ""; // both flags are gone by now -> argv[1] is the  mode
        if (mode.empty()) return runAll(oldData); //no argument -> the normal report
        if (mode == "export") return runExport(oldData); // export + batch are what analysis/monte_carlo.py drives
        if (mode == "facts") return runFacts(oldData);
        if (mode == "batch") return runBatchCommand(argc, argv);
        if (mode == "dump") return runDumpCommand(argc, argv);
        if (mode == "verify") return runVerifyCommand(argc, argv);
        if (mode == "bench") return runBenchCommand(argc, argv);
        if (mode == "walkforward") return runWalkForwardCommand(argc, argv);
        if (mode == "trace") return runTrace(argc, argv);
        throw std::runtime_error(
            "unknown mode \"" + mode +
            "\" -> run with no argument, or: export, facts, batch, dump, verify, bench, walkforward, trace");
    } catch (const std::exception& error) {
        //missing/empty data file (or a singular matrix in inverse) -> say what went wrong and exit cleanly
        std::cout << std::flush; // flush first so everything printed before the error still shows up
        std::cerr << "\nerror: " << error.what() << "\n";
        return 1;
    }
}
