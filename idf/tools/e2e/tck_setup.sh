#!/usr/bin/env bash
# Issue #176: installs what the optional E2E scenario 'tck' (tools/e2e/tck.py) needs into DIR:
#   DIR/jdk      Eclipse Temurin JDK 21 (Linux x64)
#   DIR/hivemq   HiveMQ CE with the Eclipse Sparkplug TCK 3.0.0 extension in extensions/
#   DIR/SparkplugTCK  the unpacked TCK package (report.py, coverage report, web console)
# Then run the scenario with E2E_TCK_DIR=DIR (docs/how-to/run-the-sparkplug-tck.md).
#
#   tools/e2e/tck_setup.sh /work/tck
#
# The downloads are pinned and checked with SHA-256. Nothing is installed outside DIR.
set -euo pipefail
DIR="${1:?usage: tck_setup.sh DIR}"
mkdir -p "$DIR/dl"
DIR="$(cd "$DIR" && pwd)"

TCK_URL=https://download.eclipse.org/sparkplug/3.0.0/Eclipse-Sparkplug-TCK-3.0.0.zip
TCK_SHA=a70b2c2f00d67ac714eadd5ac50f6241e0efa26036d95ca8ec667d491021b86b
HIVEMQ_URL=https://github.com/hivemq/hivemq-community-edition/releases/download/2025.5/hivemq-ce-2025.5.zip
HIVEMQ_SHA=4db96cc5a0f5aaf9c8c5fa756e73014a588864b9a7d538f288196d294704624e
JDK_URL=https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.13%2B9/OpenJDK21U-jdk_x64_linux_hotspot_21.0.13_9.tar.gz
JDK_SHA=d7f5d5cf865a2902b70ff549b97586a7d3f234e016f041ab447ea740c7d66d1c

fetch() {   # url file sha256
    if [ ! -s "$DIR/dl/$2" ]; then
        curl -fsSL -o "$DIR/dl/$2.part" "$1"
        mv "$DIR/dl/$2.part" "$DIR/dl/$2"
    fi
    echo "$3  $DIR/dl/$2" | sha256sum -c --quiet -
}

fetch "$TCK_URL" tck.zip "$TCK_SHA"
fetch "$HIVEMQ_URL" hivemq-ce.zip "$HIVEMQ_SHA"
fetch "$JDK_URL" jdk.tar.gz "$JDK_SHA"

rm -rf "$DIR/jdk" "$DIR/hivemq" "$DIR/SparkplugTCK"
mkdir "$DIR/jdk"
tar xzf "$DIR/dl/jdk.tar.gz" -C "$DIR/jdk" --strip-components=1
(cd "$DIR" && unzip -q dl/tck.zip)
TMP="$(mktemp -d)"
(cd "$TMP" && unzip -q "$DIR/dl/hivemq-ce.zip")
mv "$TMP"/hivemq-ce-* "$DIR/hivemq"
rmdir "$TMP"
(cd "$DIR/hivemq/extensions" && unzip -q "$DIR/SparkplugTCK/hivemq-extension/sparkplug-tck-3.0.0.zip")

"$DIR/jdk/bin/java" -version 2>&1 | head -1
ls "$DIR/hivemq/extensions"
echo "TCK ready in $DIR: run the E2E with E2E_TCK_DIR=$DIR --only tck"
