//kalman filter for the hedge ratio ->  tracks alpha and beta in lnA = alpha + beta.lnB, its daily surprise is the spread
#pragma once
#include <cstddef>
#include <vector>
#include "DataLoader.h"
#include "Matrix.h"

//everything the filter needs set from config -> stops runKalman args growing
struct KalmanParams {
    double Q = 1e-6; // how much true state can drift per day
    double R = 1e-4; //startign R -> if adaptive this is used until warm up ends, then the data takes over
    bool adaptiveR = false; //false -> old fixed R filter (keep for ablation)
    double lambda = 0.98; //share of the old average kept each day -> memory ~1/(1-lambda) = 50 days
    double minR = 1e-8; // safety floor -> R is a variance so must stay positive. set tiny so it isnt a hidden model choice. binds a lot (days at floor) -> data says R ~ 0, HPH alone alredy explains the surprises so lfetover misfit is Q not R (v/ma binds ~36%)
    size_t warmUp = 60; //days before R starts adapting -> lets P settle from its made  up starting value first
};

struct KalmanFilter {
    Vec2 x; // state [alpha, beta]
    Mat2 P; // current uncertaity about the state
    double Q; // how much true state can  drift per day
    double R; //how noisy one days price is -> changes daily if adaptive
    bool adaptiveR;
    double lambda;
    double minR;
    size_t warmUp; // days to wait before adapting
    size_t stepsSeen; //days processed so far (only counted when adaptive) -> compared against warmUp
    double avgSqSurprise; //running average of e^2 -> the total surprise size the data is actually showing
};

KalmanFilter initialiseKalman(const AlignedBar& firstBar, const KalmanParams& params);
// same thing from the 2 log prices directly -> the event engine holds prices as plain arrays, not AlignedBar
KalmanFilter initialiseKalman(double lnCloseA, double lnCloseB, const KalmanParams& params);

struct KalmanStepResult {
    double innovation; //the surprise and your tradable spread
    double S; // filters own prediction of that surrises variance
    double betaPrior; // beta before todays update -> the one the innovation was measured with
};

struct KalmanOutput {
    std::vector<std::string> dates; // index i is bars[i+1] as the filter starts on the 2nd bar -> same for every series here
    std::vector<double> beta; // hedge ratio series
    std::vector<double> betaPrior; //prior -> the beta the innovation was measured with so use this 1
    std::vector<double> alphaPrior; //prior alpha -> with betaPrior it rebuilds the exact relationship a trade entered on
    std::vector<double> spread; //surprise/e/mispricing series
    std::vector<double> S; //predicted innovation variance
    std::vector<double> R; // the R actually used each day (useful to see for adaptive)
    size_t daysAtFloor = 0; //days R was pnined at minR -> high means R is being squeezed out
};

//predict + update for one day. returns that days spread, its S and the prior beta
KalmanStepResult kalmanStep(KalmanFilter& filter, const AlignedBar& bar);
//the one real filter step -> AlignedBar version above just forwars here, so legacy pipeline and event engine run exactly the same arithmetic
KalmanStepResult kalmanStep(KalmanFilter& filter, double lnCloseA, double lnCloseB);

//mean of e^2/S over [start,end). a correctly specified filter gives ~1
double computeNIS(const KalmanOutput& kalman, size_t start, size_t end);
double nisBand(size_t n); //half width of the 95% accepntance band for a NIS computed from n samples
void outputNISResults(const KalmanOutput& kalman, size_t start, size_t end);

//average log likelihood per day over [start,end) -> higher = better fit. used to pick Q (and R when fixed) on train data without ever lookng at P&L
double kalmanLogLikelihood(const KalmanOutput& kalman, size_t start, size_t end);

KalmanOutput runKalman(const std::vector<AlignedBar>& bars, const KalmanParams& params);
void outputKalmanResults(const KalmanOutput& kalman);

