#include "Verify.h"
#include "Backtest.h"
#include "DataLoader.h"
#include "Engine.h"
#include "Filters.h"
#include "Market.h"
#include "Screen.h"
#include "Signals.h"
#include "Trading.h"
#include "WalkForward.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// "BackTester verify" -> proof that the event engine reproduces the legacy pipeline and cannot look ahead

static const double TOLERANCE = 1e-9; //absolute -> dollars for P&L, z units for z-scores

namespace {

struct Tally {
    size_t runs = 0, runsIdentical = 0, runsBitIdentical = 0; //identical = all within TOLERANCE
    size_t trades = 0, tradeCountMismatches = 0, fieldMismatches = 0, dailyMismatches = 0;
    double maxTradeDiff = 0, maxDailyDiff = 0; //largest abs differences seen
    void add(const Tally& other) {
        runs += other.runs;
        runsIdentical += other.runsIdentical;
        runsBitIdentical += other.runsBitIdentical;
        trades += other.trades;
        tradeCountMismatches += other.tradeCountMismatches;
        fieldMismatches += other.fieldMismatches;
        dailyMismatches += other.dailyMismatches;
        maxTradeDiff = std::max(maxTradeDiff, other.maxTradeDiff);
        maxDailyDiff = std::max(maxDailyDiff, other.maxDailyDiff);
    }
    bool clean() const { return tradeCountMismatches == 0 && fieldMismatches == 0 && dailyMismatches == 0; }
};

struct PairReport {
    std::string error; //non-empty -> the pair threw
    Tally single, variants, walkForward, staticRun;
    size_t gateTests = 0, gateExact = 0, gateMismatches = 0;
    double gateWorstGap = 0; // as a fraction of the critical value
    size_t cuts = 0, cutMismatchesClose = 0, cutMismatchesOpen = 0; //values that moved before a cut, per fill model
    size_t bars = 0;
    size_t pricesChecked = 0, priceMismatches = 0;
};

//memcmp not == -> NaN matches itself, -0 and +0 differ
bool sameBits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

// NaN on both sides counts as equal (a z-score of 0/0 is the same answer twice)
bool closeEnough(double a, double b, double& worst, bool& bitIdentical) {
    if (!sameBits(a, b)) bitIdentical = false;
    if (std::isnan(a) && std::isnan(b)) return true;
    const double diff = std::abs(a - b); //NaN  on one side only -> diff is NaN and fails the <= below
    if (diff > worst) worst = diff;
    return diff <= TOLERANCE;
}

//legacy trades vs engine trades, one run
void compareRun(const std::vector<PairTrade>& legacy, const std::vector<double>& legacyDaily,
                const std::vector<EngineTrade>& engine, const std::vector<double>& engineDaily, Tally& tally) {
    tally.runs++;
    tally.trades += legacy.size();
    bool ok = true, bitIdentical = true;
    if (legacy.size() != engine.size()) {
        tally.tradeCountMismatches++;
        ok = false;
    } else {
        for (size_t k = 0; k < legacy.size(); k++) {
            const PairTrade& legacyTrade = legacy[k];
            const PairTrade engineTrade = toPairTrade(engine[k]); //legacy record (dates as strings) so fields line up
            bool same =
                legacyTrade.entryDate == engineTrade.entryDate && legacyTrade.exitDate == engineTrade.exitDate &&
                legacyTrade.direction == engineTrade.direction && legacyTrade.holdingDays == engineTrade.holdingDays &&
                legacyTrade.exitReason == engineTrade.exitReason;
            //closeEnough on the left of && -> still runs after a mismatch, so the largest difference is always kept
            same = closeEnough(legacyTrade.zEntry, engineTrade.zEntry, tally.maxTradeDiff, bitIdentical) && same;
            same = closeEnough(legacyTrade.zExit, engineTrade.zExit, tally.maxTradeDiff, bitIdentical) && same;
            same = closeEnough(legacyTrade.pnl, engineTrade.pnl, tally.maxTradeDiff, bitIdentical) && same;
            same = closeEnough(legacyTrade.cost, engineTrade.cost, tally.maxTradeDiff, bitIdentical) && same;
            same = closeEnough(legacyTrade.netPnl, engineTrade.netPnl, tally.maxTradeDiff, bitIdentical) && same;
            same =
                closeEnough(legacyTrade.entryNotional, engineTrade.entryNotional, tally.maxTradeDiff, bitIdentical) &&
                same;
            if (!same) {
                tally.fieldMismatches++;
                ok = false;
            }
        }
    }
    if (legacyDaily.size() != engineDaily.size()) {
        tally.dailyMismatches++;
        ok = false;
    } else {
        for (size_t i = 0; i < legacyDaily.size(); i++) {
            if (!closeEnough(legacyDaily[i], engineDaily[i], tally.maxDailyDiff, bitIdentical)) {
                tally.dailyMismatches++;
                ok = false;
            }
        }
    }
    if (ok) tally.runsIdentical++;
    if (ok && bitIdentical) tally.runsBitIdentical++;
}

// the settings the README tables are built from, bseides the default
std::vector<BacktestConfig> variantConfigs() {
    std::vector<BacktestConfig> variants;
    const BacktestConfig base{};
    {
        BacktestConfig variant = base;
        variant.tradeUntil = "2021-12-31";
        variants.push_back(variant);
    } //train run
    {
        BacktestConfig variant = base;
        variant.tradeFrom = "2022-01-01";
        variants.push_back(variant);
    } //test run
    {
        BacktestConfig variant = base;
        variant.tradeFrom = "2020-01-01";
        variants.push_back(variant);
    } // monte carlo export
    {
        BacktestConfig variant = base;
        variant.kalman.adaptiveR = true;
        variants.push_back(variant);
    } //adaptive R
    {
        BacktestConfig variant = base;
        variant.trade.stopZ = 6.0;
        variants.push_back(variant);
    } //wide stop
    {
        BacktestConfig variant = base;
        variant.trade.frozenExit = false;
        variants.push_back(variant);
    } //old live-z exits
    {
        BacktestConfig variant = base;
        variant.trade.frozenExit = false;
        variant.trade.stopZ = 6.0;
        variants.push_back(variant);
    } // old live-z exits  + wide stop
    {
        BacktestConfig variant = base;
        variant.useGates = false;
        variant.tradeFrom = "2020-01-01";
        variants.push_back(variant);
    } //gates off
    {
        BacktestConfig variant = base;
        variant.trade.costBps = 0.0002; //2bps instead of 5
        variant.zWindow = 40;
        variants.push_back(variant);
    } //other cost + window
    {
        BacktestConfig variant = base;
        variant.trade.timeoutHalfLives = 0.0;
        variant.trade.exitZ = 0.5;
        variants.push_back(variant);
    } // no timeout
    return variants;
}

struct EngineRun {
    std::vector<EngineTrade> trades;
    std::vector<double> daily;
};

//the default settings fed only the first `bars` bars, w/ the fill model asked for
EngineRun runEngineUpTo(const PairData& data, size_t bars, FillModel fill) {
    const BacktestConfig config{};
    EngineConfig engineConfig = legacyEngineConfig(config);
    engineConfig.fill = fill;
    engineConfig.forceCloseAtEnd = false; //no forced exit at the last bar fed -> an open trade stays open, marked
    PairEngine engine(engineConfig);
    const int signal = engine.addKalmanSignal(config.kalman, config.zWindow);
    engine.addRun(signal, legacyStrategyConfig(config));
    for (size_t t = 0; t < bars; t++) {
        engine.onBar(barAt(data, t));
        if (engine.candidateCount()) engine.approveAll(); //no risk layer -> every opening order approved, as legacy
    }
    engine.finish();
    EngineRun result;
    result.trades = std::move(engine.trades);
    result.daily = std::move(engine.dailyPnl);
    return result;
}

//first `cut` bars only vs the full run -> trades closed before the  cut and all `cut - 1` P&L days match bit for bit
size_t truncationMismatches(const PairData& data, const EngineRun& full, FillModel fill, size_t cut) {
    const EngineRun partial = runEngineUpTo(data, cut, fill);
    size_t mismatches = 0;
    size_t closedInFull = 0;
    for (const EngineTrade& trade : full.trades) {
        if (trade.exitBar < cut) closedInFull++; //exit filled on a bar the short run saw (0 to cut - 1)
    }
    if (partial.trades.size() != closedInFull) mismatches++;
    //trades listed in the order they closed -> the short run ones are the first ones of the full run
    for (size_t k = 0; k < partial.trades.size() && k < full.trades.size(); k++) {
        const EngineTrade& partTrade = partial.trades[k];
        const EngineTrade& fullTrade = full.trades[k];
        if (partTrade.entryBar != fullTrade.entryBar || partTrade.exitBar != fullTrade.exitBar ||
            partTrade.direction != fullTrade.direction || partTrade.reason != fullTrade.reason ||
            !sameBits(partTrade.pnl, fullTrade.pnl) || !sameBits(partTrade.cost, fullTrade.cost) ||
            !sameBits(partTrade.zEntry, fullTrade.zEntry) || !sameBits(partTrade.zExit, fullTrade.zExit))
            mismatches++;
    }
    if (partial.daily.size() != cut - 1) mismatches++; //one P&L per bar after the first (index bar - 1)
    for (size_t i = 0; i < partial.daily.size() && i < full.daily.size(); i++) {
        if (!sameBits(partial.daily[i], full.daily[i])) mismatches++;
    }
    return mismatches;
}

//small deterministic generator for the cut points  -> the same 20 cuts every time for a given pair
unsigned long long nextRandom(unsigned long long& state) {
    state ^= state << 13; //xorshift64 (marsaglia, shifts 13 7 17)
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

//legacy walk-forward control on the engine -> one filter, one strategy  per test year, each entering only in its year
EngineRun engineWalkForward(const PairData& data, const BacktestConfig& base, const WalkForwardParams& walkForward) {
    PairEngine engine(legacyEngineConfig(base));
    const int signal = engine.addKalmanSignal(base.kalman, base.zWindow);
    for (int year = walkForward.firstTestYear; year <= walkForward.lastTestYear; year++) {
        BacktestConfig fold = base;
        fold.tradeFrom = std::to_string(year) + "-01-01";
        fold.tradeUntil = std::to_string(year) + "-12-31";
        engine.addRun(signal, legacyStrategyConfig(fold));
    }
    for (size_t t = 0; t < data.n; t++) {
        engine.onBar(barAt(data, t));
        if (engine.candidateCount()) engine.approveAll();
    }
    engine.finish();
    EngineRun result;
    result.trades = std::move(engine.trades);
    //legacy lists trades fold by fold, the engine as they close (same order inside a fold) -> stable sort by fold
    std::stable_sort(result.trades.begin(), result.trades.end(),
                     [](const EngineTrade& a, const EngineTrade& b) { return a.run < b.run; }); //run = fold index
    result.daily = std::move(engine.dailyPnl);
    return result;
}

struct LegacyStatic {
    std::vector<PairTrade> trades;
    std::vector<double> daily;
};

//static-hedge reference built ONLY from legacy functions -> fixed alpha/beta put where the filter output goes
LegacyStatic legacyStaticRun(const std::vector<AlignedBar>& bars, double alpha, double beta,
                             const BacktestConfig& config) {
    KalmanOutput kalman;
    for (size_t t = 1; t < bars.size(); t++) { //from bar 1 like the filter -> index i is bars[i+1]
        kalman.dates.push_back(bars[t].date);
        kalman.alphaPrior.push_back(alpha);
        kalman.betaPrior.push_back(beta);
        kalman.beta.push_back(beta); //prior and posterior the same -> nothing ever updates
        kalman.spread.push_back(bars[t].lnCloseA - (alpha + beta * bars[t].lnCloseB));
        kalman.S.push_back(0.0); //S and R not read by the z-score, gates or trading loop
        kalman.R.push_back(0.0);
    }
    std::vector<double> zMean, zSd;
    std::vector<double> z = rollingZScore(kalman.spread, config.zWindow, &zMean, &zSd);
    std::vector<double> halfLife = rollingFrozenHalfLife(bars, kalman, config.gates.ouWindow);
    GateParams gateParams = config.gates;
    gateParams.minHalfLife = config.minHalfLife; // half-life band from the config, as runBacktest does
    gateParams.maxHalfLife = config.maxHalfLife;
    std::vector<GateDay> gates = computeGates(bars, kalman, gateParams);
    for (size_t i = 0; i < gates.size(); i++) { //same as applyTradeWindow
        if (!config.tradeFrom.empty() && kalman.dates[i] < config.tradeFrom) gates[i].canEnter = false;
        if (!config.tradeUntil.empty() && kalman.dates[i] > config.tradeUntil) gates[i].canEnter = false;
    }
    LegacyStatic result;
    result.trades = runTradingLoop(bars, kalman, z, zMean, zSd, halfLife, gates, config.trade, &result.daily);
    return result;
}

void checkStatic(const PairSpec& spec, const PairData& data, int trainFrom, int trainUntil, int tradeFrom,
                 int tradeUntil, Tally& tally) {
    //training window -> OLS coefficients
    size_t trainStart = 0, trainEnd = 0;
    while (trainStart < data.n && data.date[trainStart] < trainFrom) trainStart++;
    trainEnd = trainStart;
    while (trainEnd < data.n && data.date[trainEnd] <= trainUntil) trainEnd++; //one past the last training bar
    if (trainEnd - trainStart < 300) return; //not enough history for this window on this pair -> nothing to check
    const OlsFit ols = olsFit(data.lnA + trainStart, data.lnB + trainStart, trainEnd - trainStart); //log prices

    BacktestConfig config;
    config.tradeFrom = dateToString(tradeFrom); //legacy windows compare YYYY-MM-DD strings
    config.tradeUntil = dateToString(tradeUntil);

    //reference -> legacy functions over the whole series
    const std::vector<Bar> barsA = loadCSV(spec.fileA), barsB = loadCSV(spec.fileB);
    const std::vector<AlignedBar> bars = alignBars(barsA, barsB);
    const LegacyStatic reference = legacyStaticRun(bars, ols.alpha, ols.beta, config);

    //engine -> the static signal only starts with the trading window, its history rebuilt from what the ring remembers
    PairEngine engine(legacyEngineConfig(config));
    bool started = false;
    for (size_t t = 0; t < data.n; t++) {
        if (!started && data.date[t] >= tradeFrom) {
            const int signal = engine.addStaticSignal(ols.alpha, ols.beta, config.zWindow);
            engine.addRun(signal, legacyStrategyConfig(config));
            started = true;
        }
        engine.onBar(barAt(data, t));
        if (engine.candidateCount()) engine.approveAll();
    }
    engine.finish();
    compareRun(reference.trades, reference.daily, engine.trades, engine.dailyPnl, tally);
}

//legacy loader against the new one, bit for  bit
void checkPrices(const PairSpec& spec, const PairData& data, PairReport& report) {
    const std::vector<Bar> barsA = loadCSV(spec.fileA), barsB = loadCSV(spec.fileB);
    const std::vector<AlignedBar> legacy = alignBars(barsA, barsB);
    report.pricesChecked = legacy.size();
    if (legacy.size() != data.n) report.priceMismatches++; //a different no of common days counts once
    for (size_t t = 0; t < legacy.size() && t < data.n; t++) {
        //legacy  keeps dates as YYYY-MM-DD strings
        if (legacy[t].date != dateToString(data.date[t]) || !sameBits(legacy[t].closeA, data.closeA[t]) ||
            !sameBits(legacy[t].closeB, data.closeB[t]) || !sameBits(legacy[t].lnCloseA, data.lnA[t]) ||
            !sameBits(legacy[t].lnCloseB, data.lnB[t]))
            report.priceMismatches++;
    }
}

//legacy pipeline against the engine under one set of settings
void checkRun(const PairSpec& spec, const PairData& data, const BacktestConfig& config, Tally& tally) {
    const BacktestResult legacy = runBacktest(spec.fileA, spec.fileB, config); // re-reads both csv files itslef
    const EngineRunResult engine = runPairEngine(data, config);
    compareRun(legacy.trades, legacy.dailyPnl, engine.trades, engine.dailyPnl, tally);
}

//one engine run that re-does every quick johansen test exactly -> the gate counters
void checkGate(const PairData& data, PairReport& report) {
    const BacktestConfig config{};
    PairEngine engine(legacyEngineConfig(config));
    engine.gateForVerify().checkEveryTest = true;
    const int signal = engine.addKalmanSignal(config.kalman, config.zWindow);
    engine.addRun(signal, legacyStrategyConfig(config));
    for (size_t t = 0; t < data.n; t++) {
        engine.onBar(barAt(data, t));
        if (engine.candidateCount()) engine.approveAll();
    }
    report.gateTests = engine.gate().tests;
    report.gateExact = engine.gate().exactTests;
    report.gateWorstGap = engine.gate().worstGap;
    report.gateMismatches = engine.gate().verdictMismatches;
}

// the data cut short at 20 places, both fill models -> counts whateevr moved before a cut
void checkTruncation(const PairSpec& spec, const PairData& data, PairReport& report) {
    if (data.n <= 900) return; //too short for 20 cuts after bar 800
    const EngineRun fullClose = runEngineUpTo(data, data.n, FillModel::SameClose);
    const EngineRun fullOpen = runEngineUpTo(data, data.n, FillModel::NextOpen);
    //label hashed into the seed -> each pair gets its own cuts, the same ones every run
    unsigned long long state = 1469598103934665603ULL;
    for (char character : spec.label) state = (state ^ (unsigned char)character) * 1099511628211ULL;
    if (state == 0) state = 88172645463325252ULL; //xorshift never leaves 0 -> any nonzero seed
    for (int k = 0; k < 20; k++) {
        const size_t cut = 800 + (size_t)(nextRandom(state) % (data.n - 800)); //cut in [800, n - 1]
        report.cuts++;
        report.cutMismatchesClose += truncationMismatches(data, fullClose, FillModel::SameClose, cut);
        report.cutMismatchesOpen += truncationMismatches(data, fullOpen, FillModel::NextOpen, cut);
    }
}

//legacy walk-forward control against the engine running one strategy per test year
void checkWalkForward(const PairSpec& spec, const PairData& data, int firstTestYear, int lastTestYear, Tally& tally) {
    const BacktestConfig base{};
    WalkForwardParams walkForward;
    walkForward.refit = false; //false = keep the base settings every fold (the control)
    walkForward.firstTestYear = firstTestYear;
    walkForward.lastTestYear = lastTestYear;
    const WalkForwardResult legacy = runWalkForward(spec.fileA, spec.fileB, base, walkForward);
    const EngineRun engine = engineWalkForward(data, base, walkForward);
    compareRun(legacy.oosTrades, legacy.oosDailyPnl, engine.trades, engine.daily, tally);
}

PairReport verifyPair(const PairSpec& spec, const PairData& data, bool all, int firstTestYear, int lastTestYear) {
    PairReport report;
    report.bars = data.n;

    //0. the 2 loaders must hand over the same prices -> every date, close and log close, bit for bit
    checkPrices(spec, data, report);

    //1. single run, the final design -> every trade and every daily P&L against legacy runBacktest
    checkRun(spec, data, BacktestConfig{}, report.single);

    //2. the quick gate test against the exact one, on every test of this pair
    checkGate(data, report);

    //3. truncation -> 20 cuts, both fill models. nothing before a cut may move (no look-ahead)
    checkTruncation(spec, data, report);

    if (!all) return report; //checks 4 to 6 only w/ --all

    //4. walk-forward control
    checkWalkForward(spec, data, firstTestYear, lastTestYear, report.walkForward);

    //5. static hedge,  2 windows -> 3 training years then the next year (dates as YYYYMMDD ints)
    for (int year : {firstTestYear, lastTestYear - 1}) {
        checkStatic(spec, data, (year - 3) * 10000 + 101, (year - 1) * 10000 + 1231, year * 10000 + 101,
                    year * 10000 + 1231, report.staticRun);
    }

    // 6. check 1 again under the other settings
    for (const BacktestConfig& config : variantConfigs()) checkRun(spec, data, config, report.variants);
    return report;
}

void printTally(const char* name, const Tally& tally) {
    if (tally.runs == 0) return; //check never ran (e.g. no --all) -> no line
    std::cout << "  " << std::left << std::setw(20) << name << std::right << tally.runsIdentical << "/" << tally.runs
              << " runs match (" << tally.runsBitIdentical << " bit for bit), " << tally.trades
              << " trades, largest difference: trade field " << std::scientific << std::setprecision(1)
              << tally.maxTradeDiff << ", daily P&L " << tally.maxDailyDiff << std::defaultfloat
              << std::setprecision(6); //sticky formatting -> put cout back
    if (!tally.clean()) {
        std::cout << "   <-- MISMATCH: " << tally.tradeCountMismatches << " trade counts, " << tally.fieldMismatches
                  << " trade fields, " << tally.dailyMismatches << " daily values";
    }
    std::cout << "\n";
}

//the numbers of every pair added up for the report
struct VerifyTotals {
    Tally single, variants, walkForward, staticRun;
    size_t gateTests = 0, gateExact = 0, gateMismatches = 0, cuts = 0, cutMismatchesClose = 0, cutMismatchesOpen = 0,
           bars = 0, failed = 0;
    size_t pricesChecked = 0, priceMismatches = 0;
    double gateWorstGap = 0;
};

//a pair that threw is named first, in list order -> its line comes before the report
void printThrown(const std::vector<PairSpec>& specs, const std::vector<PairReport>& reports) {
    for (size_t k = 0; k < reports.size(); k++) {
        if (!reports[k].error.empty()) std::cout << "  " << specs[k].label << " threw: " << reports[k].error << "\n";
    }
}

// adds the pairs up in list order -> a pair that threw is counted as failed, not summed
VerifyTotals sumReports(const std::vector<PairReport>& reports) {
    VerifyTotals total;
    for (size_t k = 0; k < reports.size(); k++) {
        const PairReport& report = reports[k];
        if (!report.error.empty()) {
            total.failed++;
            continue;
        }
        total.single.add(report.single);
        total.variants.add(report.variants);
        total.walkForward.add(report.walkForward);
        total.staticRun.add(report.staticRun);
        total.gateTests += report.gateTests;
        total.gateExact += report.gateExact;
        total.gateMismatches += report.gateMismatches;
        total.gateWorstGap = std::max(total.gateWorstGap, report.gateWorstGap); //worst over all pairs, not a sum
        total.cuts += report.cuts;
        total.cutMismatchesClose += report.cutMismatchesClose;
        total.cutMismatchesOpen += report.cutMismatchesOpen;
        total.bars += report.bars;
        total.pricesChecked += report.pricesChecked;
        total.priceMismatches += report.priceMismatches;
    }
    return total;
}

//the verify report -> what was checked, then one line or block per check
void printVerify(const std::string& listFile, size_t pairs, const VerifyTotals& total) {
    std::cout << "verify: " << pairs << " pairs, " << total.bars << " pair-bars (" << listFile << ")\n"
              << "prices: " << total.pricesChecked << " aligned bars from the new loader against the legacy loader, "
              << total.priceMismatches << " differ (date, close and log close compared bit for bit)\n"
              << "legacy pipeline vs event engine, same-close fills (tolerance 1e-9):\n";
    printTally("single run", total.single);
    printTally("other settings", total.variants);
    printTally("walk-forward", total.walkForward);
    printTally("static hedge", total.staticRun);
    std::cout << "johansen gate: " << total.gateTests << " tests, " << total.gateExact
              << " fell back to the exact test, " << total.gateMismatches
              << " verdicts differed from the exact test, largest quick-vs-exact trace gap " << std::scientific
              << std::setprecision(1) << total.gateWorstGap << std::defaultfloat << std::setprecision(6)
              << " of the critical value (the fallback margin is 1e-05)\n"
              << "truncation: " << total.cuts
              << " cuts per fill model, values changed before the cut: " << total.cutMismatchesClose
              << " (same-close), " << total.cutMismatchesOpen << " (next-open)\n";
}

} //namespace

int runVerify(const std::string& listFile, bool all, unsigned threads, int firstTestYear, int lastTestYear) {
    const std::vector<PairSpec> specs = readPairList(listFile);
    Market market;
    market.load(specs, 0, threads); //0 = no end  date of our own (the hold-out seal still applies)

    std::vector<PairReport> reports(specs.size());
    parallelFor(specs.size(), threads, [&](size_t k) { //each pair writes only reports[k] -> no lock needed
        try {
            PairData data;
            alignPair(market.series(specs[k].fileA), market.series(specs[k].fileB), data);
            reports[k] = verifyPair(specs[k], data, all, firstTestYear, lastTestYear);
        } catch (const std::exception& error) { //one bad pair must not stop the rest
            reports[k].error = error.what();
        }
    });

    printThrown(specs, reports);
    const VerifyTotals total = sumReports(reports);
    printVerify(listFile, specs.size(), total);

    const bool pass = total.failed == 0 && total.single.clean() && total.variants.clean() &&
                      total.walkForward.clean() && total.staticRun.clean() && total.gateMismatches == 0 &&
                      total.cutMismatchesClose == 0 && total.cutMismatchesOpen == 0 && total.priceMismatches == 0;
    std::cout << (pass ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return pass ? 0 : 1;
}
