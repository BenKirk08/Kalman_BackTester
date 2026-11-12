#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "Engine.h"

//walk-forward portfolio study (DECISIONS.md D9-D21) -> one event engine per pair, all stepped a date  at a time, one book per arm on top

// same pairs in every arm, the hedge differs -> K kalman, S OLS of the training window, KS kalman signal w/ OLS legs, SR rolling OLS, ST = S but z = spread / training sd
enum Arm { ARM_KALMAN = 0, ARM_STATIC, ARM_KALMAN_STATIC_LEGS, ARM_ROLLING_OLS, ARM_STATIC_TRAIN_Z, ARM_COUNT };
const char* armName(int arm); // "kalman", "static", "kalman_static_legs", "rolling_ols", "static_train_z"
const char* armLabel(int arm); //K, S, KS, SR, ST

struct ResearchConfig {
    //universe
    std::string pairList; //csv of label,fileA,fileB[,group]
    std::string membersFile = "tools/sp500_members.csv"; // ticker,dateAdded,sector
    std::string dataFixesFile = "tools/data_fixes.csv"; //bad vendor data found by tools/data_qa.py (D25). "" = none
    bool pointInTime = true; //a ticker only counts from its index-entry date (D10)
    std::vector<std::string> exclude = {"v_ma", "xom_cvx", "ko_pep"}; //pairs the trade rules were developed on (D11)
    bool swapLegs = false; //sensitivity -> A and B swapped

    //walk-forward
    int firstTestYear = 2008, lastTestYear = 2026;
    int windowMonths = 12; //12 or 6
    int trainYears = 3; //calendar years just before each test window
    size_t minTrainBars = 740; //fewer training bars -> not screened. a full 3 years is approx 756
    double selectMinHalfLife = 0.5, selectMaxHalfLife = 30.0; //half-life band, training OLS residual (trading days)
    double selectMargin = 1.0; //selected only if the johansen trace is at least this multiple of its critical value

    //per-pair strategy (inherited, the same in every arm)
    BacktestConfig base; //kalman Q/R, z window, half-life band, gates, trade rules
    bool halfLifeGate = true; //false -> entries need ONLY the johansen gate (sensitivity). the screen keeps its band
    double minBeta = 0.25, maxBeta = 4.0; // hedge-ratio guard (D14) -> no entry w/ a leg beta outside this  band
    size_t rollingOlsWindow = 750; //trailing bars the  SR arm refits its hedge on
    FillModel fill = FillModel::NextOpen; //order made at the close of bar t fills at the open of t+1 (D15)
    double rebase = 100.0; //both legs start at this price for the models (D13). 0 = raw log prices
    int banFrom = 20080919, banUntil = 20081008; //SEC short-sale ban (D16) -> no financials OPENED, first to last day
    int banSignalFrom = 20080918, banSignalUntil = 20081007; //next-open fills -> block the signals 1 sesison earlier

    //portfolio (D16, D17)
    double capital = 1000000.0; //dollars, fixed (no compoudning)
    double costPerExecution = 0.0005; // 5 bp of notional, each leg, each fill
    double borrowPerYear = 0.005; //50 bp a year on the short legs entry notional -> 1/252 of it per trading day held
    bool equalRisk = true; //false -> every trade the same gross notional
    double riskPerTrade = 0.0001; //daily P&L volatility each trade is sized to, as a share of capital
    double grossPerTrade = 0.02; //equal-notional sizing only -> gross per trade as a share of capital
    bool caps = true; //false -> none of the 3 limits below applies
    double maxTradeGross = 0.04, maxTickerGross = 0.10, maxBookGross = 2.0; // shares of capital

    //sessions in a row w/ no bar before the book closes what the pair holds (a guard, no ticker in the universe stops early). looks back ONLY -> a data cut never changes earlier days
    int strandAfterSessions = 10;

    bool arms[ARM_COUNT] = {true, true, true, true, true}; //which arms run, in Arm order
    std::string outPrefix; //empty -> nothing written
    unsigned threads = 0; //for reading the files
    int dataEnd = 0; //drop every bar after this date at load (0 = keep everything the seal allows) -> truncation checks
    int designUntil = 0; //design chosen on windows up to this date (0 = fixed in advance) -> ONLY splits report (D23)
    bool quiet = false; //write the files, print nothing abot performance
};

struct DailyRow {
    int date = 0;
    double pnl = 0; //net of costs and borrow
    double cost = 0, borrow = 0; //charged that day, already taken off pnl
    double gross = 0; //entry notional (at the fill prices, both legs) of the positions open at the end of the day
    int openCount = 0; //positions open at the end of the day
    double reserved = 0; // against the book limit -> open positions + orders approved today, at their decision closes
};

struct PortfolioTrade {
    int id = 0; //serial number within the arm, in order of entry
    int pair = 0; //index into labels and groups of the result
    int window = 0; //test window the entry signal  fell in
    int signalDate = 0, entryDate = 0, exitDate = 0; //exitDate 0 = still open at the end of the data
    int direction = 0; //+1 = long spread (long A short B), -1 = short spread
    double zEntry = 0, zExit = 0, beta = 0, sigma = 0; //beta = hedge ratio of the legs, sigma = daily spread volatility
    double scale = 0; //multipel of the engines fixed unit
    double gross = 0; //entry notional, both legs
    double grossPnl = 0, cost = 0, borrow = 0, netPnl = 0; //netPnl = grossPnl - cost - borrow
    int holdingDays = 0; // bars of the pair from entry fill to exit (trading days, not calendar)
    ExitReason reason = ExitReason::None;
};

struct PortfolioMetrics {
    size_t days = 0; //daily rows in the period, flat days included
    double pnl = 0; //net dollars
    double annualReturn = 0, annualVol = 0; //as a share of capital
    double sharpe = 0; //annualised
    double sharpeSE = 0; //lo (2002), IID returns -> sqrt((1 + SR^2 / (2 x 252)) / years) for an annualised daily sharpe
    double maxDrawdown = 0; //biggest fall below the running peak, as a share of capital (NOT % of the peak)
};

//the ONE sharpe definition (D20) -> daily net P&L / fixed capital over rows in [from, until], flat days included, x sqrt(252), no risk-free rate
PortfolioMetrics portfolioMetrics(const std::vector<DailyRow>& daily, double capital, int from, int until);

struct WindowRow {
    int from = 0, until = 0; //first day, and the day before the next window starts
    size_t alive = 0; //pairs that reach the window w/ some price history already (controls NOT counted)
    size_t members = 0; // ...whose 2 tickers were both in the index by then
    size_t eligible = 0; // ...with a full training  window
    size_t johansenPass = 0, selected = 0; //selected = johansen pass + half-life in band
    size_t controls = 0, controlsPass = 0; // control pairs screened / cointegrated (never traded)
};

//one day of one trades gross P&L -> lets the monte carlo rebuild the daily series trade by trade
struct TradeDay {
    int id = 0; //PortfolioTrade id, unique inside one arm
    int date = 0;
    double pnl = 0; //dollars, before costs and borrow
};

struct ArmResult {
    std::vector<DailyRow> daily; // one row per date of the run, flat days included
    std::vector<TradeDay> tradeDays;
    std::vector<PortfolioTrade> trades; //closed trades in the order they closed, then whatever is still open
    size_t orders = 0, approved = 0; //opening orders the strategies sent / the risk layer approved
    size_t approvedFills = 0; // approved orders that were filled (the last days orders never are) -> trade ids
    size_t refusedBookCap = 0, refusedTickerCap = 0, refusedNoRisk = 0; // ORDER-DAYS -> refused 5 days running counts 5
    size_t scaledByTradeCap = 0; // approved orders that were shrunk to the per-trade limit
    size_t betaRefusals = 0; //order-days the hedge-ratio guard stopped
    size_t stranded = 0; //positions closed because their pairs price history stopped before the data did
    size_t withdrawn = 0; //approved entries withdrawn because the pair failed the next windows screen before the fill
    double reconcile = 0; //sum of daily  P&L minus sum of trade P&L -> must be ~0
};

struct ResearchResult {
    std::vector<std::string> labels, groups; //per pair
    std::vector<WindowRow> windows; //one per test window
    ArmResult arm[ARM_COUNT];
    int firstDate = 0, lastDate = 0; //of the data
    int firstTestDate = 0; //first trading day of the first window
    size_t pairs = 0, controls = 0, excluded = 0; //index pairs run, control pairs, rule-development pairs left out
    size_t pairBars = 0; // bar events handled, once per pair (D24)
    size_t distinctEligible = 0, distinctSelected = 0; //pairs screened / selected in at least 1 window
    size_t closeFallbackFills = 0; //next-open fills that used the close because an open was unusable
    size_t screenErrors = 0; // pair-windows whose johansen test could not be computed (singular matrix) -> not selected
    size_t droppedPairs = 0; //pairs removed because a data fix dropped one of their tickers
    std::vector<std::string> fixesApplied; //what the data fixes file did, one line each
    std::vector<int> marketDates; //SPY dates and closes as loaded, for the market-exposure check
    std::vector<double> marketCloses;
    double seconds = 0; //wall-clock, pair setp + event loop + wrap-up. file reading and csv writing NOT in it
};

//the periods a runs results are reported over, in order. key = the name used in PREFIX_summary.csv
struct ReportPeriod {
    std::string key, label;
    int from = 0, until = 0; // both ends included
};
std::vector<ReportPeriod> reportPeriods(const ResearchConfig& config, const ResearchResult& result);

ResearchResult runResearch(const ResearchConfig& config);
void printResearch(const ResearchConfig& config, const ResearchResult& result);
void writeResearch(const ResearchConfig& config, const ResearchResult& result);

//"BackTester walkforward <pairList.csv> [options]" -> the options are read by parseWalkForwardArgs in Research.cpp
int runWalkForwardCommand(int argc, char* argv[]);
