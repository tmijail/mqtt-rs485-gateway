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


if __name__ == "__main__":
    unittest.main(verbosity=2)
