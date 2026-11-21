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


def is_template(m):
    """A template metric: by its datatype (births) or by its value field (DATA, no datatype)."""
    return m.datatype == 19 or m.WhichOneof("value") == "template_value"


def param_value(p):
    """The value of a Template parameter, or None when it has no value."""
    which = p.WhichOneof("value")
    if which is None:
        return None
    v = getattr(p, which)
    if p.type == 4 and which == "long_value" and v >= 1 << 63:   # Int64: two's complement
        v -= 1 << 64
    return v


def check_parameters(kind, m):
    """#251, #276: the parameters of the template metric @p m.

    - tck-id-payloads-template-parameter-name-required: each parameter has a name (and only one
      parameter of a template has that name).
    - tck-id-payloads-template-parameter-type-req: each parameter of a birth has a type.
    - tck-id-payloads-template-parameter-type-value: the type is a basic datatype (1 to 14).
    - tck-id-payloads-template-parameter-value: a value is in the field of the type.
    - The gateway (#276): the parameters are the parameters that the user defined for the device
      type. A template of a birth has a maximum of 8 of them. Each one is a String, an Int64, a
      Double or a Boolean, has a name of 1 to 39 bytes, and always has a value (a String of a
      maximum of 63 bytes). The instance of a DATA message has no parameters."""
    t, errors = m.template_value, []
    who = m.name or f"alias {m.alias}"
    births = kind in ("NBIRTH", "DBIRTH")
    names = [p.name for p in t.parameters]
    if len(set(names)) != len(names):
        errors.append(f"{who}: duplicated template parameters {names}")
    for p in t.parameters:
        if not p.HasField("name") or not p.name:
            errors.append(f"{who}: template parameter without name")
        if births and not p.HasField("type"):
            errors.append(f"{who}: template parameter {p.name} without type in a birth")
        if p.HasField("type") and p.type not in PARAM_FIELD:
            errors.append(f"{who}: template parameter {p.name} of type {p.type}, "
                          "not a basic datatype")
        which = p.WhichOneof("value")
        if which is not None and p.type in PARAM_FIELD and which != PARAM_FIELD[p.type]:
            errors.append(f"{who}: template parameter {p.name} of type {p.type} in {which}")
    if births:
        if len(names) > MAX_TYPE_PARAMS:
            errors.append(f"{who}: {len(names)} template parameters in a birth; the gateway "
                          f"has a maximum of {MAX_TYPE_PARAMS}")
        for p in t.parameters:
            which = p.WhichOneof("value")
            if p.type not in GW_PARAM_TYPES:
                errors.append(f"{who}: template parameter {p.name} of type {p.type}; the gateway "
                              f"gives only {sorted(GW_PARAM_TYPES)}")
            if which is None:
                errors.append(f"{who}: template parameter {p.name} without value in a birth")
            if not 1 <= len(p.name.encode()) <= 39:
                errors.append(f"{who}: template parameter name {p.name!r} is not 1 to 39 bytes")
            if which == "string_value" and len(p.string_value.encode()) > 63:
                errors.append(f"{who}: template parameter {p.name} has a text of more than 63 "
                              "bytes")
    elif names:
        errors.append(f"{who}: template parameters {names} in {kind} (the births give them)")
    return errors


def check_template(kind, m):
    """SPB-18: definitions only in NBIRTH, without alias or template_ref; instances name their
    definition; members carry a name and no alias, and are not templates. Members carry their
    datatype in births and no datatype in DATA (tck-id-payloads-metric-datatype-not-req).
    #251: the parameters (check_parameters)."""
    if not is_template(m) or m.is_null:
        return []
    t, errors = m.template_value, check_parameters(kind, m)
    data = kind in ("NDATA", "DDATA")
    if t.is_definition:
        if kind != "NBIRTH":
            errors.append(f"{m.name}: template definition outside NBIRTH")
        if m.HasField("alias") or t.HasField("template_ref"):
            errors.append(f"{m.name}: a template definition has no alias and no template_ref")
    elif not t.template_ref:
        errors.append(f"alias {m.alias}: template instance without template_ref")
    for mm in t.metrics:
        if not mm.HasField("name") or mm.HasField("alias"):
            errors.append(f"{m.name or m.alias}: member without name or with an alias")
        if mm.HasField("datatype") == data:
            errors.append(f"{m.name or m.alias}: member {mm.name} "
                          + ("with a datatype in DATA" if data else "without datatype"))
        if mm.datatype == 19 or mm.WhichOneof("value") == "template_value":
            errors.append(f"{m.name or m.alias}: nested template member {mm.name}")
        expected = DATATYPE_FIELD.get(mm.datatype)
        if not mm.is_null and not t.is_definition and expected and mm.WhichOneof("value") != expected:
            errors.append(f"member {mm.name}: datatype {mm.datatype} in {mm.WhichOneof('value')}")
        errors += check_properties(mm)
    return errors


CELL_FIELD = {k: v for k, v in DATATYPE_FIELD.items() if k <= 14}   # SPB-19 DataSet columns
PARAM_FIELD = CELL_FIELD   # #251: a Template parameter has a basic datatype, as a DataSet cell
# #276: the datatypes of the parameters that the gateway gives (gw_node.c param_out): Int64,
# Double, Boolean and String. Each examined decoder reads them (sparkplug-behavior.md)
GW_PARAM_TYPES = {4, 10, 11, 12}
MAX_TYPE_PARAMS = 8        # GW_MAX_TYPE_PARAMS


def check_dataset(m):
    """SPB-19: num_of_columns, names and types agree; every row has one element per column,
    in the field its column type asks for."""
    if (m.datatype != 16 and m.WhichOneof("value") != "dataset_value") or m.is_null:
        return []
    ds, errors = m.dataset_value, []
    if not (ds.num_of_columns == len(ds.columns) == len(ds.types)):
        errors.append(f"alias {m.alias}: DataSet num_of_columns {ds.num_of_columns}, "
                      f"{len(ds.columns)} names, {len(ds.types)} types")
    for r, row in enumerate(ds.rows):
        if len(row.elements) != len(ds.types):
            errors.append(f"alias {m.alias}: DataSet row {r} has {len(row.elements)} elements")
            continue
        for c, (e, t) in enumerate(zip(row.elements, ds.types, strict=True)):
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
    return dict(zip(m.properties.keys, m.properties.values, strict=False))


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
        out = {head: {mm.name: value_of(mm) for mm in t.metrics}}
        if t.parameters:   # #251
            out["parameters"] = {q.name: param_value(q) for q in t.parameters}
        return out
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
        # tck-id-payloads-metric-datatype-not-req: no datatype in NDATA and DDATA (#159); the
        # host knows it from the birth (BirthTypes checks the value field against it)
        if kind in ("NDATA", "DDATA") and m.HasField("datatype"):
            errors.append(f"{kind} metric with a datatype: alias {m.alias}, datatype {m.datatype}")
        # tck-id-payloads-name-birth-data-requirement: a timestamp on every metric of NBIRTH,
        # DBIRTH, NDATA and DDATA (and on the members of a template instance, which are values;
        # the members of a definition are not)
        if kind in TIMESTAMPED:
            members = [] if definition or not is_template(m) else list(m.template_value.metrics)
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


def topic_parts(topic):
    """(kind, node, device) of a Sparkplug topic spBv1.0/<group>/<TYPE>/<edge>[/<device>]."""
    parts = topic.split("/")
    if len(parts) < 4:
        return (parts[2] if len(parts) > 2 else None), None, None
    return parts[2], (parts[1], parts[3]), parts[4] if len(parts) > 4 else None


def field_error(what, dt, m):
    """The value of @p m is not in the field of datatype @p dt (None when it is)."""
    expected = DATATYPE_FIELD.get(dt)
    if m.is_null or expected is None or m.WhichOneof("value") == expected:
        return None
    return f"{what}: the birth datatype {dt} is in {expected}, the value is in {m.WhichOneof('value')}"


class BirthTypes:
    """The datatype of each alias, from the births of each node (#159). NDATA and DDATA carry
    no datatype (tck-id-payloads-metric-datatype-not-req), so the value of a DATA metric must be
    in the field of the datatype that its birth gave (and each member of a template instance in
    the field of the datatype of that member in the DBIRTH).

    Feed the messages in publish order. With @p strict, a DATA message or alias that no birth
    gave is an error (a complete dump); without it (a capture that can start late), it is not
    examined."""

    def __init__(self, strict=True):
        self.strict = strict
        self.nodes = {}   # node -> {device or None: {alias: (datatype, {member: datatype})}}
        self.defs = {}    # node -> {definition name: {parameter name: type}} (#251)

    def parameter_errors(self, kind, node, where, p):
        """tck-id-payloads-template-definition-parameters (#251): each parameter of a template
        instance is in its definition of the NBIRTH, with the same type.
        #276: the instance of a DBIRTH gives each parameter of its definition (the choice of the
        gateway for tck-id-payloads-template-instance-parameters, a MAY)."""
        defs = self.defs.get(node)
        if defs is None:
            return []
        errors = []
        for m in p.metrics:
            if m.WhichOneof("value") != "template_value" or m.template_value.is_definition:
                continue
            t = m.template_value
            known = defs.get(t.template_ref)
            if known is None:
                if t.parameters:
                    errors.append(f"{kind} {where} alias {m.alias}: parameters, and the NBIRTH "
                                  f"has no definition {t.template_ref!r}")
                continue
            for q in t.parameters:
                if q.name not in known:
                    errors.append(f"{kind} {where} alias {m.alias}: parameter {q.name} is not in "
                                  f"the definition {t.template_ref}")
                elif q.HasField("type") and q.type != known[q.name]:
                    errors.append(f"{kind} {where} alias {m.alias}: parameter {q.name} has the "
                                  f"type {q.type}, the definition gives {known[q.name]}")
            if kind == "DBIRTH":
                absent = [n for n in known if n not in {q.name for q in t.parameters}]
                if absent:
                    errors.append(f"{kind} {where} alias {m.alias}: the instance does not give "
                                  f"the parameters {absent} of the definition {t.template_ref}")
        return errors

    @staticmethod
    def types_of(p):
        out = {}
        for m in p.metrics:
            if m.HasField("alias"):
                members = {mm.name: mm.datatype for mm in m.template_value.metrics} \
                    if m.datatype == 19 else {}
                out[m.alias] = (m.datatype, members)
        return out

    def datatype(self, node, device, alias):
        """The birth datatype of @p alias of @p device (None: a node metric), or None."""
        t = self.nodes.get(node, {}).get(device, {}).get(alias)
        return t[0] if t else None

    def feed(self, kind, node, device, p):
        if node is None:
            return []
        if kind == "NDEATH":
            self.nodes.pop(node, None)
            self.defs.pop(node, None)
            return []
        where = "/".join(node) + (f"/{device}" if device else "")
        if kind == "NBIRTH":
            self.nodes[node] = {None: self.types_of(p)}
            self.defs[node] = {
                m.name: {q.name: q.type for q in m.template_value.parameters}
                for m in p.metrics
                if m.WhichOneof("value") == "template_value" and m.template_value.is_definition}
            return self.parameter_errors(kind, node, where, p)
        if kind == "DBIRTH":
            self.nodes.setdefault(node, {})[device] = self.types_of(p)
            return self.parameter_errors(kind, node, where, p)
        if kind not in ("NDATA", "DDATA"):
            return []
        known = self.nodes.get(node, {}).get(device if kind == "DDATA" else None)
        if known is None:
            return [f"{kind} {where}: no birth gave its datatypes"] if self.strict else []
        errors = self.parameter_errors(kind, node, where, p)
        for m in p.metrics:
            t = known.get(m.alias) if m.HasField("alias") else None
            if t is None:
                if self.strict:
                    errors.append(f"{kind} {where}: alias {m.alias} is not in the birth")
                continue
            dt, members = t
            e = field_error(f"{kind} {where} alias {m.alias}", dt, m)
            errors += [e] if e else []
            for mm in m.template_value.metrics if dt == 19 else []:
                if mm.name not in members:
                    errors.append(f"{kind} {where} alias {m.alias}: member {mm.name} is not in "
                                  "the DBIRTH")
                    continue
                e = field_error(f"{kind} {where} alias {m.alias} member {mm.name}",
                                members[mm.name], mm)
                errors += [e] if e else []
        return errors


class Sessions:
    """Rules across the messages of one Edge Node (#166). Feed every message of the node in
    publish order: a new NBIRTH starts a new session, an NDEATH ends it.

    - tck-id-payloads-sequence-num-incrementing: each message after the NBIRTH has the seq of
      the message before it plus one, 255 followed by 0.
    - tck-id-payloads-alias-uniqueness: an alias of the NBIRTH or of a DBIRTH is unique across
      all the metrics of the node: the NBIRTH and the DBIRTHs of all its devices. A new DBIRTH
      of the same device (after a DDEATH) replaces the aliases of that device.
    - #159: the value of each DATA metric is in the field of its birth datatype (BirthTypes).
    """

    def __init__(self):
        self.nodes = {}
        self.types = BirthTypes(strict=True)

    def feed(self, kind, node, device, p):
        """Errors of message @p p (type @p kind) from @p node ((group, edge)) and @p device
        (None for a node message). A message without a node gives no error here."""
        if node is None or kind in ("NCMD", "DCMD", "STATE"):
            return []
        where = "/".join(node) + (f"/{device}" if device else "")
        if kind == "NDEATH":
            self.nodes.pop(node, None)
            self.types.feed(kind, node, device, p)
            return []
        if kind == "NBIRTH":
            self.nodes[node] = {"next": 0, "node": set(), "devices": {}}
        s = self.nodes.get(node)
        if s is None:
            return [f"{kind} {where} before the NBIRTH of its node"]
        errors = self.types.feed(kind, node, device, p)
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
