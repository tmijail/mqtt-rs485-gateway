#!/usr/bin/env python3
"""End-to-end test of the real firmware under QEMU (QA-3), against SPEC section 7.

Runs the ESP32-S3 image built with sdkconfig.qemu in Espressif's QEMU:
  * Ethernet: OpenCores MAC (open_eth) + slirp; the guest reaches the host as 10.0.2.2
  * UART1 (the RS485 port): QEMU chardev socket -> pymodbus RTU slave (RTU framing over TCP)
  * UART0 (console): socket used to read the log and to inject DI levels ("di <n> <0|1>")
  * MQTT: a private mosquitto; an SNTP responder answers on the host's UDP 123
  * Remote syslog (REL-4): a UDP listener on the host collects what the gateway forwards
  * Web server (CFG-3/CFG-4): slirp forwards the host's 127.0.0.1:HTTPS_PORT to the guest's port
    443 (HTTPS with the device certificate, issue #64) and HTTP_PORT to port 80 (redirect only)

Isolation: by default the harness moves itself into a private network namespace (Linux,
needs CAP_SYS_ADMIN, e.g. root in a container) with only a loopback interface. Every port,
UDP 123 included, is then private to the run, so parallel runs never collide. Without that
capability it falls back to the host's loopback with the ports given by --mqtt-port,
--modbus-port and --console-port (or E2E_MQTT_PORT, E2E_MODBUS_PORT, E2E_CONSOLE_PORT).

Usage: tools/e2e/run_e2e.sh  (prepares the flash image and calls this script)
"""
import argparse
import asyncio
import base64
import copy
import http.client as httplib
import json
import logging
import math
import os
import pathlib
import queue
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback
import zlib

import hashlib
import ssl

import google.protobuf
import paho.mqtt.client as mqtt
from pymodbus import FramerType
from pymodbus.client import ModbusTcpClient
from pymodbus.datastore import ModbusSequentialDataBlock, ModbusServerContext, ModbusSlaveContext
from pymodbus.server import ModbusTcpServer

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import spb_decode  # noqa: E402  (official proto loader + structural checks)
sys.path.insert(0, str(HERE))
import scenarios  # noqa: E402  (issue #121: the --only selection and the CI groups)
import tck  # noqa: E402  (issue #176: the optional Sparkplug TCK scenario)

# Defaults; main() overrides them from the command line or the environment. The MQTT port
# is written into the config.json of the flash image under test, so any free port works.
MQTT_PORT = int(os.environ.get("E2E_MQTT_PORT", 18831))
MODBUS_PORT = int(os.environ.get("E2E_MODBUS_PORT", 15020))
CONSOLE_PORT = int(os.environ.get("E2E_CONSOLE_PORT", 15555))
SYSLOG_PORT = int(os.environ.get("E2E_SYSLOG_PORT", 15514))
# MB-15: QEMU forwards this host port to the gateway's Modbus TCP server (port 502)
MBTCP_PORT = int(os.environ.get("E2E_MBTCP_PORT", 15502))
HTTP_PORT = int(os.environ.get("E2E_HTTP_PORT", 18080))
HTTPS_PORT = int(os.environ.get("E2E_HTTPS_PORT", 18443))
PEER_CERT = {}    # DER of the certificate the gateway presented on the last HTTPS request
WEB_PASSWORD = "e2e-pass-1"
SYSLOG = []       # (t, datagram text) received by the syslog listener
SNTP_BOUND = threading.Event()  # set when the SNTP responder owns UDP 123
SNTP_PAUSED = threading.Event()  # TIME-2: while set, the responder stays silent
PROBE_MAX_S = 10.0                # CONFIG_GW_MB_OFFLINE_PROBE_MAX_MS / 1000, read in main()
REL6_LIMIT_S = 10.0               # REL-6 (docs/SPEC.md): from the boot to the NBIRTH
GROUP, NODE, DEVICE, BULK = "Plant1", "GW01", "Meter1", "Bulk"
T = f"spBv1.0/{GROUP}"
ALIAS = {"bdSeq": 1, "DI/1": 10, "DO/1": 20, "DO/2": 21, "DO/3": 22, "DO/4": 23, "DO/5": 24, "DO/6": 25, "Counter/1": 30, "Voltage": 100, "Setpoint": 101, "Run": 102,
         "Limit": 103, "Energy": 104, "Counter": 105, "Flow": 106, "Temp": 107, "Serial": 108,
         "Alarm": 109, "Ready": 110, "Bulk0": 111}
# MB-6 values served by unit 1, and the registers that hold them (see ModbusSlave.set_mb6)
MB6 = {"Energy": 2 ** 53 + 1, "Counter": -(2 ** 53 + 1), "Flow": 1234.56789012345, "Temp": -123456,
       "Serial": "SN-0042-ABCD", "Alarm": True, "Ready": True}

spb = spb_decode.load_module()
# The dead units (Dead3..Dead5) get no answer by design; pymodbus would log every request.
logging.getLogger("pymodbus").setLevel(logging.CRITICAL)
results = []
CAPTURE = []      # every message seen by any observer: (t, topic, Payload, raw bytes)
# Issue #107: the messages of a discarded (retried) attempt. A retry starts from the same device
# (bdSeq, seq), so scenario checks that scan CAPTURE must not see them twice; criterion 6 still
# decodes them
DISCARDED_CAPTURE = []
# Issue #159: NDATA and DDATA carry no datatype (tck-id-payloads-metric-datatype-not-req). The
# datatype of each alias comes from the last NBIRTH/DBIRTH that an observer saw (datatype_of).
BIRTH_TYPES = spb_decode.BirthTypes(strict=False)
BIRTH_TYPES_LOCK = threading.Lock()   # observers run in their own paho threads
GATEWAYS = []     # every QEMU the harness started (issue #86: did one die on its own?)
RETRIES = []      # issue #86: scenarios repeated once because QEMU died under them
PROCS = []        # brokers and QEMU instances to clean up
ARGS = None       # the parsed command line (main)
T0 = time.monotonic()   # issue #92: check and fault times are seconds since the harness started
FAULTS = []       # issue #92: every emulator fault (exit, hang, known panic) and scenario timeout
DURATIONS = {}    # issue #92: seconds each scenario attempt took ("name #attempt")
# Issue #92: set while the current scenario attempt has an emulator fault or ran out of time:
# every wait of the harness then returns at once (see wait_left), so a hung QEMU costs seconds
FAULT = threading.Event()
# Issue #105: per-scenario resources that must not outlive their attempt when an exception skips
# the scenario's own clean-up: [object, close function, attempt key, closed]. An observer left
# subscribed after a retried attempt doubled every message of the rest of the run.
RESOURCES = []
STALE = []        # issue #105: messages delivered to an observer of an attempt that had ended
ATTEMPTS = [0, 0]  # issue #105: key of the current attempt (0 outside any scenario), last key


def track(obj, closer):
    """Issue #105: @closer (idempotent) runs after the current attempt if the scenario did not
    run it. Returns @obj."""
    RESOURCES.append([obj, closer, ATTEMPTS[0], False])
    return obj


def close_attempt_resources(key):
    """Issue #105: close what attempt @key left open (observers, relays, fake brokers, image
    servers); returns the type names of those it had to close."""
    left = []
    for r in RESOURCES:
        if r[2] == key and not r[3]:
            r[3] = True
            if getattr(r[0], "closed", False):
                continue   # the scenario closed it
            left.append(type(r[0]).__name__)
            try:
                r[1]()
            except Exception:   # noqa: BLE001 (a clean-up must not end the run)
                traceback.print_exc()
    return left


def now_s():
    return round(time.monotonic() - T0, 2)


class AttemptAborted(BaseException):
    """Issue #96 (round 2): raised by every wait of the harness once the attempt has an emulator
    fault or ran out of time, so no check is asserted on evidence cut short by the fault. A
    BaseException: a scenario's `except Exception` does not swallow it; run_scenario catches it."""


def wait_left(end):
    """Issue #92: seconds left until @end (time.monotonic()); AttemptAborted once the attempt has
    a fault (the scenario ends at its next wait)."""
    if FAULT.is_set():
        raise AttemptAborted(ATTEMPT.get("abort_reason") or "emulator fault")
    return end - time.monotonic()


def check(name, ok, detail=""):
    if FAULT.is_set():   # issue #92: after a fault a wait returns at once; say so
        detail = f"{detail} [after an emulator fault or timeout of this attempt]"
    results.append({"criterion": name, "ok": bool(ok), "detail": detail, "t": now_s()})
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


# ---------------------------------------------------------------- Modbus slave

class RecordingContext(ModbusSlaveContext):
    """Slave context that records every write with its function code."""

    def __init__(self, **kw):
        super().__init__(**kw)
        self.writes = []
        self.refuse = set()   # (function code, address) answered with exception 02 (SPB-16)

    def validate(self, fc_as_hex, address, count=1):
        if (fc_as_hex, address) in self.refuse:
            return False
        return super().validate(fc_as_hex, address, count)

    def setValues(self, fc_as_hex, address, values):  # noqa: N802 (pymodbus API)
        if fc_as_hex in (5, 6, 15, 16):
            self.writes.append((fc_as_hex, address, list(values)))
        super().setValues(fc_as_hex, address, values)


class ModbusSlave:
    """Unit 1 = Meter1 (SPEC example tags), unit 2 = Bulk (60 holding registers 100..159).
    Units 3..5 (Dead3..Dead5 in the configuration) do not exist: requests to them get no answer.
    Unit 7 = the second device of a device type (#213): the same addresses as a device on unit 1,
    other values; unit 2 keeps only the FC03 of Bulk, which the main scenario checks."""

    def __init__(self):
        block = lambda: ModbusSequentialDataBlock(0, [0] * 300)  # noqa: E731
        self.ctx = RecordingContext(di=block(), co=block(), hr=block(), ir=block())
        self.bulk = RecordingContext(di=block(), co=block(), hr=block(), ir=block())
        self.bulk.setValues(3, 100, list(range(1000, 1060)))
        self.unit7 = RecordingContext(di=block(), co=block(), hr=block(), ir=block())
        self.server_ctx = ModbusServerContext(slaves={1: self.ctx, 2: self.bulk, 7: self.unit7},
                                              single=False)
        self.reads = []  # (unit, function code, address, count) of every request received
        self.times = []  # monotonic arrival time of each entry of self.reads
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()
        self.server = None
        self.requests = 0
        self.delay = 0.0   # extra response time of unit 1's reads (MB-14 check: a long cycle)

    def _trace(self, sending, pdu):
        if not sending:
            self.requests += 1
            self.times.append(time.monotonic())
            self.reads.append((getattr(pdu, "dev_id", None), pdu.function_code,
                               getattr(pdu, "address", None), getattr(pdu, "count", None)))
            if self.delay and self.reads[-1][0] == 1 and pdu.function_code <= 4:
                time.sleep(self.delay)   # blocks the server loop: the answer leaves later
        return pdu

    def start(self):
        async def run():
            self.server = ModbusTcpServer(self.server_ctx, framer=FramerType.RTU,
                                          address=("127.0.0.1", MODBUS_PORT), trace_pdu=self._trace,
                                          ignore_missing_slaves=True)
            await self.server.listen()
        asyncio.run_coroutine_threadsafe(run(), self.loop).result(5)

    def stop(self):
        asyncio.run_coroutine_threadsafe(self.server.shutdown(), self.loop).result(5)

    def set_float(self, fc, addr, value):
        hi, lo = struct.unpack(">HH", struct.pack(">f", value))
        self.ctx.setValues(fc, addr, [hi, lo])

    def get(self, fc, addr, count=1):
        return self.ctx.getValues(fc, addr, count)

    def reset(self):
        """Issue #105 round 2: the values a fresh run starts with (main() and a retried
        attempt): every register and bit 0, then the ones the configuration expects."""
        for ctx in (self.ctx, self.bulk):
            for fc in (1, 2, 3, 4):
                ctx.setValues(fc, 0, [0] * 300)
            ctx.refuse.clear()
        self.delay = 0.0
        self.bulk.setValues(3, 100, list(range(1000, 1060)))
        self.set_float(4, 0, 230.5)       # Voltage, input 0..1, float32 ABCD
        self.ctx.setValues(3, 10, [50])   # Setpoint raw 50 -> 5.0
        self.set_mb6()

    def snapshot(self):
        """Issue #105 round 3: every register and bit of both units, the refused addresses and
        the response delay, as a scenario attempt starts."""
        return {"values": [[ctx.getValues(fc, 0, 300) for fc in (1, 2, 3, 4)]
                           for ctx in (self.ctx, self.bulk)],
                "refuse": [set(self.ctx.refuse), set(self.bulk.refuse)], "delay": self.delay}

    def restore(self, snap):
        """Back to @snap (snapshot()): after an attempt that a fault or an exception ended
        before the scenario put back what it changed (a Voltage, the Bulk registers, a delay)."""
        for ctx, vals, ref in zip((self.ctx, self.bulk), snap["values"], snap["refuse"], strict=True):
            for fc, v in zip((1, 2, 3, 4), vals, strict=True):
                ctx.setValues(fc, 0, list(v))
            ctx.refuse.clear()
            ctx.refuse.update(ref)
        self.delay = snap["delay"]

    def set_mb6(self):
        """MB-6 tags of Meter1 (tools/e2e/fs/config.json), each in its byte order."""
        words = lambda v, n: [(v >> (16 * (n - 1 - i))) & 0xFFFF for i in range(n)]  # noqa: E731
        swap = lambda w: ((w & 0xFF) << 8) | (w >> 8)  # noqa: E731
        self.ctx.setValues(3, 30, words(MB6["Energy"], 4))                              # ABCD
        self.ctx.setValues(3, 34, words(MB6["Counter"] & (2 ** 64 - 1), 4)[::-1])       # CDAB
        flow = struct.unpack(">Q", struct.pack(">d", MB6["Flow"]))[0]
        self.ctx.setValues(4, 40, [swap(w) for w in words(flow, 4)[::-1]])              # DCBA
        self.ctx.setValues(4, 44, [swap(w) for w in words(MB6["Temp"] & 0xFFFFFFFF, 2)])  # BADC
        text = MB6["Serial"].encode().ljust(16, b"\0")
        self.ctx.setValues(3, 50, list(struct.unpack(">8H", text)))
        self.ctx.setValues(3, 60, [0x0109])     # Alarm = bit 3; bits 0 and 8 must survive writes
        self.ctx.setValues(4, 46, [0x8000])     # Ready = bit 15


# ---------------------------------------------------------------- SNTP responder

def sntp_responder(stop):
    """Minimal SNTP server so TIME-1 is exercised (the guest asks 10.0.2.2:123)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("127.0.0.1", 123))
    except OSError as e:
        print(f"SNTP responder disabled: {e}")
        return
    SNTP_BOUND.set()
    s.settimeout(0.5)
    while not stop.is_set():
        try:
            data, addr = s.recvfrom(512)
        except socket.timeout:
            continue
        if len(data) < 48 or SNTP_PAUSED.is_set():
            continue
        now = time.time() + 2208988800
        sec, frac = int(now), int((now % 1) * 2**32)
        reply = struct.pack("!BBbb11I", 0x24, 1, 0, -20, 0, 0, 0x4C4F434C, sec, frac,
                            *struct.unpack("!II", data[40:48]), sec, frac, sec, frac)
        s.sendto(reply, addr)


# ---------------------------------------------------------------- misbehaving brokers

class FakeBroker:
    """Minimal MQTT 3.1.1 server for the connection-management checks. Every CONNECT gets a
    CONNACK (accepted); then, by @mode:
      "cut":    the connection is closed at once (a broker that accepts and drops: ACL on the
                will, duplicate client_id, connection limit);
      "refuse": every SUBSCRIBE gets a SUBACK with 0x80 (failure) for each topic;
      "cutsub": every SUBSCRIBE is granted and the connection is reset (RST) right after the SUBACK
                (issue #71: a SUBACK handled just before the transport error behind it).
    Records the connection times, the PUBLISH topics and whether the client sent DISCONNECT;
    issue #89: also one record per connection in self.sessions (its SUBSCRIBE refused, a
    DISCONNECT received, the connection over), so a check can wait for each one to end."""

    def __init__(self, port, mode):
        self.mode, self.accepted, self.published, self.disconnects = mode, [], [], 0
        self.sessions = []
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(8)
        self.closed = False
        self._th = threading.Thread(target=self._loop, daemon=True)
        self._th.start()
        track(self, self.close)   # issue #105

    def _loop(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            self.accepted.append(time.monotonic())
            threading.Thread(target=self._client, args=(c,), daemon=True).start()

    @staticmethod
    def _read(c, n):
        data = b""
        while len(data) < n:
            chunk = c.recv(n - len(data))
            if not chunk:
                raise OSError("closed")
            data += chunk
        return data

    def _packet(self, c):
        kind = self._read(c, 1)[0]
        length, shift = 0, 0
        while True:
            b = self._read(c, 1)[0]
            length |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
        return kind, self._read(c, length)

    def _client(self, c):
        rec = {"refused": False, "disconnect": False, "closed": False}
        self.sessions.append(rec)
        try:
            c.settimeout(30)
            while True:
                kind, body = self._packet(c)
                if kind >> 4 == 1:                       # CONNECT
                    c.sendall(b"\x20\x02\x00\x00")
                    if self.mode == "cut":
                        time.sleep(0.1)
                        return
                elif kind >> 4 == 8:                     # SUBSCRIBE
                    n, i = 0, 2
                    while i < len(body):
                        i += 2 + struct.unpack(">H", body[i:i + 2])[0] + 1
                        n += 1
                    if self.mode == "cutsub":
                        c.sendall(bytes([0x90, 2 + n]) + body[:2] + b"\x01" * n)
                        c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                        return
                    c.sendall(bytes([0x90, 2 + n]) + body[:2] + b"\x80" * n)
                    rec["refused"] = True
                elif kind >> 4 == 3:                     # PUBLISH
                    tlen = struct.unpack(">H", body[:2])[0]
                    self.published.append(body[2:2 + tlen].decode(errors="replace"))
                elif kind >> 4 == 12:                    # PINGREQ
                    c.sendall(b"\xd0\x00")
                elif kind >> 4 == 14:                    # DISCONNECT
                    self.disconnects += 1
                    rec["disconnect"] = True
                    return
        except OSError:
            pass
        finally:
            c.close()
            rec["closed"] = True

    def close(self):
        """Frees the port (issue #105 M5): close() alone leaves the accept() thread blocked on
        the listening socket, and a retry of the scenario could not bind it again."""
        if self.closed:
            return
        self.closed = True
        try:
            self.srv.shutdown(socket.SHUT_RDWR)   # wakes the accept()
        except OSError:
            pass
        self.srv.close()
        self._th.join(5)


REFUSED_SESSIONS = 3
REFUSED_END_S = 15   # bound for the end of each refused connection (issue #89)


def check_suback_refused(args, work):
    """Issue #11: a broker that refuses the NCMD/DCMD subscriptions (SUBACK 0x80). The node must
    not announce itself (no NBIRTH: it could not receive commands); it disconnects with
    DISCONNECT (the will is dropped) and retries with a growing backoff.
    Issue #89: each of the first REFUSED_SESSIONS refused connections must end with the client's
    DISCONNECT. The firmware sends it before it logs the next reconnect attempt, but the packet
    crosses slirp and the fake broker's thread while the log line crosses the console: on a
    loaded host the line can come first. So the check no longer counts DISCONNECTs at the moment
    of a log line: it waits, up to REFUSED_END_S, for each of those connections to end, then
    requires a DISCONNECT on every one (a connection closed without it fails, as does one that
    has not ended in time)."""
    refuser = FakeBroker(MQTT_PORT + 2, "refuse")
    gw = Gateway(args, work / "uart0-refuse.log",
                 flash_variant(args, work, "refuse", {"config.json": config_json(MQTT_PORT + 2)}))
    t_ref = time.monotonic()
    gw.start()
    gw.wait_log(r"reconnect attempt %d in" % REFUSED_SESSIONS, 40, t_ref)
    at_log = refuser.disconnects
    end = time.monotonic() + REFUSED_END_S
    while (wait_left(end) > 0 and
           sum(1 for r in refuser.sessions if r["refused"] and r["closed"]) < REFUSED_SESSIONS):
        time.sleep(0.05)
    sessions = [dict(r) for r in refuser.sessions if r["refused"]][:REFUSED_SESSIONS]
    refused = gw.find(r"refused the NCMD/DCMD subscriptions", t_ref)
    delays = [int(x) for x in re.findall(r"in (\d+) ms", " ".join(gw.find(r"reconnect attempt", t_ref)))]
    # the firmware's side of a missing DISCONNECT (esp-mqtt could not build or send it)
    send_err = gw.find(r"Error sending disconnect message|Disconnect message cannot be created", t_ref)
    gw.kill()
    refuser.close()
    ended = ["DISCONNECT" if r["disconnect"] else "closed without DISCONNECT" if r["closed"]
             else "still open" for r in sessions]
    check("MQTT-2 / issue #11 SUBACK refused -> no NBIRTH, DISCONNECT and retry with growing backoff "
          "(each refused connection ends with DISCONNECT, waited for with a bound: issue #89)",
          len(refused) >= REFUSED_SESSIONS and len(sessions) == REFUSED_SESSIONS
          and all(r["disconnect"] for r in sessions) and not refuser.published
          and len(delays) >= REFUSED_SESSIONS and delays[2] > delays[1] > delays[0],
          f"{len(refused)} refusals logged; refused connections: {ended}; DISCONNECTs received when "
          f"'reconnect attempt {REFUSED_SESSIONS}' was logged {at_log}, in the end "
          f"{refuser.disconnects}; publishes {refuser.published}, delays {delays} ms; "
          f"{(refused or ['no refusal logged'])[0]}"
          + (f"; esp-mqtt: {send_err[:2]}" if send_err else ""))


# ---------------------------------------------------------------- relay

class Relay:
    """TCP relay 127.0.0.1:@port -> a broker on 127.0.0.1:@target. stop() closes the listener and
    every relayed connection (to the gateway the broker is gone: connections are refused) while
    the broker and the observers keep running; start() opens it again.
    Issue #59: with @rate (bytes/s) the gateway -> broker direction is read at that rate while
    self.rate is set (None = unlimited), with the smallest receive buffer, so the gateway's TCP
    sees a slow reader once slirp's own buffering is full."""

    def __init__(self, port, target, rate=None):
        self.port, self.target, self.conns, self.srv = port, target, [], None
        self.rate, self.up_bytes = rate, 0
        self.start()
        track(self, self.stop)   # issue #105

    def start(self):
        self.closed = False
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if self.rate:
            self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)   # inherited
        self.srv.bind(("127.0.0.1", self.port))
        self.srv.listen(4)
        threading.Thread(target=self._loop, args=(self.srv,), daemon=True).start()

    def stop(self):
        self.closed = True
        try:
            self.srv.shutdown(socket.SHUT_RDWR)   # wakes the accept(); close() alone does not
        except OSError:
            pass
        self.srv.close()
        for s in self.conns:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.close()
        self.conns = []

    def _loop(self, srv):
        while True:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            try:
                b = socket.create_connection(("127.0.0.1", self.target))
            except OSError:
                c.close()
                continue
            self.conns += [c, b]
            threading.Thread(target=self._pump, args=(c, b, True), daemon=True).start()
            threading.Thread(target=self._pump, args=(b, c, False), daemon=True).start()

    def _pump(self, src, dst, up):
        try:
            while True:
                rate = self.rate if up else None
                data = src.recv(max(1, rate // 10) if rate else 4096)
                if not data:
                    break
                dst.sendall(data)
                if up:
                    self.up_bytes += len(data)
                if rate:
                    time.sleep(len(data) / rate)
        except OSError:
            pass
        finally:
            for s in (src, dst):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass


# ---------------------------------------------------------------- syslog listener

def syslog_listener(stop):
    """UDP syslog collector (REL-4); the guest sends to 10.0.2.2:SYSLOG_PORT."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", SYSLOG_PORT))
    s.settimeout(0.5)
    while not stop.is_set():
        try:
            data, _ = s.recvfrom(2048)
        except socket.timeout:
            continue
        SYSLOG.append((time.monotonic(), data.decode(errors="replace")))


RFC5424 = re.compile(r'^<(\d{1,3})>1 (\S+) (\S+) (\S+) - - \[meta sequenceId="(\d+)"\] (.*)$', re.S)


def syslog_from(host, since=0.0):
    """Parsed datagrams from @host: (t, pri, timestamp, app, sequence, msg)."""
    out = []
    for t, text in list(SYSLOG):
        m = RFC5424.match(text)
        if t >= since and m and m.group(3) == host:
            out.append((t, int(m.group(1)), m.group(2), m.group(4), int(m.group(5)), m.group(6)))
    return out


def wait_syslog(host, pattern, timeout, since=0.0):
    rx = re.compile(pattern)
    end = time.monotonic() + timeout
    while True:
        hits = [d for d in syslog_from(host, since) if rx.search(d[5])]
        if hits or wait_left(end) <= 0:
            return hits[0] if hits else None
        time.sleep(0.1)


# ---------------------------------------------------------------- MQTT observer

class Observer:
    """Subscribes to spBv1.0/#. A new instance is created after each broker restart."""

    def __init__(self, port=None, auth=None):
        self.q = queue.Queue()
        self.closed = False
        self.attempt = ATTEMPTS[0]   # issue #105
        self.subscribed = threading.Event()
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"e2e-{time.monotonic()}")
        if auth:
            self.c.username_pw_set(*auth)
        self.c.on_message = self._on_msg
        self.c.on_subscribe = lambda *a: self.subscribed.set()
        # Long keepalive: the observer must survive the frozen-broker scenario.
        self.c.connect("127.0.0.1", port or MQTT_PORT, keepalive=120)
        self.c.subscribe("spBv1.0/#", qos=1)
        self.c.loop_start()
        track(self, self.close)
        if not self.subscribed.wait(5):
            raise SystemExit("observer: no SUBACK from the broker")

    def close(self):
        if self.closed:   # issue #105: the scenario and run_scenario may both close it
            return
        self.closed = True
        self.c.loop_stop()
        self.c.disconnect()

    def _on_msg(self, client, userdata, msg):
        if msg.topic.startswith("spBv1.0/STATE/"):
            return   # SPB-13: the primary host's JSON STATE, published by the harness itself
        if self.closed:
            return
        if self.attempt and self.attempt != ATTEMPTS[0]:
            # issue #105: never counted twice; reported by the check at the end of the run
            STALE.append((time.monotonic(), msg.topic, self.attempt))
            return
        p = spb.Payload()
        p.ParseFromString(msg.payload)  # raises on anything the official proto cannot parse
        item = (time.monotonic(), msg.topic, p, bytes(msg.payload))
        kind, node, device = spb_decode.topic_parts(msg.topic)
        if kind in ("NBIRTH", "DBIRTH"):
            with BIRTH_TYPES_LOCK:
                BIRTH_TYPES.feed(kind, node, device, p)
        CAPTURE.append(item)
        self.q.put(item)

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def wait_all(self, topics, timeout):
        """{topic: first message} for every topic in @topics seen within @timeout."""
        found, end = {}, time.monotonic() + timeout
        while len(found) < len(topics) and wait_left(end) > 0:
            try:
                item = self.q.get(timeout=min(0.5, max(0.01, wait_left(end))))
            except queue.Empty:
                continue
            if item[1] in topics and item[1] not in found:
                found[item[1]] = item
        return found

    def wait(self, topic, timeout, pred=lambda p: True, since=None):
        """The first message on @topic for which @pred(payload) is true and, with @since, that
        arrived at or after that time (issue #161: a new boot has bdSeq 0 again, so a new session
        cannot be told apart by its bdSeq alone)."""
        end = time.monotonic() + timeout
        while wait_left(end) > 0:
            try:
                item = self.q.get(timeout=min(0.5, max(0.01, wait_left(end))))
            except queue.Empty:
                continue
            if item[1] == topic and (since is None or item[0] >= since) and pred(item[2]):
                return item
        return None

    def publish_cmd(self, topic, metrics):
        p = spb.Payload()
        p.timestamp = int(time.time() * 1000)
        for m in metrics:
            pm = p.metrics.add()
            for k, v in m.items():
                setattr(pm, k, v)
        self.c.publish(topic, p.SerializeToString(), qos=0)


def metric(p, alias=None, name=None):
    for m in p.metrics:
        if (alias is not None and m.alias == alias) or (name is not None and m.name == name):
            return m
    return None


def datatype_of(topic, m):
    """Sparkplug datatype of metric @p m of a message on @p topic: its own in a birth, else the
    one that the last birth of its node or device gave to its alias (DATA has none, #159)."""
    if m.HasField("datatype"):
        return m.datatype
    _, node, device = spb_decode.topic_parts(topic)
    with BIRTH_TYPES_LOCK:
        return BIRTH_TYPES.datatype(node, device, m.alias)


def bdseq_of(p):
    return metric(p, name="bdSeq").long_value


def do_states(p):
    """{n: bool} of the DO/n metrics of an NBIRTH (by name) or NDATA (by alias)."""
    out = {}
    for n in range(1, 9):
        m = metric(p, name=f"DO/{n}") or metric(p, alias=20 + n - 1)
        if m is not None and not m.is_null:
            out[n] = m.boolean_value
    return out


def counter_of(p, n=1):
    """Value of Counter/n (IO-4, UInt64) in an NBIRTH (by name) or NDATA (by alias), or None."""
    m = metric(p, name=f"Counter/{n}") or metric(p, alias=30 + n - 1)
    if m is None:
        return None
    # NBIRTH: the datatype UInt64. NDATA has no datatype (#159): the value is in long_value, the
    # field of the UInt64 of the NBIRTH (criterion 6 checks each DATA field against its birth)
    uint64 = m.datatype == 8 if m.HasField("datatype") else m.WhichOneof("value") == "long_value"
    return m.long_value if uint64 else None


def set_do(obs, ncmd_t, ndata_t, states):
    """NCMD DO/n = v for every {n: v}; the NDATA that confirms them (or None)."""
    obs.drain()
    obs.publish_cmd(ncmd_t, [{"name": f"DO/{n}", "datatype": 11, "boolean_value": v}
                             for n, v in states.items()])
    return obs.wait(ndata_t, 3, lambda q: all(do_states(q).get(n) == v for n, v in states.items()))


# ---------------------------------------------------------------- emulator faults (issue #92)
#
# QEMU can fail in three ways the firmware has no part in (docs/reference/e2e-harness.md#retry-rules, issues with the label upstream):
# it ends (signal or exit, #86/#90), it hangs while staying alive, or the guest takes a fault that
# only the emulator causes. A monitor thread watches every emulator of the current scenario
# attempt; on a fault it stops the attempt (every wait raises AttemptAborted), saves a state dump
# (two "info registers" through the QEMU monitor, a second apart, and the tail of the console and
# of QEMU's stderr) and ends the emulator. Only the known signatures below may be repeated (once,
# run_scenario), and only with positive evidence; any other hang, panic or timeout is a failure.

# A reset (ROM banner) starts a boot; app_main's first line ends it. The firmware never stays
# silent for long in between, nor after the line that starts an image's signature check.
BOOT_RX = re.compile(r"^ESP-ROM:|^rst:0x")
APP_RX = re.compile(r"main: rs485_spb_gateway .* reset reason")
TRUSTED_RX = re.compile(r"secure_boot_v2: Take trusted digest key\(s\)")
VERIFY_RX = re.compile(r"esp_image: Verifying image signature|secure_boot_v2: |esp_image: segment")
GURU_RX = re.compile(r"Guru Meditation Error: Core\s+\d panic'ed \((.*)\)")
EXCVADDR_RX = re.compile(r"EXCVADDR\s*:\s*0x([0-9a-fA-F]{8})")
# Issue #96 round 3: firmware failures on the console; any of them in an attempt forbids its
# retry (a Guru Meditation panic is one unless it matches the known emulator signature, and a
# backtrace is one unless it belongs to such a panic or to the shell's deliberate 'panic')
FAILURE_RX = [(re.compile(p), what) for p, what in (
    (r"assert failed:", "assert failed"),
    (r"task_wdt: Aborting|Task watchdog got triggered", "task watchdog"),
    (r"ESP_ERROR_CHECK failed", "ESP_ERROR_CHECK failed"),
    (r"CORRUPT HEAP", "corrupt heap"),
    (r"stack overflow", "stack overflow"),
    # issue #111: a park of core 1 that had to give up (QEMU build) is the firmware's
    (r"fw: core 1 park (bound reached|refused)", "core 1 park refused or past its bound"),
    # issue #112: a blocking IPC call to core 1 while it was parked (QEMU build)
    (r"fw: blocking IPC to core 1 while it was parked", "blocking IPC to the parked core"),
)]
ABORT_RX = re.compile(r"abort\(\) was called")
BACKTRACE_RX = re.compile(r"^Backtrace:")
DELIBERATE_RX = re.compile(r"panic requested from the console")   # the shell's 'panic' command
DELIBERATE_LINES = 20   # how far back an abort()/backtrace looks for it, on the same gateway
# Issue #132: the shell's 'coredump' summary quotes the panic reason of the dump in flash
# ("reason    abort() was called at ..."): a past failure that the harness asked about, not a
# new one. The panic handler's own line starts with the reason itself.
DUMP_QUOTE_RX = re.compile(r"^reason {4}")
# Issue #256: the panic handler takes no console lock, and QEMU does not stall the other core
# (#238): the two cores' output can mix character by character, and then GURU_RX, EXCVADDR_RX
# and BACKTRACE_RX find nothing. The dump writer's last line came out whole. In a boot without
# the shell's deliberate 'panic' and without a known emulator panic, it is a firmware failure
# (it has no signature: never repeated).
DUMP_SAVED_RX = re.compile(r"Core dump has been saved to flash")
# An image being written or verified by the app (download, upload, esp_ota_end, a rollback's
# check). Only lines after app_main's first line count (the bootloader prints esp_image too).
OTA_RX = re.compile(r"esp_https_ota: |fw: downloading firmware|fw: receiving a firmware upload|"
                    r"esp_image: |secure_boot_v2|esp_ota_ops: |fw: firmware .* rejected .*: back to")
PERIPHERALS = (0x60000000, 0x600FFFFF)   # ESP32-S3 peripheral address space
# Functions resolved from the ELF (load_symbols): the loops where a core waits (waiti), for the
# hang dump's analysis (diagnostic only: a silence is never repeated)
WAITING = ("park_loop", "esp_cpu_wait_for_intr")
SYMBOLS = {}         # name -> [(start, end)], from every ELF given (main)
HANG_S = 60.0        # --hang-timeout
TRUSTED_HANG_S = 30.0  # after 'Take trusted digest key(s)' the next line comes in milliseconds
MAX_RETRIES = 2      # scenarios a run may repeat; one more is a failure (issue #96 round 2)
MON_DIR = None       # short directory for the monitor sockets (main)
SIMULATED = []       # self-tests that already happened
WORK = None          # the work directory (main)
ATTEMPT = {"active": False}
MON_LOCK = threading.RLock()   # held by the monitor while it handles an attempt (issue #92 M4)
MONITOR_ERRORS = []  # exceptions of the fault monitor (each fails the run)
# Seconds a scenario attempt may take, scaled by --timeout-scale: DEFAULT_BUDGET_S unless a name
# is listed here. 900 s is less than twice the longest attempt measured on a loaded 4-CPU host
# (the #97 integration with PSRAM in single-threaded TCG, 477 s, results.json "durations"; main
# is about 200 s): on a slower host raise --timeout-scale / E2E_TIMEOUT_SCALE.
# Issue #146: the #97 integration also examines the configuration from before an update (one
# more update with its self-check (SELFCHECK_S), three saves and the restart into the earlier
# image).
SCENARIO_BUDGET_S = {"tck": 3600.0,   # issue #176: 14 TCK tests, each with a boot and a HiveMQ
                     "issue #97 OTA trial wiring": 1800.0}
DEFAULT_BUDGET_S = 900.0


def allow_core_dumps():
    """Issue #107: core dumps up to CORE_LIMIT_B for this process and so for every QEMU it starts
    (QEMU maps a large translation buffer; a core is mostly holes, but the cap bounds the disk).
    Returns the limit set, 0 if the hard limit allows none."""
    import resource
    hard = resource.getrlimit(resource.RLIMIT_CORE)[1]
    want = CORE_LIMIT_B if hard == resource.RLIM_INFINITY else min(CORE_LIMIT_B, hard)
    resource.setrlimit(resource.RLIMIT_CORE, (want, hard))
    return want


CORE_LIMIT_B = 4 << 30
QEMU_VERSION = None  # qemu-system-xtensa --version (main)


def core_backtrace(qemu, core):
    """Issue #107: every thread's backtrace of a QEMU core dump (gdb, when the host has it),
    written next to the core as core-bt.txt (the core itself is too big for a CI artefact).
    Returns the file name relative to the work directory, or why there is none."""
    gdb = shutil.which("gdb")
    if gdb is None:
        return "no gdb on this host"
    out = pathlib.Path(core).with_name("core-bt.txt")
    try:
        r = subprocess.run([gdb, "-batch", "-nx", "-ex", "info sharedlibrary", "-ex",
                            "thread apply all bt 30", str(qemu), str(core)],
                           capture_output=True, text=True, timeout=120)
        out.write_text(r.stdout + r.stderr)
    except (OSError, subprocess.SubprocessError) as e:
        return f"gdb failed ({e})"
    return str(out.relative_to(WORK))


# Issue #107: the scenarios that map flash all the time (downloads, uploads, image checks): with
# MTTCG the S3 MMU write handler reads the flash image while the other vCPU keeps running and
# fetches through the mapping being replaced ("Bad ram pointer", SIGSEGV in code_gen_buffer under
# host load, issue #242). They run both vCPUs on one host thread. The others keep
# MTTCG: single-threaded TCG is about 1.5-2x slower, too slow for their latency bounds under load.
SINGLE_TCG_SCENARIOS = {"FW-1 OTA by NCMD", "FW-1 OTA by web upload", "issue #97 OTA trial wiring",
                        "issue #144 OTA self-check",
                        "issue #94 OTA to NVS encryption", "FW-2 OTA signature"}


def tcg_mode(option, scenario):
    """QEMU's TCG threading for an emulator of @scenario: @option ('single', 'multi', or 'auto':
    single-threaded for SINGLE_TCG_SCENARIOS, multi-threaded otherwise)."""
    if option != "auto":
        return option
    return "single" if scenario in SINGLE_TCG_SCENARIOS else "multi"


def host_qemus():
    """Issue #107: pids of the qemu-system-xtensa processes running on the host (any user's run,
    other namespaces included): the parallel load a crash came with."""
    out = []
    for d in pathlib.Path("/proc").iterdir():
        if d.name.isdigit():
            try:
                if (d / "comm").read_text().strip().startswith("qemu-system-xt"):
                    out.append(int(d.name))
            except OSError:
                pass
    return out


def load_symbols(nm, elfs):
    """{name: [(start, end)]} of the WAITING functions in @elfs (nm -S), {} if nm is missing (the
    hang dump's analysis then cannot tell a waiting core)."""
    out = {}
    for elf in elfs:
        if not elf or not pathlib.Path(elf).exists():
            continue
        try:
            r = subprocess.run([nm, "-S", "--defined-only", str(elf)], capture_output=True,
                               text=True, check=True)
        except (OSError, subprocess.CalledProcessError) as e:
            print(f"[FAULT] no symbols from {elf} ({e}): the hang dumps name no function",
                  flush=True)
            continue
        for ln in r.stdout.splitlines():
            cols = ln.split()
            if len(cols) == 4 and cols[3] in WAITING:
                start = int(cols[0], 16)
                out.setdefault(cols[3], []).append((start, start + int(cols[1], 16)))
    return out


def symbol_at(pc):
    for name, ranges in SYMBOLS.items():
        if any(lo <= pc < hi for lo, hi in ranges):
            return name
    return None


def known_panic(exception, excvaddr, before):
    """Issue #92: the known emulator panic, or None. QEMU does not model the S3 cache freeze
    while an image is mapped for its write or check: an access of the other core to a
    peripheral then faults with LoadStorePIFAddrError (issue #241).
    Only that exception, at a peripheral address, with an image being written or checked by the
    app in @before (the lines since app_main started, at most 40). Round 3: the fault with the
    PC in IRAM and no such line (the tick ISR at a trial image's start, seen only with heap
    poisoning) is a firmware failure, like any other PIF fault (the RS485 ISR and mask_port are
    IRAM code that writes peripherals)."""
    if exception != "LoadStorePIFAddrError" or not PERIPHERALS[0] <= excvaddr <= PERIPHERALS[1]:
        return None
    if not any(OTA_RX.search(ln) for ln in before):
        return None
    return f"LoadStorePIFAddrError at 0x{excvaddr:08x} (peripheral) during an image write or check"


def hang_decision(regs):
    """Issue #92: (stalled, analysis) of a silent emulator's two register dumps, recorded as
    diagnostics only (round 3: no silence is ever repeated; the one dumped instance was the
    firmware's esp_timer stack overflow). "stalled" is what this analysis would have called an
    emulator stall: Retried only if the monitor answered both times and a core that is
    not in a wait loop (park_loop, the idle task's waiti) kept exactly the same PC, in the ROM,
    for the second between them: the emulator stopped inside the ROM's code. The one dumped
    occurrence stood still in IRAM, in a spinlock with a bogus pointer, which fits the esp_timer
    stack overflow found later (fix/ota-trial e09b7b4): a stall in IRAM or flash code may be the
    firmware's (QEMU reports the PC at the start of a translated block, so a short loop also
    looks still) and is not retried. A moving PC is the firmware running; a monitor that does
    not answer, or only waiting cores standing still, is no evidence."""
    if any(r is None for r in regs):
        return False, "the QEMU monitor did not answer: no evidence of where it stopped"
    pcs = [dict(re.findall(r"CPU#(\d+)\s+PC=([0-9a-f]{8})", r)) for r in regs]
    if not pcs[0] or pcs[0].keys() != pcs[1].keys():
        return False, "the register dumps have no PC for every core"
    still = []
    for cpu, pc in pcs[0].items():
        name = symbol_at(int(pc, 16))
        if pcs[1][cpu] == pc and name not in WAITING:
            where = f"0x{pc} ({pc_region(int(pc, 16))}{', ' + name if name else ''})"
            if pc_region(int(pc, 16)) == "ROM":
                return True, f"core {cpu} stalled in the ROM at {where} for the second between the dumps"
            still.append(f"core {cpu} at {where}")
    if still:
        return False, (f"{', '.join(still)} stood still outside the ROM: possibly the firmware "
                       "(a short loop, or a corrupt lock as in the esp_timer stack overflow)")
    moving = [c for c, pc in pcs[0].items() if pcs[1][c] != pc]
    if moving:
        return False, f"core(s) {', '.join(moving)} still running (PC moved): the firmware, not QEMU"
    return False, "only cores in a wait loop stood still"


def monitor_cmd(path, command, timeout=3.0):
    """Answer of the QEMU monitor (HMP) at @path to @command, or None if it gives none."""
    try:
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(timeout)
        s.connect(str(path))
    except OSError:
        return None
    buf, end = b"", time.monotonic() + timeout
    try:
        def until_prompt():
            nonlocal buf
            while not buf.endswith(b"(qemu) ") and time.monotonic() < end:
                d = s.recv(4096)
                if not d:
                    break
                buf += d
        until_prompt()
        if not buf.endswith(b"(qemu) "):
            return None
        buf = b""
        s.sendall(command.encode() + b"\n")
        until_prompt()
    except OSError:
        pass
    finally:
        s.close()
    if not buf.endswith(b"(qemu) "):
        return None
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", buf.decode(errors="replace"))
    return text.rsplit("(qemu)", 1)[0].split("\n", 1)[-1].strip()


def pc_region(pc):
    for lo, hi, name in ((0x40000000, 0x4005FFFF, "ROM"), (0x40370000, 0x403DFFFF, "IRAM"),
                         (0x42000000, 0x43FFFFFF, "flash")):
        if lo <= pc <= hi:
            return name
    return "other"


def dump_state(g, why):
    """Issue #92: the emulator's state before the harness ends it: two register dumps a second
    apart (a PC that does not move points at the emulator, one that moves at the firmware), the
    tail of the console and of QEMU's stderr. Returns (dump file name, {cpu: [pc1, pc2]}, the
    two raw dumps, None where the monitor did not answer)."""
    regs = []
    for i in range(2):
        if i:
            time.sleep(1)
        regs.append(monitor_cmd(g.mon_path, "info registers -a") if g.mon_path else None)
    pcs = {}
    for r in regs:
        for cpu, pc in re.findall(r"CPU#(\d+)\s+PC=([0-9a-f]{8})", r or ""):
            name = symbol_at(int(pc, 16))
            pcs.setdefault(f"cpu{cpu}", []).append(
                f"0x{pc} ({pc_region(int(pc, 16))}{', ' + name if name else ''})")
    err = pathlib.Path(str(g.log_path) + ".qemu")
    tail_err = []
    if err.exists():
        with open(err, "rb") as f:
            f.seek(getattr(g, "err_offset", 0))
            tail_err = f.read().decode(errors="replace").splitlines()[-10:]
    path = WORK / f"fault-{len(FAULTS):02d}-{g.log_path.stem}.txt"
    quiet = time.monotonic() - (g.last_rx or g.started or time.monotonic())
    with open(path, "w") as f:
        f.write(f"{why}\nemulator {g.log_path.name}, pid {g.proc.pid}, {quiet:.1f} s since its last "
                f"console output, phase {g.phase}\n\n--- last console lines\n")
        f.write("\n".join(ln for _, ln in g.lines[-40:]) + "\n")
        for i, r in enumerate(regs):
            f.write(f"\n--- info registers -a ({'first' if i == 0 else 'one second later'})\n")
            f.write((r or "(the QEMU monitor did not answer)") + "\n")
        f.write("\n--- QEMU stderr (this boot)\n" + "\n".join(tail_err) + "\n")
    return path.name, pcs, regs


def record_fault(g, kind, t, signature=None, detail="", end_it=True, key=None):
    """Issue #92: a fault of attempt @key (default: the current one). The attempt is stopped
    first (FAULT: every wait raises), then the state is dumped while the scenario cannot end
    the emulator (Gateway.kill waits for the dump), then the emulator is ended. Only a QEMU
    exit and the known emulator panic are repeatable (@signature); a silence never is (round 3),
    its dump's analysis (hang_decision) is kept as a diagnostic."""
    key = ATTEMPTS[0] if key is None else key
    f = {"scenario": ATTEMPT.get("name"), "attempt": ATTEMPT.get("attempt"), "attempt_key": key,
         "kind": kind, "gateway": g.log_path.name if g is not None else None,
         "gateway_id": id(g) if g is not None else None, "t": round(t - T0, 2),
         "signature": signature,
         "retry": signature is not None and kind in ("qemu exit", "emulator panic"),
         "detail": detail,
         "dump": None, "pc": {}, "monitor_answered": None, "decision": None, "analysis": None}
    FAULTS.append(f)
    if key == ATTEMPTS[0]:
        ATTEMPT.setdefault("abort_reason", f"{kind}: {signature or detail}")
        FAULT.set()
    alive = g is not None and g.proc is not None and g.proc.poll() is None and not g.killed
    if alive:
        with g._kill_lock:   # the scenario's kill() waits for the dump
            f["dump"], f["pc"], regs = dump_state(g, f"{kind}: {signature or detail}")
            f["monitor_answered"] = all(r is not None for r in regs)
            if kind in ("qemu hang", "silent emulator"):
                f["analysis"] = hang_decision(regs)[1]   # diagnostic only: never repeated
            if end_it:
                g.kill()
    if kind in ("qemu hang", "silent emulator"):
        f["decision"] = ("a silence is never repeated (it may be the firmware: the one dumped "
                         "case was the esp_timer stack overflow)")
    print(f"[FAULT] {f['scenario']}: {kind} ({f['gateway']}): {signature or detail}"
          f"{'; state in ' + f['dump'] if f['dump'] else ''}"
          f"{'; ' + f['decision'] if f['decision'] else ''}"
          f"{'; dump: ' + f['analysis'] if f['analysis'] else ''}"
          f"{'; known emulator fault, the scenario may run once more' if f['retry'] else ''}",
          flush=True)
    return f


def fault_monitor(stop):
    """Issue #92: every half second, each emulator of the running attempt: ended on its own (#86,
    #90), a known emulator panic on its console, or silent where it always prints (a boot; the
    signature check of an image); and the attempt's time budget. It handles an attempt only
    while holding MON_LOCK, which run_scenario takes to end the attempt: a fault (and its dump)
    never lands in the next attempt. An exception of its own is recorded and fails the run."""
    seen = set()
    while not stop.wait(0.5):
        try:
            with MON_LOCK:
                if ATTEMPT.get("active"):
                    monitor_once(seen)
        except Exception:   # noqa: BLE001 (recorded, and the run fails)
            traceback.print_exc()
            MONITOR_ERRORS.append(traceback.format_exc().splitlines()[-1])


def monitor_once(seen):
    now = time.monotonic()
    for g in list(GATEWAYS[ATTEMPT["first_gw"]:]):
        if id(g) in seen or g.proc is None:
            continue
        code = g.proc.poll()
        if g.killed:
            continue
        if code is not None:
            seen.add(id(g))
            died = g.qemu_died() or f"QEMU ended (code {code})"
            record_fault(g, "qemu exit", now, signature=died, detail=died)
            continue
        if g.signature is not None:
            seen.add(id(g))
            record_fault(g, "emulator panic", g.signature[0], signature=g.signature[1],
                         detail=f"known emulator panic on the console of {g.log_path.name}")
            continue
        last_t = g.last_rx or g.started
        last = g.lines[-1][1] if g.lines else ""
        if last_t is None:
            continue
        if TRUSTED_RX.search(last) and now - last_t > min(TRUSTED_HANG_S, HANG_S):
            seen.add(id(g))
            record_fault(g, "qemu hang", last_t,
                         signature="QEMU silent after 'Take trusted digest key(s)' (image "
                                   "signature check)",
                         detail=f"no console output for {now - last_t:.0f} s after {last!r}")
        elif now - last_t > HANG_S and (g.phase == "boot" or VERIFY_RX.search(last)):
            seen.add(id(g))
            record_fault(g, "silent emulator", last_t,
                         detail=f"no console output for {now - last_t:.0f} s in the "
                                f"{'boot' if g.phase == 'boot' else 'image check'}, after "
                                f"{last!r}: not a known emulator fault")
    dl = ATTEMPT.get("deadline")
    if dl is not None and now > dl and not ATTEMPT.get("timed_out"):
        ATTEMPT["timed_out"] = True
        live = [g for g in GATEWAYS[ATTEMPT["first_gw"]:] if not g.killed and g.proc is not None
                and g.proc.poll() is None]
        for g in live or [None]:
            seen.add(id(g))
            record_fault(g, "scenario timeout", dl,
                         detail=f"{ATTEMPT['name']} still running after its "
                                f"{ATTEMPT['budget']:.0f} s budget")


# ---------------------------------------------------------------- QEMU

class QemuGone(ConnectionError):
    """Issue #90: the emulator is gone (its console write failed)."""


class Gateway:
    def __init__(self, args, log_path, flash=None, efuse=None, psram=None):
        self.args, self.log_path = args, log_path
        self.flash = flash or args.flash
        self.efuse = efuse or args.efuse   # issue #97: a scenario may bring its own eFuse file
        # issue #135: False boots the image on an emulator without the octal PSRAM model (a
        # board whose PSRAM is not detected); None: as the build (--psram)
        self.psram = psram
        self.proc, self.sock, self.lines = None, None, []
        self.killed = False          # the harness ended it (kill())
        self.gone_before_kill = None  # returncode if QEMU had already exited when killed
        self._kill_lock = threading.RLock()   # also held by record_fault while it dumps
        # issue #92: what the fault monitor watches
        self.started = None          # time.monotonic() at the launch
        self.last_rx = None          # time of the last byte from the console
        self.phase = "boot"          # "boot" from a reset until app_main logs, then "run"
        self.signature = None        # (time, text) of a known emulator panic seen on the console
        self._panic = None           # (time, exception, line index) of a panic being read
        self.panics = []             # [time, what, known signature or None, line] (#96)
        self.app_line = None         # index of this boot's app_main line (#92 M1)
        self.boot_line = 0           # index of this boot's first line (its ROM banner, #256)
        self.mon_path = None         # QEMU monitor (HMP) socket, for the state dump

    def start(self):
        a = self.args
        psram = a.psram if self.psram is None else self.psram   # issue #135
        # With the octal PSRAM, -m is its size (the board's 8 MB): with 32M QEMU maps 31 MB and
        # leaves no virtual address space for the flash mmap (the partitions are not found).
        cmd = [a.qemu, "-M", "esp32s3", "-m", "8M" if psram else "32M", "-nographic",
               "-drive", f"file={pathlib.Path(self.flash).resolve()},if=mtd,format=raw",
               "-drive", f"file={pathlib.Path(self.efuse).resolve()},if=none,format=raw,id=efuse",
               "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
               "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
               *(["-global", "driver=ssi_psram,property=is_octal,value=true"] if psram else []),
               "-nic", f"user,model=open_eth,hostfwd=tcp:127.0.0.1:{HTTP_PORT}-:80,"
                       f"hostfwd=tcp:127.0.0.1:{HTTPS_PORT}-:443,"
                       f"hostfwd=tcp:127.0.0.1:{MBTCP_PORT}-:502",
               "-chardev", f"socket,id=con,host=127.0.0.1,port={CONSOLE_PORT},server=on,wait=on",
               "-serial", "chardev:con",
               "-chardev", f"socket,id=rs485,host=127.0.0.1,port={MODBUS_PORT},reconnect-ms=500",
               "-serial", "chardev:rs485",
               # issue #107: see tcg_mode()
               "-accel", f"tcg,thread={tcg_mode(a.tcg_thread, ATTEMPT.get('name'))}"]
        # issue #92: a monitor socket per emulator, read by dump_state() before a kill
        self.mon_path = MON_DIR / f"q{len(GATEWAYS)}.sock"
        cmd += ["-monitor", f"unix:{self.mon_path},server=on,wait=off"]
        err = pathlib.Path(str(self.log_path) + ".qemu")
        # issue #107: what a QEMU crash needs for its diagnosis. Each emulator runs in a directory
        # of its own with core dumps allowed (the container's core_pattern is "core": the file
        # lands in the process' working directory); its command line, the host's load and the
        # other QEMU processes of the host head its part of the stderr file.
        self.cwd = WORK / "qemu" / f"{len(GATEWAYS):03d}-{self.log_path.stem}" if WORK else None
        if self.cwd is not None:
            self.cwd.mkdir(parents=True, exist_ok=True)
        others = host_qemus()
        self.info = {"log": self.log_path.name, "cwd": str(self.cwd) if self.cwd else None,
                     "cmd": cmd, "host_qemus_at_start": len(others),
                     "loadavg_at_start": os.getloadavg()[0], "started": now_s()}
        with open(err, "ab") as f:
            f.write((f"# harness: {' '.join(cmd)}\n# harness: {len(others)} other QEMU process(es) "
                     f"on the host, load average {os.getloadavg()[0]:.2f}\n").encode())
        self.err_offset = err.stat().st_size if err.exists() else 0   # this boot's stderr only
        self.started = time.monotonic()
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=open(err, "ab"), cwd=self.cwd)
        self.info["pid"] = self.proc.pid
        PROCS.append(self.proc)
        GATEWAYS.append(self)
        if a.simulate_qemu_death == "console" and not RETRIES and len(GATEWAYS) == 1:
            # issue #96 self-test: the emulator ends before the harness reaches its console
            self.proc.send_signal(signal.SIGSEGV)
            self.proc.wait()
        for _ in range(100):
            if self.proc.poll() is not None:
                break   # issue #96: QEMU ended before its console connected
            try:
                self.sock = socket.create_connection(("127.0.0.1", CONSOLE_PORT))
                break
            except OSError:
                time.sleep(0.1)
        if self.sock is None:
            # issue #96: not an error of the harness: the fault monitor reports the death (or
            # the silence) like any other, console() raises QemuGone, waits return at once
            print(f"[QEMU] {self.log_path.name}: no console "
                  f"({self.qemu_died() or 'QEMU did not open it in 10 s'})", flush=True)
            return
        threading.Thread(target=self._reader, args=(self.sock,), daemon=True).start()
        if a.simulate_qemu_death == "boot" and not RETRIES and len(GATEWAYS) == 2:
            # issue #90 self-test: the emulator dies during a boot that is not the first
            threading.Timer(3, self.proc.send_signal, (signal.SIGSEGV,)).start()

    def _reader(self, sock):
        buf = b""
        with open(self.log_path, "ab") as log:
            while True:
                try:
                    data = sock.recv(4096)
                except OSError:
                    return
                if not data:
                    return
                log.write(data)
                log.flush()
                self.last_rx = time.monotonic()
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self._line(time.monotonic(), line.decode(errors="replace").rstrip())

    def _line(self, t, text):
        """One console line: kept, and read for the fault monitor (issue #92)."""
        self.lines.append((t, text))
        i = len(self.lines) - 1
        if BOOT_RX.search(text):
            self.phase, self.app_line, self.boot_line = "boot", None, i
        elif APP_RX.search(text):
            self.phase, self.app_line = "run", i
        # issue #96 round 3: every firmware failure the console shows (see FAILURE_RX)
        m = GURU_RX.search(text)
        if m:
            self._panic = (t, m.group(1), i)
            self.panics.append([t, f"Guru Meditation ({m.group(1)})", None, i])
        for rx, what in FAILURE_RX:
            if rx.search(text):
                self.panics.append([t, what, None, i])
        if (ABORT_RX.search(text) or BACKTRACE_RX.search(text)) and not DUMP_QUOTE_RX.search(text) \
                and not self._excused(i):
            self.panics.append([t, "abort() not requested by the harness" if ABORT_RX.search(text)
                                else "backtrace without a known emulator signature", None, i])
        if DUMP_SAVED_RX.search(text) and not self._dump_excused(i):
            self.panics.append([t, "core dump saved without a panic requested by the harness",
                                None, i])
        m = EXCVADDR_RX.search(text)
        if m and self._panic and i - self._panic[2] <= 12:
            # issue #92 M1: only the app's lines of this boot (the bootloader prints esp_image)
            lo = max(self._panic[2] - 40, (self.app_line + 1) if self.app_line is not None
                     else self._panic[2])
            sig = known_panic(self._panic[1], int(m.group(1), 16),
                              [ln for _, ln in self.lines[lo:self._panic[2]]])
            if sig:
                for p in self.panics:
                    if p[3] == self._panic[2]:
                        p[2] = sig
                if self.signature is None:
                    self.signature = (self._panic[0], sig)
            self._panic = None
        sim = self.args.simulate_qemu_death
        if sim in ("hang", "hang-mute") and not RETRIES and not SIMULATED and TRUSTED_RX.search(text):
            # issue #92 self-tests: the emulator stops right where the real hang happens; 'hang'
            # pauses the guest through the monitor (it still answers: the dump shows the stall),
            # 'hang-mute' stops the whole process (no answer: no evidence, not repeated)
            SIMULATED.append(self.log_path.name)
            if sim == "hang":
                threading.Thread(target=monitor_cmd, args=(self.mon_path, "stop"),
                                 daemon=True).start()
            else:
                self.proc.send_signal(signal.SIGSTOP)

    def _excused(self, i):
        """An abort() or a backtrace at line @i that is not a firmware failure: the shell's
        deliberate 'panic' (its marker in the DELIBERATE_LINES before, on this gateway), or the
        backtrace of a known emulator panic (its Guru line in the 30 before)."""
        if any(DELIBERATE_RX.search(ln) for _, ln in self.lines[max(0, i - DELIBERATE_LINES):i]):
            return True
        return any(p[2] is not None and 0 < i - p[3] <= 30 for p in self.panics)

    def _dump_excused(self, i):
        """Issue #256: a core dump saved at line @i that is not a firmware failure: in the same
        boot (since its ROM banner), the shell's deliberate 'panic' (its marker, at any distance:
        the dump writer prints many lines), or a known emulator panic (its own fault)."""
        lo = self.boot_line
        if any(DELIBERATE_RX.search(ln) for _, ln in self.lines[lo:i]):
            return True
        return any(p[2] is not None and lo <= p[3] < i for p in self.panics)

    def unknown_panics(self):
        """Issue #96: the firmware failures seen on this emulator's console that matched no
        known emulator signature: a Guru Meditation panic of any reason, an assert, the task
        watchdog, ESP_ERROR_CHECK, a corrupt heap, a stack overflow, an abort() or a backtrace
        other than the shell's deliberate 'panic', and (issue #256) a core dump saved in a boot
        without that 'panic' or a known emulator panic."""
        return [p for p in self.panics if p[2] is None]

    def console(self, text):
        if self.sock is None:   # issue #96: QEMU ended before its console connected
            raise QemuGone(self.qemu_died() or "QEMU console never connected")
        try:
            self.sock.sendall(text.encode() + b"\n")
        except OSError as e:   # issue #90: BrokenPipeError when the emulator is gone
            time.sleep(0.5)    # let the process be reaped so its exit status is known
            raise QemuGone(self.qemu_died() or f"QEMU console closed ({e})") from e

    def kill(self):
        # Once only (the fault monitor and the scenario may both end it): a second kill() would
        # read the harness' own SIGKILL as a QEMU death (#86). Tolerates no console (#96).
        with self._kill_lock:
            if self.killed or self.proc is None:
                return
            self.gone_before_kill = self.proc.poll()
            self.killed = True
            if hasattr(self, "info"):
                self.info["ended_at"] = now_s()
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()
            if self.sock is not None:
                self.sock.close()

    def qemu_died(self):
        """Issue #86: why the emulator process ended on its own (signal, or an exit while the
        harness still used it, with QEMU's own error such as 'Bad ram pointer' or
        'iotlb_to_section' from its stderr), or None if it ran until the harness ended it.
        A firmware crash is not this: the chip resets and QEMU keeps running."""
        code = self.gone_before_kill if self.killed else self.proc.poll() if self.proc else None
        if code is None:
            return None
        err = pathlib.Path(str(self.log_path) + ".qemu")
        lines = []
        if err.exists():
            with open(err, "rb") as f:
                f.seek(getattr(self, "err_offset", 0))
                lines = f.read().decode(errors="replace").splitlines()
        tail = [ln.strip() for ln in lines if ln.strip() and "waiting for connection" not in ln
                and ln.strip() != "Adding SPI flash device"
                and not ln.startswith("# harness:")][-3:]
        how = f"signal {-code}" if code < 0 else f"exit status {code}"
        core = self.core_file()
        return (f"QEMU ended ({how}, {self.log_path.name}); stderr: "
                f"{' | '.join(tail) if tail else 'no error message'}"
                f"{f'; core dump {core}' if core else ''}")

    def core_file(self):
        """Issue #107: the core dump this emulator left (relative to the work directory)."""
        if self.cwd is None:
            return None
        cores = sorted(self.cwd.glob("core*"))
        return str(cores[0].relative_to(WORK)) if cores else None

    def shell(self, commands, patterns, timeout=3, tries=3):
        """Ask the console shell (read-only commands only: they are repeated) until every regex in
        @patterns has matched a line of the answer. The firmware writes each answer atomically
        with respect to the log (#56); asking again also covers a line lost on the way.
        Returns (start time of the asking, {pattern: time of the first match or None})."""
        if isinstance(commands, str):
            commands = [commands]
        start = time.monotonic()
        hits = {p: None for p in patterns}
        for _ in range(tries):
            asked = time.monotonic()
            for c in commands:
                self.console(c)
            end = asked + timeout
            while wait_left(end) > 0 and None in hits.values():
                for p in hits:
                    if hits[p] is None:
                        hits[p] = self.wait_log(p, 0, asked)
                time.sleep(0.05)
            if None not in hits.values():
                break
        return start, hits

    def find(self, pattern, since=0.0):
        rx = re.compile(pattern)
        return [l for t, l in self.lines if t >= since and rx.search(l)]

    def wait_log(self, pattern, timeout, since=0.0):
        """Time of the first log line matching @pattern, or None."""
        rx = re.compile(pattern)
        end = time.monotonic() + timeout
        while True:
            for t, line in list(self.lines):
                if t >= since and rx.search(line):
                    return t
            if wait_left(end) <= 0:
                return None
            time.sleep(0.05)


# ---------------------------------------------------------------- HTTP (CFG-3, CFG-4)

# Issue #113: QEMU's open_eth model drops a received frame now and then when slirp bursts
# ("RX frame dropped (0x14)"). If the lost frame is a new connection's SYN, slirp sends it again
# only after its backoff, past a 15 s client timeout. The web clients therefore make the TCP
# connection and TLS handshake in up to TLS_CONNECT_TRIES attempts that share the caller's
# timeout (a new attempt is a new SYN; each gets at least TLS_CONNECT_MIN_S, so a short caller
# timeout keeps its single attempt, and at most TLS_CONNECT_MAX_S but the last, which gets what
# is left), on a timeout only, and before any byte of the request is
# sent: the request itself is never repeated, and a server that does not accept within the
# caller's timeout still fails as before. Every repeat is counted here.
TLS_CONNECT_TRIES = 3
TLS_CONNECT_MIN_S = 5.0
TLS_CONNECT_MAX_S = 15.0   # issue #116: a long caller timeout (an upload's cap) still retries soon
TLS_CONNECT_RETRIES = []   # (time, what) of each repeated handshake


def tls_connect(timeout, what):
    """Connected and TLS-wrapped socket to the gateway's HTTPS port (see TLS_CONNECT_TRIES)."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    end = time.monotonic() + timeout
    for attempt in range(TLS_CONNECT_TRIES):
        left = end - time.monotonic()
        per = min(max(timeout / TLS_CONNECT_TRIES, TLS_CONNECT_MIN_S), TLS_CONNECT_MAX_S)
        last = attempt == TLS_CONNECT_TRIES - 1 or left - per < TLS_CONNECT_MIN_S - 0.5
        if last:
            per = left   # the last attempt gets whatever the caller's timeout leaves
        raw = None
        try:
            raw = socket.create_connection(("127.0.0.1", HTTPS_PORT), timeout=max(per, 0.1))
            sock = ctx.wrap_socket(raw)
            sock.settimeout(timeout)
            return sock
        except TimeoutError:
            if raw is not None:
                raw.close()
            if last:
                raise
            TLS_CONNECT_RETRIES.append((time.monotonic(), what))


class _GatewayHTTPS(httplib.HTTPSConnection):
    """http.client over tls_connect()."""

    _what = "HTTPS session"

    def connect(self):
        self.sock = tls_connect(self.timeout, f"{self._what}")


def http(method, path, body=None, auth=("admin", WEB_PASSWORD), timeout=15, headers=None,
         plain=False):
    """(status, headers dict, body bytes) of one request to the gateway's web server, or
    (None, {}, error text) when it cannot be reached. @auth None = no Authorization header.
    HTTPS by default (issue #64): the device certificate is self-signed, so it is not verified
    here; its DER goes to PEER_CERT["der"] for the fingerprint check. @plain: HTTP port.
    Issue #100: a write (not GET) carries X-Requested-With, as the web UI's do; a header given
    as None in @headers is left out (the CSRF checks)."""
    h = {} if method == "GET" else {"X-Requested-With": "e2e"}
    h.update(headers or {})
    h = {k: v for k, v in h.items() if v is not None}
    if auth is not None:
        h["Authorization"] = "Basic " + base64.b64encode(f"{auth[0]}:{auth[1]}".encode()).decode()
    if body is not None and not isinstance(body, bytes):
        body = json.dumps(body).encode()
        h.setdefault("Content-Type", "application/json")
    try:
        if plain:
            c = httplib.HTTPConnection("127.0.0.1", HTTP_PORT, timeout=timeout)
        else:
            c = _GatewayHTTPS("127.0.0.1", HTTPS_PORT, timeout=timeout)
            c._what = f"{method} {path}"
        c.request(method, path, body=body, headers=h)
        if not plain:
            PEER_CERT["der"] = c.sock.getpeercert(binary_form=True)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, {k.lower(): v for k, v in r.getheaders()}, data
    except (OSError, httplib.HTTPException, ssl.SSLError) as e:
        return None, {}, str(e).encode()


def wait_http(path, timeout, auth=("admin", WEB_PASSWORD)):
    """First answer to GET @path within @timeout (the server starts after the network)."""
    end = time.monotonic() + timeout
    while True:
        r = http("GET", path, auth=auth, timeout=5)
        if r[0] is not None or wait_left(end) <= 0:
            return r
        time.sleep(0.5)


def as_json(body):
    try:
        return json.loads(body)
    except ValueError:
        return None


# ---------------------------------------------------------------- storage partition variants

def storage_partition(name="storage"):
    """(offset, size) of partition @name in partitions.csv."""
    for line in (HERE.parent.parent / "partitions.csv").read_text().splitlines():
        cols = [c.strip() for c in line.split(",")]
        if cols[0] == name:
            return int(cols[3], 0), int(cols[4], 0)
    raise SystemExit(f"no {name} partition in partitions.csv")


def flash_variant(args, work, name, files, base=None):
    """Copy of the flash image (@base, default the image under test) whose LittleFS partition
    holds @files ({name: bytes}); files=None leaves the partition erased (not mountable)."""
    offset, size = storage_partition()
    if files is None:
        image = b"\xff" * size
    else:
        src = work / f"fs-{name}"
        shutil.rmtree(src, ignore_errors=True)
        src.mkdir()
        for fname, data in files.items():
            (src / fname).parent.mkdir(parents=True, exist_ok=True)
            (src / fname).write_bytes(data)
        img = work / f"storage-{name}.bin"
        subprocess.run([args.littlefs_python, "-m", "littlefs", "create", str(src), str(img),
                        f"--fs-size={size}", "--name-max=64", "--block-size=4096"],
                       check=True, stdout=subprocess.DEVNULL)
        image = img.read_bytes()
    flash = bytearray(pathlib.Path(base or args.flash).read_bytes())
    flash[offset:offset + len(image)] = image
    out = work / f"flash-{name}.bin"
    out.write_bytes(flash)
    return out


def storage_files(args, work, name, flash):
    """The files of the LittleFS partition in the flash image @flash ({path: bytes}), read with
    littlefs-python. None when the partition cannot be read. Call it only while no emulator
    writes the image (after gw.kill())."""
    offset, size = storage_partition()
    img = work / f"storage-read-{name}.bin"
    img.write_bytes(pathlib.Path(flash).read_bytes()[offset:offset + size])
    dst = work / f"fs-read-{name}"
    shutil.rmtree(dst, ignore_errors=True)
    dst.mkdir()
    done = subprocess.run([args.littlefs_python, "-m", "littlefs", "extract", str(img), str(dst),
                           "--name-max=64", "--block-size=4096"], capture_output=True)
    if done.returncode != 0:
        return None
    return {p.relative_to(dst).as_posix(): p.read_bytes() for p in dst.rglob("*") if p.is_file()}


def write_otadata(path):
    """Issue #85: a valid first otadata entry (ota_seq 1 = ota_0, state VALID, CRC32 of the
    sequence as esp_rom_crc32_le computes it), as the 2nd stage bootloader writes it on the
    first boot of an erased otadata. With QEMU's octal ssi_psram that first write crashes QEMU
    (SIGSEGV in psram_quad_read: issue #239), so the PSRAM variant boots
    images that already carry it. Checked against the bytes the bootloader writes."""
    offset, _ = storage_partition("otadata")
    seq = struct.pack("<I", 1)
    entry = seq + b"\xff" * 20 + struct.pack("<II", 2, zlib.crc32(seq, 0xFFFFFFFF))
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(entry)


# ---------------------------------------------------------------- isolation and build facts

def isolate_network():
    """Move this process into a new network namespace with loopback up. False if not allowed."""
    if not hasattr(os, "unshare"):
        return False
    try:
        os.unshare(os.CLONE_NEWNET)
    except OSError as e:
        print(f"network isolation unavailable ({e}); using the host loopback")
        return False
    import fcntl
    s = socket.socket()
    flags = struct.unpack("16sh", fcntl.ioctl(s, 0x8913, struct.pack("16sh", b"lo", 0)))[1]  # SIOCGIFFLAGS
    fcntl.ioctl(s, 0x8914, struct.pack("16sh", b"lo", flags | 1))                          # SIOCSIFFLAGS IFF_UP
    s.close()
    return True


def sdkconfig_int(path, name):
    m = re.search(rf"^CONFIG_{name}=(\d+)$", pathlib.Path(path).read_text(), re.M)
    if not m:
        raise SystemExit(f"CONFIG_{name} not found in {path}")
    return int(m.group(1))


def config_json(mqtt_port=None, **overrides):
    """tools/e2e/fs/config.json with the harness' MQTT port (or @mqtt_port) and optional
    top-level overrides."""
    cfg = json.loads((HERE / "fs" / "config.json").read_text())
    cfg["mqtt"]["port"] = mqtt_port or MQTT_PORT
    cfg["syslog"]["port"] = SYSLOG_PORT
    for k, v in overrides.items():
        cfg[k] = v
    return json.dumps(cfg, indent=2).encode() + b"\n"


def boot_variant(args, work, name, cfg, files=None):
    """Start the gateway on a copy of the image whose LittleFS holds @cfg as config.json plus
    @files ({name: bytes}). Its console goes to uart0-<name>.log."""
    files = dict(files or {})
    files["config.json"] = json.dumps(cfg, indent=2).encode() + b"\n"
    gw = Gateway(args, work / f"uart0-{name}.log", flash_variant(args, work, name, files))
    gw.start()
    return gw


def node_config(node, **mqtt):
    """The harness configuration with edge node id @node and the "mqtt" keys in @mqtt."""
    cfg = json.loads(config_json())
    cfg["sparkplug"]["edge_node_id"] = node
    cfg["mqtt"].update(mqtt)
    return cfg


# ---------------------------------------------------------------- TLS (MQTT-4, MQTT-5)

def make_pki(work):
    """A private CA (EC P-256), a broker certificate for IP 10.0.2.2 and DNS broker.e2e, and a
    client certificate for GW13. Returns {name: path}."""
    d = work / "pki"
    d.mkdir(exist_ok=True)

    def ossl(*a):
        subprocess.run(["openssl", *a], cwd=d, check=True, capture_output=True)

    def key(name):
        ossl("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", f"{name}.key")

    key("ca")
    ossl("req", "-x509", "-new", "-key", "ca.key", "-sha256", "-days", "30", "-subj", "/CN=E2E test CA",
         "-out", "ca.pem")
    for name, cn, ext, days in (("server", "broker.e2e", "subjectAltName=IP:10.0.2.2,DNS:broker.e2e", "30"),
                                ("client", "GW13", "extendedKeyUsage=clientAuth", "30"),
                                # issue #70: expired yesterday (openssl takes a negative validity)
                                ("expired", "broker.e2e", "subjectAltName=IP:10.0.2.2,DNS:broker.e2e",
                                 "-1")):
        key(name)
        ossl("req", "-new", "-key", f"{name}.key", "-subj", f"/CN={cn}", "-out", f"{name}.csr")
        (d / f"{name}.ext").write_text(ext + "\n")
        ossl("x509", "-req", "-in", f"{name}.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
             "-CAcreateserial", "-days", days, "-sha256", "-extfile", f"{name}.ext",
             "-out", f"{name}.crt")
    return {n: d / n for n in ("ca.pem", "server.crt", "server.key", "client.crt", "client.key",
                               "expired.crt", "expired.key")}


def run_tls(args, work):
    """MQTT-4 / MQTT-5 against a mosquitto with four listeners: TLS 1.3 only (server
    authentication), TLS 1.2 only with client certificates required (mutual TLS), plain TCP
    for the observer, and TLS 1.3 with an expired broker certificate (issue #70, GW25). Separate
    boots with their own edge node ids:
      GW12  CA file in LittleFS, name checked against the IP literal (SAN IP:10.0.2.2), TLS 1.3
      GW13  mutual TLS (MQTT-5): client certificate and key from LittleFS, TLS 1.2, the broker name
            checked against tls.server_name (SAN DNS:broker.e2e); mosquitto takes the certificate's
            CN as the user name
      GW16  the same listener without a client certificate -> refused by the broker
      GW14  tls.server_name that the certificate does not carry -> rejected, never connects
      GW15  ca_file that does not exist -> reported, no connection attempted"""
    pki = make_pki(work)
    p13, pm, plain, pexp = MQTT_PORT + 5, MQTT_PORT + 6, MQTT_PORT + 7, MQTT_PORT + 12
    conf = work / "broker-tls.conf"
    # "user root": started as root, mosquitto would drop to user "mosquitto", which cannot read
    # the certificates in the work directory (no effect when not started as root)
    conf.write_text(
        "user root\nper_listener_settings true\n"
        f"listener {p13} 127.0.0.1\nallow_anonymous true\ncafile {pki['ca.pem']}\n"
        f"certfile {pki['server.crt']}\nkeyfile {pki['server.key']}\ntls_version tlsv1.3\n"
        f"listener {pm} 127.0.0.1\nallow_anonymous true\ncafile {pki['ca.pem']}\n"
        f"certfile {pki['server.crt']}\nkeyfile {pki['server.key']}\ntls_version tlsv1.2\n"
        "require_certificate true\nuse_identity_as_username true\n"
        f"listener {plain} 127.0.0.1\nallow_anonymous true\n"
        f"listener {pexp} 127.0.0.1\nallow_anonymous true\ncafile {pki['ca.pem']}\n"
        f"certfile {pki['expired.crt']}\nkeyfile {pki['expired.key']}\ntls_version tlsv1.3\n")
    blog = work / "mosquitto-tls.log"
    broker = start_broker(conf, blog)
    obs = Observer(plain)
    ca = pki["ca.pem"].read_bytes()

    # GW12: server authentication with the CA file; the name is the IP literal of mqtt.host
    node = "GW12"
    t0 = time.monotonic()
    gw = boot_variant(args, work, "tls13", node_config(node, port=p13, tls={"enabled": True,
                                                                            "ca_file": "ca.pem"}),
                      {"ca.pem": ca})
    nb = obs.wait(f"{T}/NBIRTH/{node}", 60)
    loaded = gw.find(r"TLS: ca_file: 1 certificate\(s\) from /cfg/ca\.pem", t0)
    attempt = gw.find(r"connecting to 10\.0\.2\.2:%d \(TLS\) as 'GW12'" % p13, t0)
    gw.wait_log(r"stack free \(high-water mark\)", 5, t0)
    stacks = gw.find(r"stack free \(high-water mark\)", t0)
    asked, h = gw.shell("config", [r"^tls\s+CA /cfg/ca\.pem, name checked against the broker host, "
                                   r"client certificate none"])
    shown = list(h.values())[0]
    connected = re.search(r"New client connected from \S+ as GW12 ", blog.read_text())
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node}", 5)
    check("MQTT-4 TLS 1.3 (TLS-1.3-only listener) with the CA file from LittleFS and the broker "
          "name verified against the IP literal of mqtt.host (SAN IP:10.0.2.2): NBIRTH",
          nb and loaded and attempt and connected and shown,
          f"NBIRTH {f'{nb[0] - t0:.1f} s after boot' if nb else 'missing'}; "
          f"{(loaded or ['CA not loaded'])[0]}; {(stacks or ['no stack log'])[0]}; "
          + " | ".join(gw.find(r"^tls\s", asked)))

    # GW13: mutual TLS (MQTT-5) on the TLS-1.2-only listener that requires a client certificate
    node = "GW13"
    t0 = time.monotonic()
    gw = boot_variant(args, work, "mtls", node_config(node, port=pm, tls={
        "enabled": True, "ca_file": "ca.pem", "server_name": "broker.e2e",
        "cert_file": "gw13.crt", "key_file": "gw13.key"}),
        {"ca.pem": ca, "gw13.crt": pki["client.crt"].read_bytes(),
         "gw13.key": pki["client.key"].read_bytes()})
    nb = obs.wait(f"{T}/NBIRTH/{node}", 60)
    loaded = gw.find(r"TLS: (cert_file: 1 certificate\(s\) from /cfg/gw13\.crt|client key from "
                     r"/cfg/gw13\.key)", t0)
    identity = re.search(r"New client connected from \S+ as GW13 \(p\d, c1, k\d+, u'GW13'\)",
                         blog.read_text())
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node}", 5)
    check("MQTT-5 mutual TLS (TLS 1.2 listener requiring a client certificate): certificate and key "
          "from LittleFS, broker name checked against tls.server_name; the broker sees the "
          "certificate's identity",
          nb and len(loaded) == 2 and identity,
          f"NBIRTH {'yes' if nb else 'no'}; {' | '.join(loaded)}; mosquitto: "
          f"{identity.group(0) if identity else 'no connection with user GW13'}")

    # GW16: the same listener without a client certificate: the broker refuses the handshake
    node = "GW16"
    t0 = time.monotonic()
    gw = boot_variant(args, work, "mtlsnocert", node_config(node, port=pm, tls={
        "enabled": True, "ca_file": "ca.pem", "server_name": "broker.e2e"}), {"ca.pem": ca})
    failed = gw.wait_log(r"TLS: (TLS handshake failed|the broker certificate)", 40, t0)
    nb = obs.wait(f"{T}/NBIRTH/{node}", 3)
    gw.kill()
    check("MQTT-5 a broker that requires a client certificate refuses a gateway without one",
          failed and not nb and not re.search(r"as GW16 ", blog.read_text()),
          (gw.find(r"TLS: (TLS handshake failed|the broker certificate)", t0)
           or ["no TLS error logged"])[0])

    # GW14: the certificate does not carry tls.server_name -> the handshake fails, no session
    node = "GW14"
    t0 = time.monotonic()
    gw = boot_variant(args, work, "tlsname", node_config(node, port=p13, tls={
        "enabled": True, "ca_file": "ca.pem", "server_name": "wrong.e2e"}), {"ca.pem": ca})
    # issue #70: the cause is named (also with TLS 1.3), with the certificate's names
    rejected = gw.wait_log(r"TLS: the broker certificate does not carry the expected name .*"
                           r"mbedTLS -0x2700", 40, t0)
    detail = gw.wait_log(r"TLS: certificate CN=broker\.e2e \[IP:10\.0\.2\.2,DNS:broker\.e2e\], valid .*"
                         r"expected name 'wrong\.e2e' \(mqtt\.tls\.server_name\); clock .* SNTP "
                         r"synchronised", 5, t0)
    nb = obs.wait(f"{T}/NBIRTH/{node}", 3)
    lines = gw.find(r"TLS: (the broker certificate|certificate CN)", t0)[:2]
    gw.kill()
    check("MQTT-4 / issue #70 hostname verification: tls.server_name that the broker certificate "
          "does not carry -> handshake rejected, no session; the log names the cause (TLS 1.3), "
          "the certificate's names and the expected one",
          rejected and detail and not nb and not re.search(r"as GW14 ", blog.read_text()),
          " | ".join(lines) or "no TLS error logged")

    # GW25 (issue #70): a broker certificate that expired yesterday, TLS 1.3
    node = "GW25"
    t0 = time.monotonic()
    gw = boot_variant(args, work, "tlsexpired", node_config(node, port=pexp, tls={
        "enabled": True, "ca_file": "ca.pem"}), {"ca.pem": ca})
    expired = gw.wait_log(r"TLS: the broker certificate expired \(", 40, t0)
    detail = gw.wait_log(r"TLS: certificate CN=broker\.e2e .* to \d{4}-\d\d-\d\d \d\d:\d\d:\d\d UTC; "
                         r"expected name '10\.0\.2\.2' \(the broker host\); clock \d{4}-.* SNTP "
                         r"synchronised", 5, t0)
    nb = obs.wait(f"{T}/NBIRTH/{node}", 3)
    lines = gw.find(r"TLS: (the broker certificate|certificate CN)", t0)[:2]
    gw.kill()
    check("Issue #70 an expired broker certificate (TLS 1.3) is reported as expired, with its "
          "validity, the clock and the SNTP state; no session",
          expired and detail and not nb, " | ".join(lines) or "no TLS error logged")

    # GW15: a CA file that is not in LittleFS: reported, nothing attempted
    node = "GW15"
    t0 = time.monotonic()
    cfg15 = node_config(node, port=p13, tls={"enabled": True, "ca_file": "missing.pem"})
    cfg15["web"] = {"username": "admin", "password": WEB_PASSWORD}
    gw = boot_variant(args, work, "tlsmissing", cfg15)
    missing = gw.wait_log(r"mqtt\.tls\.ca_file: /cfg/missing\.pem: not in the LittleFS partition"
                          r".*MQTT not started", 30, t0)
    attempts = gw.find(r"connecting to .* as 'GW15'", t0)
    check("MQTT-4 a CA file missing from LittleFS is reported and no connection is attempted",
          missing and not attempts,
          (gw.find(r"mqtt\.tls\.ca_file", t0) or ["no error logged"])[0])
    try:
        check_pem_files(gw, obs, node, pki, t0)
    finally:
        gw.kill()
    check_ram_margin(args, work, obs, p13, pki["ca.pem"].read_bytes())
    check_tls_during_validation(args, work, obs, p13, pki["ca.pem"].read_bytes())
    obs.close()
    broker.terminate()
    broker.wait()


def check_pem_files(gw, obs, node, pki, t0):
    """Issue #221 on GW15 (its document names /cfg/missing.pem, which is not there): the PEM files
    of the web UI. A dry run refuses the missing file at its JSON path; PUT /api/files loads the
    CA (201, its subject and the CA flag), refuses the same name again without replace=1 (409)
    and a file that is not PEM (400); GET /api/files lists it,
    never with its content. A client key that does not agree with the client certificate is
    refused at mqtt.tls.key_file, the right key is not, and GET /api/files tells which
    certificate the key agrees with. After a restart the gateway connects to the TLS broker with
    the loaded CA (the configuration did not change)."""
    wait_http("/api/status", 30)
    _, _, doc = http("GET", "/api/config")
    doc = as_json(doc) or {}

    def dry(tls):
        d = json.loads(json.dumps(doc))
        d["mqtt"]["tls"] = tls
        st, _, body = http("PUT", "/api/config?dry_run=1", d)
        return st, as_json(body) or {}

    def put(name, data, q=""):
        st, _, body = http("PUT", f"/api/files?name={name}{q}", data,
                           headers={"Content-Type": "application/x-pem-file"})
        return st, as_json(body) or {}

    ca = pki["ca.pem"].read_bytes()
    st0, j0 = dry({"enabled": True, "ca_file": "missing.pem"})
    st1, j1 = put("missing.pem", ca)
    st2, j2 = put("missing.pem", ca)
    st2r, j2r = put("missing.pem", ca, "&replace=1")
    st3, j3 = put("notes.pem", b"not a certificate\n")
    st4, _, b4 = http("GET", "/api/files")
    files = {f.get("name"): f for f in (as_json(b4) or {}).get("files", [])}
    st5, j5 = dry({"enabled": True, "ca_file": "missing.pem"})
    loaded = st0 == 400 and j0.get("path") == "mqtt.tls.ca_file" and "not in the LittleFS" in \
        j0.get("error", "") and st1 == 201 and j1.get("file", {}).get("kind") == "certificates" \
        and j1["file"].get("certs", [{}])[0].get("subject") == "CN=E2E test CA" \
        and j1["file"]["certs"][0].get("ca") is True and st2 == 409 and st2r == 201 \
        and j2r.get("files", [{}])[0].get("replaced") is True \
        and st3 == 400 \
        and st4 == 200 and files.get("missing.pem", {}).get("count") == 1 \
        and "notes.pem" not in files and b"BEGIN" not in b4 and st5 == 200 and j5.get("ok")
    check("Issue #221 PEM files from the web UI: a dry run refuses a missing CA file at "
          "mqtt.tls.ca_file; PUT /api/files loads it (201, subject, CA), refuses the same name "
          "(409; replace=1 replaces it) and a file that is not PEM (400); GET /api/files lists it "
          "without its content; "
          "then the dry run passes", loaded,
          f"dry {st0} {j0.get('path')} {j0.get('error', '')[:80]!r}; put {st1} {j1.get('file')}; "
          f"again {st2}, replace=1 {st2r}; not PEM {st3} {j3.get('error', '')[:60]!r}; list {st4} "
          f"{sorted(files)}; dry after {st5}")

    # mutual TLS files: a key that agrees with the certificate, and one that does not
    sk1, _ = put("gw15.crt", pki["client.crt"].read_bytes())
    sk2, jk = put("gw15.key", pki["client.key"].read_bytes())
    sk3, _ = put("other.key", pki["server.key"].read_bytes())
    stk, _, bk = http("GET", "/api/files")
    kf = {f.get("name"): f for f in (as_json(bk) or {}).get("files", [])}
    st6, j6 = dry({"enabled": True, "ca_file": "missing.pem", "cert_file": "gw15.crt",
                   "key_file": "other.key"})
    st7, j7 = dry({"enabled": True, "ca_file": "missing.pem", "cert_file": "gw15.crt",
                   "key_file": "gw15.key"})
    key = kf.get("gw15.key", {})
    check("Issue #221 the client key must agree with the client certificate: GET /api/files gives "
          "the key type and the agreeing certificate (never the key); a dry run with another key "
          "is refused at mqtt.tls.key_file, with the right key it passes",
          sk1 == sk2 == sk3 == 201 and jk.get("file", {}).get("kind") == "key"
          and key.get("key_type") == "EC P-256" and key.get("agrees_with") == ["gw15.crt"]
          and kf.get("other.key", {}).get("agrees_with") == [] and b"PRIVATE" not in bk
          and st6 == 400 and j6.get("path") == "mqtt.tls.key_file"
          and "does not agree with the certificate gw15.crt" in j6.get("error", "")
          and st7 == 200,
          f"put {sk1} {sk2} {sk3}; key {key}; other {kf.get('other.key')}; mismatch {st6} "
          f"{j6.get('path')} {j6.get('error', '')[:80]!r}; match {st7}")

    check_pem_manage(gw, pki)

    # the restart: the CA is now there, the session comes up over TLS
    asked = time.monotonic()
    rs = http("POST", "/api/restart", b"")[0]
    used = gw.wait_log(r"TLS: ca_file: 1 certificate\(s\) from /cfg/missing\.pem", 60, asked)
    nb = obs.wait(f"{T}/NBIRTH/{node}", 60) if used else None
    check("Issue #221 after the load of the CA file and a restart, the gateway connects to the "
          "TLS broker with it (NBIRTH)", rs == 202 and used and nb is not None,
          f"restart {rs}; {(used and 'CA loaded') or 'no CA line'}; NBIRTH {nb is not None}")


def check_pem_manage(gw, pki):
    """#221, the card Certificates (on GW15, whose saved document names missing.pem as
    mqtt.tls.ca_file): GET /api/files gives the keys that use each file, the limits and the space
    of the LittleFS partition; a dry run of a load writes nothing; the kind of a configured file
    cannot change (a key for the CA file: 400 at mqtt.tls.ca_file); a certificate and its key load
    together (a key that does not agree: 400), and a body with both in one file is refused;
    DELETE /api/files/<name> refuses a file in use (409 with used_by), removes another one (200),
    then 404; the CSRF check covers the new routes; the 17th file is refused (507)."""
    def put(q, data):
        st, _, body = http("PUT", f"/api/files?{q}", data,
                           headers={"Content-Type": "application/x-pem-file"})
        return st, as_json(body) or {}

    def listing():
        st, _, body = http("GET", "/api/files")
        j = as_json(body) or {}
        return st, j, {f.get("name"): f for f in j.get("files", [])}, body

    crt, key, other = (pki[n].read_bytes() for n in ("client.crt", "client.key", "server.key"))
    ca = pki["ca.pem"].read_bytes()
    st0, j0, f0, b0 = listing()
    print(f"  #221 LittleFS: {j0.get('fs_used_bytes')} of {j0.get('fs_total_bytes')} bytes used "
          f"with {j0.get('count')} PEM file(s) of {j0.get('total_bytes')} bytes", flush=True)
    kind = put("name=missing.pem&replace=1&dry_run=1", key)
    dry_pair = put("name=gw15b.crt&key_name=gw15b.key&dry_run=1", crt + key)
    bad_pair = put("name=gw15b.crt&key_name=gw15b.key&dry_run=1", crt + other)
    one_file = put("name=both.pem", crt + key)
    _, _, f1, _ = listing()
    pair = put("name=gw15b.crt&key_name=gw15b.key", crt + key)
    _, _, f2, b2 = listing()
    check("Issue #221 GET /api/files gives the keys of the saved configuration that use each file, "
          "the limits (16 files, 16384 bytes) and the space of the LittleFS partition; a dry run "
          "writes nothing; a key in place of the configured CA file is refused at "
          "mqtt.tls.ca_file; a certificate and its key load together (201, both listed, the key "
          "agrees), a key that does not agree is refused, and one file with both is refused",
          st0 == 200 and f0.get("missing.pem", {}).get("used_by") == ["mqtt.tls.ca_file"]
          and f0.get("gw15.crt", {}).get("used_by") == [] and j0.get("max_files") == 16
          and j0.get("max_file_bytes") == 16384 and j0.get("config_known") is True
          and (j0.get("fs_total_bytes") or 0) > (j0.get("fs_used_bytes") or 0) > 0
          and kind[0] == 400 and kind[1].get("path") == "mqtt.tls.ca_file"
          and dry_pair[0] == 200 and [f.get("kind") for f in dry_pair[1].get("files", [])]
          == ["certificates", "key"] and dry_pair[1].get("saved") is False
          and "gw15b.crt" not in f1 and bad_pair[0] == 400
          and "does not agree" in bad_pair[1].get("error", "") and one_file[0] == 400
          and "both.pem" not in f1 and pair[0] == 201
          and "gw15b.crt" in (f2.get("gw15b.key", {}).get("agrees_with") or [])
          and b"PRIVATE" not in b2 and b"BEGIN" not in b2,
          f"list {st0} {f0.get('missing.pem', {}).get('used_by')} limits {j0.get('max_files')}/"
          f"{j0.get('max_file_bytes')} fs {j0.get('fs_used_bytes')}/{j0.get('fs_total_bytes')}; "
          f"kind {kind[0]} {kind[1].get('path')} {kind[1].get('error', '')[:70]!r}; dry pair "
          f"{dry_pair[0]}; bad pair {bad_pair[0]} {bad_pair[1].get('error', '')[:60]!r}; one file "
          f"{one_file[0]}; pair {pair[0]}; key {f2.get('gw15b.key')}")

    forged = {
        "DELETE, no header": http("DELETE", "/api/files/other.key",
                                  headers={"X-Requested-With": None})[0],
        "DELETE, other Origin": http("DELETE", "/api/files/other.key",
                                     headers={"Origin": "https://evil.e2e"})[0],
        "PUT, other Origin": http("PUT", "/api/files?name=x.pem", ca,
                                  headers={"Origin": "https://evil.e2e"})[0],
        "DELETE, wrong password": http("DELETE", "/api/files/other.key", auth=("admin", "no"))[0],
    }
    used = http("DELETE", "/api/files/missing.pem")
    gone = http("DELETE", "/api/files/other.key")
    again = http("DELETE", "/api/files/other.key")
    _, _, f3, _ = listing()
    uj = as_json(used[2]) or {}
    check("Issue #221 DELETE /api/files/<name>: the CSRF check (403) and the password (401) first; "
          "a file that the saved configuration uses is refused (409 with used_by), another file is "
          "removed (200, then 404)",
          forged == {"DELETE, no header": 403, "DELETE, other Origin": 403, "PUT, other Origin": 403,
                     "DELETE, wrong password": 401}
          and used[0] == 409 and uj.get("used_by") == ["mqtt.tls.ca_file"]
          and "missing.pem" in f3 and gone[0] == 200 and again[0] == 404 and "other.key" not in f3,
          f"{forged}; in use {used[0]} {uj}; remove {gone[0]}, again {again[0]}; files {sorted(f3)}")

    # the limit of the number of files: the copies fill the list to 16, the 17th is refused
    _, j4, f4, _ = listing()
    n = j4.get("count", len(f4))
    fill = [put(f"name=fill{i}.pem", ca)[0] for i in range(16 - n)]
    over = put("name=fill-over.pem", ca)
    _, j5, _, _ = listing()
    print(f"  #221 LittleFS with {j5.get('count')} PEM files: {j5.get('fs_used_bytes')} of "
          f"{j5.get('fs_total_bytes')} bytes used", flush=True)
    removed = [http("DELETE", f"/api/files/fill{i}.pem")[0] for i in range(16 - n)]
    check("Issue #221 the limit of the PEM files: up to 16 files, the 17th is refused (507); the "
          "copies are then removed",
          all(c == 201 for c in fill) and over[0] == 507 and "maximum is 16" in
          over[1].get("error", "") and j5.get("count") == 16 and all(c == 200 for c in removed),
          f"{n} file(s), loads {fill}, the 17th {over[0]} {over[1].get('error', '')[:60]!r}; "
          f"removed {removed}")


def make_web_pki(work):
    """#205: a plant CA, a web certificate of the plant for gw22 (DNS:gw22.plant.e2e and
    IP:127.0.0.1) and another key. Returns {name: path}; plant.pem is the chain (leaf + CA)."""
    d = work / "webpki"
    d.mkdir(exist_ok=True)

    def ossl(*a):
        subprocess.run(["openssl", *a], cwd=d, check=True, capture_output=True)

    for k in ("ca", "web", "other"):
        ossl("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", f"{k}.key")
    ossl("req", "-x509", "-new", "-key", "ca.key", "-sha256", "-days", "30", "-subj",
         "/CN=E2E plant CA", "-out", "ca.pem")
    ossl("req", "-new", "-key", "web.key", "-subj", "/CN=gw22.plant.e2e/O=E2E plant", "-out",
         "web.csr")
    (d / "web.ext").write_text("subjectAltName=DNS:gw22.plant.e2e,IP:127.0.0.1\n")
    ossl("x509", "-req", "-in", "web.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
         "-days", "30", "-sha256", "-extfile", "web.ext", "-out", "web.crt")
    (d / "plant.pem").write_bytes((d / "web.crt").read_bytes() + (d / "ca.pem").read_bytes())
    return {n: d / n for n in ("plant.pem", "web.crt", "web.key", "other.key")}


def presented_fp():
    der = PEER_CERT.get("der", b"")
    return ":".join(f"{b:02X}" for b in hashlib.sha256(der).digest()) if der else ""


def check_web_cert(gw, work, node):
    """#205 on GW22: the HTTPS certificate of the web UI. GET /api/web/certificate gives the
    certificate of the first boot (its SHA-256 is the one presented, the SANs contain the
    hostname, never a key). PUT /api/web/certificate examines a certificate of the plant with its
    chain and key (dry run: 200, nothing waits; a key that does not agree, no key: 400); the CSRF
    check covers the new routes. A load waits for the restart; Discard (DELETE .../pending)
    removes it (200, then 404). After a restart, the server presents the certificate of the
    plant; then POST .../new makes a self-signed certificate that, after the next restart, is the
    one presented."""
    pk = make_web_pki(work)
    chain, key, other = (pk[n].read_bytes() for n in ("plant.pem", "web.key", "other.key"))

    def get():
        st, _, body = http("GET", "/api/web/certificate")
        return st, as_json(body) or {}, body

    def put(data, q=""):
        st, _, body = http("PUT", f"/api/web/certificate{q}", data,
                           headers={"Content-Type": "application/x-pem-file"})
        return st, as_json(body) or {}

    st0, g0, b0 = get()
    http("GET", "/api/status")
    fp0 = presented_fp()
    cur = g0.get("current") or {}
    check("Issue #205 GET /api/web/certificate: the certificate of the first boot (self-signed, "
          "its SANs contain the hostname, its SHA-256 is the one presented), nothing waits for the "
          "restart, no key in the answer",
          st0 == 200 and cur.get("source") == "first-boot" and cur.get("self_signed") is True
          and cur.get("sans") == f"DNS:{node.lower()}.local,DNS:{node.lower()}"
          and cur.get("hostname_in_sans") is True and cur.get("sha256") == fp0
          and g0.get("pending") is None and g0.get("https") is True and b"PRIVATE" not in b0,
          f"{st0} {cur}; presented {fp0[:23]}...")

    dry = put(chain + key, "?dry_run=1")
    bad = put(chain + other, "?dry_run=1")
    nokey = put(chain, "?dry_run=1")
    _, g1, _ = get()
    forged = {
        "PUT, no header": http("PUT", "/api/web/certificate", chain + key,
                               headers={"X-Requested-With": None})[0],
        "POST new, other Origin": http("POST", "/api/web/certificate/new", b"",
                                       headers={"Origin": "https://evil.e2e"})[0],
        "DELETE pending, no header": http("DELETE", "/api/web/certificate/pending",
                                          headers={"X-Requested-With": None})[0],
        "PUT, wrong password": http("PUT", "/api/web/certificate?dry_run=1", chain + key,
                                    auth=("admin", "wrong-cert"))[0],
    }
    dc = dry[1].get("certificate") or {}
    check("Issue #205 PUT /api/web/certificate?dry_run=1 examines the certificate of the plant: "
          "with its key 200 (subject, issuer, a chain of 2, SANs with the hostname, nothing "
          "waits); a key that does not agree, or no key: 400; the CSRF check (403) and the "
          "password (401) on the new routes",
          dry[0] == 200 and dc.get("subject") == "CN=gw22.plant.e2e, O=E2E plant"
          and dc.get("issuer") == "CN=E2E plant CA" and dc.get("chain") == 2
          and dc.get("sans") == "DNS:gw22.plant.e2e,IP:127.0.0.1"
          and dc.get("hostname_in_sans") is True and g1.get("pending") is None
          and bad[0] == 400 and "does not agree" in bad[1].get("error", "")
          and nokey[0] == 400
          and forged == {"PUT, no header": 403, "POST new, other Origin": 403,
                         "DELETE pending, no header": 403, "PUT, wrong password": 401},
          f"dry {dry[0]} {dc}; bad key {bad[0]} {bad[1].get('error', '')[:60]!r}; no key "
          f"{nokey[0]} {nokey[1].get('error', '')[:50]!r}; {forged}")

    load = put(chain + key)
    _, g2, _ = get()
    dis = http("DELETE", "/api/web/certificate/pending")[0]
    _, g3, _ = get()
    dis2 = http("DELETE", "/api/web/certificate/pending")[0]
    load2 = put(chain + key)
    check("Issue #205 a loaded certificate waits for the restart (201, GET shows it as pending, "
          "the page keeps its certificate); Discard removes it (200, then 404)",
          load[0] == 201 and (g2.get("pending") or {}).get("subject") == dc.get("subject")
          and (g2.get("current") or {}).get("sha256") == fp0 and dis == 200
          and g3.get("pending") is None and dis2 == 404 and load2[0] == 201,
          f"load {load[0]}; pending {(g2.get('pending') or {}).get('subject')}; discard {dis}, "
          f"again {dis2}; load again {load2[0]}")

    def restart():
        t = time.monotonic()
        rs = http("POST", "/api/restart", b"")[0]
        used = gw.wait_log(r"HTTPS: the new certificate is in use now", 90, t)
        st = wait_http("/api/status", 60)[0] if used else None
        return rs, used, st, t

    rs1, used1, st1, t1 = restart()
    fp1 = presented_fp()
    _, g4, _ = get()
    c4 = g4.get("current") or {}
    printed = gw.find(r"HTTPS certificate SHA-256 fingerprint ([0-9A-F:]{95})", t1)
    check("Issue #205 after a restart the server presents the certificate of the plant (its "
          "SHA-256 on the console and in GET), the source is 'loaded' and nothing waits",
          rs1 == 202 and used1 and st1 == 200 and fp1 and fp1 != fp0 and c4.get("sha256") == fp1
          and c4.get("source") == "loaded" and g4.get("pending") is None and printed
          and fp1 in printed[0],
          f"restart {rs1}; {'switched' if used1 else 'no log line'}; status {st1}; presented "
          f"{fp1[:23]}..., GET {c4.get('sha256', '')[:23]}... {c4.get('source')}")

    new = http("POST", "/api/web/certificate/new", b"")
    nj = (as_json(new[2]) or {}).get("certificate") or {}
    rs2, used2, st2, _ = restart()
    fp2 = presented_fp()
    _, g5, _ = get()
    c5 = g5.get("current") or {}
    check("Issue #205 POST /api/web/certificate/new: a new self-signed certificate for the "
          "hostname waits for the restart (201), and after the restart the server presents it "
          "(source 'new')",
          new[0] == 201 and nj.get("self_signed") is True and rs2 == 202 and used2 and st2 == 200
          and fp2 == nj.get("sha256") and fp2 not in (fp0, fp1) and c5.get("source") == "new"
          and c5.get("sans") == f"DNS:{node.lower()}.local,DNS:{node.lower()}",
          f"new {new[0]} {nj.get('sha256', '')[:23]}...; restart {rs2}; status {st2}; presented "
          f"{fp2[:23]}...; GET {c5.get('source')} {c5.get('sans')}")


# Issue #80: internal RAM without PSRAM (see check_ram_margin for the reasoning)
RAM_MIN_FREE_B = 8 * 1024
RAM_MIN_BLOCK_B = 17 * 1024


def check_ram_margin(args, work, obs, port, ca):
    """Issue #80 in one boot (GW29) on the QEMU path, which has no PSRAM: MQTT over TLS 1.3, the
    full births of the harness configuration (Bulk's 60-tag DBIRTH among them, diagnostics with
    the DataSets), a web session open over HTTPS (keep-alive) and, on a second HTTPS connection,
    PUT /api/config?dry_run=1 of the whole document (the validation holds a second
    configuration, its document and, before them, the parser's tree). Then the shell gives the
    minimum free internal heap since boot, the largest free block left at the peak of the
    validation and the stacks' high-water marks.

    Thresholds, for what must still work at that lowest point (mbedTLS allocates its record
    buffers per record, CONFIG_MBEDTLS_DYNAMIC_BUFFER):
      * minimum free >= RAM_MIN_FREE_B (8 KB): the traffic that can arrive meanwhile, an MQTT
        command (at most CONFIG_GW_CMD_MAX_PAYLOAD, 1 KB, plus its TLS record), the lwIP pbufs of
        a few full segments and the esp-mqtt outbox of a QoS 1 message;
      * largest block at the validation >= RAM_MIN_BLOCK_B (17 KB): one full TLS input record
        (16 KB, CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN) plus its header and MAC, so a large record on
        either session is still received in one piece.
    A new TLS handshake during the validation is check_tls_during_validation (issue #84)."""
    node = "GW29"
    cfg = node_config(node, port=port, tls={"enabled": True, "ca_file": "ca.pem"})
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    t0 = time.monotonic()
    gw = boot_variant(args, work, "ram", cfg, {"ca.pem": ca})
    nb = obs.wait(f"{T}/NBIRTH/{node}", 60)
    bulk = obs.wait(f"{T}/DBIRTH/{node}/{BULK}", 15)
    st0, _, _ = wait_http("/api/status", 40)
    session, kept = None, False
    try:
        session = _GatewayHTTPS("127.0.0.1", HTTPS_PORT, timeout=15)   # issue #116
        auth = {"Authorization": "Basic " + base64.b64encode(f"admin:{WEB_PASSWORD}".encode()).decode()}
        session.request("GET", "/api/status", headers=auth)
        session.getresponse().read()
        st, _, body = http("PUT", "/api/config?dry_run=1", cfg)
        session.request("GET", "/api/status", headers=auth)   # the session is still served
        kept = session.getresponse().status == 200
    except (OSError, httplib.HTTPException) as e:
        st, body = None, str(e).encode()
    finally:
        if session is not None:
            session.close()
    asked, h = gw.shell("status", [r"^heap\s+internal \d+ B free \(minimum \d+ B, largest block \d+ B, "
                                   r"lowest at a validation [1-9]\d* B\)", r"^stack\s+sparkplug \d+ "])
    heap = (gw.find(r"^heap\s+internal", asked) or [""])[0]
    stack = (gw.find(r"^stack\s+sparkplug", asked) or [""])[0]
    nums = [int(x) for x in re.findall(r"(\d+) B", heap)]
    minimum, valid_block = (nums[1], nums[3]) if len(nums) >= 4 else (None, None)
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node}", 5)
    print(f"RAM margin: {heap} | {stack}", flush=True)
    check(f"Issue #80 internal RAM {'with the PSRAM (#85)' if args.psram else 'without PSRAM'} after "
          f"the worst case (MQTT over TLS, full births, an "
          f"HTTPS session open and PUT /api/config?dry_run=1 over HTTPS): minimum free >= "
          f"{RAM_MIN_FREE_B} B and largest block at the validation >= {RAM_MIN_BLOCK_B} B",
          nb and bulk and st0 == 200 and st == 200 and kept and minimum is not None
          and minimum >= RAM_MIN_FREE_B and valid_block >= RAM_MIN_BLOCK_B,
          f"NBIRTH {'yes' if nb else 'no'}, Bulk DBIRTH {'yes' if bulk else 'no'}; PUT dry run {st} "
          f"{body[:80]!r}; session kept {kept}; {heap}; {stack}")
    check_print_block(gw, args, t0)
    check_web_stacks(stack, "after the #80 validation and its HTTPS sessions", redirect=False)


# Issue #110: the two esp_http_server tasks are both named "httpd"; the shell gives each by its
# handle. What must stay free on each at its deepest: the web server (10 KB) runs a TLS
# handshake and a configuration validation; the redirect server (3 KB) only answers 301/308.
HTTPD_STACK_MIN_B = 2048
REDIRECT_STACK_MIN_B = 512


def check_web_stacks(line, when, redirect=True):
    web = re.search(r" httpd (\d+)", line)
    red = re.search(r" httpd_redirect (\d+)", line)
    guard = re.search(r" web_guard (\d+)", line)
    ok = web is not None and int(web.group(1)) >= HTTPD_STACK_MIN_B and (
        not redirect or (red is not None and int(red.group(1)) >= REDIRECT_STACK_MIN_B)) and (
        guard is not None and int(guard.group(1)) >= REDIRECT_STACK_MIN_B)
    check(f"Issue #110 web server task stack (httpd, by its handle) >= {HTTPD_STACK_MIN_B} B free"
          + (f", HTTP redirect task (httpd_redirect) >= {REDIRECT_STACK_MIN_B} B" if redirect else "")
          + f" and its watchdog (web_guard) >= {REDIRECT_STACK_MIN_B} B"
          + f" {when}", ok, line.strip() or "no stack line")


# Issue #91: what must stay contiguous beside the printed document at the validation without
# PSRAM: the largest free block just before the print asks for it, minus the document
PRINT_MIN_SPARE_B = 4 * 1024


def check_print_block(gw, args, since):
    """Issue #91 on the #80 validation: the print of the validated document asks for one block
    of exactly its size (+ NUL), not about twice it (cJSON_Print's doubling buffer and the tab
    conversion), and without PSRAM the largest internal block just before it leaves
    PRINT_MIN_SPARE_B to spare."""
    line = (gw.find(r"validation: document \d+ B, largest request \d+ B", since) or [""])[-1]
    m = re.search(r"document (\d+) B, largest request (\d+) B, largest internal block before it "
                  r"(\d+) B", line)
    doc, req, room = (int(x) for x in m.groups()) if m else (0, 0, 0)
    spare = room - req
    ok = m is not None and doc > 1024 and req == doc + 1
    if not args.psram:   # with PSRAM the document goes there: the internal block does not matter
        ok = ok and spare >= PRINT_MIN_SPARE_B
    print(f"print block: {line.strip()}", flush=True)
    check(f"Issue #91 the validated document is printed into one block of its size"
          f"{'' if args.psram else f' with >= {PRINT_MIN_SPARE_B} B of the largest internal block to spare'}",
          ok, f"document {doc} B, largest request {req} B, largest internal block before it {room} B "
              f"(spare {spare} B; before #91 the print asked for up to twice the tab-indented text "
              f"in one block, about 21 KB for this document)")


VALIDATION_HOLD_MS = 12000


def check_tls_during_validation(args, work, obs, port, ca):
    """Issue #84 in one boot (GW30, no PSRAM): an MQTT over TLS reconnection, with its handshake,
    while a configuration is being validated. The emulator-only shell command 'validation hold'
    keeps the validation's buffers (a second gw_config_t and its document) at their peak for
    VALIDATION_HOLD_MS; meanwhile the harness drops the broker connection (a relay between the
    gateway and the TLS broker), so the gateway reconnects inside the hold. Expected: the PUT
    answers, the handshake either completes inside the hold or fails cleanly and is retried after
    it (NBIRTH again), and nothing hangs, panics or trips the task watchdog. As in #80, a web
    session stays open over HTTPS (keep-alive) through the whole case."""
    node, rport = "GW30", MQTT_PORT + 17
    relay = Relay(rport, port)
    cfg = node_config(node, port=rport, tls={"enabled": True, "ca_file": "ca.pem"})
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    t0 = time.monotonic()
    gw = boot_variant(args, work, "tlsvalid", cfg, {"ca.pem": ca})
    nb = obs.wait(f"{T}/NBIRTH/{node}", 60)
    bulk = obs.wait(f"{T}/DBIRTH/{node}/{BULK}", 15)
    st0, _, _ = wait_http("/api/status", 40)
    # as in #80, a second web session stays open (keep-alive) through the whole case
    auth = {"Authorization": "Basic " + base64.b64encode(f"admin:{WEB_PASSWORD}".encode()).decode()}
    session = _GatewayHTTPS("127.0.0.1", HTTPS_PORT,   # issue #116
                            timeout=VALIDATION_HOLD_MS / 1000 + 30)
    try:
        session.request("GET", "/api/status", headers=auth)
        opened = session.getresponse().read() is not None
    except (OSError, httplib.HTTPException):
        opened = False
    _, h = gw.shell("validation hold %d" % VALIDATION_HOLD_MS,
                    [r"next configuration validation is held for %d ms" % VALIDATION_HOLD_MS])
    armed = all(h.values())
    res = {}

    def put():
        res["r"] = http("PUT", "/api/config?dry_run=1", cfg, timeout=VALIDATION_HOLD_MS / 1000 + 30)

    th = threading.Thread(target=put, daemon=True)
    th.start()
    held = gw.wait_log(r"emulator: validation held for", 20, t0)
    cut = time.monotonic()
    if held:
        relay.stop()
        relay.start()
    nb2 = obs.wait(f"{T}/NBIRTH/{node}", VALIDATION_HOLD_MS / 1000 + 40)
    th.join(VALIDATION_HOLD_MS / 1000 + 35)
    released = gw.wait_log(r"emulator: validation released", 1, cut)
    try:
        session.request("GET", "/api/status", headers=auth)   # the session is still served
        kept = session.getresponse().status == 200
    except (OSError, httplib.HTTPException):
        kept = False
    session.close()
    st = res.get("r", (None, {}, b"no answer"))[0]
    body = res.get("r", (None, {}, b"no answer"))[2]
    tries = gw.find(r"connecting to 10\.0\.2\.2:%d \(TLS\) as 'GW30'" % rport, cut)
    tls_err = gw.find(r"(?i)(esp-tls|mbedtls|TLS: |SSL).*(fail|error|alloc|memory)|"
                      r"MQTT down; reconnect attempt", cut)
    during = nb2 is not None and released is not None and nb2[0] < released
    alive, _ = gw.shell("status", [r"^heap\s+internal"])
    heap = (gw.find(r"^heap\s+internal", alive) or [""])[0]
    rel_line = (gw.find(r"emulator: validation released", cut) or [""])[0]
    bad = gw.find(r"Guru Meditation|task_wdt|Task watchdog|abort\(\) was called|rst:0x(?!1 )", t0)
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node}", 5)
    relay.stop()
    print(f"TLS during validation: PUT {st}; handshake inside the hold {during}; attempts "
          f"{len(tries)}; {' | '.join(tls_err[:6])}; {rel_line}; {heap}", flush=True)
    check(f"Issue #84 an MQTT over TLS reconnection (new handshake) while a configuration is being "
          f"validated, with a second web session open, {'with the PSRAM (#85)' if args.psram else 'no PSRAM'}: "
          f"the PUT answers (200, or 503 with the "
          f"reason), MQTT comes back (NBIRTH), the session is still served, no hang, panic or "
          f"watchdog",
          nb and bulk and st0 == 200 and opened and armed and held and st in (200, 503)
          and nb2 is not None and kept and heap and not bad,
          f"NBIRTH {'yes' if nb else 'no'}; web session open {opened}, kept {kept}; armed {armed}; "
          f"held {held is not None}; PUT {st} "
          f"{body[:90]!r}; NBIRTH again {'yes' if nb2 else 'no'} "
          f"({'inside' if during else 'after'} the hold); {len(tries)} attempt(s); "
          f"{' | '.join(tls_err[:4])}; {rel_line}; {bad[:2]}")


# ---------------------------------------------------------------- broker failover (MQTT-6)

def run_failover(args, work):
    """MQTT-6 in one boot (GW17): the primary broker (mqtt.host, port +8) is down at boot, the
    backup is the harness' main mosquitto; failover_after 2, primary_return_s 10.
      1. two failed attempts on the primary, then the backup: NBIRTH there
      2. the primary comes up: once the session on the backup has been stable (one keepalive)
         for primary_return_s, NDEATH on the backup and NBIRTH on the primary
      3. the primary goes down again: two failed attempts, back to the backup"""
    node, prim = "GW17", MQTT_PORT + 8
    nbirth_t, ndeath_t = f"{T}/NBIRTH/{node}", f"{T}/NDEATH/{node}"
    keepalive = json.loads(config_json())["mqtt"]["keepalive_s"]
    cfg = node_config(node, port=prim, backup_brokers=[{"host": "10.0.2.2", "port": MQTT_PORT}],
                      failover_after=2, primary_return_s=10)
    obs = Observer()
    t0 = time.monotonic()
    gw = boot_variant(args, work, "failover", cfg)
    nb = obs.wait(nbirth_t, 60)
    moved = gw.find(r"broker 1/2 \(10\.0\.2\.2:%d\) failed 2 time\(s\) in a row: next attempt on "
                    r"broker 2/2 \(10\.0\.2\.2:%d\)" % (prim, MQTT_PORT), t0)
    tries = gw.find(r"connecting to 10\.0\.2\.2:%d \(broker 1/2\)" % prim, t0)
    asked, h = gw.shell(["status", "config"],
                        [r"^mqtt\s+broker 2/2 10\.0\.2\.2:%d, 0 failed" % MQTT_PORT,
                         r"^backup 1\s+10\.0\.2\.2:%d" % MQTT_PORT,
                         r"^failover\s+after 2 failed attempt\(s\); back to the primary after 10 s"])
    shown = all(h.values())
    check("MQTT-6 primary broker down at boot: two failed attempts, then the backup (NBIRTH there); "
          "the shell shows the current broker and the list",
          nb and moved and len(tries) == 2 and shown,
          f"{len(tries)} attempts on the primary; {(moved or ['no switch logged'])[0]}; "
          + " | ".join(gw.find(r"^mqtt\s|^backup 1|^failover", asked)))

    pconf = work / "broker-primary.conf"
    pconf.write_text(f"listener {prim} 127.0.0.1\nallow_anonymous true\n")
    pb = start_broker(pconf, work / "mosquitto-primary.log")
    pobs = Observer(prim)
    back = pobs.wait(nbirth_t, 60)
    death = [m for m in CAPTURE if m[1] == ndeath_t and nb and m[0] > nb[0] and m[2].metrics
             and bdseq_of(m[2]) == bdseq_of(nb[2])]
    waited = back[0] - nb[0] if back and nb else None
    check(f"MQTT-6 return to the primary: after a stable session on the backup (keepalive {keepalive} s) "
          "plus primary_return_s (10 s), orderly NDEATH on the backup and NBIRTH on the primary",
          back and death and waited >= keepalive + 10 - 1 and bdseq_of(back[2]) != bdseq_of(nb[2]),
          f"NBIRTH on the primary {waited:.1f} s after the one on the backup; NDEATH on the backup "
          f"with the session's bdSeq: {bool(death)}; "
          f"{(gw.find(r'returning to the primary broker', t0) or ['no log'])[0]}" if back and nb
          else "no NBIRTH on the primary")

    # the session on the primary must be stable first: until then it is the single return try
    stable = gw.wait_log(r"broker 1/2: session stable", keepalive + 5, back[0] if back else 0)
    pobs.close()
    since = time.monotonic()
    pb.terminate()
    pb.wait()
    nb2 = obs.wait(nbirth_t, 60)
    moved = gw.find(r"broker 1/2 .* failed 2 time\(s\) in a row: next attempt on broker 2/2", since)
    gw.kill()
    obs.wait(ndeath_t, 5)
    obs.close()
    check("MQTT-6 the primary fails again: the stable session is lost, two failed attempts, back to "
          "the backup", stable and nb2 and moved,
          f"NBIRTH on the backup {nb2[0] - since:.1f} s after stopping the primary; {moved[0]}"
          if nb2 and moved else f"NBIRTH {bool(nb2)}, switch {bool(moved)}")


# ---------------------------------------------------------------- MQTT 5 (MQTT-8)

def run_mqtt5(args, work, mosquitto_log):
    """MQTT-8 in one boot (GW18) against the harness' main mosquitto: CONNECT with protocol
    level 5 and Clean Start, NBIRTH, the NCMD subscription working (rebirth), an NCMD Reboot whose
    DISCONNECT carries the reason code 0x04 (issue #155: the broker publishes the will too), a
    rebirth whose NBIRTH publish fails with the connection open (issue #164: a new session), and
    the NDEATH will published as soon as the TCP connection dies (Will Delay Interval 0)."""
    node = "GW18"
    nbirth_t, ndeath_t = f"{T}/NBIRTH/{node}", f"{T}/NDEATH/{node}"
    obs = Observer()
    t0 = time.monotonic()
    gw = boot_variant(args, work, "mqtt5", node_config(node, protocol="5"))
    nb = obs.wait(nbirth_t, 40)
    attempt = gw.find(r"connecting to 10\.0\.2\.2:%d MQTT 5 as 'GW18'" % MQTT_PORT, t0)
    v5 = re.search(r"New client connected from \S+ as GW18 \(p5, c1, k10\)",
                   pathlib.Path(mosquitto_log).read_text())
    obs.drain()
    obs.publish_cmd(f"{T}/NCMD/{node}", [{"name": "Node Control/Rebirth", "datatype": 11,
                                          "boolean_value": True}])
    rb = obs.wait(nbirth_t, 5)

    # Issue #155: NCMD Reboot -> explicit NDEATH, then DISCONNECT with the reason code 0x04
    # (Disconnect with Will Message): the broker publishes the will NDEATH too, with the same
    # bdSeq (tck-id-payloads-ndeath-will-message-publisher-disconnect-mqtt50)
    obs.drain()
    rebooted = time.monotonic()
    obs.publish_cmd(f"{T}/NCMD/{node}", [{"name": "Node Control/Reboot", "datatype": 11,
                                          "boolean_value": True}])
    nb2 = obs.wait(nbirth_t, 40)
    deaths = [m for m in CAPTURE if m[1] == ndeath_t and m[0] >= rebooted and nb
              and bdseq_of(m[2]) == bdseq_of(nb[2])]
    explicit = len(re.findall(r"Received PUBLISH from GW18 \(d0, q1, r0, m\d+, 'spBv1\.0/Plant1/NDEATH/GW18'",
                              pathlib.Path(mosquitto_log).read_text()))
    check("Issue #155 MQTT 5: NCMD Reboot publishes the NDEATH, then sends DISCONNECT with the reason "
          "code 0x04 (Disconnect with Will Message): the broker publishes the will NDEATH too, with "
          "the same bdSeq; the gateway restarts (NBIRTH)",
          nb2 and explicit >= 1 and len(deaths) == 2,
          f"NDEATHs with the session's bdSeq: {len(deaths)} (2 expected: the explicit one and the "
          f"will); explicit NDEATH publishes in the broker log: {explicit}; NBIRTH after the reboot "
          f"{'yes' if nb2 else 'no'}")

    # Issue #164: a rebirth whose NBIRTH publish fails one time while the connection stays open
    # (emulator-only fault) must not leave the node connected with no Sparkplug session: NDEATH,
    # DISCONNECT, a new connection with the next bdSeq and every birth
    gw.wait_log(r"Sparkplug session up", 10, rebooted)
    obs.drain()
    failed_at = time.monotonic()
    gw.console("pubfail 1")
    gw.wait_log(r"the next 1 Sparkplug publish\(es\) fail without a send", 5, failed_at)
    gw.console("rebirth")
    nb3 = obs.wait(nbirth_t, 30)
    ended = gw.find(r"rebirth: NBIRTH not published \(bdSeq=\d+\) with the connection open", failed_at)
    nd3 = [m for m in CAPTURE if m[1] == ndeath_t and m[0] >= failed_at and nb2
           and bdseq_of(m[2]) == bdseq_of(nb2[2])]
    check("Issue #164 a rebirth whose NBIRTH publish fails while the connection stays open ends the "
          "session (NDEATH, DISCONNECT); the node connects again: NBIRTH with the next bdSeq",
          ended and nd3 and nb2 and nb3 and bdseq_of(nb3[2]) == (bdseq_of(nb2[2]) + 1) % 256
          and nb3[2].seq == 0,
          f"{(ended or ['no end of the session logged'])[0]}; NDEATHs with the old bdSeq: {len(nd3)}; "
          + (f"NBIRTH bdSeq {bdseq_of(nb3[2])} after bdSeq {bdseq_of(nb2[2])}, "
             f"{nb3[0] - failed_at:.1f} s after the fault" if nb3 and nb2 else "no new NBIRTH"))
    # Issue #186 with MQTT 5: esp-mqtt sent the explicit NDEATH of the old session again (DUP 1)
    # right after the CONNECT of the new one; the explicit NDEATH and the will only (0x04)
    resent = old_session_publishes(mosquitto_log, node)
    check("Issue #186 MQTT 5: the new session does not send the explicit NDEATH of the old session "
          "again (no DUP PUBLISH, no NDEATH before its own NBIRTH; the old bdSeq comes 2 times: the "
          "explicit NDEATH and the will)",
          nb3 and not resent and len(nd3) == 2,
          f"NDEATHs with the old bdSeq: {len(nd3)}; sent again: {' | '.join(resent) or 'none'}")

    killed = time.monotonic()
    gw.kill()
    will = obs.wait(ndeath_t, 10)
    obs.close()
    check("MQTT-8 MQTT 5.0 (mqtt.protocol \"5\"): CONNECT protocol level 5 with Clean Start, NBIRTH, "
          "NCMD rebirth received over the v5 subscription, NDEATH will with the session's bdSeq "
          "right after the connection dies (Will Delay 0)",
          nb and attempt and v5 and rb and nb3 and will and bdseq_of(will[2]) == bdseq_of(nb3[2])
          and will[0] - killed < 5,
          f"NBIRTH {'yes' if nb else 'no'}; mosquitto: {v5.group(0) if v5 else 'no p5 connection'}; "
          f"rebirth {'yes' if rb else 'no'}; will "
          f"{f'{will[0] - killed:.1f} s after the kill, bdSeq {bdseq_of(will[2])}' if will else 'missing'}")


# Issue #73: the Maximum Packet Size of the test broker. Between the NBIRTH of GW26 (2065 B
# with the node metrics of #132 and #173; it was below 2000 B before) and the Meter1 DBIRTH (about
# 560 B) on one side, and the Bulk DBIRTH (about 3.8 KB) on the other, with a margin of more than
# 500 B to each. A failed NBIRTH ends the session (#164), so the NBIRTH must fit.
MAXPKT_B = 2600


def run_max_packet(args, work, slave):
    """Issue #73 (GW26, MQTT 5) against a mosquitto with max_packet_size MAXPKT_B (2600), which
    it announces in the CONNACK: NBIRTH and the small DBIRTH of Meter1 go out; the DBIRTH of Bulk
    (about 3.8 KB) is not sent, with a clear error and the shell showing it, and the session
    stays up (no reconnection loop)."""
    node, port = "GW26", MQTT_PORT + 14
    conf = work / "broker-maxpkt.conf"
    conf.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\nmax_packet_size {MAXPKT_B}\n")
    broker = start_broker(conf, work / "mosquitto-maxpkt.log")
    obs = Observer(port)
    t0 = time.monotonic()
    cfg = node_config(node, port=port, protocol="5")
    gw = boot_variant(args, work, "maxpkt", cfg)
    # Issue #78: an operator starts 'config check' as soon as the shell is there and pastes
    # half of a document; the boot must go on (the shell context lock is not held while the
    # paste is read): NBIRTH on time, then the rest of the paste is validated
    ready = gw.wait_log(r"shell: console ready", 30, t0)
    gw.console("config check")
    gw.wait_log(r"^paste the JSON document", 5, ready or t0)
    text = (json.dumps(cfg, indent=2) + "\n").splitlines(keepends=True)
    half = len(text) // 2
    gw.sock.sendall("".join(text[:half]).encode())
    pasting = time.monotonic()
    nb = obs.wait(f"{T}/NBIRTH/{node}", 40)
    nb_during = nb and not gw.find(r"^config: ", t0)
    gw.sock.sendall("".join(text[half:]).encode())
    valid = gw.wait_log(r"^config: valid \(\d+ bytes\), nothing saved$", 15, pasting)
    check("Issue #78 'config check' half pasted during the boot does not hold the boot back: "
          "NBIRTH on time while the paste waits, then the document is validated",
          ready and nb_during and nb[0] - t0 < 15 and valid,
          (f"paste started {pasting - t0:.1f} s after QEMU started, NBIRTH at {nb[0] - t0:.1f} s"
           if nb else "no NBIRTH") + "; " + (" | ".join(gw.find(r"^config: ", t0)) or "no answer"))
    db = obs.wait(f"{T}/DBIRTH/{node}/{DEVICE}", 10)
    err = gw.wait_log(r"spBv1\.0/Plant1/DBIRTH/GW26/Bulk: \d+ bytes, more than the broker's Maximum "
                      rf"Packet Size of {MAXPKT_B} \(by \d+\): not published", 15, t0)
    time.sleep(8)
    # Issue #76: Bulk is polled (and reported to the Sparkplug task) every cycle, but its DBIRTH
    # is tried once per session, not rebuilt and rejected each time: the count stays at 1; a
    # rebirth tries it once more
    asked, h = gw.shell("status", [rf"^limit\s+broker Maximum Packet Size {MAXPKT_B} B; 1 message\(s\) too "
                                   r"large, not published \(last: spBv1\.0/Plant1/DBIRTH/GW26/Bulk\)"])
    shown = list(h.values())[0]
    bulk_cycles = len([r for t, r in zip(list(slave.times), list(slave.reads), strict=False) if r[0] == 2 and t >= t0])
    gw.console("rebirth")
    gw.wait_log(r"^rebirth: NBIRTH and every DBIRTH requested", 5, asked)
    time.sleep(3)
    _, h2 = gw.shell("status", [rf"^limit\s+broker Maximum Packet Size {MAXPKT_B} B; 2 message\(s\) too large"])
    once = list(h2.values())[0]
    check("Issue #76 a DBIRTH larger than the broker's limit is not rebuilt on every change of its "
          "device: one attempt per session, one more per rebirth",
          shown and once and bulk_cycles > 3,
          f"{bulk_cycles} Bulk poll cycles; " + " | ".join(gw.find(r"^limit\s", asked)))
    bulk = [m for m in CAPTURE if m[1] == f"{T}/DBIRTH/{node}/{BULK}"]
    attempts = gw.find(r"connecting to .* as 'GW26'", t0)
    dropped = re.search(r"Client GW26 .*(disconnected|exceeded)", (work / "mosquitto-maxpkt.log").read_text())
    gw.kill()
    obs.close()
    broker.terminate()
    broker.wait()
    check("Issue #73 MQTT 5 Maximum Packet Size from the CONNACK: a DBIRTH larger than it is not "
          "published (clear error, shown in the shell); the rest goes out and the session stays",
          nb and db and err and shown and not bulk and len(attempts) == 1 and not dropped,
          f"NBIRTH {'yes' if nb else 'no'}, Meter1 DBIRTH {'yes' if db else 'no'}, Bulk DBIRTH "
          f"{len(bulk)}; {(gw.find(rf'Maximum Packet Size of {MAXPKT_B}', t0) or ['no error'])[0][:150]}; "
          f"{len(attempts)} connection(s); " + " | ".join(gw.find(r"^limit\s", asked)))


# ---------------------------------------------------------------- primary host (SPB-13)

def host_state(obs, host, online, ts, retain=True):
    """Publish a Sparkplug 3.0 STATE message for @host as its primary host application would."""
    obs.c.publish(f"spBv1.0/STATE/{host}", json.dumps({"online": online, "timestamp": ts}),
                  qos=1, retain=retain).wait_for_publish(5)


def old_session_publishes(log, client):
    """Issue #186: the PUBLISH packets that @client sent again from an earlier MQTT session, from
    a verbose mosquitto log (`-v`). Every connection of the gateway is a clean session, so each
    PUBLISH with DUP 1 is one of them; and an explicit NDEATH comes only after the NBIRTH of its
    own connection, so an NDEATH before it has the bdSeq of an earlier session
    (tck-id-operational-behavior-primary-application-state-with-multiple-servers-walk)."""
    bad, born = [], False
    c = re.escape(client)
    for line in pathlib.Path(log).read_text(errors="replace").splitlines():
        if re.search(rf"New client connected from \S+ as {c} \(", line):
            born = False
            continue
        m = re.search(rf"Received PUBLISH from {c} \(d(\d), q\d, r\d, m\d+, 'spBv1\.0/[^/']+/(\w+)/",
                      line)
        if not m:
            continue
        if m.group(1) == "1" or (m.group(2) == "NDEATH" and not born):
            bad.append(line.strip()[:160])
        born = born or m.group(2) == "NBIRTH"
    return bad


def run_primary_host(args, work):
    """SPB-13 in one boot (GW19, primary_host_id SCADA1) against the main mosquitto, with the
    harness playing the host's STATE messages (retained, JSON, Sparkplug 3.0):
      1. STATE offline retained before the boot: the node connects and subscribes, no NBIRTH
      2. STATE online: NBIRTH
      3. a stale STATE offline (older timestamp) is ignored: no NDEATH
      4. STATE offline: NDEATH (published by the node, session bdSeq) and disconnect; the new
         session waits (retained offline): no NBIRTH
      5. STATE online again: NBIRTH with a new bdSeq"""
    node, host = "GW19", "SCADA1"
    nbirth_t, ndeath_t = f"{T}/NBIRTH/{node}", f"{T}/NDEATH/{node}"
    obs = Observer()
    ts = int(time.time() * 1000)
    host_state(obs, host, False, ts)
    t0 = time.monotonic()
    cfg = node_config(node)
    cfg["sparkplug"]["primary_host_id"] = host
    gw = boot_variant(args, work, "phost", cfg)
    waiting = gw.wait_log(r"waiting for the primary host 'SCADA1' \(STATE online\) before NBIRTH", 40, t0)
    early = obs.wait(nbirth_t, 5)
    asked, h = gw.shell("status", [r"^host\s+primary 'SCADA1' offline, NBIRTH waits for it"])
    shown = list(h.values())[0]
    check("SPB-13 STATE offline (retained) at connection: subscribed, no NBIRTH; the shell says why",
          waiting and not early and shown,
          f"{(gw.find(r'waiting for the primary host', t0) or ['no wait logged'])[0]}; NBIRTH "
          f"{'published' if early else 'not published'} 5 s later; "
          + " | ".join(gw.find(r"^host\s", asked)))

    sent = time.monotonic()
    host_state(obs, host, True, ts + 1000)
    nb = obs.wait(nbirth_t, 10)
    check("SPB-13 STATE online -> NBIRTH (and the DBIRTHs)", nb and nb[2].seq == 0,
          f"NBIRTH {nb[0] - sent:.2f} s after STATE online" if nb else "no NBIRTH")

    # #223: ota.mqtt is off by default: the NBIRTH has no Node Control/Firmware Update and no
    # Node Control/Firmware SHA-256 (tck-id-topics-nbirth-metric-reqs), and an NCMD with them
    # starts no update (tck-id-operational-behavior-data-commands-ncmd-metric-name); the log
    # gives the cause
    since = time.monotonic()
    obs.publish_cmd(f"{T}/NCMD/{node}", [
        {"name": "Node Control/Firmware SHA-256", "datatype": 12, "string_value": "a" * 64},
        {"name": "Node Control/Firmware Update", "datatype": 12,
         "string_value": "https://10.0.2.2:1/gw.bin"}])
    obs.publish_cmd(f"{T}/NCMD/{node}", [{"alias": 40, "datatype": 12,
                                          "string_value": "https://10.0.2.2:1/gw.bin"}])
    off = gw.wait_log(r"NCMD: metric '' \(alias 40\) discarded: firmware updates from MQTT are "
                      r"off \(ota\.mqtt\); no update started", 10, since)
    named = gw.find(r"NCMD: metric 'Node Control/Firmware (Update|SHA-256)'.* discarded: firmware "
                    r"updates from MQTT are off \(ota\.mqtt\)", since)
    time.sleep(1)
    started = gw.find(r"fw: downloading|fw: firmware update (refused|failed)|NCMD Firmware Update "
                      r"from", since)
    check("Issue #223 without ota.mqtt (the default) the NBIRTH has no Node Control/Firmware "
          "Update and no Node Control/Firmware SHA-256, and an NCMD with them (by name or by "
          "alias) starts no update; the log gives the cause",
          nb and metric(nb[2], name="Node Control/Firmware Update") is None
          and metric(nb[2], alias=40) is None
          and metric(nb[2], name="Node Control/Firmware SHA-256") is None
          and fw_state(nb[2]) is not None and off and len(named) == 2 and not started,
          f"NBIRTH metrics {[m.name for m in nb[2].metrics if 'irmware' in m.name] if nb else None}; "
          f"refused {len(named)} by name, by alias {off is not None}; started {started[:1]}")

    # #144 item 6, #223: Node Control/Boot Other Slot needs ota.mqtt too: no metric in the NBIRTH
    # (tck-id-topics-nbirth-metric-reqs), and an NCMD with it, by name or by alias, does not boot
    # the other slot (tck-id-operational-behavior-data-commands-ncmd-metric-name)
    since = time.monotonic()
    obs.publish_cmd(f"{T}/NCMD/{node}", [{"name": "Node Control/Boot Other Slot", "datatype": 11,
                                          "boolean_value": True}])
    obs.publish_cmd(f"{T}/NCMD/{node}", [{"alias": 43, "datatype": 11, "boolean_value": True}])
    boot_off = gw.wait_log(r"NCMD: metric '' \(alias 43\) discarded: firmware updates from MQTT "
                           r"are off \(ota\.mqtt\); no boot of the other slot", 10, since)
    boot_named = gw.find(r"NCMD: metric 'Node Control/Boot Other Slot' discarded: firmware "
                         r"updates from MQTT are off \(ota\.mqtt\); no boot of the other slot",
                         since)
    time.sleep(1)
    booted = gw.find(r"NCMD Boot Other Slot|booting the other slot", since)
    check("Issue #144 item 6 without ota.mqtt (#223) the NBIRTH has no Node Control/Boot Other "
          "Slot, and an NCMD with it (by name or by alias) does not boot the other slot; the log "
          "gives the cause",
          nb and metric(nb[2], name="Node Control/Boot Other Slot") is None
          and metric(nb[2], alias=43) is None and boot_off and len(boot_named) == 1 and not booted,
          f"NBIRTH has it: {metric(nb[2], name='Node Control/Boot Other Slot') is not None if nb else None}; "
          f"refused {len(boot_named)} by name, by alias {boot_off is not None}; "
          f"boot {booted[:1]}")

    since = time.monotonic()
    host_state(obs, host, False, ts + 500, retain=False)
    stale = obs.wait(ndeath_t, 3)
    ignored = gw.find(r"STATE of 'SCADA1' ignored: older than the last one", since)
    check("SPB-13 a STATE offline older than the last one accepted is ignored (no NDEATH)",
          not stale and ignored, (ignored or ["no 'ignored' log"])[0])

    since = time.monotonic()
    host_state(obs, host, False, ts + 2000)
    nd = obs.wait(ndeath_t, 10)
    explicit = gw.wait_log(r"the primary host went offline: NDEATH published", 5, since)
    rewait = gw.wait_log(r"waiting for the primary host 'SCADA1'", 20, since)
    nb2 = obs.wait(nbirth_t, 3)
    check("SPB-13 STATE offline -> NDEATH (session bdSeq) and disconnect; the next session waits "
          "for the host (no NBIRTH)",
          nd and nb and bdseq_of(nd[2]) == bdseq_of(nb[2]) and explicit and rewait and not nb2,
          f"NDEATH bdSeq {bdseq_of(nd[2]) if nd else None} (session {bdseq_of(nb[2]) if nb else None}); "
          f"{(gw.find(r'the primary host went offline', since) or ['no log'])[0]}; waiting again: "
          f"{bool(rewait)}")

    sent = time.monotonic()
    host_state(obs, host, True, ts + 3000)
    nb3 = obs.wait(nbirth_t, 10)
    check("SPB-13 the host back online -> NBIRTH with a new bdSeq",
          nb3 and nb and bdseq_of(nb3[2]) != bdseq_of(nb[2]),
          f"NBIRTH bdSeq {bdseq_of(nb3[2])} after {nb3[0] - sent:.2f} s" if nb3 else "no NBIRTH")
    resent = old_session_publishes(work / "mosquitto.log", node)
    check("Issue #186 the session after the STATE offline does not send the explicit NDEATH of the "
          "old session again (no DUP PUBLISH, no NDEATH before its own NBIRTH)",
          nb3 and not resent, " | ".join(resent) or "nothing sent again")
    gw.kill()
    obs.wait(ndeath_t, 5)
    obs.c.publish(f"spBv1.0/STATE/{host}", b"", qos=1, retain=True).wait_for_publish(5)
    obs.close()


def run_primary_host_brokers(args, work):
    """Issues #156 and #157 in one boot (GW48, primary_host_id SCADA2, failover_after 3): broker
    1/2 is the main mosquitto, broker 2/2 a second mosquitto (port +28). The host connected to
    broker 2 first, so its STATE timestamp there is older than on broker 1 (a host keeps one
    timestamp for each MQTT server).
      1. STATE online on broker 1: NBIRTH there
      2. a valid STATE offline on broker 1: NDEATH, and the next attempt goes to broker 2 at
         once, not after failover_after attempts on broker 1
         (tck-id-operational-behavior-edge-node-termination-host-offline-reconnect)
      3. the older STATE online of broker 2 is valid there: NBIRTH on broker 2
         (tck-id-operational-behavior-host-application-multi-server-timestamp)"""
    node, host, port = "GW48", "SCADA2", MQTT_PORT + 28
    nbirth_t, ndeath_t = f"{T}/NBIRTH/{node}", f"{T}/NDEATH/{node}"
    conf = work / "broker-phost2.conf"
    conf.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\n")
    b2 = start_broker(conf, work / "mosquitto-phost2.log")
    obs, obs2 = Observer(), Observer(port)
    ts = int(time.time() * 1000)
    host_state(obs2, host, True, ts)          # broker 2: the host connected there first
    host_state(obs, host, True, ts + 5000)    # broker 1: later
    t0 = time.monotonic()
    cfg = node_config(node, backup_brokers=[{"host": "10.0.2.2", "port": port}], failover_after=3)
    cfg["sparkplug"]["primary_host_id"] = host
    gw = boot_variant(args, work, "phost2", cfg)
    nb = obs.wait(nbirth_t, 40)
    first = gw.find(r"connecting to 10\.0\.2\.2:%d \(broker 1/2\)" % MQTT_PORT, t0)
    check("SPB-13 with backup brokers: STATE online on broker 1/2 -> NBIRTH there",
          nb and len(first) == 1, f"NBIRTH {'yes' if nb else 'no'}; {len(first)} attempt(s) on broker 1")

    since = time.monotonic()
    host_state(obs, host, False, ts + 6000)
    nd = obs.wait(ndeath_t, 10)
    nb2 = obs2.wait(nbirth_t, 30)
    moved = gw.find(r"primary host offline on broker: next attempt on broker 2/2 \(10\.0\.2\.2:%d\)"
                    % port, since)
    again = gw.find(r"connecting to 10\.0\.2\.2:%d \(broker 1/2\)" % MQTT_PORT, since)
    to_b2 = gw.find(r"connecting to 10\.0\.2\.2:%d \(broker 2/2\)" % port, since)
    check("Issue #156 a valid STATE offline on broker 1/2: NDEATH, then the next attempt goes to "
          "broker 2/2 at once (not after failover_after 3 attempts on broker 1/2)",
          nd and nb and bdseq_of(nd[2]) == bdseq_of(nb[2]) and moved and not again
          and len(to_b2) == 1,
          f"NDEATH {'yes' if nd else 'no'}; {(moved or ['no switch logged'])[0]}; "
          f"{len(again)} attempt(s) on broker 1/2 and {len(to_b2)} on broker 2/2 after the STATE")
    stale = gw.find(r"STATE of 'SCADA2' ignored: older than the last one", since)
    check("Issue #157 the STATE online of broker 2/2, older than the last one of broker 1/2, is "
          "valid on broker 2/2 (one timestamp for each broker): NBIRTH there",
          nb2 and nb2[2].seq == 0 and not stale,
          (f"NBIRTH on broker 2/2 {nb2[0] - since:.1f} s after the STATE offline"
           if nb2 else "no NBIRTH on broker 2/2") + f"; {(stale or ['no STATE ignored'])[0]}")
    # Issue #186: esp-mqtt sent the explicit QoS 1 NDEATH of broker 1/2 again (DUP 1, the old
    # bdSeq) as the first PUBLISH of the session on broker 2/2. The two observers see the two
    # brokers: the old bdSeq comes one time only (the explicit NDEATH on broker 1/2)
    old = [m for m in CAPTURE if m[1] == ndeath_t and m[0] >= since and nb
           and bdseq_of(m[2]) == bdseq_of(nb[2])]
    resent = (old_session_publishes(work / "mosquitto.log", node)
              + old_session_publishes(work / "mosquitto-phost2.log", node))
    fresh = gw.find(r"new MQTT client, nothing of the old session goes out again", since)
    check("Issue #186 nothing of the old MQTT session goes out after the new CONNECT: after the "
          "STATE offline on broker 1/2, no NDEATH with the old bdSeq on broker 2/2 and no PUBLISH "
          "sent again (DUP) on either broker "
          "(tck-id-operational-behavior-primary-application-state-with-multiple-servers-walk)",
          nd and nb2 and len(old) == 1 and not resent,
          f"NDEATHs with the old bdSeq {bdseq_of(nb[2]) if nb else None} on the two brokers: "
          f"{len(old)} (1 expected); sent again: {' | '.join(resent) or 'none'}; "
          f"{(fresh or ['outbox empty at the reconnection'])[0]}")
    gw.kill()
    obs2.wait(ndeath_t, 5)
    for o in (obs, obs2):
        o.c.publish(f"spBv1.0/STATE/{host}", b"", qos=1, retain=True).wait_for_publish(5)
        o.close()
    b2.terminate()
    b2.wait()


# ---------------------------------------------------------------- store & forward (SPB-14)

def run_store_forward(args, work, slave):
    """SPB-14 in one boot (GW20, sparkplug.store_forward) connected through a Relay (port +9) to
    the main mosquitto; stopping the relay takes the broker away from the gateway while the
    observer stays subscribed. While it is away Voltage takes three values (each beyond the 0.1
    deadband) and DI/3 goes on and off. Back: NBIRTH, DBIRTH with the current value, then the
    stored changes as DDATA/NDATA with is_historical, in order and with their sample times.
    Without PSRAM (the default QEMU build) the small internal-RAM ring is used; with --psram
    (issue #85) the ring in PSRAM (1000 samples in this build, sdkconfig.qemu-psram; the board:
    40000)."""
    node, port = "GW20", MQTT_PORT + 9
    where = "PSRAM" if args.psram else "internal RAM"
    ddata_t, ndata_t = f"{T}/DDATA/{node}/{DEVICE}", f"{T}/NDATA/{node}"
    relay = Relay(port, MQTT_PORT)
    obs = Observer()
    cfg = node_config(node, port=port)
    cfg["sparkplug"]["store_forward"] = True
    t0 = time.monotonic()
    gw = boot_variant(args, work, "sf", cfg)
    nb = obs.wait(f"{T}/NBIRTH/{node}", 40)
    obs.wait(f"{T}/DBIRTH/{node}/{BULK}", 10)
    ring = gw.find(r"store & forward: \d+ samples \(\d+ B\) in "
                   + (r"PSRAM" if args.psram else r"internal RAM \(no PSRAM\)"), t0)
    since = time.monotonic()
    relay.stop()
    gw.wait_log(r"MQTT down; reconnect attempt", 10, since)
    values = [240.0, 241.0, 242.0]
    for v in values:
        slave.set_float(4, 0, v)
        # every value is read at least once: two reads of the Voltage registers after the change
        # (poll_ms 1000; issue #107: with single-threaded TCG under host load a cycle can take
        # longer than the 1.6 s this used to wait)
        n0, end = len(slave.reads), time.monotonic() + 15
        while wait_left(end) > 0 and sum(
                1 for r in list(slave.reads)[n0:] if r[0] == 1 and r[1] == 4
                and r[2] is not None and r[2] <= 0 < r[2] + (r[3] or 1)) < 2:
            time.sleep(0.1)
    gw.console("di 3 1")
    time.sleep(0.5)
    gw.console("di 3 0")
    time.sleep(0.5)
    asked, h = gw.shell("status", [r"^store\s+[1-9]\d* of \d+ samples waiting \(%s\)" % where])
    waiting = list(h.values())[0]
    relay.start()
    nb2 = obs.wait(f"{T}/NBIRTH/{node}", 70)
    time.sleep(3)
    after = [m for m in CAPTURE if nb2 and m[0] >= nb2[0] and m[1].split("/")[3:4] == [node]]
    db = next((m for m in after if m[1] == f"{T}/DBIRTH/{node}/{DEVICE}"), None)
    hist = [(m[2].seq, mm) for m in after if m[1] == ddata_t for mm in m[2].metrics
            if mm.is_historical and mm.alias == ALIAS["Voltage"]]
    di = [mm for m in after if m[1] == ndata_t for mm in m[2].metrics
          if mm.is_historical and mm.alias == ALIAS["DI/1"] + 2]
    got = [round(mm.float_value, 1) for _, mm in hist]
    ts = [mm.timestamp for _, mm in hist]
    in_order = db and all(m[0] >= db[0] for m in after if m[1] in (ddata_t, ndata_t))
    asked2, h = gw.shell("status", [r"^store\s+0 of \d+ samples waiting .*, [1-9]\d* historical messages sent"])
    sent = list(h.values())[0]
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node}", 5)
    obs.close()
    relay.stop()
    slave.set_float(4, 0, 230.5)   # what the main scenario expects
    check("SPB-14 store & forward: changes while the broker is away come after NBIRTH and DBIRTH as "
          "DDATA/NDATA with is_historical, oldest first, with their sample times ("
          + ("ring in PSRAM)" if args.psram else "internal-RAM ring without PSRAM)"),
          nb and ring and waiting and nb2 and db and got == values and ts == sorted(ts)
          and [m.boolean_value for m in di] == [True, False] and in_order and sent,
          f"{(ring or ['no ring log'])[0]}; DBIRTH Voltage "
          f"{round(metric(db[2], name='Voltage').float_value, 1) if db else None}; historical Voltage "
          f"{got} (seq {[s for s, _ in hist]}); historical DI/3 {[m.boolean_value for m in di]}; "
          + " | ".join(gw.find(r"^store\s", asked) + gw.find(r"^store\s", asked2)))


STACK_MARGIN_B = 1024   # see run_stack_margin


def run_stack_margin(args, work, slave):
    """SPB-16..19 and the 6 KB Sparkplug stack (review of 2ea6ffd), in one boot (GW27) with
    store & forward, diagnostics every 10 s, two template devices (Pump) and a bytes tag. Through
    a Relay (port +16) it runs the deepest paths of the Sparkplug task: a DCMD on a template
    member (members decoded one level deep), a DCMD with Bytes, a diagnostics NDATA with the
    Modbus DataSet, and, after the broker was away, the replay of a template instance and raw
    bytes as historical data. Then the shell reports the task's lowest free stack since boot.

    Margin: at least STACK_MARGIN_B (1 KB) free. Interrupts run on their own stack on Xtensa,
    so the task stack only has to absorb paths the scenario did not hit at their deepest: a log
    line formatted on top of the deepest frame (esp_log and the syslog hook, a few hundred bytes)
    and a longer template or DataSet than these. The TLS write path (mbedTLS in this task) is
    measured separately by the MQTT-4 boot after the births (log_stacks) and is not nested in a
    decode: commands are decoded first and their confirmations published afterwards."""
    node, port = "GW27", MQTT_PORT + 16
    relay = Relay(port, MQTT_PORT)
    obs = Observer()
    cfg = node_config(node, port=port)
    cfg["sparkplug"]["store_forward"] = True
    cfg["sparkplug"]["diagnostics_s"] = 10
    # #213: the two pumps of the type Pump: the same addresses, on unit 1 and unit 7
    cfg["device_types"] = [{"name": "Pump", "tags": [
        {"name": "Speed", "area": "holding", "address": 232, "type": "uint16", "units": "rpm",
         "eng_high": 3000, "writable": True},
        {"name": "Run", "area": "coil", "address": 7, "type": "bool"}]}]
    for k, (uid, unit) in enumerate(((1, slave.ctx), (7, slave.unit7))):
        cfg["devices"].append({"name": f"Pump{'AB'[k]}", "unit_id": uid, "type": "Pump",
                               "poll_ms": 500})
        unit.setValues(3, 232, [1000 + k])
    cfg["devices"].append({"name": "Raw", "unit_id": 1, "poll_ms": 500, "tags": [
        {"name": "Block", "area": "holding", "address": 240, "type": "bytes", "length": 4,
         "writable": True}]})
    slave.ctx.setValues(3, 240, [0x0001, 0x0203, 0x0405, 0x0607])
    gw = boot_variant(args, work, "stack", cfg)
    nb = obs.wait(f"{T}/NBIRTH/{node}", 40)
    for d in ("PumpA", "Raw"):
        if not any(m[1] == f"{T}/DBIRTH/{node}/{d}" for m in CAPTURE):
            obs.wait(f"{T}/DBIRTH/{node}/{d}", 10)
    # DCMD on a template member
    p = spb.Payload()
    p.timestamp = int(time.time() * 1000)
    m = p.metrics.add()
    m.name, m.datatype = "Pump", 19
    m.template_value.template_ref = "Pump"
    mm = m.template_value.metrics.add()
    mm.name, mm.datatype, mm.int_value = "Speed", 6, 1500
    obs.drain()
    obs.c.publish(f"{T}/DCMD/{node}/PumpA", p.SerializeToString(), qos=0)
    tmpl_dd = obs.wait(f"{T}/DDATA/{node}/PumpA", 5)
    # DCMD with Bytes
    obs.publish_cmd(f"{T}/DCMD/{node}/Raw", [{"name": "Block", "datatype": 17,
                                             "bytes_value": bytes(range(0x10, 0x18))}])
    raw_dd = obs.wait(f"{T}/DDATA/{node}/Raw", 5)
    # diagnostics NDATA with the DataSet (the counters move every cycle)
    diag = obs.wait(f"{T}/NDATA/{node}", 13, lambda q: metric(q, alias=54) is not None)
    # store & forward of a template instance and raw bytes
    relay.stop()
    gw.wait_log(r"MQTT down; reconnect attempt", 10, time.monotonic() - 1)
    slave.unit7.setValues(3, 232, [1777])   # PumpB
    slave.ctx.setValues(3, 240, [0x00FF, 0x00FE, 0x00FD, 0x00FC])
    time.sleep(2.5)
    relay.start()
    nb2 = obs.wait(f"{T}/NBIRTH/{node}", 70)
    time.sleep(3)
    after = [x for x in CAPTURE if nb2 and x[0] >= nb2[0] and x[1].split("/")[3:4] == [node]]
    hist_pump = [x for x in after if x[1] == f"{T}/DDATA/{node}/PumpB" and x[2].metrics
                 and x[2].metrics[0].is_historical and datatype_of(x[1], x[2].metrics[0]) == 19]
    hist_raw = [x for x in after if x[1] == f"{T}/DDATA/{node}/Raw"
                and any(y.is_historical and datatype_of(x[1], y) == 17 for y in x[2].metrics)]
    asked, h = gw.shell("status", [r"^stack\s+sparkplug \d+ "])
    line = (gw.find(r"^stack\s+sparkplug \d+ ", asked) or [""])[0]
    free = int(re.search(r"sparkplug (\d+)", line).group(1)) if line else None
    print(f"stack after the deep paths: {line}", flush=True)
    gw.kill()
    relay.stop()
    obs.wait(f"{T}/NDEATH/{node}", 5)
    obs.close()
    paths = {"template DCMD": bool(tmpl_dd), "bytes DCMD": bool(raw_dd), "DataSet NDATA": bool(diag),
             "replayed instance": bool(hist_pump), "replayed bytes": bool(hist_raw)}
    check(f"Review of 2ea6ffd: the Sparkplug task keeps >= {STACK_MARGIN_B} B of its 6 KB stack free "
          f"after a template-member DCMD, a Bytes DCMD, a diagnostics DataSet NDATA and the "
          f"store & forward replay of a template instance and raw bytes",
          nb and all(paths.values()) and free is not None and free >= STACK_MARGIN_B,
          f"paths run {paths}; {line or 'no stack line'}")


# ---------------------------------------------------------------- slow link (issue #59)

def run_slow_link(args, work, budget_ms):
    """Issue #59 in one boot (GW21) through a Relay (port +10 -> the main mosquitto) that reads
    the gateway's data at 700 B/s. The harness asks for rebirths until slirp's and the sockets'
    buffers are full and a publish has to wait for the slow reader: it must be cut at the
    publish budget (CONFIG_GW_MQTT_PUBLISH_BUDGET_MS, lowered in the QEMU build so payloads of at
    most 6 KB exceed it) with the connection closed, instead of running on past the task
    watchdog. Then the link is fast again and the gateway comes back with a new session."""
    node, port = "GW21", MQTT_PORT + 10
    relay = Relay(port, MQTT_PORT, 700)
    obs = Observer()
    t0 = time.monotonic()
    # keepalive 180 s: with 10 s the PINGREQ, queued behind the slow data, ends the session
    # before the buffers (about 1 MB between slirp and the host sockets) fill; a slow link fails
    # that way too, but here the bound of a single publish is what is tested
    gw = boot_variant(args, work, "slow", node_config(node, port=port, keepalive_s=180))
    nb = obs.wait(f"{T}/NBIRTH/{node}", 60)
    cut = None
    end = time.monotonic() + 170
    while wait_left(end) > 0:
        obs.publish_cmd(f"{T}/NCMD/{node}", [{"name": "Node Control/Rebirth", "datatype": 11,
                                              "boolean_value": True}])
        cut = gw.wait_log(r"publish of \d+ bytes to \S+ still running after \d+ ms", 0.05, t0)
        if cut or gw.find(r"No PING_RESP", t0):
            break
    aborted = gw.wait_log(r"aborted: returned after \d+ ms", 10, cut or t0)
    lines = gw.find(r"publish of \d+ bytes to \S+ (still running|aborted)", t0)
    took = [int(x) for x in re.findall(r"returned after (\d+) ms", " ".join(lines))]
    down = gw.wait_log(r"MQTT down; reconnect attempt", 10, cut or t0)
    relay.rate = None
    back = obs.wait(f"{T}/NBIRTH/{node}", 90,
                    lambda p: nb is None or bdseq_of(p) != bdseq_of(nb[2]))
    panics = gw.find(r"Guru Meditation|task_wdt|abort\(\)", t0)
    gw.kill()
    relay.stop()
    obs.wait(f"{T}/NDEATH/{node}", 5)
    obs.close()
    check(f"Issue #59 a publish on a slow link (700 B/s once the buffers are full) is cut at the "
          f"{budget_ms} ms budget: connection closed, no watchdog, a new session once the link is "
          f"fast again",
          nb and cut and aborted and took and max(took) < budget_ms + 6000 and down and back
          and not panics,
          f"{' | '.join(lines[:2]) or 'no publish cut'}; {relay.up_bytes} B through the relay; "
          f"reconnect {'logged' if down else 'missing'}; new NBIRTH {'yes' if back else 'no'}; "
          f"panics {panics[:1]}")


# Issue #124: extent files beyond the ring on the "card" at the boot of GW23 (the ring of this
# configuration has the 32 extents r0000.sf to r001f.sf; a gateway has such files after a decrease
# of max_kb). Each one is deleted after the boot, in steps of 128 units of 32 B (the stand-in's
# "cluster": one 4 KB LittleFS block), with no mark
SPILL_EXTRA_EXTENTS = {
    # 5 truncates (20 480, 16 384, 12 288, 8 192 and 4 096 B) and the unlink
    "sf/r0020.sf": bytes(range(256)) * 96,
    # 2 truncates and the unlink
    "sf/r0021.sf": struct.pack("<I", 0x44414544) + bytes(10236),
    # a short file (a cut while the extent was made): only the unlink
    "sf/r00ff.sf": b"GWS",
}
SPILL_EXTRA_STEPS = 6 + 3 + 1
# Issues #134 and #248: files in the folder of the card that are not of the ring. The gateway
# reads no earlier card format: it never reads, changes or deletes such a file, and does not count
# it. The first one has the block magic "GSWB", version 2 and the record size of a ring block
OTHER_SPILL_FILES = {
    "sf/00000100.sf": struct.pack("<IHH", 0x42575347, 2, 24) + bytes(range(256)) * 80,
    "sf/80000001.sf": b"not a spill file",
    "sf/notes.txt": b"a file of the operator\n",
}


def spill_line(gw, since, rx=r"^spill\s"):
    return (gw.find(rx, since) or [""])[-1]


def bulk0_history(nb, node):
    """Historical Bulk0 values and times in the DDATAs after the NBIRTH @nb."""
    hist = [mm for m in CAPTURE if nb and m[0] >= nb[0] and m[1] == f"{T}/DDATA/{node}/{BULK}"
            for mm in m[2].metrics if mm.is_historical and mm.alias == ALIAS["Bulk0"]]
    return [mm.int_value for mm in hist], [mm.timestamp for mm in hist]


CARD_STACK_MIN_B = 1024   # #139: the format task's stack never used, at least (of 5120 B)


def check_card_web(gw, since, psram, auth):
    """Issue #139 (GW23; QEMU has no microSD card): POST /api/card/format goes through the same
    checks as every write: no credentials -> 401, no X-Requested-With or another Origin -> 403
    (before the password check), no ?confirm=1 -> 400; with all of them, the clear refusal of a
    build without a card (501, reason "no_support") and nothing changes. /api/status has the
    optional card, spill and card_format objects (without PSRAM: the spill does not start, and
    why), and the page has the microSD section with its format button."""
    path = "/api/card/format?confirm=1"
    codes = {
        "no credentials": http("POST", path, b"", auth=None)[0],
        "no X-Requested-With": http("POST", path, b"", auth=auth, headers={"X-Requested-With": None})[0],
        "other Origin": http("POST", path, b"", auth=auth, headers={"Origin": "https://evil.e2e"})[0],
        "no confirm": http("POST", "/api/card/format", b"", auth=auth)[0],
    }
    st, _, body = http("POST", path, b"", auth=auth)
    ans = as_json(body) or {}
    st_s, _, b_s = http("GET", "/api/status", auth=auth)
    status = as_json(b_s) or {}
    card, spill, fmt = (status.get(k) or {} for k in ("card", "spill", "card_format"))
    want_spill = (spill == {"state": "not_started", "detail": "it needs PSRAM, and none was detected"}
                  if not psram else spill.get("state") == "on" and "waiting" in spill)
    st_p, _, page = http("GET", "/", auth=auth)
    ui = all(x.encode() in page for x in ("<h2>microSD card</h2>", ">Format the card<",
                                          '"/api/card/format?confirm=1"', '"FORMAT"'))
    requested = gw.find(r"card format requested", since)
    check("Issue #139 POST /api/card/format: the same Basic auth and CSRF checks as every write "
          "(401, 403 before the password, 400 without ?confirm=1); in QEMU the clear refusal "
          "of a build without a card (501, no_support) and nothing changes",
          codes == {"no credentials": 401, "no X-Requested-With": 403, "other Origin": 403,
                    "no confirm": 400}
          and st == 501 and ans.get("reason") == "no_support" and ans.get("ok") is False
          and "has no microSD card" in ans.get("error", "") and fmt == {"state": "idle"}
          and not requested,
          ", ".join(f"{k} -> {v}" for k, v in codes.items()) + f"; confirmed -> {st} {ans}; "
          f"card_format {fmt}; format requests in the log {len(requested)}")
    check(f"Issue #139 /api/status has the card, spill and card_format objects ("
          f"{'PSRAM: the spill is on' if psram else 'no PSRAM: the spill does not start, and why'})"
          f", and the web UI has the microSD section, its format button and its confirmation",
          st_s == 200 and card.get("state") == "unsupported" and "LittleFS" in card.get("detail", "")
          and want_spill and fmt == {"state": "idle"} and st_p == 200 and ui,
          f"/api/status {st_s}: card {card}, spill {spill}, card_format {fmt}; page {st_p} "
          f"{len(page)} B, section and button {ui}")


def check_card_simulated(gw, spill_on, auth):
    """Issues #139/#137 (emulator only: 'card format simulate', which writes nothing): the shell
    only starts the format task and returns; while it runs, a second request is refused (409
    busy) and /api/status and the shell 'card' show it running; with the spill on, the Sparkplug
    task first stops the spill and gives the card up. Then the result, the restart prompt, and
    the task's stack margin."""
    asked = time.monotonic()
    gw.console("card format simulate 5000")
    started = gw.wait_log(r"^card format: started in the background .*\(simulated: nothing is "
                          r"written\)", 5, asked)
    st409, _, b409 = http("POST", "/api/card/format?confirm=1", b"", auth=auth)
    busy = as_json(b409) or {}
    _, _, b_run = http("GET", "/api/status", auth=auth)
    run = (as_json(b_run) or {}).get("card_format") or {}
    _, h = gw.shell("card", [r"^format\s+running for \d+ s \(simulated: nothing written\)"])
    shell_run = list(h.values())[0]
    done = gw.wait_log(r"card format: done in \d+ s \(simulated: nothing written\): FAT32, 32768 B "
                       r"clusters, 0 MB; restart the gateway to use the card \(format task: \d+ "
                       r"of 5120 B of stack never used\)", 40, asked)
    line = (gw.find(r"card format: done in", asked) or [""])[0]
    m = re.search(r"format task: (\d+) of", line)
    stack_free = int(m.group(1)) if m else 0
    _, _, b_end = http("GET", "/api/status", auth=auth)
    end = as_json(b_end) or {}
    efmt, espill = end.get("card_format") or {}, end.get("spill") or {}
    asked2, h2 = gw.shell("status", [r"^format\s+done in \d+ s \(simulated"])
    stopping = gw.find(r"card format: the spill uses the card: the Sparkplug task stops it first",
                       asked)
    released = gw.find(r"store & forward: the card is released for a format", asked)
    spill_ok = (stopping and released and espill.get("state") == "stopped"
                and "formatted" in espill.get("detail", "")) if spill_on \
        else not stopping and not released
    check(f"Issues #139/#137 the format runs in its own task: the shell starts it and returns; "
          f"while it runs a second request gets 409 (busy) and /api/status and 'card' show it "
          f"running; "
          f"{'the Sparkplug task stops the spill and gives the card up first; ' if spill_on else ''}"
          f"then done with the restart prompt, and >= {CARD_STACK_MIN_B} B of the task's stack "
          f"never used",
          started and st409 == 409 and busy.get("reason") == "busy" and run.get("state") == "running"
          and run.get("simulated") is True and "elapsed_s" in run and shell_run and done
          and efmt.get("state") == "done" and efmt.get("restart") is True
          and all(h2.values()) and spill_ok and stack_free >= CARD_STACK_MIN_B,
          f"409 {busy}; running {run}; {line or 'not done'}; after: {efmt}, spill {espill}; "
          f"stop {len(stopping)}, released {len(released)}")



def check_restart_flush(gw, obs, relay, slave, node, prev_nb, first, how, auth=None, new_cfg=None,
                        replay=True):
    """Issue #136 (GW23, PSRAM variant): an outage that leaves samples in the RAM ring below three
    quarters (750 of the 1000 samples of this build: nothing goes to the card by itself), then a
    planned restart (@how: the shell 'restart', POST /api/restart, or "config": PUT /api/config
    with @new_cfg, issue #145). The Sparkplug task moves the RAM ring to the card first; after the
    boot the recovery finds it, and the replay sends exactly those samples as historical DDATA,
    once and oldest first, and nothing from before. With @replay False (a configuration that
    changes what the records mean, #145) the boot starts a new ring instead: 0 samples waiting
    and no historical DDATA; and issue #141: the Sparkplug task does not move the RAM ring before
    that restart, and its log line says why (the saved hash, which is the hash of the next boot,
    and the hash of the ring). Returns (ok, detail, the NBIRTH after the restart, the
    configuration hash of the spill in the boot log after the restart)."""
    since = time.monotonic()
    relay.stop()
    gw.wait_log(r"MQTT down; reconnect attempt", 10, since)
    value, waiting, end = first, 0, time.monotonic() + 40
    while wait_left(end) > 0 and waiting < 300:
        value += 1
        slave.bulk.setValues(3, 100, [value] * 60)
        time.sleep(0.5)
        asked, _ = gw.shell("status", [r"^store\s+\d+ of 1000 samples"], timeout=2, tries=1)
        m = re.search(r"^store\s+(\d+) of", spill_line(gw, asked, r"^store\s"))
        waiting = int(m.group(1)) if m else waiting
    time.sleep(2)   # the last value is polled and stored
    asked, _ = gw.shell("status", [r"^store\s+\d+ of 1000 samples", r"^spill\s+on: "])
    store_l, spill_l = spill_line(gw, asked, r"^store\s"), spill_line(gw, asked)
    m = re.search(r"^store\s+(\d+) of", store_l)
    waiting = int(m.group(1)) if m else 0
    m = re.search(r", (\d+) moved there", spill_l)
    spilled = int(m.group(1)) if m else -1
    t = time.monotonic()
    if how == "shell":
        gw.console("restart")
        answer = gw.wait_log(r"^restarting\s*$", 10, t) is not None
    elif how == "config":
        answer = http("PUT", "/api/config", new_cfg, auth=auth, timeout=30)[0] == 200
    else:
        answer = http("POST", "/api/restart", b"", auth=auth)[0] == 202
    flush_rx = (r"store & forward: before the restart, (\d+) samples \((\d+) records\) moved from "
                r"the RAM ring to the card in (\d+) ms .*sparkplug stack (\d+) B never used")
    skip_rx = (r"store & forward: the saved configuration has another configuration hash "
               r"\(([0-9a-f]{8}), the ring has ([0-9a-f]{8})\): the \d+ samples of the RAM ring are "
               r"not moved to the card \(the next boot starts a new ring\)")
    gw.wait_log(flush_rx if replay else skip_rx, 30, t)
    flush_l = (gw.find(r"store & forward: before the restart", t) or [""])[0]
    skip_l = (gw.find(r"store & forward: the saved configuration has another", t) or [""])[0]
    ready_rx = (r"store & forward: card ready in \d+ ms \(\d+ blocks read\): [1-9]\d* samples "
                r"waiting, ring \d+ of \d+ blocks, nonce [0-9a-f]{8} \(ring found with its "
                r"superblock\); " if replay
                else r"store & forward: card ready in \d+ ms \(\d+ blocks read\): 0 samples waiting, "
                r".* \(new ring: another configuration or geometry\); ")
    ready = gw.wait_log(ready_rx, 90, t)
    ready_l = (gw.find(r"store & forward: card ready", t) or [""])[0]
    ring_l = (gw.find(r"store & forward: spill to .* configuration [0-9a-f]{8}", t) or [""])[0]
    mh = re.search(r"configuration ([0-9a-f]{8})", ring_l)
    relay.start()
    # #161: the boot after the restart starts at bdSeq 0 again: its NBIRTH by its time
    nb = obs.wait(f"{T}/NBIRTH/{node}", 90, since=t)
    done, deadline = None, time.monotonic() + 60
    while wait_left(deadline) > 0 and not done:
        _, h = gw.shell("status", [r"^store\s+0 of \d+ samples waiting"], timeout=2, tries=1)
        done = list(h.values())[0]
    time.sleep(1)
    reg, ts = bulk0_history(nb, node)
    total = sum(1 for m in CAPTURE if nb and m[0] >= nb[0] and m[1].split("/")[3:4] == [node]
                for mm in m[2].metrics if mm.is_historical)
    mf = re.search(flush_rx, flush_l)
    moved, recs, ms, stack = (int(x) for x in mf.groups()) if mf else (0, 0, -1, 0)
    restarted = answer and 0 < waiting < 750 and spilled == 0 and ready and nb and done
    if replay:
        ok = (restarted and mf and recs >= waiting and stack >= STACK_MARGIN_B and not skip_l
              and total == moved and reg and reg == sorted(reg) and len(set(reg)) == len(reg)
              and all(first < v <= value for v in reg) and reg[0] <= first + 5 and reg[-1] == value
              and ts == sorted(ts))
    else:
        # #141: no move, and the line names the hash of the next boot and the hash of the ring
        ms_ = re.search(skip_rx, skip_l)
        ok = (restarted and ms_ and not mf and mh and int(ms_.group(1), 16) == int(mh.group(1), 16)
              and ms_.group(1) != ms_.group(2) and total == 0 and not reg)
    detail = (f"{how}: {waiting} records in the RAM ring before the restart, {spilled} moved to "
              f"the card by the spill | {flush_l or skip_l or 'no flush line'} | "
              f"{ring_l or 'no ring line'} | "
              f"{ready_l or 'not ready'} | {total} historical metrics after the restart, register "
              f"100: {reg[:2]}..{reg[-2:]} ({len(reg)} samples, the outage set {first + 1}..{value})")
    return ok, detail, nb, int(mh.group(1), 16) if mh else None


def run_store_forward_spill(args, work, slave):
    """SPB-14 spill (GW23) with store_forward.sd enabled. QEMU has no SDMMC: the build puts the
    "card" in a LittleFS directory (CONFIG_GW_SF_SPILL_LITTLEFS), whose 32 B "clusters" make
    every extent of the ring one 4 KB block (issue #124: 32 extents for max_kb 128).
    Default variant (no PSRAM): issue #124, the spill needs the PSRAM: it does not start, the
    boot log and the shell say why, and the card is not used.
    PSRAM variant (the ring has 1000 samples in this build: sdkconfig.qemu-psram):
      1. The image holds SPILL_EXTRA_EXTENTS (extent files beyond the ring) and
         OTHER_SPILL_FILES (files that are not of the ring). The card is read after the
         network starts: a new ring; the extra extents are deleted in exactly
         SPILL_EXTRA_STEPS bounded steps (#124). The other files are not counted, and after
         the power cut of step 3 they are in the flash as the image had them (#134, #248).
      2. The broker goes away (a relay) while the harness changes all 60 Bulk registers twice a
         second: the ring spills its oldest samples to the card, 4 KB blocks. Back: every stored
         sample comes as historical DDATA, oldest first, none lost or overwritten.
      3. A second outage, then a power cut (QEMU killed) while offline: after the reboot the
         blocks on the card are found (superblock and scan), and replayed once: none of the
         first outage again (the checkpoint).
      3b. Issue #136: an outage below three quarters of the RAM ring, then a planned restart
         (the shell 'restart', then POST /api/restart): the RAM ring goes to the card first, and
         its samples come back once, in order, after the boot (check_restart_flush).
      3c. Issue #145: the same with a saved configuration (PUT /api/config) that changes only
         fields outside the hash of the spill: the same hash, the samples come back; then one
         that renames a tag: another hash, a new ring, no replay.
    Both variants: 'card format' asks for 'confirm', and 'card format confirm' refuses
    cleanly (no card in QEMU); the Sparkplug task keeps its stack margin. Issue #139: the web
    API's checks and refusal, the status objects and the page (check_card_web), and the
    format task with a simulated format (check_card_simulated: in the PSRAM variant at the end,
    with the spill on)."""
    node, port = "GW23", MQTT_PORT + 11
    relay = Relay(port, MQTT_PORT)
    obs = Observer()
    cfg = node_config(node, port=port)
    cfg["sparkplug"]["store_forward"] = {"enabled": True, "sd": {"enabled": True, "max_kb": 128}}
    t0 = time.monotonic()
    gw = boot_variant(args, work, "sfsd", cfg, files={**SPILL_EXTRA_EXTENTS, **OTHER_SPILL_FILES})
    nb = obs.wait(f"{T}/NBIRTH/{node}", 40)
    obs.wait(f"{T}/DBIRTH/{node}/{BULK}", 10)
    # the format command: never without "confirm", and no card to format in QEMU
    fmt_rx = (r"^card format: this deletes everything on the microSD card",
              r"^card format: REFUSED: this build has no microSD card")
    asked_f, hf = gw.shell(["card format", "card format confirm"], list(fmt_rx))
    fmt_ok = all(hf.values())
    # #139: no web password in this configuration (its move to NVS would format LittleFS, and
    # the files of the image with it): the commissioning password
    web = ("admin", commissioning_password(gw, t0) or "?")
    wait_http("/api/status", 20, auth=web)
    check_card_web(gw, t0, args.psram, web)
    if not args.psram:
        refused = gw.find(r"store & forward: microSD spill not started: it needs PSRAM, and none "
                          r"was detected", t0)
        asked, h = gw.shell("status", [r"^spill\s+off: it needs PSRAM, and none was detected; the "
                                       r"RAM ring overwrites", r"^stack\s+sparkplug \d+ "])
        line, stack_line = spill_line(gw, asked), spill_line(gw, asked, r"^stack\s+sparkplug \d+ ")
        free = re.search(r"sparkplug (\d+)", stack_line)
        card_used = gw.find(r"sfcard|spill to LittleFS|card ready", t0)
        check_card_simulated(gw, spill_on=False, auth=web)
        gw.kill()
        obs.wait(f"{T}/NDEATH/{node}", 5)
        obs.close()
        relay.stop()
        check("Issue #124 the microSD spill needs the PSRAM: without it (the default QEMU build) it "
              "does not start, the boot log and the shell 'spill' line say why, the card is not "
              f"used; 'card format' asks for 'confirm' and then refuses (no card in QEMU); the "
              f"Sparkplug task keeps >= {STACK_MARGIN_B} B of its stack",
              nb and refused and all(h.values()) and not card_used and fmt_ok and free
              and int(free.group(1)) >= STACK_MARGIN_B,
              f"{(refused or ['no refusal in the log'])[0]} | {line or 'no spill line'} | card "
              f"used: {card_used[:2]} | format: {gw.find(r'^card format:', asked_f)} | "
              f"{stack_line or 'no stack line'}")
        return
    ring_log = gw.find(r"store & forward: spill to LittleFS /cfg/sf .*: a ring of 128 KB, 32 extents "
                       r"of 4 KB, configuration ([0-9a-f]{8})", t0)
    m = re.search(r"configuration ([0-9a-f]{8})", ring_log[0]) if ring_log else None
    cfg_hash = int(m.group(1), 16) if m else 0
    ready = gw.wait_log(r"store & forward: card ready in \d+ ms .* \(new ring: no valid ring on the "
                        r"card\); 3 file\(s\) to delete", 20, t0)

    def load(seconds, first):
        value, end = first, time.monotonic() + seconds
        while wait_left(end) > 0:
            value += 1
            slave.bulk.setValues(3, 100, [value] * 60)
            time.sleep(0.5)
        return value

    # 2. the first outage: the ring spills to the card
    since = time.monotonic()
    relay.stop()
    gw.wait_log(r"MQTT down; reconnect attempt", 10, since)
    last1 = load(40, 2000)
    asked, h = gw.shell("status", [r"^spill\s+on: [1-9]\d* samples on the card, [1-9]\d* moved "
                                   r"there, 0 overwritten unsent",
                                   r"^store\s+[1-9]\d* of 1000 samples waiting \(PSRAM\), .* 0 overwritten"])
    spilled = all(h.values())
    before1 = spill_line(gw, asked)
    waiting = [int(x) for x in re.findall(r"^store\s+(\d+) of", "\n".join(gw.find(r"^store\s", asked)), re.M)]
    relay.start()
    nb2 = obs.wait(f"{T}/NBIRTH/{node}", 70)
    done, deadline = None, time.monotonic() + 60
    while wait_left(deadline) > 0 and not done:
        _, h = gw.shell("status", [r"^store\s+0 of \d+ samples waiting"], timeout=2, tries=1)
        done = list(h.values())[0]
    # the checkpoint and the deletes end (one step per loop); then the counters
    end_rx = (r"^spill\s+on: 0 samples on the card, .*, 0 overwritten unsent \(card full\), 0 "
              r"block\(s\) lost as holes; .*\((\d+) checkpoints, at most (\d+) samples replayed "
              r"between two\); boot recovery \d+ ms, \d+ blocks read \(new ring: no valid ring on "
              r"the card\); 0 file\(s\) to delete, (\d+) deleted in (\d+) steps, 0 left")
    gc_line = card_line = stack_line = ""
    end = time.monotonic() + 30
    while wait_left(end) > 0 and not gc_line:
        asked3, _ = gw.shell("status", [end_rx, r"^card\s", r"^stack\s+sparkplug \d+ "], timeout=2, tries=1)
        gc_line = spill_line(gw, asked3, end_rx)
        card_line = spill_line(gw, asked3, r"^card\s")
        stack_line = spill_line(gw, asked3, r"^stack\s+sparkplug \d+ ")
    mm = re.search(end_rx, gc_line)
    ckpts, ck_max, nfiles, nsteps = (int(x) for x in mm.groups()) if mm else (0, -1, 0, -1)
    ops = {k: int(v) for k, v in re.findall(r" (\w+) (\d+) avg [\d.]+ max [\d.]+ ms", card_line)}
    unfed = re.search(r"longest unfed (\d+) ms", card_line)
    free = re.search(r"sparkplug (\d+)", stack_line)
    gc_ok = (nfiles == 3 and nsteps == SPILL_EXTRA_STEPS and ops.get("step") == SPILL_EXTRA_STEPS
             and "mark" not in ops and ckpts >= 1 and 0 <= ck_max <= 2730
             and all(ops.get(k, 0) > 0 for k in ("open", "block", "super", "read", "alloc", "list"))
             and re.search(r" 0 error\(s\)", card_line))
    reg1, ts1 = bulk0_history(nb2, node)
    total = sum(1 for m in CAPTURE if nb2 and m[0] >= nb2[0] and m[1].split("/")[3:4] == [node]
                for mm in m[2].metrics if mm.is_historical)
    check("SPB-14 / issue #124 spill to the card (LittleFS stand-in, PSRAM ring of 1000 samples): "
          "the ring spills its oldest samples in 4 KB blocks; after the reconnection all of them "
          "come back as historical DDATA, oldest first, none overwritten or lost",
          nb and ring_log and ready and spilled and waiting and waiting[0] > 1000 and nb2 and done
          and total >= waiting[0] and reg1 == sorted(reg1) and ts1 == sorted(ts1)
          and len(set(reg1)) == len(reg1) and reg1 and 2000 < reg1[0] < reg1[-1] <= last1,
          f"{(ring_log or ['no ring log'])[0]}; waiting before the reconnection {waiting}; "
          f"{total} historical metrics after it, register 100: {reg1[:2]}..{reg1[-2:]} "
          f"({len(reg1)} samples, in order {reg1 == sorted(reg1)}) | {before1}")
    check(f"Issues #124/#134/#248 the card work in bounded steps after the network starts: the "
          f"3 extent files beyond the ring deleted in {SPILL_EXTRA_STEPS} steps (truncates of "
          f"4 KB, unlink, no mark; the {len(OTHER_SPILL_FILES)} files that are not of the ring "
          f"are not counted), a checkpoint after the replay (at most 64 KB "
          f"replayed between two), the card's operation times in the shell, the Sparkplug task fed "
          f"within the task watchdog and >= {STACK_MARGIN_B} B of its stack free",
          nb and ready and gc_ok and unfed and int(unfed.group(1)) < 15000
          and free and int(free.group(1)) >= STACK_MARGIN_B,
          f"{gc_line or 'deletes or checkpoint not finished'} | {card_line or 'no card line'} | "
          f"{stack_line or 'no stack line'}")

    # 3. a second outage, then a power cut while offline: the blocks on the card survive
    since = time.monotonic()
    relay.stop()
    gw.wait_log(r"MQTT down; reconnect attempt", 10, since)
    last2 = load(25, 5000)
    asked, h = gw.shell("status", [r"^spill\s+on: [1-9]\d* samples on the card"])
    before2 = spill_line(gw, asked)
    on_card = re.search(r"^spill\s+on: (\d+) samples", before2)
    on_card = int(on_card.group(1)) if on_card else 0
    gw.kill()   # the power cut: what is in the RAM ring is lost, the card is not
    # #134, #248: after the boot recovery, two spills, a replay and the deletes, the files that
    # are not of the ring are in the flash as the image had them; the extra extents are gone
    on_flash = storage_files(args, work, "sfsd", gw.flash)
    kept = {n: (on_flash or {}).get(n) for n in OTHER_SPILL_FILES}
    left = sorted(n for n in SPILL_EXTRA_EXTENTS if n in (on_flash or {}))
    ring_files = sorted(n for n in (on_flash or {}) if re.fullmatch(r"sf/(r00[01][0-9a-f]|ring[ab])\.sf", n))
    check("Issues #134/#248 a file in the folder of the card that is not of the ring (a name of 8 "
          "hexadecimal digits with the header of a ring block, a number at or above 0x80000000, a "
          "text file) is never changed and never deleted: after the boot recovery, the spill, the "
          "replay and the deletes, the LittleFS of the flash has each one with the bytes of the "
          "image, next to the files of the ring; the extent files beyond the ring are gone",
          on_flash is not None and all(kept[n] == OTHER_SPILL_FILES[n] for n in OTHER_SPILL_FILES)
          and not left and "sf/ringa.sf" in ring_files and "sf/r0000.sf" in ring_files,
          f"LittleFS read back: {'no' if on_flash is None else len(on_flash)} file(s); not of the "
          f"ring: {({n: ('same' if kept[n] == OTHER_SPILL_FILES[n] else 'absent' if kept[n] is None else 'CHANGED') for n in kept})}; "
          f"extra extents left: {left or 'none'}; ring files: {len(ring_files)}")
    gw2 = Gateway(args, work / "uart0-sfsd-cut.log", gw.flash)
    t2 = time.monotonic()
    gw2.start()
    ready2 = gw2.wait_log(r"store & forward: card ready in \d+ ms \(\d+ blocks read\): ([1-9]\d*) "
                          r"samples waiting, .* \(ring found with its superblock\); ", 60, t2)
    ready2_line = (gw2.find(r"store & forward: card ready", t2) or [""])[0]
    relay.start()
    nb3 = obs.wait(f"{T}/NBIRTH/{node}", 70, since=t2)
    done3, deadline = None, time.monotonic() + 60
    while wait_left(deadline) > 0 and not done3:
        _, h = gw2.shell("status", [r"^store\s+0 of \d+ samples waiting"], timeout=2, tries=1)
        done3 = list(h.values())[0]
    time.sleep(1)
    reg2, ts2 = bulk0_history(nb3, node)
    m2 = re.search(r"(\d+) samples waiting", ready2_line)
    found = int(m2.group(1)) if m2 else 0
    check("Issue #124 a power cut while offline: after the reboot the boot recovery finds the "
          "blocks on the card, and the replay sends them once, oldest first: values of the second "
          "outage only (the checkpoint keeps the first outage from coming again)",
          ready2 and nb3 and done3 and found > 0 and found >= on_card and reg2
          and reg2 == sorted(reg2) and len(set(reg2)) == len(reg2)
          and all(5000 < v <= last2 for v in reg2) and reg2[0] <= 5000 + 25 and ts2 == sorted(ts2),
          f"before the cut: {before2}; {ready2_line}; register 100 after the reboot: "
          f"{reg2[:2]}..{reg2[-2:]} ({len(reg2)} samples)")

    # 3b. #136: samples below three quarters of the RAM ring, then a planned restart: the shell
    # 'restart', then POST /api/restart (through the web server's task)
    ok_sh, det_sh, nb_sh, _ = check_restart_flush(gw2, obs, relay, slave, node, nb3, 9000, "shell")
    web2 = ("admin", commissioning_password(gw2, t2) or "?")
    t_web = time.monotonic()
    ok_web, det_web, nb_web, _ = check_restart_flush(gw2, obs, relay, slave, node, nb_sh, 9500,
                                                     "web", web2)
    check("Issue #136 a planned restart while offline (the shell 'restart', and POST /api/restart) "
          "moves the RAM ring to the card first (bounded, one card operation per step): after the "
          "boot its samples come back as historical DDATA, once, oldest first, and nothing from "
          f"before; the Sparkplug task keeps >= {STACK_MARGIN_B} B of its stack",
          ok_sh and ok_web, f"{det_sh} || {det_web}")

    # 3c. #145: a saved configuration that changes only fields outside the hash of the spill
    # (a deadband, a scale that keeps the tag scaled, units, poll period, offline_after) keeps
    # the data on the card: the same hash, no new ring, the moved samples replayed after the
    # restart. Then a saved configuration that renames a tag starts a new ring (another hash):
    # the moved samples are not replayed.
    keep = copy.deepcopy(cfg)
    meter, bulk = keep["devices"][0], keep["devices"][1]
    meter["tags"][0]["deadband"] = 0.5
    meter["tags"][0]["units"] = "kV"
    meter["tags"][1]["scale"] = 0.5
    bulk["poll_ms"] = 900
    bulk["offline_after"] = 4
    web3 = ("admin", commissioning_password(gw2, t_web) or "?")
    t_keep = time.monotonic()
    ok_keep, det_keep, nb_keep, h_keep = check_restart_flush(
        gw2, obs, relay, slave, node, nb_web, 10000, "config", web3, new_cfg=keep)
    renamed = copy.deepcopy(keep)
    renamed["devices"][-1]["tags"][0]["name"] = "Value2"
    web4 = ("admin", commissioning_password(gw2, t_keep) or "?")
    t_new = time.monotonic()
    ok_new, det_new, _, h_new = check_restart_flush(
        gw2, obs, relay, slave, node, nb_keep, 10500, "config", web4, new_cfg=renamed, replay=False)
    check("Issues #145/#141 the hash of the spill covers only what a record means on replay: a "
          "saved configuration (PUT /api/config, a planned restart) that changes a deadband, units, "
          "a scale of a scaled tag, a poll period and offline_after keeps the hash and the data on "
          "the card (the RAM ring is moved, and the samples come back once, in order); one that "
          "renames a tag gets another hash and a new ring, and the RAM ring is not moved before "
          "that restart (the log line names both hashes)",
          ok_keep and ok_new and cfg_hash and h_keep == cfg_hash and h_new and h_new != cfg_hash,
          f"configuration {cfg_hash:08x} at the first boot, {h_keep or 0:08x} after the first "
          f"change, {h_new or 0:08x} after the rename || {det_keep} || {det_new}")
    check_card_simulated(gw2, spill_on=True, auth=("admin", commissioning_password(gw2, t_new) or "?"))
    gw2.kill()
    obs.wait(f"{T}/NDEATH/{node}", 5)
    obs.close()
    relay.stop()
    slave.bulk.setValues(3, 100, list(range(1000, 1060)))   # what the main scenario expects
    check("Issue #124 'card format' asks for 'confirm' and never runs by itself; 'card format "
          "confirm' refuses cleanly in QEMU (no microSD card)", fmt_ok,
          " | ".join(gw.find(r"^card format:", asked_f)))


# ---------------------------------------------------------------- HTTPS and login limit (issue #64)

def run_https(args, work):
    """Issue #64 in one boot (GW22): the web UI and the REST API over HTTPS with the certificate
    the device generated at its first boot (its SHA-256 fingerprint on the console matches the
    one presented), plain HTTP only redirecting, and the login limit: after 5 failed logins in a
    row the source address gets 429 (also with the right password) until the block ends."""
    node = "GW22"
    cfg = node_config(node)
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    t0 = time.monotonic()
    gw = boot_variant(args, work, "https", cfg)
    st, _, _ = wait_http("/api/status", 40)
    printed = gw.find(r"HTTPS certificate SHA-256 fingerprint ([0-9A-F:]{95})", t0)
    der = PEER_CERT.get("der", b"")
    fp = ":".join(f"{b:02X}" for b in hashlib.sha256(der).digest()) if der else ""
    generated = gw.find(r"HTTPS: new device certificate for gw22 generated", t0)
    check("Issue #64 HTTPS with the device certificate generated at the first boot; its SHA-256 "
          "fingerprint on the console matches the certificate presented",
          st == 200 and printed and fp and fp in printed[0] and generated,
          f"/api/status over HTTPS -> {st}; presented {fp[:23]}...; console "
          f"{(printed or ['no fingerprint'])[0][-60:]}")
    st_r, hdr_r, _ = http("GET", "/api/status?x=1", auth=None, plain=True)
    st_p, hdr_p, _ = http("PUT", "/api/config", b"{}", auth=None, plain=True)
    loc = hdr_r.get("location", "")
    check("Issue #64 plain HTTP only redirects to HTTPS (301 for GET, 308 for PUT; no API over HTTP)",
          st_r == 301 and loc == "https://127.0.0.1/api/status?x=1" and st_p == 308
          and hdr_p.get("location", "").startswith("https://"),
          f"GET -> {st_r} {loc}; PUT -> {st_p} {hdr_p.get('location')}")

    check_csrf(gw, t0)
    check_refused_bodies(gw, t0)
    check_slow_client(gw, t0)
    check_web_cert(gw, work, node)
    if args.web_stress:
        web_stress(gw, args.web_stress)

    codes = [http("GET", "/api/status", auth=("admin", f"wrong-{i}"))[0] for i in range(5)]
    st6, hdr6, _ = http("GET", "/api/status", auth=("admin", "wrong-6"))
    st7, hdr7, _ = http("GET", "/api/status")   # the right password, while blocked
    blocked = gw.wait_log(r"10\.0\.2\.2 blocked for 30 s after 5 failed logins in a row", 3, t0)
    asked, h = gw.shell("status", [r"^web\s+HTTPS \(HTTP redirects\), logins failed 5, refused "
                                   r"while blocked 2, blocks 1, addresses blocked now 1"])
    counters = list(h.values())[0]
    retry = int(hdr7.get("retry-after", "0") or 0)
    time.sleep(retry + 1)
    st8, _, _ = http("GET", "/api/status")
    gw.kill()
    check("Issue #64 login limit per source address: 5 failures -> 429 with Retry-After for 30 s, "
          "also with the right password; logged and counted in the shell; then the right password "
          "works again",
          codes == [401] * 5 and st6 == 429 and st7 == 429 and 0 < retry <= 30
          and blocked and counters and st8 == 200,
          f"failures {codes}, then {st6} / right password {st7} (Retry-After {retry}); after the "
          f"block {st8}; {(gw.find(r'blocked for', t0) or ['no log'])[0]}; "
          + " | ".join(gw.find(r"^web\s", asked)))


Y2020_MS = 1577836800000   # 2020-01-01T00:00:00Z: a timestamp before it counts from 1970


def stamps(p):
    """The payload timestamp and every metric timestamp of @p (ms)."""
    return [p.timestamp] + [m.timestamp for m in p.metrics]


def node_msgs(node, since, kinds=("NBIRTH", "DBIRTH", "NDATA", "DDATA")):
    """Sparkplug messages of @node (of the types @kinds) captured at or after @since."""
    return [m for m in CAPTURE if m[0] >= since and len(m[1].split("/")) > 3
            and m[1].split("/")[2] in kinds and m[1].split("/")[3] == node]


def time_data(node, since):
    """NDATA and DDATA of @node captured at or after @since."""
    return node_msgs(node, since, ("NDATA", "DDATA"))


def now_ms():
    return int(time.time() * 1000)


WAIT_CLOCK_RX = r"edge: waiting for a valid clock: no MQTT connection and no NBIRTH until then"
BATTERY_RX = r"the RTC lost the time while the power was off \(OS bit\): examine the RTC battery"
CLOCK_WINDOW_S = 8   # issue #160: the bounded time in which no CONNECT may come (the old 3 s wait x2)


def broker_clients(work, client, offset):
    """CONNECTs of @client in the main broker's log after its first @offset characters."""
    text = (work / "mosquitto.log").read_text(errors="replace")[offset:]
    return re.findall(rf"New client connected from \S+ as {re.escape(client)} \(", text)


def broker_log_len(work):
    try:
        return len((work / "mosquitto.log").read_text(errors="replace"))
    except OSError:
        return 0


def run_valid_time(args, work, slave):
    """Issue #160, decision of the product owner of 2026-11-14 (TIME-1;
    tck-id-message-flow-edge-node-birth-publish-connect, tck-id-topics-nbirth-timestamp,
    tck-id-payloads-nbirth-timestamp, tck-id-payloads-timestamp-in-UTC,
    tck-id-payloads-metric-timestamp-in-UTC, tck-id-topics-nbirth-bdseq-increment), in two boots
    of GW31, each a power-on (the simulated PCF85063 has its OS bit set, so no time from it) with
    the SNTP responder silent. Without a valid clock the gateway opens no MQTT session: no CONNECT
    reaches the broker and no message of the node comes in a bounded window after the log line of
    the wait, while a tag changes and DI/1 pulses. The log, the shell, /api/status and the LED show
    the wait and the battery hint. Boot 1: POST /api/time (the web UI's button) with the CSRF and
    password checks of every write, then the first CONNECT (bdSeq 0) and the NBIRTH with UTC
    timestamps and the values of the changes, no rebirth, and DATA with UTC timestamps; the source
    is "browser", Time Synced stays false. Boot 2: SNTP answers late, then the first NBIRTH with UTC
    timestamps and Time Synced true; a time from the browser is refused (409) over a clock that SNTP
    synchronises. No message of the node in either boot has a timestamp before 2020."""
    node = "GW31"
    nbirth_t, dbirth_t = f"{T}/NBIRTH/{node}", f"{T}/DBIRTH/{node}/{DEVICE}"
    ndata_t, ddata_t = f"{T}/NDATA/{node}", f"{T}/DDATA/{node}/{DEVICE}"
    cfg = node_config(node)
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    obs = Observer()
    volt = (lambda p: metric(p, alias=ALIAS["Voltage"]))
    SNTP_PAUSED.set()
    try:
        # ---- boot 1: the time from the browser
        t0 = time.monotonic()
        blog0 = broker_log_len(work)
        gw = boot_variant(args, work, "time", cfg)
        waiting = gw.wait_log(WAIT_CLOCK_RX, 60, t0)
        slave.set_float(4, 0, 236.0)
        gw.console("di 1 1")
        time.sleep(0.5)
        gw.console("di 1 0")
        # a bounded window after the evidence of the wait: no CONNECT and no message of the node
        time.sleep(CLOCK_WINDOW_S)
        early = node_msgs(node, t0)
        conns = broker_clients(work, node, blog0)
        tried = gw.find(r"edge: connecting to ", t0)
        battery = gw.find(r"io: RTC: " + BATTERY_RX, t0) + gw.find(r"edge: " + BATTERY_RX, t0)
        led = gw.find(r"led: status: waiting for a valid clock", t0)
        st0, _, b0 = http("GET", "/api/status")
        s0 = as_json(b0) or {}
        tm0 = s0.get("time", {})
        asked, h = gw.shell("status", [
            r"^network\s+up, MQTT waiting for a valid clock",
            r"^time\s+1970-.*NOT valid \(no SNTP, RTC or browser time yet\)",
            r"^clock\s+waiting for a valid clock: no MQTT connection and no NBIRTH until then\. "
            r"Make sure .*Set the time from the browser",
            r"^battery\s+" + BATTERY_RX + r" \(a rechargeable 3 V cell, for example ML1220; never "
            r"a CR1220\)"])
        shown0 = all(h.values())
        check("Issue #160 without a valid clock (no SNTP, RTC OS bit set): the gateway waits (log) "
              f"and sends no CONNECT and no message for {CLOCK_WINDOW_S} s after it, while a tag "
              "changes and DI/1 pulses; battery hint in the log; LED 'waiting for a valid clock'; "
              "/api/status time.valid false, source none, waiting true, rtc.lost true, MQTT not "
              "connected; shell (MQTT line, time, clock and battery hints)",
              waiting and not early and not conns and not tried and len(battery) >= 2 and led
              and st0 == 200 and tm0.get("valid") is False and tm0.get("source") == "none"
              and tm0.get("synced") is False and tm0.get("waiting") is True
              and (tm0.get("rtc") or {}).get("lost") is True
              and (s0.get("mqtt") or {}).get("connected") is False and shown0,
              f"wait logged {'yes' if waiting else 'no'}; {len(early)} message(s) of the node "
              f"({[m[1] for m in early[:3]]}); broker CONNECTs {len(conns)}; attempts logged "
              f"{len(tried)}; battery lines {len(battery)}; LED {'yes' if led else 'no'}; "
              f"/api/status {st0} {tm0}, mqtt {s0.get('mqtt')}; shell "
              f"{[p[:20] for p, v in h.items() if not v]} missing")

        refused = {
            "no X-Requested-With": http("POST", "/api/time", {"utc_ms": now_ms()},
                                        headers={"X-Requested-With": None})[0],
            "other Origin": http("POST", "/api/time", {"utc_ms": now_ms()},
                                 headers={"Origin": "https://evil.e2e"})[0],
            "wrong password": http("POST", "/api/time", {"utc_ms": now_ms()},
                                   auth=("admin", "wrong-time"))[0],
            "1970": http("POST", "/api/time", {"utc_ms": 5000})[0],
            "utc_s": http("POST", "/api/time", {"utc_s": now_ms() // 1000})[0],
            "not JSON": http("POST", "/api/time", b"now")[0],
        }
        st1, _, b1 = http("GET", "/api/status")
        still = (as_json(b1) or {}).get("time", {}).get("valid")
        check("Issue #160 POST /api/time: CSRF (no X-Requested-With, other Origin) 403, wrong "
              "password 401, a time before 2025, another key or no JSON 400; the clock stays not "
              "valid and no CONNECT comes",
              refused == {"no X-Requested-With": 403, "other Origin": 403, "wrong password": 401,
                          "1970": 400, "utc_s": 400, "not JSON": 400} and still is False
              and not broker_clients(work, node, blog0) and not node_msgs(node, t0),
              f"{refused}; time.valid after them {still}; broker CONNECTs "
              f"{len(broker_clients(work, node, blog0))}")

        obs.drain()
        sent = now_ms()
        t_set = time.monotonic()
        st2, _, b2 = http("POST", "/api/time", {"utc_ms": sent})
        nb2 = obs.wait(nbirth_t, 20, since=t_set)
        db2 = obs.wait(dbirth_t, 10, since=t_set)
        logged = gw.find(r"edge: clock set from the browser: \d{4}-\d\d-\d\dT.*Z \(was none", t_set)
        resumed = gw.find(r"edge: clock valid \(source browser\) after a wait of \d+ s: connecting",
                          t_set)
        attempts = [int(x) for x in re.findall(r"bdSeq=(\d+)", " ".join(
            gw.find(r"edge: connecting to .* bdSeq=\d+", t0)))]
        ts_all = stamps(nb2[2]) + stamps(db2[2]) if nb2 and db2 else []
        v2 = volt(db2[2]) if db2 else None
        cnt = counter_of(nb2[2]) if nb2 else None
        synced = metric(nb2[2], name="Node Info/Time Synced") if nb2 else None
        check("Issue #160 POST /api/time 200 with the browser's time -> the first CONNECT of the "
              "boot (bdSeq 0, tck-id-topics-nbirth-bdseq-increment) and the NBIRTH (seq 0) and "
              "DBIRTH with every timestamp UTC (within 60 s), the value of the change made without "
              "a valid clock (Voltage 236) and DI/1's pulse (Counter/1 1); Time Synced false "
              "(TIME-3: SNTP only)",
              st2 == 200 and (as_json(b2) or {}).get("source") == "browser" and nb2 and db2
              and logged and resumed and nb2[2].seq == 0 and attempts[:1] == [0]
              and bdseq_of(nb2[2]) == 0 and ts_all
              and all(abs(t - sent) < 60000 for t in ts_all) and v2 is not None
              and abs(v2.float_value - 236.0) < 1e-3 and cnt == 1 and synced is not None
              and synced.boolean_value is False,
              f"POST {st2} {b2[:80]!r}; NBIRTH {nb2[2].timestamp - sent if nb2 else None} ms from "
              f"the browser's time, seq {nb2[2].seq if nb2 else None}, bdSeq "
              f"{bdseq_of(nb2[2]) if nb2 else None}; attempts {attempts}; timestamps off by at most "
              f"{max((abs(t - sent) for t in ts_all), default=None)} ms; Voltage "
              f"{round(v2.float_value, 1) if v2 else None}; Counter/1 {cnt}; Time Synced "
              f"{synced.boolean_value if synced else None}; {(logged or ['no log'])[0]} | "
              f"{(resumed or ['no resume log'])[0]}")

        t_chg = time.monotonic()
        slave.set_float(4, 0, 237.0)
        dd = obs.wait(ddata_t, 10, lambda q: volt(q) is not None, since=t_chg)
        gw.console("di 1 1")
        nd = obs.wait(ndata_t, 5, lambda q: metric(q, alias=ALIAS["DI/1"]) is not None, since=t_chg)
        st3, _, b3 = http("GET", "/api/status")
        tm3 = (as_json(b3) or {}).get("time", {})
        asked, h = gw.shell("status", [r"^time\s+\d{4}-.*Z, set from the browser \(not synchronised\)",
                                       r"^network\s+up, MQTT connected"])
        shown3 = all(h.values())
        nbirths = node_msgs(node, t0, ("NBIRTH",))
        check("Issue #160 after the time from the browser: DDATA and NDATA, with UTC timestamps, "
              "and one NBIRTH only in the boot (no rebirth); /api/status time.valid true, source "
              "browser, synced false, waiting false; shell",
              dd and nd and all(t >= Y2020_MS for t in stamps(dd[2]) + stamps(nd[2]))
              and abs(volt(dd[2]).float_value - 237.0) < 1e-3 and len(nbirths) == 1
              and st3 == 200 and tm3.get("valid") is True and tm3.get("source") == "browser"
              and tm3.get("synced") is False and tm3.get("waiting") is False
              and abs(tm3.get("utc_s", 0) - time.time()) < 60 and shown3,
              f"DDATA {stamps(dd[2]) if dd else None}; NDATA {stamps(nd[2]) if nd else None}; "
              f"{len(nbirths)} NBIRTH; /api/status {tm3}; " + " | ".join(gw.find(r"^time\s", asked)))
        boot1 = node_msgs(node, t0)
        gw.kill()

        # ---- boot 2: SNTP answers late
        t1 = time.monotonic()
        blog1 = broker_log_len(work)
        gw = boot_variant(args, work, "time2", cfg)
        waiting = gw.wait_log(WAIT_CLOCK_RX, 60, t1)
        slave.set_float(4, 0, 238.0)
        time.sleep(CLOCK_WINDOW_S / 2)
        before = node_msgs(node, t1)
        conns = broker_clients(work, node, blog1)
        obs.drain()
        t_sntp = time.monotonic()
        SNTP_PAUSED.clear()
        synced_at = gw.wait_log(r"edge: time synchronised", 45, t1)
        nb2 = obs.wait(nbirth_t, 15, since=t_sntp)
        db2 = obs.wait(dbirth_t, 10, since=t_sntp)
        resumed = gw.find(r"edge: clock valid \(source sntp\) after a wait of \d+ s: connecting", t1)
        v2 = volt(db2[2]) if db2 else None
        synced = metric(nb2[2], name="Node Info/Time Synced") if nb2 else None
        t_chg = time.monotonic()
        slave.set_float(4, 0, 239.0)
        dd = obs.wait(ddata_t, 10, lambda q: volt(q) is not None, since=t_chg)
        st4, _, b4 = http("POST", "/api/time", {"utc_ms": now_ms()})
        st5, _, b5 = http("GET", "/api/status")
        tm5 = (as_json(b5) or {}).get("time", {})
        nbirths = node_msgs(node, t1, ("NBIRTH",))
        check("Issue #160 SNTP late: no CONNECT and no message before it; when SNTP answers, the "
              "first NBIRTH (bdSeq 0) and DBIRTH with UTC timestamps and the current value "
              "(Voltage 238), Time Synced true, one NBIRTH only, then DDATA; POST /api/time over "
              "a clock that SNTP synchronises -> 409; /api/status source sntp",
              waiting and not before and not conns and synced_at and resumed and nb2 and db2
              and bdseq_of(nb2[2]) == 0
              and all(t >= Y2020_MS for t in stamps(nb2[2]) + stamps(db2[2]))
              and v2 is not None and abs(v2.float_value - 238.0) < 1e-3 and synced is not None
              and synced.boolean_value is True and len(nbirths) == 1 and dd and st4 == 409
              and tm5.get("source") == "sntp" and tm5.get("valid") is True,
              f"wait logged {'yes' if waiting else 'no'}; {len(before)} message(s) and "
              f"{len(conns)} CONNECT(s) before SNTP; SNTP {'yes' if synced_at else 'no'}; "
              f"{(resumed or ['no resume log'])[0]}; NBIRTH {nb2[2].timestamp if nb2 else None} "
              f"bdSeq {bdseq_of(nb2[2]) if nb2 else None}; Voltage "
              f"{round(v2.float_value, 1) if v2 else None}; Time Synced "
              f"{synced.boolean_value if synced else None}; {len(nbirths)} NBIRTH; DDATA "
              f"{'yes' if dd else 'no'}; POST {st4} {b4[:90]!r}; /api/status {tm5}")
        all_msgs = boot1 + node_msgs(node, t1)
        early = [(m[1], min(stamps(m[2]))) for m in all_msgs if min(stamps(m[2])) < Y2020_MS]
        check("Issue #160 no NBIRTH, DBIRTH, NDATA or DDATA of the node (both boots) has a payload "
              "or metric timestamp before 2020",
              all_msgs and not early, f"{len(all_msgs)} messages; before 2020: {early[:3]}")
        gw.kill()
    finally:
        SNTP_PAUSED.clear()
        slave.set_float(4, 0, 230.5)   # what the main scenario expects
        obs.close()

# Issue #117: QEMU's open_eth model (4 RX buffers without PSRAM, #113) also drops frames inside
# connections already open; slirp retransmits them about a second later, which alone breaks a
# check bounded at about 1 s although the firmware is fine. A timed measurement whose own window
# holds an "RX frame dropped" line of the device log may be measured once more; the discarded
# attempt goes to REMEASURED (results.json "remeasured"). What the check asserts does not
# change, a failure without a drop in its window is a failure, and so is the second attempt.
RX_DROP_RX = re.compile(r"opencores\.emac: .*RX frame dropped")
RX_DROP_BEFORE_S, RX_DROP_AFTER_S = 0.2, 0.5   # console latency around the harness's clock
REMEASURED = []


def rx_drops_in(lines, t0, t1):
    """The device log lines of a dropped received frame that the harness got within the
    window [t0, t1] of a measurement (its own clock, with RX_DROP_BEFORE/AFTER_S of slack)."""
    return [(t, ln) for t, ln in lines
            if t0 - RX_DROP_BEFORE_S <= t <= t1 + RX_DROP_AFTER_S and RX_DROP_RX.search(ln)]


def remeasure(check_name, what, attempt, ok, drops, detail):
    """Issue #117: whether a failed timed measurement is measured once more: only after its
    first attempt, only if it failed, and only if a frame was dropped in its window. Records
    the discarded attempt."""
    if ok or attempt != 1 or not drops:
        return False
    REMEASURED.append({"check": check_name, "measurement": what, "discarded": detail,
                       "rx_dropped": [ln.strip() for _, ln in drops]})
    print(f"[REMEASURE] {check_name}: {what}: {detail} with an RX frame dropped in its window "
          f"({drops[0][1].strip()}); measured once more", flush=True)
    return True


def slow_upload(headers, total, rate, out, plain=False):
    """Issue #106: one POST /api/firmware with a body of @total bytes sent at about @rate B/s
    (a client uploading over a slow link), over HTTPS or, with @plain, the HTTP port; stops at
    the first send error. Fills @out with the bytes sent, the reply's first line if one arrived,
    the error and the duration."""
    t0 = time.monotonic()
    out.update(sent=0, status=None, error=None)
    reply = b""
    sock = None
    try:
        port = HTTP_PORT if plain else HTTPS_PORT
        sock = (socket.create_connection(("127.0.0.1", port), timeout=10) if plain
                else tls_connect(10, "upload"))
        head = (f"POST /api/firmware HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                f"Content-Type: application/octet-stream\r\nContent-Length: {total}\r\n"
                + "".join(f"{k}: {v}\r\n" for k, v in headers.items()) + "\r\n").encode()
        sock.sendall(head)
        chunk = b"\xe9" + b"\0" * 16383
        while out["sent"] < total:
            n = min(len(chunk), total - out["sent"])
            sock.sendall(chunk[:n])
            out["sent"] += n
            sock.setblocking(False)
            try:
                reply += sock.recv(512)
            except (ssl.SSLWantReadError, BlockingIOError):
                pass
            sock.setblocking(True)
            sock.settimeout(10)
            time.sleep(n / rate)
        # the whole body went out: what a browser does next, read the reply (if not yet seen)
        while b"\r\n" not in reply:
            more = sock.recv(512)
            if not more:
                break
            reply += more
    except (OSError, ssl.SSLError) as e:
        out["error"] = type(e).__name__
        try:
            reply += sock.recv(512) if sock is not None else b""
        except Exception:   # noqa: BLE001  (the reset may have taken the reply)
            pass
    out["status"] = reply.split(b"\r\n", 1)[0].decode(errors="replace") if reply else None
    out["duration"] = time.monotonic() - t0


def check_refused_bodies(gw, since):
    """Issue #106 (GW22): a refused POST with a large body must not hold the web server.

    HTTPS (the API): without credentials (401) and from another site with the right ones (403,
    no X-Requested-With), a 1.5 MB image upload sent at about 150 KB/s (some 10 s); half a
    second into it a GET /api/status, over an HTTPS session opened before (a new handshake takes
    about as long as the bound in QEMU), must answer within 1 s, and the gateway must cut the
    upload early (the client's send fails), not drain it. In QEMU the HTTPS GET was not held
    even with the cut disabled (control run; why is not established: the server's sockets
    block with SO_RCVTIMEO, esp-tls included), so over HTTPS the cut is what shows the fix.

    Plain HTTP (the redirect port; also the API with web.https false): there the drain blocks
    on the socket for as long as data keeps coming. The same slow upload, then a GET on that
    port half a second into it: within 1 s, upload cut. Before the fix the GET waited for the
    end of the upload."""
    total, rate = 1536 * 1024, 150 * 1024
    auth = "Basic " + base64.b64encode(f"admin:{WEB_PASSWORD}".encode()).decode()
    cases = {"HTTPS 401 no credentials": ({"X-Requested-With": "e2e"}, False),
             "HTTPS 403 cross-site, right credentials": ({"Authorization": auth}, False),
             "HTTP port (redirect), 308": ({}, True)}
    res = {}
    session = _GatewayHTTPS("127.0.0.1", HTTPS_PORT, timeout=15)
    session._what = "session"

    def get_status(plain):
        t = time.monotonic()
        if plain:
            st = http("GET", "/api/status", auth=None, plain=True, timeout=15)[0]
            return st, time.monotonic() - t
        try:
            session.request("GET", "/api/status", headers={"Authorization": auth})
            r = session.getresponse()
            r.read()
            st = r.status
        except (OSError, httplib.HTTPException):
            st = None
        return st, time.monotonic() - t

    def case_ok(st, took, up, want):   # what the check asserts of each case
        return (st == want and took < 1.0 and up.get("sent", total) < total and up.get("error")
                and up.get("duration", 99) < 5)

    base_st, base = get_status(False)
    remeasured = []
    for name, (hdrs, plain) in cases.items():
        for attempt in (1, 2):
            up = {}
            t0 = time.monotonic()
            th = threading.Thread(target=slow_upload, args=(hdrs, total, rate, up, plain),
                                  daemon=True)
            th.start()
            time.sleep(0.5)
            st, took = get_status(plain)
            th.join(20)
            want = 301 if plain else 200
            res[name] = (st, took, dict(up), want)
            # issue #117: a frame dropped by QEMU's NIC model in this case's own window
            drops = rx_drops_in(list(gw.lines), t0, time.monotonic())
            if not remeasure("issue #106 refused bodies", name, attempt,
                             bool(case_ok(st, took, up, want)), drops,
                             f"GET {st} in {took:.2f} s, upload stopped after "
                             f"{up.get('sent', 0)} B in {up.get('duration', 0):.1f} s"):
                break
            remeasured.append(name)
    session.close()
    # issue #106 review: an authenticated, same-origin upload the gateway rejects is not cut:
    # 1.5 MB that is not an image, sent whole as a browser does, then the reply is read
    up = {}
    slow_upload({"Authorization": auth, "X-Requested-With": "e2e",
                 "Origin": f"https://127.0.0.1:{HTTPS_PORT}"}, total, 4 * 1024 * 1024, up)
    rejected = up.get("status") or ""
    st_after = http("GET", "/api/status")[0]
    check("Issue #106 an authenticated, same-origin 1.5 MB upload the gateway rejects (not an "
          "image) is drained in full, not cut: the client sends the whole body and then reads "
          "the 4xx with its reason; the web answers afterwards",
          rejected.startswith("HTTP/1.1 4") and up.get("sent") == total and not up.get("error")
          and st_after == 200,
          f"reply {rejected!r} after {up.get('sent')} of {total} B in {up.get('duration', 0):.1f} s "
          f"(error {up.get('error')}); GET /api/status afterwards {st_after}")
    asked, h = gw.shell("status", [r"^web\s+HTTPS .*, bodies cut [1-9]\d*, stalls \d+$"])
    cut_line = (gw.find(r"^web\s", asked) or [""])[-1]
    m = re.search(r"bodies cut (\d+)", cut_line)
    ok = base_st == 200 and m is not None and all(case_ok(*r) for r in res.values())
    check("Issue #106 a refused 1.5 MB upload (HTTPS 401 without credentials, 403 from another "
          "site; the HTTP port's redirect) is cut, not drained: a GET half a second into it "
          "answers within 1 s",
          ok, f"GET without an upload {base_st} in {base:.2f} s; " + "; ".join(
              f"{k}: GET {st} in {took:.2f} s, upload stopped after {up.get('sent', 0)} of {total} "
              f"B in {up.get('duration', 0):.1f} s ({up.get('error')}), reply seen "
              f"{up.get('status')!r}" for k, (st, took, up, _) in res.items())
          + (f"; measured again after an RX frame dropped in its window: {remeasured}"
             if remeasured else "") + f"; {cut_line.strip()}")

STALL_S = 8   # CONFIG_GW_WEB_STALL_S default (issue #110)


def check_slow_client(gw, since):
    """Issue #110 (GW22): slowloris. A client opens an HTTPS session and sends a request's
    headers one byte every 2 s (under the 5 s receive timeout, so httpd never gives up on it):
    esp_http_server's only task is held. A normal GET started a second later must be answered
    within STALL_S + 4 s: the stall watchdog shuts down the held server's sessions after
    STALL_S. Without it the GET waited for as long as the client kept trickling (here 30 s).
    Then the stacks of both web tasks, after the TLS sessions, the refused uploads and this."""
    out = {"sent": 0, "closed": None}

    def trickle():
        t0 = time.monotonic()
        try:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            sock = ctx.wrap_socket(socket.create_connection(("127.0.0.1", HTTPS_PORT), timeout=10))
            sock.sendall(f"GET /api/status HTTP/1.1\r\nHost: 127.0.0.1:{HTTPS_PORT}\r\nX-Slow: ".encode())
            while time.monotonic() - t0 < 30:
                sock.send(b"a")
                out["sent"] += 1
                time.sleep(2)
        except (OSError, ssl.SSLError) as e:
            out["closed"] = f"{type(e).__name__} after {time.monotonic() - t0:.1f} s"

    th = threading.Thread(target=trickle, daemon=True)
    th.start()
    time.sleep(1.5)
    t = time.monotonic()
    st, _, _ = http("GET", "/api/status", timeout=40)
    took = time.monotonic() - t
    th.join(35)
    held = gw.wait_log(r"HTTPS server held for more than \d+ s by a request that does not arrive", 2,
                       since)
    asked, h = gw.shell("status", [r"^web\s+HTTPS .*, stalls [1-9]\d*$", r"^stack\s+sparkplug \d+ "])
    web = (gw.find(r"^web\s", asked) or [""])[-1]
    stack = (gw.find(r"^stack\s+sparkplug", asked) or [""])[-1]
    check(f"Issue #110 slowloris: an HTTPS client trickling its headers (1 B every 2 s) does not "
          f"hold the web server: a normal GET started meanwhile answers within {STALL_S + 4} s "
          f"(stall watchdog after {STALL_S} s, logged and counted)",
          st == 200 and took <= STALL_S + 4 and held is not None and out["closed"] is not None
          and re.search(r"stalls 1$", web.strip()) is not None,
          f"GET {st} in {took:.1f} s; slow client: {out['sent']} B, connection {out['closed']}; "
          f"log {'yes' if held else 'no'}; {web.strip()}")
    check_web_stacks(stack, "after GW22 (TLS sessions, refused uploads, redirects, slow client)")

def web_stress(gw, rounds):
    """Issue #110, item 2: once, in QEMU with PSRAM, the third accepted PUT of check_csrf never
    reached httpd for 15 s, right after six cut sessions and two normal ones. Each round here:
    six forged PUTs whose bodies are cut, the three accepted PUTs of check_csrf (UI-like,
    Referer only, script), a refused 1.5 MB upload (cut) and a GET. Every accepted request must
    be answered; a failure records the time, the client's error and the shell's web line."""
    me = f"https://127.0.0.1:{HTTPS_PORT}"
    doc = node_config("GW22")
    doc["web"] = {"username": "admin", "password": "********"}
    dry = "/api/config?dry_run=1"
    failures, slow, t_start = [], 0, time.monotonic()
    for i in range(rounds):
        for _ in range(6):
            http("PUT", dry, doc, headers={"Origin": "https://evil.e2e"})
        for name, hdrs in (("UI", {"Origin": me, "Referer": me + "/"}),
                           ("Referer", {"Referer": me + "/"}), ("script", {})):
            t = time.monotonic()
            st, _, b = http("PUT", dry, doc, headers=hdrs)
            took = time.monotonic() - t
            slow += took > 5
            if st != 200:
                asked, _ = gw.shell("status", [r"^web\s"])
                line = (gw.find(r"^web\s", asked) or [""])[-1].strip()
                failures.append(f"round {i + 1} {name} PUT -> {st} after {took:.1f} s "
                                f"({b[:80]!r}); {line}")
        up = {}
        slow_upload({"X-Requested-With": "e2e"}, 1536 * 1024, 4 * 1024 * 1024, up)
        st, _, _ = http("GET", "/api/status")
        if st != 200:
            failures.append(f"round {i + 1} GET after the cut upload -> {st}")
    check(f"Issue #110 web stress: {rounds} round(s) of cut sessions and accepted PUTs, every "
          f"accepted request answered", not failures,
          f"{rounds} rounds in {time.monotonic() - t_start:.0f} s, {slow} PUT(s) over 5 s, "
          f"{sum(1 for t, _ in TLS_CONNECT_RETRIES if t >= t_start)} TLS handshake(s) repeated "
          f"(issue #113); "
          + ("; ".join(failures[:5]) or "no failure"))

def check_csrf(gw, since):
    """Issue #100 (GW22, before the login limit checks): every write needs X-Requested-With and,
    from a browser (Origin, or Referer without it), this same origin. Every forged request but one
    comes with the right credentials, as a browser that has them cached would send them, and must be
    refused before the password check (403, not counted as a failed login); one of them carries a
    wrong password, and still gets 403, not 401, with the counter at 0. The web UI's own
    requests (the header, same Origin) still work; /api/firmware takes only its two media
    types."""
    me = f"https://127.0.0.1:{HTTPS_PORT}"   # the Host header http.client sends
    doc = node_config("GW22")
    doc["web"] = {"username": "admin", "password": "********"}
    dry = "/api/config?dry_run=1"
    forged = {
        "PUT config, no header": http("PUT", dry, doc, headers={"X-Requested-With": None})[0],
        "PUT config, other Origin": http("PUT", dry, doc, headers={"Origin": "https://evil.e2e"})[0],
        "PUT config, Origin null": http("PUT", dry, doc, headers={"Origin": "null"})[0],
        "PUT config, other Referer": http("PUT", dry, doc,
                                          headers={"Referer": "https://evil.e2e/127.0.0.1"})[0],
        "PUT config, http Origin": http("PUT", dry, doc,
                                        headers={"Origin": f"http://127.0.0.1:{HTTPS_PORT}"})[0],
        "POST restart, no header": http("POST", "/api/restart", b"",
                                        headers={"X-Requested-With": None})[0],
        # the no-cors fetch of the issue: text/plain, no custom header
        "POST firmware text/plain, no header": http(
            "POST", "/api/firmware", b"\xe9" + b"\0" * 255,
            headers={"Content-Type": "text/plain", "X-Requested-With": None,
                     "Origin": "https://evil.e2e"})[0],
        "POST firmware, other Origin": http(
            "POST", "/api/firmware", {"url": "https://evil.e2e/fw.bin"},
            headers={"Origin": "https://evil.e2e"})[0],
        # wrong credentials: 403, not 401, shows the origin check runs before the password
        # check (and the failed-login counter below stays at 0)
        "PUT config, other Origin, wrong password": http(
            "PUT", dry, doc, auth=("admin", "wrong-csrf"), headers={"Origin": "https://evil.e2e"})[0],
    }
    refused_log = gw.wait_log(r"PUT /api/config\?dry_run=1 from 10\.0\.2\.2 refused: "
                              r"cross-origin request", 3, since)
    wrong_type = http("POST", "/api/firmware", b"\xe9" + b"\0" * 255,
                      headers={"Content-Type": "text/plain", "Origin": me})[0]
    asked, h = gw.shell("status", [r"^web\s+HTTPS \(HTTP redirects\), logins failed 0, .*"
                                   r"cross-site writes refused 9, bodies cut \d+, stalls 0$"])
    counted = list(h.values())[0]
    # the web UI: X-Requested-With and its own Origin (and Referer); a script: the header only
    st_ui, _, b_ui = http("PUT", dry, doc, headers={"Origin": me, "Referer": me + "/"})
    st_ref, _, b_ref = http("PUT", dry, doc, headers={"Referer": me + "/"})
    st_cli, _, b_cli = http("PUT", dry, doc)
    st_page, _, page = http("GET", "/")
    # the three writes of the page: the API helper and the PEM file load (fetch, an object of
    # headers), and the image upload (#215, XMLHttpRequest.setRequestHeader)
    sends = {"API helper": page.count(b'const h={"X-Requested-With":"gateway"}'),
             "PEM file load": page.count(b'headers:{"Content-Type":"application/x-pem-file",'
                                         b'"X-Requested-With":"gateway"}'),
             "image upload": page.count(b'x.setRequestHeader("X-Requested-With","gateway")')}
    ui_sends = page.count(b'"X-Requested-With":"gateway"') + \
        page.count(b'setRequestHeader("X-Requested-With","gateway")')
    up = gw.wait_log(r"restart requested through the REST API", 0.5, since) is None
    check("Issue #100 CSRF: writes without X-Requested-With or from another origin (Origin, "
          "Referer, 'null', http) -> 403 before the password check (also with a wrong password: "
          "403, not 401, and not a failed login; counted in the shell), nothing restarted; /api/firmware only application/json or "
          "application/octet-stream (text/plain -> 415)",
          all(v == 403 for v in forged.values()) and refused_log and wrong_type == 415
          and counted and up,
          ", ".join(f"{k} -> {v}" for k, v in forged.items()) + f"; same origin text/plain -> "
          f"{wrong_type}; still up {up}; " + " | ".join(gw.find(r"^web\s", asked)))
    check("Issue #100 the web UI still works: its requests (X-Requested-With, same Origin and "
          "Referer) are accepted, the page sends the header on every write (API helper, image "
          "upload and PEM file upload, #221), and a script with the header and no Origin is accepted",
          st_ui == 200 and (as_json(b_ui) or {}).get("saved") is False and st_ref == 200
          and st_cli == 200 and st_page == 200 and ui_sends == 3
          and all(n == 1 for n in sends.values()),
          f"UI PUT dry run {st_ui} {b_ui[:60]!r}; Referer only {st_ref} {b_ref[:60]!r}; no Origin "
          f"{st_cli} {b_cli[:60]!r}; page "
          f"{st_page} with the header in {ui_sends} request(s) {sends}")


# ---------------------------------------------------------------- scenario

# ---------------------------------------------------------------- FW-1 firmware updates

APP_DESC_MAGIC = 0xABCD5432


# Issue #107: a 1 MB web upload is answered once the image is written and verified: with the
# PSRAM build (software AES and SHA) and single-threaded TCG that took about 150 s, about 25 s
# with MTTCG. Issue #115: no fixed timeout then (300 s had little margin on a loaded host): the
# upload waits as long as the gateway makes progress, and fails after UPLOAD_STALL_S without
# any, or UPLOAD_CAP_S in all (both times --timeout-scale). Progress is what the gateway reports:
# the bytes written so far in the shell's `status` (asked every UPLOAD_STATUS_S; the client's
# own sending says nothing, slirp takes the whole megabyte at once), then the lines of the image
# check on the console. The stall bound is above the harness' own 60 s silence limit in an image
# check, which catches a hang in the verification first.
UPLOAD_STALL_S = 90
UPLOAD_CAP_S = 900
UPLOAD_STATUS_S = 10
UPLOAD_CHUNK = 16384
UPLOAD_PROGRESS_RX = re.compile(r"fw: |esp_image: |esp_ota_ops: |secure_boot_v2: |"
                                r"web: firmware upload")
UPLOAD_BYTES_RX = re.compile(r"^ota\s.*\((\d+) of \d+ bytes\)")


def wait_progress(ready, token, stall_s, cap_s, poll_s=1.0, clock=time.monotonic,
                  sleep=time.sleep):
    """Issue #115: wait until ready() is true ("ready"), as long as token() keeps changing: "stall"
    once it has not changed for @stall_s, "cap" after @cap_s in all."""
    start = last_at = clock()
    last = token()
    while True:
        if ready():
            return "ready"
        now = clock()
        cur = token()
        if cur != last:
            last, last_at = cur, now
        elif now - last_at >= stall_s:
            return "stall"
        if now - start >= cap_s:
            return "cap"
        sleep(poll_s)


def upload_firmware(gw, data, auth=("admin", WEB_PASSWORD), force=False, sha256=None):
    """POST /api/firmware with the image @data (@force: ?force=1, issue #144; @sha256: the header
    X-Image-SHA256, issue #215), as http() does it (Basic auth, the CSRF header,
    the device certificate into PEER_CERT): (status, headers dict, body bytes), or (None, {},
    reason) when the request failed, stalled or ran past its cap (issue #115, wait_progress()).
    Progress: the bytes written as @gw's shell reports them, and its console lines of the upload
    and the image check. Raises AttemptAborted after an emulator fault, like a wait."""
    scale = ARGS.timeout_scale if ARGS is not None else 1.0
    stall_s, cap_s = UPLOAD_STALL_S * scale, UPLOAD_CAP_S * scale
    h = {"X-Requested-With": "e2e", "Content-Type": "application/octet-stream",
         "Content-Length": str(len(data))}
    if sha256 is not None:
        h["X-Image-SHA256"] = sha256
    if auth is not None:
        h["Authorization"] = "Basic " + base64.b64encode(f"{auth[0]}:{auth[1]}".encode()).decode()
    first_line = len(gw.lines)
    sent, result, conn = [0], {}, {}

    def run():
        try:
            c = _GatewayHTTPS("127.0.0.1", HTTPS_PORT, timeout=cap_s)   # issue #116
            path = "/api/firmware?force=1" if force else "/api/firmware"
            c._what = f"POST {path}"
            conn["c"] = c
            c.putrequest("POST", path, skip_accept_encoding=True)
            for k, v in h.items():
                c.putheader(k, v)
            c.endheaders()
            PEER_CERT["der"] = c.sock.getpeercert(binary_form=True)
            for i in range(0, len(data), UPLOAD_CHUNK):
                c.send(data[i:i + UPLOAD_CHUNK])
                sent[0] = min(len(data), i + UPLOAD_CHUNK)
            r = c.getresponse()
            body = r.read()
            c.close()
            result["r"] = (r.status, {k.lower(): v for k, v in r.getheaders()}, body)
        except (OSError, httplib.HTTPException, ssl.SSLError) as e:
            result["r"] = (None, {}, str(e).encode())

    def lines():
        return sum(1 for _, ln in list(gw.lines)[first_line:] if UPLOAD_PROGRESS_RX.search(ln))

    asked = [0.0]

    def written():
        """The bytes written so far, from the shell's latest `status` (asked again every
        UPLOAD_STATUS_S)."""
        if time.monotonic() - asked[0] >= UPLOAD_STATUS_S:
            asked[0] = time.monotonic()
            try:
                gw.console("status")
            except QemuGone:
                pass   # the fault monitor reports it; the wait raises
        got = [int(m.group(1)) for _, ln in list(gw.lines)[first_line:]
               if (m := UPLOAD_BYTES_RX.search(ln))]
        return max(got, default=0)

    worker = threading.Thread(target=run, daemon=True)
    worker.start()
    how = "aborted"
    try:
        how = wait_progress(lambda: wait_left(math.inf) > 0 and not worker.is_alive(),
                            lambda: (written(), lines()), stall_s, cap_s)
    finally:
        if how != "ready":   # unblock the worker: the request is given up
            c = conn.get("c")
            if c is not None and c.sock is not None:
                try:
                    c.sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            worker.join(10)
    if how == "ready":
        return result.get("r", (None, {}, b"no answer"))
    what = (f"no progress for {stall_s:.0f} s" if how == "stall" else
            f"still running after {cap_s:.0f} s")
    return None, {}, (f"upload {what}: {sent[0]} of {len(data)} bytes sent, {written()} written "
                      f"(shell), {lines()} upload line(s) on the console").encode()


CAPS_OFFSET = 288   # gw_ota.h GW_OTA_CAPS_OFFSET: the capabilities block after esp_app_desc_t
# Issue #144: a new image is confirmed by its self-check at SELFCHECK_S of uptime
# (CONFIG_GW_FW_SELFCHECK_S, the FreeRTOS tick of the boot; read from --sdkconfig in main(): 30 s in
# the QEMU build since #233, 60 s on the board). SELFCHECK_WAIT_S bounds the wait for its
# "installed" state after its NBIRTH (or its boot): the self-check time plus SELFCHECK_MARGIN_S.
SELFCHECK_MARGIN_S = 90
SELFCHECK_S = 30
SELFCHECK_WAIT_S = SELFCHECK_S + SELFCHECK_MARGIN_S


def image_cfg_schema(src):
    """The configuration schema that the app image @src declares in its capabilities block
    (GW_CONFIG_SCHEMA of its build, issue #144)."""
    b = pathlib.Path(src).read_bytes()
    if struct.unpack_from("<I", b, CAPS_OFFSET)[0] != 0x50435747:
        raise SystemExit(f"{src}: no capabilities block at {CAPS_OFFSET}")
    return struct.unpack_from("<H", b, CAPS_OFFSET + 12)[0]


def fw_image(src, version, dst, corrupt=False, caps_version=None, cfg_schema=None):
    """Copy of the app image @src with esp_app_desc_t.version = @version: checksum and the
    appended SHA-256 recomputed so the bootloader and esp_ota_end() accept it (@corrupt: the
    checksum is wrong, so the image must be rejected). Issue #144: @caps_version and @cfg_schema
    rewrite those fields of the capabilities block (an image built before the pre-flight check
    declares version 1; a downgrade declares an older schema). @src must be unsigned."""
    b = bytearray(pathlib.Path(src).read_bytes())
    if b[0] != 0xE9:
        raise SystemExit(f"{src}: not an ESP app image")
    nseg, hashed = b[1], b[23] == 1
    pos = 24
    first = pos + 8
    for _ in range(nseg):
        pos += 8 + struct.unpack_from("<I", b, pos + 4)[0]
    csum = pos + (15 - pos % 16)
    if struct.unpack_from("<I", b, first)[0] != APP_DESC_MAGIC:
        raise SystemExit(f"{src}: no app description in the first segment")
    vo = first + 16
    new = version.encode().ljust(32, b"\0")
    x = 0
    for o, n in zip(b[vo:vo + 32], new, strict=True):
        x ^= o ^ n
    b[vo:vo + 32] = new
    for off, value in ((4, caps_version), (12, cfg_schema)):   # u16 fields of the block
        if value is not None:
            at = CAPS_OFFSET + off
            if struct.unpack_from("<I", b, CAPS_OFFSET)[0] != 0x50435747:
                raise SystemExit(f"{src}: no capabilities block at {CAPS_OFFSET}")
            old = bytes(b[at:at + 2])
            struct.pack_into("<H", b, at, value)
            for o, n in zip(old, b[at:at + 2], strict=True):
                x ^= o ^ n
    # the checksum of the new version; a corrupt image gets it off by one bit (the stale one
    # matches whenever the old and new version strings XOR to 0, about one build in 256)
    b[csum] ^= x ^ (0x01 if corrupt else 0)
    end = csum + 1
    if hashed:
        b[end:end + 32] = hashlib.sha256(bytes(b[:end])).digest()
        end += 32
    pathlib.Path(dst).write_bytes(bytes(b[:end]))
    return dst


class FirmwareServer:
    """HTTPS file server for the update images (FW-1), with the harness' CA (server.crt is for
    IP 10.0.2.2, the host as QEMU's slirp sees it)."""

    def __init__(self, port, root, cert, key):
        import http.server
        root = str(root)

        class Handler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **kw):
                super().__init__(*a, directory=root, **kw)

            def log_message(self, fmt, *a):
                FirmwareServer.log.append(fmt % a)

            def do_GET(self):  # noqa: N802 (http.server API)
                cut = FirmwareServer.cuts.get(self.path)
                if cut is None:
                    return super().do_GET()
                # issue #98: the whole Content-Length announced, the connection closed after
                # @n bytes of the body (a server or a network that goes away mid-transfer)
                data, n = pathlib.Path(root, cut[0]).read_bytes(), cut[1]
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data[:n])
                self.wfile.flush()
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(str(cert), str(key))
        self.srv.socket = ctx.wrap_socket(self.srv.socket, server_side=True)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.closed = False
        track(self, self.close)   # issue #105

    log = []
    cuts = {}   # path -> (file in root, bytes of it sent before the connection is closed)

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.srv.shutdown()
        self.srv.server_close()


def observer_retry(port, timeout=10, auth=None):
    """Observer on a broker that was just (re)started: it may not listen yet."""
    end = time.monotonic() + timeout
    while True:
        try:
            return Observer(port, auth)
        except OSError:
            if time.monotonic() >= end:
                raise
            time.sleep(0.3)


def fw_state(p):
    m = metric(p, alias=41)
    return m.string_value if m is not None else None


PARK_RX = re.compile(r"fw: parking core 1 \(")
RESUMED_RX = re.compile(r"fw: core 1 resumed after (\d+) ms")
STORM_RX = re.compile(r"bus: console storm from core 1, line (\d+)")
PARK_BOUND_RX = re.compile(r"fw: core 1 park (bound|refused)|fw: blocking IPC to core 1 while it")


def run_park_storm(gw, image, obs, ndata_t):
    """Issue #111 (QEMU build: CONFIG_GW_FW_PARK_OTHER_CORE): the two parks of a web upload
    (esp_ota_begin(), then the verification) with core 1 parked while the bus task (pinned to core
    1) floods the console (emulator command `storm`). Before the fix the park preempted that task
    in the middle of a log line, with the console's locks taken, and the verifying task blocked on
    its next line for good. Now the bus task stops between two transactions before core 1 is
    parked: the upload is answered (the image is rejected), no storm line comes from a park
    (between its 'parking' and 'resumed' lines), the storm goes on after it, and no park reached
    its bound. Issue #114: the storm runs right before each park, not through the whole transfer:
    with single-threaded TCG (#107) and PSRAM a storm through it slowed a 1 MB upload past 200 s
    of guest time. It runs from before the upload until its first park is over, and again from
    80 % of the transfer (Firmware Update/Progress) until the answer."""
    def started(since):   # the shell's answer to a storm command
        return gw.wait_log(r"console storm from the bus task for \d+ ms", 15, since)

    obs.drain()   # no NDATA left from the downloads before (their Progress reached 100)
    asked = time.monotonic()
    gw.console("storm 600000")
    first_on = started(asked)
    storming = gw.wait_log(STORM_RX.pattern, 15, asked)
    result = {}

    def post():
        result["r"] = upload_firmware(gw, image.read_bytes())
        result["t"] = time.monotonic()

    poster = threading.Thread(target=post, daemon=True)
    poster.start()
    receiving = gw.wait_log(r"fw: receiving a firmware upload", UPLOAD_STALL_S * ARGS.timeout_scale,
                            asked)
    gw.console("storm 0")   # the transfer itself without the storm
    paused = gw.wait_log(r"console storm stopped", 15, receiving or asked)
    # an NDATA carries the State only when its text changes ("receiving e2e-bad" at 10 %), the
    # Progress (alias 42) at each step: this upload is the only transfer running
    # (issue #115: for as long as the upload runs, its own helper bounds it)
    late = None
    while late is None and poster.is_alive():
        late = obs.wait(ndata_t, 2, lambda p: metric(p, alias=42) is not None
                        and (metric(p, alias=42).long_value or metric(p, alias=42).int_value) >= 80)
    again = time.monotonic()
    gw.console("storm 600000")
    second_on = started(again)
    poster.join()
    st, _, body = result.get("r", (None, {}, b"no answer"))
    answered = result.get("t", time.monotonic())
    after = gw.wait_log(STORM_RX.pattern, 15, answered)
    gw.console("storm 0")
    gw.wait_log(r"console storm stopped", 10, answered)
    rejected = gw.wait_log(r"fw: firmware update failed: image e2e-bad rejected", 10, asked)
    # in console order: each park, the storm lines inside it, those before it since the storm
    # was (re)started, and those after the last one
    parks, inside, before, open_at, last_resume = [], 0, [], None, None
    lines = [(t, ln) for t, ln in list(gw.lines) if t >= asked]
    storm_lines, since_on = 0, 0
    for _t, ln in lines:
        if re.search(r"console storm from the bus task for \d+ ms", ln):
            since_on = 0
        elif PARK_RX.search(ln):
            open_at = len(parks)
            parks.append(None)
            before.append(since_on)
            since_on = 0
        elif (m := RESUMED_RX.search(ln)) and open_at is not None:
            parks[open_at] = int(m.group(1))
            open_at, last_resume = None, storm_lines
        elif STORM_RX.search(ln):
            storm_lines += 1
            since_on += 1
            if open_at is not None:
                inside += 1
    after_park = storm_lines - (last_resume or 0) if last_resume is not None else 0
    bound = [ln for _, ln in lines if PARK_BOUND_RX.search(ln)]
    check("Issue #111 core 1 parked for the verification of a web upload while the bus task "
          "(core 1) floods the console: the bus task stops first, between two transactions, so the "
          "verification never waits on a lock left on core 1; the upload is answered (image "
          "rejected), both parks of the upload come during the storm, no console line from core 1 "
          "while it is parked, the storm goes on after it, no park bound reached",
          first_on and storming and receiving and paused and late and second_on and st == 400
          and isinstance(body, bytes) and b"rejected" in body and rejected and len(parks) == 2
          and None not in parks and all(n > 0 for n in before) and inside == 0 and after
          and after_park > 0 and not bound,
          f"POST {st} {body[:70]!r} {answered - asked:.1f} s after the storm began; parks "
          f"{parks} ms, after {before} storm lines each; {storm_lines} storm lines, {inside} while "
          f"parked, {after_park} after the last park; storm again at "
          f"{'80 %' if late else 'no 80 % progress'}; {bound[:1]}")


POST_PARK_ATTEMPT_S = 90   # issue #114: bound for the next attempt to unit 1 after the parks


def run_park_long_txn(args, work, slave):
    """Issue #112 (QEMU build): a park of core 1 while the bus task is in a long RS485
    transaction. GW47 polls with the longest response timeout (5 s) and the most retries (5): a
    request that gets no answer takes 6 attempts, over 30 s. Meter1 (unit 1) stops answering, and
    once its next request is on the bus a corrupt image is uploaded through the web (two parks:
    esp_ota_begin(), the verification). The bus task stops between two attempts, so neither park
    waits for the whole transaction: before, a park waited at most 10 s and was refused
    ('fw: core 1 park refused'). The upload is answered (rejected), every park ended, and the
    transaction went on after the parks (more attempts to unit 1)."""
    node, port = "GW47", MQTT_PORT + 27
    fwdir = work / "fw-longtxn"
    fwdir.mkdir(exist_ok=True)
    image = signed_image(args, app_source(args), "e2e-long", fwdir, "long", corrupt=True)
    conf = work / "broker-longtxn.conf"
    conf.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\n")
    broker = start_broker(conf, work / "mosquitto-longtxn.log")
    obs = Observer(port)
    cfg = node_config(node, port=port)
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    cfg["rs485"].update({"response_timeout_ms": 5000, "retries": 5})
    gw = boot_variant(args, work, "longtxn", cfg)
    muted = False
    try:
        nb = obs.wait(f"{T}/NBIRTH/{node}", 90)
        up = wait_http("/api/firmware", 60)
        if not check("Issue #112 GW47 runs with a 5 s response timeout and 5 retries",
                     nb and up[0] == 200, f"NBIRTH {'yes' if nb else 'no'}, web {up[0]}"):
            return
        n0 = len(slave.reads)
        del slave.server_ctx[1]   # Meter1 stops answering (ignore_missing_slaves: no answer)
        muted = True
        asked = time.monotonic()
        def unit1_attempts():
            return [t for (u, *_), t in zip(slave.reads[n0:], slave.times[n0:], strict=False) if u == 1]

        def until(cond, seconds):   # a harness wait: ends the attempt after an emulator fault
            end = time.monotonic() + seconds
            while not cond() and wait_left(end) > 0:
                time.sleep(0.2)
            return cond()

        until(unit1_attempts, 20)
        first = (unit1_attempts() or [None])[0]
        until(lambda: False, 1.0)   # the first attempt of that request is under way
        st, _, body = upload_firmware(gw, image.read_bytes())
        answered = time.monotonic()
        gw.wait_log(r"fw: firmware update failed: image e2e-long rejected", 20, asked)
        lines = [(t, ln) for t, ln in list(gw.lines) if t >= asked]
        parks = [t for t, ln in lines if PARK_RX.search(ln)]
        resumed = [int(m.group(1)) for _, ln in lines if (m := RESUMED_RX.search(ln))]
        refused = [ln for _, ln in lines if PARK_BOUND_RX.search(ln)]
        # Issue #114: the next attempt to unit 1 after the parks, waited for up to a bound, not
        # in a fixed window: the rest of the transaction (attempts 5 s apart), or, once it is
        # over, the next poll of Meter1 behind the probes of Dead3..Dead5 (5 s each), or its own
        # offline probe (every 10 s at most): under 40 s of guest time, and the guest runs
        # slower than the host under load (#107)
        until(lambda: parks and any(t > parks[-1] for t in unit1_attempts()),
              POST_PARK_ATTEMPT_S * ARGS.timeout_scale)
        unit1 = unit1_attempts()
        after = [t for t in unit1 if parks and t > parks[-1]]
        check("Issue #112 core 1 parked while the bus task is in an RS485 transaction of over 30 s "
              "(no answer, 5 s timeout, 5 retries): the bus task stops between two attempts, so no "
              "park is refused; the upload is answered (image rejected) and the transaction goes on "
              "after the parks",
              first is not None and parks and first < parks[0] and st == 400
              and isinstance(body, bytes) and b"rejected" in body and len(parks) >= 2
              and len(resumed) == len(parks) and not refused and after,
              f"POST {st} {body[:60]!r} {answered - asked:.1f} s after the mute; first park "
              f"{parks[0] - first:.1f} s into the transaction; parks {resumed} ms; "
              f"{len(unit1)} attempts to unit 1 ({len(after)} after the parks); {refused[:1]}"
              if first is not None and parks else
              f"POST {st} {body[:60]!r}; request to unit 1 {first}; parks {len(parks)}; "
              f"{refused[:1]}")
    finally:
        if muted:
            slave.server_ctx[1] = slave.ctx
        gw.kill()
        obs.close()
        broker.terminate()
        broker.wait()


def run_ota(args, work, part):
    """FW-1 in one gateway with its own broker and HTTPS image server. Issue #233: two scenarios,
    each with its own boot, so that the CI can run them in different groups.
    @part "ncmd" (FW-1 OTA by NCMD, GW40): the NBIRTH metrics of an update; updates by NCMD
    (HTTPS download with the CA from ota.ca_file); bad images and bad URLs rejected without a
    restart; the parks of a rejected upload (#111) and a download cut in the middle (#98); then an
    update by NCMD whose new image on trial is installed by its self-check (issue #144: NBIRTH
    "testing <v>", then "installed <v>", and "installed <v>" again after a power cut).
    @part "web" (FW-1 OTA by web upload, GW45): an upload whose SHA-256 does not agree (#215); an
    upload on trial: the other-slot button, NCMD Boot Other Slot and a configuration save are
    refused, and the self-check installs it; a reset during the trial of the next update (panic)
    returns to the earlier image, whose NBIRTH gives the cause; the "Restart into <version>"
    button boots that rejected image of the other slot on trial."""
    if part == "ncmd":
        node, port, fw_port = "GW40", MQTT_PORT + 20, MQTT_PORT + 21
    else:
        node, port, fw_port = "GW45", MQTT_PORT + 18, MQTT_PORT + 19
    pki = make_pki(work)
    fwdir = work / f"fw-{part}"
    fwdir.mkdir(exist_ok=True)
    src = app_source(args)
    # issue #261: the signature check of an image (FW-2) is in run_ota_signature (secure build)
    images = {k: signed_image(args, src, f"e2e-{k}", fwdir, k, corrupt=k == "bad")
              for k in ("v2", "v3", "bad")}
    server = FirmwareServer(fw_port, fwdir, pki["server.crt"], pki["server.key"])
    conf = work / "broker-ota.conf"
    conf.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\n")
    broker = start_broker(conf, work / "mosquitto-ota.log")
    obs = Observer(port)
    cfg = node_config(node, port=port)
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    # #223: the updates by NCMD need ota.mqtt (off by default)
    cfg["ota"] = {"ca_file": "ca.pem", "mqtt": True}
    t0 = time.monotonic()
    gw = boot_variant(args, work, f"ota-{part}", cfg, {"ca.pem": pki["ca.pem"].read_bytes()})
    nbirth_t, ndata_t, ncmd_t = f"{T}/NBIRTH/{node}", f"{T}/NDATA/{node}", f"{T}/NCMD/{node}"
    try:
        nb = obs.wait(nbirth_t, 40)
        if part == "web" and not check(f"FW-1 {node} runs on its broker with ota.mqtt",
                                       nb is not None and metric(nb[2], alias=40) is not None
                                       and fw_state(nb[2]) == "idle",
                                       f"state {fw_state(nb[2])!r}" if nb else "no NBIRTH"):
            return
        if part == "ncmd" and not check(
                     "FW-1 NBIRTH declares Node Control/Firmware Update (String) and the update's "
                     "state and progress; with ota.mqtt (#223) also Node Control/Firmware SHA-256 "
                     "(String, #215; tck-id-topics-nbirth-metrics)",
                     nb and metric(nb[2], alias=40) is not None
                     and metric(nb[2], alias=40).datatype == 12 and fw_state(nb[2]) == "idle"
                     and metric(nb[2], alias=42) is not None
                     and metric(nb[2], name="Node Control/Firmware SHA-256") is not None
                     and metric(nb[2], name="Node Control/Firmware SHA-256").datatype == 12
                     and metric(nb[2], name="Node Control/Firmware SHA-256").alias == 44,
                     f"state {fw_state(nb[2])!r}" if nb else "no NBIRTH"):
            return
        gw.wait_log(r"edge: time synchronised", 10, t0)   # the image server's certificate dates

        def update(url, sha256=None):
            # issue #215: the SHA-256 after the URL in the same NCMD still applies to it
            obs.publish_cmd(ncmd_t, [{"name": "Node Control/Firmware Update", "datatype": 12,
                                      "string_value": url}]
                            + ([{"name": "Node Control/Firmware SHA-256", "datatype": 12,
                                 "string_value": sha256}] if sha256 is not None else []))

        def states(since):
            return [fw_state(m[2]) for m in CAPTURE
                    if m[0] >= since and m[1] in (ndata_t, nbirth_t) and fw_state(m[2]) is not None]

        if part == "web":
            # #233: on its own boot the uploads come right after the NBIRTH; the HTTPS server
            # answers before it registers the handlers (a reset, no 400): wait for the evidence
            gw.wait_log(r"web: web UI and REST API on port 443", 30, t0)
            run_ota_web(gw, obs, cfg, images, node, fw_port, nb, update)
            return

        # refused before any download: plain HTTP; an image with a stale checksum: rejected after
        # the download, no restart
        asked = time.monotonic()
        update(f"http://10.0.2.2:{fw_port}/v2.bin")
        refused = obs.wait(ndata_t, 10, lambda p: (fw_state(p) or "").startswith(
            "failed: only https"))
        update(f"https://10.0.2.2:{fw_port}/bad.bin")
        bad = obs.wait(ndata_t, 120, lambda p: (fw_state(p) or "").startswith("failed: image e2e-bad "
                                                                             "rejected"))
        time.sleep(2)
        still = not any(m[1] == f"{T}/NDEATH/{node}" for m in CAPTURE if m[0] >= asked)
        check("FW-1 bad updates refused without a restart: http:// refused, an image that fails "
              "verification rejected after the download (the running firmware stays)",
              refused and bad and still,
              " | ".join(dict.fromkeys(states(asked))) + f"; restarted: {not still}")

        run_park_storm(gw, images["bad"], obs, ndata_t)

        # Issue #98: a download cut in the middle (the server closes after CUT bytes of the
        # announced image): failed with the bytes actually received, no panic, no restart
        cut_n = 300_000
        FirmwareServer.cuts["/cut.bin"] = ("v2.bin", cut_n)
        asked = time.monotonic()
        update(f"https://10.0.2.2:{fw_port}/cut.bin")
        cut = obs.wait(ndata_t, 120, lambda p: (fw_state(p) or "").startswith("failed: download of "
                                                                             "e2e-v2 interrupted"))
        # the console line may come after the NDATA (115200 baud, the reader thread): wait for it
        gw.wait_log(r"fw: firmware update failed: download of e2e-v2 interrupted after", 10, asked)
        line = gw.find(r"fw: firmware update failed: download of e2e-v2 interrupted after (\d+) "
                       r"bytes", asked)
        got = int(re.search(r"after (\d+) bytes", line[0]).group(1)) if line else -1
        time.sleep(2)
        crashed = gw.find(r"Guru Meditation|abort\(\)|task_wdt|rst:0x(?!1 )", asked)
        restarted = any(m[1] == f"{T}/NDEATH/{node}" for m in CAPTURE if m[0] >= asked)
        _, st_cut = gw.shell("status", [r"^ota\s+.*\| download of e2e-v2 interrupted after \d+ bytes"])
        check("Issue #98 a download cut mid-transfer (the server closes after "
              f"{cut_n} bytes): state failed with a plausible byte count (read before "
              "esp_https_ota_abort() frees the handle), no panic, no restart",
              cut and 0 < got <= cut_n and got >= cut_n - 65536 and not crashed and not restarted
              and list(st_cut.values())[0],
              f"state {fw_state(cut[2]) if cut else None!r}; {got} bytes counted of {cut_n} sent; "
              f"crash {crashed[:1]}; restarted {restarted}")

        # Issue #87: URLs longer than 63 characters arrive whole (up to 255 for an update);
        # longer ones are refused with a clear reason, never cut
        def long_url(n):
            base = f"https://10.0.2.2:{fw_port}/v2.bin?pad="
            return base + "p" * (n - len(base))

        if args.simulate_qemu_death == "ota-failed" and not RETRIES and not SIMULATED:
            # issue #96 self-test: a firmware failure before the emulator dies is never retried
            SIMULATED.append("ota-failed")
            check("issue #96 self-test: a check that fails before QEMU dies (expected FAIL)", False,
                  "simulated firmware failure")
        if args.simulate_qemu_death == "ota-panic" and not RETRIES and not SIMULATED:
            # issue #96 M3 self-test: a firmware panic that matches no emulator signature (here
            # fed to the console reader as if the emulator had printed it) forbids the retry
            SIMULATED.append("ota-panic")
            for ln in ("Guru Meditation Error: Core  0 panic'ed (StoreProhibited). Exception was "
                       "unhandled.", "PC      : 0x42012345  PS      : 0x00060030",
                       "EXCVADDR: 0x00000004  LBEG    : 0x00000000"):
                gw._line(time.monotonic(), ln)
        if args.simulate_qemu_death in ("ota", "ota-failed", "ota-panic") and not RETRIES:
            # issue #105 self-test: an observer on the main broker that the exception below
            # leaves subscribed (run_scenario must close it, or every later scenario on that
            # broker sees each message twice)
            Observer()
            # issue #86 self-test: the emulator dies as it did in the real aborts (SIGABRT)
            gw.proc.send_signal(signal.SIGABRT)
            gw.proc.wait()
            raise ConnectionError("simulated QEMU death")
        asked = time.monotonic()
        update(long_url(300))   # over the codec's 256: the NCMD is undecodable, nothing applied
        undecodable = gw.wait_log(r"NCMD: undecodable payload \(\d+ bytes: string overflow; strings up "
                                  r"to 256 characters", 10, asked)
        update(long_url(256))   # fits the codec, not an update URL (255 at most)
        too_long = obs.wait(ndata_t, 10, lambda p: (fw_state(p) or "") ==
                            "failed: URL longer than 255 characters")
        check("Issue #87 a firmware URL over the limits is refused with its reason, never cut: 300 "
              "characters (more than the codec's 256) -> NCMD undecodable, 256 -> refused (255 at "
              "most)", undecodable and too_long,
              f"{(gw.find(r'NCMD: undecodable payload', asked) or ['no log'])[0][:140]}; state "
              f"{fw_state(too_long[2]) if too_long else None!r}")

        # NCMD with the URL: download with progress, restart into it, on trial ("testing"), then
        # installed by its self-check (issue #144); a 250-character URL (issue #87: whole)
        asked = time.monotonic()
        v2_sha = hashlib.sha256(images["v2"].read_bytes()).hexdigest()
        update(long_url(250), v2_sha.upper())   # issue #215: capital letters too
        nb2 = obs.wait(nbirth_t, 240, lambda p: metric(p, name="Node Info/Firmware").string_value
                       == "e2e-v2")
        conf2 = obs.wait(ndata_t, SELFCHECK_WAIT_S, lambda p: fw_state(p) == "installed e2e-v2")
        seen = states(asked)
        prog = [int(metric(m[2], alias=42).long_value or metric(m[2], alias=42).int_value)
                for m in CAPTURE if m[0] >= asked and m[1] == ndata_t and metric(m[2], alias=42)]
        took = [ln for ln in gw.find(r"fw: downloading firmware e2e-v2", asked)]
        selfcheck = gw.find(r"fw: firmware e2e-v2 installed: the self-check passed in normal mode",
                            asked)
        agrees = gw.find(rf"fw: image e2e-v2: the SHA-256 agrees \({v2_sha}\)", asked)
        check("FW-1 update by NCMD over HTTPS (CA from ota.ca_file, a 250-character URL): progress "
              "as NDATA, restart into the new image, on trial (NBIRTH 'testing e2e-v2'), then "
              "installed by its self-check in normal mode (issue #144); issue #215: the NCMD "
              "Firmware SHA-256 (capital letters, after the URL) is checked before the boot",
              nb2 and conf2 and "restarting into e2e-v2" in seen and fw_state(nb2[2]) ==
              "testing e2e-v2" and prog and max(prog) == 100 and len(prog) >= 5 and took
              and selfcheck and agrees,
              f"states {list(dict.fromkeys(seen))}; progress {prog[:12]}; "
              f"{nb2[0] - asked:.1f} s to the new NBIRTH; self-check {bool(selfcheck)}"
              if nb2 else f"states {seen}")
        _, h = gw.shell("status", [r"^ota\s+e2e-v2 in ota_1, other slot \S+ \(previous\) \| "
                                   r"firmware e2e-v2 installed"])
        # a power cut after the confirmation: still the new image, and its NBIRTH publishes the
        # result of the last update (issue #144 item 5)
        gw.kill()
        obs.drain()
        gw = Gateway(args, work / "uart0-ota-ncmd.log", gw.flash)
        gw.start()
        nb3 = obs.wait(nbirth_t, 40)
        check("FW-1 a confirmed image stays after a power cut (shell shows the slots); issue #144: "
              "the NBIRTH after the restart publishes the last result ('installed e2e-v2')",
              nb3 and metric(nb3[2], name="Node Info/Firmware").string_value == "e2e-v2"
              and fw_state(nb3[2]) == "installed e2e-v2" and list(h.values())[0],
              f"NBIRTH firmware {metric(nb3[2], name='Node Info/Firmware').string_value!r}, state "
              f"{fw_state(nb3[2])!r}; shell {list(h.values())}" if nb3 else "no NBIRTH")

    finally:
        gw.kill()
        obs.close()
        server.close()
        broker.terminate()
        broker.wait()


def run_ota_web(gw, obs, cfg, images, node, fw_port, nb, update):
    """Issue #233: the web part of FW-1 OTA (run_ota part "web") on the running gateway @gw (its
    first NBIRTH @nb). The image of the build runs in ota_0: the upload goes to ota_1, and the
    next update by NCMD (@update) to ota_0."""
    nbirth_t, ndata_t, ncmd_t = f"{T}/NBIRTH/{node}", f"{T}/NDATA/{node}", f"{T}/NCMD/{node}"
    base = metric(nb[2], name="Node Info/Firmware").string_value
    # issue #215: an upload whose SHA-256 does not agree is not installed: 422 with the two
    # values (also in GET /api/firmware), no restart; a wrong format is refused at once
    v3 = images["v3"].read_bytes()
    v3_sha = hashlib.sha256(v3).hexdigest()
    wrong = "0" * 63 + "1"
    asked = time.monotonic()
    st_f, _, b_f = upload_firmware(gw, v3[:4096], sha256="12 34")
    st_w, _, b_w = upload_firmware(gw, v3, sha256=wrong)
    fw_w = as_json(http("GET", "/api/firmware")[2]) or {}
    jw = as_json(b_w) or {}
    time.sleep(2)
    restarted = any(m[1] == f"{T}/NDEATH/{node}" for m in CAPTURE if m[0] >= asked)
    mism = obs.wait(ndata_t, 10, lambda p: (fw_state(p) or "") ==
                    "failed: image e2e-v3: the SHA-256 of the file does not agree")
    check("Issue #215 an upload with X-Image-SHA256: a wrong format -> 400; a SHA-256 that does "
          "not agree -> 422 with the two values (GET /api/firmware too), state failed, the "
          "image is not installed and the gateway does not restart",
          st_f == 400 and b"X-Image-SHA256: expected 64 hexadecimal characters" in b_f
          and st_w == 422 and jw.get("sha256_expected") == wrong
          and jw.get("sha256_file") == v3_sha and fw_w.get("sha256_expected") == wrong
          and fw_w.get("sha256_file") == v3_sha and fw_w.get("phase") == "failed"
          and fw_w.get("mqtt") is True and mism and not restarted,
          f"format {st_f} {b_f[:100]!r}; mismatch {st_w} {b_w[:240]!r}; GET phase "
          f"{fw_w.get('phase')!r} expected {fw_w.get('sha256_expected')!r} file "
          f"{fw_w.get('sha256_file')!r} mqtt {fw_w.get('mqtt')!r}; NDATA {mism is not None}; "
          f"restarted {restarted}")
    # issue #144 items 1 and 6: a web upload of v3; during its trial the other-slot button
    # (web and NCMD) and a configuration save are refused; the self-check installs it
    asked = time.monotonic()
    st, _, body = upload_firmware(gw, v3, sha256=v3_sha)
    nb4 = obs.wait(nbirth_t, 240, lambda p: metric(p, name="Node Info/Firmware").string_value
                   == "e2e-v3")
    st_b, _, b_b = http("POST", "/api/firmware/other", b"", timeout=30)
    obs.publish_cmd(ncmd_t, [{"name": "Node Control/Boot Other Slot", "datatype": 11,
                              "boolean_value": True}])
    ncmd_refused = gw.wait_log(r"edge: NCMD Boot Other Slot refused: testing e2e-v3: \d+ s left",
                               20, nb4[0] if nb4 else asked)
    st_c, _, b_c = http("PUT", "/api/config", cfg, timeout=30)
    _, h_t = gw.shell("status", [rf"^ota\s+e2e-v3 in ota_1, other slot {re.escape(base)} "
                                 r"\(previous\) \| .*\(testing: it must reach normal mode, \d+ s "
                                 r"left\)$"])
    conf3 = obs.wait(ndata_t, SELFCHECK_WAIT_S, lambda p: fw_state(p) == "installed e2e-v3")
    check("Issue #144 a web upload: NBIRTH 'testing e2e-v3'; during the trial the other-slot "
          "button (409, 'testing e2e-v3: N s left'), NCMD Boot Other Slot and a configuration "
          "save (409) are refused, and the shell shows the expected mode and the time left; "
          "then the self-check installs it",
          st == 200 and nb4 and fw_state(nb4[2]) == "testing e2e-v3" and st_b == 409
          and b"testing e2e-v3: " in b_b and ncmd_refused and st_c == 409
          and b"testing e2e-v3: " in b_c and all(h_t.values()) and conf3,
          f"POST {st} {body[:60]!r}; NBIRTH {fw_state(nb4[2]) if nb4 else None!r}; button "
          f"{st_b} {b_b[:80]!r}; NCMD refused {ncmd_refused is not None}; PUT {st_c} "
          f"{b_c[:80]!r}; shell {list(h_t.values())}; installed {conf3 is not None}")

    # a reset during the trial (panic) of v2, downloaded again into the other slot: the
    # bootloader goes back to v3, and its NBIRTH gives the cause (issue #144 items 5 and 7)
    gw.wait_log(r"edge: time synchronised", 10, nb4[0] - 5 if nb4 else nb[0])
    asked = time.monotonic()
    update(f"https://10.0.2.2:{fw_port}/v2.bin")
    death = obs.wait(f"{T}/NDEATH/{node}", 240)
    trial = gw.wait_log(r"fw: firmware e2e-v2 \(ota_0\) on trial: it must reach normal mode", 30,
                        asked)
    # the trial is logged before the shell starts (issue #101): the panic comes once
    # gw_fw_init() ran (the shell is up, the configuration loaded; a panic in the middle of
    # the configuration load once froze QEMU in the core dump write)
    gw.wait_log(r"fw: firmware e2e-v2 in ota_0; updates by URL", 30, trial or asked)
    gw.console("panic")
    back = gw.wait_log(r"fw: firmware e2e-v2 was rolled back \(restarted during the test "
                       r"\(panic\)\); running e2e-v3 again", 60, trial or asked)
    nb5 = obs.wait(nbirth_t, 60, lambda p: metric(p, name="Node Info/Firmware").string_value
                   == "e2e-v3")
    _, h_r = gw.shell("status", [r"^ota\s+e2e-v3 in ota_1, other slot e2e-v2 \(rejected: "
                                 r"restarted during the test \(panic\)\)"])
    check("FW-1 a reset during the trial (panic) makes the bootloader boot the earlier image; "
          "issue #144: its NBIRTH publishes 'rolled back from e2e-v2: restarted during the test "
          "(panic)', and the shell shows the other slot as rejected with that cause",
          death and trial and back and nb5
          and metric(nb5[2], name="Node Info/Reset Reason").string_value == "panic"
          and fw_state(nb5[2]) == "rolled back from e2e-v2: restarted during the test (panic)"
          and all(h_r.values()),
          f"NBIRTH firmware {metric(nb5[2], name='Node Info/Firmware').string_value!r}, reset "
          f"{metric(nb5[2], name='Node Info/Reset Reason').string_value!r}, state "
          f"{fw_state(nb5[2])!r}; shell {list(h_r.values())}" if nb5
          else f"trial {trial}, back {back}")

    # issue #144 item 6: the "Restart into e2e-v2" button boots the rejected image of the
    # other slot on trial; its self-check installs it
    asked = time.monotonic()
    fw0 = as_json(http("GET", "/api/firmware")[2]) or {}
    st_o, _, b_o = http("POST", "/api/firmware/other", b"", timeout=30)
    nb6 = obs.wait(nbirth_t, 240, lambda p: metric(p, name="Node Info/Firmware").string_value
                   == "e2e-v2")
    conf6 = obs.wait(ndata_t, SELFCHECK_WAIT_S, lambda p: fw_state(p) == "installed e2e-v2")
    check("Issue #144 the 'Restart into <version>' button (POST /api/firmware/other) boots the "
          "image of the other slot (rejected before) on trial: NBIRTH 'testing e2e-v2', then "
          "installed by its self-check",
          fw0.get("other") == "e2e-v2" and fw0.get("other_state") == "rejected"
          and fw0.get("can_boot_other") is True and st_o == 202 and nb6
          and fw_state(nb6[2]) == "testing e2e-v2" and conf6,
          f"before: other {fw0.get('other')!r} {fw0.get('other_state')!r} can boot "
          f"{fw0.get('can_boot_other')!r}; POST {st_o} {b_o[:60]!r}; NBIRTH "
          f"{fw_state(nb6[2]) if nb6 else None!r}; installed {conf6 is not None}")


def run_ota_signature(args, work):
    """FW-2, issue #261 (GW50; the secure build only, which requires a signed OTA image:
    CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT, the key taken from the running app, no eFuse).
    By NCMD: (1) an image with a correct checksum and no signature, into the erased slot;
    (2) a correct image signed with another key. Each is rejected after its download: the
    signature check ran, trusted no key, and the gateway did not restart. (3) By web upload: the
    same image signed with the key of the build. The key is trusted, the image boots on trial
    and its self-check installs it. The rejections go by NCMD and the accepted image by web
    upload, because a web upload of an image takes much more time than its download, and one
    upload covers the path that a person uses."""
    node, port, fw_port = "GW50", MQTT_PORT + 32, MQTT_PORT + 33
    pki = make_pki(work)
    fwdir = work / "fw-signed"
    fwdir.mkdir(exist_ok=True)
    src = app_source(args)
    other = work / "foreign_signing_key.pem"
    if not other.exists():
        subprocess.run([args.idf_python, "-m", "espsecure", "generate-signing-key", "--version",
                        "2", "--scheme", "rsa3072", str(other)], check=True, capture_output=True)
    unsigned = fw_image(src, "e2e-unsigned", fwdir / "unsigned.bin")
    foreign = signed_image(args, src, "e2e-foreign", fwdir, "foreign", key=other)
    good = signed_image(args, src, "e2e-signed", fwdir, "signed")
    # the fixtures: the signed images are the image plus one signature sector, and the two keys
    # give different signature sectors
    sizes = (len(unsigned.read_bytes()), len(foreign.read_bytes()), len(good.read_bytes()))
    if not (sizes[1] == sizes[2] == sizes[0] + 4096
            and foreign.read_bytes()[-4096:] != good.read_bytes()[-4096:]):
        raise SystemExit(f"signature fixtures: sizes {sizes} (unsigned, foreign, signed)")
    server = FirmwareServer(fw_port, fwdir, pki["server.crt"], pki["server.key"])
    conf = work / "broker-signed.conf"
    conf.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\n")
    broker = start_broker(conf, work / "mosquitto-signed.log")
    obs = Observer(port)
    cfg = node_config(node, port=port)
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    cfg["ota"] = {"ca_file": "ca.pem", "mqtt": True}   # #223: the updates by NCMD need it
    nbirth_t, ndata_t, ncmd_t = f"{T}/NBIRTH/{node}", f"{T}/NDATA/{node}", f"{T}/NCMD/{node}"
    verify_rx = r"esp_image: Verifying image signature"
    trusted_rx = r"secure_boot_v2: #\d+ app key digest == #\d+ trusted key digest"
    why = "rejected (checksum or signature)"
    t0 = time.monotonic()
    gw = boot_variant(args, work, "ota-signed", cfg, {"ca.pem": pki["ca.pem"].read_bytes()})
    try:
        nb = obs.wait(nbirth_t, 60)
        if not check(f"FW-2 {node} runs the secure build on its broker with ota.mqtt",
                     nb is not None and fw_state(nb[2]) == "idle",
                     f"state {fw_state(nb[2])!r}" if nb else "no NBIRTH"):
            return
        base = metric(nb[2], name="Node Info/Firmware").string_value
        gw.wait_log(r"edge: time synchronised", 20, t0)   # the image server's certificate dates
        gw.wait_log(r"web: web UI and REST API on port 443", 30, t0)

        def rejected(name):
            """An update by NCMD to @name.bin: (the NDATA with the failed state, whether the
            signature check ran, the lines of a trusted key), after the console line of the
            failure (the NDATA can arrive before it)."""
            asked = time.monotonic()
            obs.publish_cmd(ncmd_t, [{"name": "Node Control/Firmware Update", "datatype": 12,
                                      "string_value": f"https://10.0.2.2:{fw_port}/{name}.bin"}])
            state = obs.wait(ndata_t, 180, lambda p: (fw_state(p) or "") ==
                             f"failed: image e2e-{name} {why}")
            gw.wait_log(rf"fw: firmware update failed: image e2e-{name} {re.escape(why)}", 10,
                        asked)
            return state, gw.find(verify_rx, asked), gw.find(trusted_rx, asked)

        # 1. no signature: the slot is erased, so the sector after the image holds no signature
        # of an earlier image. 2. another key
        asked = time.monotonic()
        rej_u, verify_u, trusted_u = rejected("unsigned")
        rej_f, verify_f, trusted_f = rejected("foreign")
        time.sleep(2)
        restarted = any(m[1] == f"{T}/NDEATH/{node}" for m in CAPTURE if m[0] >= asked) \
            or bool(gw.find(BOOT_RX.pattern, asked))
        _, h = gw.shell("status", [rf"^ota\s+{re.escape(base)} in ota_0"])
        failure = gw.find(r"secure_boot_v2: .*(fail|not)|[Ss]ignature.*fail", asked)
        check("FW-2 (secure build) an OTA image without a valid signature of the key of the "
              "running firmware is rejected after its download by NCMD, and the running firmware "
              "stays (no restart): an image with no signature, and an image signed with another "
              "key; in the two cases the signature check ran and trusted no key",
              rej_u and verify_u and not trusted_u and rej_f and verify_f and not trusted_f
              and not restarted and all(h.values()),
              f"no signature: state {fw_state(rej_u[2]) if rej_u else None!r}, signature check "
              f"{bool(verify_u)}, trusted key {bool(trusted_u)}; another key: state "
              f"{fw_state(rej_f[2]) if rej_f else None!r}, signature check {bool(verify_f)}, "
              f"trusted key {bool(trusted_f)}; restarted {restarted}; shell {list(h.values())}; "
              f"{(failure or ['no failure line'])[0][:110]}")

        # 3. the key of the build, by web upload: verified, on trial, installed by the self-check
        asked3 = time.monotonic()
        st, _, body = upload_firmware(gw, good.read_bytes())
        body = body if isinstance(body, bytes) else str(body).encode()
        nb2 = obs.wait(nbirth_t, 240, lambda p: metric(p, name="Node Info/Firmware").string_value
                       == "e2e-signed")
        verify_s = gw.find(verify_rx, asked3)
        trusted_s = gw.find(trusted_rx, asked3)
        conf2 = obs.wait(ndata_t, SELFCHECK_WAIT_S, lambda p: fw_state(p) == "installed e2e-signed")
        _, h2 = gw.shell("status", [r"^ota\s+e2e-signed in ota_1, other slot \S+ \(previous\) \| "
                                    r"firmware e2e-signed installed"])
        check("FW-2 (secure build) an OTA image signed with the key of the running firmware is "
              "accepted by web upload (200): the signature check trusts its key, the image boots "
              "on trial (NBIRTH 'testing e2e-signed') and its self-check installs it",
              st == 200 and nb2 and fw_state(nb2[2]) == "testing e2e-signed" and verify_s
              and trusted_s and conf2 and all(h2.values()),
              f"POST {st} {body[:60]!r}; NBIRTH {fw_state(nb2[2]) if nb2 else None!r}; signature "
              f"check {bool(verify_s)}; {(trusted_s or ['no trusted key'])[0][:80]}; installed "
              f"{conf2 is not None}; shell {list(h2.values())}")
    finally:
        gw.kill()
        obs.close()
        server.close()
        broker.terminate()
        broker.wait()


WIRING_NODE = "GW46"   # issue #97: the commissioning gateway's edge node id (unique in this file)


def signed_image(args, src, version, fwdir, name, key=None, **caps):
    """fw_image() of @src with @version (and the @caps fields, or corrupt=True) as
    @fwdir/@name.bin. FW-2, issue #261: signed with the test key of the build (or with @key) only
    when the build requires signed images: the secure build (--signing-key). The board build
    does not check a signature: its images stay unsigned, as the images of a release."""
    fw_image(src, version, fwdir / f"{name}-unsigned.bin", **caps)
    key = key or args.signing_key
    if key:
        # --keyfile takes several values: an explicit --output keeps the data file apart
        subprocess.run([args.idf_python, "-m", "espsecure", "sign-data", "--version", "2",
                        "--keyfile", str(key), "--output", str(fwdir / f"{name}.bin"),
                        str(fwdir / f"{name}-unsigned.bin")], check=True, capture_output=True)
    else:
        shutil.copy(fwdir / f"{name}-unsigned.bin", fwdir / f"{name}.bin")
    return fwdir / f"{name}.bin"


def app_source(args):
    """The app image of the build under test without a signature: the source of each test image
    (fw_image). The secure build (--signing-key) signs its app image and keeps the image from
    before the signature as <name>-unsigned.bin. A board build has no such file. One next to its
    ELF is a leftover of a build with another configuration in the same directory (the build
    does not write it again): it holds another firmware, and it is never used (issue #261)."""
    app = pathlib.Path(args.elf).with_suffix(".bin")
    if not args.signing_key:
        return app
    unsigned = pathlib.Path(args.elf).with_name(app.stem + "-unsigned.bin")
    if not unsigned.exists():
        raise SystemExit(f"{unsigned}: the secure build has no app image without a signature")
    return unsigned


def run_ota_wiring(args, work):
    """Issue #144 (and #97: how main.c wires the outcomes of a new image's trial), by web upload,
    with the main broker and the web server only (no port of its own):
    (1) an image installed from recovery mode that rejects the stored configuration stays in
    recovery mode (expected mode: recovery or better), and its self-check installs it there;
    (2) in normal mode, the pre-flight check refuses an image that reads an older schema (409,
    with the paths) and an image without capabilities version 2 (409, unknown); an image that it
    accepts but that cannot read the configuration (emulator hook /cfg/.sim-reject-config, for a
    new image on trial only) goes back to the earlier image at once, with the path in the state;
    the same older-schema image with force must reach recovery mode only: it stays, a save is
    refused during its trial and works after its self-check;
    (3) an image installed from commissioning (after a factory reset) is installed there by its
    self-check, and stays after a power cut. Safe mode is in run_ota_trial (the budget of one
    attempt);
    (4) issue #146: after an update to an image that adds a key (the emulator counts
    network.hostname as the schema after the one of the build, gw_storage.c), the first save of
    the key keeps the earlier
    document as config.prev.json; a second change makes the button refuse the earlier image with
    the JSON path that changed; with the document of the capture again, the button restores the
    earlier document, and the earlier image starts in normal mode with it."""
    fwdir = work / "fw-wiring"
    fwdir.mkdir(exist_ok=True)
    src = app_source(args)
    # the schema that the image under test reads (GW_CONFIG_SCHEMA); the emulator counts
    # network.hostname as the next one (gw_storage.c, k_sim_since)
    k2 = image_cfg_schema(src) + 1
    img = {
        "e2e-rej": signed_image(args, src, "e2e-rej", fwdir, "e2e-rej"),
        "e2e-rjn": signed_image(args, src, "e2e-rjn", fwdir, "e2e-rjn", cfg_schema=1),
        "e2e-s0": signed_image(args, src, "e2e-s0", fwdir, "e2e-s0", cfg_schema=0),
        "e2e-v1caps": signed_image(args, src, "e2e-v1caps", fwdir, "e2e-v1caps", caps_version=1),
        "e2e-com": signed_image(args, src, "e2e-com", fwdir, "e2e-com"),
        "e2e-k2": signed_image(args, src, "e2e-k2", fwdir, "e2e-k2", cfg_schema=k2),
    }

    def upload(version, pw, force=False):
        st, _, body = upload_firmware(gw, img[version].read_bytes(), auth=("admin", pw or "?"),
                                      force=force)
        return st, body if isinstance(body, bytes) else str(body).encode()

    # 1. a stored configuration that the images reject (a syntax error): the running image runs
    # in recovery mode, where web uploads stay possible
    t0 = time.monotonic()
    gw = Gateway(args, work / "uart0-otarej.log",
                 flash_variant(args, work, "otarej", {"config.json": b'{"sparkplug": \n'}))
    gw.start()
    try:
        rec = gw.wait_log(r"main: configuration rejected", 60, t0)
        pw = commissioning_password(gw, t0, 30)
        wait_http("/api/status", 30, auth=("admin", pw or "?"))
        asked = time.monotonic()
        st, body = upload("e2e-rej", pw)
        boot = gw.wait_log(r"main: rs485_spb_gateway e2e-rej .*reset reason", 120, asked)
        trial = gw.wait_log(r"fw: firmware e2e-rej \(ota_\d\) on trial: it must reach recovery mode",
                            30, boot or asked)
        rec2 = gw.wait_log(r"main: configuration rejected", 30, boot or asked)
        inst = gw.wait_log(r"fw: firmware e2e-rej installed: the self-check passed in recovery mode",
                           SELFCHECK_WAIT_S, boot or asked)
        rolled = gw.find(r"fw: firmware e2e-rej rejected|was rolled back", asked)
        _, h = gw.shell("status", [r"^ota\s+e2e-rej in ota_\d, other slot \S+ \(previous\) \| "
                                   r"firmware e2e-rej installed"])
        check("Issue #144 item 2 an image installed from recovery mode that rejects the stored "
              "configuration stays (it must reach recovery mode or better): its self-check "
              "installs it in recovery mode, with no rollback (main.c -> gw_fw_mode)",
              rec and pw and st == 200 and boot and trial and rec2 and inst and not rolled
              and list(h.values())[0],
              f"recovery {rec is not None}; POST {st} {body[:60]!r}; on trial {trial is not None}, "
              f"in recovery {rec2 is not None}, installed "
              f"{f'{inst - boot:.1f} s after its boot' if inst and boot else 'no'}; rolled back "
              f"{rolled[:1]}; shell {bool(list(h.values())[0])}")
    finally:
        gw.kill()

    # 2. normal mode with the emulator hook: a new image on trial cannot read the configuration.
    # No web password in the document: a password in clear makes the first boot move it to NVS
    # and rewrite LittleFS with config.json and the certificates only (CFG-7), without the hook
    cfg = node_config(WIRING_NODE)
    flash = flash_variant(args, work, "otanorm", {"config.json": json.dumps(cfg, indent=2).encode(),
                                                  ".sim-reject-config": b"1"})
    t0 = time.monotonic()
    gw = Gateway(args, work / "uart0-otanorm.log", flash)
    gw.start()
    try:
        up = gw.wait_log(r"edge: Sparkplug session up", 60, t0)
        pw = commissioning_password(gw, t0, 30)
        auth = ("admin", pw or "?")
        wait_http("/api/firmware", 30, auth=auth)
        asked = time.monotonic()
        st0, b0 = upload("e2e-s0", pw)
        st1, b1 = upload("e2e-v1caps", pw)
        time.sleep(1)
        booted = gw.find(r"main: rs485_spb_gateway e2e-(s0|v1caps) ", asked)
        check("Issue #144 item 3 the pre-flight check refuses before any write: an image that reads "
              "an older configuration schema (409, the JSON paths that are too new) and an image "
              "without capabilities version 2 (409, unknown: force needed); no restart",
              up and st0 == 409
              and b"the configuration needs schema 1, the image reads 0: network (1), " in b0
              and b"force installs it into recovery mode" in b0
              and st1 == 409 and b"declares no configuration schema" in b1 and not booted,
              f"session {up is not None}; schema 0: {st0} {b0[:200]!r}; version 1: {st1} "
              f"{b1[:120]!r}; booted {booted[:1]}")

        asked = time.monotonic()
        st2, b2 = upload("e2e-rjn", pw)
        boot = gw.wait_log(r"main: rs485_spb_gateway e2e-rjn .*reset reason", 120, asked)
        trial = gw.wait_log(r"fw: firmware e2e-rjn \(ota_\d\) on trial: it must reach normal mode",
                            30, boot or asked)
        rej = gw.wait_log(r"fw: firmware e2e-rjn rejected \(configuration rejected: e2e\.simulated\): "
                          r"restarting into ", 60, boot or asked)
        prev = gw.wait_log(r"main: rs485_spb_gateway (?!e2e-rjn )\S+ .*reset reason: software", 120,
                           rej or asked)
        back = gw.wait_log(r"fw: firmware e2e-rjn was rolled back \(configuration rejected: "
                           r"e2e\.simulated\); running \S+ again", 30, prev or asked)
        up2 = gw.wait_log(r"edge: Sparkplug session up", 60, prev or asked)
        fw2 = as_json(wait_http("/api/firmware", 30, auth=auth)[2]) or {}
        # "at once": in half the self-check time (#233: the bound stays below SELFCHECK_S)
        fast = boot is not None and rej is not None and rej - boot < SELFCHECK_S / 2
        check("Issue #144 item 2 an image installed from normal mode (accepted by the pre-flight "
              "check) that rejects the configuration goes back to the earlier image at once (a "
              f"planned restart, in less than {SELFCHECK_S / 2:.0f} s: before its {SELFCHECK_S} s "
              "self-check), and the earlier image, in normal mode, gives the path in the state and "
              "the other slot as rejected",
              st2 == 200 and boot and trial and rej and fast and prev and back and up2
              and fw2.get("state") == "rolled back from e2e-rjn: configuration rejected: "
                                      "e2e.simulated"
              and fw2.get("other") == "e2e-rjn" and fw2.get("other_state") == "rejected"
              and fw2.get("other_why") == "configuration rejected: e2e.simulated",
              f"POST {st2} {b2[:60]!r}; on trial {trial is not None}; rejected "
              f"{f'{rej - boot:.1f} s after its boot' if rej and boot else 'no'}; earlier image "
              f"{prev is not None}, says so {back is not None}, session {up2 is not None}; "
              f"GET /api/firmware state {fw2.get('state')!r} other {fw2.get('other')!r} "
              f"{fw2.get('other_state')!r} {fw2.get('other_why')!r}")

        # the older-schema image again, forced: it must reach recovery mode only
        asked = time.monotonic()
        st3, b3 = upload("e2e-s0", pw, force=True)
        boot = gw.wait_log(r"main: rs485_spb_gateway e2e-s0 .*reset reason", 120, asked)
        trial = gw.wait_log(r"fw: firmware e2e-s0 \(ota_\d\) on trial: it must reach recovery mode",
                            30, boot or asked)
        rec = gw.wait_log(r"main: configuration rejected", 30, boot or asked)
        wait_http("/api/firmware", 30, auth=auth)
        st_t, _, b_t = http("PUT", "/api/config", cfg, auth=auth, timeout=30)
        inst = gw.wait_log(r"fw: firmware e2e-s0 installed: the self-check passed in recovery mode",
                           SELFCHECK_WAIT_S, boot or asked)
        st_s, _, b_s = http("PUT", "/api/config", cfg, auth=auth, timeout=30)
        boot2 = gw.wait_log(r"main: rs485_spb_gateway e2e-s0 .*reset reason: software", 120,
                            inst or asked)
        up3 = gw.wait_log(r"edge: Sparkplug session up", 60, boot2 or asked)
        rolled = gw.find(r"fw: firmware e2e-s0 rejected|e2e-s0 was rolled back", asked)
        check("Issue #144 items 1 to 3 the same image forced: it must reach recovery mode, so it "
              "stays when it rejects the configuration; during its trial a configuration save is "
              "refused (409, 'testing e2e-s0: N s left'); after its self-check a save works, and "
              "the restart runs it in normal mode",
              st3 == 200 and boot and trial and rec and st_t == 409 and b"testing e2e-s0: " in b_t
              and inst and st_s == 200 and boot2 and up3 and not rolled,
              f"POST {st3} {b3[:60]!r}; on trial {trial is not None}, recovery {rec is not None}; "
              f"save during the trial {st_t} {b_t[:80]!r}; installed {inst is not None}; save "
              f"after it {st_s} {b_s[:60]!r}; normal mode after the restart {up3 is not None}; "
              f"rolled back {rolled[:1]}")
    finally:
        gw.kill()

    # 3. commissioning (factory reset): the new image is installed by its self-check there, and
    # survives a power cut
    cfg = node_config(WIRING_NODE)
    flash = flash_variant(args, work, "otacom", {"config.json": json.dumps(cfg, indent=2).encode()})
    t0 = time.monotonic()
    gw = Gateway(args, work / "uart0-otacom.log", flash)
    gw.start()
    try:
        up = gw.wait_log(r"edge: Sparkplug session up", 60, t0)
        gw.shell("status", [r"^config\s"])   # the shell is up: 'boot 1' is not lost
        pressed = time.monotonic()
        gw.console("boot 1")
        fired = gw.wait_log(r"button: BOOT held 10 s: factory reset", 15, pressed)
        comm = gw.wait_log(r"main: commissioning: no configuration since the factory reset", 40,
                           pressed)
        pw = commissioning_password(gw, pressed, 30)
        wait_http("/api/status", 30, auth=("admin", pw or "?"))
        asked = time.monotonic()
        st, body = upload("e2e-com", pw)
        boot = gw.wait_log(r"main: rs485_spb_gateway e2e-com .*reset reason", 120, asked)
        conf = gw.wait_log(r"fw: firmware e2e-com installed: the self-check passed in "
                           r"commissioning mode; the bootloader keeps it", SELFCHECK_WAIT_S,
                           boot or asked)
        comm2 = gw.wait_log(r"main: commissioning: no configuration since the factory reset", 30,
                            boot or asked)
        gw.kill()   # a power cut after the confirmation: the bootloader keeps the new image
        t1 = time.monotonic()
        gw = Gateway(args, work / "uart0-otacom.log", flash)
        gw.start()
        boot2 = gw.wait_log(r"main: rs485_spb_gateway e2e-com .*reset reason", 60, t1)
        comm3 = gw.wait_log(r"main: commissioning: no configuration since the factory reset", 40,
                            t1)
        _, h = gw.shell("status", [r"^ota\s+e2e-com in ota_\d"])
        rolled = gw.find(r"was rolled back|rejected \(", t1)
        check("Issue #97/#144 a new image installed from commissioning (after a factory reset) is "
              "installed there by its self-check (commissioning counts as recovery mode or "
              "better), and the bootloader keeps it after a power cut",
              up and fired and comm and pw and st == 200 and boot and conf and comm2 and boot2
              and comm3 and list(h.values())[0] and not rolled,
              f"factory reset {fired is not None}, commissioning {comm is not None}; POST {st} "
              f"{body[:60]!r}; new image booted {boot is not None}, installed "
              f"{f'{conf - boot:.1f} s after its boot' if conf and boot else 'no'}; after the "
              f"power cut: e2e-com {boot2 is not None}, commissioning {comm3 is not None}, "
              f"rolled back {rolled[:1]}")
    finally:
        gw.kill()

    # 4. issue #146: the configuration from before an update. The base image reads schema k2 - 1;
    # e2e-k2 declares schema k2, and in the QEMU build network.hostname is a schema k2 key (the
    # documents without it need schema 1)
    cfg = node_config(WIRING_NODE)
    cfg["network"].pop("hostname", None)
    cfg_k = json.loads(json.dumps(cfg))
    cfg_k["network"]["hostname"] = "gw-e2e-146"
    cfg_k2 = json.loads(json.dumps(cfg_k))
    cfg_k2["network"]["hostname"] = "gw-e2e-146b"
    flash = flash_variant(args, work, "otaprev", {"config.json": json.dumps(cfg, indent=2).encode()})
    t0 = time.monotonic()
    gw = Gateway(args, work / "uart0-otaprev.log", flash)
    gw.start()

    def save(doc, what):
        """PUT @doc; True when it was saved and the gateway runs again in normal mode."""
        asked = time.monotonic()
        st, _, b = http("PUT", "/api/config", doc, auth=auth, timeout=30)
        boot = gw.wait_log(r"main: rs485_spb_gateway \S+ .*reset reason: software", 120, asked)
        up = gw.wait_log(r"edge: Sparkplug session up", 60, boot or asked)
        wait_http("/api/firmware", 30, auth=auth)
        ok = st == 200 and boot is not None and up is not None
        if not ok:
            notes.append(f"save {what}: {st} {b[:80]!r}, restart {boot is not None}, session "
                         f"{up is not None}")
        return ok

    notes = []
    try:
        up = gw.wait_log(r"edge: Sparkplug session up", 60, t0)
        pw = commissioning_password(gw, t0, 30)
        auth = ("admin", pw or "?")
        # the HTTPS server answers (404) before it registers the handlers: wait for the evidence
        # that the REST API is up, else "running" can be missing and base "?"
        gw.wait_log(r"web: web UI and REST API on port 443", 30, t0)
        base = (as_json(wait_http("/api/firmware", 30, auth=auth)[2]) or {}).get("running", "?")
        asked = time.monotonic()
        st, body = upload("e2e-k2", pw)
        boot = gw.wait_log(r"main: rs485_spb_gateway e2e-k2 .*reset reason", 120, asked)
        inst = gw.wait_log(r"fw: firmware e2e-k2 installed: the self-check passed in normal mode",
                           SELFCHECK_WAIT_S, boot or asked)
        armed = gw.find(rf"storage: the first change of the configuration keeps the configuration "
                        rf"of {re.escape(base)}", boot or asked)
        wait_http("/api/firmware", 30, auth=auth)
        kept_at = time.monotonic()
        saved = save(cfg_k, "with the new key")
        kept = gw.find(rf"storage: first change after the update: the configuration of "
                       rf"{re.escape(base)} \(schema 1\) kept as /cfg/config.prev.json", kept_at)
        fw1 = as_json(http("GET", "/api/firmware", auth=auth)[2]) or {}
        prev1 = fw1.get("cfg_prev") or {}
        check("Issue #146 the confirmation of an update arms the record with the earlier version "
              "(the other slot); the first save that changes the configuration keeps the earlier "
              "document as config.prev.json, and GET /api/firmware gives its version (no restore "
              "button in normal mode)",
              up and st == 200 and boot and inst and armed and saved and kept
              and prev1.get("version") == base and prev1.get("schema") == 1
              and fw1.get("cfg_prev_restore") is False,
              f"base {base!r}; POST {st} {body[:60]!r}; installed {inst is not None}; armed "
              f"{armed[:1]}; saved {saved}, kept {kept[:1]}; cfg_prev {prev1!r} restore "
              f"{fw1.get('cfg_prev_restore')!r}; {notes}")

        # a second change: the earlier document is not restored, the refusal gives the path
        saved2 = save(cfg_k2, "with another value")
        st_r, _, b_r = http("POST", "/api/firmware/other", auth=auth, timeout=30)
        time.sleep(1)
        booted = gw.find(rf"main: rs485_spb_gateway {re.escape(base)} ", kept_at)
        check("Issue #146 when the configuration changed since the capture, the button refuses the "
              "earlier image (409) with the paths that are too new and the paths that changed "
              "since the update; nothing restarts",
              saved2 and st_r == 409 and f"network.hostname ({k2})".encode() in b_r
              and b"it changed since (network.hostname)" in b_r and not booted,
              f"saved {saved2}; POST {st_r} {b_r[:300]!r}; booted {booted[:1]}; {notes}")

        # the document of the capture again (the same hash): the button restores the earlier one
        saved3 = save(cfg_k, "of the capture again")
        asked = time.monotonic()
        st_b, _, b_b = http("POST", "/api/firmware/other", auth=auth, timeout=30)
        plan = gw.wait_log(rf"fw: pre-flight check: image {re.escape(base)} reads the configuration "
                           rf"from before the update", 30, asked)
        rest = gw.wait_log(r"storage: /cfg/config.prev.json restored as /cfg/config.json", 60, asked)
        boot = gw.wait_log(rf"main: rs485_spb_gateway {re.escape(base)} .*reset reason", 120, asked)
        trial = gw.wait_log(rf"fw: firmware {re.escape(base)} \(ota_\d\) on trial: it must reach "
                            r"normal mode", 30, boot or asked)
        up3 = gw.wait_log(r"edge: Sparkplug session up", 60, boot or asked)
        rec = gw.find(r"main: configuration rejected", boot or asked)
        cfg3 = as_json(wait_http("/api/config", 30, auth=auth)[2]) or {}
        inst3 = gw.wait_log(rf"fw: firmware {re.escape(base)} installed: the self-check passed in "
                            r"normal mode", SELFCHECK_WAIT_S, boot or asked)
        rolled = gw.find(rf"fw: firmware {re.escape(base)} rejected|{re.escape(base)} was rolled "
                         r"back", asked)
        check("Issue #146 with the configuration of the capture again (the same hash), the button "
              "restores the earlier document before the restart; the earlier image starts in "
              "normal mode with it (no network.hostname) and its self-check installs it",
              saved3 and st_b == 202 and plan and rest and boot and trial and up3 and not rec
              and cfg3.get("network") is not None and "hostname" not in cfg3["network"]
              and cfg3.get("sparkplug", {}).get("edge_node_id") == WIRING_NODE
              and inst3 and not rolled,
              f"saved {saved3}; POST {st_b} {b_b[:120]!r}; planned {plan is not None}, restored "
              f"{rest is not None}; earlier image {boot is not None}, on trial {trial is not None}, "
              f"session {up3 is not None}, rejected {rec[:1]}; network {cfg3.get('network')!r}; "
              f"installed {inst3 is not None}; rolled back {rolled[:1]}; {notes}")
    finally:
        gw.kill()


def run_ota_trial(args, work):
    """Issue #144 (GW43): (1) item 1: the trial of a new image is a self-check of the gateway; it
    does not depend on the broker. A web upload, then the broker goes away before the new image
    boots: during the trial an update is refused (409, "testing <v>: N s left") and the shell shows
    the expected mode and the time left; the self-check installs the image with no broker and no
    rollback; when the broker is back, the NBIRTH publishes "installed <v>". (2) item 4: three
    crashes in a row (emulator 'panic') start safe mode, and safe mode accepts an upload (the new
    image, on trial, ignores the boot-loop counter)."""
    node, port = "GW43", MQTT_PORT + 25
    fwdir = work / "fw-trial"
    fwdir.mkdir(exist_ok=True)
    image = signed_image(args, app_source(args), "e2e-t2", fwdir, "t2")
    safe_image = signed_image(args, app_source(args), "e2e-safe", fwdir, "safe")
    conf = work / "broker-trial.conf"
    conf.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\n")
    broker = start_broker(conf, work / "mosquitto-trial.log")
    obs = Observer(port)
    cfg = node_config(node, port=port)
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    nbirth_t = f"{T}/NBIRTH/{node}"

    def fw(item):
        return metric(item[2], name="Node Info/Firmware").string_value if item else None

    gw = boot_variant(args, work, "trial", cfg)
    try:
        nb = obs.wait(nbirth_t, 40)
        if not check("Issue #144 GW43 runs on its broker", nb is not None and fw(nb),
                     f"NBIRTH {'yes' if nb else 'no'}"):
            return
        wait_http("/api/firmware", 20)
        asked = time.monotonic()
        st, _, body = upload_firmware(gw, image.read_bytes())
        broker.terminate()   # before the new image boots
        broker.wait()
        obs.close()
        boot = gw.wait_log(r"main: rs485_spb_gateway e2e-t2 .*reset reason", 120, asked)
        trial = gw.wait_log(r"fw: firmware e2e-t2 \(ota_\d\) on trial: it must reach normal mode", 30,
                            boot or asked)
        wait_http("/api/firmware", 30)
        st_u, _, b_u = http("POST", "/api/firmware", {"url": "https://10.0.2.2:1/x.bin"},
                            timeout=30)
        _, h = gw.shell("status", [r"^ota\s+e2e-t2 in ota_\d, other slot \S+ \(previous\) \| "
                                   r"testing e2e-t2 \(testing: it must reach normal mode, \d+ s "
                                   r"left\)$"])
        inst = gw.wait_log(r"fw: firmware e2e-t2 installed: the self-check passed in normal mode",
                           SELFCHECK_WAIT_S, boot or asked)
        session = gw.find(r"edge: Sparkplug session up", boot or asked)
        rolled = gw.find(r"fw: firmware e2e-t2 rejected|e2e-t2 was rolled back", asked)
        # The broker comes back for the observer first: a broker that accepts only the observer,
        # then a reload that lets the gateway in. Before, the gateway's backoff could connect it
        # in the 0.5 s between the broker start and the SUBACK of the observer, and the NBIRTH
        # went out unseen (2026-11-13: the reconnect came 0.2 s after the installation).
        broker, conf_back = auth_broker(work, "trial-back", port, {})
        obs = observer_retry(port, auth=OBS_AUTH)
        conf_back.write_text(conf_back.read_text().replace("allow_anonymous false",
                                                           "allow_anonymous true"))
        broker.send_signal(signal.SIGHUP)   # mosquitto reloads allow_anonymous
        nb2 = obs.wait(nbirth_t, 90)
        check("Issue #144 item 1 a new image whose broker is down is installed by its self-check "
              "(no Sparkplug session, no rollback); during its trial an update is refused (409, "
              "'testing e2e-t2: N s left') and the shell shows the expected mode and the time "
              "left; with the broker back, its NBIRTH publishes 'installed e2e-t2'",
              st == 200 and boot and trial and st_u == 409 and b"testing e2e-t2: " in b_u
              and all(h.values()) and inst and not session and not rolled and nb2
              and fw(nb2) == "e2e-t2" and fw_state(nb2[2]) == "installed e2e-t2",
              f"POST {st} {body[:60]!r}; on trial {trial is not None}; update during the trial "
              f"{st_u} {b_u[:80]!r}; shell {list(h.values())}; installed "
              f"{f'{inst - boot:.1f} s after its boot' if inst and boot else 'no'}; sessions "
              f"before {len(session)}; rolled back {rolled[:1]}; NBIRTH {fw(nb2)!r} state "
              f"{fw_state(nb2[2]) if nb2 else None!r}")
    finally:
        gw.kill()

    # 2. issue #144 item 4: three crashes in a row start safe mode; safe mode accepts an upload
    flash = flash_variant(args, work, "otasafe", {"config.json": json.dumps(cfg, indent=2).encode()})
    t0 = time.monotonic()
    gw = Gateway(args, work / "uart0-otasafe.log", flash)
    gw.start()
    try:
        boots = []
        since = t0
        for _ in range(3):
            gw.wait_log(r"edge: Sparkplug session up", 60, since)
            gw.shell("status", [r"^config\s"])   # the shell is up: 'panic' is not lost
            asked = time.monotonic()
            gw.console("panic net-off")   # QEMU's openeth survives a panic reset (docs/reference/e2e-harness.md)
            boots.append(gw.wait_log(r"main: rs485_spb_gateway \S+ .*reset reason: panic", 60,
                                     asked))
            since = boots[-1] or asked
        safe = gw.wait_log(r"fw: safe mode: 3 crash resets in a row", 30, since)
        main_safe = gw.wait_log(r"main: safe mode: 3 crash resets in a row; the configuration is "
                                r"not loaded", 30, since)
        wait_http("/api/firmware", 40)
        fws = as_json(http("GET", "/api/firmware")[2]) or {}
        _, h = gw.shell("status", [r"^bootloop 3 crash resets in a row: SAFE MODE"])
        session = gw.find(r"edge: Sparkplug session up", since)
        asked = time.monotonic()
        st, _, body = upload_firmware(gw, safe_image.read_bytes())
        boot = gw.wait_log(r"main: rs485_spb_gateway e2e-safe .*reset reason", 120, asked)
        trial = gw.wait_log(r"fw: firmware e2e-safe \(ota_\d\) on trial: it must reach recovery "
                            r"mode", 30, boot or asked)
        up = gw.wait_log(r"edge: Sparkplug session up", 60, boot or asked)
        inst = gw.wait_log(r"fw: firmware e2e-safe installed: the self-check passed in normal mode",
                           SELFCHECK_WAIT_S, boot or asked)
        safe_again = gw.find(r"fw: safe mode:", boot or asked)
        check("Issue #144 item 4 three crashes in a row (emulator 'panic') start safe mode "
              "(recovery mode: nothing started, the shell and GET /api/firmware say so); safe mode "
              "accepts a firmware upload, and the new image on trial ignores the boot-loop "
              "counter (normal mode, installed by its self-check)",
              all(boots) and safe and main_safe and fws.get("safe_mode") is True
              and fws.get("boot_count") == 3 and list(h.values())[0] and not session and st == 200
              and boot and trial and up and inst and not safe_again,
              f"panic resets {sum(1 for b in boots if b)}/3; safe mode {safe is not None} "
              f"({main_safe is not None}); GET /api/firmware safe_mode {fws.get('safe_mode')!r} "
              f"boot_count {fws.get('boot_count')!r}; shell {bool(list(h.values())[0])}; session "
              f"in safe mode {len(session)}; POST {st} {body[:60]!r}; new image {boot is not None}, "
              f"on trial {trial is not None}, session {up is not None}, installed "
              f"{inst is not None}; safe mode again {safe_again[:1]}")
    finally:
        gw.kill()
        obs.close()
        broker.terminate()
        broker.wait()


# ---------------------------------------------------------------- issues #67/#94 NVS migration

def auth_broker(work, name, port, users):
    """A broker on @port that only accepts the @users ({name: password}) and the harness'
    observer ("e2e"). Returns (process, conf path)."""
    pw = work / f"passwd-{name}"
    pw.unlink(missing_ok=True)
    for i, (u, p) in enumerate({**users, "e2e": "e2e-observer"}.items()):
        subprocess.run(["mosquitto_passwd", "-b", *(["-c"] if i == 0 else []), str(pw), u, p],
                       check=True, capture_output=True)
    conf = work / f"broker-{name}.conf"
    # "user root": as in run_tls, user "mosquitto" could not reach a file in the work directory
    conf.write_text(f"user root\nlistener {port} 127.0.0.1\nallow_anonymous false\n"
                    f"password_file {pw}\n")
    return start_broker(conf, work / f"mosquitto-{name}.log"), conf


OBS_AUTH = ("e2e", "e2e-observer")


def nvs_clear(flash, words):
    """The @words found in clear in the NVS partition of the image @flash."""
    off, size = storage_partition("nvs")
    region = pathlib.Path(flash).read_bytes()[off:off + size]
    return [w for w in words if w in region]


def secure_only(args, name, fn):
    """Issue #261: the runner of scenario @name, which examines a function that only the secure
    build has (NVS encryption, signed OTA images). On the board build it is a [SKIP]: the
    scenario would examine a function that the build does not have."""
    def run():
        if args.secure:
            return fn()
        SKIPPED.append(name)
        print(f"[SKIP] {name}: it needs the secure build (E2E_SECURE=1; issue #261)", flush=True)
        return None
    return run


def run_nvs_encryption(args, work):
    """CFG-7, issue #261 (GW51; the secure build only): the first boot of a device whose eFuse
    KEY5 is blank (its own eFuse file), with the MQTT password and the web password in clear in
    config.json. That boot generates the NVS keys (the HMAC key goes to the emulated eFuse),
    starts the encrypted NVS and moves the secrets out of the file. The broker requires the
    MQTT password, and the web UI the web password. After it, no secret is in clear anywhere in
    the flash image. A second boot reads the secrets with the key of the eFuse: it generates no
    key, moves nothing, and the eFuse file does not change."""
    node, port = "GW51", MQTT_PORT + 34
    secret = "s3cret-mqtt-cfg7"
    cfg = node_config(node, port=port, username="gw51", password=secret)
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    flash = flash_variant(args, work, "nvsenc", {"config.json": json.dumps(cfg, indent=2).encode()})
    efuse = work / "efuse-nvsenc.bin"
    shutil.copy(work / "efuse-blank.bin", efuse)
    blank = efuse.read_bytes()
    broker, _ = auth_broker(work, "nvsenc", port, {"gw51": secret})
    obs = observer_retry(port, auth=OBS_AUTH)
    log = work / "uart0-nvsenc.log"
    nbirth_t = f"{T}/NBIRTH/{node}"
    status_rx = r"^secrets\s+NVS encrypted \(HMAC key in eFuse KEY5\)"
    # the keys of a blank NVS come from nvs_flash_init() itself (ESP-IDF); the migration of an
    # unencrypted NVS makes them first (gw_storage, run_nvs_migration)
    keygen_rx = r"nvs: Generating NVS encr-keys|storage: NVS keys not readable"
    t0 = time.monotonic()
    gw = Gateway(args, log, flash, efuse=efuse)
    gw.start()
    try:
        nb = obs.wait(nbirth_t, 60)
        keygen = gw.find(keygen_rx, t0)
        moved = gw.find(r"storage: secrets moved from /cfg/config\.json to NVS", t0)
        encrypted = gw.find(r'nvs: NVS partition "nvs" is encrypted', t0)
        _, h = gw.shell("status", [status_rx])
        st, _, body = wait_http("/api/config", 30)
        doc = as_json(body) or {}
        gw.kill()
        obs.wait(f"{T}/NDEATH/{node}", 5)
        burned = efuse.read_bytes()
        image = pathlib.Path(flash).read_bytes()
        regions = (("storage", storage_partition()), ("nvs", storage_partition("nvs")),
                   ("image", (0, len(image))))
        leaks = [(what.decode(), where) for what in (secret.encode(), WEB_PASSWORD.encode())
                 for where, (o, n) in regions if what in image[o:o + n]]
        check("CFG-7 (secure build) the first boot with a blank eFuse KEY5 generates the NVS keys "
              "(the HMAC key in the emulated eFuse), starts the encrypted NVS and moves the "
              "secrets out of config.json: the MQTT password from the NVS opens the broker "
              "(which requires it), the web password opens the web UI, the shell shows the NVS "
              "as encrypted, and no secret is in clear anywhere in the flash image",
              nb and len(keygen) == 1 and burned != blank and moved and encrypted
              and list(h.values())[0] and st == 200
              and (doc.get("mqtt") or {}).get("password") == "********" and not leaks,
              f"NBIRTH {nb is not None}; key generations {len(keygen)}, eFuse file changed "
              f"{burned != blank}; {(moved or ['no secrets moved'])[0][:90]}; "
              f"{(encrypted or ['NVS not encrypted'])[0][:60]}; shell {list(h.values())}; GET "
              f"/api/config {st}; secrets found in clear: {leaks or 'none'}")
        obs.drain()
        t1 = time.monotonic()
        gw = Gateway(args, log, flash, efuse=efuse)
        gw.start()
        nb2 = obs.wait(nbirth_t, 60)
        encrypted2 = gw.find(r'nvs: NVS partition "nvs" is encrypted', t1)
        again = gw.find(rf"{keygen_rx}|storage: secrets moved|NVS migration|"
                        r"NVS layout changed|NVS unavailable", t1)
        _, h2 = gw.shell("status", [status_rx])
        st2 = wait_http("/api/config", 30)[0]
        gw.kill()
        check("CFG-7 (secure build) the next boot reads the encrypted NVS with the key of the "
              "eFuse: no key generated, nothing moved or erased, the eFuse file unchanged; the "
              "MQTT password still opens the broker, and the web password the web UI",
              nb2 and encrypted2 and not again and efuse.read_bytes() == burned
              and list(h2.values())[0] and st2 == 200,
              f"NBIRTH {nb2 is not None}; {(encrypted2 or ['NVS not encrypted'])[0][:60]}; "
              f"unexpected lines {again[:2]}; eFuse file unchanged "
              f"{efuse.read_bytes() == burned}; shell {list(h2.values())}; GET /api/config {st2}")
    finally:
        if not gw.killed:   # a second kill() would read as a QEMU death (#86)
            gw.kill()
        obs.close()
        broker.terminate()
        broker.wait()


def run_nvs_migration(args, work):
    """Issues #67 and #94 (GW41): a flash whose NVS was written without NVS encryption (the
    format of the default build, generated here with ESP-IDF's nvs_partition_gen.py: the
    commissioning password, the MQTT password) boots the encrypted build, and the power is
    cut twice in the middle of the migration (emulated cut points: right after the partition is
    erased, then after half of the entries are written encrypted). The next boots finish it from
    the reset-safe copy: the MQTT password still opens the broker (which requires it), the commissioning password is the old one, nothing stays in clear and the
    migration does not run again. Issue #104: after each cut, one boot whose LittleFS does not
    mount (superblocks damaged, not erased: the copy is out of reach) opens no NVS at all, so
    nothing it writes can be lost or win over the copy; then the partition comes back. Issue #109:
    two more cuts first, before the erase (the partition intact: that boot runs on it) and in the
    middle of it (the "erase in progress" entry: no NVS); and at the end, from the state of cut 2
    with LittleFS out of reach, the operator's way out, 'config load discard-nvs-copy'."""
    node, port = "GW41", MQTT_PORT + 24
    nvs_off, nvs_size = storage_partition("nvs")
    st_off, st_size = storage_partition("storage")
    csv = work / "nvs-plain.csv"
    # issue #109 round 3b: the "gw" namespace entry and a 3840-byte blob fill page 0, so the
    # "erase in progress" entry the migration writes into "gw" lands on a later page (as on a device
    # that ran a while): the erase takes the namespace's page before the entry's
    filler = work / "nvs-filler.bin"
    filler.write_bytes(bytes(range(256)) * 15)
    # issue #185: the "secrets" namespace entry completes page 0, and its items are on page 1 with
    # the "erase in progress" entry. Cut 3 erases page 0: the next boot lists the items with an
    # empty namespace, and takes them from the copy of the interrupted migration
    csv.write_text("key,type,encoding,value\ngw,namespace,,\ndo_hold,data,u8,0\n"
                   f"filler,file,binary,{filler}\n"
                   "secrets,namespace,,\ncommission,data,string,abcdefgh1234\n"
                   "mqtt_pw,data,string,s3cret-mqtt-67\n")
    gen = pathlib.Path(os.environ["IDF_PATH"]) / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    plain = work / "nvs-plain.bin"
    subprocess.run([args.idf_python, str(gen), "generate", str(csv), str(plain), hex(nvs_size)],
                   check=True, capture_output=True)
    raw = plain.read_bytes()
    if raw[0:4] != b"\xfc\xff\xff\xff" or raw[64 + 8:64 + 10] != b"gw":
        raise SystemExit("nvs fixture: page 0 should be full and start with the gw namespace")
    # the password is only in the NVS (CFG-7); the broker refuses the gateway without it
    cfg = node_config(node, port=port, username="gw41")
    flash = flash_variant(args, work, "nvsmig", {"config.json": json.dumps(cfg).encode(),
                                                  ".nvsmig-cut": b"0312"})
    img = bytearray(flash.read_bytes())
    img[nvs_off:nvs_off + nvs_size] = plain.read_bytes()
    flash.write_bytes(img)
    # Issue #97: the real case is a device whose KEY5 is not burned yet; in a full run earlier
    # scenarios burned it in the shared eFuse file: this one gets a blank copy of its own
    efuse = work / "efuse-nvsmig.bin"
    shutil.copy(work / "efuse-blank.bin", efuse)
    blank = efuse.read_bytes()
    broker, _ = auth_broker(work, "nvsmig", port, {"gw41": "s3cret-mqtt-67"})
    obs = observer_retry(port, auth=OBS_AUTH)
    log = work / "uart0-nvsmig.log"
    words = (b"abcdefgh1234", b"s3cret-mqtt-67", b"commission")
    boots = []
    try:
        cuts, out_of_reach, intact, snap = [], [], None, None
        for point in ("0", "3", "1", "2"):
            t = time.monotonic()
            gw = Gateway(args, log, flash, efuse=efuse)
            gw.start()
            boots.append(gw)
            held = gw.wait_log(rf"storage: NVS migration: emulated power cut {point} ", 60, t)
            time.sleep(2)   # QEMU writes the flash file behind: let the last writes land
            resumed = gw.find(r"storage: NVS migration was interrupted \((.*)\): writing its copy", t)
            gw.kill()
            region = flash.read_bytes()[nvs_off:nvs_off + nvs_size]
            cuts.append((held is not None, resumed[0].split("storage: ")[-1][:90] if resumed else None,
                         region.count(b"\xff") == len(region)))
            if point == "2":
                snap = flash.read_bytes()   # for the operator's way out, at the end
            # issue #104: LittleFS does not mount for one boot (the copy is in it)
            img = bytearray(flash.read_bytes())
            saved = bytes(img[st_off:st_off + 8192])
            img[st_off:st_off + 8192] = b"\x55" * 8192
            flash.write_bytes(img)
            storage_before = bytes(img[st_off:st_off + st_size])
            t = time.monotonic()
            gw = Gateway(args, log, flash, efuse=efuse)
            gw.start()
            boots.append(gw)
            if point == "0":
                # issue #109: keys made, nothing erased: the partition is intact and this boot
                # runs on it (its commissioning password is the old one), not without NVS
                stays = gw.wait_log(r'storage: NVS partition "nvs" is not encrypted and stays so this '
                                    r"boot: LittleFS is not mountable", 60, t)
                pw0 = commissioning_password(gw, t)
                no_nvs = gw.find(r"NVS left untouched this boot", t)
                gw.kill()
                img = bytearray(flash.read_bytes())
                img[st_off:st_off + 8192] = saved
                flash.write_bytes(img)
                intact = (stays is not None, pw0, not no_nvs)
                continue
            left = gw.wait_log(r"storage: NVS left untouched this boot \(no NVS: .*\): LittleFS does "
                               r"not mount and the NVS looks like an interrupted migration", 60, t)
            _, h_out = gw.shell("status", [r"^secrets\s+NVS UNAVAILABLE this boot: LittleFS does not "
                                           r"mount"], timeout=10)
            # issue #108 M1: a configuration saved in that boot is refused (formatting LittleFS
            # would destroy the copy); the partition stays as it was
            asked = time.monotonic()
            gw.console("config load")
            gw.wait_log(r"^paste the JSON document", 10, asked)
            gw.sock.sendall((json.dumps(cfg, indent=2) + "\n").encode())
            refused = gw.wait_log(r"^config: rejected: configuration not saved: LittleFS does not "
                                  r"mount and can hold the copy of an NVS migration", 30, asked)
            formatted = gw.find(r"formatting it to save", asked)
            time.sleep(1)
            gw.kill()
            img = bytearray(flash.read_bytes())
            same = img[nvs_off:nvs_off + nvs_size] == region
            kept = bytes(img[st_off:st_off + st_size]) == storage_before
            img[st_off:st_off + 8192] = saved
            flash.write_bytes(img)
            # issue #108 B9: the boot does not change the NVS either (cuts 3 and 1: not even
            # initialised; cut 2: only read for the migration's markers)
            out_of_reach.append((left is not None, all(h_out.values()), same,
                                 refused is not None and not formatted and kept))
        burned = efuse.read_bytes() != blank
        t0 = time.monotonic()
        gw = Gateway(args, log, flash, efuse=efuse)
        gw.start()
        boots.append(gw)
        nb = obs.wait(f"{T}/NBIRTH/{node}", 60)
        done = gw.find(r"storage: NVS migration resumed: \d+ entries moved into the encrypted NVS "
                       r"\(0 skipped\)", t0)
        pw = commissioning_password(gw, t0)
        gw.kill()
        obs.wait(f"{T}/NDEATH/{node}", 5)
        clear = nvs_clear(flash, words)
        obs.drain()
        t1 = time.monotonic()
        gw = Gateway(args, log, flash, efuse=efuse)
        gw.start()
        boots.append(gw)
        nb2 = obs.wait(f"{T}/NBIRTH/{node}", 60)
        again = gw.find(r"NVS migration", t1)
        _, h = gw.shell("status", [r"^secrets\s+NVS encrypted \(HMAC key in eFuse KEY5\)"])
        gw.kill()
        # cut 0: before the erase (the partition as it was); cut 3: every page erased but the one
        # with the "erase in progress" entry; cut 1: right after the erase (all 0xFF); none of them
        # a resume; cut 2: the boot resumed from the copy and held halfway through the writes
        cut_ok = (len(cuts) == 4 and all(c[0] for c in cuts)
                  and all(c[1] is None for c in cuts[:3]) and not cuts[0][2] and not cuts[1][2]
                  and cuts[2][2] and cuts[3][1] is not None and not cuts[3][2])
        check("Issue #94 an unencrypted NVS meets the encrypted build and the power is cut four "
              "times during the migration (#109: before the erase and in the middle of it; after "
              "the erase, then halfway through the encrypted writes): "
              "the next boot finishes it from the reset-safe copy (0 skipped); the MQTT "
              "password from the NVS opens the broker that requires it, the commissioning "
              "password is the old one, nothing in clear, no migration on the next boot "
              "(issue #67); #161: bdSeq 0 at each boot",
              cut_ok and nb and bdseq_of(nb[2]) == 0 and done and pw == "abcdefgh1234"
              and not clear and nb2 and bdseq_of(nb2[2]) == 0 and not again
              and list(h.values())[0],
              f"cuts (held, resumed, erased): {cuts}; NBIRTH bdSeq "
              f"{bdseq_of(nb[2]) if nb else None} then {bdseq_of(nb2[2]) if nb2 else None}; "
              f"{(done or ['no resume log'])[0].split('storage: ')[-1][:90]}; commissioning "
              f"password {pw!r}; in clear: {clear}; migration on the next boot: {bool(again)}")
        # issue #97: the first boot found KEY5 blank and burned it (its own eFuse file), before
        # its first emulated cut; none of the later boots generated keys again
        keygen = [len(b.find(r"storage: NVS keys not readable \(.*\): generating them"))
                  for b in boots]
        first_cut = boots[0].wait_log(r"storage: NVS migration: emulated power cut 0 ", 0)
        gen_t = boots[0].wait_log(r"storage: NVS keys not readable", 0)
        check("Issue #97 the NVS migration scenario starts on a device whose eFuse KEY5 is not "
              "burned (its own blank eFuse file, also in a full run): the first boot generates the "
              "NVS keys and burns KEY5 before it erases anything, no later boot does it again",
              burned and keygen[:1] == [1] and not any(keygen[1:]) and gen_t is not None and first_cut is not None
              and gen_t <= first_cut,
              f"eFuse file changed by the interrupted boots: {burned}; key generations per boot "
              f"{keygen}; {(boots[0].find(r'storage: NVS keys not readable') or ['no log'])[0][:110]}")
        check("Issue #104 after each cut (#109: in the middle of the erase, after it, halfway "
              "through the writes), a boot whose LittleFS does not mount (the copy is in it) "
              "opens no NVS at all and leaves the NVS partition as it was (no empty or partial "
              "encrypted NVS), the shell says why; issue #108 M1: a configuration saved in that "
              "boot is refused and LittleFS is not formatted, so the next boot with LittleFS "
              "still finishes the migration from the copy (check above)",
              len(out_of_reach) == 3 and all(all(x) for x in out_of_reach),
              f"(logged, shell, NVS untouched, save refused and LittleFS kept) per cut: "
              f"{out_of_reach}")
        check("Issue #109 a migration cut before its erase (keys made, nothing erased) with LittleFS "
              "out of reach: the boot runs on the intact unencrypted NVS (the old commissioning "
              "password), not without NVS; cut in the middle of the erase: the \"erase in "
              "progress\" entry makes it a boot without NVS (row 2 above)",
              intact is not None and intact[0] and intact[1] == "abcdefgh1234" and intact[2],
              f"(stays unencrypted, commissioning password, not without NVS): {intact}")

        # issue #109: the operator's way out, from the state of cut 2 with LittleFS out of reach:
        # the configuration saved with discard-nvs-copy formats LittleFS (the copy is lost), and
        # the next boot runs with it
        img = bytearray(snap)
        img[st_off:st_off + 8192] = b"\x55" * 8192
        flash.write_bytes(img)
        t3 = time.monotonic()
        gw = Gateway(args, log, flash, efuse=efuse)
        gw.start()
        gw.wait_log(r"storage: NVS left untouched this boot", 60, t3)
        gw.wait_log(r"shell: console ready", 30, t3)
        asked = time.monotonic()
        gw.console("config load discard-nvs-copy")
        gw.wait_log(r"^paste the JSON document", 10, asked)
        gw.sock.sendall((json.dumps(cfg, indent=2) + "\n").encode())
        discarded = gw.wait_log(r"storage: configuration saved discarding the unreachable NVS "
                                r"migration copy", 30, asked)
        saved_ok = gw.wait_log(r"^config: saved \(\d+ bytes\); restarting", 30, asked)
        loaded = gw.wait_log(r"main: configuration from /cfg/config\.json", 60, saved_ok or asked)
        after = gw.find(r"NVS left untouched this boot", loaded or asked)
        gw.kill()
        check("Issue #109 the operator's way out of a LittleFS that does not mount with an "
              "unreachable migration copy: 'config load discard-nvs-copy' formats it (the copy is "
              "lost), and the next boot runs with the saved configuration and an NVS",
              discarded and saved_ok and loaded and not after,
              f"discarded {discarded is not None}, saved {saved_ok is not None}, next boot from "
              f"config.json {loaded is not None}, without NVS again {bool(after)}")
    finally:
        obs.close()
        broker.terminate()
        broker.wait()


def run_nvs_full(args, work):
    """Issue #109 round 3b (GW44): an unencrypted NVS with no free page (generated with ESP-IDF's
    nvs_partition_gen.py, five 3840-byte blobs, its last page then started as NVS does): it does
    not even start (ESP_ERR_NVS_NO_FREE_PAGES), and IDF would erase it. The migration must still
    happen: erased whole, without the "erase in progress" entry (written through an NVS that
    starts), encrypted from then on."""
    node, port = "GW44", MQTT_PORT + 26
    nvs_off, nvs_size = storage_partition("nvs")
    filler = work / "nvs-filler.bin"
    filler.write_bytes(bytes(range(256)) * 15)
    csv = work / "nvs-full.csv"
    csv.write_text("key,type,encoding,value\ngw,namespace,,\n"
                   + "".join(f"fill{i},file,binary,{filler}\n" for i in range(1, 6)))
    gen = pathlib.Path(os.environ["IDF_PATH"]) / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    full = work / "nvs-full.bin"
    subprocess.run([args.idf_python, str(gen), "generate", str(csv), str(full), hex(nvs_size)],
                   check=True, capture_output=True)
    nv = bytearray(full.read_bytes())
    pages = nvs_size // 4096
    last = (pages - 1) * 4096
    if nv[last:last + 4] != b"\xff\xff\xff\xff":
        raise SystemExit("nvs-full: the generator filled the last page itself")
    # the active page before the last becomes full, the last becomes the active one (empty): no
    # free page is left; the page header CRC covers bytes 4..27 (as esp_rom_crc32_le)
    prev = last - 4096
    nv[prev:prev + 4] = b"\xfc\xff\xff\xff"
    hdr = bytearray(b"\xff" * 32)
    hdr[0:4] = b"\xfe\xff\xff\xff"
    hdr[4:8] = struct.pack("<I", pages - 1)
    hdr[8] = nv[prev + 8]   # the format version of the generator's pages
    hdr[28:32] = struct.pack("<I", zlib.crc32(bytes(hdr[4:28]), 0xFFFFFFFF))
    nv[last:last + 32] = hdr
    cfg = node_config(node, port=port)
    flash = flash_variant(args, work, "nvsfull", {"config.json": json.dumps(cfg).encode()})
    img = bytearray(flash.read_bytes())
    img[nvs_off:nvs_off + nvs_size] = nv
    flash.write_bytes(img)
    efuse = work / "efuse-nvsfull.bin"
    shutil.copy(work / "efuse-blank.bin", efuse)
    conf = work / "broker-nvsfull.conf"
    conf.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\n")
    broker = start_broker(conf, work / "mosquitto-nvsfull.log")
    obs = observer_retry(port)
    log = work / "uart0-nvsfull.log"
    nbirth_t = f"{T}/NBIRTH/{node}"
    try:
        t0 = time.monotonic()
        gw = Gateway(args, log, flash, efuse=efuse)
        gw.start()
        nb = obs.wait(nbirth_t, 60)
        nostart = gw.find(r"storage: NVS migration: the unencrypted NVS does not start "
                          r"\(ESP_ERR_NVS_NO_FREE_PAGES\)", t0)
        moved = gw.find(r"storage: NVS migration: \d+ entries moved into the encrypted NVS "
                        r"\(0 skipped\)", t0)
        stuck = gw.find(r"erase in progress\" entry could not be written|NVS migration not finished",
                        t0)
        gw.kill()
        obs.wait(f"{T}/NDEATH/{node}", 5)
        obs.drain()
        t1 = time.monotonic()
        gw = Gateway(args, log, flash, efuse=efuse)
        gw.start()
        nb2 = obs.wait(nbirth_t, 60)
        again = gw.find(r"NVS migration", t1)
        _, h = gw.shell("status", [r"^secrets\s+NVS encrypted \(HMAC key in eFuse KEY5\)"])
        gw.kill()
        check("Issue #109 round 3b an unencrypted NVS with no free page (it does not start: "
              "NO_FREE_PAGES) is still migrated: erased whole without the \"erase in progress\" "
              "entry, encrypted on the next boot (no migration again); #161: bdSeq 0 at each boot",
              nostart and moved and not stuck and nb and bdseq_of(nb[2]) == 0 and nb2
              and bdseq_of(nb2[2]) == 0 and not again and all(h.values()),
              f"does not start: {bool(nostart)}; {(moved or stuck or ['no migration log'])[0][:110]}; "
              f"NBIRTH bdSeq {bdseq_of(nb[2]) if nb else None} then "
              f"{bdseq_of(nb2[2]) if nb2 else None}; migration again {bool(again)}")
    finally:
        obs.close()
        broker.terminate()
        broker.wait()


def run_nvs_ota(args, work):
    """Issue #94 (GW42): a device on the build WITHOUT NVS encryption (issue #261: the board
    build of the PSRAM variant, --board-flash; commissioning password and MQTT password in its
    unencrypted NVS) is updated by OTA to the secure build under test. The board build checks no
    signature, so it accepts the signed image. On trial, the new image keeps the NVS
    unencrypted; a reset during the trial (panic) rolls it back and the old image finds its
    secrets. Updated again with the broker up: installed by its self-check (issue #144), one
    restart, and that boot moves the NVS into the encrypted one. Issue #102: then the build
    without NVS encryption (signed with the key of the secure build, so that only its
    capabilities refuse it), sent back by OTA, is refused before it is set to boot, and the NVS
    (secrets) stays intact. Issue #161: each boot starts at bdSeq 0."""
    if not args.board_flash:
        SKIPPED.append("issue #94 OTA to NVS encryption")
        print("[SKIP] issue #94 OTA from a build without NVS encryption: no --board-flash "
              "(run_e2e.sh builds it with E2E_SECURE=1, unless E2E_BOARD=0)", flush=True)
        return
    node, port, fw_port = "GW42", MQTT_PORT + 22, MQTT_PORT + 23
    secret = "s3cret-mqtt-94"
    pki = make_pki(work)
    fwdir = work / "fw-nvs"
    fwdir.mkdir(exist_ok=True)
    signed_image(args, app_source(args), "e2e-enc", fwdir, "enc")
    server = FirmwareServer(fw_port, fwdir, pki["server.crt"], pki["server.key"])
    broker, conf = auth_broker(work, "nvsota", port, {"gw42": secret})
    obs = observer_retry(port, auth=OBS_AUTH)
    # the password in clear in config.json: the board build moves it to its (unencrypted) NVS
    cfg = node_config(node, port=port, username="gw42", password=secret)
    cfg["ota"] = {"ca_file": "ca.pem", "mqtt": True}   # #223: the update by NCMD needs it
    base = pathlib.Path(args.board_flash)
    files = {"config.json": json.dumps(cfg, indent=2).encode(), "ca.pem": pki["ca.pem"].read_bytes()}
    flash = flash_variant(args, work, "nvsota", files, base=base)
    if args.psram:
        write_otadata(flash)
    log = work / "uart0-nvsota.log"
    # issue #97: a device on its way to NVS encryption has KEY5 blank: its own eFuse file
    efuse = work / "efuse-nvsota.bin"
    shutil.copy(work / "efuse-blank.bin", efuse)
    nbirth_t, ncmd_t = f"{T}/NBIRTH/{node}", f"{T}/NCMD/{node}"
    ndata_t = f"{T}/NDATA/{node}"
    words = (secret.encode(), b"commission", b"mqtt_pw")
    # issue #102: the build without NVS encryption as an update, signed like the others
    # (--board-app comes with --board-flash: check_args)
    signed_image(args, args.board_app, "e2e-board", fwdir, "board")

    def update(name="enc"):
        obs.publish_cmd(ncmd_t, [{"name": "Node Control/Firmware Update", "datatype": 12,
                                  "string_value": f"https://10.0.2.2:{fw_port}/{name}.bin"}])

    def fw(nb):
        return metric(nb[2], name="Node Info/Firmware").string_value if nb else None

    t0 = time.monotonic()
    gw = Gateway(args, log, flash, efuse=efuse)
    gw.start()
    try:
        nb0 = obs.wait(nbirth_t, 60)
        pw0 = commissioning_password(gw, t0)
        v1 = fw(nb0)
        clear0 = nvs_clear(flash, words)
        if not check("Issue #94 the build without NVS encryption runs (its secrets in an "
                     "unencrypted NVS) and reaches the broker that requires its password; #161: "
                     "bdSeq 0 at the boot",
                     nb0 and pw0 and v1 != "e2e-enc" and secret.encode() in clear0
                     and bdseq_of(nb0[2]) == 0,
                     f"firmware {v1!r}, bdSeq {bdseq_of(nb0[2]) if nb0 else None}, commissioning "
                     f"password {pw0!r}; in clear in its NVS: {clear0}"):
            return
        gw.wait_log(r"edge: time synchronised", 20, t0)

        # 1. update; the broker goes away before the new image reaches a session; a reset (panic)
        # during the trial rolls it back
        asked = time.monotonic()
        update()
        death = obs.wait(f"{T}/NDEATH/{node}", 240)
        broker.terminate()
        broker.wait()
        obs.close()
        trial = gw.wait_log(r"fw: firmware e2e-enc \(ota_1\) on trial", 60, asked)
        kept = gw.find(r'storage: NVS partition "nvs" is not encrypted and stays so this boot: '
                       r"this firmware is on trial", asked)
        # a connection attempt of the trial image (the broker is down)
        attempt = gw.wait_log(r"edge: connecting to .* bdSeq=\d+", 60, trial or asked)
        time.sleep(1)
        _, h_trial = gw.shell("status", [r"^secrets\s+NVS NOT encrypted yet \(new firmware on trial"])
        gw.console("panic")
        back = gw.wait_log(r"fw: firmware e2e-enc was rolled back \(restarted during the test "
                           r"\(panic\)\); running \S+ again", 90, trial or asked)
        erased = gw.find(r"NVS layout changed, erasing|NVS unavailable", asked)
        broker = start_broker(conf, work / "mosquitto-nvsota.log")
        obs = observer_retry(port, auth=OBS_AUTH)
        nb1 = obs.wait(nbirth_t, 90)
        pw1 = commissioning_password(gw, back or asked) if back else None
        clear1 = nvs_clear(flash, words)
        b1 = bdseq_of(nb1[2]) if nb1 else None
        check("Issue #94 OTA from the build without NVS encryption to the encrypted one, rolled back "
              "by a reset during the trial: the new image kept the NVS unencrypted while on trial, "
              "and the old image finds its secrets (the MQTT password opens the broker, the "
              "commissioning password is the same); #161: bdSeq 0 again after the restart",
              death and trial and kept and attempt and list(h_trial.values())[0] and back
              and not erased
              and nb1 and fw(nb1) == v1 and b1 == 0 and pw1 == pw0 and secret.encode() in clear1,
              f"trial {trial is not None}, kept unencrypted {bool(kept)}, rolled back "
              f"{back is not None}; NVS erased by the old image: {erased[:1]}; NBIRTH firmware "
              f"{fw(nb1)!r} bdSeq {b1}; commissioning password {pw1!r}; in clear: {clear1}")

        # 2. update again with the broker up: installed by its self-check (issue #144), one
        # restart, migrated
        gw.wait_log(r"edge: time synchronised", 20, nb1[0] - 5 if nb1 else t0)
        asked = time.monotonic()
        update()
        conf_t = gw.wait_log(r"fw: firmware e2e-enc confirmed: restarting once to encrypt the NVS",
                             300, asked)
        moved_t = gw.wait_log(r"storage: NVS migration: \d+ entries moved into the encrypted NVS "
                              r"\(0 skipped\)", 90, conf_t or asked)
        # the NBIRTH of the boot that migrated (the trial's own session came before it)
        nb3, end = None, time.monotonic() + 90
        while moved_t is not None and nb3 is None and time.monotonic() < end:
            item = obs.wait(nbirth_t, end - time.monotonic())
            if item is None:
                break
            nb3 = item if item[0] > moved_t else None
        pw2 = commissioning_password(gw, moved_t) if moved_t else None
        _, h_enc = gw.shell("status", [r"^secrets\s+NVS encrypted \(HMAC key in eFuse KEY5\)"])
        # exactly one restart after the confirmation: the migrated boot stays up
        quiet_from = nb3[0] if nb3 else time.monotonic()
        later = obs.wait(nbirth_t, 20)
        boots = gw.find(r"main: rs485_spb_gateway .* reset reason", conf_t) if conf_t else []
        restarts = gw.find(r"restarting once to encrypt the NVS", conf_t) if conf_t else []
        one_restart = (later is None and len(boots) == 1 and len(restarts) == 1
                       and not any(m[1] == f"{T}/NDEATH/{node}" for m in CAPTURE
                                   if m[0] > quiet_from))
        gw.kill()
        clear2 = nvs_clear(flash, words)
        b3 = bdseq_of(nb3[2]) if nb3 else None
        check("Issue #94 the same update with the broker up: installed by its self-check, one "
              "restart (and no second one), and that boot moves the NVS into the encrypted one (the "
              "MQTT password still opens the broker, same commissioning password, nothing in "
              "clear; #161: bdSeq 0 at that boot)",
              conf_t and moved_t and nb3 and fw(nb3) == "e2e-enc" and b3 == 0 and pw2 == pw0
              and not clear2
              and list(h_enc.values())[0] and one_restart,
              f"confirmed {conf_t is not None}, migrated {moved_t is not None}; boots after the "
              f"confirmation {len(boots)}, restart requests {len(restarts)}, another NBIRTH in "
              f"20 s {later is not None}; NBIRTH firmware "
              f"{fw(nb3)!r} bdSeq {b3}; commissioning password {pw2!r}; in clear: {clear2}")

        # 3. issue #102: the build without NVS encryption sent to the migrated device: refused
        # once its head is in the slot (before esp_ota_set_boot_partition), no restart; the next
        # boot still has the encrypted NVS (the password opens the broker)
        obs.drain()
        t3 = time.monotonic()
        gw = Gateway(args, log, flash, efuse=efuse)
        gw.start()
        nb4 = obs.wait(nbirth_t, 60)
        gw.wait_log(r"edge: time synchronised", 20, t3)
        asked = time.monotonic()
        update("board")
        refused = obs.wait(ndata_t, 120, lambda p: (fw_state(p) or "").startswith(
            "failed: image e2e-board refused: built without NVS encryption"))
        # the NDATA can arrive before the console line is read (issue #107: slower emulation)
        why = gw.wait_log(r"fw: update refused: the image built without NVS encryption \(this "
                          r"device's NVS is encrypted\)", 10, asked)
        time.sleep(3)
        restarted = any(m[1] == f"{T}/NDEATH/{node}" for m in CAPTURE if m[0] >= asked)
        gw.kill()
        obs.wait(f"{T}/NDEATH/{node}", 5)
        obs.drain()
        t4 = time.monotonic()
        gw = Gateway(args, log, flash, efuse=efuse)
        gw.start()
        nb5 = obs.wait(nbirth_t, 60)
        pw3 = commissioning_password(gw, t4)
        _, h3 = gw.shell("status", [r"^secrets\s+NVS encrypted \(HMAC key in eFuse KEY5\)",
                                    r"^ota\s+e2e-enc in "])
        gw.kill()
        clear3 = nvs_clear(flash, words)
        b4 = bdseq_of(nb4[2]) if nb4 else None
        b5 = bdseq_of(nb5[2]) if nb5 else None
        check("Issue #102 an OTA update to the build without NVS encryption, on a device whose NVS "
              "is already encrypted, is refused with its reason before it is set to boot (no "
              "restart); the next boot runs the same firmware with the NVS intact (the MQTT "
              "password from the NVS opens the broker, same commissioning password; #161: bdSeq 0 "
              "at each boot)",
              nb4 and refused and why and not restarted and nb5 and fw(nb5) == "e2e-enc"
              and b4 == 0 and b5 == 0 and pw3 == pw0 and all(h3.values()) and not clear3,
              f"state {fw_state(refused[2]) if refused else None!r}; restarted {restarted}; NBIRTH "
              f"firmware {fw(nb5)!r} bdSeq {b4} -> {b5}; commissioning password {pw3!r}; shell "
              f"{h3}; in clear: {clear3}")
    finally:
        if not gw.killed:   # a second kill() would read as a QEMU death (#86)
            gw.kill()
        obs.close()
        server.close()
        broker.terminate()
        broker.wait()


# ---------------------------------------------------------------- issue #148 downgrade

DOWNGRADE_NODE = "GW49"   # issue #148 (unique in this file)


def run_downgrade(args, work):
    """Issue #148 (optional: only with --only downgrade and E2E_DOWNGRADE_REF, else [SKIP]): the
    image under test boots on a new flash, saves a configuration through the web UI (the MQTT
    password and the web password go to the NVS: not encrypted in the board build, encrypted in
    the secure build, issue #261) and runs in normal mode. Then the
    image of an earlier release (run_e2e.sh builds it from the git ref E2E_DOWNGRADE_REF) is
    written over ota_0 of the same flash: the bootloader, the partition table, otadata, the NVS
    and LittleFS stay as the new image left them, as after an OTA rollback. The earlier image must
    start in normal mode with the same configuration, and with the secrets: its broker requires
    the MQTT password, and the web UI accepts the web password. bdSeq is in RAM only (#161).
    Run it each time that littlefs, an NVS format or ESP-IDF changes (CLAUDE.md)."""
    if not args.downgrade_app:
        SKIPPED.append("downgrade")
        print("[SKIP] issue #148 downgrade to an earlier release: no --downgrade-app (set "
              "E2E_DOWNGRADE_REF=<git ref> for run_e2e.sh)", flush=True)
        return
    old_app = pathlib.Path(args.downgrade_app).read_bytes()
    port = MQTT_PORT + 31
    broker, _ = auth_broker(work, "downgrade", port, {"gw148": "mqtt-secret-148"})
    obs = observer_retry(port, auth=OBS_AUTH)
    nbirth_t = f"{T}/NBIRTH/{DOWNGRADE_NODE}"
    cfg = node_config(DOWNGRADE_NODE, port=port)
    cfg_new = json.loads(json.dumps(cfg))
    cfg_new["mqtt"].update(username="gw148", password="mqtt-secret-148")
    cfg_new["web"] = {"username": "admin", "password": WEB_PASSWORD}
    flash = flash_variant(args, work, "downgrade", {"config.json": json.dumps(cfg, indent=2).encode()})
    t0 = time.monotonic()
    gw = Gateway(args, work / "uart0-downgrade.log", flash)
    gw.start()
    try:
        boot = gw.wait_log(r"main: rs485_spb_gateway (\S+) .*reset reason", 60, t0)
        new_ver = re.search(r"rs485_spb_gateway (\S+) ", gw.find(
            r"main: rs485_spb_gateway \S+ .*reset reason", t0)[0]).group(1) if boot else "?"
        pw = commissioning_password(gw, t0, 40)
        wait_http("/api/status", 40, auth=("admin", pw or "?"))
        asked = time.monotonic()
        st, _, b = http("PUT", "/api/config", cfg_new, auth=("admin", pw or "?"), timeout=30)
        boot2 = gw.wait_log(r"main: rs485_spb_gateway \S+ .*reset reason: software", 120, asked)
        nb = obs.wait(nbirth_t, 90, since=boot2 or asked)
        wait_http("/api/config", 40)
        st_c, _, b_c = http("GET", "/api/config", timeout=30)
        before = as_json(b_c) or {}
        if not check("Issue #148 the image under test saves the configuration with the MQTT and "
                     "web passwords (NVS) and runs in normal mode on its broker, which requires "
                     "the password",
                     boot and pw and st == 200 and boot2 and nb and st_c == 200
                     and (before.get("mqtt") or {}).get("password") == "********",
                     f"version {new_ver}; PUT {st} {b[:80]!r}; restart {boot2 is not None}; NBIRTH "
                     f"{nb is not None}; GET /api/config {st_c}, mqtt.password "
                     f"{(before.get('mqtt') or {}).get('password')!r}"):
            return
    finally:
        gw.kill()

    # the earlier image over ota_0; everything else of the flash as the new image left it
    off, size = storage_partition("ota_0")
    image = bytearray(pathlib.Path(flash).read_bytes())
    image[off:off + size] = old_app + b"\xff" * (size - len(old_app))
    pathlib.Path(flash).write_bytes(bytes(image))
    t1 = time.monotonic()
    gw = Gateway(args, work / "uart0-downgrade-old.log", flash)
    gw.start()
    try:
        boot = gw.wait_log(r"main: rs485_spb_gateway \S+ .*reset reason", 60, t1)
        line = gw.find(r"main: rs485_spb_gateway \S+ .*reset reason", t1)
        old_ver = re.search(r"rs485_spb_gateway (\S+) ", line[0]).group(1) if line else "?"
        nb = obs.wait(nbirth_t, 120, since=t1)
        up = gw.wait_log(r"Sparkplug session up", 30, t1)   # the console after the NBIRTH
        wait_http("/api/config", 60)
        st_c, _, b_c = http("GET", "/api/config", timeout=30)
        after = as_json(b_c) or {}
        bad = gw.find(r"main: configuration rejected|main: commissioning|not mountable|"
                      r"formatting it|safe mode", t1)
        diff = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
        check(f"Issue #148 the image of the earlier release ({args.downgrade_ref or old_ver}) boots "
              f"on the flash that the image under test wrote: normal mode, the same configuration "
              f"(GET /api/config), the MQTT password (its broker requires it: NBIRTH) and the web "
              f"password (the web UI accepts it); LittleFS and the NVS are not formatted",
              boot and old_ver != new_ver and nb and up and st_c == 200 and not diff
              and (after.get("mqtt") or {}).get("password") == "********" and not bad,
              f"versions {new_ver} -> {old_ver}; NBIRTH {nb is not None}, session "
              f"{up is not None}; GET /api/config {st_c} {b_c[:80] if st_c != 200 else b''!r}; "
              f"different sections {diff}; lines {bad[:2]}")
    finally:
        gw.kill()
        obs.close()
        broker.terminate()
        broker.wait()


# ---------------------------------------------------------------- UMH Core interoperability (#175)
#
# Optional (scenarios.OPTIONAL): a real benthos-umh with the sparkplug_b input and the bridge of
# docs/how-to/connect-to-umh-core.md reads the gateway through the main mosquitto. The harness
# takes the input keys and the tag_processor code from that document, so the test examines what
# the document tells users to do. Get the binary (it is not in the CI image):
#   curl -sSLo benthos-umh https://github.com/united-manufacturing-hub/benthos-umh/releases/\
#       download/v0.16.2/benthos-linux-amd64 && chmod +x benthos-umh
#   E2E_BENTHOS_UMH=$PWD/benthos-umh tools/e2e/run_e2e.sh --only umh
# Without E2E_BENTHOS_UMH the scenario is a SKIP. The checks assert what v0.16.2 does, the gaps of
# UMH included, so a new UMH version that changes its behaviour makes a check fail.

UMH_VERSION, UMH_COMMIT = "0.16.2", "0d3a203"
# sha256 of the release asset benthos-linux-amd64 of v0.16.2 (the GitHub release digest)
UMH_SHA256 = "39a876816578efbd5b36474c079dbdca5ba376ae25cc8453ba52b8e7b46d7c33"
UMH_GROUP, UMH_NODE, UMH_NODE2 = "acme", "acme:berlin:gw01", "acme:berlin:gw02"
# Issue #172: six location levels before each device name, more than the five levels of ISA-95
UMH_PREFIX = "acme:berlin:tempelhof:packaging:hall2:line3"
UMH_LOC = UMH_PREFIX.replace(":", ".")
UMH_HOWTO = HERE.parent.parent.parent / "docs" / "how-to" / "connect-to-umh-core.md"
UMH_TS_LINE = "msg.meta.timestamp_ms = msg.meta.spb_timestamp;"
# Issue #175: the scenarios that did not run (a program or a build that they need is absent).
# Issue #261: results.json records them ("skipped"), so that the summary of the CI groups does
# not count a scenario that was skipped as a scenario that ran (scenarios.summary), and
# --no-skip makes each of them a failed check.
SKIPPED = []
# The tags of the device cell2:meter1 (unit 1): (name, tag configuration, value at the input of
# the bridge). Each scalar type of the gateway, signed integers included.
UMH_TAGS = [
    ("L1/Voltage", {"area": "input", "address": 0, "type": "float32", "word_order": "big",
                    "deadband": 0.1, "units": "V"}, 230.5),
    ("Setpoint", {"area": "holding", "address": 10, "type": "int16", "scale": 0.1}, 5.0),
    ("Run", {"area": "coil", "address": 0, "type": "bool"}, False),
    ("Energy", {"area": "holding", "address": 30, "type": "uint64", "word_order": "ABCD"},
     MB6["Energy"]),
    ("Counter", {"area": "holding", "address": 34, "type": "int64", "word_order": "CDAB"},
     MB6["Counter"]),
    ("Flow", {"area": "input", "address": 40, "type": "float64", "word_order": "DCBA"},
     MB6["Flow"]),
    ("Temp", {"area": "input", "address": 44, "type": "int32", "word_order": "BADC"},
     MB6["Temp"]),
    ("Serial", {"area": "holding", "address": 50, "type": "string", "length": 8}, MB6["Serial"]),
    ("Ready", {"area": "input", "address": 46, "type": "bool", "bit_offset": 15}, True),
    ("Neg16", {"area": "holding", "address": 70, "type": "int16"}, -2),
    ("Max16", {"area": "holding", "address": 71, "type": "uint16"}, 65535),
    ("Big32", {"area": "holding", "address": 72, "type": "uint32"}, 4000000000),
]


def umh_bridge():
    """(input section, tag_processor code) of the bridge example in the how-to, or None."""
    try:
        text = UMH_HOWTO.read_text()
    except OSError:
        return None
    m = re.search(r"^## Configure the UMH bridge$.*?^```yaml\n(.*?)^```", text, re.S | re.M)
    if not m:
        return None
    lines = m.group(1).splitlines()
    try:
        inp = lines[lines.index("input:"):lines.index("pipeline:")]
        j = next(k for k, ln in enumerate(lines) if ln.strip() == "defaults: |")
    except (ValueError, StopIteration):
        return None
    ind = len(lines[j]) - len(lines[j].lstrip())
    code = []
    for ln in lines[j + 1:]:
        if ln.strip() and len(ln) - len(ln.lstrip()) <= ind:
            break
        code.append(ln)
    import textwrap
    return "\n".join(inp).rstrip(), textwrap.dedent("\n".join(code)).strip() + "\n"


def umh_yaml(inp, outputs):
    """A benthos-umh configuration: the input section @inp, and a file of JSON lines for each
    (path, tag_processor code or None) of @outputs. None: the messages of the input as they are
    (payload and metadata); code: the tag_processor with that code, then the UMH topic, the
    payload and the metadata."""
    import textwrap
    y = ["http:", "  enabled: false", "logger:", "  level: INFO", "  format: logfmt", inp,
         "output:", "  broker:", "    pattern: fan_out", "    outputs:"]
    for path, code in outputs:
        y += [f"      - file: {{path: {json.dumps(str(path))}, codec: lines}}", "        processors:"]
        if code is not None:
            y += ["          - tag_processor:", "              defaults: |",
                  textwrap.indent(code, " " * 16).rstrip(),
                  "          - mapping: |", "              root.topic = @umh_topic"]
        else:
            y += ["          - mapping: |"]
        y += ["              root.payload = this.catch(content().string())",
              "              root.meta = @"]
    return "\n".join(y) + "\n"


class Benthos:
    """Issue #175: one benthos-umh process (configuration @yaml_text, outputs {key: path})."""

    def __init__(self, exe, work, name, yaml_text, outputs):
        self.dir = work / f"umh-{name}"
        shutil.rmtree(self.dir, ignore_errors=True)
        self.dir.mkdir(parents=True)
        self.outputs, self.closed = outputs, False
        cfg = self.dir / "config.yaml"
        cfg.write_text(yaml_text)
        self.log = self.dir / "benthos.log"
        self.proc = subprocess.Popen([exe, "-c", str(cfg)], stdin=subprocess.DEVNULL,
                                     stdout=open(self.log, "ab"), stderr=subprocess.STDOUT,
                                     cwd=self.dir)
        PROCS.append(self.proc)
        track(self, self.stop)

    def stop(self):
        if self.closed:
            return
        self.closed = True
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

    def entries(self, key):
        """The JSON lines of output @key written so far (a line in progress is left out)."""
        try:
            text = pathlib.Path(self.outputs[key]).read_text()
        except OSError:
            return []
        out = []
        for ln in text.splitlines():
            try:
                out.append(json.loads(ln))
            except ValueError:
                pass
        return out

    def log_lines(self, pattern):
        try:
            text = self.log.read_text(errors="replace")
        except OSError:
            return []
        return [ln for ln in text.splitlines() if re.search(pattern, ln)]

    def wait_log(self, pattern, timeout):
        end = time.monotonic() + timeout
        while True:
            if self.log_lines(pattern):
                return True
            if self.proc.poll() is not None or wait_left(end) <= 0:
                return False
            time.sleep(0.2)


def wait_for(fn, timeout, step=0.5):
    """The first true result of @fn within @timeout (or its last result)."""
    end = time.monotonic() + timeout
    while True:
        v = fn()
        if v or wait_left(end) <= 0:
            return v
        time.sleep(step)


def uns_values(entries, node=None):
    """{UMH topic: [values in order]} of tag_processor output @entries (of edge node @node)."""
    out = {}
    for e in entries:
        if node is None or e.get("meta", {}).get("spb_edge_node_id") == node:
            out.setdefault(e.get("topic"), []).append((e.get("payload") or {}).get("value"))
    return out


def raw_of(entries, node, name=None, kind=None, device=None):
    """The input messages of edge node @node (with metric @name, message type @kind and the
    device @device: a suffix of the device_id)."""
    out = []
    for e in entries:
        meta = e.get("meta", {})
        if meta.get("spb_edge_node_id") != node:
            continue
        if name is not None and meta.get("spb_metric_name") != name:
            continue
        if kind is not None and meta.get("spb_message_type") != kind:
            continue
        if device is not None and not meta.get("spb_device_id", "").endswith(device):
            continue
        out.append(e)
    return out


def umh_device_cfg(cfg, preset):
    """The configuration of the UMH scenario on the harness configuration @cfg."""
    cfg["sparkplug"].update(group_id=UMH_GROUP, device_prefix=UMH_PREFIX, diagnostics_s=0,
                            primary_host_id="")
    if preset:
        cfg["sparkplug"]["naming"] = "umh"
    cfg.pop("modbus_tcp", None)
    cfg["device_types"] = [{"name": "Pump", "tags": [   # #213
        {"name": "Speed", "area": "holding", "address": 80, "type": "uint16", "units": "rpm"},
        {"name": "Run", "area": "coil", "address": 5, "type": "bool"}]}]
    cfg["devices"] = [
        {"name": "cell2:meter1", "unit_id": 1, "poll_ms": 1000, "offline_after": 3, "max_gap": 10,
         "tags": [dict(t, name=n) for n, t, _ in UMH_TAGS] + [
             {"name": "Raw", "area": "holding", "address": 76, "type": "bytes", "length": 2}]},
        {"name": "cell2:pump1", "unit_id": 1, "type": "Pump", "poll_ms": 1000},
        {"name": "cell2:dead1", "unit_id": 3, "poll_ms": 1000, "offline_after": 3, "tags": [
            {"name": "Value", "area": "holding", "address": 0, "type": "uint16"}]}]
    return cfg


def run_umh(args, work, slave):
    """Issue #175: the gateway against a real benthos-umh v0.16.2 (UMH Core's sparkplug_b input),
    with the bridge of docs/how-to/connect-to-umh-core.md. Two bridges read the main mosquitto:
    A (the how-to: role secondary_active, include_edge_node_in_location false; its outputs: the
    input messages, the tag_processor of the how-to, and the same without the timestamp line) and
    B (include_edge_node_in_location true, secondary_passive). GW 'acme:berlin:gw01' (UMH preset,
    store & forward, through a relay) has a device prefix of six levels, a device with each scalar
    type, a typed device, a bytes tag and a device that never answers. Then: a restart of bridge
    A (rebirth request, names), a failed read, store & forward (is_historical and the two
    timestamp choices), and GW 'acme:berlin:gw02' without the preset (template and DataSet)."""
    exe = os.environ.get("E2E_BENTHOS_UMH", "")
    if not exe or not os.access(exe, os.X_OK):
        SKIPPED.append("umh")
        print(f"[SKIP] issue #175 UMH Core interoperability: no benthos-umh (set E2E_BENTHOS_UMH "
              f"to the release binary benthos-linux-amd64 of v{UMH_VERSION}, commit "
              f"{UMH_COMMIT}; got {exe!r})", flush=True)
        return
    bridge = umh_bridge()
    if bridge is None:
        check("issue #175 the bridge example of the how-to can be read", False, str(UMH_HOWTO))
        return
    inp, code = bridge
    try:
        version = subprocess.run([exe, "--version"], capture_output=True, text=True,
                                 timeout=30).stdout
    except (OSError, subprocess.SubprocessError) as e:
        version = str(e)
    m = re.search(r"Version:\s*(\S+)", version)
    version = m.group(1) if m else version.strip()[:80]
    with open(exe, "rb") as f:
        digest = hashlib.file_digest(f, "sha256").hexdigest()
    print(f"benthos-umh {version}, sha256 {digest} "
          f"({'the release asset' if digest == UMH_SHA256 else 'NOT the release asset'})",
          flush=True)
    check(f"issue #175 benthos-umh is v{UMH_VERSION} (commit {UMH_COMMIT})",
          version == UMH_VERSION, f"{version}; sha256 {digest}")
    arrival = "\n".join(ln for ln in code.splitlines()
                        if UMH_TS_LINE not in ln and "Sparkplug timestamp" not in ln) + "\n"
    port = MQTT_PORT + 30
    tu = f"spBv1.0/{UMH_GROUP}"
    node, node2 = UMH_NODE, UMH_NODE2
    meter, pump, dead = (f"{UMH_PREFIX}:cell2:{d}" for d in ("meter1", "pump1", "dead1"))
    snap = slave.snapshot()
    slave.ctx.setValues(3, 70, [0xFFFE, 65535, 4000000000 >> 16, 4000000000 & 0xFFFF])
    slave.ctx.setValues(3, 76, [0xABCD, 0x0102])
    slave.ctx.setValues(3, 80, [1450])
    slave.ctx.setValues(1, 5, [1])
    relay = Relay(port, MQTT_PORT)
    obs = Observer()

    def bridge_a(name):
        d = work / f"umh-{name}"
        outs = {"raw": d / "raw.jsonl", "uns": d / "uns.jsonl", "arrival": d / "uns-arrival.jsonl"}
        return Benthos(exe, work, name, umh_yaml(inp, [(outs["raw"], None), (outs["uns"], code),
                                                       (outs["arrival"], arrival)]), outs)

    inp_b = inp.replace("include_edge_node_in_location: false",
                        "include_edge_node_in_location: true") \
        .replace('role: "secondary_active"', 'role: "secondary_passive"') \
        .replace('client_id: "umh-sparkplug-acme"', 'client_id: "umh-sparkplug-acme-b"')
    inp_a = inp.replace("tcp://192.168.1.10:1883", f"tcp://127.0.0.1:{MQTT_PORT}")
    inp_b = inp_b.replace("tcp://192.168.1.10:1883", f"tcp://127.0.0.1:{MQTT_PORT}")
    inp = inp_a
    db = work / "umh-b"
    a = bridge_a("a")
    bridges = [a]
    b = Benthos(exe, work, "b", umh_yaml(inp_b, [(db / "raw.jsonl", None), (db / "uns.jsonl", code)]),
                {"raw": db / "raw.jsonl", "uns": db / "uns.jsonl"})
    bridges.append(b)
    subscribed = all(x.wait_log(r"Subscribed to Sparkplug topic", 30) for x in (a, b))
    check("issue #175 the two bridges (the how-to configuration, and include_edge_node_in_location "
          "true) connect to the broker and subscribe", subscribed and "include_edge_node" in inp_b
          and f"127.0.0.1:{MQTT_PORT}" in inp_b,
          f"A: {(a.log_lines(r'Subscribed') or a.log_lines(r'.')[-1:] or ['no log'])[0][:200]}; "
          f"B: {(b.log_lines(r'Subscribed') or b.log_lines(r'.')[-1:] or ['no log'])[0][:200]}")
    gw = None
    try:
        cfg = umh_device_cfg(node_config(node, port=port), preset=True)
        cfg["sparkplug"]["store_forward"] = True
        t0 = time.monotonic()
        gw = boot_variant(args, work, "umh", cfg)
        nb = obs.wait(f"{tu}/NBIRTH/{node}", 60)
        dbirth = obs.wait(f"{tu}/DBIRTH/{node}/{meter}", 20)
        obs.wait(f"{tu}/DBIRTH/{node}/{pump}", 10)
        alias = {mm.name: mm.alias for mm in dbirth[2].metrics} if dbirth else {}
        dev_topic = f"umh.v1.{UMH_LOC}.cell2.meter1._raw"
        node_topic = "umh.v1.acme.berlin.gw01._raw"
        want_dev = {f"{dev_topic}.{n.replace('/', '.')}" for n, _, _ in UMH_TAGS}
        want_node = {f"{node_topic}.{t}" for t in ("DI.1", "Counter.1", "Node_Info.Uptime",
                                                   "Node_Info.Time_Synced",
                                                   "Devices.cell2.meter1.Online",
                                                   "Devices.cell2.pump1.Online",
                                                   "Devices.cell2.dead1.Online",
                                                   "Node_Info.Modbus.Cycles")}
        want_pump = {f"umh.v1.{UMH_LOC}.cell2.pump1._raw.{t}" for t in ("Speed", "Run")}
        # the dead device goes offline after 3 failed polls; its state then is false
        wait_for(lambda: uns_values(a.entries("uns"), node).get(
            f"{node_topic}.Devices.cell2.dead1.Online", [None])[-1] is False
            and want_dev | want_node | want_pump <= set(uns_values(a.entries("uns"), node)), 30)
        warns = gw.find(r"configuration: \d+ warning|warning: .*bytes|warning: .*type", t0)
        uns = uns_values(a.entries("uns"), node)
        raw = a.entries("raw")
        dev_loc = {e["meta"].get("umh_location_path") for e in raw_of(raw, node, kind="DBIRTH",
                                                                      device="meter1")
                   if e["meta"].get("umh_conversion_status") == "success"}
        missing = sorted((want_dev | want_node | want_pump) - set(uns))
        check("issue #175 UMH-1 location path and tag names: device prefix of six levels and a "
              "Parris device name -> umh.v1.acme.berlin.tempelhof.packaging.hall2.line3.cell2."
              "meter1._raw.<tag> (L1/Voltage -> L1.Voltage); node metrics under the edge node "
              "acme:berlin:gw01 (Node Info/Uptime -> Node_Info.Uptime); no bdSeq, Node Control or "
              "alias_<n> topic", nb and dbirth and not missing
              and dev_loc == {f"{UMH_LOC}.cell2.meter1"}
              and not [t for t in uns if re.search(r"bdSeq|Node_Control|alias_", t or "")],
              f"missing {missing[:6]}; location of the device {sorted(dev_loc)}; {len(uns)} UNS "
              f"topics of the node, e.g. {sorted(uns)[:4]}")
        # Issue #160 (decision of 2026-11-14): no message goes out before a valid clock, so no
        # UNS value has a timestamp near 1970 (with the timestamp line of the how-to UMH keeps the
        # metric timestamp); for information only
        old = [(e.get("topic"), e["payload"].get("timestamp_ms")) for e in a.entries("uns")
               if isinstance(e.get("payload"), dict)
               and (e["payload"].get("timestamp_ms") or 2 ** 62) < 10 ** 12]
        print(f"[INFO] issue #175/#160: {len(old)} UNS values with a timestamp before 2001 "
              f"(0 expected), e.g. {old[:3]}", flush=True)
        # UMH-2: the values. The input of the bridge has them exact; the tag_processor runs
        # JavaScript, where each number is a double: 64-bit integers beyond 2^53 lose digits.
        got_raw, got_uns, bad = {}, {}, []
        for n, _, v in UMH_TAGS:
            r = raw_of(raw, node, name=n, kind="DBIRTH", device="meter1")
            got_raw[n] = r[-1]["payload"].get("value") if r else "absent"
            got_uns[n] = (uns.get(f"{dev_topic}.{n.replace('/', '.')}") or ["absent"])[0]
            want_uns = float(v) if isinstance(v, int) and not isinstance(v, bool) \
                and abs(v) > 2 ** 53 else v
            if got_raw[n] != v or isinstance(got_raw[n], bool) != isinstance(v, bool):
                bad.append(f"input {n}={got_raw[n]!r} (want {v!r})")
            if got_uns[n] != want_uns:
                bad.append(f"UNS {n}={got_uns[n]!r} (want {want_uns!r})")
        types = {e["meta"].get("spb_metric_name"): e["meta"].get("spb_datatype")
                 for e in raw_of(raw, node, kind="DBIRTH", device="meter1")}
        check("issue #175 UMH-2 values of each scalar type (bool, int16 with and without a scale, "
              "uint16, int32, uint32, int64, uint64, float32, float64, string): exact at the input "
              "of the bridge, signed integers included; in the UNS exact too, except the 64-bit "
              "integers beyond 2^53, which the JavaScript of the tag_processor rounds to a double "
              f"(Energy {MB6['Energy']} -> {float(MB6['Energy']):.0f}, Counter {MB6['Counter']} -> "
              f"{float(MB6['Counter']):.0f})", not bad,
              f"{'; '.join(bad[:6]) or 'all as expected'}; UNS {got_uns}; datatypes {types}")
        # UMH-6: the UMH preset: the typed device as plain metrics, the device state, the sums of
        # the Modbus counters; the bytes tag has no value in UMH
        bytes_in = raw_of(raw, node, name="Raw", kind="DBIRTH", device="meter1")
        bytes_meta = bytes_in[-1]["meta"] if bytes_in else {}
        online = {d: (uns.get(f"{node_topic}.Devices.cell2.{d}.Online") or [None])[-1]
                  for d in ("meter1", "pump1", "dead1")}
        pump_v = {t: (uns.get(f"umh.v1.{UMH_LOC}.cell2.pump1._raw.{t}") or [None])[-1]
                  for t in ("Speed", "Run")}
        cycles = (uns.get(f"{node_topic}.Node_Info.Modbus.Cycles") or [None])[-1]
        check("issue #175 UMH-6 the UMH preset: the typed device cell2:pump1 shows its tags (Speed "
              "1450, Run true), Devices/<name>/Online shows each device (cell2:dead1 false), "
              "Node Info/Modbus/Cycles is a number; the bytes tag has a null value at the input "
              "and no UNS topic; the gateway warns about the bytes tag",
              pump_v == {"Speed": 1450, "Run": True}
              and online == {"meter1": True, "pump1": True, "dead1": False}
              and isinstance(cycles, int) and bytes_in and bytes_in[-1]["payload"].get("value")
              is None and bytes_meta.get("umh_conversion_status") != "success"
              and f"{dev_topic}.Raw" not in uns and warns,
              f"pump {pump_v}; online {online}; Modbus cycles {cycles!r}; bytes tag "
              f"{bytes_in[-1]['payload'] if bytes_in else 'absent'} (status "
              f"{bytes_meta.get('umh_conversion_status')}, datatype "
              f"{bytes_meta.get('spb_datatype')}); warnings {warns[:2]}")
        # UMH-5: include_edge_node_in_location true (bridge B)
        raw_b = b.entries("raw")
        loc_b = {e["meta"].get("umh_location_path") for e in raw_of(raw_b, node, kind="DBIRTH",
                                                                    device="meter1")
                 if e["meta"].get("umh_conversion_status") == "success"}
        loc_bn = {e["meta"].get("umh_location_path") for e in raw_of(raw_b, node, kind="NBIRTH")
                  if e["meta"].get("umh_conversion_status") == "success"}
        check("issue #175 UMH-5 include_edge_node_in_location true puts the edge node id before "
              "the device_id (acme.berlin.gw01.acme.berlin.tempelhof...cell2.meter1); the node "
              "metrics keep acme.berlin.gw01",
              loc_b == {f"acme.berlin.gw01.{UMH_LOC}.cell2.meter1"} and loc_bn == {"acme.berlin.gw01"},
              f"device {sorted(loc_b)}; node {sorted(loc_bn)}")

        # UMH-3: a restart of bridge A. Before its first DATA it has no BIRTH: the DATA gives
        # alias_<n> and, without the datatype of the BIRTH (#159), a negative int16 as a large
        # number. secondary_active asks for a rebirth; after it the names come back.
        a.stop()
        restart = time.monotonic()
        a2 = bridge_a("a2")
        bridges.append(a2)
        up = a2.wait_log(r"Subscribed to Sparkplug topic", 30)
        obs.drain()
        slave.ctx.setValues(3, 70, [0xFFFD])   # Neg16 -3: a DDATA before bridge A has a BIRTH
        ncmd = obs.wait(f"{tu}/NCMD/{node}", 20,
                        lambda p: any(mm.name == "Node Control/Rebirth" and mm.boolean_value
                                      for mm in p.metrics), since=restart)
        nb2 = obs.wait(f"{tu}/NBIRTH/{node}", 20, since=restart)
        neg_t = f"{dev_topic}.Neg16"
        wait_for(lambda: -3 in uns_values(a2.entries("uns"), node).get(neg_t, []), 20)
        raw2, uns2 = a2.entries("raw"), uns_values(a2.entries("uns"), node)
        before = [e for e in raw2 if e["meta"].get("spb_metric_name", "").startswith("alias_")
                  and e["meta"].get("spb_edge_node_id") == node]
        neg_alias = [e["payload"].get("value") for e in before
                     if e["meta"].get("spb_alias") == str(alias.get("Neg16"))]
        rebirth_logs = a2.log_lines(r"rebirth for")
        check("issue #175 UMH-3 restart of the bridge (secondary_active): the first DDATA has no "
              "BIRTH (alias_<n>, Neg16 -3 not as -3, the how-to drops it); the bridge sends NCMD "
              "Node Control/Rebirth, the gateway answers with NBIRTH and DBIRTH, and the UNS "
              "has the names and Neg16 -3; no alias_<n> topic in the UNS",
              up and ncmd and nb2 and neg_alias and all(v != -3 for v in neg_alias)
              and uns2.get(neg_t, [None])[-1] == -3
              and not [t for t in uns2 if "alias_" in (t or "")]
              and want_dev <= set(uns2),
              f"NCMD {'yes' if ncmd else 'no'} {round(ncmd[0] - restart, 1) if ncmd else ''} s, "
              f"NBIRTH {'yes' if nb2 else 'no'}; before the BIRTH: "
              f"{[(e['meta'].get('spb_metric_name'), e['payload'].get('value')) for e in before][:4]};"
              f" UNS Neg16 {uns2.get(neg_t)}; bridge log {[ln[-120:] for ln in rebirth_logs[:2]]}")

        # UMH-6 (failed read): a read with an exception gives a null value with Quality BAD;
        # the bridge has no value for it, and the UNS keeps the last value
        v_name, v_topic = "L1/Voltage", f"{dev_topic}.L1.Voltage"
        obs.drain()
        slave.ctx.refuse.add((4, 0))
        bad_dd = obs.wait(f"{tu}/DDATA/{node}/{meter}", 10, lambda p: any(
            mm.alias == alias.get(v_name) and mm.is_null for mm in p.metrics))
        slave.ctx.refuse.clear()
        good_dd = obs.wait(f"{tu}/DDATA/{node}/{meter}", 10, lambda p: any(
            mm.alias == alias.get(v_name) and not mm.is_null for mm in p.metrics))
        time.sleep(2)
        nulls = [e for e in raw_of(a2.entries("raw"), node, name=v_name, kind="DDATA")
                 if e["payload"].get("value") is None]
        uns_v = uns_values(a2.entries("uns"), node).get(v_topic, [])
        check("issue #175 UMH-6 a failed read (exception 02): the DDATA null value with Quality BAD "
              "reaches the bridge as null (no UMH value), the how-to drops it, and the UNS keeps "
              "the last value: no null and no topic for the Quality",
              bad_dd and good_dd and nulls and None not in uns_v
              and nulls[-1]["meta"].get("umh_conversion_status") != "success",
              f"null at the input {len(nulls)} (status "
              f"{nulls[-1]['meta'].get('umh_conversion_status') if nulls else None}); UNS "
              f"L1.Voltage {uns_v}")

        # UMH-4: store & forward. While the broker is away, three Voltage values; back: NBIRTH,
        # DBIRTH, then DDATA with is_historical
        since = time.monotonic()
        relay.stop()
        gw.wait_log(r"MQTT down; reconnect attempt", 15, since)
        values = [240.0, 241.0, 242.0]
        for v in values:
            slave.set_float(4, 0, v)
            n0, end = len(slave.reads), time.monotonic() + 15
            while wait_left(end) > 0 and sum(
                    1 for r in list(slave.reads)[n0:] if r[0] == 1 and r[1] == 4
                    and r[2] is not None and r[2] <= 0 < r[2] + (r[3] or 1)) < 2:
                time.sleep(0.1)
        back_ms = int(time.time() * 1000)
        relay.start()
        nb3 = obs.wait(f"{tu}/NBIRTH/{node}", 70, since=since)

        def hist_raw():
            return [e for e in raw_of(a2.entries("raw"), node, name=v_name, kind="DDATA")
                    if e["meta"].get("spb_is_historical") == "true"]
        wait_for(lambda: len(hist_raw()) >= 3, 30)
        time.sleep(2)
        after = [mm for x in CAPTURE if nb3 and x[0] >= nb3[0]
                 and x[1] == f"{tu}/DDATA/{node}/{meter}" for mm in x[2].metrics
                 if mm.is_historical and mm.alias == alias.get(v_name)]
        sample = [(round(mm.float_value, 1), mm.timestamp) for mm in after]
        hr = [(e["payload"].get("value"), int(e["meta"].get("spb_timestamp", 0))) for e in hist_raw()]
        sample_ts = {ts for _, ts in sample}

        def hist_uns(key):
            return [(e["payload"].get("value"), e["payload"].get("timestamp_ms"))
                    for e in a2.entries(key) if e.get("topic") == v_topic
                    and e.get("meta", {}).get("spb_is_historical") == "true"
                    and e.get("meta", {}).get("spb_edge_node_id") == node]
        with_ts, without_ts = hist_uns("uns"), hist_uns("arrival")
        stale = a2.log_lines(r"stale NDEATH")
        check("issue #175 UMH-4 store & forward: the bridge gets the replayed DDATA with "
              "spb_is_historical true, the values in order and their sample times; with "
              "'msg.meta.timestamp_ms = msg.meta.spb_timestamp' the UNS keeps the sample time, "
              "without it the UNS has the arrival time",
              nb3 and [v for v, _ in sample] == values and sorted(hr) == sorted(sample)
              and [v for v, _ in with_ts] == values and {t for _, t in with_ts} == sample_ts
              and [v for v, _ in without_ts] == values
              and all(t >= back_ms - 1000 and t not in sample_ts for _, t in without_ts),
              f"gateway {sample}; input {hr}; UNS with the line {with_ts}; UNS without it "
              f"{without_ts} (broker back at {back_ms}); bridge log: {[ln[-140:] for ln in stale[:2]]}")
        gw.kill()
        gw = None

        # UMH-7: the same devices without the UMH preset: a template instance, a template
        # definition and the DataSet Node Info/Modbus, which UMH cannot read
        cfg2 = umh_device_cfg(node_config(node2), preset=False)
        gw = boot_variant(args, work, "umh-plain", cfg2)
        nb4 = obs.wait(f"{tu}/NBIRTH/{node2}", 60)
        obs.wait(f"{tu}/DBIRTH/{node2}/{pump}", 20)
        wait_for(lambda: raw_of(a2.entries("raw"), node2, kind="DBIRTH", device="pump1"), 15)
        time.sleep(2)
        raw4 = a2.entries("raw")
        tmpl_def = raw_of(raw4, node2, name="Pump", kind="NBIRTH")
        dataset = raw_of(raw4, node2, name="Node Info/Modbus", kind="NBIRTH")
        inst = raw_of(raw4, node2, kind="DBIRTH", device="pump1")
        uns4 = uns_values(a2.entries("uns"), node2)
        nulls4 = [(e["meta"].get("spb_metric_name"), e["payload"].get("value"),
                   e["meta"].get("umh_conversion_status")) for e in tmpl_def + dataset + inst]
        check("issue #175 UMH-7 without the UMH preset: the template definition (NBIRTH), the "
              "template instance of cell2:pump1 (DBIRTH) and the DataSet Node Info/Modbus reach "
              "the bridge with a null value; the UNS has no topic for cell2:pump1 and no "
              "Devices/<name>/Online",
              nb4 and tmpl_def and dataset and inst
              and all(v is None and s != "success" for _, v, s in nulls4)
              and not [t for t in uns4 if ".cell2.pump1." in (t or "") or ".Devices." in (t or "")]
              and f"{dev_topic}.Neg16" in uns4,
              f"{nulls4[:4]}; UNS topics of gw02 {len(uns4)}, pump1 or Devices: "
              f"{[t for t in uns4 if '.cell2.pump1.' in (t or '') or '.Devices.' in (t or '')][:3]}")
        gw.kill()
        gw = None
        a2.stop()
    finally:
        if gw is not None:
            gw.kill()
        for x in bridges:
            x.stop()
        obs.close()
        relay.stop()
        slave.restore(snap)


def start_broker(conf, log):
    # A renamed copy of the binary: other jobs on a shared host that clean up with
    # `pkill mosquitto` must not take this broker down in the middle of the scenario.
    binary = conf.parent / "spb-e2e-broker"
    if not binary.exists():
        shutil.copy(shutil.which("mosquitto"), binary)
    proc = subprocess.Popen([str(binary), "-c", str(conf), "-v"], stdout=open(log, "w"),
                            stderr=subprocess.STDOUT)
    PROCS.append(proc)
    time.sleep(0.5)
    return proc


SECURE_OPTIONS = ("NVS_ENCRYPTION", "SECURE_SIGNED_APPS_NO_SECURE_BOOT")


def check_args(args, sdkconfig_text):
    """Issue #261: why the arguments do not agree with each other or with the sdkconfig of the
    build under test (@sdkconfig_text), or None. --secure must say what the build is: with a
    wrong value, each scenario of the secure variant would be a [SKIP] (or would run on a build
    without its function), and a run would pass with nothing examined."""
    on = [o for o in SECURE_OPTIONS if re.search(rf"^CONFIG_{o}=y$", sdkconfig_text, re.M)]
    if args.secure and len(on) != len(SECURE_OPTIONS):
        missing = sorted(set(SECURE_OPTIONS) - set(on))
        return (f"--secure, but --sdkconfig has not CONFIG_{' nor CONFIG_'.join(missing)}: not "
                f"the secure build (sdkconfig.qemu-secure)")
    if not args.secure and on:
        return (f"--sdkconfig has CONFIG_{' and CONFIG_'.join(on)}: the secure build needs "
                f"--secure (run_e2e.sh E2E_SECURE=1)")
    if args.secure != bool(args.signing_key):
        return ("--secure and --signing-key go together: only the secure build requires signed "
                "images, and the harness signs only for it")
    if not args.secure and (args.board_flash or args.board_app or args.board_elf):
        return "--board-flash, --board-app and --board-elf are only for --secure"
    if bool(args.board_flash) != bool(args.board_app):
        return "--board-flash and --board-app go together (the scenario of issues #94 and #102)"
    if args.simulate_qemu_death in ("hang", "hang-mute") and not args.secure:
        return (f"--simulate-qemu-death {args.simulate_qemu_death} needs --secure: it stops the "
                f"emulator at 'Take trusted digest key(s)', a line that only the secure build "
                f"writes; on another build it does nothing and the run passes")
    return None


def main():
    global MQTT_PORT, MODBUS_PORT, CONSOLE_PORT, SYSLOG_PORT, HTTP_PORT, HTTPS_PORT, MBTCP_PORT
    global ARGS, HANG_S, MON_DIR, WORK
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", required=True)
    ap.add_argument("--flash", required=True)
    ap.add_argument("--efuse", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--littlefs-python", required=True,
                    help="python with the littlefs-python package (build-qemu/littlefs_py_venv)")
    ap.add_argument("--sdkconfig", required=True, help="sdkconfig of the QEMU build")
    ap.add_argument("--elf", required=True, help="ELF of the QEMU build (core dump decoding)")
    ap.add_argument("--idf-python", required=True, help="ESP-IDF python (has esp_coredump)")
    ap.add_argument("--simulate-qemu-death", nargs="?", const="ota",
                    choices=("ota", "boot", "console", "hang", "hang-mute", "ota-failed",
                             "ota-panic", "main"),
                    help="issues #86/#90/#92/#96/#105 self-tests. Repeated once, then pass: 'ota' "
                         "(the default: SIGABRTs the emulator in the OTA scenario, leaving an "
                         "observer subscribed), 'boot' (SIGSEGVs the second emulator 3 s into its "
                         "boot), 'console' (SIGSEGVs the first one before its console connects), "
                         "'main' (SIGABRTs it in the main scenario after its broker swap). Not "
                         "repeated (the run fails): 'hang' (only with --secure: the guest paused "
                         "through the QEMU monitor right after 'Take trusted digest key(s)': "
                         "detected, dumped, never repeated), 'hang-mute' (the same with the QEMU "
                         "process SIGSTOPped), "
                         "'ota-failed' (a failed check, then the death), 'ota-panic' (an unknown "
                         "firmware panic on the console, then the death)")
    ap.add_argument("--tcg-thread", choices=("auto", "single", "multi"),
                    default=os.environ.get("E2E_TCG_THREAD", "auto"),
                    help="issue #107: QEMU's TCG threading (E2E_TCG_THREAD): 'auto' (default) runs "
                         "the flash-mapping OTA scenarios with both vCPUs on one host thread "
                         "(SINGLE_TCG_SCENARIOS) and the rest with MTTCG; 'single' or 'multi' for "
                         "every scenario. MTTCG crashes QEMU now and then under host load in the "
                         "OTA scenarios; single-threaded TCG is too slow for the latency bounds "
                         "of the others")
    ap.add_argument("--board-elf", default="",
                    help="ELF of the --board-flash build (symbols for the fault monitor)")
    ap.add_argument("--nm", default="xtensa-esp32s3-elf-nm",
                    help="nm of the toolchain (the fault monitor's symbols, issue #92)")
    ap.add_argument("--hang-timeout", type=float, default=HANG_S,
                    help="issue #92: seconds without console output in a boot or an image check "
                         "after which the emulator counts as hung")
    ap.add_argument("--scenario-timeout", type=float, default=0,
                    help="issue #92: seconds every scenario attempt may take (default: each "
                         "scenario's own budget, SCENARIO_BUDGET_S)")
    ap.add_argument("--timeout-scale", type=float,
                    default=float(os.environ.get("E2E_TIMEOUT_SCALE", "1")),
                    help="issue #92: factor on the scenario budgets (E2E_TIMEOUT_SCALE; a slower "
                         "host)")
    ap.add_argument("--signing-key", default="",
                    help="FW-2: the test signing key of the secure build (signed images "
                         "required); without it the harness signs no image (issue #261)")
    ap.add_argument("--secure", action="store_true",
                    help="issue #261: the image under test is the secure build "
                         "(sdkconfig.qemu-secure: NVS encryption, signed OTA images); without it, "
                         "the configuration of the board, and the scenarios of these two "
                         "functions show [SKIP]")
    ap.add_argument("--board-flash", default="",
                    help="issues #94 and #261: with --secure, flash image of the board build "
                         "(no NVS encryption), the image before the OTA update")
    ap.add_argument("--downgrade-app", default="",
                    help="issue #148: app image of an earlier release (run_e2e.sh builds it from "
                         "E2E_DOWNGRADE_REF) for the optional scenario 'downgrade'")
    ap.add_argument("--downgrade-ref", default="",
                    help="issue #148: the git ref of --downgrade-app (for the messages)")
    ap.add_argument("--board-app", default="",
                    help="issue #102: the app image of that build (an OTA update the "
                         "device must refuse once its NVS is encrypted)")
    ap.add_argument("--mqtt-port", type=int, default=MQTT_PORT)
    ap.add_argument("--modbus-port", type=int, default=MODBUS_PORT)
    ap.add_argument("--console-port", type=int, default=CONSOLE_PORT)
    ap.add_argument("--syslog-port", type=int, default=SYSLOG_PORT)
    ap.add_argument("--mbtcp-port", type=int, default=MBTCP_PORT,
                    help="host port QEMU forwards to the gateway's Modbus TCP port 502")
    ap.add_argument("--http-port", type=int, default=HTTP_PORT)
    ap.add_argument("--https-port", type=int, default=HTTPS_PORT)
    ap.add_argument("--only", default="",
                    help=f"comma-separated scenarios to run ({','.join(scenarios.ONLY_NAMES)}); "
                         "default the full run of the variant (issue #135: the PSRAM variant runs "
                         "all but the OTA trial scenarios, the variant without PSRAM only those; "
                         "issue #261: the secure variant only the scenarios of the NVS encryption "
                         "and of the signed OTA images). "
                         "'ota' is ota-fw1,ota-park,ota-wiring,ota-trial; 'ota-fw1' is "
                         "ota-fw1-ncmd,ota-fw1-web (issue #233); 'nvs' is nvs-boot,nvs-ota; "
                         "'refuse' runs alone only without 'main' (which includes it)")
    ap.add_argument("--web-stress", type=int, default=0, metavar="N",
                    help="issue #110: N extra rounds of cut sessions and accepted PUTs in the "
                         "https scenario (GW22), to reproduce a connection that never reached httpd")
    ap.add_argument("--psram", action="store_true",
                    help="issue #63: emulate the octal PSRAM (build with sdkconfig.qemu-psram)")
    ap.add_argument("--panics", type=int, default=1,
                    help="issue #62: panics in the core dump check (each dump must decode)")
    ap.add_argument("--no-isolate", action="store_true",
                    help="stay on the host loopback instead of a private network namespace")
    ap.add_argument("--no-skip", action="store_true",
                    help="issue #261: a selected scenario that shows [SKIP] is a failed check "
                         "(the CI groups: each of their scenarios must run)")
    args = ap.parse_args()
    try:
        only = scenarios.parse_only(args.only)   # issue #121: a typo must not select nothing
    except ValueError as e:
        ap.error(str(e))
    wrong = check_args(args, pathlib.Path(args.sdkconfig).read_text())
    if wrong:
        ap.error(wrong)
    ARGS, HANG_S = args, args.hang_timeout
    SYMBOLS.update(load_symbols(args.nm, [args.elf, args.board_elf]))
    global QEMU_VERSION
    try:
        QEMU_VERSION = subprocess.run([args.qemu, "--version"], capture_output=True, text=True,
                                      timeout=10).stdout.splitlines()[0]
    except (OSError, subprocess.SubprocessError, IndexError) as e:
        QEMU_VERSION = f"unknown ({e})"
    pattern = pathlib.Path("/proc/sys/kernel/core_pattern")
    pattern = pattern.read_text().strip() if pattern.exists() else "?"
    print(f"emulator: {QEMU_VERSION}; core dumps up to {allow_core_dumps() >> 20} MB, "
          f"core_pattern {pattern!r}" + ("" if not pattern.startswith("|") else
                                         " (piped to a handler: no core file in qemu/)"),
          flush=True)
    print(f"fault monitor symbols: {', '.join(sorted(SYMBOLS)) or 'none'}", flush=True)
    MQTT_PORT, MODBUS_PORT, CONSOLE_PORT = args.mqtt_port, args.modbus_port, args.console_port
    SYSLOG_PORT, HTTP_PORT, HTTPS_PORT = args.syslog_port, args.http_port, args.https_port
    MBTCP_PORT = args.mbtcp_port
    global PROBE_MAX_S, SELFCHECK_S, SELFCHECK_WAIT_S
    PROBE_MAX_S = sdkconfig_int(args.sdkconfig, "GW_MB_OFFLINE_PROBE_MAX_MS") / 1000
    SELFCHECK_S = sdkconfig_int(args.sdkconfig, "GW_FW_SELFCHECK_S")
    SELFCHECK_WAIT_S = SELFCHECK_S + SELFCHECK_MARGIN_S
    isolated = False if args.no_isolate else isolate_network()
    print(f"network: {'private namespace' if isolated else 'host loopback'}; ports MQTT {MQTT_PORT}, "
          f"Modbus {MODBUS_PORT}, console {CONSOLE_PORT}, syslog {SYSLOG_PORT}, HTTP {HTTP_PORT}, "
          f"HTTPS {HTTPS_PORT}",
          flush=True)
    work = pathlib.Path(args.workdir)
    WORK = work
    # issue #92: the QEMU monitor sockets (a UNIX socket path has at most 107 characters)
    import tempfile
    MON_DIR = pathlib.Path(tempfile.mkdtemp(prefix="e2e-mon-"))
    # issue #97: the eFuse file before any scenario burns a key (KEY5 for the NVS keys)
    shutil.copy(args.efuse, work / "efuse-blank.bin")
    log = work / "uart0.log"
    # The image under test gets the harness' config.json (with its MQTT port).
    args.flash = str(flash_variant(args, work, "main", {"config.json": config_json()}))
    if args.psram:
        write_otadata(args.flash)   # every variant is a copy of this image

    conf = work / "broker.conf"
    conf.write_text(f"listener {MQTT_PORT} 127.0.0.1\nallow_anonymous true\n")
    broker = start_broker(conf, work / "mosquitto.log")
    MAIN_BROKER.update(proc=broker, conf=conf)
    stop = threading.Event()
    threading.Thread(target=fault_monitor, args=(stop,), daemon=True).start()
    threading.Thread(target=sntp_responder, args=(stop,), daemon=True).start()
    threading.Thread(target=syslog_listener, args=(stop,), daemon=True).start()

    slave = ModbusSlave()
    slave.reset()
    slave.start()
    MAIN_BROKER["slave"] = slave
    # Issues #86/#90: every scenario through run_scenario: a QEMU that dies under one (at any
    # boot) is reported as such and the scenario runs once more; firmware failures are not
    # retried. The OTA scenario is the one where it happens most (docs/reference/e2e-harness.md). Issue #121: the
    # selection and the order come from scenarios.SCENARIOS (the CI groups are checked on it).
    runners = {
        "failsafe": lambda: run_config_failsafe(args, work, slave),
        "network": lambda: run_network_variant(args, work, slave),
        "web": lambda: run_web(args, work),
        "reset": lambda: run_factory_reset(args, work),
        "tls": lambda: run_tls(args, work),
        "failover": lambda: run_failover(args, work),
        "mqtt5": lambda: run_mqtt5(args, work, work / "mosquitto.log"),
        "max_packet": lambda: run_max_packet(args, work, slave),
        "phost": lambda: (run_primary_host(args, work), run_primary_host_brokers(args, work)),
        "sf": lambda: run_store_forward(args, work, slave),
        "sf spill": lambda: run_store_forward_spill(args, work, slave),
        "stack margin": lambda: run_stack_margin(args, work, slave),
        "https": lambda: run_https(args, work),
        "time": lambda: run_valid_time(args, work, slave),
        "FW-1 OTA by NCMD": lambda: run_ota(args, work, "ncmd"),
        "FW-1 OTA by web upload": lambda: run_ota(args, work, "web"),
        "issue #112 park in a long RS485 transaction": lambda: run_park_long_txn(args, work, slave),
        "issue #97 OTA trial wiring": lambda: run_ota_wiring(args, work),
        "issue #144 OTA self-check": lambda: run_ota_trial(args, work),
        "CFG-7 NVS encryption": lambda: run_nvs_encryption(args, work),
        "issues #67/#94 NVS migration": lambda: run_nvs_migration(args, work),
        "issue #109 full unencrypted NVS": lambda: run_nvs_full(args, work),
        "issue #94 OTA to NVS encryption": lambda: run_nvs_ota(args, work),
        "FW-2 OTA signature": lambda: run_ota_signature(args, work),
        "slow": lambda: run_slow_link(
            args, work, sdkconfig_int(args.sdkconfig, "GW_MQTT_PUBLISH_BUDGET_MS")),
        "issue #135 no PSRAM": lambda: run_no_psram(args, work, slave),
        # part of "main"; alone only on request (#89)
        "refuse": lambda: check_suback_refused(args, work),
        "main": lambda: run(args, log, slave, MAIN_BROKER["proc"], conf, work),
        "umh": lambda: run_umh(args, work, slave),   # optional (issue #175)
        # issue #176: only with --only tck, and only with E2E_TCK_DIR (else [SKIP])
        "tck": lambda: tck.run(sys.modules[__name__], args, work, slave),
        # issue #148: only with --only downgrade, and only with E2E_DOWNGRADE_REF (else [SKIP])
        "downgrade": lambda: run_downgrade(args, work),
    }
    assert set(runners) == {name for name, _ in scenarios.SCENARIOS}
    # issue #261: the scenarios of the two functions of the secure build need that build
    for name in scenarios.SECURE_VARIANT_SCENARIOS:
        runners[name] = secure_only(args, name, runners[name])
    # issues #135 and #261: without --only, the full run of this variant (scenarios.variant_of)
    chosen = scenarios.selected(only, "secure" if args.secure else
                                "psram" if args.psram else "default")
    try:
        for name in chosen:
            run_scenario(name, runners[name])
    except BaseException:
        traceback.print_exc()   # the summary below exits: keep the reason visible
        check("harness completed every scenario", False, traceback.format_exc().splitlines()[-1])
        raise
    finally:
        stop.set()
        for proc in PROCS:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        # Criterion 6 over everything captured
        bad = 0
        dump = work / "payloads"
        dump.mkdir(exist_ok=True)
        # #159: DATA has no datatype; its value field must match the datatype of the birth (each
        # list in its own order: a discarded attempt is another boot)
        types = {id(CAPTURE): spb_decode.BirthTypes(strict=False),
                 id(DISCARDED_CAPTURE): spb_decode.BirthTypes(strict=False)}
        numbered = [(src, item) for src in (CAPTURE, DISCARDED_CAPTURE) for item in src]
        for i, (src, (_, topic, p, raw)) in enumerate(numbered):
            kind, node, device = spb_decode.topic_parts(topic)
            (dump / f"{i:03d}_{kind}.bin").write_bytes(raw)
            errs = spb_decode.check(kind, p) + types[id(src)].feed(kind, node, device, p)
            for e in errs[:3]:
                print(f"criterion 6: {topic}: {e}", flush=True)
            bad += len(errs)
        if not CAPTURE and chosen and set(chosen) <= set(SKIPPED):
            # issues #175/#176: every scenario of the run was a SKIP (a program it needs is absent)
            print("[SKIP] 6. payloads decode: every selected scenario was skipped", flush=True)
        else:
            check(f"6. payloads decode with the official sparkplug_b.proto (Python protobuf "
                  f"{google.protobuf.__version__})",
                  CAPTURE and bad == 0,
                  f"{len(CAPTURE) + len(DISCARDED_CAPTURE)} messages decoded "
                  f"({len(DISCARDED_CAPTURE)} of discarded attempts), {bad} structural errors")
        runs, errs, prev = 0, [], None
        for d in syslog_from(NODE):
            if d[4] == 1:
                runs += 1
            elif prev is None or d[4] != prev + 1:
                errs.append(f"{prev}->{d[4]}")
            prev = d[4]
        if "main" in chosen:   # GW01's syslog: only the main scenario has it (E2E_POISON: ota)
            check("REL-4 syslog sequenceId continuous within each boot (UDP over the emulator's "
                  "NAT, nothing lost)", runs >= 1 and not errs,
                  f"{len(syslog_from(NODE))} datagrams in {runs} boot(s); gaps {errs[:5]}")
        else:
            print("[SKIP] REL-4 syslog sequenceId continuity: the main scenario did not run",
                  flush=True)
        check("Issue #105 no message reached an observer of a scenario attempt that had ended "
              "(run_scenario closes what an attempt leaves open: observers, relays, fake brokers, "
              "image servers, private brokers)", not STALE,
              f"{len(STALE)} stale deliveries" + (f", first {STALE[0][1]} for attempt {STALE[0][2]}"
                                                  if STALE else ""))
        check("Issue #92 the fault monitor ran without an error of its own", not MONITOR_ERRORS,
              "; ".join(MONITOR_ERRORS[:3]) or "ok")
        parks = [int(m.group(1)) for g in GATEWAYS for _, ln in list(g.lines)
                 if (m := RESUMED_RX.search(ln))]
        gave_up = [ln for g in GATEWAYS for _, ln in list(g.lines) if PARK_BOUND_RX.search(ln)]
        check("Issue #111 every park of core 1 (QEMU build) ended within its bound and none was "
              "refused; issue #112: no blocking IPC call reached core 1 while it was parked (all "
              "emulators of the run)", not gave_up,
              f"{len(parks)} parks, longest {max(parks) if parks else 0} ms; {gave_up[:2]}")
        if args.no_skip:
            # issue #261: in a CI group each scenario must run; a SKIP there is a broken build
            # or a broken job, and it must not pass
            for name in dict.fromkeys(SKIPPED):
                check(f"--no-skip: the scenario '{name}' ran", False,
                      "skipped (see its [SKIP] line above)")
        seq_errors = seq_continuity(CAPTURE)
        check("SPB-5 seq continuity per session", not seq_errors, "; ".join(seq_errors) or "ok")
        # issue #86: the retries next to the checks (their discarded checks included); #92:
        # every emulator fault (with its state dump file) and each attempt's duration
        # issue #107: every emulator (command line, host load, how it ended, its core dump)
        emus = []
        for g in GATEWAYS:
            e = dict(getattr(g, "info", {}))
            e["ended"] = g.qemu_died() or "ended by the harness"
            e["core"] = g.core_file() if g.proc is not None else None
            if e["core"]:
                e["backtrace"] = core_backtrace(args.qemu, WORK / e["core"])
            emus.append(e)
        (work / "results.json").write_text(json.dumps({"checks": results, "retries": RETRIES,
                                                       "faults": FAULTS, "durations": DURATIONS,
                                                       "remeasured": REMEASURED,
                                                       "skipped": list(dict.fromkeys(SKIPPED)),
                                                       "qemu": QEMU_VERSION, "emulators": emus},
                                                      indent=2))
        failed = [r for r in results if not r["ok"]]
        # issue #90: every emulator that ended on its own, with its stderr tail, and the
        # artefacts left in place (the harness deletes nothing)
        exits = [d for d in (g.qemu_died() for g in GATEWAYS) if d]
        (work / "qemu-exits.txt").write_text("".join(f"{d}\n" for d in exits) or "none\n")
        shutil.rmtree(MON_DIR, ignore_errors=True)
        if failed or exits or FAULTS:
            print(f"artefacts kept for the diagnosis: {work} (uart0-*.log, uart0-*.log.qemu, "
                  f"qemu-exits.txt: {len(exits)} QEMU exit(s) not requested by the harness; "
                  f"fault-*.txt: {sum(1 for f in FAULTS if f['dump'])} state dump(s))")
        for f in FAULTS:
            print(f"[FAULT] {f['scenario']} attempt {f['attempt']}: {f['kind']} ({f['gateway']}): "
                  f"{f['signature'] or f['detail']}{'; ' + f['dump'] if f['dump'] else ''}")
        gha = os.environ.get("GITHUB_ACTIONS") == "true"
        for r in RETRIES:
            lost = [c for c in r["discarded_checks"] if not c["ok"]]
            print(f"[RETRY] {r['scenario']}: {r['qemu']}; {len(lost)} discarded failed check(s)"
                  + "".join(f"\n[RETRY]   discarded: {c['criterion']}: {c['detail'][:300]}"
                            for c in lost))
            if gha:   # issue #96: visible in the CI log, not only in results.json
                print(f"::warning title=E2E scenario repeated::{r['scenario']}: {r['qemu']}; "
                      f"{len(lost)} discarded failed check(s): "
                      f"{'; '.join(c['criterion'][:100] for c in lost) or 'none'}")
        retried = (f"; {len(RETRIES)} scenario(s) repeated after a known emulator fault: " +
                   ", ".join(f"{r['scenario']} ({r['qemu']})" for r in RETRIES)) if RETRIES else ""
        print(f"\n{len(results) - len(failed)}/{len(results)} checks passed{retried}; artefacts "
              f"in {work}")
        sys.exit(1 if failed else 0)


MAIN_BROKER = {}   # issue #105 M6: the main broker (proc, conf), restarted for an attempt if dead


def ensure_main_broker():
    """Issue #105 M6: the main broker is shared by the scenarios (and 'main' replaces it during
    its MQTT-2 check, a replacement that run_scenario ends with the attempt): a new attempt
    starts with one running."""
    proc = MAIN_BROKER.get("proc")
    if proc is not None and proc.poll() is not None:
        MAIN_BROKER["proc"] = start_broker(MAIN_BROKER["conf"],
                                           WORK / f"mosquitto-restarted-{ATTEMPTS[1] + 1}.log")
        print("[CLEANUP] the main broker was not running: restarted for the next attempt",
              flush=True)


def snapshot_device():
    """Issue #105 round 2: a scenario that boots the image under test itself (main) changes it
    (NVS: counters, bdSeq, DO hold) and the eFuse file, and DCMDs change the Modbus slave. A
    retry must start from the same device as the first attempt."""
    if ARGS is None or not getattr(ARGS, "flash", None) or WORK is None:
        return
    for src, name in ((ARGS.flash, "retry-flash.bin"), (ARGS.efuse, "retry-efuse.bin")):
        if src and pathlib.Path(src).exists():
            shutil.copy(src, WORK / name)


def restore_device():
    if ARGS is None or not getattr(ARGS, "flash", None) or WORK is None:
        return
    for dst, name in ((ARGS.flash, "retry-flash.bin"), (ARGS.efuse, "retry-efuse.bin")):
        if dst and (WORK / name).exists():
            shutil.copy(WORK / name, dst)
    print("[CLEANUP] the retry starts from the device of the first attempt (image under test, "
          "eFuse file; the Modbus slave was put back after the attempt)", flush=True)


def retry_refusal(first, failed, unknown, attempt, retry_on_qemu_death):
    """Issue #96 round 2: None if the attempt may run once more, else why not. Only a known
    emulator fault, and only if no check of the attempt failed at all (checks are asserted at
    the end of their waits: one asserted before the fault may rest on evidence the fault did not
    touch, such as a rollback by timeout), no unknown panic was seen in the attempt, it is the
    first attempt and the run has repeated fewer than MAX_RETRIES scenarios."""
    if not first["retry"]:
        return (f"not repeated: {first['decision']}" if first["decision"]
                else "not a known emulator fault: not repeated")
    if not retry_on_qemu_death:
        return "this scenario is never repeated"
    if attempt > 1:
        return "second attempt after a known emulator fault"
    if failed:
        return (f"not repeated: {len(failed)} check(s) of the attempt had failed "
                f"({'; '.join(c['criterion'][:80] for c in failed)})")
    if unknown:
        return (f"not repeated: a firmware panic in the attempt matched no known emulator "
                f"fault ({unknown[0]})")
    if len(RETRIES) >= MAX_RETRIES:
        return f"not repeated: the run already repeated {len(RETRIES)} scenario(s) (at most {MAX_RETRIES})"
    return None


def run_scenario(name, fn, retry_on_qemu_death=True):
    """Issues #86, #90, #92, #96 and #105: run scenario @name under the fault monitor, within its
    time budget. From an emulator fault on, every wait raises AttemptAborted: the scenario ends
    there, and no check is asserted on evidence the fault cut short. The scenario runs once
    more (only once, and at most MAX_RETRIES scenarios a run) when its first fault is a known
    one (QEMU ended on its own; QEMU stalled after 'Take trusted digest key(s)', as its register
    dumps show; LoadStorePIFAddrError at a peripheral with a flash mapping in progress), no check
    of the attempt failed and no other firmware panic was seen in it (retry_refusal). The
    discarded attempt stays in results.json (retries[]). Anything else is a failed scenario with
    every check kept. An exception that ends the scenario is a failed check, not the end of
    the run. After each attempt the monitor is quiesced (MON_LOCK), its emulators are ended and
    whatever it left open is closed (#105)."""
    for attempt in (1, 2):
        ensure_main_broker()
        if attempt == 1:
            snapshot_device()
        else:
            restore_device()
        slave = MAIN_BROKER.get("slave")
        slave_snap = slave.snapshot() if slave is not None else None
        first_check, first_gw, first_proc = len(results), len(GATEWAYS), len(PROCS)
        first_capture = len(CAPTURE)
        budget = (SCENARIO_BUDGET_S.get(name, DEFAULT_BUDGET_S) if not ARGS.scenario_timeout
                  else ARGS.scenario_timeout) * ARGS.timeout_scale
        started = time.monotonic()
        with MON_LOCK:
            ATTEMPTS[1] += 1
            ATTEMPTS[0] = key = ATTEMPTS[1]
            FAULT.clear()
            ATTEMPT.clear()
            ATTEMPT.update(name=name, attempt=attempt, first_gw=first_gw, budget=budget,
                           deadline=started + budget, active=True)
        error = None
        try:
            fn()
        except Exception:
            traceback.print_exc()
            error = traceback.format_exc().splitlines()[-1]
        except AttemptAborted as e:
            error = f"stopped at its next wait: {e}"
            print(f"[FAULT] {name}: attempt stopped ({e})", flush=True)
        with MON_LOCK:   # issue #92 M4: no fault of this attempt is still being recorded
            ATTEMPT["active"] = False
        DURATIONS[f"{name} #{attempt}"] = round(time.monotonic() - started, 1)
        for g in GATEWAYS[first_gw:]:   # leave no emulator of this attempt running
            try:
                g.kill()
            except OSError:
                pass
        faults = [f for f in FAULTS if f["attempt_key"] == key]
        for g in GATEWAYS[first_gw:]:   # a death after the monitor's last look
            d = g.qemu_died()
            if d and not any(f["gateway_id"] == id(g) for f in faults):
                faults.append(record_fault(g, "qemu exit", time.monotonic(), signature=d,
                                           detail=d, key=key))
        # issue #105: no observer still subscribed, relay, fake broker, image server or private
        # broker (an exception skipped the scenario's own clean-up)
        left = close_attempt_resources(key)
        for proc in PROCS[first_proc:]:
            if proc.poll() is None:
                left.append("broker")
                proc.kill()
                proc.wait()
        ATTEMPTS[0] = 0
        FAULT.clear()
        if slave_snap is not None and (error is not None or faults):
            # issue #105 round 3: retried or not, the next attempt or scenario finds the slave
            # as this attempt found it
            slave.restore(slave_snap)
            left.append("Modbus slave values")
        if left:
            print(f"[CLEANUP] {name}: closed what the attempt left open: {', '.join(left)}",
                  flush=True)
        first = min(faults, key=lambda f: f["t"]) if faults else None
        failed = [c for c in results[first_check:] if not c["ok"]]
        unknown = [f"{g.log_path.name}: {p[1]}" for g in GATEWAYS[first_gw:]
                   for p in g.unknown_panics()]
        if unknown:   # visible even when nothing else went wrong
            print(f"[PANIC] {name}: firmware failure(s) on the console: {'; '.join(unknown[:5])}",
                  flush=True)
            # issue #256: in results.json too, as faults of this attempt (never repeated; no
            # state dump: its emulators have ended). After `first`: they change no retry decision
            for g in GATEWAYS[first_gw:]:
                for p in g.unknown_panics():
                    record_fault(g, "firmware failure", p[0], key=key,
                                 detail=f"{p[1]}: {g.lines[p[3]][1][:200]!r}")
        refusal =retry_refusal(first, failed, unknown, attempt, retry_on_qemu_death) if first \
            else None
        if first and refusal is None:
            discarded = results[first_check:]
            del results[first_check:]
            # issue #107: the attempt's observers are closed: its messages leave CAPTURE
            DISCARDED_CAPTURE.extend(CAPTURE[first_capture:])
            del CAPTURE[first_capture:]
            RETRIES.append({"scenario": name, "qemu": first["signature"], "fault": first,
                            "error": error, "discarded_checks": discarded})
            print(f"[RETRY] {name}: {first['signature']}"
                  f"{'; ' + first['decision'] if first['decision'] else ''}; the scenario runs "
                  f"once more ({len(discarded)} passed check(s) of the first attempt discarded)",
                  flush=True)
            continue
        if error is not None or first:
            why = [error] if error else []
            if first:
                why.append(f"{first['kind']}: {first['signature'] or first['detail']}"
                           + (f" (state in {first['dump']})" if first["dump"] else ""))
                why.append(refusal)
                first["refused"] = refusal
            check(f"{name} scenario completed", False, "; ".join(why))
        return


def seq_continuity(msgs):
    errors, expect = [], None
    for _, topic, p, _ in msgs:
        kind = topic.split("/")[2]
        if not topic.startswith(T) or kind in ("NDEATH", "NCMD", "DCMD", "STATE"):
            continue
        if kind == "NBIRTH":
            expect = 0
        if expect is not None and p.seq != expect:
            errors.append(f"{kind} seq {p.seq} expected {expect}")
        expect = (p.seq + 1) % 256
    return errors


def commissioning_password(gw, since, timeout=20):
    """The password the gateway prints on its console while no web password is set."""
    t = gw.wait_log(r"commissioning password '[a-z0-9]+'", timeout, since)
    lines = gw.find(r"commissioning password '[a-z0-9]+'", since) if t else []
    return re.search(r"commissioning password '([a-z0-9]+)'", lines[0]).group(1) if lines else None


def run_no_psram(args, work, slave):
    """Issue #135: the firmware assumes the board's PSRAM. The PSRAM build (sdkconfig.qemu-psram:
    CONFIG_SPIRAM with SPIRAM_IGNORE_NOTFOUND, as on the board) boots on an emulator without the
    octal PSRAM model, as a board with a defective or missing chip: the boot does not find the
    PSRAM and goes on in the fault mode. Expected: the log, the shell and GET /api/status give the
    fault; the network and the HTTPS server run (from internal RAM, with the commissioning
    password of the console); nothing of the gateway starts (no configuration loaded, no polling,
    no MQTT); POST /api/rebirth gets 409 with the reason; no panic."""
    if not args.psram:
        SKIPPED.append("issue #135 no PSRAM")
        print("[SKIP] issue #135 no PSRAM: it boots the PSRAM build (E2E_PSRAM=1)", flush=True)
        return
    polls = len(slave.reads)
    t0 = time.monotonic()
    gw = Gateway(args, work / "uart0-nopsram.log",
                 flash_variant(args, work, "nopsram", {"config.json": config_json()}), psram=False)
    gw.start()
    fault = gw.wait_log(r"main: PSRAM not detected: the board has 8 MB of PSRAM and the boot did "
                        r"not find it \(a defective or missing chip\); the gateway does not start "
                        r"\(issue #135\)", 40, t0)
    halted = gw.wait_log(r"nothing is started \(no polling, no MQTT\): only the network, the web "
                         r"UI on port \d+ and the shell, to report the fault", 15, t0)
    led = gw.find(r"status: config error", t0)
    pw = commissioning_password(gw, t0, 20)
    auth = ("admin", pw or "?")
    st, _, body = wait_http("/api/status", 40, auth=auth)
    status = as_json(body) or {}
    st_rb, _, b_rb = http("POST", "/api/rebirth", auth=auth)
    e_rb = (as_json(b_rb) or {}).get("error", "")
    page = http("GET", "/", auth=auth)
    asked, h = gw.shell("status", [r"^FAULT\s+PSRAM not detected: .* the gateway does not start",
                                   r"nothing is running: no polling, no MQTT; network, web UI and "
                                   r"shell only", r"^heap\s+internal \d+ B free .*PSRAM 0 B free"])
    shell = gw.find(r"^(FAULT|heap)\s", asked)
    time.sleep(3)   # the polling and an MQTT attempt would have started by now
    started = gw.find(r"configuration from|connecting to|rs485: UART|Sparkplug engine|"
                      r"store & forward:", t0)
    births = [m for m in CAPTURE if m[0] >= t0 and "/NBIRTH/" in m[1]]
    polled = len(slave.reads) - polls
    bad = gw.find(r"Guru Meditation|abort\(\) was called|task_wdt|Task watchdog|CORRUPT HEAP|"
                  r"stack overflow", t0)
    gw.kill()
    check("Issue #135 a board without PSRAM (the PSRAM build on an emulator without the octal "
          "PSRAM): the fault mode. The log, the shell and GET /api/status (mode fault, the reason) "
          "give the fault; the network and the HTTPS server run; POST /api/rebirth -> 409; no "
          "configuration, polling or MQTT; no panic",
          fault and halted and led and pw and st == 200 and status.get("mode") == "fault"
          and "PSRAM not detected" in status.get("fault", "")
          and status.get("config", {}).get("ok") is False
          and st_rb == 409 and "hardware fault" in e_rb
          and page[0] == 200 and b's.mode==="fault"' in page[2]
          and all(h.values()) and not started and not births and polled == 0 and not bad,
          f"log {'yes' if fault else 'no'}, halted {'yes' if halted else 'no'}, LED "
          f"{'yes' if led else 'no'}; password {'read' if pw else 'not found'}; /api/status {st} "
          f"mode {status.get('mode')!r} fault {status.get('fault')!r}; rebirth {st_rb} {e_rb!r}; "
          f"/ {page[0]}; shell {' | '.join(shell) or 'no FAULT line'}; started {started[:2]}; "
          f"{len(births)} NBIRTH; {polled} Modbus requests; {bad[:2]}")


def run_config_failsafe(args, work, slave):
    """CFG-1: the embedded default is used only when /cfg/config.json does not exist; any
    other problem stops the gateway (no polling, no MQTT) with the error state and a log.
    CFG-3 recovery: the network still comes up (DHCP, no valid configuration was ever seen on
    these images) with the web UI, so the configuration can be fixed without reflashing."""
    good = config_json()
    limit = sdkconfig_int(args.sdkconfig, "GW_CONFIG_MAX_BYTES")
    cases = [
        ("oversized", {"config.json": good + b" " * (limit + 1 - len(good))},
         rf"config\.json: {limit + 1} bytes; the maximum is {limit}"),
        ("unmountable", None, r"LittleFS partition 'storage' not mountable"),
        ("empty", {"config.json": b""}, r"config\.json: empty file"),
        # without the ',' that closes "network" (line 4), the parser stops at the '"' of
        # "ntp": line 5, column 3
        ("syntax-error", {"config.json": good.replace(b'"dhcp": true\n  },', b'"dhcp": true\n  }', 1)},
         r"JSON syntax error at line 5, column 3"),
        ("semantic-error", {"config.json": good.replace(b'"type": "float32"', b'"type": "float16"', 1)},
         r"devices\[0\]\.tags\[0\]\.type: invalid value"),
    ]
    obs = Observer()
    for name, files, reason in cases:
        gw = Gateway(args, work / f"uart0-{name}.log", flash_variant(args, work, name, files))
        obs.drain()
        polls = len(slave.reads)
        t0 = time.monotonic()
        gw.start()
        hit = gw.wait_log(r"configuration rejected .*" + reason, 20, t0)
        halted = gw.wait_log(r"nothing is started", 2, t0)
        led = gw.wait_log(r"status: config error", 2, t0)
        dos_off = gw.find(r"io: DO .*all outputs off", t0)
        time.sleep(6)  # long enough for DHCP + SNTP wait + a connection attempt
        # Positive signal: 6 s later the shell still reports the halted state.
        asked, h = gw.shell("status", [r"nothing is running: no polling, no MQTT; recovery network and "
                                       r"web UI only", r"config\s+REJECTED .*" + reason])
        still = list(h.values())[0]
        shell = gw.find(r"config\s+REJECTED .*" + reason, asked)
        # Supplementary negative signals: no component logged its start, no NBIRTH reached MQTT,
        # no Modbus request reached the slave.
        started = gw.find(r"configuration from|connecting to|rs485: UART", t0)
        births = [m for m in CAPTURE if m[0] >= t0 and "/NBIRTH/" in m[1]]
        polled = len(slave.reads) - polls
        # CFG-3 recovery: web UI and status over the recovery network, with the commissioning
        # password (nothing stored on these images)
        pw = commissioning_password(gw, t0, 1)
        st, _, body = wait_http("/api/status", 20, auth=("admin", pw or "?"))
        status = as_json(body) or {}
        page = http("GET", "/", auth=("admin", pw or "?"))
        check(f"CFG-3 {name} config file -> web UI and /api/status answer in recovery mode with the "
              f"reason (commissioning password from the console)",
              pw and st == 200 and status.get("mode") == "recovery"
              and re.search(reason, status.get("config", {}).get("error", ""))
              and page[0] == 200 and b"Gateway RS485" in page[2]
              and gw.find(r"net: IPv4", t0) and not status.get("devices"),
              f"password {'read' if pw else 'not found'}; /api/status {st} mode "
              f"{status.get('mode')!r} error {status.get('config', {}).get('error')!r}; / {page[0]}")
        if name == "semantic-error":
            # issues #182 and #171: no bus and no Sparkplug session in recovery: a clear 409
            st_mb, _, b_mb = http("POST", "/api/modbus/read", {"unit": 1, "area": "holding", "address": 10},
                                  auth=("admin", pw or "?"))
            st_rb, _, b_rb = http("POST", "/api/rebirth", auth=("admin", pw or "?"))
            e_mb, e_rb = (as_json(b_mb) or {}).get("error", ""), (as_json(b_rb) or {}).get("error", "")
            check("CFG-3 issues #182 and #171 recovery: POST /api/modbus/read and POST /api/rebirth "
                  "-> 409 with the reason (no bus, no Sparkplug session)",
                  st_mb == 409 and "recovery mode" in e_mb and st_rb == 409 and "no Sparkplug session" in e_rb,
                  f"modbus/read {st_mb} {e_mb!r}; rebirth {st_rb} {e_rb!r}")
            # the rejected document can be exported, fixed and saved: the gateway then starts
            st2, _, b2 = http("GET", "/api/config", auth=("admin", pw or "?"))
            fixed = as_json(b2) or {}
            path = (status.get("config") or {}).get("path")
            ok_export = st2 == 200 and fixed.get("devices", [{}])[0].get("tags", [{}])[0].get("type") == "float16"
            if ok_export:
                fixed["devices"][0]["tags"][0]["type"] = "float32"
                fixed["sparkplug"]["edge_node_id"] = "GW03"
            st3, _, b3 = http("PUT", "/api/config", fixed, auth=("admin", pw or "?"))
            nb = obs.wait(f"{T}/NBIRTH/GW03", 40)
            check("CFG-3/CFG-4 recovery: the rejected document is exported (path in /api/status), fixed "
                  "and saved through the REST API; the gateway restarts and runs it",
                  ok_export and path == "devices[0].tags[0].type" and st3 == 200 and nb,
                  f"GET {st2} (path {path!r}); PUT {st3} {b3.decode().strip()}; NBIRTH GW03: {bool(nb)}")
        gw.kill()
        if name == "semantic-error":
            obs.wait(f"{T}/NDEATH/GW03", 5)
        if name == "oversized":
            check("CFG-6 console shell answers 'status' while the configuration is rejected",
                  shell, shell[0] if shell else "no answer")
        check(f"IO-2 {name} config file -> DOs switched off before the configuration is loaded",
              bool(dos_off), (dos_off or ["no 'all outputs off' log"])[0])
        check(f"CFG-1 {name} config file -> halted (shell 'status' after 6 s), error LED, ERROR log "
              f"with the reason, nothing started",
              hit and halted and still and shell and led and not started and not births
              and polled == 0,
              (gw.find(r"configuration rejected", t0) or ["no 'configuration rejected' log"])[0] +
              f"; halted={bool(halted)}; shell still halted={bool(still)}; LED error={bool(led)}; "
              f"started={started[:1]}; NBIRTHs={len(births)}; Modbus requests={polled}")

    gw = Gateway(args, work / "uart0-absent.log", flash_variant(args, work, "absent", {}))
    t0 = time.monotonic()
    gw.start()
    hit = gw.wait_log(r"configuration from embedded default", 20, t0)
    gw.kill()
    check("CFG-2 missing config file -> embedded default", hit,
          (gw.find(r"configuration from", t0) or ["no 'configuration from' log"])[0])
    obs.close()


def check_offline_backoff(slave, t0):
    """Issues #4 and #61: dead slaves (units 3..5, never answered since boot) are offline from the
    start: one attempt per probe from their first cycle on, at 2, 4, 8 ... x poll_ms up to the
    cap, and they never delay the healthy device's 1 s polling, not even right after boot."""
    def bursts():
        """Requests to dead unit 3 grouped by proximity: a full cycle would be a burst of
        attempts 200 ms apart, a probe is alone."""
        ts = [t for t, r in zip(list(slave.times), list(slave.reads), strict=False) if r[0] == 3 and t >= t0]
        out = []
        for t in ts:
            if out and t - out[-1][-1] <= 0.5:
                out[-1].append(t)
            else:
                out.append([t])
        return out

    end = t0 + 60
    while wait_left(end) > 0 and len(bursts()) < 5:
        time.sleep(0.5)
    reads = list(zip(slave.times, slave.reads, strict=False))
    cl = bursts()
    single = bool(cl) and all(len(c) == 1 for c in cl)
    gaps = [round(b[0] - a[0], 1) for a, b in zip(cl, cl[1:], strict=False)]
    full = [min(2.0 * 2 ** i, PROBE_MAX_S) for i in range(len(gaps) + 1)]
    # The very first request may be lost while QEMU's UART socket connects: then the series
    # starts one step later. A due probe may also wait for another device's cycle (~0.6 s).
    def fits(k):
        return all(-0.2 <= g - e <= 0.7 for g, e in zip(gaps, full[k:], strict=False))
    offset = next((k for k in (0, 1) if fits(k)), None)
    probes_ok = len(gaps) >= 4 and single and offset is not None
    check("MB-9 / issues #4 #61 never-answered device probed from boot with a single attempt and an "
          "exponential backoff (2, 4, 8 ... s, cap), no full cycles", probes_ok,
          f"unit 3: {[len(c) for c in cl]} requests per burst; probes after {gaps} s (expected "
          f"{full[offset or 0:][:len(gaps)]}, offset {offset}); single attempt each: {single}")
    # Healthy Meter1 from its very first request: cycle starts = its first request kind
    meter = [(t, r) for t, r in reads if r[0] == 1 and t >= t0]
    kind = meter[0][1][1:] if meter else None
    starts = [t for t, r in meter if r[1:] == kind and t <= meter[0][0] + 20]
    span = starts[-1] - starts[0] if len(starts) > 1 else 0
    worst = max((b - a for a, b in zip(starts, starts[1:], strict=False)), default=0)
    # 1 s period; at worst one probe of each dead unit (200 ms timeout) in between + jitter
    ok = len(starts) >= 8 and span and (len(starts) - 1) / span >= 0.9 and worst <= 1.0 + 3 * 0.2 + 0.4
    return check("Issues #4 #61 healthy device keeps its 1 s period next to 3 dead ones, from boot",
                 ok, f"{len(starts)} Meter1 cycles in the first {span:.1f} s, longest interval "
                 f"{worst:.2f} s")


def mbap_raw(pdu, unit=1, tid=0x4D42, timeout=5):
    """Send one MBAP frame to the gateway (through QEMU's port forward); the answer's PDU, b"" if
    the gateway closed the connection without answering, None on timeout."""
    with socket.create_connection(("127.0.0.1", MBTCP_PORT), timeout=timeout) as c:
        c.sendall(struct.pack(">HHHB", tid, 0, len(pdu) + 1, unit) + pdu)
        data = b""
        try:
            while len(data) < 7 or len(data) < 6 + struct.unpack(">H", data[4:6])[0]:
                chunk = c.recv(300)
                if not chunk:
                    return b""
                data += chunk
        except (socket.timeout, ConnectionResetError):
            return b"" if not data else None
        return data[7:]


def check_modbus_tcp(gw, slave):
    """MB-15: a Modbus TCP client (pymodbus) reads and writes an RTU slave through the gateway;
    a unit that does not answer gives exception 0B; a function the gateway does not forward
    gets 01 from the gateway itself, without reaching the RS485 bus."""
    since = time.monotonic()
    client = ModbusTcpClient("127.0.0.1", port=MBTCP_PORT, timeout=4, retries=0)
    connected = client.connect()
    rr = client.read_holding_registers(10, count=1, slave=1) if connected else None
    read = None if rr is None or rr.isError() else list(rr.registers)
    t_w = time.monotonic()
    wr = client.write_register(200, 4242, slave=1) if connected else None
    w_lat = time.monotonic() - t_w
    written = slave.get(3, 200)[0]
    dead = client.read_holding_registers(0, count=1, slave=3) if connected else None
    dead_code = getattr(dead, "exception_code", None) if dead is not None and dead.isError() else None
    client.close()
    n0 = len(slave.reads)
    unknown = mbap_raw(b"\x2b\x0e\x01\x00")
    reached = [r for r in list(slave.reads)[n0:] if r[1] == 0x2B]
    seen = [r for t, r in zip(list(slave.times), list(slave.reads), strict=False) if t >= since and r[0] == 1
            and r[1:3] in ((3, 10), (6, 200))]
    log = gw.find(r"mbtcp: client 10\.0\.2\.2 connected", 0)
    ok = connected and read == [slave.get(3, 10)[0]] and wr is not None and not wr.isError() \
        and written == 4242 and dead_code == 0x0B and unknown == b"\xab\x01" and not reached \
        and len(seen) >= 2 and log
    return check("MB-15 Modbus TCP gateway: pymodbus client reads holding 10 and writes holding 200 "
                 "of unit 1 over TCP 502; dead unit -> exception 0B; FC 0x2B -> 01 from the gateway",
                 ok, f"connected {connected}; read {read} (slave {slave.get(3, 10)[0]}); write "
                 f"{'ok' if wr is not None and not wr.isError() else wr} in {w_lat:.2f} s, slave "
                 f"holding 200 = {written}; unit 3 -> exception {dead_code}; FC 0x2B -> "
                 f"{unknown!r}, reached the bus: {bool(reached)}; RTU requests {seen}; "
                 f"{(log or ['no connect log'])[0]}")


def check_panic_during_traffic(gw, obs, slave, nbirth_t):
    """Issue #65: QEMU keeps UART1's interrupt line across a restart; a Modbus answer arriving
    while the gateway restarts used to hang the next boot in uart_driver_install(). Unit 1
    answers 120 ms late, and the panic is sent right after one of its requests reached the
    slave, so the answer lands during the panic handler. Every boot must come back (NBIRTH with
    reset reason "panic"); a panic restart does not quiesce the bus. Issue #144: three crash
    resets in a row start safe mode, so a restart by the operator (shell 'restart', which clears
    the boot-loop counter) comes before the third panic."""
    outcomes = []
    slave.delay = 0.12
    try:
        for i in range(3):
            if i == 2:
                obs.drain()
                gw.console("restart")
                if obs.wait(nbirth_t, 60) is None:
                    outcomes.append("no NBIRTH after the operator restart")
                    break
            n0 = len(slave.reads)
            end = time.monotonic() + 5
            while time.monotonic() < end and not any(r[0] == 1 for r in list(slave.reads)[n0:]):
                time.sleep(0.002)
            obs.drain()
            gw.console("panic")
            nb = obs.wait(nbirth_t, 40)
            reason = metric(nb[2], name="Node Info/Reset Reason") if nb else None
            outcomes.append(reason.string_value if reason else None)
            if nb is None:
                break
    finally:
        slave.delay = 0.0
    return check("Issue #65 panic while a Modbus answer is on its way: the next boot does not hang "
                 "(3 times, NBIRTH with reset reason 'panic')", outcomes == ["panic"] * 3,
                 f"reset reasons after each panic: {outcomes}")


def esp_coredump_info(args, core, fmt, out):
    """esp_coredump info_corefile of @core (--core-format @fmt) against the ELF; the text goes
    to @out. True when it decodes with the backtrace through panic_task and abort."""
    info = subprocess.run([args.idf_python, "-m", "esp_coredump", "--chip", "esp32s3",
                           "info_corefile", "--core", str(core), "--core-format", fmt, args.elf],
                          capture_output=True, text=True, timeout=120)
    out.write_text(info.stdout + info.stderr)
    return info.returncode == 0 and "panic_task" in info.stdout and "abort" in info.stdout


def check_core_dump_api(gw, obs, args, work, nb, since, ndata_t, off):
    """Issue #132: the dump of the last panic, read by the running firmware. The NBIRTH has
    'Node Info/Core Dump' true, /api/status has core_dump, the summary route gives the task, PC,
    backtrace and reason, GET /api/coredump gives the bytes of the partition (esp_coredump
    decodes them), and the shell gives the summary and the dump in base64 lines (decoded with
    --core-format b64, as the how-to does). DELETE without the CSRF header is refused; with it
    the dump is erased: NDATA Core Dump false (alias only), 404, and no dump in the status."""
    pw = commissioning_password(gw, since, 10)
    auth = ("admin", pw or "?")
    flag = metric(nb[2], name="Node Info/Core Dump") if nb else None
    born = flag is not None and flag.alias == 57 and flag.datatype == 11 and flag.boolean_value
    st, _, body = http("GET", "/api/status", auth=auth)
    cd = (as_json(body) or {}).get("core_dump", {})
    s_st, _, s_body = http("GET", "/api/coredump/summary", auth=auth)
    summ = as_json(s_body) or {}
    d_st, d_h, dump = http("GET", "/api/coredump", auth=auth, timeout=60)
    dump = dump if d_st == 200 else b""
    flash = pathlib.Path(args.flash).read_bytes()[off:off + len(dump)]
    core = work / "coredump-api.bin"
    core.write_bytes(dump)
    decoded = bool(dump) and esp_coredump_info(args, core, "raw", work / "coredump-api-info.txt")
    status_ok = st == 200 and cd.get("present") is True and cd.get("state") == "valid" \
        and cd.get("size") == len(dump)
    summary_ok = s_st == 200 and summ.get("task") == "panic" and summ.get("reset_reason") == "panic" \
        and re.fullmatch(r"0x[0-9a-f]{8}", summ.get("pc", "")) is not None \
        and len(summ.get("backtrace", [])) >= 2 and "abort()" in summ.get("reason", "") \
        and summ.get("size") == len(dump)
    download_ok = d_st == 200 and len(dump) > 1000 and dump == flash \
        and d_h.get("content-type") == "application/octet-stream" \
        and "attachment" in d_h.get("content-disposition", "")
    check("Issue #132 the core dump through the running firmware: NBIRTH 'Node Info/Core Dump' "
          "true (Boolean, alias 57), /api/status core_dump, GET /api/coredump/summary (task, PC, "
          "backtrace, reason), GET /api/coredump = the partition's bytes, decoded by esp_coredump",
          born and status_ok and summary_ok and download_ok and decoded,
          f"NBIRTH Core Dump {flag.boolean_value if flag else None} (alias "
          f"{flag.alias if flag else None}); status HTTP {st} {cd}; summary HTTP {s_st} "
          f"task {summ.get('task')!r} pc {summ.get('pc')} bt {len(summ.get('backtrace', []))} "
          f"reason {summ.get('reason', '')[:50]!r}; download HTTP {d_st} {len(dump)} B, same as "
          f"the flash {dump == flash and bool(dump)}, decoded {decoded}")

    # the shell: the summary, then the whole dump in base64 lines between two markers
    # (its reason line quotes the dump's abort(): not a new failure, DUMP_QUOTE_RX)
    _, hits = gw.shell("coredump", [r"^coredump  \d+ bytes in flash, checksum correct",
                                    r"^task\s+panic$", r"^backtrace( 0x[0-9a-f]{8}){2,}",
                                    r"^reason {4}abort\(\) was called at PC 0x"])
    t_dump = time.monotonic()
    gw.console("coredump dump")
    end = gw.wait_log(r"^-----END CORE DUMP-----$", 60, t_dump)
    lines = [ln for t, ln in list(gw.lines) if t >= t_dump]
    begin = next((i for i, ln in enumerate(lines) if ln.endswith("-----BEGIN CORE DUMP-----")), None)
    stop = next((i for i, ln in enumerate(lines) if ln == "-----END CORE DUMP-----"), None)
    b64 = [ln for ln in lines[begin + 1:stop] if re.fullmatch(r"[A-Za-z0-9+/=]+", ln)] \
        if begin is not None and stop is not None else []
    b64_file = work / "coredump-console.b64"
    b64_file.write_text("".join(f"{ln}\n" for ln in b64))
    try:
        console = base64.b64decode("".join(b64), validate=True)
    except ValueError:
        console = b""
    console_decoded = bool(b64) and esp_coredump_info(args, b64_file, "b64",
                                                      work / "coredump-console-info.txt")
    check("Issue #132 shell 'coredump' (summary: size, task, backtrace, reason) and 'coredump "
          "dump' (the dump in base64 lines between two markers: the same bytes as the download, "
          "decoded by esp_coredump --core-format b64)",
          all(hits.values()) and end and console == dump and bool(dump) and console_decoded,
          f"summary lines missing: {[p for p, t in hits.items() if t is None] or 'none'}; "
          f"dump end marker {'yes' if end else 'no'}, {len(b64)} lines, {len(console)} B, same "
          f"as the download {console == dump}, decoded {console_decoded}")

    # erase: the CSRF check first, then the dump goes and the metric follows
    f_st, _, _ = http("DELETE", "/api/coredump", auth=auth, headers={"X-Requested-With": None})
    kept = ((as_json(http("GET", "/api/status", auth=auth)[2]) or {}).get("core_dump") or {}) \
        .get("present")
    obs.drain()
    e_st, _, e_body = http("DELETE", "/api/coredump", auth=auth)
    nd = obs.wait(ndata_t, 15, lambda q: metric(q, alias=57) is not None)
    nd_m = metric(nd[2], alias=57) if nd else None
    g_st, _, _ = http("GET", "/api/coredump", auth=auth)
    a_st, _, a_body = http("GET", "/api/status", auth=auth)
    gone = (as_json(a_body) or {}).get("core_dump", {})
    summ2 = as_json(http("GET", "/api/coredump/summary", auth=auth)[2]) or {}
    again, _, _ = http("DELETE", "/api/coredump", auth=auth)
    _, none = gw.shell("coredump", [r"^coredump  none in flash$"])
    erased = gw.find(r"coredump: core dump erased through the REST API", t_dump)
    check("Issue #132 DELETE /api/coredump: refused without the CSRF header (403, the dump "
          "stays), then erased (200, NDATA 'Node Info/Core Dump' false by alias only); afterwards "
          "GET 404, /api/status and the summary show no dump, a second DELETE 404, shell 'none'",
          f_st == 403 and kept is True and e_st == 200 and (as_json(e_body) or {}).get("erased")
          and nd_m is not None and not nd_m.name and nd_m.boolean_value is False and g_st == 404
          and a_st == 200 and gone.get("present") is False and summ2.get("present") is False
          and again == 404 and all(none.values()) and erased,
          f"without header HTTP {f_st} (dump kept {kept}); DELETE HTTP {e_st} {e_body[:60]!r}; "
          f"NDATA {nd_m.boolean_value if nd_m else None} name {nd_m.name if nd_m else None!r}; "
          f"GET after {g_st}; status {gone}; summary present {summ2.get('present')}; second "
          f"DELETE {again}; shell {'none' if all(none.values()) else 'no answer'}; "
          f"log {(erased or ['no erase log'])[0]}")


def check_scan_rate(obs, gw, slave, nbirth, ncmd_t, ndata_t):
    """SPB-15: writable Node Control/Scan Rate (UInt64 ms, 0 = each device at its poll_ms).
    3000 makes Meter1 poll every 3 s, an invalid value is refused (NDATA keeps 3000), 0 brings
    back its 1 s."""
    name = "Node Control/Scan Rate"
    birth = metric(nbirth, name=name)
    def set_rate(value, datatype=8):
        obs.drain()
        obs.publish_cmd(ncmd_t, [{"name": name, "datatype": datatype, "long_value": value}])
        nd = obs.wait(ndata_t, 3, lambda q: metric(q, alias=7) is not None)
        return metric(nd[2], alias=7).long_value if nd else None
    def starts(since, until):
        return [t for t, r in zip(list(slave.times), list(slave.reads), strict=False)
                if since <= t <= until and r[0] == 1 and r[1:] == (1, 0, 1)]
    t_set = time.monotonic()
    confirmed = set_rate(3000)
    time.sleep(11)
    slow = starts(t_set + 1.5, time.monotonic())
    gaps3 = [round(b - a, 2) for a, b in zip(slow, slow[1:], strict=False)]
    since = time.monotonic()
    refused = set_rate(10)
    logged = gw.wait_log(r"NCMD: metric 'Node Control/Scan Rate' .*discarded: Scan Rate must be", 2, since)
    t_back = time.monotonic()
    back = set_rate(0)
    time.sleep(5)
    fast = starts(t_back + 1.5, time.monotonic())
    gaps1 = [round(b - a, 2) for a, b in zip(fast, fast[1:], strict=False)]
    ok = birth is not None and birth.datatype == 8 and birth.long_value == 0 and confirmed == 3000 \
        and len(gaps3) >= 2 and all(2.7 <= g <= 3.6 for g in gaps3) and refused == 3000 and logged \
        and back == 0 and len(gaps1) >= 2 and all(g <= 1.6 for g in gaps1)
    return check("SPB-15 NCMD Node Control/Scan Rate: NBIRTH UInt64 0, 3000 -> Meter1 every 3 s "
                 "(NDATA), invalid 10 refused, 0 -> back to its poll_ms", ok,
                 f"NBIRTH {(birth.datatype, birth.long_value) if birth else None}; NDATA {confirmed}, "
                 f"cycles {gaps3} s apart; 10 -> NDATA {refused}, logged {bool(logged)}; 0 -> NDATA "
                 f"{back}, cycles {gaps1} s apart")


def check_write_between_blocks(obs, slave, dcmd_t, ddata_t):
    """MB-14: with unit 1 answering 120 ms late, a Meter1 cycle (6 requests) takes about 1 s.
    A DCMD sent in the middle of a cycle is written between two requests of that cycle (Meter1
    reads before it, after the cycle start, and after it, before the next start), not once the
    cycle is over, and its latency is about one transaction."""
    start = (1, 0, 1)   # first request of every Meter1 cycle (coil 0)
    slave.delay = 0.12
    tries = []
    try:
        for attempt in range(3):
            n0 = len(slave.reads)
            end = time.monotonic() + 4
            while time.monotonic() < end and not any(
                    r[0] == 1 and r[1:] == start for r in list(slave.reads)[n0:]):
                time.sleep(0.01)
            time.sleep(0.25)   # well into the cycle
            value = 2 ** 62 + 100 + attempt
            obs.drain()
            sent = time.monotonic()
            obs.publish_cmd(dcmd_t, [{"name": "Counter", "datatype": 4, "long_value": value}])
            dd = obs.wait(ddata_t, 6, lambda q: metric(q, alias=ALIAS["Counter"]) is not None)
            time.sleep(1.5)    # let the cycle finish
            reads = [(t, r) for t, r in zip(list(slave.times), list(slave.reads), strict=False) if r[0] == 1]
            w = next((i for i, (t, r) in enumerate(reads) if t >= sent and r[1:3] == (16, 34)), None)
            if w is None:
                tries.append(f"attempt {attempt + 1}: no write seen")
                continue
            before = 0
            for _t, r in reversed(reads[:w]):
                if r[1:] == start:
                    break
                before += 1
            after = 0
            for _t, r in reads[w + 2:]:      # w + 1 is the read-back
                if r[1:] == start:
                    break
                after += 1
            latency = reads[w][0] - sent
            exact = dd is not None and int64_of(metric(dd[2], alias=ALIAS["Counter"])) == value
            tries.append((before, after, round(latency, 2), exact))
            if after >= 1 and exact and latency <= 0.6:
                break
    finally:
        slave.delay = 0.0
    ok = any(isinstance(x, tuple) and x[1] >= 1 and x[3] and x[2] <= 0.6 for x in tries)
    return check("MB-14 DCMD written between two requests of a running cycle (latency ~ one "
                 "transaction, not the rest of the cycle)", ok,
                 "(Meter1 requests of the cycle before the write, after it, latency s, DDATA exact): "
                 f"{tries}")


def check_scan_classes(slave):
    """MB-12: Meter1's slow-scan blocks (Energy at holding 30, Serial at 50; slow_poll_ms 3000)
    are read every third cycle, its fast ones (cycle start: coil 0) every second."""
    since = time.monotonic() - 12
    reads = [(t, r[1:]) for t, r in zip(list(slave.times), list(slave.reads), strict=False) if r[0] == 1 and t >= since]
    fast = [t for t, k in reads if k == (1, 0, 1)]
    slow = [t for t, k in reads if k == (3, 30, 4)]
    # a DCMD to Serial (check_mb6_writes) is read back right after its FC16: that request is not
    # a scan read (only the one that immediately follows each write)
    serial = [t for i, (t, k) in enumerate(reads)
              if k == (3, 50, 8) and not (i > 0 and reads[i - 1][1][:2] == (16, 50))]
    gaps = [round(b - a, 2) for a, b in zip(slow, slow[1:], strict=False)]
    fgaps = [b - a for a, b in zip(fast, fast[1:], strict=False)]
    ok = len(slow) >= 3 and len(serial) == len(slow) and all(2.8 <= g <= 3.6 for g in gaps) \
        and len(fast) >= 10 and max(fgaps) <= 1.6
    return check("MB-12 slow-scan tags read every slow_poll_ms (3 s), fast ones every poll_ms (1 s)",
                 ok, f"last 12 s: {len(fast)} fast cycles (longest gap {max(fgaps, default=0):.2f} s), "
                 f"{len(slow)} slow reads {gaps} s apart, Serial read {len(serial)} times")


def props(m):
    """SPB-16: {key: value} of a metric's PropertySet."""
    return {k: spb_decode.value_of(v) for k, v in spb_decode.props_of(m).items()}


def check_properties_and_quality(obs, slave, db, ddata_t):
    """SPB-16: the DBIRTH carries engUnit/engLow/engHigh from the configuration and Quality GOOD;
    a read answered with an exception is null with Quality BAD, and the next good read brings
    the value back with Quality GOOD."""
    v = metric(db[2], name="Voltage") if db else None
    birth = props(v) if v is not None else {}
    others = [props(m).get("Quality") for m in db[2].metrics] if db else []
    check("SPB-16 DBIRTH properties: Voltage engUnit 'V', engLow 0, engHigh 400 and Quality 192 "
          "(GOOD) on every metric", birth == {"engUnit": "V", "engLow": 0.0, "engHigh": 400.0,
                                              "Quality": 192} and others and
          all(q == 192 for q in others), f"Voltage {birth}; Quality of all: {sorted(set(others))}")
    obs.drain()
    slave.ctx.refuse.add((4, 0))          # Voltage alone in its block (input 0-1): exception 02
    bad = obs.wait(ddata_t, 5, lambda p: any(m.alias == ALIAS["Voltage"] for m in p.metrics))
    slave.ctx.refuse.clear()
    good = obs.wait(ddata_t, 5, lambda p: any(m.alias == ALIAS["Voltage"] for m in p.metrics))
    bm = metric(bad[2], alias=ALIAS["Voltage"]) if bad else None
    gm = metric(good[2], alias=ALIAS["Voltage"]) if good else None
    check("SPB-16 failed read (exception 02) -> DDATA Voltage null with Quality 0 (BAD); next read "
          "-> value with Quality 192 (GOOD), the device stays online",
          bm is not None and bm.is_null and props(bm) == {"Quality": 0} and gm is not None
          and not gm.is_null and abs(gm.float_value - 231.0) < 1e-3 and props(gm) == {"Quality": 192},
          f"failed: {'null' if bm is not None and bm.is_null else bm} {props(bm) if bm else ''}; "
          f"after: {round(gm.float_value, 3) if gm else None} {props(gm) if gm else ''}")


def run_hours_of(p, ch):
    """#217: the value of Run Hours/<ch> ("DI/4", "DO/2"; UInt64 s) in an NBIRTH (by name) or an
    NDATA (by alias 356 + channel, DI/1..8 then DO/1..8), or None."""
    k = (0 if ch.startswith("DI") else 8) + int(ch[3:]) - 1
    m = metric(p, name=f"Run Hours/{ch}") or metric(p, alias=356 + k)
    return m.long_value if m is not None else None


def check_local_io_channels(gw, obs, nb, ncmd_t, ndata_t, t0):
    """#211 invert and debounce of each DI, #218 a fast counter (the simulated PCNT), #210 a pulse
    DO, #217 run hours and #204 the DO commands of the web UI, with the local_io keys of
    tools/e2e/fs/config.json: DI/5 inverted with 200 ms, DI/8 a fast counter, DO/7 a pulse of
    300 ms, run hours of DI/4 and DO/2, rules on DO/5 (follow DI/2) and DO/6."""
    # #211: DI/5 is inverted: true at rest; the console's level 1 makes it false after 200 ms
    rest = metric(nb[2], name="DI/5")
    obs.drain()
    sent = time.monotonic()
    gw.console("di 5 1")
    nd = obs.wait(ndata_t, 3, lambda q: metric(q, alias=14) is not None)
    lat = (nd[0] - sent) * 1000 if nd else None
    obs.drain()
    gw.console("di 5 0")
    nd2 = obs.wait(ndata_t, 3, lambda q: counter_of(q, 5) is not None)
    check("#211 invert and debounce for each DI: DI/5 (invert, 200 ms) is true at rest in the "
          "NBIRTH, false at least 200 ms after the input becomes active, and its counter counts the "
          "activation after the inversion (the input back to rest)",
          rest is not None and rest.boolean_value and nd
          and metric(nd[2], alias=14).boolean_value is False and lat >= 180
          and nd2 and counter_of(nd2[2], 5) == 1,
          f"NBIRTH DI/5 {rest.boolean_value if rest else None}; console->NDATA {lat:.0f} ms; "
          f"Counter/5 after the release {counter_of(nd2[2], 5) if nd2 else None}"
          if nd else f"NBIRTH DI/5 {rest.boolean_value if rest else None}; no NDATA for DI/5")

    # #218: DI/8 is a fast counter: the PCNT counts it (the simulated unit in QEMU: 'pcnt' and
    # the edges of 'di'), not the debounced activations
    obs.drain()
    gw.console("pcnt 8 1000")
    nd = obs.wait(ndata_t, 3, lambda q: counter_of(q, 8) is not None)
    c1 = counter_of(nd[2], 8) if nd else None
    since = time.monotonic()
    gw.console("di 8 1")
    nd = obs.wait(ndata_t, 3, lambda q: counter_of(q, 8) is not None)
    time.sleep(1)
    gw.console("di 8 0")
    time.sleep(0.5)
    later = [counter_of(m[2], 8) for m in CAPTURE if m[0] >= since and m[1] == ndata_t
             and counter_of(m[2], 8) is not None]
    check("#218 fast counter: Counter/8 counts the pulses of the PCNT unit of DI/8 (1000 from the "
          "simulated unit, then one edge of the input), and the debounced activation does not "
          "count it a second time", c1 == 1000 and later == [1001],
          f"Counter/8 after 'pcnt 8 1000': {c1}; after one activation: {later}")

    # #210: DO/7 is a pulse DO of 300 ms: NCMD true gives NDATA true, then NDATA false
    obs.drain()
    since = time.monotonic()
    obs.publish_cmd(ncmd_t, [{"name": "DO/7", "datatype": 11, "boolean_value": True}])
    on = obs.wait(ndata_t, 3, lambda q: do_states(q).get(7) is True)
    off = obs.wait(ndata_t, 3, lambda q: do_states(q).get(7) is False)
    dt = (off[0] - on[0]) * 1000 if on and off else None
    log = gw.wait_log(r"io: DO7: pulse of 300 ms", 2, since)
    check("#210 pulse DO: an NCMD DO/7=true (do_pulse_ms 300) gives an NDATA with true, and an NDATA "
          "with false after the pulse, without a second command (tck-id-topics-ndata-payload)",
          on and off and 200 <= dt <= 3000 and log,
          f"true -> false {dt:.0f} ms; {(gw.find(r'io: DO7: pulse', since) or ['no log'])[0]}"
          if dt is not None else f"NDATA true {bool(on)}, NDATA false {bool(off)}")

    # #217: run hours of DI/4 and DO/2 (DO/2 is on since the boot: do_failsafe "on")
    names = {m.name: m.datatype for m in nb[2].metrics if m.name.startswith("Run Hours/")}
    gw.console("di 4 1")
    time.sleep(3)
    obs.drain()
    gw.console("di 4 0")
    nd = obs.wait(ndata_t, 3, lambda q: run_hours_of(q, "DI/4") is not None)
    rh4 = run_hours_of(nd[2], "DI/4") if nd else None
    # DO/2 is on since the boot: its value so far (the NBIRTH, or an NDATA of each 60 s while
    # on); then 3 s off, 3 s on by NCMD, and off again: the NDATA of the last edge has 3 s more
    up = time.monotonic() - t0
    off2 = set_do(obs, ncmd_t, ndata_t, {2: False})
    time.sleep(1.5)
    seen = [run_hours_of(m[2], "DO/2") for m in CAPTURE if m[0] >= nb[0]
            and m[1] in (ndata_t, nb[1]) and run_hours_of(m[2], "DO/2") is not None]
    before = seen[-1] if seen else None
    on2 = set_do(obs, ncmd_t, ndata_t, {2: True})
    time.sleep(3)
    obs.drain()
    since = time.monotonic()
    set_do(obs, ncmd_t, ndata_t, {2: False})
    # the run hours can be in the NDATA that confirms the command (set_do takes it), or in the
    # next one
    end, rh2 = time.monotonic() + 3, None
    while rh2 is None and time.monotonic() < end:
        rh2 = next((run_hours_of(m[2], "DO/2") for m in CAPTURE if m[0] >= since
                    and m[1] == ndata_t and run_hours_of(m[2], "DO/2") is not None), None)
        time.sleep(0.1)
    check("#217 run hours: the NBIRTH has Run Hours/DI/4 and Run Hours/DO/2 (UInt64, only the "
          "channels of local_io.run_hours: tck-id-topics-nbirth-metric-reqs); an NDATA gives the "
          "time on of DI/4 (3 s) when it goes off; DO/2 (on since the boot) has a value up to the "
          "time since the boot, and 3 s more after 3 s on by NCMD",
          names == {"Run Hours/DI/4": 8, "Run Hours/DO/2": 8} and run_hours_of(nb[2], "DI/4") == 0
          and rh4 is not None and 2 <= rh4 <= 4 and off2 and on2 and before is not None
          and before <= up and rh2 is not None and 2 <= rh2 - before <= 4,
          f"NBIRTH {names}; DI/4 {rh4} s; DO/2 {before} s before ({up:.0f} s since QEMU started), "
          f"{rh2} s after 3 s more on")

    # #204: POST /api/do sets a DO through the queue of the NCMD; the NDATA has the new state; a DO
    # of a local rule is refused
    auth = ("admin", commissioning_password(gw, t0, 5) or "?")
    obs.drain()
    since = time.monotonic()
    st1, _, b1 = http("POST", "/api/do", {"do": 8, "on": True}, auth=auth)
    on8 = obs.wait(ndata_t, 3, lambda q: do_states(q).get(8) is True)
    st2, _, b2 = http("POST", "/api/do", {"do": 5, "on": False}, auth=auth)
    st3, _, _ = http("POST", "/api/do", {"do": 8, "on": False}, auth=auth)
    off8 = obs.wait(ndata_t, 3, lambda q: do_states(q).get(8) is False)
    st4, _, _ = http("POST", "/api/do", {"do": 9, "on": True}, auth=auth)
    st5, _, _ = http("POST", "/api/do", {"do": 8, "on": True}, auth=auth, headers={"X-Requested-With": None})
    refused = gw.wait_log(r"io: DO5 is driven by local rule 0 \(follow DI2\): web command refused", 2,
                          since)
    st, _, body = http("GET", "/api/status", auth=auth)
    io = (as_json(body) or {}).get("io", {})
    rh = io.get("run_hours", {})
    status_ok = io.get("rules") == [{"di": 2, "do": 5, "mode": "follow"},
                                    {"di": 3, "do": 6, "mode": "toggle"}] \
        and io.get("pulse_ms") == [0, 0, 0, 0, 0, 0, 300, 0] and io.get("fast") == 0x80 \
        and isinstance(rh.get("di", [None] * 8)[3], int) and rh.get("di", [0])[0] is None \
        and isinstance(rh.get("do", [None] * 8)[1], int) and rh.get("do", [0])[0] is None
    check("#204 POST /api/do: DO/8 on and off through the queue of the NCMD commands, each state in "
          "the next NDATA (tck-id-topics-ndata-payload); a DO of a local rule is refused (409), a "
          "bad DO number (400) and a request without X-Requested-With (403) too; /api/status gives "
          "the rules, the pulse times, the fast counters and the run hours",
          st1 == 200 and on8 and st2 == 409 and refused and st3 == 200 and off8 and st4 == 400
          and st5 == 403 and status_ok,
          f"on {st1} {b1[:60]!r} NDATA {bool(on8)}; DO/5 {st2} {b2[:70]!r}; off {st3} NDATA "
          f"{bool(off8)}; DO/9 {st4}; no X-Requested-With {st5}; status {st}: rules "
          f"{io.get('rules')}, pulse_ms {io.get('pulse_ms')}, fast {io.get('fast')}, run_hours {rh}")


def check_status_api(gw, since):
    """Issue #75: /api/status (HTTPS) reports what IO-3/4/6/7, SPB-17 and TIME-1..3 added: pulse
    counters, failsafe, DOs owned by a local rule, unknown DOs, the buzzer alarm, the clock and
    the RTC, exact 64-bit values and strings of the tags."""
    pw = commissioning_password(gw, since, 5)
    st, _, body = http("GET", "/api/status", auth=("admin", pw or "?"))
    s = as_json(body) or {}
    io, bz, tm = s.get("io", {}), s.get("buzzer", {}), s.get("time", {})
    meter = next((d for d in s.get("devices", []) if d.get("name") == DEVICE), {})
    tags = {t["name"]: t["value"] for t in meter.get("tags", [])}
    # issue #188: the 64-bit values also as text (a browser reads a JSON number as a double).
    # Counter (Int64) has the value of an earlier DCMD here (2^62 + 4), also above 2^53.
    texts = {t["name"]: t.get("value_text") for t in meter.get("tags", [])}
    ok = st == 200 and io.get("counters", [None])[0] == 2 and len(io.get("counters", [])) == 8 \
        and io.get("rule_do") == 0x30 and io.get("failsafe") is False and io.get("do_unknown") is False \
        and isinstance(io.get("do"), int) and bz.get("enabled") == ["device_offline", "mqtt_down", "config_error"] \
        and bz.get("alarm") == "device_offline" and tm.get("synced") is True and tm.get("source") == "sntp" \
        and tm.get("rtc", {}).get("present") is True and tm.get("rtc", {}).get("stores", 0) >= 1 \
        and abs(tm.get("utc_s", 0) - time.time()) < 60 and s.get("sparkplug", {}).get("diagnostics_s") == 10 \
        and tags.get("Energy") == MB6["Energy"] and isinstance(tags.get("Serial"), str) \
        and texts.get("Energy") == str(MB6["Energy"]) \
        and isinstance(tags.get("Counter"), int) and texts.get("Counter") == str(tags["Counter"]) \
        and texts.get("Serial") is None and texts.get("Voltage") is None \
        and meter.get("enabled") is True and meter.get("write_refused") == 0
    check("Issue #75 /api/status over HTTPS: pulse counters, failsafe, DOs of local rules, unknown "
          "DOs, buzzer alarm, clock (SNTP/RTC), diagnostics period, exact UInt64 and string values; "
          "issue #188: value_text of the Int64 and UInt64 tags only; #214 enabled and #196 "
          "write_refused of each device",
          ok, f"HTTP {st}; io {io}; buzzer {bz}; time {tm}; Energy {tags.get('Energy')!r} "
              f"Serial {tags.get('Serial')!r}; value_text {texts}; enabled {meter.get('enabled')!r} "
              f"write_refused {meter.get('write_refused')!r}")


def check_rtc(gw, obs, nb, t0, since_reboot, ndata_t, first_nbirth):
    """TIME-2: the first boot (power-on: the simulated PCF85063 has its OS bit set) gets no time
    from it, and stores the SNTP time in it; the boot after NCMD Reboot, with SNTP silent, sets
    the system clock from the RTC, publishes its NBIRTH with valid timestamps at once, and when
    SNTP answers again there is no rebirth (the births were already timed)."""
    stopped = gw.find(r"io: RTC: oscillator stopped \(OS bit\): time lost: system time not set", t0)
    stored = gw.find(r"io: RTC: time stored in the RTC \(again after every SNTP", t0)
    restored = gw.find(r"io: RTC: system time set from the RTC: \d{4}-\d\d-\d\dT", since_reboot)
    synced_early = gw.find(r"edge: time synchronised", since_reboot)
    skew = abs(nb[2].timestamp / 1000 - time.time()) if nb else None
    asked, h = gw.shell("status", [r"^time\s+\d{4}-.*Z, from the RTC \(not synchronised\); RTC ok"])
    shown = list(h.values())[0]
    obs.drain()
    SNTP_PAUSED.clear()
    synced = gw.wait_log(r"edge: time synchronised", 45, since_reboot)
    ts_ndata = obs.wait(ndata_t, 5, lambda q: metric(q, alias=55) is not None)
    first_ts = metric(first_nbirth, name="Node Info/Time Synced")
    reboot_ts = metric(nb[2], name="Node Info/Time Synced") if nb else None
    check("TIME-3 'Node Info/Time Synced' (Boolean, alias 55): true in the NBIRTH after SNTP, false "
          "in the NBIRTH timed by the RTC, NDATA true when SNTP synchronises later",
          first_ts is not None and first_ts.boolean_value and reboot_ts is not None
          and not reboot_ts.boolean_value and ts_ndata is not None
          and metric(ts_ndata[2], alias=55).boolean_value,
          f"first NBIRTH {first_ts.boolean_value if first_ts else None}; NBIRTH after the reboot "
          f"{reboot_ts.boolean_value if reboot_ts else None}; NDATA "
          f"{metric(ts_ndata[2], alias=55).boolean_value if ts_ndata else None}")
    # no NBIRTH after the one of the reboot: no rebirth when SNTP answers (issue #160)
    reborn = [m for m in CAPTURE if nb and m[0] > nb[0] and m[1].split("/")[2:4] == ["NBIRTH", NODE]]
    restore_store = gw.wait_log(r"edge: time synchronised", 0, since_reboot) and not reborn
    check("TIME-2 RTC: OS bit at power-on (no time from it), SNTP time stored in it; after a "
          "reboot with SNTP silent the clock comes from the RTC, NBIRTH timestamps valid at once, "
          "no rebirth when SNTP answers later",
          stopped and stored and restored and not synced_early and skew is not None and skew < 60
          and shown and synced and restore_store,
          f"{(stopped or ['no OS-bit log'])[0]} | {(stored or ['no store log'])[0]} | "
          f"{(restored or ['no restore log'])[0]} | NBIRTH skew {round(skew, 1) if skew else None} s; "
          f"SNTP before the NBIRTH: {bool(synced_early)}; shell {'ok' if shown else 'no'}; "
          f"SNTP later {'yes' if synced else 'no'}")


def check_birth_stamps(t0):
    """Issue #160 (decision of 2026-11-14) and the #175 interop test: no birth goes out before a
    valid clock, so every NBIRTH and DBIRTH (after SNTP, and with the RTC time after NCMD Reboot)
    has a UTC payload timestamp and only UTC metric timestamps. A value read before the clock was
    valid (the DO boot state, read before the RTC set the clock; a poll before SNTP) gets the
    time of the birth, not ~1970."""
    births = [m for m in CAPTURE if m[0] >= t0 and m[1].split("/")[2:4] in
              (["NBIRTH", NODE], ["DBIRTH", NODE])]
    early = [(m[1].split("/")[2], mm.name or mm.alias, mm.timestamp) for m in births
             for mm in m[2].metrics if mm.timestamp < Y2020_MS]
    early += [(m[1].split("/")[2], "payload", m[2].timestamp) for m in births
              if m[2].timestamp < Y2020_MS]
    nbirths = sum(1 for m in births if "/NBIRTH/" in m[1])
    check("Issue #160 every NBIRTH and DBIRTH (after SNTP, and with the RTC time after the reboot) "
          "has a UTC payload timestamp and UTC metric timestamps: none before 2020",
          nbirths >= 2 and not early,
          f"{nbirths} NBIRTH, {len(births) - nbirths} DBIRTH; timestamps before 2020: {early[:4]}")


def modbus_rows(m):
    """SPB-17: {device: {column: value}} of the 'Node Info/Modbus' DataSet."""
    ds = m.dataset_value
    return {row.elements[0].string_value: {c: spb_decode.value_of(e) for c, e in zip(ds.columns, row.elements, strict=False)}
            for row in ds.rows}


def check_modbus_tcp_table(obs, ndata_t):
    """Issue #81: the next diagnostics NDATA carries "Node Info/Modbus TCP" (alias 56) with the
    same per-unit counters the shell shows after check_modbus_tcp (#69): Meter1 2 OK, Dead3 3
    timeouts, units without a device none, and the poll table unchanged in its columns."""
    obs.drain()
    nd = obs.wait(ndata_t, 13, lambda q: metric(q, alias=56) is not None)
    m = metric(nd[2], alias=56) if nd else None
    ds = m.dataset_value if m is not None else None
    rows = {r.elements[0].string_value: {c: spb_decode.value_of(e) for c, e in zip(ds.columns, r.elements, strict=False)}
            for r in ds.rows} if ds is not None else {}
    meter, dead, other = rows.get("Meter1", {}), rows.get("Dead3", {}), rows.get("(other units)", {})
    ok = ds is not None and list(ds.columns) == ["Device", "Unit", "Online", "OK", "Timeout", "CRC",
                                                 "Frame", "Exception"] \
        and list(rows) == ["Meter1", "Bulk", "Dead3", "Dead4", "Dead5", "(other units)"] \
        and meter.get("Unit") == 1 and meter.get("Online") is True and meter.get("OK", 0) >= 2 \
        and meter.get("Timeout") == 0 and dead.get("Unit") == 3 and dead.get("Online") is False \
        and dead.get("OK") == 0 and dead.get("Timeout") == 3 and other.get("OK") == 0 \
        and other.get("Timeout") == 0
    check("Issue #81 'Node Info/Modbus TCP' DataSet (alias 56): a row per device with the Modbus TCP "
          "requests forwarded to its unit and one for units without a device, as in the shell (#69)",
          ok, f"Meter1 {meter}; Dead3 {dead}; other {other}; rows {list(rows)}")


def check_modbus_tcp_via_ap(gw, slave):
    """Issue #120 (MB-15, NET-5): the Modbus TCP gateway refuses each connection through the
    commissioning access point, also when modbus_tcp.allowed_clients contains the client. QEMU has
    no radio: the emulator-only 'claimsim 0 eth' makes the Ethernet count as the access point.
    The refusal closes the connection without an answer, writes a log line and counts as rejected.
    'claimsim 0' makes the Ethernet a plant interface again, and a request is answered."""
    _, h0 = gw.shell("claimsim 0 eth", [r"claim windows: 0 s \(Kconfig\); Ethernet counts as the "
                                         r"access point"])
    asked = time.monotonic()
    refused = mbap_raw(b"\x03\x00\x0a\x00\x01")
    log = gw.wait_log(r"mbtcp: connection from 10\.0\.2\.2 refused: through the access point", 5, asked)
    _, h1 = gw.shell("devices", [r"^modbus_tcp connections 2 accepted 1 rejected \| forwarded 3 \|"])
    _, h2 = gw.shell("claimsim 0", [r"claim windows: 0 s \(Kconfig\); Ethernet is not the access point"])
    again = mbap_raw(b"\x03\x00\x0a\x00\x01")
    expect = b"\x03\x02" + struct.pack(">H", slave.get(3, 10)[0])
    check("MB-15/NET-5 #120 the Modbus TCP gateway refuses a client of the access point (Ethernet as "
          "the access point in QEMU), also when allowed_clients contains it: closed without an "
          "answer, logged, counted as rejected; a plant client is answered again",
          all(h0.values()) and refused == b"" and log and all(h1.values()) and all(h2.values())
          and again == expect,
          f"claimsim eth {all(h0.values())}; answer through the access point {refused!r}; log "
          f"{bool(log)}; devices {all(h1.values())} "
          f"{(gw.find(r'^modbus_tcp connections', asked) or ['no answer'])[0]}; claimsim 0 "
          f"{all(h2.values())}; answer after {again!r} (expected {expect!r})")


LOCAL_UNIT = 100   # #216: modbus_tcp.local_unit_id of tools/e2e/fs/config.json


def check_modbus_local(gw, obs, ndata_t):
    """Issue #216: the local unit of the Modbus TCP server (modbus_tcp.local_unit_id 100,
    local_writes true) answers from the I/O of the gateway, never from the bus: FC02 gives the
    DIs (DI/4 set with the emulator), FC04 the counter of DI/4 (uint64 big, +1 after one pulse),
    FC05 switches DO/1 through the local_io task (FC01 reads it back, the NDATA has it) and back.
    A DO that a local rule drives (DO/5, rule di 2 -> do 5) gives exception 03; FC03 gives 01; an
    address past DO/8 gives 02. Issue #234: FC04 32..95 gives the run hours of DI/1..8 and
    DO/1..8, the values of the last Run Hours metrics (DI/4 and DO/2 of local_io.run_hours; 0
    for the other channels). DO/2 (on since the boot) goes off by FC05 first, so that its value
    is the time on since the boot (not 0), then on again for the later checks. Input register 96,
    and a range that crosses 95, give 02."""
    def frame(pdu):
        try:
            return mbap_raw(pdu, unit=LOCAL_UNIT)
        except OSError as e:
            return f"{e}".encode()

    def u64(a):
        return struct.unpack(">Q", a[2:10])[0] if a and len(a) == 10 and a[0] == 4 else None

    c0 = u64(frame(b"\x04\x00\x0c\x00\x04"))           # the counter of DI/4: registers 12..15
    gw.console("di 4 1")
    time.sleep(0.5)                                          # > the debounce of 20 ms
    di = frame(b"\x02\x00\x00\x00\x08")
    gw.console("di 4 0")
    time.sleep(0.5)
    c1 = u64(frame(b"\x04\x00\x0c\x00\x04"))
    before = frame(b"\x01\x00\x00\x00\x08")
    on0 = bool(before[2] & 1) if before and len(before) == 3 and before[0] == 1 else None
    obs.drain()
    wr = frame(b"\x05\x00\x00" + (b"\x00\x00" if on0 else b"\xff\x00"))
    after = frame(b"\x01\x00\x00\x00\x01")
    nd = obs.wait(ndata_t, 5, pred=lambda pl: metric(pl, alias=ALIAS["DO/1"]) is not None)
    do1 = metric(nd[2], alias=ALIAS["DO/1"]) if nd else None
    back = frame(b"\x05\x00\x00" + (b"\xff\x00" if on0 else b"\x00\x00"))
    rule = frame(b"\x05\x00\x04\xff\x00")
    hold = frame(b"\x03\x00\x00\x00\x01")
    past = frame(b"\x01\x00\x08\x00\x01")
    ok = c0 is not None and c1 == c0 + 1 and di[:2] == b"\x02\x01" and di[2] & 0x08 \
        and on0 is not None and wr[:3] == b"\x05\x00\x00" and after == bytes([1, 1, 0 if on0 else 1]) \
        and do1 is not None and do1.boolean_value == (not on0) and back[:1] == b"\x05" \
        and rule == b"\x85\x03" and hold == b"\x83\x01" and past == b"\x81\x02"
    check("Issue #216 Modbus TCP local unit 100: FC02 DIs, FC04 counter of DI/4 (uint64 big, +1 per "
          "pulse), FC05 DO/1 through the local_io task (FC01 and NDATA confirm, then back); DO/5 of "
          "a local rule -> 03, FC03 -> 01, coil 8 -> 02", ok,
          f"counter {c0} -> {c1}; FC02 {di!r}; DO/1 {on0} -> FC05 {wr!r}, FC01 {after!r}, NDATA "
          f"{do1.boolean_value if do1 else None}, back {back!r}; DO/5 {rule!r}; FC03 {hold!r}; "
          f"coil 8 {past!r}")

    # #234: the run hours. DO/2 is on since the boot (do_failsafe "on"): its value moves only
    # every 60 s while on, so FC05 switches it off; the edge gives its time on at once. DI/4 got a
    # new value when it went off above. The tagdb gives the value before the NDATA goes out, so
    # wait (bounded) until the read agrees with the last published metrics.
    rh_topics = (f"{T}/NBIRTH/{NODE}", ndata_t)
    do2_on = bool(before[2] & 2) if before and len(before) == 3 and before[0] == 1 else None
    do2_off = frame(b"\x05\x00\x01\x00\x00")

    def published(ch):
        return next((run_hours_of(m[2], ch) for m in reversed(list(CAPTURE))
                     if m[1] in rh_topics and run_hours_of(m[2], ch) is not None), None)

    want = got = rh = None
    end = time.monotonic() + 5
    while time.monotonic() < end:
        rh = frame(b"\x04\x00\x20\x00\x40")                  # 32..95
        time.sleep(0.5)                                        # the NDATA of a new value
        got = list(struct.unpack(">16Q", rh[2:130])) if rh and len(rh) == 130 \
            and rh[:2] == b"\x04\x80" else None
        want = [0] * 16
        want[3], want[9] = published("DI/4"), published("DO/2")
        if got == want:
            break
    do2_back = frame(b"\x05\x00\x01\xff\x00")                # DO/2 on again
    over = frame(b"\x04\x00\x60\x00\x01")                   # 96
    cross = frame(b"\x04\x00\x5e\x00\x04")                  # 94..97
    check("Issue #234 Modbus TCP local unit 100: FC04 32..95 = the run hours of DI/1..8 and "
          "DO/1..8 (uint64 s, big), the same values as Run Hours/DI/4 and Run Hours/DO/2 (DO/2 "
          "switched off by FC05 after its time on since the boot: not 0), 0 for the channels "
          "outside local_io.run_hours; input 96 and 94..97 -> 02",
          do2_on is True and do2_off == b"\x05\x00\x01\x00\x00" and got is not None
          and got == want and None not in want and want[9] > 0
          and do2_back == b"\x05\x00\x01\xff\x00" and over == b"\x84\x02"
          and cross == b"\x84\x02",
          f"DO/2 on {do2_on}, FC05 off {do2_off!r}, on again {do2_back!r}; FC04 32..95 "
          f"{got if got is not None else rh!r}; metrics {want}; 96 {over!r}; 94..97 {cross!r}")


def check_diagnostics(obs, nbirth, ndata_t):
    """SPB-17 / MB-13: NBIRTH with Uptime, Free Heap, Min Free Heap (no RSSI: no Wi-Fi in QEMU) and
    the Modbus DataSet (a row per device, counters of the dead units too); then, with
    sparkplug.diagnostics_s = 10, an NDATA every 10 s with uptime and heap, and the DataSet
    because the counters keep moving."""
    up = metric(nbirth, name="Node Info/Uptime")
    heap = metric(nbirth, name="Node Info/Free Heap")
    low = metric(nbirth, name="Node Info/Min Free Heap")
    mb = metric(nbirth, name="Node Info/Modbus")
    rows = modbus_rows(mb) if mb is not None and mb.datatype == 16 else {}
    birth_ok = up is not None and up.datatype == 8 and up.alias == 50 and heap is not None \
        and heap.datatype == 8 and heap.long_value > 10000 and low is not None \
        and low.long_value <= heap.long_value and metric(nbirth, name="Node Info/RSSI") is None \
        and list(rows) == ["Meter1", "Bulk", "Dead3", "Dead4", "Dead5"] \
        and list(mb.dataset_value.columns) == ["Device", "Online", "Cycles", "Failed Cycles", "OK",
                                               "Timeout", "CRC", "Frame", "Exception"]
    obs.drain()
    is_diag = lambda q: metric(q, alias=50) is not None  # noqa: E731
    first = obs.wait(ndata_t, 13, is_diag)
    second = obs.wait(ndata_t, 13, is_diag)
    gap = second[0] - first[0] if first and second else None
    u1 = metric(first[2], alias=50).long_value if first else None
    u2 = metric(second[2], alias=50).long_value if second else None
    ds = metric(second[2], alias=54) if second else None
    later = modbus_rows(ds) if ds is not None else {}
    m1, d3 = later.get("Meter1", {}), later.get("Dead3", {})
    ok = birth_ok and gap is not None and 9.0 <= gap <= 11.5 and u2 - u1 in (9, 10, 11) \
        and m1.get("Online") is True and m1.get("OK", 0) > rows.get("Meter1", {}).get("OK", 0) \
        and d3.get("Online") is False and d3.get("Timeout", 0) > 0
    check("SPB-17 / MB-13 node diagnostics: NBIRTH Uptime/Free Heap/Min Free Heap and the 'Node "
          "Info/Modbus' DataSet (one row per device, dead units too); NDATA every diagnostics_s (10 s), "
          "uptime moving, counters updated", ok,
          f"NBIRTH uptime {up.long_value if up else None} s, heap {heap.long_value if heap else None} B "
          f"(min {low.int_value if low else None}), rows {list(rows)}; NDATA gap "
          f"{round(gap, 2) if gap else None} s, uptime {u1} -> {u2}; Meter1 {m1}; Dead3 {d3}")


def int64_of(m):
    """Int64 metric value: long_value carries the two's complement as an unsigned 64-bit int."""
    return m.long_value - 2 ** 64 if m.long_value >= 2 ** 63 else m.long_value


def check_mb6_birth(db):
    """MB-6: each new type and byte order, decoded with the right Sparkplug datatype."""
    if not db:
        return check("MB-6 DBIRTH with 64-bit, string and bit tags", False, "no DBIRTH")
    p = db[2]
    got = {}
    want = {"Energy": (8, MB6["Energy"]), "Counter": (4, MB6["Counter"]), "Flow": (10, MB6["Flow"]),
            "Temp": (3, MB6["Temp"]), "Serial": (12, MB6["Serial"]), "Alarm": (11, True),
            "Ready": (11, True)}
    for name in want:
        m = metric(p, name=name)
        if m is None:
            got[name] = None
            continue
        # B023: a false positive; the lambda is called at once, in the same iteration as its m
        value = {8: lambda: m.long_value, 4: lambda: int64_of(m), 10: lambda: m.double_value,  # noqa: B023
                 3: lambda: m.int_value - 2 ** 32 if m.int_value >= 2 ** 31 else m.int_value,  # noqa: B023
                 12: lambda: m.string_value, 11: lambda: m.boolean_value}[m.datatype]()  # noqa: B023
        got[name] = (m.datatype, value)
    return check("MB-6 DBIRTH: uint64 ABCD (2^53+1 exact), int64 CDAB, float64 DCBA, int32 BADC, "
                 "string and register bits, with datatypes UInt64/Int64/Double/Int32/String/Boolean",
                 got == want, f"got {got}")


def check_mb6_writes(obs, slave, dcmd_t, ddata_t):
    """MB-6 DCMD: a 64-bit value beyond 2^53 written exactly with one FC16 of 4 registers, a
    string, and a register bit by read-modify-write (the register's other bits survive)."""
    value = 2 ** 62 + 3
    obs.drain()
    slave.ctx.writes.clear()
    obs.publish_cmd(dcmd_t, [{"name": "Counter", "datatype": 4, "long_value": value}])
    dd = obs.wait(ddata_t, 5, lambda q: metric(q, alias=ALIAS["Counter"]) is not None)
    regs = [(value >> (16 * i)) & 0xFFFF for i in range(4)]   # CDAB: least significant word first
    back = int64_of(metric(dd[2], alias=ALIAS["Counter"])) if dd else None
    check("MB-6 DCMD int64 2^62+3 -> one FC16 of 4 registers (CDAB), DDATA exact",
          dd and slave.ctx.writes == [(16, 34, regs)] and back == value,
          f"slave writes {slave.ctx.writes} (expected {[(16, 34, regs)]}), DDATA {back}")
    obs.drain()
    slave.ctx.writes.clear()
    obs.publish_cmd(dcmd_t, [{"name": "Alarm", "datatype": 11, "boolean_value": False},
                             {"name": "Serial", "datatype": 12, "string_value": "HELLO"}])
    seen = {}
    end = time.monotonic() + 5
    while len(seen) < 2 and time.monotonic() < end:
        dd = obs.wait(ddata_t, max(0.1, end - time.monotonic()))
        for a in ("Alarm", "Serial"):
            m = metric(dd[2], alias=ALIAS[a]) if dd else None
            if m is not None:
                seen[a] = m.boolean_value if a == "Alarm" else m.string_value
    hello = list(struct.unpack(">8H", b"HELLO".ljust(16, b"\0")))
    reg60 = slave.get(3, 60)[0]
    check("MB-6 DCMD register bit (read-modify-write keeps the other bits) and string",
          (6, 60, [0x0101]) in slave.ctx.writes and (16, 50, hello) in slave.ctx.writes
          and reg60 == 0x0101 and seen == {"Alarm": False, "Serial": "HELLO"},
          f"slave writes {slave.ctx.writes}, holding 60 = {reg60:#06x}, DDATA {seen}")


def run_network_variant(args, work, slave):
    """Optional network keys on the emulator, in one boot with a separate edge node id (its
    messages stay apart from the main scenario's):
      * NET-4: no Wi-Fi radio (CONFIG_GW_WIFI=n): a configuration that enables network.wifi still
        boots, says that Wi-Fi is not available and runs over Ethernet exactly as before.
      * NET-7: slirp announces fec0::/64 and answers ICMPv6 for its virtual host fec0::2. The
        gateway must get a link-local address, a SLAAC address and the static one, and reach
        fec0::2 over IPv6. MQTT over IPv6 cannot be tested here: slirp would relay it to the
        host's ::1 and this container's kernel has no IPv6 (AF_INET6 is not supported).
      * SPB-19: a "bytes" tag (device Raw, unit 1 holding 220-222) is a Bytes metric with the raw
        register bytes, and a DCMD with Bytes writes them back (FC16).
      * SPB-18 #213: two devices of the device type "Pump" (PumpA on unit 1 and PumpB on unit 7,
        so that unit 2 keeps the single FC03 the main scenario checks; both at holding 230 and
        coil 5): one template definition in the NBIRTH, an instance per DBIRTH, a DCMD member
        write and its DDATA.
      * #196, #197, #202, #214 (check_devices_gw02): a device with write_multiple, a tag with write
        limits and a field of 3 bits (Drive, unit 1 holding 250 and 252), and a disabled device
        (Spare, unit 1 holding 240)."""
    node = "GW02"
    cfg = json.loads(config_json())
    cfg["sparkplug"]["edge_node_id"] = node
    cfg["network"] = {"dhcp": True,
                      "ipv6": {"enabled": True, "slaac": True, "address": "fec0::1234"},
                      "wifi": {"enabled": True, "ssid": "Plant WLAN", "password": "12345678",
                               "role": "backup", "failback_s": 30}}
    cfg["modbus_tcp"] = {"enabled": True, "allowed_clients": ["192.0.2.0/24"]}
    # #201: the first SNTP server never answers (TEST-NET-1, no route), the second one is the
    # harness' responder
    cfg["ntp"] = {"servers": ["192.0.2.1", "10.0.2.2"]}
    cfg["devices"].append({"name": "Raw", "unit_id": 1, "poll_ms": 500, "tags": [
        {"name": "Block", "area": "holding", "address": 220, "type": "bytes", "length": 3,
         "writable": True}]})
    slave.ctx.setValues(3, 220, [0x00FF, 0x1000, 0x7F80])
    # #213: two devices of one device type: the tag list is in device_types one time, and
    # PumpA (unit 1) and PumpB (unit 7) read the same addresses
    cfg["device_types"] = [{"name": "Pump", "tags": [
        {"name": "Speed", "area": "holding", "address": 230, "type": "uint16", "units": "rpm",
         "writable": True},
        {"name": "Run", "area": "coil", "address": 5, "type": "bool"}]}]
    for k, (uid, unit) in enumerate(((1, slave.ctx), (7, slave.unit7))):
        cfg["devices"].append({"name": f"Pump{'AB'[k]}", "unit_id": uid, "type": "Pump",
                               "poll_ms": 500})
        unit.setValues(3, 230, [1450 + 10 * k])
        unit.setValues(1, 5, [True])
    # #197 and #196: FC16 also for one register, and the write limits of a tag
    # #202: State, a field of 3 bits (4 to 6) of the holding register 252
    cfg["devices"].append({"name": "Drive", "unit_id": 1, "poll_ms": 500, "write_multiple": True,
                           "tags": [{"name": "Speed", "area": "holding", "address": 250,
                                     "type": "uint16", "writable": True, "write_min": 0,
                                     "write_max": 1500},
                                    {"name": "State", "area": "holding", "address": 252,
                                     "type": "uint16", "bit_offset": 4, "bit_width": 3,
                                     "writable": True}]})
    slave.ctx.setValues(3, 250, [100])
    slave.ctx.setValues(3, 252, [0xA0D9])   # bits 4 to 6 = 0b101 = 5
    # #214: a disabled device on a unit that answers: it must get no request
    cfg["devices"].append({"name": "Spare", "unit_id": 1, "poll_ms": 200, "enabled": False,
                           "tags": [{"name": "Level", "area": "holding", "address": 240,
                                     "type": "uint16", "writable": True}]})
    flash = flash_variant(args, work, "network", {"config.json": json.dumps(cfg, indent=2).encode()})
    obs = Observer()
    gw = Gateway(args, work / "uart0-network.log", flash)
    t0 = time.monotonic()
    gw.start()
    # #201: lwIP asks the second server after SNTP_RECV_TIMEOUT (15 s) without an answer, so the
    # clock becomes valid by SNTP after a wait of 10 s or more (only 10.0.2.2 answers): wait for
    # this evidence first, then for the NBIRTH that follows it
    synced = gw.wait_log(r"edge: clock valid \(source sntp\) after a wait of (1[0-9]|[2-5][0-9]) s", 75,
                         t0)
    nb = obs.wait(f"{T}/NBIRTH/{node}", 40)
    ignored = gw.find(r"network\.wifi ignored: Wi-Fi is not available in this build", t0)
    asked, h = gw.shell(["status", "config"],
                        [r"^iface\s+ethernet up \d+\.\d+\.\d+\.\d+ \(active\) \| wifi not configured",
                         r"^wifi\s+'Plant WLAN', backup of Ethernet, failback 30 s, DHCP",
                         r"^ntp\s+192\.0\.2\.1, 10\.0\.2\.2$"])
    iface, shown, ntp_shown = h.values()
    # NET-7
    lla = gw.wait_log(r"net: ethernet: IPv6 fe80:[0-9a-f:]+ \(link-local\)", 10, t0)
    slaac = gw.wait_log(r"net: ethernet: IPv6 fec0::(?!1234 )[0-9a-f:]+ \(site local\)", 10, t0)
    static = gw.wait_log(r"net: ethernet: IPv6 fec0::1234 \(site local\)", 1, t0)
    asked6, h = gw.shell(["status", "config"],
                         [r"^ipv6\s+ethernet fe80:.*ethernet fec0::",
                          r"^ipv6\s+SLAAC on, static address fec0::1234, DNS none"])
    listed, cfg6 = h.values()
    pinged = time.monotonic()
    gw.console("ping fec0::2")
    replies = gw.wait_log(r"3 sent, [1-3] received", 8, pinged)
    # MB-15 allow-list: slirp's 10.0.2.2 is not in 192.0.2.0/24, so the connection is closed
    # without an answer
    asked_tcp = time.monotonic()
    try:
        denied = mbap_raw(b"\x03\x00\x0a\x00\x01")
    except OSError as e:
        denied = f"{e}"
    refused = gw.wait_log(r"mbtcp: connection from 10\.0\.2\.2 refused: not in modbus_tcp\.allowed_clients",
                          3, asked_tcp)
    _, h = gw.shell("devices", [r"^modbus_tcp connections 0 accepted 1 rejected \| forwarded 0 "])
    counted = all(h.values())
    # SPB-19 Bytes
    raw_birth = obs.wait(f"{T}/DBIRTH/{node}/Raw", 15)
    block = metric(raw_birth[2], name="Block") if raw_birth else None
    obs.drain()
    written_from = len(slave.ctx.writes)
    obs.publish_cmd(f"{T}/DCMD/{node}/Raw", [{"name": "Block", "datatype": 17,
                                              "bytes_value": b"\xde\xad\xbe\xef\x01\x02"}])
    raw_data = obs.wait(f"{T}/DDATA/{node}/Raw", 5)
    writes = slave.ctx.writes[written_from:]
    tmpl = check_templates_gw02(obs, slave, node, nb)
    devs = check_devices_gw02(obs, slave, gw, node, t0)
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node}", 5)   # the will, before the main scenario starts
    obs.close()
    check("Issue #201 two SNTP servers: the first one does not answer, the second one gives the "
          "time; the clock becomes valid and the NBIRTH follows; the shell lists both servers",
          synced and nb and ntp_shown,
          " | ".join(gw.find(r"clock valid|^ntp\s", t0)) + f"; NBIRTH {'yes' if nb else 'no'}")
    check("NET-4 network.wifi on a build without the radio (QEMU): accepted, reported, Ethernet "
          "carries the session as before",
          nb and ignored and iface and shown,
          f"NBIRTH {'yes' if nb else 'no'}; {(ignored or ['no warning'])[0]}; shell: "
          + " | ".join(gw.find(r"^iface\s|^wifi\s", asked)))
    check("MB-15 Modbus TCP allow-list: a client outside modbus_tcp.allowed_clients is "
          "disconnected without an answer and counted as rejected (#69)", denied == b"" and refused
          and counted,
          f"answer {denied!r}; {(gw.find(r'mbtcp: connection from', asked_tcp) or ['no log'])[0]}")
    got = raw_data[2].metrics[0].bytes_value if raw_data and raw_data[2].metrics else None
    check("SPB-19 'bytes' tag: Bytes metric (17) with the raw register bytes (NUL included) in the "
          "DBIRTH; DCMD with Bytes -> one FC16 of the 3 registers and DDATA with the new bytes",
          block is not None and block.datatype == 17 and block.bytes_value == b"\x00\xff\x10\x00\x7f\x80"
          and writes == [(16, 220, [0xDEAD, 0xBEEF, 0x0102])] and got == b"\xde\xad\xbe\xef\x01\x02",
          f"DBIRTH {block.datatype if block else None} {block.bytes_value.hex() if block else None}; "
          f"writes {writes}; DDATA {got.hex() if got else None}")
    check("SPB-18 device type 'Pump': one template definition in the NBIRTH (no alias, members "
          "Speed UInt16 'rpm' and Run Boolean), an instance per DBIRTH (template_ref, values, "
          "Quality), DCMD on a member -> FC06 and DDATA with the instance and that member only",
          tmpl[0], tmpl[1])
    for name, ok, detail in devs:
        check(name, ok, detail)
    check("NET-7 IPv6 on Ethernet: link-local, SLAAC (fec0::/64 from slirp's RA) and static "
          "address, shown by the shell; ICMPv6 echo to fec0::2 answered",
          lla and slaac and static and listed and cfg6 and replies,
          " | ".join(gw.find(r"net: ethernet: IPv6 ", t0)[:4]) + " | "
          + " | ".join(gw.find(r"^ipv6\s|reply from|sent, \d+ received", asked6)))


def check_templates_gw02(obs, slave, node, nb):
    """SPB-18 on the GW02 boot (see run_network_variant). Returns (ok, detail)."""
    defs = [m for m in nb[2].metrics if m.datatype == 19] if nb else []
    d = defs[0] if defs else None
    members = [(mm.name, mm.datatype, props(mm).get("engUnit")) for mm in d.template_value.metrics] if d else []
    def_ok = len(defs) == 1 and d.name == "Pump" and not d.HasField("alias") \
        and d.template_value.is_definition and not d.template_value.template_ref \
        and members == [("Speed", 6, "rpm"), ("Run", 11, None)]
    births = {}
    for name in ("PumpA", "PumpB"):
        b = next((m for m in CAPTURE if m[1] == f"{T}/DBIRTH/{node}/{name}"), None) \
            or obs.wait(f"{T}/DBIRTH/{node}/{name}", 10)
        births[name] = b
    inst = {}
    for name, b in births.items():
        m = b[2].metrics[0] if b and len(b[2].metrics) == 1 else None
        if m is not None and m.datatype == 19 and m.template_value.template_ref == "Pump" \
                and not m.template_value.is_definition:
            inst[name] = {mm.name: (spb_decode.value_of(mm), props(mm).get("Quality"))
                          for mm in m.template_value.metrics}
    inst_ok = inst.get("PumpA") == {"Speed": (1450, 192), "Run": (True, 192)} \
        and inst.get("PumpB") == {"Speed": (1460, 192), "Run": (True, 192)}
    # DCMD: the instance metric with one member, by name
    p = spb.Payload()
    p.timestamp = int(time.time() * 1000)
    m = p.metrics.add()
    m.name, m.datatype = "Pump", 19
    m.template_value.template_ref = "Pump"
    mm = m.template_value.metrics.add()
    mm.name, mm.datatype, mm.int_value = "Speed", 6, 1234
    obs.drain()
    before = len(slave.ctx.writes)
    obs.c.publish(f"{T}/DCMD/{node}/PumpA", p.SerializeToString(), qos=0)
    dd = obs.wait(f"{T}/DDATA/{node}/PumpA", 5)
    wrote = slave.ctx.writes[before:]
    dm = dd[2].metrics[0] if dd and dd[2].metrics else None
    alias = births["PumpA"][2].metrics[0].alias if births.get("PumpA") else None
    # #159: no datatype in DDATA, on the instance or its members; the DBIRTH gives Template
    data_ok = dm is not None and not dm.HasField("name") and dm.alias == alias \
        and not dm.HasField("datatype") and datatype_of(dd[1], dm) == 19 \
        and not any(x.HasField("datatype") for x in dm.template_value.metrics) \
        and [(x.name, x.int_value) for x in dm.template_value.metrics] == [("Speed", 1234)]
    ok = def_ok and inst_ok and wrote == [(6, 230, [1234])] and data_ok
    return ok, (f"definition {members} (alias {'yes' if d is not None and d.HasField('alias') else 'no'}); "
                f"instances {inst}; DCMD writes {wrote}; DDATA "
                f"{[(x.name, x.int_value) for x in dm.template_value.metrics] if dm else None}")


def check_devices_gw02(obs, slave, gw, node, t0):
    """#196, #197 and #214 on the GW02 boot (see run_network_variant). Returns [(name, ok, detail)].
    Drive (write_multiple, Speed with write_min 0 and write_max 1500): a DCMD in the limits is one
    FC16 of one register and a DDATA; a DCMD out of the limits writes nothing, gives the warning
    (tck-id-operational-behavior-data-commands-dcmd-metric-value) and the refusal counter. Spare
    (enabled false, on unit 1, which answers): no request at its address, no DBIRTH, the state
    "disabled" in the shell, and a DCMD for it is discarded."""
    out = []
    drive = next((m for m in CAPTURE if m[1] == f"{T}/DBIRTH/{node}/Drive"), None) \
        or obs.wait(f"{T}/DBIRTH/{node}/Drive", 15)
    obs.drain()
    before = len(slave.ctx.writes)
    obs.publish_cmd(f"{T}/DCMD/{node}/Drive", [{"name": "Speed", "datatype": 6, "int_value": 1000}])
    dd = obs.wait(f"{T}/DDATA/{node}/Drive", 5)
    wrote = slave.ctx.writes[before:]
    got = dd[2].metrics[0].int_value if dd and dd[2].metrics else None
    out.append(("#197 write_multiple: a DCMD of one uint16 register is one FC16 (not FC06), and the "
                "DDATA confirms it", drive is not None and wrote == [(16, 250, [1000])] and got == 1000,
                f"DBIRTH {'yes' if drive else 'no'}; writes {wrote}; DDATA {got}"))
    # #202: the field of 3 bits: its value in the DBIRTH (UInt16), and a DCMD changes only its bits
    state = metric(drive[2], name="State") if drive else None
    obs.drain()
    before = len(slave.ctx.writes)
    obs.publish_cmd(f"{T}/DCMD/{node}/Drive", [{"name": "State", "datatype": 6, "int_value": 3}])
    dd = obs.wait(f"{T}/DDATA/{node}/Drive", 5)
    wrote = slave.ctx.writes[before:]
    got = dd[2].metrics[0].int_value if dd and dd[2].metrics else None
    out.append(("#202 a field of 3 bits (bit_offset 4, bit_width 3, uint16): 5 from 0xA0D9 in the "
                "DBIRTH; a DCMD of 3 reads the register and writes 0xA0B9 (the other bits stay), "
                "and the DDATA confirms 3",
                state is not None and state.datatype == 6 and state.int_value == 5
                and wrote == [(16, 252, [0xA0B9])] and got == 3,
                f"DBIRTH State {(state.datatype, state.int_value) if state else None}; writes "
                f"{[(f, a, [hex(x) for x in v]) for f, a, v in wrote]}; DDATA {got}"))
    obs.drain()
    since = time.monotonic()
    before = len(slave.ctx.writes)
    obs.publish_cmd(f"{T}/DCMD/{node}/Drive", [{"name": "Speed", "datatype": 6, "int_value": 2000}])
    refused = gw.wait_log(r"DCMD: metric 'Speed' discarded: value 2000 out of the write limits "
                          r"\[0, 1500\]", 5, since)
    late = obs.wait(f"{T}/DDATA/{node}/Drive", 1.5)
    wrote = slave.ctx.writes[before:]
    _, h = gw.shell("devices", [r"^Drive\s+unit\s+1 online .*\| writes refused 1$",
                                r"^Spare\s+unit\s+1 disabled\s+cycles 0 failed 0 \| ok 0 timeout 0 "])
    counted, listed = h.values()
    out.append(("#196 a DCMD out of the write limits (2000 > write_max 1500) writes nothing, gives a "
                "warning and counts the refusal (shell 'devices')",
                refused is not None and not wrote and late is None and counted is not None
                and slave.get(3, 250)[0] == 1000,
                f"log {(gw.find(r'metric .Speed. discarded', since) or ['none'])[0]}; writes {wrote}; "
                f"DDATA {late is not None}; register {slave.get(3, 250)[0]}; "
                + " | ".join(gw.find(r"^Drive\s+unit", since)[:1])))
    # #196: GET /api/status gives the count, and the Status tab shows it in the State cell of the
    # device ("1 write refused", amber, with an explainer; nothing for 0). No scenario runs a
    # browser: the served page must hold that code. No web password here: the API takes the
    # commissioning password of the console.
    auth = ("admin", commissioning_password(gw, t0, 5) or "?")
    st_s, _, b_s = http("GET", "/api/status", auth=auth)
    refusals = {d.get("name"): d.get("write_refused") for d in (as_json(b_s) or {}).get("devices", [])}
    st_p, _, page = http("GET", "/", auth=auth)
    text = page.decode(errors="replace") if st_p == 200 else ""
    shown = ('${plur(d.write_refused,"write")} refused' in text and "d.write_refused>0?" in text
             and 'data-x="wref"' in text and "wref:\"The number of DCMD writes" in text)
    out.append(("#196 GET /api/status gives write_refused 1 for Drive (0 for the other devices), "
                "and the Status tab shows '1 write refused' in the State cell, with an explainer",
                st_s == 200 and refusals.get("Drive") == 1
                and all(v == 0 for k, v in refusals.items() if k != "Drive") and shown,
                f"/api/status {st_s}: {refusals}; page {st_p}, the code of the State cell "
                f"{'present' if shown else 'missing'}"))
    # #214: Spare is never polled and has no DBIRTH; a DCMD for it is discarded
    since = time.monotonic()
    before = len(slave.ctx.writes)
    obs.publish_cmd(f"{T}/DCMD/{node}/Spare", [{"name": "Level", "datatype": 6, "int_value": 7}])
    dropped = gw.wait_log(r"DCMD for disabled device 'Spare' discarded", 5, since)
    _, h = gw.shell(["status", "config"],
                    [r"^network\s+.*Modbus \d+/9 devices online, 1 disabled$",
                     r"^\s+Spare\s+unit\s+1, poll 200 ms, .*, disabled \(not polled\)$"])
    status, summary = h.values()
    asked = [r for t, r in zip(list(slave.times), list(slave.reads), strict=False)
             if t >= t0 and r[0] == 1 and r[2] == 240]
    born = [m for m in CAPTURE if m[1] == f"{T}/DBIRTH/{node}/Spare"]
    out.append(("#214 a disabled device (enabled false, on unit 1, which answers) gets no request on "
                "the bus and no DBIRTH; the shell gives 'disabled' and does not count it; a DCMD "
                "for it is discarded",
                not asked and not born and listed is not None and status is not None
                and summary is not None and dropped is not None
                and slave.ctx.writes[before:] == [],
                f"requests at holding 240: {asked[:3]}; DBIRTH {len(born)}; "
                + " | ".join(gw.find(r"^Spare\s+unit|^\s+Spare\s+unit|^network\s", t0)[-3:])
                + f"; DCMD log {dropped is not None}"))
    return out


# Issue #172: five location levels before each device name (Meter1: a device_id of 62 characters)
WEB_DEVICE_PREFIX = "acme-corporation:berlin-tempelhof:packaging:line3:cell2"


def check_wifi_scan(gw):
    """Issue #208: GET /api/wifi/scan in the emulator (no radio, network.wifi off): without
    ?new=1 only the state; ?new=1 starts the simulated scan (202 while it runs, the log line),
    then 200 with the list: strongest first, one record for each SSID (the strongest), no hidden
    network, the enterprise network not supported, simulated. A second ?new=1 within 10 s gives
    the same result at once. Without credentials: 401."""
    s0, _, b0 = http("GET", "/api/wifi/scan")
    t = time.monotonic()
    s1, _, _ = http("GET", "/api/wifi/scan?new=1")
    started = gw.wait_log(r"net: wifi scan started \(simulated", 5, t)
    end, s2, j = time.monotonic() + 15, None, {}
    while time.monotonic() < end:
        s2, _, b2 = http("GET", "/api/wifi/scan")
        j = as_json(b2) or {}
        if s2 != 202:
            break
        time.sleep(0.5)
    nets = j.get("networks", [])
    ssids = [n.get("ssid") for n in nets]
    rssi = [n.get("rssi") for n in nets]
    corp = [n for n in nets if n.get("ssid") == "Plant-Corp"]
    s3, _, b3 = http("GET", "/api/wifi/scan?new=1")
    s4, _, _ = http("GET", "/api/wifi/scan", auth=None)
    check("Issue #208 GET /api/wifi/scan (simulated in QEMU): ?new=1 starts a scan (202), then 200 "
          "with the networks strongest first, one record for each SSID, no hidden network, the "
          "enterprise network not supported; a second ?new=1 within 10 s answers at once; 401 "
          "without credentials",
          s0 == 200 and (as_json(b0) or {}).get("state") == "idle" and s1 == 202 and started
          and s2 == 200 and j.get("state") == "done" and j.get("simulated") is True
          and ssids == ["Plant-IoT", "Plant-Office", "Plant-Corp", "Warehouse-AP2", "Guest",
                        "DIRECT-7F-HP M479"]
          and rssi == sorted(rssi, reverse=True) and rssi[0] == -48 and "" not in ssids
          and corp and corp[0].get("supported") is False
          and corp[0].get("security") == "WPA2-Enterprise"
          and all(n.get("supported") for n in nets if n.get("ssid") != "Plant-Corp")
          and s3 == 200 and (as_json(b3) or {}).get("networks") == nets and s4 == 401,
          f"before {s0} {b0[:40]!r}; new {s1}, log {started is not None}; result {s2} "
          f"{j.get('state')} simulated {j.get('simulated')} {list(zip(ssids, rssi, strict=True))}; "
          f"Plant-Corp {corp}; again {s3}; no auth {s4}")


def check_local_writes_off():
    """Issue #216: with modbus_tcp.local_writes false, the local unit answers the reads and
    refuses a write of a DO with exception 01."""
    try:
        di = mbap_raw(b"\x02\x00\x00\x00\x08", unit=100)
        wr = mbap_raw(b"\x05\x00\x00\xff\x00", unit=100)
        mw = mbap_raw(b"\x0f\x00\x00\x00\x02\x01\x03", unit=100)
    except OSError as e:
        di = wr = mw = f"{e}".encode()
    check("Issue #216 local unit without local_writes: FC02 answered, FC05 and FC15 -> exception 01",
          di[:2] == b"\x02\x01" and wr == b"\x85\x01" and mw == b"\x8f\x01",
          f"FC02 {di!r}; FC05 {wr!r}; FC15 {mw!r}")


def run_web(args, work):
    """CFG-4 REST API (and the web UI, CFG-3), in boots of their own with separate edge node ids:
    authentication, export with masked secrets, invalid import -> 400 with the JSON path,
    dry run, oversized body, and a valid import -> orderly restart with the new configuration."""
    node, node2 = "GW04", "GW05"
    cfg = json.loads(config_json())
    cfg["sparkplug"]["edge_node_id"] = node
    cfg["mqtt"]["password"] = "mqtt-secret"      # the broker allows anonymous: ignored there
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    cfg["gateway"] = dict(GATEWAY_LABELS)       # #206
    cfg["modbus_tcp"]["local_writes"] = False   # #216: the local unit refuses the writes
    flash = flash_variant(args, work, "web", {"config.json": json.dumps(cfg, indent=2).encode()})
    obs = Observer()
    gw = Gateway(args, work / "uart0-web.log", flash)
    t0 = time.monotonic()
    gw.start()
    nb = obs.wait(f"{T}/NBIRTH/{node}", 40)
    st0, hdr0, _ = wait_http("/api/config", 20, auth=None)
    st1, _, _ = http("GET", "/api/config", auth=("admin", "wrong-pass"))
    st2, hdr2, body = http("GET", "/api/config")
    doc = as_json(body) or {}
    check("CFG-4 REST API requires HTTP Basic auth (no credentials / wrong password -> 401)",
          st0 == 401 and "basic" in hdr0.get("www-authenticate", "").lower() and st1 == 401,
          f"no auth -> {st0} ({hdr0.get('www-authenticate')}); wrong password -> {st1}")
    masked = doc.get("mqtt", {}).get("password") == "********" and \
        doc.get("web", {}).get("password") == "********"
    check("CFG-4 GET /api/config exports the configuration with every secret masked",
          st2 == 200 and nb and doc.get("sparkplug", {}).get("edge_node_id") == node and masked
          and b"mqtt-secret" not in body and WEB_PASSWORD.encode() not in body
          and "json" in hdr2.get("content-type", ""),
          f"status {st2}, {len(body)} bytes, node {doc.get('sparkplug', {}).get('edge_node_id')}, "
          f"mqtt.password {doc.get('mqtt', {}).get('password')!r}, web.password "
          f"{doc.get('web', {}).get('password')!r}")

    st_p, hdr_p, page = http("GET", "/")
    st_s, _, b_s = http("GET", "/api/status")
    status = as_json(b_s) or {}
    devs = {d.get("name"): d for d in status.get("devices", [])}
    volt = [t for t in devs.get(DEVICE, {}).get("tags", []) if t.get("name") == "Voltage"]
    check("CFG-3 web UI page and live status (/api/status: mode, MQTT, devices with counters and "
          "tag values, interfaces)",
          st_p == 200 and "text/html" in hdr_p.get("content-type", "") and b"/api/status" in page
          and st_s == 200 and status.get("mode") == "normal" and status.get("mqtt", {}).get("connected")
          and devs.get(DEVICE, {}).get("online") and volt and volt[0].get("valid")
          and status.get("network", {}).get("interfaces", [{}])[0].get("active"),
          f"/ {st_p} {len(page)} B; /api/status {st_s}: mode {status.get('mode')}, MQTT "
          f"{status.get('mqtt')}, {DEVICE} online {devs.get(DEVICE, {}).get('online')}, Voltage "
          f"{volt[0] if volt else None}")
    check_wifi_scan(gw)
    check_local_writes_off()

    # Issue #170: the form has the byte limits of the names (maxlength 39 for device and tag names,
    # 32 for the Sparkplug IDs), the help texts of primary_host_id, type and bytes, the topic
    # preview (Sparkplug and UMH) and the device prefix as a list of levels (#172)
    page_text = page.decode(errors="replace")
    ui_items = {"maxlength 39": "ml:39" in page_text, "maxlength 32": "ml:32" in page_text,
                "maxlength attribute": 'maxlength="${x.ml}"' in page_text,
                "primary_host_id help": "UMH cannot be the primary host" in page_text,
                "type help": "shows no value for a template instance" in page_text,
                "bytes help": "shows no value for Bytes" in page_text,
                "topic preview": "umh.v1." in page_text and "spBv1.0/" in page_text,
                "prefix levels": "data-add-lvl" in page_text}
    check("CFG-3 issue #170: the configuration form has maxlength on the names and IDs, help texts "
          "(primary_host_id, type, bytes), the Sparkplug and UMH topic preview and the device "
          "prefix levels",
          st_p == 200 and all(ui_items.values()),
          f"missing: {[k for k, v in ui_items.items() if not v] or 'none'}")

    # Issues #192 and #193: the confirmations are one <dialog> of the page (no prompt() or
    # confirm()); a refused upload shows a box that installs the same file into recovery mode;
    # /api/status ap.request_via_ap is false for a request over the Ethernet (true needs the radio
    # of the board, #130; the emulator-only 'claimsim ... eth' case is in the reset scenario)
    ap_s = status.get("ap") or {}
    dlg_items = {"no prompt()": "prompt(" not in page_text, "no confirm()": "confirm(" not in page_text,
                 "dialog": '<dialog id="dlg"' in page_text and ".showModal()" in page_text,
                 "refusal box": 'id="fw-ref"' in page_text and "Install into recovery mode" in page_text
                 and '"?force=1"' in page_text,
                 "request_via_ap false": ap_s.get("request_via_ap") is False}
    check("Issues #192 #193: the web UI confirms in a <dialog> of the page (no prompt() or "
          "confirm()), has the box of a refused upload with 'Install into recovery mode', and "
          "/api/status ap.request_via_ap is false over the Ethernet",
          st_p == 200 and st_s == 200 and all(dlg_items.values()),
          f"missing: {[k for k, v in dlg_items.items() if not v] or 'none'}; ap {ap_s}")

    check_admin_web(gw, nb, status, b_s, doc, page_text, t0)

    # Issue #194: a new web password needs the same value in a second field, and a save that
    # removes the web password asks first. The behaviour in a browser is checked by the shots
    # web-password-* of tools/docs_shots.py (no scenario runs a browser); here the served page
    # must hold that code.
    pw_items = {"second field": 'id="i-pw2"' in page_text and "Confirm the password" in page_text,
                "mismatch refused": "The two passwords are not the same" in page_text
                and "if(!same194())return;" in page_text,
                "removal dialog": "Remove the web password?" in page_text
                and "if(!dry&&!await keep194(b))return;" in page_text}
    check("Issue #194: the web UI asks for a new web password two times, and it asks before a "
          "save that removes the web password",
          st_p == 200 and all(pw_items.values()),
          f"missing: {[k for k, v in pw_items.items() if not v] or 'none'}")
    check_web_controls_221(doc, page_text)
    check_ip4_norm_226(doc, page_text)

    # Issue #167: a <select> without the current value shows its first option, and a save writes
    # it back (a CDAB tag became "big"). The served page's lists must hold every value of the
    # configuration (float64, CDAB, DCBA, BADC tags here): then a save with no change sends the
    # exported document unchanged (the form keeps an absent optional key absent), and it is valid.
    text = page.decode(errors="replace")
    lists = {}
    for name in ("PAR", "AREA", "TYPE", "WO"):
        m = re.search(r"\b%s=\[([^\]]*)\]" % name, text)
        lists[name] = re.findall(r'"([^"]*)"', m.group(1)) if m else []
    used = [("PAR", doc.get("rs485", {}).get("parity", "none"))]
    for d in doc.get("devices", []):
        for t in d.get("tags", []):
            used += [("AREA", t.get("area")), ("TYPE", t.get("type"))]
            if "word_order" in t:
                used.append(("WO", t["word_order"]))
    missing = sorted({f"{k}:{v}" for k, v in used if v not in lists[k]})
    st_rt, _, b_rt = http("PUT", "/api/config?dry_run=1", body)   # the exported bytes, unchanged
    check("CFG-3 issue #167: the form's lists (PAR, AREA, TYPE, WO) hold every value of the "
          "configuration (float64, CDAB, DCBA, BADC), and the unchanged exported document is valid",
          st_p == 200 and not missing and ("TYPE", "float64") in used and ("WO", "CDAB") in used
          and st_rt == 200 and (as_json(b_rt) or {}).get("saved") is False,
          f"lists {[f'{k}={len(v)}' for k, v in lists.items()]}; not in the lists: "
          f"{missing or 'none'}; PUT dry run {st_rt} {b_rt.decode(errors='replace').strip()}")

    bad = json.loads(json.dumps(doc))
    bad["devices"][0]["tags"][0]["type"] = "float16"   # invalid (float64 is a type since MB-6)
    st3, _, b3 = http("PUT", "/api/config", bad)
    e3 = as_json(b3) or {}
    syntax = json.dumps(doc, indent=2).replace('"ntp"', '"ntp" "x"', 1).encode()
    st4, _, b4 = http("PUT", "/api/config", syntax)
    e4 = as_json(b4) or {}
    st5, _, b5 = http("PUT", "/api/config?dry_run=1", doc)
    big = b"{" + b" " * (sdkconfig_int(args.sdkconfig, "GW_CONFIG_MAX_BYTES") + 1) + b"}"
    st6, _, _ = http("PUT", "/api/config", big)
    check("CFG-4 PUT invalid -> 400 with the JSON path; syntax error -> 400 with line and column; "
          "dry run validates without saving; oversized body -> 413",
          st3 == 400 and e3.get("path") == "devices[0].tags[0].type" and st4 == 400
          and e4.get("line", 0) > 0 and e4.get("path") == "" and st5 == 200
          and (as_json(b5) or {}).get("saved") is False and st6 == 413
          and not gw.find(r"configuration saved", t0),
          f"schema {st3} {e3.get('path')!r}: {e3.get('error')}; syntax {st4} line {e4.get('line')} "
          f"column {e4.get('column')}; dry run {st5} {b5.decode().strip()}; oversized {st6}")

    # Issue #169: a name that UMH Core changes (a space in a level): a warning with its JSON path
    # in the dry run; the UMH preset (sparkplug.naming "umh") refuses the same name
    lint = json.loads(json.dumps(doc))
    lint["devices"][0]["name"] = "line 3:" + DEVICE
    st_w, _, b_w = http("PUT", "/api/config?dry_run=1", lint)
    j_w = as_json(b_w) or {}
    lint["sparkplug"]["naming"] = "umh"
    st_u, _, b_u = http("PUT", "/api/config?dry_run=1", lint)
    j_u = as_json(b_u) or {}
    warns = j_w.get("warnings") or []
    check("CFG issue #169: the dry run gives the name warnings for UNS hosts with their JSON paths "
          "(valid, not saved); with sparkplug.naming \"umh\" the same name is an error (400 with "
          "the path)",
          st_w == 200 and j_w.get("saved") is False and j_w.get("warning_count", 0) >= 1
          and any(w.get("path") == "devices[0].name" and "space" in w.get("message", "")
                  for w in warns)
          and st_u == 400 and j_u.get("path") == "devices[0].name",
          f"dry run {st_w}: {warns[:3]} (count {j_w.get('warning_count')}); UMH preset {st_u} "
          f"{j_u.get('path')}: {j_u.get('error')}")

    new = json.loads(json.dumps(doc))
    new["sparkplug"]["edge_node_id"] = node2
    # Issue #172: a device prefix of five levels; the device_id of Meter1 has 62 characters
    new["sparkplug"]["device_prefix"] = pfx = WEB_DEVICE_PREFIX
    obs.drain()
    st7, _, b7 = http("PUT", "/api/config", new)
    death = obs.wait(f"{T}/NDEATH/{node}", 10)
    nb2 = obs.wait(f"{T}/NBIRTH/{node2}", 40)
    db_long = obs.wait(f"{T}/DBIRTH/{node2}/{pfx}:{DEVICE}", 20)
    db_bulk = obs.wait(f"{T}/DBIRTH/{node2}/{pfx}:{BULK}", 10)
    st8, _, b8 = wait_http("/api/config", 20)
    doc8 = as_json(b8) or {}
    check("CFG issue #172: sparkplug.device_prefix of five levels: the DBIRTHs use the device_id "
          f"'<prefix>:<name>' ({len(pfx) + 1 + len(DEVICE)} characters for {DEVICE})",
          db_long and db_bulk and len(pfx) + 1 + len(DEVICE) >= 60
          and doc8.get("sparkplug", {}).get("device_prefix") == pfx
          and not [m for m in CAPTURE if m[1] == f"{T}/DBIRTH/{node2}/{DEVICE}"],
          f"DBIRTH {pfx}:{DEVICE} {bool(db_long)}, {pfx}:{BULK} {bool(db_bulk)}; GET device_prefix "
          f"{doc8.get('sparkplug', {}).get('device_prefix')!r}")
    check("CFG-4 PUT valid -> saved atomically, orderly restart (NDEATH) and NBIRTH with the new "
          "configuration; secrets kept through the mask",
          st7 == 200 and (as_json(b7) or {}).get("saved") and death and nb2 and st8 == 200
          and doc8.get("sparkplug", {}).get("edge_node_id") == node2
          and doc8.get("mqtt", {}).get("password") == "********"
          and gw.find(r"configuration saved to /cfg/config\.json", t0),
          f"PUT {st7} {b7.decode().strip()}; NDEATH {node}: {bool(death)}; NBIRTH {node2}: "
          f"{bool(nb2)}; GET after restart {st8} node {doc8.get('sparkplug', {}).get('edge_node_id')}"
          f" (same password still accepted)")
    # CFG-6 shell: configuration and active diagnostics, on the same gateway (now GW05)
    asked = time.monotonic()
    gw.console("config show")
    shown = gw.wait_log(r'^\s*"edge_node_id": "%s"' % node2, 5, asked)
    end = time.monotonic() + 10   # about 10 KB at 115200 baud
    while len(gw.find(r'^\s*"password": "\*{8}"', asked)) < 2 and time.monotonic() < end:
        time.sleep(0.2)
    masked_sh = gw.find(r'^\s*"password": "\*{8}"', asked)
    leaked = gw.find(r"mqtt-secret|" + WEB_PASSWORD, asked)
    asked, h = gw.shell(["mbread 1 holding 10 1", "mbread 1 input 0 2", "mbread 9 holding 0"],
                        [r"^mbread: unit 1 holding 10: 50$",
                         r"^mbread: unit 1 input 0: 17254 32768$",           # 230.5 as float32 ABCD
                         r"^mbread: unit 9 holding 0: (timeout|framing error)$"], timeout=15)
    reg, flt, absent = h.values()
    check("CFG-6 shell 'config show' (secrets masked) and 'mbread' through the bus owner (register, "
          "float32 words, unit without a slave -> error, not a value)",
          shown and len(masked_sh) >= 2 and not leaked and reg and flt and absent,
          f"show: node {bool(shown)}, {len(masked_sh)} masked secrets, leaks {len(leaked)}; "
          + " | ".join(gw.find(r"^mbread: ", asked)))
    # Issue #182: the same test read through the REST API (the bus owner runs it, MB-16)
    def mb_read(req, **kw):
        st, _, b = http("POST", "/api/modbus/read", req, timeout=20, **kw)
        return st, as_json(b) or {"raw": b.decode(errors="replace")[:120]}
    r_reg = mb_read({"unit": 1, "area": "holding", "address": 10})
    r_flt = mb_read({"unit": 1, "area": "input", "address": 0, "type": "float32"})
    # issue #188: Energy (UInt64 ABCD, holding 30) = 2^53 + 1, also as text
    r_u64 = mb_read({"unit": 1, "area": "holding", "address": 30, "type": "uint64"})
    r_dead = mb_read({"unit": 3, "area": "holding", "address": 0})
    r_bad = mb_read({"unit": 1, "area": "holding", "address": 0, "count": 17})
    r_csrf = mb_read({"unit": 1, "area": "holding", "address": 10}, headers={"X-Requested-With": None})
    check("CFG-6 issue #182: POST /api/modbus/read through the bus owner (register, float32 value, "
          "UInt64 value and its text (#188), unit 3 without a slave -> no value, count 17 -> 400, "
          "no CSRF header -> 403)",
          r_reg[0] == 200 and r_reg[1].get("values") == [50]
          and r_flt[0] == 200 and r_flt[1].get("values") == [17254, 32768] and r_flt[1].get("value") == 230.5
          and "value_text" not in r_flt[1]
          and r_u64[0] == 200 and r_u64[1].get("value") == MB6["Energy"]
          and r_u64[1].get("value_text") == str(MB6["Energy"])
          # as the shell check of unit 9: no answer, or noise taken for a frame in the emulator
          and (r_dead[0], r_dead[1].get("result")) in ((504, "timeout"), (502, "frame"))
          and "value" not in r_dead[1] and r_bad[0] == 400 and "1-16" in r_bad[1].get("error", "")
          and r_csrf[0] == 403,
          " | ".join(f"{st} {j}" for st, j in (r_reg, r_flt, r_u64, r_dead, r_bad, r_csrf)))
    obs.drain()
    asked = time.monotonic()
    gw.console("rebirth")
    rb = obs.wait(f"{T}/NBIRTH/{node2}", 5)
    said = gw.wait_log(r"^rebirth: NBIRTH and every DBIRTH requested", 3, asked)
    obs.wait(f"{T}/DBIRTH/{node2}/{pfx}:{BULK}", 5)
    dbs = sorted(m[2].seq for m in CAPTURE if m[0] >= asked and m[1].startswith(f"{T}/DBIRTH/{node2}/"))
    check("CFG-6 shell 'rebirth' re-issues NBIRTH (seq 0, same session) and the DBIRTHs",
          rb and rb[2].seq == 0 and dbs == [1, 2] and said,
          f"NBIRTH {'seq %d' % rb[2].seq if rb else 'missing'}, DBIRTH seq {dbs}, shell answer "
          f"{bool(said)}")
    # Issue #171: the same through the REST API. Without the CSRF header: 403 and no rebirth.
    obs.drain()
    t_nc = time.monotonic()
    st_nc, _, b_nc = http("POST", "/api/rebirth", headers={"X-Requested-With": None})
    time.sleep(2)
    forged = [m for m in CAPTURE if m[0] >= t_nc and m[1] == f"{T}/NBIRTH/{node2}"]
    asked = time.monotonic()
    st_rb, _, b_rb = http("POST", "/api/rebirth")
    rb2 = obs.wait(f"{T}/NBIRTH/{node2}", 5)
    obs.wait(f"{T}/DBIRTH/{node2}/{pfx}:{BULK}", 5)
    dbs2 = sorted(m[2].seq for m in CAPTURE if m[0] >= asked and m[1].startswith(f"{T}/DBIRTH/{node2}/"))
    bd0, bd2 = (bdseq_of(nb2[2]) if nb2 else None), (bdseq_of(rb2[2]) if rb2 else None)
    check("CFG-4 issue #171: POST /api/rebirth re-issues NBIRTH (seq 0, the same bdSeq) and the "
          "DBIRTHs; without the CSRF header -> 403 and no rebirth",
          st_nc == 403 and not forged and st_rb == 202 and (as_json(b_rb) or {}).get("ok")
          and rb2 and rb2[2].seq == 0 and bd2 is not None and bd2 == bd0 and dbs2 == [1, 2],
          f"no header {st_nc} {b_nc.decode(errors='replace').strip()}, {len(forged)} NBIRTH after it; "
          f"POST {st_rb} {b_rb.decode(errors='replace').strip()}; NBIRTH "
          f"{'seq %d' % rb2[2].seq if rb2 else 'missing'}, bdSeq {bd2} (session NBIRTH {bd0}), "
          f"DBIRTH seq {dbs2}")

    def paste(doc_obj):
        gw.console("config load" if doc_obj.get("_save", True) else "config check")
        time.sleep(0.5)
        text = json.dumps({k: v for k, v in doc_obj.items() if k != "_save"}, indent=2)
        gw.sock.sendall(text.encode() + b"\n")

    bad = json.loads(json.dumps(doc8))
    bad["devices"][0]["tags"][0]["type"] = "float16"   # invalid (float64 is a type since MB-6)
    asked = time.monotonic()
    paste(bad)
    rej = gw.wait_log(r"^config: rejected: devices\[0\]\.tags\[0\]\.type: invalid value", 15, asked)
    rpath = gw.wait_log(r"^config: path devices\[0\]\.tags\[0\]\.type$", 2, asked)
    good = json.loads(json.dumps(doc8))
    good["_save"] = False
    asked2 = time.monotonic()
    paste(good)
    valid = gw.wait_log(r"^config: valid \(\d+ bytes\), nothing saved$", 15, asked2)
    still = gw.find(r"configuration saved|restart", asked)
    good = json.loads(json.dumps(doc8))
    good["sparkplug"]["edge_node_id"] = node3 = "GW06"
    obs.drain()
    asked3 = time.monotonic()
    paste(good)
    saved = gw.wait_log(r"^config: saved \(\d+ bytes\); restarting$", 15, asked3)
    death = obs.wait(f"{T}/NDEATH/{node2}", 10)
    nb3 = obs.wait(f"{T}/NBIRTH/{node3}", 40)
    st9, _, b9 = wait_http("/api/config", 20)
    check("CFG-6 shell 'config load': invalid document rejected with its JSON path; 'config check' "
          "validates only; a valid document is saved atomically and the gateway restarts with it "
          "(NDEATH, NBIRTH, secrets kept)",
          rej and rpath and valid and not still and saved and death and nb3 and st9 == 200
          and (as_json(b9) or {}).get("mqtt", {}).get("password") == "********",
          " | ".join(gw.find(r"^config: ", asked)) + f"; NDEATH {node2}: {bool(death)}; NBIRTH "
          f"{node3}: {bool(nb3)}; GET {st9}")
    # issue #261: the board build does not encrypt the NVS (no build for the board does, #133);
    # the secure build does
    _, h = gw.shell("status", [r"^secrets\s+NVS encrypted \(HMAC key in eFuse KEY5\)" if args.secure
                               else r"^secrets\s+NVS NOT encrypted \(default build\)"])
    nvs_line = list(h.values())[0]
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node3}", 5)
    obs.close()
    # CFG-7: the secrets left the file at the first boot, and the web password is only stored as
    # a hash. In the secure build the NVS is encrypted: no secret appears anywhere in the flash
    # image in clear. In the board build the NVS is not encrypted: the MQTT password is in clear
    # in the NVS partition, and only there.
    image = pathlib.Path(flash).read_bytes()
    so, ss = storage_partition()
    no, ns = storage_partition("nvs")
    leaks = [(what.decode(), where) for what in (b"mqtt-secret", WEB_PASSWORD.encode())
             for where, (o, n) in (("storage", (so, ss)), ("nvs", (no, ns)), ("image", (0, len(image))))
             if what in image[o:o + n]]
    moved = gw.find(r"storage: secrets moved from /cfg/config\.json to NVS", t0)
    encrypted = gw.find(r'nvs: NVS partition "nvs" is encrypted', t0)
    if args.secure:
        check("CFG-7 secrets migrated out of config.json at the first boot (partition erased: no "
              "stale copy), NVS encrypted with the HMAC key in eFuse, web password only as a "
              "hash: no secret in clear anywhere in the flash image",
              moved and encrypted and nvs_line and not leaks,
              f"{(moved or ['no migration log'])[0]}; {(encrypted or ['NVS not encrypted'])[0]}; "
              f"shell {bool(nvs_line)}; secrets found in clear: {leaks or 'none'}")
    else:
        mqtt_at = sorted(where for what, where in leaks if what == "mqtt-secret")
        web_at = sorted(where for what, where in leaks if what == WEB_PASSWORD)
        outside = image[:no] + image[no + ns:]
        check("CFG-7 (board build: the NVS is not encrypted, #133) secrets migrated out of "
              "config.json at the first boot (partition erased: no stale copy): the MQTT "
              "password is in the NVS partition only, the web password is only a hash (in clear "
              "nowhere in the flash image), and the shell shows the NVS as not encrypted",
              moved and not encrypted and nvs_line and mqtt_at == ["image", "nvs"]
              and b"mqtt-secret" not in outside and not web_at,
              f"{(moved or ['no migration log'])[0]}; NVS encryption line {bool(encrypted)}; "
              f"shell {bool(nvs_line)}; MQTT password in clear in {mqtt_at or 'none'} (outside "
              f"the NVS partition: {b'mqtt-secret' in outside}); web password in clear in "
              f"{web_at or 'none'}")


# #206: the name, the location and the comment of the web scenario's gateway
GATEWAY_LABELS = {"name": "Pump panel 2", "location": "Plant 2, hall B",
                  "comment": "Feeds the cooling towers 1 and 2. Contact: maintenance, ext. 214."}


def check_ip4_norm_226(doc, page_text):
    """Issue #226: the firmware rejects an IPv4 number with a leading zero (other programs read
    010 as the octal 8), with the error at its field. The web UI removes the leading zeros and the
    spaces when an IPv4 field loses the focus (decision of the product owner). No scenario runs a
    browser, and idf/test/web/test_ip4norm.mjs tests the functions in Node.js: here the served
    page must hold the code, and the validation must refuse the typed forms and accept the forms
    that the page writes."""
    ui = {"ip4Norm": "function ip4Norm(v,net)" in page_text,
          "ip4Kind": "function ip4Kind(p)" in page_text,
          "focusout": '$("#tab-config").addEventListener("focusout",ip4Blur);' in page_text}

    def dry(dns, clients):
        d = json.loads(json.dumps(doc))
        d.setdefault("network", {})["dns"] = dns
        d.setdefault("modbus_tcp", {})["allowed_clients"] = clients
        st, _, b = http("PUT", "/api/config?dry_run=1", d)
        return st, (as_json(b) or {}).get("path")
    typed = [dry("192.168.001.010", ["10.0.0.1/8"]), dry("192.168.1.10", ["010.000.000.001/08"])]
    norm = dry("192.168.1.10", ["10.0.0.1/8"])
    check("Issue #226: the web UI removes the leading zeros of an IPv4 value when its field loses "
          "the focus; the validation refuses 192.168.001.010 and 010.000.000.001/08 at their "
          "fields, and accepts 192.168.1.10 and 10.0.0.1/8",
          all(ui.values()) and typed == [(400, "network.dns"), (400, "modbus_tcp.allowed_clients[0]")]
          and norm[0] == 200,
          f"missing: {[k for k, v in ui.items() if not v] or 'none'}; typed {typed}; "
          f"normalized {norm}")


def check_web_controls_221(doc, page_text):
    """Issues #221 and #195 on the web scenario's gateway: the last keys of the configuration
    document get a control (sparkplug.diagnostics_s, sparkplug.store_forward, ota.ca_file), and
    Restart is only in the card Restart and reset. No scenario runs a browser (the browser check
    is a shot of tools/docs_shots.py): the served page must hold the code. The validation gives
    the errors of store_forward at the paths that the controls name (data-perr, #191), and accepts
    the forms that the page writes (true or false with the default spill keys, else an object)."""
    ui = {"diagnostics_s field": '"sparkplug.diagnostics_s","Diagnostics period (s)"' in page_text,
          "store & forward section": 'id="f-sf"' in page_text and "readSf();" in page_text
          and "Keep the changes without a session" in page_text
          and "Spill to the microSD card" in page_text and "Space on the card (KB)" in page_text,
          "store_forward paths": all(f'data-perr="sparkplug.store_forward{p}"' in page_text
                                     for p in ("", ".enabled", ".sd.enabled", ".sd.max_kb")),
          "GPIO45 caution": "pull-up on D0 (GPIO45)" in page_text,
          "ota.ca_file field": 'fileFld("ota.ca_file","CA file of the image server"' in page_text
          and 'id="ota-msg"' in page_text,
          "no Restart in the bottom card": 'id="b-restart"' not in page_text
          and 'id="rr-restart"' in page_text}

    def dry(sf, diag=None):
        d = json.loads(json.dumps(doc))
        d["sparkplug"]["store_forward"] = sf
        if diag is not None:
            d["sparkplug"]["diagnostics_s"] = diag
        st, _, b = http("PUT", "/api/config?dry_run=1", d)
        return st, (as_json(b) or {}).get("path")
    forms = [dry(True, 0), dry(False, 86400),
             dry({"enabled": True, "sd": {"enabled": True, "max_kb": 262144}}),
             dry({"enabled": False, "sd": {"enabled": False, "max_kb": 64}})]
    errs = [dry({"enabled": False, "sd": {"enabled": True, "max_kb": 131072}}),
            dry({"enabled": True, "sd": {"enabled": True, "max_kb": 32}}), dry(False, 5)]
    check("Issues #221 #195: the Configuration tab has controls for sparkplug.diagnostics_s, "
          "sparkplug.store_forward (with the GPIO45 caution) and ota.ca_file (a file field with "
          "'Load a file…'), and Restart is only in the card Restart and reset; the documents that "
          "the controls write are valid, and the errors have the paths of the controls",
          all(ui.values()) and [s for s, _ in forms] == [200] * 4
          and errs == [(400, "sparkplug.store_forward.sd.enabled"),
                       (400, "sparkplug.store_forward.sd.max_kb"),
                       (400, "sparkplug.diagnostics_s")],
          f"missing: {[k for k, v in ui.items() if not v] or 'none'}; valid forms {forms}; "
          f"errors {errs}")

    # #191 #213: an error of the list itself (17 device types, the maximum is 16) has the path
    # "device_types": the card Device types has a field for it, as the card of the devices for
    # "devices"
    d = json.loads(json.dumps(doc))
    d["device_types"] = [{"name": f"T{i}", "tags": [{"name": "v", "area": "holding",
                                                     "address": 0, "type": "uint16"}]}
                         for i in range(17)]
    st, _, b = http("PUT", "/api/config?dry_run=1", d)
    path = (as_json(b) or {}).get("path")
    lists = 'id="f-dt" data-perr="device_types"' in page_text and \
        'id="f-dev" data-perr="devices"' in page_text
    check("Issues #191 #213: 17 device types are refused at the path device_types, and the "
          "Configuration tab shows the error of that path, and of devices, at its card",
          st == 400 and path == "device_types" and lists,
          f"dry run {st} {path}; the fields of the lists {'present' if lists else 'missing'}")


def check_admin_web(gw, nb, status, status_body, doc, page_text, t0):
    """The Status tab and the administration of #195 on the web scenario's gateway (GW04, normal
    mode): the name and the location (#206), the health values (#212), Identify (#207), the live
    log (#200) and the log level of each tag (#199). The factory reset (#203) and safe mode (#209)
    restart the gateway: they are in the reset scenario (check_restart_admin)."""
    # #206: the name and the location in /api/status and in the NBIRTH (Node Info/Name and
    # Node Info/Location, String, aliases 8 and 9); the comment only in the document
    gl = status.get("gateway") or {}
    nb_p = nb[2] if nb else None
    m_name = metric(nb_p, name="Node Info/Name") if nb_p else None
    m_loc = metric(nb_p, name="Node Info/Location") if nb_p else None
    check("Issue #206: /api/status gives gateway.name and gateway.location (not the comment); the "
          "NBIRTH has Node Info/Name and Node Info/Location (String, aliases 8 and 9; "
          "tck-id-topics-nbirth-metrics); GET /api/config keeps the comment; the Configuration "
          "tab has the card Gateway and the page sets the title of the browser tab",
          gl.get("name") == GATEWAY_LABELS["name"] and gl.get("location") == GATEWAY_LABELS["location"]
          and "comment" not in gl and b"cooling towers" not in status_body
          and m_name is not None and m_name.string_value == GATEWAY_LABELS["name"]
          and m_name.alias == 8 and m_name.datatype == 12
          and m_loc is not None and m_loc.string_value == GATEWAY_LABELS["location"]
          and m_loc.alias == 9 and m_loc.datatype == 12
          and (doc.get("gateway") or {}).get("comment") == GATEWAY_LABELS["comment"]
          and 'id="f-gw"' in page_text and '"gateway.comment"' in page_text
          and "document.title=" in page_text,
          f"status gateway {gl}; NBIRTH name {m_name.string_value if m_name else None!r} "
          f"(alias {m_name.alias if m_name else None}), location "
          f"{m_loc.string_value if m_loc else None!r}; config comment "
          f"{(doc.get('gateway') or {}).get('comment')!r}")

    # #212: the largest free block of the internal RAM, the free PSRAM and the temperature (QEMU
    # has no sensor: null) in /api/status and in the NBIRTH (UInt64 58, UInt64 59, Float 68)
    heap = status.get("heap") or {}
    m_blk = metric(nb_p, name="Node Info/Largest Free Block") if nb_p else None
    m_psr = metric(nb_p, name="Node Info/Free PSRAM") if nb_p else None
    m_tmp = metric(nb_p, name="Node Info/Temperature") if nb_p else None
    psram = ARGS.psram
    check("Issue #212: /api/status gives heap.internal_largest and temperature_c (null: QEMU has no "
          "sensor); the NBIRTH has Node Info/Largest Free Block (UInt64, alias 58), Node Info/Free "
          "PSRAM (UInt64, alias 59) and Node Info/Temperature (Float, alias 68, null)",
          0 < heap.get("internal_largest", 0) <= heap.get("internal_free", 0)
          and "temperature_c" in status and status["temperature_c"] is None
          and m_blk is not None and m_blk.alias == 58 and m_blk.datatype == 8 and m_blk.long_value > 0
          and m_psr is not None and m_psr.alias == 59 and m_psr.datatype == 8
          and (m_psr.long_value > 1000000 if psram else True)
          and m_tmp is not None and m_tmp.alias == 68 and m_tmp.datatype == 9 and m_tmp.is_null,
          f"heap {heap}; temperature_c {status.get('temperature_c', 'absent')}; NBIRTH block "
          f"{m_blk.long_value if m_blk else None}, PSRAM {m_psr.long_value if m_psr else None}, "
          f"temperature null {m_tmp.is_null if m_tmp else None}")

    # #207: Identify. No CSRF header: 403 and nothing starts. Then 202 with 10 s, the LED pattern
    # "identify" and the beeps (logged by the QEMU build), the time left in /api/status, a second
    # request 409 (the time does not start again), the end after 10 s; then the shell command
    t_id = time.monotonic()
    st_x, _, _ = http("POST", "/api/identify", headers={"X-Requested-With": None})
    forged = gw.find(r"web: identify requested", t_id)
    st_i, _, b_i = http("POST", "/api/identify")
    j_i = as_json(b_i) or {}
    led = gw.wait_log(r"led: status: identify$", 3, t_id)
    beeps = gw.wait_log(r"buzzer: identify: short beeps for 10 s \(simulated\)", 3, t_id)
    st_s2, _, b_s2 = http("GET", "/api/status")
    left = (as_json(b_s2) or {}).get("identify_left_s", 0)
    st_i2, _, b_i2 = http("POST", "/api/identify")
    j_i2 = as_json(b_i2) or {}
    over = gw.wait_log(r"buzzer: identify over \(simulated\)", 15, t_id)
    # the LED task and the buzzer task end the identify independently: the LED can go back a few
    # ms before the buzzer logs "over", so the LED line is searched from the start of the pattern
    back = gw.wait_log(r"led: status: (?!identify)", 3, led) if over and led else None
    asked = time.monotonic()
    gw.console("identify")
    sh1 = gw.wait_log(r"^identify: the LED shows red, green and blue in turn and the buzzer beeps "
                      r"for 10 s$", 5, asked)
    gw.console("identify")
    sh2 = gw.wait_log(r"^identify: already running, \d+ s left$", 5, asked)
    check("Issue #207: POST /api/identify -> 202 (10 s): the LED pattern 'identify' and the beeps; "
          "/api/status identify_left_s; a second request -> 409 while it runs; the end after 10 s; "
          "no CSRF header -> 403 and nothing; the shell 'identify' (and 'already running')",
          st_x == 403 and not forged and st_i == 202 and j_i.get("identify_left_s") == 10
          and led and beeps and 1 <= left <= 10 and st_i2 == 409 and j_i2.get("identify_left_s", 0) > 0
          and over and 9.0 <= over - led <= 12.5 and back and sh1 and sh2
          and "/api/identify" in page_text and 'id="ident"' in page_text,
          f"no header {st_x}; POST {st_i} {j_i}; LED {bool(led)}, beeps {bool(beeps)}, left {left}; "
          f"second {st_i2} {j_i2}; over after {(over - led) if over and led else None} s, LED "
          f"{(gw.find(r'led: status: ', over or t_id) or ['?'])[0] if over else None}; shell "
          f"{bool(sh1)} {bool(sh2)}")

    # #200: GET /api/log has the lines of the console (the PSRAM ring of 16 KB); since=<last>
    # gives only the newer lines; no credentials -> 401
    st_l, _, b_l = http("GET", "/api/log")
    j_l = as_json(b_l) or {}
    lines = j_l.get("lines") or []
    in_log = [x for x in lines if re.search(r"web: identify requested through the REST API", x)]
    in_console = gw.find(r"web: identify requested through the REST API", t_id)
    last = j_l.get("last", 0)
    st_l2, _, b_l2 = http("GET", f"/api/log?since={last}")
    j_l2 = as_json(b_l2) or {}
    st_l3, _, _ = http("GET", "/api/log", auth=None)
    ring = sdkconfig_int(ARGS.sdkconfig, "GW_LOG_RING_KB") * 1024
    check("Issue #200: GET /api/log gives the log lines that the console shows (identify through "
          "the REST API), with their numbers; ?since=<last> gives only the newer lines; no "
          "credentials -> 401",
          st_l == 200 and in_log and in_console
          and in_log[0].strip() == re.sub(r"\x1b\[[0-9;]*m", "", in_console[0]).strip()
          and j_l.get("size") == ring and j_l.get("count", 0) > 10 and last >= j_l.get("count", 0)
          and j_l.get("lost") == 0 and st_l2 == 200 and j_l2.get("since") == last
          and not [x for x in j_l2.get("lines") or [] if x in in_log] and st_l3 == 401,
          f"GET {st_l}: {len(lines)} lines, count {j_l.get('count')}, last {last}, size "
          f"{j_l.get('size')} (expected {ring}), in the log {in_log[:1]}, on the console "
          f"{in_console[:1]}; since {st_l2} {len(j_l2.get('lines') or [])} lines; no auth {st_l3}")

    # #199: the log level of a tag at run time. The tag spb has debug lines (each publish): none
    # before; POST /api/log/level debug -> they show on the console and in GET /api/log; the shell
    # 'log poll debug' (the offline probes of Dead3..5); /api/status log_levels; 'log reset'
    lv = as_json(http("GET", "/api/log/level")[2]) or {}
    before = gw.find(r"^D \(\d+\) spb: ", t0)
    t_lv = time.monotonic()
    st_bad, _, b_bad = http("POST", "/api/log/level", {"tag": "spb", "level": "trace"})
    st_set, _, b_set = http("POST", "/api/log/level", {"tag": "spb", "level": "debug"})
    dbg = gw.wait_log(r"^D \(\d+\) spb: spBv1\.0/", 25, t_lv)
    asked = time.monotonic()
    gw.console("log poll debug")
    sh_set = gw.wait_log(r"^log: poll: debug \(not saved\)$", 5, asked)
    dbg_poll = gw.wait_log(r"^D \(\d+\) poll: Dead\d offline: next probe", 25, asked)
    st_lv = as_json(http("GET", "/api/status")[2]) or {}
    j_dbg = as_json(http("GET", f"/api/log?since={last}")[2]) or {}
    dbg_web = [x for x in j_dbg.get("lines") or [] if re.match(r"D \(\d+\) spb: spBv1\.0/", x)]
    _, h_lv = gw.shell("log", [r"^log\s+level of the build info \(maximum verbose\); changed "
                               r"levels \(not saved\):(?=.* spb=debug)(?=.* poll=debug)",
                               r"^log\s+tags: .*\bweb\b"])
    asked = time.monotonic()
    gw.console("log reset")
    sh_reset = gw.wait_log(r"^log: every tag has the level of the build again \(info\)$", 5, asked)
    t_off = time.monotonic()
    time.sleep(3)
    after = gw.find(r"^D \(\d+\) (spb|poll): ", t_off + 1)
    st_rs = as_json(http("GET", "/api/status")[2]) or {}
    check("Issue #199: the log level of a tag at run time, not saved: GET /api/log/level (levels "
          "none to verbose, tags of the firmware); POST {tag spb, level debug} -> the debug lines "
          "of spb on the console and in GET /api/log; an unknown level -> 400; the shell 'log poll "
          "debug'; /api/status log_levels; 'log reset' ends both",
          lv.get("build") == "info" and lv.get("max") == "verbose"
          and lv.get("levels") == ["none", "error", "warn", "info", "debug", "verbose"]
          and {"web", "edge", "spb", "poll", "mb"} <= set(lv.get("tags") or []) and not before
          and st_bad == 400 and b"level: one of" in b_bad and st_set == 200 and dbg and dbg_web
          and sh_set and dbg_poll
          and {"tag": "spb", "level": "debug"} in (st_lv.get("log_levels") or [])
          and {"tag": "poll", "level": "debug"} in (st_lv.get("log_levels") or [])
          and all(h_lv.values()) and sh_reset and not after and st_rs.get("log_levels") == [],
          f"GET level {lv.get('build')}/{lv.get('max')} {lv.get('levels')} {len(lv.get('tags') or [])} "
          f"tags; debug lines before {len(before)}; bad {st_bad} {b_bad[:80]!r}; set {st_set} "
          f"{b_set[:80]!r}; console {bool(dbg)}, /api/log {len(dbg_web)}; shell {bool(sh_set)} "
          f"{bool(dbg_poll)}; status {st_lv.get('log_levels')}; shell log {all(h_lv.values())}; reset "
          f"{bool(sh_reset)}, debug lines after it {after[:2]}, status {st_rs.get('log_levels')}")

    # #200 (the decision of the product owner): the page asks for GET /api/log only while the
    # card Log is open. A browser test is in tools/docs_shots.py (the shot live-log); here the
    # served page must hold that code: a closed <details>, and every request of the card behind
    # its open state.
    lg_items = {"closed card": '<details class="card" id="lg-card">' in page_text,
                "log only while open": "async function lgPoll(){if(lgBusy||!lgOpen())return;" in page_text,
                "tags only on open": "if(!o)return;lgState();lgLevels();lgPoll();" in page_text,
                "stops on close": "clearInterval(lgT);lgT=0;" in page_text,
                "one /api/log request": page_text.count("/api/log?since=") == 1,
                "explainer": "`syslog.level` of the configuration filters only" in page_text}
    check("Issue #200: the web UI reads GET /api/log only while the card Log is open (a closed "
          "<details>; the poll stops when it closes), and the explainer of the log level",
          all(lg_items.values()), f"missing: {[k for k, v in lg_items.items() if not v] or 'none'}")

    # #222: a closed card with a validation error opens, and "Go to the field" opens it and shows
    # the field. A browser test is in tools/docs_shots.py (closed_card_error, the shot
    # validate-error); here the served page must hold that code.
    cc_items = {"error opens the card": "ccReveal(el);ccBadge()}" in page_text,
                "reveal opens a closed card": 'if(c&&!c.classList.contains("open"))ccApply(c,true,true)}'
                in page_text,
                "go to the field reveals": 'function goto191(el){if(typeof revealField==="function")'
                'revealField(el);el.scrollIntoView({block:"center"});el.focus(' in page_text,
                "reveal of a field": "togDmore(document.querySelector(`[aria-controls=\"${d.id}\"]`));"
                "ccReveal(el)}" in page_text}
    check("Issue #222: a validation error opens its closed card and device row, and 'Go to the "
          "field' opens the card and focuses the field",
          all(cc_items.values()), f"missing: {[k for k, v in cc_items.items() if not v] or 'none'}")


CLAIM_PASSWORD = "e2e-claimed-pw"   # issue #120: set through the claim window
PUT_WEB_PASSWORD = "e2e put pw ~!"  # issue #187: printable ASCII with spaces and symbols


def boot_press(gw, seconds=3):
    """The BOOT gesture of 2-10 s (the commissioning access point, and the claim window)."""
    gw.console("boot 1")
    time.sleep(seconds)
    gw.console("boot 0")


AP_SSID_RX = r"GW-Setup-[0-9A-F]{6}"


def pmk_digest(password, ssid):
    """Issue #187: the start of the SHA-256 of the PMK (64 hexadecimal characters) of @password
    for @ssid, as the emulator-only shell 'apsim key' gives it. The PMK of WPA2-PSK (IEEE
    802.11i): PBKDF2-HMAC-SHA1 of the passphrase with the SSID as the salt, 4096 iterations, 32
    bytes."""
    pmk = hashlib.pbkdf2_hmac("sha1", password.encode(), ssid.encode(), 4096, 32).hex()
    return hashlib.sha256(pmk.encode()).hexdigest()[:16]


def ap_key_digest(gw):
    """Issue #187: 'apsim key': the digest of the PMK in NVS, "none" when there is no PMK, None
    without an answer."""
    asked, _ = gw.shell("apsim key", [r"^ap key: "])
    lines = gw.find(r"^ap key: ", asked)
    if not lines:
        return None
    m = re.search(r"sha256 ([0-9a-f]{16})", lines[0])
    return m.group(1) if m else "none"


def ap_object(auth):
    """Issue #187: the "ap" object of GET /api/status ({} without an answer)."""
    st, _, body = http("GET", "/api/status", auth=auth)
    if st != 200:
        return {}
    return (as_json(body) or {}).get("ap") or {}


def pmk_ms(gw, since):
    """Issue #187: the time of the last PMK calculation in the log (ms), or None."""
    lines = gw.find(r"storage: WPA2 key of the access point '%s' \(PMK of the web password\) "
                    r"calculated in \d+ ms" % AP_SSID_RX, since)
    return int(re.search(r"in (\d+) ms", lines[-1]).group(1)) if lines else None


def check_claim_success(gw, j, t_claim, set_log):
    """NET-5, issue #187: the answer of a claim has what the success page shows (the SSID, the
    address of the access point, the seconds to WPA2, the plant addresses, the mDNS name); the
    access point (the simulated one of the QEMU build) changes to WPA2 with the new web password
    a few seconds after the answer; NVS holds the PMK of the password (the harness calculates the
    same PMK with the SSID), and the log gives the time of its calculation. Returns the SSID."""
    ap = j.get("ap") or {}
    ssid = ap.get("ssid") or ""
    url = ap.get("url") or ""
    plant = j.get("plant") or []
    mdns = j.get("mdns")
    later = gw.wait_log(r"ap: access point '%s': WPA2 with the web password in \d+ s \(the claim "
                        r"is done\)" % AP_SSID_RX, 5, t_claim)
    secure = gw.wait_log(r"ap: access point '%s' uses WPA2 with the web password \(the open "
                         r"period is over\)" % AP_SSID_RX, 20, t_claim)
    ms = pmk_ms(gw, t_claim - 1)
    digest = ap_key_digest(gw)
    want = pmk_digest(CLAIM_PASSWORD, ssid) if ssid else "?"
    a = ap_object(("admin", CLAIM_PASSWORD))
    host = re.match(r"https://(\d+\.\d+\.\d+\.\d+)/$", url)
    check("NET-5 #187 the answer of the claim gives the SSID, the address of the access point, the "
          "seconds to WPA2, the plant addresses and the mDNS name; the access point changes to "
          "WPA2 with the new web password about 5 s after the answer; NVS holds the PMK of the "
          "password (the same PMK as PBKDF2-HMAC-SHA1(password, SSID, 4096)); /api/status ap "
          "(QEMU timing of the PMK is only indicative)",
          re.fullmatch(AP_SSID_RX, ssid) and ap.get("secure_in_s") == 5 and host
          and f"https://{host.group(1)}/" in plant and (mdns is None or mdns.endswith(".local/"))
          and later and secure and set_log and secure - set_log >= 4 and ms is not None
          and digest == want and a.get("running") is True and a.get("security") == "wpa2"
          and a.get("key") == "web_password" and a.get("ssid") == ssid,
          f"answer {j}; '{('in 5 s' if later else 'no claim log')}', WPA2 "
          f"{(secure - set_log) if secure and set_log else -1:.1f} s after the set; PMK calculated "
          f"in {ms} ms; apsim key {digest} (expected {want}); ap {a}")
    return ssid


def check_access_point(gw, auth):
    """NET-5, issue #187, on the simulated access point of the QEMU build (no radio: its clients
    come from the emulator-only shell 'apsim'), in commissioning with a web password: POST
    /api/ap/stop needs the credential and the CSRF header, stops the access point a short time
    after its 202, and a second stop gets 409; the "ap" object, the shell and the LED follow it;
    a BOOT gesture starts it again with the web password as its key; it does not stop while a
    client is connected, and stops after the idle time without a client ('apsim idle', short
    here)."""
    # 1. the stop on request
    a0 = ap_object(auth)
    st0, _, _ = http("POST", "/api/ap/stop", {}, auth=None)
    st1, _, _ = http("POST", "/api/ap/stop", {}, auth=auth, headers={"X-Requested-With": None})
    t_stop = time.monotonic()
    st2, _, b2 = http("POST", "/api/ap/stop", {}, auth=auth)
    stopped = gw.wait_log(r"ap: commissioning access point '%s' stopped \(on request\)" % AP_SSID_RX,
                          10, t_stop)
    a1 = ap_object(auth)
    st3, _, b3 = http("POST", "/api/ap/stop", {}, auth=auth)
    _, h = gw.shell("ap", [r"^ap\s+off \(last stop: on request\); a BOOT push"])
    led = gw.wait_log(r"led: status: commissioning(?!, access point)", 5, t_stop)
    check("NET-5 #187 POST /api/ap/stop: 401 without the credential, 403 without the CSRF header; "
          "with both 202, the access point stops a short time later (log, /api/status ap, shell "
          "'ap', the LED without the access point flash); a second stop gets 409",
          a0.get("running") is True and st0 == 401 and st1 == 403 and st2 == 202
          and (as_json(b2) or {}).get("stopping") is True and stopped and a1.get("running") is False
          and a1.get("last_stop") == "request" and a1.get("security") is None and st3 == 409
          and all(h.values()) and led,
          f"ap before {a0}; no credential {st0}, no CSRF header {st1}, stop {st2} "
          f"{b2.decode(errors='replace').strip()}; stopped "
          f"{(stopped - t_stop) if stopped else -1:.1f} s later; ap after {a1}; again {st3} "
          f"{b3.decode(errors='replace').strip()[:60]}; shell {all(h.values())}; LED {bool(led)}")
    # 2. a BOOT gesture starts it again; a client keeps it on; the idle time stops it
    gw.shell("apsim idle 8", [r"access point: idle stop after 8 s"])
    gw.shell("apsim clients 1", [r"access point: 1 simulated client"])
    t_start = time.monotonic()
    boot_press(gw)
    started = gw.wait_log(r"ap: SIMULATED \(QEMU has no radio\) commissioning access point '%s' "
                          r"\(WPA2, the web password\)" % AP_SSID_RX, 10, t_start)
    led_on = gw.wait_log(r"led: status: commissioning, access point", 5, t_start)
    time.sleep(12)
    kept = not gw.find(r"ap: commissioning access point .* stopped", t_start)
    a2 = ap_object(auth)
    _, h2 = gw.shell("status", [r"^ap\s+'%s' on \(simulated: no radio\), WPA2 with the web "
                                r"password, 1 client\(s\), no stop while a client is connected"
                                % AP_SSID_RX])
    t_idle = time.monotonic()
    gw.shell("apsim clients 0", [r"access point: 0 simulated client"])
    idle = gw.wait_log(r"ap: commissioning access point '%s' stopped \(no client for the idle "
                       r"time\)" % AP_SSID_RX, 25, t_idle)
    a3 = ap_object(auth)
    gw.shell("apsim idle 0", [r"access point: idle stop after \d+ s \(Kconfig\)"])
    check("NET-5 #187 a BOOT gesture starts the access point again (WPA2 with the web password, "
          "LED flash of the access point); it stays on while a client is connected (no time "
          "limit in commissioning), and stops after the idle time without a client (8 s here)",
          started and led_on and kept and a2.get("running") is True and a2.get("clients") == 1
          and a2.get("left_s") is None and a2.get("key") == "web_password" and all(h2.values())
          and idle and 7 <= idle - t_idle <= 20 and a3.get("running") is False
          and a3.get("last_stop") == "idle",
          f"started {bool(started)}, LED {bool(led_on)}, kept 12 s with a client {kept}; ap {a2}; "
          f"shell {all(h2.values())}; stopped {(idle - t_idle) if idle else -1:.1f} s after the "
          f"last client left; ap after {a3}")
    return a1.get("ssid") or a0.get("ssid") or ""


def check_claim_window(gw, commissioning_pw):
    """CFG-3 / NET-5, issue #120, on a gateway in commissioning (no web password): the claim
    window opens only after the BOOT gesture, closes at the end of its time, accepts only a client
    of the access point (QEMU has no radio: the emulator-only 'claimsim ... eth' makes the
    Ethernet count as the access point), leaves every other endpoint behind its credential, sets
    the first web password without a credential, then closes and does not open again. Returns
    the new web password (None if it was not set)."""
    t0 = time.monotonic()
    # 1. no gesture: no window
    st0, _, b0 = http("GET", "/api/claim", auth=None)
    st1, _, b1 = http("POST", "/api/claim", {"password": CLAIM_PASSWORD}, auth=None)
    check("CFG-3/NET-5 #120 no claim window before the BOOT gesture: a set without a credential is "
          "refused",
          st0 == 200 and (as_json(b0) or {}).get("open") is False and st1 == 403
          and b"no claim window is open" in b1,
          f"GET /api/claim {st0} {b0.decode(errors='replace').strip()}; POST {st1} "
          f"{b1.decode(errors='replace').strip()[:100]}")
    # 2. a short window closes at the end of its time
    gw.shell("claimsim 15", [r"claim windows: 15 s; Ethernet is not the access point"])
    t_open = time.monotonic()
    boot_press(gw)
    opened = gw.wait_log(r"web: claim window open for 15 s", 10, t_open)
    led = gw.wait_log(r"led: status: claim window", 5, t_open)
    closed = gw.wait_log(r"web: claim window closed: its time is over", 40, t_open)
    led_back = gw.wait_log(r"led: status: commissioning", 5, closed or t_open) if closed else None
    st2, _, b2 = http("POST", "/api/claim", {"password": CLAIM_PASSWORD}, auth=None)
    check("CFG-3/NET-5 #120 the BOOT gesture opens the claim window (LED pattern 'claim window'); it "
          "closes at the end of its time, and a set is then refused",
          opened and led and closed and led_back and closed - opened >= 14 and st2 == 403
          and b"no claim window is open" in b2,
          f"open {bool(opened)}, LED {bool(led)}, closed after "
          f"{(closed - opened) if opened and closed else -1:.1f} s (15 s), LED back {bool(led_back)}; "
          f"POST {st2} {b2.decode(errors='replace').strip()[:80]}")
    # 3. an open window: only a client of the access point; the rest still needs the credential
    gw.shell("claimsim 600", [r"claim windows: 600 s; Ethernet is not the access point"])
    t_open = time.monotonic()
    boot_press(gw)
    opened = gw.wait_log(r"web: claim window open for 600 s", 10, t_open)
    _, h = gw.shell("status", [r"^claim\s+window OPEN, \d+ s left"])
    st3, _, b3 = http("GET", "/api/claim", auth=None)
    st4, _, b4 = http("POST", "/api/claim", {"password": CLAIM_PASSWORD}, auth=None)
    st5, _, _ = http("GET", "/api/status", auth=None)
    st6, _, _ = http("GET", "/api/config", auth=None)
    st7, _, _ = http("POST", "/api/restart", {}, auth=None)
    j3 = as_json(b3) or {}
    check("CFG-3/NET-5 #120 during the claim window, a client of another interface than the access "
          "point is refused (a person on the plant network cannot take the gateway), and the other "
          "endpoints still require the credential",
          opened and all(h.values()) and st3 == 200 and j3.get("open") is True
          and j3.get("allowed") is False and st4 == 403 and b"client of the access point" in b4
          and st5 == 401 and st6 == 401 and st7 == 401,
          f"open {bool(opened)}, shell {all(h.values())}; GET /api/claim {st3} {j3}; POST {st4} "
          f"{b4.decode(errors='replace').strip()[:80]}; without a credential: status {st5}, config "
          f"{st6}, restart {st7}")
    # 4. the same window, with the Ethernet as the access point (emulator)
    gw.shell("claimsim 600 eth", [r"claim windows: 600 s; Ethernet counts as the access point"])
    st8, h8, _ = http("GET", "/", auth=None)
    st9, _, b9 = http("GET", "/claim", auth=None)
    st10, _, b10 = http("GET", "/api/claim", auth=None)
    j10 = as_json(b10) or {}
    st11, _, b11 = http("POST", "/api/claim", {"password": "short"}, auth=None)
    st12, _, _ = http("POST", "/api/claim", {"password": CLAIM_PASSWORD}, auth=None,
                      headers={"X-Requested-With": None})
    t_claim = time.monotonic()
    st13, _, b13 = http("POST", "/api/claim", {"password": CLAIM_PASSWORD}, auth=None)
    set_log = gw.wait_log(r"web: web password set through the claim window", 10, t_claim)
    beep = gw.wait_log(r"buzzer: 1 beep\(s\) \(simulated\)", 5, t_claim)
    check_claim_success(gw, as_json(b13) or {}, t_claim, set_log)
    check("CFG-3/NET-5 #120 a client of the access point: '/' without a credential goes to /claim; a "
          "short password and a request without the CSRF header are refused; the first web "
          "password is set without a credential (log without the password, one beep)",
          st8 == 303 and h8.get("location") == "/claim" and st9 == 200 and b"claim window" in b9
          and st10 == 200 and j10.get("allowed") is True and j10.get("left_s", 0) > 0
          and st11 == 400 and b"at least 8 characters" in b11 and st12 == 403 and st13 == 200
          and (as_json(b13) or {}).get("ok") is True and set_log and beep,
          f"GET / {st8} -> {h8.get('location')}; GET /claim {st9}; GET /api/claim {st10} {j10}; "
          f"short {st11} {b11.decode(errors='replace').strip()[:60]}; no CSRF header {st12}; "
          f"set {st13} {b13.decode(errors='replace').strip()[:80]}; log {bool(set_log)}, beep {bool(beep)}")
    # 5. the window is closed; it does not open again; only the new password opens the web UI
    st14, _, b14 = http("POST", "/api/claim", {"password": "e2e-other-pw"}, auth=None)
    st15, _, b15 = http("GET", "/api/claim", auth=None)
    st16, _, _ = http("GET", "/api/status", auth=("admin", commissioning_pw or "?"))
    st17, _, b17 = http("GET", "/api/status", auth=("admin", CLAIM_PASSWORD))
    led_back = gw.wait_log(r"led: status: commissioning", 5, t_claim)
    t_again = time.monotonic()
    boot_press(gw)
    again = gw.wait_log(r"main: claim window not opened: a web password is set", 10, t_again)
    _, h = gw.shell("status", [r"^claim\s+window closed \(a web password is set"])
    leaked = gw.find(re.escape(CLAIM_PASSWORD), t0)
    via17 = ((as_json(b17) or {}).get("ap") or {}).get("request_via_ap")
    check("CFG-3/NET-5 #120 after the set the window is closed: a second set is refused, the "
          "commissioning password no longer opens the web UI, the new one does (#193: its "
          "/api/status ap.request_via_ap is true: the Ethernet counts as the access point), a new "
          "BOOT gesture opens no window, and the password is in no log line",
          st14 == 403 and b"web password is set" in b14 and (as_json(b15) or {}).get("open") is False
          and st16 == 401 and st17 == 200 and via17 is True and led_back and again
          and not gw.find(r"web: claim window open", t_again) and all(h.values()) and not leaked,
          f"second set {st14} {b14.decode(errors='replace').strip()[:60]}; GET /api/claim {st15} "
          f"{b15.decode(errors='replace').strip()}; commissioning password {st16}, new password "
          f"{st17} (request_via_ap {via17}); LED back {bool(led_back)}; new gesture {bool(again)}; shell {all(h.values())}; "
          f"password in the log: {leaked[:1] or 'no'}")
    return CLAIM_PASSWORD if st13 == 200 else None


def run_factory_reset(args, work):
    """CFG-5 factory reset with the BOOT button (simulated: shell 'boot 1'), then commissioning:
    no configuration, no example identity, the web UI with a new commissioning password, and a
    configuration loaded through the REST API that boots the gateway."""
    node, node2 = "GW07", "GW08"
    cfg = json.loads(config_json())
    cfg["sparkplug"]["edge_node_id"] = node
    cfg["web"] = {"password": WEB_PASSWORD}
    flash = flash_variant(args, work, "reset", {"config.json": json.dumps(cfg, indent=2).encode()})
    obs = Observer()
    gw = Gateway(args, work / "uart0-reset.log", flash)
    gw.start()
    nb = obs.wait(f"{T}/NBIRTH/{node}", 40)
    # the shell starts after MQTT and the web server (HTTPS: the certificate is generated at the
    # first boot): a 'boot 1' typed before the prompt is lost
    gw.shell("status", [r"^config\s"])
    # short press (3 s): the commissioning access point gesture, no reset
    t_short = time.monotonic()
    gw.console("boot 1")
    time.sleep(3)
    gw.console("boot 0")
    ap = gw.wait_log(r"button: BOOT pressed \d+ ms: commissioning access point", 3, t_short)
    # issue #187: QEMU has no radio: the simulated access point; the web password of the flashed
    # config.json (clear text, moved to NVS at the first boot) gave its PMK, the WPA2 key
    ap_try = gw.wait_log(r"ap: SIMULATED \(QEMU has no radio\) commissioning access point '%s' "
                         r"\(WPA2, the web password\)" % AP_SSID_RX, 5, t_short)
    ssid0 = (ap_object(("admin", WEB_PASSWORD)).get("ssid") or "")
    key0 = ap_key_digest(gw)
    ms0 = pmk_ms(gw, 0)
    # Issue #120: with a web password set, the same gesture opens no claim window
    no_claim = gw.wait_log(r"main: claim window not opened: a web password is set", 5, t_short)
    st_pw, _, b_pw = http("POST", "/api/claim", {"password": "e2e-claim-0"}, auth=None)
    _, h_pw = gw.shell("status", [r"^claim\s+window closed \(a web password is set"])
    check("CFG-3/NET-5 #120 with a web password set, the BOOT gesture opens no claim window, and "
          "a set without a credential is refused",
          no_claim and not gw.find(r"web: claim window open", t_short) and st_pw == 403
          and b"web password is set" in b_pw and all(h_pw.values()),
          f"{(gw.find(r'claim window', t_short) or ['no claim log'])[0]}; POST /api/claim {st_pw} "
          f"{b_pw.decode(errors='replace').strip()[:100]}; shell {all(h_pw.values())}")
    # Issue #74: DO/1 on before the reset (the simulated TCA9554 keeps its latch across the warm
    # restart), and the expander silent for the first 13 s of the next boot
    lit = set_do(obs, f"{T}/NCMD/{node}", f"{T}/NDATA/{node}", {1: True})
    gw.shell("expander absent 13000", [r"expander silent for 13000 ms after the next boot"])
    obs.drain()
    pressed = time.monotonic()
    gw.console("boot 1")
    warn = gw.wait_log(r"button: BOOT held: factory reset at 10 s", 5, pressed)
    fired = gw.wait_log(r"button: BOOT held 10 s: factory reset", 15, pressed)
    death = obs.wait(f"{T}/NDEATH/{node}", 10)
    commissioning = gw.wait_log(r"main: commissioning: no configuration since the factory reset", 30,
                                pressed)
    beep1 = gw.find(r"buzzer: 1 beep\(s\) \(simulated\)", t_short)
    beep2 = gw.find(r"buzzer: 2 beep\(s\) \(simulated\)", pressed)
    check("IO-6 / CFG-5 buzzer: 1 beep for the access point gesture, 2 beeps confirm the factory "
          "reset (patterns logged by the QEMU build)", beep1 and beep2,
          " | ".join(gw.find(r"buzzer: \d beep", t_short)) or "no beep logged")
    check("CFG-5 BOOT held 10 s -> factory reset (LED and log at 2 s, at 10 s), orderly NDEATH, "
          "restart into commissioning; a 3 s press is the access point gesture, not a reset",
          nb and ap and warn and fired and death and commissioning
          and fired - pressed >= 9.5,
          f"NBIRTH {node}: {bool(nb)}; 3 s press: {bool(ap)}; warning after "
          f"{(warn - pressed) if warn else -1:.1f} s, reset after {(fired - pressed) if fired else -1:.1f} s; "
          f"NDEATH: {bool(death)}; commissioning: {bool(commissioning)}")
    t1 = commissioning or time.monotonic()
    ap_comm = gw.wait_log(r"ap: SIMULATED \(QEMU has no radio\) commissioning access point '%s' "
                          r"\(WPA2, the commissioning password\)" % AP_SSID_RX, 10, t1 - 5)
    key1 = ap_key_digest(gw)
    check("NET-5 commissioning access point started on a 2-10 s BOOT press and when there is no "
          "configuration (QEMU has no radio: the simulated access point; the SoftAP itself is "
          "compiled only). Issue #187: the web password of a config.json in clear text gives the "
          "PMK in NVS (the WPA2 key); the factory reset removes it (the key is then the "
          "commissioning password)",
          ap_try and ap_comm and ssid0 and key0 == pmk_digest(WEB_PASSWORD, ssid0) and ms0 is not None
          and key1 == "none",
          f"{' | '.join(gw.find(r'ap: |BOOT pressed', t_short)[:3])}; PMK at the first boot "
          f"{key0} (expected {pmk_digest(WEB_PASSWORD, ssid0) if ssid0 else '?'}, calculated in "
          f"{ms0} ms); after the factory reset {key1}")
    pw = commissioning_password(gw, t1 - 5, 10)
    st_old, _, _ = wait_http("/api/status", 20)                    # the old web password is gone
    st, _, body = http("GET", "/api/status", auth=("admin", pw or "?"))
    status = as_json(body) or {}
    _, h = gw.shell("status", [r"^config\s+NONE: commissioning after a factory reset",
                               r"^local\s+DI 0x00, DO unknown: output expander not answering \(retrying"])
    shell, shell_unknown = h.values()
    io0 = status.get("io", {})
    time.sleep(4)
    births = [m for m in CAPTURE if m[0] >= t1 and "/NBIRTH/" in m[1]]
    led = gw.find(r"led: status: commissioning", t1 - 5)
    check("CFG-5 after the reset: commissioning mode (no polling, no MQTT, not even the example "
          "identity), old web password rejected, new commissioning password, web and shell report it",
          st_old == 401 and pw and st == 200 and status.get("mode") == "commissioning" and shell
          and not births and led,
          f"old password -> {st_old}; commissioning password {'read' if pw else 'missing'}; "
          f"/api/status {st} mode {status.get('mode')!r}; shell {bool(shell)}; NBIRTHs {len(births)}; "
          f"LED {bool(led)}")
    # Issue #74: retried in commissioning too, with every DO off as the target
    answered = gw.wait_log(r"io: TCA9554 answered on attempt \d+: DOs available, all off \(no valid "
                           r"configuration\)", 25, t1 - 5)
    _, h = gw.shell("status", [r"^local\s+DI 0x00, DO 0x00 "])
    shell_off = list(h.values())[0]
    st3, _, body3 = http("GET", "/api/status", auth=("admin", pw or "?"))
    io1 = (as_json(body3) or {}).get("io", {})
    absent = gw.find(r"io: TCA9554 at 0x20 not responding", t1 - 5)
    check("Issue #74 commissioning / recovery boot with the DO expander absent: DOs unknown "
          "(shell, /api/status do null), the expander is retried and, once it answers, every DO "
          "is switched off (DO/1 was on before the reset, the latch kept it)",
          lit and absent and shell_unknown and io0.get("do") is None and io0.get("do_unknown") is True
          and io0.get("expander", {}).get("retrying") is True and answered and shell_off
          and st3 == 200 and io1.get("do") == 0 and io1.get("do_unknown") is False,
          f"DO/1 on before: {bool(lit)}; {(absent or ['no absent log'])[0]}; shell unknown "
          f"{bool(shell_unknown)}; /api/status before {io0}; "
          f"{(gw.find(r'io: TCA9554 answered', t1 - 5) or ['never answered'])[0]}; after {io1}")
    claimed = check_claim_window(gw, pw)
    ssid = check_access_point(gw, ("admin", claimed or "?"))
    # issue #187: a configuration with a new web password: its PMK replaces the PMK of the claim
    new = json.loads(config_json())
    new["sparkplug"]["edge_node_id"] = node2
    new["web"] = {"password": PUT_WEB_PASSWORD}
    t_put = time.monotonic()
    st2, _, b2 = http("PUT", "/api/config", new, auth=("admin", claimed or "?"))
    nb2 = obs.wait(f"{T}/NBIRTH/{node2}", 40)
    check("CFG-5 commissioning: a configuration loaded through the REST API starts the gateway",
          st2 == 200 and nb2, f"PUT {st2} {b2.decode().strip()}; NBIRTH {node2}: {bool(nb2)}")
    gw.shell("status", [r"^config\s"])
    key2 = ap_key_digest(gw)
    ms2 = pmk_ms(gw, t_put)
    # ... and a document without web.password removes the password and its PMK
    new.pop("web")
    obs.drain()
    t_put2 = time.monotonic()
    st4, _, b4 = http("PUT", "/api/config", new, auth=("admin", PUT_WEB_PASSWORD))
    nb3 = obs.wait(f"{T}/NBIRTH/{node2}", 40, since=t_put2)
    gw.shell("status", [r"^config\s"])
    key3 = ap_key_digest(gw)
    st5, _, _ = http("GET", "/api/status", auth=("admin", PUT_WEB_PASSWORD))
    check("NET-5 #187 PUT /api/config with a new web.password: NVS holds its PMK (calculated once, "
          "at the save); a document without web.password removes the password and its PMK",
          st2 == 200 and nb2 and ssid and key2 == pmk_digest(PUT_WEB_PASSWORD, ssid)
          and ms2 is not None and st4 == 200 and nb3 and key3 == "none" and st5 == 401,
          f"PUT with the password {st2}, apsim key {key2} (expected "
          f"{pmk_digest(PUT_WEB_PASSWORD, ssid) if ssid else '?'}, calculated in {ms2} ms); PUT "
          f"without {st4} {b4.decode(errors='replace').strip()[:60]}, NBIRTH {bool(nb3)}, apsim key "
          f"{key3}; the old password now {st5}")
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node2}", 5)
    obs.close()
    image = pathlib.Path(flash).read_bytes()
    so, ss = storage_partition()
    stale = [w for w in (b'"GW07"', WEB_PASSWORD.encode()) if w in image[so:so + ss]]
    check("CFG-5 the factory reset erased the storage partition (no block of the old configuration "
          "is left)", not stale and b'"GW08"' in image[so:so + ss], f"stale: {stale or 'none'}")
    check_restart_admin(args, work, flash)
    check_ap_without_pmk(args, work)


def check_ap_without_pmk(args, work):
    """Issue #248 (NET-5, #187) on a flash of its own (GW09), in two boots: the stored secrets
    have the hash of a web password and no PMK. The firmware writes the two NVS entries one after
    the other, so a write error or a power cut between them leaves this state. The harness makes
    it with ESP-IDF's nvs_partition_gen.py (the NVS format of the board: not in the secure build,
    which would first move this NVS into the encrypted one). Expected: the web password opens
    the web UI; a BOOT gesture starts the access point with the commissioning password, with a
    warning in the log, and opens no claim window; the same password as a new value of
    web.password gives the PMK, and the access point then uses the web password."""
    if args.secure:
        print("[SKIP] Issue #248 a web password without a PMK: the check makes an NVS in the format "
              "of the board (not encrypted), and the secure build would first move it into the "
              "encrypted NVS", flush=True)
        return
    node, comm_pw = "GW09", "abcdefgh2345"
    salt = bytes(range(16))
    web_hash = "pbkdf2-sha256$2048$%s$%s" % (
        salt.hex(), hashlib.pbkdf2_hmac("sha256", WEB_PASSWORD.encode(), salt, 2048, 32).hex())
    nvs_off, nvs_size = storage_partition("nvs")
    csv = work / "nvs-nopmk.csv"
    csv.write_text("key,type,encoding,value\nsecrets,namespace,,\n"
                   f"commission,data,string,{comm_pw}\nweb_hash,data,string,{web_hash}\n")
    gen = pathlib.Path(os.environ["IDF_PATH"]) / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    nvs = work / "nvs-nopmk.bin"
    subprocess.run([args.idf_python, str(gen), "generate", str(csv), str(nvs), hex(nvs_size)],
                   check=True, capture_output=True)
    cfg = node_config(node)
    cfg.pop("web", None)   # no web.password in the document: the boot takes the stored hash
    flash = flash_variant(args, work, "nopmk", {"config.json": json.dumps(cfg, indent=2).encode()})
    img = bytearray(flash.read_bytes())
    img[nvs_off:nvs_off + nvs_size] = nvs.read_bytes()
    flash.write_bytes(img)
    obs = Observer()
    gw = Gateway(args, work / "uart0-nopmk.log", flash)
    t0 = time.monotonic()
    gw.start()
    nb = obs.wait(f"{T}/NBIRTH/{node}", 60)
    gw.shell("status", [r"^config\s"])
    key0 = ap_key_digest(gw)
    st_web, _, _ = wait_http("/api/status", 20)
    st_comm, _, _ = http("GET", "/api/status", auth=("admin", comm_pw))
    t_ap = time.monotonic()
    boot_press(gw)
    started = gw.wait_log(r"ap: SIMULATED \(QEMU has no radio\) commissioning access point '%s' "
                          r"\(WPA2, the commissioning password\)" % AP_SSID_RX, 10, t_ap)
    warned = gw.wait_log(r"main: access point: a web password is set, but its WPA2 key \(PMK\) is "
                         r"not in the stored secrets: the key is the commissioning password", 5, t_ap)
    console = gw.wait_log(r"^ap: WPA2 key = commissioning password '%s'" % comm_pw, 5, t_ap)
    no_claim = gw.wait_log(r"main: claim window not opened: a web password is set", 5, t_ap)
    a0 = ap_object(("admin", WEB_PASSWORD))
    ssid = a0.get("ssid") or ""
    check("Issue #248 (NET-5 #187) the stored secrets have a web password and no PMK (a fault of "
          "the stored secrets, made here with nvs_partition_gen.py): the web password opens the "
          "web UI and the commissioning password does not; a BOOT gesture starts the access point "
          "with the commissioning password as its key (the console gives it), the log warns, and "
          "no claim window opens",
          nb and key0 == "none" and st_web == 200 and st_comm == 401 and started and warned
          and console and no_claim and not gw.find(r"web: claim window open", t0)
          and a0.get("running") is True and a0.get("security") == "wpa2"
          and a0.get("key") == "commissioning_password",
          f"NBIRTH {bool(nb)}; apsim key {key0}; web password {st_web}, commissioning password "
          f"{st_comm}; {' | '.join(gw.find(r'ap: SIMULATED|main: access point: |claim window', t_ap)[:3]) or 'no log line'}; "
          f"console line {bool(console)}; ap {a0}")
    # the same password as a new value of web.password: its PMK goes to the NVS at the save
    new = node_config(node)
    new["web"] = {"password": WEB_PASSWORD}
    obs.drain()
    t_put = time.monotonic()
    st_put, _, b_put = http("PUT", "/api/config", new)
    nb2 = obs.wait(f"{T}/NBIRTH/{node}", 60, since=t_put)
    gw.shell("status", [r"^config\s"])
    key1 = ap_key_digest(gw)
    t_ap2 = time.monotonic()
    boot_press(gw)
    started2 = gw.wait_log(r"ap: SIMULATED \(QEMU has no radio\) commissioning access point '%s' "
                           r"\(WPA2, the web password\)" % AP_SSID_RX, 10, t_ap2)
    time.sleep(1)
    warned2 = gw.find(r"main: access point: a web password is set, but its WPA2 key", t_put)
    a1 = ap_object(("admin", WEB_PASSWORD))
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node}", 5)
    obs.close()
    check("Issue #248 (NET-5 #187) the web password set again (PUT /api/config with the same "
          "password as a new value) gives the PMK: after the restart the access point uses the "
          "web password, with no warning",
          st_put == 200 and nb2 and ssid and key1 == pmk_digest(WEB_PASSWORD, ssid) and started2
          and not warned2 and a1.get("key") == "web_password",
          f"PUT {st_put} {b_put.decode(errors='replace').strip()[:60]}; NBIRTH {bool(nb2)}; apsim "
          f"key {key1} (expected {pmk_digest(WEB_PASSWORD, ssid) if ssid else '?'}); "
          f"{(gw.find(r'ap: SIMULATED', t_ap2) or ['no access point line'])[0]}; warnings "
          f"{len(warned2)}; ap {a1}")


def check_restart_admin(args, work, flash):
    """Issues #209 and #203 on the flash of the reset scenario (GW08, no web password: the API
    takes the commissioning password of the console), in one more boot: the restart into safe mode
    (POST /api/restart?safe=1), its status and its end at a restart; then the shell
    'factory-reset' with a wrong word (nothing erased) and POST /api/factory-reset?confirm=1 (the
    erase of BOOT held 10 s, then commissioning)."""
    node = "GW08"
    obs = Observer()
    gw = Gateway(args, work / "uart0-reset-admin.log", flash)
    t0 = time.monotonic()
    gw.start()
    nb = obs.wait(f"{T}/NBIRTH/{node}", 60)
    pw = commissioning_password(gw, t0, 30)
    auth = ("admin", pw or "?")
    gw.shell("status", [r"^config\s"])
    # #203, #209: the dialogs give the SSID of the access point of the next boot: /api/status
    # has it also before the first start (it comes from the MAC)
    st_a, _, b_a = http("GET", "/api/status", auth=auth)
    a0 = (as_json(b_a) or {}).get("ap") or {}
    check("Issues #203 and #209: /api/status gives the SSID of the access point before its first "
          "start (the dialogs of the restart into safe mode and of the factory reset show it)",
          st_a == 200 and a0.get("running") is False and a0.get("last_stop") == "none"
          and re.fullmatch(AP_SSID_RX, a0.get("ssid") or "") is not None,
          f"status {st_a}; ap {a0}")
    # #209: no CSRF header -> 403 and no restart; then 202, NDEATH, safe mode on request
    t1 = time.monotonic()
    st_x, _, _ = http("POST", "/api/restart?safe=1", auth=auth, headers={"X-Requested-With": None})
    st_s, _, b_s = http("POST", "/api/restart?safe=1", auth=auth)
    req = gw.wait_log(r"fw: safe mode requested: the next boot starts safe mode for 60 min", 5, t1)
    death = obs.wait(f"{T}/NDEATH/{node}", 15)
    safe = gw.wait_log(r"fw: safe mode on request \(web UI, REST API or shell\)", 60, t1)
    st_st, _, b_st = wait_http("/api/status", 40, auth=auth)
    s = as_json(b_st) or {}
    sm = s.get("safe_mode") or {}
    asked = time.monotonic()
    gw.console("restart safe")
    refused = gw.wait_log(r"^restart safe: REFUSED: the gateway is in safe mode now", 5, asked)
    st_r2, _, b_r2 = http("POST", "/api/restart?safe=1", auth=auth)
    time.sleep(3)
    births = [m for m in CAPTURE if m[0] >= t1 and m[1] == f"{T}/NBIRTH/{node}"]
    check("Issue #209: POST /api/restart?safe=1 -> 202, NDEATH, and the next boot in safe mode on "
          "request: no NBIRTH, /api/status mode recovery with safe_mode {active, cause request, "
          "about 60 min left}; in safe mode 'restart safe' and a second request are refused; no "
          "CSRF header -> 403",
          nb and pw and st_x == 403 and st_s == 202 and (as_json(b_s) or {}).get("safe_mode")
          and req and death and safe and st_st == 200 and s.get("mode") == "recovery"
          and sm.get("active") is True and sm.get("cause") == "request"
          and 3500 <= sm.get("left_s", 0) <= 3600
          and s.get("config", {}).get("source") == "not loaded (safe mode: on request)"
          and refused and st_r2 == 409 and not births,
          f"NBIRTH {bool(nb)}, password {'read' if pw else 'missing'}; no header {st_x}; POST {st_s} "
          f"{b_s.decode(errors='replace').strip()}; log {bool(req)} {bool(safe)}; NDEATH "
          f"{bool(death)}; status {st_st} mode {s.get('mode')!r} safe_mode {sm} source "
          f"{s.get('config', {}).get('source')!r}; shell {bool(refused)}; second {st_r2} "
          f"{b_r2.decode(errors='replace').strip()[:80]}; NBIRTHs {len(births)}")
    # #209: safe mode does not reject the configuration: the LED has a pattern of its own, and
    # the "config_error" alarm (enabled by this configuration, kept in NVS) does not sound
    led_safe = gw.wait_log(r"led: status: safe mode", 5, t1)
    led_err = gw.find(r"led: status: config error", t1)
    bz_err = gw.find(r"buzzer: alarm 'configuration error'", t1)
    bz = s.get("buzzer") or {}
    check("Issue #209: safe mode on request shows the LED pattern 'safe mode', not 'config error', "
          "and the buzzer alarm 'config_error' of the configuration does not sound (/api/status "
          "buzzer.alarm none)",
          led_safe and not led_err and not bz_err and bz.get("alarm") == "none",
          f"LED {(gw.find(r'led: status: ', t1) or ['none'])[-1]!r}; config error LED {led_err[:1]}; buzzer log {bz_err[:1]}; status buzzer {bz}")
    # a restart ends safe mode (the request was for one boot)
    t2 = time.monotonic()
    st_r, _, _ = http("POST", "/api/restart", auth=auth)
    nb2 = obs.wait(f"{T}/NBIRTH/{node}", 60, since=t2)
    st_n, _, b_n = wait_http("/api/status", 30, auth=auth)
    sm2 = (as_json(b_n) or {}).get("safe_mode") or {}
    check("Issue #209: POST /api/restart in safe mode -> normal mode again (NBIRTH; safe_mode "
          "active false)",
          st_r == 202 and nb2 and st_n == 200 and (as_json(b_n) or {}).get("mode") == "normal"
          and sm2.get("active") is False,
          f"restart {st_r}; NBIRTH {bool(nb2)}; status {st_n} {(as_json(b_n) or {}).get('mode')!r} "
          f"{sm2}")
    # #203: the shell asks for ERASE (a wrong word erases nothing); the API needs ?confirm=1
    gw.shell("status", [r"^config\s"])
    asked = time.monotonic()
    gw.console("factory-reset")
    prompt = gw.wait_log(r"^factory-reset: this erases the configuration.*Type ERASE to confirm", 5,
                         asked)
    gw.console("erase")
    aborted = gw.wait_log(r"^factory-reset: aborted, nothing erased$", 5, asked)
    st_nc, _, b_nc = http("POST", "/api/factory-reset", auth=auth)
    st_cs, _, _ = http("POST", "/api/factory-reset?confirm=1", auth=auth,
                       headers={"X-Requested-With": None})
    time.sleep(1)
    still = gw.find(r"storage: factory reset", asked)
    obs.drain()
    t3 = time.monotonic()
    st_f, _, b_f = http("POST", "/api/factory-reset?confirm=1", auth=auth)
    beeps = gw.wait_log(r"buzzer: 2 beep\(s\) \(simulated\)", 5, t3)
    erased = gw.wait_log(r"storage: factory reset: erasing LittleFS 'storage' and NVS", 10, t3)
    death3 = obs.wait(f"{T}/NDEATH/{node}", 15)
    comm = gw.wait_log(r"main: commissioning: no configuration since the factory reset", 60, t3)
    pw2 = commissioning_password(gw, t3, 30)
    st_c, _, b_c = wait_http("/api/status", 30, auth=("admin", pw2 or "?"))
    st_old, _, _ = http("GET", "/api/status", auth=auth)
    check("Issue #203: the shell 'factory-reset' with a wrong word erases nothing; POST "
          "/api/factory-reset without ?confirm=1 -> 400, without the CSRF header -> 403; with "
          "?confirm=1 -> 202, the 2 beeps, the erase, NDEATH and commissioning with a new "
          "commissioning password",
          prompt and aborted and st_nc == 400 and b"confirm=1" in b_nc and st_cs == 403 and not still
          and st_f == 202 and (as_json(b_f) or {}).get("erased") is True and beeps and erased
          and death3 and comm and pw2 and pw2 != pw and st_c == 200
          and (as_json(b_c) or {}).get("mode") == "commissioning" and st_old == 401,
          f"shell prompt {bool(prompt)}, aborted {bool(aborted)}; no confirm {st_nc}; no header "
          f"{st_cs}; erase log before the request {len(still)}; POST {st_f} "
          f"{b_f.decode(errors='replace').strip()}; beeps {bool(beeps)}, erase {bool(erased)}, NDEATH "
          f"{bool(death3)}, commissioning {bool(comm)}; new password {bool(pw2 and pw2 != pw)}; "
          f"status {st_c} {(as_json(b_c) or {}).get('mode')!r}; the old password {st_old}")
    gw.kill()
    obs.close()
    image = pathlib.Path(flash).read_bytes()
    so, ss = storage_partition()
    check("Issue #203: the factory reset through the REST API erased the storage partition (no "
          "block of the configuration GW08 is left)", b'"GW08"' not in image[so:so + ss],
          "GW08 still in the storage partition" if b'"GW08"' in image[so:so + ss] else "erased")


def run(args, log, slave, broker, conf, work):
    nbirth_t, dbirth_t = f"{T}/NBIRTH/{NODE}", f"{T}/DBIRTH/{NODE}/{DEVICE}"
    ndata_t, ddata_t = f"{T}/NDATA/{NODE}", f"{T}/DDATA/{NODE}/{DEVICE}"
    ndeath_t, ddeath_t = f"{T}/NDEATH/{NODE}", f"{T}/DDEATH/{NODE}/{DEVICE}"
    ncmd_t, dcmd_t = f"{T}/NCMD/{NODE}", f"{T}/DCMD/{NODE}/{DEVICE}"

    obs = Observer()
    gw = Gateway(args, log)
    t0 = time.monotonic()
    gw.start()
    # Issue #72: the shell starts first; a command sent as soon as it is there, before MQTT,
    # the bus and the web server are up, gets an answer (sent once: nothing repeats it)
    ready = gw.wait_log(r"shell: console ready", 30, t0)
    early = time.monotonic()
    gw.console("status")
    answered = gw.wait_log(r"^firmware \S+, reset reason", 5, early)
    gw.console("mbread 1 holding 10")
    mb = gw.wait_log(r"^mbread: (not available yet: .* (is starting|is loading)|unit 1 holding 10: )", 5,
                     early)
    nb = obs.wait(nbirth_t, 40)
    later = [ln for ln in gw.find(r"edge: Sparkplug session up|web: HTTPS on port", t0)]
    first_up = gw.wait_log(r"edge: Sparkplug session up", 5, t0)   # the console lags MQTT
    check("Issue #72 the console shell answers a command sent right after it starts, before the "
          "MQTT session and the web server (a command whose subsystem is not up says so)",
          ready and answered and mb and first_up and answered < first_up,
          f"shell ready {ready - t0:.2f} s after QEMU started, 'status' answered "
          f"{answered - early:.2f} s later, first NBIRTH log {first_up - t0:.2f} s; "
          f"{(gw.find(r'^mbread: ', early) or ['no mbread answer'])[0]}; {later[:2]}"
          if ready and answered and first_up else
          f"ready {ready}, answered {answered}; {gw.find(r'^config |^firmware ', t0)[:2]}")
    if not check("1a. NBIRTH published", nb, "no NBIRTH within 40 s" if not nb else
                 f"{nb[0] - t0:.1f} s after QEMU start"):
        return
    # REL-6: less than 10 s from the boot to the NBIRTH with the network and the broker up. t0 is
    # before the start of QEMU, so the time also holds the start of the emulator (stricter than the
    # boot of the chip). The limit scales with --timeout-scale, as every budget for a slow host.
    rel6_limit = REL6_LIMIT_S * args.timeout_scale
    check("REL-6 less than 10 s from the boot to the NBIRTH (network and broker available)",
          nb[0] - t0 < rel6_limit,
          f"{nb[0] - t0:.1f} s from the start of QEMU to the NBIRTH, limit {rel6_limit:.0f} s")
    if args.psram:
        # Issue #85: the board's memory path. The configuration, the Sparkplug engine and its
        # full payload buffer are allocated in the octal PSRAM, and the shell sees it.
        placed = {w: gw.find(r"storage: %s: \d+ B in PSRAM" % w, t0)
                  for w in ("configuration", "Sparkplug engine", "Sparkplug payload buffer")}
        asked, h = gw.shell("status", [r"^heap\s+internal .*PSRAM [1-9]\d* B free"])
        check("Issue #85 octal PSRAM in QEMU: configuration, Sparkplug engine and payload buffer "
              "in PSRAM; the shell reports the free PSRAM",
              all(placed.values()) and all(h.values()),
              " | ".join((v or [f"{k}: not in PSRAM"])[0] for k, v in placed.items()) + "; "
              + ((gw.find(r"^heap\s+internal", asked) or ["no heap line"])[0]))
    p = nb[2]
    nb0 = [nb]
    b0 = bdseq_of(p)
    reason0 = metric(p, name="Node Info/Reset Reason")
    check("1b. NBIRTH seq=0, bdSeq metric, timestamp", p.seq == 0 and p.timestamp > 0,
          f"seq={p.seq} bdSeq={b0} timestamp={p.timestamp}")
    # SPB-3 / #161 (tck-id-topics-nbirth-bdseq-increment): the first CONNECT of the boot has bdSeq
    # 0; the console logs the bdSeq of each attempt, and the NBIRTH carries that of its attempt
    tried = [int(x) for x in re.findall(r"bdSeq=(\d+)", " ".join(
        l for l in gw.find(r"edge: connecting to .* bdSeq=\d+", t0)))]
    check("SPB-3 bdSeq starts at 0 at the boot (the first attempt), and the NBIRTH carries the bdSeq "
          "of its attempt", tried[:1] == [0] and b0 == tried[-1],
          f"bdSeq of the attempts {tried}; NBIRTH bdSeq {b0}")
    check("TIME-1 timestamps are UTC ms from SNTP (the harness' own responder)",
          SNTP_BOUND.is_set() and abs(p.timestamp / 1000 - time.time()) < 60,
          f"NBIRTH timestamp {p.timestamp} vs host {int(time.time() * 1000)}; responder on UDP 123: "
          f"{SNTP_BOUND.is_set()}")
    gw.wait_log(r"births sent in", 10)
    births = gw.find(r"births sent in \d+ ms, longest stretch without feeding the watchdog")
    nums = [int(x) for x in re.findall(r"(\d+) ms", births[0])] if births else []
    check("REL-1 / issue #3 the Sparkplug task feeds the watchdog after every birth it publishes",
          len(nums) == 2 and nums[1] <= nums[0] and nums[1] <= 1000,
          births[0] if births else "no 'births sent' log")
    db = obs.wait(dbirth_t, 10)
    ok = db and abs(metric(db[2], name="Voltage").float_value - 230.5) < 1e-3 \
        and abs(metric(db[2], name="Setpoint").float_value - 5.0) < 1e-4
    check("SPB-6 DBIRTH with current values", ok,
          spb_decode.describe("DBIRTH", db[2]).replace("\n", " |") if db else "no DBIRTH")
    check_mb6_birth(db)
    if not any(m[1] == f"{T}/DBIRTH/{NODE}/{BULK}" for m in CAPTURE):
        obs.wait(f"{T}/DBIRTH/{NODE}/{BULK}", 10)
    bulk = [m for m in CAPTURE if m[1] == f"{T}/DBIRTH/{NODE}/{BULK}"]
    vals = [m.int_value for m in bulk[0][2].metrics] if bulk else []
    check("SPB-6 DBIRTH larger than the 2 KiB esp-mqtt buffer (fragmented publish)",
          bulk and len(bulk[0][3]) > 2048 and vals == list(range(1000, 1060)),
          f"{len(bulk[0][3])} bytes, {len(vals)} metrics, values {vals[:3]}..{vals[-1:]}" if bulk
          else "no DBIRTH for Bulk")
    reqs = [r for r in slave.reads if r[0] == 2]
    check("MB-11 60 contiguous registers read with one FC03 request per cycle",
          reqs and all(r[1:] == (3, 100, 60) for r in reqs),
          f"{len(reqs)} requests to unit 2 so far, first {reqs[0] if reqs else None}")
    # Issue #38: Meter1 sets max_gap 10, Bulk keeps the build default (0). Fast scan: holding
    # 10 and 20-21 (gap 9) are one FC03, Counter 34-37 another (gap 12 from 21), Alarm 60 a third;
    # slow scan (MB-12, never merged with fast tags): Energy 30-33 and Serial 50-57 (gap 16).
    # Input 0-1 alone and 40-46 contiguous. No DCMD has run yet: only cycle reads so far.
    # this boot only: the earlier scenarios (web: diagnostic reads) share the slave
    meter = {r[1:] for t, r in zip(list(slave.times), list(slave.reads), strict=False) if r[0] == 1 and t >= t0}
    want = {(1, 0, 1), (3, 10, 12), (3, 34, 4), (3, 60, 1), (3, 30, 4), (3, 50, 8), (4, 0, 2),
            (4, 40, 7)}
    # the issue #72 'mbread 1 holding 10' sent at the start reads the register when the bus is
    # already up at that moment (the boot got faster with PSRAM); that one FC03 is this
    # scenario's own request, not a cycle read
    if gw.find(r"^mbread: unit 1 holding 10: ", early):
        want.add((3, 10, 1))
    check("MB-11 / issue #38 per-device max_gap: Meter1 (max_gap 10) bridges holes up to 10 "
          "registers, Bulk keeps the build default", meter == want,
          f"Meter1 requests {sorted(meter)} (expected {sorted(want)})")

    # NET-6: host name derived from edge_node_id; the mDNS responder starts with it. It cannot
    # answer here: open_eth has no multicast filter, so joining 224.0.0.251 fails and the
    # responder leaves the interface out (the W5500 has the filter).
    announced = gw.find(r"mdns: gw01\.local, _https\._tcp port 443 \('Plant1/GW01'\)", t0)
    check("NET-6 host name gw01 derived from edge_node_id; mDNS responder and _https._tcp service "
          "started (answers not testable on QEMU's open_eth)",
          announced, (announced or ["no mdns log"])[0])

    # CFG-6 diagnostic shell on the console UART
    asked, h = gw.shell("status", [r"config\s+\S+.*: ok",
                                   r"network\s+.*MQTT \S+, Modbus 2/5 devices online",
                                   r"^iface\s+ethernet up \d+\.\d+\.\d+\.\d+ \(active\) \| wifi not configured",
                                   r"^tasks\s"])
    status_ok, status_net, status_if, _ = h.values()
    check("CFG-6 console shell: 'status' with a valid configuration (config ok, MQTT, devices online, "
          "Ethernet the only and active interface)",
          status_ok and status_net and status_if,
          " | ".join(gw.find(r"^config\s|^network\s|^iface\s", asked)) or "no answer")
    # Issue #15: the network stack runs on core 0, the RS485 bus owner on core 1
    tasks = gw.find(r"^tasks\s", asked)
    want = ["rs485_bus 1", "sparkplug 0", "tcpip 0", "mqtt_task 0", "emac_rx 0", "sys_evt 0"]
    check("Issue #15 task affinity: lwIP, esp-mqtt, Ethernet RX and event loop on core 0, bus on core 1",
          tasks and all(f" {w}" in tasks[0] for w in want), tasks[0] if tasks else "no 'tasks' line")
    asked, h = gw.shell(["devices", "config"],
                        [r"Meter1\s+unit\s+1 online\s+cycles [1-9]\d* failed \d+ \| ok [1-9]",
                         r"Bulk\s+unit\s+2 online", r"sparkplug\s+group 'Plant1', edge node 'GW01'"])
    meter, bulk, summary = h.values()
    check("CFG-6 console shell: 'devices' (online state, counters) and 'config' summary",
          meter and bulk and summary,
          " | ".join(gw.find(r"Meter1\s+unit|Bulk\s+unit|sparkplug\s+group", asked)) or "no answer")
    # Issue #68: the summary shows what the poll engine runs with (MB-11, MB-12) and MB-15
    asked, h = gw.shell("config",
                        [r"^\s+Meter1\s+unit\s+1, poll 1000 ms, slow 3000 ms, offline after 3, 11 tag\(s\) "
                         r"\(9 fast, 2 slow\), blocks max_gap 10$",
                         r"^\s+Bulk\s+unit\s+2, poll 1000 ms, slow 10000 ms, offline after 3, 60 tag\(s\) "
                         r"\(60 fast, 0 slow\), blocks max_gap 0 \(default\)$",
                         r"^modbus_tcp enabled, port 502, idle timeout 60 s, clients 10\.0\.2\.2/32$",
                         r"^modbus_tcp local unit 100 \(DIs, DOs, counters\), writes on$"])
    check("Issue #68 shell 'config': per-device periods, scan classes and block reads, modbus_tcp line",
          all(h.values()), " | ".join(gw.find(r"^\s+(Meter1|Bulk)\s+unit\s+\d+, poll|^modbus_tcp", asked))
          or "no answer")

    # REL-4 remote syslog: RFC 5424 over UDP, hostname = edge node id, app-name = esp_log tag,
    # facility local0, timestamp from SNTP
    # the line goes on with the births' duration and watchdog margin (issue #3)
    up = wait_syslog(NODE, r"^Sparkplug session up \(bdSeq=%d\): births sent in \d+ ms, .*$" % b0, 5, t0)
    host_now = time.time()
    ts_ok = False
    if up:
        try:
            import datetime
            ts = datetime.datetime.strptime(up[2], "%Y-%m-%dT%H:%M:%S.%fZ").replace(
                tzinfo=datetime.timezone.utc).timestamp()
            ts_ok = abs(ts - host_now) < 60
        except ValueError:
            pass
    bad = [text for t, text in SYSLOG if t >= t0 and not RFC5424.match(text)]
    asked, h = gw.shell("status", [r"^syslog\s+10\.0\.2\.2:%d connected, sent [1-9]\d*, " % SYSLOG_PORT])
    stats = list(h.values())[0]
    check("REL-4 log lines reach the syslog server as RFC 5424 (local0, hostname GW01, app-name = "
          "tag, SNTP timestamp); the shell reports the counters",
          up and up[1] == 16 * 8 + 6 and up[3] == "edge" and ts_ok and not bad and stats,
          (f"<{up[1]}>1 {up[2]} {NODE} {up[3]} seq {up[4]}: {up[5]}" if up else "no 'session up' line")
          + f"; {len(syslog_from(NODE, t0))} datagrams, {len(bad)} malformed; "
          + " | ".join(gw.find(r"^syslog\s", asked)[:1]))

    # Report by exception and deadband
    obs.drain()
    slave.set_float(4, 0, 230.55)
    quiet = obs.wait(ddata_t, 2.5)
    slave.set_float(4, 0, 231.0)
    dd = obs.wait(ddata_t, 3)
    ok = quiet is None and dd and len(dd[2].metrics) == 1 and dd[2].metrics[0].alias == ALIAS["Voltage"] \
        and not dd[2].metrics[0].HasField("name")
    check("SPB-7/SPB-12 DDATA by exception, alias only, deadband 0.1 respected", ok,
          f"0.05 V change -> {'DDATA' if quiet else 'nothing'}; 0.5 V change -> "
          f"{[(m.alias, round(m.float_value, 3)) for m in dd[2].metrics] if dd else 'nothing'}")
    check_properties_and_quality(obs, slave, db, ddata_t)
    check_diagnostics(obs, p, ndata_t)

    # 3. DI -> NDATA < 100 ms; NCMD DO/3
    obs.drain()
    sent = time.monotonic()
    gw.console("di 1 1")
    nd = obs.wait(ndata_t, 2, lambda q: metric(q, alias=ALIAS["DI/1"]) is not None)
    lat = (nd[0] - sent) * 1000 if nd else None
    check("3a. DI change visible in NDATA < 100 ms", nd and lat < 100 and
          metric(nd[2], alias=ALIAS["DI/1"]).boolean_value,
          f"{lat:.0f} ms console->NDATA (includes 20 ms debounce and emulator latency)" if nd else "no NDATA")
    births = [counter_of(nb0[0][2], n) for n in range(1, 9)] if nb0 else []
    check("IO-4 pulse counters: NBIRTH carries Counter/1..8 (UInt64, 0 on a fresh device); the DI "
          "activation counts one pulse, in the same NDATA as the DI change",
          births == [0] * 8 and nd and counter_of(nd[2]) == 1,
          f"NBIRTH counters {births}; NDATA {[(m.alias, m.long_value if datatype_of(nd[1], m) == 8 else m.boolean_value) for m in nd[2].metrics] if nd else None}")
    obs.drain()
    obs.publish_cmd(ncmd_t, [{"name": "DO/3", "datatype": 11, "boolean_value": True}])
    nd = obs.wait(ndata_t, 3, lambda q: metric(q, alias=ALIAS["DO/3"]) is not None)
    check("3b. NCMD DO/3=true drives DO3 and NDATA confirms it",
          nd and metric(nd[2], alias=ALIAS["DO/3"]).boolean_value,
          f"NDATA {[(m.alias, m.boolean_value) for m in nd[2].metrics]}" if nd else "no NDATA")

    # Issue #10: a metric name longer than the codec accepts aborts the decode; the DO/1 before
    # it must not be applied either (all or nothing), so no NDATA for DO/1.
    obs.drain()
    since = time.monotonic()
    obs.publish_cmd(ncmd_t, [{"name": "DO/1", "datatype": 11, "boolean_value": True},
                             {"name": "X" * 80, "datatype": 11, "boolean_value": True}])
    nd = obs.wait(ndata_t, 2, lambda q: metric(q, alias=ALIAS["DO/1"]) is not None)
    dropped = gw.wait_log(r"NCMD: undecodable payload .*nothing applied", 1, since)
    check("SPB-11 / issue #10 NCMD aborted mid-payload (80-character name) applies nothing",
          dropped and nd is None,
          f"log: {(gw.find(r'NCMD: undecodable', since) or ['none'])[0]}; NDATA for DO/1: {nd is not None}")

    # 4. DCMD Setpoint=12.3 -> holding 10 = 123 via FC06, confirmed by DDATA
    obs.drain()
    slave.ctx.writes.clear()
    obs.publish_cmd(dcmd_t, [{"name": "Setpoint", "datatype": 9, "float_value": 12.3}])
    dd = obs.wait(ddata_t, 5, lambda q: metric(q, alias=ALIAS["Setpoint"]) is not None)
    reg = slave.get(3, 10)[0]
    check("4. DCMD Setpoint=12.3 writes 123 to holding 10 with FC06 and DDATA confirms",
          dd and reg == 123 and (6, 10, [123]) in slave.ctx.writes
          and abs(metric(dd[2], alias=ALIAS["Setpoint"]).float_value - 12.3) < 1e-4,
          f"slave writes {slave.ctx.writes}, register={reg}, DDATA "
          f"{metric(dd[2], alias=ALIAS['Setpoint']).float_value if dd else None}")
    # MB-4 / issue #2: a 32-bit value is written with one FC16 of 2 registers (13 bytes, the
    # frame that timed out at 1200 baud), executed exactly once and confirmed by DDATA.
    obs.drain()
    slave.ctx.writes.clear()
    obs.publish_cmd(dcmd_t, [{"name": "Limit", "datatype": 9, "float_value": 1234.5}])
    dd = obs.wait(ddata_t, 5, lambda q: metric(q, alias=ALIAS["Limit"]) is not None)
    hi, lo = struct.unpack(">HH", struct.pack(">f", 1234.5))
    check("MB-4 DCMD float32 -> a single FC16 of 2 registers, confirmed by DDATA",
          dd and slave.ctx.writes == [(16, 20, [hi, lo])]
          and abs(metric(dd[2], alias=ALIAS["Limit"]).float_value - 1234.5) < 1e-3,
          f"slave writes {slave.ctx.writes}, DDATA "
          f"{metric(dd[2], alias=ALIAS['Limit']).float_value if dd else None}")
    check_mb6_writes(obs, slave, dcmd_t, ddata_t)
    obs.drain()
    slave.ctx.writes.clear()
    obs.publish_cmd(dcmd_t, [{"alias": ALIAS["Run"], "datatype": 11, "boolean_value": True}])
    dd = obs.wait(ddata_t, 5, lambda q: metric(q, alias=ALIAS["Run"]) is not None)
    check("SPB-10 DCMD by alias (Run=true via FC05)", dd and slave.get(1, 0)[0] and
          (5, 0, [True]) in slave.ctx.writes, f"slave writes {slave.ctx.writes}")
    since = time.monotonic()
    obs.publish_cmd(dcmd_t, [{"name": "Voltage", "datatype": 9, "float_value": 1.0}])
    time.sleep(1.5)
    # the command came by name only: the log must not invent an "alias 0" (issue #14)
    check("SPB-10 DCMD on read-only metric discarded and logged",
          gw.find(r"DCMD: metric 'Voltage' discarded: read-only", since) and not slave.ctx.writes[1:],
          (gw.find(r"DCMD: metric 'Voltage'", since) or ["no log line"])[0])

    # 5. Rebirth
    obs.drain()
    obs.publish_cmd(ncmd_t, [{"name": "Node Control/Rebirth", "datatype": 11, "boolean_value": True}])
    nb = obs.wait(nbirth_t, 5)
    db = obs.wait(dbirth_t, 5)
    check("5. NCMD Rebirth re-emits NBIRTH (seq 0, same bdSeq) and every DBIRTH",
          nb and db and nb[2].seq == 0 and bdseq_of(nb[2]) == b0 and db[2].seq == 1,
          f"NBIRTH seq={nb[2].seq if nb else None} bdSeq={bdseq_of(nb[2]) if nb else None}, "
          f"DBIRTH seq={db[2].seq if db else None}")

    check_offline_backoff(slave, t0)
    check_scan_classes(slave)
    check_write_between_blocks(obs, slave, dcmd_t, ddata_t)
    check_scan_rate(obs, gw, slave, p, ncmd_t, ndata_t)
    check_modbus_tcp(gw, slave)
    # Issue #69: the forwarded transactions are counted per unit (apart from the polling) and
    # the gateway counts connections, forwarded requests and the exceptions it made up: the
    # pymodbus client and the raw frame (2 connections), read + write of unit 1, the read of the
    # dead unit 3 (1 + 2 retries, 0B) and FC 0x2B (01)
    asked, h = gw.shell("devices",
                        [r"^Meter1\s+unit\s+1 online .*\| tcp ok 2 timeout 0 crc 0 frame 0 exception 0$",
                         r"^Dead3\s+unit\s+3 offline .*\| tcp ok 0 timeout 3 crc 0 frame 0 exception 0$",
                         r"^modbus_tcp connections 2 accepted 0 rejected \| forwarded 3 \| gateway exceptions "
                         r"01 1 03 0 0A 0 0B 1 \| other units ok 0 timeout 0 crc 0 frame 0 exception 0 "
                         r"\| local unit 0$"])
    check("Issue #69 Modbus TCP counters: forwarded transactions per unit, gateway connections, "
          "forwarded requests and its own exceptions (shell 'devices')", all(h.values()),
          " | ".join(gw.find(r"^(Meter1|Dead3)\s+unit|^modbus_tcp", asked)) or "no answer")
    check_modbus_tcp_table(obs, ndata_t)
    check_modbus_tcp_via_ap(gw, slave)
    check_modbus_local(gw, obs, ndata_t)

    # 2. Slave off -> DDEATH; back on -> DBIRTH
    obs.drain()
    off = time.monotonic()
    slave.stop()
    dd = obs.wait(ddeath_t, 15)
    bound = 3 * 1.0 + 3 * (0.2 * 3) + 1.0  # offline_after*poll + per-cycle timeouts + 1 s margin
    check("2a. Slave powered off -> DDEATH within offline_after x poll_ms + timeouts",
          dd and dd[0] - off <= bound, f"{dd[0] - off:.2f} s (bound {bound:.1f} s)" if dd else "no DDEATH")
    asked, h = gw.shell("devices", [r"Meter1\s+unit\s+1 offline\s+cycles \d+ failed [1-9]\d* \| ok \d+ "
                                    r"timeout [1-9]"])
    offline = list(h.values())[0]
    check("CFG-6 / MB-13 shell shows the device offline with its failed cycles and timeouts",
          offline, (gw.find(r"Meter1\s+unit", asked) or ["no answer"])[0])
    on = time.monotonic()
    slave.start()
    # Offline devices are probed with a backoff capped at GW_MB_OFFLINE_PROBE_MAX_MS: the
    # DBIRTH comes at most one capped interval plus one poll cycle after the slave is back.
    back = PROBE_MAX_S + 1.0 + 1.0
    db = obs.wait(dbirth_t, back + 5)
    check("2b. Slave back -> DBIRTH within the offline probe interval (cap) + one cycle",
          db and db[0] - on <= back,
          f"{db[0] - on:.2f} s after restart (bound {back:.1f} s)" if db else "no DBIRTH")

    dbirth_topics = [f"{T}/DBIRTH/{NODE}/{d}" for d in (DEVICE, BULK)]

    def births_ok(nb, dbs):
        """NBIRTH with seq 0 followed by one DBIRTH per device with seq 1, 2."""
        return nb and nb[2].seq == 0 and sorted(m[2].seq for m in dbs.values()) == [1, 2] \
            and all(m[0] >= nb[0] for m in dbs.values())

    # IO-3 safe state at a restart: DO/2 "on", DO/1 and DO/3 "off", DO/4 "hold" (power-up: off)
    boot_dos = do_states(nb0[0][2]) if nb0 else {}
    check("IO-3 restart state per output: do_failsafe 'on' outputs on once the configuration is "
          "valid, 'off' and (after a power-up) 'hold' outputs off",
          boot_dos.get(2) is True and not any(boot_dos.get(n) for n in (1, 3, 4, 5, 6, 7, 8)),
          f"NBIRTH DO states {boot_dos}; {(gw.find(r'io: DO at restart', t0) or ['no log'])[0]}")
    # IO-7 local rules: DO/5 follows DI/2, DO/6 toggles on every activation of DI/3
    obs.drain()
    sent = time.monotonic()
    gw.console("di 2 1")
    nd = obs.wait(ndata_t, 3, lambda q: do_states(q).get(5) is True)
    lat = (nd[0] - sent) * 1000 if nd else None
    since = time.monotonic()
    refused = set_do(obs, ncmd_t, ndata_t, {5: False}) is None
    confirm = [do_states(m[2])[5] for m in CAPTURE if m[0] >= since and m[1] == ndata_t
               and 5 in do_states(m[2])]
    log5 = gw.wait_log(r"io: DO5 is driven by local rule 0 \(follow DI2\): NCMD refused", 2, since)
    check("IO-7 rule 'follow': DI/2 active -> DO/5 on (NDATA), with no command; NCMD on DO/5 "
          "refused (the rule owns it) and the NDATA confirms the real state",
          nd and refused and log5 and confirm == [True],
          f"{lat:.0f} ms console->NDATA with DO/5 on; NCMD DO/5=false refused: {bool(log5)}"
          if nd else "no NDATA with DO/5")
    toggles = []
    for _ in range(2):
        obs.drain()
        gw.console("di 3 1")
        nd = obs.wait(ndata_t, 3, lambda q: 6 in do_states(q))
        toggles.append(do_states(nd[2])[6] if nd else None)
        gw.console("di 3 0")
        time.sleep(0.3)
    check("IO-7 rule 'toggle': each activation of DI/3 toggles DO/6", toggles == [True, False],
          f"DO/6 after each activation: {toggles}")
    check_local_io_channels(gw, obs, nb0[0], ncmd_t, ndata_t, t0)

    # IO-6: Dead3..Dead5 never answer: the "device offline" alarm sounds alarm_after_s (5 s)
    # after the boot (the QEMU build logs the patterns instead of driving GPIO46)
    dev_alarm = gw.find(r"buzzer: alarm 'device offline': 1 beep\(s\) every 10 s", t0)
    check("IO-6 buzzer alarm 'device offline' (enabled in local_io.buzzer.alarms) after "
          "alarm_after_s with a Modbus device offline: 1 beep every 10 s",
          dev_alarm and gw.find(r"buzzer: alarms: configuration error MQTT down device offline "
                                r"\(MQTT down / device offline after 5 s\)", t0),
          (dev_alarm or gw.find(r"buzzer: ", t0) or ["no buzzer log"])[0])
    # Before the outage: DO/2 off, DO/3 on (3b) and DO/4 on by NCMD
    pre = set_do(obs, ncmd_t, ndata_t, {2: False, 3: True, 4: True})

    # MQTT-2 broker restart: the broker publishes the session's will (NDEATH) while shutting
    # down; the gateway retries with exponential backoff (the attempts fail at TCP: no CONNECT
    # packet, so the same bdSeq, #161), and the new session starts with NBIRTH and every DBIRTH.
    obs.drain()
    since = time.monotonic()
    broker.terminate()
    broker.wait()
    will = obs.wait(ndeath_t, 2)
    time.sleep(6.5)
    # IO-7 without the network (and after the failsafe tripped): DI/3 toggles DO/6 on
    gw.console("di 3 1")
    time.sleep(0.5)
    gw.console("di 3 0")
    time.sleep(2)
    delays = [int(x) for x in re.findall(r"in (\d+) ms", " ".join(gw.find(r"reconnect attempt", since)))]
    attempts = gw.find(r"connecting to .* bdSeq=", since)
    obs.close()
    warn = wait_syslog(NODE, r"^MQTT down; reconnect attempt \d+ in \d+ ms$", 1, since)
    check("REL-4 warnings are forwarded with their severity while the broker is down (UDP, no "
          "MQTT needed)", warn and warn[1] == 16 * 8 + 4 and warn[3] == "edge",
          f"<{warn[1]}> {warn[3]}: {warn[5]}" if warn else "no warning datagram")
    broker = start_broker(conf, work / "mosquitto2.log")
    obs = Observer()
    nb = obs.wait(nbirth_t, 70)
    dbs = obs.wait_all(dbirth_topics, 10)
    growing = len(delays) >= 3 and all(1000 <= d <= 60000 for d in delays) and delays[2] > delays[0]
    check("MQTT-2 reconnect with exponential backoff and jitter (1 s..60 s)", growing and nb,
          f"delays {delays} ms, {len(attempts)} attempts while down, NBIRTH after restart: {bool(nb)}")
    b1 = bdseq_of(nb[2]) if nb else None
    down_seqs = sorted({int(x) for x in re.findall(r"bdSeq=(\d+)", " ".join(attempts))})
    check("SPB-3 / issue #161 bdSeq increases by exactly 1 for each CONNECT packet: the attempts "
          "while the broker is down fail at TCP (no CONNECT) and keep the same value; the new "
          "session has the bdSeq of the old one + 1 (tck-id-topics-nbirth-bdseq-increment)",
          nb and len(attempts) >= 2 and down_seqs == [(b0 + 1) % 256] and b1 == (b0 + 1) % 256,
          f"first session bdSeq={b0}, attempts while down={len(attempts)} with bdSeq {down_seqs}, "
          f"new NBIRTH bdSeq={b1}")
    if args.simulate_qemu_death == "main" and not RETRIES:
        # issue #105 M6 self-test: the emulator dies after the main broker was replaced
        # (mosquitto2): the retry must find a main broker running
        gw.proc.send_signal(signal.SIGABRT)
        gw.proc.wait()
        raise ConnectionError("simulated QEMU death after the broker swap")
    # IO-3: 9 s without a session > failsafe_s (5 s): DO/2 on, DO/1 and DO/3 off, DO/4 holds
    lost = gw.wait_log(r"edge: MQTT down; reconnect attempt 1 in", 0, since)
    # DO/1, DO/3, DO/7 and DO/8 (absent from do_failsafe) are "off", DO/2 "on", DO/4 "hold";
    # DO/5 and DO/6 belong to the rules (IO-7): the failsafe leaves them alone
    trip = gw.wait_log(r"io: failsafe: no Sparkplug session for 5 s: DO on 0x02, off 0xc5, the rest "
                       r"hold", 0, since)
    dos = do_states(nb[2]) if nb else {}
    clear = gw.wait_log(r"io: failsafe: Sparkplug session back", 5, since)
    check("IO-3 / IO-7 failsafe: after failsafe_s (5 s) without a Sparkplug session each DO takes "
          "its safe state (on / off; 'hold' keeps it), except the rule outputs: DO/5 stays on "
          "(DI/2) and DO/6 toggled on by DI/3 without the network; the next NBIRTH shows it",
          pre and lost and trip and 4.5 <= trip - lost <= 6.5 and clear
          and dos == {1: False, 2: True, 3: False, 4: True, 5: True, 6: True, 7: False, 8: False},
          f"before {do_states(pre[2]) if pre else 'no NDATA'}; trip {trip - lost:.1f} s after the loss; "
          f"NBIRTH after the outage {dos}" if lost and trip else
          f"loss logged {bool(lost)}, trip logged {bool(trip)}: {gw.find(r'failsafe', since)[:2]}")
    mqtt_alarm = gw.wait_log(r"buzzer: alarm 'MQTT down': 2 beep\(s\) every 10 s", 0, since)
    back_alarm = gw.wait_log(r"buzzer: alarm 'device offline': 1 beep\(s\)", 5,
                             mqtt_alarm or time.monotonic())
    check("IO-6 buzzer alarm 'MQTT down' (higher priority) after alarm_after_s (5 s) without a "
          "session; back to 'device offline' once the session is up",
          lost and mqtt_alarm and 4.5 <= mqtt_alarm - lost <= 6.5 and back_alarm and nb
          and back_alarm >= nb[0] - 1,
          f"MQTT down alarm {mqtt_alarm - lost:.1f} s after the loss; device offline again "
          f"{back_alarm - nb[0]:+.1f} s from the NBIRTH" if lost and mqtt_alarm and back_alarm and nb
          else " | ".join(gw.find(r"buzzer: ", since)[:3]) or "no buzzer log")
    check("Broker restart: NDEATH (will, session bdSeq) at shutdown, then NBIRTH (new bdSeq, "
          "seq 0) and every DBIRTH",
          will and bdseq_of(will[2]) == b0 and b1 != b0 and births_ok(nb, dbs),
          f"NDEATH bdSeq={bdseq_of(will[2]) if will else None} (session {b0}); NBIRTH bdSeq={b1} "
          f"seq={nb[2].seq if nb else None}; DBIRTH seq "
          f"{ {t.split('/')[-1]: m[2].seq for t, m in dbs.items()} }")

    # MQTT keepalive: a frozen broker (SIGSTOP for longer than 1.5 x keepalive) keeps the TCP
    # connection open but never answers PINGREQ. The gateway must declare the session dead on
    # its own and, once the broker runs again, start a new session.
    keepalive = json.loads(config_json())["mqtt"]["keepalive_s"]
    obs.drain()
    stopped = time.monotonic()
    broker.send_signal(signal.SIGSTOP)
    time.sleep(1.5 * keepalive + 5)
    lost = gw.wait_log(r"No PING_RESP|MQTT down; reconnect attempt", 0, stopped)
    broker.send_signal(signal.SIGCONT)
    resumed = time.monotonic()
    check(f"MQTT keepalive: frozen broker ({1.5 * keepalive + 5:.0f} s SIGSTOP, keepalive "
          f"{keepalive} s) detected by the gateway within 1.5 x keepalive",
          lost and lost - stopped <= 1.5 * keepalive,
          f"session declared dead {lost - stopped:.1f} s after SIGSTOP: "
          f"{gw.find(r'No PING_RESP|MQTT down', stopped)[:2]}" if lost else "never detected")
    nb = obs.wait(nbirth_t, 70)
    time.sleep(3)  # let stale connections queued while frozen play out
    nbs = [m for m in CAPTURE if m[0] >= resumed and m[1] == nbirth_t]
    nb = nbs[-1] if nbs else None
    dbs = {t: m for t in dbirth_topics for m in CAPTURE if nb and m[1] == t and m[0] >= nb[0]}
    b2 = bdseq_of(nb[2]) if nb else None
    deaths = [bdseq_of(m[2]) for m in CAPTURE if m[0] >= resumed and m[1] == ndeath_t]
    attempts = gw.find(r"connecting to .* bdSeq=", stopped)
    check("Frozen broker resumed: NDEATH (will) of the dead session, then NBIRTH with a new "
          "bdSeq and every DBIRTH; the new session stays up",
          b1 in deaths and nb and b2 == (b1 + len(attempts)) % 256 and b2 != b1
          and b2 not in deaths and births_ok(nb, dbs),
          f"NDEATH bdSeqs after SIGCONT {deaths} (dead session {b1}); {len(attempts)} attempts; "
          f"NBIRTH bdSeq={b2} seq={nb[2].seq if nb else None}; DBIRTH seq "
          f"{ {t.split('/')[-1]: m[2].seq for t, m in dbs.items()} }")

    # Issue #5: the backoff restarts from the minimum only once a session has lasted one keepalive
    # period (this one began after 5 failed attempts)
    stable = gw.wait_log(r"session stable for \d+ ms: reconnect backoff reset", keepalive + 5, resumed)
    check("MQTT-2 / issue #5 reconnect backoff reset only after the session lasted one keepalive",
          stable and nb and stable - nb[0] >= keepalive - 0.5,
          f"reset {stable - nb[0]:.1f} s after the NBIRTH (keepalive {keepalive} s): "
          f"{gw.find(r'session stable', resumed)[:1]}" if stable and nb else "no backoff reset logged")

    # 1c. Kill the TCP connection (QEMU process) -> broker publishes the will
    obs.drain()
    gw.kill()
    nd = obs.wait(ndeath_t, 10)
    check("1c. TCP connection killed -> broker publishes NDEATH with the same bdSeq",
          nd and bdseq_of(nd[2]) == b2 and not nd[2].HasField("seq"),
          f"NDEATH bdSeq={bdseq_of(nd[2]) if nd else None} (NBIRTH had {b2})")
    b1 = b2

    # Restart on the same flash: SPB-3 / #161, bdSeq starts at 0 again (it is not persisted)
    gw = Gateway(args, log)
    gw.start()
    nb = obs.wait(nbirth_t, 40)
    b2 = bdseq_of(nb[2]) if nb else None
    check("SPB-3 / issue #161 bdSeq starts at 0 after a power cycle (not persisted; "
          "tck-id-topics-nbirth-bdseq-increment)", nb and b2 == 0,
          f"after power cycle NBIRTH bdSeq={b2}, previous {b1}")

    # IO-3 'hold' across a software restart: the expander keeps its latch (the QEMU model keeps
    # its registers in RTC memory like the chip); DO/4 must come back on, DO/2 ('on') too.
    cold = do_states(nb[2]) if nb else {}
    # IO-4: the counter saved periodically (every 60 s) survives the power cycle
    # (the whole console log: gw.find() only sees the current boot)
    saved = re.findall(r"io: pulse counters saved\r?$", log.read_text(errors="replace"), re.M)
    check("IO-4 pulse counters saved to NVS every counter_save_s (60 s) and restored after a power "
          "cycle", saved and nb and counter_of(nb[2]) == 1,
          f"{len(saved)} periodic saves logged; Counter/1 after the power cycle "
          f"{counter_of(nb[2]) if nb else None} (1 expected)")
    held = set_do(obs, ncmd_t, ndata_t, {4: True})
    obs.drain()
    gw.console("di 1 1")   # DI/1 idle after the power cycle: one more pulse
    pulse = obs.wait(ndata_t, 3, lambda q: counter_of(q) is not None)
    since_reboot = time.monotonic()

    # SPB-2 orderly disconnect: NCMD Reboot -> explicit NDEATH, then reboot. TIME-2: with SNTP
    # silent from here on, the next boot takes its time from the RTC.
    obs.drain()
    SNTP_PAUSED.set()
    cmd_t = time.monotonic()
    obs.publish_cmd(ncmd_t, [{"name": "Node Control/Reboot", "datatype": 11, "boolean_value": True}])
    nd = obs.wait(ndeath_t, 10)
    time.sleep(0.5)
    explicit = re.search(r"Received PUBLISH from GW01 \(d0, q1, r0, m\d+, 'spBv1\.0/Plant1/NDEATH/GW01'",
                         (work / "mosquitto2.log").read_text()) is not None
    nb = obs.wait(nbirth_t, 40)
    # #161: the earlier sessions of this run had bdSeq 0 too: only the NDEATHs since the command
    deaths = [m for m in CAPTURE if m[1] == ndeath_t and m[0] >= cmd_t and bdseq_of(m[2]) == b2]
    check("SPB-2/SPB-11 NCMD Reboot publishes NDEATH explicitly (QoS 1), disconnects and restarts "
          "(#161: bdSeq 0 again)",
          nd and explicit and bdseq_of(nd[2]) == b2 and len(deaths) == 1 and nb
          and bdseq_of(nb[2]) == 0,
          f"NDEATH bdSeq={bdseq_of(nd[2]) if nd else None} sent by the node={explicit}, "
          f"NDEATHs with that bdSeq={len(deaths)} (will discarded if 1), "
          f"NBIRTH after reboot bdSeq={bdseq_of(nb[2]) if nb else None}")
    warm = do_states(nb[2]) if nb else {}
    check("IO-4 pulse counters saved at an orderly restart (NCMD Reboot) and restored",
          pulse and counter_of(pulse[2]) == 2 and nb and counter_of(nb[2]) == 2
          and gw.find(r"io: pulse counters saved before the restart", since_reboot),
          f"Counter/1 before {counter_of(pulse[2]) if pulse else None}, after the reboot "
          f"{counter_of(nb[2]) if nb else None}")
    check("IO-3 'hold' output kept across a software restart (TCA9554 latch read back before the "
          "configuration), off after a power cycle; 'on' output on after both",
          cold.get(4) is False and cold.get(2) is True and held and warm.get(4) is True
          and warm.get(2) is True and warm.get(3) is False
          and gw.find(r"io: DO active-low mask 0x\w+: held across the restart 0x08 of 0x08", since_reboot),
          f"after the power cycle {cold}; after NCMD Reboot {warm}; "
          f"{(gw.find(r'io: DO active-low mask', since_reboot) or ['no log'])[0]}")
    reason1 = metric(nb[2], name="Node Info/Reset Reason") if nb else None
    check("REL-5 NBIRTH 'Node Info/Reset Reason': power-on at first boot, software after NCMD Reboot",
          reason0 and reason1 and reason0.string_value == "power-on"
          and reason1.string_value == "software",
          f"first boot {reason0.string_value if reason0 else None!r}, after reboot "
          f"{reason1.string_value if reason1 else None!r}")
    check_rtc(gw, obs, nb, t0, since_reboot, ndata_t, nb0[0][2])
    check_birth_stamps(t0)
    check_status_api(gw, since_reboot)
    check_panic_during_traffic(gw, obs, slave, nbirth_t)

    # Issue #12 / REL-1: a panic writes a core dump to the "coredump" partition, the gateway
    # restarts ("panic" in NBIRTH) and the dump decodes with esp_coredump against the ELF.
    # Issue #62: QEMU does not emulate the stall of the other core that the panic handler asks
    # for, so a task running there could change the task list and stacks while the ELF writer
    # walks them (truncated dumps, wrong checksums, nested aborts). The emulator's 'panic'
    # parks the other core first, as the chip's stall does: one panic, and its dump must decode.
    # Issue #88: a dump is valid when it decodes on the host AND the firmware's own check at the
    # next boot (CONFIG_ESP_COREDUMP_CHECK_BOOT, what the device reads back from its flash) says
    # its checksum is correct. An invalid dump whose UART log shows another task running inside
    # the dump window (from "Save core dump to flash" to the ROM banner of the restart: on the
    # chip the other core stays stalled through the reboot delay too), or a nested abort, is
    # the emulator's missing stall: it is reported as such and the panic is repeated, at most
    # twice per run. Any other invalid dump (the writer itself failing) fails the check at once.
    off, size = storage_partition("coredump")
    attempts, emulator_retries = [], 2
    other_task = re.compile(r"^[IWE] \(\d+\) (?!esp_core_dump)[\w.-]+: |Re-entered core dump")

    def decode(n):
        core = work / f"coredump-{n}.bin"
        core.write_bytes(pathlib.Path(args.flash).read_bytes()[off:off + size])
        info = subprocess.run([args.idf_python, "-m", "esp_coredump", "--chip", "esp32s3",
                               "info_corefile", "--core", str(core), "--core-format", "raw", args.elf],
                              capture_output=True, text=True, timeout=120)
        (work / f"coredump-info-{n}.txt").write_text(info.stdout + info.stderr)
        (work / "coredump-info.txt").write_text(info.stdout + info.stderr)
        ok = info.returncode == 0 and "panic_task" in info.stdout and "abort" in info.stdout
        return ok, (info.stderr.strip().splitlines() or [""])[-1][:90]

    n = 0
    crashes = 1   # issue #144: the boot-loop counter after the last panic of #65 (3 start safe mode)
    while sum(1 for a in attempts if a["kind"] != "emulator") < args.panics:
        if crashes >= 2:   # a restart by the operator clears the counter
            obs.drain()
            gw.console("restart")
            obs.wait(nbirth_t, 60)
            crashes = 0
        obs.drain()
        since = time.monotonic()
        gw.console("panic")
        crashes += 1
        nb = obs.wait(nbirth_t, 40)
        lines = [l for t, l in gw.lines if t >= since]
        (work / f"uart0-panic-{n}.log").write_text("\n".join(lines))
        saved = any("Core dump has been saved to flash" in l for l in lines)
        boot_check = [l for l in lines if re.search(
            r"esp_core_dump_flash: Core dump data (checksum is correct|check failed)", l)]
        start = next((i for i, l in enumerate(lines) if "Save core dump to flash" in l), None)
        end = next((i for i, l in enumerate(lines) if start is not None and i > start
                    and re.search(r"^ESP-ROM:|rst:0x", l)), len(lines))
        during = [l for l in lines[start + 1:end] if other_task.search(l)] if start is not None else []
        reason = metric(nb[2], name="Node Info/Reset Reason") if nb else None
        decoded, err = decode(n)
        device_ok = bool(boot_check) and "checksum is correct" in boot_check[0]
        good = saved and reason is not None and reason.string_value == "panic" and decoded and device_ok
        kind = "valid" if good else "invalid"
        if not good and during and emulator_retries > 0:
            kind, emulator_retries = "emulator", emulator_retries - 1
        attempts.append({"saved": saved, "reason": reason.string_value if reason else None,
                         "decoded": decoded, "err": err, "kind": kind, "during": during[:2],
                         "device": (boot_check or ["no boot check logged"])[0]})
        n += 1
        if kind == "invalid":
            break
    check_core_dump_api(gw, obs, args, work, nb, since, ndata_t, off)   # issue #132
    gw.kill()

    def describe(i, a):
        text = (f"attempt {i + 1}: saved={a['saved']} reset reason={a['reason']!r} "
                f"decoded={a['decoded']}")
        if a["kind"] != "valid":
            text += f" ({a['err']}; device: {a['device']}"
            if a["kind"] == "emulator":
                text += f"; emulator: another task ran during the dump: {a['during']}"
            text += ")"
        return text

    check("Issue #12 / #62 / #88 one panic -> core dump in flash, restart with 'panic' reset reason, "
          "the dump decodes with esp_coredump (backtrace through panic_task/abort) and the device's "
          "own boot check finds it valid, deterministically (a dump spoilt by the emulator's missing "
          "CPU stall is told apart and repeated, at most twice)",
          all(a["kind"] != "invalid" for a in attempts)
          and sum(1 for a in attempts if a["kind"] == "valid") == args.panics,
          "; ".join(describe(i, a) for i, a in enumerate(attempts)))

    # IO-6 "config_error" alarm: a rejected configuration cannot enable it, so the last valid one
    # left the choice in NVS: a boot of this same flash with a broken config.json beeps 3 times
    gw = Gateway(args, work / "uart0-cfgerr.log",
                 flash_variant(args, work, "cfgerr", {"config.json": b'{"mqtt": '}))
    t_err = time.monotonic()
    gw.start()
    cfg_alarm = gw.wait_log(r"buzzer: alarm 'configuration error': 3 beep\(s\) every 10 s", 30, t_err)
    rejected = gw.wait_log(r"main: configuration rejected", 30, t_err)
    gw.kill()
    check("IO-6 buzzer alarm 'configuration error' in recovery, as enabled by the last valid "
          "configuration (kept in NVS): 3 beeps every 10 s",
          cfg_alarm and rejected,
          " | ".join(gw.find(r"buzzer: alarm|configuration rejected", t_err)[:2]) or "no log")

    check_suback_refused(args, work)

    # Issue #5: a broker that accepts the connection and drops it right after the CONNACK.
    # The backoff must keep growing (1, 2, 4, 8 s, -25 % jitter): it is reset only after a
    # stable session, not on every CONNACK.
    cutter = FakeBroker(MQTT_PORT + 1, "cut")
    gw = Gateway(args, work / "uart0-cutter.log",
                 flash_variant(args, work, "cutter", {"config.json": config_json(MQTT_PORT + 1)}))
    t_cut = time.monotonic()
    gw.start()
    gw.wait_log(r"reconnect attempt 4 in", 40, t_cut)
    delays = [int(x) for x in re.findall(r"in (\d+) ms", " ".join(gw.find(r"reconnect attempt", t_cut)))]
    gw.kill()
    cutter.close()
    grows = len(delays) >= 4 and all(0.75 * 1000 * 2 ** i <= d <= 1000 * 2 ** i
                                     for i, d in enumerate(delays[:4]))
    check("MQTT-2 / issue #5 broker that drops every connection after CONNACK: the backoff keeps "
          "growing (1, 2, 4, 8 s with jitter)", grows,
          f"delays {delays} ms over {len(cutter.accepted)} accepted connections")

    # Issue #71: a broker that grants the subscriptions and resets the connection right after
    # the SUBACK. Each attempt ends without a session; whenever the births ran into the dead
    # connection the gateway says so, and it never claims "session up" for an NBIRTH that did
    # not go out (the esp-mqtt write error comes before it).
    resetter = FakeBroker(MQTT_PORT + 13, "cutsub")
    gw = Gateway(args, work / "uart0-cutsub.log",
                 flash_variant(args, work, "cutsub", {"config.json": config_json(MQTT_PORT + 13)}))
    t_cs = time.monotonic()
    gw.start()
    gw.wait_log(r"reconnect attempt 4 in", 60, t_cs)
    gw.kill()
    resetter.close()
    segments, cur = [], None
    for t, line in gw.lines:
        if t < t_cs:
            continue
        if re.search(r"edge: connecting to ", line):
            cur = []
            segments.append(cur)
        elif cur is not None:
            cur.append(line)
    lying = sum(1 for seg in segments
                if any(re.search(r"Writing failed|Losing qos0|transport error", ln) for ln in seg)
                and any("Sparkplug session up" in ln for ln in seg)
                and [i for i, ln in enumerate(seg) if re.search(r"Writing failed|Losing qos0", ln)][:1]
                < [i for i, ln in enumerate(seg) if "Sparkplug session up" in ln][:1])
    caught = gw.find(r"NBIRTH not published|SUBACK received but the connection is already gone|"
                     r"births incomplete", t_cs)
    ups = gw.find(r"Sparkplug session up", t_cs)
    check("Issue #71 connection reset right after the SUBACK: no 'session up' for births that did "
          "not go out; the gateway says the session did not start and retries",
          len(segments) >= 4 and caught and lying == 0,
          f"{len(segments)} attempts, {len(caught)} without a session "
          f"({(caught or ['none'])[0][:90]}), 'session up' {len(ups)} time(s), with a write error "
          f"before it {lying}")
    obs.close()


if __name__ == "__main__":
    main()
