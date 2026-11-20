#!/usr/bin/env python3
"""Self-test of spb_decode.py --check: each structural rule rejects a payload that breaks it.

    python3 tools/test_spb_decode.py

Requires: pip install protobuf grpcio-tools (as spb_decode.py).
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import spb_decode  # noqa: E402

spb = spb_decode.load_module()

TS = 1762171200000
NODE_NAMES = ("Node Control/Reboot", "Node Info/Firmware", "Node Info/Hardware",
              "Node Info/Reset Reason", "Node Control/Scan Rate",
              *(f"DI/{i}" for i in range(1, 9)), *(f"DO/{i}" for i in range(1, 9)))


def add(p, name=None, alias=None, datatype=11, ts=TS, **value):
    m = p.metrics.add()
    if name is not None:
        m.name = name
    if alias is not None:
        m.alias = alias
    if datatype is not None:
        m.datatype = datatype
    if ts is not None:
        m.timestamp = ts
    for field, v in (value or {"boolean_value": False}).items():
        setattr(m, field, v)
    return m


def nbirth(seq=0, rebirth_alias=None):
    """A minimal NBIRTH that passes every check."""
    p = spb.Payload(timestamp=TS, seq=seq)
    add(p, "bdSeq", 1, 4, long_value=0)
    add(p, spb_decode.REBIRTH, rebirth_alias)
    for i, name in enumerate(NODE_NAMES):
        add(p, name, 10 + i)
    return p


def errors(kind, p):
    return spb_decode.check(kind, p)


def unit_param(t, value):
    """#251: the parameter "Unit ID" (Int32) of the gateway on the Template @p t."""
    return t.parameters.add(name=spb_decode.UNIT_PARAM, type=3, int_value=value)


class Rebirth(unittest.TestCase):
    """tck-id-operational-behavior-data-commands-rebirth-name-aliases (#153)."""

    def test_nbirth_without_rebirth_alias_passes(self):
        self.assertEqual(errors("NBIRTH", nbirth()), [])

    def test_nbirth_with_rebirth_alias_fails(self):
        e = errors("NBIRTH", nbirth(rebirth_alias=2))
        self.assertTrue(any("must not have an alias" in x for x in e), e)


def dbirth(seq, *aliases):
    p = spb.Payload(timestamp=TS, seq=seq)
    for a in aliases:
        add(p, f"Tag{a}", a, 9, float_value=1.0)
    return p


def data(seq, *aliases, ts=TS, datatype=None, field="float_value", value=1.0):
    """DATA metrics by alias, without datatype (#159) unless @p datatype."""
    p = spb.Payload(timestamp=TS, seq=seq)
    for a in aliases:
        add(p, alias=a, datatype=datatype, ts=ts, **{field: value})
    return p


def ndata(seq):
    """An NDATA with the alias 10 of nbirth() (Node Control/Reboot, a Boolean)."""
    return data(seq, 10, field="boolean_value", value=True)


class Timestamps(unittest.TestCase):
    """tck-id-payloads-name-birth-data-requirement (#166)."""

    def test_metrics_with_timestamps_pass(self):
        self.assertEqual(errors("DBIRTH", dbirth(1, 100)), [])
        self.assertEqual(errors("DDATA", data(2, 100)), [])

    def test_data_metric_without_timestamp_fails(self):
        for kind in ("NDATA", "DDATA"):
            e = errors(kind, data(2, 100, ts=None))
            self.assertTrue(any("without timestamp" in x for x in e), (kind, e))

    def test_birth_metric_without_timestamp_fails(self):
        p = nbirth()
        p.metrics[3].ClearField("timestamp")
        e = errors("NBIRTH", p)
        self.assertTrue(any("without timestamp" in x for x in e), e)

    def test_template_instance_member_without_timestamp_fails(self):
        p = spb.Payload(timestamp=TS, seq=1)
        inst = p.metrics.add(name="Pump", alias=100, datatype=19, timestamp=TS)
        inst.template_value.template_ref = "Pump"
        unit_param(inst.template_value, 1)
        mm = inst.template_value.metrics.add(name="Speed", datatype=6, int_value=5)
        self.assertTrue(any("without timestamp: Speed" in x for x in errors("DBIRTH", p)))
        mm.timestamp = TS
        self.assertEqual(errors("DBIRTH", p), [])

    def test_commands_and_deaths_need_none(self):
        p = spb.Payload(timestamp=TS)
        add(p, "bdSeq", 1, 4, ts=None, long_value=3)
        self.assertEqual(errors("NDEATH", p), [])
        self.assertEqual(errors("DCMD", data(0, 100, ts=None)), [])


NODE = ("G", "N")


def feed_all(messages):
    s, out = spb_decode.Sessions(), []
    for kind, device, p in messages:
        out += s.feed(kind, NODE, device, p)
    return out


class Sequence(unittest.TestCase):
    """tck-id-payloads-sequence-num-incrementing (#166)."""

    def test_continuous_seq_passes(self):
        self.assertEqual(feed_all([("NBIRTH", None, nbirth()), ("DBIRTH", "M", dbirth(1, 100)),
                                   ("DDATA", "M", data(2, 100)), ("NDATA", None, ndata(3))]),
                         [])

    def test_gap_fails(self):
        e = feed_all([("NBIRTH", None, nbirth()), ("DBIRTH", "M", dbirth(1, 100)),
                      ("DDATA", "M", data(3, 100))])
        self.assertEqual(len(e), 1, e)
        self.assertIn("seq 3, expected 2", e[0])

    def test_repeated_seq_fails(self):
        e = feed_all([("NBIRTH", None, nbirth()), ("NDATA", None, ndata(0))])
        self.assertTrue(any("seq 0, expected 1" in x for x in e), e)

    def test_wrap_and_rebirth_pass(self):
        msgs = [("NBIRTH", None, nbirth())] + \
               [("NDATA", None, ndata(i % 256)) for i in range(1, 300)] + \
               [("NBIRTH", None, nbirth()), ("NDATA", None, ndata(1))]
        self.assertEqual(feed_all(msgs), [])

    def test_message_before_nbirth_fails(self):
        e = feed_all([("DDATA", "M", data(5, 100))])
        self.assertTrue(any("before the NBIRTH" in x for x in e), e)

    def test_nodes_are_separate(self):
        s = spb_decode.Sessions()
        self.assertEqual(s.feed("NBIRTH", ("G", "A"), None, nbirth()), [])
        self.assertEqual(s.feed("NBIRTH", ("G", "B"), None, nbirth()), [])
        self.assertEqual(s.feed("NDATA", ("G", "A"), None, ndata(1)), [])
        self.assertEqual(s.feed("NDATA", ("G", "B"), None, ndata(1)), [])


class AliasesAcrossBirths(unittest.TestCase):
    """tck-id-payloads-alias-uniqueness across the NBIRTH and the DBIRTHs (#166)."""

    def test_unique_aliases_pass(self):
        self.assertEqual(feed_all([("NBIRTH", None, nbirth()), ("DBIRTH", "A", dbirth(1, 100)),
                                   ("DBIRTH", "B", dbirth(2, 101))]), [])

    def test_alias_of_the_nbirth_in_a_dbirth_fails(self):
        e = feed_all([("NBIRTH", None, nbirth()), ("DBIRTH", "A", dbirth(1, 10))])
        self.assertTrue(any("alias 10 is also in the NBIRTH" in x for x in e), e)

    def test_alias_of_another_device_fails(self):
        e = feed_all([("NBIRTH", None, nbirth()), ("DBIRTH", "A", dbirth(1, 100)),
                      ("DBIRTH", "B", dbirth(2, 100))])
        self.assertTrue(any("alias 100 is also in the DBIRTH of A" in x for x in e), e)

    def test_new_dbirth_of_the_same_device_passes(self):
        ddeath = spb.Payload(timestamp=TS, seq=2)
        self.assertEqual(feed_all([("NBIRTH", None, nbirth()), ("DBIRTH", "A", dbirth(1, 100)),
                                   ("DDEATH", "A", ddeath), ("DBIRTH", "A", dbirth(3, 100))]),
                         [])


def instance(kind_birth, seq, member_dt=6, field="int_value"):
    """A template instance Pump (alias 100) with one member Speed: with datatypes in a birth,
    without datatypes in DATA."""
    p = spb.Payload(timestamp=TS, seq=seq)
    inst = p.metrics.add(alias=100, timestamp=TS)
    if kind_birth:
        inst.name, inst.datatype = "Pump", 19
    inst.template_value.template_ref = "Pump"
    mm = inst.template_value.metrics.add(name="Speed", timestamp=TS)
    if kind_birth:
        mm.datatype = member_dt
        unit_param(inst.template_value, 2)   # #251: births only
    setattr(mm, field, 5)
    return p, mm


class DataDatatypes(unittest.TestCase):
    """tck-id-payloads-metric-datatype-not-req (#159): no datatype in NDATA and DDATA, also on
    template members; the value is in the field of the birth datatype."""

    def test_data_without_datatype_passes(self):
        self.assertEqual(errors("DDATA", data(2, 100)), [])
        self.assertEqual(errors("DDATA", instance(False, 2)[0]), [])

    def test_data_with_datatype_fails(self):
        for kind in ("NDATA", "DDATA"):
            e = errors(kind, data(2, 100, datatype=9))
            self.assertTrue(any("with a datatype" in x for x in e), (kind, e))

    def test_member_with_datatype_in_data_fails(self):
        p, mm = instance(False, 2)
        mm.datatype = 6
        self.assertTrue(any("member Speed with a datatype in DATA" in x
                            for x in errors("DDATA", p)))

    def test_births_need_the_datatype(self):
        p, mm = instance(True, 1)
        self.assertEqual(errors("DBIRTH", p), [])
        mm.ClearField("datatype")
        self.assertTrue(any("member Speed without datatype" in x for x in errors("DBIRTH", p)))
        p = dbirth(1, 100)
        p.metrics[0].ClearField("datatype")
        self.assertTrue(any("without name/alias/datatype" in x for x in errors("DBIRTH", p)))

    def test_value_in_the_field_of_the_birth_datatype(self):
        ok = feed_all([("NBIRTH", None, nbirth()), ("DBIRTH", "M", dbirth(1, 100)),
                       ("DDATA", "M", data(2, 100))])
        self.assertEqual(ok, [])
        e = feed_all([("NBIRTH", None, nbirth()), ("DBIRTH", "M", dbirth(1, 100)),
                      ("DDATA", "M", data(2, 100, field="double_value"))])
        self.assertTrue(any("birth datatype 9 is in float_value" in x for x in e), e)

    def test_member_value_in_the_field_of_the_birth_datatype(self):
        ok = feed_all([("NBIRTH", None, nbirth_pump()), ("DBIRTH", "P", instance(True, 1)[0]),
                       ("DDATA", "P", instance(False, 2)[0])])
        self.assertEqual(ok, [])
        e = feed_all([("NBIRTH", None, nbirth_pump()), ("DBIRTH", "P", instance(True, 1)[0]),
                      ("DDATA", "P", instance(False, 2, field="long_value")[0])])
        self.assertTrue(any("member Speed: the birth datatype 6" in x for x in e), e)

    def test_alias_not_in_a_birth(self):
        msgs = [("NBIRTH", None, nbirth()), ("DBIRTH", "M", dbirth(1, 100)),
                ("DDATA", "M", data(2, 101))]
        self.assertTrue(any("alias 101 is not in the birth" in x for x in feed_all(msgs)))
        lax = spb_decode.BirthTypes(strict=False)   # a capture that started late
        self.assertEqual(lax.feed("DDATA", NODE, "M", data(2, 100)), [])
        self.assertEqual(lax.feed("DBIRTH", NODE, "M", dbirth(3, 100)), [])
        self.assertEqual(lax.datatype(NODE, "M", 100), 9)
        self.assertEqual(lax.feed("DDATA", NODE, "M", data(4, 101)), [])

    def test_topic_parts(self):
        self.assertEqual(spb_decode.topic_parts("spBv1.0/G/DDATA/N/M"), ("DDATA", ("G", "N"), "M"))
        self.assertEqual(spb_decode.topic_parts("spBv1.0/G/NDATA/N"), ("NDATA", ("G", "N"), None))


def definition(p, default=0):
    """Add the template definition Pump (member Speed, the parameter of the gateway) to the
    NBIRTH @p p. Returns its Template."""
    d = p.metrics.add(name="Pump", datatype=19, timestamp=TS)
    d.template_value.is_definition = True
    d.template_value.metrics.add(name="Speed", datatype=6, int_value=0)
    unit_param(d.template_value, default)
    return d.template_value


def nbirth_pump():
    """An NBIRTH with the definition Pump."""
    p = nbirth()
    definition(p)
    return p


class TemplateParameters(unittest.TestCase):
    """#251: the parameters of a template (tck-id-payloads-template-definition-parameters,
    tck-id-payloads-template-instance-parameters, tck-id-payloads-template-parameter-*)."""

    def birth(self):
        p = nbirth()
        return p, definition(p)

    def test_definition_and_instance_pass(self):
        p, _ = self.birth()
        self.assertEqual(errors("NBIRTH", p), [])
        inst = instance(True, 1)[0]
        self.assertEqual(errors("DBIRTH", inst), [])
        self.assertEqual(feed_all([("NBIRTH", None, p), ("DBIRTH", "P", inst),
                                   ("DDATA", "P", instance(False, 2)[0])]), [])

    def test_birth_template_without_the_parameter_fails(self):
        p, t = self.birth()
        del t.parameters[:]
        self.assertTrue(any("must be ['Unit ID'], not []" in x for x in errors("NBIRTH", p)))
        inst = instance(True, 1)[0]
        del inst.metrics[0].template_value.parameters[:]
        self.assertTrue(any("must be ['Unit ID'], not []" in x for x in errors("DBIRTH", inst)))

    def test_parameter_without_name_or_type_fails(self):
        p, t = self.birth()
        t.parameters[0].ClearField("name")
        self.assertTrue(any("template parameter without name" in x for x in errors("NBIRTH", p)))
        p, t = self.birth()
        t.parameters[0].ClearField("type")
        self.assertTrue(any("without type in a birth" in x for x in errors("NBIRTH", p)))

    def test_type_must_be_basic_and_agree_with_the_value_field(self):
        for bad in (0, 15, 16, 19, 22):
            p, t = self.birth()
            t.parameters[0].type = bad
            self.assertTrue(any("not a basic datatype" in x for x in errors("NBIRTH", p)), bad)
        p, t = self.birth()
        t.parameters[0].long_value = 0          # Int32 is a uint32: int_value
        e = errors("NBIRTH", p)
        self.assertTrue(any("of type 3 in long_value" in x for x in e), e)

    def test_values_of_the_gateway_parameter(self):
        p, t = self.birth()
        t.parameters[0].int_value = 1           # the default of a definition is 0
        self.assertTrue(any("0 in a definition" in x for x in errors("NBIRTH", p)))
        for bad in (0, 248):
            inst = instance(True, 1)[0]
            inst.metrics[0].template_value.parameters[0].int_value = bad
            self.assertTrue(any("1 to 247 in an instance" in x for x in errors("DBIRTH", inst)),
                            bad)
        inst = instance(True, 1)[0]
        inst.metrics[0].template_value.parameters[0].type = 7
        self.assertTrue(any("must be Int32 (3)" in x for x in errors("DBIRTH", inst)))

    def test_duplicated_parameter_fails(self):
        p, t = self.birth()
        unit_param(t, 0)
        self.assertTrue(any("duplicated template parameters" in x for x in errors("NBIRTH", p)))

    def test_parameters_in_data_fail(self):
        d = instance(False, 2)[0]
        unit_param(d.metrics[0].template_value, 2)
        self.assertTrue(any("template parameters ['Unit ID'] in DDATA" in x
                            for x in errors("DDATA", d)))

    def test_instance_parameter_must_be_in_the_definition(self):
        # tck-id-payloads-template-definition-parameters
        p, t = self.birth()
        inst = instance(True, 1)[0]
        self.assertEqual(feed_all([("NBIRTH", None, p), ("DBIRTH", "P", inst)]), [])
        t.parameters[0].name = "Other"
        e = feed_all([("NBIRTH", None, p), ("DBIRTH", "P", inst)])
        self.assertTrue(any("parameter Unit ID is not in the definition Pump" in x for x in e), e)
        p, t = self.birth()
        t.parameters[0].type = 7
        e = feed_all([("NBIRTH", None, p), ("DBIRTH", "P", inst)])
        self.assertTrue(any("has the type 3, the definition gives 7" in x for x in e), e)
        e = feed_all([("NBIRTH", None, nbirth()), ("DBIRTH", "P", inst)])
        self.assertTrue(any("the NBIRTH has no definition 'Pump'" in x for x in e), e)

    def test_describe_shows_the_parameters(self):
        inst = instance(True, 1)[0]
        self.assertIn("'parameters': {'Unit ID': 2}", spb_decode.describe("DBIRTH", inst))


class FileNames(unittest.TestCase):
    def test_spb_dump_names(self):
        P = pathlib.Path
        self.assertEqual(spb_decode.parse_name(P("3_Plant1+DBIRTH+GW01+Meter1.bin")),
                         ("DBIRTH", ("Plant1", "GW01"), "Meter1"))
        self.assertEqual(spb_decode.parse_name(P("1_Plant1+NBIRTH+GW01.bin")),
                         ("NBIRTH", ("Plant1", "GW01"), None))
        self.assertEqual(spb_decode.parse_name(P("000_NDEATH.bin")), ("NDEATH", None, None))
        names = ["10_a.bin", "9_b.bin", "x.bin", "0_c.bin"]
        self.assertEqual([p.name for p in sorted(map(P, names), key=spb_decode.publish_order)],
                         ["0_c.bin", "9_b.bin", "10_a.bin", "x.bin"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
