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
import subprocess
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
# The browser opens the web UI at the mDNS name of the gateway (the default hostname gw01), as an
# operator does on the network, not at 127.0.0.1: the card Certificates warns when the certificate
# does not name the address of the page (#205), and the self-signed certificate names gw01.local.
# Chromium maps the name to the forwarded port on 127.0.0.1 (--host-resolver-rules). The pages of
# the access point (the state "claim") open at an address, as on the board (192.168.4.1).
DOCS_HOST = "gw01.local"
IP_STATES = {"claim"}

# Viewports: a phone for the pages that the operator opens on the access point, a desktop for the
# others. Playwright's own device list changes between releases: the values are fixed here.
PHONE = dict(viewport={"width": 390, "height": 844}, device_scale_factor=2, is_mobile=True,
             has_touch=True)
DESKTOP = dict(viewport={"width": 1280, "height": 800}, device_scale_factor=1)


# ---------------------------------------------------------------- the gateway states

# #206: the name, the location and the comment of the state "named" (the prototype's values)
GATEWAY_LABELS = {"name": "Pump panel 2", "location": "Plant 2, hall B",
                  "comment": "Feeds the cooling towers 1 and 2. Contact: maintenance, ext. 214."}


def docs_config(web_password=None, labels=None):
    """The E2E configuration (tools/e2e/fs/config.json) with only the device that answers
    (Meter1, unit 1 of the pymodbus slave), and the web password @web_password (None: no web
    password, the claim window can open). @labels: the section "gateway" (#206)."""
    cfg = json.loads(e2e.config_json())
    cfg["devices"] = [d for d in cfg["devices"] if d["name"] == e2e.DEVICE]
    del cfg["syslog"]   # its counters change at each run
    if web_password:
        cfg["web"] = {"username": "admin", "password": web_password}
    if labels:
        cfg["gateway"] = dict(labels)
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


def state_named(h):
    """#206: a configured gateway with a name, a location and a comment (GATEWAY_LABELS), in
    normal mode, with the web password DOCS_PASSWORD. The shots of #195 (Status tab and
    administration) use it."""
    gw = h.boot("named", docs_config(DOCS_PASSWORD, GATEWAY_LABELS))
    auth = ("admin", DOCS_PASSWORD)
    wait_ready(h, "named", gw, auth)
    return gw, auth


def state_pem(h):
    """#205 #221: a configured gateway whose saved configuration names PEM files of LittleFS:
    mqtt.tls (CA, client certificate and key, TLS off: the broker of the shots is plain) and
    ota.ca_file. The card Certificates then shows the keys that use each file."""
    pki = e2e.make_pki(pathlib.Path(tempfile.mkdtemp(prefix="docs-pki-")))
    cfg = docs_config(DOCS_PASSWORD)
    cfg["mqtt"]["tls"] = {"enabled": False, "ca_file": "ca.pem", "cert_file": "gw01.crt",
                          "key_file": "gw01.key"}
    cfg["ota"] = dict(cfg.get("ota", {}), ca_file="ca.pem")
    files = {"ca.pem": pki["ca.pem"].read_bytes(), "gw01.crt": pki["client.crt"].read_bytes(),
             "gw01.key": pki["client.key"].read_bytes()}
    gw = h.boot("pem", cfg, files)
    auth = ("admin", DOCS_PASSWORD)
    wait_ready(h, "pem", gw, auth)
    return gw, auth


# name -> function(harness) that boots the gateway and returns (Gateway, Basic auth or None)
STATES = {"configured": state_configured, "claim": state_claim, "named": state_named,
          "pem": state_pem}


def safe_mode_restart(gw):
    """#209: restart into safe mode through the REST API, as the button 'Restart into safe mode'
    does; it changes the state for the shots after it."""
    auth = ("admin", DOCS_PASSWORD)
    t = time.monotonic()
    st, _, body = e2e.http("POST", "/api/restart?safe=1", auth=auth)
    if st != 202 or not gw.wait_log(r"fw: safe mode on request", 120, t):
        raise SystemExit(f"safe mode did not start: POST {st} {body[:120]!r}")
    end = time.monotonic() + 90
    while time.monotonic() < end:
        code, _, body = e2e.http("GET", "/api/status", auth=auth, timeout=10)
        if code == 200 and ((e2e.as_json(body) or {}).get("safe_mode") or {}).get("active"):
            return
        time.sleep(1)
    raise SystemExit("safe mode: no status of the web server")


def gateway_card(page):
    """#206: the card Gateway at the top of the Configuration tab."""
    page.click("nav button[data-tab=config]")


def ask_safe_mode(page):
    """#209: the confirmation of 'Restart into safe mode' (the dialog stays open)."""
    page.click("#rr-safe")
    page.wait_for_selector("#dlg[open]")


def ask_factory_reset(page):
    """#203: the confirmation of 'Factory reset', with ERASE typed (the button is enabled)."""
    page.click("#rr-reset")
    page.fill("#dlg-i", "ERASE")


def live_log(page):
    """#200 and #199: the card Log open, with the tag poll at debug. This action is also
    the browser test of the decision of #200 (no E2E scenario runs a browser): the page sends no
    GET /api/log and no GET /api/log/level before the card opens."""
    page.wait_for_timeout(2500)
    asked = page.evaluate("performance.getEntriesByType('resource').map(e => e.name)"
                          ".filter(n => /\\/api\\/log/.test(n))")
    if asked:
        raise SystemExit(f"issue #200: the web UI asked for the log before the card opened: {asked}")
    page.evaluate("document.querySelector('#lg-card').scrollIntoView()")
    page.click("#lg-card summary")
    page.wait_for_selector("#lg-lines .ln")
    # the options of the tags have no value attribute (their text is the value)
    page.wait_for_function("[...document.querySelectorAll('#lg-tag option')]"
                           ".some(o => o.value === 'poll')")
    page.select_option("#lg-tag", "poll")
    page.select_option("#lg-lvl", "debug")


# #200: the lines of the shot live-log (the log of the firmware: each run has other times and
# other lines), as the prototype shows them
LOG_SAMPLE = [
    "W (3590441) net: link down",
    "W (3590442) net: active interface: ethernet -> none",
    "W (3590951) edge: MQTT down; reconnect attempt 1 in 1000 ms",
    "I (3592105) net: link up",
    "I (3592870) net: IPv4 192.168.10.50 mask 255.255.255.0 gw 192.168.10.1",
    "I (3592871) net: active interface: none -> ethernet",
    "I (3593010) edge: connecting to 192.168.10.5:1883 as 'GW01', bdSeq=8",
    "I (3593214) edge: Sparkplug session up (bdSeq=8): births sent in 182 ms",
    "D (3598402) mb: unit 3 fc 3 addr 0: attempt 1 failed (1)",
    "D (3598604) mb: unit 3 fc 3 addr 0: attempt 2 failed (1)",
    "D (3598807) poll: Pump1: fc 3 addr 0 qty 4 unanswered (status 1); cycle failed",
    "W (3600812) poll: Pump1 (unit 3) offline after 3 failed cycles",
    "D (3600813) poll: Pump1 offline: next probe in 2000 ms",
    "E (3601907) esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x7780",
    "W (3601908) httpd: httpd_accept_conn: session creation failed",
]


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
    """Click 'Close the access point', accept the confirmation (the dialog of the page, #193), and
    open the page again when the access point is off (the page polls only once: NO_TIMERS)."""
    page.click("#ap-stop")
    page.click("#dlg-ok")
    page.locator("#ap-msg").filter(has_text="stops in a few seconds").wait_for(state="visible")
    end = time.monotonic() + 20
    while True:
        page.wait_for_timeout(1000)
        page.reload(wait_until="networkidle")
        if page.locator("#st-ap").filter(has_text="Off").count() or time.monotonic() > end:
            break

def ask_close_ap(page):
    """Issue #193: the confirmation of 'Close the access point' (the dialog stays open)."""
    page.click("#ap-stop")
    page.wait_for_selector("#dlg[open]")


def ask_format(page):
    """Issue #193: the confirmation of 'Format the card', with FORMAT typed (the button is
    enabled)."""
    page.click("#sd-fmt")
    page.fill("#dlg-i", "FORMAT")


# Issue #192: the answer of the pre-flight check to an image that cannot read the stored
# configuration (gw_fw.c check_image). The QEMU build of the docs has only one image, so the
# answer to the upload is given here; the box is the real code of the page.
REFUSAL = ("pre-flight check: image 0.0.9 refused: the configuration needs schema 2, the image "
           "reads 1: devices[0].tags[3].scan=slow (2); force installs it into recovery mode")


def refuse_upload(page):
    """Issue #192: upload an image, accept the confirmation, and get the 409 of the pre-flight
    check (REFUSAL): the box of the refused image shows. Issue #215: the choice of the file does
    not start the upload; the button 'Upload and install' does."""
    page.route(re.compile(r".*/api/firmware$"), lambda r: r.fulfill(
        status=409, content_type="application/json",
        body=json.dumps({"ok": False, "error": REFUSAL})) if r.request.method == "POST"
        else r.fallback())
    page.click('#fw-card [data-src="file"]')
    page.set_input_files("#fw-file", {"name": "rs485_spb_gateway-0.0.9.bin",
                                      "mimeType": "application/octet-stream",
                                      "buffer": bytes([0xE9]) + bytes(4095)})
    page.click("#fw-go")
    page.click("#dlg-ok")
    page.wait_for_selector("#fw-ref:not(.hidden)")


# Issue #215: an image URL and its SHA-256 (the values of the prototype)
FW_URL = "https://files.example.com/gw/rs485_spb_gateway-0.2.0.bin"
FW_SHA = "17bf8431d18e8d424fb300303419bebde3749ad415de75afdaf4ecb1776030dd"
FW_FILE_SHA = "a1542ecce24b2afa8515aab3436f5e32ea054fa57257faf50f63efc255fef64b"


def firmware_url(page):
    """Issue #215: the source 'From a URL', with the URL and the SHA-256 of the image."""
    page.click('#fw-card [data-src="url"]')
    page.fill("#fw-url", FW_URL)
    page.fill("#fw-sha", FW_SHA)


def sha_mismatch(page):
    """Issue #215: the answer of GET /api/firmware after a download whose SHA-256 does not agree
    (the QEMU build of the docs has no image server)."""
    def answer(r):
        if r.request.method != "GET":
            r.fallback()
            return
        w = json.loads(fetch(r).text())
        w.update({"phase": "failed", "state": "failed: image 0.2.0: the SHA-256 of the file does "
                  "not agree", "detail": "image 0.2.0: the SHA-256 of the file does not agree",
                  "sha256_expected": FW_SHA, "sha256_file": FW_FILE_SHA})
        r.fulfill(status=200, content_type="application/json",
                  body=json.dumps(freeze(w, FREEZE["/api/firmware"])))
    page.route(re.compile(r".*/api/firmware$"), answer)
    # the page asks for /api/firmware only in its poll, and the poll runs once (NO_TIMERS): load
    # the page again to get the answer above
    page.reload(wait_until="networkidle")
    firmware_url(page)
    page.wait_for_selector("#fw-panel .pb")


def ota_card(page):
    """Issue #223: the card 'Firmware updates' of the Configuration tab."""
    page.click("nav button[data-tab=config]")
    page.evaluate("document.querySelector('#c-ota').scrollIntoView({block: 'center'})")


def store_forward(page):
    """Issue #221: the card Sparkplug of the Configuration tab with the diagnostics period and the
    section Store & forward: store & forward on, the spill to the microSD card on (the GPIO45
    caution shows) and the space on the card at its default. No save."""
    page.click("nav button[data-tab=config]")
    page.wait_for_selector("#sf-en")
    page.check("#sf-en")
    page.check("#sf-sd")
    page.wait_for_selector("#sf-sd-h.lwarn")
    page.evaluate("document.querySelector('#f-spb').closest('.card').scrollIntoView()")


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
    page.click('[data-more="devices.0|0"]')
    page.click("#det-devices-0-0 button.qi[data-x=enghi]")

def device_settings(page):
    """Issues #197 and #221: the panel 'More device settings' of Meter1. No save."""
    page.click("nav button[data-tab=config]")
    page.evaluate("document.querySelector('#f-dev>.card').scrollIntoView()")
    page.click('[data-dmore="0"]')


def write_limits(page):
    """Issue #196: the details row of the writable tag Setpoint of Meter1, with Write min 0 and
    Write max 150. No save."""
    page.click("nav button[data-tab=config]")
    page.evaluate("document.querySelector('#f-dev>.card').scrollIntoView()")
    page.click('[data-more="devices.0|1"]')
    page.fill('[data-p="devices.0.tags.1.write_min"]', "0")
    page.fill('[data-p="devices.0.tags.1.write_max"]', "150")


def bit_field(page):
    """Issue #202: the tag Alarm of Meter1 (holding 60) as a field of 3 bits: type uint16, Bit
    offset 4 and Bit width 3, with the hint below Bit width. No save."""
    page.click("nav button[data-tab=config]")
    page.evaluate("document.querySelector('#f-dev>.card').scrollIntoView()")
    page.select_option('[data-p="devices.0.tags.9.type"]', "uint16")
    page.click('[data-more="devices.0|9"]')
    page.fill('[data-p="devices.0.tags.9.bit_offset"]', "4")
    page.fill('[data-p="devices.0.tags.9.bit_width"]', "3")


def ask_make_type(page):
    """Issue #213: the dialog of 'Make a type from these tags' of Meter1, with the name Meter. No
    save."""
    page.click("nav button[data-tab=config]")
    page.click('[data-mk-type="0"]')
    page.fill("#dlg-n", "Meter")


def make_type(page):
    """Issue #213: Meter1 after 'Make a type from these tags' (the type Meter): the card Device
    types open with the type Meter open. No save."""
    ask_make_type(page)
    page.click("#dlg-ok")
    page.wait_for_selector("#f-dt [data-dt]")
    page.evaluate("document.querySelector('#dt-card').scrollIntoView()")


def device_with_type(page):
    """Issue #213: the device Meter1 with the device type Meter: no tag table, the number of the
    tags of the type and the link to the type. No save."""
    make_type(page)
    page.evaluate("document.querySelector('#f-dev>.card').scrollIntoView()")


def device_disabled(page):
    """Issue #214: Meter1 with Enabled cleared: the notice and the dashed border. No save."""
    page.click("nav button[data-tab=config]")
    page.uncheck('[data-p="devices.0.enabled"]')
    page.wait_for_selector("#off-0:not(.hidden)")


def local_io_card(page):
    """#210 #211 #217 #218 #221: the card Local I/O of the Configuration tab, with the local_io keys
    of the E2E configuration (DI/5 inverted with 200 ms, DI/8 a fast counter, DO/7 a pulse DO, run
    hours of DI/4 and DO/2, two local rules)."""
    page.click("nav button[data-tab=config]")
    page.wait_for_selector("#lio-di tr[data-i]")
    page.evaluate("document.querySelector('#lio-card').scrollIntoView()")


def ask_do(page):
    """#204: the first change of a DO in the page session (Turn on DO 3) asks first (#193)."""
    page.wait_for_selector('#st-io button[data-do="3"]')
    page.evaluate("document.querySelector('#st-io').scrollIntoView()")
    page.click('#st-io button[data-do="3"]')
    page.wait_for_selector("#dlg[open]")


# ---------------------------------------------------------------- the frozen values

# Issue #194, docs/how-to/change-the-web-password.md. These actions are also the test of #194
# (no E2E scenario runs a browser): a save with two different passwords, and a save that removes
# the web password and that the operator cancels, send no PUT /api/config.
NEW_PW = "new-password-2"


def web_password_form(page, confirm):
    """A new value in the Password field of the card 'Web interface', and @confirm in the field
    'Confirm the password' that it shows."""
    page.click("nav button[data-tab=config]")
    page.evaluate("document.querySelector('#f-web').closest('.card')"
                  ".scrollIntoView({block: 'center'})")
    page.fill('[data-p="web.password"]', NEW_PW)
    page.wait_for_selector("#i-pw2", state="visible")
    page.fill("#i-pw2", confirm)
    page.wait_for_selector("#b-chg:has-text('1 change, not saved')")   # a debounced count


def no_put(page, step):
    """Do @step(page); fail when the page sends a PUT (a save) meanwhile."""
    puts = []
    page.on("request", lambda r: puts.append(r.url) if r.method == "PUT" else None)
    step(page)
    page.wait_for_timeout(1000)
    if puts:
        raise SystemExit(f"issue #194: the web UI sent {puts}")


def web_password(page):
    web_password_form(page, NEW_PW)


def web_password_mismatch(page):
    """Save with a different value in the second field: the error shows below it, no PUT."""
    web_password_form(page, NEW_PW + "x")
    no_put(page, lambda p: (p.click("#b-save"), p.wait_for_selector("#i-pw2 + .ferr")))


def web_password_remove(page):
    """An empty Password field: Save asks first. Esc sends nothing; the second Save opens the
    dialog again for the image."""
    page.click("nav button[data-tab=config]")
    page.fill('[data-p="web.password"]', "")
    page.wait_for_selector("#b-chg:has-text('1 change, not saved')")

    def esc(p):
        p.click("#b-save")
        p.wait_for_selector("#dlg[open]")
        p.keyboard.press("Escape")
        p.wait_for_selector("#dlg", state="hidden")
    no_put(page, esc)
    page.click("#b-save")


# #201 #208 #216 #221: the cards of the network, the services and MQTT
# (docs/how-to/connect-the-gateway-to-wi-fi.md, connect-to-a-broker-with-tls.md,
# docs/reference/configuration.md). No save: the state does not change.
def wifi_networks(page):
    """The card Wi-Fi after "Find networks": the simulated list of QEMU (no radio)."""
    page.click("nav button[data-tab=config]")
    page.evaluate("document.querySelector('#wifi-card').scrollIntoView()")
    page.click("#w-find")
    page.wait_for_selector("#w-nets button.net")
    page.fill('[data-p="network.wifi.ssid"]', "Plant-IoT")
    page.wait_for_selector("#w-nets button.net.on")


def mqtt_tls(page):
    """The card MQTT with a backup broker and TLS on. "Load a file..." loads the CA, the client
    certificate and the client key of the E2E PKI (e2e.make_pki, openssl), and each field selects
    its file (decision of the product owner, #221). No save."""
    page.click("nav button[data-tab=config]")
    page.wait_for_selector("#f-tls select")
    pki = e2e.make_pki(pathlib.Path(tempfile.mkdtemp(prefix="docs-pki-")))
    for name, src, key in (("ca.pem", "ca.pem", "ca_file"), ("gw01.crt", "client.crt", "cert_file"),
                           ("gw01.key", "client.key", "key_file")):
        with page.expect_file_chooser() as fc:
            page.click(f'button[data-load="mqtt.tls.{key}"]')
        fc.value.set_files(files=[{"name": name, "mimeType": "application/x-pem-file",
                                   "buffer": pki[src].read_bytes()}])
        page.wait_for_selector(f"#tls-msg.ok:has-text('{name}')")
    page.click('[data-ladd="bk"]')
    page.fill('[data-p="mqtt.backup_brokers.0.host"]', "broker2.plant.local")
    page.check('[data-p="mqtt.tls.enabled"]')
    page.fill('[data-p="mqtt.port"]', "8883")
    page.evaluate("document.querySelector('#f-mqtt').closest('.card').scrollIntoView()")


def certificates_card(page):
    """#205 #221: the card Certificates in the state "pem" (the saved configuration uses ca.pem,
    gw01.crt and gw01.key): the HTTPS certificate of the first boot and the list of the PEM
    files with the keys that use them."""
    page.click("nav button[data-tab=config]")
    page.wait_for_selector("#pf .pf-t")
    page.wait_for_selector("#wc dl.kv")
    page.evaluate("document.getElementById('certs').scrollIntoView()")


def choose(page, pick, name, data):
    """Choose the file @name (@data) with the picker @pick of the open dialog."""
    with page.expect_file_chooser() as fc:
        page.click(f'#dlg [data-pick="{pick}"]')
    fc.value.set_files(files=[{"name": name, "mimeType": "application/x-pem-file",
                               "buffer": data}])


def pem_replace(page):
    """#221: Replace gw01.crt in the card Certificates, with a new certificate and its key (the
    files of the E2E PKI): the dialog after the dry run of the gateway (no replace)."""
    certificates_card(page)
    pki = e2e.make_pki(pathlib.Path(tempfile.mkdtemp(prefix="docs-pki-")))
    page.click('#certs .pf-t [data-pfrep="gw01.crt"]')
    choose(page, "f", "gw01-2027.crt", pki["client.crt"].read_bytes())
    choose(page, "k", "gw01-2027.key", pki["client.key"].read_bytes())


def make_plant_pki():
    """#205: a plant CA and a web certificate of the plant for this gateway (gw01: DNS:gw01.local,
    as the self-signed certificate of the first boot), with openssl. e2e.make_web_pki makes a
    certificate for gw22, and the dialog then warns that the SANs do not contain the hostname.
    Returns (the chain: leaf + CA, the key) as bytes."""
    d = pathlib.Path(tempfile.mkdtemp(prefix="docs-plantpki-"))

    def ossl(*a):
        subprocess.run(["openssl", *a], cwd=d, check=True, capture_output=True)

    for k in ("ca", "web"):
        ossl("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", f"{k}.key")
    ossl("req", "-x509", "-new", "-key", "ca.key", "-sha256", "-days", "3650", "-subj",
         "/CN=Plant CA/O=Plant", "-out", "ca.pem")
    ossl("req", "-new", "-key", "web.key", "-subj", "/CN=gw01.plant.local/O=Plant", "-out",
         "web.csr")
    (d / "web.ext").write_text("subjectAltName=DNS:gw01.plant.local,DNS:gw01.local,"
                               "IP:192.168.1.50\n")
    ossl("x509", "-req", "-in", "web.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
         "-days", "365", "-sha256", "-extfile", "web.ext", "-out", "web.crt")
    return (d / "web.crt").read_bytes() + (d / "ca.pem").read_bytes(), (d / "web.key").read_bytes()


def web_cert_load(page):
    """#205: Load a certificate in the card Certificates, with a certificate of a plant CA for
    gw01 and its key (make_plant_pki): the dialog after the dry run of the gateway (no load)."""
    certificates_card(page)
    chain, key = make_plant_pki()
    page.click("#wc-load")
    choose(page, "c", "gw01-plant.pem", chain)
    choose(page, "k", "gw01-plant.key", key)


def time_servers(page):
    """The card Time with a second SNTP server."""
    page.click("nav button[data-tab=config]")
    page.click('[data-ladd="ntp"]')
    page.fill('[data-p="ntp.servers.1"]', "192.168.10.1")


def modbus_local(page):
    """The card Modbus TCP with the local unit."""
    page.click("nav button[data-tab=config]")
    page.evaluate("document.querySelector('#f-mbloc').scrollIntoView({block: 'center'})")


def ipv6_card(page):
    """#221: the card IPv6 with IPv6 on, SLAAC, a static address for Ethernet and an IPv6 DNS
    server. No save."""
    page.click("nav button[data-tab=config]")
    page.check('[data-p="network.ipv6.enabled"]')
    page.fill('[data-p="network.ipv6.address"]', "fd00:1::50")
    page.fill('[data-p="network.ipv6.dns"]', "fd00:1::1")
    page.evaluate("document.querySelector('#f-v6').closest('.card').scrollIntoView({block: 'center'})")


def syslog_card(page):
    """#221: the card Remote syslog with a server, its port and the level. No save."""
    page.click("nav button[data-tab=config]")
    page.fill('[data-p="syslog.host"]', "192.168.10.5")
    page.fill('[data-p="syslog.port"]', "514")
    page.evaluate("document.querySelector('#f-syslog').closest('.card').scrollIntoView({block: 'center'})")


def web_off(page):
    """Enabled off in the card Web interface, then Save: the confirmation with the steps to turn
    the web interface on again (no save: the dialog stays open for the image)."""
    page.click("nav button[data-tab=config]")
    page.uncheck('[data-p="web.enabled"]')
    page.wait_for_selector("#lw-web\\.enabled")
    page.click("#b-save")


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
    # #200: the lines of the live log change at each run (LOG_SAMPLE)
    "/api/log": {"lines": LOG_SAMPLE, "count": 212, "lost": 0},
    # #199: the changed levels of the shot live-log (the card Log reads them here, not in
    # /api/status)
    "/api/log/level": {"changed": [{"tag": "poll", "level": "debug"},
                                   {"tag": "mb", "level": "debug"}]},
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
# #212: the largest free block changes at each run; QEMU has no temperature sensor (null): the
# value of a board
FREEZE["/api/status"].update({"heap.internal_largest": 110592, "temperature_c": 46.5})
# #217: the run hours of DO/2 (on since the boot) grow at each run: a fixed value (96.1 h)
FREEZE["/api/status"].update({"io.run_hours.do.*": lambda v: None if v is None else 345960})
# The RTC of the emulator loses the time at each start of QEMU (the board keeps it with its
# battery), and the Wi-Fi scan of QEMU is a simulated list (#208): the values of the board
FREEZE["/api/status"].update({"time.rtc.lost": False})
FREEZE["/api/wifi/scan"] = {"simulated": False}
# The SNTP server of the E2E configuration is the host of the emulator (10.0.2.2): the default
# server of a board. The test certificates of e2e.make_pki are valid for 30 days from each run
# (the card MQTT shows "in N days"): a fixed last day, more than 90 days away (no warning).
FREEZE["/api/config"] = {"ntp.servers.*": "pool.ntp.org"}
FREEZE["/api/files"] = {"files.*.certs.*.not_after": "2036-11-30"}
# #205: the gateway makes its self-signed certificate at the first boot of each run (a new
# SHA-256 fingerprint), and the last day of the plant certificate of the shot web-cert-load-dialog
# (the dry run of PUT /api/web/certificate) is one year after each run: fixed values
FREEZE["/api/web/certificate"] = {
    "current.sha256": "A7:3C:51:E2:09:8B:D4:6F:20:C9:7E:13:B5:48:FA:02:6D:91:3A:C7:E4:58:0B:F6:"
                      "2D:84:19:AE:63:CB:70:15",
    "certificate.not_after": "2027-11-30"}


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
    freeze: dict = field(default_factory=dict)   # more FREEZE paths of /api/status, this shot only


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
         alt="The Status tab of the web UI: the card Gateway with the mode normal, the network "
             "and MQTT (connected), the access point (off), the Modbus devices with Meter1 "
             "online and the values of its tags, the local I/O, the firmware, the microSD card, "
             "the log (closed), and the card 'Restart and reset'."),
    # the top of the Status tab, for the project README
    Shot("status-overview", "configured", DESKTOP, "/", wait="#st-mb table",
         alt="The top of the Status tab of the web UI: the card Gateway (firmware, mode normal, "
             "heap and PSRAM), the network and MQTT (connected), the access point (off), and "
             "the Modbus device Meter1 online with the values of its tags."),
    Shot("configuration-page", "configured", DESKTOP, "/",
         actions=[("click", "nav button[data-tab=config]")], wait="#f-mqtt input",
         alt="The Configuration tab of the web UI: the buttons 'Open all' and 'Close all' at "
             "the top, the card Gateway, and the group Network with the cards Network (Ethernet) "
             "and Wi-Fi. The bar at the bottom of the window shows 'No changes' and the buttons "
             "Validate and 'Save and restart'."),
    # #222: docs/how-to/change-the-configuration.md. Each shot has a new browser context: the
    # state of the cards (localStorage) of this shot does not go to the shots after it. A tall
    # viewport, not a full-page capture: the full tab with the bar at the bottom of the window.
    Shot("configuration-cards-closed", "configured",
         dict(DESKTOP, viewport={"width": 1280, "height": 1400}), "/",
         actions=[("click", "nav button[data-tab=config]"), ("wait", "#f-mqtt input"),
                  ("click", "#cc-close")],
         wait='#tab-config .card.cc[data-cc="dev"]:not(.open)',
         alt="The Configuration tab after 'Close all': the card Gateway, and the groups Network, "
             "MQTT and Sparkplug, Services and Field with their cards. Each card is closed and "
             "shows a summary of its values in its header, for example 'DHCP' for Network "
             "(Ethernet), 'Group Plant1 · Edge Node GW01 · naming sparkplug' for Sparkplug and "
             "'9600 baud · 8N1' for RS485."),
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
         alt="The tag table of Meter1 with the details row of the float32 tag Voltage open: "
             "Offset, Deadband, Eng. low, Eng. high, Write min, Write max and Scan. The "
             "explainer of Eng. high is open below its (?) button. It tells that Eng. high is "
             "the top of the normal range of the value, and that the gateway does not limit "
             "the value."),
    # issues #196, #197, #214 and #221: docs/how-to/add-a-modbus-device.md and
    # write-to-a-modbus-device.md (no save: the state does not change)
    Shot("device-settings", "configured", DESKTOP, "/", actions=[device_settings],
         wait="#dset-0",
         alt="The device Meter1 with the panel 'More device settings' open below its fields: "
             "Slow poll interval (ms), Block reads, Max gap and Write with FC15/FC16."),
    Shot("tag-write-limits", "configured", DESKTOP, "/", actions=[write_limits],
         wait="#det-devices-0-1",
         alt="The details row of the writable tag Setpoint of Meter1: Write min 0 and Write "
             "max 150."),
    # issues #202 and #213: docs/how-to/add-a-modbus-device.md and
    # docs/how-to/add-devices-of-one-type.md (no save: the state does not change)
    Shot("tag-bit-field", "configured", DESKTOP, "/", actions=[bit_field],
         wait="#det-devices-0-9",
         alt="The details row of the tag Alarm of Meter1 with the type uint16: Bit offset 4 "
             "and Bit width 3, and below Bit width the hint 'Bits 4 to 6: values 0 to 7'. The "
             "row shows no Length (registers), because it applies only to string and bytes."),
    Shot("device-type-make-dialog", "configured", DESKTOP, "/", actions=[ask_make_type],
         wait="#dlg[open]",
         alt="The dialog 'Make a device type from the tags of Meter1': the field 'Name of the new "
             "type' with Meter, the hint '1 to 39 bytes. No /, + or #. It is also the name of "
             "the Sparkplug template.', the list of what occurs, and the buttons Cancel and "
             "'Make the type'."),
    Shot("device-types-card", "configured", DESKTOP, "/", actions=[make_type],
         element="#dt-card",
         alt="The card 'Device types (1)' of the Configuration tab with the type Meter open: "
             "its name, the line 'Used by 1 device: Meter1', the table of its tags and the "
             "buttons '+ tag' and 'Remove type'. 'Remove type' is disabled, and the text next to "
             "it tells to give the devices another type first."),
    Shot("device-with-type", "configured", DESKTOP, "/", actions=[device_with_type],
         element="#f-dev>.card",
         alt="The device Meter1 with 'Meter' in the field Device type. In place of a tag table, "
             "a grey box tells that the device uses the tags of the device type Meter, with the "
             "link 'Go to the type Meter'. The device has no button '+ tag'."),
    Shot("device-disabled", "configured", DESKTOP, "/", actions=[device_disabled],
         element="#f-dev>.card",
         alt="The device Meter1 with the check box Enabled cleared. The card has a dashed "
             "border, and a notice below the fields tells that the gateway does not poll this "
             "device and sends no DBIRTH, and that the validation still examines it."),
    # issue #191, docs/how-to/change-the-configuration.md and add-a-modbus-device.md: a Validate
    # (dry run, the state does not change) that the gateway refuses
    Shot("validate-error", "configured", DESKTOP, "/",
         actions=[("click", "nav button[data-tab=config]"),
                  ("fill", '[data-p="devices.0.unit_id"]', "300"), ("click", "#b-check")],
         wait=".ferr",
         alt="The Configuration tab after Validate with the unit ID 300 for Meter1. Below the "
             "field Unit ID, the text 'Value 300 out of range [1, 247]' shows in red. The bar "
             "at the bottom of the window shows '1 change, not saved', '1 error: Meter1 · "
             "Unit ID' with the link 'Go to the field', and the buttons Validate and 'Save and "
             "restart'."),
    # docs/how-to/change-the-web-password.md (no save: the state does not change)
    Shot("web-password-field", "configured", DESKTOP, "/", actions=[web_password],
         element=".card:has(#f-web)",
         alt="The card 'Web interface' of the Configuration tab: the user admin, a new password "
             "in the field 'Password (8 to 63)', the same password in the field 'Confirm the "
             "password'. Above them are Enabled, HTTPS, HTTPS port and HTTP port. Below the "
             "second field: after the restart, the browser asks for the new password, and the "
             "Wi-Fi password of the access point changes to the new password."),
    Shot("web-password-mismatch", "configured", DESKTOP, "/", actions=[web_password_mismatch],
         alt="The Configuration tab after 'Save and restart' with two different passwords. "
             "Below the field 'Confirm the password', a red text tells that the two passwords "
             "are not the same. The bar at the bottom of the window shows '1 error: Web "
             "interface · Confirm the password' with the link 'Go to the field'."),
    Shot("web-password-remove", "configured", DESKTOP, "/", actions=[web_password_remove],
         wait="#dlg[open]",
         alt="The confirmation 'Remove the web password?' after 'Save and restart' with an "
             "empty password field: web.password is empty. After the restart, the gateway "
             "accepts only the user admin with the commissioning password of the serial console, "
             "the Wi-Fi password of the access point becomes the commissioning password, and a "
             "push of BOOT opens the claim window. It has the red button 'Remove the password' "
             "and Cancel."),
    # docs/how-to/update-the-firmware.md
    # issue #215: the reorganized card
    Shot("firmware-card", "configured", DESKTOP, "/", element="#fw-card",
         actions=[("wait_text", "#fw-mq", "Off"), firmware_url], wait="#fw-go:enabled",
         alt="The card 'Firmware' of the Status tab: a summary with the running version and its "
             "slot, the last update (idle), the other slot and Update from MQTT (Off). Below is "
             "the area 'Install a new image' with the source 'From a URL' selected, the field "
             "'Image URL', the field 'SHA-256 (optional)' with a value, and the button 'Download "
             "and install'."),
    Shot("firmware-sha-mismatch", "configured", DESKTOP, "/", element="#fw-card",
         actions=[("wait_text", "#fw-mq", "Off"), sha_mismatch], wait="#fw-panel .pb",
         alt="The card 'Firmware' after a download whose SHA-256 does not agree: the last update "
             "is failed, and a red panel gives the expected SHA-256 and the SHA-256 of the file. "
             "The gateway did not install the image, and the running firmware continues to "
             "run."),
    # issue #223: docs/how-to/update-the-firmware.md and docs/reference/configuration.md
    Shot("firmware-updates-card", "configured", DESKTOP, "/", actions=[ota_card],
         element="#c-ota",
         alt="The card 'Firmware updates' of the Configuration tab: the check box 'Update from "
             "MQTT' is off. The hint tells to turn it on only when the broker lets only the "
             "Sparkplug host publish to the NCMD topic of the gateway. The card also has the "
             "field 'CA file of the image server', with a button 'Load a file…'."),
    # issue #221: docs/reference/store-and-forward.md and docs/reference/configuration.md
    Shot("sparkplug-store-forward", "configured", DESKTOP, "/", actions=[store_forward],
         element='.card[data-cc="spb"]', wait="#sf-sd-h.lwarn",
         alt="The card 'Sparkplug' of the Configuration tab: Group ID, Edge node ID, Primary host "
             "ID, Naming and 'Diagnostics period (s)', the device prefix, and the section 'Store "
             "& forward'. 'Keep the changes without a session' and 'Spill to the microSD card' "
             "are on, and a caution tells to make sure that the card slot has no pull-up on D0 "
             "(GPIO45). 'Space on the card (KB)' is 131072, '= 128 MB'."),
    # issue #192: docs/how-to/update-the-firmware.md
    Shot("firmware-refused", "configured", DESKTOP, "/", element="#fw-card",
         actions=[("wait_text", "#fw-mq", "Off"), refuse_upload], wait="#fw-rgo",
         alt="The card 'Firmware' after an upload that the pre-flight check refused: a red box "
             "gives the file and its version, the reason (the configuration needs schema 2, "
             "the image reads 1) and what recovery mode is. It has the buttons 'Install into "
             "recovery mode' and 'Cancel'."),
    # issue #193: docs/reference/store-and-forward.md. QEMU has no microSD card: the card of
    # the board (a card that the gateway cannot mount, with files of another device)
    # (the viewport: a full-page capture puts a fixed element at another place)
    Shot("card-format-dialog", "configured", DESKTOP, "/", actions=[ask_format],
         wait="#dlg-ok:enabled",
         freeze={"card": {"state": "not_mounted", "detail": "no FAT file system", "name": "SD64G",
                          "size_mb": 60928, "other_entries": 3}},
         alt="The confirmation 'Format the microSD card?': the card SD64G, 59.5 GB, cannot "
             "mount. The format erases all the contents of the card, also its 3 files or "
             "folders that are not from the gateway. The field has FORMAT, and the red button "
             "'Format the card' is enabled."),
    # #210 #211 #217 #218 #221: docs/how-to/set-up-the-local-io.md
    Shot("local-io-card", "configured", DESKTOP, "/", actions=[local_io_card],
         element=".card:has(#lio-di)", wait="#lio-di tr[data-i]",
         alt="The card 'Local I/O' of the Configuration tab: Safe state after 5 s and Counter save "
             "interval 60 s. The table of the DIs has Invert, Debounce, Counter, Glitch filter and "
             "Run hours: DI 5 is inverted with a debounce of 200 ms, DI 8 is a fast (PCNT) counter "
             "with a glitch filter of 1000 ns, DI 4 counts its run hours. The table of the DOs has "
             "Safe state, Pulse, Local rule, Mode and Run hours: DO 7 is a pulse DO of 300 ms, DO 5 "
             "follows DI 2, DO 6 toggles with DI 3, DO 2 counts its run hours. Below are the alarms "
             "of the buzzer and Alarm after."),
    # issue #196: docs/how-to/write-to-a-modbus-device.md and docs/reference/status-signals.md.
    # A count of the refused writes (the docs configuration has no write limits)
    Shot("status-writes-refused", "configured", DESKTOP, "/", element=".card:has(#st-mb)",
         wait="#st-mb .wr", freeze={"devices.*.write_refused": 3},
         alt="The card 'Modbus devices' of the Status tab: the device Meter1 is online, and below "
             "'online' its State cell shows '3 writes refused' in amber, with a (?) button for "
             "the explainer."),
    # #204 #217: docs/how-to/set-up-the-local-io.md
    Shot("status-local-io", "configured", DESKTOP, "/", element=".card:has(#st-io)",
         wait="#st-io table",
         alt="The card 'Local I/O' of the Status tab: the table of the DIs with the state, the "
             "counter (DI 8: fast) and the run hours, and the table of the DOs with the state, a "
             "button Turn on or Turn off, Pulse 300 ms for DO 7, the local rules of DO 5 and DO 6 "
             "in place of a button, and the run hours of DO 2. Below are the safe state, the "
             "buzzer and the time."),
    Shot("do-first-change-dialog", "configured", DESKTOP, "/", actions=[ask_do],
         wait="#dlg-ok",
         alt="The confirmation 'Turn on DO 3?' of the Status tab: DO 3 is off and has no local "
             "rule. Make sure that it is safe to start the machine on DO 3. The new state goes to "
             "the Sparkplug host in the next NDATA. The page does not ask again until it loads "
             "again. It has the red button 'Turn on DO 3' and Cancel."),
    Shot("do-first-change-dialog-phone", "configured", PHONE, "/", actions=[ask_do],
         wait="#dlg-ok",
         alt="The same confirmation 'Turn on DO 3?' as a sheet at the bottom of a phone, with the "
             "red button 'Turn on DO 3' above Cancel."),
    # #208: docs/how-to/connect-the-gateway-to-wi-fi.md
    Shot("wifi-networks", "configured", DESKTOP, "/", actions=[wifi_networks],
         element="#wifi-card", wait="#w-nets button.net.on",
         alt="The card 'Wi-Fi' of the Configuration tab with the list of the networks open: "
             "Plant-IoT, Plant-Office, Plant-Corp (WPA2-Enterprise, not supported), "
             "Warehouse-AP2, Guest and DIRECT-7F-HP M479, strongest first, each with its signal "
             "in dBm and its security. Plant-IoT is in the field SSID."),
    # #221: docs/how-to/connect-to-a-broker-with-tls.md
    Shot("mqtt-tls", "configured", DESKTOP, "/", actions=[mqtt_tls],
         element=".card:has(#f-tls)", wait="#f-tls select",
         alt="The card 'MQTT' of the Configuration tab: the broker, the port 8883, the protocol, "
             "the table of the backup brokers, and the section TLS with Encrypt (TLS) on. The CA "
             "file ca.pem shows its subject and its last day, the client certificate gw01.crt "
             "its subject, and the client key gw01.key 'Private key, EC P-256'. Each file field "
             "has a button 'Load a file…'. Below the section, a message tells that gw01.key is "
             "in the gateway."),
    Shot("web-cert-load-dialog", "configured", DESKTOP, "/", actions=[web_cert_load],
         wait="#wc-ex .exam b.ok",
         alt="The dialog 'Load the HTTPS certificate of the plant': the certificate "
             "gw01-plant.pem with its chain and the key gw01-plant.key are chosen. The gateway "
             "examined the files: the subject, the issuer CN=Plant CA, a chain of 2 "
             "certificates, the SANs with gw01.local, and the key agrees. The list says that "
             "the new certificate applies after a restart. It has the button 'Load the "
             "certificate' and Cancel."),
    # #201: docs/reference/configuration.md (ntp)
    Shot("time-card", "configured", DESKTOP, "/", actions=[time_servers],
         element='.card[data-cc="ntp"]',
         alt="The card 'Time' of the Configuration tab with two SNTP servers, pool.ntp.org and "
             "192.168.10.1, and the button '+ server'."),
    # #216: docs/reference/configuration.md (the local unit)
    Shot("modbus-tcp-card", "configured", DESKTOP, "/", actions=[modbus_local],
         element='.card[data-cc="mbtcp"]',
         alt="The card 'Modbus TCP' of the Configuration tab: Enabled, the port 502, the idle "
             "timeout, the allowed client 10.0.2.2, and the section 'The I/O of the gateway as "
             "a Modbus unit' with the local unit 100 and Local writes on."),
    # #221: docs/reference/configuration.md (network.ipv6)
    Shot("ipv6-card", "configured", DESKTOP, "/", actions=[ipv6_card],
         element='.card[data-cc="v6"]',
         alt="The card 'IPv6' of the Configuration tab with Enabled and SLAAC on, the static "
             "address fd00:1::50 for Ethernet and the IPv6 DNS server fd00:1::1."),
    # #221: docs/reference/configuration.md (syslog)
    Shot("syslog-card", "configured", DESKTOP, "/", actions=[syslog_card],
         element='.card[data-cc="syslog"]',
         alt="The card 'Remote syslog' of the Configuration tab with the host 192.168.10.5, the "
             "UDP port 514 and the level (default)."),
    # #221: docs/how-to/recover-the-gateway.md ("The web interface is off")
    Shot("web-off-dialog", "configured", DESKTOP, "/", actions=[web_off], wait="#dlg[open]",
         alt="The confirmation 'Turn off the web interface?' after 'Save and restart' with "
             "Enabled off: after the restart, the page and the REST API do not answer. The steps "
             "to turn the web interface on again: push BOOT for 2 to 10 s, open the web UI, turn "
             "on Enabled and save. It has the red button 'Save and restart' and Cancel."),
    # issue #187: after the shots above, because a push of BOOT starts the access point
    Shot("ap-card-phone", "configured", PHONE, "/", prep=ap_on_with_client,
         element=".card:has(#st-ap)", wait="#ap-stop:not(.hidden)",
         alt="The card 'Access point' of the Status tab on a phone: the access point GW-Setup "
             "is on, its Wi-Fi password is the web password, it has 1 client, and it stops in "
             "28 min. Below is the button 'Close the access point'."),
    # issue #193: docs/how-to/get-on-the-access-point.md. The phone of that procedure is a client
    # of the access point; in QEMU the browser is on the Ethernet (request_via_ap false)
    Shot("ap-close-dialog-phone", "configured", PHONE, "/", prep=ap_on_with_client,
         actions=[ask_close_ap], wait="#dlg-ok", freeze={"ap.request_via_ap": True},
         alt="The confirmation 'Close the access point?' as a sheet at the bottom of a phone: "
             "GW-Setup stops in a few seconds, and its client loses the connection. This "
             "browser uses the access point: the page then stops. Below are the red button "
             "'Close the access point' and 'Cancel'."),
    Shot("ap-card-closed-phone", "configured", PHONE, "/", prep=ap_on_with_client,
         actions=[close_ap],
         element=".card:has(#st-ap)", wait="#st-ap",
         alt="The card 'Access point' after the button 'Close the access point': the access "
             "point is off (closed on request). To start it, push the BOOT button of the "
             "gateway for 2 to 10 s."),
    # #205 #221: docs/how-to/manage-the-certificates.md (the state "pem": the saved configuration
    # uses the files)
    Shot("certificates-card", "pem", DESKTOP, "/", actions=[certificates_card],
         element="#certs", wait="#pf .pf-t",
         alt="The card 'Certificates' of the Configuration tab. The section 'HTTPS certificate "
             "of this page' shows the self-signed certificate of the first boot: its source, "
             "subject, SANs, last day and SHA-256 fingerprint, with the buttons 'Load a "
             "certificate…' and 'Make a new self-signed certificate'. The section 'Files for "
             "MQTT TLS and firmware updates' lists ca.pem (used by MQTT · CA file and Firmware "
             "updates · CA file), gw01.crt (MQTT · Client certificate) and gw01.key (MQTT · "
             "Client key) with their content, subject and last day. Each file has Replace, and "
             "Remove is off because the configuration uses the file. Below: the button 'Load a "
             "file…'."),
    Shot("pem-replace-dialog", "pem", DESKTOP, "/", actions=[pem_replace],
         wait="#pf-ex .exam b.ok",
         alt="The dialog 'Replace gw01.crt and gw01.key': the new certificate gw01-2027.crt and "
             "the new key gw01-2027.key are chosen. The gateway examined the files: a "
             "certificate CN=GW13 and a private key EC P-256 that agrees with it. The text says "
             "that the names stay gw01.crt and gw01.key, and that the MQTT connection uses the "
             "new files after the next restart. It has the button 'Replace the files' and "
             "Cancel."),
    # #206: docs/reference/configuration.md (the state "named": the section gateway)
    Shot("gateway-name", "named", DESKTOP, "/", actions=[gateway_card], wait="#f-gw input",
         alt="The top of the Configuration tab. The header shows 'Pump panel 2 · Plant 2, hall B', "
             "and below it the name of the product and Plant1 / GW01. The card Gateway has the "
             "fields Name, Location and Comment."),
    # #212 and #207: docs/reference/status-signals.md
    Shot("status-gateway-card", "named", DESKTOP, "/", element=".card:has(#st-dev)",
         wait="#st-dev div",
         alt="The card Gateway of the Status tab: the firmware, the reset reason, the uptime, the "
             "mode, the configuration, the free internal heap, the largest free internal block, "
             "the free PSRAM and the chip temperature (46.5 °C). Below the values: the button "
             "Identify and the text 'For 10 s, the status LED blinks and the buzzer beeps.'"),
    # #209 and #203: docs/how-to/restart-into-safe-mode-or-reset.md
    Shot("restart-reset-card", "named", DESKTOP, "/", element="#rr-card", wait="#rr-card",
         alt="The card 'Restart and reset' at the bottom of the Status tab: the buttons Restart "
             "and 'Restart into safe mode', the text about safe mode, and a red box Factory "
             "reset with its button."),
    Shot("safe-mode-dialog", "named", DESKTOP, "/", actions=[ask_safe_mode], wait="#dlg-ok",
         alt="The confirmation 'Restart into safe mode?': the gateway Pump panel 2, Plant1 / "
             "GW01, its address and firmware. In safe mode the gateway does not poll and does "
             "not connect to MQTT, and the hosts show the tags as stale. Only the network, the "
             "web UI, the shell and the firmware upload operate, and the access point "
             "GW-Setup-000001 starts. The configuration stays. Safe mode ends after 60 min, or "
             "with a restart. It has Cancel and the red button 'Restart into safe mode'."),
    Shot("factory-reset-dialog", "named", DESKTOP, "/", actions=[ask_factory_reset],
         wait="#dlg-ok:enabled",
         alt="The confirmation 'Do a factory reset of the gateway?': the list of what the reset "
             "erases (the configuration, the secrets, the certificate of the web UI, the web "
             "password and the pulse counters of the DIs; the restart also sets the Modbus "
             "counters to 0), the restart in commissioning mode, the access point and the new "
             "certificate. A link exports the configuration first. "
             "The field has ERASE, and the red button 'Erase and restart' is enabled."),
    # #200 and #199: docs/how-to/read-the-live-log.md
    Shot("live-log", "named", DESKTOP, "/", actions=[live_log],
         wait="#lg-lines .ln",
         freeze={"log_levels": [{"tag": "poll", "level": "debug"}, {"tag": "mb", "level": "debug"}]},
         alt="The card Log of the Status tab, open: the buttons Pause and Copy, 'Live: new lines "
             "every 2 s', 212 lines in the buffer (16 KB), and the last log lines (the warnings "
             "in brown, the errors in red, the debug lines in grey). Below: the tag poll and "
             "the log level debug, the button 'Set the level', and the line 'Changed levels (not "
             "saved)' with the button 'Reset all'."),
    # #209: the last shot of the state "named": the restart into safe mode changes the state
    Shot("safe-mode-status", "named", DESKTOP, "/", prep=safe_mode_restart, full_page=True,
         wait="#banner:not(.hidden)", freeze={"safe_mode.left_s": 3540},
         alt="The Status tab after a restart into safe mode: the red banner 'Safe mode on "
             "request: no polling and no MQTT. It ends in 59 min, or with a restart.' The mode "
             "is recovery (safe mode) and the configuration is not loaded. The access point "
             "GW-Setup-000001 is on, the firmware card gives safe mode on request, and in the "
             "card Restart and reset the button 'Restart into safe mode' is disabled."),
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

    def boot(self, name, cfg, files=None):
        """Start the emulator on a copy of the image with @cfg as config.json, and @files
        ({name: bytes}) beside it in LittleFS."""
        fs = {"config.json": json.dumps(cfg, indent=2).encode() + b"\n", **(files or {})}
        flash = e2e.flash_variant(self.args, self.work, name, fs)
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


def fetch(route):
    """route.fetch() for the gateway. It sends the request from the Playwright driver, which does
    not know the --host-resolver-rules of the browser: send it to 127.0.0.1 with the Host header
    of the page (the CSRF check compares the Origin with the Host)."""
    url = route.request.url
    if f"//{DOCS_HOST}:" not in url:
        return route.fetch()
    return route.fetch(url=url.replace(f"//{DOCS_HOST}:", "//127.0.0.1:", 1),
                       headers={**route.request.headers, "host": url.split("/")[2]})


def route_freeze(route, extra=None):
    """Answer an API request with the real answer of the gateway, with FREEZE applied (and
    @extra, the freeze of the shot, for /api/status)."""
    path = re.sub(r"\?.*", "", route.request.url.split("//", 1)[1].split("/", 1)[-1])
    paths = FREEZE.get("/" + path)
    if extra and path == "api/status":
        paths = {**paths, **extra}
    try:
        resp = fetch(route)
        body = resp.body()
    except Exception:   # noqa: BLE001 (a request of the page after the capture closed the context)
        return
    if not paths or "json" not in resp.headers.get("content-type", ""):
        route.fulfill(response=resp)
        return
    try:
        doc = json.loads(body)
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
        ctx.route(re.compile(r".*/api/.*"), lambda r: route_freeze(r, shot.freeze))
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
        # the Configuration tab reads the certificates and the PEM files after it shows (#205
        # #221); the load state networkidle came before. Wait for them: the card Certificates,
        # the file fields of the cards MQTT and Firmware updates and the card summaries use them
        page.wait_for_function("(t => !t || t.classList.contains('hidden') ||"
                               " (document.getElementById('wc-load') &&"
                               " document.getElementById('pf-load')))"
                               "(document.getElementById('tab-config'))")
        if shot.wait:
            page.wait_for_selector(shot.wait, state="visible")
        # the bar of the Configuration tab ("1 change, not saved") and the card summaries follow
        # an input after 150 ms (#191, #222): let them show the state of the page
        page.wait_for_timeout(400)
        opts = dict(animations="disabled", caret="hide", type="png")
        if shot.element:
            # the bar sticks to the bottom of the window, also over a card that is taller than the
            # window: keep it out of the image of a card
            page.add_style_tag(content="#bar{visibility:hidden}")
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


def token_colours():
    """The colours of the CSS tokens of the pages (--name:#rrggbb in www/*.html), as RGB tuples."""
    found = set()
    for page in (HERE.parent / "components" / "gw_web" / "www").glob("*.html"):
        text = page.read_text(encoding="utf-8")
        for h in re.findall(r"--[a-z]+:#([0-9a-fA-F]{6}|[0-9a-fA-F]{3})\b", text):
            h = "".join(c * 2 for c in h) if len(h) == 3 else h
            found.add(tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)))
    return sorted(found)


def write_png(png, out, lossless):
    """Write @png to @out, reduced to 256 colours unless @lossless (the UI has few colours), only
    when its pixels differ from the file there. Returns (written, size in bytes)."""
    from PIL import Image, ImageChops
    img = Image.open(io.BytesIO(png)).convert("RGB")
    if not lossless:
        # kmeans: without it, the median cut of a large page drops the small colours (the green
        # of "online" and "connected" became grey). A small mark of a token colour (the green dot
        # "Live" of the card Log, 8 px) still became grey: the palette also holds each token
        # colour of the pages exactly
        tokens = token_colours()
        base = img.quantize(colors=256 - len(tokens), method=Image.Quantize.MEDIANCUT,
                            dither=Image.Dither.NONE, kmeans=1)
        first = 256 - len(tokens)
        pal = base.getpalette()[:3 * first]
        base.putpalette(pal + [0] * (3 * first - len(pal)) + [v for c in tokens for v in c])
        # each pixel of a token colour gets the index of that colour
        for k, c in enumerate(tokens):
            diff = ImageChops.difference(img, Image.new("RGB", img.size, c)).split()
            same = ImageChops.lighter(ImageChops.lighter(diff[0], diff[1]), diff[2])
            base.paste(first + k, mask=same.point(lambda v: 255 if v == 0 else 0))
        img = base
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
            browser = pw.chromium.launch(args=[f"--host-resolver-rules=MAP {DOCS_HOST} 127.0.0.1"])
            for s in shots:
                auth = h.enter(s.state)
                if s.prep:
                    s.prep(h.gw)
                host = "127.0.0.1" if s.state in IP_STATES else DOCS_HOST
                png = capture(browser, s, auth, f"https://{host}:{e2e.HTTPS_PORT}")
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
