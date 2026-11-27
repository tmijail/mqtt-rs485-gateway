// Issue #219: the install tool of the documentation site (direction C of the prototype, approved
// on 2026-11-22): connect first, then the page reads the board and shows one panel with its state
// and the next action. The logic without the DOM is in access.js, reader.js and releases.js (unit
// test: idf/test/web/test_flash_tool.mjs). boot.js loads this module only on the page that has
// the element #gw-flash (docs/how-to/flash-the-firmware-from-the-browser.md).

import { BoardReader } from "./reader.js";
import { bestUrl, noAddressReason, passwordSet } from "./access.js";
import { readIndex, checkManifest, esptoolCommand, firmwareNote, olderThanBoard } from "./releases.js";
import { SerialLink, BAUD, portLabel } from "./serial.js";
import { fetchParts, flashParts } from "./flasher.js";

const LOG_MAX = 2000;
const TICK = 250;

function esc(v) {
    return String(v ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
}

function siteBase() {
    try {
        const cfg = JSON.parse(document.getElementById("__config").textContent);
        return new URL(cfg.base + "/", location.href);
    } catch (e) {
        return new URL("../../", location.href);
    }
}

function warn(title, body) {
    return `<div class="admonition warning"><p class="admonition-title">${title}</p>${body ? `<p>${body}</p>` : ""}</div>`;
}

const CHECK = { ok: ["gwf-ok", "✓"], no: ["gwf-no", "✗"], wait: ["gwf-wait", "…"] };
function check(kind, label, info) {
    const [cls, mark] = CHECK[kind];
    return `<div class="gwf-chk"><span class="gwf-mark ${cls}" aria-hidden="true">${mark}</span><span>${label}</span><span class="gwf-mut">${esc(info)}</span></div>`;
}

export function mount(root) {
    const base = siteBase();
    const doc = (path) => new URL(path, base).href;
    const S = {
        view: "serial" in navigator ? "start" : "noserial",
        msg: null,
        releases: null, releaseError: null, sel: 0, manifests: {},
        port: null, link: null, portName: "", cableOut: false,
        tab: "board", board: "reader",
        setup: { kind: null, loads: false },
        flash: null, fail: null, pw: null, notice: null, webOpened: false, copied: "",
        log: [],
    };
    let reader = null;
    let timer = null;
    let alive = true;

    root.innerHTML = `<div class="gwf-main"></div><div class="gwf-console" hidden>
<div class="gwf-cols"><div class="gwf-col3"><pre class="gwf-log" aria-live="off"></pre>
<form class="gwf-row gwf-cmd"><input type="text" aria-label="Command" placeholder="Command, for example status" autocomplete="off" spellcheck="false">
<button type="submit" class="gwf-btn gwf-pri">Send</button><button type="button" class="gwf-btn" data-act="clear">Clear</button></form></div>
<div class="gwf-col2"><div class="gwf-box"><b>Usual commands</b><p class="gwf-mut">A click puts the command in the field.</p>
<div class="gwf-row gwf-wrap">${["status", "access", "config show", "devices", "help"].map((c) => `<button type="button" class="gwf-btn gwf-mono" data-act="fill" data-arg="${c}">${c}</button>`).join("")}</div></div>
<div class="gwf-box"><b>Nobody knows the web password?</b><p>The page can remove it. The configuration stays.</p>
<button type="button" class="gwf-btn" data-act="pwask">Reset the web password</button>
<p><a href="${doc("how-to/recover-the-gateway/")}">Recover the gateway</a></p></div>
<p class="gwf-mut">The page and the console use the same port. After a restart, the page reads the board again.</p></div></div></div>`;
    const main = root.querySelector(".gwf-main");
    const consoleEl = root.querySelector(".gwf-console");
    const logEl = root.querySelector(".gwf-log");
    const cmdForm = root.querySelector(".gwf-cmd");
    const cmdInput = cmdForm.querySelector("input");

    // ---------- releases ----------
    async function loadReleases() {
        try {
            const res = await fetch(new URL("firmware/index.json", base), { cache: "no-cache" });
            if (!res.ok) {
                throw new Error(`HTTP ${res.status}`);
            }
            const r = readIndex(await res.json());
            S.releases = r.releases || [];
            S.releaseError = r.error || null;
        } catch (e) {
            S.releases = [];
            S.releaseError = "This site has no list of releases. A local build of the site has none.";
        }
        render();
        if (S.releases.length > 0) {
            loadManifest(S.releases[0]);
        }
    }

    function release() {
        return S.releases && S.releases.length > 0 ? S.releases[S.sel] : null;
    }

    async function loadManifest(rel) {
        if (!rel || S.manifests[rel.tag]) {
            return;
        }
        const url = new URL(rel.manifest, new URL("firmware/index.json", base));
        S.manifests[rel.tag] = { loading: true, url };
        try {
            const res = await fetch(url, { cache: "no-cache" });
            if (!res.ok) {
                throw new Error(`HTTP ${res.status}`);
            }
            const c = checkManifest(await res.json(), rel.version);
            S.manifests[rel.tag] = c.errors ? { errors: c.errors, url } : { manifest: c.manifest, url };
        } catch (e) {
            S.manifests[rel.tag] = { errors: [`The page could not get the manifest: ${e.message}.`], url };
        }
        render();
    }

    function manifestOf(rel) {
        return rel ? S.manifests[rel.tag] : null;
    }

    // ---------- the port ----------
    function now() {
        return performance.now();
    }

    function consoleText(line, cls) {
        S.log.push(line);
        const span = document.createElement("span");
        if (cls) {
            span.className = cls;
        }
        span.textContent = line + "\n";
        logEl.appendChild(span);
        while (logEl.childNodes.length > LOG_MAX) {
            logEl.removeChild(logEl.firstChild);
            S.log.shift();
        }
        if (!consoleEl.hidden) {
            logEl.scrollTop = logEl.scrollHeight;
        }
        if (S.pw && S.pw.waiting) {
            pwLine(line);
        }
        const last = root.querySelector(".gwf-last span");
        if (last) {
            last.textContent = line;
        }
    }

    async function openLink() {
        const link = new SerialLink(S.port, {
            onText: (t) => reader && reader.feed(t, now()),
            onEnd: () => {
                if (S.link === link) {
                    S.cableOut = true;
                    render();
                }
            },
        });
        await link.open();
        S.link = link;
        S.cableOut = false;
        reader = new BoardReader({
            send: (t) => link.write(t).catch(() => {}),
            onChange: () => render(),
            onLine: (l) => consoleText(l),
        });
        reader.reset(now());
        if (!timer) {
            timer = setInterval(tick, TICK);
        }
    }

    async function closeLink() {
        const link = S.link;
        S.link = null;
        if (link) {
            await link.close();
        }
    }

    function tick() {
        if (!alive) {
            return;
        }
        if (reader && S.link && S.board === "reader" && !S.cableOut) {
            reader.tick(now());
        }
        const bar = root.querySelector(".gwf-bar[data-read] i");
        if (bar && reader) {
            bar.style.width = `${Math.round(reader.progress(now()) * 100)}%`;
        }
    }

    async function connect() {
        S.msg = null;
        let port;
        try {
            port = await navigator.serial.requestPort();
        } catch (e) {
            S.msg = "noport";
            render();
            return;
        }
        S.port = port;
        S.portName = portLabel(port);
        try {
            await openLink();
        } catch (e) {
            S.port = null;
            S.msg = "busy";
            render();
            return;
        }
        S.view = "panel";
        S.tab = "board";
        S.board = "reader";
        S.notice = null;
        render();
    }

    async function disconnect() {
        await closeLink();
        reader = null;
        S.port = null;
        S.view = "start";
        S.cableOut = false;
        render();
    }

    async function selectOther() {
        await closeLink();
        reader = null;
        S.port = null;
        await connect();
        if (!S.port) {
            S.view = "start";
            render();
        }
    }

    function sameDevice(port) {
        if (!S.port || !port) {
            return false;
        }
        if (port === S.port) {
            return true;
        }
        const a = S.port.getInfo();
        const b = port.getInfo();
        return a.usbVendorId !== undefined && a.usbVendorId === b.usbVendorId && a.usbProductId === b.usbProductId;
    }

    async function onSerialConnect(ev) {
        if (!S.cableOut || !sameDevice(ev.target) || S.board === "flashing") {
            return;
        }
        S.port = ev.target;
        await closeLink();
        try {
            await openLink();
            S.board = "reader";
            render();
        } catch (e) {
            // The device is not ready yet: the next event or a click on Retry opens it.
        }
    }

    function onSerialDisconnect(ev) {
        if (sameDevice(ev.target) && S.board !== "flashing") {
            S.cableOut = true;
            render();
        }
    }

    if ("serial" in navigator) {
        navigator.serial.addEventListener("connect", onSerialConnect);
        navigator.serial.addEventListener("disconnect", onSerialDisconnect);
    }

    async function retry() {
        S.notice = null;
        S.board = "reader";
        if (S.cableOut || !S.link) {
            await closeLink();
            try {
                await openLink();
            } catch (e) {
                S.cableOut = true;
            }
        } else {
            reader.restart(now());
        }
        render();
    }

    // ---------- web password ----------
    function pwAsk() {
        S.tab = "board";
        S.board = "pwreset";
        render();
    }

    async function pwDo() {
        S.pw = { waiting: true };
        S.board = "pwbusy";
        reader.setAsking(false, now());
        render();
        await S.link.write("web-password remove\r");
        S.pw.timer = setTimeout(() => {
            if (S.pw && S.pw.waiting) {
                pwEnd("none");
            }
        }, 8000);
    }

    function pwLine(line) {
        if (/web-password: removed/.test(line)) {
            pwEnd("removed");
        } else if (/web-password: no web password is set/.test(line)) {
            pwEnd("noneset");
        } else if (/web-password: REFUSED/.test(line)) {
            pwEnd("trial");
        } else if (/web-password: NOT removed: (.*)$/.test(line)) {
            pwEnd("error", /NOT removed: (.*)$/.exec(line)[1]);
        } else if (/Unrecognized command|usage: web-password/.test(line)) {
            pwEnd("unknown");
        }
    }

    function pwEnd(result, detail) {
        clearTimeout(S.pw && S.pw.timer);
        S.pw = { result, detail };
        reader.setAsking(true, now());
        if (result === "removed" || result === "noneset") {
            S.notice = result;
            S.board = "reader";
            reader.restart(now());   // "removed": the gateway restarts, and the reader waits for it
        } else {
            S.board = "pwresult";
        }
        render();
    }

    // ---------- the flash ----------
    function openSetup(kind) {
        S.tab = "board";
        S.board = "setup";
        S.setup = { kind: kind || null, loads: S.setup.loads };
        loadManifest(release());
        render();
    }

    async function runFlash() {
        const rel = release();
        const m = manifestOf(rel);
        if (!rel || !m || !m.manifest || !S.setup.kind) {
            return;
        }
        const erase = S.setup.kind === "install";
        S.flash = { kind: S.setup.kind, rel, stage: "download", pct: 0, n: 0, erase };
        S.fail = null;
        S.board = "flashing";
        reader.setAsking(false, now());
        render();
        let parts;
        try {
            parts = await fetchParts(m.url, m.manifest, () => {
                S.flash.n++;
                render();
            });
        } catch (e) {
            flashFailed(e);
            return;
        }
        await closeLink();
        consoleText(`--- the page flashes release ${rel.version} (${erase ? "Install: erase all" : "Repair: no erase"}) ---`, "gwf-me");
        try {
            await flashParts({
                port: S.port, parts, erase,
                onStep: (stage) => {
                    S.flash.stage = stage;
                    render();
                },
                onProgress: (p) => {
                    S.flash.pct = p;
                    const bar = root.querySelector(".gwf-bar[data-flash] i");
                    const info = root.querySelector("[data-pct]");
                    if (bar) {
                        bar.style.width = `${Math.round(p * 100)}%`;
                    }
                    if (info) {
                        info.textContent = `${Math.round(p * 100)} %`;
                    }
                },
                log: (t) => t.split("\n").filter((l) => l.trim() !== "").forEach((l) => consoleText(l, "gwf-me")),
            });
        } catch (e) {
            await reopenAfterFlash();
            flashFailed(e);
            return;
        }
        S.notice = erase ? "installed" : "repaired";
        await reopenAfterFlash();
        S.board = "reader";
        render();
    }

    async function reopenAfterFlash() {
        try {
            await openLink();
        } catch (e) {
            S.cableOut = true;
        }
    }

    function flashFailed(e) {
        S.fail = { stage: e.stage || "write", detail: e.detail || e.message };
        if (reader) {
            reader.setAsking(true, now());
        }
        S.board = "fail";
        render();
    }

    // ---------- copy ----------
    async function copy(text, what) {
        try {
            await navigator.clipboard.writeText(text);
            S.copied = `${what} is in the clipboard.`;
        } catch (e) {
            S.copied = "The browser did not permit the copy. Select the text, then copy it.";
        }
        render();
    }

    // ---------- views ----------
    function filesBlock() {
        if (S.releases === null) {
            return `<p class="gwf-mut">The page reads the list of the releases.</p>`;
        }
        const rel = release();
        if (!rel) {
            return `<p>This site has no release yet. ${esc(S.releaseError || "")}</p>
<p>Build the firmware: <a href="${doc("how-to/build-the-firmware/")}">Build the firmware</a>.</p>`;
        }
        const m = manifestOf(rel);
        let body = `<p class="gwf-mut">The page reads the manifest of release ${esc(rel.version)}.</p>`;
        if (m && m.errors) {
            body = warn("The manifest of this release is not correct.", esc(m.errors.join(" ")));
        } else if (m && m.manifest) {
            const at = (f) => new URL(f, m.url).href;
            body = `<table class="gwf-files"><thead><tr><th>Offset</th><th>File</th><th>Size</th></tr></thead><tbody>
${m.manifest.parts.map((p) => `<tr><td class="gwf-mono">0x${p.offset.toString(16)}</td><td><a href="${esc(at(p.file))}" download>${esc(p.file)}</a></td><td>${p.size.toLocaleString("en")} bytes</td></tr>`).join("")}
</tbody></table>
<p><a href="${esc(at("manifest.json"))}" download>manifest.json</a> · <a href="${esc(at("SHA256SUMS"))}" download>SHA256SUMS</a></p>
<p>Flash with esptool from a computer:</p>
<pre class="gwf-pre"><code>${esc(esptoolCommand(m.manifest))}</code></pre>
<p>A new board: run <code>python -m esptool --chip esp32s3 erase-flash</code> first. The erase removes the configuration and the passwords.</p>
<p>Disconnect the loads from the outputs before the flash.</p>`;
        }
        return `<div class="gwf-row gwf-wrap"><b>Release</b>${versionButtons()}</div>${body}`;
    }

    function versionButtons() {
        return S.releases.map((r, i) => `<button type="button" class="gwf-seg${i === S.sel ? " gwf-on" : ""}" aria-pressed="${i === S.sel}" data-act="ver" data-arg="${i}">${esc(r.version)}${i === 0 ? " (last)" : ""}</button>`).join("");
    }

    function viewStart() {
        const msg = {
            noport: warn("No port was selected.", "If the list had no serial port: use a USB cable with data lines, because a cable for power only does not work. Install the driver of the USB chip of the board. Then click <b>Connect the board</b> again."),
            busy: warn("The page cannot open this port.", "Another program uses it, for example a serial monitor or another tab. Close the other program. Then click <b>Connect the board</b> again."),
        }[S.msg] || "";
        const noRel = S.releases && S.releases.length === 0
            ? `<p class="gwf-mut">This site has no release yet. The page can read a gateway, but it cannot flash.</p>` : "";
        return `<div class="gwf-box">
<h3>Connect the board</h3>
<ol><li>Connect the board to the computer with the USB-C cable.</li><li>Click <b>Connect the board</b>.</li><li>Select the serial port of the board in the list of the browser.</li></ol>
<div class="gwf-row gwf-wrap"><button type="button" class="gwf-btn gwf-pri" data-act="connect">Connect the board</button><span class="gwf-mut">Nothing goes to the flash of the board at this step.</span></div>
<p class="gwf-mut">The connection can restart a gateway that runs.</p>
${msg}${noRel}
<p><a href="#the-list-has-no-serial-port">The list has no serial port</a> · <a href="#gw-flash" data-act="files">Download the image files</a></p>
</div>
<div class="gwf-box gwf-fill"><b>Does the web UI of your gateway open?</b>
<p>Then do not flash over USB. Use the card <b>Firmware</b> of the web UI: it examines the image first, and it can go back to the earlier image. <a href="${doc("how-to/update-the-firmware/")}">Update the firmware</a></p></div>`;
    }

    function viewNoSerial() {
        return `${warn("This browser cannot use a serial port.", "Open this page in Chrome or Edge on a computer. A phone or a tablet cannot flash the board.")}
<div class="gwf-row gwf-wrap"><button type="button" class="gwf-btn" data-act="copylink">Copy the link of this page</button><span class="gwf-mut">${esc(S.copied)}</span></div>
<div class="gwf-box"><h3>The image files</h3>${filesBlock()}</div>`;
    }

    function viewFiles() {
        return `<div class="gwf-row"><button type="button" class="gwf-btn" data-act="back">Back</button></div>
<div class="gwf-box"><h3>The image files</h3>${filesBlock()}</div>`;
    }

    function header() {
        const d = reader && reader.phase === "answered" ? reader.data : null;
        let title = "Board on USB";
        let mono = `${S.portName} · ${BAUD}`;
        let right = "port open";
        if (d && S.board !== "flashing") {
            title = "Gateway on USB";
            mono = `${d.mode} mode · firmware ${d.firmware}`;
        }
        if (reader && reader.phase === "noanswer") {
            right = "port open · asks again each 5 s";
        }
        if (S.board === "flashing") {
            right = `flash of release ${S.flash.rel.version}`;
        }
        if (S.cableOut) {
            right = "cable disconnected";
        }
        const disc = S.board === "flashing" ? "" : `<button type="button" class="gwf-btn" data-act="disconnect">Disconnect</button>`;
        return `<div class="gwf-hdr"><b>${title}</b><span class="gwf-mono gwf-mut">${esc(mono)}</span><span class="gwf-grow"></span><span class="gwf-mut">${esc(right)}</span>${disc}</div>`;
    }

    function tabs() {
        if (S.board === "flashing") {
            return "";
        }
        const t = (id, label) => `<button type="button" role="tab" aria-selected="${S.tab === id}" class="gwf-tab${S.tab === id ? " gwf-on" : ""}" data-act="tab" data-arg="${id}">${label}</button>`;
        return `<div class="gwf-tabs" role="tablist">${t("board", "Board")}${t("console", "Console")}${t("files", "Files")}</div>`;
    }

    function viewReading() {
        const r = reader;
        const logInfo = { none: "listening", ours: "found: the gateway starts", other: "found: not the log of this firmware", download: "the download mode" }[r.log];
        const starting = r.data && r.state === "starting";
        return `<div class="gwf-cols"><div class="gwf-box gwf-col3">
<h3>The page reads the board</h3>
${check("ok", "Serial port", "open")}
${check(r.log === "ours" ? "ok" : "wait", "Start log", logInfo)}
${check("wait", "Gateway answer", starting ? "the gateway starts" : "asking")}
<div class="gwf-bar" data-read><i style="width:${Math.round(r.progress(now()) * 100)}%"></i></div>
<p class="gwf-mut">A board that does not answer takes about 5 s. A gateway that starts can take up to 30 s.</p>
</div><div class="gwf-box gwf-col2"><b>A new board?</b><p>A new board does not answer. You do not have to wait.</p>
<button type="button" class="gwf-btn" data-act="setup" data-arg="install">Install now</button></div></div>`;
    }

    function viewNoAnswer() {
        const r = reader;
        const texts = {
            silent: ["not found", "none in 5 s", "This is a new board, or a gateway whose firmware does not start.", "If this is not your board, select another port."],
            other: ["not the log of this firmware", "none in 5 s", "This is a new board, or a gateway whose firmware does not start.", "The board writes a log, but not the log of this firmware. If this is not your board, select another port."],
            download: ["the download mode", "none", "The board is in the download mode.", "It waits for a flash. To start its firmware, disconnect all the power, then connect it again without a push of BOOT."],
            "ours-silent": ["found", "none in 30 s", "This gateway starts, but it does not answer.", "Its firmware is older than this page, or it is not ready yet. Click Retry, or look in the console."],
            format: ["found", "a format that this page does not know", "The gateway answered in a format that this page does not know.", "The firmware is newer than this page. Load the page again."],
            garbled: ["found", "not complete", "The answer of the gateway was not complete.", "Click Retry."],
        }[r.reason] || ["", "", "", ""];
        const logOk = r.reason === "ours-silent" || r.reason === "format" || r.reason === "garbled";
        return `<h3>This board did not answer as a gateway</h3>
<div class="gwf-cols"><div class="gwf-box gwf-col3">
${check("ok", "Serial port", `open: ${S.portName}`)}
${check(logOk ? "ok" : "no", "Start log", texts[0])}
${check("no", "Gateway answer", texts[1])}
<hr><p><b>${texts[2]}</b></p><p class="gwf-mut">${texts[3]}</p></div>
<div class="gwf-box gwf-col2"><b class="gwf-mut">Recommended</b>
<button type="button" class="gwf-btn gwf-pri" data-act="setup">Install or repair</button><hr><b class="gwf-mut">Other actions</b>
<button type="button" class="gwf-btn" data-act="retry">Retry</button>
<button type="button" class="gwf-btn" data-act="other">Select another port</button>
<button type="button" class="gwf-btn" data-act="tab" data-arg="console">Open the console</button></div></div>
<p class="gwf-mut">If the web UI of this gateway opens, do not flash over USB. Use the card <b>Firmware</b> of the web UI. <a href="${doc("how-to/update-the-firmware/")}">Update the firmware</a></p>`;
    }

    function accessBox(d) {
        const url = bestUrl(d);
        const others = d.urls.slice(1);
        const user = d.login === "web-password-any-user" ? "Any user name" : (d.user || "admin");
        const pw = d.login === "commissioning"
            ? (d.commissioningPassword
                ? `<span class="gwf-stack"><span class="gwf-row gwf-mono">${esc(d.commissioningPassword)}<button type="button" class="gwf-cp" data-act="copypw">Copy</button></span><span class="gwf-mut">The commissioning password. It is valid until you set a web password.</span></span>`
                : `<span>The gateway gave no commissioning password. Look in the console.</span>`)
            : `<span>A web password is set. The gateway does not show it.</span>`;
        const cert = d.fingerprint
            ? `<span class="gwf-stack"><span class="gwf-mono gwf-break">${esc(d.fingerprint)}</span><span class="gwf-mut">SHA-256 fingerprint</span></span>`
            : `<span class="gwf-mut">${d.web.on && d.web.scheme === "http" ? "The web UI uses HTTP. It has no certificate." : "None"}</span>`;
        return `<div class="gwf-box"><b>Access data</b><div class="gwf-kv">
<span class="gwf-mut">Address</span><span class="gwf-stack">${url ? `<span class="gwf-row gwf-mono">${esc(url)}<button type="button" class="gwf-cp" data-act="copyaddr">Copy</button></span>` : `<span>None</span>`}${others.map((u) => `<span class="gwf-mono gwf-mut">${esc(u)}</span>`).join("")}</span>
<span class="gwf-mut">User</span><span class="gwf-mono">${esc(user)}</span>
<span class="gwf-mut">Password</span>${pw}
<span class="gwf-mut">Certificate</span>${cert}
</div><p class="gwf-mut" aria-live="polite">${esc(S.copied)}</p></div>`;
    }

    function firmwareBox(d) {
        const latest = S.releases && S.releases[0];
        return `<div class="gwf-box"><b>Firmware</b>
<div class="gwf-row gwf-wrap"><span class="gwf-mono">${esc(d.firmware)}</span><span class="gwf-mut">${esc(firmwareNote(d.firmware, latest))}</span></div>
<p class="gwf-mut">To update a gateway that runs, use the card <b>Firmware</b> of the web UI. It examines the image first, and it can go back. <a href="${doc("how-to/update-the-firmware/")}">Update the firmware</a></p>
<a href="#gw-flash" data-act="setup" data-arg="repair">The web UI does not open in any mode: repair with a USB flash</a></div>`;
    }

    function notices() {
        const n = {
            installed: ["The flash is complete.", "The flash erased all. The gateway starts in commissioning mode, with no configuration."],
            repaired: ["The flash is complete.", "The configuration and the passwords stayed."],
            removed: ["The web password is removed.", "Log in with the commissioning password of this panel. Then set a new web password in the web UI."],
            noneset: ["No web password is set.", "The commissioning password of this panel opens the web UI."],
        }[S.notice];
        let out = n ? `<div class="admonition success"><p class="admonition-title">${n[0]}</p><p>${n[1]}</p></div>` : "";
        if (reader && reader.late) {
            out += warn("The gateway answered late.", "Before the answer, this panel said \"no answer\" and recommended a flash. The gateway started all the same.");
        }
        return out;
    }

    function viewReady() {
        const d = reader.data;
        const st = reader.state;
        const pwSet = passwordSet(d);
        const latest = S.releases && S.releases[0];
        let heading;
        let sub = `Firmware ${d.firmware}. ${firmwareNote(d.firmware, latest)}`.trim();
        if (st === "noaddr") {
            heading = "The gateway has no address.";
            sub = `The gateway runs in ${d.mode} mode. ${noAddressReason(d)}`;
        } else if (d.mode === "recovery") {
            heading = "The gateway runs in recovery mode. It refused its configuration.";
        } else if (d.mode === "commissioning") {
            heading = "The gateway runs. It waits for its first configuration.";
        } else {
            heading = pwSet ? "The gateway runs. A web password is set." : "The gateway runs. No web password is set.";
        }
        let rec;
        if (st === "noaddr") {
            rec = `<b class="gwf-mut">Recommended</b><div class="gwf-rec"><b>Connect the Ethernet cable.</b><p>Make sure that the network has a DHCP server. No click is necessary: the page reads the board again each 5 s.</p></div>`;
        } else {
            const url = bestUrl(d);
            const https = d.web.scheme === "https";
            const steps = [];
            if (https) {
                steps.push("A new tab opens. The browser gives a warning about the certificate.",
                    "Open the details of the certificate.",
                    "Compare its SHA-256 fingerprint with this panel. Then accept the certificate.");
            }
            if (!pwSet) {
                steps.push("Log in with the user and the password of this panel.");
                if (d.mode === "commissioning") {
                    steps.push("In the tab Configuration, set the configuration. In the card Web interface, set a web password.",
                        "Click Validate, then click Save and restart.");
                }
            } else {
                steps.push(d.login === "web-password-any-user" ? "Log in with any user name and your web password." : `Log in with the user ${d.user} and your web password.`);
            }
            if (d.mode === "recovery") {
                steps.push("Correct the configuration in the tab Configuration. The card Gateway of the tab Status gives the error.");
            }
            rec = `<b class="gwf-mut">Recommended</b>
<a class="gwf-btn gwf-pri" href="${esc(url)}" target="_blank" rel="noopener" data-act="openweb">Open the web UI</a>
${S.webOpened ? `<p><b>The web UI opened in a new tab.</b> This panel stays here: compare the fingerprint, then log in.</p>` : ""}
<ol>${steps.map((s) => `<li>${esc(s)}</li>`).join("")}</ol>`;
        }
        const ap = st === "noaddr" ? `<p>Use the access point: push BOOT until you hear 1 beep (at 2 s), then release it. Release it before 10 s: a release after the 2 beeps at 10 s does a factory reset. <a href="${doc("how-to/get-on-the-access-point/")}">Get on the access point</a></p>` : "";
        return `<div class="gwf-head"><h3>${esc(heading)}</h3><p class="gwf-mut">${esc(sub)}</p></div>${notices()}
<div class="gwf-cols"><div class="gwf-col1">${accessBox(d)}${firmwareBox(d)}</div>
<div class="gwf-box gwf-col1">${rec}<hr><b class="gwf-mut">Other actions</b>${ap}
<div class="gwf-row gwf-wrap"><button type="button" class="gwf-btn" data-act="retry">Read again</button>${pwSet ? `<button type="button" class="gwf-btn" data-act="pwask">Reset the web password</button>` : ""}</div></div></div>`;
    }

    function viewWebOff() {
        const d = reader.data;
        return `<div class="gwf-head"><h3>The configuration turns the web UI off.</h3><p class="gwf-mut">The gateway runs firmware ${esc(d.firmware)} in ${esc(d.mode)} mode. Its web UI does not open.</p></div>${notices()}
<div class="gwf-cols"><div class="gwf-box gwf-col1"><b class="gwf-mut">Recommended</b>
<div class="gwf-rec"><b>Push BOOT until you hear 1 beep (at 2 s), then release it.</b><p>The gateway opens the access point and its web UI. Then the page reads the board again.</p></div>
${warn("Release BOOT before 10 s.", "At 10 s, the gateway gives 2 beeps. A release after the 2 beeps does a factory reset.")}</div>
<div class="gwf-box gwf-col1"><b class="gwf-mut">Other actions</b>
<button type="button" class="gwf-btn" data-act="tab" data-arg="console">Console: change the configuration</button>
<button type="button" class="gwf-btn" data-act="retry">Read again</button>
<a href="#gw-flash" data-act="setup" data-arg="repair">The gateway does not start: repair with a USB flash</a></div></div>`;
    }

    function viewProblem() {
        const d = reader.data;
        const fault = reader.state === "fault";
        const heading = fault ? "The gateway is in fault mode." : "The gateway did not complete its start in 30 s.";
        const sub = fault ? "It does not start the functions of the gateway. The console and the web UI give the fault." : "The console gives the reason.";
        return `<div class="gwf-head"><h3>${heading}</h3><p class="gwf-mut">${sub} Firmware ${esc(d.firmware)}.</p></div>${notices()}
<div class="gwf-cols"><div class="gwf-col1">${d.web.on ? accessBox(d) : ""}</div>
<div class="gwf-box gwf-col1"><b class="gwf-mut">Recommended</b>
<button type="button" class="gwf-btn gwf-pri" data-act="tab" data-arg="console">Open the console</button><hr><b class="gwf-mut">Other actions</b>
<button type="button" class="gwf-btn" data-act="retry">Read again</button>
${d.web.on && bestUrl(d) ? `<a href="${esc(bestUrl(d))}" target="_blank" rel="noopener">Open the web UI</a>` : ""}
<a href="#gw-flash" data-act="setup" data-arg="repair">Repair with a USB flash</a></div></div>`;
    }

    function viewPwReset() {
        return `<h3>Reset the web password</h3>
<div class="gwf-cols"><div class="gwf-box gwf-col3"><b>What this does</b><ol>
<li>The gateway removes the web password. The configuration and the other secrets stay.</li>
<li>The gateway restarts.</li><li>This panel shows the commissioning password. Log in with it.</li>
<li>Set a new web password in the web UI.</li></ol>
<p class="gwf-mut">Until you set the new web password, the commissioning password is also the Wi-Fi password of the access point.</p></div>
<div class="gwf-box gwf-col2">${warn("The gateway restarts.", "It stops the polling and the MQTT connection for some seconds.")}
<button type="button" class="gwf-btn gwf-pri" data-act="pwdo">Remove the web password</button>
<button type="button" class="gwf-btn" data-act="toboard">Cancel</button></div></div>`;
    }

    function viewPwBusy() {
        return `<h3>Reset the web password</h3><div class="gwf-box"><p>The page waits for the answer of the gateway.</p></div>`;
    }

    function viewPwResult() {
        const p = S.pw || {};
        const t = {
            trial: "The gateway refused: a new firmware is on trial. Wait for the end of the trial. Then try again.",
            error: `The gateway did not remove the web password: ${p.detail || ""}.`,
            unknown: "This gateway does not know the command. Its firmware is older than this page, or its shell has no commands that change the gateway.",
            none: "The gateway did not answer.",
        }[p.result] || "";
        return `<h3>Reset the web password</h3>${warn("The web password stays.", esc(t))}
<div class="gwf-row gwf-wrap"><button type="button" class="gwf-btn gwf-pri" data-act="retry">Back to the panel</button>
<button type="button" class="gwf-btn" data-act="tab" data-arg="console">Open the console</button>
<a href="${doc("how-to/recover-the-gateway/")}">Recover the gateway</a></div>`;
    }

    function viewSetup() {
        const rel = release();
        const m = manifestOf(rel);
        const k = S.setup.kind;
        const d = reader && reader.phase === "answered" ? reader.data : null;
        const answered = d !== null && reader.state !== "starting";
        const notes = [];
        if (k === "repair" && rel && (S.sel > 0 || (d && olderThanBoard(rel, d.firmware)))) {
            notes.push(warn("An earlier release can be unable to read the configuration of this gateway.", "A flash over USB does not examine this. Use an earlier release only with Install."));
        }
        if (k === "repair" && answered) {
            notes.push(`<div class="gwf-rec"><b>This gateway answered.</b> A repair is for a gateway that does not start. To update a gateway that runs, use the card <b>Firmware</b> of the web UI: it examines the image first, and it can go back.</div>`);
        }
        if (k === "install" && answered) {
            notes.push(warn("This board answered as a gateway.", "Install erases its configuration and its passwords."));
        }
        let relPart;
        if (!rel) {
            relPart = `<p>This site has no release yet. The page cannot flash. ${esc(S.releaseError || "")}</p>`;
        } else {
            relPart = `<div class="gwf-row gwf-wrap"><b>Version</b>${versionButtons()}</div>`;
            if (m && m.errors) {
                relPart += warn("The manifest of this release is not correct.", esc(m.errors.join(" ")));
            } else if (!m || m.loading) {
                relPart += `<p class="gwf-mut">The page reads the manifest of release ${esc(rel.version)}.</p>`;
            }
        }
        const ready = rel && m && m.manifest && k && S.setup.loads;
        const label = !k ? "Select Install or Repair" : k === "install" ? `Install release ${rel ? rel.version : ""}` : `Repair with release ${rel ? rel.version : ""}`;
        const radio = (id, title, text) => `<label class="gwf-opt"><input type="radio" name="gwf-case" value="${id}" ${k === id ? "checked" : ""} data-act="kind"><span class="gwf-stack"><b>${title}</b><span>${text}</span></span></label>`;
        return `<div class="gwf-cols"><div class="gwf-col1"><h3>What do you want to do?</h3>
${radio("install", "Install", "For a new board, or to start again from zero. The page erases all the flash, with any configuration and passwords. The gateway then starts in commissioning mode, with no configuration.")}
${radio("repair", "Repair", "For a gateway whose web UI does not open in any mode. The page writes only the firmware. The configuration and the passwords stay.")}
${relPart}${notes.join("")}</div>
<div class="gwf-col1"><div class="admonition warning"><p class="admonition-title">Disconnect the loads from the outputs before the flash.</p>
<p>This firmware has not run on the board yet, and the polarity of the outputs is not verified.</p>
<label class="gwf-row"><input type="checkbox" data-act="loads" ${S.setup.loads ? "checked" : ""}><span>The loads are disconnected.</span></label></div>
<div class="gwf-row gwf-wrap"><button type="button" class="gwf-btn gwf-pri" data-act="flash" ${ready ? "" : "disabled"}>${esc(label)}</button>
<button type="button" class="gwf-btn" data-act="toboard">Back</button></div></div></div>`;
    }

    function viewFlashing() {
        const f = S.flash;
        const order = ["download", "connect", ...(f.erase ? ["erase"] : []), "write", "restart"];
        const idx = order.indexOf(f.stage);
        const label = { download: "Download the images", connect: "Connect to the download mode", erase: "Erase all the flash", write: "Write the firmware", restart: "Restart the board" };
        const info = (s, i) => {
            if (s === "download") {
                return i === idx ? `${f.n} of ${f.rel ? 4 : 0}` : "SHA-256 correct";
            }
            if (s === "write" && i === idx) {
                return `<span data-pct>${Math.round(f.pct * 100)} %</span>`;
            }
            if (s === "erase" && i === idx) {
                return "this can take some time";
            }
            return "";
        };
        const rows = order.map((s, i) => {
            const kind = i < idx ? "ok" : "wait";
            const [cls, mark] = i < idx ? CHECK.ok : i === idx ? CHECK.wait : ["gwf-next", "·"];
            return `<div class="gwf-chk"><span class="gwf-mark ${cls}" aria-hidden="true">${mark}</span><span>${label[s]}</span><span class="gwf-mut" data-kind="${kind}">${info(s, i)}</span></div>`;
        }).join("");
        return `<div class="gwf-box"><h3>${f.kind === "install" ? "Install" : "Repair"}: the flash of release ${esc(f.rel.version)}</h3>${rows}
<div class="gwf-chk"><span class="gwf-mark gwf-next" aria-hidden="true">·</span><span>Read the board again (no click)</span><span></span></div>
<div class="gwf-bar" data-flash><i style="width:${Math.round(f.pct * 100)}%"></i></div>
<p>Keep the cable connected. Keep this tab in view.</p></div>`;
    }

    function viewFail() {
        const f = S.fail;
        const detail = `<p class="gwf-mut gwf-mono gwf-break">${esc(f.detail)}</p>`;
        if (f.stage === "download") {
            return `<div class="gwf-box"><h3>The page could not get the images</h3><p>Nothing changed on the board.</p>${detail}
<div class="gwf-row gwf-wrap"><button type="button" class="gwf-btn gwf-pri" data-act="flash">Try the flash again</button><button type="button" class="gwf-btn" data-act="retry">Back to the panel</button></div></div>`;
        }
        if (f.stage === "chip") {
            return `<div class="gwf-box"><h3>This board has another chip</h3><p>The chip is ${esc(f.detail)}, not ESP32-S3. Nothing changed on the board. Select the port of the gateway.</p>
<div class="gwf-row gwf-wrap"><button type="button" class="gwf-btn gwf-pri" data-act="other">Select another port</button><button type="button" class="gwf-btn" data-act="retry">Back to the panel</button></div></div>`;
        }
        if (f.stage === "connect") {
            return `<div class="gwf-box"><h3>The flash did not start</h3><p>Nothing was erased. Put the board in the download mode by hand:</p>
${warn("Do not hold BOOT while the gateway has power.", "A push of 2 to 10 s opens the access point. A push of more than 10 s does a factory reset at the release.")}
<ol><li>Remove all the power: the USB cable, PoE and the terminal block.</li><li>Hold the BOOT button.</li><li>Connect the USB cable. Then release BOOT.</li><li>Click <b>Try the flash again</b>.</li></ol>${detail}
<div class="gwf-row gwf-wrap"><button type="button" class="gwf-btn gwf-pri" data-act="flash">Try the flash again</button>
<button type="button" class="gwf-btn" data-act="other">Select another port</button><a href="#gw-flash" data-act="tab" data-arg="files">Download the image files</a></div></div>`;
        }
        return `<div class="gwf-box"><h3>The flash stopped</h3>
${warn("Keep the cable connected, and try the flash again.", "The flash of the board changed. The gateway possibly does not start until a flash is complete.")}${detail}
<div class="gwf-row gwf-wrap"><button type="button" class="gwf-btn gwf-pri" data-act="flash">Try the flash again</button>
<button type="button" class="gwf-btn" data-act="tab" data-arg="console">Open the console</button><a href="#gw-flash" data-act="tab" data-arg="files">Download the image files</a></div></div>`;
    }

    function viewBoard() {
        switch (S.board) {
        case "setup": return viewSetup();
        case "flashing": return viewFlashing();
        case "fail": return viewFail();
        case "pwreset": return viewPwReset();
        case "pwbusy": return viewPwBusy();
        case "pwresult": return viewPwResult();
        default: break;
        }
        if (!reader) {
            return "";
        }
        if (reader.phase === "reading") {
            return viewReading();
        }
        if (reader.phase === "noanswer") {
            return viewNoAnswer();
        }
        if (reader.state === "weboff") {
            return viewWebOff();
        }
        if (reader.state === "fault" || reader.state === "starting") {
            return viewProblem();
        }
        return viewReady();
    }

    function viewPanel() {
        const out = S.cableOut ? warn("The USB cable is disconnected.", "The last data stays in view, in gray. When the cable comes back, the page reads the board again with no click.") : "";
        let body = "";
        if (S.tab === "board") {
            const last = S.log.length > 0 ? S.log[S.log.length - 1] : "";
            body = `<div class="${S.cableOut ? "gwf-gray" : ""}">${viewBoard()}</div>` +
                (S.board === "flashing" ? "" : `<div class="gwf-last gwf-row"><b>Console</b><span class="gwf-mono gwf-mut">${esc(last)}</span><a href="#gw-flash" data-act="tab" data-arg="console">Open</a></div>`);
        } else if (S.tab === "files") {
            body = `<div class="gwf-box"><h3>The image files</h3>${filesBlock()}</div>`;
        }
        return header() + out + tabs() + body;
    }

    let lastHtml = "";
    function render() {
        if (!alive) {
            return;
        }
        let html;
        if (S.view === "noserial") {
            html = viewNoSerial();
        } else if (S.view === "files") {
            html = viewFiles();
        } else if (S.view === "panel") {
            html = viewPanel();
        } else {
            html = viewStart();
        }
        // Each tick can call render(): change the DOM only when the text changed, so that a click
        // between two ticks does not get lost.
        if (html !== lastHtml) {
            main.innerHTML = html;
            lastHtml = html;
        }
        const showConsole = S.view === "panel" && S.tab === "console" && S.board !== "flashing";
        if (consoleEl.hidden === showConsole) {
            consoleEl.hidden = !showConsole;
            if (showConsole) {
                logEl.scrollTop = logEl.scrollHeight;
            }
        }
        root.classList.toggle("gwf-busy", S.board === "flashing");
    }

    // ---------- events ----------
    async function act(name, arg, el, ev) {
        switch (name) {
        case "connect": await connect(); break;
        case "disconnect": await disconnect(); break;
        case "other": await selectOther(); break;
        case "files": S.view = "files"; loadManifest(release()); break;
        case "back": S.view = "start"; break;
        case "copylink": await copy(location.href, "The link"); return;
        case "tab": S.tab = arg; if (arg === "files") {
            loadManifest(release());
        } break;
        case "retry": await retry(); return;
        case "setup": openSetup(arg); return;
        case "toboard": S.board = "reader"; break;
        case "kind": S.setup.kind = el.value; break;
        case "loads": S.setup.loads = el.checked; break;
        case "ver": S.sel = Number(arg); loadManifest(release()); break;
        case "flash": await runFlash(); return;
        case "pwask": pwAsk(); return;
        case "pwdo": await pwDo(); return;
        case "copyaddr": await copy(bestUrl(reader.data), "The address"); return;
        case "copypw": await copy(reader.data.commissioningPassword, "The password"); return;
        case "openweb": S.webOpened = true; break;
        case "fill": cmdInput.value = arg; cmdInput.focus(); return;
        case "clear": logEl.textContent = ""; S.log = []; return;
        default: return;
        }
        if (ev && name !== "openweb") {
            ev.preventDefault();
        }
        render();
    }

    root.addEventListener("click", (ev) => {
        const el = ev.target.closest("[data-act]");
        if (!el || !root.contains(el) || el.matches("input")) {
            return;
        }
        if (el.tagName === "A" && el.dataset.act !== "openweb") {
            ev.preventDefault();
        }
        act(el.dataset.act, el.dataset.arg, el, null);
    });
    root.addEventListener("change", (ev) => {
        const el = ev.target.closest("input[data-act]");
        if (el) {
            act(el.dataset.act, el.dataset.arg, el, null);
        }
    });
    cmdForm.addEventListener("submit", async (ev) => {
        ev.preventDefault();
        const cmd = cmdInput.value;
        if (!S.link || cmd.trim() === "") {
            return;
        }
        consoleText(`> ${cmd}`, "gwf-me");
        cmdInput.value = "";
        await S.link.write(cmd + "\r").catch(() => {});
    });
    function beforeUnload(ev) {
        if (S.board === "flashing") {
            ev.preventDefault();
            ev.returnValue = "";
        }
    }
    window.addEventListener("beforeunload", beforeUnload);
    // The instant navigation of the theme changes the page without an unload: during a flash, stop
    // a click on a link out of the tool, because the change of page closes the port.
    function guardLinks(ev) {
        const a = ev.target.closest && ev.target.closest("a[href]");
        if (S.board === "flashing" && a && !root.contains(a)) {
            ev.preventDefault();
            ev.stopPropagation();
            alert("The flash runs. Wait until it is complete, then open the other page.");
        }
    }
    document.addEventListener("click", guardLinks, true);

    render();
    loadReleases();

    return {
        busy: () => S.board === "flashing",
        async unmount() {
            alive = false;
            clearInterval(timer);
            window.removeEventListener("beforeunload", beforeUnload);
            document.removeEventListener("click", guardLinks, true);
            if ("serial" in navigator) {
                navigator.serial.removeEventListener("connect", onSerialConnect);
                navigator.serial.removeEventListener("disconnect", onSerialDisconnect);
            }
            await closeLink();
        },
    };
}
