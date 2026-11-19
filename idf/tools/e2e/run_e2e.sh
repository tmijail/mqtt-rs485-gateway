#!/usr/bin/env bash
# QA-3: builds the QEMU variant of the firmware and runs the end-to-end scenario
# (tools/e2e/e2e.py) against mosquitto and a pymodbus RTU slave.
#
#   . $IDF_PATH/export.sh
#   E2E_PYTHON=/path/to/venv/bin/python tools/e2e/run_e2e.sh
#
# E2E_PSRAM=1 (issue #63): the variant with the board's octal PSRAM (sdkconfig.qemu-psram, build
# dir build-qemu-psram, QEMU's ssi_psram in octal mode), so the configuration, the Sparkplug
# engine and its full payload buffer live in PSRAM as on the board. e2e.py writes otadata into
# the image first (issue #85: QEMU crashes on the bootloader's first otadata write).
#
# E2E_SECURE=1 (issue #261): the secure variant. The builds above are the configuration of the
# board: no NVS encryption, no signature check of an OTA image. This variant adds
# sdkconfig.qemu-secure (NVS encryption with an HMAC key in the emulated eFuse, signed OTA images;
# build dir build-qemu-psram-secure) to the PSRAM build, and its full run has only the scenarios
# of these two functions (scenarios.py). It always has the PSRAM (E2E_PSRAM=0 is an error). The
# script also builds the board build of the PSRAM variant (build-qemu-psram): the firmware
# before the OTA update to NVS encryption (issue #94). E2E_BOARD=0 skips that build (and the
# scenario that needs it). The throwaway signing key of the secure build is
# build-qemu/e2e_signing_key.pem; the harness signs the test images with it only in this variant.
#
# E2E_POISON=1 (issue #104): the same build with comprehensive heap poisoning on top
# (sdkconfig.qemu-poison, build dir build-qemu-poison or build-qemu-psram-poison): freed memory is
# overwritten, so a use after free changes what the firmware reports. Meant for --only ota (the
# issue #98 check); its memory cost is in docs/reference/e2e-harness.md#heap-poisoning.
#
# JOBS: parallel jobs of the builds (default: every CPU).
#
# E2E_TCG_THREAD (issue #107): auto (default: the OTA scenarios with both vCPUs on one host
# thread, the rest with multi-threaded TCG), single or multi for every scenario. MTTCG crashes
# QEMU now and then under host load in the OTA scenarios (issue #242).
#
# E2E_STAGE=<dir> (issue #121): build the images as above, copy what the harness needs into <dir>
# (the merged flash, the ELF and the app image, the sdkconfig, flash_args, the littlefs-python
# requirement; with E2E_SECURE=1 also the unsigned app image, the signing key and the board build
# in <dir>/board) and stop: no scenario runs. E2E_PREBUILT=<dir>: no build; run the harness on the
# images of such a <dir>. Its E2E_PSRAM, E2E_POISON and E2E_SECURE must be the ones of the build;
# with E2E_SECURE=1, E2E_BOARD=1 (the default) requires <dir>/board. The CI builds once per
# variant and runs its groups (scenarios.py) from the images.
#
# E2E_TCK_DIR=<dir> (issue #176): Java, HiveMQ CE and the Eclipse Sparkplug TCK, as
# tools/e2e/tck_setup.sh <dir> installs them. Only for --only tck (E2E_TCK_TESTS,
# E2E_TCK_PROTOCOLS: see tools/e2e/tck.py); without it that scenario shows [SKIP].
#
# E2E_DOWNGRADE_REF=<git ref> (issue #148): the optional scenario "downgrade" (--only downgrade)
# boots the image of that earlier release on the flash that the image under test wrote. The
# script builds it once, with the overlays of this variant and the sources of the ref, into
# ${E2E_DOWNGRADE_DIR:-<repo>/build-downgrade}/<commit>/idf/<build dir>. Without it that scenario
# shows [SKIP]. A ref from before issue #261 has the two functions of E2E_SECURE=1 in its
# sdkconfig.qemu: without E2E_SECURE=1 the script turns them off with one more overlay, so that
# the earlier image has the configuration of the board too.
#
# E2E_PYTHON needs: paho-mqtt>=2, pymodbus==3.8.6, protobuf, grpcio-tools (tools/e2e/requirements.txt).
# Host tools: mosquitto, qemu-system-xtensa (idf_tools.py install qemu-xtensa; needs libslirp0).
set -euo pipefail
: "${IDF_PATH:?source the ESP-IDF export.sh first}"
IDF_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${E2E_WORKDIR:-$(mktemp -d -t gw-e2e-XXXX)}"
PY="${E2E_PYTHON:-python3}"
mkdir -p "$WORK"
STAGE="${E2E_STAGE:-}"
PRE="${E2E_PREBUILT:-}"
if [ -n "$STAGE" ] && [ -n "$PRE" ]; then
    echo "E2E_STAGE and E2E_PREBUILT exclude each other" >&2
    exit 2
fi
[ -z "$STAGE" ] || STAGE="$(mkdir -p "$STAGE" && cd "$STAGE" && pwd)"
[ -z "$PRE" ] || PRE="$(cd "$PRE" && pwd)"
SECURE="${E2E_SECURE:-0}"
PSRAM="${E2E_PSRAM:-0}"
if [ "$SECURE" = 1 ]; then
    # issue #261: the secure variant has the memory configuration of the board (#135)
    if [ "${E2E_PSRAM:-1}" != 1 ]; then
        echo "E2E_SECURE=1 always has the PSRAM: E2E_PSRAM=0 does not go with it" >&2
        exit 2
    fi
    PSRAM=1
fi
# what the images were built for: a prebuilt directory must match the run
VARIANT="psram=$PSRAM poison=${E2E_POISON:-0} secure=$SECURE"
cd "$IDF_DIR"

BUILD=build-qemu
DEFAULTS="sdkconfig.defaults;sdkconfig.qemu"
EXTRA=()
if [ "$PSRAM" = 1 ]; then
    BUILD=build-qemu-psram
    DEFAULTS="$DEFAULTS;sdkconfig.qemu-psram"
    EXTRA=(--psram)
fi
# Issue #261: the board build of this variant (no poisoning): in the secure variant, the firmware
# before the OTA update to NVS encryption (issue #94)
BOARD_BUILD="$BUILD"
BOARD_DEFAULTS="$DEFAULTS"
BOARD=0
if [ "$SECURE" = 1 ]; then
    BUILD="$BUILD-secure"
    DEFAULTS="$DEFAULTS;sdkconfig.qemu-secure"
    EXTRA+=(--secure)
    BOARD="${E2E_BOARD:-1}"
fi
if [ "${E2E_POISON:-0}" = 1 ]; then
    BUILD="$BUILD-poison"
    DEFAULTS="$DEFAULTS;sdkconfig.qemu-poison"
fi
# FW-2: only the secure build requires signed images (no eFuse involved). Its throwaway test key
# (CONFIG_SECURE_BOOT_SIGNING_KEY in sdkconfig.qemu-secure) lives in a build directory, never in
# the repository
KEY=build-qemu/e2e_signing_key.pem

build_images() {   # <build dir> <SDKCONFIG_DEFAULTS>
    local dir=$1 defaults=$2 f
    # sdkconfig.defaults and the overlays only apply to a fresh sdkconfig
    for f in ${defaults//;/ }; do
        if [ "$f" -nt "$dir/sdkconfig" ]; then
            rm -f "$dir/sdkconfig"
        fi
    done
    idf.py -B "$dir" -D SDKCONFIG="$dir/sdkconfig" -D "SDKCONFIG_DEFAULTS=$defaults" \
        -D GW_FS_DIR="$IDF_DIR/tools/e2e/fs" reconfigure   # picks up new Kconfig files
    cmake --build "$dir" -j "${JOBS:-$(nproc)}"
}

# Issue #261: the two functions of the secure variant are in its sdkconfig, and in no other one
# (a stale sdkconfig, or an overlay that brings them back, must not pass as the board build)
check_functions() {   # <sdkconfig> <1: the secure build, 0: a board build>
    local o
    for o in NVS_ENCRYPTION SECURE_SIGNED_APPS_NO_SECURE_BOOT; do
        if [ "$2" = 1 ]; then
            grep -q "^CONFIG_$o=y" "$1" || { echo "$1: CONFIG_$o is not set" >&2; exit 2; }
        else
            grep -q "^# CONFIG_$o is not set" "$1" || {
                echo "$1: CONFIG_$o is set; only E2E_SECURE=1 has it (issue #261)" >&2; exit 2; }
        fi
    done
}

if [ -z "$PRE" ]; then
    if [ "$SECURE" = 1 ]; then
        mkdir -p build-qemu
        [ -f "$KEY" ] || python -m espsecure generate-signing-key --version 2 --scheme rsa3072 "$KEY"
    fi
    build_images "$BUILD" "$DEFAULTS"

    OUT="${STAGE:-$WORK}"
    (cd "$BUILD" && python -m esptool --chip esp32s3 merge-bin --output "$OUT/flash.bin" \
        --pad-to-size 16MB @flash_args)
    SDKCONFIG="$BUILD/sdkconfig"
    ELF="$BUILD/rs485_spb_gateway.elf"
    LFS_PY="$BUILD/littlefs_py_venv/bin/python"

    # Issues #94 and #261: the firmware before an OTA update to NVS encryption is the board build
    # of this variant. E2E_BOARD=0 skips it (and the scenario that needs it).
    BOARD_ARGS=()
    if [ "$BOARD" = 1 ]; then
        build_images "$BOARD_BUILD" "$BOARD_DEFAULTS"
        check_functions "$BOARD_BUILD/sdkconfig" 0
        BOUT="$OUT"
        [ -z "$STAGE" ] || { BOUT="$STAGE/board"; mkdir -p "$BOUT"; }
        (cd "$BOARD_BUILD" && python -m esptool --chip esp32s3 merge-bin \
            --output "$BOUT/flash-board.bin" --pad-to-size 16MB @flash_args)
        # issue #102: its app image too, the update a migrated device must refuse; its ELF for
        # the fault monitor's symbols (issue #92)
        BOARD_ARGS=(--board-flash "$BOUT/flash-board.bin"
                    --board-app "$IDF_DIR/$BOARD_BUILD/rs485_spb_gateway.bin"
                    --board-elf "$IDF_DIR/$BOARD_BUILD/rs485_spb_gateway.elf")
    fi
    if [ -n "$STAGE" ]; then
        # issue #121: the images for the runs without a build (E2E_PREBUILT); the ELF next to
        # the app image (the harness derives its name from it)
        cp "$SDKCONFIG" "$ELF" "$BUILD/rs485_spb_gateway.bin" "$BUILD/flash_args" "$STAGE/"
        if [ "$SECURE" = 1 ]; then
            cp "$BUILD/rs485_spb_gateway-unsigned.bin" "$STAGE/"
            cp "$KEY" "$STAGE/e2e_signing_key.pem"
        fi
        "$LFS_PY" -m pip freeze > "$STAGE/littlefs-requirements.txt"
        if [ "$BOARD" = 1 ]; then
            cp "$BOARD_BUILD/rs485_spb_gateway.bin" "$BOARD_BUILD/rs485_spb_gateway.elf" \
                "$STAGE/board/"
        fi
        check_functions "$SDKCONFIG" "$SECURE"
        echo "$VARIANT" > "$STAGE/variant"
        echo "images for E2E_PREBUILT ($VARIANT) in $STAGE"
        exit 0
    fi
else
    NEED=(variant flash.bin sdkconfig rs485_spb_gateway.elf rs485_spb_gateway.bin
          littlefs-requirements.txt)
    [ "$SECURE" != 1 ] || NEED+=(rs485_spb_gateway-unsigned.bin e2e_signing_key.pem)
    for f in "${NEED[@]}"; do
        [ -f "$PRE/$f" ] || { echo "E2E_PREBUILT: $PRE/$f is missing" >&2; exit 2; }
    done
    if [ "$(cat "$PRE/variant")" != "$VARIANT" ]; then
        echo "E2E_PREBUILT: $PRE holds the images for '$(cat "$PRE/variant")', not '$VARIANT'" >&2
        exit 2
    fi
    cp "$PRE/flash.bin" "$WORK/flash.bin"   # the harness writes into the image under test
    SDKCONFIG="$PRE/sdkconfig"
    ELF="$PRE/rs485_spb_gateway.elf"
    KEY="$PRE/e2e_signing_key.pem"
    # the build's littlefs-python (the harness makes the configuration variants with it)
    LFS_PY="$WORK/littlefs_py_venv/bin/python"
    if [ ! -x "$LFS_PY" ]; then
        python -m venv "$WORK/littlefs_py_venv"
        "$LFS_PY" -m pip install -q -r "$PRE/littlefs-requirements.txt"
    fi
    BOARD_ARGS=()
    if [ "$BOARD" = 1 ]; then
        for f in flash-board.bin rs485_spb_gateway.bin rs485_spb_gateway.elf; do
            [ -f "$PRE/board/$f" ] || {
                echo "E2E_PREBUILT: $PRE/board/$f is missing (E2E_BOARD=0 runs without it)" >&2
                exit 2; }
        done
        cp "$PRE/board/flash-board.bin" "$WORK/flash-board.bin"
        BOARD_ARGS=(--board-flash "$WORK/flash-board.bin"
                    --board-app "$PRE/board/rs485_spb_gateway.bin"
                    --board-elf "$PRE/board/rs485_spb_gateway.elf")
    fi
fi
check_functions "$SDKCONFIG" "$SECURE"
if [ "${E2E_POISON:-0}" = 1 ]; then
    grep -q '^CONFIG_HEAP_POISONING_COMPREHENSIVE=y' "$SDKCONFIG"
fi
# Issue #148: the app image of an earlier release, built from its own sources (a worktree of the
# ref) with the overlays of this variant. The secure build signs its images: the same throwaway
# key. Issue #261: a ref from before that issue has no sdkconfig.qemu-secure, and its
# sdkconfig.qemu has the two functions of the secure variant. Then the secure variant builds the
# ref without that overlay, and the other variants turn the two functions off with one more
# overlay: the earlier image has the configuration of the image under test.
DOWNGRADE_ARGS=()
if [ -n "${E2E_DOWNGRADE_REF:-}" ] && [ -z "$STAGE" ]; then
    REPO="$(cd "$IDF_DIR/.." && pwd)"
    DSHA="$(git -C "$REPO" rev-parse --verify "${E2E_DOWNGRADE_REF}^{commit}")"
    DDIR="${E2E_DOWNGRADE_DIR:-$REPO/build-downgrade}/${DSHA:0:12}"
    DAPP="$DDIR/idf/$BUILD/rs485_spb_gateway.bin"
    if [ ! -f "$DAPP" ]; then
        if [ ! -f "$DDIR/idf/CMakeLists.txt" ]; then   # the sources, once for all the variants
            rm -rf "$DDIR"
            git -C "$REPO" worktree prune
            mkdir -p "$(dirname "$DDIR")"
            # a git worktree, not an archive: the build takes its version (the hash) from git
            git -C "$REPO" worktree add --detach "$DDIR" "$DSHA"
        fi
        mkdir -p "$DDIR/idf/build-qemu"
        DDEFAULTS="$DEFAULTS"
        if [ ! -f "$DDIR/idf/sdkconfig.qemu-secure" ]; then
            DDEFAULTS="${DDEFAULTS//;sdkconfig.qemu-secure/}"
            if [ "$SECURE" != 1 ]; then
                printf '%s\n' "CONFIG_NVS_ENCRYPTION=n" \
                    "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=n" \
                    > "$DDIR/idf/build-qemu/sdkconfig.e2e-board"
                DDEFAULTS="$DDEFAULTS;build-qemu/sdkconfig.e2e-board"
            fi
        fi
        [ "$SECURE" != 1 ] || cp "$KEY" "$DDIR/idf/build-qemu/e2e_signing_key.pem"
        (cd "$DDIR/idf" && idf.py -B "$BUILD" -D SDKCONFIG="$BUILD/sdkconfig" \
            -D "SDKCONFIG_DEFAULTS=$DDEFAULTS" -D GW_FS_DIR="$DDIR/idf/tools/e2e/fs" reconfigure \
            && cmake --build "$BUILD" -j "${JOBS:-$(nproc)}")
        check_functions "$DDIR/idf/$BUILD/sdkconfig" "$SECURE"
    fi
    echo "issue #148: the earlier release $E2E_DOWNGRADE_REF ($DSHA): $DAPP"
    DOWNGRADE_ARGS=(--downgrade-app "$DAPP" --downgrade-ref "$E2E_DOWNGRADE_REF (${DSHA:0:12})")
fi
# FW-2: the harness signs its test images only for the build that checks the signature
SIGN_ARGS=()
[ "$SECURE" != 1 ] || SIGN_ARGS=(--signing-key "$KEY")
python - "$WORK/efuse.bin" <<'PYEOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["IDF_PATH"], "tools"))
from idf_py_actions.qemu_ext import QEMU_TARGETS
open(sys.argv[1], "wb").write(QEMU_TARGETS["esp32s3"].default_efuse)
PYEOF

"$PY" tools/e2e/e2e.py --qemu "$(command -v qemu-system-xtensa)" \
    --flash "$WORK/flash.bin" --efuse "$WORK/efuse.bin" --workdir "$WORK" \
    --littlefs-python "$LFS_PY" --sdkconfig "$SDKCONFIG" \
    --elf "$ELF" --idf-python "$(command -v python)" \
    ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"} ${BOARD_ARGS[@]+"${BOARD_ARGS[@]}"} \
    ${EXTRA[@]+"${EXTRA[@]}"} ${DOWNGRADE_ARGS[@]+"${DOWNGRADE_ARGS[@]}"} "$@"
