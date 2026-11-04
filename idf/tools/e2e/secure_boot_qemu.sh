#!/usr/bin/env bash
# FW-2 on the emulator: the production Secure Boot V2 on QEMU's emulated eFuse file (nothing
# real is burned). Builds the QEMU variant with sdkconfig.production on top (flash encryption
# off: QEMU cannot read back what it encrypts, README 6.12), signed with a throwaway key, and
# boots it three times on a blank eFuse file:
#   1. first boot: the bootloader burns the key digest (emulated) and starts the app;
#   2. second boot: the ROM verifies the signed bootloader, the app runs again;
#   3. the app replaced by one signed with another key: the bootloader refuses it.
#
#   . $IDF_PATH/export.sh && [JOBS=n] tools/e2e/secure_boot_qemu.sh [workdir]
# Not part of run_e2e.sh (a separate full build, about 10 minutes).
set -euo pipefail
: "${IDF_PATH:?source the ESP-IDF export.sh first}"
IDF_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${1:-$(mktemp -d -t gw-sb-XXXX)}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
cd "$IDF_DIR"
B="$WORK/build"
[ -f "$WORK/key.pem" ] || python -m espsecure generate-signing-key --version 2 --scheme rsa3072 "$WORK/key.pem" >/dev/null
[ -f "$WORK/other.pem" ] || python -m espsecure generate-signing-key --version 2 --scheme rsa3072 "$WORK/other.pem" >/dev/null
printf '%s\n' 'CONFIG_SECURE_FLASH_ENC_ENABLED=n' 'CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y' \
    "CONFIG_SECURE_BOOT_SIGNING_KEY=\"$WORK/key.pem\"" > "$WORK/sdkconfig.sb"
idf.py -B "$B" -D SDKCONFIG="$B/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu;sdkconfig.production;$WORK/sdkconfig.sb" \
    -D GW_FS_DIR="$IDF_DIR/tools/e2e/fs" reconfigure >"$WORK/build.log" 2>&1
cmake --build "$B" -j "${JOBS:-$(nproc)}" >>"$WORK/build.log" 2>&1
grep -q '^CONFIG_SECURE_BOOT_V2_ENABLED=y' "$B/sdkconfig"
(cd "$B" && python -m esptool --chip esp32s3 merge-bin --output "$WORK/flash.bin" --pad-to-size 16MB \
    0x0 bootloader/bootloader.bin @flash_args >/dev/null)
python - "$WORK/efuse.bin" <<'PYEOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["IDF_PATH"], "tools"))
from idf_py_actions.qemu_ext import QEMU_TARGETS
open(sys.argv[1], "wb").write(QEMU_TARGETS["esp32s3"].default_efuse)
PYEOF
boot() {   # name flash efuse seconds
    timeout "$4" qemu-system-xtensa -M esp32s3 -m 32M -nographic \
        -drive "file=$2,if=mtd,format=raw" -drive "file=$3,if=none,format=raw,id=efuse" \
        -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
        -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
        -nic user,model=open_eth -serial "file:$WORK/$1.log" -serial null -monitor none \
        >"$WORK/$1.qemu" 2>&1 || true
}
ok=0
boot first "$WORK/flash.bin" "$WORK/efuse.bin" 30
grep -aq 'main: configuration from' "$WORK/first.log" && echo "PASS first boot: the app runs" || { echo "FAIL first boot"; ok=1; }
boot second "$WORK/flash.bin" "$WORK/efuse.bin" 30
grep -aq 'secure boot verification succeeded' "$WORK/second.log" && grep -aq 'main: configuration from' "$WORK/second.log" &&
    echo "PASS second boot: ROM verified the bootloader, the app runs" || { echo "FAIL second boot"; ok=1; }
python -m espsecure sign-data --version 2 --keyfile "$WORK/other.pem" --output "$WORK/other-app.bin" \
    "$B/rs485_spb_gateway-unsigned.bin" >/dev/null
cp "$WORK/flash.bin" "$WORK/flash-other.bin"
cp "$WORK/efuse.bin" "$WORK/efuse-other.bin"
dd if="$WORK/other-app.bin" of="$WORK/flash-other.bin" bs=4096 seek=$((0x20000 / 4096)) conv=notrunc status=none
boot other "$WORK/flash-other.bin" "$WORK/efuse-other.bin" 20
grep -aq 'signature bad' "$WORK/other.log" && ! grep -aq 'main: configuration from' "$WORK/other.log" &&
    echo "PASS an app signed with another key is refused by the bootloader" || { echo "FAIL other key"; ok=1; }
exit $ok
