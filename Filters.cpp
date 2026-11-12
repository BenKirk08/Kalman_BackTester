#include "Filters.h"
#include "Cointegration.h"
#include "DataLoader.h"
#include "Signals.h"
#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <vector>

std::vector<GateDay> computeGates(const std::vector<AlignedBar>& bars,
        const KalmanOutput& kalman, const GateParams& params) {
    size_t N = kalman.spread.size(); //tarding days total
    std::vector<GateDay> gates(N);

    bool gateOpen = false; //shut until enough passes accumulate
    int failStreak = 0, passStreak = 0;
    size_t warmUp = std::max(params.johansenWindow, params.ouWindow); //input historical data enough to warmup both filters windows
    
    for (size_t i = warmUp; i < N; i++) {
        gates[i].hasEnoughHistory = true;

        //1. rolling johansen
        if ((i - warmUp) % params.recomputeEvery == 0) { //heavy -> only every recomputeEvery days, so the streaks count tests not days
            size_t barEnd = i + 2; //spread[i] belongs to bars[i+1] -> end exclusive, so the window ends on todays bar
            size_t barStart = barEnd - params.johansenWindow; // i >= warmUp >= johansenWindow -> never underflows
            std::vector<AlignedBar> window(bars.begin() + (std::ptrdiff_t)barStart, bars.begin() + (std::ptrdiff_t)barEnd);
            
            bool pass = johansenTest(window).isCointegrated;
            if (pass) { passStreak++; failStreak = 0; }
            else { failStreak++; passStreak = 0; }

            if (gateOpen && failStreak >= params.closeAfter) gateOpen = false; //hysteresis -> one odd test doesnt flip the gate
            if (!gateOpen && passStreak >= params.openAfter) gateOpen = true;
        }
        gates[i].johansenOK = gateOpen;

        //2. rolling OU (stops wjhats no longer reverting usefully)
        OUResult ou = fitOU(kalman.spread, i - params.ouWindow, i); //spread [i-ouWindow, i) -> ends yesterday, today  not in the fit
        gates[i].halfLifeOK = ou.meanReverting
                && ou.halfLife >= params.minHalfLife
                && ou.halfLife <= params.maxHalfLife;
            
        gates[i].canEnter = gates[i].johansenOK && gates[i].halfLifeOK;
    }
    return gates;
}

std::vector<GateDay> openGates(const std::vector<GateDay>& gates) {
    std::vector<GateDay> opened = gates;
    for (GateDay& gate : opened) gate.canEnter = gate.hasEnoughHistory; // same period but dont filter anything
    return opened;
}

void outputGateStats(const std::vector<GateDay>& gates) {
    size_t liveDays = 0, openDays = 0, johansenFails = 0, halfLifeFails = 0;
    for (const GateDay& gate : gates) {
        if (!gate.hasEnoughHistory) continue;
        liveDays++;
        if (gate.canEnter) openDays++;
        if (!gate.johansenOK) johansenFails++; //a day can fail both -> the 2 counts can add up past the blocked days
        if (!gate.halfLifeOK) halfLifeFails++;
    }
    std::cout << "\n<------Regime Gates (days allowed to open a trade)------>\n" << std::fixed << std::setprecision(1)
              << "evaluated days: " << liveDays << "   gate open: " << openDays
              << " (" << (100.0 * (double)openDays / (double)liveDays) << "%)\n"
              << "blocked by johansen: " << johansenFails
              << "   half-life: " << halfLifeFails << "\n"
              << std::defaultfloat << std::setprecision(6); // sticky formatting -> put cout back
}