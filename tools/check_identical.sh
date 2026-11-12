#!/bin/sh
#re-runs the development-mode reproduction and diffs every output against a saved reference byte for byte (--save-reference makes it), never unseals the hold-out
set -u
cd "$(dirname "$0")/.."
REF="${BACKTESTER_REFERENCE:-../BackTester_reference}"
MODE=check
[ "${1:-}" = "--save-reference" ] && MODE=save
BIN=./build/BackTester
WORK=$(mktemp -d /tmp/backtester_check.XXXXXX)
GOT="$WORK/got"
mkdir -p "$GOT/side"

#timing lines keep their words, only the measured numbers bceome placeholders
mask_timing() {
    LC_ALL=C sed -E \
        -e '/pair-bars\/s|^load csv/s/[0-9]+\.[0-9]+ s/<T> s/g' \
        -e '/pair-bars\/s/s/\([0-9.]+-[0-9.]+\)/(<T>-<T>)/g' \
        -e '/pair-bars\/s/s/-> [0-9]+ pair-bars\/s/-> <R> pair-bars\/s/g' \
        "$1"
}

fail() { echo "check_identical: FAIL -> $1"; echo "(work folder kept: $WORK)"; exit 1; }

touch "$WORK/marker"
sleep 1 #so every file the run writes is strictly newer than the marker

echo "check_identical: tools/reproduce.sh (development mode) ..."
tools/reproduce.sh > "$WORK/console_raw.txt" 2>&1 || { tail -20 "$WORK/console_raw.txt"; fail "tools/reproduce.sh exited non-zero"; }
echo "check_identical: verify + bench on the baseline pair list ..."
$BIN verify tests/baseline/pairs_baseline.csv > "$WORK/verify_raw.txt" 2>&1 || fail "verify exited non-zero"
$BIN bench tests/baseline/pairs_baseline.csv --legacy > "$WORK/bench_raw.txt" 2>&1 || fail "bench exited non-zero"

mask_timing "$WORK/console_raw.txt" > "$GOT/console.txt"
mask_timing "$WORK/verify_raw.txt" > "$GOT/verify_baseline.txt"
mask_timing "$WORK/bench_raw.txt" > "$GOT/bench_legacy.txt"

#every file the run  wrote under Data/Output -> the list has to match too, not just the contents
find Data/Output -type f -newer "$WORK/marker" ! -name .DS_Store | LC_ALL=C sort > "$GOT/written_files.txt"

#files reproduce.sh writes outside Data/Output -> the regression dumps, the engine traces, the rebuilt pair list
cp /tmp/backtester_report_olddata.txt /tmp/backtester_dump.txt /tmp/backtester_replay.log "$GOT/side/"
cp -R /tmp/backtester_trace "$GOT/side/trace"
cp tools/sp500_groups.csv tools/sp500_members.csv Data/Input/stooq/pairs.csv "$GOT/side/"

echo "check_identical: tools/check_extended.sh (every other command and flag, sealed) ..."
tools/check_extended.sh "$WORK/extended_raw" > "$WORK/extended_log.txt" 2>&1 || { tail -20 "$WORK/extended_log.txt"; fail "tools/check_extended.sh exited non-zero"; }
mkdir -p "$GOT/extended"
for f in "$WORK"/extended_raw/*; do mask_timing "$f" > "$GOT/extended/$(basename "$f")"; done

if [ "$MODE" = save ]; then
    [ -e "$REF/dev" ] && fail "$REF/dev already exists, not overwriting a reference"
    mkdir -p "$REF/dev/files"
    cp -R "$GOT/." "$REF/dev/"
    while read -r f; do mkdir -p "$REF/dev/files/$(dirname "$f")"; cp "$f" "$REF/dev/files/$f"; done < "$GOT/written_files.txt"
    cp "$WORK/bench_raw.txt" "$REF/bench_legacy_before_raw.txt" #the before-speed, timings left in
    cp "$WORK/console_raw.txt" "$REF/dev/console_raw.txt"
    echo "check_identical: reference saved in $REF/dev ($(wc -l < "$GOT/written_files.txt" | tr -d ' ') output files)"
    rm -rf "$WORK"
    exit 0
fi

[ -d "$REF/dev" ] || fail "no reference at $REF/dev (run with --save-reference on untouched code first)"
BAD=0
for f in console.txt verify_baseline.txt bench_legacy.txt written_files.txt; do
    if ! diff "$REF/dev/$f" "$GOT/$f" > "$WORK/diff_$f" 2>&1; then
        echo "DIFFERS: $f"; head -20 "$WORK/diff_$f"; BAD=$((BAD + 1))
    fi
done
if ! diff -r "$REF/dev/side" "$GOT/side" > "$WORK/diff_side.txt" 2>&1; then
    echo "DIFFERS: side files"; head -20 "$WORK/diff_side.txt"; BAD=$((BAD + 1))
fi
if ! diff -r "$REF/dev/extended" "$GOT/extended" > "$WORK/diff_extended.txt" 2>&1; then
    echo "DIFFERS: extended runs"; grep -E "^(diff|Only)" "$WORK/diff_extended.txt" | sed 's|.*/extended/||' | head -20; head -12 "$WORK/diff_extended.txt"; BAD=$((BAD + 1))
fi
N=0
while read -r f; do
    N=$((N + 1))
    if ! cmp -s "$REF/dev/files/$f" "$f"; then echo "DIFFERS: $f"; BAD=$((BAD + 1)); fi
done < "$REF/dev/written_files.txt"

if [ $BAD -ne 0 ]; then fail "$BAD output(s) differ from the reference"; fi
E=$(ls "$GOT/extended" | wc -l | tr -d ' ')
echo "check_identical: PASS -> console, verify, bench, $N output files, the side files and $E extended runs are byte-identical to the reference (timing numbers aside)"
#bench speed lines for the record
grep -E "^(engine|incl\. reading files|legacy pipeline)" "$WORK/bench_raw.txt"
rm -rf "$WORK"
