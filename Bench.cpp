#include "Bench.h"
#include "Backtest.h"
#include "Cointegration.h"
#include "Engine.h"
#include "Market.h"
#include "Metrics.h"
#include "Signals.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// macOS sysctl key -> nothing comes back elsewhere, then unknown cpu
std::string cpuName() {
    std::string name;
    std::FILE* pipe = popen("sysctl -n machdep.cpu.brand_string 2>/dev/null", "r");
    if (pipe) {
        char buffer[256];
        while (std::fgets(buffer, sizeof(buffer), pipe)) name += buffer;
        pclose(pipe);
    }
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back(); // fgets keeps the newline
    return name.empty() ? "unknown cpu" : name;
}

struct PairOutcome {
    size_t bars = 0, trades = 0; //bars counted even when the pair is too short to run
    double net = 0; // sum of trade netPnl -> what the determinism check compares
    Metrics metrics{};
};

// the whole pipeline for one pair -> line the 2 tickers up, feed  the engine bar by bar, then the metrics
PairOutcome runOne(const Market& market, const PairSpec& spec, const BacktestConfig& config) {
    PairData data;
    alignPair(market.series(spec.fileA), market.series(spec.fileB), data);
    PairOutcome outcome;
    outcome.bars = data.n;
    if (data.n < 2) return outcome; //fewer than 2 common days -> counted, not run (the legacy loader throws here)
    const EngineRunResult run = runPairEngine(data, config);
    std::vector<PairTrade> trades;
    trades.reserve(run.trades.size());
    for (const EngineTrade& trade : run.trades) {
        trades.push_back(toPairTrade(trade)); // metrics take the legacy trade record
        outcome.net += trade.netPnl;
    }
    outcome.trades = trades.size();
    outcome.metrics = computeMetrics(trades, run.dailyPnl);
    return outcome;
}

struct Timing {
    double median = 0, fastest = 0, slowest = 0;
};
//by value -> sorting a copy leaves the cller vector alone
Timing summarise(std::vector<double> seconds) {
    std::sort(seconds.begin(), seconds.end());
    Timing timing;
    timing.fastest = seconds.front();
    timing.slowest = seconds.back();
    const size_t count = seconds.size();
    //even count -> mean of the middle 2
    timing.median = (count % 2) ? seconds[count / 2] : 0.5 * (seconds[count / 2 - 1] + seconds[count / 2]);
    return timing;
}

//times reading every ticker file into a frehs Market, 1 thread then all of them
void timeLoads(const std::vector<PairSpec>& specs, unsigned threads, int runs, std::vector<double>& loadOne,
               std::vector<double>& loadMany) {
    for (int run = 0; run < std::min(runs, 3); run++) { //at most 3 rounds of loads whatever --runs says
        {
            Market market;
            const auto start = std::chrono::steady_clock::now();
            market.load(specs, 0, 1);
            loadOne.push_back(secondsSince(start));
        }
        {
            Market market;
            const auto start = std::chrono::steady_clock::now();
            market.load(specs, 0, threads);
            loadMany.push_back(secondsSince(start));
        }
    }
}

//what every engine run over the whole list has to add up to
struct EngineTotals {
    size_t bars = 0, trades = 0;
    double net = 0;
};

//times the engine over every pair, 1 thread  then all of them -> throws  if a run adds up differently
EngineTotals timeEngine(const Market& market, const std::vector<PairSpec>& specs, const BacktestConfig& config,
                        unsigned threads, int runs, std::vector<double>& oneThread, std::vector<double>& manyThreads) {
    std::vector<PairOutcome> outcomes(specs.size());
    size_t bars = 0, trades = 0;
    double net = 0;
    for (int run = 0; run < runs; run++) {
        for (unsigned threadCount : {1u, threads}) { //threads == 1 -> both passes land in oneThread
            const auto start = std::chrono::steady_clock::now();
            parallelFor(specs.size(), threadCount, [&](size_t k) { outcomes[k] = runOne(market, specs[k], config); });
            const double elapsed = secondsSince(start);
            (threadCount == 1 ? oneThread : manyThreads).push_back(elapsed);

            // summed in list order whatever the thread count -> the totals can be compared with exact ==
            size_t barSum = 0, tradeSum = 0;
            double netSum = 0;
            for (const PairOutcome& outcome : outcomes) {
                barSum += outcome.bars;
                tradeSum += outcome.trades;
                netSum += outcome.net;
            }
            if (run == 0 && threadCount == 1) { //first pass sets the totals, every later one must match exactly
                bars = barSum;
                trades = tradeSum;
                net = netSum;
            } else if (barSum != bars || tradeSum != trades || netSum != net) {
                throw std::runtime_error("bench: results changed between runs/thread counts -> not deterministic");
            }
        }
    }
    return EngineTotals{bars, trades, net};
}

} //namespace

int runBench(const std::string& listFile, unsigned threads, int runs, bool withLegacy) {
    if (threads == 0) threads = hardwareThreads(); //0 = every hardware thread
    if (runs < 1) runs = 1;
    const std::vector<PairSpec> specs = readPairList(listFile);
    const BacktestConfig config{}; //final design, full sample

    // stage 1 -> time the csv reads, then  read every ticker file once for real
    std::vector<double> loadOne, loadMany;
    Market market;
    timeLoads(specs, threads, runs, loadOne, loadMany);
    market.load(specs, 0, threads); //0 = no end date of our own (the hold-out seal still applies)

    // stage 2 -> the engine, 1 thread then all threads. the totals must come out the same every time
    std::vector<double> oneThread, manyThreads;
    const EngineTotals totals = timeEngine(market, specs, config, threads, runs, oneThread, manyThreads);
    const size_t bars = totals.bars, trades = totals.trades;
    const double net = totals.net;
    const Timing engineOne = summarise(oneThread), engineMany = summarise(manyThreads);
    const Timing loadOneTiming = summarise(loadOne), loadManyTiming = summarise(loadMany);

    std::cout << std::fixed;
    std::cout << "bench: " << specs.size() << " pairs, " << bars << " pair-bars, " << trades << " trades, net $"
              << std::setprecision(2) << net << " (same in every run)\n"
              << "cpu: " << cpuName() << ", " << hardwareThreads() << " hardware threads. build: "
#ifdef NDEBUG
              << "Release (-O3)"
#else
              << "not optimised"
#endif
              << ". runs: " << runs << " (median, fastest-slowest)\n";
    std::cout << std::setprecision(4) << "load csv   (" << market.tickers() << " tickers, " << market.rows()
              << " rows): 1 thread " << loadOneTiming.median << " s, " << threads << " threads "
              << loadManyTiming.median << " s\n";
    const auto printLine = [&](const char* name, const Timing& timing) {
        std::cout << std::left << std::setw(34) << name << std::right << std::setprecision(4) << timing.median << " s ("
                  << timing.fastest << "-" << timing.slowest << ")  -> " << std::setprecision(0)
                  << (double)bars / timing.median << " pair-bars/s\n"; //rate off the median run, not the fastest
    };
    printLine("engine, 1 thread:", engineOne);
    printLine(("engine, " + std::to_string(threads) + " threads:").c_str(), engineMany);
    // incl. reading = engine + load added stat by stat (so not the median of a sum)
    Timing endToEndOne = engineOne, endToEndMany = engineMany;
    endToEndOne.median += loadOneTiming.median;
    endToEndOne.fastest += loadOneTiming.fastest;
    endToEndOne.slowest += loadOneTiming.slowest;
    endToEndMany.median += loadManyTiming.median;
    endToEndMany.fastest += loadManyTiming.fastest;
    endToEndMany.slowest += loadManyTiming.slowest;
    printLine("incl. reading files, 1 thread:", endToEndOne);
    printLine(("incl. reading files, " + std::to_string(threads) + " threads:").c_str(), endToEndMany);

    if (withLegacy) {
        //legacy pipeline in this same build -> re-reads both files for every pair, whole-series passes
        auto start = std::chrono::steady_clock::now();
        size_t legacyTrades = 0;
        double legacyNet = 0;
        for (const PairSpec& spec : specs) {
            const BacktestResult result = runBacktest(spec.fileA, spec.fileB, config);
            legacyTrades += result.trades.size();
            for (const PairTrade& trade : result.trades) legacyNet += trade.netPnl;
        }
        const double elapsed = secondsSince(start);
        // rate over the engine pair-bar count -> same yardstick as the engine lines above
        std::cout << std::left << std::setw(34) << "legacy pipeline, 1 thread:" << std::right << std::setprecision(4)
                  << elapsed << " s  -> " << std::setprecision(0) << (double)bars / elapsed << " pair-bars/s   ("
                  << legacyTrades << " trades, net $" << std::setprecision(2) << legacyNet << ")\n";
    }
    std::cout << std::defaultfloat << std::setprecision(6); //sticky formatting -> put cout back
    return 0;
}

int runBatchEngine(const std::string& listFile, const std::string& resultsFile, unsigned threads) {
    const std::vector<PairSpec> specs = readPairList(listFile);
    Market market;
    market.load(specs, 0, threads);
    const BacktestConfig config{}; //final design, full sample

    struct Row {
        PairOutcome outcome;
        double trace0 = 0, halfLife = 0; //whole-sample johansen trace for H0 (no cointegration), OU half-life
        bool cointegrated = false;
        std::string error; //non-empty -> the pair threw, its row is skipped
    };
    std::vector<Row> rows(specs.size());
    parallelFor(specs.size(), threads, [&](size_t k) { // each pair writes only rows[k] -> no lock needed
        try {
            rows[k].outcome = runOne(market, specs[k], config);
            // the 2 whole-sample diagnostics the legacy batch wrote -> johansen on every bar, OU fit of every innovation
            PairData data;
            alignPair(market.series(specs[k].fileA), market.series(specs[k].fileB), data);
            const JohansenResult johansen = johansenExact(data.lnA, data.lnB, data.n);
            rows[k].trace0 = johansen.trace0;
            rows[k].cointegrated = johansen.isCointegrated;
            KalmanFilter filter = initialiseKalman(data.lnA[0], data.lnB[0], config.kalman);
            std::vector<double> spread;
            spread.reserve(data.n);
            for (size_t t = 1; t < data.n; t++) //bar 0 only  seeds the filter -> n - 1 innovations
                spread.push_back(kalmanStep(filter, data.lnA[t], data.lnB[t]).innovation);
            rows[k].halfLife = fitOU(spread, 0, spread.size()).halfLife;
        } catch (const std::exception& error) { //one bad pair must not stop the rest
            rows[k].error = error.what();
        }
    });

    std::ofstream results(resultsFile);
    if (!results) throw std::runtime_error("couldnt write results: " + resultsFile);
    //17 digits -> every double survives the round trip through text
    results << "label,trades,netPnl,sharpe,maxDrawdown,trace0,cointegrated,ouHalfLife\n" << std::setprecision(17);
    size_t failed = 0;
    for (size_t k = 0; k < specs.size(); k++) {
        const Row& row = rows[k];
        if (!row.error.empty()) {
            failed++;
            std::cerr << "batch: " << specs[k].label << " skipped -> " << row.error << "\n";
            continue;
        }
        results << specs[k].label << "," << row.outcome.metrics.trades << "," << row.outcome.metrics.netPnl << ","
                << row.outcome.metrics.sharpe << "," << row.outcome.metrics.maxDrawdown << "," << row.trace0 << ","
                << (row.cointegrated ? 1 : 0) << "," << row.halfLife << "\n";
    }
    std::cout << "batch: " << specs.size() - failed << " pairs -> " << resultsFile;
    if (failed) std::cout << " (" << failed << " skipped, see above)";
    std::cout << "\n";
    return 0;
}
