#pragma once
#include <cstddef>
#include <limits>
#include <string>
#include <vector>
#include "Backtest.h"
#include "Cointegration.h"
#include "Filters.h"
#include "Kalman.h"
#include "Market.h"
#include "Signals.h"
#include "Trading.h"

//event-driven trading core -> the ONLY input is one bar at a time (PairEngine onBar) and memory is bounded, so no decision can peek at a later day. usual per-bar work is O(1) -> running sums answer when they are certain, the exact legacy formula only runs on the few bars where it could matter

enum class EventType : unsigned char { Bar, Order, Fill }; //BAR (a close) -> ORDER -> FILL (same close or next open)
enum class OrderAction : unsigned char { Open, Close };
enum class ExitReason : unsigned char { None, Target, Stop, Timeout, End };
// SameClose -> fills at the close that made the signal (legacy -> "BackTester verify" matches it trade for trade). NextOpen -> at the open of bar t+1, so the trigger price is never the traded price (research default, D15)
enum class FillModel : unsigned char { SameClose, NextOpen };
const char* exitReasonName(ExitReason reason); //"target", "stop", "timeout", "end" -> as in PairTrade.exitReason

// what travels on the queue -> a small message saying WHAT happened, to WHICH strategy, on WHICH bar.the order details live in that strategys ticket
struct Event {
    EventType type = EventType::Bar;
    int run = 0; // which strategy instance (ORDER / FILL)
    size_t bar = 0;
};

//an order and, once eexcuted, its fill -> a strategy has at most one order outstanding, so each instance owns exactly one  ticket
struct Ticket {
    OrderAction action = OrderAction::Open;
    ExitReason reason = ExitReason::None; //why a closing order was made. None on an opening order
    int run = 0;
    int direction = 0; //+1 = long spread (long A short B), -1 = short spread
    size_t signalBar = 0; //bar whose close created the order
    size_t fillBar = 0; // bar it was filled on
    double sharesA = 0, sharesB = 0; //shares of each leg, for the fixed unit of capital (TradeParams.capital on leg A)
    double refPriceA = 0, refPriceB = 0; //the closes the order was decided on
    double fillPriceA = 0, fillPriceB = 0;
    double z = 0; //the z that triggeerd the order
    double beta = 0; //hedge ratio the legs are built on
    double sigma = 0; //daily volatility of the spread the legs will earn -> the portfolio sizes by it
};

//fixed size ring -> a bar triggers at most a couple of events per strategy so 64 slots is plenty (there is NO overflow check), and nothing is allocated while the loop runs
class EventQueue {
public:
    void push(const Event& event) {
        ring[tail & MASK] = event; //head and tail only ever count up -> the mask wraps them into the ring
        tail++;
    }
    bool pop(Event& event) {
        if (head == tail) return false; //empty
        const Event& front = ring[head & MASK];
        //field by field -> each read matches 1 write of the push just before. 1 wide read over those 3 narrow writes cant be forwarded, the cpu waits for them to land first
        event.type = front.type;
        event.run = front.run;
        event.bar = front.bar;
        head++;
        return true;
    }

private:
    static constexpr size_t CAPACITY = 64; //power of 2 -> & MASK does the job of % CAPACITY
    static constexpr size_t MASK = CAPACITY - 1;
    Event ring[CAPACITY];
    size_t head = 0, tail = 0; //next to read, next to write
};

//one bar of one pair -> the only thing the engine is ever gvien
struct BarData {
    int date = 0; // yyyymmdd as one number
    double openA = 0, openB = 0; //NaN = no usable open (a fill on this bar uses its close)
    double closeA = 0, closeB = 0;
    double lnA = 0, lnB = 0; //ln(close)
};

//the last `capacity` bars of log prices and dates -> the engines whole memory of the price history
class BarRing {
public:
    void init(size_t capacity); //rounded up to a power of 2
    void push(double lnA, double lnB, int date) {
        ringLnA[count & mask] = lnA; // once the ring is full this lands on the oldest bar
        ringLnB[count & mask] = lnB;
        ringDate[count & mask] = date;
        count++;
    }
    size_t size() const { return count; } //bars seen so far, NOT held.  bar t is readable while t + capacity >= size()
    size_t capacity() const { return mask + 1; }
    double lnA(size_t bar) const { return ringLnA[bar & mask]; } //bar = index since the pairs first bar
    double lnB(size_t bar) const { return ringLnB[bar & mask]; }
    int date(size_t bar) const { return ringDate[bar & mask]; }

private:
    std::vector<double> ringLnA, ringLnB;
    std::vector<int> ringDate;
    size_t mask = 0, count = 0; //mask = capacity - 1. count = bars pushed so far
};

struct ZScore {
    double z, mean, sd; //z = (spread - mean) / sd -> mean and sd travel with it so an entry can freeze its yardstick
};

//one hedge model + the recent history of its spread, for one pair -> the model is the ONLY thing that differs between the arms of the comparison
class SignalEngine {
public:
    //kalman -> alpha, beta re-estimated every bar (spread = the innovation). static -> fixed. rolling OLS -> fit on the trailing bars before today. all 3 -> spread = ln A - (alphaPrior + betaPrior ln B)
    enum class Kind : unsigned char { Kalman, Static, RollingOls };

    void startKalman(const KalmanParams& params, size_t zWindow, size_t historyWindow);
    void startRollingOls(size_t window, size_t zWindow, size_t historyWindow);
    void seed(double lnA0, double lnB0); // the pairs first bar -> the kalman starting state, the rolling OLS centre
    void startStatic(double alpha, double beta, const BarRing& bars, size_t zWindow, size_t historyWindow,
                     double trainingSd = 0.0); //trainingSd > 0 -> z = spread / trainingSd, mean 0 ("training-window z")

    void onBar(double lnA, double lnB); //-> alphaPrior, betaPrior,  spread for this bar
    void commit(const BarRing& bars); //this bars spread joins the history, AFTER the strategies have decided

    double alphaPrior = 0, betaPrior = 0, spread = 0; //what was known BEFORE this bars price + the spread against it
    bool live = false; //false -> never started, or stopped. the engine skips it

    bool hasZ() const { return fixedZ || count >= zLookback; }
    // true -> abs(z) is certainly below limit, from the running sums alone. false -> not sure, ask zExact(). NEVER true when the exact z is at or above it
    bool absZCertainlyBelow(double limit) const;
    bool zCertainlyFinite() const { return fastOk; } // true -> the window has real variance, so the exact z is finite
    ZScore zExact() const; //legacy z -> todays spread vs mean and sd of the zLookback spreads before it. needs hasZ()
    OUResult gateOU(size_t window, std::vector<double>& scratch) const; //OU fit of the window spreads before today

private:
    void resetHistory(size_t zWindow, size_t historyWindow);
    void refreshSums();
    double past(size_t k) const { return spreadHistory[k & historyMask]; } // spread number k (count - 1 = the newest)

    Kind kind = Kind::Kalman;
    KalmanParams kalmanParams;
    KalmanFilter filter{};
    double staticAlpha = 0, staticBeta = 1; //the fixed relationship of a static  signal
    bool fixedZ = false; //true  -> z = spread / fixedSd, no rolling window
    double fixedSd = 0;

    //rolling OLS -> sums over the last olsWindow bars before today, x = ln B and y = ln A measured from (centreX, centreY)
    size_t olsWindow = 750, olsCount = 0, olsSinceRebuild = 0;
    double centreX = 0, centreY = 0, sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
    void rebuildOls(const BarRing& bars);

    size_t zLookback = 60; //spreads in the z window
    double invZLookback = 1.0 / 60.0; //1 / zLookback -> a multiply on every bar instead of a divide
    std::vector<double> spreadHistory; //ring of the most recent spreads
    size_t historyMask = 0, count = 0; //count = spreads stored so far

    //running sums over the last zLookback spreads, measured from a nearby constant `centre` so the squares stay small
    double centre = 0, sum = 0, sumSq = 0;
    size_t sinceRefresh = 0; //window slides since the sums were last rebuilt from the stored spreads

    //filled by onBar from the sums -> is the window usable, and where todays spread sits in it
    bool fastOk = false;
    double fastVariance = 0, fastDeviation = 0; //variance of the window, todays spread minus the window mean
};

//rolling johansen gate -> the trace test on the last `window` bars, re-run every recomputeEvery bars, with hysteresis. too close to call from the running sums -> rednoe exactly, so pass/fail matches the legacy gate
class RollingJohansen {
public:
    void start(const GateParams& params);
    bool onBar(const BarRing& bars, size_t t); //bar t (t >= 1) was just pushed into bars. returns the gate state
    size_t tests = 0, exactTests = 0; // how many tests ran, and how many needed the exact fallback
    bool checkEveryTest = false; // verify only -> also run the exact test every time and record the worst gap
    double worstGap = 0; // largest abs(quick trace - exact trace) / critical  value seen
    size_t verdictMismatches = 0; //tests where the quick verdict differed from the exact one (must stay 0)

private:
    void zeroSums();
    void addRow(const BarRing& bars, size_t row, double sign); //sign +1 -> the row joins the window, -1 -> it leaves
    void rebuild(const BarRing& bars, size_t t);
    bool test(const BarRing& bars, size_t t);
    MomentMatrices momentsFromSums(double n, double meanDA, double meanDB) const;
    bool quickVerdict(const MomentMatrices& S, double product, double meanDA, double meanDB, double n,
                      bool& verdict) const;
    void compareWithExact(const BarRing& bars, size_t t, double n, double product, bool verdict);
    bool exactVerdict(const BarRing& bars, size_t t);
    JohansenResult exactTest(const BarRing& bars, size_t t);

    size_t window = 750, recomputeEvery = 5, warmUp = 750; // bars per test, bars between tests, spread index of test 1
    int closeAfter = 4, openAfter = 3; // fails in a row that shut the gate, passes in a row that reoepn it
    bool gateOpen = false;
    int failStreak = 0, passStreak = 0;
    double centreA = 0, centreB = 0;
    //the 14 running sums of the test, 1 row in and 1 out per bar (legacy rebuilds every row for every test). D = daily change, Y = level the day before
    double sumD[2] = {0, 0}, sumY[2] = {0, 0}, sumDD[3] = {0, 0, 0}, sumYY[3] = {0, 0, 0}, sumDY[4] = {0, 0, 0, 0};
    size_t sinceRebuild = 0; //bars since the sums were last rebuilt from the  ring
    std::vector<double> scratchA, scratchB;
    //(1 - ev1)(1 - ev2) below productPass -> trace above every possible critical value. above productFail -> below every one
    double productPass = 0, productFail = 0;
};

//everything a strategy instance needs apart from its signal
struct StrategyConfig {
    TradeParams trade;
    bool useGates = true; //false -> neither gate is asked (the warm-up still applies)
    double minHalfLife = 0.5, maxHalfLife = 30.0; //half-life gate band (trading days)
    size_t ouWindow = 250; //half-life gate window, also the window for the frozen half-life beihnd the timeout
    size_t gateWarmUp = 750; //spreads of history before any entry is allowed, ie bar - 1 >= this (the johansen window)
    int tradeFrom = 0, tradeUntil = 0; //entries only on dates in [from, until]. 0 = open ended
    double minBeta = -std::numeric_limits<double>::infinity(); //hedge-ratio guard -> entry needs beta in [min, max]
    double maxBeta = std::numeric_limits<double>::infinity(); //infinite defaults = off, like the legacy loop (D14)
    int noEntryFrom = 0, noEntryUntil = 0; //no entries on dates in [from, until] (0 = no blackout)
    bool useHalfLifeGate = true; //false -> only the johansen gate (a sensitivity run)
};

//one closed round trip, as the engine saw it, for the fixed unit of capital
struct EngineTrade {
    int run = 0; // strategy instance that made it
    size_t signalBar = 0; // bar whose  close triggered the entry
    size_t entryBar = 0, exitBar = 0; //bars the entry and the exit were FILLED on
    int entryDate = 0, exitDate = 0; //dates of thoes 2 fill bars
    int direction = 0; // +1 = long spread, -1 = short spread
    double zEntry = 0, zExit = 0; //z that triggered the entry, last  z the exit rules saw
    double sharesA = 0, sharesB = 0;
    double entryPriceA = 0, entryPriceB = 0, exitPriceA = 0, exitPriceB = 0; //fill prices
    double pnl = 0, cost = 0, netPnl = 0, entryNotional = 0; //netPnl = pnl - cost. notional = both legs at entry
    int holdingDays = 0; //bars from the entry fill to the exit fill
    ExitReason reason = ExitReason::None;
};
PairTrade toPairTrade(const EngineTrade& trade); //legacy trade record (dates as strings) for Metrics / export

//one bar of one signal exactly as the strategy saw it -> "BackTester trace" writes it out so analysis/replay_check.py can replay the trade rules independently
struct TraceRow {
    size_t bar = 0;
    int date = 0;
    double lnA = 0, lnB = 0; // the log prices the models saw (rebased if rebasing is on)
    double alpha = 0, beta = 0, spread = 0; //the prior relatoinship and the spread  against it
    bool hasZ = false; //false -> z window not full yet, the 3 below stay 0
    double z = 0, zMean = 0, zSd = 0;
    bool johansenOK = false;
    bool hasGate = false, gateMeanReverting = false; //hasGate -> enough spreads for the OU  window
    double gateHalfLife = 0;
    bool hasFrozenHalfLife = false;
    double frozenHalfLife = 0;
    double sigma = 0; //daily volatility of the frozen spread, last  60 daily changes (0 until they exist)
};

struct EngineConfig {
    GateParams gates;
    FillModel fill = FillModel::SameClose;
    //0 -> raw  log prices (legacy). > 0 -> both legs rebased to this value at the pairs first bar, so the kalman filter doesnt depend on the price unit (D13)
    double rebase = 0.0;
    //true -> data running out closes whatever is open at the last bar ("end" exit), like the legacy loop. false -> it stays open, markde at the last close (research -> nothing before a date depends on what comes after it)
    bool forceCloseAtEnd = true;
    //true -> a bar with no usable z (e.g. 60 identical spreads -> 0/0) is skipped completely, even the mark of an open position, as the legacy loop does. false -> an open position is still marked on it
    bool legacyNanSkip = true;
    bool keepDaily = true; // keep the engines own daily P&L series (per unit of capital, costs booked at exit)
    size_t historyBars = 1024; //bars of price history kept (must cover the johansen window and any training window)
};

class PairEngine {
public:
    explicit PairEngine(const EngineConfig& engineConfig);

    //kalman and rolling OLS must be added before the first bar (they have to see every bar). a static signal can be (re)started at any time, live from the next bar
    int addKalmanSignal(const KalmanParams& params, size_t zWindow);
    int addRollingOlsSignal(size_t olsWindow, size_t zWindow);
    int addStaticSignal(double alpha, double beta, size_t zWindow, double fixedSd = 0.0);
    void restartStaticSignal(int signal, double alpha, double beta, size_t zWindow, double fixedSd = 0.0);
    void stopSignal(int signal); //nobdy listens to it any more -> stop updating it

    int addRun(int signal, const StrategyConfig& strategyConfig); //a strategy istance trading that signal
    void setRunEnabled(int runIndex, bool enabled); // false -> no new entries (an open trade still runs to its exit)
    void setRunLegBeta(int runIndex, bool use, double beta); //legs + frozen spread use this beta, not the signals own

    // the 3 ways a driver can overrule a strategy from outside
    void clearCoolOff(int runIndex); // the spread the cool-off was about is gone (a static hedge was re-estimated)
    bool cancelPendingEntry(int runIndex); //an opening order not fillde yet is withdrawn. true if  tehre was one
    void flattenRun(int runIndex); //the book shut the position itself -> forget position, order, cool-off

    //THE input -> one bar. everything it triggers is handled before this returns, except opening orders, which wait as candidates for resolve() (NextOpen -> every fill waits for the next bar too)
    void onBar(const BarData& bar) {
        previousCloseA = today.closeA; //today still holds the bar before this one
        previousCloseB = today.closeB;
        //in the header so it inlines into the driver -> the bar goes from the price arrays sraight into today, not through a copy on the stack first
        today = bar;
        processBar();
    }
    size_t candidateCount() const { return candidates.size(); }
    const Ticket& candidate(size_t k) const { return runs[(size_t)candidates[k]].ticket; }
    void resolve(size_t k, bool approved); //the risk layers decision on candidate k
    void approveAll() {
        for (size_t k = 0; k < candidates.size(); k++) resolve(k, true);
    }
    void finish(); // no more data -> closes whatever is still open if forceCloseAtEnd

    //what happened during the current bar (cleared at the start of the next)
    const std::vector<Ticket>& fillsThisBar() const { return fills; }
    double markThisBar(int runIndex) const { return runs[(size_t)runIndex].markToday; } //unit P&L  bokoed today
    size_t betaRefusals(int runIndex) const { return runs[(size_t)runIndex].betaRefusals; } //running total

    //results, per unit of capital
    std::vector<EngineTrade> trades; // every runs closed trades, in the order they closed
    std::vector<double> dailyPnl; //one entry per bar after the first (index bar-1), all runs summed (keepDaily)
    size_t closeFallbackFills = 0; //next-open fills that used the bars close because an open was unusable

    //record every bar of one signal into `out` (see TraceRow). off by default -> it works out every exact statistic on every bar, the work the engine avoids
    void traceSignal(int signal, std::vector<TraceRow>* out) {
        tracedSignal = signal;
        traceOut = out;
    }

    //read-only views for drivers and checks
    const BarRing& history() const { return bars; }
    const RollingJohansen& gate() const { return johansen; }
    RollingJohansen& gateForVerify() { return johansen; } //writable -> verify switches checkEveryTest on

private:
    //AwaitingRisk -> opening order sent to the risk layer. EntryApproved, Exit -> waiting for the next open (next-open model only)
    enum class Pending : unsigned char { None, AwaitingRisk, EntryApproved, Exit };
    struct Run {
        int signal = 0; //index into signals
        StrategyConfig config;
        bool active = true; //false -> past its trading window with nothing open, never looked at again
        bool enabled = true; //false -> no new entries
        bool useLegBeta = false; //true -> legs and frozen spread are built on legBeta, not the signals beta
        double legBeta = 1.0;
        int state = 0; //0 = flat, +1 = long spread, -1 = short spread
        bool coolOff = false; // after a stop or timeout -> no re-entry until z is back inside the entry band
        Pending pending = Pending::None;
        Ticket ticket; // this strategys one outstanding order (and its fill)
        double sharesA = 0, sharesB = 0;
        size_t entryIndex = 0; //spread index (bar - 1) of the bar whose close triggered the entry
        size_t entryFillBar = 0;
        double entryPriceA = 0, entryPriceB = 0;
        bool markFromFill = false; //first mark after an open fill runs from the fill price, not yesterdays close
        double alpha0 = 0, beta0 = 0, mean0 = 0, sd0 = 0; //frozen at entry -> relationship + z yardstick
        double zEntry = 0, zHold = 0; //zHold = the z the exit rules judged on the last bar
        size_t timeoutDays = 0; //0 = no timeout for this trade
        double pendingAlpha = 0, pendingBeta = 0, pendingMean = 0, pendingSd = 0;
        size_t pendingTimeout = 0; //pending -> decided when the order was made, applied when its fill comes back
        double markToday = 0; //unit P&L bokoed on the  current bar
        size_t betaRefusals = 0; // entries refused only because the hedge ratio was outside the guard band
    };

    void processBar();
    void onBarEvent(size_t bar);
    void recordTraceRow(size_t bar, double lnA, double lnB);
    void dropFinishedRuns();
    void onOrderEvent(const Event& event);
    void onFillEvent(const Event& event);
    void applyOpenFill(Run& run, const Ticket& fill);
    void applyCloseFill(Run& run, int runIndex, const Ticket& fill);
    EngineTrade closedTrade(const Run& run, int runIndex, const Ticket& fill) const;
    void drain();
    void pushEvent(EventType type, int runIndex, size_t bar);
    void fillAtClose(Run& run, int runIndex, size_t bar);
    void startFirstBar(const BarData& bar);
    void fillPendingAtOpen(const BarData& bar, size_t t);
    void strategyOnBar(int runIndex, size_t bar);
    void manageOpenTrade(Run& run, int runIndex, size_t bar, const SignalEngine& signalEngine, bool noZ);
    void markToClose(Run& run, size_t bar);
    bool updateHoldZ(Run& run, const SignalEngine& signalEngine, bool noZ, size_t bar);
    static ExitReason exitDue(const Run& run, size_t bar);
    void queueCloseOrder(Run& run, int runIndex, size_t bar, ExitReason reason);
    void lookForEntry(Run& run, int runIndex, size_t bar, const SignalEngine& signalEngine, bool noZ);
    void queueOpenOrder(Run& run, int runIndex, size_t bar, const SignalEngine& signalEngine, const ZScore& zScore,
                        double legBeta);
    void freezeTimeout(Run& run, size_t bar);
    bool entryAllowed(const Run& run, size_t bar) const; //the cheap conditions -> window, warm-up, johansen gate
    bool halfLifeGateOpen(const Run& run, const SignalEngine& signalEngine);
    double frozenHalfLife(size_t bar, double alpha0, double beta0, size_t window);
    double frozenSpreadSigma(size_t bar, double beta0) const;
    void book(Run& run, size_t bar, double amount);

    EngineConfig config;
    BarRing bars;
    BarData today{};
    double previousCloseA = 0, previousCloseB = 0; //yesterdays closes -> where todays marks start from
    bool marksDirty = false; //some strategy booked P&L on the last bar -> its per-bar total needs clearing
    double rebaseOffsetA = 0, rebaseOffsetB = 0; //taken off the log prices before any model sees them (0 = raw)
    RollingJohansen johansen;
    bool johansenOK = false; //the gate state for the current bar
    std::vector<SignalEngine> signals;
    std::vector<Run> runs;
    std::vector<int> activeRuns; //the runs the bar loop still walks
    std::vector<int> candidates; //strategies whose opening  order is waiting for the risk layer
    std::vector<Ticket> fills; //this bars fills
    EventQueue queue;
    size_t historyWindow = 250; // spreads each signal keeps (the half-life gate window)
    bool dailyTouched = false; //has anything been written to todays P&L slot yet
    std::vector<double> scratch; //reused for the half-life windows
    int tracedSignal = -1; //-1 = tracing off
    std::vector<TraceRow>* traceOut = nullptr;
};

struct EngineRunResult {
    std::vector<EngineTrade> trades;
    std::vector<double> dailyPnl;
};
//one pair, one strategy -> the event engines version of runBacktest (same config struct). SameClose + approve everything = the legacy behaviour
EngineRunResult runPairEngine(const PairData& data, const BacktestConfig& config);
EngineConfig legacyEngineConfig(const BacktestConfig& config);
StrategyConfig legacyStrategyConfig(const BacktestConfig& config);
inline BarData barAt(const PairData& data, size_t t) {
    BarData bar;
    bar.date = data.date[t];
    bar.openA = data.openA[t]; //NaN where the csv had no usable open
    bar.openB = data.openB[t];
    bar.closeA = data.closeA[t];
    bar.closeB = data.closeB[t];
    bar.lnA = data.lnA[t];
    bar.lnB = data.lnB[t];
    return bar;
}
