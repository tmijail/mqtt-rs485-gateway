#!/usr/bin/env bash
# FW-2: sign the images of a production build (Secure Boot V2, RSA-3072 PSS on the ESP32-S3),
# and check signed images against the published key digest before the factory flashes them.
#
# The production overlay (sdkconfig.production) builds UNSIGNED images
# (CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n): the private key never sits on the build host.
# This script signs them where the key is (offline machine or HSM), verifies every signature
# and prints the public key digest that the first boot burns into eFuse.
#
#   tools/sign_firmware.sh --keyfile /secure/gw_signing_key.pem [--build-dir build-prod] [--expect-digest D]
#   tools/sign_firmware.sh --hsm-config hsm.ini --pub-key gw_signing_pub.pem [--build-dir ...] [--expect-digest D]
#   tools/sign_firmware.sh --check --expect-digest D [--build-dir ...]   # factory, before flashing
#   tools/sign_firmware.sh --test-key [--build-dir ...]   # throwaway key, never for devices
#
# D is the SHA-256 digest of the real signing key's public key, published apart from the build
# (64 hex digits, spaces or colons allowed, or a file with the 32 bytes or the hex text). Give it
# once per key when the images carry reserve signatures. --check (and --expect-digest when
# signing) fails unless every signature block of both signed images is from one of those keys:
# the first boot burns the digest of every valid block into eFuse, for good.
#
# Outputs with --keyfile/--hsm-config, next to the inputs in the build directory (issue #93: the
# only files the factory procedure flashes, README 3.6):
#   bootloader/bootloader-signed.bin   flashed once at the factory (0x0)
#   rs485_spb_gateway-signed.bin       flashed at the factory (ota_0) and served for OTA (FW-1)
#   signing_key_digest.bin             SHA-256 of the public key (what the eFuse will hold)
# Outputs with --test-key, in <build-dir>/test-signed/ only (with the key), never under those names:
#   bootloader-TESTKEY-signed.bin, rs485_spb_gateway-TESTKEY-signed.bin,
#   signing_key_digest-TESTKEY.bin, test_signing_key.pem
# Every run first deletes the outputs it is about to write, and deletes them again if it fails:
# a failed signature never leaves older images behind.
#
# The build must be a complete production one: Secure Boot V2, flash encryption in release mode,
# ROM download mode disabled and NVS encryption (issue #93); anything else is refused.
#
# Signing is offline file work: nothing here talks to a device or touches an eFuse.
set -euo pipefail
: "${IDF_PATH:?source the ESP-IDF export.sh first}"
IDF_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$IDF_DIR/build-prod"
KEY="" HSM="" PUB="" TEST=0 CHECK=0
EXPECT=()
while [ $# -gt 0 ]; do
    case "$1" in
        --keyfile) KEY="$2"; shift 2 ;;
        --hsm-config) HSM="$2"; shift 2 ;;
        --pub-key) PUB="$2"; shift 2 ;;
        --test-key) TEST=1; shift ;;
        --check) CHECK=1; shift ;;
        --expect-digest) EXPECT+=("$2"); shift 2 ;;
        --build-dir) BUILD="$(cd "$2" && pwd)"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
modes=$(( TEST + CHECK + (${#KEY} > 0 ? 1 : 0) + (${#HSM} > 0 ? 1 : 0) ))
[ "$modes" = 1 ] || { echo "give exactly one of --keyfile, --hsm-config (with --pub-key), --test-key or --check" >&2; exit 2; }
[ "$CHECK" = 0 ] || [ ${#EXPECT[@]} -gt 0 ] || { echo "--check needs --expect-digest" >&2; exit 2; }
[ "$TEST" = 0 ] || [ ${#EXPECT[@]} = 0 ] || { echo "--expect-digest is for the real key, not --test-key" >&2; exit 2; }

SDK="$BUILD/sdkconfig"
[ -f "$SDK" ] || { echo "$SDK: no such file (configure the production build first)" >&2; exit 1; }
missing=()
for o in SECURE_BOOT_V2_ENABLED SECURE_FLASH_ENC_ENABLED SECURE_FLASH_ENCRYPTION_MODE_RELEASE \
         SECURE_DISABLE_ROM_DL_MODE NVS_ENCRYPTION; do
    grep -q "^CONFIG_$o=y" "$SDK" || missing+=("CONFIG_$o")
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "$SDK: not a complete production build, missing: ${missing[*]}" >&2
    echo "(build with SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.production, README 3.6)" >&2
    exit 1
fi
grep -q '^CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y' "$SDK" &&
    { echo "$SDK: this build signs itself; nothing to do" >&2; exit 1; }

hexdigest() {   # file with 32 bytes -> 64 lowercase hex digits
    od -An -tx1 -v "$1" | tr -d ' \n'
}
key_digest() {   # key file (private or public PEM) -> hex digest of its public key
    local out
    out="$(mktemp)"
    python -m espsecure digest-sbv2-public-key --keyfile "$1" --output "$out" >/dev/null
    hexdigest "$out"
    rm -f "$out"
}
norm_digest() {   # --expect-digest argument -> 64 lowercase hex digits, or nothing
    local d="$1"
    if [ -f "$d" ]; then
        if [ "$(wc -c <"$d")" -eq 32 ]; then
            d="$(hexdigest "$d")"
        else
            d="$(cat "$d")"
        fi
    fi
    d="$(printf '%s' "$d" | tr -d ' :\n\r\t' | tr 'A-F' 'a-f')"
    [[ "$d" =~ ^[0-9a-f]{64}$ ]] && printf '%s' "$d"
}
WANT=()
for d in ${EXPECT[@]+"${EXPECT[@]}"}; do
    n="$(norm_digest "$d")" || true
    [ -n "$n" ] || { echo "--expect-digest $d: not a SHA-256 digest (64 hex digits or a 32-byte file)" >&2; exit 2; }
    WANT+=("$n")
done

# Throwaway keys this tree knows of: no production image may carry their signature
TESTDIR="$BUILD/test-signed"
TEST_KEYS=("$TESTDIR/test_signing_key.pem" "$BUILD/test_signing_key.pem"
           "$IDF_DIR/build-qemu/e2e_signing_key.pem")
TEST_DIGESTS=()
for k in "${TEST_KEYS[@]}"; do
    [ -f "$k" ] && TEST_DIGESTS+=("$(key_digest "$k")")
done
is_test_digest() {
    local t
    for t in ${TEST_DIGESTS[@]+"${TEST_DIGESTS[@]}"}; do
        [ "$t" = "$1" ] && return 0
    done
    return 1
}

# Every valid signature block of @1 must be from a key in WANT (and from no known test key).
check_image() {
    local info n=0 bad=0 d
    [ -f "$1" ] || { echo "MISSING: $1" >&2; return 1; }
    info="$(python -m espsecure signature-info-v2 "$1" 2>&1)" || { echo "NO VALID SIGNATURE: $1" >&2; return 1; }
    while read -r d; do
        n=$((n + 1))
        if is_test_digest "$d"; then
            echo "TEST KEY: $1 is signed with a throwaway test key ($d)" >&2; bad=1
        elif [[ " ${WANT[*]} " != *" $d "* ]]; then
            echo "WRONG KEY: $1 is signed with key digest $d, not the expected one" >&2; bad=1
        fi
    done < <(printf '%s\n' "$info" | sed -n 's/^Public key digest for block [0-9]*: //p' | tr -d ' ')
    [ "$n" -gt 0 ] || { echo "NO VALID SIGNATURE: $1" >&2; return 1; }
    [ "$bad" = 0 ] && echo "digest OK ($n signature block(s)): $1"
    return "$bad"
}
check_outputs() {   # bootloader, app
    local ok=0
    check_image "$1" || ok=1
    check_image "$2" || ok=1
    return "$ok"
}

PROD_OUT=("$BUILD/bootloader/bootloader-signed.bin" "$BUILD/rs485_spb_gateway-signed.bin"
          "$BUILD/signing_key_digest.bin")
if [ "$CHECK" = 1 ]; then
    if check_outputs "${PROD_OUT[0]}" "${PROD_OUT[1]}"; then
        echo "the signed images in $BUILD carry only the expected key: ready to flash"
        exit 0
    fi
    echo "DO NOT FLASH: the signed images in $BUILD do not match the expected key digest" >&2
    exit 1
fi

if [ "$TEST" = 1 ]; then
    mkdir -p "$TESTDIR"
    KEY="$TESTDIR/test_signing_key.pem"
    [ -f "$KEY" ] || python -m espsecure generate-signing-key --version 2 --scheme rsa3072 "$KEY" >/dev/null
    TEST_DIGESTS+=("$(key_digest "$KEY")")
    OUT=("$TESTDIR/bootloader-TESTKEY-signed.bin" "$TESTDIR/rs485_spb_gateway-TESTKEY-signed.bin"
         "$TESTDIR/signing_key_digest-TESTKEY.bin")
    # Before issue #93 --test-key wrote the production names: drop such leftovers
    if [ -f "${PROD_OUT[2]}" ] && is_test_digest "$(hexdigest "${PROD_OUT[2]}")"; then
        rm -f "${PROD_OUT[@]}"
        echo "removed test-signed images left under the production names in $BUILD" >&2
    fi
    echo "WARNING: throwaway test key $KEY: never flash images signed with it on a device" >&2
else
    OUT=("${PROD_OUT[@]}")
fi
if [ -n "$HSM" ]; then
    [ -n "$PUB" ] || { echo "--hsm-config needs --pub-key (to verify and digest)" >&2; exit 2; }
    SIGN=(--hsm --hsm-config "$HSM")
    VERIFY_KEY="$PUB"
else
    SIGN=(--keyfile "$KEY")
    VERIFY_KEY="$KEY"
fi
if [ "$TEST" = 0 ] && is_test_digest "$(key_digest "$VERIFY_KEY")"; then
    echo "$VERIFY_KEY is a throwaway test key: use --test-key for that (separate outputs)" >&2
    exit 1
fi

done_ok=0
cleanup() {
    [ "$done_ok" = 1 ] || { rm -f "${OUT[@]}"; echo "signing FAILED: no signed image left in place" >&2; }
}
trap cleanup EXIT
rm -f "${OUT[@]}"

sign() {   # unsigned input, signed output
    python -m espsecure sign-data --version 2 "${SIGN[@]}" --output "$2" "$1" >/dev/null
    python -m espsecure verify-signature --version 2 --keyfile "$VERIFY_KEY" "$2" >/dev/null
    echo "signed and verified: $2"
}
sign "$BUILD/bootloader/bootloader.bin" "${OUT[0]}"
sign "$BUILD/rs485_spb_gateway.bin" "${OUT[1]}"
python -m espsecure digest-sbv2-public-key --keyfile "$VERIFY_KEY" --output "${OUT[2]}" >/dev/null
if [ ${#WANT[@]} -gt 0 ]; then
    check_outputs "${OUT[0]}" "${OUT[1]}"
fi
done_ok=1
echo "public key digest (eFuse KEY_PURPOSE SECURE_BOOT_DIGEST0 after the first boot):"
hexdigest "${OUT[2]}"
echo
if [ "$TEST" = 1 ]; then
    echo "TEST-KEY images in $TESTDIR: for checking the signing only; the factory never flashes them" >&2
elif [ ${#WANT[@]} = 0 ]; then
    echo "next: compare that digest with the published one: $0 --check --expect-digest <digest> --build-dir $BUILD" >&2
fi
