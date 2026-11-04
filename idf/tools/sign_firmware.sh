#!/usr/bin/env bash
# FW-2: sign the images of a production build (Secure Boot V2, RSA-3072 PSS on the ESP32-S3).
#
# The production overlay (sdkconfig.production) builds UNSIGNED images
# (CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n): the private key never sits on the build host.
# This script signs them where the key is (offline machine or HSM), verifies every signature
# and prints the public key digest that the first boot burns into eFuse.
#
#   tools/sign_firmware.sh --keyfile /secure/gw_signing_key.pem [--build-dir build-prod]
#   tools/sign_firmware.sh --hsm-config hsm.ini --pub-key gw_signing_pub.pem [--build-dir ...]
#   tools/sign_firmware.sh --test-key [--build-dir ...]   # throwaway key, never for devices
#
# Outputs, next to the inputs in the build directory:
#   bootloader/bootloader-signed.bin   flashed once at the factory (0x0)
#   rs485_spb_gateway-signed.bin       flashed at the factory (ota_0) and served for OTA (FW-1)
#   signing_key_digest.bin             SHA-256 of the public key (what the eFuse will hold)
#
# Signing is offline file work: nothing here talks to a device or touches an eFuse.
set -euo pipefail
: "${IDF_PATH:?source the ESP-IDF export.sh first}"
IDF_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$IDF_DIR/build-prod"
KEY="" HSM="" PUB="" TEST=0
while [ $# -gt 0 ]; do
    case "$1" in
        --keyfile) KEY="$2"; shift 2 ;;
        --hsm-config) HSM="$2"; shift 2 ;;
        --pub-key) PUB="$2"; shift 2 ;;
        --test-key) TEST=1; shift ;;
        --build-dir) BUILD="$(cd "$2" && pwd)"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
SDK="$BUILD/sdkconfig"
grep -q '^CONFIG_SECURE_BOOT_V2_ENABLED=y' "$SDK" ||
    { echo "$SDK: not a Secure Boot V2 build (use sdkconfig.production)" >&2; exit 1; }
grep -q '^CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y' "$SDK" &&
    { echo "$SDK: this build signs itself; nothing to do" >&2; exit 1; }
if [ "$TEST" = 1 ]; then
    KEY="$BUILD/test_signing_key.pem"
    [ -f "$KEY" ] || python -m espsecure generate-signing-key --version 2 --scheme rsa3072 "$KEY" >/dev/null
    echo "WARNING: throwaway test key $KEY: never flash images signed with it on a device" >&2
fi
if [ -n "$HSM" ]; then
    [ -n "$PUB" ] || { echo "--hsm-config needs --pub-key (to verify and digest)" >&2; exit 2; }
    SIGN=(--hsm --hsm-config "$HSM")
    VERIFY_KEY="$PUB"
elif [ -n "$KEY" ]; then
    SIGN=(--keyfile "$KEY")
    VERIFY_KEY="$KEY"
else
    echo "give --keyfile, --hsm-config with --pub-key, or --test-key" >&2
    exit 2
fi

sign() {   # unsigned input, signed output
    python -m espsecure sign-data --version 2 "${SIGN[@]}" --output "$2" "$1" >/dev/null
    python -m espsecure verify-signature --version 2 --keyfile "$VERIFY_KEY" "$2" >/dev/null
    echo "signed and verified: $2"
}
sign "$BUILD/bootloader/bootloader.bin" "$BUILD/bootloader/bootloader-signed.bin"
sign "$BUILD/rs485_spb_gateway.bin" "$BUILD/rs485_spb_gateway-signed.bin"
python -m espsecure digest-sbv2-public-key --keyfile "$VERIFY_KEY" --output "$BUILD/signing_key_digest.bin" >/dev/null
echo "public key digest (eFuse KEY_PURPOSE SECURE_BOOT_DIGEST0 after the first boot):"
od -An -tx1 -v "$BUILD/signing_key_digest.bin" | tr -d ' \n'
echo
