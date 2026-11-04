#!/usr/bin/env python3
"""End-to-end test of the real firmware under QEMU (QA-3), against SPEC section 7.

Runs the ESP32-S3 image built with sdkconfig.qemu in Espressif's QEMU:
  * Ethernet: OpenCores MAC (open_eth) + slirp; the guest reaches the host as 10.0.2.2
  * UART1 (the RS485 port): QEMU chardev socket -> pymodbus RTU slave (RTU framing over TCP)
  * UART0 (console): socket used to read the log and to inject DI levels ("di <n> <0|1>")
  * MQTT: a private mosquitto; an SNTP responder answers on the host's UDP 123
  * Remote syslog (REL-4): a UDP listener on the host collects what the gateway forwards

Isolation: by default the harness moves itself into a private network namespace (Linux,
needs CAP_SYS_ADMIN, e.g. root in a container) with only a loopback interface. Every port,
UDP 123 included, is then private to the run, so parallel runs never collide. Without that
capability it falls back to the host's loopback with the ports given by --mqtt-port,
--modbus-port and --console-port (or E2E_MQTT_PORT, E2E_MODBUS_PORT, E2E_CONSOLE_PORT).

Usage: tools/e2e/run_e2e.sh  (prepares the flash image and calls this script)
"""
import argparse
import asyncio
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

import google.protobuf
import paho.mqtt.client as mqtt
from pymodbus import FramerType
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
SYSLOG = []       # (t, datagram text) received by the syslog listener
SNTP_BOUND = threading.Event()  # set when the SNTP responder owns UDP 123
PROBE_MAX_S = 10.0                # CONFIG_GW_MB_OFFLINE_PROBE_MAX_MS / 1000, read in main()
GROUP, NODE, DEVICE, BULK = "Plant1", "GW01", "Meter1", "Bulk"
T = f"spBv1.0/{GROUP}"
ALIAS = {"bdSeq": 1, "DI/1": 10, "DO/1": 20, "DO/3": 22, "Voltage": 100, "Setpoint": 101, "Run": 102,
         "Limit": 103, "Energy": 104, "Counter": 105, "Flow": 106, "Temp": 107, "Serial": 108,
         "Alarm": 109, "Ready": 110}
# MB-6 values served by unit 1, and the registers that hold them (see ModbusSlave.set_mb6)
MB6 = {"Energy": 2 ** 53 + 1, "Counter": -(2 ** 53 + 1), "Flow": 1234.56789012345, "Temp": -123456,
       "Serial": "SN-0042-ABCD", "Alarm": True, "Ready": True}

spb = spb_decode.load_module()
# The dead units (Dead3..Dead5) get no answer by design; pymodbus would log every request.
logging.getLogger("pymodbus").setLevel(logging.CRITICAL)
results = []
CAPTURE = []      # every message seen by any observer: (t, topic, Payload, raw bytes)
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

    def _trace(self, sending, pdu):
        if not sending:
            self.requests += 1
            self.times.append(time.monotonic())
            self.reads.append((getattr(pdu, "dev_id", None), pdu.function_code,
                               getattr(pdu, "address", None), getattr(pdu, "count", None)))
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
        if len(data) < 48:
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
      "refuse": every SUBSCRIBE gets a SUBACK with 0x80 (failure) for each topic.
    Records the connection times, the PUBLISH topics and whether the client sent DISCONNECT."""

    def __init__(self, port, mode):
        self.mode, self.accepted, self.published, self.disconnects = mode, [], [], 0
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
                    c.sendall(bytes([0x90, 2 + n]) + body[:2] + b"\x80" * n)
                elif kind >> 4 == 3:                     # PUBLISH
                    tlen = struct.unpack(">H", body[:2])[0]
                    self.published.append(body[2:2 + tlen].decode(errors="replace"))
                elif kind >> 4 == 12:                    # PINGREQ
                    c.sendall(b"\xd0\x00")
                elif kind >> 4 == 14:                    # DISCONNECT
                    self.disconnects += 1
                    return
        except OSError:
            pass
        finally:
            c.close()

    def close(self):
        self.srv.close()


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

    def __init__(self):
        self.q = queue.Queue()
        self.subscribed = threading.Event()
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"e2e-{time.monotonic()}")
        self.c.on_message = self._on_msg
        self.c.on_subscribe = lambda *a: self.subscribed.set()
        # Long keepalive: the observer must survive the frozen-broker scenario.
        self.c.connect("127.0.0.1", MQTT_PORT, keepalive=120)
        self.c.subscribe("spBv1.0/#", qos=1)
        self.c.loop_start()
        if not self.subscribed.wait(5):
            raise SystemExit("observer: no SUBACK from the broker")

    def close(self):
        self.c.loop_stop()
        self.c.disconnect()

    def _on_msg(self, client, userdata, msg):
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


# ---------------------------------------------------------------- QEMU

class Gateway:
    def __init__(self, args, log_path, flash=None):
        self.args, self.log_path = args, log_path
        self.flash = flash or args.flash
        self.proc, self.sock, self.lines = None, None, []

    def start(self):
        a = self.args
        cmd = [a.qemu, "-M", "esp32s3", "-m", "32M", "-nographic",
               "-drive", f"file={self.flash},if=mtd,format=raw",
               "-drive", f"file={a.efuse},if=none,format=raw,id=efuse",
               "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
               "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
               "-nic", "user,model=open_eth",
               "-chardev", f"socket,id=con,host=127.0.0.1,port={CONSOLE_PORT},server=on,wait=on",
               "-serial", "chardev:con",
               "-chardev", f"socket,id=rs485,host=127.0.0.1,port={MODBUS_PORT},reconnect-ms=500",
               "-serial", "chardev:rs485",
               "-monitor", "none"]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=open(str(self.log_path) + ".qemu", "ab"))
        PROCS.append(self.proc)
        for _ in range(100):
            try:
                self.sock = socket.create_connection(("127.0.0.1", CONSOLE_PORT))
                break
            except OSError:
                time.sleep(0.1)
        threading.Thread(target=self._reader, args=(self.sock,), daemon=True).start()

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
        self.sock.sendall(text.encode() + b"\n")

    def kill(self):
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait()
        self.sock.close()

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


# ---------------------------------------------------------------- storage partition variants

def storage_partition(name="storage"):
    """(offset, size) of partition @name in partitions.csv."""
    for line in (HERE.parent.parent / "partitions.csv").read_text().splitlines():
        cols = [c.strip() for c in line.split(",")]
        if cols[0] == name:
            return int(cols[3], 0), int(cols[4], 0)
    raise SystemExit(f"no {name} partition in partitions.csv")


def flash_variant(args, work, name, files):
    """Copy of the flash image whose LittleFS partition holds @files ({name: bytes});
    files=None leaves the partition erased (not mountable)."""
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
    flash = bytearray(pathlib.Path(args.flash).read_bytes())
    flash[offset:offset + len(image)] = image
    out = work / f"flash-{name}.bin"
    out.write_bytes(flash)
    return out


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


# ---------------------------------------------------------------- scenario

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
    global MQTT_PORT, MODBUS_PORT, CONSOLE_PORT, SYSLOG_PORT
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
    ap.add_argument("--mqtt-port", type=int, default=MQTT_PORT)
    ap.add_argument("--modbus-port", type=int, default=MODBUS_PORT)
    ap.add_argument("--console-port", type=int, default=CONSOLE_PORT)
    ap.add_argument("--syslog-port", type=int, default=SYSLOG_PORT)
    ap.add_argument("--no-isolate", action="store_true",
                    help="stay on the host loopback instead of a private network namespace")
    args = ap.parse_args()
    MQTT_PORT, MODBUS_PORT, CONSOLE_PORT = args.mqtt_port, args.modbus_port, args.console_port
    SYSLOG_PORT = args.syslog_port
    global PROBE_MAX_S
    PROBE_MAX_S = sdkconfig_int(args.sdkconfig, "GW_MB_OFFLINE_PROBE_MAX_MS") / 1000
    isolated = False if args.no_isolate else isolate_network()
    print(f"network: {'private namespace' if isolated else 'host loopback'}; ports MQTT {MQTT_PORT}, "
          f"Modbus {MODBUS_PORT}, console {CONSOLE_PORT}, syslog {SYSLOG_PORT}", flush=True)
    work = pathlib.Path(args.workdir)
    log = work / "uart0.log"
    # The image under test gets the harness' config.json (with its MQTT port).
    args.flash = str(flash_variant(args, work, "main", {"config.json": config_json()}))

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
        run_config_failsafe(args, work)
        run_network_variant(args, work)
        run(args, log, slave, broker, conf, work)
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
        (work / "results.json").write_text(json.dumps(results, indent=2))
        failed = [r for r in results if not r["ok"]]
        print(f"\n{len(results) - len(failed)}/{len(results)} checks passed; artefacts in {work}")
        sys.exit(1 if failed else 0)


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


def run_config_failsafe(args, work):
    """CFG-1: the embedded default is used only when /cfg/config.json does not exist; any
    other problem stops the gateway (no polling, no MQTT) with the error state and a log."""
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
        t0 = time.monotonic()
        gw.start()
        hit = gw.wait_log(r"configuration rejected .*" + reason, 20, t0)
        halted = gw.wait_log(r"nothing is started", 2, t0)
        led = gw.wait_log(r"status: config error", 2, t0)
        dos_off = gw.find(r"io: DO .*all outputs off", t0)
        time.sleep(6)  # long enough for DHCP + SNTP wait + a connection attempt
        # Positive signal: 6 s later the shell still reports the halted state.
        asked = time.monotonic()
        gw.console("status")
        still = gw.wait_log(r"nothing is running: no polling, no network, no MQTT", 3, asked)
        shell = gw.find(r"config\s+REJECTED .*" + reason, asked)
        # Supplementary negative signals: no component logged its start, no NBIRTH reached MQTT.
        started = gw.find(r"configuration from|connecting to|rs485: UART|net: IPv4", t0)
        births = [m for m in CAPTURE if m[0] >= t0 and "/NBIRTH/" in m[1]]
        gw.kill()
        if name == "oversized":
            check("CFG-6 console shell answers 'status' while the configuration is rejected",
                  shell, shell[0] if shell else "no answer")
        check(f"IO-2 {name} config file -> DOs switched off before the configuration is loaded",
              bool(dos_off), (dos_off or ["no 'all outputs off' log"])[0])
        check(f"CFG-1 {name} config file -> halted (shell 'status' after 6 s), error LED, ERROR log "
              f"with the reason, nothing started",
              hit and halted and still and shell and led and not started and not births,
              (gw.find(r"configuration rejected", t0) or ["no 'configuration rejected' log"])[0] +
              f"; halted={bool(halted)}; shell still halted={bool(still)}; LED error={bool(led)}; "
              f"started={started[:1]}; NBIRTHs={len(births)}")

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


def check_scan_classes(slave):
    """MB-12: Meter1's slow-scan blocks (Energy at holding 30, Serial at 50; slow_poll_ms 3000)
    are read every third cycle, its fast ones (cycle start: coil 0) every second."""
    since = time.monotonic() - 12
    reads = [(t, r[1:]) for t, r in zip(list(slave.times), list(slave.reads)) if r[0] == 1 and t >= since]
    fast = [t for t, k in reads if k == (1, 0, 1)]
    slow = [t for t, k in reads if k == (3, 30, 4)]
    serial = [t for t, k in reads if k == (3, 50, 8)]
    gaps = [round(b - a, 2) for a, b in zip(slow, slow[1:])]
    fgaps = [b - a for a, b in zip(fast, fast[1:])]
    ok = len(slow) >= 3 and len(serial) == len(slow) and all(2.8 <= g <= 3.6 for g in gaps) \
        and len(fast) >= 10 and max(fgaps) <= 1.6
    return check("MB-12 slow-scan tags read every slow_poll_ms (3 s), fast ones every poll_ms (1 s)",
                 ok, f"last 12 s: {len(fast)} fast cycles (longest gap {max(fgaps, default=0):.2f} s), "
                 f"{len(slow)} slow reads {gaps} s apart, Serial read {len(serial)} times")


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


def run_network_variant(args, work):
    """Optional network keys on the emulator, in one boot with a separate edge node id (its
    messages stay apart from the main scenario's):
      * NET-4: no Wi-Fi radio (CONFIG_GW_WIFI=n): a configuration that enables network.wifi still
        boots, says that Wi-Fi is not available and runs over Ethernet exactly as before.
      * NET-7: slirp announces fec0::/64 and answers ICMPv6 for its virtual host fec0::2. The
        gateway must get a link-local address, a SLAAC address and the static one, and reach
        fec0::2 over IPv6. MQTT over IPv6 cannot be tested here: slirp would relay it to the
        host's ::1 and this container's kernel has no IPv6 (AF_INET6 is not supported)."""
    node = "GW02"
    cfg = json.loads(config_json())
    cfg["sparkplug"]["edge_node_id"] = node
    cfg["network"] = {"dhcp": True,
                      "ipv6": {"enabled": True, "slaac": True, "address": "fec0::1234"},
                      "wifi": {"enabled": True, "ssid": "Plant WLAN", "password": "12345678",
                               "role": "backup", "failback_s": 30}}
    flash = flash_variant(args, work, "network", {"config.json": json.dumps(cfg, indent=2).encode()})
    obs = Observer()
    gw = Gateway(args, work / "uart0-network.log", flash)
    t0 = time.monotonic()
    gw.start()
    nb = obs.wait(f"{T}/NBIRTH/{node}", 40)
    ignored = gw.find(r"network\.wifi ignored: Wi-Fi is not available in this build", t0)
    asked = time.monotonic()
    iface = shown = None
    for _ in range(3):   # a log line from another task can split the shell's output: ask again
        gw.console("status")
        gw.console("config")
        iface = iface or gw.wait_log(
            r"^iface\s+ethernet up \d+\.\d+\.\d+\.\d+ \(active\) \| wifi not configured", 3, asked)
        shown = shown or gw.wait_log(r"^wifi\s+'Plant WLAN', backup of Ethernet, failback 30 s, DHCP",
                                     1, asked)
        if iface and shown:
            break
    # NET-7
    lla = gw.wait_log(r"net: ethernet: IPv6 fe80:[0-9a-f:]+ \(link-local\)", 10, t0)
    slaac = gw.wait_log(r"net: ethernet: IPv6 fec0::(?!1234 )[0-9a-f:]+ \(site local\)", 10, t0)
    static = gw.wait_log(r"net: ethernet: IPv6 fec0::1234 \(site local\)", 1, t0)
    asked6 = time.monotonic()
    listed = cfg6 = None
    for _ in range(3):
        gw.console("status")
        gw.console("config")
        listed = listed or gw.wait_log(r"^ipv6\s+ethernet fe80:.*ethernet fec0::", 3, asked6)
        cfg6 = cfg6 or gw.wait_log(r"^ipv6\s+SLAAC on, static address fec0::1234, DNS none", 1, asked6)
        if listed and cfg6:
            break
    pinged = time.monotonic()
    gw.console("ping fec0::2")
    replies = gw.wait_log(r"3 sent, [1-3] received", 8, pinged)
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node}", 5)   # the will, before the main scenario starts
    obs.close()
    check("NET-4 network.wifi on a build without the radio (QEMU): accepted, reported, Ethernet "
          "carries the session as before",
          nb and ignored and iface and shown,
          f"NBIRTH {'yes' if nb else 'no'}; {(ignored or ['no warning'])[0]}; shell: "
          + " | ".join(gw.find(r"^iface\s|^wifi\s", asked)))
    check("NET-7 IPv6 on Ethernet: link-local, SLAAC (fec0::/64 from slirp's RA) and static "
          "address, shown by the shell; ICMPv6 echo to fec0::2 answered",
          lla and slaac and static and listed and cfg6 and replies,
          " | ".join(gw.find(r"net: ethernet: IPv6 ", t0)[:4]) + " | "
          + " | ".join(gw.find(r"^ipv6\s|reply from|sent, \d+ received", asked6)))


def run(args, log, slave, broker, conf, work):
    nbirth_t, dbirth_t = f"{T}/NBIRTH/{NODE}", f"{T}/DBIRTH/{NODE}/{DEVICE}"
    ndata_t, ddata_t = f"{T}/NDATA/{NODE}", f"{T}/DDATA/{NODE}/{DEVICE}"
    ndeath_t, ddeath_t = f"{T}/NDEATH/{NODE}", f"{T}/DDEATH/{NODE}/{DEVICE}"
    ncmd_t, dcmd_t = f"{T}/NCMD/{NODE}", f"{T}/DCMD/{NODE}/{DEVICE}"

    obs = Observer()
    gw = Gateway(args, log)
    t0 = time.monotonic()
    gw.start()
    nb = obs.wait(nbirth_t, 40)
    if not check("1a. NBIRTH published", nb, "no NBIRTH within 40 s" if not nb else
                 f"{nb[0] - t0:.1f} s after QEMU start (REL-6 asks < 10 s)"):
        return
    p = nb[2]
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
    meter = {r[1:] for r in slave.reads if r[0] == 1}
    want = {(1, 0, 1), (3, 10, 12), (3, 34, 4), (3, 60, 1), (3, 30, 4), (3, 50, 8), (4, 0, 2),
            (4, 40, 7)}
    check("MB-11 / issue #38 per-device max_gap: Meter1 (max_gap 10) bridges holes up to 10 "
          "registers, Bulk keeps the build default", meter == want,
          f"Meter1 requests {sorted(meter)} (expected {sorted(want)})")

    # CFG-6 diagnostic shell on the console UART
    asked = time.monotonic()
    gw.console("status")
    status_ok = gw.wait_log(r"config\s+\S+.*: ok", 3, asked)
    status_net = gw.wait_log(r"network\s+.*MQTT \S+, Modbus 2/5 devices online", 1, asked)
    status_if = gw.wait_log(r"^iface\s+ethernet up \d+\.\d+\.\d+\.\d+ \(active\) \| wifi not configured",
                            1, asked)
    check("CFG-6 console shell: 'status' with a valid configuration (config ok, MQTT, devices online, "
          "Ethernet the only and active interface)",
          status_ok and status_net and status_if,
          " | ".join(gw.find(r"^config\s|^network\s|^iface\s", asked)) or "no answer")
    # Issue #15: the network stack runs on core 0, the RS485 bus owner on core 1
    tasks = gw.find(r"^tasks\s", asked)
    want = ["rs485_bus 1", "sparkplug 0", "tcpip 0", "mqtt_task 0", "emac_rx 0", "sys_evt 0"]
    check("Issue #15 task affinity: lwIP, esp-mqtt, Ethernet RX and event loop on core 0, bus on core 1",
          tasks and all(f" {w}" in tasks[0] for w in want), tasks[0] if tasks else "no 'tasks' line")
    asked = time.monotonic()
    gw.console("devices")
    gw.console("config")
    meter = gw.wait_log(r"Meter1\s+unit\s+1 online\s+cycles [1-9]\d* failed \d+ \| ok [1-9]", 3, asked)
    bulk = gw.wait_log(r"Bulk\s+unit\s+2 online", 1, asked)
    summary = gw.wait_log(r"sparkplug\s+group 'Plant1', edge node 'GW01'", 1, asked)
    check("CFG-6 console shell: 'devices' (online state, counters) and 'config' summary",
          meter and bulk and summary,
          " | ".join(gw.find(r"Meter1\s+unit|Bulk\s+unit|sparkplug\s+group", asked)) or "no answer")

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
    asked = time.monotonic()
    stats = None
    for _ in range(3):
        gw.console("status")
        stats = gw.wait_log(r"^syslog\s+10\.0\.2\.2:%d connected, sent [1-9]\d*, " % SYSLOG_PORT, 3, asked)
        if stats:
            break
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

    # 3. DI -> NDATA < 100 ms; NCMD DO/3
    obs.drain()
    sent = time.monotonic()
    gw.console("di 1 1")
    nd = obs.wait(ndata_t, 2, lambda q: metric(q, alias=ALIAS["DI/1"]) is not None)
    lat = (nd[0] - sent) * 1000 if nd else None
    check("3a. DI change visible in NDATA < 100 ms", nd and lat < 100 and
          metric(nd[2], alias=ALIAS["DI/1"]).boolean_value,
          f"{lat:.0f} ms console->NDATA (includes 20 ms debounce and emulator latency)" if nd else "no NDATA")
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

    # 2. Slave off -> DDEATH; back on -> DBIRTH
    obs.drain()
    off = time.monotonic()
    slave.stop()
    dd = obs.wait(ddeath_t, 15)
    bound = 3 * 1.0 + 3 * (0.2 * 3) + 1.0  # offline_after*poll + per-cycle timeouts + 1 s margin
    check("2a. Slave powered off -> DDEATH within offline_after x poll_ms + timeouts",
          dd and dd[0] - off <= bound, f"{dd[0] - off:.2f} s (bound {bound:.1f} s)" if dd else "no DDEATH")
    asked = time.monotonic()
    gw.console("devices")
    offline = gw.wait_log(r"Meter1\s+unit\s+1 offline\s+cycles \d+ failed [1-9]\d* \| ok \d+ timeout [1-9]",
                          3, asked)
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

    # MQTT-2 broker restart: the broker publishes the session's will (NDEATH) while shutting
    # down; the gateway retries with exponential backoff and a new bdSeq per attempt, and the
    # new session starts with NBIRTH and every DBIRTH.
    obs.drain()
    since = time.monotonic()
    broker.terminate()
    broker.wait()
    will = obs.wait(ndeath_t, 2)
    time.sleep(9)
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

    # SPB-2 orderly disconnect: NCMD Reboot -> explicit NDEATH, then reboot
    obs.drain()
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
    reason1 = metric(nb[2], name="Node Info/Reset Reason") if nb else None
    check("REL-5 NBIRTH 'Node Info/Reset Reason': power-on at first boot, software after NCMD Reboot",
          reason0 and reason1 and reason0.string_value == "power-on"
          and reason1.string_value == "software",
          f"first boot {reason0.string_value if reason0 else None!r}, after reboot "
          f"{reason1.string_value if reason1 else None!r}")

    # Issue #12 / REL-1: a panic writes a core dump to the "coredump" partition, the gateway
    # restarts ("panic" in NBIRTH) and the dump decodes with esp_coredump against the ELF.
    # QEMU does not seem to stall the other core during the panic handler as the chip does: a
    # task on it (the 1 kHz DI sampling, for one) can change a stack between the two passes of
    # the ELF writer, and the dump then fails its checksum. Up to 3 panics; every one must save
    # a dump and restart with "panic", and at least one dump must decode.
    off, size = storage_partition("coredump")
    attempts = []
    for n in range(3):
        obs.drain()
        since = time.monotonic()
        gw.console("panic")
        nb = obs.wait(nbirth_t, 40)
        saved = gw.find(r"Core dump has been saved to flash", since)
        reason = metric(nb[2], name="Node Info/Reset Reason") if nb else None
        core = work / f"coredump-{n}.bin"
        core.write_bytes(pathlib.Path(args.flash).read_bytes()[off:off + size])
        info = subprocess.run([args.idf_python, "-m", "esp_coredump", "--chip", "esp32s3",
                               "info_corefile", "--core", str(core), "--core-format", "raw", args.elf],
                              capture_output=True, text=True, timeout=120)
        decoded = info.returncode == 0 and "cmd_panic" in info.stdout and "abort" in info.stdout
        if decoded or n == 0:
            (work / "coredump-info.txt").write_text(info.stdout + info.stderr)
        err = (info.stderr.strip().splitlines() or [""])[-1][:90]
        attempts.append((bool(saved), reason.string_value if reason else None, decoded, err))
        if decoded:
            break
    gw.kill()
    check("Issue #12 panic -> core dump in flash, restart with 'panic' reset reason, dump decodes "
          "with esp_coredump (backtrace through cmd_panic/abort)",
          all(a[0] and a[1] == "panic" for a in attempts) and attempts[-1][2],
          "; ".join(f"attempt {i + 1}: saved={a[0]} reset reason={a[1]!r} decoded={a[2]}"
                    f"{'' if a[2] else ' (' + a[3] + ')'}" for i, a in enumerate(attempts)))

    # Issue #11: a broker that refuses the NCMD/DCMD subscriptions (SUBACK 0x80). The node must
    # not announce itself (no NBIRTH: it could not receive commands); it disconnects with
    # DISCONNECT (the will is dropped) and retries with a growing backoff.
    refuser = FakeBroker(MQTT_PORT + 2, "refuse")
    gw = Gateway(args, work / "uart0-refuse.log",
                 flash_variant(args, work, "refuse", {"config.json": config_json(MQTT_PORT + 2)}))
    t_ref = time.monotonic()
    gw.start()
    gw.wait_log(r"reconnect attempt 3 in", 40, t_ref)
    refused = gw.find(r"refused the NCMD/DCMD subscriptions", t_ref)
    delays = [int(x) for x in re.findall(r"in (\d+) ms", " ".join(gw.find(r"reconnect attempt", t_ref)))]
    gw.kill()
    refuser.close()
    check("MQTT-2 / issue #11 SUBACK refused -> no NBIRTH, DISCONNECT and retry with growing backoff",
          len(refused) >= 3 and not refuser.published and refuser.disconnects >= 3
          and len(delays) >= 3 and delays[2] > delays[0],
          f"{len(refused)} refusals logged, {refuser.disconnects} DISCONNECTs, publishes "
          f"{refuser.published}, delays {delays} ms; {(refused or ['no refusal logged'])[0]}")

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
    obs.close()


if __name__ == "__main__":
    main()
