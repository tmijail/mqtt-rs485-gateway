#!/usr/bin/env bash
# The screenshots of the web UI in docs/images: builds the QEMU images of the E2E (run_e2e.sh with
# E2E_STAGE, the PSRAM variant: the configuration of the board) and runs tools/docs_shots.py on
# them. Runs in the gw-docs image (idf/tools/docs/Dockerfile); see
# docs/how-to/make-the-docs-screenshots.md.
#
#   . $IDF_PATH/export.sh
#   tools/docs/make_shots.sh [docs_shots.py options: --only <names>, --list, --lossless]
#
# DOCS_WORKDIR: the work directory (default: a new temporary one): the images, the logs of the
#   emulator and the real status JSON of each state (status-*.json).
# DOCS_PREBUILT=<dir>: no build; use the images of an earlier E2E_STAGE run in <dir>.
# DOCS_PSRAM=0: the QEMU build without PSRAM (faster to boot; the status shows no PSRAM).
# DOCS_LOCK=<file>: hold this lock (flock) while QEMU runs, e.g. the lock of the E2E runs on a
#   shared host. The build does not hold it.
# E2E_PYTHON: the python with the E2E packages and Playwright (the image sets it).
set -euo pipefail
: "${IDF_PATH:?source the ESP-IDF export.sh first}"
IDF_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${DOCS_WORKDIR:-$(mktemp -d -t gw-docs-XXXX)}"
PY="${E2E_PYTHON:-python3}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"

IMAGES="${DOCS_PREBUILT:-}"
if [ -z "$IMAGES" ]; then
    IMAGES="$WORK/images"
    E2E_STAGE="$IMAGES" E2E_PSRAM="${DOCS_PSRAM:-1}" E2E_POISON=0 E2E_PLAIN=0 \
        "$IDF_DIR/tools/e2e/run_e2e.sh"
fi
IMAGES="$(cd "$IMAGES" && pwd)"

# the littlefs-python of the build (the script writes config.json into the flash image)
LFS_PY="$WORK/littlefs_py_venv/bin/python"
if [ ! -x "$LFS_PY" ]; then
    python -m venv "$WORK/littlefs_py_venv"
    "$LFS_PY" -m pip install -q -r "$IMAGES/littlefs-requirements.txt"
fi
python - "$WORK/efuse.bin" <<'PYEOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["IDF_PATH"], "tools"))
from idf_py_actions.qemu_ext import QEMU_TARGETS
open(sys.argv[1], "wb").write(QEMU_TARGETS["esp32s3"].default_efuse)
PYEOF

LOCK=()
[ -z "${DOCS_LOCK:-}" ] || LOCK=(flock "$DOCS_LOCK")
${LOCK[@]+"${LOCK[@]}"} "$PY" "$IDF_DIR/tools/docs_shots.py" --images "$IMAGES" \
    --qemu "$(command -v qemu-system-xtensa)" --efuse "$WORK/efuse.bin" \
    --littlefs-python "$LFS_PY" --workdir "$WORK" "$@"
