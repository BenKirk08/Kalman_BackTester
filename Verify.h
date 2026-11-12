#pragma once
#include <string>

//"BackTester verify" -> event engine vs the legacy pipeline on every pair in the list. 0 = all matched, 1 otherwise
int runVerify(const std::string& listFile, bool all, unsigned threads, int firstTestYear, int lastTestYear);
