#pragma once
#include <cstddef>
#include <vector>
#include "DataLoader.h"
#include "Kalman.h"

//regime gates -> which days a new trade is allowed to open
struct GateDay {
    bool hasEnoughHistory = false; //enough days for gates to work at all
    bool johansenOK = false; // cointegration statistically there still
    bool halfLifeOK = false; //still reverts on tradable clock
    bool canEnter = false; //all
};

struct GateParams {
    size_t johansenWindow = 750; //approx 3 trading years
    size_t ouWindow = 250; //approx 1 trading year
    double minHalfLife; //done in config
    double maxHalfLife; //trading days, band includes both ends
    int closeAfter = 4; //consecutive failed checks before shuting
    int openAfter = 3; // consecutive passes before reopening
    size_t recomputeEvery = 5; //re-run the heavy test weekly (trading days)
};

std::vector<GateDay> computeGates(const std::vector<AlignedBar>& bars,
        const KalmanOutput& kalman, const GateParams& params);

// control run ->  gates always open once enough data, isolates the gate effect
std::vector<GateDay> openGates(const std::vector<GateDay>& gates);

void outputGateStats(const std::vector<GateDay>& gates);