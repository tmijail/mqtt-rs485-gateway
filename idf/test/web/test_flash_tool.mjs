// Issue #219: the unit test of the logic of the install tool on the documentation site
// (docs/flash/): the parser of the access data (#280), the reader of the board with its times, the
// list of the releases and the manifest, and MD5. The DOM, Web Serial and esptool-js are not here:
// docs/how-to/build-the-documentation-site.md tells how to examine the page in a browser.
// Run it from any directory with Node.js 20 or later: node --test idf/test/web/test_flash_tool.mjs
// The CI job host-tests runs it after ESLint.
import { test } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";

import { AccessCollector, classify, noAddressReason, passwordSet, bestUrl } from "../../../docs/flash/access.js";
import { BoardReader, TIMES } from "../../../docs/flash/reader.js";
import {
    readIndex, checkManifest, integrityOf, compareVersions, firmwareNote, olderThanBoard, esptoolCommand,
} from "../../../docs/flash/releases.js";
import { md5Hex } from "../../../docs/flash/md5.js";

// Answers as gw_access_format() writes them (idf/components/gw_core/src/gw_access.c).
const COMMISSIONING = `access-data v1
mode commissioning
firmware 0.2.0
hostname gw-a1b2c3
web on https 443
login commissioning
user admin
commissioning-password k7Qm2xPa9TbE
fingerprint 4F:9A:21:C3
if eth up 192.168.1.57
if wifi off
if ap off
url https://192.168.1.57/
url https://gw-a1b2c3.local/
access-data end
`;
const NO_ADDRESS = `access-data v1
mode normal
firmware 0.2.0
hostname gw-a1b2c3
web on https 8443
login web-password
user admin
fingerprint 4F:9A:21:C3
if eth no-link
if wifi off
if ap off
url-none no-address
access-data end
`;
const WEB_OFF = `access-data v1
mode normal
firmware 0.2.0
web off
login web-password
user admin
if eth no-link
if wifi off
if ap off
url-none web-off
access-data end
`;
const STARTING = `access-data v1
mode starting
firmware 0.2.0
web off
login commissioning
user admin
if eth off
if wifi off
if ap off
url-none web-off
access-data end
`;

function collect(text) {
    const c = new AccessCollector();
    let r = null;
    for (const line of text.split("\n")) {
        r = c.push(line) || r;
    }
    return r;
}

test("the parser reads an answer in commissioning mode", () => {
    const r = collect(COMMISSIONING);
    assert.equal(r.ok, true);
    const d = r.data;
    assert.equal(d.mode, "commissioning");
    assert.equal(d.firmware, "0.2.0");
    assert.equal(d.hostname, "gw-a1b2c3");
    assert.deepEqual(d.web, { on: true, scheme: "https", port: 443 });
    assert.equal(d.login, "commissioning");
    assert.equal(d.user, "admin");
    assert.equal(d.commissioningPassword, "k7Qm2xPa9TbE");
    assert.equal(d.fingerprint, "4F:9A:21:C3");
    assert.deepEqual(d.ifs.eth, { state: "up", ip: "192.168.1.57" });
    assert.deepEqual(d.urls, ["https://192.168.1.57/", "https://gw-a1b2c3.local/"]);
    assert.equal(classify(d), "ready");
    assert.equal(passwordSet(d), false);
    assert.equal(bestUrl(d), "https://192.168.1.57/");
});

test("the parser gives the states no address, web UI off and starts", () => {
    const na = collect(NO_ADDRESS).data;
    assert.equal(classify(na), "noaddr");
    assert.equal(passwordSet(na), true);
    assert.equal(na.commissioningPassword, null);
    assert.equal(noAddressReason(na), "The Ethernet link is down.");
    assert.equal(classify(collect(WEB_OFF).data), "weboff");
    assert.equal(classify(collect(STARTING).data), "starting");
});

test("the parser accepts the prompt, CR LF, colors and log lines around the answer", () => {
    const noisy = "I (100) web: something\r\ngw> access\r\ngw> " +
        COMMISSIONING.replace(/\n/g, "\r\n").replace("mode commissioning", "\x1b[0mmode commissioning") +
        "\x1b[0;32mI (200) poll: Meter1 online\x1b[0m\r\n";
    const r = collect(noisy);
    assert.equal(r.ok, true);
    assert.equal(r.data.mode, "commissioning");
});

test("the parser ignores an unknown key and refuses an unknown version or a broken answer", () => {
    const extra = COMMISSIONING.replace("if wifi off", "if wifi off\nnew-key some value");
    assert.equal(collect(extra).ok, true);
    assert.deepEqual(collect(COMMISSIONING.replace("access-data v1", "access-data v2")),
        { ok: false, error: "version", version: 2 });
    assert.deepEqual(collect("access-data v1\nmode normal\naccess-data end\n"), { ok: false, error: "incomplete" });
    const long = "access-data v1\n" + "x y\n".repeat(50) + "access-data end\n";
    assert.deepEqual(collect(long), { ok: false, error: "too-long" });
});

// The reader with a fake clock.
function reader() {
    const sent = [];
    const changes = [];
    const r = new BoardReader({ send: (t) => sent.push(t), onChange: (x) => changes.push(x.phase) });
    return { r, sent, changes };
}

function run(r, from, to, step = 100) {
    for (let t = from; t <= to; t += step) {
        r.tick(t);
    }
}

test("a silent board gives no answer after 5 s, and the reader asks for the access data", () => {
    const { r, sent } = reader();
    run(r, 0, TIMES.QUIET - 100);
    assert.equal(r.phase, "reading");
    assert.ok(sent.length >= 3 && sent.every((s) => s === "access\r"));
    r.tick(TIMES.QUIET);
    assert.equal(r.phase, "noanswer");
    assert.equal(r.reason, "silent");
});

test("a board with another firmware gives no answer after 5 s", () => {
    const { r } = reader();
    r.feed("Some other firmware 1.2\r\n", 50);
    assert.equal(r.log, "other");
    run(r, 100, TIMES.QUIET);
    assert.equal(r.phase, "noanswer");
    assert.equal(r.reason, "other");
});

test("a board in the download mode gives no answer at once", () => {
    const { r } = reader();
    r.feed("ESP-ROM:esp32s3-20210327\r\nrst:0x1 (POWERON),boot:0x0 (DOWNLOAD(USB/UART0))\r\nwaiting for download\r\n", 200);
    assert.equal(r.phase, "noanswer");
    assert.equal(r.reason, "download");
});

test("the start log of this firmware makes the reader wait up to 30 s", () => {
    const { r } = reader();
    r.feed("I (412) app_init: Project name:     rs485_spb_gateway\r\n", 1000);
    assert.equal(r.log, "ours");
    run(r, 1100, 1000 + TIMES.START - 100);
    assert.equal(r.phase, "reading");
    r.tick(1000 + TIMES.START);
    assert.equal(r.phase, "noanswer");
    assert.equal(r.reason, "ours-silent");
});

test("an answer in the start waits for the final state", () => {
    const { r } = reader();
    r.feed("gw> " + STARTING, 1500);
    assert.equal(r.phase, "reading");
    assert.equal(r.state, "starting");
    r.feed(COMMISSIONING, 4000);
    assert.equal(r.phase, "answered");
    assert.equal(r.state, "ready");
    assert.equal(r.late, false);
});

test("a gateway that only starts after 30 s shows the start state", () => {
    const { r } = reader();
    r.feed(STARTING, 1000);
    run(r, 1100, 1000 + TIMES.START);
    assert.equal(r.phase, "answered");
    assert.equal(r.state, "starting");
});

test("a late answer after no answer shows the panel with the late note", () => {
    const { r, sent } = reader();
    run(r, 0, TIMES.QUIET);
    assert.equal(r.phase, "noanswer");
    const before = sent.length;
    run(r, TIMES.QUIET + 100, TIMES.QUIET + TIMES.POLL + 1000);
    assert.ok(sent.length > before, "the reader asks again after no answer");
    r.feed(COMMISSIONING, 12000);
    assert.equal(r.phase, "answered");
    assert.equal(r.late, true);
});

test("a restart of the board makes the reader read it again", () => {
    const { r, changes } = reader();
    r.feed(COMMISSIONING, 1000);
    assert.equal(r.phase, "answered");
    r.feed("ESP-ROM:esp32s3-20210327\r\n", 20000);
    assert.equal(r.phase, "reading");
    assert.equal(r.data, null);
    assert.ok(changes.includes("answered"));
});

test("a final state asks no more; no address asks each 5 s", () => {
    const a = reader();
    a.r.feed(COMMISSIONING, 1000);
    const n = a.sent.length;
    run(a.r, 1100, 30000);
    assert.equal(a.sent.length, n);
    const b = reader();
    b.r.feed(NO_ADDRESS, 1000);
    const m = b.sent.length;
    run(b.r, 1100, 1000 + 3 * TIMES.POLL + 100);
    assert.equal(b.sent.length - m, 3);
});

test("the reader does not ask while the asks are off (the console is busy)", () => {
    const { r, sent } = reader();
    r.setAsking(false, 0);
    run(r, 0, 4000);
    assert.equal(sent.length, 0);
});

test("the list of the releases keeps only valid tags, newest first by number", () => {
    const r = readIndex({
        format: 1, releases: [
            { tag: "v0.9.0", version: "0.9.0", manifest: "v0.9.0/manifest.json" },
            { tag: "v0.10.0", version: "0.10.0", manifest: "v0.10.0/manifest.json" },
            { tag: "v1.0", version: "1.0", manifest: "v1.0/manifest.json" },
            { tag: "v0.3.0", version: "0.3.0", manifest: "../x/manifest.json" },
            { tag: "v0.4.0", version: "0.4.0", manifest: "https://evil.example/m.json" },
        ],
    });
    assert.deepEqual(r.releases.map((x) => x.tag), ["v0.10.0", "v0.9.0"]);
    assert.ok(readIndex({ format: 2, releases: [] }).error);
    assert.deepEqual(readIndex({ format: 1, releases: [] }).releases, []);
});

const MANIFEST = {
    format: 1, name: "rs485_spb_gateway", version: "0.2.0", chip: "ESP32-S3",
    flash: { mode: "dio", freq: "80m", size: "16MB" },
    parts: [
        { name: "app", file: "rs485_spb_gateway.bin", offset: 131072, size: 1200000, sha256: "a".repeat(64), partition: "ota_0" },
        { name: "bootloader", file: "bootloader.bin", offset: 0, size: 21000, sha256: "b".repeat(64), partition: null },
        { name: "partition-table", file: "partition-table.bin", offset: 32768, size: 3072, sha256: "c".repeat(64), partition: null },
        { name: "otadata", file: "ota_data_initial.bin", offset: 61440, size: 8192, sha256: "d".repeat(64), partition: "otadata" },
    ],
    keep: [
        { name: "nvs", offset: 36864, size: 24576 },
        { name: "storage", offset: 8519680, size: 1048576 },
    ],
};

test("the manifest check accepts a release and sorts the parts by offset", () => {
    const r = checkManifest(MANIFEST, "0.2.0");
    assert.equal(r.errors, undefined);
    assert.deepEqual(r.manifest.parts.map((p) => p.offset), [0, 32768, 61440, 131072]);
    assert.equal(esptoolCommand(r.manifest),
        "python -m esptool --chip esp32s3 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB " +
        "0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x20000 rs485_spb_gateway.bin");
});

test("the manifest check refuses a wrong format, chip, version, path and an overlap with a kept partition", () => {
    assert.ok(checkManifest({ ...MANIFEST, format: 2 }).errors);
    assert.ok(checkManifest({ ...MANIFEST, chip: "ESP32" }).errors);
    assert.ok(checkManifest(MANIFEST, "0.3.0").errors);
    const path = structuredClone(MANIFEST);
    path.parts[0].file = "../x.bin";
    assert.ok(checkManifest(path).errors);
    const over = structuredClone(MANIFEST);
    over.parts[3].size = 8192 + 4096 * 3;   // otadata (0xf000) then runs into phy_init (0x11000)
    over.keep.push({ name: "phy_init", offset: 69632, size: 4096 });
    assert.match(checkManifest(over).errors.join(" "), /writes into phy_init/);
    const three = structuredClone(MANIFEST);
    three.parts.pop();
    assert.ok(checkManifest(three).errors);
});

test("integrity, version compare, firmware note and the older-release check", () => {
    const hex = createHash("sha256").update("abc").digest("hex");
    assert.equal(integrityOf(hex), "sha256-" + createHash("sha256").update("abc").digest("base64"));
    assert.ok(compareVersions("0.10.0", "0.9.9") > 0);
    assert.equal(compareVersions("0.2.0+g1234abcd", "0.2.0"), 0);
    const latest = { version: "0.3.0" };
    assert.equal(firmwareNote("0.3.0", latest), "This is the last release.");
    assert.equal(firmwareNote("0.2.0", latest), "The last release is 0.3.0.");
    assert.equal(firmwareNote("0.3.0+g1234abcd.dirty", latest), "A build that is not a release.");
    assert.equal(olderThanBoard({ version: "0.2.0" }, "0.3.0"), true);
    assert.equal(olderThanBoard({ version: "0.3.0" }, "0.3.0"), false);
});

test("MD5 agrees with node:crypto", () => {
    for (const n of [0, 1, 55, 56, 63, 64, 65, 1000, 100003]) {
        const data = new Uint8Array(n).map((_, i) => (i * 31 + 7) & 0xff);
        assert.equal(md5Hex(data), createHash("md5").update(data).digest("hex"), `length ${n}`);
    }
});
