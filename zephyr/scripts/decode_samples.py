#!/usr/bin/env python3
"""Decode Sparkplug payloads with the official Eclipse Tahu proto.

Input: a log containing lines "SPB_SAMPLE <topic> <hex>" (printed by the
spb_node unit tests) or raw payload files given with --bin. Every payload is
parsed with Python protobuf from proto/sparkplug_b.proto and printed in text
form. The exit status is non-zero if any payload fails to decode or if a
BIRTH metric lacks name, alias, datatype or value.
"""

import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_DIR = os.path.join(HERE, "..", "proto")


def load_proto():
    out = tempfile.mkdtemp(prefix="spb-proto-")
    subprocess.run(["protoc", f"--proto_path={PROTO_DIR}", f"--python_out={out}",
                    os.path.join(PROTO_DIR, "sparkplug_b.proto")], check=True)
    sys.path.insert(0, out)
    import sparkplug_b_pb2  # noqa: E402 (generated above)
    return sparkplug_b_pb2


def check(pb, topic, raw):
    from google.protobuf import text_format

    payload = pb.Payload()
    payload.ParseFromString(raw)  # raises DecodeError on malformed input
    kind = topic.split("/")[2] if topic.count("/") >= 2 else "?"
    problems = []
    if kind in ("NBIRTH", "DBIRTH"):
        for m in payload.metrics:
            if not (m.name and m.HasField("alias") and m.HasField("datatype") and
                    (m.WhichOneof("value") or m.is_null)):
                problems.append(f"incomplete metric {m.name!r}")
    if kind == "NDEATH" and payload.HasField("seq"):
        problems.append("NDEATH must not carry seq")
    if kind != "NDEATH" and not payload.HasField("seq"):
        problems.append("missing seq")
    print(f"== {topic} ({len(raw)} bytes) seq={payload.seq if payload.HasField('seq') else '-'}")
    print(text_format.MessageToString(payload, as_one_line=False), end="")
    for p in problems:
        print(f"!! {p}")
    return not problems


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", nargs="?", help="log with SPB_SAMPLE lines (default stdin)")
    parser.add_argument("--bin", nargs="*", default=[],
                        help="raw payload files named sample_<TYPE>.bin")
    args = parser.parse_args()

    pb = load_proto()
    samples = []
    if args.log or not args.bin:
        src = open(args.log) if args.log else sys.stdin
        for line in src:
            if "SPB_SAMPLE " in line:
                topic, hexdata = line.split("SPB_SAMPLE ", 1)[1].strip().split(" ", 1)
                samples.append((topic, bytes.fromhex(hexdata)))
    for path in args.bin:
        kind = os.path.basename(path).removeprefix("sample_").removesuffix(".bin")
        with open(path, "rb") as f:
            samples.append((f"spBv1.0/-/{kind}/-", f.read()))

    ok = 0
    for topic, raw in samples:
        try:
            ok += check(pb, topic, raw)
        except Exception as exc:  # noqa: BLE001 - report any decode failure
            print(f"!! {topic}: {exc}")
    print(f"\n{ok}/{len(samples)} payloads decoded and valid")
    return 0 if samples and ok == len(samples) else 1


if __name__ == "__main__":
    sys.exit(main())
