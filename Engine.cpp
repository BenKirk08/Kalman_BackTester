//the event engine -> the signal engines, the rolling johansen gate and PairEngine, which runs each bar through the event queue
#include "Engine.h"
#include "Matrix.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

const char* exitReasonName(ExitReason reason) {
    switch (reason) {
        case ExitReason::Target: return "target";
        case ExitReason::Stop: return "stop";
        case ExitReason::Timeout: return "timeout";
        case ExitReason::End: return "end";
        default: return ""; //only None lands here
    }
}

PairTrade toPairTrade(const EngineTrade& trade) {
    PairTrade pairTrade{}; //the legacy record -> dates as strings, no shares or prices
    pairTrade.entryDate = dateToString(trade.entryDate);
    pairTrade.exitDate = dateToString(trade.exitDate);
    pairTrade.direction = trade.direction;
    pairTrade.zEntry = trade.zEntry;
    pairTrade.zExit = trade.zExit;
    pairTrade.pnl = trade.pnl;
    pairTrade.entryNotional = trade.entryNotional;
    pairTrade.holdingDays = trade.holdingDays;
    pairTrade.exitReason = exitReasonName(trade.reason); //same words the legacy loop writes
    pairTrade.cost = trade.cost;
    pairTrade.netPnl = trade.netPnl;
    return pairTrade;
}

//ring sizes are powers of 2 -> an index wraps with & mask, no division
static size_t powerOfTwoAtLeast(size_t minimum) {
    size_t power = 1;
    while (power < minimum) power <<= 1; //doubles until it covers minimum (0 and 1 both give 1)
    return power;
}

void BarRing::init(size_t capacity) {
    const size_t slots = powerOfTwoAtLeast(capacity);
    ringLnA.assign(slots, 0.0);
    ringLnB.assign(slots, 0.0);
    ringDate.assign(slots, 0);
    mask = slots - 1; // slots is a power of 2 -> index & mask is index % slots
    count = 0;
}

//------------------------------------------------------------------ signal -> hedge model + spread history

//room the quick test leaves before the exact z takes over -> the running sums are good to about 1 part in 1e9, this margin is 100,000 times wider
static constexpr double Z_MARGIN = 1e-4;
//the quick z is only trusted while the variance of the window is above this share of its mean of squares
static constexpr double Z_MIN_VAR_SHARE = 1e-6;
//bars between full rebuilds of the running sums -> rounding drift never gets the time to build up
static constexpr size_t Z_REFRESH_EVERY = 256;
static constexpr size_t OLS_REBUILD_EVERY = 512;
// the z sums are also rebuilt at once when the suqare that left the window is this many times what remains
static constexpr double Z_LEAVER_LIMIT = 1e4;

// empty z window and spread ring, running sums at zero, switched on -> where every start begins
void SignalEngine::resetHistory(size_t zWindow, size_t historyWindow) {
    zLookback = zWindow;
    invZLookback = 1.0 / (double)zLookback;
    fixedZ = false;
    //ring covers the longer of the 2 windows + 1 -> commit still reads the spread that just fell out of the z window
    spreadHistory.assign(powerOfTwoAtLeast(std::max(zWindow, historyWindow) + 1), 0.0);
    historyMask = spreadHistory.size() - 1;
    count = 0;
    centre = 0;
    sum = 0;
    sumSq = 0;
    sinceRefresh = 0;
    fastOk = false;
    live = true; //switched on -> the engine updates it from the next bar
}

void SignalEngine::startKalman(const KalmanParams& params, size_t zWindow, size_t historyWindow) {
    kind = Kind::Kalman;
    kalmanParams = params; //kept for seed -> the filter itself is bulit from the first bar
    resetHistory(zWindow, historyWindow);
}

void SignalEngine::startRollingOls(size_t window, size_t zWindow, size_t historyWindow) {
    kind = Kind::RollingOls;
    olsWindow = window;
    olsCount = 0;
    olsSinceRebuild = 0;
    centreX = centreY = sumX = sumY = sumXX = sumXY = 0; //seed puts the centre on the first bar
    resetHistory(zWindow, historyWindow);
}

void SignalEngine::seed(double lnA0, double lnB0) {
    if (kind == Kind::Kalman) {
        filter = initialiseKalman(lnA0, lnB0, kalmanParams);
    } else if (kind == Kind::RollingOls) {
        centreX = lnB0;
        centreY = lnA0; //measure from the first bar -> it enters the sums as (0, 0)
        olsCount = 1; //the first bar is already one point of the regression
    }
}

void SignalEngine::startStatic(double alpha, double beta, const BarRing& bars, size_t zWindow, size_t historyWindow,
                               double trainingSd) {
    kind = Kind::Static;
    staticAlpha = alpha;
    staticBeta = beta;
    resetHistory(zWindow, historyWindow);
    fixedZ = trainingSd > 0.0; //0 (the default) ->  ordinary rolling z
    fixedSd = trainingSd;
    const size_t keep = std::max(zWindow, historyWindow);

    //the spreads of the bars already seen, with THESE coefficients (nothing ahead is used) -> a restart dosent wait for the z window to refill. bar 0 never has a spread, same numbering as the legacy pipeline
    const size_t barCount = bars.size();
    //barCount - 1 spreads exist (none for bar 0),  capped at capacity - 1 so every bar read is still in the ring
    const size_t available = std::min(barCount > 0 ? barCount - 1 : 0, bars.capacity() - 1);
    const size_t take = std::min(keep, available);
    for (size_t bar = barCount - take; bar < barCount; bar++) { //the newest take bars, oldest first
        spreadHistory[count & historyMask] = bars.lnA(bar) - (staticAlpha + staticBeta * bars.lnB(bar));
        count++;
    }
    refreshSums(); // the running sums start from the rebuilt history
}

void SignalEngine::refreshSums() {
    //rebuild the running sums from the stored spreads -> wipes out any roudning drift, and re-centres them
    sinceRefresh = 0;
    const size_t from = (count > zLookback) ? count - zLookback : 0; //the last zLookback spreads, or all there are
    centre = count ? past(count - 1) : 0.0; //the newest spread ->  the window sits close to it, so squares stay small
    sum = 0;
    sumSq = 0;
    for (size_t k = from; k < count; k++) {
        const double x = past(k) - centre;
        sum += x;
        sumSq += x * x;
    }
}

void SignalEngine::rebuildOls(const BarRing& bars) {
    //same idea for the rolling regression -> recompute its sums from the bars, measured from the latest bar
    olsSinceRebuild = 0;
    const size_t barCount = bars.size();
    centreX = bars.lnB(barCount - 1);
    centreY = bars.lnA(barCount - 1);
    sumX = sumY = sumXX = sumXY = 0;
    for (size_t bar = barCount - olsCount; bar < barCount; bar++) { // the olsCount newest bars, todays included
        const double x = bars.lnB(bar) - centreX, y = bars.lnA(bar) - centreY;
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
    }
}

void SignalEngine::onBar(double lnA, double lnB) {
    if (kind == Kind::Static) {
        alphaPrior = staticAlpha;
        betaPrior = staticBeta;
        spread = lnA - (staticAlpha + staticBeta * lnB);
    } else if (kind == Kind::Kalman) {
        //the alpha/beta the filter held BEFORE seeing this bar -> exactyl what the innovation is measured against
        alphaPrior = filter.x[0];
        betaPrior = filter.x[1];
        spread = kalmanStep(filter, lnA, lnB).innovation;
    } else {
        //least squares over the bars before today (todays bar joins the window in commit)
        const double n = (double)olsCount;
        const double denominator = n * sumXX - sumX * sumX; // n^2 x the variance of x
        if (olsCount >= 2 && denominator > 0.0) { //needs 2 points and some spread in x. a NaN fails it too
            betaPrior = (n * sumXY - sumX * sumY) / denominator; //slope = cov(x, y) / var(x), the n^2 cancels
            alphaPrior = centreY + (sumY - betaPrior * sumX) / n - betaPrior * centreX; // intercept, centre put back
        } else {
            betaPrior = 1.0; //no fit possible -> beta = 1, alpha = the gap at the centre
            alphaPrior = centreY - centreX;
        }
        spread = lnA - (alphaPrior + betaPrior * lnB);
    }

    if (fixedZ) {
        fastOk = true; //z = spread / a fixed positive sd -> always finite
        return;
    }
    //quick z from the running sums -> variance = mean of squares - mean^2 loses digits when the variance is tiny, so its only trusted above 1e-6 x mean of squares (~9 good digits left). otherwise the exact path
    if (count >= zLookback) {
        const double mean = sum * invZLookback;
        const double meanSq = sumSq * invZLookback;
        fastVariance = meanSq - mean * mean;
        fastOk = fastVariance > Z_MIN_VAR_SHARE * meanSq;
        fastDeviation = (spread - centre) - mean; //mean is measured from centre too
    } else {
        fastOk = false; //window not full yet
    }
}

bool SignalEngine::absZCertainlyBelow(double limit) const {
    const double safeLimit = limit * (1.0 - Z_MARGIN); //a touch inside the limit -> borderline z goes to the exact test
    if (fixedZ) return std::abs(spread) < safeLimit * fixedSd; //abs(spread / sd) < limit without the division
    if (!fastOk) return false;
    // abs(dev) / sd < limit, without the square root or the diivsion
    return fastDeviation * fastDeviation < safeLimit * safeLimit * fastVariance;
}

ZScore SignalEngine::zExact() const {
    ZScore out;
    if (fixedZ) {
        out.mean = 0.0; //training-window z -> mean 0 and the  sd given at the start
        out.sd = fixedSd;
        out.z = (spread - out.mean) / out.sd;
        return out;
    }
    //same sums in the same order as rollingZScore.  leave the 2 loops exacty as written -> the last digit of the reference z depends on how they compile
    const size_t window = zLookback;
    const size_t first = count - window; //oldest spread of the window. todays is not stored yet (commit does that)
    double mean = 0;
    for (size_t i = 0; i < window; i++) {
        mean += past(first + i);
    }
    mean /= (double)window;

    double variance = 0;
    for (size_t i = 0; i < window; i++) {
        variance += ((past(first + i) - mean) * (past(first + i) - mean));
    }
    variance /= (double)window; // divide by n, not n - 1
    double sd = std::sqrt(variance);

    out.z = (spread - mean) / sd; //sd 0 and todays spread on the mean -> 0/0 = NaN, the strategy takes that as no z
    out.mean = mean;
    out.sd = sd;
    return out;
}

OUResult SignalEngine::gateOU(size_t window, std::vector<double>& scratch) const {
    if (count < window) { //not enough spreads yet -> the  gate stays shut
        OUResult none{};
        none.meanReverting = false;
        none.halfLife = -1.0; // the value fitOU gives for no pull back
        return none;
    }
    scratch.resize(window);
    for (size_t k = 0; k < window; k++) scratch[k] = past(count - window + k); // oldest first, newest = yesterdays
    return fitOU(scratch, 0, window); //the legacy fit, on the same `window` spreads in the same order
}

void SignalEngine::commit(const BarRing& bars) {
    if (kind == Kind::RollingOls) {
        //todays bar joins the regression window, the bar that fell out of it leaves
        const size_t t = bars.size() - 1;
        const double x = bars.lnB(t) - centreX, y = bars.lnA(t) - centreY;
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
        olsCount++;
        if (olsCount > olsWindow) { //window was full -> bar t - olsWindow drops out
            const double xLeaving = bars.lnB(t - olsWindow) - centreX, yLeaving = bars.lnA(t - olsWindow) - centreY;
            sumX -= xLeaving;
            sumY -= yLeaving;
            sumXX -= xLeaving * xLeaving;
            sumXY -= xLeaving * yLeaving;
            olsCount = olsWindow;
        }
        if (++olsSinceRebuild >= OLS_REBUILD_EVERY) rebuildOls(bars); //redone from the ring every 512 bars
    }

    spreadHistory[count & historyMask] = spread;
    count++;
    if (fixedZ) return; // a fixed sd z needs no running sums
    const double x = spread - centre;
    sum += x;
    sumSq += x * x;
    if (count > zLookback) {
        const double leaving = past(count - 1 - zLookback) - centre; //the spread that just left the window
        sum -= leaving;
        sumSq -= leaving * leaving;
        //rebuild every 256 bars, and at once if the value that left dwarfed what remains (the small remainder has almost no correct digits). negated  so a NaN rebuilds too
        if (++sinceRefresh >= Z_REFRESH_EVERY || !(leaving * leaving < Z_LEAVER_LIMIT * sumSq)) refreshSums();
    }
}

//------------------------------------------------------------------ rolling johansen gate

//how close to the critical  value the quick trace may come before the exact test takes over -> "BackTester verify" sees gaps under 1e-9 of it, this is over 10,000 times wider
static constexpr double JOHANSEN_MARGIN = 1e-5;
static constexpr size_t JOHANSEN_REBUILD_EVERY = 2048; //bars between full rebuilds of the 14 sums
static constexpr double MIN_DET_SHARE = 1e-3; //each determinant has to beat this share of its diagonal product
static constexpr double MIN_DET = 1e-11; //and above this outright ->  inverse() refuses below 1e-12

//the 14 running sums of the test back to nothing
void RollingJohansen::zeroSums() {
    sumD[0] = sumD[1] = sumY[0] = sumY[1] = 0;
    sumDD[0] = sumDD[1] = sumDD[2] = 0;
    sumYY[0] = sumYY[1] = sumYY[2] = 0;
    sumDY[0] = sumDY[1] = sumDY[2] = sumDY[3] = 0;
}

void RollingJohansen::start(const GateParams& params) {
    window = params.johansenWindow;
    recomputeEvery = params.recomputeEvery;
    warmUp = std::max(params.johansenWindow, params.ouWindow); //same warm-up as the legacy gate -> both windows full
    closeAfter = params.closeAfter;
    openAfter = params.openAfter;
    gateOpen = false; //shut until enough passes accumulate
    failStreak = 0;
    passStreak = 0;
    centreA = 0;
    centreB = 0;
    zeroSums();
    sinceRebuild = 0;
    tests = 0;
    exactTests = 0;
    worstGap = 0;
    verdictMismatches = 0;
    scratchA.resize(window); //sized once here -> the exact test never allocates
    scratchB.resize(window);
    //trace = -T ln((1-ev1)(1-ev2)) -> the 2 trace bounds as bounds on that product, a margin clear of each
    const double T = (double)(window - 1); //window  bars -> window - 1 rows
    productPass = std::exp(-johansenLargestCritical() * (1.0 + JOHANSEN_MARGIN) / T);
    productFail = std::exp(-johansenSmallestCritical() * (1.0 - JOHANSEN_MARGIN) / T);
}

//one row = the change from bar row-1 to bar row, next to the LEVEL of bar row-1. levels  are mesaured from centreA/B -> the test only uses them demeaned so the constant cancels, and small numbers keep the sums of squares accurate
void RollingJohansen::addRow(const BarRing& bars, size_t row, double sign) {
    const double dA = bars.lnA(row) - bars.lnA(row - 1), dB = bars.lnB(row) - bars.lnB(row - 1);
    const double yA = bars.lnA(row - 1) - centreA, yB = bars.lnB(row - 1) - centreB;
    sumD[0] += sign * dA; //sign = +1 adds the row, -1 takes it back out
    sumD[1] += sign * dB;
    sumY[0] += sign * yA;
    sumY[1] += sign * yB;
    sumDD[0] += sign * (dA * dA);
    sumDD[1] += sign * (dA * dB); //[0] = AA, [1] = AB, [2] = BB
    sumDD[2] += sign * (dB * dB);
    sumYY[0] += sign * (yA * yA);
    sumYY[1] += sign * (yA * yB);
    sumYY[2] += sign * (yB * yB);
    sumDY[0] += sign * (dA * yA);
    sumDY[1] += sign * (dA * yB); //change x level -> [0] = AA, [1] = AB, [2] = BA,  [3] = BB
    sumDY[2] += sign * (dB * yA);
    sumDY[3] += sign * (dB * yB);
}

void RollingJohansen::rebuild(const BarRing& bars, size_t t) {
    sinceRebuild = 0;
    centreA = bars.lnA(t); //re-centre on todays levels -> the numbers in the sums stay small
    centreB = bars.lnB(t);
    zeroSums();
    for (size_t row = t + 2 - window; row <= t; row++) addRow(bars, row, 1.0); // window - 1 rows, bars [t+1-window, t]
}

bool RollingJohansen::onBar(const BarRing& bars, size_t t) {
    if (t == 1) { // the first row -> measure the levels from the pairs first bar
        centreA = bars.lnA(0);
        centreB = bars.lnB(0);
    }
    addRow(bars, t, 1.0);
    if (t >= window) addRow(bars, t + 1 - window, -1.0); //rows are now bars [t-window+1, t] -> window-1 rows
    if (++sinceRebuild >= JOHANSEN_REBUILD_EVERY && t >= window) rebuild(bars, t); //wipes the rounding drift

    const size_t spreadIndex = t - 1; //the legacy gates clock -> spread i belongs to bar i+1
    if (spreadIndex >= warmUp && (spreadIndex - warmUp) % recomputeEvery == 0) { //streaks count tests, not bars
        const bool pass = test(bars, t);
        if (pass) {
            passStreak++;
            failStreak = 0;
        } else {
            failStreak++;
            passStreak = 0;
        }

        //hysteresis -> one odd test doesnt flip the gate
        if (gateOpen && failStreak >= closeAfter) gateOpen = false;
        if (!gateOpen && passStreak >= openAfter) gateOpen = true;
    }
    return gateOpen; //between tests the last state stands
}

JohansenResult RollingJohansen::exactTest(const BarRing& bars, size_t t) {
    //the windows bars copied out in order (bars [t+1-window, t]), then the allocation-free legacy test on them
    for (size_t k = 0; k < window; k++) {
        scratchA[k] = bars.lnA(t + 1 - window + k);
        scratchB[k] = bars.lnB(t + 1 - window + k);
    }
    return johansenExact(scratchA.data(), scratchB.data(), window);
}

//the moment matrices of the test straight from the running sums, no pass over the window -> covariance = mean of products - product of means
MomentMatrices RollingJohansen::momentsFromSums(double n, double meanDA, double meanDB) const {
    const double meanYA = sumY[0] / n, meanYB = sumY[1] / n;
    MomentMatrices S;
    S.S00[0][0] = sumDD[0] / n - meanDA * meanDA; //S00 -> covairance of the daily changes
    S.S00[0][1] = S.S00[1][0] = sumDD[1] / n - meanDA * meanDB;
    S.S00[1][1] = sumDD[2] / n - meanDB * meanDB;
    S.S11[0][0] = sumYY[0] / n - meanYA * meanYA; //S11 -> covariance of the levels
    S.S11[0][1] = S.S11[1][0] = sumYY[1] / n - meanYA * meanYB;
    S.S11[1][1] = sumYY[2] / n - meanYB * meanYB;
    S.S01[0][0] = sumDY[0] / n - meanDA * meanYA; // S01 -> changes against levels, NOT symmetric
    S.S01[0][1] = sumDY[1] / n - meanDA * meanYB;
    S.S01[1][0] = sumDY[2] / n - meanDB * meanYA;
    S.S01[1][1] = sumDY[3] / n - meanDB * meanYB;
    S.S10 = transpose(S.S01);
    return S;
}

//can the quick formula be trusted -> the 2 matrices it inverts must be well clear of singular (2 nearly identical series, e.g. 2 funds on one index, amplify the rounding)
static bool wellConditioned(const MomentMatrices& S, double det00, double det11) {
    return det00 > MIN_DET_SHARE * S.S00[0][0] * S.S00[1][1] && det11 > MIN_DET_SHARE * S.S11[0][0] * S.S11[1][1] &&
           det00 >= MIN_DET && det11 >= MIN_DET; // inverse() refuses below 1e-12 -> stay clear of it
}

//(1 - ev1)(1 - ev2) = det(I - K) = 1 - tr(K) + det(K), K = S11^-1 S10 S00^-1 S01 -> all the verdict needs, no eigenvalues, square root or logarithm
static double oneMinusEigenProduct(const MomentMatrices& S, double det00, double det11) {
    const double u00 = S.S00[0][0], u01 = S.S00[0][1], u11 = S.S00[1][1]; //u = S00, v = S11 (symmetric), s = S01
    const double v00 = S.S11[0][0], v01 = S.S11[0][1], v11 = S.S11[1][1];
    const double s00 = S.S01[0][0], s01 = S.S01[0][1], s10 = S.S01[1][0], s11 = S.S01[1][1];
    const double m00 = u11 * s00 - u01 * s10, m01 = u11 * s01 - u01 * s11; // S00^-1 S01, times det00
    const double m10 = u00 * s10 - u01 * s00, m11 = u00 * s11 - u01 * s01;
    const double n00 = v11 * s00 - v01 * s01, n01 = v11 * s10 - v01 * s11; //S11^-1 S10, times det11 (S10 = S01)
    const double n10 = v00 * s01 - v01 * s00, n11 = v00 * s11 - v01 * s10;
    const double scale = 1.0 / (det00 * det11); //divides out the det00 and det11 that m and n carry
    const double traceK = (n00 * m00 + n01 * m10 + n10 * m01 + n11 * m11) * scale; // tr(K) = tr(n m) / (det00 det11)
    const double det01 = s00 * s11 - s01 * s10;
    const double product = 1.0 - traceK + det01 * det01 * scale; //det(K) = det(S01)^2 / (det00 det11)
    return product;
}

// true -> the running sums settle the test and verdict holds the answer. false -> too close to call
bool RollingJohansen::quickVerdict(const MomentMatrices& S, double product, double meanDA, double meanDB, double n,
                                   bool& verdict) const {
    bool decided = false;
    if (product < productPass) { //trace above the largest critical value -> passes whatever the drift
        decided = true;
        verdict = true;
    } else if (product > productFail) { //trace below the smallest one -> fails whatever the drift
        decided = true;
        verdict = false;
    } else {
        // inside  the range the critical value can take -> it needs the drift, so the full legacy tail
        const JohansenResult result = johansenFromMoments(S, {meanDA, meanDB}, n);
        if (std::abs(result.trace0 - result.criticalValue) > JOHANSEN_MARGIN * result.criticalValue) {
            decided = true; //further than the margin from the critical value -> the quick verdict stands
            verdict = result.isCointegrated;
        }
    }
    return decided;
}

//verify only -> how far the quick trace is from the exact one, and whether the 2 verdicts agree
void RollingJohansen::compareWithExact(const BarRing& bars, size_t t, double n, double product, bool verdict) {
    const JohansenResult exact = exactTest(bars, t);
    //quick trace = -n ln(product). the gap is measured as a share of the critical value
    const double gap = std::abs(-n * std::log(product) - exact.trace0) / exact.criticalValue;
    if (gap > worstGap) worstGap = gap;
    if (exact.isCointegrated != verdict) verdictMismatches++;
}

//the slow exact test, for the windows the quick one hands over
bool RollingJohansen::exactVerdict(const BarRing& bars, size_t t) {
    try {
        const JohansenResult result = exactTest(bars, t);
        return result.isCointegrated;
    } catch (const std::runtime_error&) {
        //the exact test throws when the 2 series move so nearly as one that a matrix is singular (inverse() refuses) -> counts as not cointegrated. the legacy pipeline lets the throw end that pairs run instead
        return false;
    }
}

bool RollingJohansen::test(const BarRing& bars, size_t t) {
    tests++;

    const double n = (double)(window - 1); //rows in the window
    const double meanDA = sumD[0] / n, meanDB = sumD[1] / n; //average daily change of each leg (the drift)
    const MomentMatrices S = momentsFromSums(n, meanDA, meanDB);

    //quick answre only when both inverted matrices are well conditioned and the trace is not within JOHANSEN_MARGIN of the critical value. anything else -> the exact test
    const double det00 = determinant(S.S00), det11 = determinant(S.S11);
    if (wellConditioned(S, det00, det11)) {
        const double product = oneMinusEigenProduct(S, det00, det11);
        bool verdict = false;
        if (quickVerdict(S, product, meanDA, meanDB, n, verdict)) {
            if (checkEveryTest) compareWithExact(bars, t, n, product, verdict);
            return verdict;
        }
    }
    exactTests++;
    return exactVerdict(bars, t);
}

//------------------------------------------------------------------ the engine

PairEngine::PairEngine(const EngineConfig& engineConfig) : config(engineConfig) {
    //history has to raech past the longer gate window -> taking the oldest johansen row out reads bar t - window
    const size_t needed = std::max(config.gates.johansenWindow, config.gates.ouWindow) + 2;
    bars.init(std::max(config.historyBars, needed));
    johansen.start(config.gates);
    historyWindow = config.gates.ouWindow; //each signal keeps enough spreads for the half-life gate
}

int PairEngine::addKalmanSignal(const KalmanParams& params, size_t zWindow) {
    if (bars.size() != 0) throw std::runtime_error("the kalman signal has to be added before the first bar");
    signals.emplace_back();
    signals.back().startKalman(params, zWindow, historyWindow);
    return (int)signals.size() - 1; //a signal is known by its index
}

int PairEngine::addRollingOlsSignal(size_t olsWindow, size_t zWindow) {
    if (bars.size() != 0) throw std::runtime_error("the rolling OLS signal has to be added before the first bar");
    if (olsWindow + 2 > bars.capacity()) //the sliding window reads bar t - olsWindow back out of the ring
        throw std::runtime_error("rolling OLS window is longer than the price history kept");
    signals.emplace_back();
    signals.back().startRollingOls(olsWindow, zWindow, historyWindow);
    return (int)signals.size() - 1;
}

int PairEngine::addStaticSignal(double alpha, double beta, size_t zWindow, double fixedSd) {
    signals.emplace_back();
    signals.back().startStatic(alpha, beta, bars, zWindow, historyWindow, fixedSd); // bars -> history rebuitl at once
    return (int)signals.size() - 1;
}

// same signal index -> the runs trading it carry on with the new relationship
void PairEngine::restartStaticSignal(int signal, double alpha, double beta, size_t zWindow, double fixedSd) {
    signals[(size_t)signal].startStatic(alpha, beta, bars, zWindow, historyWindow, fixedSd);
}

void PairEngine::stopSignal(int signal) { signals[(size_t)signal].live = false; }

int PairEngine::addRun(int signal, const StrategyConfig& strategyConfig) {
    Run run;
    run.signal = signal;
    run.config = strategyConfig;
    runs.push_back(run);
    activeRuns.push_back((int)runs.size() - 1); //every run starts on the list the bar loop walks
    return (int)runs.size() - 1;
}

void PairEngine::setRunEnabled(int runIndex, bool enabled) { runs[(size_t)runIndex].enabled = enabled; }
void PairEngine::setRunLegBeta(int runIndex, bool use, double beta) {
    runs[(size_t)runIndex].useLegBeta = use;
    runs[(size_t)runIndex].legBeta = beta;
}

void PairEngine::clearCoolOff(int runIndex) { runs[(size_t)runIndex].coolOff = false; }

bool PairEngine::cancelPendingEntry(int runIndex) {
    Run& run = runs[(size_t)runIndex];
    // only an opening order can be withdrawn -> a pending exit stays
    if (run.pending != Pending::EntryApproved && run.pending != Pending::AwaitingRisk) return false;
    run.pending = Pending::None;
    return true;
}

void PairEngine::flattenRun(int runIndex) {
    Run& run = runs[(size_t)runIndex];
    run.state = 0; // the driver closed the position in its own books -> no fill and no trade record here
    run.pending = Pending::None;
    run.coolOff = false;
    run.markFromFill = false;
}

void PairEngine::drain() {
    //the event loop -> handle events until nothing is left to trigger
    Event event;
    while (queue.pop(event)) {
        switch (event.type) {
            case EventType::Bar: onBarEvent(event.bar); break;
            case EventType::Order: onOrderEvent(event); break;
            case EventType::Fill: onFillEvent(event); break;
        }
    }
}

//everything a bar triggers, once onBar has put it in today
void PairEngine::processBar() {
    const size_t t = bars.size(); // this bars index

    // an opening order the risk layer never answered counts as refused
    for (int runIndex : candidates) {
        Run& run = runs[(size_t)runIndex];
        if (run.pending == Pending::AwaitingRisk) run.pending = Pending::None;
    }
    candidates.clear();
    fills.clear();
    if (marksDirty) { // only strategies that were in a position have anything to clear
        for (int runIndex : activeRuns) runs[(size_t)runIndex].markToday = 0.0;
        marksDirty = false;
    }

    if (t == 0) { //the pairs first bar only sets things up
        startFirstBar(today);
        return;
    }
    bars.push(today.lnA - rebaseOffsetA, today.lnB - rebaseOffsetB, today.date);
    if (config.keepDaily) dailyPnl.push_back(0.0); //todays slot, index t - 1
    dailyTouched = false;

    //market open -> entries approved and exits ordered on the previous bar fill at this bars opening prices
    if (config.fill == FillModel::NextOpen) fillPendingAtOpen(today, t);

    //market close -> the BAR event
    pushEvent(EventType::Bar, 0, t);
    drain();
}

// the pairs very first bar -> nothing trades on it, it only fixes the rebasing (if any) and seeds the models
void PairEngine::startFirstBar(const BarData& bar) {
    if (config.rebase > 0.0) {
        rebaseOffsetA = bar.lnA - std::log(config.rebase); //so ln A - offset = ln(rebase) on this bar, same for B
        rebaseOffsetB = bar.lnB - std::log(config.rebase);
    }
    bars.push(bar.lnA - rebaseOffsetA, bar.lnB - rebaseOffsetB, bar.date);
    for (SignalEngine& signalEngine : signals) signalEngine.seed(bar.lnA - rebaseOffsetA, bar.lnB - rebaseOffsetB);
}

// next-open model -> the orders waiting from yesterday fill at this bars open
void PairEngine::fillPendingAtOpen(const BarData& bar, size_t t) {
    const bool openOk = (bar.openA == bar.openA) && (bar.openB == bar.openB); //false if either is NaN (NaN != NaN)
    for (int runIndex : activeRuns) {
        Run& run = runs[(size_t)runIndex];
        if (run.pending != Pending::EntryApproved && run.pending != Pending::Exit) continue;
        run.ticket.fillBar = t;
        //either open unusable -> BOTH legs fill at this bars close instead (one consistent snapshot)
        run.ticket.fillPriceA = openOk ? bar.openA : bar.closeA;
        run.ticket.fillPriceB = openOk ? bar.openB : bar.closeB;
        if (!openOk) closeFallbackFills++;
        run.pending = Pending::None;
        pushEvent(EventType::Fill, runIndex, t);
    }
    drain(); //fills land before the BAR event -> todays close marks the new position
}

//one message onto the queue
void PairEngine::pushEvent(EventType type, int runIndex, size_t bar) {
    Event event;
    event.type = type;
    event.run = runIndex;
    event.bar = bar;
    queue.push(event);
}

void PairEngine::finish() {
    if (!config.forceCloseAtEnd || bars.size() == 0) return;
    //data ran out while holding -> close at the last bar, like the "end" exit of the legacy loop
    const size_t lastBar = bars.size() - 1;
    for (size_t k = 0; k < runs.size(); k++) {
        Run& run = runs[k];
        if (run.state == 0) continue;
        queueCloseOrder(run, (int)k, lastBar, ExitReason::End); //forced close fills right here -> no next open exists
    }
    drain();
}

//what the 2 legs make when each price moves by that much -> long A short B, or the other way round
static double legsPnl(int state, double sharesA, double moveA, double sharesB, double moveB) {
    return state * (sharesA * moveA - sharesB * moveB);
}

void PairEngine::book(Run& run, size_t bar, double amount) {
    run.markToday += amount;
    marksDirty = true;
    if (!config.keepDaily) return;
    //first write of the day stores the value as is (so one strategy on its own gives the legacy series bit for bit), later writes add to it
    if (!dailyTouched) {
        dailyPnl[bar - 1] = amount; //bar 0 has no slot ->  bar t sits at index t - 1
        dailyTouched = true;
    } else dailyPnl[bar - 1] += amount;
}

void PairEngine::onBarEvent(size_t bar) {
    //1. regime gate and signals take in the new close
    johansenOK = johansen.onBar(bars, bar);
    const double lnA = bars.lnA(bar), lnB = bars.lnB(bar);
    for (SignalEngine& signalEngine : signals) {
        if (signalEngine.live) signalEngine.onBar(lnA, lnB);
    }

    // 2. every strategy reacts (marks its position to this  close, then decides) -> may queue an order
    bool anyFinished = false;
    for (size_t k = 0; k < activeRuns.size(); k++) {
        strategyOnBar(activeRuns[k], bar);
        if (!runs[(size_t)activeRuns[k]].active) anyFinished = true;
    }

    // traced between 2 and 3 -> the row sees exactly the windows the strategies saw
    if (traceOut && tracedSignal >= 0 && signals[(size_t)tracedSignal].live) recordTraceRow(bar, lnA, lnB);

    //3. todays spread joins the history AFTER  the decisions -> every window a strategy saw ended yseterday
    for (SignalEngine& signalEngine : signals) {
        if (signalEngine.live) signalEngine.commit(bars);
    }

    if (anyFinished) dropFinishedRuns(); //strategies past their trading window with nothing open are done for good
}

//tracing -> every exact statistic of the traced signal on this bar, as the strategy would see it
void PairEngine::recordTraceRow(size_t bar, double lnA, double lnB) {
    const SignalEngine& signalEngine = signals[(size_t)tracedSignal];
    TraceRow row;
    row.bar = bar;
    row.date = today.date;
    row.lnA = lnA;
    row.lnB = lnB;
    row.alpha = signalEngine.alphaPrior;
    row.beta = signalEngine.betaPrior;
    row.spread = signalEngine.spread;
    row.hasZ = signalEngine.hasZ();
    if (row.hasZ) { //zExact reads a full  window  -> only once  it has filled
        const ZScore zScore = signalEngine.zExact();
        row.z = zScore.z;
        row.zMean = zScore.mean;
        row.zSd = zScore.sd;
    }
    row.johansenOK = johansenOK;
    const OUResult ou = signalEngine.gateOU(config.gates.ouWindow, scratch);
    row.hasGate = bar - 1 >= config.gates.ouWindow; //spread index bar - 1 -> the OU window has flled
    row.gateMeanReverting = ou.meanReverting;
    row.gateHalfLife = ou.halfLife;
    row.hasFrozenHalfLife = bar - 1 >= config.gates.ouWindow;
    if (row.hasFrozenHalfLife)
        row.frozenHalfLife =
            frozenHalfLife(bar, signalEngine.alphaPrior, signalEngine.betaPrior, config.gates.ouWindow);
    row.sigma = frozenSpreadSigma(bar, signalEngine.betaPrior);
    traceOut->push_back(row);
}

// takes the strategies that can never trade again off the list the bar loop walks
void PairEngine::dropFinishedRuns() {
    size_t kept = 0;
    for (size_t k = 0; k < activeRuns.size(); k++) {
        if (runs[(size_t)activeRuns[k]].active) activeRuns[kept++] = activeRuns[k]; //compact in place, order kept
    }
    activeRuns.resize(kept);
}

void PairEngine::strategyOnBar(int runIndex, size_t bar) {
    Run& run = runs[(size_t)runIndex];
    const int date = today.date;

    if (run.state == 0 && run.pending == Pending::None && run.config.tradeUntil != 0 && date > run.config.tradeUntil) {
        run.active = false; //flat and past the last entry date -> nothing can ever happen again
        return;
    }
    //entries switched off, flat, no cool-off to release -> every path below would end w/o changing anthing (entryAllowed says no), so the z is never even looked at
    if (!run.enabled && run.state == 0 && !run.coolOff) return;
    const SignalEngine& signalEngine = signals[(size_t)run.signal];
    // no usable z today -> signal siwtched off, window not filled or a degenerate window (60 identical spreads -> 0/0, only checked exactly when the quick sums cant rule it out)
    const bool noZ = !signalEngine.live || !signalEngine.hasZ() ||
                     (!signalEngine.zCertainlyFinite() && std::isnan(signalEngine.zExact().z));

    if (run.state != 0) manageOpenTrade(run, runIndex, bar, signalEngine, noZ);
    else lookForEntry(run, runIndex, bar, signalEngine, noZ);
}

//in a position -> mark it to todays close, then let the exit rules have a look
void PairEngine::manageOpenTrade(Run& run, int runIndex, size_t bar, const SignalEngine& signalEngine, bool noZ) {
    if (noZ && config.legacyNanSkip) return; // the legacy loop skips such a bar completely, mark included

    markToClose(run, bar);
    if (!updateHoldZ(run, signalEngine, noZ, bar)) return;
    const ExitReason reason = exitDue(run, bar);
    if (reason != ExitReason::None) queueCloseOrder(run, runIndex, bar, reason);
}

//todays move of the frozen legs goes into the books -> normally from yesterdays close, on the day of an open fill from the fill price
void PairEngine::markToClose(Run& run, size_t bar) {
    const double fromA = run.markFromFill ? run.entryPriceA : previousCloseA;
    const double fromB = run.markFromFill ? run.entryPriceB : previousCloseB;
    run.markFromFill = false; //only the first mark after the fill
    book(run, bar, legsPnl(run.state, run.sharesA, today.closeA - fromA, run.sharesB, today.closeB - fromB));
}

//the z the exit rules judge today. false -> there is none, so nothing more to decide on this bar
bool PairEngine::updateHoldZ(Run& run, const SignalEngine& signalEngine, bool noZ, size_t bar) {
    const TradeParams& tradeParams = run.config.trade;
    const double lnA = bars.lnA(bar), lnB = bars.lnB(bar);

    if (tradeParams.frozenExit) {
        //todays prices  put through the ENTRY relationship -> moves only when prices move
        const double frozenSpread = lnA - (run.alpha0 + run.beta0 * lnB);
        run.zHold = (frozenSpread - run.mean0) / run.sd0; //the yardstick the entry z was measured with
    } else {
        if (noZ) return false; //no live z today -> nothing to judge the exits on
        run.zHold = signalEngine.zExact().z; //old way ->  live z
    }
    return true;
}

//which exit rule fires on this bar, None if the trade carries on.  stop beats target beats timeout
ExitReason PairEngine::exitDue(const Run& run, size_t bar) {
    const TradeParams& tradeParams = run.config.trade;
    const size_t spreadIndex = bar - 1; //same clock as entryIndex -> the i of the legacy loop

    //long spread went in at z < 0 -> target = z back up to -exitZ, stop = z down past -stopZ (short spread = the mirror). timeout -> held timeoutDays bars with neither firing (0 = no timeout)
    const bool target =
        (run.state == +1 && run.zHold >= -tradeParams.exitZ) || (run.state == -1 && run.zHold <= tradeParams.exitZ);
    const bool stop =
        (run.state == +1 && run.zHold < -tradeParams.stopZ) || (run.state == -1 && run.zHold > tradeParams.stopZ);
    const bool timeout = !target && !stop && run.timeoutDays > 0 && (spreadIndex - run.entryIndex) >= run.timeoutDays;

    if (!(target || stop || timeout)) return ExitReason::None;
    return stop ? ExitReason::Stop : (target ? ExitReason::Target : ExitReason::Timeout);
}

//fills in the ticket for geting out of the position and queues it
void PairEngine::queueCloseOrder(Run& run, int runIndex, size_t bar, ExitReason reason) {
    Ticket& order = run.ticket;
    order = Ticket(); //wipes whateevr the last order left in the ticket
    order.action = OrderAction::Close;
    order.reason = reason;
    order.run = runIndex;
    order.direction = run.state;
    order.signalBar = bar;
    order.sharesA = run.sharesA;
    order.sharesB = run.sharesB;
    order.refPriceA = today.closeA;
    order.refPriceB = today.closeB;
    order.z = run.zHold; //last z the exit rules saw
    pushEvent(EventType::Order, runIndex, bar);
}

//flat -> the entry checks in their fixed order, cheapest first
void PairEngine::lookForEntry(Run& run, int runIndex, size_t bar, const SignalEngine& signalEngine, bool noZ) {
    const TradeParams& tradeParams = run.config.trade;

    if (noZ || run.pending != Pending::None) return; // no z today, or an order is already on its way
    if (run.coolOff) { //after a stop or timeout -> no re-entry until z is back  inside the band
        if (signalEngine.absZCertainlyBelow(tradeParams.entryZ)) { //quick sums already sure -> no exact z needed
            run.coolOff = false;
            return;
        }
        if (std::abs(signalEngine.zExact().z) < tradeParams.entryZ) run.coolOff = false;
        return; //no entry on the release bar itself, as in the legacy loop
    }
    if (signalEngine.absZCertainlyBelow(tradeParams.entryZ)) return; //the common case -> nowhere near an entry, O(1)
    if (!entryAllowed(run, bar)) return; // outside the trading  window, or the johansen gate is shut -> also O(1)

    //possible entry -> from here on every number is the exact legacy one
    const ZScore zScore = signalEngine.zExact();
    // entry band -> stretched past entryZ but not past maxEntryZ (a jump that big is more likely a break than a bargain). written negated so a NaN z never enters
    if (!(std::abs(zScore.z) > tradeParams.entryZ && std::abs(zScore.z) < tradeParams.maxEntryZ)) return;
    if (!halfLifeGateOpen(run, signalEngine)) return; //the costly gate (an OU fit) -> only once the cheap checks pass
    const double legBeta = run.useLegBeta ? run.legBeta : signalEngine.betaPrior;
    //hedge-ratio guard last -> a refusal here means the trade would otherwise have been taken. written negated so a NaN beta is refused too
    if (!(legBeta >= run.config.minBeta && legBeta <= run.config.maxBeta)) {
        run.betaRefusals++;
        return;
    }
    queueOpenOrder(run, runIndex, bar, signalEngine, zScore, legBeta);
}

//fills in the ticket for a new position, freezes what the trade will be judged on and queues it
void PairEngine::queueOpenOrder(Run& run, int runIndex, size_t bar, const SignalEngine& signalEngine,
                                const ZScore& zScore, double legBeta) {
    const TradeParams& tradeParams = run.config.trade;
    const double lnA = bars.lnA(bar), lnB = bars.lnB(bar);

    //size = the fixed unit of capital on leg A, leg B scaled by the hedge ratio known before todays price
    Ticket& order = run.ticket;
    order = Ticket();
    order.action = OrderAction::Open;
    order.run = runIndex;
    order.direction = (zScore.z < 0) ? +1 : -1; // cheap -> long spread, rich -> short spread
    order.signalBar = bar;
    order.sharesA = tradeParams.capital / today.closeA; //sized at the signal close, even if it fills at the next open
    order.sharesB = legBeta * tradeParams.capital / today.closeB; //B gets beta x the money on A
    order.refPriceA = today.closeA;
    order.refPriceB = today.closeB;
    order.z = zScore.z;
    order.beta = legBeta;

    //frozen now -> relationship, z yardstick, timeout. with an imposed leg beta ("kalman signal, static legs" arm) alpha is set so the frozen spread still equals todays spread
    run.pendingBeta = legBeta;
    run.pendingAlpha = run.useLegBeta ? (lnA - legBeta * lnB - signalEngine.spread) : signalEngine.alphaPrior;
    run.pendingMean = zScore.mean;
    run.pendingSd = zScore.sd;
    freezeTimeout(run, bar);
    order.sigma = frozenSpreadSigma(bar, legBeta); //rides on the order -> the portfolio sizes by risk
    pushEvent(EventType::Order, runIndex, bar);
}

//how long the trade may run -> so many half-lives of the spread it is about to freeze
void PairEngine::freezeTimeout(Run& run, size_t bar) {
    const TradeParams& tradeParams = run.config.trade;
    const size_t spreadIndex = bar - 1; //spread index -> the i of the legacy loop
    const double halfLifeNow = (spreadIndex >= run.config.ouWindow)
                                   ? frozenHalfLife(bar, run.pendingAlpha, run.pendingBeta, run.config.ouWindow)
                                   : std::numeric_limits<double>::quiet_NaN(); //OU window not full yet
    if (tradeParams.timeoutHalfLives > 0.0 && !std::isnan(halfLifeNow) && halfLifeNow >= 0.0) {
        //round up -> never cut short
        run.pendingTimeout = (size_t)std::ceil(tradeParams.timeoutHalfLives * halfLifeNow);
        if (run.pendingTimeout < 1) run.pendingTimeout = 1; //half-life 0 (reverts inside a day) still gets 1 day
    } else {
        run.pendingTimeout = 0; // no measurable  reversion speed -> no timeout, leave it to the target and stop
    }
}

bool PairEngine::entryAllowed(const Run& run, size_t bar) const {
    if (!run.enabled) return false;
    const int date = today.date;
    if (run.config.tradeFrom != 0 && date < run.config.tradeFrom) return false; //yyyymmdd numbers, 0 = no limit
    if (run.config.tradeUntil != 0 && date > run.config.tradeUntil) return false;
    //blackout -> no entries on dates inside [noEntryFrom, noEntryUntil]
    if (run.config.noEntryFrom != 0 && date >= run.config.noEntryFrom && date <= run.config.noEntryUntil) return false;
    if (bar - 1 < run.config.gateWarmUp) return false; //not enough history for the gates to mean anythig
    if (run.config.useGates && !johansenOK) return false; //the regime gate is shut
    return true;
}

//the half-life gate -> the spread has to be pulling back, at a speed inside [minHalfLife, maxHalfLife]
bool PairEngine::halfLifeGateOpen(const Run& run, const SignalEngine& signalEngine) {
    if (!run.config.useGates || !run.config.useHalfLifeGate) return true; //gate switchde off -> always open
    const OUResult ou = signalEngine.gateOU(run.config.ouWindow, scratch);
    return ou.meanReverting && ou.halfLife >= run.config.minHalfLife && ou.halfLife <= run.config.maxHalfLife;
}

double PairEngine::frozenHalfLife(size_t bar, double alpha0, double beta0, size_t window) {
    // same as rollingFrozenHalfLife for one bar -> the `window` bars ending yesterday, put through the relationship a trade entering today would freeze, then the OU fit
    scratch.resize(window);
    for (size_t k = 0; k < window; k++) {
        const size_t pastBar = bar - window + k; //bars [bar - window, bar - 1]
        scratch[k] = bars.lnA(pastBar) - (alpha0 + beta0 * bars.lnB(pastBar));
    }
    return fitOU(scratch, 0, window).halfLife;
}

double PairEngine::frozenSpreadSigma(size_t bar, double beta0) const {
    //daily volatility of the spread the frozen legs will earn, over the 60 daily changes up to and including today. only travels with the order so the portfolio can size by risk
    const size_t window = 60;
    if (bar < window) return 0.0; // the oldest of the 60 changes reaches back to bar  - 60
    double sumReturns = 0, sumSqReturns = 0;
    //each row = one days change of ln  A - beta0 ln B. leave the loop exactly as written -> the last digit of the reference sigma depends on how it  compiles
    for (size_t row = bar + 1 - window; row <= bar; row++) {
        const double spreadReturn = (bars.lnA(row) - bars.lnA(row - 1)) - beta0 * (bars.lnB(row) - bars.lnB(row - 1));
        sumReturns += spreadReturn;
        sumSqReturns += spreadReturn * spreadReturn;
    }
    const double n = (double)window;
    const double variance = (sumSqReturns - sumReturns * sumReturns / n) / (n - 1.0); //sample variance, n - 1
    return variance > 0.0 ? std::sqrt(variance) : 0.0; //rounding can leave a hair below 0, and a NaN lands on 0  too
}

void PairEngine::onOrderEvent(const Event& event) {
    Run& run = runs[(size_t)event.run];
    const Ticket& order = run.ticket;
    if (order.action == OrderAction::Close) {
        //closing orders are never refused -> same-close model (and the forced close when data runs  out) fills at this bars close, next-open holds it until the next open
        if (config.fill == FillModel::SameClose || order.reason == ExitReason::End) {
            fillAtClose(run, event.run, event.bar);
        } else {
            run.pending = Pending::Exit;
        }
        return;
    }
    //opening orders go to the risk layer -> resolve() comes back with its dceision
    run.pending = Pending::AwaitingRisk;
    candidates.push_back(event.run);
}

//the order trades at the close of the bar it was made on -> stamp the ticket and queue the fill
void PairEngine::fillAtClose(Run& run, int runIndex, size_t bar) {
    run.ticket.fillBar = bar;
    run.ticket.fillPriceA = today.closeA;
    run.ticket.fillPriceB = today.closeB;
    pushEvent(EventType::Fill, runIndex, bar);
}

void PairEngine::resolve(size_t k, bool approved) {
    const int runIndex = candidates[k];
    Run& run = runs[(size_t)runIndex];
    if (run.pending != Pending::AwaitingRisk) return; //already decided, or withdrawn in the meantime
    if (!approved) {
        run.pending = Pending::None; //refused -> the pair stays flat, no cool-off as nothing was traded
        return;
    }
    if (config.fill == FillModel::SameClose) {
        run.pending = Pending::None;
        fillAtClose(run, runIndex, bars.size() - 1); // the newest bar is the one that made the order
        drain();
    } else {
        run.pending = Pending::EntryApproved; // fills at the next bars open
    }
}

void PairEngine::onFillEvent(const Event& event) {
    Run& run = runs[(size_t)event.run];
    const Ticket& fill = run.ticket;
    fills.push_back(fill); // a COPY for fillsThisBar -> the ticket itself can take a new order later on this same bar

    if (fill.action == OrderAction::Open) applyOpenFill(run, fill);
    else applyCloseFill(run, event.run, fill);
}

//the position is on -> the strategy takes over what was frozen when the order was made
void PairEngine::applyOpenFill(Run& run, const Ticket& fill) {
    run.state = fill.direction;
    run.sharesA = fill.sharesA;
    run.sharesB = fill.sharesB;
    run.entryIndex = fill.signalBar - 1; //the clock for the timeout starts at the signal bar
    run.entryFillBar = fill.fillBar;
    run.entryPriceA = fill.fillPriceA;
    run.entryPriceB = fill.fillPriceB;
    run.markFromFill = (config.fill == FillModel::NextOpen); //filled at an open -> first mark runs from  that price
    run.alpha0 = run.pendingAlpha;
    run.beta0 = run.pendingBeta;
    run.mean0 = run.pendingMean;
    run.sd0 = run.pendingSd;
    run.timeoutDays = run.pendingTimeout;
    run.zEntry = fill.z;
    run.zHold = fill.z; // in case data ends on the entry bar
}

//the position is gone -> the last overnight move, the trade record, the friction, then flat again
void PairEngine::applyCloseFill(Run& run, int runIndex, const Ticket& fill) {
    // next-open model -> the move from yesterdays close (the last mark) to this open still belongs to the trade, so it is booked today before the position goes
    if (config.fill == FillModel::NextOpen && fill.reason != ExitReason::End) { //an End exit fills at the last close
        book(run, fill.fillBar,
             legsPnl(run.state, run.sharesA, fill.fillPriceA - previousCloseA, run.sharesB,
                     fill.fillPriceB - previousCloseB));
    }

    const EngineTrade trade = closedTrade(run, runIndex, fill);
    trades.push_back(trade);

    if (config.keepDaily) dailyPnl[fill.fillBar - 1] -= trade.cost; //friction is charged on the exit day, like legacy
    run.state = 0;
    if (fill.reason == ExitReason::Stop || fill.reason == ExitReason::Timeout) run.coolOff = true; //wait for normal
}

//the finished round trip written up as one record -> P&L and friction per unit of capital. the portfolio layer does its own accounting from the fills
EngineTrade PairEngine::closedTrade(const Run& run, int runIndex, const Ticket& fill) const {
    const TradeParams& tradeParams = run.config.trade;

    EngineTrade trade;
    trade.run = runIndex;
    trade.signalBar = run.entryIndex + 1; //entryIndex is a spread index -> + 1 gives the bar
    trade.entryBar = run.entryFillBar;
    trade.exitBar = fill.fillBar;
    trade.entryDate = bars.date(trade.entryBar); //from the ring -> right while the trade is shorter than the ring
    trade.exitDate = bars.date(fill.fillBar);
    trade.direction = run.state;
    trade.zEntry = run.zEntry;
    trade.zExit = fill.z;
    trade.sharesA = run.sharesA;
    trade.sharesB = run.sharesB;
    trade.entryPriceA = run.entryPriceA;
    trade.entryPriceB = run.entryPriceB;
    trade.exitPriceA = fill.fillPriceA;
    trade.exitPriceB = fill.fillPriceB;
    trade.pnl = legsPnl(run.state, run.sharesA, fill.fillPriceA - run.entryPriceA, run.sharesB,
                        fill.fillPriceB - run.entryPriceB);
    //friction = costBps of  the notional moved at entry and at exit + commission x 4 executions (2 legs in, 2 legs out), same as the legacy loop
    const double entryNotional = run.sharesA * run.entryPriceA + run.sharesB * run.entryPriceB;
    const double exitNotional = run.sharesA * fill.fillPriceA + run.sharesB * fill.fillPriceB;
    trade.cost = tradeParams.costBps * (entryNotional + exitNotional) + 4.0 * tradeParams.commissionPerExecution;
    trade.netPnl = trade.pnl - trade.cost;
    trade.entryNotional = entryNotional;
    trade.holdingDays = (int)(fill.fillBar - run.entryFillBar); //fill to fill, in bars
    trade.reason = fill.reason;
    return trade;
}

EngineConfig legacyEngineConfig(const BacktestConfig& config) {
    EngineConfig engineConfig;
    engineConfig.gates = config.gates;
    engineConfig.fill = FillModel::SameClose; // the legacy loop trades at the close that made the signal
    engineConfig.rebase = 0.0; //raw log prices
    engineConfig.forceCloseAtEnd = true;
    engineConfig.legacyNanSkip = true;
    engineConfig.keepDaily = true;
    return engineConfig;
}

StrategyConfig legacyStrategyConfig(const BacktestConfig& config) {
    StrategyConfig strategyConfig;
    strategyConfig.trade = config.trade;
    strategyConfig.useGates = config.useGates;
    strategyConfig.minHalfLife = config.minHalfLife;
    strategyConfig.maxHalfLife = config.maxHalfLife;
    strategyConfig.ouWindow = config.gates.ouWindow;
    strategyConfig.gateWarmUp = std::max(config.gates.johansenWindow, config.gates.ouWindow); //as in computeGates
    strategyConfig.tradeFrom = dateToInt(config.tradeFrom); //"" -> 0 = no limit
    strategyConfig.tradeUntil = dateToInt(config.tradeUntil);
    return strategyConfig; //beta guard and blackout keep their defaults -> off
}

EngineRunResult runPairEngine(const PairData& data, const BacktestConfig& config) {
    if (data.n < 2) throw std::runtime_error("fewer than 2 common days: " + data.nameA + " / " + data.nameB);
    PairEngine engine(legacyEngineConfig(config));
    const int signal = engine.addKalmanSignal(config.kalman, config.zWindow);
    engine.addRun(signal, legacyStrategyConfig(config));
    engine.dailyPnl.reserve(data.n - 1); //one slot per bar after the first -> sized once, never regrown mid run

    //the backtest drievr -> feed the bars one at a time. a single pair on its own has no risk layer, so every opening order is approved as it comes
    for (size_t t = 0; t < data.n; t++) {
        engine.onBar(barAt(data, t));
        if (engine.candidateCount()) engine.approveAll();
    }
    engine.finish();

    EngineRunResult out;
    out.trades = std::move(engine.trades); //the engine is finished -> take its vectors, no copy
    out.dailyPnl = std::move(engine.dailyPnl);
    return out;
}
