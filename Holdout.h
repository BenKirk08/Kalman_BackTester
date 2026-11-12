#pragma once
#include <string>

// the hold-out seal (DECISIONS.md D1, D22) -> DEFAULT-DENY, both loaders (loadCSV & loadSeries) drop every row after 2022-12-31 unless the run has --unseal-holdout or the file really sits (symlinks & case resolved first) directly in Data/Input, in Data/Input/synthetic or under Data/Output/montecarlo/paths_tmp

const int HOLDOUT_LAST_DEV_DATE = 20221231;
const char* const HOLDOUT_LAST_DEV_DATE_TEXT = "2022-12-31";

//0 -> this file is not sealed (keep every row). otehrwise the last date that may be read from it
int holdoutCutoff(const std::string& file);
bool holdoutUnsealed();

//called once by main for --unseal-holdout -> prints a banner and appends the time, command line and pair list fingerprint to HOLDOUT_UNSEAL.log in the folder the program is run from
void unsealHoldout(const std::string& commandLine, const std::string& pairList);
