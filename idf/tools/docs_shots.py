#!/usr/bin/env python3
"""Screenshots of the web UI for docs/images, made with Playwright against the real firmware in
QEMU (the QEMU build of the E2E, issue #187 and the docs). Run it through
tools/docs/make_shots.sh, which builds the images and prepares the flash; see
docs/how-to/make-the-docs-screenshots.md.

The shots are declarative (SHOTS below): each one names the gateway state that it needs (STATES),
a viewport (PHONE for the pages of the commissioning access point, DESKTOP for the others), the
page, optional actions, and what to capture. Shots of the same state that follow each other share
one boot of the emulator, in the order of SHOTS; an action of a shot (a claim, a save) changes the
state for the shots after it.

Determinism: the numbers of the status that change at each boot or each second (uptime, heap,
counters, the clock) are replaced in the JSON answers of the API before the page gets them
(FREEZE), setInterval is disabled (the page polls once), animations are off and the colour scheme
is light. What the freeze does not touch (for example a tag value that the page computes) is the
same at each run because the Modbus slave serves fixed values. The images are written only when
their pixels change.

The harness machinery (QEMU, mosquitto, the pymodbus slave, SNTP, flash variants) comes from
tools/e2e/e2e.py, imported as a module: this script changes nothing in it.
"""
import argparse
import copy
import io
import json
import os
import pathlib
import re
import shutil
import sys
import tempfile
import threading
import time
import types
from dataclasses import dataclass, field

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "e2e"))
import e2e  # noqa: E402  (the E2E harness: Gateway, flash_variant, brokers, the Modbus slave)

DOCS_PASSWORD = "docs-pass-1"   # the web password of the configured states (it is in no image)

# Viewports: a phone for the pages that the operator opens on the access point, a desktop for the
# others. Playwright's own device list changes between releases: the values are fixed here.
PHONE = dict(viewport={"width": 390, "height": 844}, device_scale_factor=2, is_mobile=True,
             has_touch=True)
DESKTOP = dict(viewport={"width": 1280, "height": 800}, device_scale_factor=1)


# ---------------------------------------------------------------- the gateway states

def docs_config(web_password=None):
    """The E2E configuration (tools/e2e/fs/config.json) with only the device that answers
    (Meter1, unit 1 of the pymodbus slave), and the web password @web_password (None: no web
    password, the claim window can open)."""
    cfg = json.loads(e2e.config_json())
    cfg["devices"] = [d for d in cfg["devices"] if d["name"] == e2e.DEVICE]
    del cfg["syslog"]   # its counters change at each run
    if web_password:
        cfg["web"] = {"username": "admin", "password": web_password}
    return cfg


def wait_ready(h, name, gw, auth, timeout=180):
    """Wait until the shell answers and /api/status shows MQTT connected, a synchronized time and
    each device online (the status page then shows a steady state). The last status goes to
    status-<name>.json in the work directory (for the choice of the FREEZE paths)."""
    gw.shell("status", [r"^config\s"], timeout=10, tries=int(timeout / 10))
    end = time.monotonic() + timeout
    st, code, ready = {}, None, False
    while not ready and time.monotonic() < end:
        code, _, body = e2e.http("GET", "/api/status", auth=auth, timeout=10)
        st = (e2e.as_json(body) or {}) if code == 200 else {}
        devs = st.get("devices", [])
        ready = bool(st.get("mqtt", {}).get("connected") and st.get("time", {}).get("synced")
                     and devs and all(d.get("online") for d in devs))
        if not ready:
            time.sleep(1)
    (h.work / f"status-{name}.json").write_text(json.dumps(st, indent=2))
    if not ready:
        raise SystemExit(f"{name}: no steady state in {timeout} s (/api/status {code}); see "
                         f"uart0-{name}.log and status-{name}.json in {h.work}")


def state_configured(h):
    """A configured gateway in normal mode, with the web password DOCS_PASSWORD."""
    gw = h.boot("configured", docs_config(DOCS_PASSWORD))
    auth = ("admin", DOCS_PASSWORD)
    wait_ready(h, "configured", gw, auth)
    return gw, auth


def state_claim(h):
    """A configured gateway without a web password, with the claim window open (issue #120).
    QEMU has no radio: 'claimsim 600 eth' makes the Ethernet count as the access point."""
    gw = h.boot("claim", docs_config(None))
    # without a web password, the API takes the commissioning password of the console
    wait_ready(h, "claim", gw, ("admin", e2e.commissioning_password(gw, 0, 60) or "?"))
    gw.shell("claimsim 600 eth", [r"claim windows: 600 s; Ethernet counts as the access point"])
    t = time.monotonic()
    e2e.boot_press(gw)
    if not gw.wait_log(r"web: claim window open for 600 s", 20, t):
        raise SystemExit("the claim window did not open")
    return gw, None


# name -> function(harness) that boots the gateway and returns (Gateway, Basic auth or None)
STATES = {"configured": state_configured, "claim": state_claim}


def ap_on_with_client(gw):
    """Issue #187: the access point of a configured gateway after a push of BOOT (WPA2 with the
    web password, the maximum time of normal mode), with one client: the phone of the operator.
    QEMU has no radio: the emulator-only 'apsim' gives the clients of the simulated model."""
    gw.shell("apsim clients 1", [r"access point: 1 simulated client"])
    e2e.boot_press(gw)
    # the shell line, not the log line of the start: a push while it runs only starts the times
    # again (no start line)
    _, hits = gw.shell("ap", [r"^ap\s+'%s' on .*WPA2 with the web password, 1 client" %
                              e2e.AP_SSID_RX], timeout=5, tries=4)
    if None in hits.values():
        raise SystemExit("the access point did not start with the web password")


def close_ap(page):
    """Click 'Close the access point', accept the confirmation, and open the page again when the
    access point is off (the page polls only once: NO_TIMERS)."""
    page.once("dialog", lambda d: d.accept())
    page.click("#ap-stop")
    page.locator("#ap-msg").filter(has_text="stops in a few seconds").wait_for(state="visible")
    end = time.monotonic() + 20
    while True:
        page.wait_for_timeout(1000)
        page.reload(wait_until="networkidle")
        if page.locator("#st-ap").filter(has_text="Off").count() or time.monotonic() > end:
            break

def test_read(page):
    """The Modbus test read of the Configuration tab: the float32 Voltage of Meter1 (input 0)."""
    page.click("nav button[data-tab=config]")
    page.fill("#mb-unit", "1")
    page.select_option("#mb-area", "input")
    page.fill("#mb-addr", "0")
    page.select_option("#mb-type", "float32")
    page.select_option("#mb-wo", "ABCD")
    page.click("#mb-read")


def tag_explainer(page):
    """Issues #189 and #190: the details row of the first tag of Meter1 (Voltage), and the
    explainer of its Eng. high field (a click keeps it open)."""
    page.click("nav button[data-tab=config]")
    page.evaluate("document.querySelector('#f-dev>.card').scrollIntoView()")
    page.click('[data-more="0.0"]')
    page.click("#det-0-0 button.qi[data-x=enghi]")

# ---------------------------------------------------------------- the frozen values

def no_hash(version):
    """The firmware version without its git hash ("0.1.0+g05af957f" -> "0.1.0"): the hash
    changes at each commit."""
    return re.sub(r"\+g[0-9a-f]+", "", version) if isinstance(version, str) else version


# Path in the JSON answer -> fixed value, or a function(real value) -> value; "*" is each element
# of a list. A path that is not in the answer is ignored. Keep only values that change from run to
# run, and use values near the real ones (the status-*.json files of the work directory).
FREEZE = {
    "/api/status": {
        "firmware": no_hash, "uptime_s": 3600,
        "heap.internal_free": 200000, "heap.internal_min": 170000, "heap.psram_free": 8290000,
        "time.utc_s": 1767268800,   # 2026-01-01 12:00:00 UTC
        "devices.*.cycles": 3600, "devices.*.failed": 0, "devices.*.ok": 24000,
        "devices.*.timeout": 0, "devices.*.crc": 0, "devices.*.frame": 0, "devices.*.exception": 0,
    },
    "/api/firmware": {"running": no_hash, "other": no_hash},
    # the countdown of the claim page starts from the full window (claimsim 600)
    "/api/claim": {"left_s": 600},
}
# Issue #187: the values of the access point that change at each run (the time to the stop), and
# the values that only the emulator gives, replaced with the values of the board. QEMU has no
# radio: the access point is a model ("simulated": the board gives false), and the claim goes
# over the emulated Ethernet ('claimsim ... eth'), so "ap.url" of the claim answer is the address
# of the Ethernet; on the board it is the address of the access point.
FREEZE["/api/status"].update({
    "ap.simulated": False,
    "ap.left_s": lambda v: None if v is None else 1680,   # 28 min of the 30 min maximum time
    "ap.open_left_s": 0,
})
FREEZE["/api/claim"].update({"ap.url": "https://192.168.4.1/"})


def freeze(doc, paths):
    """@doc with each path of @paths (FREEZE) set to its value where the path exists."""
    doc = copy.deepcopy(doc)
    for path, value in paths.items():
        _set(doc, path.split("."), value)
    return doc


def _set(node, keys, value):
    k, rest = keys[0], keys[1:]
    items = (list(enumerate(node)) if isinstance(node, list) else []) if k == "*" else \
        ([(k, node[k])] if isinstance(node, dict) and k in node else [])
    for key, child in items:
        if rest:
            _set(child, rest, value)
        else:
            node[key] = value(child) if callable(value) else value


# ---------------------------------------------------------------- the shots

@dataclass
class Shot:
    """One image. @name: the file docs/images/<name>.png. @state: a key of STATES. @device: PHONE
    or DESKTOP. @path: the page. @actions: what to do before the capture, each one of
    ("click", selector), ("fill", selector, text), ("press", selector, key), ("wait", selector)
    (visible), ("wait_text", selector, text), ("sleep", seconds), or a function(page).
    @prep: a function(Gateway) that runs on the emulator before the page opens (for example
    a push of BOOT); it changes the state for the shots after it.
    @wait: a selector that must be visible before the capture. @element: capture only this
    element (with @pad CSS pixels around it); else @full_page or the viewport. @auth: send the
    credential of the state (False: open the page without it). @alt: the text alternative to put
    next to the image in the docs (the script prints it)."""
    name: str
    state: str
    device: dict
    path: str
    alt: str
    actions: list = field(default_factory=list)
    prep: object = None
    wait: str = ""
    element: str = ""
    pad: int = 8
    full_page: bool = False
    auth: bool = True
    freeze_timers: bool = True


SHOTS = [
    Shot("claim-page-phone", "claim", PHONE, "/claim", auth=False, wait="#form:not(.hidden)",
         element="main", pad=0,
         alt="The page 'Set the web password' on a phone: the claim window is open for 10 min. "
             "The page tells that the web password is also the Wi-Fi password of the access "
             "point, and it has two password fields and the button 'Set the password'."),
    # issue #187: the claim changes the state (a web password is set); keep it after the page
    Shot("claim-done-phone", "claim", PHONE, "/claim", auth=False, element="main", pad=0,
         actions=[("wait", "#form:not(.hidden)"), ("fill", "#pw", DOCS_PASSWORD),
                  ("fill", "#pw2", DOCS_PASSWORD), ("click", "#go")],
         wait="#done:not(.hidden)",
         alt="The page after the claim on a phone: 'The web password is set'. In 5 seconds, "
             "the Wi-Fi network GW-Setup changes to WPA2 and asks for the new web password. "
             "The steps tell how to connect again, and the page gives the addresses of the "
             "gateway on the plant network."),
    Shot("status-page", "configured", DESKTOP, "/", wait="#st-mb table", full_page=True,
         alt="The Status tab of the web UI: the gateway, the network and MQTT, the access "
             "point (off), the Modbus devices, the local I/O, the firmware and the microSD "
             "card."),
    Shot("configuration-page", "configured", DESKTOP, "/",
         actions=[("click", "nav button[data-tab=config]")], wait="#f-mqtt input",
         alt="The Configuration tab of the web UI: the forms for the network, MQTT and "
             "Sparkplug."),
    # docs/how-to/add-a-modbus-device.md and the tutorial
    Shot("modbus-test-read", "configured", DESKTOP, "/", actions=[test_read],
         element=".card:has(#mb-read)", wait="#mb-msg.ok",
         alt="The card 'Modbus test read' of the Configuration tab: unit 1, area input, address "
             "0, type float32, word order ABCD. The result line shows the two registers 17254 "
             "and 32768 and the value 230.5."),
    Shot("devices-and-tags", "configured", DESKTOP, "/",
         actions=[("click", "nav button[data-tab=config]")], element=".card:has(#f-dev)",
         wait="#f-dev table",
         alt="The card 'Devices and tags' of the Configuration tab: the device Meter1, unit 1, "
             "poll interval 1000 ms, its Sparkplug topic and its UMH topic, and the table of "
             "its tags with the area, the address, the type, the word order, the scale, the "
             "units and the writable column. Each field has a (?) button, and each tag has a "
             "button 'More' for its other fields."),
    Shot("tag-explainer", "configured", DESKTOP, "/", actions=[tag_explainer],
         wait="#xpl:not(.hidden)",
         alt="The tag table of Meter1 with the details row of the tag Voltage open: Offset, "
             "Deadband, Eng. low, Eng. high, Bit and Length. The explainer of Eng. high is open "
             "below its (?) button. It tells that Eng. high is the top of the normal range of "
             "the value, and that the gateway does not limit the value."),
    # docs/how-to/update-the-firmware.md
    Shot("firmware-card", "configured", DESKTOP, "/", element=".card:has(#st-fw)",
         actions=[("wait_text", "#st-fw", "Running")], wait="#st-fw",
         alt="The card 'Firmware' of the Status tab: the running version, its slot, the state "
             "of the last update, and the other slot. Below are the field for the URL of an "
             "image with the button 'Download and install', the field 'Upload an image', and "
             "the check box 'Force (into recovery mode)'."),
    # issue #187: after the shots above, because a push of BOOT starts the access point
    Shot("ap-card-phone", "configured", PHONE, "/", prep=ap_on_with_client,
         element=".card:has(#st-ap)", wait="#ap-stop:not(.hidden)",
         alt="The card 'Access point' of the Status tab on a phone: the access point GW-Setup "
             "is on, its Wi-Fi password is the web password, it has 1 client, and it stops in "
             "28 min. Below is the button 'Close the access point'."),
    Shot("ap-card-closed-phone", "configured", PHONE, "/", prep=ap_on_with_client,
         actions=[close_ap],
         element=".card:has(#st-ap)", wait="#st-ap",
         alt="The card 'Access point' after the button 'Close the access point': the access "
             "point is off (closed on request). To start it, push the BOOT button of the "
             "gateway for 2 to 10 s."),
]


# ---------------------------------------------------------------- the harness

class Harness:
    """The E2E harness' services for the screenshots: one mosquitto, the Modbus slave, SNTP and
    one emulator at a time."""

    def __init__(self, a):
        self.a, self.work = a, pathlib.Path(a.workdir)
        self.gw, self.state = None, None
        e2e.WORK = self.work
        e2e.MON_DIR = pathlib.Path(tempfile.mkdtemp(prefix="docs-mon-"))
        psram = "psram=1" in (pathlib.Path(a.images) / "variant").read_text()
        flash = self.work / "flash.bin"
        shutil.copy(pathlib.Path(a.images) / "flash.bin", flash)
        if psram:
            e2e.write_otadata(flash)   # issue #85 (see e2e.write_otadata)
        # what e2e.Gateway and e2e.flash_variant read from the command line of e2e.py
        self.args = types.SimpleNamespace(
            qemu=a.qemu, flash=str(flash), efuse=a.efuse, psram=psram,
            littlefs_python=a.littlefs_python, tcg_thread="multi", simulate_qemu_death=None)
        e2e.ARGS = self.args
        isolated = False if a.no_isolate else e2e.isolate_network()
        print(f"network: {'private namespace' if isolated else 'host loopback'}; "
              f"PSRAM {'on' if psram else 'off'}", flush=True)
        conf = self.work / "broker.conf"
        conf.write_text(f"listener {e2e.MQTT_PORT} 127.0.0.1\nallow_anonymous true\n")
        e2e.start_broker(conf, self.work / "mosquitto.log")
        self.stop = threading.Event()
        threading.Thread(target=e2e.sntp_responder, args=(self.stop,), daemon=True).start()
        self.slave = e2e.ModbusSlave()
        self.slave.reset()
        self.slave.start()

    def boot(self, name, cfg):
        """Start the emulator on a copy of the image with @cfg as config.json."""
        flash = e2e.flash_variant(self.args, self.work, name,
                                  {"config.json": json.dumps(cfg, indent=2).encode() + b"\n"})
        gw = e2e.Gateway(self.args, self.work / f"uart0-{name}.log", flash)
        gw.start()
        return gw

    def enter(self, state):
        """The emulator in @state (a key of STATES): the running one if it is in that state."""
        if self.state != state:
            self.leave()
            t = time.monotonic()
            self.gw, self.auth = STATES[state](self)
            self.state = state
            print(f"state {state}: ready in {time.monotonic() - t:.0f} s", flush=True)
        return self.auth

    def leave(self):
        if self.gw is not None:
            self.gw.kill()
        self.gw, self.state = None, None

    def close(self):
        self.leave()
        self.stop.set()
        for p in e2e.PROCS:
            if p.poll() is None:
                p.kill()
                p.wait()
        shutil.rmtree(e2e.MON_DIR, ignore_errors=True)


# ---------------------------------------------------------------- Playwright

NO_TIMERS = "window.setInterval = () => 0;"   # the page polls once; no countdown runs


def route_freeze(route):
    """Answer an API request with the real answer of the gateway, with FREEZE applied."""
    path = re.sub(r"\?.*", "", route.request.url.split("//", 1)[1].split("/", 1)[-1])
    paths = FREEZE.get("/" + path)
    resp = route.fetch()
    if not paths or "json" not in resp.headers.get("content-type", ""):
        route.fulfill(response=resp)
        return
    try:
        doc = json.loads(resp.body())
    except ValueError:
        route.fulfill(response=resp)
        return
    route.fulfill(response=resp, body=json.dumps(freeze(doc, paths)))


def capture(browser, shot, auth, base):
    ctx_args = dict(shot.device, ignore_https_errors=True, color_scheme="light",
                    reduced_motion="reduce", locale="en-US", timezone_id="UTC")
    if auth and shot.auth:
        ctx_args["http_credentials"] = {"username": auth[0], "password": auth[1]}
    ctx = browser.new_context(**ctx_args)
    ctx.set_default_timeout(60000)
    try:
        if shot.freeze_timers:
            ctx.add_init_script(NO_TIMERS)
        ctx.route(re.compile(r".*/api/.*"), route_freeze)
        page = ctx.new_page()
        page.goto(base + shot.path, wait_until="networkidle")
        for act in shot.actions:
            if callable(act):
                act(page)
            elif act[0] == "click":
                page.click(act[1])
            elif act[0] == "fill":
                page.fill(act[1], act[2])
            elif act[0] == "press":
                page.press(act[1], act[2])
            elif act[0] == "wait":
                page.wait_for_selector(act[1], state="visible")
            elif act[0] == "wait_text":
                page.locator(act[1]).filter(has_text=act[2]).first.wait_for(state="visible")
            elif act[0] == "sleep":
                page.wait_for_timeout(act[1] * 1000)
            else:
                raise SystemExit(f"{shot.name}: unknown action {act!r}")
        page.wait_for_load_state("networkidle")
        if shot.wait:
            page.wait_for_selector(shot.wait, state="visible")
        opts = dict(animations="disabled", caret="hide", type="png")
        if shot.element:
            box = page.locator(shot.element).first.bounding_box()
            # the box is relative to the viewport, the clip of a full page to the page: add the
            # scroll position (a click scrolls the page, and a reload keeps the position)
            sx, sy = page.evaluate("[window.scrollX, window.scrollY]")
            p = shot.pad
            png = page.screenshot(full_page=True, clip={
                "x": max(box["x"] + sx - p, 0), "y": max(box["y"] + sy - p, 0),
                "width": box["width"] + 2 * p, "height": box["height"] + 2 * p}, **opts)
        else:
            png = page.screenshot(full_page=shot.full_page, **opts)
        return png
    finally:
        ctx.close()


def write_png(png, out, lossless):
    """Write @png to @out, reduced to 256 colours unless @lossless (the UI has few colours), only
    when its pixels differ from the file there. Returns (written, size in bytes)."""
    from PIL import Image
    img = Image.open(io.BytesIO(png)).convert("RGB")
    if not lossless:
        img = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    if out.exists():
        old = Image.open(out)
        if old.size == img.size and old.convert("RGB").tobytes() == img.convert("RGB").tobytes():
            return False, out.stat().st_size
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    out.write_bytes(buf.getvalue())
    return True, len(buf.getvalue())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--images", required=True,
                    help="the QEMU images of tools/e2e/run_e2e.sh with E2E_STAGE (flash.bin, "
                         "variant)")
    ap.add_argument("--qemu", default=shutil.which("qemu-system-xtensa"))
    ap.add_argument("--efuse", required=True, help="a blank eFuse file of the emulator")
    ap.add_argument("--littlefs-python", required=True,
                    help="python with the littlefs-python package of the build")
    ap.add_argument("--workdir", required=True, help="logs, flash copies and status-*.json")
    ap.add_argument("--out", default=str(HERE.parent.parent / "docs" / "images"))
    ap.add_argument("--only", default="", help="comma-separated shot names (default: all)")
    ap.add_argument("--list", action="store_true", help="list the shots and their alt texts")
    ap.add_argument("--lossless", action="store_true", help="keep all colours (larger files)")
    ap.add_argument("--no-isolate", action="store_true",
                    help="stay on the host loopback instead of a private network namespace")
    a = ap.parse_args()
    shots = SHOTS
    if a.only:
        want = a.only.split(",")
        unknown = set(want) - {s.name for s in SHOTS}
        if unknown:
            ap.error(f"unknown shot(s): {', '.join(sorted(unknown))}")
        shots = [s for s in SHOTS if s.name in want]
    if a.list:
        for s in shots:
            print(f"{s.name}.png ({s.state}): {s.alt}")
        return
    assert all(s.state in STATES for s in shots)
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    pathlib.Path(a.workdir).mkdir(parents=True, exist_ok=True)
    from playwright.sync_api import sync_playwright
    t0 = time.monotonic()
    h = Harness(a)
    try:
        with sync_playwright() as pw:
            browser = pw.chromium.launch()
            base = f"https://127.0.0.1:{e2e.HTTPS_PORT}"
            for s in shots:
                auth = h.enter(s.state)
                if s.prep:
                    s.prep(h.gw)
                png = capture(browser, s, auth, base)
                written, size = write_png(png, out / f"{s.name}.png", a.lossless)
                print(f"{s.name}.png: {'written' if written else 'unchanged'}, {size} bytes",
                      flush=True)
            browser.close()
    finally:
        h.close()
    print(f"{len(shots)} shot(s) in {time.monotonic() - t0:.0f} s; alt texts: --list", flush=True)


if __name__ == "__main__":
    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    main()
