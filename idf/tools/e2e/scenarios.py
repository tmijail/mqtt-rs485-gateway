#!/usr/bin/env python3
"""Issue #121: which scenarios a run of the E2E harness selects (--only), and how the CI splits
a full run into parallel jobs. No third-party imports: the CI reads it without the harness'
packages, e2e.py dispatches with it and test_harness.py checks the groups against it.

    python3 scenarios.py matrix                 # GitHub outputs: build=<json>, run=<json>
    python3 scenarios.py groups [VARIANT]       # one line per CI group (of VARIANT): id, --only
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
# them into separate jobs. Issue #233: FW-1 OTA is two scenarios, each with its own boot (by NCMD,
# by web upload); "ota-fw1" selects the two. Issue #261: "nvs" is the union of the scenarios of
# the NVS encryption: "nvs-boot" (at a boot) and "nvs-ota" (with an OTA update).
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
    ("time", ("time",)),   # issue #160: no NDATA/DDATA before a valid clock
    ("FW-1 OTA by NCMD", ("ota", "ota-fw1", "ota-fw1-ncmd")),
    ("FW-1 OTA by web upload", ("ota", "ota-fw1", "ota-fw1-web")),
    ("issue #112 park in a long RS485 transaction", ("ota", "ota-park")),
    ("issue #97 OTA trial wiring", ("ota", "ota-wiring")),
    ("issue #144 OTA self-check", ("ota", "ota-trial")),
    ("CFG-7 NVS encryption", ("nvs", "nvs-boot")),   # issue #261: only in the secure build
    ("issues #67/#94 NVS migration", ("nvs", "nvs-boot")),
    ("issue #109 full unencrypted NVS", ("nvs", "nvs-boot")),
    ("issue #94 OTA to NVS encryption", ("nvs", "nvs-ota")),
    ("FW-2 OTA signature", ("signed",)),             # issue #261: only in the secure build
    ("slow", ("slow",)),
    ("issue #135 no PSRAM", ("nopsram",)),
    ("refuse", ("refuse",)),   # part of "main"; alone only when "main" is not selected (#89)
    ("main", ("main",)),
    ("umh", ("umh",)),         # optional: only when named (issue #175)
    ("tck", ("tck",)),         # optional: only when named (issue #176, the Sparkplug TCK)
    ("downgrade", ("downgrade",)),   # optional: only when named (issue #148, E2E_DOWNGRADE_REF)
)
# Issues #175, #176 and #148: scenarios that a run selects only when --only names them. A full run
# and the CI groups leave them out: they need programs that are not in the CI image (benthos-umh;
# Java, HiveMQ CE and the TCK extension: E2E_TCK_DIR, tools/e2e/tck.py) or the build of an
# earlier release (E2E_DOWNGRADE_REF, run_e2e.sh).
OPTIONAL = {"umh", "tck", "downgrade"}
ONLY_NAMES = tuple(dict.fromkeys(n for _, names in SCENARIOS for n in names))

# Issue #135: the firmware assumes the PSRAM of the board. The PSRAM variant ("psram") runs the
# configuration of the board, so a full run of it runs every scenario that depends on memory
# (the #80 margins of the internal RAM among them: "tls"). The variant without PSRAM ("default":
# the QEMU build with CONFIG_GW_EMU_NO_PSRAM) keeps only these scenarios: they examine logic,
# not memory, and they are 4 to 7 times slower with PSRAM (single-threaded TCG, #107, with the
# software AES and SHA of sdkconfig.qemu-psram). FW-1 OTA (its two scenarios) stays with PSRAM: on the board its
# transfer buffers and its TLS session are in PSRAM. --only selects in any variant.
DEFAULT_VARIANT_SCENARIOS = {"issue #112 park in a long RS485 transaction",
                             "issue #97 OTA trial wiring", "issue #144 OTA self-check"}
# Scenarios that need the PSRAM build (the harness skips them without --psram).
PSRAM_BUILD_SCENARIOS = {"issue #135 no PSRAM"}
# Issue #261: the variants "psram" and "default" run the configuration of the board: no NVS
# encryption, no signature check of an OTA image. These two functions stay in main only in the
# QEMU build (decision of #133, for the optional owner profile of #255): the secure variant
# ("secure": the PSRAM build with sdkconfig.qemu-secure, run_e2e.sh E2E_SECURE=1) has them, and
# its full run has only the scenarios that examine them. They need that build: the harness skips
# them without --secure. --only selects the other scenarios in the secure variant too.
SECURE_VARIANT_SCENARIOS = {"CFG-7 NVS encryption", "issues #67/#94 NVS migration",
                            "issue #109 full unencrypted NVS", "issue #94 OTA to NVS encryption",
                            "FW-2 OTA signature"}
# Scenarios that need the board build next to the secure build (run_e2e.sh E2E_BOARD; without it
# the harness skips them): the firmware before the OTA update to NVS encryption.
BOARD_BUILD_SCENARIOS = {"issue #94 OTA to NVS encryption"}


def would_skip(name, variant):
    """Issue #261: true when the harness shows the scenario @name as [SKIP] in @variant: an
    OPTIONAL scenario (its program or its git ref is not in the CI), or a scenario that needs
    another build. No CI group may hold such a scenario (test_harness.py), and the summary
    reports a group that skipped one (results.json "skipped")."""
    env = dict(VARIANTS)[variant]
    return (name in OPTIONAL
            or (name in SECURE_VARIANT_SCENARIOS and env["secure"] != "1")
            or (name in PSRAM_BUILD_SCENARIOS and env["psram"] != "1"))


def parse_only(text):
    """The set of --only names in @text (comma-separated); ValueError on an unknown name (a typo
    would otherwise select nothing and pass)."""
    only = set(filter(None, (n.strip() for n in text.split(","))))
    unknown = sorted(only - set(ONLY_NAMES))
    if unknown:
        raise ValueError(f"unknown --only name(s): {', '.join(unknown)} "
                         f"(known: {','.join(ONLY_NAMES)})")
    return only


def variant_of(name):
    """Issues #135 and #261: the variant whose full run runs the scenario @name."""
    if name in SECURE_VARIANT_SCENARIOS:
        return "secure"
    return "default" if name in DEFAULT_VARIANT_SCENARIOS else "psram"


def selected(only, variant=None):
    """Names of the scenarios a run with the --only set @only runs, in order. Without --only
    (empty set): the full run of @variant (issues #135 and #261), or of all the variants (None),
    without the OPTIONAL scenarios (issue #175)."""
    out = []
    for name, names in SCENARIOS:
        if name in OPTIONAL:
            if only & set(names):
                out.append(name)
        elif name == "refuse":
            if "refuse" in only and "main" not in only:
                out.append(name)
        elif only:
            if only & set(names):
                out.append(name)
        elif variant is None or variant_of(name) == variant:
            out.append(name)
    return out


# The CI's E2E groups: (variant, id, --only). One job per group runs them in parallel; the union
# of the groups of a variant must be the full run of that variant (issue #135), each scenario in
# exactly one group (test_harness.py checks it, and the summary job checks it on the results).
# Issue #233: balanced on the scenario durations of a full run (a host with 4 CPUs, two groups at
# the same time, the QEMU self-check of 30 s). The longest scenario (FW-1 OTA by web upload) sets
# the floor of the PSRAM variant. Issue #261: the secure variant has two groups, so that none is
# longer than that floor.
CI_GROUPS = (
    ("psram", "ota-fw1-web", "ota-fw1-web,nopsram"),
    ("psram", "ota-fw1-ncmd", "ota-fw1-ncmd,reset,slow"),
    ("psram", "main", "main,https,time"),
    ("psram", "sf-tls", "sf,tls,mqtt5,network,failover"),
    ("psram", "other", "failsafe,web,phost"),
    ("default", "ota-trial", "ota-park,ota-wiring,ota-trial"),
    ("secure", "nvs-ota", "nvs-ota"),
    ("secure", "nvs-signed", "nvs-boot,signed"),
)
# The CI's variants: the environment of their build and of their runs (E2E_PSRAM, E2E_SECURE).
# The build job of the secure variant also builds the board build of the PSRAM variant
# (run_e2e.sh E2E_BOARD): the #94 scenario starts on it.
VARIANTS = (("default", {"psram": "0", "secure": "0"}),
            ("psram", {"psram": "1", "secure": "0"}),
            ("secure", {"psram": "1", "secure": "1"}))
# Scenarios with no MQTT observer: a run of only these fails the run-level check "6. payloads
# decode" (it requires at least one captured payload).
NO_CAPTURE = {"issue #97 OTA trial wiring", "https", "refuse", "issue #135 no PSRAM"}
# The checks that the harness makes once per run, after the scenarios (each CI group repeats
# them). REL-4 runs only with "main", so once in the CI too.
RUN_CHECKS = ("6. payloads decode", "Issue #105 no message reached", "Issue #92 the fault monitor",
              "Issue #111 every park of core 1", "SPB-5 seq continuity per session",
              "harness completed every scenario")


def group_scenarios(only_text):
    return selected(parse_only(only_text))


def groups_of(variant):
    """[(id, --only)] of the CI groups of @variant."""
    return [(g, only) for v, g, only in CI_GROUPS if v == variant]


def matrix():
    build = [{"variant": v, **env} for v, env in VARIANTS]
    envs = dict(VARIANTS)
    run = [{"variant": v, **envs[v], "group": g, "only": only} for v, g, only in CI_GROUPS]
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
    skipped = results.get("skipped", [])
    stext = f"; SKIPPED: {', '.join(skipped)}" if skipped else ""
    retries = results.get("retries", [])
    rtext = "none" if not retries else "; ".join(f"{r['scenario']} ({r['qemu']})"
                                                 for r in retries)
    etext = "missing" if exits is None else ("none" if not exits else "; ".join(exits))
    return (f"| {variant} | {group} | {ok}/{len(checks)} checks passed{stext} | retries: {rtext} | "
            f"qemu-exits: {etext} |")


HEADER = "| Variant | Group | Checks | Retries | QEMU exits |\n|---|---|---|---|---|"


def summary(root, variants=None):
    """Markdown for every (variant, group) of the CI in @root/e2e-result-<variant>-<group>/
    (@variants: these ids only; default all), and the problems: a group without results or with
    a failed check, a scenario that a group skipped (issue #261: a [SKIP] has a duration, as a
    scenario that ran, and no check), a scenario of the variant's full run that ran in no group
    or in two, more retries than one full run may have (MAX_RETRIES)."""
    root = pathlib.Path(root)
    lines, problems = [HEADER], []
    for variant, _ in VARIANTS:
        if variants and variant not in variants:
            continue
        full = selected(set(), variant)
        seen, checks, failed, retries, run_level, groups = [], 0, 0, 0, 0, 0
        for group, only in groups_of(variant):
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
            skipped = results.get("skipped")
            if skipped is None:   # the harness always writes the list: no list, no evidence
                problems.append(f"{variant}/{group}: results.json has no \"skipped\" list")
                skipped = []
            if skipped:
                problems.append(f"{variant}/{group}: skipped {skipped}: each scenario of a CI "
                                f"group must run")
            seen += [name for name in got if name not in skipped]
        missing = [s for s in full if s not in seen]
        twice = sorted({s for s in seen if seen.count(s) > 1})
        if groups == len(groups_of(variant)) and (missing or twice):
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
    if argv[:1] == ["groups"] and len(argv) <= 2:
        for v, g, only in CI_GROUPS:
            if argv[1:] in ([], [v]):
                print(g, only)
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
