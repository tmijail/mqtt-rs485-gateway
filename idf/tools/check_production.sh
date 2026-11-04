#!/usr/bin/env bash
# Issue #95: build and check the production configuration as the CI does, so a change that
# breaks it shows up before the factory (README 3.6, 4.5). Offline file work only: nothing here
# talks to a device or touches an eFuse.
#
#   . $IDF_PATH/export.sh && [JOBS=n] tools/check_production.sh [build-dir]   (default build-prod)
#
#  1. builds sdkconfig.defaults + sdkconfig.production (Secure Boot V2, flash encryption in
#     release mode, NVS encryption, partitions-production.csv): 0 compiler warnings;
#  2. signs it with tools/sign_firmware.sh --test-key (a throwaway key; its outputs are only in
#     <build-dir>/test-signed/, with TESTKEY in their names) and verifies both signatures again
#     with espsecure against that key; no production-named image may carry it;
#  3. the signed bootloader ends before the partition table (CONFIG_PARTITION_TABLE_OFFSET);
#  4. the built partition table is the production one (storage and coredump encrypted) and the
#     signed app fits both OTA slots;
#  5. writes <build-dir>/size.txt (idf.py size) and <build-dir>/production-check.txt (the figures).
#
# Never flash anything from this build: it is signed with a throwaway key, and the production
# overlay burns eFuses on the first boot.
set -euo pipefail
: "${IDF_PATH:?source the ESP-IDF export.sh first}"
IDF_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$IDF_DIR"
B="${1:-build-prod}"
DEFAULTS="sdkconfig.defaults;sdkconfig.production"
mkdir -p "$B"
B="$(cd "$B" && pwd)"
for f in ${DEFAULTS//;/ }; do   # the defaults only apply to a fresh sdkconfig
    if [ "$f" -nt "$B/sdkconfig" ]; then
        rm -f "$B/sdkconfig"
    fi
done
idf.py -B "$B" -D SDKCONFIG="$B/sdkconfig" -D "SDKCONFIG_DEFAULTS=$DEFAULTS" reconfigure \
    2>&1 | tee "$B/configure.log"
cmake --build "$B" -j "${JOBS:-$(nproc)}" 2>&1 | tee "$B/build.log"
if grep -hE "warning:" "$B/configure.log" "$B/build.log"; then
    echo "FAIL: the production build produced warnings" >&2
    exit 1
fi
idf.py -B "$B" -D SDKCONFIG="$B/sdkconfig" size | tee "$B/size.txt"

tools/sign_firmware.sh --test-key --build-dir "$B"
T="$B/test-signed"
KEY="$T/test_signing_key.pem"
BL="$T/bootloader-TESTKEY-signed.bin"
APP="$T/rs485_spb_gateway-TESTKEY-signed.bin"
for img in "$BL" "$APP"; do
    python -m espsecure verify-signature --version 2 --keyfile "$KEY" "$img" >/dev/null ||
        { echo "FAIL: $img: the test-key signature does not verify" >&2; exit 1; }
    echo "signature verified: $img"
done
# the factory flashes the production names only: none may be signed with the test key
for img in "$B/bootloader/bootloader-signed.bin" "$B/rs485_spb_gateway-signed.bin"; do
    if [ -f "$img" ] && python -m espsecure verify-signature --version 2 --keyfile "$KEY" "$img" \
        >/dev/null 2>&1; then
        echo "FAIL: $img is signed with the throwaway test key" >&2
        exit 1
    fi
done

python - "$B" "$BL" "$APP" <<'PYEOF'
import os, re, subprocess, sys
build, bl, app = sys.argv[1:]
sdk = open(os.path.join(build, "sdkconfig")).read()
def opt(name):
    m = re.search(rf"^CONFIG_{name}=(.*)$", sdk, re.M)
    return m.group(1).strip('"') if m else None
errors = []
for name in ("SECURE_BOOT_V2_ENABLED", "SECURE_FLASH_ENC_ENABLED",
             "SECURE_FLASH_ENCRYPTION_MODE_RELEASE", "SECURE_DISABLE_ROM_DL_MODE", "NVS_ENCRYPTION"):
    if opt(name) != "y":
        errors.append(f"CONFIG_{name} is not set")
if opt("SECURE_BOOT_BUILD_SIGNED_BINARIES") == "y":
    errors.append("the build signs its own images (the key must not sit on the build host)")
if opt("PARTITION_TABLE_CUSTOM_FILENAME") != "partitions-production.csv":
    errors.append(f"partition table {opt('PARTITION_TABLE_CUSTOM_FILENAME')!r}, not "
                  "partitions-production.csv")
table_off = int(opt("PARTITION_TABLE_OFFSET") or "0", 0)
bl_off = int(opt("BOOTLOADER_OFFSET_IN_FLASH") or "0", 0)
bl_size = os.path.getsize(bl)
lines = [f"bootloader (signed) {bl_size} B at 0x{bl_off:x}, partition table at 0x{table_off:x}: "
         f"{table_off - bl_off - bl_size} B free"]
if bl_off + bl_size > table_off:
    errors.append(f"the signed bootloader ({bl_size} B at 0x{bl_off:x}) overlaps the partition "
                  f"table at 0x{table_off:x}")
gen = os.path.join(os.environ["IDF_PATH"], "components/partition_table/gen_esp32part.py")
csv = subprocess.run([sys.executable, gen, os.path.join(build, "partition_table/partition-table.bin")],
                     check=True, capture_output=True, text=True).stdout
def size(v):
    v = v.strip()
    mul = {"K": 1024, "M": 1024 * 1024}.get(v[-1:].upper(), 1)
    return int(v[:-1] if mul > 1 else v, 0) * mul
parts = {}
for ln in csv.splitlines():
    cols = [c.strip() for c in ln.split(",")]
    if len(cols) >= 5 and not ln.lstrip().startswith("#"):
        parts[cols[0]] = (cols[1], cols[2], int(cols[3], 0), size(cols[4]),
                          cols[5] if len(cols) > 5 else "")
for name in ("storage", "coredump"):
    if name not in parts or "encrypted" not in parts[name][4]:
        errors.append(f"partition {name} is not marked encrypted in the built table")
if min((p[2] for p in parts.values()), default=0) < table_off + 0x1000:
    errors.append("a partition starts inside the partition table")
app_size = os.path.getsize(app)
for slot in ("ota_0", "ota_1"):
    if slot not in parts:
        errors.append(f"no {slot} partition")
        continue
    cap = parts[slot][3]
    lines.append(f"app (signed) {app_size} B in {slot} of {cap} B: {cap - app_size} B free "
                 f"({100 * app_size / cap:.1f} % used)")
    if app_size > cap:
        errors.append(f"the signed app ({app_size} B) does not fit {slot} ({cap} B)")
out = "\n".join(lines + [f"FAIL: {e}" for e in errors]) + "\n"
open(os.path.join(build, "production-check.txt"), "w").write(out)
print(out, end="")
sys.exit(1 if errors else 0)
PYEOF
echo "production build checked: $B (test-key images only in $T: never flash them)"
