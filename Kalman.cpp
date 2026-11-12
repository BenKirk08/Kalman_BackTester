#include "Kalman.h"
#include "Matrix.h"
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <limits>

//step 1 -> initialise
KalmanFilter initialiseKalman(const AlignedBar &firstBar, const KalmanParams& params) {
    return initialiseKalman(firstBar.lnCloseA, firstBar.lnCloseB, params);
}

KalmanFilter initialiseKalman(double lnCloseA, double lnCloseB, const KalmanParams& params) {
    KalmanFilter filter;
    //beta = 1, alpha is diffence (to start)
    filter.x = {lnCloseA - lnCloseB, 1.0}; 
    filter.P = {{{0.01, 0},
            {0, 0.01}}}; //start P meidum -> mostly unsure about both
    filter.Q = params.Q;
    filter.R = params.R; //starting guess -> held fixed through warm up, then the data takes over (if adaptive)
    filter.adaptiveR = params.adaptiveR;
    filter.lambda = params.lambda;
    filter.minR = params.minR;
    filter.warmUp = params.warmUp;
    filter.stepsSeen = 0;
    filter.avgSqSurprise = params.R; // seed the average at the satrting R -> starts somewhere sensible instead of 0
    return filter;
}

KalmanStepResult kalmanStep(KalmanFilter& filter, const AlignedBar& bar) {
    return kalmanStep(filter, bar.lnCloseA, bar.lnCloseB);
}

KalmanStepResult kalmanStep(KalmanFilter& filter, double lnCloseA, double lnCloseB) {
    double b = lnCloseB; double y = lnCloseA; // model -> y = alpha + beta.b + noise, so A is predicted from  B

    KalmanStepResult result;
    result.betaPrior = filter.x[1]; // capture before anything modified

    //step 2 -> predict. state is a random walk so x stays put, only uncertainty grows (same Q on alpha and beta)
    filter.P[0][0] += filter.Q;
    filter.P[1][1] += filter.Q;

    //step 3&4 -> what A should be vs what it  is. e > 0  means A is rich against B
    double e = y - (filter.x[0] + filter.x[1] * b); // e = lnA - (alpha + beta.lnb)

    //step 5 -> kalman gain. S = H.P.H + R, K = P.H/S with H = [1, b]
    double S = filter.P[0][0] + b * (filter.P[0][1] + filter.P[1][0]) + b*b*filter.P[1][1] + filter.R;
    Vec2 K = {(filter.P[0][0] + b*filter.P[0][1]) / S,
              (filter.P[1][0] + b*filter.P[1][1]) / S};

    // step 6 -> update. correct state and shrink uncertainty, x = x + K.e then P = (I - K.H).P
    filter.x[0] += K[0] * e;
    filter.x[1] += K[1] * e;
    Mat2 identityMinusKH = {{{1.0 - K[0], -K[0] * b}, //K.H = [[K0, K0.b], [K1, K1.b]] as H = [1, b]
                  {-K[1], 1.0 - K[1] * b}}};
    filter.P = multiply(identityMinusKH, filter.P);

    result.innovation = e;
    result.S = S;

    //step 7 -> adapt R. done last so it only changes tomorrow,  not the decision just made
    if (filter.adaptiveR) { // filter right -> e ~ N(0,S) so on average e^2 = S = HPH + R. rearrange -> R = average(e^2) - HPH (HPH = the part of the surprise from not knowing alpha/beta exactly)
        filter.stepsSeen++;

        //average e^2 iself, not (e^2 - HPH) -> e^2 is never negative so the average cant be dragged below 0. old version averaged (e^2 - HPH) which goes negative on quiet days  -> clamp fired inside the average and R sat stukc on the floor climbing back only 2% a day (25% of days for v/ma)
        filter.avgSqSurprise = filter.lambda * filter.avgSqSurprise + (1.0 - filter.lambda) * e * e; //EWMA  -> one day of e^2 is far too noiys to use alone

        //warm up -> P starts at a made up 0.01 so early HPH is huge (b^2 * 0.01 ~ 0.13 as b = ln price ~ 3.6, vs e^2 ~ 0.0001) -> adapting would give a massively negative R and slam it to the floor on day 2. fill the average but leave R aloen until P settles
        if (filter.stepsSeen > filter.warmUp) { //strict > -> R first changes at the end of step warmUp + 1, so step warmUp + 2 is the first to use an adapted R
            double HPH = S - filter.R; // S was buitl with todays R and prior P so this is the prior HPH (what e was measured against)
            double rNew = filter.avgSqSurprise - HPH; //surprise the data shows minus the part P already explains = noise
            filter.R = (rNew < filter.minR) ? filter.minR : rNew; //clamp only the  output -> the average underneath is untouched
        }
    }

    return result;
}

KalmanOutput runKalman(const std::vector<AlignedBar>& bars, const KalmanParams& params) {
    KalmanOutput output;
    KalmanFilter filter = initialiseKalman(bars[0], params);

    for (size_t t = 1; t < bars.size(); t++) { //bars[0] only seeds the filter -> output index i is bars[i+1]
        double betaPrior = filter.x[1]; // the beta used to compute this innovation
        double alphaPrior = filter.x[0]; //and the alpha -> togethre they are exactly what e was measured against
        double rUsed = filter.R; //capture before step -> kalmanStep adapts at the end so after it filter.R is tomorrows

        KalmanStepResult step {};
        step = kalmanStep(filter, bars[t]);
        output.dates.push_back(bars[t].date);
        output.betaPrior.push_back(betaPrior);
        output.alphaPrior.push_back(alphaPrior);
        output.beta.push_back(filter.x[1]); // beta AFTER todays update -> not the one e was measured with
        output.spread.push_back(step.innovation);
        output.S.push_back(step.S); //needed for NIS
        output.R.push_back(rUsed); //the R used that day -> matches the S stored above
        if (rUsed <= params.minR) output.daysAtFloor++; //clamp sets R to exactyl minR so <= catches every pinned day
    }
    return output;
}

void outputKalmanResults(const KalmanOutput& kalman) {
    std::cout << "\n<------Kalman Filter (hedge ratio, one line per year)------>\n" << std::fixed;
    for (size_t i = 0; i < kalman.beta.size(); i += 250) // 250 trading days ~ 1 year
        std::cout << kalman.dates[i] << "  beta = " << std::setprecision(3) << kalman.beta[i]
                  << "  spread = " << std::setprecision(4) << kalman.spread[i] << "\n";
    double betaMin = 1e9, betaMax = -1e9;
    for (double beta : kalman.beta) { 
        if (beta < betaMin) betaMin = beta; 
        if (beta > betaMax) betaMax = beta; 
    }
    std::cout << std::setprecision(3) << "beta range: [" << betaMin << ", " << betaMax << "]\n";

    double rMin = 1e9, rMax = -1e9;
    for (double rValue : kalman.R) {
        if (rValue < rMin) rMin = rValue;
        if (rValue > rMax) rMax = rValue;
    }
    std::cout << std::scientific << std::setprecision(1) //1.0e-04 style -> R is an order of magnitude
              << "R range: [" << rMin << ", " << rMax << "]   days at floor: " << kalman.daysAtFloor
              << std::fixed << " (" << 100.0 * (double)kalman.daysAtFloor / (double)kalman.R.size() << "%)\n"
              << std::defaultfloat << std::setprecision(6); // sticky formatting -> put cout back
}

//NIS = mean of e^2/S -> under a corretc filter e ~ N(0,S) so e^2/S is chi-squared with 1 dof and mean is 1
double computeNIS(const KalmanOutput& kalman, size_t start, size_t end) {
    if (end > kalman.spread.size()) end = kalman.spread.size();
    if (start >= end) return 0.0; //empty range -> 0, nothing to average
    double sum = 0; size_t n = 0;
    for (size_t i = start; i < end; i++) {
        if (kalman.S[i] <= 0.0) continue; //S is a variance -> a day without a positive one cant be scored
        sum += (kalman.spread[i] * kalman.spread[i]) / kalman.S[i];
        n++;
    }
    return n ? sum / (double)n : 0.0; // mean over the days actually scored -> 0 if none
}

//log likelihood of the innovations -> rewards e^2/S small  (surprises land inside  the forecast) AND ln S small (forecast not just made huge). the 2 pull against each other, so unlike NIS ~ 1 alone it cant be gamed by inflating S -> picks the sharpest forecast thats still honest
double kalmanLogLikelihood(const KalmanOutput& kalman, size_t start, size_t end) {
    const double badFit = -std::numeric_limits<double>::infinity(); //returned when thers nothing to score -> never chosen
    if (kalman.S.size() != kalman.spread.size()) return badFit; //S was never stored
    if (end > kalman.spread.size()) end = kalman.spread.size();
    if (start >= end) return badFit;

    const double LN_2PI = std::log(2.0 * 3.14159265358979323846); //filter right -> each e ~ N(0,S), and the normal pdf says how likely that days e was -> ln f(e) = -0.5  * (ln(2pi) + ln S + e^2/S)
    double sum = 0; size_t n = 0;
    for (size_t i = start; i < end; i++) {
        if (kalman.S[i] <= 0.0) continue; //S must be positive -> log of it  otherwise meaningless
        sum += -0.5 * (LN_2PI + std::log(kalman.S[i]) + (kalman.spread[i] * kalman.spread[i]) / kalman.S[i]); //each days ln f(e) added up = the log likelihood -> textboko way to fit kalman settings (prediction error decomposition)
        n++;
    }
    return n ? sum / (double)n : badFit; //per day average -> comparable across windows of different lengths
}

//n*NIS is approx chi squared w/  n dof -> mean n, var 2n -> NIS has sd sqrt(2/n). big n -> close to normal, so  the 95% 2 tailed band is 1 +- 1.96*sqrt(2/n)
double nisBand(size_t n) {
    return n ? 1.96 * std::sqrt(2.0 / double(n)) : 0.0; //n = 0 -> band of 0 instead of divdiing by 0
}

void outputNISResults(const KalmanOutput& kalman, size_t start, size_t end) {
    if (end > kalman.spread.size()) end = kalman.spread.size();
    const size_t n = (end > start) ? end - start : 0; //size_t would wrap if start is past end
    const double nis = computeNIS(kalman, start, end);
    const double band = nisBand(n);

    std::cout << "\n<------Filter Consistency (NIS, should be ~1)------>\n" << std::fixed << std::setprecision(3)
              << "NIS = " << nis << " over " << n << " days"
              << "   95% band [" << (1.0 - band) << ", " << (1.0 + band) << "]\n"
              << std::defaultfloat << std::setprecision(6); //sticky formatting -> put cout back

    if (nis > 1.0 + band) {
        std::cout << "  -> OVER CONFIDENT: surprises exceed the filter's forecast. "
                     "R and/or Q too small so the filter under reacts.\n";
    }
    else if (nis < 1.0 - band) {
        std::cout << "  -> OVER CAUTIOUS: surprises smaller than forecast. "
                     "R and/or Q too large so beta will be stale.\n";
    }
    else { std::cout << "  -> well specified.\n"; }
}