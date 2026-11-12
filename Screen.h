#pragma once
#include <cstddef>
#include <vector>
#include "Cointegration.h"
#include "Signals.h"

// pair screening -> only reads the  window it is given, the walk-forward passes the training bars and nothing else

//least squares y = alpha + beta x
struct OlsFit {
    double alpha = 0, beta = 1; //what stays when there is nothing to fit (n < 3 or x never moved)
    double residualSd = 0; //standard deviation of the residuals (their mean is 0 by construction)
};
OlsFit olsFit(const double* y, const double* x, size_t n);

struct PairScreen {
    JohansenResult johansen{}; //trace test on the windows log prices
    OlsFit ols; //ln A = alpha + beta ln B over the window -> the static arms hedge
    OUResult residualOU{}; //how fast the OLS residual reverts
    bool passed = false; //cointegrated at 5% AND residual half-life inside the band
};
//scratch -> callre owned buffer for the residuals, reused between screens
PairScreen screenPair(const double* lnA, const double* lnB, size_t n, double minHalfLife, double maxHalfLife,
                      std::vector<double>& scratch);
