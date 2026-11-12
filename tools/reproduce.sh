#!/bin/sh
#one command that rebuilds every number in the README and RESUME_STATS.md. run from the project folder as tools/reproduce.sh, or w/ --final to add the 2023 onwards runs that unseal the hold-out
set -e
FINAL=0
[ "$1" = "--final" ] && FINAL=1
OUT=Data/Output/portfolio
BIN=./build/BackTester
PY=".venv/bin/python -I"
say() { printf '\n==================== %s ====================\n' "$1"; }

say "build (Release)"
cmake -B build > /dev/null
cmake --build build -j > /tmp/backtester_build.log 2>&1 || { cat /tmp/backtester_build.log; echo "build failed"; exit 1; }
tail -1 /tmp/backtester_build.log

say "pair list from the sector groups"
$PY tools/make_sp500_groups.py
$PY tools/make_pairs.py tools/sp500_groups.csv | tail -4

say "data checks (pre-2023 bars only)"
$PY tools/data_qa.py | grep -E "tickers,|missing sessions:|split-like|one-bar spikes|stale prices:|large moves"

say "regression: the legacy code still gives the recorded outputs"
#cmp is silent when identical, so the echo only prints on a match
$BIN --old-data > /tmp/backtester_report_olddata.txt 2>&1 && cmp /tmp/backtester_report_olddata.txt tests/baseline/runall_olddata.txt && echo "original report on the old files: identical to tests/baseline/runall_olddata.txt"
$BIN dump tests/baseline/pairs_baseline.csv /tmp/backtester_dump.txt > /dev/null && cmp /tmp/backtester_dump.txt tests/baseline/dump_dev_O0.txt && echo "1,426 legacy fingerprints: identical to tests/baseline/dump_dev_O0.txt"

say "the event engine reproduces the legacy pipeline, and cannot look ahead"
$BIN verify tests/pairs_original.csv --all
$BIN verify tests/baseline/pairs_baseline.csv

say "the trade rules, replayed by an independent implementation"
rm -rf /tmp/backtester_trace && mkdir -p /tmp/backtester_trace
TRACES=""
#tail +2 skips the header, label is column 1
for label in $(tail -n +2 tests/pairs_original.csv | cut -d, -f1); do
    fileA=$(grep "^$label," tests/pairs_original.csv | cut -d, -f2)
    fileB=$(grep "^$label," tests/pairs_original.csv | cut -d, -f3)
#every pair runs under both fill rules, next bar open and same bar close
    $BIN trace "$fileA" "$fileB" /tmp/backtester_trace/${label}_open --fill open > /dev/null
    $BIN trace "$fileA" "$fileB" /tmp/backtester_trace/${label}_close --fill close > /dev/null
    TRACES="$TRACES /tmp/backtester_trace/${label}_open /tmp/backtester_trace/${label}_close"
done
$PY analysis/replay_check.py $TRACES > /tmp/backtester_replay.log || { cat /tmp/backtester_replay.log; echo "replay check failed"; exit 1; }
awk '{ n++; if ($1 == "OK") ok++ } END { print ok " of " n " traces replayed identically" }' /tmp/backtester_replay.log

F_FLAGS="--sizing gross --select-margin 1.25 --design-until 2015-12-31" #the final design (DECISIONS.md L10)

say "walk-forward portfolio: the pre-registered configuration (H0), development data only (hold-out sealed)"
mkdir -p $OUT
$BIN walkforward Data/Input/stooq/pairs.csv --out $OUT/h0_dev

say "the design candidates, scored on test windows 2008-2015 only"
tools/run_design_candidates.sh > /dev/null
tools/run_design_candidates.sh c8_notional_and_trace_1.25x --sizing gross --select-margin 1.25

say "the chosen design (F) on development data: design windows 2008-2015, validation 2016-2022"
$BIN walkforward Data/Input/stooq/pairs.csv $F_FLAGS --out $OUT/f_dev | grep -E "^\[|design windows|validation"

say "the accounting, rebuilt from the raw price files by an independent script"
$PY analysis/portfolio_check.py $OUT/h0_dev

say "speed, like for like with the legacy baseline (data to 2022)"
$BIN bench tests/baseline/pairs_baseline.csv --legacy

if [ $FINAL -eq 0 ]; then
    say "done (development period only). tools/reproduce.sh --final adds everything that reads the hold-out"
    exit 0
fi

say "HOLD-OUT: H0 unsealed, checked against its development run"
$BIN walkforward Data/Input/stooq/pairs.csv --out $OUT/h0 --unseal-holdout
#unsealed run has to match its sealed dev run up to the cut date
$PY tools/compare_runs.py $OUT/h0_dev $OUT/h0 --until 2022-12-30
$PY analysis/portfolio_check.py $OUT/h0

say "HOLD-OUT: the final design (F) unsealed, checked against its development run"
$BIN walkforward Data/Input/stooq/pairs.csv $F_FLAGS --out $OUT/f --unseal-holdout
$PY tools/compare_runs.py $OUT/f_dev $OUT/f --until 2022-12-30
$PY analysis/portfolio_check.py $OUT/f --sizing gross

say "HOLD-OUT: the 13 pre-registered sensitivities around H0, then the same 13 around F"
tools/run_sensitivities.sh $OUT/full --unseal-holdout > /tmp/backtester_sensitivities.log 2>&1 || { cat /tmp/backtester_sensitivities.log; exit 1; }
$PY tools/sensitivity_table.py $OUT/full
#BASE swaps the second set of 13 over to design F
BASE="$F_FLAGS" tools/run_sensitivities.sh $OUT/f_full --unseal-holdout > /tmp/backtester_sensitivities_f.log 2>&1 || { cat /tmp/backtester_sensitivities_f.log; exit 1; }
$PY tools/sensitivity_table.py $OUT/f_full

say "monte carlo on the portfolio (10,000 resamples, 10,000 random books): F, then H0"
$PY analysis/monte_carlo.py --portfolio $OUT/f
$PY analysis/monte_carlo.py --portfolio $OUT/h0

say "speed on the full history"
$BIN bench Data/Input/stooq/pairs.csv --unseal-holdout

say "the dividend-adjustment bug in the hand-downloaded files (reads 2023+ closes of 8 tickers, logged)"
$PY tools/check_dividend_adjustment.py

say "the original pairs on the corrected data (README results 7, findings 1 to 4)"
$BIN --unseal-holdout
$BIN facts --unseal-holdout

say "the original single-pair monte carlo on the corrected data"
$PY analysis/monte_carlo.py --unseal-holdout
