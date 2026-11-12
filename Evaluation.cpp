#include "Evaluation.h"
#include <iomanip>
#include <iostream>

PairVerdict judgePair(const JohansenResult& johansen, const OUResult& ou, const std::vector<PairTrade>& trades, double minHalfLife, double maxHalfLife) {
    PairVerdict verdict{};
    verdict.cointegrated = johansen.isCointegrated;
    verdict.halfLifeOK = ou.meanReverting && //must revert AND sit inside the half-life band
        ou.halfLife >= minHalfLife && ou.halfLife <= maxHalfLife;
    //average net P&L  vs average cost per trade over past trades
    double avgNet = 0, avgCost = 0;
    for (const PairTrade& trade : trades) {
        avgNet += trade.netPnl;
        avgCost += trade.cost;
    }
    if (!trades.empty()) {
        avgNet /= (double)trades.size();
        avgCost /= (double)trades.size(); 
    }
    // make 2 times the cost (worth trading)
    verdict.economicsOK = !trades.empty() && avgNet > 2.0 * avgCost;

    verdict.tradable = verdict.cointegrated && verdict.halfLifeOK && verdict.economicsOK;
    return verdict;
}

void outputPairVerdict(const PairVerdict& verdict, const OUResult& ou) {
    std::cout << "\n<------Pair Verdict------>\n"
              << "cointegration: " << (verdict.cointegrated ? "PASS" : "FAIL") << "\n"
              << "half-life:     " << (verdict.halfLifeOK ? "PASS" : "FAIL")
              << std::fixed << std::setprecision(2) << " (" << ou.halfLife << " days, band 0.5-30)\n"
              << "economics:     " << (verdict.economicsOK ? "PASS" : "FAIL")
              << " (avg trade vs 2x avg cost)\n"
              << (verdict.tradable ? "==> TRADABLE\n" : "==> NOT TRADABLE\n")
              << std::defaultfloat << std::setprecision(6); //sticky  formatting -> put cout back
}