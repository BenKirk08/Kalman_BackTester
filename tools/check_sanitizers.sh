#!/bin/sh
#debug build w/ address + undefined sanitizers pushed through the sealed check commands -> tools/check_sanitizers.sh [--quick], unoptimised so engine outputs are not compared byte for byte
set -u
cd "$(dirname "$0")/.."
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1
REF="${BACKTESTER_REFERENCE:-../BackTester_reference}"
BUILD=build-asan
OUT=/tmp/backtester_sanitizers
rm -rf "$OUT"; mkdir -p "$OUT/log" "$OUT/trace" "$OUT/wf"

echo "check_sanitizers: building $BUILD (Debug, -fsanitize=address,undefined) ..."
cmake -B $BUILD -DCMAKE_BUILD_TYPE=Debug -DBACKTESTER_SANITIZE=ON > /dev/null || exit 1
cmake --build $BUILD -j > "$OUT/build.log" 2>&1 || { cat "$OUT/build.log"; echo "build failed"; exit 1; }
BIN=$PWD/$BUILD/BackTester

#keep giong after a report so one run lists every problem, and check the stack after returns too
export ASAN_OPTIONS=halt_on_error=0:detect_stack_use_after_return=1:strict_string_checks=1
export UBSAN_OPTIONS=print_stacktrace=1

run() { name="$1"; shift; "$@" > "$OUT/log/$name.txt" 2>&1; echo "exit=$?" >> "$OUT/log/$name.txt"; }
PAIRS=Data/Input/stooq/pairs.csv

echo "check_sanitizers: the commands of tools/reproduce.sh (development mode) ..."
run olddata_report $BIN --old-data
run verify_original $BIN verify tests/pairs_original.csv --all
for label in $(tail -n +2 tests/pairs_original.csv | cut -d, -f1); do
    fileA=$(grep "^$label," tests/pairs_original.csv | cut -d, -f2)
    fileB=$(grep "^$label," tests/pairs_original.csv | cut -d, -f3)
    run "trace_${label}_open" $BIN trace "$fileA" "$fileB" "$OUT/trace/${label}_open" --fill open
    run "trace_${label}_close" $BIN trace "$fileA" "$fileB" "$OUT/trace/${label}_close" --fill close
done
run wf_h0_dev $BIN walkforward $PAIRS --out "$OUT/wf/h0_dev"
run wf_f_dev $BIN walkforward $PAIRS --sizing gross --select-margin 1.25 --design-until 2015-12-31 --out "$OUT/wf/f_dev"
run wf_design_h0 $BIN walkforward $PAIRS --arms K,S --end 2015-12-31 --last-year 2015 --out "$OUT/wf/design_h0" --quiet
run wf_design_c6 $BIN walkforward $PAIRS --arms K,S --end 2015-12-31 --last-year 2015 --out "$OUT/wf/design_c6" --quiet --window-months 6
run bench_baseline $BIN bench tests/baseline/pairs_baseline.csv --runs 1
if [ $QUICK -eq 0 ]; then
    run dump_baseline $BIN dump tests/baseline/pairs_baseline.csv "$OUT/dump.txt"
    run verify_baseline $BIN verify tests/baseline/pairs_baseline.csv
fi

echo "check_sanitizers: tools/check_extended.sh with the sanitizer binary ..."
BACKTESTER_BIN=$BIN BACKTESTER_SANDBOX="$OUT/sandbox" tools/check_extended.sh "$OUT/extended" > "$OUT/extended.log" 2>&1

BAD=0
#a report in any log counts
REPORTS=$(grep -l -E "runtime error:|ERROR: AddressSanitizer|ERROR: LeakSanitizer|SUMMARY: (Address|UndefinedBehavior)Sanitizer" "$OUT"/log/*.txt "$OUT"/extended/*.txt 2>/dev/null)
if [ -n "$REPORTS" ]; then
    echo "SANITIZER REPORTS in:"; echo "$REPORTS"
    grep -h -E "runtime error:|ERROR: AddressSanitizer|SUMMARY:" $REPORTS | sort | uniq -c | head -40
    BAD=1
fi
for f in "$OUT"/log/*.txt; do
    [ "$(tail -1 "$f")" = "exit=0" ] || { echo "non-zero exit: $(basename "$f"): $(tail -1 "$f")"; BAD=1; }
done
#the extended list has runs that are meant to fail (usage errors) -> their exit codes have to match the reference
if [ -d "$REF/dev/extended" ]; then
    for f in "$OUT"/extended/*.txt; do
        name=$(basename "$f")
        case "$name" in _*) continue ;; esac
        [ "$(tail -1 "$f")" = "$(tail -1 "$REF/dev/extended/$name")" ] || { echo "exit code differs from the reference: $name"; BAD=1; }
    done
else
    echo "no reference at $REF/dev/extended -> exit codes of the extended runs not compared"
fi
#sed drops the trailing exit= line so the log matches the baseline report
sed '$d' "$OUT/log/olddata_report.txt" | cmp -s - tests/baseline/runall_olddata.txt || { echo "old-data report differs from tests/baseline/runall_olddata.txt"; BAD=1; }
grep -q "^RESULT: PASS" "$OUT/log/verify_original.txt" || { echo "verify (original pairs) did not pass"; BAD=1; }
if [ $QUICK -eq 0 ]; then
    cmp -s "$OUT/dump.txt" tests/baseline/dump_dev_O0.txt || { echo "dump differs from tests/baseline/dump_dev_O0.txt"; BAD=1; }
    grep -q "^RESULT: PASS" "$OUT/log/verify_baseline.txt" || { echo "verify (baseline pairs) did not pass"; BAD=1; }
fi

N=$(ls "$OUT"/log/*.txt "$OUT"/extended/*.txt | wc -l | tr -d ' ')
if [ $BAD -ne 0 ]; then echo "check_sanitizers: FAIL (outputs kept in $OUT)"; exit 1; fi
echo "check_sanitizers: PASS -> $N runs, no AddressSanitizer or UndefinedBehaviorSanitizer report, legacy outputs identical to tests/baseline"
grep -h "single run" "$OUT"/log/verify_*.txt
