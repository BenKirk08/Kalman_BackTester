#pragma once
#include <string>
#include <vector>

struct Bar {
    std::string date; //text yyyy-mm-dd so plain string compare orders it
    double open, high, low, close;
    long long volume; // shares traded (long long)
};
std::vector<Bar> loadCSV(const std::string& filename);

struct AlignedBar {
    std::string date;
    double closeA, closeB; //raw closes of both legs on the same day
    double lnCloseA, lnCloseB; // natural log of the closes
};
std::vector<AlignedBar> alignBars(const std::vector<Bar>& barsA, const std::vector<Bar>& barsB);