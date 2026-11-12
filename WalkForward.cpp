#include "WalkForward.h"
#include "DataLoader.h"
#include "Kalman.h"
#include "Metrics.h"
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

//[start,end) of the spread indices dated inside [from, until] -> sorted YYYY-MM-DD strings so plain < works, empty = open ended
static void indexRange(const std::vector<std::string>& dates, const std::string& from,
                       const std::string& until, size_t& start, size_t& end) {
    start = 0;
    while (start < dates.size() && !from.empty() && dates[start] < from) start++;
    end = start;
    while (end < dates.size() && (until.empty() || dates[end] <= until)) end++;
}

// train metrics from ONLY what was known by the cutoff -> trades closing after it are  dropped (they used test year prices) & later daily P&L zeroed
static Metrics trainMetrics(const BacktestResult& result, const std::string& until) {
    std::vector<PairTrade> closedTrades;
    for (const PairTrade& trade : result.trades) {
        if (trade.exitDate <= until) closedTrades.push_back(trade);
    }
    std::vector<double> dailyPnl = result.dailyPnl;
    for (size_t i = 0; i < dailyPnl.size(); i++) {
        if (result.kalman.dates[i] > until) dailyPnl[i] = 0.0;
    }
    return computeMetrics(closedTrades, dailyPnl);
}

// max likelihood over the train days only. filter is causal so running the whole history & scoring train days leaks nothing. adaptive R -> only Q searched (only Q/R ratio drives the gain)
static void estimateKalman(const std::vector<AlignedBar>& bars, KalmanParams& params, const ParamGrid& grid,
                           const std::string& from, const std::string& until, size_t burnIn) {
    std::vector<double> rGrid = params.adaptiveR ? std::vector<double>{params.R} : grid.R;

    double bestLikelihood = -std::numeric_limits<double>::infinity();
    KalmanParams best = params; //if every trial fails, keep what we came in with
    for (double q : grid.Q) {
        for (double r : rGrid) {
            KalmanParams trial = params;
            trial.Q = q;
            trial.R = r;
            KalmanOutput kalman = runKalman(bars, trial);

            size_t start, end;
            indexRange(kalman.dates, from, until, start, end);
            if (start < burnIn) start = burnIn; //P still settling from its made up 0.01 ->  early days not representative

            double likelihood = kalmanLogLikelihood(kalman, start, end);
            if (likelihood > bestLikelihood) { bestLikelihood = likelihood; best = trial; } //strict > -> ties keep the earlier (smaller) value
        }
    }
    params = best;
}

WalkForwardResult runWalkForward(const std::string& fileA, const std::string& fileB,
                                 const BacktestConfig& base, const WalkForwardParams& params,
                                 const ParamGrid& grid) {
    WalkForwardResult result;
    result.nameA = shortName(fileA); result.nameB = shortName(fileB);
    result.refit = params.refit;
    result.adaptiveR = base.kalman.adaptiveR;

    //load once for the likelihood search -> runBacktest loads its own copy each call (7ms, not worth sharing)
    std::vector<AlignedBar> bars = loadAlignedBars(fileA, fileB, base); // same bars runBacktest will see
    result.oosDailyPnl.assign(bars.size() - 1, 0.0); //one entry per spread index -> same length as every runs dailyPnl

    for (int year = params.firstTestYear; year <= params.lastTestYear; year++) {
        FoldResult fold;
        fold.testYear = year;

        //rolling -> train is the trainYears before the test year. anchored (0) -> train from the start of the data
        const std::string trainFrom = (params.trainYears > 0) ? std::to_string(year - params.trainYears) + "-01-01" : "";
        const std::string trainUntil = std::to_string(year - 1) + "-12-31";
        const std::string testFrom = std::to_string(year) + "-01-01";
        const std::string testUntil = std::to_string(year) + "-12-31";

        BacktestConfig config = base; //every fold starts from the base settings -> nothing carries over between folds

        if (params.refit) {
            // filter settings by likelihood (statistics only, no P&L)
            estimateKalman(bars, config.kalman, grid, trainFrom, trainUntil, params.likelihoodBurnIn);

            // trading settings by train sharpe, with the filter settings above fixed
            double bestSharpe = -std::numeric_limits<double>::infinity();
            bool found = false;
            BacktestConfig best = config;
            Metrics bestMetrics{};
            for (double entryZ : grid.entryZ) {
                for (double exitZ : grid.exitZ) {
                    for (size_t zWindow : grid.zWindow) {
                        BacktestConfig trial = config;
                        trial.trade.entryZ = entryZ;
                        trial.trade.exitZ = exitZ;
                        trial.zWindow = zWindow;
                        trial.tradeFrom = trainFrom; //entries only inside the train years
                        trial.tradeUntil = trainUntil;

                        Metrics metrics = trainMetrics(runBacktest(fileA, fileB, trial), trainUntil);
                        if (metrics.trades < params.minTrainTrades) continue; //too few trades -> sharpe is just noise
                        if (metrics.sharpe > bestSharpe) { bestSharpe = metrics.sharpe; best = trial; bestMetrics = metrics; found = true; }
                    }
                }
            }
            //only trade if something actually worked in training -> a best sharpe of -0.3 is still a loser
            fold.traded = found && bestSharpe > 0.0;
            if (found) {
                config = best;
                fold.trainSharpe = bestSharpe;
                fold.trainTrades = bestMetrics.trades;
            }
        } else {
            // control -> base settings untouched every fold. still score them on train so the table compares like for like
            BacktestConfig trial = config;
            trial.tradeFrom = trainFrom;
            trial.tradeUntil = trainUntil;
            Metrics metrics = trainMetrics(runBacktest(fileA, fileB, trial), trainUntil);
            fold.trainSharpe = metrics.sharpe;
            fold.trainTrades = metrics.trades;
            fold.traded = true; //control always trades -> its the "do nothing clever" baseline
        }

        fold.Q = config.kalman.Q;
        fold.R = config.kalman.R;
        fold.entryZ = config.trade.entryZ;
        fold.exitZ = config.trade.exitZ;
        fold.zWindow = config.zWindow;

        //trade the test year with whatever was chosen -> the only result that counts
        if (fold.traded) {
            config.tradeFrom = testFrom; //entries only inside the test year
            config.tradeUntil = testUntil; //a trade entered in december may exit in january -> runs on under these settings
            BacktestResult testRun = runBacktest(fileA, fileB, config);

            fold.testTrades = testRun.trades;
            for (const PairTrade& trade : testRun.trades) {
                fold.testNet += trade.netPnl;
                result.oosTrades.push_back(trade);
            }
            //sum onto one timeline -> a january exit from last fold can overlap this fold first trade, as a real book would
            for (size_t i = 0; i < testRun.dailyPnl.size() && i < result.oosDailyPnl.size(); i++) {
                result.oosDailyPnl[i] += testRun.dailyPnl[i];
            }
        }
        result.folds.push_back(fold);
    }

    result.oosMetrics = computeMetrics(result.oosTrades, result.oosDailyPnl);
    return result;
}

void outputWalkForward(const WalkForwardResult& result) {
    std::cout << "\n============ walk forward: " << result.nameA << " / " << result.nameB << "   ("
              << (result.refit ? "settings re-fit each year on the years before" : "control: same settings every year")
              << (result.adaptiveR ? ", adaptive R" : ", fixed R") << ") ============\n";
    //column names spelt out -> "win" used to mean the z window, which read like win rate
    std::cout << std::left << std::setw(6) << "year"
              << std::right << std::setw(9) << "Q"
                            << std::setw(9) << "R"
                            << std::setw(7) << "entry"
                            << std::setw(6) << "exit"
                            << std::setw(8) << "window"
                            << std::setw(13) << "trainSharpe"
                            << std::setw(13) << "trainTrades"
                            << std::setw(12) << "testTrades"
                            << std::setw(10) << "testNet" << "\n"
              << std::string(93, '-') << "\n";

    for (const FoldResult& fold : result.folds) {
        std::cout << std::left << std::setw(6) << fold.testYear << std::right
                  << std::scientific << std::setprecision(0) //1e-06  style -> Q and R are orders of magnitdue
                  << std::setw(9) << fold.Q;
        if (result.adaptiveR) std::cout << std::setw(9) << "adapt"; //starting R is  irrelevatn once it adapts
        else std::cout << std::setw(9) << fold.R;

        std::cout << std::fixed << std::setprecision(1)
                  << std::setw(7) << fold.entryZ
                  << std::setw(6) << fold.exitZ
                  << std::setw(8) << fold.zWindow
                  << std::setprecision(2) << std::setw(13) << fold.trainSharpe
                  << std::setw(13) << fold.trainTrades;

        if (fold.traded) {
            std::cout << std::setw(12) << fold.testTrades.size()
                      << std::setprecision(0) << std::setw(10) << fold.testNet << "\n";
        } else {
            std::cout << "   stood aside\n"; //nothing earned a positive train sharpe with enough trades
        }
    }

    //sticky formatting -> put cout back before outputMetrics prints anything
    std::cout << std::defaultfloat << std::setprecision(6);
    if (result.oosTrades.empty()) {
        //nothing traded -> say so in one line instead of a whole metrics block of zeros
        std::cout << "\nno out of sample trades -> stood aside every year\n";
        return;
    }
    std::cout << "\nout of sample " << result.folds.front().testYear << "-" << result.folds.back().testYear
              << ", all years combined:";
    outputMetrics(result.oosMetrics);
}
