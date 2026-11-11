#!/usr/bin/env python3
"""Issue #176: the Edge Node profile of the Eclipse Sparkplug TCK (v3.0.0) against the real
firmware in QEMU. The E2E harness runs it as the optional scenario 'tck' (e2e.py --only tck);
it is not part of a full run and not in the CI groups (scenarios.py).

What the TCK is: a HiveMQ extension. It watches every MQTT packet of the broker, plays the
Sparkplug host application (STATE, NCMD) and writes one result for each tck-id. A client starts
a test with "NEW_TEST edge <Test> <parameters>" on SPARKPLUG_TCK/TEST_CONTROL and ends it with
"END_TEST"; the extension publishes the summary on SPARKPLUG_TCK/RESULT ("<tck-id>: PASS;" lines,
"Monitor:<tck-id>: ..." for the checks of its message monitor). The web console of the TCK is
only a front end for these topics: this module replaces it and does what the operator would do
(boot the gateway, change a register, set a DI, stop the Modbus slave, restart the gateway).

Environment (see docs/how-to/run-the-sparkplug-tck.md; tools/e2e/tck_setup.sh makes the dir):
  E2E_TCK_DIR        directory with jdk/ (Java 11 or later) and hivemq/ (HiveMQ CE with the TCK
                     extension in hivemq/extensions/sparkplug-tck). Not set or incomplete: the
                     scenario prints [SKIP] and runs nothing.
  E2E_TCK_TESTS      comma-separated Edge Node tests (default: all seven, TESTS below)
  E2E_TCK_PROTOCOLS  comma-separated MQTT versions of the gateway (default "3.1.1,5")

The TCK's own clients (results collector, simulated host) connect to tcp://localhost:1883, a
fixed address in the extension. HiveMQ therefore listens on 127.0.0.1:1883; the harness' private
network namespace keeps that port free. MultipleBrokerTest needs a second broker: a mosquitto on
127.0.0.1:1884 (the gateway's backup broker).

Each test gets a new HiveMQ (new data folder) and a new boot of the gateway. Reason: the message
monitor of the TCK keeps the last bdSeq of each edge node for the life of the broker, and a
gateway starts at bdSeq 0 at each boot (tck-id-topics-nbirth-bdseq-increment: "MUST start at
zero"); a second boot on the same broker would be a false failure.
"""
import json
import os
import pathlib
import queue
import shutil
import subprocess
import threading
import time

import paho.mqtt.client as mqtt

TCK_PORT = 1883          # fixed in the TCK extension (Results, HostApplication)
BROKER2_PORT = 1884      # MultipleBrokerTest: the second MQTT server
HOST = "TCKHost"         # the Sparkplug host application id (the TCK simulates it)
GROUP, NODE = "Plant1", "TCK1"
DEVICE, TYPED = "Meter1", "Pump1"
TESTS = ("SessionEstablishmentTest", "SessionTerminationTest", "SendDataTest",
         "SendComplexDataTest", "ReceiveCommandTest", "PrimaryHostTest", "MultipleBrokerTest")
PROTOCOLS = ("3.1.1", "5")
CONTROL, RESULT, LOG, PROMPT = ("SPARKPLUG_TCK/TEST_CONTROL", "SPARKPLUG_TCK/RESULT",
                                "SPARKPLUG_TCK/LOG", "SPARKPLUG_TCK/CONSOLE_PROMPT")

# The value field of each Sparkplug datatype. SPEC is the datatype table of the specification
# (chapter 6, "Google Protocol Buffer Type"): UInt32 is a uint32, thus int_value. TCK is
# Utils.hasValue() of the TCK 3.0.0: it requires long_value for UInt32, as the Java library
# (Tahu) did before eclipse-tahu/tahu#202 (eclipse-sparkplug/sparkplug#102).
_ARRAYS = {n: "bytes_value" for n in range(22, 35)}
SPEC_FIELD = {1: "int_value", 2: "int_value", 3: "int_value", 4: "long_value", 5: "int_value",
              6: "int_value", 7: "int_value", 8: "long_value", 9: "float_value",
              10: "double_value", 11: "boolean_value", 12: "string_value", 13: "long_value",
              14: "string_value", 15: "string_value", 16: "dataset_value", 17: "bytes_value",
              18: "bytes_value", 19: "template_value", **_ARRAYS}
TCK_FIELD = {**SPEC_FIELD, 7: "long_value"}


def has_value(m, fields):
    """True if metric @m has a name, a datatype and the value field of @fields (or is_null)."""
    if not m.HasField("name") or not m.HasField("datatype"):
        return False
    if m.HasField("is_null") and m.is_null:
        return True
    f = fields.get(m.datatype)
    return f is not None and m.HasField(f)


def uint32_cause(payloads):
    """The reason text when the only metrics of @payloads that the TCK takes as "no value" are
    UInt32 metrics with int_value (correct by the specification), else None."""
    metrics = [m for p in payloads for m in p.metrics]
    tck_bad = [m for m in metrics if not has_value(m, TCK_FIELD)]
    spec_bad = [m for m in metrics if not has_value(m, SPEC_FIELD)]
    if not payloads or not tck_bad or spec_bad:
        return None
    if not all(m.datatype == 7 and m.HasField("int_value") for m in tck_bad):
        return None
    names = sorted({m.name for m in tck_bad})
    return (f"TCK bug: Utils.hasValue() wants long_value for UInt32; the specification gives "
            f"uint32 (int_value), sparkplug#102, tahu#202 ({', '.join(names)})")


# FAILs of the TCK that are not gateway bugs: tck-id -> (message kind, rule). The rule gets the
# messages of that kind that the harness captured during the test and returns the reason, or
# None when the reason does not apply (then the FAIL counts). README 4.7 explains each entry.
KNOWN_RULES = {
    "topics-nbirth-metrics": ("NBIRTH", uint32_cause),
    "operational-behavior-data-publish-nbirth-values": ("NBIRTH", uint32_cause),
    "topics-dbirth-metrics": ("DBIRTH", uint32_cause),
    "operational-behavior-data-publish-dbirth-values": ("DBIRTH", uint32_cause),
}


def known_reason(ident, captured):
    """The reason why the FAIL of tck-id @ident is not a gateway bug, or None. @captured: the
    (topic, payload) pairs of the edge node under test during the test."""
    rule = KNOWN_RULES.get(ident)
    if rule is None:
        return None
    kind, fn = rule
    return fn([p for topic, p in captured if topic.split("/")[2] == kind])


def configured():
    """(jdk dir, hivemq dir) from E2E_TCK_DIR, or (None, reason) when the TCK is not there."""
    d = os.environ.get("E2E_TCK_DIR", "")
    if not d:
        return None, "E2E_TCK_DIR is not set"
    root = pathlib.Path(d)
    java = root / "jdk" / "bin" / "java"
    jar = root / "hivemq" / "bin" / "hivemq.jar"
    ext = root / "hivemq" / "extensions" / "sparkplug-tck" / "hivemq-extension.xml"
    for p in (java, jar, ext):
        if not p.exists():
            return None, f"{p} is missing (run tools/e2e/tck_setup.sh {root})"
    return root, None


def parse_result(text):
    """(test name, {key: status text}, overall) of one SPARKPLUG_TCK/RESULT payload. A key is a
    tck-id without its 'tck-id-' prefix, or 'Monitor:<id>' for the checks of the monitor."""
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    name, results, overall = "", {}, ""
    if lines and "Summary Test Results for " in lines[0]:
        name = lines[0].split("Summary Test Results for ", 1)[1].strip()
        lines = lines[1:]
    for ln in lines:
        if ln.endswith(";"):
            ln = ln[:-1]
        key, sep, value = ln.partition(": ")
        if not sep:
            continue
        if key == "OVERALL":
            overall = value
        else:
            results[key] = value
    return name, results, overall


def status_of(value):
    """PASS, FAIL, MAYBE or NOT EXECUTED (the first word(s) of a result value)."""
    for s in ("NOT EXECUTED", "NOT YET IMPLEMENTED", "PASS", "FAIL", "MAYBE", "EMPTY"):
        if value.startswith(s):
            return s
    return value


class HiveMQ:
    """One HiveMQ CE process with the TCK extension, a new data folder and its own log."""

    def __init__(self, root, work, name):
        self.root, self.dir = root, work / f"tck-{name}"
        shutil.rmtree(self.dir, ignore_errors=True)
        (self.dir / "conf").mkdir(parents=True)
        (self.dir / "conf" / "config.xml").write_text(
            "<?xml version=\"1.0\"?>\n<hivemq>\n  <listeners>\n    <tcp-listener><port>"
            f"{TCK_PORT}</port><bind-address>127.0.0.1</bind-address></tcp-listener>\n"
            "  </listeners>\n  <anonymous-usage-statistics><enabled>false</enabled>"
            "</anonymous-usage-statistics>\n</hivemq>\n")
        shutil.copy(root / "hivemq" / "conf" / "logback.xml", self.dir / "conf" / "logback.xml")
        self.log = self.dir / "hivemq.out"
        self.proc = None

    def start(self, procs):
        home = self.root / "hivemq"
        cmd = [str(self.root / "jdk" / "bin" / "java"), f"-Dhivemq.home={home}",
               f"-Dhivemq.config.folder={self.dir / 'conf'}",
               f"-Dhivemq.data.folder={self.dir / 'data'}",
               f"-Dhivemq.log.folder={self.dir / 'log'}",
               f"-Dhivemq.extensions.folder={home / 'extensions'}",
               "-Djava.net.preferIPv4Stack=true", "-Duser.language=en", "-Duser.region=US",
               "-jar", str(home / "bin" / "hivemq.jar")]
        self.proc = subprocess.Popen(cmd, cwd=self.dir, stdin=subprocess.DEVNULL,
                                     stdout=open(self.log, "wb"), stderr=subprocess.STDOUT)
        procs.append(self.proc)
        end = time.monotonic() + 60
        while time.monotonic() < end:
            text = self.log.read_text(errors="replace") if self.log.exists() else ""
            if "Started HiveMQ in" in text:
                if "Sparkplug™ TCK\" version 3.0.0 started successfully" not in text:
                    raise RuntimeError(f"HiveMQ started without the TCK extension ({self.log})")
                time.sleep(1.5)   # the extension's results collector connects 1 s after the start
                return
            if self.proc.poll() is not None:
                break
            time.sleep(0.2)
        raise RuntimeError(f"HiveMQ did not start in 60 s ({self.log})")

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


class Console:
    """The TCK console's MQTT side: test control, log, prompts and results."""

    def __init__(self):
        self.results, self.lines = queue.Queue(), []
        self.closed = False
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"tck-console-{time.monotonic()}")
        self.c.on_message = self._on_msg
        sub = threading.Event()
        self.c.on_subscribe = lambda *a: sub.set()
        self.c.connect("127.0.0.1", TCK_PORT, keepalive=60)
        self.c.subscribe([(RESULT, 1), (LOG, 1), (PROMPT, 1)])
        self.c.loop_start()
        if not sub.wait(5):
            raise RuntimeError("TCK console: no SUBACK from HiveMQ")

    def _on_msg(self, client, userdata, msg):
        text = msg.payload.decode(errors="replace")
        self.lines.append((time.monotonic(), msg.topic, text))
        if msg.topic == RESULT:
            self.results.put(text)

    def new_test(self, test, params):
        self.c.publish(CONTROL, f"NEW_TEST edge {test} {' '.join(params)}", qos=1).wait_for_publish(5)

    def end_test(self):
        self.c.publish(CONTROL, "END_TEST", qos=1).wait_for_publish(5)

    def result(self, timeout, wait_left):
        end = time.monotonic() + timeout
        while wait_left(end) > 0:
            try:
                return self.results.get(timeout=min(0.5, max(0.01, wait_left(end))))
            except queue.Empty:
                continue
        return None

    def close(self):
        """Disconnect, and wait at most 5 s for the network thread (a daemon): with a HiveMQ that
        does not answer, paho's loop_stop() can wait for ever."""
        if not self.closed:
            self.closed = True
            self.c.disconnect()
            t = threading.Thread(target=self.c.loop_stop, daemon=True)
            t.start()
            t.join(5)


def gateway_config(h, protocol, backup=False):
    """The configuration of the gateway under test: Meter1 (unit 1, plain tags with aliases) and
    Pump1 (unit 2, a template instance, SPB-18), the TCK's host as primary host (SPB-13), no
    periodic diagnostics (an NDATA during a rebirth would fail the receive command test)."""
    cfg = json.loads(h.config_json())
    cfg["mqtt"].update(host="10.0.2.2", port=TCK_PORT, keepalive_s=10)
    if protocol == "5":
        cfg["mqtt"]["protocol"] = "5"
    if backup:
        cfg["mqtt"]["backup_brokers"] = [{"host": "10.0.2.2", "port": BROKER2_PORT}]
    cfg["sparkplug"].update(group_id=GROUP, edge_node_id=NODE, primary_host_id=HOST,
                            diagnostics_s=0)
    meter = next(d for d in cfg["devices"] if d["name"] == DEVICE)
    pump = {"name": TYPED, "unit_id": 2, "type": "Pump", "poll_ms": 1000, "offline_after": 3,
            "tags": [{"name": "Speed", "area": "holding", "address": 100, "type": "uint16",
                      "units": "rpm", "eng_high": 3000},
                     {"name": "Run", "area": "coil", "address": 7, "type": "bool"}]}
    cfg["devices"] = [meter, pump]
    cfg["modbus_tcp"]["enabled"] = False
    return cfg


def run(h, args, work, slave):
    """The 'tck' scenario. @h is the e2e module (its Observer, boot_variant, check, ...)."""
    root, why = configured()
    if root is None:
        print(f"[SKIP] tck: {why}; see docs/how-to/run-the-sparkplug-tck.md", flush=True)
        h.SKIPPED.append("tck")
        return
    tests = [t.strip() for t in os.environ.get("E2E_TCK_TESTS", ",".join(TESTS)).split(",") if t.strip()]
    protocols = [p.strip() for p in os.environ.get("E2E_TCK_PROTOCOLS", ",".join(PROTOCOLS)).split(",")
                 if p.strip()]
    unknown = [t for t in tests if t not in TESTS] + [p for p in protocols if p not in PROTOCOLS]
    if unknown:
        raise ValueError(f"E2E_TCK_TESTS/E2E_TCK_PROTOCOLS: unknown {unknown}")
    table = []
    for protocol in protocols:
        for test in tests:
            name = f"{test}-mqtt{protocol.replace('.', '')}"
            print(f"[TCK] {test} (MQTT {protocol}): start", flush=True)
            rows, notes = run_one(h, args, work, slave, root, test, protocol, name)
            if not rows:
                # The TCK gave no result: the extension hung (seen once: its simulated host
                # connects to HiveMQ from inside the publish interceptor and did not return).
                # A fault of the TCK, never a result of an assertion: one more attempt, with a
                # new HiveMQ and a new boot. A FAIL is never repeated.
                print(f"[TCK] {test} (MQTT {protocol}): no result ({'; '.join(notes)}): "
                      "one more attempt", flush=True)
                h.RETRIES.append({"scenario": f"tck {name}", "qemu": "no TCK result",
                                  "fault": None, "error": "; ".join(notes),
                                  "discarded_checks": []})
                rows, again = run_one(h, args, work, slave, root, test, protocol, f"{name}-2")
                notes = [f"attempt 1 gave no result ({'; '.join(notes)})"] + again
            table += rows
            fails = [r for r in rows if r["status"] == "FAIL" and not r["known"]]
            known = [r for r in rows if r["status"] == "FAIL" and r["known"]]
            counts = {s: sum(1 for r in rows if r["status"] == s) for s in ("PASS", "FAIL", "NOT EXECUTED")}
            h.check(f"Issue #176 TCK edge {test} (MQTT {protocol}): a result came and no assertion "
                    f"failed that is not explained (tools/e2e/tck.py KNOWN_RULES)",
                    rows and not fails,
                    f"{counts['PASS']} PASS, {counts['FAIL']} FAIL ({len(known)} known), "
                    f"{counts['NOT EXECUTED']} NOT EXECUTED"
                    + (f"; failed: {', '.join(r['id'] for r in fails)}" if fails else "")
                    + (f"; {'; '.join(notes)}" if notes else ""))
    (work / "tck-results.json").write_text(json.dumps(table, indent=2))
    print(f"[TCK] {len(table)} assertion results in {work / 'tck-results.json'}", flush=True)


def run_one(h, args, work, slave, root, test, protocol, name):
    """One TCK test against one boot of the gateway: ([row], [note])."""
    first = len(h.CAPTURE)
    hmq = HiveMQ(root, work, name)
    hmq.start(h.PROCS)
    h.track(hmq, hmq.stop)
    console = Console()
    h.track(console, console.close)
    obs = h.Observer(TCK_PORT)
    b2 = obs2 = None
    notes, gw = [], None
    params = [HOST, GROUP, NODE, DEVICE]
    dbirth_t = f"spBv1.0/{GROUP}/DBIRTH/{NODE}/{DEVICE}"
    try:
        if test == "MultipleBrokerTest":
            conf = work / f"broker-{name}.conf"
            conf.write_text(f"listener {BROKER2_PORT} 127.0.0.1\nallow_anonymous true\n")
            b2 = h.start_broker(conf, work / f"mosquitto-{name}.log")
            obs2 = h.Observer(BROKER2_PORT)
            params = params + [f"tcp://localhost:{BROKER2_PORT}"]
        if test == "SessionEstablishmentTest":
            params = [HOST, GROUP, NODE, DEVICE, TYPED]
        cfg = gateway_config(h, protocol, backup=test == "MultipleBrokerTest")

        if test == "PrimaryHostTest":
            # the edge node first: connected, subscribed, waiting for its primary host
            t0 = time.monotonic()
            gw = h.boot_variant(args, work, name, cfg)
            if not gw.wait_log(rf"waiting for the primary host '{HOST}'", 90, t0):
                notes.append("the gateway did not reach 'waiting for the primary host'")
            console.new_test(test, params)
        else:
            # the test first: it takes the client id of the edge node from its CONNECT
            console.new_test(test, params)
            time.sleep(1.0)
            gw = h.boot_variant(args, work, name, cfg)

        timeout = 120
        if test in ("SendDataTest", "SendComplexDataTest"):
            nb = obs.wait(dbirth_t, 90)
            if not nb:
                notes.append("no DBIRTH of Meter1")
            time.sleep(1.0)
            gw.console("di 1 1")                 # NDATA: DI/1
            slave.set_float(4, 0, 231.5)         # DDATA: Voltage (deadband 0.1)
            time.sleep(3.0)
            gw.console("di 1 0")
            slave.set_float(4, 0, 230.5)
            if test == "SendComplexDataTest":
                # it ends only when every assertion has a result; the gateway has no template
                # parameters (README 6.3.1), so the console ends it, as an operator would
                time.sleep(5.0)
                console.end_test()
        elif test == "SessionTerminationTest":
            if not obs.wait(dbirth_t, 90):
                notes.append("no DBIRTH of Meter1")
            time.sleep(1.0)
            slave.stop()                         # the devices stop answering: DDEATH
            if not obs.wait(f"spBv1.0/{GROUP}/DDEATH/{NODE}/{DEVICE}", 30):
                notes.append("no DDEATH of Meter1")
            gw.console("restart")                # intentional disconnect: NDEATH, DISCONNECT
            timeout = 60
        elif test == "ReceiveCommandTest":
            timeout = 150
        elif test == "PrimaryHostTest":
            timeout = 90
        elif test == "MultipleBrokerTest":
            timeout = 150

        text = console.result(timeout, h.wait_left)
        if text is None:
            notes.append(f"no result in {timeout} s: END_TEST sent")
            console.end_test()
            text = console.result(20, h.wait_left)
    finally:
        if test == "SessionTerminationTest":
            try:
                slave.start()
            except Exception as e:   # noqa: BLE001 (already running)
                notes.append(f"slave restart: {e}")
        if gw is not None:
            gw.kill()
        # HiveMQ first: a QoS 1 message that a hung HiveMQ never acknowledged keeps paho's
        # loop_stop() waiting until the socket closes
        hmq.stop()
        if b2 is not None:
            b2.terminate()
            b2.wait()
        for o in (obs, obs2):
            if o is not None:
                o.close()
        console.close()
    (work / f"tck-{name}.result.txt").write_text(text or "")
    (work / f"tck-{name}.console.txt").write_text(
        "".join(f"{t:.2f} {topic} {payload}\n" for t, topic, payload in console.lines))
    rows = []
    captured = [(topic, p) for _, topic, p, _ in h.CAPTURE[first:]
                if topic.startswith(f"spBv1.0/{GROUP}/") and topic.split("/")[3] == NODE]
    if text:
        tname, results, overall = parse_result(text)
        print(f"[TCK] {test} (MQTT {protocol}): {tname}: OVERALL {overall}", flush=True)
        for key, value in sorted(results.items()):
            st = status_of(value)
            ident = key.split(":", 1)[1] if key.startswith("Monitor:") else key
            known = known_reason(ident, captured) if st == "FAIL" else None
            rows.append({"protocol": protocol, "test": test, "id": key, "status": st,
                         "known": known, "detail": value})
            print(f"[TCK] {protocol:5} {test}: {key}: {st}"
                  + (f" (known: {known})" if st == "FAIL" and known else "")
                  + (f" -- {value}" if st == "FAIL" else ""), flush=True)
    return rows, notes
