// Issue #226: the unit test of the IPv4 normalization of the web UI (ip4Kind, ip4Norm, ip4Blur in
// idf/components/gw_web/www/index.html). The functions are in the inline script of the page,
// which goes into the firmware image as it is. Thus the test reads the page, takes the source of
// each function (its first line starts with "function <name>(", its next lines start with one
// space, as all the code of the page) and runs it in a separate V8 context, without a browser.
// Run it from any directory with Node.js 20 or later: node --test idf/test/web/test_ip4norm.mjs
// The CI job host-tests runs it after ESLint.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";

const page = readFileSync(new URL("../../components/gw_web/www/index.html", import.meta.url), "utf8");

function source(name) {
    const m = new RegExp(`^function ${name}\\(.*(?:\\n .*)*`, "m").exec(page);
    assert.ok(m, `index.html has no top-level function ${name}`);
    return m[0];
}

// The page uses the browser class Event only to make the events of a changed field.
class FakeEvent {
    constructor(type, opts) { this.type = type; this.bubbles = !!(opts && opts.bubbles); }
}
const fns = vm.runInNewContext(
    `${source("ip4Kind")}\n${source("ip4Norm")}\n${source("ip4Blur")}\n({ip4Kind, ip4Norm, ip4Blur})`,
    { Event: FakeEvent });
const { ip4Kind, ip4Norm, ip4Blur } = fns;

test("ip4Norm removes the leading zeros of a full IPv4 address", () => {
    assert.equal(ip4Norm("192.168.001.010", false), "192.168.1.10");
    assert.equal(ip4Norm("010.000.000.001", false), "10.0.0.1");
    assert.equal(ip4Norm("255.255.255.000", false), "255.255.255.0");
    assert.equal(ip4Norm("000.000.000.000", false), "0.0.0.0");
});

test("ip4Norm removes the leading zeros of a network only in a network field", () => {
    assert.equal(ip4Norm("010.000.000.001/08", true), "10.0.0.1/8");
    assert.equal(ip4Norm("192.168.001.000/024", true), "192.168.001.000/024", "a prefix has 1 or 2 digits");
    assert.equal(ip4Norm("10.0.0.1/00", true), "10.0.0.1/0");
    assert.equal(ip4Norm("010.000.000.001/08", false), "010.000.000.001/08", "no prefix in an address field");
});

test("ip4Norm removes the spaces around each value", () => {
    assert.equal(ip4Norm(" 10.0.0.1 ", false), "10.0.0.1");
    assert.equal(ip4Norm("\t192.168.001.010\n", false), "192.168.1.10");
    assert.equal(ip4Norm("  pool.ntp.org ", false), "pool.ntp.org");
    assert.equal(ip4Norm(" 10.0.0.0/24 ", true), "10.0.0.0/24");
});

test("ip4Norm keeps each other value as typed", () => {
    for (const v of ["+1.2.3.4", "-1.2.3.4", "1.2.3", "1.2.3.4.5", "0.0.0.0", "10.0.0.1", "pool.ntp.org",
                     "fe80::1", "::ffff:192.0.2.001", "broker-01", "1. 2.3.4", "1.2.3.0x1", "0001.2.3.4",
                     "1.2.3.4/", "1.2.3.4/123", "1.2.3.4/-0", "1.2.3.4/+8", "١.٢.٣.٤", ""]) {
        assert.equal(ip4Norm(v, true), v, v);
        assert.equal(ip4Norm(v, false), v, v);
    }
    assert.equal(ip4Norm(" +1.2.3.4 ", false), "+1.2.3.4", "only the trim");
});

test("ip4Norm keeps a number above 255 for the validation", () => {
    assert.equal(ip4Norm("192.168.001.256", false), "192.168.1.256");
    assert.equal(ip4Norm("999.0.0.1/33", true), "999.0.0.1/33");
});

test("ip4Kind gives the IPv4 fields of the Configuration tab", () => {
    for (const p of ["network.ip", "network.netmask", "network.gateway", "network.dns",
                     "network.wifi.ip", "network.wifi.netmask", "network.wifi.gateway", "network.wifi.dns",
                     "mqtt.host", "mqtt.backup_brokers.0.host", "mqtt.backup_brokers.2.host",
                     "ntp.servers.0", "ntp.servers.2", "syslog.host"]) {
        assert.equal(ip4Kind(p), "addr", p);
    }
    assert.equal(ip4Kind("modbus_tcp.allowed_clients.0"), "net");
    assert.equal(ip4Kind("modbus_tcp.allowed_clients.7"), "net");
    for (const p of ["", "network.ipv6.address", "network.ipv6.dns", "network.hostname", "network.dhcp",
                     "network.wifi.ssid", "mqtt.port", "mqtt.tls.server_name", "mqtt.backup_brokers.0.port",
                     "ntp.servers", "syslog.port", "modbus_tcp.allowed_clients", "devices.0.name",
                     "xnetwork.ip", "network.ipx", "mqtt.hostname"]) {
        assert.equal(ip4Kind(p), "", p);
    }
});

// A fake <input>: its value and the events that the page sends to it.
function field(p, value) {
    const sent = [];
    return { dataset: { p }, value, sent, dispatchEvent(e) { sent.push(`${e.type}:${e.bubbles}`); return true; } };
}

test("ip4Blur changes the field and sends input and change, as a typed change", () => {
    const f = field("network.ip", "192.168.001.010");
    ip4Blur({ target: f });
    assert.equal(f.value, "192.168.1.10");
    assert.deepEqual(f.sent, ["input:true", "change:true"]);
    const c = field("modbus_tcp.allowed_clients.3", " 010.000.000.001/08 ");
    ip4Blur({ target: c });
    assert.equal(c.value, "10.0.0.1/8");
});

test("ip4Blur does not change a correct value or a field without an IPv4 value", () => {
    for (const [p, v] of [["network.ip", "192.168.1.10"], ["ntp.servers.0", "pool.ntp.org"],
                          ["network.hostname", " 010.0.0.1 "], ["network.ipv6.dns", "fe80::1"],
                          ["mqtt.host", "+1.2.3.4"]]) {
        const f = field(p, v);
        ip4Blur({ target: f });
        assert.equal(f.value, v, p);
        assert.deepEqual(f.sent, [], p);
    }
    ip4Blur({ target: { value: "010.0.0.1" } });   // an element without dataset
});

test("the page calls ip4Blur when a field of the Configuration tab loses the focus", () => {
    assert.ok(page.includes('$("#tab-config").addEventListener("focusout",ip4Blur);'));
});
