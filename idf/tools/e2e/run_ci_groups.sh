#!/usr/bin/env bash
# Issue #121: the CI's E2E groups (scenarios.py CI_GROUPS) on this machine, the way the CI runs
# them: build the images of one variant once (run_e2e.sh with E2E_STAGE), then run the groups
# from those images (E2E_PREBUILT), PARALLEL at a time, each in its own work directory (and its
# own network namespace when the harness can make one). At the end, the CI's summary table.
#
#   . $IDF_PATH/export.sh
#   E2E_PYTHON=~/e2e/bin/python tools/e2e/run_ci_groups.sh /tmp/gw-groups            # all groups
#   E2E_PSRAM=1 E2E_PYTHON=... tools/e2e/run_ci_groups.sh /tmp/gw-groups ota-fw1-web other
#
# Issue #135: each variant has its own groups (the PSRAM variant runs most scenarios, the
# variant without PSRAM only the OTA trial scenarios); run the script once for each variant.
#
# Each group writes <dir>/e2e-result-<variant>-<group>/ (its E2E_WORKDIR: results.json,
# qemu-exits.txt, logs) and <dir>/<variant>-<group>.out (its console output). PARALLEL (default
# 2): groups at the same time; QEMU uses up to 2 host threads per group, and a loaded host makes
# the timing checks slower. E2E_SKIP_BUILD=1: use the images already in <dir>/images-<variant>.
# Other variables (E2E_TIMEOUT_SCALE, E2E_TCG_THREAD, JOBS) go to run_e2e.sh.
set -euo pipefail
: "${IDF_PATH:?source the ESP-IDF export.sh first}"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ $# -ge 1 ] || { sed -n '2,15p' "$0" >&2; exit 2; }
DIR="$(mkdir -p "$1" && cd "$1" && pwd)"
shift
if [ "${E2E_PSRAM:-0}" = 1 ]; then VARIANT=psram; else VARIANT=default; fi
IMAGES="$DIR/images-$VARIANT"

# the selected groups: id, --only, needs the build without NVS encryption
mapfile -t ROWS < <(python3 "$HERE/scenarios.py" groups "$VARIANT")
SEL=()
PLAIN=0
for r in "${ROWS[@]}"; do
    read -r id only plain <<< "$r"
    if [ $# -eq 0 ] || [[ " $* " == *" $id "* ]]; then
        SEL+=("$r")
        [ "$plain" = 0 ] || PLAIN=1
    fi
done
[ ${#SEL[@]} -gt 0 ] || { echo "no such group: $* (scenarios.py groups)" >&2; exit 2; }

if [ "${E2E_SKIP_BUILD:-0}" != 1 ]; then
    rm -rf "$IMAGES"
    E2E_STAGE="$IMAGES" E2E_PLAIN="$PLAIN" E2E_WORKDIR="$DIR/build-work" "$HERE/run_e2e.sh"
fi

run_group() {   # its exit status in <dir>/<variant>-<group>.rc
    local id=$1 only=$2 plain=$3 work="$DIR/e2e-result-$VARIANT-$1" rc=0
    rm -rf "$work"
    E2E_PREBUILT="$IMAGES" E2E_PLAIN="$plain" E2E_WORKDIR="$work" \
        "$HERE/run_e2e.sh" --only "$only" > "$DIR/$VARIANT-$id.out" 2>&1 || rc=$?
    echo "$rc" > "$DIR/$VARIANT-$id.rc"
}
for r in "${SEL[@]}"; do
    read -r id only plain <<< "$r"
    while [ "$(jobs -rp | wc -l)" -ge "${PARALLEL:-2}" ]; do
        wait -n || true
    done
    echo "group $id (--only $only) -> $DIR/$VARIANT-$id.out"
    rm -f "$DIR/$VARIANT-$id.rc"
    run_group "$id" "$only" "$plain" &
done
wait
status=0
for r in "${SEL[@]}"; do
    read -r id _ _ <<< "$r"
    if [ "$(cat "$DIR/$VARIANT-$id.rc" 2>/dev/null)" != 0 ]; then
        echo "group $id failed (see $DIR/$VARIANT-$id.out)"
        status=1
    fi
done

echo
if [ $# -eq 0 ]; then
    # every group of the variant: the CI's checks (coverage, retries over the whole run)
    python3 "$HERE/scenarios.py" summary "$DIR" "$VARIANT" || status=1
else
    header=--header
    for r in "${SEL[@]}"; do
        read -r id _ _ <<< "$r"
        python3 "$HERE/scenarios.py" line $header "$VARIANT" "$id" "$DIR/e2e-result-$VARIANT-$id"
        header=
    done
fi
exit $status
