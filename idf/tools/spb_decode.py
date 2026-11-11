#!/usr/bin/env python3
"""Decode Sparkplug B payloads with the official Eclipse Tahu sparkplug_b.proto.

Interoperability check (SPEC section 7, criterion 6): the payloads produced by the
firmware are parsed by Google's protobuf runtime, independently of nanopb.

    python3 tools/spb_decode.py FILE_OR_DIR...        # *.bin files, as written by spb_dump
    python3 tools/spb_decode.py --check DIR           # also assert Sparkplug structure

--check examines each message, and also the messages of one node in the order of their file
numbers (seq continuity, alias uniqueness across the NBIRTH and the DBIRTHs) when the file
names give the node (spb_dump: <n>_<group>+<TYPE>+<edge>[+<device>].bin).

Requires: pip install protobuf grpcio-tools
"""
import argparse
import importlib
import pathlib
import sys
import tempfile

PROTO = pathlib.Path(__file__).resolve().parent.parent / "components/gw_core/proto/sparkplug_b.proto"
REBIRTH = "Node Control/Rebirth"
TIMESTAMPED = ("NBIRTH", "DBIRTH", "NDATA", "DDATA")

DATATYPE_FIELD = {  # Sparkplug datatype -> oneof field that must carry the value
    1: "int_value", 2: "int_value", 3: "int_value", 5: "int_value", 6: "int_value", 7: "int_value",
    4: "long_value", 8: "long_value", 13: "long_value",
    9: "float_value", 10: "double_value", 11: "boolean_value", 12: "string_value", 14: "string_value",
    16: "dataset_value", 17: "bytes_value", 18: "bytes_value", 19: "template_value",
}


def check_template(kind, m):
    """SPB-18: definitions only in NBIRTH, without alias or template_ref; instances name their
    definition; members carry name and datatype and no alias, and are not templates."""
    if m.datatype != 19 or m.is_null:
        return []
    t, errors = m.template_value, []
    if t.is_definition:
        if kind != "NBIRTH":
            errors.append(f"{m.name}: template definition outside NBIRTH")
        if m.HasField("alias") or t.HasField("template_ref"):
            errors.append(f"{m.name}: a template definition has no alias and no template_ref")
    elif not t.template_ref:
        errors.append(f"alias {m.alias}: template instance without template_ref")
    for mm in t.metrics:
        if not (mm.HasField("name") and mm.HasField("datatype")) or mm.HasField("alias"):
            errors.append(f"{m.name or m.alias}: member without name/datatype or with an alias")
        if mm.datatype == 19:
            errors.append(f"{m.name or m.alias}: nested template member {mm.name}")
        expected = DATATYPE_FIELD.get(mm.datatype)
        if not mm.is_null and not t.is_definition and expected and mm.WhichOneof("value") != expected:
            errors.append(f"member {mm.name}: datatype {mm.datatype} in {mm.WhichOneof('value')}")
        errors += check_properties(mm)
    return errors
CELL_FIELD = {k: v for k, v in DATATYPE_FIELD.items() if k <= 14}   # SPB-19 DataSet columns


def check_dataset(m):
    """SPB-19: num_of_columns, names and types agree; every row has one element per column,
    in the field its column type asks for."""
    if m.datatype != 16 or m.is_null:
        return []
    ds, errors = m.dataset_value, []
    if not (ds.num_of_columns == len(ds.columns) == len(ds.types)):
        errors.append(f"alias {m.alias}: DataSet num_of_columns {ds.num_of_columns}, "
                      f"{len(ds.columns)} names, {len(ds.types)} types")
    for r, row in enumerate(ds.rows):
        if len(row.elements) != len(ds.types):
            errors.append(f"alias {m.alias}: DataSet row {r} has {len(row.elements)} elements")
            continue
        for c, (e, t) in enumerate(zip(row.elements, ds.types)):
            if e.WhichOneof("value") != CELL_FIELD.get(t):
                errors.append(f"alias {m.alias}: DataSet cell [{r}][{c}] of type {t} in "
                              f"{e.WhichOneof('value')}")
    return errors


def load_module():
    from grpc_tools import protoc

    out = tempfile.mkdtemp(prefix="spb_py_")
    rc = protoc.main(["protoc", f"-I{PROTO.parent}", f"--python_out={out}", str(PROTO)])
    if rc != 0:
        raise SystemExit("protoc failed")
    sys.path.insert(0, out)
    return importlib.import_module("sparkplug_b_pb2")


PROPERTY_TYPES = {"engUnit": (12, "string_value"), "engLow": (9, "float_value"),
                  "engHigh": (9, "float_value"), "Quality": (3, "int_value")}
QUALITY_CODES = (0, 192, 500)


def props_of(m):
    """SPB-16: the metric's properties as {key: PropertyValue}."""
    if not m.HasField("properties"):
        return {}
    return dict(zip(m.properties.keys, m.properties.values))


def check_properties(m):
    errors = []
    if not m.HasField("properties"):
        return errors
    ps = m.properties
    if len(ps.keys) != len(ps.values):
        errors.append(f"alias {m.alias}: {len(ps.keys)} property keys but {len(ps.values)} values")
    if len(set(ps.keys)) != len(ps.keys):
        errors.append(f"alias {m.alias}: duplicated property keys {list(ps.keys)}")
    for key, pv in props_of(m).items():
        if key in PROPERTY_TYPES:
            dt, field = PROPERTY_TYPES[key]
            if pv.type != dt or pv.WhichOneof("value") != field:
                errors.append(f"alias {m.alias}: property {key} must be type {dt} in {field}")
        if key == "Quality" and pv.int_value not in QUALITY_CODES:
            errors.append(f"alias {m.alias}: Quality {pv.int_value} is not 0, 192 or 500")
    q = props_of(m).get("Quality")
    if q is not None and q.int_value == 0 and not m.is_null and m.datatype != 19:
        errors.append(f"alias {m.alias}: Quality BAD with a value (a failed read is null)")
    return errors


def value_of(m):
    which = m.WhichOneof("value")
    if which == "template_value":
        t = m.template_value
        head = "definition" if t.is_definition else f"instance of {t.template_ref}"
        return {head: {mm.name: value_of(mm) for mm in t.metrics}}
    if which == "dataset_value":
        ds = m.dataset_value
        return {"columns": list(ds.columns), "types": list(ds.types),
                "rows": [[value_of(e) for e in row.elements] for row in ds.rows]}
    return None if which is None else getattr(m, which)


def describe(kind, p):
    lines = [f"{kind}: timestamp={p.timestamp} seq={p.seq if p.HasField('seq') else '-'} "
             f"metrics={len(p.metrics)}"]
    for m in p.metrics:
        name = m.name if m.HasField("name") else "<alias only>"
        null = " is_null" if m.is_null else ""
        props = props_of(m)
        extra = (" {" + ", ".join(f"{k}={value_of(v)!r}" for k, v in props.items()) + "}") if props else ""
        lines.append(f"    alias={m.alias:<4} dt={m.datatype:<2} {name} = {value_of(m)!r}{null}{extra}")
    return "\n".join(lines)


def check(kind, p):
    """Structural rules for messages published by the edge node (commands are host-side)."""
    errors = []
    if kind in ("NCMD", "DCMD", "STATE"):
        return errors
    if kind == "NDEATH":
        if p.HasField("seq"):
            errors.append("NDEATH must not carry seq")
        # tck-id-message-flow-edge-node-birth-publish-will-message-payload-bdSeq
        if [m.name for m in p.metrics] != ["bdSeq"] or p.metrics[0].datatype != 4:
            errors.append("NDEATH must carry exactly the Int64 bdSeq metric")
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
        definition = m.datatype == 19 and m.template_value.is_definition
        rebirth = kind == "NBIRTH" and m.name == REBIRTH
        if births and not (m.HasField("name") and (definition or rebirth or m.HasField("alias"))
                           and m.HasField("datatype")):
            errors.append(f"birth metric without name/alias/datatype: {m}")
        # tck-id-operational-behavior-data-commands-rebirth-name-aliases (#153)
        if rebirth and m.HasField("alias"):
            errors.append(f"NBIRTH {REBIRTH} must not have an alias (alias {m.alias})")
        if not births and m.HasField("name"):
            errors.append(f"{kind} metric should use the alias only: {m.name}")
        # tck-id-payloads-name-birth-data-requirement: a timestamp on every metric of NBIRTH,
        # DBIRTH, NDATA and DDATA (and on the members of a template instance, which are values;
        # the members of a definition are not)
        if kind in TIMESTAMPED:
            members = [] if definition or m.datatype != 19 else list(m.template_value.metrics)
            for mm in [m] + members:
                if not mm.HasField("timestamp"):
                    errors.append(f"{kind} metric without timestamp: "
                                  f"{mm.name or 'alias ' + str(mm.alias)}")
        if m.HasField("alias"):
            if m.alias in aliases:
                errors.append(f"duplicate alias {m.alias}")
            aliases.add(m.alias)
        expected = DATATYPE_FIELD.get(m.datatype)
        if not m.is_null and expected and m.WhichOneof("value") != expected:
            errors.append(f"alias {m.alias}: datatype {m.datatype} carried in {m.WhichOneof('value')}")
        errors += check_properties(m)
        errors += check_dataset(m)
        errors += check_template(kind, m)
    if kind == "NBIRTH":
        names = {m.name for m in p.metrics}
        for required in ("bdSeq", REBIRTH, "Node Control/Reboot",
                         "Node Info/Firmware", "Node Info/Hardware", "Node Info/Reset Reason",
                         "Node Control/Scan Rate",
                         *(f"DI/{i}" for i in range(1, 9)), *(f"DO/{i}" for i in range(1, 9))):
            if required not in names:
                errors.append(f"NBIRTH lacks {required}")
        # tck-id-message-flow-edge-node-birth-publish-nbirth-payload-bdSeq
        if any(m.name == "bdSeq" and m.datatype != 4 for m in p.metrics):
            errors.append("NBIRTH bdSeq must have the datatype Int64 (4)")
    return errors


class Sessions:
    """Rules across the messages of one Edge Node (#166). Feed every message of the node in
    publish order: a new NBIRTH starts a new session, an NDEATH ends it.

    - tck-id-payloads-sequence-num-incrementing: each message after the NBIRTH has the seq of
      the message before it plus one, 255 followed by 0.
    - tck-id-payloads-alias-uniqueness: an alias of the NBIRTH or of a DBIRTH is unique across
      all the metrics of the node: the NBIRTH and the DBIRTHs of all its devices. A new DBIRTH
      of the same device (after a DDEATH) replaces the aliases of that device.
    """

    def __init__(self):
        self.nodes = {}

    def feed(self, kind, node, device, p):
        """Errors of message @p p (type @p kind) from @p node ((group, edge)) and @p device
        (None for a node message). A message without a node gives no error here."""
        if node is None or kind in ("NCMD", "DCMD", "STATE"):
            return []
        where = "/".join(node) + (f"/{device}" if device else "")
        if kind == "NDEATH":
            self.nodes.pop(node, None)
            return []
        if kind == "NBIRTH":
            self.nodes[node] = {"next": 0, "node": set(), "devices": {}}
        s = self.nodes.get(node)
        if s is None:
            return [f"{kind} {where} before the NBIRTH of its node"]
        errors = []
        if p.HasField("seq"):
            if p.seq != s["next"]:
                errors.append(f"{kind} {where}: seq {p.seq}, expected {s['next']} "
                              "(the seq of the message before it plus one)")
            s["next"] = (p.seq + 1) % 256
        aliases = {m.alias for m in p.metrics if m.HasField("alias")}
        if kind == "NBIRTH":
            s["node"] = aliases
        elif kind == "DBIRTH":
            for owner, used in [("the NBIRTH", s["node"])] + [
                    (f"the DBIRTH of {d}", a) for d, a in s["devices"].items() if d != device]:
                for alias in sorted(aliases & used):
                    errors.append(f"DBIRTH {where}: alias {alias} is also in {owner}")
            s["devices"][device] = aliases
        return errors


def parse_name(path):
    """(kind, node, device) of a payload file. spb_dump writes
    <n>_<group>+<TYPE>+<edge>[+<device>].bin (the topic after spBv1.0/, '+' for '/'); node is
    (group, edge). Other names (<n>_<TYPE>.bin) give no node."""
    rest = path.stem.split("_", 1)[-1]
    parts = rest.split("+")
    if len(parts) >= 3:
        return parts[1], (parts[0], parts[2]), parts[3] if len(parts) > 3 else None
    return rest.split("_")[-1], None, None


def publish_order(path):
    """Sort key: the number before the first '_' (10 after 9), then the name."""
    head = path.stem.split("_", 1)[0]
    return (int(head) if head.isdigit() else sys.maxsize, path.name)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", type=pathlib.Path)
    ap.add_argument("--check", action="store_true", help="validate Sparkplug structure")
    args = ap.parse_args()
    spb = load_module()
    files = []
    for path in args.paths:
        files += sorted(path.glob("*.bin"), key=publish_order) if path.is_dir() else [path]
    failed = 0
    sessions = Sessions()
    for f in files:
        kind, node, device = parse_name(f)
        p = spb.Payload()
        p.ParseFromString(f.read_bytes())  # raises DecodeError on malformed input
        print(f"[{f.name}] " + describe(kind, p))
        if args.check:
            for e in check(kind, p) + sessions.feed(kind, node, device, p):
                print(f"    ERROR: {e}")
                failed += 1
    print(f"{len(files)} payload(s) decoded with the official proto, {failed} structural error(s)")
    return 1 if failed or not files else 0


if __name__ == "__main__":
    sys.exit(main())
