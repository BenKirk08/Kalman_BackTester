#pragma once
#include "Kalman.h"
#include <cstddef>
#include <vector>

//OU (ornstein-uhlenbeck) fit  over the spread
struct OUResult { 
    double slope; //pull stength 
    double halfLife; //trading days for the expected deviation to halve
    bool meanReverting; // false when slope >= 0 -> no pull back to the mean
};

//can call with different ranges. At start full range to check quick enough profitable to begin with
OUResult fitOU(const std::vector<double>& spread, size_t start, size_t end); // end exclusive -> fits spread[start..end-1]
void outputOUResults(const OUResult& ou);

//z of each spread vs the window of days before it (excludes today) -> NaN until the window fills
std::vector<double> rollingZScore(const std::vector<double>& spread, size_t window,
                                  std::vector<double>* meanOut = nullptr, std::vector<double>* sdOut = nullptr); //optional -> the mean/sd behind each z, trading freezes the entry days pair
void outputZScoreResults(const std::vector<double>& z, const KalmanOutput& kalman);

//half-life of the spread a trade would actually HOLD -> sets the timeout. NOT the innovation one (reverts ~20x faster, used to time most trades out before they reverted)
std::vector<double> rollingFrozenHalfLife(const std::vector<AlignedBar>& bars, const KalmanOutput& kalman, size_t window); //window ends yesterday -> no look ahead, NaN until it fills