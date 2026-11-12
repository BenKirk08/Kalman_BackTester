#include "Screen.h"
#include <cmath>

OlsFit olsFit(const double* y, const double* x, size_t n) {
    OlsFit fit;
    if (n < 3) return fit;
    //centred sums -> log prices sit around 4-6 while their spread is ~0.1, so raw sums of squares would cancel badly
    double meanX = 0, meanY = 0;
    for (size_t i = 0; i < n; i++) {
        meanX += x[i];
        meanY += y[i];
    }
    meanX /= (double)n;
    meanY /= (double)n;
    double sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; i++) {
        const double dx = x[i] - meanX;
        sxx += dx * dx;
        sxy += dx * (y[i] - meanY);
    }
    if (!(sxx > 0.0)) return fit; //x never moved -> no slope to estimate, keep beta = 1
    fit.beta = sxy / sxx; // slope = cov(x, y) / var(x), the 1/n cancels
    fit.alpha = meanY - fit.beta * meanX; // line goes through the 2 means
    double sumSquares = 0;
    for (size_t i = 0; i < n; i++) {
        const double residual = y[i] - (fit.alpha + fit.beta * x[i]);
        sumSquares += residual * residual;
    }
    fit.residualSd = std::sqrt(sumSquares / (double)n); // over n (not n - 2) -> plain sd of the residuals
    return fit;
}

PairScreen screenPair(const double* lnA, const double* lnB, size_t n, double minHalfLife, double maxHalfLife,
                      std::vector<double>& scratch) {
    PairScreen screen;
    if (n < 30) return screen; // nothing meaningful to test
    screen.johansen = johansenExact(lnA, lnB, n);
    screen.ols = olsFit(lnA, lnB, n); //y = ln A, x = ln B
    scratch.resize(n);
    // OLS residual per bar -> the series the OU fit below measures
    for (size_t i = 0; i < n; i++) scratch[i] = lnA[i] - (screen.ols.alpha + screen.ols.beta * lnB[i]);
    screen.residualOU = fitOU(scratch, 0, n);
    //cointegrated AND pulls back (slope < 0) w/ a half-life inside [min, max], both ends allowed
    screen.passed = screen.johansen.isCointegrated && screen.residualOU.meanReverting &&
                    screen.residualOU.halfLife >= minHalfLife && screen.residualOU.halfLife <= maxHalfLife;
    return screen;
}
