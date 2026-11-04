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
# E2E_PYTHON needs: paho-mqtt>=2, pymodbus==3.8.6, protobuf, grpcio-tools (tools/e2e/requirements.txt).
# Host tools: mosquitto, qemu-system-xtensa (idf_tools.py install qemu-xtensa; needs libslirp0).
set -euo pipefail
: "${IDF_PATH:?source the ESP-IDF export.sh first}"
IDF_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${E2E_WORKDIR:-$(mktemp -d -t gw-e2e-XXXX)}"
PY="${E2E_PYTHON:-python3}"
mkdir -p "$WORK"
cd "$IDF_DIR"

BUILD=build-qemu
DEFAULTS="sdkconfig.defaults;sdkconfig.qemu"
EXTRA=()
if [ "${E2E_PSRAM:-0}" = 1 ]; then
    BUILD=build-qemu-psram
    DEFAULTS="$DEFAULTS;sdkconfig.qemu-psram"
    EXTRA=(--psram)
fi

# FW-2: the QEMU builds require signed images (no eFuse involved). Their throwaway test key
# (CONFIG_SECURE_BOOT_SIGNING_KEY in sdkconfig.qemu, shared by the PSRAM variant) lives in a
# build directory, never in the repository
KEY=build-qemu/e2e_signing_key.pem
mkdir -p build-qemu
[ -f "$KEY" ] || python -m espsecure generate-signing-key --version 2 --scheme rsa3072 "$KEY"
# sdkconfig.defaults/sdkconfig.qemu only apply to a fresh sdkconfig
for f in ${DEFAULTS//;/ }; do
    if [ "$f" -nt "$BUILD/sdkconfig" ]; then
        rm -f "$BUILD/sdkconfig"
    fi
done
idf.py -B "$BUILD" -D SDKCONFIG="$BUILD/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$DEFAULTS" \
    -D GW_FS_DIR="$IDF_DIR/tools/e2e/fs" reconfigure build   # reconfigure: picks up new Kconfig files

(cd "$BUILD" && python -m esptool --chip esp32s3 merge-bin --output "$WORK/flash.bin" \
    --pad-to-size 16MB @flash_args)

# Issue #94: the firmware before an OTA update to NVS encryption, the same QEMU build without it
# (sdkconfig.qemu-plain, build dir build-qemu-plain; without PSRAM in both runs). E2E_PLAIN=0 skips
# it (and the scenario that needs it). JOBS: its build's parallel jobs.
PLAIN_ARGS=()
if [ "${E2E_PLAIN:-1}" = 1 ]; then
    PLAIN=build-qemu-plain
    PDEFAULTS="sdkconfig.defaults;sdkconfig.qemu;sdkconfig.qemu-plain"
    for f in ${PDEFAULTS//;/ }; do
        if [ "$f" -nt "$PLAIN/sdkconfig" ]; then
            rm -f "$PLAIN/sdkconfig"
        fi
    done
    idf.py -B "$PLAIN" -D SDKCONFIG="$PLAIN/sdkconfig" -D "SDKCONFIG_DEFAULTS=$PDEFAULTS" \
        -D GW_FS_DIR="$IDF_DIR/tools/e2e/fs" reconfigure
    cmake --build "$PLAIN" -j "${JOBS:-$(nproc)}"
    grep -q '^# CONFIG_NVS_ENCRYPTION is not set' "$PLAIN/sdkconfig"
    (cd "$PLAIN" && python -m esptool --chip esp32s3 merge-bin --output "$WORK/flash-plain.bin" \
        --pad-to-size 16MB @flash_args)
    PLAIN_ARGS=(--plain-flash "$WORK/flash-plain.bin")
fi
python - "$WORK/efuse.bin" <<'PYEOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["IDF_PATH"], "tools"))
from idf_py_actions.qemu_ext import QEMU_TARGETS
open(sys.argv[1], "wb").write(QEMU_TARGETS["esp32s3"].default_efuse)
PYEOF

"$PY" tools/e2e/e2e.py --qemu "$(command -v qemu-system-xtensa)" \
    --flash "$WORK/flash.bin" --efuse "$WORK/efuse.bin" --workdir "$WORK" \
    --littlefs-python "$BUILD/littlefs_py_venv/bin/python" --sdkconfig "$BUILD/sdkconfig" \
    --elf "$BUILD/rs485_spb_gateway.elf" --idf-python "$(command -v python)" \
    --signing-key "$KEY" ${PLAIN_ARGS[@]+"${PLAIN_ARGS[@]}"} ${EXTRA[@]+"${EXTRA[@]}"} "$@"
