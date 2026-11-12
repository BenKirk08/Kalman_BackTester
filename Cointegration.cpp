//johansen  test for cointegration w/ constant term, no trend (series integrated of order 1 -> stationary after first difference). answers yes/no -> does an anchored relationship exist using a single averaged hedge ratio
#include "Cointegration.h"
#include "Matrix.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>

Residuals buildResiduals(const std::vector<AlignedBar>& bars) {
    size_t barCount = bars.size();
    Residuals residuals;

    //build raw dy_t and lagged level y_t-1 -> starting  at t = 1
    std::vector<Vec2> dy; //dy_t -> log price today minus yesterday, {A, B}
    std::vector<Vec2> yLag; //y_t-1
    for (size_t t = 1; t < barCount; t++) {
        double dA = bars[t].lnCloseA - bars[t-1].lnCloseA;
        double dB = bars[t].lnCloseB - bars[t-1].lnCloseB;
        dy.push_back({dA, dB});
        yLag.push_back({bars[t-1].lnCloseA, bars[t-1].lnCloseB}); //level from t-1 NOT t -> row i pairs the change into bar i+1 with the lveel at bar i
    }

    size_t usableRows = dy.size(); //barCount  - 1 -> the first bar has no change

    //column menas of each series
    double meanDyA = 0, meanDyB = 0, meanYA = 0, meanYB = 0;
    for (size_t i = 0; i < usableRows; i++) {
        meanDyA += dy[i][0]; meanDyB += dy[i][1];
        meanYA += yLag[i][0]; meanYB += yLag[i][1];
    }
    meanDyA /= (double)usableRows; meanDyB /= (double)usableRows;
    meanYA /= (double)usableRows; meanYB /= (double)usableRows;
    residuals.meanDy = {meanDyA, meanDyB}; //kept -> the drift decides which critical value applies

    //lag 1 -> no lagegd changes to take out, only the cnstant, so subtracting the mean IS the regression (gives R0 & R1)
    for (size_t i = 0; i < usableRows; i++) {
        residuals.R0.push_back({dy[i][0] - meanDyA, dy[i][1] - meanDyB});
        residuals.R1.push_back({yLag[i][0] - meanYA, yLag[i][1] - meanYB});
    }

    return residuals; //what short term runs cant explain (->cleaned)
}

MomentMatrices computeMoments(const Residuals & residuals) {
    size_t rowCount = residuals.R0.size();
    MomentMatrices S{}; //{} zeroes all 4 -> the sums below need a 0 start

    //sum of outer products over every row
    for (size_t k = 0; k < rowCount; k++ ){
        for (size_t i = 0; i < 2; i++) {
            for (size_t j = 0; j < 2; j++) {
                S.S00[i][j] += residuals.R0[k][i] * residuals.R0[k][j];
                S.S11[i][j] += residuals.R1[k][i] * residuals.R1[k][j];
                S.S01[i][j] += residuals.R0[k][i] * residuals.R1[k][j]; //row index = which change, column = which level
            }
        }
    }

    //divide number by roes -> sums  become averages
    for (size_t i = 0; i < 2; i++) {
        for (size_t j = 0; j < 2; j++) {
            S.S00[i][j] /= (double)rowCount;
            S.S11[i][j] /= (double)rowCount;
            S.S01[i][j] /= (double)rowCount;
        }
    } //residuals have mean 0 -> the average product IS the covariance (E[xy] - 0)
    S.S10 = transpose(S.S01); //S01 is same but transposed -> save time computing

    return S; //S00/S11 -> how much the changes/levels vary, S01/S10 -> the change & level link the test measures
}

//original eqn det(ev.S11 - S10.S00^-1.S01) = 0 -> same as det(S11).det(ev.I - K) = 0, det(S11) not 0 so divide out -> det(ev.I - K) = 0 whree K = S11^-1.S10.S00^-1.S01
Vec2 johansenEigenvalues(const MomentMatrices& S) {
    Mat2 K = multiply(multiply(multiply(
    inverse(S.S11), S.S10), inverse(S.S00)), S.S01); //inverse throws when S11 or S00 is close to singular (abs(det) < 1e-12)
    return eigenvalues(K); // {larger, smaller} -> each is a squared correlation (0 to 1) between a mix of the 2 levels and a mix of the 2 changes, ie the srength of the level to change link
}

//5% critical values for trace0 (2 series, constant, lag 1) by drift strength kappa, built by simulation -> textbook 15.49 only fits a strong drift but stock prices braely drift vs their daily noise, w/ no drift the real cut-off is ~18.4 so 15.49 passed ~11% of unrelated pairs not 5% (see README)
const std::array<double, 12> KAPPA_GRID = {0, 0.5, 1, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10}; //drift in standard errors, 0 = none. the test ONLY feels the drift through this one number -> 1 table works for every pair and window length (checked at T = 750 the rolling gate and 2500 full sample -> unrelated pairs pass 4-5.5% not 11%)
const std::array<double, 12> CV_TRACE0 = {18.36, 17.96, 17.68, 17.15, 16.90, 16.62, 16.20, 15.80, 15.68, 15.61, 15.61, 15.4943}; //95th percentile of trace0 over 24,000 pairs of unrelated random walks per kappa (T = 1000 days) -> kept never rising, reaches the textbook value by kappa 10

//straight line between the 2 nearest grid points -> flat at 15.4943 past kappa 10
static double traceCriticalValue(double kappa) {
    if (kappa >= KAPPA_GRID.back()) return CV_TRACE0.back();
    size_t i = 1; //from 1 so i - 1 is always a valid left point (kappa 0 -> i stays 1, weight 0, gives 18.36)
    while (KAPPA_GRID[i] < kappa) i++; //first grid point at or above kappa -> stops by the last one (kappa < 10)
    double weight = (kappa - KAPPA_GRID[i - 1]) / (KAPPA_GRID[i] - KAPPA_GRID[i - 1]); //0 -> left point, 1 -> right point
    return CV_TRACE0[i - 1] + weight * (CV_TRACE0[i] - CV_TRACE0[i - 1]);
}

//the 2 ends of the table -> 15.4943 (strong drift) and 18.36 (no drift)
double johansenSmallestCritical() { return *std::min_element(CV_TRACE0.begin(), CV_TRACE0.end()); }
double johansenLargestCritical() { return *std::max_element(CV_TRACE0.begin(), CV_TRACE0.end()); }

//run full test from bars -> the backtest verdict (Backtest.cpp) and the rolling gate (Filters.cpp) call it
JohansenResult johansenTest(const std::vector<AlignedBar>& bars) {
    Residuals residuals = buildResiduals(bars);
    MomentMatrices S = computeMoments(residuals);
    double T = (double)residuals.R0.size(); // no of useable rows
    return johansenFromMoments(S, residuals.meanDy, T);
}

JohansenResult johansenFromMoments(const MomentMatrices& S, const Vec2& meanDy, double T) {
    Vec2 lambda = johansenEigenvalues(S); // lambda[0] = larger eigenvalue, lambda[1] = smaller

    JohansenResult result;
    //trace0 = -T[ln(1-ev1) + ln(1-ev2)] -> the bigger the eigenvalues the bigger the trace
    result.trace0 = -T * (std::log(1.0 - lambda[0]) + std::log(1.0 - lambda[1]));
    result.trace1 = -T * std::log(1.0 - lambda[1]); //second relatino (presumes larger value already)

    //drift strength -> kappa^2 = T . mean . S00^-1 . mean (S00 = covariance of the daily changes)
    Mat2 inverseS00 = inverse(S.S00);
    double kappaSq = T * (meanDy[0] * (inverseS00[0][0] * meanDy[0] + inverseS00[0][1] * meanDy[1])
                        + meanDy[1] * (inverseS00[1][0] * meanDy[0] + inverseS00[1][1] * meanDy[1]));
    // meausred kappa^2 comes out 2 too big on average (noise in the 2 sample means adds ~1 each even w/ zero drift) -> take it off or a driftlses pair gets an easier cut-off (6% false passes not 5%)
    result.drift = std::sqrt(std::max(kappaSq - 2.0, 0.0)); //floored at 0 (for sqrt)
    result.criticalValue = traceCriticalValue(result.drift);

    //verdict from trace0 only -> "at least 1 relation". trace1 used to have to stay under 3.84 too ("exactly 1") but rank 2 = both prices statoinary on their own -> any spread of them is too, still tradable. and that 3.84 has the same drift problem (real cut-off ~7.4-7.9 w/ realistic drift) which threw out ~25-34% of genuinely cointegrated pairs
    result.isCointegrated = result.trace0 > result.criticalValue;
    return result;
}

//johansenTest without the vectors -> pass 1 = the 4 column means, pass 2 = the 3 moment matrices. every sum adds the same terms in the same order as buildResiduals + computeMoments so the result matches
JohansenResult johansenExact(const double* lnA, const double* lnB, size_t barCount) {
    const size_t usableRows = barCount - 1; //first bar has no change
    double meanDyA = 0, meanDyB = 0, meanYA = 0, meanYB = 0;
    for (size_t t = 1; t < barCount; t++) {
        double dA = lnA[t] - lnA[t-1];
        double dB = lnB[t] - lnB[t-1];
        meanDyA += dA; meanDyB += dB;
        meanYA += lnA[t-1]; meanYB += lnB[t-1]; // lagged lveel -> t-1 NOT t
    }
    meanDyA /= (double)usableRows; meanDyB /= (double)usableRows;
    meanYA /= (double)usableRows; meanYB /= (double)usableRows;

    MomentMatrices S{};
    for (size_t t = 1; t < barCount; t++) {
        double dA = lnA[t] - lnA[t-1]; //worked out again -> pass 1 stored  nothing
        double dB = lnB[t] - lnB[t-1];
        Vec2 R0 = {dA - meanDyA, dB - meanDyB}; //the row buildResiduals stores as R0[t-1]  -> here used once and dropped
        Vec2 R1 = {lnA[t-1] - meanYA, lnB[t-1] - meanYB};
        for (size_t i = 0; i < 2; i++) {
            for (size_t j = 0; j < 2; j++) {
                S.S00[i][j] += R0[i] * R0[j];
                S.S11[i][j] += R1[i] * R1[j];
                S.S01[i][j] += R0[i] * R1[j];
            }
        }
    }
    for (size_t i = 0; i < 2; i++) {
        for (size_t j = 0; j < 2; j++) {
            S.S00[i][j] /= (double)usableRows; //sums -> averages. divide by rows NOT rows - 1, same as  computeMoments
            S.S11[i][j] /= (double)usableRows;
            S.S01[i][j] /= (double)usableRows;
        }
    }
    S.S10 = transpose(S.S01);
    return johansenFromMoments(S, {meanDyA, meanDyB}, (double)usableRows); //T = usable rows, same as johansenTest passes
}

void outputJohansenResults(const JohansenResult& result) {
    std::cout << "\n<------Cointegration (Johansen)------>\n" << std::fixed << std::setprecision(2)
              << (result.isCointegrated ? "IS cointegrated at 5%" : "is NOT cointegrated at 5%")
              << " -> trace " << result.trace0 << " vs threshold " << result.criticalValue
              << " (needs to be above. threshold set by drift " << result.drift << ")\n"
              << std::defaultfloat << std::setprecision(6); //stikcy formatting -> put cout back
}