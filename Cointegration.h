#pragma once
#include <cstddef>
#include <vector>
#include "DataLoader.h"
#include "Matrix.h" // bring Vec2 & Mat2

// holds the two demeaned redisual series -> i.e.  what short run terms cant explain 
struct Residuals {
    std::vector<Vec2> R0; // cleaned changes (dy_t)
    std::vector<Vec2> R1; //cleaned levels (y_t-1)
    Vec2 meanDy; // average daily change of each series (the drift) -> picks the critical value in johansenFromMoments
};

//the 4 moment matices
struct MomentMatrices{
    Mat2 S00, S11, S01, S10; //00  = changes w/ changes, 11 = levels w/ levels, 01 = changes w/ levels, 10 = 01 transposed
};

struct JohansenResult {
    double trace0; // test for H0 -> no cointegration. the verdict is  built from this one
    double trace1; //test for H0 -> at most 1 relation. filled in but NOT part of the verdict (see johansenFromMoments)
    double drift; //kappa -> how big the drift is vs the noise, in standard errors (0 = no drift)
    double criticalValue; // 5% cut-off trace0 had to beat -> depends on the drift, so its stored for the output
    bool isCointegrated; //true -> at least one cointegrating relation at 5% (trace0 > criticalValue)
};

Residuals buildResiduals(const std::vector<AlignedBar>& bars);
MomentMatrices computeMoments(const Residuals& residuals);
Vec2 johansenEigenvalues(const MomentMatrices& S);
JohansenResult johansenTest(const std::vector<AlignedBar>& bars);

//last step of the test, from the moment matrices on (eigenvalues -> trace -> drift -> critical value -> verdict). johansenTest and johansenExact both end by calling this, the engines rolling gate calls it direct  w/ moments from its running sums. T  = no of usable rows
JohansenResult johansenFromMoments(const MomentMatrices& S, const Vec2& meanDy, double T);

// same test straght from 2 arrays of log prices w/ no vectors built -> same sums in the same order as johansenTest so it gives the same answer. used by the event engine (rolling gate, pair screening)
JohansenResult johansenExact(const double* lnA, const double* lnB, size_t barCount);

//smallest and largest value the drift-adjusted 5% critical value can take -> a trace above the largest passes and one below the smallest fails whatever the drift is (the engines quick verdict uses tehse)
double johansenSmallestCritical();
double johansenLargestCritical();
void outputJohansenResults(const JohansenResult& result);