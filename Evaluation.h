#pragma once
#include "Cointegration.h"
#include "Trading.h"
#include "Signals.h"

//check after wether tradable or not
struct PairVerdict {
    bool cointegrated;
    bool halfLifeOK;
    bool economicsOK;
    bool tradable; //all 3
};

PairVerdict judgePair(const JohansenResult& johansen, const OUResult& ou,
                      const std::vector<PairTrade>& trades, double minHalfLife, double maxHalfLife);

void outputPairVerdict(const PairVerdict& verdict, const OUResult& ou);