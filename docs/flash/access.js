// Issue #219: the parser of the answer of the shell command 'access' (#280), and the state of
// the board that the panel shows. The answer has a fixed form: gw_access_format() in
// idf/components/gw_core/src/gw_access.c, documented in docs/reference/commissioning-and-secrets.md
// ("The command access"). Pure functions: the unit test is idf/test/web/test_flash_tool.mjs.

export const ACCESS_VERSION = 1;
const MAX_LINES = 40;

// Remove the ANSI escape sequences (the colors of the log) and the carriage returns.
export function cleanLine(line) {
    // eslint-disable-next-line no-control-regex
    return line.replace(/\x1b\[[0-9;?]*[ -/]*[@-~]/g, "").replace(/\r/g, "");
}

// Collects the lines of one answer from the stream of the console. The first line can carry the
// prompt and the echo of the command before it ("gw> access-data v1"). Lines of the log can come
// before and after the answer, but not inside it: the firmware writes the answer with one write.
export class AccessCollector {
    constructor() {
        this.lines = null;
    }

    // Returns null, or {ok: true, data} for a full answer, or {ok: false, error} for an answer
    // that this page cannot read.
    push(rawLine) {
        const line = cleanLine(rawLine);
        const start = /access-data v(\d+)\s*$/.exec(line);
        if (start) {
            if (Number(start[1]) !== ACCESS_VERSION) {
                this.lines = null;
                return { ok: false, error: "version", version: Number(start[1]) };
            }
            this.lines = [];
            return null;
        }
        if (this.lines === null) {
            return null;
        }
        if (line.trim() === "access-data end") {
            const lines = this.lines;
            this.lines = null;
            return parseAccessLines(lines);
        }
        this.lines.push(line);
        if (this.lines.length > MAX_LINES) {
            this.lines = null;
            return { ok: false, error: "too-long" };
        }
        return null;
    }
}

// Parses the lines between "access-data v1" and "access-data end".
export function parseAccessLines(lines) {
    const d = {
        mode: null, firmware: null, hostname: null,
        web: null, login: null, user: null, commissioningPassword: null, fingerprint: null,
        ifs: {}, urls: [], urlNone: null,
    };
    for (const raw of lines) {
        const line = raw.trim();
        if (line === "") {
            continue;
        }
        const sp = line.indexOf(" ");
        const key = sp < 0 ? line : line.slice(0, sp);
        const val = sp < 0 ? "" : line.slice(sp + 1).trim();
        switch (key) {
        case "mode": d.mode = val; break;
        case "firmware": d.firmware = val; break;
        case "hostname": d.hostname = val; break;
        case "web": {
            const m = /^on (https?) (\d+)$/.exec(val);
            if (m) {
                d.web = { on: true, scheme: m[1], port: Number(m[2]) };
            } else if (val === "off") {
                d.web = { on: false };
            }
            break;
        }
        case "login": d.login = val; break;
        case "user": d.user = val; break;
        case "commissioning-password": d.commissioningPassword = val; break;
        case "fingerprint": d.fingerprint = val; break;
        case "if": {
            const m = /^(\S+) (off|no-link|no-address|up)(?: (\d{1,3}(?:\.\d{1,3}){3}))?$/.exec(val);
            if (m) {
                d.ifs[m[1]] = { state: m[2], ip: m[3] || null };
            }
            break;
        }
        case "url":
            if (/^https?:\/\/[^\s/]+\/$/.test(val)) {
                d.urls.push(val);
            }
            break;
        case "url-none": d.urlNone = val; break;
        default: break;   // a key that this page does not know: ignore it (a new key, same format)
        }
    }
    if (d.mode === null || d.firmware === null || d.web === null || d.login === null) {
        return { ok: false, error: "incomplete" };
    }
    return { ok: true, data: d };
}

// The state of the board for the panel. "starting": ask again. The other states are final.
export function classify(d) {
    if (d.mode === "starting") {
        return "starting";
    }
    if (d.mode === "fault") {
        return "fault";
    }
    if (!d.web.on) {
        return "weboff";
    }
    if (d.urls.length === 0) {
        return "noaddr";
    }
    return "ready";
}

export function passwordSet(d) {
    return d.login === "web-password" || d.login === "web-password-any-user";
}

// The URL to open: the first IPv4 address of Ethernet, then Wi-Fi, then the access point, then
// the mDNS name. The firmware gives the addresses in this order.
export function bestUrl(d) {
    return d.urls.length > 0 ? d.urls[0] : null;
}

// A sentence for the reason of "no address", from the state of the interfaces.
export function noAddressReason(d) {
    const eth = d.ifs.eth ? d.ifs.eth.state : "off";
    const wifi = d.ifs.wifi ? d.ifs.wifi.state : "off";
    if (eth === "no-link" && (wifi === "off" || wifi === "no-link")) {
        return "The Ethernet link is down.";
    }
    if (eth === "no-address" || wifi === "no-address") {
        return "The network gave no address.";
    }
    return "The Ethernet link is down, or the network gave no address.";
}
