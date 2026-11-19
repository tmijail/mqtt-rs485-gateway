#!/usr/bin/env python3
"""Self-test of the E2E harness' fault handling (issues #92, #96 and #105), without QEMU: the
known emulator-fault signatures, the hang decision, the retry rule of run_scenario, the fault
monitor's hand-over between attempts and the clean-up of what an attempt leaves open. Runs in
the CI host-tests job:

    python idf/tools/e2e/test_harness.py   (needs tools/e2e/requirements.txt)
"""
import argparse
import contextlib
import io
import pathlib
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import e2e  # noqa: E402
import scenarios  # noqa: E402

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
# park_loop and the idle wait, for the hang dump's analysis
SYMS = {"park_loop": [(0x40380000, 0x40380040)],
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

    def test_iram_panic_without_an_image_check_is_the_firmwares(self):
        # round 3: the PIF fault with an IRAM PC (the tick ISR at a trial image's start, or the
        # RS485 ISR, mask_port) and no image write or check before it is a firmware failure
        for context in (["I (18925) main: configuration from /cfg/config.json (LittleFS)",
                         "I (18929) syslog: forwarding logs to 10.0.2.2:15514"],
                        ["I (18929) syslog: forwarding logs to 10.0.2.2:15514"]):
            g = FakeGateway()
            g.feed(BOOTLOADER + [APP] + context + PANIC
                   + ["", "Backtrace: 0x4037c264:0x3fc935f0 0x40377acf:0x3fc93610"])
            self.assertIsNone(g.signature)
            whats = [p[1] for p in g.unknown_panics()]
            self.assertIn("Guru Meditation (LoadStorePIFAddrError)", whats)
            self.assertIn("backtrace without a known emulator signature", whats)

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

    def test_every_firmware_failure_is_seen(self):
        # round 3: each of these on the console forbids a retry
        cases = {
            "Guru Meditation Error: Core  0 panic'ed (Interrupt wdt timeout on CPU0).":
                "Guru Meditation (Interrupt wdt timeout on CPU0)",
            "Guru Meditation Error: Core  1 panic'ed (Unhandled debug exception).":
                "Guru Meditation (Unhandled debug exception)",
            "Guru Meditation Error: Core  0 panic'ed (Double exception).":
                "Guru Meditation (Double exception)",
            "Guru Meditation Error: Core  0 panic'ed (Cache error).": "Guru Meditation (Cache error)",
            "Guru Meditation Error: Core  0 panic'ed (StoreProhibited). Exception was unhandled.":
                "Guru Meditation (StoreProhibited)",
            "assert failed: xQueueGenericSend queue.c:832 (pxQueue)": "assert failed",
            "E (5000) task_wdt: Aborting.": "task watchdog",
            "E (5000) task_wdt: Task watchdog got triggered. The following tasks/users did not "
            "reset the watchdog in time:": "task watchdog",
            "ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM) at 0x42001234":
                "ESP_ERROR_CHECK failed",
            "CORRUPT HEAP: Bad head at 0x3fc9a000. Expected 0xabba1234 got 0xfefefefe":
                "corrupt heap",
            "***ERROR*** A stack overflow in task esp_timer has been detected.": "stack overflow",
            "abort() was called at PC 0x4037a1b2 on core 0": "abort() not requested by the harness",
            "Backtrace: 0x4037c264:0x3fc935f0 0x40377acf:0x3fc93610":
                "backtrace without a known emulator signature",
        }
        for line, what in cases.items():
            g = FakeGateway()
            g.feed([APP, "I (1) edge: Sparkplug session up", line])
            self.assertIn(what, [p[1] for p in g.unknown_panics()], line)

    def test_the_shells_panic_is_not_a_failure(self):
        # the shell's 'panic': its marker, then (up to DELIBERATE_LINES later, other tasks'
        # lines in between) the abort and its backtrace
        g = FakeGateway()
        g.feed([APP, "panic requested from the console"] + [f"I ({n}) edge: noise" for n in range(15)]
               + ["abort() was called at PC 0x42001234 on core 0", "",
                  "Backtrace: 0x40375c4e:0x3fcf2a30 0x4037a1b5:0x3fcf2a50"])
        self.assertEqual([], g.unknown_panics())
        # but not once the marker is further back than that
        g = FakeGateway()
        g.feed([APP, "panic requested from the console"]
               + [f"I ({n}) edge: noise" for n in range(e2e.DELIBERATE_LINES + 1)]
               + ["abort() was called at PC 0x42001234 on core 0"])
        self.assertEqual(1, len(g.unknown_panics()))
        # issue #132: the shell's 'coredump' summary quotes the reason of the dump in flash
        g = FakeGateway()
        g.feed([APP, "coredump  12345 bytes in flash, checksum correct; the reset reason of "
                "this boot: panic", "task      panic",
                "reason    abort() was called at PC 0x42001234 on core 0"])
        self.assertEqual([], g.unknown_panics())
        # the same reason anywhere else is still a failure
        g = FakeGateway()
        g.feed([APP, "I (1) edge: Sparkplug session up",
                "panic reason abort() was called at PC 0x42001234 on core 0"])
        self.assertEqual(1, len(g.unknown_panics()))
        # the backtrace of the known emulator panic is not a failure either
        g = FakeGateway()
        g.feed([APP] + FLAKE + ["", "Backtrace: 0x4037c264:0x3fc935f0 0x40377acf:0x3fc93610"])
        self.assertIsNotNone(g.signature)
        self.assertEqual([], g.unknown_panics())

    def test_a_dump_without_a_requested_panic_is_a_failure(self):
        # issue #256: the panic output of core 0 mixed with core 1's character by character: no
        # Guru, EXCVADDR or backtrace line; only the dump writer's line came out whole
        mixed = ["GD u(r1u 7M3editat8i5on) E rreors: pCor_e core 0_ dpaunmicp'e_d",
                 "I (61234) esp_core_dump_flash: Save core dump to flash...",
                 "I (61500) esp_core_dump_flash: Core dump has been saved to flash.",
                 "Rebooting..."]
        g = FakeGateway()
        g.feed([APP, "I (1) edge: Sparkplug session up"] + mixed + BOOTLOADER)
        self.assertEqual(["core dump saved without a panic requested by the harness"],
                         [p[1] for p in g.unknown_panics()])
        # the shell's 'panic' of an earlier boot does not excuse a dump of this boot
        g = FakeGateway()
        g.feed([APP, "panic requested from the console",
                "I (500) esp_core_dump_flash: Core dump has been saved to flash."]
               + BOOTLOADER + [APP, "I (1) edge: Sparkplug session up"] + mixed)
        self.assertEqual(1, len(g.unknown_panics()))

    def test_a_dump_of_a_requested_or_known_panic_is_not_a_failure(self):
        # issue #256: the shell's 'panic', with the dump writer's lines far after the marker
        g = FakeGateway()
        g.feed([APP, "panic requested from the console",
                "abort() was called at PC 0x42001234 on core 0", "",
                "Backtrace: 0x40375c4e:0x3fcf2a30 0x4037a1b5:0x3fcf2a50",
                "I (61234) esp_core_dump_flash: Save core dump to flash..."]
               + [f"I ({n}) esp_core_dump_elf: noise" for n in range(3 * e2e.DELIBERATE_LINES)]
               + ["I (61500) esp_core_dump_flash: Core dump has been saved to flash.",
                  "Rebooting..."] + BOOTLOADER + [APP])
        self.assertEqual([], g.unknown_panics())
        # the dump of the known emulator panic belongs to that fault
        g = FakeGateway()
        g.feed([APP] + FLAKE + ["", "Backtrace: 0x4037c264:0x3fc935f0 0x40377acf:0x3fc93610"]
               + [f"I ({n}) esp_core_dump_elf: noise" for n in range(40)]
               + ["I (30000) esp_core_dump_flash: Core dump has been saved to flash."])
        self.assertIsNotNone(g.signature)
        self.assertEqual([], g.unknown_panics())

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
    """Round 3: a silence is never repeated; the dumps' analysis stays as a diagnostic."""

    def setUp(self):
        e2e.SYMBOLS.clear()
        e2e.SYMBOLS.update(SYMS)

    def test_analysis(self):
        ok, why = e2e.hang_decision([regs(0x40012345, 0x40380010), regs(0x40012345, 0x40380010)])
        self.assertTrue(ok)
        self.assertIn("core 0 stalled in the ROM at 0x40012345", why)
        ok, why = e2e.hang_decision([regs(0x40379b8f, 0x40380010), regs(0x40379b8f, 0x40380010)])
        self.assertIn("outside the ROM", why)
        self.assertFalse(e2e.hang_decision([None, None])[0])
        ok, why = e2e.hang_decision([regs(0x42001000, 0x40380010), regs(0x42001100, 0x40380020)])
        self.assertIn("still running", why)

    def test_a_silence_is_never_repeated(self):
        RetryRule.setUp(self)
        dump = e2e.dump_state

        def stalled_in_rom(g, why):   # even the strongest stall the dump could show
            return "fault-x.txt", {}, [regs(0x40012345, 0x40380010), regs(0x40012345, 0x40380010)]
        e2e.dump_state = stalled_in_rom
        try:
            def fn():
                self.attempts += 1
                g = FakeGateway()
                g.proc, g.started, g.last_rx = FakeProc(), time.monotonic(), time.monotonic()
                e2e.GATEWAYS.append(g)
                e2e.record_fault(g, "qemu hang", time.monotonic(),
                                 signature="QEMU silent after 'Take trusted digest key(s)'",
                                 detail="simulated")
            e2e.run_scenario("s", fn)
        finally:
            e2e.dump_state = dump
        self.assertEqual(1, self.attempts)
        self.assertEqual([], e2e.RETRIES)
        f = e2e.FAULTS[-1]
        self.assertFalse(f["retry"])
        self.assertIn("never repeated", f["decision"])
        self.assertIn("stalled in the ROM", f["analysis"])


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

    def test_unexpected_dump_is_a_fault_and_forbids_the_retry(self):
        # issue #256: a dump saved without a requested panic goes to results.json's faults, and
        # the known fault after it is not repeated
        def fn():
            self.attempts += 1
            g = FakeGateway()
            e2e.GATEWAYS.append(g)
            g.feed([APP, "I (1) edge: Sparkplug session up",
                    "I (61500) esp_core_dump_flash: Core dump has been saved to flash."])
            self.fault()
        e2e.run_scenario("s", fn)
        self.assertEqual(1, self.attempts)
        self.assertEqual([], e2e.RETRIES)
        self.assertIn("matched no known emulator fault", e2e.results[-1]["detail"])
        dumps = [f for f in e2e.FAULTS if f["kind"] == "firmware failure"]
        self.assertEqual(1, len(dumps))
        self.assertFalse(dumps[0]["retry"])
        self.assertEqual("s", dumps[0]["scenario"])
        self.assertIn("core dump saved without a panic requested by the harness",
                      dumps[0]["detail"])
        # a scenario with no failure on the console records none
        self.setUp()
        e2e.run_scenario("t", self.scenario("pass"))
        self.assertEqual([], e2e.FAULTS)

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

    def test_slave_is_put_back_after_a_faulted_attempt(self):
        """Issue #105 round 3: to the values at the attempt's start, retried or not."""
        slave = e2e.ModbusSlave()
        slave.reset()
        slave.set_float(4, 0, 777.0)          # not an initial value: the scenario-start state
        e2e.MAIN_BROKER["slave"] = slave
        start = slave.snapshot()

        def fn():
            self.attempts += 1
            slave.set_float(4, 0, 240.0)
            slave.bulk.setValues(3, 100, [2001] * 60)
            slave.ctx.refuse.add((4, 0))
            slave.delay = 0.12
            self.fault(known=False)
            raise ConnectionError("simulated")
        e2e.run_scenario("s", fn)
        self.assertEqual(start, slave.snapshot())
        e2e.MAIN_BROKER.clear()

    def test_retried_attempt_messages_leave_capture(self):
        """Issue #107: a retry starts from the same device (same bdSeq): the first attempt's
        messages must not count again in the checks that scan CAPTURE."""
        e2e.CAPTURE.clear()
        e2e.DISCARDED_CAPTURE.clear()
        e2e.CAPTURE.append((0.0, "before", None, b""))

        def fn():
            self.attempts += 1
            e2e.CAPTURE.append((0.0, f"attempt {self.attempts}", None, b""))
            if self.attempts == 1:
                self.fault()
        e2e.run_scenario("s", fn)
        self.assertEqual(["before", "attempt 2"], [m[1] for m in e2e.CAPTURE])
        self.assertEqual(["attempt 1"], [m[1] for m in e2e.DISCARDED_CAPTURE])
        e2e.CAPTURE.clear()
        e2e.DISCARDED_CAPTURE.clear()

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


class CrashDiagnostics(unittest.TestCase):
    """Issue #107: what the harness keeps of a QEMU crash."""

    def test_core_dumps_are_allowed(self):
        import resource
        e2e.allow_core_dumps()
        soft, hard = resource.getrlimit(resource.RLIMIT_CORE)
        self.assertEqual(e2e.CORE_LIMIT_B if hard == resource.RLIM_INFINITY
                         else min(e2e.CORE_LIMIT_B, hard), soft)

    def test_death_report_skips_the_harness_header(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            work = pathlib.Path(d)
            g = FakeGateway()
            g.log_path = work / "uart0-x.log"
            g.cwd = work / "qemu" / "000-uart0-x"
            g.cwd.mkdir(parents=True)
            (work / "uart0-x.log.qemu").write_text(
                "# harness: qemu-system-xtensa -M esp32s3 ...\n# harness: 2 other QEMU "
                "process(es) on the host, load average 3.06\nAdding SPI flash device\n")

            class Dead(FakeProc):
                def poll(self):
                    return -11
            g.proc, g.err_offset = Dead(), 0
            work_before, e2e.WORK = e2e.WORK, work
            try:
                self.assertIn("signal 11", g.qemu_died())
                self.assertIn("no error message", g.qemu_died())
                self.assertIsNone(g.core_file())
                (g.cwd / "core").write_bytes(b"\0")
                self.assertIn("core dump qemu/000-uart0-x/core", g.qemu_died())
                which = e2e.shutil.which
                e2e.shutil.which = lambda name: None
                try:
                    self.assertEqual("no gdb on this host",
                                     e2e.core_backtrace("qemu", g.cwd / "core"))
                finally:
                    e2e.shutil.which = which
            finally:
                e2e.WORK = work_before

    def test_tcg_mode(self):
        self.assertEqual("single", e2e.tcg_mode("auto", "FW-1 OTA by NCMD"))
        self.assertEqual("single", e2e.tcg_mode("auto", "FW-1 OTA by web upload"))
        self.assertEqual("single", e2e.tcg_mode("auto", "issue #144 OTA self-check"))
        self.assertEqual("multi", e2e.tcg_mode("auto", "main"))
        self.assertEqual("multi", e2e.tcg_mode("auto", None))
        self.assertEqual("single", e2e.tcg_mode("single", "main"))
        self.assertEqual("multi", e2e.tcg_mode("multi", "FW-1 OTA by NCMD"))

    def test_host_qemus(self):
        self.assertTrue(all(isinstance(p, int) for p in e2e.host_qemus()))


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


class UploadProgress(unittest.TestCase):
    """Issue #115: a web upload waits as long as the gateway makes progress, and fails once it
    makes none for UPLOAD_STALL_S (or after UPLOAD_CAP_S in all), against a local TLS server."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = pathlib.Path(cls.tmp.name)
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                        "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", str(d / "k.pem"),
                        "-out", str(d / "c.pem"), "-subj", "/CN=e2e", "-days", "1"],
                       check=True, capture_output=True)
        cls.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cls.ctx.load_cert_chain(d / "c.pem", d / "k.pem")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        e2e.FAULT.clear()
        self.saved = (e2e.HTTPS_PORT, e2e.UPLOAD_STALL_S, e2e.UPLOAD_CAP_S, e2e.ARGS,
                      e2e.UPLOAD_STATUS_S)
        e2e.ARGS = None
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        e2e.HTTPS_PORT = self.srv.getsockname()[1]
        self.gw = type("G", (), {})()
        self.gw.lines = []
        self.status = []   # bytes the next `status` answers report (one per command)
        self.gw.console = lambda text: (self.gw.lines.append(
            (time.monotonic(), f"ota      v1 in ota_0, other slot empty | receiving e2e "
                               f"({self.status.pop(0)} of 1052672 bytes)")) if self.status
            else None)
        self.stop = threading.Event()

    def tearDown(self):
        self.stop.set()
        self.srv.close()
        (e2e.HTTPS_PORT, e2e.UPLOAD_STALL_S, e2e.UPLOAD_CAP_S, e2e.ARGS,
         e2e.UPLOAD_STATUS_S) = self.saved

    def serve(self, read_body, lines_for, answer):
        """One request: read the headers (and the body if @read_body), add an image-check line
        to the console every 0.25 s for @lines_for s, then send @answer (None: never)."""
        def run():
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            with self.ctx.wrap_socket(c, server_side=True) as t:
                buf = b""
                while b"\r\n\r\n" not in buf:
                    buf += t.recv(4096)
                head, body = buf.split(b"\r\n\r\n", 1)
                n = int(next(ln.split(b":")[1] for ln in head.split(b"\r\n")
                             if ln.lower().startswith(b"content-length")))
                while read_body and len(body) < n:
                    body += t.recv(65536)
                end = time.monotonic() + lines_for
                k = 0
                while time.monotonic() < end and not self.stop.is_set():
                    k += 1
                    self.gw.lines.append((time.monotonic(), f"I ({k}) esp_image: segment {k}"))
                    time.sleep(0.25)
                if answer is not None:
                    t.sendall(answer)
                self.stop.wait(30)
        threading.Thread(target=run, daemon=True).start()

    def test_slow_but_progressing_upload_is_answered(self):
        e2e.UPLOAD_STALL_S, e2e.UPLOAD_CAP_S = 1.0, 30
        self.serve(True, 3.0, b"HTTP/1.1 400 Bad Request\r\nContent-Length: 8\r\n\r\nrejected")
        t0 = time.monotonic()
        st, _, body = e2e.upload_firmware(self.gw, b"\xe9" * 300_000)
        self.assertEqual((400, b"rejected"), (st, body))
        self.assertGreater(time.monotonic() - t0, 2.5)   # longer than the stall bound

    def test_bytes_written_count_as_progress(self):
        """The shell's byte count: the client sent everything long before (slirp buffers it)."""
        e2e.UPLOAD_STALL_S, e2e.UPLOAD_CAP_S, e2e.UPLOAD_STATUS_S = 1.0, 30, 0.2
        self.status = list(range(0, 1052672, 65536))
        self.serve(True, 0, None)
        t0 = time.monotonic()
        st, _, body = e2e.upload_firmware(self.gw, b"\xe9" * 1000)
        self.assertIsNone(st)   # the server never answers: the reports end, then a stall
        self.assertIn(b"no progress for 1 s", body)
        self.assertIn(b"1048576 written", body)   # the last report
        self.assertGreater(time.monotonic() - t0, 3.0)   # 17 reports 0.2 s apart, then 1 s

    def test_a_stall_fails_promptly(self):
        e2e.UPLOAD_STALL_S, e2e.UPLOAD_CAP_S = 1.0, 30
        self.serve(False, 0, None)
        t0 = time.monotonic()
        st, _, body = e2e.upload_firmware(self.gw, b"\xe9" * 300_000)
        self.assertIsNone(st)
        self.assertIn(b"no progress for 1 s", body)
        self.assertLess(time.monotonic() - t0, 10)

    def test_the_cap_ends_an_endless_upload(self):
        e2e.UPLOAD_STALL_S, e2e.UPLOAD_CAP_S = 1.0, 2.0
        self.serve(True, 20, None)
        st, _, body = e2e.upload_firmware(self.gw, b"\xe9" * 1000)
        self.assertIsNone(st)
        self.assertIn(b"still running after 2 s", body)

    def test_wait_progress(self):
        t = [0.0]
        clock, sleep = (lambda: t[0]), (lambda s: t.__setitem__(0, t[0] + s))
        # progress every 5 s for 300 s, then ready: never a stall with a 10 s bound
        self.assertEqual("ready", e2e.wait_progress(lambda: t[0] >= 300, lambda: int(t[0] // 5),
                                                    10, 900, clock=clock, sleep=sleep))
        t[0] = 0.0
        self.assertEqual("stall", e2e.wait_progress(lambda: False, lambda: 0, 10, 900,
                                                    clock=clock, sleep=sleep))
        self.assertEqual(10.0, t[0])
        t[0] = 0.0
        self.assertEqual("cap", e2e.wait_progress(lambda: False, lambda: t[0], 10, 60,
                                                  clock=clock, sleep=sleep))
        self.assertEqual(60.0, t[0])


class RxDropWindow(unittest.TestCase):
    """Issue #117: a timed measurement is measured again only when the device log shows a frame
    dropped by QEMU's NIC model inside its own window, once, and the discarded attempt is kept."""

    DROP = "W (15079) opencores.emac: emac_opencores_isr_handler: RX frame dropped (0x14)"

    def setUp(self):
        self.saved = list(e2e.REMEASURED)
        e2e.REMEASURED.clear()

    def tearDown(self):
        e2e.REMEASURED[:] = self.saved

    def test_window(self):
        lines = [(9.0, self.DROP), (10.1, "I (1) web: something else"), (10.5, self.DROP),
                 (12.4, self.DROP), (13.0, self.DROP)]
        got = [t for t, _ in e2e.rx_drops_in(lines, 10.0, 12.0)]
        self.assertEqual([10.5, 12.4], got)   # inside, and within the slack after the end
        self.assertEqual([9.0], [t for t, _ in e2e.rx_drops_in(lines, 9.1, 9.2)])   # slack before
        self.assertEqual([], e2e.rx_drops_in([(10.5, "W (1) other: RX frame dropped")], 10, 12))

    def test_rule(self):
        drops = [(10.5, self.DROP)]
        self.assertFalse(e2e.remeasure("c", "m", 1, True, drops, "passed"))       # passed
        self.assertFalse(e2e.remeasure("c", "m", 1, False, [], "no drop"))        # real failure
        self.assertFalse(e2e.remeasure("c", "m", 2, False, drops, "second"))      # once only
        self.assertEqual([], e2e.REMEASURED)
        self.assertTrue(e2e.remeasure("c", "m", 1, False, drops, "GET 200 in 1.60 s"))
        self.assertEqual([{"check": "c", "measurement": "m", "discarded": "GET 200 in 1.60 s",
                           "rx_dropped": [self.DROP]}], e2e.REMEASURED)


class CiGroups(unittest.TestCase):
    """Issue #121: the CI runs the scenarios in parallel groups (scenarios.CI_GROUPS); the groups
    of a variant must be exactly one full run of that variant, and the variants together
    every scenario once (issues #135 and #261)."""

    def test_selection(self):
        full = scenarios.selected(set())
        # every one but 'refuse' and the optional ones (issues #175 and #176)
        self.assertEqual(len(scenarios.SCENARIOS) - 1 - len(scenarios.OPTIONAL), len(full))
        self.assertNotIn("refuse", full)
        # issue #175: an optional scenario runs only when named, alone or with others
        self.assertEqual(set(), set(full) & scenarios.OPTIONAL)
        self.assertEqual(scenarios.OPTIONAL - set(dict(scenarios.SCENARIOS)), set())
        self.assertEqual(["umh"], scenarios.selected({"umh"}))
        self.assertEqual(["main", "umh"], scenarios.selected({"main", "umh"}))
        self.assertEqual(["refuse"], scenarios.selected({"refuse"}))
        self.assertEqual(["main"], scenarios.selected({"refuse", "main"}))
        for name in scenarios.OPTIONAL:
            self.assertNotIn(name, full)
            self.assertEqual([name], scenarios.selected({name}))
        self.assertEqual(["main", "tck"], scenarios.selected({"main", "tck"}))
        self.assertEqual(["umh", "tck"], scenarios.selected({"tck", "umh"}))
        # the existing names select what they did before the finer OTA names
        fw1 = ["FW-1 OTA by NCMD", "FW-1 OTA by web upload"]
        ota = fw1 + ["issue #112 park in a long RS485 transaction",
               "issue #97 OTA trial wiring", "issue #144 OTA self-check"]
        self.assertEqual(ota, scenarios.selected({"ota"}))
        self.assertEqual(ota, scenarios.selected({"ota-fw1", "ota-park", "ota-wiring",
                                                  "ota-trial"}))
        # issue #233: ota-fw1 is the union of the two FW-1 scenarios, each with its own name
        self.assertEqual(fw1, scenarios.selected({"ota-fw1"}))
        self.assertEqual(fw1, scenarios.selected({"ota-fw1-ncmd", "ota-fw1-web"}))
        self.assertEqual(fw1[:1], scenarios.selected({"ota-fw1-ncmd"}))
        self.assertEqual(fw1[1:], scenarios.selected({"ota-fw1-web"}))
        self.assertEqual(["mqtt5", "max_packet"], scenarios.selected({"mqtt5"}))
        self.assertEqual(["sf", "sf spill", "stack margin"], scenarios.selected({"sf"}))
        # issue #261: nvs is the union of the scenarios of the NVS encryption
        nvs = ["CFG-7 NVS encryption", "issues #67/#94 NVS migration",
               "issue #109 full unencrypted NVS", "issue #94 OTA to NVS encryption"]
        self.assertEqual(nvs, scenarios.selected({"nvs"}))
        self.assertEqual(nvs, scenarios.selected({"nvs-boot", "nvs-ota"}))
        self.assertEqual(nvs[:3], scenarios.selected({"nvs-boot"}))
        self.assertEqual(nvs[3:], scenarios.selected({"nvs-ota"}))
        self.assertEqual(["FW-2 OTA signature"], scenarios.selected({"signed"}))
        # issue #135: a full run of a variant runs its own scenarios; --only selects in any one
        default, psram = scenarios.selected(set(), "default"), scenarios.selected(set(), "psram")
        secure = scenarios.selected(set(), "secure")
        self.assertEqual(sorted(scenarios.DEFAULT_VARIANT_SCENARIOS), sorted(default))
        self.assertEqual([], [n for n in default if n in psram])
        self.assertEqual(sorted(full), sorted(default + psram + secure))
        for name in ("tls", "main", "web", "sf spill", *fw1, "issue #135 no PSRAM"):
            self.assertIn(name, psram)   # the real configuration: #80, the spill, OTA buffers
        self.assertEqual(ota, scenarios.selected({"ota"}, "psram"))
        self.assertEqual(["tls", "main"], scenarios.selected({"tls", "main"}, "default"))
        self.assertTrue(scenarios.PSRAM_BUILD_SCENARIOS <= set(psram))
        # issue #261: the secure variant runs the scenarios of the NVS encryption and of the
        # signed OTA images, and only them; no other variant runs one of them
        self.assertEqual(nvs + ["FW-2 OTA signature"], secure)
        self.assertEqual(sorted(scenarios.SECURE_VARIANT_SCENARIOS), sorted(secure))
        self.assertEqual([], [n for n in secure if n in default + psram])
        self.assertEqual(["secure"] * len(secure), [scenarios.variant_of(n) for n in secure])
        self.assertEqual("psram", scenarios.variant_of("FW-1 OTA by NCMD"))
        self.assertEqual("default", scenarios.variant_of("issue #144 OTA self-check"))
        self.assertEqual(ota, scenarios.selected({"ota"}, "secure"))   # --only: in any variant
        self.assertTrue(scenarios.BOARD_BUILD_SCENARIOS <= set(secure))
        self.assertEqual(set(), scenarios.SECURE_VARIANT_SCENARIOS
                         & (scenarios.DEFAULT_VARIANT_SCENARIOS | scenarios.PSRAM_BUILD_SCENARIOS
                            | scenarios.OPTIONAL))
        with self.assertRaises(ValueError):
            scenarios.parse_only("ota,otaa")
        self.assertEqual({"ota", "main"}, scenarios.parse_only("ota, main,"))
        self.assertEqual(set(), scenarios.parse_only(""))

    def test_groups_cover_one_full_run(self):
        everything = []
        for variant, _ in scenarios.VARIANTS:
            full = scenarios.selected(set(), variant)
            seen = []
            for group, only in scenarios.groups_of(variant):
                got = scenarios.group_scenarios(only)   # raises on an unknown name
                self.assertTrue(got, f"group {group} selects nothing")
                # "6. payloads decode" needs at least one captured payload in each run
                self.assertTrue(set(got) - scenarios.NO_CAPTURE,
                                f"group {group} has no scenario with an MQTT observer")
                seen += got
            for name in full:
                self.assertEqual(1, seen.count(name),
                                 f"{name!r} in {seen.count(name)} CI groups of {variant}")
            self.assertEqual(sorted(full), sorted(seen), variant)
            everything += seen
        # issues #135 and #261: each scenario in exactly one variant
        self.assertEqual(sorted(scenarios.selected(set())), sorted(everything))
        # issue #261: a group runs only scenarios of its own variant, and none that the harness
        # skips there: the scenarios of the secure build on a board build, the scenario of the
        # PSRAM build without PSRAM, an optional scenario (its program is not in the CI)
        for variant, group, only in scenarios.CI_GROUPS:
            for name in scenarios.group_scenarios(only):
                self.assertEqual(variant, scenarios.variant_of(name), f"{name!r} in {group}")
                self.assertFalse(scenarios.would_skip(name, variant), f"{name!r} in {group}")
        # would_skip itself: each legitimate SKIP of the harness
        for name in scenarios.SECURE_VARIANT_SCENARIOS:
            self.assertEqual([True, True, False],
                             [scenarios.would_skip(name, v) for v in ("default", "psram", "secure")])
        for name in scenarios.PSRAM_BUILD_SCENARIOS:
            self.assertEqual([True, False, False],
                             [scenarios.would_skip(name, v) for v in ("default", "psram", "secure")])
        for name in scenarios.OPTIONAL:
            self.assertTrue(all(scenarios.would_skip(name, v) for v, _ in scenarios.VARIANTS))
        self.assertFalse(any(scenarios.would_skip("main", v) for v, _ in scenarios.VARIANTS))
        # each scenario that would skip in the variant of its full run is optional: a full run
        # of a variant skips nothing
        for variant, _ in scenarios.VARIANTS:
            for name in scenarios.selected(set(), variant):
                self.assertFalse(scenarios.would_skip(name, variant), f"{name!r} in {variant}")
        # issue #175: no CI group runs an optional scenario (the CI image has no benthos-umh)
        self.assertEqual(set(), set(everything) & scenarios.OPTIONAL)
        for _, _, only in scenarios.CI_GROUPS:
            self.assertEqual(set(), scenarios.parse_only(only) & scenarios.OPTIONAL)
        ids = [g for _, g, _ in scenarios.CI_GROUPS]
        self.assertEqual(len(ids), len(set(ids)))
        for g in ids:   # artifact names and directory names
            self.assertRegex(g, r"^[a-z0-9-]+$")
        self.assertEqual({v for v, _ in scenarios.VARIANTS},
                         {v for v, _, _ in scenarios.CI_GROUPS})

    def test_matrix(self):
        m = scenarios.matrix()
        run = m["run"]["include"]
        self.assertEqual(len(scenarios.CI_GROUPS), len(run))
        envs = dict(scenarios.VARIANTS)
        for job in run:
            self.assertEqual(envs[job["variant"]],
                             {"psram": job["psram"], "secure": job["secure"]})
        # one build job for each variant, with the environment of its runs
        build = m["build"]["include"]
        self.assertEqual([v for v, _ in scenarios.VARIANTS], [b["variant"] for b in build])
        for b in build:
            self.assertEqual(envs[b["variant"]], {"psram": b["psram"], "secure": b["secure"]})
        # issue #261: only the secure variant has the two functions, and it has the memory
        # configuration of the board (#135); the main variants are the configuration of the board
        self.assertEqual({"default": "0", "psram": "0", "secure": "1"},
                         {v: env["secure"] for v, env in scenarios.VARIANTS})
        self.assertEqual("1", envs["secure"]["psram"])
        # the GW42 scenario needs the board build next to the secure build: the build job of the
        # secure variant makes the two (run_e2e.sh E2E_BOARD), so it runs in that variant
        nvs = [j for j in run if "issue #94 OTA to NVS encryption" in
               scenarios.group_scenarios(j["only"])]
        self.assertEqual(["secure"], [j["variant"] for j in nvs])
        self.assertEqual(e2e.MAX_RETRIES, scenarios.max_retries())
        for name in (scenarios.NO_CAPTURE | scenarios.BOARD_BUILD_SCENARIOS
                     | scenarios.SECURE_VARIANT_SCENARIOS | scenarios.DEFAULT_VARIANT_SCENARIOS
                     | scenarios.PSRAM_BUILD_SCENARIOS):
            self.assertIn(name, dict(scenarios.SCENARIOS))
        self.assertEqual(e2e.SINGLE_TCG_SCENARIOS - set(dict(scenarios.SCENARIOS)), set())

    def test_summary(self):
        """The summary job: one row per group, and a failure for a missing group, a failed
        check, a scenario in no group and more retries than one full run may have."""
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)

            def write(variant, group, only, ok=True, retries=0, skipped=()):
                d = root / f"e2e-result-{variant}-{group}"
                d.mkdir(exist_ok=True)
                names = scenarios.group_scenarios(only)
                # a scenario that the harness skipped has a duration and no check
                checks = [{"criterion": f"{n} works", "ok": ok} for n in names
                          if n not in skipped]
                checks += [{"criterion": c, "ok": True} for c in scenarios.RUN_CHECKS[:5]]
                res = {"checks": checks, "durations": {f"{n} #1": 1.0 for n in names},
                       "retries": [{"scenario": names[0], "qemu": "SIGSEGV"}] * retries}
                if skipped is not None:
                    res["skipped"] = list(skipped)
                (d / "results.json").write_text(e2e.json.dumps(res))
                (d / "qemu-exits.txt").write_text("none\n")

            for v, g, only in scenarios.CI_GROUPS:
                write(v, g, only)
            text, problems = scenarios.summary(root)
            self.assertEqual([], problems)
            for v, _ in scenarios.VARIANTS:
                n, k = len(scenarios.selected(set(), v)), len(scenarios.groups_of(v))
                self.assertIn(f"| {v} | **all** | {n + 5 * k}/{n + 5 * k} checks passed "
                              f"({n + 5} as one full run)", text)
            self.assertIn("qemu-exits: none", text)
            for v, _ in scenarios.VARIANTS:   # issue #261: the three variants have their rows
                self.assertIn(f"| {v} | **all** |", text)
            self.assertEqual(["secure"], [v for v, _ in scenarios.VARIANTS
                                          if f"| {v} | **all** |" in
                                          scenarios.summary(root, ["secure"])[0]])
            pg, ponly = scenarios.groups_of("psram")[0]
            write("psram", pg, ponly, ok=False)
            dg, donly = scenarios.groups_of("default")[0]
            write("default", dg, donly, retries=scenarios.max_retries() + 1)
            (root / f"e2e-result-psram-{scenarios.groups_of('psram')[1][0]}"
             / "results.json").unlink()
            problems = scenarios.summary(root)[1]
            self.assertEqual(3, len(problems), problems)
            self.assertTrue(any("failed check" in p for p in problems))
            self.assertTrue(any("no results.json" in p for p in problems))
            self.assertTrue(any("scenarios repeated" in p for p in problems))
            # a group that ran the scenarios of the other variant: not one full run
            write("psram", pg, ponly)
            write("default", dg, donly)
            write("psram", scenarios.groups_of("psram")[1][0], "ota-park")
            problems = scenarios.summary(root)[1]
            self.assertTrue(any("expected" in p for p in problems), problems)
            # issue #261: a secure group that did not run one of its scenarios (a SKIP on a
            # board build has a duration; a scenario that never started has none)
            write("psram", scenarios.groups_of("psram")[1][0], scenarios.groups_of("psram")[1][1])
            self.assertEqual([], scenarios.summary(root)[1])
            sg, sonly = scenarios.groups_of("secure")[0]
            (root / f"e2e-result-secure-{sg}" / "results.json").write_text(e2e.json.dumps(
                {"checks": [{"criterion": c, "ok": True} for c in scenarios.RUN_CHECKS[:5]],
                 "durations": {}, "retries": [], "skipped": []}))
            problems = scenarios.summary(root)[1]
            self.assertTrue(any(p.startswith(f"secure/{sg}: ran []") for p in problems), problems)
            self.assertTrue(any("scenarios in no group" in p for p in problems), problems)
            write("secure", sg, sonly)
            self.assertEqual([], scenarios.summary(root)[1])

    def test_summary_skipped(self):
        """Issue #261: a scenario that the harness skipped is not a scenario that ran. The two
        groups of the secure variant on a board build (each scenario a [SKIP], each check of
        the run passed, exit 0) are not a full run of the variant."""
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)

            def write(variant, group, only, skipped):
                d = root / f"e2e-result-{variant}-{group}"
                d.mkdir(exist_ok=True)
                names = scenarios.group_scenarios(only)
                res = {"checks": [{"criterion": f"{n} works", "ok": True} for n in names
                                  if skipped is None or n not in skipped]
                       + [{"criterion": c, "ok": True} for c in scenarios.RUN_CHECKS[1:5]],
                       "durations": {f"{n} #1": 0.0 for n in names}, "retries": []}
                if skipped is not None:
                    res["skipped"] = skipped
                (d / "results.json").write_text(e2e.json.dumps(res))
                (d / "qemu-exits.txt").write_text("none\n")

            groups = scenarios.groups_of("secure")
            for g, only in groups:
                write("secure", g, only, [])
            self.assertEqual([], scenarios.summary(root, ["secure"])[1])
            # every scenario of the two groups skipped
            for g, only in groups:
                write("secure", g, only, scenarios.group_scenarios(only))
            text, problems = scenarios.summary(root, ["secure"])
            for g, only in groups:
                self.assertTrue(any(p.startswith(f"secure/{g}: skipped ") for p in problems),
                                problems)
                self.assertIn(f"SKIPPED: {', '.join(scenarios.group_scenarios(only))}", text)
            missing = [p for p in problems if "scenarios in no group" in p]
            self.assertEqual(1, len(missing), problems)
            for name in scenarios.SECURE_VARIANT_SCENARIOS:
                self.assertIn(name, missing[0])
            # one scenario of one group skipped (the #94 scenario without the board build)
            for g, only in groups:
                write("secure", g, only, [])
            g, only = groups[0]
            name = scenarios.group_scenarios(only)[0]
            write("secure", g, only, [name])
            problems = scenarios.summary(root, ["secure"])[1]
            self.assertTrue(any(p == f"secure/{g}: skipped {[name]}: each scenario of a CI group "
                                     f"must run" for p in problems), problems)
            self.assertTrue(any("scenarios in no group" in p and name in p for p in problems),
                            problems)
            # a results.json without the list (not written by this harness): no evidence
            write("secure", g, only, None)
            problems = scenarios.summary(root, ["secure"])[1]
            self.assertEqual([f'secure/{g}: results.json has no "skipped" list'], problems)


class SecureVariant(unittest.TestCase):
    """Issue #261: the scenarios of the NVS encryption and of the signed OTA images run only on
    the secure build, and the harness signs a test image only for that build."""

    def test_secure_only_skips_on_the_board_build(self):
        ran = []
        args = types.SimpleNamespace(secure=False)
        del e2e.SKIPPED[:]
        try:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                e2e.secure_only(args, "FW-2 OTA signature", lambda: ran.append(1))()
            self.assertEqual([], ran)
            self.assertEqual(["FW-2 OTA signature"], e2e.SKIPPED)
            self.assertIn("[SKIP] FW-2 OTA signature", out.getvalue())
            self.assertIn("E2E_SECURE=1", out.getvalue())
            args.secure = True
            e2e.secure_only(args, "FW-2 OTA signature", lambda: ran.append(1))()
            self.assertEqual([1], ran)
            self.assertEqual(["FW-2 OTA signature"], e2e.SKIPPED)
        finally:
            del e2e.SKIPPED[:]

    def test_images_are_signed_only_with_a_key(self):
        """The board build checks no signature: without --signing-key the image of a test is
        the unsigned image, and no signing tool runs. With a key (the secure build, or the key
        of another signer) espsecure signs it."""
        calls = []

        def fake_run(cmd, **kw):
            calls.append(cmd)
            out = pathlib.Path(cmd[cmd.index("--output") + 1])
            out.write_bytes(pathlib.Path(cmd[-1]).read_bytes() + b"S" * 4096)

        def fake_image(src, version, dst, **caps):
            made.append((version, caps))
            pathlib.Path(dst).write_bytes(b"image " + version.encode())
            return dst

        made = []
        old = e2e.subprocess.run, e2e.fw_image
        e2e.subprocess.run, e2e.fw_image = fake_run, fake_image
        try:
            with tempfile.TemporaryDirectory() as tmp:
                d = pathlib.Path(tmp)
                board = types.SimpleNamespace(signing_key="", idf_python="python")
                img = e2e.signed_image(board, "app.bin", "e2e-a", d, "a", corrupt=True)
                self.assertEqual(d / "a.bin", img)
                self.assertEqual(b"image e2e-a", img.read_bytes())
                self.assertEqual([], calls)
                self.assertEqual([("e2e-a", {"corrupt": True})], made)
                secure = types.SimpleNamespace(signing_key="/k/build.pem", idf_python="python")
                img = e2e.signed_image(secure, "app.bin", "e2e-b", d, "b")
                self.assertEqual(b"image e2e-b" + b"S" * 4096, img.read_bytes())
                self.assertEqual(1, len(calls))
                self.assertEqual("/k/build.pem", calls[0][calls[0].index("--keyfile") + 1])
                self.assertEqual(str(d / "b-unsigned.bin"), calls[0][-1])
                # the key of another signer wins over the key of the build
                e2e.signed_image(secure, "app.bin", "e2e-c", d, "c", key=d / "other.pem")
                self.assertEqual(str(d / "other.pem"), calls[1][calls[1].index("--keyfile") + 1])
        finally:
            e2e.subprocess.run, e2e.fw_image = old

    def test_app_source(self):
        """A board build (no signing key) never takes <app>-unsigned.bin: one next to its ELF is
        the leftover of a build with the signature on in the same directory (another
        firmware). The secure build must have it."""
        with tempfile.TemporaryDirectory() as tmp:
            d = pathlib.Path(tmp)
            elf = d / "rs485_spb_gateway.elf"
            board = types.SimpleNamespace(elf=str(elf), signing_key="")
            secure = types.SimpleNamespace(elf=str(elf), signing_key="/k/build.pem")
            self.assertEqual(d / "rs485_spb_gateway.bin", e2e.app_source(board))
            with self.assertRaises(SystemExit):
                e2e.app_source(secure)
            (d / "rs485_spb_gateway-unsigned.bin").write_bytes(b"stale")
            self.assertEqual(d / "rs485_spb_gateway.bin", e2e.app_source(board))
            self.assertEqual(d / "rs485_spb_gateway-unsigned.bin", e2e.app_source(secure))

    def test_check_args(self):
        """The arguments agree with the build: --secure with the two options of the sdkconfig,
        --signing-key with --secure, and the hang self-tests only on the secure build."""
        board_cfg = ("CONFIG_GW_IO_SIMULATED=y\n# CONFIG_NVS_ENCRYPTION is not set\n"
                     "# CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT is not set\n"
                     "CONFIG_SECURE_BOOT_V2_PREFERRED=y\n")
        secure_cfg = ("CONFIG_GW_IO_SIMULATED=y\nCONFIG_NVS_ENCRYPTION=y\n"
                      "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y\n")

        def args(**kw):
            a = dict(secure=False, signing_key="", board_flash="", board_app="", board_elf="",
                     simulate_qemu_death=None)
            a.update(kw)
            return types.SimpleNamespace(**a)

        sec = dict(secure=True, signing_key="k.pem")
        self.assertIsNone(e2e.check_args(args(), board_cfg))
        self.assertIsNone(e2e.check_args(args(**sec), secure_cfg))
        self.assertIsNone(e2e.check_args(args(**sec, board_flash="f", board_app="a",
                                              board_elf="e"), secure_cfg))
        # --secure does not say what the build is (a job that lost E2E_SECURE on one side)
        self.assertIn("not the secure build", e2e.check_args(args(**sec), board_cfg))
        self.assertIn("needs --secure", e2e.check_args(args(), secure_cfg))
        half = "CONFIG_NVS_ENCRYPTION=y\n# CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT is not set\n"
        self.assertIn("SECURE_SIGNED_APPS_NO_SECURE_BOOT", e2e.check_args(args(**sec), half))
        self.assertIn("NVS_ENCRYPTION", e2e.check_args(args(), half))
        # the key and the build go together
        self.assertIn("--signing-key", e2e.check_args(args(secure=True), secure_cfg))
        self.assertIn("--signing-key", e2e.check_args(args(signing_key="k.pem"), board_cfg))
        # the board build as the image before the OTA: only for the secure build, flash and app
        self.assertIn("--board-flash", e2e.check_args(args(board_flash="f", board_app="a"),
                                                      board_cfg))
        self.assertIn("go together", e2e.check_args(args(**sec, board_flash="f"), secure_cfg))
        self.assertIn("go together", e2e.check_args(args(**sec, board_app="a"), secure_cfg))
        # the hang self-tests stop at a line that only the secure build writes: without it
        # they do nothing, and the run would pass where the expected result is a failure
        for case in ("hang", "hang-mute"):
            self.assertIn("needs --secure",
                          e2e.check_args(args(simulate_qemu_death=case), board_cfg))
            self.assertIsNone(e2e.check_args(args(**sec, simulate_qemu_death=case), secure_cfg))
        for case in ("ota", "boot", "console", "main", "ota-failed", "ota-panic"):
            self.assertIsNone(e2e.check_args(args(simulate_qemu_death=case), board_cfg))

    def test_no_psram_skip_is_recorded(self):
        """Each [SKIP] of a scenario goes to SKIPPED (results.json "skipped"): the scenario of
        the PSRAM build without --psram too."""
        del e2e.SKIPPED[:]
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                e2e.run_no_psram(types.SimpleNamespace(psram=False), None, None)
            self.assertEqual(["issue #135 no PSRAM"], e2e.SKIPPED)
        finally:
            del e2e.SKIPPED[:]

    def test_runners_and_tcg(self):
        # the scenarios of the secure variant with an OTA transfer run on one TCG thread (#107)
        for name in ("issue #94 OTA to NVS encryption", "FW-2 OTA signature"):
            self.assertIn(name, e2e.SINGLE_TCG_SCENARIOS)
            self.assertIn(name, scenarios.SECURE_VARIANT_SCENARIOS)


class TckResults(unittest.TestCase):
    """Issue #176: the summary that the TCK extension publishes on SPARKPLUG_TCK/RESULT."""

    TEXT = ("2026-11-11 11:24:17.272 Summary Test Results for Edge SessionEstablishment\n"
            "Monitor:topics-nbirth-bdseq-increment: NOT EXECUTED;\n"
            "payloads-nbirth-qos: PASS;\n"
            "payloads-nbirth-timestamp: FAIL NBIRTH messages MUST include a payload timestamp: "
            "x (SessionEstablishmentTest.java:540);\n"
            "OVERALL: FAIL;\n")

    def test_parse(self):
        name, res, overall = e2e.tck.parse_result(self.TEXT)
        self.assertEqual("Edge SessionEstablishment", name)
        self.assertEqual("FAIL", overall)
        self.assertEqual(["Monitor:topics-nbirth-bdseq-increment", "payloads-nbirth-qos",
                          "payloads-nbirth-timestamp"], sorted(res))
        self.assertEqual(["NOT EXECUTED", "PASS", "FAIL"],
                         [e2e.tck.status_of(res[k]) for k in sorted(res)])
        self.assertIn("SessionEstablishmentTest.java:540", res["payloads-nbirth-timestamp"])

    def test_uint32_rule(self):
        """A FAIL of tck-id-topics-nbirth-metrics is known only when the UInt32 int_value
        metrics are its whole cause."""
        def birth(*metrics):
            p = e2e.spb.Payload()
            for name, dt, field, value in metrics:
                m = p.metrics.add()
                m.name, m.datatype = name, dt
                setattr(m, field, value)
            return p
        good = ("bdSeq", 4, "long_value", 0)
        u32 = ("Node Info/Free Heap", 7, "int_value", 120000)
        reason = e2e.tck.uint32_cause([birth(good, u32)])
        self.assertIn("Free Heap", reason)
        self.assertIn("sparkplug#102", reason)
        self.assertIsNone(e2e.tck.uint32_cause([birth(good)]))           # the TCK sees no fault
        self.assertIsNone(e2e.tck.uint32_cause([]))
        self.assertIsNone(e2e.tck.uint32_cause([birth(u32, ("x", 11, "int_value", 1))]))  # Boolean
        self.assertIsNone(e2e.tck.uint32_cause([birth(("y", 7, "long_value", 1), ("z", 3, "long_value", 1))]))
        captured = [("spBv1.0/Plant1/NBIRTH/TCK1", birth(good, u32)),
                    ("spBv1.0/Plant1/DBIRTH/TCK1/Meter1", birth(good))]
        self.assertIsNotNone(e2e.tck.known_reason("topics-nbirth-metrics", captured))
        self.assertIsNone(e2e.tck.known_reason("topics-dbirth-metrics", captured))
        self.assertIsNone(e2e.tck.known_reason("payloads-nbirth-qos", captured))

    def test_not_configured(self):
        import os
        old = os.environ.pop("E2E_TCK_DIR", None)
        try:
            root, why = e2e.tck.configured()
            self.assertIsNone(root)
            self.assertIn("E2E_TCK_DIR", why)
            with tempfile.TemporaryDirectory() as d:
                os.environ["E2E_TCK_DIR"] = d
                root, why = e2e.tck.configured()
                self.assertIsNone(root)
                self.assertIn("tck_setup.sh", why)
        finally:
            os.environ.pop("E2E_TCK_DIR", None)
            if old is not None:
                os.environ["E2E_TCK_DIR"] = old


if __name__ == "__main__":
    unittest.main()
