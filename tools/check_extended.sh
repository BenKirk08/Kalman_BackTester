#!/bin/sh
#sealed runs of every command and flag that reproduce.sh only reaches with --final (or never) -> tools/check_extended.sh <output folder>, called by check_identical.sh
set -u
PROJECT=$(cd "$(dirname "$0")/.." && pwd)
RESULTS=$1
BIN="${BACKTESTER_BIN:-$PROJECT/build/BackTester}"
#a relative BACKTESTER_BIN is read from the project root
case "$BIN" in /*) ;; *) BIN="$PROJECT/$BIN" ;; esac
PY="$PROJECT/.venv/bin/python -I"

#sandbox w/ the same relative paths as the project -> fixed output folders cant touch tracked files, fixed location keeps printed paths identical
SANDBOX="${BACKTESTER_SANDBOX:-/tmp/backtester_check_sandbox}"
rm -rf "$SANDBOX" "$RESULTS"
mkdir -p "$SANDBOX/Data/Output/montecarlo" "$SANDBOX/out" "$SANDBOX/syn" "$RESULTS"
ln -s "$PROJECT/Data/Input" "$SANDBOX/Data/Input"
ln -s "$PROJECT/tests" "$SANDBOX/tests"
ln -s "$PROJECT/tools" "$SANDBOX/tools"
ln -s "$PROJECT/analysis" "$SANDBOX/analysis"
cd "$SANDBOX"

#one  command -> one result file holding stdout, stderr and the exit code
run() {
    name="$1"; shift
    "$@" > "$RESULTS/$name.txt" 2>&1
    echo "exit=$?" >> "$RESULTS/$name.txt"
}

#---- synthetic universe -> 9 simulated pairs (2016-07 to 2026-07), plus the special cases the real list has
SYN=Data/Input/synthetic
{
    echo "label,fileA,fileB,group"
    for t in beta break corr drift fast fat indep jump slow; do
        echo "syn_$t,$SYN/syn_${t}_a_Data.csv,$SYN/syn_${t}_b_Data.csv,synthetic"
    done
    echo "syn_cross1,$SYN/syn_fast_a_Data.csv,$SYN/syn_slow_b_Data.csv,synthetic"
    echo "syn_cross2,$SYN/syn_beta_a_Data.csv,$SYN/syn_fast_b_Data.csv,synthetic"
    echo "v_ma,$SYN/syn_corr_a_Data.csv,$SYN/syn_drift_b_Data.csv,synthetic"
    echo "ctl_fast_fat,$SYN/syn_fast_a_Data.csv,$SYN/syn_fat_a_Data.csv,ctl_synthetic"
} > syn/pairs.csv
{
    echo "ticker,dateAdded,sector"
    for t in beta break corr drift fast fat indep; do
        echo "syn_${t}_a,2010-01-04,Industrials"
        echo "syn_${t}_b,2010-01-04,Industrials"
    done
    echo "syn_jump_a,2021-06-01,Industrials"
    echo "syn_jump_b,2010-01-04,Financials"
    echo "syn_slow_a,2010-01-04,Financials"
    echo "syn_slow_b,2023-03-01,Industrials"
} > syn/members.csv
{
    echo "ticker,action,date,reason"
    echo "# check_extended.sh: one late start and one dropped ticker"
    echo "syn_fat_b,start,2017-03-01,made-up late start for the check"
    echo "syn_indep_a,drop,2016-07-05,made-up drop for the check"
} > syn/fixes.csv

#--data-fixes none -> the real fix list must not touch made-up tickers
WF="$BIN walkforward syn/pairs.csv --members syn/members.csv --data-fixes none --first-year 2020"
wf() { name="$1"; shift; run "wf_$name" $WF --out "out/wf_$name" "$@"; }
wfq() { name="$1"; shift; run "wf_$name" $WF --out "out/wf_$name" --arms K,S --quiet "$@"; }

wf base
wf f_design --sizing gross --select-margin 1.25 --design-until 2021-12-31
wf fixes --data-fixes syn/fixes.csv
wf no_exclude --no-exclude
wf other_arms --arms KS,SR,ST
wf arm_names --arms kalman,static
wfq fill_close --fill close
wfq fill_open --fill open
wfq cost_10bp --cost-bp 10
wfq borrow_0 --borrow-bp 0
wfq no_pit --no-pit
wfq rebase_0 --rebase 0
wfq rebase_1000 --rebase 1000
wfq swap --swap
wfq no_hl_gate --no-hl-gate
wfq sizing_gross --sizing gross
wfq sizing_risk --sizing risk
wfq no_caps --no-caps
wfq months_6 --window-months 6
wfq stop_6 --stop-z 6
wfq entry_2.5 --entry-z 2.5
wfq kalman_q --kalman-q 1e-7
wfq z_window --z-window 120
wfq margin --select-margin 1.25
wfq end_2024 --end 2024-12-31 --last-year 2024
wfq threads_1 --threads 1
wfq threads_4 --threads 4
run wf_no_out $WF --arms K --first-year 2024
run wf_usage $BIN walkforward
run wf_bad_option $WF --bogus
run wf_bad_arm $WF --arms K,X
run wf_bad_fill $WF --fill sideways
run wf_bad_sizing $WF --sizing big
run wf_missing_value $WF --out
run wf_no_members $BIN walkforward syn/pairs.csv --first-year 2020

#---- the single-pair modes, sealed (the bulk files stop at 2022-12-31) and on the old hand-downloaded files
run report_sealed $BIN
run facts_sealed $BIN facts
run facts_olddata $BIN facts --old-data
run export_sealed $BIN export
#export writes to one fixed folder -> move it out and recreate it for the next run
mv Data/Output/montecarlo out/export_sealed && mkdir -p Data/Output/montecarlo
run export_olddata $BIN export --old-data
mv Data/Output/montecarlo out/export_olddata && mkdir -p Data/Output/montecarlo
run batch_engine $BIN batch tests/pairs_original.csv out/batch_engine.csv
run batch_engine_1 $BIN batch tests/pairs_original.csv out/batch_engine_1.csv --threads 1
run batch_legacy $BIN batch tests/pairs_original.csv out/batch_legacy.csv --legacy
run dump_end $BIN dump tests/pairs_original.csv out/dump_end.txt --end 2021-12-31
run verify_options $BIN verify tests/pairs_original.csv --all --threads 2 --wf-years 2019 2022
run bench_small $BIN bench tests/pairs_original.csv --legacy --runs 3 --threads 2
run trace_rebase $BIN trace $SYN/syn_fast_a_Data.csv $SYN/syn_fast_b_Data.csv out/trace_rebase --fill close --rebase 10
run trace_usage $BIN trace
run unknown_mode $BIN nonsense
run missing_file $BIN trace nofile_a.csv nofile_b.csv out/none
run batch_usage $BIN batch
run dump_usage $BIN dump
run verify_usage $BIN verify
run bench_usage $BIN bench

#---- the python tools that only run after the hold-out line of reproduce.sh, on the synthetic runs above
cd "$PROJECT"
O="$SANDBOX/out"
run py_compare_same $PY tools/compare_runs.py "$O/wf_fill_open" "$O/wf_threads_1"
run py_compare_until $PY tools/compare_runs.py "$O/wf_end_2024" "$O/wf_threads_4" --until 2024-12-31
run py_compare_arms $PY tools/compare_runs.py "$O/wf_base" "$O/wf_threads_1"
run py_compare_differs $PY tools/compare_runs.py "$O/wf_cost_10bp" "$O/wf_base"
run py_compare_size $PY tools/compare_runs.py "$O/wf_sizing_gross" "$O/wf_base" --ignore-size
mkdir -p "$O/sens"
#copy the runs under the names run_sensitivities.sh would give them
for name in base fill_close cost_10bp borrow_0 no_pit rebase_0 rebase_1000 swap no_hl_gate sizing_gross no_caps months_6; do
    for part in summary windows; do cp "$O/wf_${name}_$part.csv" "$O/sens/${name}_$part.csv"; done
done
for part in summary windows; do
    mv "$O/sens/base_$part.csv" "$O/sens/h0_$part.csv"
    mv "$O/sens/fill_close_$part.csv" "$O/sens/fill_same_close_$part.csv"
    mv "$O/sens/no_pit_$part.csv" "$O/sens/no_point_in_time_$part.csv"
    mv "$O/sens/rebase_0_$part.csv" "$O/sens/kalman_raw_prices_$part.csv"
    mv "$O/sens/swap_$part.csv" "$O/sens/legs_swapped_$part.csv"
    mv "$O/sens/no_hl_gate_$part.csv" "$O/sens/no_halflife_gate_$part.csv"
    mv "$O/sens/sizing_gross_$part.csv" "$O/sens/equal_notional_$part.csv"
    mv "$O/sens/months_6_$part.csv" "$O/sens/windows_6_months_$part.csv"
done
run py_sensitivity_table $PY tools/sensitivity_table.py "$O/sens"
run py_design_table $PY tools/design_table.py "$O/sens"
run py_portfolio_check $PY analysis/portfolio_check.py "$O/wf_base" --pairs "$SANDBOX/syn/pairs.csv" --fixes none
run py_portfolio_check_gross $PY analysis/portfolio_check.py "$O/wf_sizing_gross" --pairs "$SANDBOX/syn/pairs.csv" --fixes none --sizing gross
run py_portfolio_check_close $PY analysis/portfolio_check.py "$O/wf_fill_close" --pairs "$SANDBOX/syn/pairs.csv" --fixes none --fill close
run py_portfolio_check_swap $PY analysis/portfolio_check.py "$O/wf_swap" --pairs "$SANDBOX/syn/pairs.csv" --fixes none --swapped
run py_portfolio_check_caps $PY analysis/portfolio_check.py "$O/wf_no_caps" --pairs "$SANDBOX/syn/pairs.csv" --fixes none --no-caps
run py_portfolio_check_fixes $PY analysis/portfolio_check.py "$O/wf_fixes" --pairs "$SANDBOX/syn/pairs.csv" --fixes "$SANDBOX/syn/fixes.csv"
run py_monte_carlo_portfolio $PY analysis/monte_carlo.py --portfolio "$O/wf_base" --boot 400 --pnull 300
run py_import_tickers $PY tools/import_stooq.py ALLDATA --tickers xom,cvx,ko,spy,vrt,ccl --start 2005-01-01 --end 2022-12-31 --out "$O/import_tickers"
run py_import_list $PY tools/import_stooq.py ALLDATA --tickers-file tools/example_groups.csv --markets stocks --end 2022-12-31 --out "$O/import_list"
run py_import_etfs $PY tools/import_stooq.py ALLDATA --tickers spy,ivv,xom --markets etfs --end 2022-12-31 --min-rows 5000 --out "$O/import_etfs"

#the single-pair monte carlo keeps its files in a fixed folder of whatever  it thinks the project is -> run it through the sandbox link to analysis/ w/ small resample counts
cd "$SANDBOX"
run py_monte_carlo_single $PY analysis/monte_carlo.py --binary "$BIN" --boot 300 --null 200 --paths 3
mv Data/Output/montecarlo out/mc_single && mkdir -p Data/Output/montecarlo
run py_monte_carlo_olddata $PY analysis/monte_carlo.py --binary "$BIN" --boot 200 --null 100 --paths 2 --old-data --block 10 --seed 7
mv Data/Output/montecarlo out/mc_olddata && mkdir -p Data/Output/montecarlo

#every file the commands wroet goes next to the console captures. the big ones as a checksum, the small ones whole
cd "$SANDBOX"
find out -type f | LC_ALL=C sort | while read -r f; do
    case "$f" in
        *.csv|*.txt) if [ "$(wc -c < "$f")" -gt 200000 ]; then shasum -a 256 "$f"; else echo "== $f"; cat "$f"; fi ;;
        *) shasum -a 256 "$f" ;; #pictures and anything else that  is not text
    esac
done > "$RESULTS/_files_written.txt"
find out -type f | LC_ALL=C sort > "$RESULTS/_file_list.txt"
echo "check_extended: $(ls "$RESULTS" | wc -l | tr -d ' ') result files in $RESULTS"
