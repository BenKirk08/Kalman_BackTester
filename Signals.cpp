#include "Signals.h"
#include "Kalman.h"
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <vector>
#include <iostream>

std::vector<double> rollingZScore(const std::vector<double>& spread, size_t window,
                                  std::vector<double>* meanOut, std::vector<double>* sdOut) {
    //NaN prefill -> the first window days never get a z
    std::vector<double> z(spread.size(), std::numeric_limits<double>::quiet_NaN());
    //same NaN warm up as z -> no yardstick exists before the window fills
    if (meanOut) meanOut->assign(spread.size(), std::numeric_limits<double>::quiet_NaN());
    if (sdOut) sdOut->assign(spread.size(), std::numeric_limits<double>::quiet_NaN());

    for (size_t t = window; t < spread.size(); t++) {
        // mean of the window ending yesterday [t-window, t-1]
        double mean = 0;
        for (size_t i = (t-window); i < t; i++) {
            mean += spread[i];
        }
        mean /= (double)window;

        //population sd of that window (n not n-1)
        double variance = 0;
        for (size_t i = (t-window); i < t; i++) { //variance = (x_i-mean)^2/n
            variance += ((spread[i]-mean) * (spread[i] - mean));
        }
        variance /= (double)window;
        double sd = std::sqrt(variance);

        z[t] = (spread[t] - mean) / sd; //how many sds from normal is today
        if (meanOut) (*meanOut)[t] = mean; // keep the yardstick -> the exact mean and sd that produced z[t]
        if (sdOut) (*sdOut)[t] = sd;
    }
    return z;
}

void outputZScoreResults(const std::vector<double>& z, const KalmanOutput& kalman) {
    size_t stretchedDays = 0, validDays = 0;
    double zMax = 0; std::string zMaxDate;
    for (size_t i = 0; i < z.size(); i++) {
        if (std::isnan(z[i])) continue;
        validDays++;
        if (std::abs(z[i]) > 2.0) stretchedDays++;
        if (std::abs(z[i]) > std::abs(zMax)) { // compare size but keep the sign
            zMax = z[i]; 
            zMaxDate = kalman.dates[i]; 
        }
    }
    std::cout << "\n<------Z-Scores (how stretched the spread is)------>\n" << std::fixed << std::setprecision(1)
              << "days with |z|>2: " << stretchedDays << " / " << validDays
              << " (" << (100.0 * (double)stretchedDays / (double)validDays) << "%)\n"
              << std::setprecision(2) << "most extreme z = " << zMax << " on " << zMaxDate << "\n"
              << std::defaultfloat << std::setprecision(6); //sticky formatting -> put cout back
}

OUResult fitOU(const std::vector<double> &spread, size_t start, size_t end) {
    //regression sums for t in [start+1, end) -> y = dS_t, x = S_t-1
    double n = 0, sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
    for (size_t t = start+1; t< end; t++) {
        double x = spread[t-1];
        double y = spread[t] - x;
        n +=1; sumX += x; sumY += y;
        sumXX += x*x; sumXY += x*y;
    }

    //OLS slope b w/ intercept -> the spread can revert to a mean that isnt 0
    OUResult result {};
    result.slope = (n * sumXY - sumX*sumY) / (n * sumXX - sumX* sumX);

    if (result.slope >= 0.0) {
        //no pull toawrds mean at all -> not tardable  as mean reversion
        result.meanReverting = false;
        result.halfLife = -1.0; // -1 = never comes home (flag, not a real half-life)
    } else if (result.slope <= -1.0) { //prevent error with log
        result.meanReverting = true; //overshoots past mean each day -> reverts faster than 1 day
        result.halfLife = 0.0; //effectively intrabale
    } else {
        result.meanReverting = true;
        result.halfLife = -std::log(2.0) / std::log(1.0 + result.slope); //theta = -ln(1+b) is the OU speed -> half-life = ln2/theta
    }
    return result;
}

void outputOUResults(const OUResult& ou) {
    std::cout << "\n<------Mean Reversion (OU)------>\n" << std::fixed << std::setprecision(3)
              << "OU slope = " << ou.slope
              << "   half-life = " << std::setprecision(2) << ou.halfLife << " days"
              << "   mean-reverting: " << (ou.meanReverting ? "yes" : "no") << "\n"
              << std::defaultfloat << std::setprecision(6); // sticky formatting -> put cout back
}

std::vector<double> rollingFrozenHalfLife(const std::vector<AlignedBar>& bars, const KalmanOutput& kalman, size_t window) {
    const size_t n = kalman.spread.size();
    std::vector<double> halfLife(n, std::numeric_limits<double>::quiet_NaN()); //NaN -> not enough history yet
    std::vector<double> frozenSpread(window); //reused every day -> the trailing spread at that days alpha/beta

    for (size_t t = window; t < n; t++) {
        //the relationship a trade entering on day  t would freeze -> prior state, known before that days price
        const double alpha0 = kalman.alphaPrior[t];
        const double beta0 = kalman.betaPrior[t];

        //spread index s belongs to bars[s+1] -> days [t-window, t) use bars [t-window+1, t]. ends yesterday
        for (size_t k = 0; k < window; k++) {
            const AlignedBar& bar = bars[t - window + k + 1];
            frozenSpread[k] = bar.lnCloseA - (alpha0 + beta0 * bar.lnCloseB); //ONE alpha/beta  across  the whole window, unlike the innovations
        }
        halfLife[t] = fitOU(frozenSpread, 0, window).halfLife; //how fast THIS spread has been pulling back lately
    }
    return halfLife;
}