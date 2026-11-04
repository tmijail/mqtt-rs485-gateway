#!/usr/bin/env python3
"""End-to-end test of the real firmware under QEMU (QA-3), against SPEC section 7.

Runs the ESP32-S3 image built with sdkconfig.qemu in Espressif's QEMU:
  * Ethernet: OpenCores MAC (open_eth) + slirp; the guest reaches the host as 10.0.2.2
  * UART1 (the RS485 port): QEMU chardev socket -> pymodbus RTU slave (RTU framing over TCP)
  * UART0 (console): socket used to read the log and to inject DI levels ("di <n> <0|1>")
  * MQTT: a private mosquitto; an SNTP responder answers on the host's UDP 123

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
SNTP_BOUND = threading.Event()  # set when the SNTP responder owns UDP 123
PROBE_MAX_S = 10.0                # CONFIG_GW_MB_OFFLINE_PROBE_MAX_MS / 1000, read in main()
GROUP, NODE, DEVICE, BULK = "Plant1", "GW01", "Meter1", "Bulk"
T = f"spBv1.0/{GROUP}"
ALIAS = {"bdSeq": 1, "DI/1": 10, "DO/3": 22, "Voltage": 100, "Setpoint": 101, "Run": 102, "Limit": 103}

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


# ---------------------------------------------------------------- broker that drops at once

class ConnackCutter:
    """TCP proxy in front of the broker that closes every connection right after relaying the
    broker's CONNACK: a broker that accepts the connection and drops it at once (ACL that
    rejects the will, duplicate client_id, connection limit)."""

    def __init__(self, port, upstream):
        self.upstream, self.accepted = upstream, []
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
            threading.Thread(target=self._pipe, args=(c,), daemon=True).start()

    def _pipe(self, c):
        u = None
        try:
            c.settimeout(10)
            u = socket.create_connection(("127.0.0.1", self.upstream), timeout=10)
            u.sendall(c.recv(4096))   # CONNECT
            c.sendall(u.recv(4096))   # CONNACK
            time.sleep(0.1)
        except OSError:
            pass
        finally:
            for sock in (c, u):
                if sock is not None:
                    sock.close()

    def close(self):
        self.srv.close()


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


def config_json(mqtt_port=None, **overrides):
    """tools/e2e/fs/config.json with the harness' MQTT port (or @mqtt_port) and optional
    top-level overrides."""
    cfg = json.loads((HERE / "fs" / "config.json").read_text())
    cfg["mqtt"]["port"] = mqtt_port or MQTT_PORT
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
    global MQTT_PORT, MODBUS_PORT, CONSOLE_PORT
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
    ap.add_argument("--no-isolate", action="store_true",
                    help="stay on the host loopback instead of a private network namespace")
    args = ap.parse_args()
    MQTT_PORT, MODBUS_PORT, CONSOLE_PORT = args.mqtt_port, args.modbus_port, args.console_port
    global PROBE_MAX_S
    PROBE_MAX_S = sdkconfig_int(args.sdkconfig, "GW_MB_OFFLINE_PROBE_MAX_MS") / 1000
    isolated = False if args.no_isolate else isolate_network()
    print(f"network: {'private namespace' if isolated else 'host loopback'}; ports MQTT {MQTT_PORT}, "
          f"Modbus {MODBUS_PORT}, console {CONSOLE_PORT}", flush=True)
    work = pathlib.Path(args.workdir)
    log = work / "uart0.log"
    # The image under test gets the harness' config.json (with its MQTT port).
    args.flash = str(flash_variant(args, work, "main", {"config.json": config_json()}))

    conf = work / "broker.conf"
    # Second listener (MQTT_PORT + 2) with an ACL that refuses the NCMD subscription (issue #11).
    acl = work / "broker.acl"
    acl.write_text(f"topic write spBv1.0/#\ntopic read {T}/DCMD/#\n")
    conf.write_text(f"per_listener_settings true\n"
                    f"listener {MQTT_PORT} 127.0.0.1\nallow_anonymous true\n"
                    f"listener {MQTT_PORT + 2} 127.0.0.1\nallow_anonymous true\nacl_file {acl}\n")
    broker = start_broker(conf, work / "mosquitto.log")
    stop = threading.Event()
    threading.Thread(target=sntp_responder, args=(stop,), daemon=True).start()

    slave = ModbusSlave()
    slave.set_float(4, 0, 230.5)       # Voltage, input 0..1, float32 ABCD
    slave.ctx.setValues(3, 10, [50])   # Setpoint raw 50 -> 5.0
    slave.start()
    try:
        run_config_failsafe(args, work)
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
        ("semantic-error", {"config.json": good.replace(b'"type": "float32"', b'"type": "float64"', 1)},
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
    """Issue #4: dead slaves (units 3..5, never answer) are probed with one attempt at 2, 4, 8 ...
    x poll_ms up to the cap, and do not delay the healthy device's 1 s polling."""
    reads = list(zip(slave.times, slave.reads))
    dead = [t for t, r in reads if r[0] == 3 and t >= t0]
    # the first 3 cycles are full (1 try + 2 retries each); then one request per probe
    gaps = [round(b - a, 1) for a, b in zip(dead[9:], dead[10:])]
    expect = [min(2.0 * 2 ** i, PROBE_MAX_S) for i in range(len(gaps))]
    probes_ok = len(gaps) >= 3 and all(abs(g - e) <= 0.6 for g, e in zip(gaps, expect))
    check("MB-9 / issue #4 offline device probed with a single attempt and an exponential "
          "backoff (2, 4, 8 ... s, cap)", probes_ok,
          f"{len(dead)} requests to dead unit 3; gaps after the first 3 cycles {gaps} s "
          f"(expected {expect})")
    # Healthy Meter1 while the three dead units back off: cycle starts = its first request kind
    if len(dead) < 10:
        return check("Issue #4 healthy device keeps its period next to 3 dead ones", False,
                     "dead devices never went offline")
    since = dead[9]
    meter = [(t, r) for t, r in reads if r[0] == 1 and t >= since]
    first = meter[0][1][1:] if meter else None
    starts = [t for t, r in meter if r[1:] == first]
    span = starts[-1] - starts[0] if len(starts) > 1 else 0
    worst = max((b - a for a, b in zip(starts, starts[1:])), default=0)
    # 1 s period; at worst one probe of each dead unit (200 ms timeout) in between + jitter
    ok = len(starts) >= 8 and (len(starts) - 1) / span >= 0.9 and worst <= 1.0 + 3 * 0.2 + 0.4
    return check("Issue #4 healthy device keeps its 1 s period next to 3 dead ones", ok,
                 f"{len(starts)} Meter1 cycles in {span:.1f} s, longest interval {worst:.2f} s")


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
    status_net = gw.wait_log(r"network\s+.*MQTT \S+, Modbus 2/5 devices online", 1, asked)
    check("CFG-6 console shell: 'status' with a valid configuration (config ok, MQTT, devices online)",
          status_ok and status_net,
          " | ".join(gw.find(r"^config\s|^network\s", asked)) or "no answer")
    asked = time.monotonic()
    gw.console("devices")
    gw.console("config")
    meter = gw.wait_log(r"Meter1\s+unit\s+1 online\s+cycles [1-9]\d* failed \d+ \| ok [1-9]", 3, asked)
    bulk = gw.wait_log(r"Bulk\s+unit\s+2 online", 1, asked)
    summary = gw.wait_log(r"sparkplug\s+group 'Plant1', edge node 'GW01'", 1, asked)
    check("CFG-6 console shell: 'devices' (online state, counters) and 'config' summary",
          meter and bulk and summary,
          " | ".join(gw.find(r"Meter1\s+unit|Bulk\s+unit|sparkplug\s+group", asked)) or "no answer")

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

    check_offline_backoff(slave, t0)

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

    # Issue #11: a broker that refuses the NCMD subscription (ACL, SUBACK 0x80). The node must
    # not announce itself (no NBIRTH: it could not receive commands); it disconnects and
    # retries with a growing backoff.
    gw.kill()
    gw = Gateway(args, work / "uart0-acl.log",
                 flash_variant(args, work, "acl", {"config.json": config_json(MQTT_PORT + 2)}))
    obs.drain()
    t_acl = time.monotonic()
    gw.start()
    gw.wait_log(r"reconnect attempt 3 in", 40, t_acl)
    refused = gw.find(r"refused the NCMD/DCMD subscriptions", t_acl)
    delays = [int(x) for x in re.findall(r"in (\d+) ms", " ".join(gw.find(r"reconnect attempt", t_acl)))]
    births = [m for m in CAPTURE if m[0] >= t_acl and m[1] == nbirth_t]
    gw.kill()
    check("MQTT-2 / issue #11 SUBACK refused -> no NBIRTH, disconnect and retry with growing backoff",
          len(refused) >= 3 and not births and len(delays) >= 3 and delays[2] > delays[0],
          f"{len(refused)} refusals logged, NBIRTHs {len(births)}, delays {delays} ms; "
          f"{(refused or ['no refusal logged'])[0]}")

    # Issue #5: a broker that accepts the connection and drops it right after the CONNACK.
    # The backoff must keep growing (1, 2, 4, 8 s, -25 % jitter): it is reset only after a
    # stable session, not on every CONNACK.
    cutter = ConnackCutter(MQTT_PORT + 1, MQTT_PORT)
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
