#!/usr/bin/env python3
"""Self-test of the E2E harness' fault handling (issues #92 and #96), without QEMU: the known
emulator-fault signatures and the retry rule of run_scenario. Runs in the CI host-tests job:

    python idf/tools/e2e/test_harness.py   (needs tools/e2e/requirements.txt)
"""
import argparse
import pathlib
import sys
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import e2e  # noqa: E402

# the panic of issue #92 (second comment): the encrypted image, core 1, verifying a download
FLAKE = """I (20817) esp_https_ota: Starting OTA...
I (20823) esp_https_ota: Writing to <ota_1> partition at offset 0x420000
W (20836) fw: downloading firmware e2e-enc (1052672 bytes) from https://10.0.2.2:18854/enc.bin
W (23150) buzzer: alarm 'device offline': 1 beep(s) every 10 s
I (25626) esp_image: segment 0: paddr=00420020 vaddr=3c0b0020 size=39c80h (236672) map
Guru Meditation Error: Core  1 panic'ed (LoadStorePIFAddrError). Exception was unhandled.

Core  1 register dump:
PC      : 0x4037c267  PS      : 0x00050031  A0      : 0x40377ad2  A1      : 0x3fc935f0
A2      : 0x3fc952fc  A3      : 0x00000001  A4      : 0x3fc9530c  A5      : 0x00000034
A6      : 0x00000002  A7      : 0x00000004  A8      : 0x00000002  A9      : 0x60023000
A10     : 0x3fc952fc  A11     : 0x00000001  A12     : 0x00000000  A13     : 0x00000000
A14     : 0x3fcc52cc  A15     : 0x00000001  SAR     : 0x0000001f  EXCCAUSE: 0x0000000f
EXCVADDR: 0x6002306c  LBEG    : 0x00000000  LEND    : 0x00000000  LCOUNT  : 0x00000000
""".splitlines()


class FakeGateway(e2e.Gateway):
    """A Gateway fed with console lines, no emulator."""

    def __init__(self):
        super().__init__(argparse.Namespace(simulate_qemu_death=None, efuse="", flash=""),
                         pathlib.Path("uart0-fake.log"), flash="x")

    def feed(self, lines):
        for ln in lines:
            self._line(time.monotonic(), ln)


class Signatures(unittest.TestCase):
    def test_known_panic(self):
        g = FakeGateway()
        g.feed(FLAKE)
        self.assertIsNotNone(g.signature)
        self.assertIn("LoadStorePIFAddrError at 0x6002306c", g.signature[1])

    def test_other_panics_are_not_known(self):
        # the same exception outside an image write or check (only the 4 lines before it)
        g = FakeGateway()
        g.feed(["I (1) edge: Sparkplug session up"] + FLAKE[5:])
        self.assertIsNone(g.signature)
        # another exception during the image check
        g = FakeGateway()
        g.feed([ln.replace("LoadStorePIFAddrError", "LoadProhibited") for ln in FLAKE])
        self.assertIsNone(g.signature)
        # LoadStorePIFAddrError at an address that is not a peripheral
        g = FakeGateway()
        g.feed([ln.replace("0x6002306c", "0x3fc90000") for ln in FLAKE])
        self.assertIsNone(g.signature)
        # an abort() (the shell's 'panic', a firmware assert)
        g = FakeGateway()
        g.feed(FLAKE[:5] + ["abort() was called at PC 0x42001234 on core 0"])
        self.assertIsNone(g.signature)

    def test_phases(self):
        g = FakeGateway()
        self.assertEqual("boot", g.phase)
        g.feed(["I (300) main: rs485_spb_gateway e2e-v2 (IDF v6.1), reset reason: software"])
        self.assertEqual("run", g.phase)
        g.feed(["ESP-ROM:esp32s3-20210327"])
        self.assertEqual("boot", g.phase)
        self.assertTrue(e2e.TRUSTED_RX.search(
            "I (37407) secure_boot_v2: Take trusted digest key(s) from running app"))


class RetryRule(unittest.TestCase):
    """run_scenario: repeated once only for a known fault with no failure before it."""

    def setUp(self):
        e2e.ARGS = argparse.Namespace(scenario_timeout=0, timeout_scale=1)
        e2e.PROCS.clear()
        for lst in (e2e.results, e2e.RETRIES, e2e.FAULTS, e2e.GATEWAYS, e2e.RESOURCES):
            lst.clear()
        self.attempts = 0

    def scenario(self, *steps):
        """A scenario whose first attempt runs @steps ('pass', 'fail', 'known', 'unknown');
        the second one passes."""
        def fn():
            self.attempts += 1
            if self.attempts > 1:
                e2e.check("second attempt", True)
                return
            for s in steps:
                if s in ("pass", "fail"):
                    e2e.check(f"check {s}", s == "pass")
                else:
                    time.sleep(0.05)
                    e2e.record_fault(None, "qemu exit", time.monotonic(),
                                     signature="QEMU ended (simulated)" if s == "known" else None,
                                     detail="simulated")
                    time.sleep(0.05)
        return fn

    def names(self):
        return [(r["criterion"], r["ok"]) for r in e2e.results]

    def test_known_fault_is_repeated(self):
        e2e.run_scenario("s", self.scenario("pass", "known", "fail"))
        self.assertEqual(2, self.attempts)
        self.assertEqual([("second attempt", True)], self.names())
        self.assertEqual(1, len(e2e.RETRIES))
        # the failure after the fault is discarded but kept
        lost = [c for c in e2e.RETRIES[0]["discarded_checks"] if not c["ok"]]
        self.assertEqual(["check fail"], [c["criterion"] for c in lost])

    def test_failure_before_the_fault_is_not_repeated(self):
        e2e.run_scenario("s", self.scenario("fail", "known"))
        self.assertEqual(1, self.attempts)
        self.assertEqual([], e2e.RETRIES)
        names = self.names()
        self.assertEqual(("check fail", False), names[0])
        self.assertEqual(("s scenario completed", False), names[1])
        self.assertIn("had failed before the emulator fault", e2e.results[1]["detail"])

    def test_unknown_fault_is_not_repeated(self):
        e2e.run_scenario("s", self.scenario("pass", "unknown"))
        self.assertEqual(1, self.attempts)
        self.assertEqual([], e2e.RETRIES)
        self.assertEqual([("check pass", True), ("s scenario completed", False)], self.names())
        self.assertIn("not a known emulator fault", e2e.results[1]["detail"])

    def test_second_fault_is_not_repeated(self):
        def fn():
            self.attempts += 1
            e2e.record_fault(None, "qemu exit", time.monotonic(), signature="again", detail="x")
        e2e.run_scenario("s", fn)
        self.assertEqual(2, self.attempts)
        self.assertEqual(1, len(e2e.RETRIES))
        self.assertEqual([("s scenario completed", False)], self.names())
        self.assertIn("second attempt", e2e.results[0]["detail"])

    def test_resources_of_an_attempt_are_closed(self):
        """Issue #105: what an exception left open is closed after the attempt, once."""
        closed = []

        class Res:
            def __init__(self, name):
                self.name, self.closed = name, False
                e2e.track(self, self.close)

            def close(self):
                closed.append(self.name)
                self.closed = True

        def fn():
            self.attempts += 1
            Res(f"left {self.attempts}")
            done = Res(f"closed {self.attempts}")
            done.close()
            if self.attempts == 1:
                e2e.record_fault(None, "qemu exit", time.monotonic(), signature="x", detail="x")
                raise ConnectionError("simulated")
        outside = Res("outside")
        e2e.run_scenario("s", fn)
        self.assertEqual(2, self.attempts)
        self.assertEqual(["closed 1", "left 1", "closed 2", "left 2"], closed)
        self.assertFalse(outside.closed)   # created outside any scenario: not the attempt's
        self.assertEqual(0, e2e.ATTEMPTS[0])

    def test_waits_return_at_once_after_a_fault(self):
        e2e.FAULT.set()
        t = time.monotonic()
        g = FakeGateway()
        self.assertIsNone(g.wait_log(r"never", 30))
        self.assertLess(time.monotonic() - t, 1)
        e2e.FAULT.clear()


if __name__ == "__main__":
    unittest.main()
