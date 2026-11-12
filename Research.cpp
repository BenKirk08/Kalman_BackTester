//the walk-forward portfolio study -> the daily loop over every pair, the books and risk layer on top, the csv files and the console report
#include "Research.h"
#include "Holdout.h"
#include "Market.h"
#include "Screen.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

const char* armName(int arm) {
    switch (arm) {
        case ARM_KALMAN: return "kalman";
        case ARM_STATIC: return "static";
        case ARM_KALMAN_STATIC_LEGS: return "kalman_static_legs";
        case ARM_ROLLING_OLS: return "rolling_ols";
        case ARM_STATIC_TRAIN_Z: return "static_train_z";
        default: return "";
    }
}

//the short code -> accepted by --arms and printed in the console blocks
const char* armLabel(int arm) {
    switch (arm) {
        case ARM_KALMAN: return "K";
        case ARM_STATIC: return "S";
        case ARM_KALMAN_STATIC_LEGS: return "KS";
        case ARM_ROLLING_OLS: return "SR";
        case ARM_STATIC_TRAIN_Z: return "ST";
        default: return "";
    }
}

PortfolioMetrics portfolioMetrics(const std::vector<DailyRow>& daily, double capital, int from, int until) {
    PortfolioMetrics metrics;
    double sum = 0, sumSq = 0, equity = 0, peak = 0;
    for (const DailyRow& row : daily) {
        if (row.date < from || (until != 0 && row.date > until)) continue; //until 0 = no end date
        const double dayReturn = row.pnl / capital; // on FIXED capital -> no compounding
        metrics.days++;
        metrics.pnl += row.pnl;
        sum += dayReturn;
        sumSq += dayReturn * dayReturn;
        equity += dayReturn; //returns add up (not multiply) as capital is  fixed
        if (equity > peak) peak = equity; //peak starts at 0 -> a loss from day 1 alreay counts as drawdown
        if (peak - equity > metrics.maxDrawdown) metrics.maxDrawdown = peak - equity;
    }
    if (metrics.days < 2) return metrics; //n - 1 below needs 2 days
    const double n = (double)metrics.days;
    const double mean = sum / n;
    const double variance = (sumSq - n * mean * mean) / (n - 1.0); // sample variance from the running sums
    const double sd = variance > 0.0 ? std::sqrt(variance) : 0.0; //clamp small negative values (for sqrt)
    metrics.annualReturn = mean * 252.0;
    metrics.annualVol = sd * std::sqrt(252.0); //vol scales w/ sqrt t
    metrics.sharpe = sd > 0.0 ? mean / sd * std::sqrt(252.0) : 0.0;
    //lo (2002) for IID returns -> the daily sharpe has variance (1 + SR_daily^2 / 2) / days. annualised on both sides gives this
    metrics.sharpeSE = std::sqrt((1.0 + 0.5 * metrics.sharpe * metrics.sharpe / 252.0) / (n / 252.0));
    return metrics;
}

namespace {

struct Member {
    int dateAdded = 0;
    bool financial = false;
};

//tools/sp500_members.csv -> ticker,dateAdded,sector
std::unordered_map<std::string, Member> readMembers(const std::string& file) {
    std::unordered_map<std::string, Member> members;
    std::ifstream input(file);
    if (!input) throw std::runtime_error("couldnt open the index membership file: " + file);
    std::string line;
    std::getline(input, line); //header
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back(); //windows line ending
        if (line.empty()) continue;
        // no 1st comma -> npos + 1 wraps to 0, so the 2nd search  finds nothing either
        const size_t comma1 = line.find(','), comma2 = line.find(',', comma1 + 1);
        if (comma1 == std::string::npos || comma2 == std::string::npos)
            throw std::runtime_error("bad row in " + file + ": " + line);
        Member member;
        member.dateAdded = dateToInt(line.substr(comma1 + 1, comma2 - comma1 - 1));
        member.financial = line.substr(comma2 + 1) == "Financials"; //sector = the rest of the row
        members[line.substr(0, comma1)] = member;
    }
    return members;
}

struct DataFix {
    std::string ticker, action, reason; //action = start (history begins on date) or drop (ticker leaves the universe)
    int date = 0;
};

//tools/data_fixes.csv -> ticker,action,date,reason (# lines are comments)
std::vector<DataFix> readDataFixes(const std::string& file) {
    std::vector<DataFix> fixes;
    if (file.empty()) return fixes; // "" = run w/o any fixes
    std::ifstream input(file);
    if (!input) throw std::runtime_error("couldnt open the data fixes file: " + file);
    std::string line;
    std::getline(input, line); //header
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t comma1 = line.find(','), comma2 = line.find(',', comma1 + 1), comma3 = line.find(',', comma2 + 1);
        if (comma1 == std::string::npos || comma2 == std::string::npos || comma3 == std::string::npos) {
            throw std::runtime_error("bad row in " + file + ": " + line);
        }
        DataFix fix;
        fix.ticker = line.substr(0, comma1);
        fix.action = line.substr(comma1 + 1, comma2 - comma1 - 1);
        fix.date = dateToInt(line.substr(comma2 + 1, comma3 - comma2 - 1));
        fix.reason = line.substr(comma3 + 1); // the rest of the row, commas and all
        if (fix.action != "start" && fix.action != "drop") throw std::runtime_error("unknown data fix action: " + line);
        fixes.push_back(fix);
    }
    return fixes;
}

//one pair in one window -> a row of PREFIX_screen.csv
struct ScreenRow {
    int pair = 0, window = 0;
    size_t bars = 0; //training bars found
    bool member = false, enoughHistory = false, selected = false;
    PairScreen screen; //stys at its defaults when the pair was not tested
};

//one open position of one arm in one pair
struct Position {
    bool open = false;
    double scale = 0; //multiple of  the engines unit trade
    double reservedGross = 0, reservedA = 0, reservedB = 0; //what was counted against the limits when it was approved
    double shortNotional = 0; //entry notional of the short leg -> borrow accrues on it
    double unitSharesA = 0, unitSharesB = 0; // the engines share counts for its fixed unit (x scale = shares held)
    double grossPnl = 0, cost = 0, borrow = 0; //running totals in dollars
    size_t entryBar = 0; // bar of the pair the entry was filled on
    PortfolioTrade trade; // the record that ends up in the result
};

//an opening order waiting for the risk layer
struct Candidate {
    int pair = 0;
    size_t index = 0; // index in that pairs engine candidate list
    double absZ = 0; //how stretched -> the sort key
};

//what the risk layer decided for one opening order
struct OrderSize {
    double scale = 0.0, gross = 0.0; //gross = both legs in dollars
    bool approved = true, shrunk = false; //shrunk = cut down to the per-trade limit
};

//one arms whole book
struct Book {
    std::vector<Position> positions; //per pair
    std::vector<double> pendingScale, pendingGross, pendingA, pendingB; //approved today, filled at the next open
    std::vector<int> pendingWindow; //window the order was approved in
    std::vector<char> pendingActive; //an approved order is waiting for its fill
    std::vector<double> tickerGross; //per ticker id, dollars reserved acros all its pairs
    double reservedGross = 0; // counted  against the book limit (reservations)
    double openNotional = 0; // entry notional of the positions actually open
    int openCount = 0;
    double dayPnl = 0, dayCost = 0, dayBorrow = 0; //todays totals, back to 0 once the daily row is written
    std::vector<Candidate> candidates; //todays opening orders
};

//one pair -> its data, its engine and where it stands in the walk-forward
struct Slot {
    PairSpec spec;
    PairData data;
    size_t nextBar = 0; //index of the next bar to feed = bars fed so far
    std::unique_ptr<PairEngine> engine;
    int runOfArm[ARM_COUNT] = {-1, -1, -1, -1, -1}; //engine run of each arm, -1 = none
    std::vector<int> armOfRun; //the other way round -> fills and orders come back by run
    int staticSignal = -1, staticZSignal = -1; //-1 = not made yet
    int memberFrom = 0; //first date both tickers were index members (0 = not an index pair, e.g. a control)
    bool financial = false, control = false; //financial -> the 2008 short-sale ban applies
    int tickerA = 0, tickerB = 0; //ids into the tickerGross of a Book
    int window = -1; //test window the pair is in, -1 = none yet
    bool selected = false; //selected in the current window
    bool ended = false; //past the last window, every run sitched off
    bool everEligible = false, everSelected = false;
    size_t fillsHandled = 0; //fills of the current bar the books have already taekn in
    int openPositions = 0; //arms whose book holds this pair right now -> 0 lets applyFills skip a quiet bar
};

//the 3 numbers the daily loop asks EVERY pair for, kept out of the big Slot -> the 2 passes over all pairs each date stay in cache
struct BarClock {
    int nextDate = -1; //date of the next bar to feed, -1 = none left
    int lastDate = -1; // date of the last bar fed, -1 = none yet
    int sessionsWithoutBar = 0; //calendar sessions since this pair last had a bar
};

// "Data/Input/v_Data.csv" -> "v"
std::string tickerOf(const std::string& file) {
    const size_t slash = file.find_last_of('/');
    const std::string name = (slash == std::string::npos) ? file : file.substr(slash + 1);
    const size_t cut = name.find("_Data");
    return cut == std::string::npos ? name : name.substr(0, cut);
}

// adds dates to the sorted calendar. nearly every US ticker trades on the same days -> the usual case is nothing new and costs 1 pass
void mergeCalendar(std::vector<int>& calendar, const int* dates, size_t count) {
    size_t i = 0;
    bool subset = true;
    for (size_t j = 0; j < count; j++) {
        const int date = dates[j];
        while (i < calendar.size() && calendar[i] < date) i++; //both sorted -> i only ever moves forward
        if (i == calendar.size() || calendar[i] != date) { // a date the calendar has not got
            subset = false;
            break;
        }
    }
    if (subset) return;
    std::vector<int> merged;
    merged.reserve(calendar.size() + count);
    // something new ->  full merge, a date in both lists is kept once
    std::set_union(calendar.begin(), calendar.end(), dates, dates + count, std::back_inserter(merged));
    calendar.swap(merged);
}

const int DAYS_IN_MONTH[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; // [0] unused -> months count from 1

//february has 29 days in these years -> shared by dayBefore and dayAfter
bool isLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

// the calendar day before a date (20090101 -> 20081231)
int dayBefore(int date) {
    int year = date / 10000, month = (date / 100) % 100, day = date % 100; //yyyymmdd  pulled apart
    if (--day < 1) { //was the 1st -> last day of the month before
        if (--month < 1) { //was january -> december of the year before
            month = 12;
            year--;
        }
        day = (month == 2 && isLeapYear(year)) ? 29 : DAYS_IN_MONTH[month];
    }
    return year * 10000 + month * 100 + day;
}

//the  calendar day after a date (20151231 -> 20160101), so a period that starts "the day after" is a real date
int dayAfter(int date) {
    int year = date / 10000, month = (date / 100) % 100, day = date % 100;
    const int last = (month == 2 && isLeapYear(year)) ? 29 : DAYS_IN_MONTH[month]; // last day of this month
    if (++day > last) {
        day = 1;
        if (++month > 12) {
            month = 1;
            year++;
        }
    }
    return year * 10000 + month * 100 + day;
}

// copies the  running totals of a position into its trade record and works out the net
void copyTotalsToTrade(Position& position) {
    position.trade.grossPnl = position.grossPnl;
    position.trade.cost = position.cost;
    position.trade.borrow = position.borrow;
    position.trade.netPnl = position.grossPnl - position.cost - position.borrow;
}

//an approved order that will never be filled gives back what it had reserved
void releasePending(Book& book, const Slot& slot, size_t pairIndex) {
    book.reservedGross -= book.pendingGross[pairIndex];
    book.tickerGross[(size_t)slot.tickerA] -= book.pendingA[pairIndex];
    book.tickerGross[(size_t)slot.tickerB] -= book.pendingB[pairIndex];
    book.pendingActive[pairIndex] = 0;
}

//the whole run -> every pair, one book per arm and the steps of the daily loop
struct Study {
    const ResearchConfig& config;
    ResearchResult& result;
    std::vector<Slot> slots; // one per pair, in list order
    std::vector<BarClock> clocks; // one per pair, same order
    Book books[ARM_COUNT];
    std::vector<int> windowStarts; //first calendar day of each test window
    int endSentinel = 0; //first day after the last window
    std::vector<ScreenRow> screenRows; // every pair-window, in the order they were screened
    std::vector<double> trainA, trainB, scratch; //buffers every screen reuses
    std::unordered_map<std::string, int> tickerIds; //per price file -> index into the tickerGross of a Book
    StrategyConfig strategyFor(const Slot& slot) const;
    void addRun(Slot& slot, int arm, int signal);
    void screenTraining(const BarRing& history, size_t firstTrainBar, size_t trainBars, ScreenRow& row);
    void withdrawPending(Slot& slot, int pairIndex);
    void switchStaticArm(Slot& slot, int arm, int& signal, const OlsFit& ols, double fixedSd, bool selected,
                         bool wasSelected);
    void switchArms(Slot& slot, const OlsFit& ols, bool selected, bool wasSelected);
    void startWindow(Slot& slot, int pairIndex, int window);
    void openPosition(Slot& slot, int pairIndex, int arm, const Ticket& fill, double unitNotionalA,
                      double unitNotionalB);
    void recordExit(const Slot& slot, Book& book, Position& position, const Ticket& fill, double unitNotionalA,
                    double unitNotionalB) const;
    void accrueBorrow(Book& book, Position& position) const;
    void recordMark(int arm, int date, double pnl, Book& book, Position& position);
    void releasePosition(int arm, Book& book, Slot& slot, Position& position);
    void queueCandidates(const Slot& slot, int pairIndex);
    void applyFills(Slot& slot, int pairIndex, int date, bool marksToo);
    void strand(Slot& slot, int pairIndex, int date);
    void sortCandidates(Book& book) const;
    OrderSize sizeOrder(const Book& book, const Slot& slot, const Ticket& ticket, double unitNotionalA,
                        double unitNotionalB, ArmResult& armResult) const;
    void decide(int arm, int date);

    void buildWindows();
    EngineConfig buildEngineConfig() const;
    int tickerId(const std::string& file);
    void buildSlot(Slot& slot, const PairSpec& spec, const Market& market,
                   const std::unordered_map<std::string, Member>& members);
    void buildEngine(Slot& slot, const EngineConfig& engineConfig);
    std::vector<int> buildSlots(const std::vector<PairSpec>& specs, const Market& market,
                                const std::unordered_map<std::string, Member>& members);
    void sizeBooks();
    void feedBars(int date);
    void strandQuietPairs(int date);
    void closeDay(int date);
    void runDay(int date);
    void wrapUpArm(int arm);
    void wrapUp();
    void writeScreenCsv() const;

    Study(const ResearchConfig& researchConfig, ResearchResult& researchResult)
        : config(researchConfig), result(researchResult) {}
};

//the trade rules of one pair -> the inherited ones, the hedge-ratio guard and, for financials, the ban dates
StrategyConfig Study::strategyFor(const Slot& slot) const {
    StrategyConfig strategyConfig = legacyStrategyConfig(config.base);
    strategyConfig.tradeFrom = 0;
    strategyConfig.tradeUntil = 0; //the window logic switches a strategy on and off instead (setRunEnabled)
    strategyConfig.minBeta = config.minBeta;
    strategyConfig.maxBeta = config.maxBeta;
    strategyConfig.useHalfLifeGate = config.halfLifeGate;
    if (slot.financial) {
        //the blackout is tested on the signal day -> w/ next-open fills that is 1 session before the position opens
        const bool nextOpen = config.fill == FillModel::NextOpen;
        strategyConfig.noEntryFrom = nextOpen ? config.banSignalFrom : config.banFrom;
        strategyConfig.noEntryUntil = nextOpen ? config.banSignalUntil : config.banUntil;
    }
    return strategyConfig;
}

// a strategy instance for one arm of the pair, trading that signal
void Study::addRun(Slot& slot, int arm, int signal) {
    slot.runOfArm[arm] = slot.engine->addRun(signal, strategyFor(slot));
    slot.engine->setRunEnabled(slot.runOfArm[arm], false); //nothing trades until a window selects the pair
    if ((size_t)slot.runOfArm[arm] >= slot.armOfRun.size()) slot.armOfRun.resize((size_t)slot.runOfArm[arm] + 1, -1);
    slot.armOfRun[(size_t)slot.runOfArm[arm]] = arm; // the reverse lookpu, gron above to fit the new run
}

//the oldest remembered bar that belongs to the training window -> everthing from there on is  screened
size_t firstTrainingBar(const BarRing& history, int trainFrom) {
    const size_t barsSeen = history.size();
    size_t first = barsSeen;
    //walks back from the newest bar. bar s is still in the ring ONLY while s + capacity > barsSeen
    while (first > 0 && (first - 1) + history.capacity() > barsSeen && history.date(first - 1) >= trainFrom) first--;
    return first;
}

//the cointegration screen of one pair on its training bars, with the selection margin on top
void Study::screenTraining(const BarRing& history, size_t firstTrainBar, size_t trainBars, ScreenRow& row) {
    trainA.resize(trainBars);
    trainB.resize(trainBars);
    for (size_t k = 0; k < trainBars; k++) { //out of the ring into plain arrays, oldest first
        trainA[k] = history.lnA(firstTrainBar + k);
        trainB[k] = history.lnB(firstTrainBar + k);
    }
    try {
        row.screen = screenPair(trainA.data(), trainB.data(), trainBars, config.selectMinHalfLife,
                                config.selectMaxHalfLife, scratch);
        //a stricter cointegration bar than the 5% critical value (design candidate C3; 1.0 = the 5% test itself)
        if (!(row.screen.johansen.trace0 >= config.selectMargin * row.screen.johansen.criticalValue))
            row.screen.passed = false; //written as !(>=) -> a NaN trace fails too
    } catch (const std::runtime_error&) {
        //2 series so alike that the tests matrices are singular (possible for the controls) -> cant be tested, so not selected
        row.screen = PairScreen();
        result.screenErrors++;
    }
}

//adds one screen to the funnel counts of its window and marks the pair selected if it passed
void selectAndCount(Slot& slot, ScreenRow& row, WindowRow& windowRow, size_t barsSeen) {
    if (slot.control) { //counted on their own, never selected
        if (row.enoughHistory) {
            windowRow.controls++;
            if (row.screen.johansen.isCointegrated) windowRow.controlsPass++;
        }
    } else {
        //the funnel -> alive, members, eligible, cointegrated, selected
        if (barsSeen > 0) windowRow.alive++;
        if (barsSeen > 0 && row.member) windowRow.members++;
        if (row.member && row.enoughHistory) {
            windowRow.eligible++;
            slot.everEligible = true;
            if (row.screen.johansen.isCointegrated) windowRow.johansenPass++;
            if (row.screen.passed) {
                windowRow.selected++;
                row.selected = true;
                slot.everSelected = true;
            }
        }
    }
}

// a pair that failed the screen opens nothing new (D12) -> an entry approved on the old windows last session is taken back before todays open
void Study::withdrawPending(Slot& slot, int pairIndex) {
    PairEngine& engine = *slot.engine;
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        if (slot.runOfArm[arm] < 0 || !engine.cancelPendingEntry(slot.runOfArm[arm])) continue;
        Book& book = books[arm];
        const size_t slotIndex = (size_t)pairIndex;
        if (book.pendingActive[slotIndex]) { // the room it had reserved goes back
            releasePending(book, slot, slotIndex);
            result.arm[arm].withdrawn++;
        }
    }
}

//one of the 2 static arms for the new window -> a fresh OLS spread if the pair is on, a stopped signal if not
void Study::switchStaticArm(Slot& slot, int arm, int& signal, const OlsFit& ols, double fixedSd, bool selected,
                            bool wasSelected) {
    PairEngine& engine = *slot.engine;
    const size_t zWindow = config.base.zWindow;
    if (selected) {
        //this windows OLS spread,  its recent history rebuilt from these coefficients (never spliced w/ the previous windows)
        if (signal < 0) { //first window the pair is on -> the signal and its run are made now
            signal = engine.addStaticSignal(ols.alpha, ols.beta, zWindow, fixedSd);
            addRun(slot, arm, signal);
        } else engine.restartStaticSignal(signal, ols.alpha, ols.beta, zWindow, fixedSd);
        //back after a window out -> an old cool-off is about a spread that no longer exists, and w/ its signal off it could not clear by itself (the kalman arms can)
        if (!wasSelected) engine.clearCoolOff(slot.runOfArm[arm]);
        engine.setRunEnabled(slot.runOfArm[arm], true);
    } else if (slot.runOfArm[arm] >= 0) { //off this window, but it was on in an earlier one
        engine.setRunEnabled(slot.runOfArm[arm], false); //an open trade runs off on its frozen entry values
        engine.stopSignal(signal);
    }
}

// every arm of the pair goes on or off for the new window, the static ones on the OLS fit of this window
void Study::switchArms(Slot& slot, const OlsFit& ols, bool selected, bool wasSelected) {
    PairEngine& engine = *slot.engine;
    if (slot.runOfArm[ARM_KALMAN] >= 0) engine.setRunEnabled(slot.runOfArm[ARM_KALMAN], selected);
    if (slot.runOfArm[ARM_ROLLING_OLS] >= 0) engine.setRunEnabled(slot.runOfArm[ARM_ROLLING_OLS], selected);
    if (slot.runOfArm[ARM_KALMAN_STATIC_LEGS] >= 0) {
        //KS -> the kalman signal decides, the legs are built on this windows OLS beta
        if (selected) engine.setRunLegBeta(slot.runOfArm[ARM_KALMAN_STATIC_LEGS], true, ols.beta);
        engine.setRunEnabled(slot.runOfArm[ARM_KALMAN_STATIC_LEGS], selected);
    }
    //fixedSd 0.0 -> the usual rolling z
    if (config.arms[ARM_STATIC]) switchStaticArm(slot, ARM_STATIC, slot.staticSignal, ols, 0.0, selected, wasSelected);
    if (config.arms[ARM_STATIC_TRAIN_Z]) {
        const bool selectedZ = selected && ols.residualSd > 0.0; // z = spread / training sd -> needs sd above 0
        switchStaticArm(slot, ARM_STATIC_TRAIN_Z, slot.staticZSignal, ols, ols.residualSd, selectedZ, wasSelected);
    }
}

//a new test window starts for this pair -> screen it on the training bars the engine remembers (all dated before today), then switch every arm on or off
void Study::startWindow(Slot& slot, int pairIndex, int window) {
    slot.window = window;
    const PairEngine& engine = *slot.engine;
    const BarRing& history = engine.history();
    const int trainFrom = windowStarts[(size_t)window] - config.trainYears * 10000; //same day, trainYears earlier
    const size_t barsSeen = history.size();
    const size_t firstTrainBar = firstTrainingBar(history, trainFrom);
    const size_t trainBars = barsSeen - firstTrainBar; // todays bar is not in yet -> all dated before the window

    ScreenRow row;
    row.pair = pairIndex;
    row.window = window;
    row.bars = trainBars;
    //both tickers in the index before the window starts (D10). controls and --no-pit runs skip the test
    row.member = slot.control || !config.pointInTime || slot.memberFrom < windowStarts[(size_t)window];
    row.enoughHistory = trainBars >= config.minTrainBars;

    WindowRow& windowRow = result.windows[(size_t)window];
    if (row.member && row.enoughHistory) screenTraining(history, firstTrainBar, trainBars, row);
    selectAndCount(slot, row, windowRow, barsSeen);
    screenRows.push_back(row);

    //switch the arms. the pair set is the same for all of them, only the hedge differs
    const bool selected = row.selected;
    const bool wasSelected = slot.selected; //in the last window it was screened in
    slot.selected = selected;
    if (!selected) withdrawPending(slot, pairIndex);
    switchArms(slot, row.screen.ols, selected, wasSelected);
}

//an approved order was fillde -> the book opens the position at the size it reserved and pays the entry cost
void Study::openPosition(Slot& slot, int pairIndex, int arm, const Ticket& fill, double unitNotionalA,
                         double unitNotionalB) {
    const size_t slotIndex = (size_t)pairIndex;
    Book& book = books[arm];
    Position& position = book.positions[slotIndex];
    position = Position(); //wipe what the last trade left
    position.open = true;
    position.scale = book.pendingScale[slotIndex];
    position.reservedGross = book.pendingGross[slotIndex];
    position.reservedA = book.pendingA[slotIndex];
    position.reservedB = book.pendingB[slotIndex];
    position.entryBar = fill.fillBar;
    position.unitSharesA = std::abs(fill.sharesA); // sizes only -> strand prices the exit w/ them
    position.unitSharesB = std::abs(fill.sharesB);
    book.pendingActive[slotIndex] = 0;
    book.openNotional += position.scale * (unitNotionalA + unitNotionalB); //at the FILL prices, unlike the reservation
    position.cost = config.costPerExecution * position.scale * (unitNotionalA + unitNotionalB); //entry cost, both legs
    book.dayCost += position.cost;
    // long spread = long A, short B
    position.shortNotional = position.scale * (fill.direction > 0 ? unitNotionalB : unitNotionalA);
    position.trade.id = (int)result.arm[arm].approvedFills++; //ids run 0, 1, 2 ... in fill order
    position.trade.pair = pairIndex;
    position.trade.window = book.pendingWindow[slotIndex]; //window of the signal, NOT of the fill
    position.trade.signalDate = slot.data.date[fill.signalBar];
    position.trade.entryDate = slot.data.date[fill.fillBar];
    position.trade.direction = fill.direction;
    position.trade.zEntry = fill.z;
    position.trade.beta = fill.beta;
    position.trade.sigma = fill.sigma;
    position.trade.scale = position.scale;
    position.trade.gross = position.scale * (unitNotionalA + unitNotionalB);
    book.openCount++;
    slot.openPositions++;
}

//a closing fill -> the exit cost is charged today and the trade record gets its exit details
void Study::recordExit(const Slot& slot, Book& book, Position& position, const Ticket& fill, double unitNotionalA,
                       double unitNotionalB) const {
    const double cost = config.costPerExecution * position.scale * (unitNotionalA + unitNotionalB); //on exit notional
    position.cost += cost;
    book.dayCost += cost;
    position.trade.exitDate = slot.data.date[fill.fillBar];
    position.trade.zExit = fill.z;
    position.trade.reason = fill.reason;
    position.trade.holdingDays = (int)(fill.fillBar - position.entryBar); //fill to fill, in bars of the pair
}

// one night of borrow on the short leg of an open position
void Study::accrueBorrow(Book& book, Position& position) const {
    const double borrowFee = config.borrowPerYear / 252.0 * position.shortNotional; //per trading day,  ENTRY notional
    position.borrow += borrowFee;
    book.dayBorrow += borrowFee;
}

//one day of P&L of one position -> into the day total, the position total and the trade-day list
void Study::recordMark(int arm, int date, double pnl, Book& book, Position& position) {
    book.dayPnl += pnl;
    position.grossPnl += pnl;
    TradeDay tradeDay;
    tradeDay.id = position.trade.id;
    tradeDay.date = date;
    tradeDay.pnl = pnl;
    result.arm[arm].tradeDays.push_back(tradeDay);
}

// a position is gone -> its trade goes on the list and its share of the limits is given back
void Study::releasePosition(int arm, Book& book, Slot& slot, Position& position) {
    result.arm[arm].trades.push_back(position.trade);
    book.reservedGross -= position.reservedGross;
    book.tickerGross[(size_t)slot.tickerA] -= position.reservedA;
    book.tickerGross[(size_t)slot.tickerB] -= position.reservedB;
    book.openNotional -= position.trade.gross; //the entry notional openPosition added
    book.openCount--;
    slot.openPositions--;
    position.open = false;
}

// the opening orders this pair sent today go to the book of their arm, where the risk layer finds them
void Study::queueCandidates(const Slot& slot, int pairIndex) {
    const PairEngine& engine = *slot.engine;
    for (size_t k = 0; k < engine.candidateCount(); k++) {
        const Ticket& ticket = engine.candidate(k);
        Candidate candidate;
        candidate.pair = pairIndex;
        candidate.index = k;
        candidate.absZ = std::abs(ticket.z);
        books[slot.armOfRun[(size_t)ticket.run]].candidates.push_back(candidate); // the run says which arm
    }
}

//the books take in what the engine did on this bar -> fills (costs on the fill  day), todays marks, then borrow on what is still open.marksToo false = new fills ONLY
void Study::applyFills(Slot& slot, int pairIndex, int date, bool marksToo) {
    const PairEngine& engine = *slot.engine;
    const std::vector<Ticket>& fills = engine.fillsThisBar();
    const size_t slotIndex = (size_t)pairIndex;

    //the usual bar -> no new fill and no arm holds the pair, so no mark, no borrow, nothing closing. only new orders are left to pass on
    if (slot.fillsHandled == fills.size() && slot.openPositions == 0) {
        if (marksToo) queueCandidates(slot, pairIndex);
        return;
    }

    // per arm -> the size todays mark is multiplied by, and whether a fill closed or opened its position on this bar
    double markScale[ARM_COUNT];
    bool closing[ARM_COUNT], opened[ARM_COUNT];
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        markScale[arm] = (slot.runOfArm[arm] >= 0 && books[arm].positions[slotIndex].open)
                             ? books[arm].positions[slotIndex].scale
                             : 0.0;
        closing[arm] = false;
        opened[arm] = false;
    }

    for (; slot.fillsHandled < fills.size(); slot.fillsHandled++) { //from where the last call for this bar stopped
        const Ticket& fill = fills[slot.fillsHandled];
        const int arm = slot.armOfRun[(size_t)fill.run];
        Book& book = books[arm];
        Position& position = book.positions[slotIndex];
        //dollars of the engines unit trade at the FILL prices
        const double unitNotionalA = std::abs(fill.sharesA) * fill.fillPriceA,
                     unitNotionalB = std::abs(fill.sharesB) * fill.fillPriceB;
        // a fill for something the book no longer has (strand closed the position or cancelled the order) -> ignored
        if (fill.action == OrderAction::Open && !book.pendingActive[slotIndex]) continue;
        if (fill.action == OrderAction::Close && !position.open) continue;
        if (fill.action == OrderAction::Open) {
            openPosition(slot, pairIndex, arm, fill, unitNotionalA, unitNotionalB);
            markScale[arm] = position.scale; //opened on this bar -> its move from the fill to the close is marked too
            opened[arm] = true;
        } else {
            recordExit(slot, book, position, fill, unitNotionalA, unitNotionalB);
            closing[arm] = true;
        }
    }
    if (!marksToo) {
        // same-close entries approved a moment ago -> open over tonights close, so tonights borrow is due (the pass below arleady ran for this bar)
        for (int arm = 0; arm < ARM_COUNT; arm++) {
            if (!opened[arm]) continue;
            accrueBorrow(books[arm], books[arm].positions[slotIndex]);
        }
        return;
    }

    for (int arm = 0; arm < ARM_COUNT; arm++) {
        if (slot.runOfArm[arm] < 0) continue;
        Book& book = books[arm];
        Position& position = book.positions[slotIndex];
        const double mark = engine.markThisBar(slot.runOfArm[arm]); //todays P&L of the engines unit trade
        if (mark != 0.0 && markScale[arm] != 0.0) { //a mark of exactly 0 leaves no trade-day row
            const double pnl = markScale[arm] * mark;
            recordMark(arm, date, pnl, book, position);
        }
        if (closing[arm]) { //closed today -> no borrow tonight
            copyTotalsToTrade(position);
            releasePosition(arm, book, slot, position);
        } else if (position.open) {
            accrueBorrow(book, position);
        }
    }
    queueCandidates(slot, pairIndex);
}

// no bar for config.strandAfterSessions sessions -> cant be marked or closed, so what it  holds is closed at its last close, the exit cost charged today, its limits given back
void Study::strand(Slot& slot, int pairIndex, int date) {
    const size_t slotIndex = (size_t)pairIndex;
    const size_t lastBar = slot.nextBar - 1; // the last bar this pair has had
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        if (slot.runOfArm[arm] < 0) continue;
        Book& book = books[arm];
        Position& position = book.positions[slotIndex];
        if (position.open) {
            //shares held x the last close, both legs
            const double exitNotional = position.scale * (position.unitSharesA * slot.data.closeA[lastBar] +
                                                          position.unitSharesB * slot.data.closeB[lastBar]);
            const double cost = config.costPerExecution * exitNotional;
            position.cost += cost;
            book.dayCost += cost;
            position.trade.exitDate = date; // today, NOT the date of its last bar
            position.trade.reason = ExitReason::End;
            position.trade.holdingDays = (int)(lastBar - position.entryBar);
            copyTotalsToTrade(position);
            releasePosition(arm, book, slot, position);
            result.arm[arm].stranded++;
        }
        if (book.pendingActive[slotIndex]) { //approved on the pairs last bar -> there is no next open to fill it at
            releasePending(book, slot, slotIndex);
        }
        //the strategy must not go on playing out a trade the book no longer has -> if the bars come back it starts flat
        slot.engine->flattenRun(slot.runOfArm[arm]);
    }
}

//the order the risk layer takes the orders in -> most stretched first, the pair label settles a tie
void Study::sortCandidates(Book& book) const {
    std::sort(book.candidates.begin(), book.candidates.end(), [this](const Candidate& left, const Candidate& right) {
        if (left.absZ != right.absZ) return left.absZ > right.absZ;
        return slots[(size_t)left.pair].spec.label < slots[(size_t)right.pair].spec.label;
    });
}

// how big  one opening order may be -> sized, shrunk to the per-trade limit, refused if the book or a ticker is full
OrderSize Study::sizeOrder(const Book& book, const Slot& slot, const Ticket& ticket, double unitNotionalA,
                           double unitNotionalB, ArmResult& armResult) const {
    const double capital = config.capital;
    //equal risk -> leg A notional = (risk  budget) / (the spreads daily volatility)
    double scale = 0.0;
    bool approved = true;
    if (config.equalRisk) {
        if (ticket.sigma > 0.0) scale = config.riskPerTrade * capital / (unitNotionalA * ticket.sigma);
        else { //no volatility estimate -> cant be sized
            approved = false;
            armResult.refusedNoRisk++;
        }
    } else {
        scale = config.grossPerTrade * capital / (unitNotionalA + unitNotionalB); //same dollars every trade
    }
    double gross = scale * (unitNotionalA + unitNotionalB); //both legs, at the closes the order was decided on
    bool shrunk = false;
    if (approved && config.caps) {
        if (gross > config.maxTradeGross * capital) { //one  trade may not be bigger than this -> shrink it to fit
            scale *= config.maxTradeGross * capital / gross;
            gross = config.maxTradeGross * capital;
            shrunk = true;
        }
        //shrunk trades land EXACTLY on round fractions of capital and can add up to exaclty a limit ->  1e-9 of slack so "at the limit" passes whicheevr way the last bit rounds
        const double slack = 1.0 + 1e-9;
        if (book.reservedGross + gross > config.maxBookGross * capital * slack) {
            approved = false; //book full -> refused, NOT shrunk
            armResult.refusedBookCap++;
        } else if (book.tickerGross[(size_t)slot.tickerA] + scale * unitNotionalA >
                       config.maxTickerGross * capital * slack ||
                   book.tickerGross[(size_t)slot.tickerB] + scale * unitNotionalB >
                       config.maxTickerGross * capital * slack) {
            approved = false;
            armResult.refusedTickerCap++; //either ticker full across all its pairs
        }
    }
    return {scale, gross, approved, shrunk};
}

//an approved order takes its share of the limits at once (laetr orders today already see it) and waits in the book for its fill
void reserveOrder(Book& book, const Slot& slot, size_t pairIndex, double scale, double gross, double unitNotionalA,
                  double unitNotionalB) {
    book.reservedGross += gross;
    book.tickerGross[(size_t)slot.tickerA] += scale * unitNotionalA;
    book.tickerGross[(size_t)slot.tickerB] += scale * unitNotionalB;
    book.pendingScale[pairIndex] = scale;
    book.pendingGross[pairIndex] = gross;
    book.pendingA[pairIndex] = scale * unitNotionalA; //kept per order -> exactly this  much is given back later
    book.pendingB[pairIndex] = scale * unitNotionalB;
    book.pendingWindow[pairIndex] = slot.window; //window of the deicsion day
    book.pendingActive[pairIndex] = 1;
}

// the risk layer for one arm, once every pair has seen todays bar -> todays opening orders, most stretched first
void Study::decide(int arm, int date) {
    Book& book = books[arm];
    if (book.candidates.empty()) return;
    ArmResult& armResult = result.arm[arm];
    sortCandidates(book);
    for (const Candidate& candidate : book.candidates) {
        Slot& slot = slots[(size_t)candidate.pair];
        const Ticket& ticket = slot.engine->candidate(candidate.index);
        const size_t pairIndex = (size_t)candidate.pair;
        armResult.orders++; //every order-day counts, approved or not
        const double unitNotionalA = std::abs(ticket.sharesA) * ticket.refPriceA,
                     unitNotionalB = std::abs(ticket.sharesB) * ticket.refPriceB; //at the decision closes

        const OrderSize size = sizeOrder(book, slot, ticket, unitNotionalA, unitNotionalB, armResult);
        if (size.approved) {
            reserveOrder(book, slot, pairIndex, size.scale, size.gross, unitNotionalA, unitNotionalB);
            armResult.approved++;
            if (size.shrunk) armResult.scaledByTradeCap++;
        }
        slot.engine->resolve(candidate.index, size.approved); //the strategy hears the verdict either way
        if (size.approved && config.fill == FillModel::SameClose)
            applyFills(slot, candidate.pair, date, false); //filled at this close already
    }
    book.candidates.clear();
}

//the pair list without the pairs the trade rules were developed on
std::vector<PairSpec> withoutExcluded(const std::vector<PairSpec>& listedSpecs, const ResearchConfig& config,
                                      ResearchResult& result) {
    std::vector<PairSpec> specs;
    for (const PairSpec& spec : listedSpecs) {
        //a pair is the same pair whichever ticker is written first -> the list has  ma_v and cvx_xom, not v_ma and xom_cvx
        const std::string tickerA = tickerOf(spec.fileA), tickerB = tickerOf(spec.fileB);
        bool excluded = false;
        for (const std::string& name : config.exclude)
            excluded = excluded || name == tickerA + "_" + tickerB || name == tickerB + "_" + tickerA;
        if (excluded) {
            result.excluded++;
            continue;
        }
        specs.push_back(spec);
    }
    return specs;
}

//the pair list without the pairs that lose  a ticker to a data fix
std::vector<PairSpec> withoutDropped(const std::vector<PairSpec>& specs, const std::vector<DataFix>& fixes,
                                     ResearchResult& result) {
    std::vector<PairSpec> kept;
    for (const PairSpec& spec : specs) {
        bool dropped = false;
        for (const DataFix& fix : fixes) {
            if (fix.action == "drop" && (tickerOf(spec.fileA) == fix.ticker || tickerOf(spec.fileB) == fix.ticker))
                dropped = true;
        }
        if (dropped) result.droppedPairs++;
        else kept.push_back(spec);
    }
    return kept;
}

//cuts the history the data fixes call bad and keeps one line per fix for the console
void applyDataFixes(Market& market, const std::vector<PairSpec>& specs, const std::vector<DataFix>& fixes,
                    ResearchResult& result) {
    for (const DataFix& fix : fixes) {
        if (fix.action == "drop") { // its pairs are already gone (withoutDropped) -> only the note is left
            result.fixesApplied.push_back(fix.ticker + ": dropped from the universe (" + fix.reason + ")");
            continue;
        }
        for (const PairSpec& spec : specs) {
            for (const std::string* file : {&spec.fileA, &spec.fileB}) {
                if (tickerOf(*file) != fix.ticker) continue;
                //0 when another pair already cut this file -> the note is written once
                const size_t droppedBars = market.dropBefore(*file, fix.date);
                if (droppedBars)
                    result.fixesApplied.push_back(fix.ticker + ": history starts " + dateToString(fix.date) + ", " +
                                                  std::to_string(droppedBars) + " earlier bars dropped (" + fix.reason +
                                                  ")");
            }
        }
    }
}

// keeps a copy of the market index closes in the result, for the files written latr
void copyMarketSeries(const Market& market, const std::vector<PairSpec>& specs, ResearchResult& result) {
    for (const PairSpec& spec : specs) {
        for (const std::string* file : {&spec.fileA, &spec.fileB}) {
            if (tickerOf(*file) == "spy" && result.marketDates.empty()) { //the first pair that has it is enough
                const Series& spy = market.series(*file);
                result.marketDates = spy.date;
                result.marketCloses = spy.close;
            }
        }
    }
}

//the test windows -> where each starts, where the last one ends and an empty result row for each
void Study::buildWindows() {
    if (config.windowMonths != 12 && config.windowMonths != 6)
        throw std::runtime_error("window length must be 6 or 12 months");
    for (int year = config.firstTestYear; year <= config.lastTestYear; year++) {
        windowStarts.push_back(year * 10000 + 101); //1 january as yyyymmdd
        if (config.windowMonths == 6) windowStarts.push_back(year * 10000 + 701); //and 1 july
    }
    endSentinel = (config.lastTestYear + 1) * 10000 + 101; //1 january after the last test year
    result.windows.resize(windowStarts.size());
    for (size_t window = 0; window < windowStarts.size(); window++) {
        result.windows[window].from = windowStarts[window];
        //ends the day before the next window starts, the last one the day before the sentinel
        result.windows[window].until =
            dayBefore(window + 1 < windowStarts.size() ? windowStarts[window + 1] : endSentinel);
    }
}

//the settings every pair engine is built with
EngineConfig Study::buildEngineConfig() const {
    EngineConfig engineConfig;
    engineConfig.gates = config.base.gates;
    engineConfig.fill = config.fill;
    engineConfig.rebase = config.rebase;
    engineConfig.forceCloseAtEnd = false; // positions stay open at the last bar -> no day depends on a later one
    engineConfig.legacyNanSkip = false; //an open position is still marekd on a bar w/ no z
    engineConfig.keepDaily = false; //the books  keep the daily P&L -> the engines own series isnt needed

    //must hold a whole training window -> at most 262 weekdays in a  year, plus some spare
    engineConfig.historyBars = (size_t)config.trainYears * 262 + 16;
    return engineConfig;
}

//a small number for each price file, handed out in the order the files are first seen
int Study::tickerId(const std::string& file) {
    const auto found = tickerIds.find(file);
    if (found != tickerIds.end()) return found->second;
    const int id = (int)tickerIds.size(); //next free id = how many are known so far
    tickerIds[file] = id;
    return id;
}

//lines one pair up on its 2 price series and notes what it is -> control or index pair, tickers, sector
void Study::buildSlot(Slot& slot, const PairSpec& spec, const Market& market,
                      const std::unordered_map<std::string, Member>& members) {
    slot.spec = spec;
    const Series& seriesA = market.series(config.swapLegs ? spec.fileB : spec.fileA); //swapped run -> B is leg A
    const Series& seriesB = market.series(config.swapLegs ? spec.fileA : spec.fileB);
    alignPair(seriesA, seriesB, slot.data); // ONLY the days both tickers have
    slot.control = spec.group.rfind("ctl_", 0) == 0; // rfind at 0 = starts w/ ctl_
    slot.tickerA = tickerId(seriesA.file);
    slot.tickerB = tickerId(seriesB.file);
    if (!slot.control) {
        const auto memberA = members.find(tickerOf(seriesA.file)), memberB = members.find(tickerOf(seriesB.file));
        if (memberA == members.end() || memberB == members.end()) {
            throw std::runtime_error("pair " + spec.label + " has a ticker with no index-entry date in " +
                                     config.membersFile);
        }
        slot.memberFrom = std::max(memberA->second.dateAdded, memberB->second.dateAdded); //later of the 2 entry dates
        slot.financial = memberA->second.financial || memberB->second.financial; //1 financial leg is enough
        result.pairs++;
    } else {
        result.controls++;
    }
    result.labels.push_back(spec.label);
    result.groups.push_back(spec.group);
}

//the engine of one pair, with the arms that must see every bar already attached
void Study::buildEngine(Slot& slot, const EngineConfig& engineConfig) {
    slot.engine = std::make_unique<PairEngine>(engineConfig); //controls get one too -> the screen reads its bar history
    if (!slot.control) {
        //kalman and rolling OLS have to see every bar, so they exist from the start. the static signals are made when a window first selects the pair
        if (config.arms[ARM_KALMAN] || config.arms[ARM_KALMAN_STATIC_LEGS]) { //K and KS share 1 kalman signal
            const int kalmanSignal = slot.engine->addKalmanSignal(config.base.kalman, config.base.zWindow);
            if (config.arms[ARM_KALMAN]) addRun(slot, ARM_KALMAN, kalmanSignal);
            if (config.arms[ARM_KALMAN_STATIC_LEGS]) addRun(slot, ARM_KALMAN_STATIC_LEGS, kalmanSignal);
        }
        if (config.arms[ARM_ROLLING_OLS]) {
            const int rollingSignal = slot.engine->addRollingOlsSignal(config.rollingOlsWindow, config.base.zWindow);
            addRun(slot, ARM_ROLLING_OLS, rollingSignal);
        }
    }
}

//sets up every pair in list order and returns the calendar -> every date on which any pair has a bar
std::vector<int> Study::buildSlots(const std::vector<PairSpec>& specs, const Market& market,
                                   const std::unordered_map<std::string, Member>& members) {
    const EngineConfig engineConfig = buildEngineConfig();
    std::vector<int> calendar;
    slots.resize(specs.size());
    clocks.resize(specs.size());
    for (size_t k = 0; k < specs.size(); k++) {
        Slot& slot = slots[k];
        buildSlot(slot, specs[k], market, members);
        buildEngine(slot, engineConfig);
        if (slot.data.n) { //no common days -> nothing to add, nothing to feed
            clocks[k].nextDate = slot.data.date[0];
            mergeCalendar(calendar, slot.data.date, slot.data.n);
        }
    }
    return calendar;
}

//every book starts empty, with room for each pair and each ticker
void Study::sizeBooks() {
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        Book& book = books[arm];
        book.positions.resize(slots.size());
        book.pendingScale.assign(slots.size(), 0.0);
        book.pendingGross.assign(slots.size(), 0.0);
        book.pendingA.assign(slots.size(), 0.0);
        book.pendingB.assign(slots.size(), 0.0);
        book.pendingWindow.assign(slots.size(), 0);
        book.pendingActive.assign(slots.size(), 0);
        book.tickerGross.assign(tickerIds.size(), 0.0); //per ticker, NOT per pair
    }
}

// every pair with a bar today takes it, screened first if a new test window starts for it
void Study::feedBars(int date) {
    const int windowCount = (int)windowStarts.size();
    for (size_t k = 0; k < slots.size(); k++) {
        if (clocks[k].nextDate != date) continue; //no bar today
        Slot& slot = slots[k];

        // a new test window? screen on the bars seen so far, BEFORE todays bar goes in
        int window = slot.window;
        while (window + 1 < windowCount && date >= windowStarts[(size_t)window + 1]) window++; //window -1  -> index 0
        if (window != slot.window) startWindow(slot, (int)k, window); //a long gap -> ONLY the newest one is screened
        if (!slot.ended && date >= endSentinel) { //past the last window -> nothing new opens
            for (int arm = 0; arm < ARM_COUNT; arm++)
                if (slot.runOfArm[arm] >= 0) slot.engine->setRunEnabled(slot.runOfArm[arm], false);
            slot.ended = true;
        }

        // everything the bar triggers happens in here, except opening orders -> thsoe wait for decide
        slot.engine->onBar(barAt(slot.data, slot.nextBar));
        slot.nextBar++;
        clocks[k].lastDate = date;
        clocks[k].nextDate = slot.nextBar < slot.data.n ? slot.data.date[slot.nextBar] : -1;
        slot.fillsHandled = 0; //the engine starts a fresh fill list every bar
        result.pairBars++;
        applyFills(slot, (int)k, date, true);
    }
}

// counts the sessions  each  pair has missed and strands the ones that raech the limit today
void Study::strandQuietPairs(int date) {
    for (size_t k = 0; k < slots.size(); k++) {
        BarClock& clock = clocks[k];
        if (clock.lastDate < 0) continue; //not started yet
        if (clock.lastDate == date) { //had a bar today -> the count starts again
            clock.sessionsWithoutBar = 0;
            continue;
        }
        //== and not >= -> stranded once, on the day the count reaches the limit
        if (++clock.sessionsWithoutBar == config.strandAfterSessions) strand(slots[k], (int)k, date);
    }
}

//one daily row per arm from what its book took in today, then the day totals start again from zero
void Study::closeDay(int date) {
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        if (!config.arms[arm]) continue;
        Book& book = books[arm];
        DailyRow row;
        row.date = date;
        row.cost = book.dayCost;
        row.borrow = book.dayBorrow;
        row.pnl = book.dayPnl - book.dayCost - book.dayBorrow;
        row.gross = book.openNotional; //positions actually open, pending orders NOT in it
        row.openCount = book.openCount;
        row.reserved = book.reservedGross; //what the book limit was tested against
        result.arm[arm].daily.push_back(row);
        book.dayPnl = book.dayCost = book.dayBorrow = 0.0;
    }
}

// one date of the study -> bars to every pair, then each arm decides, then stale pairs, then the daily rows
void Study::runDay(int date) {
    feedBars(date);
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        if (config.arms[arm]) decide(arm, date);
    }
    strandQuietPairs(date);
    closeDay(date);
    //the first date on or after the start of the first window, set once
    if (result.firstTestDate == 0 && date >= windowStarts.front()) result.firstTestDate = date;
}

//lists what one  arm still holds, checks its 2 P&L totals against each other, counts the hedge-ratio refusals
void Study::wrapUpArm(int arm) {
    ArmResult& armResult = result.arm[arm];
    double tradeSum = 0;
    for (const PortfolioTrade& trade : armResult.trades) tradeSum += trade.netPnl;
    for (size_t k = 0; k < slots.size(); k++) {
        Position& position = books[arm].positions[k];
        if (!position.open) continue;
        copyTotalsToTrade(position);
        //still open -> held up to the pairs last bar, exitDate stays 0
        position.trade.holdingDays = (int)(slots[k].data.n - 1 - position.entryBar);
        tradeSum += position.trade.netPnl;
        armResult.trades.push_back(position.trade);
    }
    double dailySum = 0;
    for (const DailyRow& row : armResult.daily) dailySum += row.pnl;
    armResult.reconcile = dailySum - tradeSum; // check -> approx 0, the same P&L added up 2 ways
    for (const Slot& slot : slots) {
        if (slot.runOfArm[arm] >= 0) armResult.betaRefusals += slot.engine->betaRefusals(slot.runOfArm[arm]);
    }
}

//after the last date -> every arm is closed off, then the counts that need a pass over all the pairs
void Study::wrapUp() {
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        if (config.arms[arm]) wrapUpArm(arm);
    }
    for (const Slot& slot : slots) {
        if (slot.everEligible) result.distinctEligible++;
        if (slot.everSelected) result.distinctSelected++;
        result.closeFallbackFills += slot.engine->closeFallbackFills;
    }
}

//opens one  output file, or stops the run if it cant be written
std::ofstream openCsv(const std::string& path) {
    std::ofstream file(path);
    if (!file) throw std::runtime_error("couldnt write " + path);
    return file;
}

//the screen rows live in the study and not in the result, so their file is written from here
void Study::writeScreenCsv() const {
    if (!config.outPrefix.empty()) {
        std::ofstream screenFile = openCsv(config.outPrefix + "_screen.csv");
        screenFile
            << "pair,group,windowFrom,trainBars,member,enoughHistory,trace,critical,cointegrated,halfLife,alpha,beta,residSd,selected\n"
            << std::setprecision(10);
        for (const ScreenRow& row : screenRows) {
            const bool tested = row.member && row.enoughHistory; //the others have no statistics to write
            screenFile << result.labels[(size_t)row.pair] << "," << result.groups[(size_t)row.pair] << ","
                       << dateToString(windowStarts[(size_t)row.window]) << "," << row.bars << ","
                       << (row.member ? 1 : 0) << "," << (row.enoughHistory ? 1 : 0) << ",";
            if (tested) {
                screenFile << row.screen.johansen.trace0 << "," << row.screen.johansen.criticalValue << ","
                           << (row.screen.johansen.isCointegrated ? 1 : 0) << "," << row.screen.residualOU.halfLife
                           << "," << row.screen.ols.alpha << "," << row.screen.ols.beta << ","
                           << row.screen.ols.residualSd;
            } else {
                screenFile << ",,,,,,"; //6 commas = the 7 statistics left empty
            }
            screenFile << "," << (row.selected ? 1 : 0) << "\n";
        }
    }
}

} //namespace

ResearchResult runResearch(const ResearchConfig& config) {
    ResearchResult result;
    Study study(config, result);

    //the universe
    const std::vector<PairSpec> listedSpecs = readPairList(config.pairList);
    const std::unordered_map<std::string, Member> members = readMembers(config.membersFile);
    const std::vector<PairSpec> allowed = withoutExcluded(listedSpecs, config, result);
    //bad vendor data (DECISIONS.md D25) -> a dropped ticker takes its pairs with it, a "start" fix cuts history
    const std::vector<DataFix> fixes = readDataFixes(config.dataFixesFile);
    const std::vector<PairSpec> specs = withoutDropped(allowed, fixes, result);
    Market market;
    market.load(specs, config.dataEnd, config.threads);
    applyDataFixes(market, specs, fixes, result);
    //SPY for the market-exposure check (it is in the pair list as a control; read through the same sealed loader)
    copyMarketSeries(market, specs, result);

    const auto startTime = std::chrono::steady_clock::now(); //the clock starts w/ the data already in memory

    study.buildWindows();
    //one engine per pair
    const std::vector<int> calendar = study.buildSlots(specs, market, members);
    study.sizeBooks();
    if (calendar.empty()) throw std::runtime_error("no price data for any pair in " + config.pairList);
    result.firstDate = calendar.front();
    result.lastDate = calendar.back();

    //the event loop over dates
    for (const int date : calendar) study.runDay(date);

    //wrap up -> positions still open are listed after the closed trades (marked to the last close, exit not charged)
    study.wrapUp();
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();

    study.writeScreenCsv();
    return result;
}

//which slices of the record a run is reported over -> development, or design + validation, then hold-out and the totals
std::vector<ReportPeriod> reportPeriods(const ResearchConfig& config, const ResearchResult& result) {
    std::vector<ReportPeriod> periods;
    const int first = result.firstTestDate, last = result.lastDate;
    const bool holdout = last > HOLDOUT_LAST_DEV_DATE; //the data reaches past the seal date
    const int developmentEnd = holdout ? HOLDOUT_LAST_DEV_DATE : last;
    //date / 10000 = the year -> labels like 2008-2015
    auto years = [](int from, int until) { return std::to_string(from / 10000) + "-" + std::to_string(until / 10000); };
    //afterDesign is junk when designUntil is 0 -> ONLY read where a design period exists
    const int afterDesign = dayAfter(config.designUntil), afterDevelopment = dayAfter(HOLDOUT_LAST_DEV_DATE);
    if (config.designUntil == 0 || config.designUntil >= last) { //no design date, or none of the data is past it
        periods.push_back({"development", "development " + years(first, developmentEnd), first, developmentEnd});
    } else {
        //the design was picekd on the first part, so only what  coms after it counts as out of sample
        periods.push_back({"design", "design windows " + years(first, config.designUntil), first, config.designUntil});
        periods.push_back(
            {"validation", "validation " + years(afterDesign, developmentEnd), afterDesign, developmentEnd});
    }
    if (holdout) {
        periods.push_back({"holdout", "HOLD-OUT " + years(afterDevelopment, last), afterDevelopment, last});
        if (config.designUntil != 0 && config.designUntil < last) {
            //validation + hold-out in one
            periods.push_back({"oos", "out of sample " + years(afterDesign, last), afterDesign, last});
        }
        periods.push_back({"full", "full record " + years(first, last), first, last});
    }
    return periods;
}

namespace {

const int FULL_PRECISION = 17; //enough digits for  python to read back the exact doubles

//the daily rows of every arm, from the first test day on
void writeDailyCsv(const ResearchConfig& config, const ResearchResult& result) {
    std::ofstream daily = openCsv(config.outPrefix + "_daily.csv");
    daily << "arm,date,pnl,cost,borrow,gross,open,reserved\n" << std::setprecision(FULL_PRECISION);
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        for (const DailyRow& row : result.arm[arm].daily) {
            if (row.date < result.firstTestDate) continue; //nothing can trade before the first window
            daily << armName(arm) << "," << dateToString(row.date) << "," << row.pnl << "," << row.cost << ","
                  << row.borrow << "," << row.gross << "," << row.openCount << "," << row.reserved << "\n";
        }
    }
}

//one row per trade of every arm, closed ones first
void writeTradesCsv(const ResearchConfig& config, const ResearchResult& result) {
    std::ofstream trades = openCsv(config.outPrefix + "_trades.csv");
    trades << "arm,id,pair,group,window,signalDate,entryDate,exitDate,direction,zEntry,zExit,beta,sigma,scale,gross,"
              "grossPnl,cost,borrow,netPnl,holdingDays,exitReason\n"
           << std::setprecision(FULL_PRECISION);
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        for (const PortfolioTrade& trade : result.arm[arm].trades) {
            trades << armName(arm) << "," << trade.id << "," << result.labels[(size_t)trade.pair] << ","
                   << result.groups[(size_t)trade.pair] << ","
                   << dateToString(result.windows[(size_t)trade.window].from) << "," << dateToString(trade.signalDate)
                   << "," << dateToString(trade.entryDate) << "," << dateToString(trade.exitDate) << ","
                   << trade.direction << "," << trade.zEntry << "," << trade.zExit << "," << trade.beta << ","
                   << trade.sigma << "," << trade.scale << "," << trade.gross << "," << trade.grossPnl << ","
                   << trade.cost << "," << trade.borrow << "," << trade.netPnl << "," << trade.holdingDays << ","
                   << (trade.exitDate ? exitReasonName(trade.reason) : "open") << "\n"; //no exit date = still open
        }
    }
}

//the screening funnel of each window, with the P&L and the entries of each arm in it
void writeWindowsCsv(const ResearchConfig& config, const ResearchResult& result) {
    std::ofstream windows = openCsv(config.outPrefix + "_windows.csv");
    windows << "from,until,alive,members,eligible,johansenPass,selected,controls,controlsPass";
    for (int arm = 0; arm < ARM_COUNT; arm++)
        if (config.arms[arm]) windows << "," << armName(arm) << "_pnl," << armName(arm) << "_entries";
    windows << "\n" << std::setprecision(FULL_PRECISION);
    for (size_t window = 0; window < result.windows.size(); window++) {
        const WindowRow& windowRow = result.windows[window];
        windows << dateToString(windowRow.from) << "," << dateToString(windowRow.until) << "," << windowRow.alive << ","
                << windowRow.members << "," << windowRow.eligible << "," << windowRow.johansenPass << ","
                << windowRow.selected << "," << windowRow.controls << "," << windowRow.controlsPass;
        for (int arm = 0; arm < ARM_COUNT; arm++) {
            if (!config.arms[arm]) continue;
            double pnl = 0;
            for (const DailyRow& row : result.arm[arm].daily)
                if (row.date >= windowRow.from && row.date <= windowRow.until) pnl += row.pnl; //by the date of the day
            size_t entries = 0;
            for (const PortfolioTrade& trade : result.arm[arm].trades)
                if (trade.window == (int)window) entries++; //by the window of the entry SIGNAL
            windows << "," << pnl << "," << entries;
        }
        windows << "\n";
    }
}

//each trades gross P&L day by day -> the null test of the monte carlo flips whole clusters of trades
void writeTradePnlCsv(const ResearchConfig& config, const ResearchResult& result) {
    std::ofstream tradeDays = openCsv(config.outPrefix + "_tradepnl.csv");
    tradeDays << "arm,id,date,pnl\n" << std::setprecision(FULL_PRECISION);
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        for (const TradeDay& tradeDay : result.arm[arm].tradeDays) {
            tradeDays << armName(arm) << "," << tradeDay.id << "," << dateToString(tradeDay.date) << "," << tradeDay.pnl
                      << "\n";
        }
    }
}

// the c++ headline numbers per arm and report period -> python must rebuild these before any simulation result is trusted
void writeSummaryCsv(const ResearchConfig& config, const ResearchResult& result) {
    std::ofstream summary = openCsv(config.outPrefix + "_summary.csv");
    summary << "arm,period,from,until,days,pnl,annualReturn,annualVol,sharpe,sharpeSE,maxDrawdown,capital\n"
            << std::setprecision(FULL_PRECISION);
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        if (!config.arms[arm]) continue;
        for (const ReportPeriod& period : reportPeriods(config, result)) {
            const PortfolioMetrics metrics =
                portfolioMetrics(result.arm[arm].daily, config.capital, period.from, period.until);
            summary << armName(arm) << "," << period.key << "," << dateToString(period.from) << ","
                    << dateToString(period.until) << "," << metrics.days << "," << metrics.pnl << ","
                    << metrics.annualReturn << "," << metrics.annualVol << "," << metrics.sharpe << ","
                    << metrics.sharpeSE << "," << metrics.maxDrawdown << "," << config.capital << "\n";
        }
    }
}

// the closes of the market index over the test period
void writeMarketCsv(const ResearchConfig& config, const ResearchResult& result) {
    std::ofstream market = openCsv(config.outPrefix + "_market.csv");
    market << "date,spy\n" << std::setprecision(FULL_PRECISION);
    for (size_t i = 0; i < result.marketDates.size(); i++) {
        //one row before the first test day too -> the first days return can be worked out
        if (i + 1 < result.marketDates.size() && result.marketDates[i + 1] < result.firstTestDate) continue;
        market << dateToString(result.marketDates[i]) << "," << result.marketCloses[i] << "\n";
    }
}

} // namespace

void writeResearch(const ResearchConfig& config, const ResearchResult& result) {
    if (config.outPrefix.empty()) return;
    writeDailyCsv(config, result);
    writeTradesCsv(config, result);
    writeWindowsCsv(config, result);
    writeTradePnlCsv(config, result);
    writeSummaryCsv(config, result);
    if (!result.marketDates.empty()) writeMarketCsv(config, result); //ONLY if SPY was in the pair list
}

namespace {

//one console line for one arm over one period, nothing if the period has no rows
void printPeriod(const char* name, const ArmResult& armResult, double capital, int from, int until) {
    const PortfolioMetrics metrics = portfolioMetrics(armResult.daily, capital, from, until);
    if (metrics.days == 0) return;
    std::cout << "    " << std::left << std::setw(28) << name << std::right << std::fixed << "Sharpe "
              << std::setprecision(2) << std::setw(6) << metrics.sharpe << " (SE " << metrics.sharpeSE << ")"
              << "   return " << std::setprecision(2) << std::setw(6) << 100.0 * metrics.annualReturn << "%/yr"
              << "   vol " << std::setw(5) << 100.0 * metrics.annualVol << "%"
              << "   max drawdown " << std::setw(5) << 100.0 * metrics.maxDrawdown << "%"
              << "   net $" << std::setprecision(0) << metrics.pnl << "   (" << metrics.days << " days)\n"
              << std::defaultfloat << std::setprecision(6); // sticky formatting -> put cout back
}

//the top of the console report -> what was run, on which data, with which settings and data fixes
void printHeader(const ResearchConfig& config, const ResearchResult& result) {
    //judged from what was loaded, not from the flag -> hand-downloaded and synthetic files sit outside the seal and can reach past 2022 w/o beign unsealed
    const bool pastCutoff = result.lastDate > HOLDOUT_LAST_DEV_DATE;
    std::cout << "walk-forward portfolio: " << result.pairs << " candidate pairs (" << result.excluded
              << " rule-development pairs left out), " << result.controls << " control pairs\n"
              << "data " << dateToString(result.firstDate) << " to " << dateToString(result.lastDate)
              << (!pastCutoff         ? "  [hold-out sealed: nothing after 2022-12-31 was read]"
                  : holdoutUnsealed() ? "  [HOLD-OUT UNSEALED]"
                                      : "  [files outside the seal: hand-downloaded or synthetic data]")
              << "\n"
              << "settings: " << config.windowMonths << "-month windows from " << config.firstTestYear << ", "
              << config.trainYears << "-year training, fills at the "
              << (config.fill == FillModel::NextOpen ? "next open" : "same close") << ", cost "
              << config.costPerExecution * 1e4 << " bp, borrow " << config.borrowPerYear * 1e4 << " bp/yr, "
              << (config.equalRisk ? "equal-risk sizing" : "equal-notional sizing") << ", caps "
              << (config.caps ? "on" : "off") << ", point-in-time membership " << (config.pointInTime ? "on" : "off")
              << ", rebase " << config.rebase << (config.halfLifeGate ? "" : ", half-life gate OFF")
              << (config.swapLegs ? ", legs SWAPPED" : "") << "\n"
              << "trade rules: entry " << config.base.trade.entryZ << " < abs(z) < " << config.base.trade.maxEntryZ
              << ", stop " << config.base.trade.stopZ << ", z window " << config.base.zWindow << ", kalman Q "
              << config.base.kalman.Q << ", selection needs trace >= " << config.selectMargin << " x critical value\n";
    for (const std::string& fix : result.fixesApplied) std::cout << "data fix: " << fix << "\n";
    if (result.droppedPairs)
        std::cout << "data fix: " << result.droppedPairs << " pairs left out with their dropped ticker\n";
}

//how many pair-windows got thruogh each stage of the screen
void printFunnel(const ResearchResult& result) {
    size_t eligible = 0, johansen = 0, selected = 0, controls = 0, controlsPass = 0;
    std::vector<size_t> selectedCounts;
    for (const WindowRow& windowRow : result.windows) {
        if (windowRow.from > result.lastDate) continue; //a window the data never reached
        eligible += windowRow.eligible;
        johansen += windowRow.johansenPass;
        selected += windowRow.selected;
        controls += windowRow.controls;
        controlsPass += windowRow.controlsPass;
        selectedCounts.push_back(windowRow.selected);
    }
    std::sort(selectedCounts.begin(), selectedCounts.end()); //median = the middle one (upper of 2 if even)
    std::cout << "screening (pair-windows): " << eligible << " screened, " << johansen << " cointegrated at 5%, "
              << selected << " selected (also half-life in band). median "
              << (selectedCounts.empty() ? 0 : selectedCounts[selectedCounts.size() / 2])
              << " pairs selected per window\n"
              << "distinct pairs: " << result.distinctEligible << " screened at least once, " << result.distinctSelected
              << " selected at least once\n"
              << "controls (same-index funds, share classes): cointegrated in " << controlsPass << " of " << controls
              << " pair-windows\n";
    if (result.screenErrors)
        std::cout << "johansen test not computable (series too alike) in " << result.screenErrors << " pair-windows\n";
}

//what the trades of one arm add up to, for its console block
struct TradeTally {
    size_t closed = 0, open = 0, wins = 0, stops = 0, timeouts = 0, pairs = 0;
    double gross = 0, cost = 0, borrow = 0, holdingDays = 0; //money includes open trades, holdingDays ONLY closed ones
};

//goes through the trades of one arm once and counts them up
TradeTally tallyTrades(const ArmResult& armResult) {
    size_t closed = 0, open = 0, wins = 0, stops = 0, timeouts = 0;
    double gross = 0, cost = 0, borrow = 0, holdingDays = 0;
    std::set<int> tradedPairs;
    for (const PortfolioTrade& trade : armResult.trades) {
        tradedPairs.insert(trade.pair);
        gross += trade.grossPnl;
        cost += trade.cost;
        borrow += trade.borrow;
        if (trade.exitDate == 0) { //still open -> its money counts, its outcome doesnt yet
            open++;
            continue;
        }
        closed++;
        holdingDays += trade.holdingDays;
        if (trade.netPnl > 0) wins++; //a win is net of costs and borrow
        if (trade.reason == ExitReason::Stop) stops++;
        if (trade.reason == ExitReason::Timeout) timeouts++;
    }
    return {closed, open, wins, stops, timeouts, tradedPairs.size(), gross, cost, borrow, holdingDays};
}

//the console block of one arm -> trades, money, exposure, what the risk layer did, then every report period
void printArm(const ResearchConfig& config, const ResearchResult& result, int arm) {
    const int firstTestDate = result.firstTestDate;
    const ArmResult& armResult = result.arm[arm];
    const TradeTally tally = tallyTrades(armResult);
    double grossSum = 0, grossMax = 0;
    size_t days = 0;
    for (const DailyRow& row : armResult.daily) {
        if (row.date < firstTestDate) continue; //exposure is averaged over test days ONLY
        grossSum += row.gross;
        grossMax = std::max(grossMax, row.gross);
        days++;
    }
    std::cout << "\n[" << armLabel(arm) << "] " << armName(arm) << "\n"
              << std::fixed << std::setprecision(1) << "    " << tally.closed << " round trips in " << tally.pairs
              << " distinct pairs (" << tally.open << " still open), "
              << (tally.closed ? 100.0 * (double)tally.wins / (double)tally.closed : 0.0) << "% winners, average hold "
              << (tally.closed ? tally.holdingDays / (double)tally.closed : 0.0) << " days, " << tally.stops
              << " stops, " << tally.timeouts << " timeouts\n"
              << std::setprecision(0) << "    gross $" << tally.gross << "   costs $" << tally.cost << "   borrow $"
              << tally.borrow << "   net $" << tally.gross - tally.cost - tally.borrow << std::setprecision(1)
              << "   average gross exposure " << (days ? 100.0 * grossSum / (double)days / config.capital : 0.0)
              << "% of capital, peak " << 100.0 * grossMax / config.capital << "%\n"
              << "    risk layer: " << armResult.orders << " opening orders (order-days), " << armResult.approved
              << " approved, of which " << armResult.scaledByTradeCap
              << " shrunk to the per-trade cap. refused: " << armResult.refusedBookCap << " by the book cap, "
              << armResult.refusedTickerCap << " by a ticker cap, " << armResult.refusedNoRisk
              << " no volatility estimate. the hedge-ratio guard stopped " << armResult.betaRefusals
              << " order-days before they became orders\n"
              << std::defaultfloat << std::setprecision(6);
    if (armResult.stranded)
        std::cout << "    " << armResult.stranded << " positions closed early because their pair's prices stopped\n";
    if (armResult.withdrawn)
        std::cout << "    " << armResult.withdrawn
                  << " approved entries withdrawn: the pair failed the next window's screen before the fill\n";
    if (std::abs(armResult.reconcile) > 0.01) //more than a cent apart -> the books dont add up
        std::cout << "    !! daily P&L and trade P&L disagree by $" << armResult.reconcile << "\n";
    for (const ReportPeriod& period : reportPeriods(config, result)) {
        printPeriod(period.label.c_str(), armResult, config.capital, period.from, period.until);
    }
}

//one line per test window with the return of the kalman and the static arm in it
void printWindowTable(const ResearchConfig& config, const ResearchResult& result) {
    std::cout << "\nper window (return on capital):\n"
              << "  window        selected   kalman  entries   static  entries   kalman ahead\n";
    size_t ahead = 0, counted = 0;
    for (size_t window = 0; window < result.windows.size(); window++) {
        const WindowRow& windowRow = result.windows[window];
        if (windowRow.from > result.lastDate) continue;
        double pnl[2] = {0, 0}; //[0] kalman, [1] static
        size_t entries[2] = {0, 0};
        const int headlineArms[2] = {ARM_KALMAN, ARM_STATIC};
        for (int j = 0; j < 2; j++) {
            for (const DailyRow& row : result.arm[headlineArms[j]].daily)
                if (row.date >= windowRow.from && row.date <= windowRow.until) pnl[j] += row.pnl;
            for (const PortfolioTrade& trade : result.arm[headlineArms[j]].trades)
                if (trade.window == (int)window) entries[j]++;
        }
        counted++;
        if (pnl[0] > pnl[1]) ahead++; //a tie counts as NOT ahead
        std::cout << "  " << dateToString(windowRow.from) << std::right << std::setw(10) << windowRow.selected
                  << std::fixed << std::setprecision(2) << std::setw(8) << 100.0 * pnl[0] / config.capital << "%"
                  << std::setw(8) << entries[0] << std::setw(8) << 100.0 * pnl[1] / config.capital << "%"
                  << std::setw(8) << entries[1] << "   " << (pnl[0] > pnl[1] ? "yes" : "no") << "\n"
                  << std::defaultfloat << std::setprecision(6);
    }
    std::cout << "  kalman ahead in " << ahead << " of " << counted << " windows\n";
}

//the last lines of the report -> close fallbacks, how fast the event loop ran, the files written
void printFooter(const ResearchConfig& config, const ResearchResult& result) {
    if (config.fill == FillModel::NextOpen) {
        std::cout << "\nfills that used the bar's close because an open was unusable: " << result.closeFallbackFills
                  << "\n";
    }
    std::cout << "event loop: " << result.pairBars << " pair-bars in " << std::fixed << std::setprecision(3)
              << result.seconds << " s -> " << std::setprecision(0) << (double)result.pairBars / result.seconds
              << " pair-bars/s (1 thread, every arm, with the risk layer)\n"
              << std::defaultfloat << std::setprecision(6);
    if (!config.outPrefix.empty()) {
        std::cout << "written: " << config.outPrefix
                  << "_daily.csv, _trades.csv, _tradepnl.csv, _windows.csv, _screen.csv, _summary.csv, _market.csv\n";
    }
}

} //namespace

void printResearch(const ResearchConfig& config, const ResearchResult& result) {
    printHeader(config, result);
    printFunnel(result);
    for (int arm = 0; arm < ARM_COUNT; arm++) {
        if (config.arms[arm]) printArm(config, result, arm);
    }
    //per window, the  2 headline arms side by side
    if (config.arms[ARM_KALMAN] && config.arms[ARM_STATIC]) printWindowTable(config, result);
    printFooter(config, result);
}

namespace {

//the arms named after --arms are switched on and every other one off
void parseArms(const std::string& names, bool arms[ARM_COUNT]) {
    for (int k = 0; k < ARM_COUNT; k++) arms[k] = false;
    std::stringstream list(names);
    std::string name;
    while (std::getline(list, name, ',')) {
        bool found = false;
        for (int k = 0; k < ARM_COUNT; k++) {
            if (name == armLabel(k) || name == armName(k)) { //short code or full name
                arms[k] = true;
                found = true;
            }
        }
        if (!found) throw std::runtime_error("unknown arm " + name + " (K, S, KS, SR, ST)");
    }
}

//the options after the pair list become the settings of the run, an unknown one stops it
ResearchConfig parseWalkForwardArgs(int argc, char* argv[]) {
    ResearchConfig config;
    config.pairList = argv[2]; //argv[1] is the mode word
    for (int i = 3; i < argc; i++) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string { //the word after the option,  and i steps past it
            if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
            return argv[++i];
        };
        if (arg == "--out") config.outPrefix = value();
        else if (arg == "--members") config.membersFile = value();
        else if (arg == "--data-fixes") {
            config.dataFixesFile = value();
            if (config.dataFixesFile == "none") config.dataFixesFile.clear(); //none = run w/o the fixes
        } else if (arg == "--end") config.dataEnd = dateToInt(value());
        else if (arg == "--quiet") config.quiet = true;
        else if (arg == "--first-year") config.firstTestYear = std::stoi(value());
        else if (arg == "--no-exclude") config.exclude.clear(); //the rule-development pairs stay in
        else if (arg == "--threads") config.threads = (unsigned)std::stoul(value());
        else if (arg == "--fill") {
            const std::string choice = value();
            if (choice == "close") config.fill = FillModel::SameClose;
            else if (choice == "open") config.fill = FillModel::NextOpen;
            else throw std::runtime_error("--fill takes close or open");
        } else if (arg == "--cost-bp") config.costPerExecution = std::stod(value()) * 1e-4; //bp -> share of notional
        else if (arg == "--borrow-bp") config.borrowPerYear = std::stod(value()) * 1e-4;
        else if (arg == "--no-pit") config.pointInTime = false;
        else if (arg == "--rebase") config.rebase = std::stod(value());
        else if (arg == "--swap") config.swapLegs = true;
        else if (arg == "--no-hl-gate") config.halfLifeGate = false;
        else if (arg == "--sizing") {
            const std::string choice = value();
            if (choice == "gross") config.equalRisk = false;
            else if (choice == "risk") config.equalRisk = true;
            else throw std::runtime_error("--sizing takes risk or gross");
        } else if (arg == "--no-caps") config.caps = false;
        // design candidates (DECISIONS.md D26)
        else if (arg == "--stop-z") config.base.trade.stopZ = std::stod(value());
        else if (arg == "--entry-z") config.base.trade.entryZ = std::stod(value());
        else if (arg == "--kalman-q") config.base.kalman.Q = std::stod(value());
        else if (arg == "--z-window") config.base.zWindow = (size_t)std::stoul(value());
        else if (arg == "--select-margin") config.selectMargin = std::stod(value());
        else if (arg == "--last-year") config.lastTestYear = std::stoi(value());
        else if (arg == "--design-until") config.designUntil = dateToInt(value());
        else if (arg == "--window-months") config.windowMonths = std::stoi(value());
        else if (arg == "--arms") parseArms(value(), config.arms);
        else throw std::runtime_error("unknown option " + arg);
    }
    return config;
}

} //namespace

int runWalkForwardCommand(int argc, char* argv[]) {
    if (argc < 3) { //no pair list given
        throw std::runtime_error(
            "usage: BackTester walkforward <pairList.csv> [--out PREFIX] [--arms K,S,KS,SR,ST]\n"
            "  sensitivities (each changes one thing in the pre-registered configuration):\n"
            "  --fill close | --cost-bp X | --borrow-bp X | --no-pit | --rebase X | --swap | --no-hl-gate |\n"
            "  --sizing gross | --no-caps | --window-months 6");
    }
    const ResearchConfig config = parseWalkForwardArgs(argc, argv);
    const ResearchResult result = runResearch(config);
    if (!config.quiet) printResearch(config, result);
    writeResearch(config, result);
    if (config.quiet) //a quiet run still says it finished
        std::cout << "walkforward: done (" << result.pairBars << " pair-bars), output: " << config.outPrefix
                  << "_*.csv\n";
    return 0;
}
