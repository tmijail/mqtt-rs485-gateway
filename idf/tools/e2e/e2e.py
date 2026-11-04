#!/usr/bin/env python3
"""End-to-end test of the real firmware under QEMU (QA-3), against SPEC section 7.

Runs the ESP32-S3 image built with sdkconfig.qemu in Espressif's QEMU:
  * Ethernet: OpenCores MAC (open_eth) + slirp; the guest reaches the host as 10.0.2.2
  * UART1 (the RS485 port): QEMU chardev socket -> pymodbus RTU slave (RTU framing over TCP)
  * UART0 (console): socket used to read the log and to inject DI levels ("di <n> <0|1>")
  * MQTT: a private mosquitto; an SNTP responder answers on the host's UDP 123
  * Remote syslog (REL-4): a UDP listener on the host collects what the gateway forwards
  * Web server (CFG-3/CFG-4): slirp forwards the host's 127.0.0.1:HTTP_PORT to the guest's port 80

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
HTTP_PORT = int(os.environ.get("E2E_HTTP_PORT", 18080))
WEB_PASSWORD = "e2e-pass-1"
SYSLOG = []       # (t, datagram text) received by the syslog listener
SNTP_BOUND = threading.Event()  # set when the SNTP responder owns UDP 123
GROUP, NODE, DEVICE, BULK = "Plant1", "GW01", "Meter1", "Bulk"
T = f"spBv1.0/{GROUP}"
ALIAS = {"bdSeq": 1, "DI/1": 10, "DO/3": 22, "Voltage": 100, "Setpoint": 101, "Run": 102}

spb = spb_decode.load_module()
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
    """Unit 1 = Meter1 (SPEC example tags), unit 2 = Bulk (60 holding registers 100..159)."""

    def __init__(self):
        block = lambda: ModbusSequentialDataBlock(0, [0] * 300)  # noqa: E731
        self.ctx = RecordingContext(di=block(), co=block(), hr=block(), ir=block())
        self.bulk = RecordingContext(di=block(), co=block(), hr=block(), ir=block())
        self.bulk.setValues(3, 100, list(range(1000, 1060)))
        self.server_ctx = ModbusServerContext(slaves={1: self.ctx, 2: self.bulk}, single=False)
        self.reads = []  # (unit, function code, address, count) of every request received
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()
        self.server = None
        self.requests = 0

    def _trace(self, sending, pdu):
        if not sending:
            self.requests += 1
            self.reads.append((getattr(pdu, "dev_id", None), pdu.function_code,
                               getattr(pdu, "address", None), getattr(pdu, "count", None)))
        return pdu

    def start(self):
        async def run():
            self.server = ModbusTcpServer(self.server_ctx, framer=FramerType.RTU,
                                          address=("127.0.0.1", MODBUS_PORT), trace_pdu=self._trace)
            await self.server.listen()
        asyncio.run_coroutine_threadsafe(run(), self.loop).result(5)

    def stop(self):
        asyncio.run_coroutine_threadsafe(self.server.shutdown(), self.loop).result(5)

    def set_float(self, fc, addr, value):
        hi, lo = struct.unpack(">HH", struct.pack(">f", value))
        self.ctx.setValues(fc, addr, [hi, lo])

    def get(self, fc, addr, count=1):
        return self.ctx.getValues(fc, addr, count)


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
               "-nic", f"user,model=open_eth,hostfwd=tcp:127.0.0.1:{HTTP_PORT}-:80",
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


# ---------------------------------------------------------------- HTTP (CFG-3, CFG-4)

def http(method, path, body=None, auth=("admin", WEB_PASSWORD), timeout=15, headers=None):
    """(status, headers dict, body bytes) of one request to the gateway's web server, or
    (None, {}, error text) when it cannot be reached. @auth None = no Authorization header."""
    h = dict(headers or {})
    if auth is not None:
        h["Authorization"] = "Basic " + base64.b64encode(f"{auth[0]}:{auth[1]}".encode()).decode()
    if body is not None and not isinstance(body, bytes):
        body = json.dumps(body).encode()
        h.setdefault("Content-Type", "application/json")
    try:
        c = httplib.HTTPConnection("127.0.0.1", HTTP_PORT, timeout=timeout)
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, {k.lower(): v for k, v in r.getheaders()}, data
    except (OSError, httplib.HTTPException) as e:
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

def storage_partition():
    """(offset, size) of the "storage" partition in partitions.csv."""
    for line in (HERE.parent.parent / "partitions.csv").read_text().splitlines():
        cols = [c.strip() for c in line.split(",")]
        if cols[0] == "storage":
            return int(cols[3], 0), int(cols[4], 0)
    raise SystemExit("no storage partition in partitions.csv")


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


def config_json(**overrides):
    """tools/e2e/fs/config.json with the harness' MQTT port and optional top-level overrides."""
    cfg = json.loads((HERE / "fs" / "config.json").read_text())
    cfg["mqtt"]["port"] = MQTT_PORT
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
    global MQTT_PORT, MODBUS_PORT, CONSOLE_PORT, SYSLOG_PORT, HTTP_PORT
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", required=True)
    ap.add_argument("--flash", required=True)
    ap.add_argument("--efuse", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--littlefs-python", required=True,
                    help="python with the littlefs-python package (build-qemu/littlefs_py_venv)")
    ap.add_argument("--sdkconfig", required=True, help="sdkconfig of the QEMU build")
    ap.add_argument("--mqtt-port", type=int, default=MQTT_PORT)
    ap.add_argument("--modbus-port", type=int, default=MODBUS_PORT)
    ap.add_argument("--console-port", type=int, default=CONSOLE_PORT)
    ap.add_argument("--syslog-port", type=int, default=SYSLOG_PORT)
    ap.add_argument("--http-port", type=int, default=HTTP_PORT)
    ap.add_argument("--only", default="",
                    help="comma-separated scenarios to run (failsafe,network,web,reset,main); default all")
    ap.add_argument("--no-isolate", action="store_true",
                    help="stay on the host loopback instead of a private network namespace")
    args = ap.parse_args()
    MQTT_PORT, MODBUS_PORT, CONSOLE_PORT = args.mqtt_port, args.modbus_port, args.console_port
    SYSLOG_PORT, HTTP_PORT = args.syslog_port, args.http_port
    isolated = False if args.no_isolate else isolate_network()
    print(f"network: {'private namespace' if isolated else 'host loopback'}; ports MQTT {MQTT_PORT}, "
          f"Modbus {MODBUS_PORT}, console {CONSOLE_PORT}, syslog {SYSLOG_PORT}, HTTP {HTTP_PORT}",
          flush=True)
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
    slave.start()
    try:
        only = set(filter(None, args.only.split(",")))
        want = lambda name: not only or name in only  # noqa: E731
        if want("failsafe"):
            run_config_failsafe(args, work, slave)
        if want("network"):
            run_network_variant(args, work)
        if want("web"):
            run_web(args, work)
        if want("reset"):
            run_factory_reset(args, work)
        if want("main"):
            run(args, log, slave, broker, conf, work)
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
        ("semantic-error", {"config.json": good.replace(b'"type": "float32"', b'"type": "float64"', 1)},
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
        asked = time.monotonic()
        gw.console("status")
        still = gw.wait_log(r"nothing is running: no polling, no MQTT; recovery network and web UI only",
                            3, asked)
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
            ok_export = st2 == 200 and fixed.get("devices", [{}])[0].get("tags", [{}])[0].get("type") == "float64"
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
    bad["devices"][0]["tags"][0]["type"] = "float64"
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
    asked = time.monotonic()
    gw.console("mbread 1 holding 10 1")
    gw.console("mbread 1 input 0 2")
    gw.console("mbread 9 holding 0")
    reg = gw.wait_log(r"^mbread: unit 1 holding 10: 50$", 8, asked)
    flt = gw.wait_log(r"^mbread: unit 1 input 0: 17254 32768$", 8, asked)    # 230.5 as float32 ABCD
    absent = gw.wait_log(r"^mbread: unit 9 holding 0: (timeout|framing error)$", 15, asked)
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
    bad["devices"][0]["tags"][0]["type"] = "float64"
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
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node3}", 5)
    obs.close()


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
    # short press (3 s): the commissioning access point gesture, no reset
    t_short = time.monotonic()
    gw.console("boot 1")
    time.sleep(3)
    gw.console("boot 0")
    ap = gw.wait_log(r"button: BOOT pressed \d+ ms: commissioning access point", 3, t_short)
    ap_try = gw.wait_log(r"ap: commissioning access point not available in this build", 3, t_short)
    obs.drain()
    pressed = time.monotonic()
    gw.console("boot 1")
    warn = gw.wait_log(r"button: BOOT held: factory reset at 10 s", 5, pressed)
    fired = gw.wait_log(r"button: BOOT held 10 s: factory reset", 15, pressed)
    death = obs.wait(f"{T}/NDEATH/{node}", 10)
    commissioning = gw.wait_log(r"main: commissioning: no configuration since the factory reset", 30,
                                pressed)
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
    asked = time.monotonic()
    gw.console("status")
    shell = gw.wait_log(r"^config\s+NONE: commissioning after a factory reset", 3, asked)
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
    new = json.loads(config_json())
    new["sparkplug"]["edge_node_id"] = node2
    st2, _, b2 = http("PUT", "/api/config", new, auth=("admin", pw or "?"))
    nb2 = obs.wait(f"{T}/NBIRTH/{node2}", 40)
    check("CFG-5 commissioning: a configuration loaded through the REST API starts the gateway",
          st2 == 200 and nb2, f"PUT {st2} {b2.decode().strip()}; NBIRTH {node2}: {bool(nb2)}")
    gw.kill()
    obs.wait(f"{T}/NDEATH/{node2}", 5)
    obs.close()


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
    db = obs.wait(dbirth_t, 10)
    ok = db and abs(metric(db[2], name="Voltage").float_value - 230.5) < 1e-3 \
        and abs(metric(db[2], name="Setpoint").float_value - 5.0) < 1e-4
    check("SPB-6 DBIRTH with current values", ok,
          spb_decode.describe("DBIRTH", db[2]).replace("\n", " |") if db else "no DBIRTH")
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

    # CFG-6 diagnostic shell on the console UART
    asked = time.monotonic()
    gw.console("status")
    status_ok = gw.wait_log(r"config\s+\S+.*: ok", 3, asked)
    status_net = gw.wait_log(r"network\s+.*MQTT \S+, Modbus 2/2 devices online", 1, asked)
    status_if = gw.wait_log(r"^iface\s+ethernet up \d+\.\d+\.\d+\.\d+ \(active\) \| wifi not configured",
                            1, asked)
    check("CFG-6 console shell: 'status' with a valid configuration (config ok, MQTT, devices online, "
          "Ethernet the only and active interface)",
          status_ok and status_net and status_if,
          " | ".join(gw.find(r"^config\s|^network\s|^iface\s", asked)) or "no answer")
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
    up = wait_syslog(NODE, r"^Sparkplug session up \(bdSeq=%d\)$" % b0, 5, t0)
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
    obs.drain()
    slave.ctx.writes.clear()
    obs.publish_cmd(dcmd_t, [{"alias": ALIAS["Run"], "datatype": 11, "boolean_value": True}])
    dd = obs.wait(ddata_t, 5, lambda q: metric(q, alias=ALIAS["Run"]) is not None)
    check("SPB-10 DCMD by alias (Run=true via FC05)", dd and slave.get(1, 0)[0] and
          (5, 0, [True]) in slave.ctx.writes, f"slave writes {slave.ctx.writes}")
    since = time.monotonic()
    obs.publish_cmd(dcmd_t, [{"name": "Voltage", "datatype": 9, "float_value": 1.0}])
    time.sleep(1.5)
    check("SPB-10 DCMD on read-only metric discarded and logged",
          gw.find(r"DCMD: metric 'Voltage'.*read-only", since) and not slave.ctx.writes[1:],
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
    db = obs.wait(dbirth_t, 10)
    check("2b. Slave back -> DBIRTH", db, f"{db[0] - on:.2f} s after restart" if db else "no DBIRTH")

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
    obs.close()


if __name__ == "__main__":
    main()
