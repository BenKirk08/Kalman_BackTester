#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

//market data for the event engine -> each ticker csv parsed once, dates as ints, a pair is 2 tickers lined up by date (usually w/o copying, see alignPair)

//20230104 <-> "2023-01-04". 0 means "no date" (open ended) everywhere a date limit is optional
int dateToInt(const std::string& date);
std::string dateToString(int date);

//one tickers history, oldest first
struct Series {
    std::string file; //path it was read from
    std::string name; //"Data/Input/v_Data.csv" -> "v" (same rule as shortName in Backtest)
    std::vector<int> date; //YYYYMMDD as an int
    std::vector<double> open; //NaN where the files open is unusable (not positive, or outside that days low-high)
    std::vector<double> close;
    std::vector<double> lnClose; //ln(close) -> worked out once here, every pair using this ticker shares it
    size_t badOpens = 0; //how many opens were unusable -> a fill on such a bar falls back to its close
};

//reads a price csv (Date,Open,High,Low,Close,Volume). rows after the hold-out seal (Holdout.h) or after dataEnd (0 = no limit) are dropped here -> nothing downstream can see them
Series loadSeries(const std::string& file, int dataEnd = 0);

//2 tickers on their common dates -> usually just a window into the 2 Series (pointers, no copy), copied only when their calendars differ
struct PairData {
    std::string nameA, nameB;
    size_t n = 0; //common days
    const int* date = nullptr; //each of the 7 points at n values -> index t is the same day in all of them
    const double* openA = nullptr;
    const double* openB = nullptr;
    const double* closeA = nullptr;
    const double* closeB = nullptr;
    const double* lnA = nullptr;
    const double* lnB = nullptr;

    //only filled when the 2 calendars differed -> the pointers above then point into them
    std::vector<int> ownDate;
    std::vector<double> ownOpenA, ownOpenB, ownCloseA, ownCloseB, ownLnA, ownLnB;
};
//out may point into seriesA and seriesB -> they must outlive it (the Market below owns them)
void alignPair(const Series& seriesA, const Series& seriesB, PairData& out);

// one row of a pair list (label,fileA,fileB[,group]) as written by tools/make_pairs.py
struct PairSpec {
    std::string label, fileA, fileB, group;
};
std::vector<PairSpec> readPairList(const std::string& listFile);

// every ticker a pair list needs, each file parsed exactly once (in parallel)
class Market {
public:
    //loads every distinct file named in specs. threads = 0 -> one per core
    void load(const std::vector<PairSpec>& specs, int dataEnd, unsigned threads);
    const Series& series(const std::string& file) const;
    // data fix -> rows before firstDate go. call before  any alignPair (a PairData points into the series). returns how many went
    size_t dropBefore(const std::string& file, int firstDate);
    size_t tickers() const { return allSeries.size(); }
    size_t rows() const; //price rows parsed in total

private:
    std::vector<std::unique_ptr<Series>> allSeries; //one per distinct file
    std::unordered_map<std::string, size_t> indexByFile; // file path -> slot in allSeries
};

//hardware threads on this machine (at least 1) -> what threads = 0 means
unsigned hardwareThreads();
//work(index) for index 0..count-1 -> work must write only its own slot so the result is the same for any no of threads. first exception rethrown once all stop
void parallelFor(size_t count, unsigned threads, const std::function<void(size_t)>& work);
