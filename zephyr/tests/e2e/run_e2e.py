#!/usr/bin/env python3
"""End-to-end test of the gateway firmware built for native_sim (QA-3).

Topology (everything on the host):

  zephyr.exe (native_sim) --NSOS sockets--> mosquitto :18830 <-- this script (paho)
       |  native TTY UART
       +-- /tmp/gw-rs485 <== socat pty pair ==> /tmp/gw-slave -- modbus_slave.py (pymodbus)
  SNTP server on 127.0.0.1:123 (in this script, needs root)

Every Sparkplug payload is decoded with the official Eclipse Tahu
sparkplug_b.proto through Python protobuf. The script checks the SPEC
section 7 acceptance criteria plus a few extra Sparkplug rules and prints
one PASS/FAIL line per check. Exit status 0 means every check passed.

Usage: run_e2e.py --exe build/zephyr/zephyr.exe [--out DIR]
"""

import argparse
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

import paho.mqtt.client as mqtt

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_DIR = os.path.join(HERE, "..", "..", "proto")
BROKER_PORT = 18830
GROUP, NODE, DEVICE = "Plant1", "GW01", "Meter1"
POLL_MS, OFFLINE_AFTER, RETRIES, TIMEOUT_MS = 500, 3, 2, 200  # e2e_config.json
NTP_EPOCH_OFFSET = 2208988800


def load_proto(workdir):
    subprocess.run(["protoc", f"--proto_path={PROTO_DIR}", f"--python_out={workdir}",
                    os.path.join(PROTO_DIR, "sparkplug_b.proto")], check=True)
    sys.path.insert(0, workdir)
    import sparkplug_b_pb2  # noqa: E402 (generated at run time)
    return sparkplug_b_pb2


class SntpServer(threading.Thread):
    """Minimal SNTP server (RFC 4330) answering with the host clock."""

    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 123))
        self.queries = 0

    def run(self):
        while True:
            data, addr = self.sock.recvfrom(512)
            if len(data) < 48:
                continue
            now = time.time() + NTP_EPOCH_OFFSET
            sec, frac = int(now), int((now % 1) * (1 << 32))
            reply = bytearray(48)
            reply[0] = (0 << 6) | (4 << 3) | 4  # LI 0, VN 4, mode 4 (server)
            reply[1] = 1                        # stratum 1
            reply[24:32] = data[40:48]          # originate = client transmit
            struct.pack_into(">II", reply, 16, sec, frac)  # reference
            struct.pack_into(">II", reply, 32, sec, frac)  # receive
            struct.pack_into(">II", reply, 40, sec, frac)  # transmit
            self.sock.sendto(bytes(reply), addr)
            self.queries += 1


class Proc:
    """A child process whose output is captured line by line."""

    def __init__(self, name, argv, logdir, stdin=False):
        self.name = name
        self.lines = []
        self.log = open(os.path.join(logdir, f"{name}.log"), "w")
        self.p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
                                  text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.p.stdout:
            self.lines.append((time.time(), line.rstrip("\n")))
            self.log.write(line)
            self.log.flush()

    def send(self, text):
        self.p.stdin.write(text + "\n")
        self.p.stdin.flush()

    def grep(self, needle, since=0.0):
        return [line for t, line in self.lines if t >= since and needle in line]

    def kill(self, sig=signal.SIGKILL):
        if self.p.poll() is None:
            self.p.send_signal(sig)
            self.p.wait(timeout=10)


class Recorder:
    """Subscribes to spBv1.0/# and decodes every payload with the official proto."""

    def __init__(self, pb):
        self.pb = pb
        self.msgs = []
        self.decode_errors = 0
        self.lock = threading.Lock()
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="e2e-host")
        self.client.on_message = self._on_message
        self.client.on_connect = lambda c, u, f, rc, p: c.subscribe("spBv1.0/#", qos=1)
        self.client.reconnect_delay_set(min_delay=1, max_delay=1)

    def connect(self):
        self.client.connect("127.0.0.1", BROKER_PORT, keepalive=30)
        self.client.loop_start()

    def _on_message(self, client, userdata, msg):
        payload = self.pb.Payload()
        try:
            payload.ParseFromString(msg.payload)
        except Exception:  # noqa: BLE001 - any decode failure is a test failure
            self.decode_errors += 1
            payload = None
        with self.lock:
            self.msgs.append({"t": time.time(), "topic": msg.topic, "raw": msg.payload,
                              "p": payload, "qos": msg.qos, "retain": msg.retain})

    def wait(self, pred, timeout, since=0.0):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                for m in self.msgs:
                    if m["t"] >= since and m["p"] is not None and pred(m):
                        return m
            time.sleep(0.005)
        return None

    def since(self, t0, kind=None):
        with self.lock:
            return [m for m in self.msgs
                    if m["t"] >= t0 and (kind is None or f"/{kind}/" in m["topic"])]

    def publish(self, topic, payload):
        self.client.publish(topic, payload.SerializeToString(), qos=1)


def topic(kind, device=None):
    return f"spBv1.0/{GROUP}/{kind}/{NODE}" + (f"/{device}" if device else "")


def metric(p, name=None, alias=None):
    for m in p.metrics:
        if (name is not None and m.name == name) or (alias is not None and m.alias == alias):
            return m
    return None


def value(m):
    field = m.WhichOneof("value")
    return getattr(m, field) if field else None


class Checks:
    def __init__(self):
        self.results = []

    def check(self, cid, ok, detail):
        self.results.append({"id": cid, "ok": bool(ok), "detail": detail})
        print(f"{'PASS' if ok else 'FAIL'}  {cid}: {detail}", flush=True)
        return ok


def run(args):
    work = tempfile.mkdtemp(prefix="gw-e2e-")
    out = args.out or work
    os.makedirs(out, exist_ok=True)
    pb = load_proto(work)
    c = Checks()
    procs = []

    def start(name, argv, stdin=False):
        p = Proc(name, argv, out, stdin)
        procs.append(p)
        return p

    try:
        sntp = SntpServer()
        sntp.start()
        broker = start("mosquitto", ["mosquitto", "-p", str(BROKER_PORT)])
        start("socat", ["socat", "pty,raw,echo=0,link=/tmp/gw-rs485",
                        "pty,raw,echo=0,link=/tmp/gw-slave"])
        time.sleep(1.0)
        slave_argv = [sys.executable, "-I", os.path.join(HERE, "modbus_slave.py"),
                      "--port", "/tmp/gw-slave", "--wobble"]
        slave = start("slave", slave_argv)
        rec = Recorder(pb)
        rec.connect()
        time.sleep(1.0)

        flash = os.path.join(work, "flash.bin")
        gw_argv = [args.exe, f"-flash={flash}", "-flash_erase"]
        t_boot = time.time()
        gw = start("gateway", gw_argv, stdin=True)

        # --- 1. NBIRTH / DBIRTH ----------------------------------------------
        nb = rec.wait(lambda m: m["topic"] == topic("NBIRTH"), 15)
        c.check("REL-6 boot to NBIRTH", nb and nb["t"] - t_boot < 10,
                f"{(nb['t'] - t_boot):.2f} s" if nb else "no NBIRTH")
        if not nb:
            return c, rec
        p = nb["p"]
        bdseq0 = value(metric(p, "bdSeq"))
        c.check("SPB-4 NBIRTH seq=0, QoS 0, not retained",
                p.seq == 0 and nb["qos"] == 0 and not nb["retain"],
                f"seq={p.seq} qos={nb['qos']} retain={nb['retain']}")
        names = {m.name for m in p.metrics}
        need = {"bdSeq", "Node Control/Rebirth", "Node Control/Reboot", "Node Info/Firmware",
                "Node Info/Hardware"} | {f"DI/{i}" for i in range(1, 9)} | \
               {f"DO/{i}" for i in range(1, 9)}
        complete = all(m.name and m.HasField("alias") and m.HasField("datatype") and
                       m.WhichOneof("value") for m in p.metrics)
        c.check("SPB-4 NBIRTH metrics complete", need <= names and complete,
                f"{len(p.metrics)} metrics, missing={sorted(need - names)}")
        fw = value(metric(p, "Node Info/Firmware"))
        c.check("FW-4 firmware version + git hash", fw and "+" in fw, f"'{fw}'")
        c.check("SPB-3 bdSeq is UInt64 (datatype 8)", metric(p, "bdSeq").datatype == 8,
                f"bdSeq={bdseq0}")
        c.check("TIME-1 NBIRTH timestamp is ms UTC",
                abs(p.timestamp / 1000.0 - time.time()) < 5,
                f"payload ts {p.timestamp}, host {int(time.time() * 1000)}")

        db = rec.wait(lambda m: m["topic"] == topic("DBIRTH", DEVICE), 10)
        ok = db is not None
        if ok:
            d = db["p"]
            v = {m.name: value(m) for m in d.metrics}
            dt = {m.name: m.datatype for m in d.metrics}
            aliases = [m.alias for m in p.metrics] + [m.alias for m in d.metrics]
            c.check("SPB-6 DBIRTH with all metrics and values",
                    set(v) == {"Voltage", "Setpoint", "Run", "Energy", "Alarm"} and
                    all(x is not None for x in v.values()), f"{v}")
            c.check("MB-3/MB-5 FC01-04, float32 ABCD, uint32 CDAB, discrete",
                    abs(v["Voltage"] - 230.0) < 1.0 and v["Energy"] == 0x12345678 and
                    v["Alarm"] is True,
                    f"Voltage={v['Voltage']} Energy={v['Energy']:#x} Alarm={v['Alarm']}")
            c.check("SPEC 6 datatype mapping", dt == {"Voltage": 9, "Setpoint": 9, "Run": 11,
                                                       "Energy": 7, "Alarm": 11}, f"{dt}")
            c.check("SPB-8 aliases unique per node", len(set(aliases)) == len(aliases),
                    f"{len(aliases)} aliases")
            c.check("SPB-5 DBIRTH continues seq", d.seq == 1, f"seq={d.seq}")
        else:
            c.check("SPB-6 DBIRTH with all metrics and values", False, "no DBIRTH")
            return c, rec
        alias = {m.name: m.alias for m in p.metrics}
        alias.update({m.name: m.alias for m in db["p"].metrics})

        # --- 2. Report by exception and deadband (SPB-7, SPB-12) -------------
        t0 = time.time()
        time.sleep(6.5)
        ddata = [m for m in rec.since(t0, "DDATA") if m["topic"] == topic("DDATA", DEVICE)]
        volts = [value(x) for m in ddata for x in m["p"].metrics
                 if x.alias == alias["Voltage"]]
        steps = slave.grep("VOLTAGE", t0)
        by_alias = all(not x.name and x.HasField("alias") and x.timestamp
                       for m in ddata for x in m["p"].metrics)
        c.check("SPB-7/SPB-12 deadband 0.1 suppresses +0.05, reports +0.5",
                len(steps) >= 6 and 0 < len(volts) <= (len(steps) + 1) // 2 + 1,
                f"{len(steps)} slave steps -> {len(volts)} DDATA values {volts}")
        c.check("SPB-7 DATA by alias without name, with timestamp", by_alias and ddata,
                f"{len(ddata)} DDATA messages")

        # --- 3. DI -> NDATA < 100 ms (IO-1) ----------------------------------
        lat = []
        for level in (1, 0, 1):
            t0 = time.time()
            gw.send(f"gw di 3 {level}")
            m = rec.wait(lambda m, lv=level: m["topic"] == topic("NDATA") and any(
                x.alias == alias["DI/3"] and x.boolean_value == bool(lv)
                for x in m["p"].metrics), 2, t0)
            lat.append((m["t"] - t0) * 1000 if m else None)
        c.check("SPEC 7.3a DI change in NDATA < 100 ms (20 ms debounce)",
                all(x is not None and x < 100 for x in lat),
                "latencies ms: " + ", ".join(f"{x:.1f}" if x else "lost" for x in lat))

        # --- 4. NCMD DO/3=true (SPB-11, IO-2) --------------------------------
        cmd = pb.Payload(timestamp=int(time.time() * 1000))
        cmd.metrics.add(name="DO/3", datatype=11, boolean_value=True)
        t0 = time.time()
        rec.publish(topic("NCMD"), cmd)
        m = rec.wait(lambda m: m["topic"] == topic("NDATA") and any(
            x.alias == alias["DO/3"] and x.boolean_value for x in m["p"].metrics), 3, t0)
        c.check("SPEC 7.3b NCMD DO/3=true confirmed by NDATA", m is not None,
                f"{(m['t'] - t0) * 1000:.0f} ms" if m else "no NDATA")

        # --- 5. DCMD Setpoint=12.3 -> FC06 123 (SPB-10) ----------------------
        cmd = pb.Payload(timestamp=int(time.time() * 1000))
        cmd.metrics.add(name="Setpoint", datatype=9, float_value=12.3)
        t0 = time.time()
        rec.publish(topic("DCMD", DEVICE), cmd)
        m = rec.wait(lambda m: m["topic"] == topic("DDATA", DEVICE) and any(
            x.alias == alias["Setpoint"] and abs(x.float_value - 12.3) < 1e-3
            for x in m["p"].metrics), 5, t0)
        time.sleep(0.2)
        wrote = slave.grep("WRITE fc=6 address=10 values=[123]", t0)
        c.check("SPEC 7.4 DCMD Setpoint=12.3 writes 123 with FC06, DDATA confirms",
                m is not None and wrote, f"slave: {wrote} ddata: {m is not None}")

        cmd = pb.Payload(timestamp=int(time.time() * 1000))
        cmd.metrics.add(alias=alias["Run"], datatype=11, boolean_value=True)
        t0 = time.time()
        rec.publish(topic("DCMD", DEVICE), cmd)
        m = rec.wait(lambda m: m["topic"] == topic("DDATA", DEVICE) and any(
            x.alias == alias["Run"] and x.boolean_value for x in m["p"].metrics), 5, t0)
        time.sleep(0.2)
        wrote = slave.grep("WRITE fc=5 address=0 values=[True]", t0)
        c.check("MB-4 DCMD by alias Run=true writes FC05, DDATA confirms",
                m is not None and wrote, f"slave: {wrote} ddata: {m is not None}")

        cmd = pb.Payload(timestamp=int(time.time() * 1000))
        cmd.metrics.add(name="Energy", datatype=7, int_value=70000)
        t0 = time.time()
        rec.publish(topic("DCMD", DEVICE), cmd)
        m = rec.wait(lambda m: m["topic"] == topic("DDATA", DEVICE) and any(
            x.alias == alias["Energy"] and x.int_value == 70000 for x in m["p"].metrics), 5, t0)
        time.sleep(0.2)
        wrote = slave.grep("WRITE fc=16 address=20 values=[4464, 1]", t0)
        c.check("MB-4 DCMD uint32 (CDAB) writes FC16, DDATA confirms",
                m is not None and wrote, f"slave: {wrote} ddata: {m is not None}")

        cmd = pb.Payload(timestamp=int(time.time() * 1000))
        cmd.metrics.add(name="Voltage", datatype=9, float_value=1.0)
        cmd.metrics.add(name="NoSuchTag", datatype=9, float_value=1.0)
        t0 = time.time()
        rec.publish(topic("DCMD", DEVICE), cmd)
        time.sleep(1.5)
        c.check("SPB-10 read-only and unknown metrics discarded and logged",
                not slave.grep("WRITE", t0) and gw.grep("Voltage: read-only", t0) and
                gw.grep("NoSuchTag: unknown metric", t0),
                f"log lines: {gw.grep('discarded', t0)}")

        # --- 6. Rebirth (SPB-9) ----------------------------------------------
        cmd = pb.Payload(timestamp=int(time.time() * 1000))
        cmd.metrics.add(name="Node Control/Rebirth", datatype=11, boolean_value=True)
        t0 = time.time()
        rec.publish(topic("NCMD"), cmd)
        nb2 = rec.wait(lambda m: m["topic"] == topic("NBIRTH"), 5, t0)
        db2 = rec.wait(lambda m: m["topic"] == topic("DBIRTH", DEVICE), 5, t0)
        c.check("SPEC 7.5 Rebirth: NBIRTH seq=0 (same bdSeq) + all DBIRTH",
                nb2 and db2 and nb2["p"].seq == 0 and db2["p"].seq == 1 and
                value(metric(nb2["p"], "bdSeq")) == bdseq0,
                f"NBIRTH seq={nb2['p'].seq if nb2 else None} "
                f"DBIRTH seq={db2['p'].seq if db2 else None}")
        do3 = metric(nb2["p"], "DO/3") if nb2 else None
        c.check("IO-2 DO state reported in rebirth", do3 is not None and do3.boolean_value,
                f"DO/3={value(do3) if do3 else None}")

        # --- 7. Device offline/online (MB-9, SPB-6) --------------------------
        t0 = time.time()
        slave.kill(signal.SIGTERM)
        dd = rec.wait(lambda m: m["topic"] == topic("DDEATH", DEVICE), 15, t0)
        bound = OFFLINE_AFTER * (POLL_MS + (RETRIES + 1) * TIMEOUT_MS) / 1000.0 + 0.5
        c.check("SPEC 7.2 slave off -> DDEATH within offline_after x poll + timeouts",
                dd is not None and dd["t"] - t0 <= bound,
                f"{dd['t'] - t0:.2f} s (bound {bound:.1f} s)" if dd else "no DDEATH")
        t0 = time.time()
        slave = start("slave2", slave_argv)
        db3 = rec.wait(lambda m: m["topic"] == topic("DBIRTH", DEVICE), 10, t0)
        c.check("SPEC 7.2 slave back -> DBIRTH", db3 is not None,
                f"{db3['t'] - t0:.2f} s" if db3 else "no DBIRTH")

        # --- 8. seq continuity over the whole first session (SPB-5) ----------
        session = [m for m in rec.since(nb["t"])
                   if m["topic"].split("/")[2] not in ("NDEATH", "NCMD", "DCMD")]
        gaps = [(a["topic"], a["p"].seq, b["topic"], b["p"].seq)
                for a, b in zip(session, session[1:])
                if b["p"].seq != (0 if "/NBIRTH/" in b["topic"] else (a["p"].seq + 1) % 256)]
        c.check("SPB-5 seq +1 (mod 256) across node and device messages, 0 at NBIRTH",
                not gaps and len(session) > 10, f"{len(session)} messages, gaps={gaps}")

        # --- 9. Broker restart: reconnect with a new bdSeq (MQTT-2, SPB-3) ---
        broker.kill(signal.SIGTERM)
        time.sleep(3.0)
        t0 = time.time()
        broker = start("mosquitto2", ["mosquitto", "-p", str(BROKER_PORT)])
        nb3 = rec.wait(lambda m: m["topic"] == topic("NBIRTH"), 70, t0)
        bdseq1 = value(metric(nb3["p"], "bdSeq")) if nb3 else None
        c.check("MQTT-2 reconnects after broker restart (backoff <= 60 s)",
                nb3 is not None and bdseq1 != bdseq0 and nb3["p"].seq == 0,
                f"{nb3['t'] - t0:.1f} s, bdSeq {bdseq0} -> {bdseq1}" if nb3 else "no NBIRTH")
        time.sleep(1.0)

        # --- 10. TCP killed: LWT NDEATH with the NBIRTH bdSeq (SPEC 7.1) ----
        t0 = time.time()
        gw.kill(signal.SIGKILL)
        nd = rec.wait(lambda m: m["topic"] == topic("NDEATH"), 30, t0)
        nd_bdseq = value(metric(nd["p"], "bdSeq")) if nd else None
        c.check("SPEC 7.1 / SPB-2 kill -> broker publishes NDEATH with NBIRTH bdSeq",
                nd is not None and nd_bdseq == bdseq1 and not nd["p"].HasField("seq") and
                nd["qos"] == 1,
                f"NDEATH bdSeq={nd_bdseq} NBIRTH bdSeq={bdseq1} qos={nd['qos'] if nd else None}")

        # --- 11. bdSeq persisted across reboot (SPB-3) -----------------------
        t0 = time.time()
        gw = start("gateway2", [args.exe, f"-flash={flash}"], stdin=True)
        nb4 = rec.wait(lambda m: m["topic"] == topic("NBIRTH"), 15, t0)
        bdseq2 = value(metric(nb4["p"], "bdSeq")) if nb4 else None
        time.sleep(2.0)  # let the second boot synchronise its clock
        c.check("SPB-3 bdSeq persisted in flash and incremented after reboot",
                bdseq2 is not None and bdseq2 == (bdseq1 + 1) % 256,
                f"before reboot {bdseq1}, after {bdseq2}")

        # --- 12. NCMD Reboot: orderly NDEATH (SPB-2, SPB-11) -----------------
        cmd = pb.Payload(timestamp=int(time.time() * 1000))
        cmd.metrics.add(name="Node Control/Reboot", datatype=11, boolean_value=True)
        t0 = time.time()
        rec.publish(topic("NCMD"), cmd)
        nd2 = rec.wait(lambda m: m["topic"] == topic("NDEATH"), 10, t0)
        c.check("SPB-11 NCMD Reboot publishes NDEATH before disconnecting",
                nd2 is not None and value(metric(nd2["p"], "bdSeq")) == bdseq2,
                f"bdSeq={value(metric(nd2['p'], 'bdSeq')) if nd2 else None}")

        synced = [p for p in procs if p.name.startswith("gateway") and
                  p.grep("clock synchronised")]
        c.check("TIME-1 SNTP synchronised", len(synced) == 2,
                f"{sntp.queries} SNTP queries served, {len(synced)}/2 boots synchronised")
        c.check("SPB-1 every payload decodes with the official proto",
                rec.decode_errors == 0 and len(rec.msgs) > 0,
                f"{len(rec.msgs)} messages, {rec.decode_errors} decode errors")

        save_samples(rec, out)
        return c, rec
    finally:
        for p in reversed(procs):
            try:
                p.kill(signal.SIGTERM)
            except Exception:  # noqa: BLE001 - best effort cleanup
                p.kill()
        shutil.rmtree(work, ignore_errors=True)


def save_samples(rec, out):
    """Keep one raw payload per message type for offline inspection."""
    seen = {}
    for m in rec.msgs:
        kind = m["topic"].split("/")[2]
        seen.setdefault(kind, m)
    for kind, m in seen.items():
        with open(os.path.join(out, f"sample_{kind}.bin"), "wb") as f:
            f.write(m["raw"])


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, help="native_sim zephyr.exe")
    parser.add_argument("--out", help="directory for logs, samples and results.json")
    args = parser.parse_args()

    checks, _ = run(args)
    passed = sum(r["ok"] for r in checks.results)
    print(f"\n{passed}/{len(checks.results)} checks passed")
    if args.out:
        with open(os.path.join(args.out, "results.json"), "w") as f:
            json.dump(checks.results, f, indent=2)
    return 0 if passed == len(checks.results) and checks.results else 1


if __name__ == "__main__":
    sys.exit(main())
