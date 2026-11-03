#!/usr/bin/env bash
# Regenerates components/gw_core/proto/sparkplug_b.pb.{c,h} from the unmodified
# Eclipse Tahu sparkplug_b.proto and sparkplug_b.options.
# Requires the nanopb generator matching the vendored runtime: pip install nanopb==0.4.9.1
set -euo pipefail
cd "$(dirname "$0")/../components/gw_core/proto"
ver="$(nanopb_generator --version 2>&1 | grep -o '0\.4\.[0-9.]*' | head -1)"
if [ "$ver" != "0.4.9.1" ]; then
    echo "nanopb generator 0.4.9.1 required (found '${ver:-none}')" >&2
    exit 1
fi
nanopb_generator -q sparkplug_b.proto
