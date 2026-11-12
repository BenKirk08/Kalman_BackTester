#!/bin/sh
#D26 design candidates (H0 and 7 one-change variants, data cut at 2015-12-31, arms K and S) -> tools/run_design_candidates.sh [name extra-flags] from the project folder
set -e
OUT=Data/Output/portfolio/design
BIN=./build/BackTester
PAIRS=Data/Input/stooq/pairs.csv
mkdir -p $OUT

run() { #name, then the flags that differ from H0
    name="$1"; shift
    $BIN walkforward $PAIRS --arms K,S --end 2015-12-31 --last-year 2015 --out "$OUT/$name" --quiet "$@" > /dev/null
}
#a  name plus flags runs just that one candidate
if [ -n "$1" ]; then
    run "$@"
else
    run h0
    run c1_stop_6            --stop-z 6
    run c2_entry_2.5         --entry-z 2.5
    run c3_trace_1.25x       --select-margin 1.25
    run c4_kalman_q_1e-7     --kalman-q 1e-7
    run c5_z_window_120      --z-window 120
    run c6_windows_6_months  --window-months 6
    run c7_equal_notional    --sizing gross
fi
.venv/bin/python -I tools/design_table.py $OUT
