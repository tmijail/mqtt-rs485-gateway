#!/usr/bin/env bash
# Issue #226: run the libFuzzer targets of gw_core, $JOBS at a time (default: the CPU count).
#
#   idf/test/fuzz/run_fuzz.sh <build dir> <seconds per target> [target ...]
#
# <build dir> is a build of idf/test/fuzz (CMakeLists.txt there). Each target fuzzes for the given
# time on <build dir>/corpus/<target> (new inputs go there) and its seeds <build dir>/seeds/<target>.
# A crash, a leak or a timeout of one input is saved as <build dir>/crashes/<target>-<kind>-<hash>,
# and the script then exits with 1. The log of each target is <build dir>/logs/<target>.log. The
# script writes one Markdown table line per target to stdout (executions, coverage, corpus).
#
# To run one input again (a crash, or a regression test):  <build dir>/<target> <file>
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <build dir> <seconds per target> [target ...]" >&2
    exit 2
fi
build=$(cd "$1" && pwd)
secs=$2
shift 2
here=$(cd "$(dirname "$0")" && pwd)
targets=("$@")
if [ ${#targets[@]} -eq 0 ]; then
    targets=(fuzz_mb_rtu fuzz_mbap fuzz_spb fuzz_node fuzz_config fuzz_cfgdoc fuzz_web_json
             fuzz_web_text fuzz_flash fuzz_nvs fuzz_text fuzz_sfspill fuzz_captive)
fi
jobs=${JOBS:-$(nproc)}
mkdir -p "$build/corpus" "$build/crashes" "$build/logs"

# The options of one target: its dictionary and the largest input (a card image of fuzz_sfspill
# is 17409 bytes, a raw NVS partition of fuzz_nvs is 4 pages, a configuration up to 32 KiB).
options() {
    case $1 in
    fuzz_config|fuzz_cfgdoc) echo "-dict=$here/json.dict -max_len=32768" ;;
    fuzz_web_json) echo "-dict=$here/json.dict -max_len=4096" ;;
    fuzz_spb|fuzz_node) echo "-dict=$here/spb.dict -max_len=8192" ;;
    fuzz_nvs) echo "-max_len=16400" ;;
    fuzz_sfspill) echo "-max_len=17409" ;;
    *) echo "-max_len=4096" ;;
    esac
}

# One target; its exit code goes to logs/<target>.rc (libFuzzer: 0, or not 0 after a finding).
run_one() {
    local t=$1 rc=0
    mkdir -p "$build/corpus/$t"
    # shellcheck disable=SC2046  # the options are words
    "$build/$t" $(options "$t") -max_total_time="$secs" -timeout=10 -rss_limit_mb=2048 \
        -print_final_stats=1 -artifact_prefix="$build/crashes/$t-" \
        "$build/corpus/$t" "$build/seeds/$t" > "$build/logs/$t.log" 2>&1 || rc=$?
    echo "$rc" > "$build/logs/$t.rc"
}

# $jobs targets at a time
for t in "${targets[@]}"; do
    while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do
        wait -n || true
    done
    run_one "$t" &
done
wait

rc=0
# FUZZ_MERGE=1 (the weekly CI job): keep only the inputs that add coverage, so that the corpus
# that the CI keeps does not grow without limit
if [ "${FUZZ_MERGE:-0}" = 1 ]; then
    for t in "${targets[@]}"; do
        rm -rf "$build/corpus.min/$t"
        mkdir -p "$build/corpus.min/$t"
        # shellcheck disable=SC2046  # the options are words
        "$build/$t" $(options "$t") -merge=1 -timeout=10 -rss_limit_mb=2048 \
            "$build/corpus.min/$t" "$build/corpus/$t" > "$build/logs/$t.merge.log" 2>&1 || rc=1
        rm -rf "$build/corpus/$t"
        mv "$build/corpus.min/$t" "$build/corpus/$t"
    done
    rm -rf "$build/corpus.min"
fi

echo "| Target | Time (s) | Executions | Coverage (edges) | Corpus (inputs) | Result |"
echo "|---|---|---|---|---|---|"
for t in "${targets[@]}"; do
    log=$build/logs/$t.log
    # a target that stopped early can have no statistics: "?"
    execs=$(sed -n 's/^stat::number_of_executed_units: *//p' "$log" 2>/dev/null | tail -1 || true)
    cov=$(grep -oE 'cov: [0-9]+' "$log" 2>/dev/null | tail -1 | cut -d' ' -f2 || true)
    corpus=$(find "$build/corpus/$t" -type f 2>/dev/null | wc -l || true)
    if [ "$(cat "$build/logs/$t.rc" 2>/dev/null)" != 0 ]; then
        result="FAILED (see logs/$t.log)"
        rc=1
    else
        result=passed
    fi
    echo "| $t | $secs | ${execs:-?} | ${cov:-?} | $corpus | $result |"
done
exit "$rc"
