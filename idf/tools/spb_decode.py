#!/usr/bin/env python3
"""Decode Sparkplug B payloads with the official Eclipse Tahu sparkplug_b.proto.

Interoperability check (SPEC section 7, criterion 6): the payloads produced by the
firmware are parsed by Google's protobuf runtime, independently of nanopb.

    python3 tools/spb_decode.py FILE_OR_DIR...        # *.bin files, as written by spb_dump
    python3 tools/spb_decode.py --check DIR           # also assert Sparkplug structure

Requires: pip install protobuf grpcio-tools
"""
import argparse
import importlib
import pathlib
import sys
import tempfile

PROTO = pathlib.Path(__file__).resolve().parent.parent / "components/gw_core/proto/sparkplug_b.proto"

DATATYPE_FIELD = {  # Sparkplug datatype -> oneof field that must carry the value
    1: "int_value", 2: "int_value", 3: "int_value", 5: "int_value", 6: "int_value", 7: "int_value",
    4: "long_value", 8: "long_value", 13: "long_value",
    9: "float_value", 10: "double_value", 11: "boolean_value", 12: "string_value", 14: "string_value",
}


def load_module():
    from grpc_tools import protoc

    out = tempfile.mkdtemp(prefix="spb_py_")
    rc = protoc.main(["protoc", f"-I{PROTO.parent}", f"--python_out={out}", str(PROTO)])
    if rc != 0:
        raise SystemExit("protoc failed")
    sys.path.insert(0, out)
    return importlib.import_module("sparkplug_b_pb2")


def value_of(m):
    which = m.WhichOneof("value")
    return None if which is None else getattr(m, which)


def describe(kind, p):
    lines = [f"{kind}: timestamp={p.timestamp} seq={p.seq if p.HasField('seq') else '-'} "
             f"metrics={len(p.metrics)}"]
    for m in p.metrics:
        name = m.name if m.HasField("name") else "<alias only>"
        null = " is_null" if m.is_null else ""
        lines.append(f"    alias={m.alias:<4} dt={m.datatype:<2} {name} = {value_of(m)!r}{null}")
    return "\n".join(lines)


def check(kind, p):
    """Structural rules for messages published by the edge node (commands are host-side)."""
    errors = []
    if kind in ("NCMD", "DCMD", "STATE"):
        return errors
    if kind == "NDEATH":
        if p.HasField("seq"):
            errors.append("NDEATH must not carry seq")
        if [m.name for m in p.metrics] != ["bdSeq"] or p.metrics[0].datatype != 8:
            errors.append("NDEATH must carry exactly the UInt64 bdSeq metric")
        return errors
    if not p.HasField("seq") or not 0 <= p.seq <= 255:
        errors.append("seq missing or out of 0..255")
    if not p.HasField("timestamp"):
        errors.append("timestamp missing")
    if kind == "NBIRTH" and p.seq != 0:
        errors.append("NBIRTH seq must be 0")
    births = kind in ("NBIRTH", "DBIRTH")
    aliases = set()
    for m in p.metrics:
        if births and not (m.HasField("name") and m.HasField("alias") and m.HasField("datatype")):
            errors.append(f"birth metric without name/alias/datatype: {m}")
        if not births and m.HasField("name"):
            errors.append(f"{kind} metric should use the alias only: {m.name}")
        if m.alias in aliases:
            errors.append(f"duplicate alias {m.alias}")
        aliases.add(m.alias)
        expected = DATATYPE_FIELD.get(m.datatype)
        if not m.is_null and expected and m.WhichOneof("value") != expected:
            errors.append(f"alias {m.alias}: datatype {m.datatype} carried in {m.WhichOneof('value')}")
    if kind == "NBIRTH":
        names = {m.name for m in p.metrics}
        for required in ("bdSeq", "Node Control/Rebirth", "Node Control/Reboot",
                         "Node Info/Firmware", "Node Info/Hardware", "Node Info/Reset Reason",
                         *(f"DI/{i}" for i in range(1, 9)), *(f"DO/{i}" for i in range(1, 9))):
            if required not in names:
                errors.append(f"NBIRTH lacks {required}")
    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", type=pathlib.Path)
    ap.add_argument("--check", action="store_true", help="validate Sparkplug structure")
    args = ap.parse_args()
    spb = load_module()
    files = []
    for path in args.paths:
        files += sorted(path.glob("*.bin")) if path.is_dir() else [path]
    failed = 0
    for f in files:
        kind = f.stem.split("_")[-1]
        p = spb.Payload()
        p.ParseFromString(f.read_bytes())  # raises DecodeError on malformed input
        print(f"[{f.name}] " + describe(kind, p))
        if args.check:
            for e in check(kind, p):
                print(f"    ERROR: {e}")
                failed += 1
    print(f"{len(files)} payload(s) decoded with the official proto, {failed} structural error(s)")
    return 1 if failed or not files else 0


if __name__ == "__main__":
    sys.exit(main())
