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
import http.client as httplib
import json
import logging
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
GATEWAYS = []     # every QEMU the harness started (issue #86: did one die on its own?)
RETRIES = []      # issue #86: scenarios repeated once because QEMU died under them
PROCS = []        # brokers and QEMU instances to clean up


def check(name, ok, detail=""):
    results.append({"criterion": name, "ok": bool(ok), "detail": detail})
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
    Units 3..5 (Dead3..Dead5 in the configuration) do not exist: requests to them get no answer."""

    def __init__(self):
        block = lambda: ModbusSequentialDataBlock(0, [0] * 300)  # noqa: E731
        self.ctx = RecordingContext(di=block(), co=block(), hr=block(), ir=block())
        self.bulk = RecordingContext(di=block(), co=block(), hr=block(), ir=block())
        self.bulk.setValues(3, 100, list(range(1000, 1060)))
        self.server_ctx = ModbusServerContext(slaves={1: self.ctx, 2: self.bulk}, single=False)
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
        threading.Thread(target=self._loop, daemon=True).start()

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
        self.srv.close()


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
    while (time.monotonic() < end and
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

    def start(self):
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if self.rate:
            self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)   # inherited
        self.srv.bind(("127.0.0.1", self.port))
        self.srv.listen(4)
        threading.Thread(target=self._loop, args=(self.srv,), daemon=True).start()

    def stop(self):
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
        if hits or time.monotonic() >= end:
            return hits[0] if hits else None
        time.sleep(0.1)


# ---------------------------------------------------------------- MQTT observer

class Observer:
    """Subscribes to spBv1.0/#. A new instance is created after each broker restart."""

    def __init__(self, port=None, auth=None):
        self.q = queue.Queue()
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
        if not self.subscribed.wait(5):
            raise SystemExit("observer: no SUBACK from the broker")

    def close(self):
        self.c.loop_stop()
        self.c.disconnect()

    def _on_msg(self, client, userdata, msg):
        if msg.topic.startswith("spBv1.0/STATE/"):
            return   # SPB-13: the primary host's JSON STATE, published by the harness itself
        p = spb.Payload()
        p.ParseFromString(msg.payload)  # raises on anything the official proto cannot parse
        item = (time.monotonic(), msg.topic, p, bytes(msg.payload))
        CAPTURE.append(item)
        self.q.put(item)

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def wait_all(self, topics, timeout):
        """{topic: first message} for every topic in @topics seen within @timeout."""
        found, end = {}, time.monotonic() + timeout
        while len(found) < len(topics) and time.monotonic() < end:
            try:
                item = self.q.get(timeout=max(0.01, end - time.monotonic()))
            except queue.Empty:
                break
            if item[1] in topics and item[1] not in found:
                found[item[1]] = item
        return found

    def wait(self, topic, timeout, pred=lambda p: True):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                item = self.q.get(timeout=max(0.01, end - time.monotonic()))
            except queue.Empty:
                break
            if item[1] == topic and pred(item[2]):
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
    return m.long_value if m is not None and m.datatype == 8 else None


def set_do(obs, ncmd_t, ndata_t, states):
    """NCMD DO/n = v for every {n: v}; the NDATA that confirms them (or None)."""
    obs.drain()
    obs.publish_cmd(ncmd_t, [{"name": f"DO/{n}", "datatype": 11, "boolean_value": v}
                             for n, v in states.items()])
    return obs.wait(ndata_t, 3, lambda q: all(do_states(q).get(n) == v for n, v in states.items()))


# ---------------------------------------------------------------- QEMU

class QemuGone(ConnectionError):
    """Issue #90: the emulator is gone (its console write failed)."""


class Gateway:
    def __init__(self, args, log_path, flash=None):
        self.args, self.log_path = args, log_path
        self.flash = flash or args.flash
        self.proc, self.sock, self.lines = None, None, []
        self.killed = False          # the harness ended it (kill())
        self.gone_before_kill = None  # returncode if QEMU had already exited when killed

    def start(self):
        a = self.args
        # With the octal PSRAM, -m is its size (the board's 8 MB): with 32M QEMU maps 31 MB and
        # leaves no virtual address space for the flash mmap (the partitions are not found).
        cmd = [a.qemu, "-M", "esp32s3", "-m", "8M" if a.psram else "32M", "-nographic",
               "-drive", f"file={self.flash},if=mtd,format=raw",
               "-drive", f"file={a.efuse},if=none,format=raw,id=efuse",
               "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
               "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
               *(["-global", "driver=ssi_psram,property=is_octal,value=true"] if a.psram else []),
               "-nic", f"user,model=open_eth,hostfwd=tcp:127.0.0.1:{HTTP_PORT}-:80,"
                       f"hostfwd=tcp:127.0.0.1:{HTTPS_PORT}-:443,"
                       f"hostfwd=tcp:127.0.0.1:{MBTCP_PORT}-:502",
               "-chardev", f"socket,id=con,host=127.0.0.1,port={CONSOLE_PORT},server=on,wait=on",
               "-serial", "chardev:con",
               "-chardev", f"socket,id=rs485,host=127.0.0.1,port={MODBUS_PORT},reconnect-ms=500",
               "-serial", "chardev:rs485",
               "-monitor", "none"]
        err = pathlib.Path(str(self.log_path) + ".qemu")
        self.err_offset = err.stat().st_size if err.exists() else 0   # this boot's stderr only
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=open(err, "ab"))
        PROCS.append(self.proc)
        GATEWAYS.append(self)
        for _ in range(100):
            try:
                self.sock = socket.create_connection(("127.0.0.1", CONSOLE_PORT))
                break
            except OSError:
                time.sleep(0.1)
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
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self.lines.append((time.monotonic(), line.decode(errors="replace").rstrip()))

    def console(self, text):
        try:
            self.sock.sendall(text.encode() + b"\n")
        except OSError as e:   # issue #90: BrokenPipeError when the emulator is gone
            time.sleep(0.5)    # let the process be reaped so its exit status is known
            raise QemuGone(self.qemu_died() or f"QEMU console closed ({e})") from e

    def kill(self):
        self.gone_before_kill = self.proc.poll()
        self.killed = True
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait()
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
                and ln.strip() != "Adding SPI flash device"][-3:]
        how = f"signal {-code}" if code < 0 else f"exit status {code}"
        return (f"QEMU terminó ({how}, {self.log_path.name}); stderr: "
                f"{' | '.join(tail) if tail else 'no error message'}")

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
            while time.monotonic() < end and None in hits.values():
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
            if time.monotonic() >= end:
                return None
            time.sleep(0.05)


# ---------------------------------------------------------------- HTTP (CFG-3, CFG-4)

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
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            c = httplib.HTTPSConnection("127.0.0.1", HTTPS_PORT, timeout=timeout, context=ctx)
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
        if r[0] is not None or time.monotonic() >= end:
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


def write_otadata(path):
    """Issue #85: a valid first otadata entry (ota_seq 1 = ota_0, state VALID, CRC32 of the
    sequence as esp_rom_crc32_le computes it), as the 2nd stage bootloader writes it on the
    first boot of an erased otadata. With QEMU's octal ssi_psram that first write crashes QEMU
    (SIGSEGV in psram_quad_read: docs/upstream-reports.md section 4), so the PSRAM variant boots
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
    """MQTT-4 / MQTT-5 against a mosquitto with three listeners: TLS 1.3 only (server
    authentication), TLS 1.2 only with client certificates required (mutual TLS), and plain TCP
    for the observer. Separate boots with their own edge node ids:
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
    gw = boot_variant(args, work, "tlsmissing", node_config(node, port=p13, tls={
        "enabled": True, "ca_file": "missing.pem"}))
    missing = gw.wait_log(r"mqtt\.tls\.ca_file: /cfg/missing\.pem: no existe .*MQTT not started", 30, t0)
    attempts = gw.find(r"connecting to .* as 'GW15'", t0)
    gw.kill()
    check("MQTT-4 a CA file missing from LittleFS is reported and no connection is attempted",
          missing and not attempts,
          (gw.find(r"mqtt\.tls\.ca_file", t0) or ["no error logged"])[0])
    check_ram_margin(args, work, obs, p13, pki["ca.pem"].read_bytes())
    check_tls_during_validation(args, work, obs, p13, pki["ca.pem"].read_bytes())
    obs.close()
    broker.terminate()
    broker.wait()


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
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        session = httplib.HTTPSConnection("127.0.0.1", HTTPS_PORT, timeout=15, context=ctx)
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
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    auth = {"Authorization": "Basic " + base64.b64encode(f"admin:{WEB_PASSWORD}".encode()).decode()}
    session = httplib.HTTPSConnection("127.0.0.1", HTTPS_PORT, timeout=VALIDATION_HOLD_MS / 1000 + 30,
                                      context=ctx)
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
    level 5 and Clean Start, NBIRTH, the NCMD subscription working (rebirth), and the NDEATH will
    published as soon as the TCP connection dies (Will Delay Interval 0)."""
    node = "GW18"
    obs = Observer()
    t0 = time.monotonic()
    gw = boot_variant(args, work, "mqtt5", node_config(node, protocol="5"))
    nb = obs.wait(f"{T}/NBIRTH/{node}", 40)
    attempt = gw.find(r"connecting to 10\.0\.2\.2:%d MQTT 5 as 'GW18'" % MQTT_PORT, t0)
    v5 = re.search(r"New client connected from \S+ as GW18 \(p5, c1, k10\)",
                   pathlib.Path(mosquitto_log).read_text())
    obs.drain()
    obs.publish_cmd(f"{T}/NCMD/{node}", [{"name": "Node Control/Rebirth", "datatype": 11,
                                          "boolean_value": True}])
    rb = obs.wait(f"{T}/NBIRTH/{node}", 5)
    killed = time.monotonic()
    gw.kill()
    will = obs.wait(f"{T}/NDEATH/{node}", 10)
    obs.close()
    check("MQTT-8 MQTT 5.0 (mqtt.protocol \"5\"): CONNECT protocol level 5 with Clean Start, NBIRTH, "
          "NCMD rebirth received over the v5 subscription, NDEATH will with the session's bdSeq "
          "right after the connection dies (Will Delay 0)",
          nb and attempt and v5 and rb and will and bdseq_of(will[2]) == bdseq_of(nb[2])
          and will[0] - killed < 5,
          f"NBIRTH {'yes' if nb else 'no'}; mosquitto: {v5.group(0) if v5 else 'no p5 connection'}; "
          f"rebirth {'yes' if rb else 'no'}; will "
          f"{f'{will[0] - killed:.1f} s after the kill, bdSeq {bdseq_of(will[2])}' if will else 'missing'}")


def run_max_packet(args, work, slave):
    """Issue #73 (GW26, MQTT 5) against a mosquitto with max_packet_size 2000, which it
    announces in the CONNACK: NBIRTH and the small DBIRTH of Meter1 go out; the DBIRTH of Bulk
    (about 2.7 KB) is not sent, with a clear error and the shell showing it, and the session
    stays up (no reconnection loop)."""
    node, port = "GW26", MQTT_PORT + 14
    conf = work / "broker-maxpkt.conf"
    conf.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\nmax_packet_size 2000\n")
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
                      r"Packet Size of 2000 \(by \d+\): not published", 15, t0)
    time.sleep(8)
    # Issue #76: Bulk is polled (and reported to the Sparkplug task) every cycle, but its DBIRTH
    # is tried once per session, not rebuilt and rejected each time: the count stays at 1; a
    # rebirth tries it once more
    asked, h = gw.shell("status", [r"^limit\s+broker Maximum Packet Size 2000 B; 1 message\(s\) too "
                                   r"large, not published \(last: spBv1\.0/Plant1/DBIRTH/GW26/Bulk\)"])
    shown = list(h.values())[0]
    bulk_cycles = len([r for t, r in zip(list(slave.times), list(slave.reads)) if r[0] == 2 and t >= t0])
    gw.console("rebirth")
    gw.wait_log(r"^rebirth: NBIRTH and every DBIRTH requested", 5, asked)
    time.sleep(3)
    _, h2 = gw.shell("status", [r"^limit\s+broker Maximum Packet Size 2000 B; 2 message\(s\) too large"])
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
          f"{len(bulk)}; {(gw.find(r'Maximum Packet Size of 2000', t0) or ['no error'])[0][:150]}; "
          f"{len(attempts)} connection(s); " + " | ".join(gw.find(r"^limit\s", asked)))


# ---------------------------------------------------------------- primary host (SPB-13)

def host_state(obs, host, online, ts, retain=True):
    """Publish a Sparkplug 3.0 STATE message for @host as its primary host application would."""
    obs.c.publish(f"spBv1.0/STATE/{host}", json.dumps({"online": online, "timestamp": ts}),
                  qos=1, retain=retain).wait_for_publish(5)


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
    gw.kill()
    obs.wait(ndeath_t, 5)
    obs.c.publish(f"spBv1.0/STATE/{host}", b"", qos=1, retain=True).wait_for_publish(5)
    obs.close()


# ---------------------------------------------------------------- store & forward (SPB-14)

def run_store_forward(args, work, slave):
    """SPB-14 in one boot (GW20, sparkplug.store_forward) connected through a Relay (port +9) to
    the main mosquitto; stopping the relay takes the broker away from the gateway while the
    observer stays subscribed. While it is away Voltage takes three values (each beyond the 0.1
    deadband) and DI/3 goes on and off. Back: NBIRTH, DBIRTH with the current value, then the
    stored changes as DDATA/NDATA with is_historical, in order and with their sample times.
    Without PSRAM (the default QEMU build) the small internal-RAM ring is used; with --psram
    (issue #85) the board's 40000-sample ring in PSRAM."""
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
        time.sleep(1.6)   # poll_ms 1000: every value is read at least once
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
    for k in (0, 1):
        cfg["devices"].append({"name": f"Pump{'AB'[k]}", "unit_id": 1, "type": "Pump", "poll_ms": 500,
                               "tags": [{"name": "Speed", "area": "holding", "address": 232 + k,
                                         "type": "uint16", "units": "rpm", "eng_high": 3000,
                                         "writable": True},
                                        {"name": "Run", "area": "coil", "address": 7 + k, "type": "bool"}]})
        slave.ctx.setValues(3, 232 + k, [1000 + k])
    cfg["devices"].append({"name": "Raw", "unit_id": 1, "poll_ms": 500, "tags": [
        {"name": "Block", "area": "holding", "address": 240, "type": "bytes", "length": 4,
         "writable": True}]})
    slave.ctx.setValues(3, 240, [0x0001, 0x0203, 0x0405, 0x0607])
    t0 = time.monotonic()
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
    slave.ctx.setValues(3, 233, [1777])
    slave.ctx.setValues(3, 240, [0x00FF, 0x00FE, 0x00FD, 0x00FC])
    time.sleep(2.5)
    relay.start()
    nb2 = obs.wait(f"{T}/NBIRTH/{node}", 70)
    time.sleep(3)
    after = [x for x in CAPTURE if nb2 and x[0] >= nb2[0] and x[1].split("/")[3:4] == [node]]
    hist_pump = [x for x in after if x[1] == f"{T}/DDATA/{node}/PumpB" and x[2].metrics
                 and x[2].metrics[0].is_historical and x[2].metrics[0].datatype == 19]
    hist_raw = [x for x in after if x[1] == f"{T}/DDATA/{node}/Raw"
                and any(y.is_historical and y.datatype == 17 for y in x[2].metrics)]
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
    while time.monotonic() < end:
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


def run_store_forward_spill(args, work, slave):
    """SPB-14 spill (GW23): store_forward with sd enabled; QEMU has no SDMMC, so the build puts
    the "card" in a LittleFS directory (CONFIG_GW_SF_SPILL_LITTLEFS). While the relay keeps the
    broker away, the harness changes all 60 Bulk registers twice a second: about 60 samples per
    poll overflow the 512-sample RAM ring, which spills its oldest chunks to the card. After the
    reconnection every stored sample comes back as historical DDATA, oldest first (spill, then
    ring), with no sample lost (the shell: nothing overwritten, nothing deleted unsent).
    With --psram (issue #85) the ring has 40000 samples in PSRAM: the same load stays in it and
    nothing spills, which is the board's behaviour for an outage this short."""
    node, port = "GW23", MQTT_PORT + 11
    ddata_t = f"{T}/DDATA/{node}/{BULK}"
    relay = Relay(port, MQTT_PORT)
    obs = Observer()
    cfg = node_config(node, port=port)
    cfg["sparkplug"]["store_forward"] = {"enabled": True, "sd": {"enabled": True, "max_kb": 128}}
    t0 = time.monotonic()
    gw = boot_variant(args, work, "sfsd", cfg)
    nb = obs.wait(f"{T}/NBIRTH/{node}", 40)
    obs.wait(f"{T}/DBIRTH/{node}/{BULK}", 10)
    spill_log = gw.find(r"store & forward: spill to LittleFS /cfg/sf .*: \d+ segments of \d+ samples", t0)
    since = time.monotonic()
    relay.stop()
    gw.wait_log(r"MQTT down; reconnect attempt", 10, since)
    value = 2000
    end = time.monotonic() + 16
    while time.monotonic() < end:
        value += 1
        slave.bulk.setValues(3, 100, [value] * 60)
        time.sleep(0.5)
    if args.psram:
        want = [r"^spill\s+on: 0 samples on the card, 0 moved there, 0 deleted unsent",
                r"^store\s+[1-9]\d* of 40000 samples waiting \(PSRAM\), .* 0 overwritten"]
    else:
        want = [r"^spill\s+on: [1-9]\d* samples on the card, [1-9]\d* moved there, 0 deleted unsent",
                r"^store\s+[1-9]\d* of 512 samples waiting \(internal RAM\), .* 0 overwritten"]
    asked, h = gw.shell("status", want)
    spilled = all(h.values())
    waiting = [int(x) for x in re.findall(r"^store\s+(\d+) of", "\n".join(gw.find(r"^store\s", asked)), re.M)]
    relay.start()
    nb2 = obs.wait(f"{T}/NBIRTH/{node}", 70)
    deadline = time.monotonic() + 60
    asked2 = time.monotonic()
    done = None
    while time.monotonic() < deadline and not done:
        _, h = gw.shell("status", [r"^store\s+0 of \d+ samples waiting"], timeout=2, tries=1)
        done = list(h.values())[0]
    time.sleep(1)
    hist = [mm for m in CAPTURE if nb2 and m[0] >= nb2[0] and m[1] == ddata_t for mm in m[2].metrics
            if mm.is_historical]
    total = sum(1 for m in CAPTURE if nb2 and m[0] >= nb2[0] and m[1].split("/")[3:4] == [node]
                for mm in m[2].metrics if mm.is_historical)
    reg100 = [mm.int_value for mm in hist if mm.alias == ALIAS["Bulk0"]]
    ts = [mm.timestamp for mm in hist if mm.alias == ALIAS["Bulk0"]]
    final = gw.find(r"^store\s|^spill\s", asked2)[-2:]
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node}", 5)
    obs.close()
    relay.stop()
    slave.bulk.setValues(3, 100, list(range(1000, 1060)))   # what the main scenario expects
    check("SPB-14 spill to the card (LittleFS stand-in on QEMU): the full 512-sample ring spills "
          "its oldest samples; after the reconnection all of them come back as historical DDATA, "
          "oldest first, none overwritten or deleted" if not args.psram else
          "SPB-14 with the PSRAM ring (issue #85): more samples than the internal ring holds stay in "
          "the 40000-sample PSRAM ring, nothing spills to the card; after the reconnection all of "
          "them come back as historical DDATA, oldest first, none overwritten",
          nb and spill_log and spilled and waiting and waiting[0] > 512 and nb2 and done
          and total >= waiting[0] and reg100 == sorted(reg100) and ts == sorted(ts)
          and len(set(reg100)) == len(reg100),
          f"{(spill_log or ['no spill log'])[0]}; waiting before the reconnection {waiting}; "
          f"{total} historical metrics after it ({len(hist)} of Bulk), register 100: {reg100[:3]}..{reg100[-2:]} "
          f"({len(reg100)} samples, in order {reg100 == sorted(reg100)}); "
          + " | ".join(gw.find(r"^spill\s", asked) + final))


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
        sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        if not plain:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            sock = ctx.wrap_socket(sock)
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
    upload early (the client's send fails), not drain it. Over TLS esp_http_server's drain of
    the unread body also ends at the first moment nothing is buffered (esp-tls sockets are
    non-blocking), so in QEMU the GET is not held even without the fix: the cut is what shows
    it there, and on a link the client keeps full the drain lasted as long as the upload.

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
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    session = httplib.HTTPSConnection("127.0.0.1", HTTPS_PORT, timeout=15, context=ctx)

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

    base_st, base = get_status(False)
    for name, (hdrs, plain) in cases.items():
        up = {}
        th = threading.Thread(target=slow_upload, args=(hdrs, total, rate, up, plain), daemon=True)
        th.start()
        time.sleep(0.5)
        st, took = get_status(plain)
        th.join(20)
        res[name] = (st, took, dict(up), 301 if plain else 200)
    session.close()
    asked, h = gw.shell("status", [r"^web\s+HTTPS .*, bodies cut [1-9]\d*$"])
    cut_line = (gw.find(r"^web\s", asked) or [""])[-1]
    m = re.search(r"bodies cut (\d+)", cut_line)
    ok = base_st == 200 and m is not None and all(
        st == want and took < 1.0 and up.get("sent", total) < total and up.get("error")
        and up.get("duration", 99) < 5 for st, took, up, want in res.values())
    check("Issue #106 a refused 1.5 MB upload (HTTPS 401 without credentials, 403 from another "
          "site; the HTTP port's redirect) is cut, not drained: a GET half a second into it "
          "answers within 1 s",
          ok, f"GET without an upload {base_st} in {base:.2f} s; " + "; ".join(
              f"{k}: GET {st} in {took:.2f} s, upload stopped after {up.get('sent', 0)} of {total} "
              f"B in {up.get('duration', 0):.1f} s ({up.get('error')}), reply seen "
              f"{up.get('status')!r}" for k, (st, took, up, _) in res.items())
          + f"; {cut_line.strip()}")

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
                                   r"cross-site writes refused 9, bodies cut \d+$"])
    counted = list(h.values())[0]
    # the web UI: X-Requested-With and its own Origin (and Referer); a script: the header only
    st_ui, _, b_ui = http("PUT", dry, doc, headers={"Origin": me, "Referer": me + "/"})
    st_ref, _, _ = http("PUT", dry, doc, headers={"Referer": me + "/"})
    st_cli, _, _ = http("PUT", dry, doc)
    st_page, _, page = http("GET", "/")
    ui_sends = page.count(b'"X-Requested-With":"gateway"')
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
          "Referer) are accepted, the page sends the header on every write (API helper and image "
          "upload), and a script with the header and no Origin is accepted",
          st_ui == 200 and (as_json(b_ui) or {}).get("saved") is False and st_ref == 200
          and st_cli == 200 and st_page == 200 and ui_sends == 2,
          f"UI PUT dry run {st_ui} {b_ui[:60]!r}; Referer only {st_ref}; no Origin {st_cli}; page "
          f"{st_page} with the header in {ui_sends} request(s)")


# ---------------------------------------------------------------- scenario

# ---------------------------------------------------------------- FW-1 firmware updates

APP_DESC_MAGIC = 0xABCD5432


def fw_image(src, version, dst, corrupt=False):
    """Copy of the app image @src with esp_app_desc_t.version = @version: checksum and the
    appended SHA-256 recomputed so the bootloader and esp_ota_end() accept it (@corrupt: the
    checksum is wrong, so the image must be rejected). @src must be unsigned."""
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
    for o, n in zip(b[vo:vo + 32], new):
        x ^= o ^ n
    b[vo:vo + 32] = new
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

    log = []
    cuts = {}   # path -> (file in root, bytes of it sent before the connection is closed)

    def close(self):
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


def run_ota(args, work):
    """FW-1 in one gateway (GW40) with its own broker: updates by NCMD (HTTPS download with the
    CA from ota.ca_file) and by web upload; bad images rejected without a restart; a new image
    on trial confirmed by its first session; rolled back when no session comes up before the
    timeout (broker stopped) and when it resets during its trial (panic)."""
    node, port, fw_port = "GW40", MQTT_PORT + 20, MQTT_PORT + 21
    pki = make_pki(work)
    fwdir = work / "fw"
    fwdir.mkdir(exist_ok=True)
    app = pathlib.Path(args.elf).with_suffix(".bin")
    unsigned = pathlib.Path(args.elf).with_name(app.stem + "-unsigned.bin")
    src = unsigned if unsigned.exists() else app
    v1 = None
    fw_image(src, "e2e-v2", fwdir / "v2.bin")
    fw_image(src, "e2e-v3", fwdir / "v3.bin")
    fw_image(src, "e2e-bad", fwdir / "bad.bin", corrupt=True)
    images = {k: fwdir / f"{k}.bin" for k in ("v2", "v3", "bad")}
    signed = bool(args.signing_key)
    if signed:
        # FW-2: the build requires signed images; "foreign" is valid but signed with another key
        fw_image(src, "e2e-foreign", fwdir / "foreign.bin")
        other = work / "foreign_signing_key.pem"
        if not other.exists():
            subprocess.run([args.idf_python, "-m", "espsecure", "generate-signing-key", "--version",
                            "2", "--scheme", "rsa3072", str(other)], check=True, capture_output=True)
        for k, key in (("v2", args.signing_key), ("v3", args.signing_key),
                       ("bad", args.signing_key), ("foreign", str(other))):
            # --keyfile takes several values: an explicit --output keeps the data file apart
            unsigned_k = fwdir / f"{k}-unsigned.bin"
            (fwdir / f"{k}.bin").rename(unsigned_k)
            subprocess.run([args.idf_python, "-m", "espsecure", "sign-data", "--version", "2",
                            "--keyfile", key, "--output", str(fwdir / f"{k}.bin"),
                            str(unsigned_k)], check=True, capture_output=True)
            images[k] = fwdir / f"{k}.bin"
    server = FirmwareServer(fw_port, fwdir, pki["server.crt"], pki["server.key"])
    conf = work / "broker-ota.conf"
    conf.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\n")
    broker = start_broker(conf, work / "mosquitto-ota.log")
    obs = Observer(port)
    cfg = node_config(node, port=port)
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    cfg["ota"] = {"ca_file": "ca.pem"}
    t0 = time.monotonic()
    gw = boot_variant(args, work, "ota", cfg, {"ca.pem": pki["ca.pem"].read_bytes()})
    nbirth_t, ndata_t, ncmd_t = f"{T}/NBIRTH/{node}", f"{T}/NDATA/{node}", f"{T}/NCMD/{node}"
    try:
        nb = obs.wait(nbirth_t, 40)
        if not check("FW-1 NBIRTH declares Node Control/Firmware Update (String) and the update's "
                     "state and progress", nb and metric(nb[2], alias=40) is not None
                     and metric(nb[2], alias=40).datatype == 12 and fw_state(nb[2]) == "idle"
                     and metric(nb[2], alias=42) is not None,
                     f"state {fw_state(nb[2])!r}" if nb else "no NBIRTH"):
            return
        v1 = metric(nb[2], name="Node Info/Firmware").string_value
        gw.wait_log(r"edge: time synchronised", 10, t0)   # the image server's certificate dates

        def update(url):
            obs.publish_cmd(ncmd_t, [{"name": "Node Control/Firmware Update", "datatype": 12,
                                      "string_value": url}])

        def states(since):
            return [fw_state(m[2]) for m in CAPTURE
                    if m[0] >= since and m[1] in (ndata_t, nbirth_t) and fw_state(m[2]) is not None]

        # refused before any download: plain HTTP; an image with a stale checksum: rejected after
        # the download, no restart
        asked = time.monotonic()
        update(f"http://10.0.2.2:{fw_port}/v2.bin")
        refused = obs.wait(ndata_t, 10, lambda p: (fw_state(p) or "").startswith("refused: only https"))
        update(f"https://10.0.2.2:{fw_port}/bad.bin")
        bad = obs.wait(ndata_t, 120, lambda p: (fw_state(p) or "").startswith("failed: image e2e-bad "
                                                                             "rejected"))
        foreign = True
        if signed:
            # FW-2: a valid image signed with a key other than the running firmware's
            update(f"https://10.0.2.2:{fw_port}/foreign.bin")
            foreign = obs.wait(ndata_t, 120, lambda p: (fw_state(p) or "").startswith(
                "failed: image e2e-foreign rejected (checksum or signature)"))
        time.sleep(2)
        still = not any(m[1] == f"{T}/NDEATH/{node}" for m in CAPTURE if m[0] >= asked)
        check("FW-1 bad updates refused without a restart: http:// refused, an image that fails "
              "verification rejected after the download (the running firmware stays)"
              + ("; FW-2: an image signed with another key rejected (signature check of the OTA "
                 "image, no eFuse)" if signed else ""),
              refused and bad and still and foreign,
              " | ".join(dict.fromkeys(states(asked))) + f"; restarted: {not still}"
              + (f"; {(gw.find(r'secure_boot_v2|[Ss]ignature', asked) or ['no signature log'])[0][:120]}"
                 if signed else ""))

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

        if args.simulate_qemu_death == "ota" and not RETRIES:
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
                            "refused: URL longer than 255 characters")
        check("Issue #87 a firmware URL over the limits is refused with its reason, never cut: 300 "
              "characters (more than the codec's 256) -> NCMD undecodable, 256 -> refused (255 at "
              "most)", undecodable and too_long,
              f"{(gw.find(r'NCMD: undecodable payload', asked) or ['no log'])[0][:140]}; state "
              f"{fw_state(too_long[2]) if too_long else None!r}")

        # NCMD with the URL: download with progress, restart into it, trial, confirmed; a
        # 250-character URL (issue #87: it must arrive whole)
        asked = time.monotonic()
        update(long_url(250))
        nb2 = obs.wait(nbirth_t, 240, lambda p: metric(p, name="Node Info/Firmware").string_value
                       == "e2e-v2")
        conf2 = obs.wait(ndata_t, 20, lambda p: fw_state(p) == "confirmed: e2e-v2")
        seen = states(asked)
        prog = [int(metric(m[2], alias=42).long_value or metric(m[2], alias=42).int_value)
                for m in CAPTURE if m[0] >= asked and m[1] == ndata_t and metric(m[2], alias=42)]
        took = [ln for ln in gw.find(r"fw: downloading firmware e2e-v2", asked)]
        check("FW-1 update by NCMD over HTTPS (CA from ota.ca_file, a 250-character URL): progress as NDATA, restart into "
              "the new image, on trial until its first session, then confirmed",
              nb2 and conf2 and "rebooting into e2e-v2" in seen and fw_state(nb2[2]) ==
              "on trial: e2e-v2" and prog and max(prog) == 100 and len(prog) >= 5 and took,
              f"states {list(dict.fromkeys(seen))}; progress {prog[:12]}; "
              f"{nb2[0] - asked:.1f} s to the new NBIRTH" if nb2 else f"states {seen}")
        _, h = gw.shell("status", [r"^ota\s+e2e-v2 in ota_1, other slot \S+ \| firmware e2e-v2 "
                                   r"confirmed"])
        # a power cut after the confirmation: still the new image
        gw.kill()
        obs.drain()
        gw = Gateway(args, work / "uart0-ota.log", gw.flash)
        gw.start()
        nb3 = obs.wait(nbirth_t, 40)
        check("FW-1 a confirmed image stays after a power cut (shell shows the slots)",
              nb3 and metric(nb3[2], name="Node Info/Firmware").string_value == "e2e-v2"
              and fw_state(nb3[2]) == "idle" and list(h.values())[0],
              f"NBIRTH firmware {metric(nb3[2], name='Node Info/Firmware').string_value!r}, state "
              f"{fw_state(nb3[2])!r}" if nb3 else "no NBIRTH")

        # web upload of v3, then the broker goes away before the new image reaches a session:
        # rolled back when the trial times out
        timeout_s = sdkconfig_int(args.sdkconfig, "GW_FW_CONFIRM_TIMEOUT_S")
        gw.wait_log(r"edge: time synchronised", 10, nb3[0] - 5 if nb3 else t0)
        asked = time.monotonic()
        st, _, body = http("POST", "/api/firmware", images["v3"].read_bytes(),
                           headers={"Content-Type": "application/octet-stream"}, timeout=120)
        death = obs.wait(f"{T}/NDEATH/{node}", 20)
        broker.terminate()
        broker.wait()
        obs.close()
        trial = gw.wait_log(r"fw: firmware e2e-v3 \(ota_0\) on trial", 30, asked)
        back = gw.wait_log(r"fw: firmware e2e-v3 rejected \(no Sparkplug session before the timeout\)",
                           timeout_s + 30, asked)
        # issue #108 B4: the rollback runs in the trial task, not esp_timer; the emulator build
        # logs the stack the verification of the previous image leaves (margin >= 1 KB)
        gw.wait_log(r"fw: rollback stack: \d+ B free", 20, back or asked)
        st_line = gw.find(r"fw: rollback stack: \d+ B free in task \S+", back or asked)
        st_m = re.search(r"rollback stack: (\d+) B free in task (\S+)", st_line[0]) if st_line else None
        broker = start_broker(conf, work / "mosquitto-ota.log")
        obs = observer_retry(port)
        nb4 = obs.wait(nbirth_t, 60)
        check(f"FW-1 web upload; a new image without a Sparkplug session for {timeout_s} s (broker "
              "down) goes back to the previous one, which says so; issue #108 B4: the rollback "
              "(verification of the previous image) runs in the trial task with at least 1 KB of "
              "stack left",
              st == 200 and death and trial and back and nb4
              and metric(nb4[2], name="Node Info/Firmware").string_value == "e2e-v2"
              and fw_state(nb4[2]) == "rolled back from e2e-v3"
              and st_m and st_m.group(2) == "fw_trial" and int(st_m.group(1)) >= 1024,
              f"POST {st} {body[:60]!r}; on trial at {trial - asked:.1f} s, rolled back at "
              f"{back - trial:.1f} s; NBIRTH {fw_state(nb4[2]) if nb4 else None!r}; "
              f"{st_m.group(0) if st_m else 'no stack line'}"
              if trial and back else f"POST {st} {body[:80]!r}; trial {trial}, back {back}")

        # NCMD v3 again; a reset during the trial (panic): the bootloader goes back
        gw.wait_log(r"edge: time synchronised", 10, nb4[0] - 5 if nb4 else t0)
        asked = time.monotonic()
        update(f"https://10.0.2.2:{fw_port}/v3.bin")
        death = obs.wait(f"{T}/NDEATH/{node}", 240)
        broker.terminate()
        broker.wait()
        obs.close()
        trial = gw.wait_log(r"fw: firmware e2e-v3 \(ota_0\) on trial", 30, asked)
        # the trial is logged before the shell starts (issue #101): the panic comes where it came
        # before, once gw_fw_init() ran (the shell is up, the configuration loaded; a panic in the
        # middle of the configuration load once froze QEMU in the core dump write)
        gw.wait_log(r"fw: firmware e2e-v3 in ota_0; updates by URL", 30, trial or asked)
        gw.console("panic")
        back = gw.wait_log(r"fw: firmware e2e-v3 \(ota_0\) was rejected or did not confirm itself; "
                           r"running e2e-v2 again", 60, trial or asked)
        broker = start_broker(conf, work / "mosquitto-ota.log")
        obs = observer_retry(port)
        nb5 = obs.wait(nbirth_t, 60)
        check("FW-1 a reset during the trial (panic) makes the bootloader boot the previous image",
              death and trial and back and nb5
              and metric(nb5[2], name="Node Info/Firmware").string_value == "e2e-v2"
              and metric(nb5[2], name="Node Info/Reset Reason").string_value == "panic"
              and fw_state(nb5[2]) == "rolled back from e2e-v3",
              f"NBIRTH firmware {metric(nb5[2], name='Node Info/Firmware').string_value!r}, reset "
              f"{metric(nb5[2], name='Node Info/Reset Reason').string_value!r}, state "
              f"{fw_state(nb5[2])!r}" if nb5 else f"trial {trial}, back {back}")
    finally:
        gw.kill()
        obs.close()
        server.close()
        broker.terminate()
        broker.wait()


def signed_image(args, src, version, fwdir, name):
    """fw_image() of @src with @version as @fwdir/@name.bin, signed with the build's test key
    when the build requires signed images (FW-2)."""
    fw_image(src, version, fwdir / f"{name}-unsigned.bin")
    if args.signing_key:
        subprocess.run([args.idf_python, "-m", "espsecure", "sign-data", "--version", "2",
                        "--keyfile", args.signing_key, "--output", str(fwdir / f"{name}.bin"),
                        str(fwdir / f"{name}-unsigned.bin")], check=True, capture_output=True)
    else:
        shutil.copy(fwdir / f"{name}-unsigned.bin", fwdir / f"{name}.bin")
    return fwdir / f"{name}.bin"


def run_ota_trial(args, work):
    """Issues #99 and #101 (GW43, MQTT 5) against a broker whose Maximum Packet Size (2400) is
    below the Bulk DBIRTH, as in run_max_packet: every session leaves that DBIRTH out.
    (1) A new image uploaded through the web: its sessions are incomplete, so none confirms it,
    and the timeout rolls it back (#99 item 2). (2) Another image, also on trial for that reason:
    an NCMD update refused during the trial keeps the trial phase and its time left (#101), and a
    configuration saved through the web confirms the image before the restart (#99 item 4): it
    comes back as the new image, not rolled back."""
    node, port = "GW43", MQTT_PORT + 25
    app = pathlib.Path(args.elf).with_suffix(".bin")
    unsigned = pathlib.Path(args.elf).with_name(app.stem + "-unsigned.bin")
    src = unsigned if unsigned.exists() else app
    fwdir = work / "fw-trial"
    fwdir.mkdir(exist_ok=True)
    images = {k: signed_image(args, src, f"e2e-{k}", fwdir, k) for k in ("t2", "t3")}
    conf = work / "broker-trial.conf"
    # 2400: the NBIRTH (about 2.1 KB with the longest update states) fits, the Bulk DBIRTH (about
    # 2.7 KB) does not
    conf.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\nmax_packet_size 2400\n")
    broker = start_broker(conf, work / "mosquitto-trial.log")
    obs = Observer(port)
    cfg = node_config(node, port=port, protocol="5")
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
    timeout_s = sdkconfig_int(args.sdkconfig, "GW_FW_CONFIRM_TIMEOUT_S")
    nbirth_t, ndata_t, ncmd_t = f"{T}/NBIRTH/{node}", f"{T}/NDATA/{node}", f"{T}/NCMD/{node}"

    def fw(item):
        return metric(item[2], name="Node Info/Firmware").string_value if item else None

    def upload(name):
        return http("POST", "/api/firmware", images[name].read_bytes(),
                    headers={"Content-Type": "application/octet-stream"}, timeout=120)

    t0 = time.monotonic()
    gw = boot_variant(args, work, "trial", cfg)
    try:
        nb = obs.wait(nbirth_t, 40)
        v1 = fw(nb)
        if not check("Issue #99 GW43 runs on the broker with Maximum Packet Size 2400 (the Bulk "
                     "DBIRTH does not fit)", nb and v1,
                     f"NBIRTH {'yes' if nb else 'no'}"):
            return

        # 1. a session without every DBIRTH does not confirm a new image
        wait_http("/api/firmware", 20)
        asked = time.monotonic()
        st, _, body = upload("t2")
        trial = gw.wait_log(r"fw: firmware e2e-t2 \(ota_\d\) on trial", 60, asked)
        partial = gw.wait_log(r"fw: firmware e2e-t2 on trial: a Sparkplug session came up without "
                              r"\d+ DBIRTH\(s\) \(too large for the broker\): not confirmed by it",
                              60, trial or asked)
        back = gw.wait_log(r"fw: firmware e2e-t2 rejected \(no complete Sparkplug session \(every "
                           r"DBIRTH\) before the timeout\)", timeout_s + 30, trial or asked)
        nb_back = obs.wait(nbirth_t, 60, lambda p: metric(p, name="Node Info/Firmware").string_value
                           == v1)
        born_t2 = [m for m in CAPTURE if m[0] >= asked and m[1] == nbirth_t and fw(m) == "e2e-t2"]
        confirmed = gw.find(r"fw: firmware e2e-t2 confirmed", asked)
        check("Issue #99 a new image whose sessions leave a DBIRTH out (larger than the broker's "
              "Maximum Packet Size) is not confirmed by them: the timeout rolls it back, and the "
              "previous image says so",
              st == 200 and trial and partial and born_t2 and back and not confirmed and nb_back
              and fw_state(nb_back[2]) == "rolled back from e2e-t2",
              f"POST {st} {body[:60]!r}; on trial {trial is not None}, session without every "
              f"DBIRTH {partial is not None} ({len(born_t2)} NBIRTH of e2e-t2), confirmed "
              f"{bool(confirmed)}, rolled back "
              f"{f'{back - trial:.1f} s into the trial' if back and trial else back}; NBIRTH "
              f"{fw(nb_back)!r} state {fw_state(nb_back[2]) if nb_back else None!r}")

        # 2. during another trial: a refused update keeps the phase; a saved configuration
        # confirms the image before its restart
        gw.wait_log(r"edge: time synchronised", 10, nb_back[0] - 5 if nb_back else t0)
        wait_http("/api/firmware", 20)
        asked = time.monotonic()
        st, _, body = upload("t3")
        trial = gw.wait_log(r"fw: firmware e2e-t3 \(ota_\d\) on trial", 60, asked)
        nb3 = obs.wait(nbirth_t, 30, lambda p: metric(p, name="Node Info/Firmware").string_value
                       == "e2e-t3")
        obs.publish_cmd(ncmd_t, [{"name": "Node Control/Firmware Update", "datatype": 12,
                                  "string_value": "https://10.0.2.2:1/x.bin"}])
        refused = obs.wait(ndata_t, 10, lambda p: fw_state(p) ==
                           "refused: the running firmware is on trial (not confirmed yet)")
        _, h = gw.shell("status", [r"^ota\s+e2e-t3 in ota_\d.*\| refused: the running firmware is "
                                   r"on trial \(not confirmed yet\); firmware e2e-t3 on trial "
                                   r"\(\d+ s left\)$"])
        obs.drain()
        saved_at = time.monotonic()
        st_put, _, b_put = http("PUT", "/api/config", cfg, timeout=30)
        conf_t = gw.wait_log(r"fw: firmware e2e-t3 confirmed: the operator saved a configuration; "
                             r"the bootloader keeps it", 20, saved_at)
        death = obs.wait(f"{T}/NDEATH/{node}", 20)
        nb4 = obs.wait(nbirth_t, 60)
        rolled = gw.find(r"fw: firmware e2e-t3 \(ota_\d\) was rejected|fw: firmware e2e-t3 rejected",
                         asked)
        again = gw.find(r"fw: firmware e2e-t3 \(ota_\d\) on trial", saved_at)
        check("Issue #101 an update refused during a trial (NCMD) is reported without hiding the "
              "trial: the phase and its time left stay (shell); issue #99 a configuration saved "
              "through the web during the trial confirms the new image before the restart, which "
              "comes back as the new image (not rolled back, not on trial again)",
              st == 200 and trial and nb3 and refused and all(h.values()) and st_put == 200
              and conf_t and death and nb4 and fw(nb4) == "e2e-t3" and not rolled and not again
              and not (fw_state(nb4[2]) or "").startswith("rolled back"),
              f"POST {st}; on trial {trial is not None}; refusal state "
              f"{fw_state(refused[2]) if refused else None!r}; shell {list(h.values())}; PUT "
              f"{st_put} {b_put[:60]!r}; confirmed {conf_t is not None}; NDEATH {death is not None}; "
              f"after the restart {fw(nb4)!r} state {fw_state(nb4[2]) if nb4 else None!r}; rolled "
              f"back {rolled[:1]}; on trial again {bool(again)}")
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


def run_nvs_migration(args, work):
    """Issues #67 and #94 (GW41): a flash whose NVS was written without NVS encryption (the
    format of the default build, generated here with ESP-IDF's nvs_partition_gen.py: bdSeq 41,
    the commissioning password, the MQTT password) boots the encrypted build, and the power is
    cut twice in the middle of the migration (emulated cut points: right after the partition is
    erased, then after half of the entries are written encrypted). The next boots finish it from
    the reset-safe copy: the bdSeq continues, the MQTT password still opens the broker (which
    requires it), the commissioning password is the old one, nothing stays in clear and the
    migration does not run again. Issue #104: after each cut, one boot whose LittleFS does not
    mount (superblocks damaged, not erased: the copy is out of reach) opens no NVS at all, so
    nothing it writes can be lost or win over the copy; then the partition comes back."""
    node, port = "GW41", MQTT_PORT + 24
    nvs_off, nvs_size = storage_partition("nvs")
    st_off, st_size = storage_partition("storage")
    csv = work / "nvs-plain.csv"
    csv.write_text("key,type,encoding,value\nspb,namespace,,\nbdseq,data,u8,41\n"
                   "secrets,namespace,,\ncommission,data,string,abcdefgh1234\n"
                   "mqtt_pw,data,string,s3cret-mqtt-67\ngw,namespace,,\ndo_hold,data,u8,0\n")
    gen = pathlib.Path(os.environ["IDF_PATH"]) / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    plain = work / "nvs-plain.bin"
    subprocess.run([args.idf_python, str(gen), "generate", str(csv), str(plain), hex(nvs_size)],
                   check=True, capture_output=True)
    # the password is only in the NVS (CFG-7); the broker refuses the gateway without it
    cfg = node_config(node, port=port, username="gw41")
    flash = flash_variant(args, work, "nvsmig", {"config.json": json.dumps(cfg).encode(),
                                                  ".nvsmig-cut": b"12"})
    img = bytearray(flash.read_bytes())
    img[nvs_off:nvs_off + nvs_size] = plain.read_bytes()
    flash.write_bytes(img)
    broker, _ = auth_broker(work, "nvsmig", port, {"gw41": "s3cret-mqtt-67"})
    obs = observer_retry(port, auth=OBS_AUTH)
    log = work / "uart0-nvsmig.log"
    words = (b"abcdefgh1234", b"s3cret-mqtt-67", b"commission", b"bdseq")
    try:
        cuts, out_of_reach = [], []
        for point in ("1", "2"):
            t = time.monotonic()
            gw = Gateway(args, log, flash)
            gw.start()
            held = gw.wait_log(rf"storage: NVS migration: emulated power cut {point} ", 60, t)
            time.sleep(2)   # QEMU writes the flash file behind: let the last writes land
            resumed = gw.find(r"storage: NVS migration was interrupted \((.*)\): writing its copy", t)
            gw.kill()
            region = flash.read_bytes()[nvs_off:nvs_off + nvs_size]
            cuts.append((held is not None, resumed[0].split("storage: ")[-1][:90] if resumed else None,
                         region.count(b"\xff") == len(region)))
            # issue #104: LittleFS does not mount for one boot (the copy is in it)
            img = bytearray(flash.read_bytes())
            saved = bytes(img[st_off:st_off + 8192])
            img[st_off:st_off + 8192] = b"\x55" * 8192
            flash.write_bytes(img)
            storage_before = bytes(img[st_off:st_off + st_size])
            t = time.monotonic()
            gw = Gateway(args, log, flash)
            gw.start()
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
            refused = gw.wait_log(r"^config: rejected: configuración no guardada: LittleFS no monta y "
                                  r"puede tener la copia de una migración de NVS", 30, asked)
            formatted = gw.find(r"formatting it to save", asked)
            time.sleep(1)
            gw.kill()
            img = bytearray(flash.read_bytes())
            same = img[nvs_off:nvs_off + nvs_size] == region
            kept = bytes(img[st_off:st_off + st_size]) == storage_before
            img[st_off:st_off + 8192] = saved
            flash.write_bytes(img)
            # issue #108 B9: the boot does not change the NVS either (cut 1: not even initialised;
            # cut 2: only read for the migration's markers)
            out_of_reach.append((left is not None, all(h_out.values()), same,
                                 refused is not None and not formatted and kept))
        t0 = time.monotonic()
        gw = Gateway(args, log, flash)
        gw.start()
        nb = obs.wait(f"{T}/NBIRTH/{node}", 60)
        done = gw.find(r"storage: NVS migration resumed: \d+ entries moved into the encrypted NVS "
                       r"\(0 skipped\); bdSeq continues", t0)
        pw = commissioning_password(gw, t0)
        gw.kill()
        obs.wait(f"{T}/NDEATH/{node}", 5)
        clear = nvs_clear(flash, words)
        obs.drain()
        t1 = time.monotonic()
        gw = Gateway(args, log, flash)
        gw.start()
        nb2 = obs.wait(f"{T}/NBIRTH/{node}", 60)
        again = gw.find(r"NVS migration", t1)
        _, h = gw.shell("status", [r"^secrets\s+NVS encrypted \(HMAC key in eFuse KEY5\)"])
        gw.kill()
        # cut 1: held right after the erase (partition all 0xFF), first boot (nothing resumed);
        # cut 2: the second boot resumed from the copy and held halfway through the writes
        cut_ok = (len(cuts) == 2 and cuts[0][0] and cuts[0][1] is None and cuts[0][2]
                  and cuts[1][0] and cuts[1][1] is not None and not cuts[1][2])
        check("Issue #94 an unencrypted NVS meets the encrypted build and the power is cut twice "
              "during the migration (after the erase, then halfway through the encrypted writes): "
              "the next boot finishes it from the reset-safe copy (0 skipped); bdSeq continues, the MQTT "
              "password from the NVS opens the broker that requires it, the commissioning "
              "password is the old one, nothing in clear, no migration on the next boot "
              "(issue #67)",
              cut_ok and nb and bdseq_of(nb[2]) == 41 and done and pw == "abcdefgh1234"
              and not clear and nb2 and bdseq_of(nb2[2]) == 42 and not again
              and list(h.values())[0],
              f"cuts (held, resumed, erased): {cuts}; NBIRTH bdSeq "
              f"{bdseq_of(nb[2]) if nb else None} then {bdseq_of(nb2[2]) if nb2 else None}; "
              f"{(done or ['no resume log'])[0].split('storage: ')[-1][:90]}; commissioning "
              f"password {pw!r}; in clear: {clear}; migration on the next boot: {bool(again)}")
        check("Issue #104 after each cut, a boot whose LittleFS does not mount (the copy is in it) "
              "opens no NVS at all and leaves the NVS partition as it was (no empty or partial "
              "encrypted NVS), the shell says why; issue #108 M1: a configuration saved in that "
              "boot is refused and LittleFS is not formatted, so the next boot with LittleFS "
              "still finishes the migration from the copy (check above)",
              len(out_of_reach) == 2 and all(all(x) for x in out_of_reach),
              f"(logged, shell, NVS untouched, save refused and LittleFS kept) per cut: "
              f"{out_of_reach}")
    finally:
        obs.close()
        broker.terminate()
        broker.wait()


def run_nvs_ota(args, work):
    """Issue #94 (GW42): a device on the build WITHOUT NVS encryption (QEMU variant
    sdkconfig.qemu-plain; bdSeq, commissioning password and MQTT password in its unencrypted
    NVS) is updated by OTA to the encrypted build under test. On trial, the new image keeps the
    NVS unencrypted; a reset during the trial (panic) rolls it back and the old image finds its
    bdSeq (continued by the trial) and its secrets. Updated again with the broker up: confirmed,
    one restart, and that boot moves the NVS into the encrypted one. Issue #102: then the build
    without NVS encryption, sent back by OTA, is refused before it is set to boot, and the NVS
    (bdSeq, secrets) stays intact."""
    if not args.plain_flash:
        print("[SKIP] issue #94 OTA from a build without NVS encryption: no --plain-flash "
              "(run_e2e.sh builds it)", flush=True)
        return
    node, port, fw_port = "GW42", MQTT_PORT + 22, MQTT_PORT + 23
    secret = "s3cret-mqtt-94"
    pki = make_pki(work)
    fwdir = work / "fw-nvs"
    fwdir.mkdir(exist_ok=True)
    app = pathlib.Path(args.elf).with_suffix(".bin")
    unsigned = pathlib.Path(args.elf).with_name(app.stem + "-unsigned.bin")
    fw_image(unsigned if unsigned.exists() else app, "e2e-enc", fwdir / "enc-unsigned.bin")
    if args.signing_key:
        subprocess.run([args.idf_python, "-m", "espsecure", "sign-data", "--version", "2",
                        "--keyfile", args.signing_key, "--output", str(fwdir / "enc.bin"),
                        str(fwdir / "enc-unsigned.bin")], check=True, capture_output=True)
    else:
        shutil.copy(fwdir / "enc-unsigned.bin", fwdir / "enc.bin")
    server = FirmwareServer(fw_port, fwdir, pki["server.crt"], pki["server.key"])
    broker, conf = auth_broker(work, "nvsota", port, {"gw42": secret})
    obs = observer_retry(port, auth=OBS_AUTH)
    # the password in clear in config.json: the plain build moves it to its (unencrypted) NVS
    cfg = node_config(node, port=port, username="gw42", password=secret)
    cfg["ota"] = {"ca_file": "ca.pem"}
    base = pathlib.Path(args.plain_flash)
    files = {"config.json": json.dumps(cfg, indent=2).encode(), "ca.pem": pki["ca.pem"].read_bytes()}
    flash = flash_variant(args, work, "nvsota", files, base=base)
    if args.psram:
        write_otadata(flash)
    log = work / "uart0-nvsota.log"
    nbirth_t, ncmd_t = f"{T}/NBIRTH/{node}", f"{T}/NCMD/{node}"
    ndata_t = f"{T}/NDATA/{node}"
    words = (secret.encode(), b"commission", b"bdseq", b"mqtt_pw")
    if args.plain_app:
        # issue #102: the build without NVS encryption as an update, signed like the others
        fw_image(args.plain_app, "e2e-plain", fwdir / "plain-unsigned.bin")
        if args.signing_key:
            subprocess.run([args.idf_python, "-m", "espsecure", "sign-data", "--version", "2",
                            "--keyfile", args.signing_key, "--output", str(fwdir / "plain.bin"),
                            str(fwdir / "plain-unsigned.bin")], check=True, capture_output=True)
        else:
            shutil.copy(fwdir / "plain-unsigned.bin", fwdir / "plain.bin")

    def update(name="enc"):
        obs.publish_cmd(ncmd_t, [{"name": "Node Control/Firmware Update", "datatype": 12,
                                  "string_value": f"https://10.0.2.2:{fw_port}/{name}.bin"}])

    def fw(nb):
        return metric(nb[2], name="Node Info/Firmware").string_value if nb else None

    t0 = time.monotonic()
    gw = Gateway(args, log, flash)
    gw.start()
    try:
        nb0 = obs.wait(nbirth_t, 60)
        pw0 = commissioning_password(gw, t0)
        v1 = fw(nb0)
        clear0 = nvs_clear(flash, words)
        if not check("Issue #94 the build without NVS encryption runs (its secrets and bdSeq in "
                     "an unencrypted NVS) and reaches the broker that requires its password",
                     nb0 and pw0 and v1 != "e2e-enc" and secret.encode() in clear0,
                     f"firmware {v1!r}, bdSeq {bdseq_of(nb0[2]) if nb0 else None}, commissioning "
                     f"password {pw0!r}; in clear in its NVS: {clear0}"):
            return
        b0 = bdseq_of(nb0[2])
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
        # a connection attempt of the trial image takes (and stores) the next bdSeq
        attempt = gw.wait_log(r"edge: connecting to .* bdSeq=\d+", 60, trial or asked)
        time.sleep(1)
        _, h_trial = gw.shell("status", [r"^secrets\s+NVS NOT encrypted yet \(new firmware on trial"])
        gw.console("panic")
        back = gw.wait_log(r"fw: firmware e2e-enc \(ota_1\) was rejected or did not confirm itself; "
                           r"running \S+ again", 90, trial or asked)
        erased = gw.find(r"NVS layout changed, erasing|NVS unavailable", asked)
        broker = start_broker(conf, work / "mosquitto-nvsota.log")
        obs = observer_retry(port, auth=OBS_AUTH)
        nb1 = obs.wait(nbirth_t, 90)
        pw1 = commissioning_password(gw, back or asked) if back else None
        clear1 = nvs_clear(flash, words)
        b1 = bdseq_of(nb1[2]) if nb1 else None
        grew = b1 is not None and 2 <= (b1 - b0) % 256 <= 64
        check("Issue #94 OTA from the build without NVS encryption to the encrypted one, rolled back "
              "by a reset during the trial: the new image kept the NVS unencrypted while on trial, "
              "and the old image finds its bdSeq (continued by the trial) and its secrets (the MQTT "
              "password opens the broker, the commissioning password is the same)",
              death and trial and kept and attempt and list(h_trial.values())[0] and back
              and not erased
              and nb1 and fw(nb1) == v1 and grew and pw1 == pw0 and secret.encode() in clear1,
              f"trial {trial is not None}, kept unencrypted {bool(kept)}, rolled back "
              f"{back is not None}; NVS erased by the old image: {erased[:1]}; NBIRTH firmware "
              f"{fw(nb1)!r} bdSeq {b0} -> {b1}; commissioning password {pw1!r}; in clear: {clear1}")

        # 2. update again with the broker up: confirmed by the session, one restart, migrated
        gw.wait_log(r"edge: time synchronised", 20, nb1[0] - 5 if nb1 else t0)
        asked = time.monotonic()
        update()
        conf_t = gw.wait_log(r"fw: firmware e2e-enc confirmed: restarting once to encrypt the NVS",
                             300, asked)
        moved_t = gw.wait_log(r"storage: NVS migration: \d+ entries moved into the encrypted NVS "
                              r"\(0 skipped\); bdSeq continues", 90, conf_t or asked)
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
        check("Issue #94 the same update with the broker up: confirmed by its first session, one "
              "restart (and no second one), and that boot moves the NVS into the encrypted one (bdSeq continues, the "
              "MQTT password still opens the broker, same commissioning password, nothing in clear)",
              conf_t and moved_t and nb3 and fw(nb3) == "e2e-enc" and b1 is not None
              and b3 is not None and 1 <= (b3 - b1) % 256 <= 64 and pw2 == pw0 and not clear2
              and list(h_enc.values())[0] and one_restart,
              f"confirmed {conf_t is not None}, migrated {moved_t is not None}; boots after the "
              f"confirmation {len(boots)}, restart requests {len(restarts)}, another NBIRTH in "
              f"20 s {later is not None}; NBIRTH firmware "
              f"{fw(nb3)!r} bdSeq {b1} -> {b3}; commissioning password {pw2!r}; in clear: {clear2}")

        # 3. issue #102: the build without NVS encryption sent to the migrated device: refused
        # once its head is in the slot (before esp_ota_set_boot_partition), no restart; the next
        # boot still has the encrypted NVS (bdSeq continues, the password opens the broker)
        if not args.plain_app:
            print("[SKIP] issue #102 OTA back to the build without NVS encryption: no --plain-app",
                  flush=True)
            return
        obs.drain()
        t3 = time.monotonic()
        gw = Gateway(args, log, flash)
        gw.start()
        nb4 = obs.wait(nbirth_t, 60)
        gw.wait_log(r"edge: time synchronised", 20, t3)
        asked = time.monotonic()
        update("plain")
        refused = obs.wait(ndata_t, 120, lambda p: (fw_state(p) or "").startswith(
            "failed: image e2e-plain refused: built without NVS encryption"))
        why = gw.find(r"fw: update refused: the image built without NVS encryption \(this device's "
                      r"NVS is encrypted\)", asked)
        time.sleep(3)
        restarted = any(m[1] == f"{T}/NDEATH/{node}" for m in CAPTURE if m[0] >= asked)
        gw.kill()
        obs.wait(f"{T}/NDEATH/{node}", 5)
        obs.drain()
        t4 = time.monotonic()
        gw = Gateway(args, log, flash)
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
              "restart); the next boot runs the same firmware with the NVS intact (bdSeq continues, "
              "the MQTT password from the NVS opens the broker, same commissioning password)",
              nb4 and refused and why and not restarted and nb5 and fw(nb5) == "e2e-enc"
              and b4 is not None and b5 is not None and 1 <= (b5 - b4) % 256 <= 64
              and pw3 == pw0 and all(h3.values()) and not clear3,
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


def main():
    global MQTT_PORT, MODBUS_PORT, CONSOLE_PORT, SYSLOG_PORT, HTTP_PORT, HTTPS_PORT, MBTCP_PORT
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
    ap.add_argument("--simulate-qemu-death", nargs="?", const="ota", choices=("ota", "boot"),
                    help="issues #86/#90 self-test: 'ota' (the default) SIGABRTs the emulator "
                         "once in the OTA scenario; 'boot' SIGSEGVs the second emulator started, "
                         "3 s into its boot, whatever the scenario; either must be repeated")
    ap.add_argument("--signing-key", default="",
                    help="FW-2: the QEMU build's test signing key (signed images required)")
    ap.add_argument("--plain-flash", default="",
                    help="issue #94: flash image of the QEMU build without NVS encryption "
                         "(sdkconfig.qemu-plain), the image before the OTA update")
    ap.add_argument("--plain-app", default="",
                    help="issue #102: the unsigned app image of that build (an OTA update the "
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
                    help="comma-separated scenarios to run (failsafe,network,web,reset,tls,failover,"
                         "mqtt5,phost,sf,https,ota,nvs,slow,refuse,main); "
                         "default all")
    ap.add_argument("--psram", action="store_true",
                    help="issue #63: emulate the octal PSRAM (build with sdkconfig.qemu-psram)")
    ap.add_argument("--panics", type=int, default=1,
                    help="issue #62: panics in the core dump check (each dump must decode)")
    ap.add_argument("--no-isolate", action="store_true",
                    help="stay on the host loopback instead of a private network namespace")
    args = ap.parse_args()
    MQTT_PORT, MODBUS_PORT, CONSOLE_PORT = args.mqtt_port, args.modbus_port, args.console_port
    SYSLOG_PORT, HTTP_PORT, HTTPS_PORT = args.syslog_port, args.http_port, args.https_port
    MBTCP_PORT = args.mbtcp_port
    global PROBE_MAX_S
    PROBE_MAX_S = sdkconfig_int(args.sdkconfig, "GW_MB_OFFLINE_PROBE_MAX_MS") / 1000
    isolated = False if args.no_isolate else isolate_network()
    print(f"network: {'private namespace' if isolated else 'host loopback'}; ports MQTT {MQTT_PORT}, "
          f"Modbus {MODBUS_PORT}, console {CONSOLE_PORT}, syslog {SYSLOG_PORT}, HTTP {HTTP_PORT}, "
          f"HTTPS {HTTPS_PORT}",
          flush=True)
    work = pathlib.Path(args.workdir)
    log = work / "uart0.log"
    # The image under test gets the harness' config.json (with its MQTT port).
    args.flash = str(flash_variant(args, work, "main", {"config.json": config_json()}))
    if args.psram:
        write_otadata(args.flash)   # every variant is a copy of this image

    conf = work / "broker.conf"
    conf.write_text(f"listener {MQTT_PORT} 127.0.0.1\nallow_anonymous true\n")
    broker = start_broker(conf, work / "mosquitto.log")
    stop = threading.Event()
    threading.Thread(target=sntp_responder, args=(stop,), daemon=True).start()
    threading.Thread(target=syslog_listener, args=(stop,), daemon=True).start()

    slave = ModbusSlave()
    slave.set_float(4, 0, 230.5)       # Voltage, input 0..1, float32 ABCD
    slave.ctx.setValues(3, 10, [50])   # Setpoint raw 50 -> 5.0
    slave.set_mb6()
    slave.start()
    try:
        only = set(filter(None, args.only.split(",")))
        want = lambda name: not only or name in only  # noqa: E731
        # Issues #86/#90: every scenario through run_scenario: a QEMU that dies under one (at any
        # boot) is reported as such and the scenario runs once more; firmware failures are not
        # retried. The OTA scenario is the one where it happens most (README 4.3).
        if want("failsafe"):
            run_scenario("failsafe", lambda: run_config_failsafe(args, work, slave))
        if want("network"):
            run_scenario("network", lambda: run_network_variant(args, work, slave))
        if want("web"):
            run_scenario("web", lambda: run_web(args, work))
        if want("reset"):
            run_scenario("reset", lambda: run_factory_reset(args, work))
        if want("tls"):
            run_scenario("tls", lambda: run_tls(args, work))
        if want("failover"):
            run_scenario("failover", lambda: run_failover(args, work))
        if want("mqtt5"):
            run_scenario("mqtt5", lambda: run_mqtt5(args, work, work / "mosquitto.log"))
            run_scenario("max_packet", lambda: run_max_packet(args, work, slave))
        if want("phost"):
            run_scenario("phost", lambda: run_primary_host(args, work))
        if want("sf"):
            run_scenario("sf", lambda: run_store_forward(args, work, slave))
            run_scenario("sf spill", lambda: run_store_forward_spill(args, work, slave))
            run_scenario("stack margin", lambda: run_stack_margin(args, work, slave))
        if want("https"):
            run_scenario("https", lambda: run_https(args, work))
        if want("ota"):
            run_scenario("FW-1 OTA", lambda: run_ota(args, work))
            run_scenario("issues #99/#101 OTA trial", lambda: run_ota_trial(args, work))
        if want("nvs"):
            run_scenario("issues #67/#94 NVS migration", lambda: run_nvs_migration(args, work))
            run_scenario("issue #94 OTA to NVS encryption", lambda: run_nvs_ota(args, work))
        if want("slow"):
            run_scenario("slow", lambda: run_slow_link(
                args, work, sdkconfig_int(args.sdkconfig, "GW_MQTT_PUBLISH_BUDGET_MS")))
        if "refuse" in only and "main" not in only:
            # part of "main"; alone only on request (#89)
            run_scenario("refuse", lambda: check_suback_refused(args, work))
        if want("main"):
            run_scenario("main", lambda: run(args, log, slave, broker, conf, work))
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
        for i, (_, topic, p, raw) in enumerate(CAPTURE):
            kind = topic.split("/")[2]
            (dump / f"{i:03d}_{kind}.bin").write_bytes(raw)
            bad += len(spb_decode.check(kind, p))
        check(f"6. payloads decode with the official sparkplug_b.proto (Python protobuf "
              f"{google.protobuf.__version__})",
              CAPTURE and bad == 0, f"{len(CAPTURE)} messages decoded, {bad} structural errors")
        runs, errs, prev = 0, [], None
        for d in syslog_from(NODE):
            if d[4] == 1:
                runs += 1
            elif prev is None or d[4] != prev + 1:
                errs.append(f"{prev}->{d[4]}")
            prev = d[4]
        check("REL-4 syslog sequenceId continuous within each boot (UDP over the emulator's NAT, "
              "nothing lost)", runs >= 1 and not errs,
              f"{len(syslog_from(NODE))} datagrams in {runs} boot(s); gaps {errs[:5]}")
        seq_errors = seq_continuity(CAPTURE)
        check("SPB-5 seq continuity per session", not seq_errors, "; ".join(seq_errors) or "ok")
        # issue #86: the retries next to the checks (their discarded checks included)
        (work / "results.json").write_text(json.dumps({"checks": results, "retries": RETRIES},
                                                      indent=2))
        failed = [r for r in results if not r["ok"]]
        # issue #90: every emulator that ended on its own, with its stderr tail, and the
        # artefacts left in place (the harness deletes nothing)
        exits = [d for d in (g.qemu_died() for g in GATEWAYS) if d]
        (work / "qemu-exits.txt").write_text("".join(f"{d}\n" for d in exits) or "none\n")
        if failed or exits:
            print(f"artefacts kept for the diagnosis: {work} (uart0-*.log, uart0-*.log.qemu, "
                  f"qemu-exits.txt: {len(exits)} QEMU exit(s) not requested by the harness)")
        retried = (f"; {len(RETRIES)} scenario(s) repeated after QEMU died: " +
                   ", ".join(f"{r['scenario']} ({r['qemu']})" for r in RETRIES)) if RETRIES else ""
        print(f"\n{len(results) - len(failed)}/{len(results)} checks passed{retried}; artefacts "
              f"in {work}")
        sys.exit(1 if failed else 0)


def run_scenario(name, fn, retry_on_qemu_death=True):
    """Issue #86: run scenario @name; if QEMU died under it (the emulator, not the firmware:
    Gateway.qemu_died), discard its checks and run it once more, recording the retry in the
    summary and results.json. A failed check with QEMU alive is never retried, nor a second
    death. An exception that ends the scenario is a failed check, not the end of the run."""
    for attempt in (1, 2):
        first_check, first_gw = len(results), len(GATEWAYS)
        error = None
        try:
            fn()
        except Exception:
            traceback.print_exc()
            error = traceback.format_exc().splitlines()[-1]
        for g in GATEWAYS[first_gw:]:   # leave no emulator of this attempt running
            if not g.killed and g.proc is not None:
                try:
                    g.kill()
                except OSError:
                    pass
        deaths = [d for d in (g.qemu_died() for g in GATEWAYS[first_gw:]) if d]
        if deaths and retry_on_qemu_death and attempt == 1:
            discarded = results[first_check:]
            del results[first_check:]
            RETRIES.append({"scenario": name, "qemu": deaths[0], "error": error,
                            "discarded_checks": discarded})
            print(f"[RETRY] {name}: {deaths[0]}; the scenario runs once more ({len(discarded)} "
                  f"check(s) of the first attempt discarded)", flush=True)
            continue
        if error is not None or deaths:
            check(f"{name} scenario completed", False,
                  (error or "") + (f"; {deaths[0]}" if deaths else "") +
                  ("; second attempt after a QEMU death" if attempt == 2 else ""))
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


def run_config_failsafe(args, work, slave):
    """CFG-1: the embedded default is used only when /cfg/config.json does not exist; any
    other problem stops the gateway (no polling, no MQTT) with the error state and a log.
    CFG-3 recovery: the network still comes up (DHCP, no valid configuration was ever seen on
    these images) with the web UI, so the configuration can be fixed without reflashing."""
    good = config_json()
    limit = sdkconfig_int(args.sdkconfig, "GW_CONFIG_MAX_BYTES")
    cases = [
        ("oversized", {"config.json": good + b" " * (limit + 1 - len(good))},
         rf"config\.json: {limit + 1} bytes; el máximo es {limit}"),
        ("unmountable", None, r"partición LittleFS 'storage' no montable"),
        ("empty", {"config.json": b""}, r"config\.json: archivo vacío"),
        # without the ',' that closes "network" (line 4), the parser stops at the '"' of
        # "ntp": line 5, column 3
        ("syntax-error", {"config.json": good.replace(b'"dhcp": true\n  },', b'"dhcp": true\n  }', 1)},
         r"JSON mal formado en la línea 5, columna 3"),
        ("semantic-error", {"config.json": good.replace(b'"type": "float32"', b'"type": "float16"', 1)},
         r"devices\[0\]\.tags\[0\]\.type: valor no válido"),
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
        ts = [t for t, r in zip(list(slave.times), list(slave.reads)) if r[0] == 3 and t >= t0]
        out = []
        for t in ts:
            if out and t - out[-1][-1] <= 0.5:
                out[-1].append(t)
            else:
                out.append([t])
        return out

    end = t0 + 60
    while time.monotonic() < end and len(bursts()) < 5:
        time.sleep(0.5)
    reads = list(zip(slave.times, slave.reads))
    cl = bursts()
    single = bool(cl) and all(len(c) == 1 for c in cl)
    gaps = [round(b[0] - a[0], 1) for a, b in zip(cl, cl[1:])]
    full = [min(2.0 * 2 ** i, PROBE_MAX_S) for i in range(len(gaps) + 1)]
    # The very first request may be lost while QEMU's UART socket connects: then the series
    # starts one step later. A due probe may also wait for another device's cycle (~0.6 s).
    def fits(k):
        return all(-0.2 <= g - e <= 0.7 for g, e in zip(gaps, full[k:]))
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
    worst = max((b - a for a, b in zip(starts, starts[1:])), default=0)
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
    seen = [r for t, r in zip(list(slave.times), list(slave.reads)) if t >= since and r[0] == 1
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
    reset reason "panic"); a panic restart does not quiesce the bus."""
    outcomes = []
    slave.delay = 0.12
    try:
        for _ in range(3):
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


def check_scan_rate(obs, gw, slave, nbirth, ncmd_t, ndata_t):
    """SPB-15: writable Node Control/Scan Rate (UInt32 ms, 0 = each device at its poll_ms).
    3000 makes Meter1 poll every 3 s, an invalid value is refused (NDATA keeps 3000), 0 brings
    back its 1 s."""
    name = "Node Control/Scan Rate"
    birth = metric(nbirth, name=name)
    def set_rate(value, datatype=7):
        obs.drain()
        obs.publish_cmd(ncmd_t, [{"name": name, "datatype": datatype, "int_value": value}])
        nd = obs.wait(ndata_t, 3, lambda q: metric(q, alias=7) is not None)
        return metric(nd[2], alias=7).int_value if nd else None
    def starts(since, until):
        return [t for t, r in zip(list(slave.times), list(slave.reads))
                if since <= t <= until and r[0] == 1 and r[1:] == (1, 0, 1)]
    t_set = time.monotonic()
    confirmed = set_rate(3000)
    time.sleep(11)
    slow = starts(t_set + 1.5, time.monotonic())
    gaps3 = [round(b - a, 2) for a, b in zip(slow, slow[1:])]
    since = time.monotonic()
    refused = set_rate(10)
    logged = gw.wait_log(r"NCMD: metric 'Node Control/Scan Rate' .*discarded: Scan Rate must be", 2, since)
    t_back = time.monotonic()
    back = set_rate(0)
    time.sleep(5)
    fast = starts(t_back + 1.5, time.monotonic())
    gaps1 = [round(b - a, 2) for a, b in zip(fast, fast[1:])]
    ok = birth is not None and birth.datatype == 7 and birth.int_value == 0 and confirmed == 3000 \
        and len(gaps3) >= 2 and all(2.7 <= g <= 3.6 for g in gaps3) and refused == 3000 and logged \
        and back == 0 and len(gaps1) >= 2 and all(g <= 1.6 for g in gaps1)
    return check("SPB-15 NCMD Node Control/Scan Rate: NBIRTH UInt32 0, 3000 -> Meter1 every 3 s "
                 "(NDATA), invalid 10 refused, 0 -> back to its poll_ms", ok,
                 f"NBIRTH {(birth.datatype, birth.int_value) if birth else None}; NDATA {confirmed}, "
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
            reads = [(t, r) for t, r in zip(list(slave.times), list(slave.reads)) if r[0] == 1]
            w = next((i for i, (t, r) in enumerate(reads) if t >= sent and r[1:3] == (16, 34)), None)
            if w is None:
                tries.append(f"attempt {attempt + 1}: no write seen")
                continue
            before = 0
            for t, r in reversed(reads[:w]):
                if r[1:] == start:
                    break
                before += 1
            after = 0
            for t, r in reads[w + 2:]:      # w + 1 is the read-back
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
    reads = [(t, r[1:]) for t, r in zip(list(slave.times), list(slave.reads)) if r[0] == 1 and t >= since]
    fast = [t for t, k in reads if k == (1, 0, 1)]
    slow = [t for t, k in reads if k == (3, 30, 4)]
    # a DCMD to Serial (check_mb6_writes) is read back right after its FC16: that request is not
    # a scan read (only the one that immediately follows each write)
    serial = [t for i, (t, k) in enumerate(reads)
              if k == (3, 50, 8) and not (i > 0 and reads[i - 1][1][:2] == (16, 50))]
    gaps = [round(b - a, 2) for a, b in zip(slow, slow[1:])]
    fgaps = [b - a for a, b in zip(fast, fast[1:])]
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
    ok = st == 200 and io.get("counters", [None])[0] == 2 and len(io.get("counters", [])) == 8 \
        and io.get("rule_do") == 0x30 and io.get("failsafe") is False and io.get("do_unknown") is False \
        and isinstance(io.get("do"), int) and bz.get("enabled") == ["device_offline", "mqtt_down", "config_error"] \
        and bz.get("alarm") == "device_offline" and tm.get("synced") is True and tm.get("source") == "sntp" \
        and tm.get("rtc", {}).get("present") is True and tm.get("rtc", {}).get("stores", 0) >= 1 \
        and abs(tm.get("utc_s", 0) - time.time()) < 60 and s.get("sparkplug", {}).get("diagnostics_s") == 10 \
        and tags.get("Energy") == MB6["Energy"] and isinstance(tags.get("Serial"), str)
    check("Issue #75 /api/status over HTTPS: pulse counters, failsafe, DOs of local rules, unknown "
          "DOs, buzzer alarm, clock (SNTP/RTC), diagnostics period, exact UInt64 and string values",
          ok, f"HTTP {st}; io {io}; buzzer {bz}; time {tm}; Energy {tags.get('Energy')!r} "
              f"Serial {tags.get('Serial')!r}")


def check_rtc(gw, obs, nb, t0, since_reboot, ndata_t, first_nbirth):
    """TIME-2: the first boot (power-on: the simulated PCF85063 has its OS bit set) gets no time
    from it, and stores the SNTP time in it; the boot after NCMD Reboot, with SNTP silent, sets
    the system clock from the RTC, publishes its NBIRTH with valid timestamps at once, and when
    SNTP answers again there is no rebirth (the births were already timed)."""
    stopped = gw.find(r"io: RTC: oscillator stopped \(OS bit\): time lost: system time not set", t0)
    stored = gw.find(r"io: RTC: time stored in the RTC after SNTP", t0)
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
    restore_store = gw.wait_log(r"edge: time synchronised", 0, since_reboot) and \
        gw.find(r"clock now synchronised: rebirth", since_reboot) == []
    check("TIME-2 RTC: OS bit at power-on (no time from it), SNTP time stored in it; after a "
          "reboot with SNTP silent the clock comes from the RTC, NBIRTH timestamps valid at once, "
          "no rebirth when SNTP answers later",
          stopped and stored and restored and not synced_early and skew is not None and skew < 60
          and shown and synced and restore_store,
          f"{(stopped or ['no OS-bit log'])[0]} | {(stored or ['no store log'])[0]} | "
          f"{(restored or ['no restore log'])[0]} | NBIRTH skew {round(skew, 1) if skew else None} s; "
          f"SNTP before the NBIRTH: {bool(synced_early)}; shell {'ok' if shown else 'no'}; "
          f"SNTP later {'yes' if synced else 'no'}")


def modbus_rows(m):
    """SPB-17: {device: {column: value}} of the 'Node Info/Modbus' DataSet."""
    ds = m.dataset_value
    return {row.elements[0].string_value: {c: spb_decode.value_of(e) for c, e in zip(ds.columns, row.elements)}
            for row in ds.rows}


def check_modbus_tcp_table(obs, ndata_t):
    """Issue #81: the next diagnostics NDATA carries "Node Info/Modbus TCP" (alias 56) with the
    same per-unit counters the shell shows after check_modbus_tcp (#69): Meter1 2 OK, Dead3 3
    timeouts, units without a device none, and the poll table unchanged in its columns."""
    obs.drain()
    nd = obs.wait(ndata_t, 13, lambda q: metric(q, alias=56) is not None)
    m = metric(nd[2], alias=56) if nd else None
    ds = m.dataset_value if m is not None else None
    rows = {r.elements[0].string_value: {c: spb_decode.value_of(e) for c, e in zip(ds.columns, r.elements)}
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
        and heap.datatype == 7 and heap.int_value > 10000 and low is not None \
        and low.int_value <= heap.int_value and metric(nbirth, name="Node Info/RSSI") is None \
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
          f"NBIRTH uptime {up.long_value if up else None} s, heap {heap.int_value if heap else None} B "
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
        value = {8: lambda: m.long_value, 4: lambda: int64_of(m), 10: lambda: m.double_value,
                 3: lambda: m.int_value - 2 ** 32 if m.int_value >= 2 ** 31 else m.int_value,
                 12: lambda: m.string_value, 11: lambda: m.boolean_value}[m.datatype]()
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
      * SPB-18: two devices of type "Pump" (PumpA and PumpB, both on unit 1 so that unit 2 keeps
        the single FC03 the main scenario checks, at holding 230 and 231): one template
        definition in the NBIRTH, an instance per DBIRTH, a DCMD member write and its DDATA."""
    node = "GW02"
    cfg = json.loads(config_json())
    cfg["sparkplug"]["edge_node_id"] = node
    cfg["network"] = {"dhcp": True,
                      "ipv6": {"enabled": True, "slaac": True, "address": "fec0::1234"},
                      "wifi": {"enabled": True, "ssid": "Plant WLAN", "password": "12345678",
                               "role": "backup", "failback_s": 30}}
    cfg["modbus_tcp"] = {"enabled": True, "allowed_clients": ["192.0.2.0/24"]}
    cfg["devices"].append({"name": "Raw", "unit_id": 1, "poll_ms": 500, "tags": [
        {"name": "Block", "area": "holding", "address": 220, "type": "bytes", "length": 3,
         "writable": True}]})
    slave.ctx.setValues(3, 220, [0x00FF, 0x1000, 0x7F80])
    for k in (0, 1):
        cfg["devices"].append({"name": f"Pump{'AB'[k]}", "unit_id": 1, "type": "Pump",
                               "poll_ms": 500, "tags": [
            {"name": "Speed", "area": "holding", "address": 230 + k, "type": "uint16", "units": "rpm",
             "writable": True},
            {"name": "Run", "area": "coil", "address": 5 + k, "type": "bool"}]})
        slave.ctx.setValues(3, 230 + k, [1450 + 10 * k])
        slave.ctx.setValues(1, 5 + k, [True])
    flash = flash_variant(args, work, "network", {"config.json": json.dumps(cfg, indent=2).encode()})
    obs = Observer()
    gw = Gateway(args, work / "uart0-network.log", flash)
    t0 = time.monotonic()
    gw.start()
    nb = obs.wait(f"{T}/NBIRTH/{node}", 40)
    ignored = gw.find(r"network\.wifi ignored: Wi-Fi is not available in this build", t0)
    asked, h = gw.shell(["status", "config"],
                        [r"^iface\s+ethernet up \d+\.\d+\.\d+\.\d+ \(active\) \| wifi not configured",
                         r"^wifi\s+'Plant WLAN', backup of Ethernet, failback 30 s, DHCP"])
    iface, shown = h.values()
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
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node}", 5)   # the will, before the main scenario starts
    obs.close()
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
    data_ok = dm is not None and not dm.HasField("name") and dm.alias == alias and dm.datatype == 19 \
        and [(x.name, x.int_value) for x in dm.template_value.metrics] == [("Speed", 1234)]
    ok = def_ok and inst_ok and wrote == [(6, 230, [1234])] and data_ok
    return ok, (f"definition {members} (alias {'yes' if d is not None and d.HasField('alias') else 'no'}); "
                f"instances {inst}; DCMD writes {wrote}; DDATA "
                f"{[(x.name, x.int_value) for x in dm.template_value.metrics] if dm else None}")


def run_web(args, work):
    """CFG-4 REST API (and the web UI, CFG-3), in boots of their own with separate edge node ids:
    authentication, export with masked secrets, invalid import -> 400 with the JSON path,
    dry run, oversized body, and a valid import -> orderly restart with the new configuration."""
    node, node2 = "GW04", "GW05"
    cfg = json.loads(config_json())
    cfg["sparkplug"]["edge_node_id"] = node
    cfg["mqtt"]["password"] = "mqtt-secret"      # the broker allows anonymous: ignored there
    cfg["web"] = {"username": "admin", "password": WEB_PASSWORD}
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

    new = json.loads(json.dumps(doc))
    new["sparkplug"]["edge_node_id"] = node2
    obs.drain()
    st7, _, b7 = http("PUT", "/api/config", new)
    death = obs.wait(f"{T}/NDEATH/{node}", 10)
    nb2 = obs.wait(f"{T}/NBIRTH/{node2}", 40)
    st8, _, b8 = wait_http("/api/config", 20)
    doc8 = as_json(b8) or {}
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
    obs.drain()
    asked = time.monotonic()
    gw.console("rebirth")
    rb = obs.wait(f"{T}/NBIRTH/{node2}", 5)
    said = gw.wait_log(r"^rebirth: NBIRTH and every DBIRTH requested", 3, asked)
    obs.wait(f"{T}/DBIRTH/{node2}/{BULK}", 5)
    dbs = sorted(m[2].seq for m in CAPTURE if m[0] >= asked and m[1].startswith(f"{T}/DBIRTH/{node2}/"))
    check("CFG-6 shell 'rebirth' re-issues NBIRTH (seq 0, same session) and the DBIRTHs",
          rb and rb[2].seq == 0 and dbs == [1, 2] and said,
          f"NBIRTH {'seq %d' % rb[2].seq if rb else 'missing'}, DBIRTH seq {dbs}, shell answer "
          f"{bool(said)}")

    def paste(doc_obj):
        gw.console("config load" if doc_obj.get("_save", True) else "config check")
        time.sleep(0.5)
        text = json.dumps({k: v for k, v in doc_obj.items() if k != "_save"}, indent=2)
        gw.sock.sendall(text.encode() + b"\n")

    bad = json.loads(json.dumps(doc8))
    bad["devices"][0]["tags"][0]["type"] = "float16"   # invalid (float64 is a type since MB-6)
    asked = time.monotonic()
    paste(bad)
    rej = gw.wait_log(r"^config: rejected: devices\[0\]\.tags\[0\]\.type: valor no válido", 15, asked)
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
    _, h = gw.shell("status", [r"^secrets\s+NVS encrypted \(HMAC key in eFuse KEY5\)"])
    enc = list(h.values())[0]
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node3}", 5)
    obs.close()
    # CFG-7: the secrets left the file at the first boot, NVS is encrypted, the web password is
    # only stored as a hash: none of them appears anywhere in the flash image in clear.
    image = pathlib.Path(flash).read_bytes()
    so, ss = storage_partition()
    no, ns = storage_partition("nvs")
    leaks = [(what.decode(), where) for what in (b"mqtt-secret", WEB_PASSWORD.encode())
             for where, (o, n) in (("storage", (so, ss)), ("nvs", (no, ns)), ("image", (0, len(image))))
             if what in image[o:o + n]]
    moved = gw.find(r"storage: secrets moved from /cfg/config\.json to NVS", t0)
    encrypted = gw.find(r'nvs: NVS partition "nvs" is encrypted', t0)
    check("CFG-7 secrets migrated out of config.json at the first boot (partition erased: no stale "
          "copy), NVS encrypted with the HMAC key in eFuse, web password only as a hash: no secret "
          "in clear anywhere in the flash image",
          moved and encrypted and enc and not leaks,
          f"{(moved or ['no migration log'])[0]}; {(encrypted or ['NVS not encrypted'])[0]}; "
          f"shell {bool(enc)}; secrets found in clear: {leaks or 'none'}")


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
    ap_try = gw.wait_log(r"ap: commissioning access point not available in this build", 3, t_short)
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
    ap_comm = gw.wait_log(r"ap: commissioning access point not available in this build", 10, t1 - 5)
    check("NET-5 commissioning access point requested on a 2-10 s BOOT press and when there is no "
          "configuration (QEMU has no radio: the request is logged; the SoftAP itself is compiled only)",
          ap_try and ap_comm, " | ".join(gw.find(r"ap: |BOOT pressed", t_short)[:3]))
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
    new = json.loads(config_json())
    new["sparkplug"]["edge_node_id"] = node2
    st2, _, b2 = http("PUT", "/api/config", new, auth=("admin", pw or "?"))
    nb2 = obs.wait(f"{T}/NBIRTH/{node2}", 40)
    check("CFG-5 commissioning: a configuration loaded through the REST API starts the gateway",
          st2 == 200 and nb2, f"PUT {st2} {b2.decode().strip()}; NBIRTH {node2}: {bool(nb2)}")
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node2}", 5)
    obs.close()
    image = pathlib.Path(flash).read_bytes()
    so, ss = storage_partition()
    stale = [w for w in (b'"GW07"', WEB_PASSWORD.encode()) if w in image[so:so + ss]]
    check("CFG-5 the factory reset erased the storage partition (no block of the old configuration "
          "is left)", not stale and b'"GW08"' in image[so:so + ss], f"stale: {stale or 'none'}")


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
                 f"{nb[0] - t0:.1f} s after QEMU start (REL-6 asks < 10 s)"):
        return
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
    meter = {r[1:] for t, r in zip(list(slave.times), list(slave.reads)) if r[0] == 1 and t >= t0}
    want = {(1, 0, 1), (3, 10, 12), (3, 34, 4), (3, 60, 1), (3, 30, 4), (3, 50, 8), (4, 0, 2),
            (4, 40, 7)}
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
                         r"^modbus_tcp enabled, port 502, idle timeout 60 s, clients 10\.0\.2\.2/32$"])
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
          f"NBIRTH counters {births}; NDATA {[(m.alias, m.long_value if m.datatype == 8 else m.boolean_value) for m in nd[2].metrics] if nd else None}")
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
                         r"01 1 03 0 0A 0 0B 1 \| other units ok 0 timeout 0 crc 0 frame 0 exception 0$"])
    check("Issue #69 Modbus TCP counters: forwarded transactions per unit, gateway connections, "
          "forwarded requests and its own exceptions (shell 'devices')", all(h.values()),
          " | ".join(gw.find(r"^(Meter1|Dead3)\s+unit|^modbus_tcp", asked)) or "no answer")
    check_modbus_tcp_table(obs, ndata_t)

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
    # down; the gateway retries with exponential backoff and a new bdSeq per attempt, and the
    # new session starts with NBIRTH and every DBIRTH.
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
    check("SPB-3 bdSeq incremented on every connection attempt",
          nb and b1 == (b0 + len(attempts) + 1) % 256,
          f"first session bdSeq={b0}, attempts while down={len(attempts)}, new NBIRTH bdSeq={b1}")
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

    # Restart on the same flash: bdSeq persisted in NVS
    gw = Gateway(args, log)
    gw.start()
    nb = obs.wait(nbirth_t, 40)
    b2 = bdseq_of(nb[2]) if nb else None
    check("SPB-3 bdSeq persisted across reboot (NVS)", nb and b2 == (b1 + 1) % 256,
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
    obs.publish_cmd(ncmd_t, [{"name": "Node Control/Reboot", "datatype": 11, "boolean_value": True}])
    nd = obs.wait(ndeath_t, 10)
    time.sleep(0.5)
    explicit = re.search(r"Received PUBLISH from GW01 \(d0, q1, r0, m\d+, 'spBv1\.0/Plant1/NDEATH/GW01'",
                         (work / "mosquitto2.log").read_text()) is not None
    nb = obs.wait(nbirth_t, 40)
    deaths = [m for m in CAPTURE if m[1] == ndeath_t and bdseq_of(m[2]) == b2]
    check("SPB-2/SPB-11 NCMD Reboot publishes NDEATH explicitly (QoS 1), disconnects and restarts",
          nd and explicit and bdseq_of(nd[2]) == b2 and len(deaths) == 1 and nb
          and bdseq_of(nb[2]) == (b2 + 1) % 256,
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
    while sum(1 for a in attempts if a["kind"] != "emulator") < args.panics:
        obs.drain()
        since = time.monotonic()
        gw.console("panic")
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
