#!/usr/bin/env bash
# CI: apt-get install of the packages in "$@", robust against a mirror that does not answer
# (2026-11-19: archive.ubuntu.com and security.ubuntu.com timed out from the GitHub runners).
# First the Azure mirror of the GitHub runners, then the Ubuntu archive, each with retries and a
# short timeout. Issue #253 removes apt from the E2E jobs (a prebuilt image on ghcr.io).
# Acquire::http::Timeout does not stop a mirror that sends data too slowly (2026-11-19: the fuzz
# job stayed in this script for more than 50 min): each apt-get has a limit of its own, and past
# it the next mirror is tried. The workflow steps have timeout-minutes as the last limit.
#
#   idf/tools/ci/apt_install.sh mosquitto libslirp0        # as root (in a container)
#   sudo idf/tools/ci/apt_install.sh clang-18              # on the runner
set -euo pipefail
[ "$#" -gt 0 ] || { echo "usage: apt_install.sh PACKAGE..." >&2; exit 2; }
opts=(-qq -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o DPkg::Lock::Timeout=120)
export DEBIAN_FRONTEND=noninteractive

# The GitHub runner image has clang 18 and clang-tidy 18 already: a package that is installed
# needs no mirror at all. Only the missing ones go to apt-get.
missing=()
for p in "$@"; do
    dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" || missing+=("$p")
done
if [ "${#missing[@]}" -eq 0 ]; then
    echo "apt: already installed: $*"
    exit 0
fi
echo "apt: to install: ${missing[*]}"
set -- "${missing[@]}"

use_mirror() {
    local f
    for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
        [ -f "$f" ] || continue
        sed -i -E "s#http://(azure\.)?(archive|security)\.ubuntu\.com/ubuntu#$1#g" "$f"
    done
}

for mirror in http://azure.archive.ubuntu.com/ubuntu http://archive.ubuntu.com/ubuntu; do
    echo "apt: $mirror"
    use_mirror "$mirror"
    if timeout 300 apt-get "${opts[@]}" update \
        && timeout 900 apt-get "${opts[@]}" install -y "$@" >/dev/null; then
        exit 0
    fi
    echo "apt: $mirror failed" >&2
done
exit 1
