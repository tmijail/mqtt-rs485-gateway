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


def data(seq, *aliases, ts=TS):
    p = spb.Payload(timestamp=TS, seq=seq)
    for a in aliases:
        add(p, alias=a, datatype=9, ts=ts, float_value=1.0)
    return p


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
                                   ("DDATA", "M", data(2, 100)), ("NDATA", None, data(3, 10))]),
                         [])

    def test_gap_fails(self):
        e = feed_all([("NBIRTH", None, nbirth()), ("DBIRTH", "M", dbirth(1, 100)),
                      ("DDATA", "M", data(3, 100))])
        self.assertEqual(len(e), 1, e)
        self.assertIn("seq 3, expected 2", e[0])

    def test_repeated_seq_fails(self):
        e = feed_all([("NBIRTH", None, nbirth()), ("NDATA", None, data(0, 10))])
        self.assertTrue(any("seq 0, expected 1" in x for x in e), e)

    def test_wrap_and_rebirth_pass(self):
        msgs = [("NBIRTH", None, nbirth())] + \
               [("NDATA", None, data(i % 256, 10)) for i in range(1, 300)] + \
               [("NBIRTH", None, nbirth()), ("NDATA", None, data(1, 10))]
        self.assertEqual(feed_all(msgs), [])

    def test_message_before_nbirth_fails(self):
        e = feed_all([("DDATA", "M", data(5, 100))])
        self.assertTrue(any("before the NBIRTH" in x for x in e), e)

    def test_nodes_are_separate(self):
        s = spb_decode.Sessions()
        self.assertEqual(s.feed("NBIRTH", ("G", "A"), None, nbirth()), [])
        self.assertEqual(s.feed("NBIRTH", ("G", "B"), None, nbirth()), [])
        self.assertEqual(s.feed("NDATA", ("G", "A"), None, data(1, 10)), [])
        self.assertEqual(s.feed("NDATA", ("G", "B"), None, data(1, 10)), [])


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
