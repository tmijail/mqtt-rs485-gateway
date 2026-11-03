#!/usr/bin/env bash
# QA-3: builds the QEMU variant of the firmware and runs the end-to-end scenario
# (tools/e2e/e2e.py) against mosquitto and a pymodbus RTU slave.
#
#   . $IDF_PATH/export.sh
#   E2E_PYTHON=/path/to/venv/bin/python tools/e2e/run_e2e.sh
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

# sdkconfig.defaults/sdkconfig.qemu only apply to a fresh sdkconfig
if [ sdkconfig.defaults -nt build-qemu/sdkconfig ] || [ sdkconfig.qemu -nt build-qemu/sdkconfig ]; then
    rm -f build-qemu/sdkconfig
fi
idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig \
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu" \
    -D GW_FS_DIR="$IDF_DIR/tools/e2e/fs" build

(cd build-qemu && python -m esptool --chip esp32s3 merge-bin --output "$WORK/flash.bin" \
    --pad-to-size 16MB @flash_args)
python - "$WORK/efuse.bin" <<'PYEOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["IDF_PATH"], "tools"))
from idf_py_actions.qemu_ext import QEMU_TARGETS
open(sys.argv[1], "wb").write(QEMU_TARGETS["esp32s3"].default_efuse)
PYEOF

"$PY" tools/e2e/e2e.py --qemu "$(command -v qemu-system-xtensa)" \
    --flash "$WORK/flash.bin" --efuse "$WORK/efuse.bin" --workdir "$WORK"
