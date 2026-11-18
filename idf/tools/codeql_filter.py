#!/usr/bin/env python3
"""Keep only the CodeQL results in the own code of the firmware (issue #232).

The workflow codeql.yml runs this on each SARIF file before upload-sarif. CodeQL ignores
paths-ignore for a compiled language, thus the filter is on the results:

- kept: the results whose first location is in idf/components/ or idf/main/;
- removed: nanopb (idf/components/nanopb/), the generated sparkplug_b.pb.c, the test programs
  (idf/test/), the FetchContent sources of Unity and cJSON (build-host/_deps/), and the files
  outside the repository (ESP-IDF, the toolchain), which have an absolute URI.

CodeQL still follows the data through the removed code. The script changes the files in place
and writes one line per file with the count of each rule.

Usage: codeql_filter.py FILE.sarif [...]
"""

import collections
import json
import re
import sys

KEEP = re.compile(r"^idf/(components|main)/")
DROP = re.compile(r"^idf/components/(nanopb/|gw_core/proto/sparkplug_b\.pb\.c$)")


def first_uri(result):
    for loc in result.get("locations") or []:
        uri = loc.get("physicalLocation", {}).get("artifactLocation", {}).get("uri")
        if uri is not None:
            return uri
    return ""


def own(result):
    uri = first_uri(result)
    return bool(KEEP.match(uri)) and not DROP.match(uri)


def filter_file(path):
    with open(path, encoding="utf-8") as f:
        sarif = json.load(f)
    removed = 0
    rules = collections.Counter()
    for run in sarif.get("runs", []):
        results = run.get("results", [])
        kept = [r for r in results if own(r)]
        removed += len(results) - len(kept)
        run["results"] = kept
        rules.update(r.get("ruleId", "?") for r in kept)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(sarif, f)
    total = sum(rules.values())
    detail = ", ".join(f"{rule} {n}" for rule, n in sorted(rules.items()))
    print(f"{path}: {total} results ({removed} removed)" + (f": {detail}" if detail else ""))


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    for path in argv[1:]:
        filter_file(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
