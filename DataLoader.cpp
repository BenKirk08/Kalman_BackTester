#include "DataLoader.h"
#include "Holdout.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <cmath>
#include <stdexcept>

std::vector<Bar> loadCSV(const std::string& filename) {
    std::vector<Bar> bars;
    std::ifstream file(filename);

    //cant open -> stop here. used to print a message then carry on with no data, which crashed later
    if (!file.is_open()) {
        throw std::runtime_error("couldnt open data file: " + filename +
                                 " -> check the file exists and you are running from the project folder");
    }
    std::string line;
    //hold-out seal -> rows after  this date are never read from the bulk data (empty = this file isnt sealed)
    const std::string cutoff = holdoutCutoff(filename) ? HOLDOUT_LAST_DEV_DATE_TEXT : "";

    std::getline(file, line); //skip header

    while (std::getline(file, line)) {
        if (line.empty()) continue; // skip empty lines
        
        std::stringstream lineStream(line);
        std::string field;
        Bar bar;

        std::getline(lineStream, bar.date, ',');
        std::getline(lineStream, field, ','); bar.open = std::stod(field);
        std::getline(lineStream, field, ','); bar.high = std::stod(field);
        std::getline(lineStream, field, ','); bar.low = std::stod(field);
        std::getline(lineStream, field, ','); bar.close = std::stod(field);
        std::getline(lineStream, field, ','); bar.volume = std::stoll(field);

        if (!cutoff.empty() && bar.date > cutoff) continue; //sealed hold-out row
        bars.push_back(bar);
    }
    //opened but held no rows (empty file / header only) -> same crsah later, so same clear error now
    if (bars.empty()) throw std::runtime_error("no price rows in data file: " + filename);
    return bars;
}


//two  pointer merge -> keep bars preesnt in both series and drop unmatched days
std::vector<AlignedBar> alignBars(const std::vector<Bar>& barsA, const std::vector<Bar>& barsB) {
    std::vector<AlignedBar> aligned;
    size_t i = 0, j = 0; // useful in .size() comparisons (same data type) -> avoids error

    while (i < barsA.size() && j < barsB.size()) {
        if (barsA[i].date == barsB[j].date) { //can compare string as same fromat
            aligned.push_back({barsA[i].date, barsA[i].close, barsB[j].close, std::log(barsA[i].close), std::log(barsB[j].close)});
            i++; // log -> natural log (NOT base 10 but base e)
            j++;
        } else if (barsA[i].date < barsB[j].date) {
            i++; // A has an  earlier date than  B, skip it
        } else {
            j++; //B has an earlier date than A, skip it
        }
    }
    return aligned;
}