#include "Market.h"
#include "Holdout.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>

int dateToInt(const std::string& date) {
    if (date.empty()) return 0;
    // "2023-01-04" -> 20230104. anything else is a mistake in a config, not something to guess at
    if (date.size() != 10 || date[4] != '-' || date[7] != '-')
        throw std::runtime_error("bad date (want YYYY-MM-DD): " + date);
    int value = 0;
    for (size_t k = 0; k < 10; k++) {
        if (k == 4 || k == 7) continue; //the 2 dashes
        if (date[k] < '0' || date[k] > '9') throw std::runtime_error("bad date (want YYYY-MM-DD): " + date);
        value = value * 10 + (date[k] - '0');
    }
    return value;
}

std::string dateToString(int date) {
    if (date == 0) return "";
    char buffer[16];
    //20230104 -> year = first 4 digits, month = middle 2, day = last 2
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", date / 10000, (date / 100) % 100, date % 100);
    return buffer;
}

//same rule as shortName in Backtest.cpp -> kept separate so the engine doesnt depend on the legacy pipeline
static std::string tickerName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1); // drop the folders
    const size_t cut = name.find("_Data");
    if (cut != std::string::npos) name = name.substr(0, cut); //"v_Data.csv" -> "v"
    return name;
}

#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
//the run of digits at the front of the 8 bytes at text -> count = how many (8 = it may go on), returns their value.all 8 bytes at once, no digit by digit multiply chain
static unsigned long long digitRun(const char* text, int& count) {
    unsigned long long word;
    std::memcpy(&word, text, sizeof(word)); //first char = lowest byte (little endian, checked in parseNumber)
    word ^= 0x3030303030303030ULL; //digits become 0..9, any other byte ends up above 9

    //a byte above 9 has a high nbible already or gets one from the + 6. bytes under the first such one are digits, so no carry reaches it
    const unsigned long long notDigit = ((word + 0x0606060606060606ULL) | word) & 0xF0F0F0F0F0F0F0F0ULL;
    count = notDigit ? (int)(__builtin_ctzll(notDigit) >> 3) : 8; //lowest marked byte = first char that isnt a digit
    if (count == 0) return 0;
    word <<= 8 * (8 - count); //the digits move  to the top bytes, zeros come in below -> they read as leading zeros
    word = ((word * 2561) >> 8) & 0x00FF00FF00FF00FFULL; //2561 = 10 x 256 + 1 -> each pair of digits, 0..99
    word = ((word * 6553601) >> 16) & 0x0000FFFF0000FFFFULL; //100 x 2^16 + 1 -> each 4 digits, 0..9999
    return (word * 42949672960001ULL) >> 32; //10000 x 2^32 + 1 -> all 8
}
#endif

//136.0575 = 1360575 / 10^4, both exact doubles -> one IEEE division gives the same double as strtod, w/o the locale lock that made loading SLOWER with more threads. reads up to 8 bytes past a number, readWholeFile pads for that
static double parseNumber(const char* start, const char** after) {
    //10^22 is the biggest power of 10 a double holds exactly
    static const double POW10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                     1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    //not a digit, point or sign (empty last field, a letter) -> without this strtod would skip the line break and read the NEXT rows year
    if (!((*start >= '0' && *start <= '9') || *start == '.' || *start == '-' || *start == '+')) {
        *after = start; //nothing read -> caller sees after == start
        return 0.0;
    }
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    //the usual price -> under 8 digits, maybe a point and under 8 more. same mantissa and same division as the loop below, anything else falls through to it
    static const unsigned long long WHOLE_POW10[8] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000};
    int wholeCount = 0, fractionCount = 0;
    const unsigned long long whole = digitRun(start, wholeCount);
    if (wholeCount < 8) {
        const char* numberEnd = start + wholeCount;
        unsigned long long fraction = 0;
        if (*numberEnd == '.') {
            fraction = digitRun(numberEnd + 1, fractionCount);
            numberEnd += 1 + fractionCount;
        }
        // at least 1 digit, no e after it and at most 14 digits in all -> always under 2^53
        if (fractionCount < 8 && wholeCount + fractionCount > 0 && *numberEnd != 'e' && *numberEnd != 'E') {
            const unsigned long long fastMantissa = whole * WHOLE_POW10[fractionCount] + fraction;
            *after = numberEnd;
            return (double)fastMantissa / POW10[fractionCount];
        }
    }
#endif
    const char* cursor = start;
    unsigned long long mantissa = 0;
    int digits = 0, fractionDigits = 0;
    bool seenDot = false, anyDigit = false;
    for (;; cursor++) {
        const char ch = *cursor;
        if (ch >= '0' && ch <= '9') {
            anyDigit = true;
            if (mantissa != 0 || ch != '0') digits++; //leding zeros dont count
            if (digits <= 18)
                mantissa = mantissa * 10 + (unsigned long long)(ch - '0'); //18 digits always fit in 64 bits
            if (seenDot) fractionDigits++; //power of 10 to divide by
        } else if (ch == '.' && !seenDot) {
            seenDot = true;
        } else {
            break; //end of the number. a leading sign stops here at once -> strtod below
        }
    }
    //fast path only when mantissa and 10^power are both exact. an  e  after the digits = exponent -> strtod
    if (anyDigit && digits <= 18 && mantissa < (1ULL << 53) && fractionDigits <= 22 && *cursor != 'e' &&
        *cursor != 'E') {
        *after = cursor;
        return (double)mantissa / POW10[fractionDigits];
    }
    char* end = nullptr;
    const double value = std::strtod(start, &end); // signs, exponents, very long numbers -> let the library do it
    *after = end;
    return value;
}

//the whole file in one buffer with a 0 after its last byte. a regular file says how long it is -> one allocation, read straight into it
static std::unique_ptr<char[]> readWholeFile(std::FILE* handle, size_t& length) {
    size_t capacity = 1 << 16; //only when the size is not known
    struct stat info;
    //only a regular file is believed -> a folder or a pipe can report any size at all. 1 spare byte so the buffer is never full at the end
    if (fstat(fileno(handle), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0)
        capacity = (size_t)info.st_size + 1;

    // + 8 -> the closing 0 and 7 more, parseNumber looks at 8 bytes at a time. new[] leaves the bytes unfilled, a string would zero them all first
    std::unique_ptr<char[]> text(new char[capacity + 8]);
    length = 0;
    for (;;) {
        //64KB a go and stop at the first read that brings nothing, as the chunked loop did -> a read that failed once is tried again
        const size_t got = std::fread(text.get() + length, 1, std::min(capacity - length, (size_t)1 << 16), handle);
        if (got == 0) break;
        length += got;
        if (length < capacity) continue;

        //longer than it said -> twice the room, what is read so far moves over
        capacity *= 2;
        std::unique_ptr<char[]> bigger(new char[capacity + 8]);
        std::memcpy(bigger.get(), text.get(), length);
        text = std::move(bigger);
    }
    std::memset(text.get() + length, 0, 8);
    return text;
}

Series loadSeries(const std::string& file, int dataEnd) {
    Series series;
    series.file = file;
    series.name = tickerName(file);

    //whole file in one read -> a 20 year daily file is ~300KB, far cheaper than a getline + stringstream per row
    std::FILE* handle = std::fopen(file.c_str(), "rb");
    if (!handle) {
        throw std::runtime_error("couldnt open data file: " + file +
                                 " -> check the file exists and you are running from the project folder");
    }
    size_t textSize = 0;
    const std::unique_ptr<char[]> text = readWholeFile(handle, textSize);
    std::fclose(handle);

    const char* cursor = text.get(); //null terminated -> strtod can never run off the end
    const char* textEnd = cursor + textSize;
    while (cursor < textEnd && *cursor != '\n') cursor++; //skip header
    if (cursor < textEnd) cursor++;

    //hold-out seal first, then the callers own limit -> whichever is earlier (0 = no limit)
    const int sealDate = holdoutCutoff(file);
    if (sealDate != 0 && (dataEnd == 0 || sealDate < dataEnd)) dataEnd = sealDate;

    //~48 bytes a row -> rough row count, only sizes the reserve
    series.date.reserve(textSize / 48);
    series.open.reserve(textSize / 48);
    series.close.reserve(textSize / 48);
    const double NaN = std::nan("");
    bool anyRow = false; // the file has price rows at all (even if the cut-off drops every one of them)
    while (cursor < textEnd) {
        const char* lineEnd = static_cast<const char*>(std::memchr(cursor, '\n', (size_t)(textEnd - cursor)));
        if (!lineEnd) lineEnd = textEnd; //last line w/o a newline
        if (lineEnd - cursor >= 10 && cursor[4] == '-' && cursor[7] == '-') { //starts like YYYY-MM-DD -> price row
            int date = 0;
            bool ok = true;
            for (int k = 0; k < 10; k++) {
                if (k == 4 || k == 7) continue; //the 2 dashes
                if (cursor[k] < '0' || cursor[k] > '9') {
                    ok = false;
                    break;
                }
                date = date * 10 + (cursor[k] - '0');
            }
            //open, high, low, close after the date -> same doubles stod gives (verify checks every close against the legacy loader)
            double ohlc[4];
            const char* field = cursor + 10; //just past the date, on its comma
            for (int column = 0; column < 4 && ok; column++) {
                if (field >= lineEnd || *field != ',') {
                    ok = false;
                    break;
                }
                field++;
                const char* after = nullptr;
                ohlc[column] = parseNumber(field, &after);
                if (after == field) ok = false; //empty or not a number
                field = after;
            }
            if (!ok || field > lineEnd) throw std::runtime_error("bad row in data file: " + file);
            //the engine takes ln(close) -> a close that is not a positive number is a broken file, not a price
            if (!(ohlc[3] > 0.0) || !std::isfinite(ohlc[3]))
                throw std::runtime_error("close is not a positive number in data file: " + file);
            anyRow = true;
            if (dataEnd == 0 || date <= dataEnd) { //dataEnd itself is kept
                const double open = ohlc[0], high = ohlc[1], low = ohlc[2], close = ohlc[3];
                //open only believed if positive & inside that days low-high (1e-6 slack for rounding in the file). else NaN -> a fill on that bar uses its close
                const bool openOK = open > 0.0 && open >= low * (1.0 - 1e-6) && open <= high * (1.0 + 1e-6);
                if (!openOK) series.badOpens++;
                series.date.push_back(date);
                series.open.push_back(openOK ? open : NaN);
                series.close.push_back(close);
            }
        } else if (lineEnd - cursor > 1) { //a non-empty line that isnt a price row ("\r" alone is just a blank line)
            throw std::runtime_error("bad row in data file: " + file);
        }
        cursor = lineEnd + 1; //after the last line this is at or past textEnd -> loop stops
    }
    //empty file = error. a ticker listed after the cut-off (seal or --end) is not -> it just has no bars yet, nor does any pair with it
    if (!anyRow) throw std::runtime_error("no price rows in data file: " + file);

    series.lnClose.resize(series.close.size());
    //same log as alignBars -> verify checks the log closes match bit for bit
    for (size_t i = 0; i < series.close.size(); i++) series.lnClose[i] = std::log(series.close[i]);
    return series;
}

// count values of source starting at first go onto the end of target -> one block copy, same bits
template <typename Value>
static void appendRun(std::vector<Value>& target, const std::vector<Value>& source, size_t first, size_t count) {
    target.insert(target.end(), source.data() + first, source.data() + first + count);
}

void alignPair(const Series& seriesA, const Series& seriesB, PairData& out) {
    out.nameA = seriesA.name;
    out.nameB = seriesB.name;
    //out may be reused -> nothing left over from the last pair
    out.ownDate.clear();
    out.ownOpenA.clear();
    out.ownOpenB.clear();
    out.ownCloseA.clear();
    out.ownCloseB.clear();
    out.ownLnA.clear();
    out.ownLnB.clear();

    //fast path -> if from the later of the 2 start dates on both tickers have exactly the same days, the pair is a window into each series (no copy)
    if (!seriesA.date.empty() && !seriesB.date.empty()) {
        const int start = std::max(seriesA.date.front(), seriesB.date.front());
        //first bar on or after start in each
        const size_t firstA =
            (size_t)(std::lower_bound(seriesA.date.begin(), seriesA.date.end(), start) - seriesA.date.begin());
        const size_t firstB =
            (size_t)(std::lower_bound(seriesB.date.begin(), seriesB.date.end(), start) - seriesB.date.begin());
        const size_t countA = seriesA.date.size() - firstA, countB = seriesB.date.size() - firstB;
        //same length AND every date equal (memcmp on the int arrays)
        if (countA == countB && countA > 0 &&
            std::memcmp(&seriesA.date[firstA], &seriesB.date[firstB], countA * sizeof(int)) == 0) {
            out.n = countA;
            out.date = &seriesA.date[firstA];
            out.openA = &seriesA.open[firstA];
            out.closeA = &seriesA.close[firstA];
            out.lnA = &seriesA.lnClose[firstA];
            out.openB = &seriesB.open[firstB];
            out.closeB = &seriesB.close[firstB];
            out.lnB = &seriesB.lnClose[firstB];
            return;
        }
    }

    // calendars differ -> two pointer merge, keep the days present in both (same walk as alignBars)
    const size_t maxCommon = std::min(seriesA.date.size(), seriesB.date.size()); //at most the shoretr length
    out.ownDate.reserve(maxCommon);
    out.ownOpenA.reserve(maxCommon);
    out.ownOpenB.reserve(maxCommon);
    out.ownCloseA.reserve(maxCommon);
    out.ownCloseB.reserve(maxCommon);
    out.ownLnA.reserve(maxCommon);
    out.ownLnB.reserve(maxCommon);
    const size_t sizeA = seriesA.date.size(), sizeB = seriesB.date.size();
    size_t i = 0, j = 0; //i walks A, j walks B
    while (i < sizeA && j < sizeB) {
        if (seriesA.date[i] == seriesB.date[j]) {
            //2 calendars agree for years at a time -> measure the stretch of shared days, then 7 block copies instead of 7 push_backs a day
            const size_t longest = std::min(sizeA - i, sizeB - j);
            size_t run = 1;
            while (run < longest && seriesA.date[i + run] == seriesB.date[j + run]) run++;
            appendRun(out.ownDate, seriesA.date, i, run);
            appendRun(out.ownOpenA, seriesA.open, i, run);
            appendRun(out.ownCloseA, seriesA.close, i, run);
            appendRun(out.ownLnA, seriesA.lnClose, i, run);
            appendRun(out.ownOpenB, seriesB.open, j, run);
            appendRun(out.ownCloseB, seriesB.close, j, run);
            appendRun(out.ownLnB, seriesB.lnClose, j, run);
            i += run;
            j += run;
        } else if (seriesA.date[i] < seriesB.date[j]) {
            i++; //A has a day B doesnt -> drop it
        } else {
            j++; // and the other way round
        }
    }
    // pointers set only after the last copy -> no reallocation can move them
    out.n = out.ownDate.size();
    out.date = out.ownDate.data();
    out.openA = out.ownOpenA.data();
    out.closeA = out.ownCloseA.data();
    out.lnA = out.ownLnA.data();
    out.openB = out.ownOpenB.data();
    out.closeB = out.ownCloseB.data();
    out.lnB = out.ownLnB.data();
}

std::vector<PairSpec> readPairList(const std::string& listFile) {
    std::FILE* handle = std::fopen(listFile.c_str(), "rb");
    if (!handle) throw std::runtime_error("couldnt open pair list: " + listFile);
    std::string text;
    char chunk[1 << 16];
    size_t bytesRead;
    while ((bytesRead = std::fread(chunk, 1, sizeof(chunk), handle)) > 0) text.append(chunk, bytesRead);
    std::fclose(handle);

    std::vector<PairSpec> specs;
    size_t position = text.find('\n'); // skip header
    position = (position == std::string::npos) ? text.size() : position + 1; //header only -> no rows
    while (position < text.size()) {
        size_t lineEnd = text.find('\n', position);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        std::string line = text.substr(position, lineEnd - position);
        position = lineEnd + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back(); // windows line ending
        if (line.empty()) continue;

        PairSpec spec;
        std::string* fields[4] = {&spec.label, &spec.fileA, &spec.fileB, &spec.group}; //csv column order
        size_t fieldStart = 0;
        //no group column -> loop stops after fileB and group stays empty
        for (int k = 0; k < 4 && fieldStart <= line.size(); k++) {
            size_t comma = line.find(',', fieldStart);
            if (comma == std::string::npos) comma = line.size();
            *fields[k] = line.substr(fieldStart, comma - fieldStart);
            fieldStart = comma + 1;
        }
        if (spec.fileA.empty() || spec.fileB.empty()) throw std::runtime_error("bad row in pair list: " + line);
        specs.push_back(spec);
    }
    return specs;
}

unsigned hardwareThreads() {
    const unsigned cores = std::thread::hardware_concurrency();
    return cores ? cores : 1; //0 = the system wouldnt say -> 1 thread
}

void parallelFor(size_t count, unsigned threads, const std::function<void(size_t)>& work) {
    if (threads == 0) threads = hardwareThreads();
    if (threads > count) threads = (unsigned)count; //never more threads than jobs
    if (threads <= 1) {
        for (size_t i = 0; i < count; i++) work(i); //no pool at all
        return;
    }
    // shared counter -> each worker takes the next unclaimde index. who does which varies run to run, output dosent (own slot each)
    std::atomic<size_t> nextIndex{0};
    std::exception_ptr firstError;
    std::mutex errorLock;
    const auto worker = [&]() {
        for (;;) {
            //relaxed is enough -> join below makes every result visible
            const size_t i = nextIndex.fetch_add(1, std::memory_order_relaxed);
            if (i >= count) return;
            try {
                work(i);
            } catch (...) {
                std::lock_guard<std::mutex> guard(errorLock);
                if (!firstError) firstError = std::current_exception(); //later errors are dropped
                nextIndex.store(count, std::memory_order_relaxed); //stop handing out work
                return;
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned i = 0; i < threads; i++) pool.emplace_back(worker);
    for (std::thread& thread : pool) thread.join();
    if (firstError) std::rethrow_exception(firstError); // only once every thread has stopped
}

void Market::load(const std::vector<PairSpec>& specs, int dataEnd, unsigned threads) {
    std::vector<std::string> files;
    for (const PairSpec& spec : specs) {
        for (const std::string* file : {&spec.fileA, &spec.fileB}) {
            //first time this file is named -> its slot goes after the series alraedy held
            if (indexByFile.emplace(*file, files.size() + allSeries.size()).second) files.push_back(*file);
        }
    }
    const size_t firstNew = allSeries.size();
    allSeries.resize(firstNew + files.size());
    //each worker fills only its own slot
    parallelFor(files.size(), threads,
                [&](size_t k) { allSeries[firstNew + k] = std::make_unique<Series>(loadSeries(files[k], dataEnd)); });
}

const Series& Market::series(const std::string& file) const {
    const auto found = indexByFile.find(file);
    if (found == indexByFile.end()) throw std::runtime_error("ticker file was never loaded: " + file);
    return *allSeries[found->second];
}

size_t Market::dropBefore(const std::string& file, int firstDate) {
    const auto found = indexByFile.find(file);
    if (found == indexByFile.end()) return 0; //never loaded -> nothing to  drop
    Series& ticker = *allSeries[found->second];
    // rows dated before firstDate (firstDate itself stays)
    const size_t cut =
        (size_t)(std::lower_bound(ticker.date.begin(), ticker.date.end(), firstDate) - ticker.date.begin());
    ticker.date.erase(ticker.date.begin(), ticker.date.begin() + (std::ptrdiff_t)cut);
    ticker.open.erase(ticker.open.begin(), ticker.open.begin() + (std::ptrdiff_t)cut);
    ticker.close.erase(ticker.close.begin(), ticker.close.begin() + (std::ptrdiff_t)cut);
    ticker.lnClose.erase(ticker.lnClose.begin(), ticker.lnClose.begin() + (std::ptrdiff_t)cut);
    return cut;
}

size_t Market::rows() const {
    size_t total = 0;
    for (const auto& ticker : allSeries) total += ticker->date.size();
    return total;
}
