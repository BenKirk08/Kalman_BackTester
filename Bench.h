#pragma once
#include <string>

//"BackTester bench" -> pair-bars per second from aligning to metrics, csv reading timed apart. pair-bar as in D24
int runBench(const std::string& listFile, unsigned threads, int runs, bool withLegacy);

//"BackTester batch" ->  one results row per pair (final design, full sample), same columns the legacy batch wrote
int runBatchEngine(const std::string& listFile, const std::string& resultsFile, unsigned threads);
