#!/usr/bin/env python3
"""Self-test of the E2E harness' fault handling (issues #92, #96 and #105), without QEMU: the
known emulator-fault signatures, the hang decision, the retry rule of run_scenario, the fault
monitor's hand-over between attempts and the clean-up of what an attempt leaves open. Runs in
the CI host-tests job:

    python idf/tools/e2e/test_harness.py   (needs tools/e2e/requirements.txt)
"""
import argparse
import pathlib
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import e2e  # noqa: E402

APP = "I (300) main: rs485_spb_gateway e2e-v2 (IDF v6.1), reset reason: software"
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
PANIC = FLAKE[5:]
# the tick ISR (SysTickIsrHandler) holds 0x4037c267 in these tests; park_loop and the idle wait
SYMS = {"SysTickIsrHandler": [(0x4037c200, 0x4037c2c0)], "park_loop": [(0x40380000, 0x40380040)],
        "esp_cpu_wait_for_intr": [(0x40379c90, 0x40379cc0)]}
BOOTLOADER = ["ESP-ROM:esp32s3-20210327", "rst:0xc (RTC_SW_CPU_RST),boot:0x4 (SPI_FLASH_BOOT)",
              "I (100) esp_image: segment 0: paddr=00020020 vaddr=3c0b0020 size=39fe0h map",
              "I (110) esp_image: segment 1: paddr=0005a008 vaddr=3fc91300 size=03a1ch",
              "I (130) boot: Loaded app from partition at offset 0x20000"]


class FakeProc:
    """An emulator process that stays alive and ignores signals."""
    pid, returncode = 1, None

    def poll(self):
        return None

    def send_signal(self, sig):
        pass

    def wait(self):
        return 0


class FakeGateway(e2e.Gateway):
    """A Gateway fed with console lines, no emulator."""

    def __init__(self):
        super().__init__(argparse.Namespace(simulate_qemu_death=None, efuse="", flash=""),
                         pathlib.Path("uart0-fake.log"), flash="x")

    def feed(self, lines):
        for ln in lines:
            self._line(time.monotonic(), ln)


class Signatures(unittest.TestCase):
    def setUp(self):
        e2e.SYMBOLS.clear()
        e2e.SYMBOLS.update(SYMS)

    def test_known_panic(self):
        g = FakeGateway()
        g.feed([APP] + FLAKE)
        self.assertIsNotNone(g.signature)
        self.assertIn("LoadStorePIFAddrError at 0x6002306c", g.signature[1])
        self.assertEqual([], g.unknown_panics())

    def test_known_panic_at_a_trial_start(self):
        # issue #92 round 2 (A1): the same fault while gw_fw_init reads the other slot, after
        # the configuration is loaded and before gw_fw_init's own line: the tick ISR's PC
        g = FakeGateway()
        g.feed(BOOTLOADER + [APP, "I (18925) main: configuration from /cfg/config.json "
                             "(LittleFS): group 'Plant1'", "I (18929) syslog: forwarding logs "
                             "to 10.0.2.2:15514"] + PANIC)
        self.assertIsNotNone(g.signature)
        self.assertIn("in the tick ISR (PC 0x4037c267) while gw_fw_init", g.signature[1])

    def test_iram_panic_in_steady_state_is_not_known(self):
        # A1: the same PC and address once gw_fw_init is done (only syslog context): the
        # firmware's (an ISR in IRAM writing a peripheral at any moment is not the emulator's)
        g = FakeGateway()
        g.feed([APP, "I (18925) main: configuration from /cfg/config.json (LittleFS)",
                "I (18930) fw: firmware e2e-v2 in ota_1; updates by URL trust /cfg/ca.pem",
                "I (18929) syslog: forwarding logs to 10.0.2.2:15514"] + PANIC)
        self.assertIsNone(g.signature)
        self.assertEqual(1, len(g.unknown_panics()))
        # in the window but another IRAM function (mask_port, the RS485 ISR): not known either
        g = FakeGateway()
        g.feed([APP, "I (18925) main: configuration from /cfg/config.json (LittleFS)"]
               + [ln.replace("0x4037c267", "0x40378888") for ln in PANIC])
        self.assertIsNone(g.signature)
        # without the ELF's symbols nothing in IRAM is known
        e2e.SYMBOLS.clear()
        g = FakeGateway()
        g.feed([APP, "I (18925) main: configuration from /cfg/config.json (LittleFS)"] + PANIC)
        self.assertIsNone(g.signature)

    def test_bootloader_lines_are_not_an_image_check(self):
        # M1: the bootloader's esp_image lines (every boot) do not count as an image check of
        # the app: a PIF fault in the first lines after app_main is not known
        g = FakeGateway()
        g.feed(BOOTLOADER + [APP, "I (320) storage: NVS ok"]
               + [ln.replace("0x4037c267", "0x42001234") for ln in PANIC])
        self.assertIsNone(g.signature)
        # and before app_main's line there is no context at all
        g = FakeGateway()
        g.feed(BOOTLOADER + FLAKE)
        self.assertIsNone(g.signature)

    def test_other_panics_are_not_known(self):
        flash_pc = [ln.replace("0x4037c267", "0x4209e706") for ln in PANIC]
        g = FakeGateway()
        g.feed([APP, "I (1) edge: Sparkplug session up"] + flash_pc)
        self.assertIsNone(g.signature)
        g = FakeGateway()
        g.feed([APP] + [ln.replace("LoadStorePIFAddrError", "LoadProhibited") for ln in FLAKE])
        self.assertIsNone(g.signature)
        g = FakeGateway()
        g.feed([APP] + [ln.replace("0x6002306c", "0x3fc90000") for ln in FLAKE])
        self.assertIsNone(g.signature)
        g = FakeGateway()
        g.feed([APP] + FLAKE[:5] + ["abort() was called at PC 0x42001234 on core 0"])
        self.assertIsNone(g.signature)
        self.assertEqual(1, len(g.unknown_panics()))   # an abort() nobody asked for

    def test_other_firmware_failures(self):
        # a stack overflow and an abort() not requested by the shell are firmware failures; the
        # shell's 'panic' (it prints its marker first) is not
        g = FakeGateway()
        g.feed([APP, "panic requested from the console",
                "abort() was called at PC 0x42001234 on core 0"])
        self.assertEqual([], g.unknown_panics())
        g.feed(["***ERROR*** A stack overflow in task esp_timer has been detected.",
                "abort() was called at PC 0x4037a1b2 on core 0"])
        self.assertEqual(["stack overflow in task esp_timer", "abort() not requested by the harness"],
                         [p[1] for p in g.unknown_panics()])

    def test_phases(self):
        g = FakeGateway()
        self.assertEqual("boot", g.phase)
        g.feed([APP])
        self.assertEqual("run", g.phase)
        g.feed(["ESP-ROM:esp32s3-20210327"])
        self.assertEqual("boot", g.phase)
        self.assertTrue(e2e.TRUSTED_RX.search(
            "I (37407) secure_boot_v2: Take trusted digest key(s) from running app"))


def regs(*pcs):
    return "\n".join(f"CPU#{i}\nPC={pc:08x}\n  A00=00000000" for i, pc in enumerate(pcs))


class HangDecision(unittest.TestCase):
    """M2: the hang after 'Take trusted digest key(s)' is retried only on a dump that shows it."""

    def setUp(self):
        e2e.SYMBOLS.clear()
        e2e.SYMBOLS.update(SYMS)

    def test_stalled_core(self):
        ok, why = e2e.hang_decision([regs(0x40012345, 0x40380010), regs(0x40012345, 0x40380010)])
        self.assertTrue(ok)
        self.assertIn("core 0 stalled in the ROM at 0x40012345", why)

    def test_stall_outside_the_rom_is_not_retried(self):
        # the one dumped occurrence: core 0 still in esp_cpu_compare_and_set (IRAM), core 1 parked
        ok, why = e2e.hang_decision([regs(0x40379b8f, 0x40380010), regs(0x40379b8f, 0x40380010)])
        self.assertFalse(ok)
        self.assertIn("outside the ROM", why)

    def test_no_evidence(self):
        self.assertFalse(e2e.hang_decision([None, None])[0])
        self.assertFalse(e2e.hang_decision([regs(0x40012345), None])[0])
        # every core moves: the firmware runs
        ok, why = e2e.hang_decision([regs(0x42001000, 0x40380010), regs(0x42001100, 0x40380020)])
        self.assertFalse(ok)
        self.assertIn("still running", why)
        # only the parked core and the idle core stand still
        ok, why = e2e.hang_decision([regs(0x40379ca0, 0x40380010), regs(0x40379ca0, 0x40380010)])
        self.assertFalse(ok)
        self.assertIn("wait loop", why)


class RetryRule(unittest.TestCase):
    """run_scenario: repeated once only for a known fault with no failure and no unknown panic."""

    def setUp(self):
        e2e.ARGS = argparse.Namespace(scenario_timeout=0, timeout_scale=1)
        e2e.PROCS.clear()
        e2e.MAIN_BROKER.clear()
        for lst in (e2e.results, e2e.RETRIES, e2e.FAULTS, e2e.GATEWAYS, e2e.RESOURCES,
                    e2e.MONITOR_ERRORS):
            lst.clear()
        self.attempts = 0

    def fault(self, known=True):
        e2e.record_fault(None, "qemu exit", time.monotonic(),
                         signature="QEMU ended (simulated)" if known else None, detail="simulated")

    def scenario(self, *steps):
        """First attempt: @steps ('pass', 'fail', 'known', 'unknown', 'wait'); then it passes."""
        def fn():
            self.attempts += 1
            if self.attempts > 1:
                e2e.check("second attempt", True)
                return
            for s in steps:
                if s in ("pass", "fail"):
                    e2e.check(f"check {s}", s == "pass")
                elif s == "wait":
                    FakeGateway().wait_log(r"never", 5)
                else:
                    time.sleep(0.05)
                    self.fault(s == "known")
                    time.sleep(0.05)
        return fn

    def names(self):
        return [(r["criterion"], r["ok"]) for r in e2e.results]

    def test_known_fault_is_repeated(self):
        e2e.run_scenario("s", self.scenario("pass", "known", "wait", "fail"))
        self.assertEqual(2, self.attempts)
        self.assertEqual([("second attempt", True)], self.names())
        self.assertEqual(1, len(e2e.RETRIES))
        # the wait after the fault stopped the attempt: the check behind it never ran
        self.assertEqual(["check pass"],
                         [c["criterion"] for c in e2e.RETRIES[0]["discarded_checks"]])

    def test_any_failed_check_forbids_the_retry(self):
        # M3: before the fault, or asserted after it without a wait in between
        for steps in (("fail", "known"), ("known", "fail")):
            self.setUp()
            e2e.run_scenario("s", self.scenario(*steps))
            self.assertEqual(1, self.attempts)
            self.assertEqual([], e2e.RETRIES)
            self.assertIn(("check fail", False), self.names())
            self.assertIn("check(s) of the attempt had failed", e2e.results[-1]["detail"])

    def test_unknown_panic_forbids_the_retry(self):
        e2e.SYMBOLS.clear()

        def fn():
            self.attempts += 1
            g = FakeGateway()
            e2e.GATEWAYS.append(g)
            g.feed([APP, "I (1) edge: Sparkplug session up"]
                   + [ln.replace("0x4037c267", "0x42001234") for ln in PANIC])
            self.fault()
        e2e.run_scenario("s", fn)
        self.assertEqual(1, self.attempts)
        self.assertIn("matched no known emulator fault", e2e.results[-1]["detail"])

    def test_unknown_fault_is_not_repeated(self):
        e2e.run_scenario("s", self.scenario("pass", "unknown"))
        self.assertEqual(1, self.attempts)
        self.assertEqual([("check pass", True), ("s scenario completed", False)], self.names())
        self.assertIn("not a known emulator fault", e2e.results[1]["detail"])

    def test_second_fault_is_not_repeated(self):
        def fn():
            self.attempts += 1
            self.fault()
        e2e.run_scenario("s", fn)
        self.assertEqual(2, self.attempts)
        self.assertEqual(1, len(e2e.RETRIES))
        self.assertEqual([("s scenario completed", False)], self.names())
        self.assertIn("second attempt", e2e.results[0]["detail"])

    def test_retries_are_capped(self):
        for i in range(e2e.MAX_RETRIES):
            e2e.run_scenario(f"s{i}", self.scenario("known"))
            self.attempts = 0
        self.assertEqual(e2e.MAX_RETRIES, len(e2e.RETRIES))
        e2e.run_scenario("last", self.scenario("known"))
        self.assertEqual(1, self.attempts)
        self.assertIn("already repeated", e2e.results[-1]["detail"])

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
                self.fault()
                raise ConnectionError("simulated")
        outside = Res("outside")
        e2e.run_scenario("s", fn)
        self.assertEqual(2, self.attempts)
        self.assertEqual(["closed 1", "left 1", "closed 2", "left 2"], closed)
        self.assertFalse(outside.closed)   # created outside any scenario: not the attempt's
        self.assertEqual(0, e2e.ATTEMPTS[0])

    def test_waits_raise_after_a_fault(self):
        e2e.FAULT.set()
        t = time.monotonic()
        with self.assertRaises(e2e.AttemptAborted):
            FakeGateway().wait_log(r"never", 30)
        self.assertLess(time.monotonic() - t, 1)
        e2e.FAULT.clear()


class Monitor(unittest.TestCase):
    """M4: a fault being recorded (its dump takes seconds) belongs to its own attempt."""

    def setUp(self):
        RetryRule.setUp(self)
        self.stop = threading.Event()
        self.th = threading.Thread(target=e2e.fault_monitor, args=(self.stop,), daemon=True)
        self.dump = e2e.dump_state
        self.once = e2e.monitor_once

    def tearDown(self):
        self.stop.set()
        self.th.join(5)
        e2e.dump_state, e2e.monitor_once = self.dump, self.once

    def test_slow_dump_stays_in_its_attempt(self):
        def slow_dump(g, why):
            time.sleep(1.5)
            return "fault-x.txt", {}, ["CPU#0\nPC=40000000", "CPU#0\nPC=40000000"]
        e2e.dump_state = slow_dump
        self.th.start()

        def fn():
            self.attempts += 1
            if self.attempts == 1:
                g = FakeGateway()
                g.proc, g.started, g.last_rx = FakeProc(), time.monotonic(), time.monotonic()
                e2e.GATEWAYS.append(g)
                g.signature = (time.monotonic(), "LoadStorePIFAddrError (simulated)")
                time.sleep(0.7)   # returns while the monitor is still dumping
        e2e.run_scenario("first", fn)
        self.assertEqual(2, self.attempts)
        self.assertEqual(1, len(e2e.RETRIES))
        self.assertEqual("first", e2e.RETRIES[0]["fault"]["scenario"])

        def nxt():
            FakeGateway().wait_log(r"never", 1.2)   # not cut short by the other attempt's fault
            e2e.check("next scenario", True)
        e2e.run_scenario("next", nxt)
        self.assertEqual(("next scenario", True), (e2e.results[-1]["criterion"],
                                                   e2e.results[-1]["ok"]))
        self.assertEqual(1, len(e2e.FAULTS))

    def test_monitor_error_is_recorded(self):
        def boom(seen):
            raise RuntimeError("simulated monitor bug")
        e2e.monitor_once = boom
        self.th.start()

        def fn():
            time.sleep(1.2)
        e2e.run_scenario("s", fn)
        self.assertTrue(any("simulated monitor bug" in m for m in e2e.MONITOR_ERRORS))
        self.assertTrue(self.th.is_alive())   # it keeps watching


class FakeBrokerPort(unittest.TestCase):
    def test_close_frees_the_port(self):
        """M5: a retried scenario binds its fake broker's port again."""
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        b = e2e.FakeBroker(port, "cut")
        c = socket.create_connection(("127.0.0.1", port))   # a client still connected
        time.sleep(0.2)
        b.close()
        self.assertFalse(b._th.is_alive())
        b2 = e2e.FakeBroker(port, "cut")
        b2.close()
        c.close()


if __name__ == "__main__":
    unittest.main()
