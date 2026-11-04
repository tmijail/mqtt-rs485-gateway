#!/usr/bin/env bash
# FW-2: sign the images of a production build (Secure Boot V2, RSA-3072 PSS on the ESP32-S3),
# and check signed images against the published key digest before the factory flashes them.
#
# The production overlay (sdkconfig.production) builds UNSIGNED images
# (CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n): the private key never sits on the build host.
# This script signs them where the key is (offline machine or HSM), verifies every signature
# and prints the public key digest that the first boot burns into eFuse.
#
#   tools/sign_firmware.sh --keyfile /secure/gw_signing_key.pem --expect-digest D [--build-dir build-prod]
#   tools/sign_firmware.sh --hsm-config hsm.ini --pub-key gw_signing_pub.pem --expect-digest D [--build-dir ...]
#   tools/sign_firmware.sh --check --pub-key gw_signing_pub.pem --expect-digest D [--build-dir ...]   # factory
#   tools/sign_firmware.sh --test-key [--build-dir ...]   # throwaway key, never for devices
#
# D is the SHA-256 digest of the real signing key's public key, published apart from the build
# (64 hex digits, spaces or colons allowed, or a file with the 32 bytes or the hex text). It is
# required by every mode but --test-key (issue #103): it is what tells the real key from any
# other, whatever test keys exist on this machine. Give it once per key when the images carry
# reserve signatures. Every signature block of both signed images must be from one of those keys
# (the first boot burns the digest of every valid block into eFuse, for good).
#
# --check (issue #103), before flashing, fails unless:
#   - in both signed images, every signature block is from a key with an expected digest, and
#     its RSA-PSS signature verifies on its own (espsecure verify-signature --version 2 on a copy
#     holding only that block) with that key's PUBLIC key, given with --pub-key (once per key;
#     it can be kept under version control: its digest is checked against D, which comes from
#     elsewhere);
#   - the bootloader's blocks are from exactly the expected keys (all the D given, no more: the
#     first boot burns those digests and revokes the other slots), and every app block is from
#     one of the bootloader's keys (the only ones trusted after that boot);
#   - the signed content is the current build: the unsigned image of the build directory,
#     padded with 0xFF to 4 KB, then the signature sector. A signed image of an older build
#     is refused, also if the real key signed it.
# Signing writes one block, with the key given; reserve signatures are added afterwards
# (espsecure sign-data --append-signatures) and --check then takes every digest.
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
# a failed signature never leaves older images behind. --test-key also deletes images under the
# production names that do not form one consistent set (both images and the digest file, every
# signature block from the key of that digest file) or carry a known test key: what an
# interrupted run of the script before issue #93 left (issue #103).
#
# The build must be a complete production one: Secure Boot V2, flash encryption in release mode,
# ROM download mode disabled and NVS encryption (issue #93); anything else is refused.
#
# Signing is offline file work: nothing here talks to a device or touches an eFuse.
set -euo pipefail
: "${IDF_PATH:?source the ESP-IDF export.sh first}"
IDF_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$IDF_DIR/build-prod"
KEY="" HSM="" TEST=0 CHECK=0
EXPECT=() PUBS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --keyfile) KEY="$2"; shift 2 ;;
        --hsm-config) HSM="$2"; shift 2 ;;
        --pub-key) PUBS+=("$2"); shift 2 ;;
        --test-key) TEST=1; shift ;;
        --check) CHECK=1; shift ;;
        --expect-digest) EXPECT+=("$2"); shift 2 ;;
        --build-dir) BUILD="$(cd "$2" && pwd)"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
modes=$(( TEST + CHECK + (${#KEY} > 0 ? 1 : 0) + (${#HSM} > 0 ? 1 : 0) ))
[ "$modes" = 1 ] || { echo "give exactly one of --keyfile, --hsm-config (with --pub-key), --test-key or --check" >&2; exit 2; }
[ "$TEST" = 1 ] || [ ${#EXPECT[@]} -gt 0 ] ||
    { echo "--expect-digest is required (the published digest of the real key; issue #103)" >&2; exit 2; }
[ "$TEST" = 0 ] || [ ${#EXPECT[@]} = 0 ] || { echo "--expect-digest is for the real key, not --test-key" >&2; exit 2; }
[ "$CHECK" = 0 ] || [ ${#PUBS[@]} -gt 0 ] ||
    { echo "--check needs --pub-key (the public key of each expected digest, to verify the signatures)" >&2; exit 2; }
[ -n "$HSM" ] || [ "$CHECK" = 1 ] || [ ${#PUBS[@]} = 0 ] ||
    { echo "--pub-key goes with --hsm-config or --check" >&2; exit 2; }
[ -z "$HSM" ] || [ ${#PUBS[@]} = 1 ] || { echo "--hsm-config needs one --pub-key (to verify and digest)" >&2; exit 2; }

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
# Key file (private or public PEM) -> hex digest of its public key. Fails (and prints nothing)
# if espsecure cannot read it: callers run it inside $(...), where set -e does not apply, so
# every failure is returned explicitly (issue #103).
key_digest() {
    local out d
    out="$(mktemp)"
    if ! python -m espsecure digest-sbv2-public-key --keyfile "$1" --output "$out" >/dev/null 2>&1 ||
       [ "$(wc -c <"$out")" -ne 32 ]; then
        rm -f "$out"
        return 1
    fi
    d="$(hexdigest "$out")"
    rm -f "$out"
    [[ "$d" =~ ^[0-9a-f]{64}$ ]] || return 1
    printf '%s' "$d"
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
    [ -f "$k" ] || continue
    if d="$(key_digest "$k")"; then
        TEST_DIGESTS+=("$d")
    else   # no empty entry: the real protection is the expected digest, not this list
        echo "warning: test key $k cannot be read: images signed with it are not recognised as" \
             "test-signed here" >&2
    fi
done
is_test_digest() {
    local t
    for t in ${TEST_DIGESTS[@]+"${TEST_DIGESTS[@]}"}; do
        [ "$t" = "$1" ] && return 0
    done
    return 1
}

# Public keys given with --pub-key, by digest: each must be one of the expected keys
PUB_DIGESTS=()
for k in ${PUBS[@]+"${PUBS[@]}"}; do
    [ -f "$k" ] || { echo "--pub-key $k: no such file" >&2; exit 2; }
    d="$(key_digest "$k")" || { echo "--pub-key $k: espsecure cannot read it as an RSA-3072 key" >&2; exit 2; }
    [[ " ${WANT[*]} " == *" $d "* ]] ||
        { echo "--pub-key $k: digest $d is none of the expected ones (--expect-digest)" >&2; exit 1; }
    PUB_DIGESTS+=("$d")
done
pub_for() {   # digest -> the --pub-key file with it, or nothing
    local i
    for i in "${!PUB_DIGESTS[@]}"; do
        [ "${PUB_DIGESTS[$i]}" = "$1" ] && { printf '%s' "${PUBS[$i]}"; return 0; }
    done
    return 1
}

block_digests() {   # signed image -> the key digest of each valid signature block, one per line
    local info
    info="$(python -m espsecure signature-info-v2 "$1" 2>&1)" || return 1
    printf '%s\n' "$info" | sed -n 's/^Public key digest for block [0-9]*: //p' | tr -d ' '
}

# Issue #103: verify block @2 (0-based) of signed image @1 on its own with public key @3.
# espsecure verify-signature passes when ANY block verifies with the key, so a block that carries
# that key with a bad signature would hide behind a good one: the check runs on a copy of the
# image whose signature sector holds only that block (the other slots erased, 0xFF).
verify_block() {
    local tmp rc=0
    tmp="$(mktemp)"
    python - "$1" "$2" "$tmp" <<'PYEOF' || { rm -f "$tmp"; return 1; }
import sys
img = open(sys.argv[1], "rb").read()
i = int(sys.argv[2])
if len(img) < 4096 or len(img) % 4096 or not 0 <= i < 3:
    sys.exit(1)
sector = img[-4096:]
block = sector[i * 1216:(i + 1) * 1216]
open(sys.argv[3], "wb").write(img[:-4096] + block + b"\xff" * (4096 - 1216))
PYEOF
    python -m espsecure verify-signature --version 2 --keyfile "$3" "$tmp" >/dev/null 2>&1 || rc=1
    rm -f "$tmp"
    return "$rc"
}

# Issue #103: the signed image @2 is the unsigned image @1 of the current build, padded with
# 0xFF to 4 KB, plus one signature sector (what espsecure sign-data --version 2 writes).
same_build() {
    python - "$1" "$2" <<'PYEOF'
import sys
u = open(sys.argv[1], "rb").read()
s = open(sys.argv[2], "rb").read()
body = len(u) + (-len(u)) % 4096
sys.exit(0 if len(s) == body + 4096 and s[:len(u)] == u and
         s[len(u):body] == b"\xff" * (body - len(u)) else 1)
PYEOF
}

# Every valid signature block of signed image @2 must be from a key in WANT (and from no known
# test key); with @3 = verify, its RSA signature must verify with the --pub-key of that digest,
# and the signed content must be the unsigned image @1 of this build.
check_image() {
    local n=0 bad=0 d k i=0
    [ -f "$2" ] || { echo "MISSING: $2" >&2; return 1; }
    local digests
    digests="$(block_digests "$2")" || { echo "NO VALID SIGNATURE: $2" >&2; return 1; }
    while read -r d; do
        [ -n "$d" ] || continue
        i=$n   # signature-info-v2 lists the valid blocks from block 0, without gaps
        n=$((n + 1))
        if is_test_digest "$d"; then
            echo "TEST KEY: $2 is signed with a throwaway test key ($d)" >&2; bad=1
        elif [[ " ${WANT[*]} " != *" $d "* ]]; then
            echo "WRONG KEY: $2 is signed with key digest $d, not the expected one" >&2; bad=1
        elif [ "${3:-}" = verify ]; then
            if ! k="$(pub_for "$d")"; then
                echo "NO PUBLIC KEY: $2 has a block from key digest $d and no --pub-key has it" >&2; bad=1
            elif ! verify_block "$2" "$i" "$k"; then
                echo "BAD SIGNATURE: block $i of $2 (key digest $d) does not verify with $k" >&2; bad=1
            fi
        fi
    done <<<"$digests"
    [ "$n" -gt 0 ] || { echo "NO VALID SIGNATURE: $2" >&2; return 1; }
    if [ "${3:-}" = verify ]; then
        [ -f "$1" ] || { echo "MISSING: $1 (the unsigned image of this build)" >&2; return 1; }
        same_build "$1" "$2" || { echo "STALE: $2 is not the signed $1 of the current build" >&2; bad=1; }
    fi
    [ "$bad" = 0 ] && echo "OK ($n signature block(s)${3:+, RSA verified, current build}): $2"
    return "$bad"
}
# Issue #103, the two images together (--check): the first boot burns the digest of every valid
# block of the BOOTLOADER and revokes the unused digest slots, and from then on the bootloader
# accepts an app only if one of its blocks is from a burned key. So the set of the bootloader's
# block digests must be exactly the expected set (a reserve key missing there is lost for good),
# and every app block must be from one of the bootloader's keys (otherwise the eFuses get burned
# and the app is refused, with ROM download disabled).
check_key_sets() {   # bootloader, app
    local bl app want d bad=0
    bl="$(block_digests "$1" | sort -u)" || return 1
    app="$(block_digests "$2" | sort -u)" || return 1
    want="$(printf '%s\n' "${WANT[@]}" | sort -u)"
    if [ "$bl" != "$want" ]; then
        echo "KEY SET: $1 is signed by key digest(s) [$(echo $bl)], the expected set is" \
             "[$(echo $want)]: the first boot would burn only the bootloader's keys and revoke the" \
             "other slots" >&2
        bad=1
    fi
    while read -r d; do
        [ -n "$d" ] || continue
        if ! grep -qx "$d" <<<"$bl"; then
            echo "KEY SET: $2 has a block from key digest $d that the bootloader does not carry:" \
                 "after the first boot that key is not trusted" >&2
            bad=1
        fi
    done <<<"$app"
    return "$bad"
}
UNSIGNED=("$BUILD/bootloader/bootloader.bin" "$BUILD/rs485_spb_gateway.bin")
check_outputs() {   # bootloader, app [verify]
    local ok=0
    check_image "${UNSIGNED[0]}" "$1" "${3:-}" || ok=1
    check_image "${UNSIGNED[1]}" "$2" "${3:-}" || ok=1
    if [ "$ok" = 0 ] && [ "${3:-}" = verify ]; then
        check_key_sets "$1" "$2" || ok=1
    fi
    return "$ok"
}

PROD_OUT=("$BUILD/bootloader/bootloader-signed.bin" "$BUILD/rs485_spb_gateway-signed.bin"
          "$BUILD/signing_key_digest.bin")
if [ "$CHECK" = 1 ]; then
    if check_outputs "${PROD_OUT[0]}" "${PROD_OUT[1]}" verify; then
        echo "the signed images in $BUILD are this build, signed only by the expected key(s): ready to flash"
        exit 0
    fi
    echo "DO NOT FLASH: the signed images in $BUILD are not this build signed by the expected key(s)" >&2
    exit 1
fi

# True if images under the production names exist and are a leftover: not one consistent set
# (both signed images and the digest file; the key of the digest file signs both images; every
# app block is from a key that also signs the bootloader), or any block or the digest file from
# a known test key. A valid set with reserve signatures (more blocks than the digest file names)
# is kept.
prod_leftover() {
    local f any=0 want d bl app
    for f in "${PROD_OUT[@]}"; do
        [ -e "$f" ] && any=1
    done
    [ "$any" = 1 ] || return 1
    for f in "${PROD_OUT[@]}"; do
        [ -f "$f" ] || return 0
    done
    [ "$(wc -c <"${PROD_OUT[2]}")" -eq 32 ] || return 0
    want="$(hexdigest "${PROD_OUT[2]}")"
    is_test_digest "$want" && return 0
    bl="$(block_digests "${PROD_OUT[0]}")" || return 0
    app="$(block_digests "${PROD_OUT[1]}")" || return 0
    [ -n "$bl" ] && [ -n "$app" ] || return 0
    while read -r d; do
        is_test_digest "$d" && return 0
    done <<<"$bl"$'\n'"$app"
    grep -qx "$want" <<<"$bl" && grep -qx "$want" <<<"$app" || return 0
    while read -r d; do
        grep -qx "$d" <<<"$bl" || return 0
    done <<<"$app"
    return 1
}

if [ "$TEST" = 1 ]; then
    mkdir -p "$TESTDIR"
    KEY="$TESTDIR/test_signing_key.pem"
    [ -f "$KEY" ] || python -m espsecure generate-signing-key --version 2 --scheme rsa3072 "$KEY" >/dev/null
    d="$(key_digest "$KEY")" || { echo "$KEY: test key cannot be read" >&2; exit 1; }
    TEST_DIGESTS+=("$d")
    OUT=("$TESTDIR/bootloader-TESTKEY-signed.bin" "$TESTDIR/rs485_spb_gateway-TESTKEY-signed.bin"
         "$TESTDIR/signing_key_digest-TESTKEY.bin")
    # Before issue #93 --test-key wrote the production names: drop such leftovers, also a run
    # left half done (issue #103), whatever test keys this machine still has
    if prod_leftover; then
        rm -f "${PROD_OUT[@]}"
        echo "removed signed images under the production names in $BUILD that were not one" \
             "consistent set (an interrupted or test-key run): sign them again" >&2
    fi
    echo "WARNING: throwaway test key $KEY: never flash images signed with it on a device" >&2
else
    OUT=("${PROD_OUT[@]}")
fi
if [ -n "$HSM" ]; then
    SIGN=(--hsm --hsm-config "$HSM")
    VERIFY_KEY="${PUBS[0]}"
else
    SIGN=(--keyfile "$KEY")
    VERIFY_KEY="$KEY"
fi
if [ "$TEST" = 0 ]; then
    vd="$(key_digest "$VERIFY_KEY")" ||
        { echo "$VERIFY_KEY: espsecure cannot read it as an RSA-3072 key" >&2; exit 1; }
    if is_test_digest "$vd"; then
        echo "$VERIFY_KEY is a throwaway test key: use --test-key for that (separate outputs)" >&2
        exit 1
    fi
    # issue #103: any key whose digest is not the published one is refused before signing
    [[ " ${WANT[*]} " == *" $vd "* ]] ||
        { echo "$VERIFY_KEY: digest $vd is not the expected one (--expect-digest): not the real key" >&2; exit 1; }
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
for i in 0 1; do   # what --check will compare: the signed content is this build
    same_build "${UNSIGNED[$i]}" "${OUT[$i]}" || { echo "${OUT[$i]}: not the signed ${UNSIGNED[$i]}" >&2; exit 1; }
done
done_ok=1
echo "public key digest (eFuse KEY_PURPOSE SECURE_BOOT_DIGEST0 after the first boot):"
hexdigest "${OUT[2]}"
echo
if [ "$TEST" = 1 ]; then
    echo "TEST-KEY images in $TESTDIR: for checking the signing only; the factory never flashes them" >&2
else
    echo "next, at the factory: $0 --check --pub-key <public key> --expect-digest <digest> --build-dir $BUILD" >&2
fi
