#!/bin/sh
#H0 (all 5 arms) and the 13 pre-registered sensitivities (D9, D19) side by side -> tools/run_sensitivities.sh <outdir> [extra walkforward flags], BASE="flags" swaps the base design
set -e
OUT="$1"
if [ -z "$OUT" ]; then echo "usage: tools/run_sensitivities.sh <output folder> [--unseal-holdout]"; exit 1; fi
shift
mkdir -p "$OUT"
BIN=./build/BackTester
PAIRS=Data/Input/stooq/pairs.csv

$BIN walkforward $PAIRS $BASE --out "$OUT/h0" --quiet "$@"

run() { #name, then the one flag that differs from the base design (a later flag overrides an earlier one)
    name="$1"; shift
    $BIN walkforward $PAIRS --arms K,S $BASE --out "$OUT/$name" --quiet "$@"
}
run fill_same_close   --fill close "$@"
run cost_10bp         --cost-bp 10 "$@"
run cost_20bp         --cost-bp 20 "$@"
run borrow_0          --borrow-bp 0 "$@"
run no_point_in_time  --no-pit "$@"
run kalman_raw_prices --rebase 0 "$@"
run rebase_10         --rebase 10 "$@"
run rebase_1000       --rebase 1000 "$@"
run legs_swapped      --swap "$@"
run no_halflife_gate  --no-hl-gate "$@"
case "$BASE" in
    *"--sizing gross"*) run equal_risk --sizing risk "$@" ;; #the base is already equal notional -> show the other one
    *) run equal_notional --sizing gross "$@" ;;
esac
run no_caps           --no-caps "$@"
run windows_6_months  --window-months 6 "$@"

.venv/bin/python -I tools/sensitivity_table.py "$OUT"
