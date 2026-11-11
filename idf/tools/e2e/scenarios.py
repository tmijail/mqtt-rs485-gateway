#!/usr/bin/env python3
"""Issue #121: which scenarios a run of the E2E harness selects (--only), and how the CI splits
a full run into parallel jobs. No third-party imports: the CI reads it without the harness'
packages, e2e.py dispatches with it and test_harness.py checks the groups against it.

    python3 scenarios.py matrix                 # GitHub outputs: build=<json>, run=<json>
    python3 scenarios.py groups                 # one line per CI group: id, --only, plain 0/1
    python3 scenarios.py line [--header] VARIANT GROUP DIR
                                                # one Markdown table row for a group's workdir
    python3 scenarios.py summary DIR [VARIANT]  # every group's results (DIR/e2e-result-*/):
                                                # table on stdout, exit 1 on any failure
"""
import json
import pathlib
import re
import sys

# The scenarios of a run, in the order they run, each with the --only names that select it.
# "ota" stays the union of the OTA scenarios for local use; the finer ota-* names let the CI put
# them into separate jobs.
SCENARIOS = (
    ("failsafe", ("failsafe",)),
    ("network", ("network",)),
    ("web", ("web",)),
    ("reset", ("reset",)),
    ("tls", ("tls",)),
    ("failover", ("failover",)),
    ("mqtt5", ("mqtt5",)),
    ("max_packet", ("mqtt5",)),
    ("phost", ("phost",)),
    ("sf", ("sf",)),
    ("sf spill", ("sf",)),
    ("stack margin", ("sf",)),
    ("https", ("https",)),
    ("FW-1 OTA", ("ota", "ota-fw1")),
    ("issue #112 park in a long RS485 transaction", ("ota", "ota-park")),
    ("issue #97 OTA trial wiring", ("ota", "ota-wiring")),
    ("issues #99/#101 OTA trial", ("ota", "ota-trial")),
    ("issues #67/#94 NVS migration", ("nvs",)),
    ("issue #109 full unencrypted NVS", ("nvs",)),
    ("issue #94 OTA to NVS encryption", ("nvs",)),
    ("slow", ("slow",)),
    ("refuse", ("refuse",)),   # part of "main"; alone only when "main" is not selected (#89)
    ("main", ("main",)),
    ("tck", ("tck",)),         # issue #176: the Sparkplug TCK, optional (OPTIONAL)
)
ONLY_NAMES = tuple(dict.fromkeys(n for _, names in SCENARIOS for n in names))
# Scenarios that run only when --only names them: not in a full run and not in the CI groups.
# 'tck' needs Java, HiveMQ CE and the TCK extension (E2E_TCK_DIR, tools/e2e/tck.py).
OPTIONAL = {"tck"}


def parse_only(text):
    """The set of --only names in @text (comma-separated); ValueError on an unknown name (a typo
    would otherwise select nothing and pass)."""
    only = set(filter(None, (n.strip() for n in text.split(","))))
    unknown = sorted(only - set(ONLY_NAMES))
    if unknown:
        raise ValueError(f"unknown --only name(s): {', '.join(unknown)} "
                         f"(known: {','.join(ONLY_NAMES)})")
    return only


def selected(only):
    """Names of the scenarios a run with the --only set @only (empty: all) runs, in order."""
    out = []
    for name, names in SCENARIOS:
        if name == "refuse":
            if "refuse" in only and "main" not in only:
                out.append(name)
        elif name in OPTIONAL:
            if only & set(names):
                out.append(name)
        elif not only or only & set(names):
            out.append(name)
    return out


# The CI's E2E groups: (id, --only). One job per group and variant runs them in parallel; their
# union must be the full run, each scenario in exactly one group (test_harness.py checks it, and
# the summary job checks it on the results). Balanced on the PSRAM durations of a full run (s):
# #97 wiring 496, FW-1 OTA 487, #99/#101 trial 369, main 201, the three nvs 201, the rest 15-80.
CI_GROUPS = (
    ("ota-wiring", "network,ota-wiring"),   # network: the wiring scenario captures no payload
    ("ota-fw1", "ota-fw1"),
    ("ota-trial", "ota-park,ota-trial"),
    ("nvs-main", "nvs,main"),
    ("other", "failsafe,web,reset,tls,failover,mqtt5,phost,sf,https,slow"),
)
# The CI's variants: the environment of their build and of their runs.
VARIANTS = (("default", "0"), ("psram", "1"))   # (id, E2E_PSRAM)
# The variant whose build job also builds build-qemu-plain (without PSRAM in both variants).
PLAIN_BUILDER = "default"
# Scenarios that need the build without NVS encryption (run_e2e.sh E2E_PLAIN; without it the
# harness skips them).
PLAIN_SCENARIOS = {"issue #94 OTA to NVS encryption"}
# Scenarios with no MQTT observer: a run of only these fails the run-level check "6. payloads
# decode" (it requires at least one captured payload).
NO_CAPTURE = {"issue #97 OTA trial wiring", "https", "refuse"}
# The checks that the harness makes once per run, after the scenarios (each CI group repeats
# them). REL-4 runs only with "main", so once in the CI too.
RUN_CHECKS = ("6. payloads decode", "Issue #105 no message reached", "Issue #92 the fault monitor",
              "Issue #111 every park of core 1", "SPB-5 seq continuity per session",
              "harness completed every scenario")


def group_scenarios(only_text):
    return selected(parse_only(only_text))


def needs_plain(only_text):
    return bool(set(group_scenarios(only_text)) & PLAIN_SCENARIOS)


def matrix():
    build = [{"variant": v, "psram": p, "plain": "1" if v == PLAIN_BUILDER else "0"}
             for v, p in VARIANTS]
    run = [{"variant": v, "psram": p, "group": g, "only": only,
            "plain": "1" if needs_plain(only) else "0"}
           for v, p in VARIANTS for g, only in CI_GROUPS]
    return {"build": {"include": build}, "run": {"include": run}}


def max_retries():
    """MAX_RETRIES of e2e.py (read as text: e2e.py needs the harness' packages)."""
    text = (pathlib.Path(__file__).resolve().parent / "e2e.py").read_text()
    return int(re.search(r"^MAX_RETRIES = (\d+)", text, re.M).group(1))


def ran(results):
    """The scenarios in results.json's durations ("name #attempt"), without repeats, in order."""
    return list(dict.fromkeys(re.sub(r" #\d+$", "", k) for k in results.get("durations", {})))


def read_group(d):
    """(results or None, qemu-exits lines) of the work or result directory @d."""
    d = pathlib.Path(d)
    try:
        results = json.loads((d / "results.json").read_text())
    except (OSError, ValueError):
        results = None
    try:
        exits = [ln for ln in (d / "qemu-exits.txt").read_text().splitlines()
                 if ln.strip() and ln.strip() != "none"]
    except OSError:
        exits = None
    return results, exits


def row(variant, group, d):
    results, exits = read_group(d)
    if results is None:
        return f"| {variant} | {group} | no results.json | | |"
    checks = results.get("checks", [])
    ok = sum(1 for c in checks if c.get("ok"))
    retries = results.get("retries", [])
    rtext = "none" if not retries else "; ".join(f"{r['scenario']} ({r['qemu']})"
                                                 for r in retries)
    etext = "missing" if exits is None else ("none" if not exits else "; ".join(exits))
    return (f"| {variant} | {group} | {ok}/{len(checks)} checks passed | retries: {rtext} | "
            f"qemu-exits: {etext} |")


HEADER = "| Variant | Group | Checks | Retries | QEMU exits |\n|---|---|---|---|---|"


def summary(root, variants=None):
    """Markdown for every (variant, group) of the CI in @root/e2e-result-<variant>-<group>/
    (@variants: these ids only; default all), and the problems: a group without results or with a failed check, a scenario that ran in no
    group or in two, more retries than one full run may have (MAX_RETRIES)."""
    root = pathlib.Path(root)
    lines, problems = [HEADER], []
    full = selected(set())
    for variant, _ in VARIANTS:
        if variants and variant not in variants:
            continue
        seen, checks, failed, retries, run_level, groups = [], 0, 0, 0, 0, 0
        for group, only in CI_GROUPS:
            d = root / f"e2e-result-{variant}-{group}"
            lines.append(row(variant, group, d))
            results, _ = read_group(d)
            if results is None:
                problems.append(f"{variant}/{group}: no results.json")
                continue
            groups += 1
            c = results.get("checks", [])
            checks += len(c)
            bad = [x["criterion"] for x in c if not x.get("ok")]
            failed += len(bad)
            if bad:
                problems.append(f"{variant}/{group}: {len(bad)} failed check(s), first: {bad[0]}")
            retries += len(results.get("retries", []))
            run_level += sum(1 for x in c if x["criterion"].startswith(RUN_CHECKS))
            got = ran(results)
            want = group_scenarios(only)
            if got != want:
                problems.append(f"{variant}/{group}: ran {got}, expected {want}")
            seen += got
        missing = [s for s in full if s not in seen]
        twice = sorted({s for s in seen if seen.count(s) > 1})
        if groups == len(CI_GROUPS) and (missing or twice):
            problems.append(f"{variant}: scenarios in no group {missing}, in two {twice}")
        if retries > max_retries():
            problems.append(f"{variant}: {retries} scenarios repeated, a full run may repeat "
                            f"{max_retries()}")
        # the run-level checks counted once, as in one full run
        once = checks - run_level + (run_level // groups if groups else 0)
        lines.append(f"| {variant} | **all** | {checks - failed}/{checks} checks passed "
                     f"({once} as one full run) | {retries} scenario(s) repeated | |")
    return "\n".join(lines), problems


def main(argv):
    if argv[:1] == ["matrix"]:
        m = matrix()
        print(f"build={json.dumps(m['build'])}")
        print(f"run={json.dumps(m['run'])}")
        return 0
    if argv[:1] == ["groups"]:
        for g, only in CI_GROUPS:
            print(g, only, "1" if needs_plain(only) else "0")
        return 0
    if argv[:1] == ["line"]:
        args = [a for a in argv[1:] if a != "--header"]
        if len(args) == 3:
            if "--header" in argv:
                print(HEADER)
            print(row(*args))
            return 0
    if argv[:1] == ["summary"] and len(argv) >= 2:
        unknown = set(argv[2:]) - {v for v, _ in VARIANTS}
        if unknown:
            print(f"unknown variant(s) {sorted(unknown)}", file=sys.stderr)
            return 2
        text, problems = summary(argv[1], argv[2:])
        print(text)
        if problems:
            print("\n**Failed:**\n" + "\n".join(f"- {p}" for p in problems))
            for p in problems:
                print(f"::error title=E2E groups::{p}", file=sys.stderr)
        return 1 if problems else 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
