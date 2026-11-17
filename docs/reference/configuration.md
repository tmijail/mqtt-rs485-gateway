# The configuration document

This document lists all the keys of the configuration document of the gateway, with their types, defaults and rules.

- For the procedures that change the configuration, refer to [Change, back up and restore the configuration](../how-to/change-the-configuration.md).
- For the devices and the tags, refer also to [Add a Modbus device and its tags](../how-to/add-a-modbus-device.md).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The parser and the validation have host tests, and the E2E tests the keys in QEMU.

## The document

The configuration is one JSON document. The gateway keeps it in the LittleFS partition as `/cfg/config.json`. Without this file, the gateway uses the configuration in the firmware.

This example has the usual keys:

```json
{
  "network": { "dhcp": true, "ip": "192.168.1.50", "netmask": "255.255.255.0",
               "gateway": "192.168.1.1", "dns": "192.168.1.1" },
  "ntp":     { "server": "pool.ntp.org" },
  "mqtt":    { "host": "192.168.1.10", "port": 1883, "client_id": "",
               "username": "", "password": "", "keepalive_s": 30 },
  "sparkplug": { "group_id": "Plant1", "edge_node_id": "GW01", "primary_host_id": "" },
  "rs485":   { "baud": 9600, "parity": "none", "stop_bits": 1,
               "response_timeout_ms": 200, "retries": 2, "inter_frame_delay_ms": 5 },
  "local_io": { "di_debounce_ms": 20 },
  "devices": [
    {
      "name": "Meter1", "unit_id": 1, "poll_ms": 1000, "offline_after": 3,
      "tags": [
        { "name": "Voltage", "area": "input",   "address": 0, "type": "float32",
          "word_order": "big", "scale": 1.0, "offset": 0.0, "deadband": 0.1,
          "writable": false, "units": "V" },
        { "name": "Setpoint", "area": "holding", "address": 10, "type": "int16",
          "scale": 0.1, "writable": true },
        { "name": "Run",      "area": "coil",    "address": 0, "type": "bool",
          "writable": true }
      ]
    }
  ]
}
```

The top-level sections are `network`, `ntp`, `mqtt`, `sparkplug`, `rs485`, `local_io`, `devices`, `syslog`, `web`, `modbus_tcp` and `ota`. The sections `mqtt` and `sparkplug` are necessary. The other sections are optional.

## General rules

- An unknown key is an error. A duplicate key is an error.
- The maximum size of the document is 128 KiB.
- A maximum of 16 devices, 64 tags for each device, and 256 tags in total.
- An error gives the JSON path of the value, for example `devices[1].tags[2].type`. A JSON syntax error gives the line and the column.
- A warning does not stop a save. The warnings are the name checks of [UMH names](#name-checks-for-umh-core).

### The rules of the names

These rules apply to `group_id`, `edge_node_id`, `primary_host_id`, `device_prefix`, the device `name` and `type`, and the tag `name` and `units`:

- The names are valid UTF-8.
- The Sparkplug IDs do not contain `/`, `+`, `#` or the control characters U+0080 to U+009F.
- Two device names must not differ only in case. Two device types, or two tag names of one device, must not differ only in case. The comparison folds the ASCII letters only.
- A tag name must not also be a folder of another tag name of the same device, for example `A` and `A/B`.
- A device `type` must not be the name or the first folder of a node metric: `bdSeq`, `DI`, `DO`, `Counter`, `Node Control`, `Node Info` or `Firmware Update`.
- The longest device topic, `spBv1.0/<group_id>/DBIRTH/<edge_node_id>/<device_id>`, must fit in 159 bytes.

### The secrets

The gateway keeps `mqtt.password`, `network.wifi.password` and `web.password` in NVS, not in the document.

| Value in a document that you load | Result |
|---|---|
| `"********"` | The gateway keeps the stored secret |
| A new text | The gateway replaces the secret |
| `""` | The gateway deletes the secret |
| The key is not in the document | The gateway deletes the secret: the document replaces the full configuration |

- An export shows each secret that has a value as `"********"`.
- In the file `/cfg/config.json`, a secret that is not there, empty or `"********"` gets its value from NVS.
- A secret in clear text in the file is valid. The gateway moves it to NVS and writes the file again without it.
- The gateway keeps `web.password` only as a salted PBKDF2-HMAC-SHA256 hash, and as the PMK of the access point. For the reasons, refer to [The security of the access point](../explanation/access-point-security.md).

## `network`

The IPv4 keys of `network` are the keys of Ethernet.

| Key | Type | Default | Rule |
|---|---|---|---|
| `dhcp` | bool | `true` | With `false`, `ip`, `netmask` and `gateway` are necessary |
| `ip`, `netmask`, `gateway` | text | — | IPv4 addresses |
| `dns` | text | — | An IPv4 DNS server |
| `hostname` | text | `""` | One DNS label: 1 to 63 letters, digits and `-` (not at the start or the end), no dots. Empty = a name from `edge_node_id`: lower case, and `-` for each character that is not a letter or a digit (`GW01` → `gw01`). The DHCP requests contain the hostname |
| `mdns` | bool | `true` | The gateway answers `<hostname>.local` with mDNS. It announces the web UI as the service `_https._tcp` on `web.https_port`, or `_http._tcp` on `web.port` with `web.https: false`. The instance name is `<group_id>/<edge_node_id>` |
| `wifi` | object | — | Refer to [`network.wifi`](#networkwifi) |
| `ipv6` | object | — | Refer to [`network.ipv6`](#networkipv6) |

### `network.wifi`

Wi-Fi station, as an alternative interface or as a backup interface for Ethernet.

```json
"network": { "dhcp": true,
             "wifi": { "enabled": true, "ssid": "Plant", "password": "secret123",
                       "role": "backup", "failback_s": 30, "dhcp": true } }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `false` | `false` = Ethernet only |
| `ssid` | text | `""` | 1 to 32 bytes. Necessary with `enabled: true` |
| `password` | text (secret) | `""` | Empty (open network), 8 to 63 characters (WPA2), or 64 hexadecimal digits (PSK) |
| `role` | `backup` or `primary` | `backup` | `backup`: Ethernet gets the default route, and the gateway uses Wi-Fi only without Ethernet. `primary`: the opposite |
| `failback_s` | integer | `30` | 0 to 3600. The time that the preferred interface must be up without a cut before it gets the session again |
| `dhcp`, `ip`, `netmask`, `gateway`, `dns` | as in `network` | DHCP | The IPv4 data of the Wi-Fi interface. With `dhcp: false`, `ip`, `netmask` and `gateway` are necessary |

**No path to the broker.** An interface can have a link and an address, but no path to the broker. When all the brokers of the list fail on the active interface, that interface has "no service":

- The session moves at once to the other interface, if it is usable.
- The wait starts at 60 s. Each new failure in sequence makes it two times longer, up to 30 min.
- At the end of the wait, the preferred interface gets the session again.
- The gateway uses an interface with no service when no other interface is usable.

### `network.ipv6`

IPv6 together with IPv4.

```json
"network": { "dhcp": true,
             "ipv6": { "enabled": true, "slaac": true, "address": "fd00:1::50", "dns": "" } }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `false` | `false` = IPv4 only. With `true`, each interface has a link-local address |
| `slaac` | bool | `true` | Stateless address autoconfiguration from the router advertisements |
| `address` | text | `""` | A static address for Ethernet (RFC 4291). The prefix on the link is /64. Multicast, link-local, `::` and `::1` are not permitted |
| `dns` | text | `""` | An IPv6 DNS server. DHCPv4 fills the first two positions, and this server is the third |

The gateway learns the default IPv6 router from the router advertisements. There is no static IPv6 gateway. `mqtt.host`, `ntp.server` and `syslog.host` accept IPv6 addresses and names with an AAAA record.

## `ntp`

| Key | Type | Default | Rule |
|---|---|---|---|
| `server` | text | `pool.ntp.org` | The name, IPv4 or IPv6 address of the SNTP server |

Without a valid clock, the gateway opens no MQTT session. Refer to [The clock and the RTC battery](clock-and-rtc-battery.md).

## `mqtt`

| Key | Type | Default | Rule |
|---|---|---|---|
| `host` | text | — | Necessary. The name, IPv4 or IPv6 address of the primary broker |
| `port` | integer | `1883` (`8883` with TLS) | 1 to 65535 |
| `client_id` | text | `""` | Empty = `edge_node_id` |
| `username` | text | `""` | |
| `password` | text (secret) | `""` | |
| `keepalive_s` | integer | `30` | 5 to 3600 |
| `protocol` | `"3.1.1"` or `"5"` | `"3.1.1"` | The MQTT version for all the brokers of the list |
| `tls` | object | — | Refer to [`mqtt.tls`](#mqtttls) |
| `backup_brokers`, `failover_after`, `primary_return_s` | | | Refer to [The list of brokers](#the-list-of-brokers) |

Each CONNECT has Clean Session = 1. With `protocol: "5"`, each CONNECT has Clean Start = 1, Session Expiry Interval = 0 and Will Delay Interval = 0. Each DISCONNECT of the gateway has the reason code 0x04 (Disconnect with Will Message). The topics, the QoS, the retain flags and the payloads do not change. The gateway connects again with an exponential backoff and jitter (1 s to 60 s).

### `mqtt.tls`

```json
"mqtt": { "host": "broker.plant.local", "port": 8883,
          "tls": { "enabled": true, "ca_file": "ca.pem", "server_name": "",
                   "cert_file": "gw01.crt", "key_file": "gw01.key" } }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `false` | `false` = TCP without encryption. With `true`, the default of `mqtt.port` is `8883` |
| `ca_file` | text | `""` | A PEM file with the CAs that sign the certificate of the broker, in the LittleFS partition (`/cfg/<name>`). Empty = the public root CAs of ESP-IDF |
| `server_name` | text | `""` | The name that the certificate of the broker must contain (SAN or CN). Empty = the `host` of the connection. You cannot disable the name check |
| `cert_file` | text | `""` | The PEM certificate of the client, for mutual TLS, in LittleFS. Empty = no client certificate |
| `key_file` | text | `""` | The PEM private key of the client, **not encrypted** (PKCS#1, SEC1 or PKCS#8). Necessary with `cert_file`, and not permitted without it |

- A file name has 1 to 32 characters `A-Z a-z 0-9 . _ -`, without `/`. It must not start with `.`, and it must not be `config.json`.
- The gateway uses TLS 1.2 or 1.3, as the broker selects.
- The gateway examines the validity dates of the certificate. Thus the TLS connection waits for a valid clock.
- The gateway validates the TLS keys also when `enabled` is `false`.

### The list of brokers

```json
"mqtt": { "host": "broker1.plant.local", "port": 1883,
          "backup_brokers": [ { "host": "broker2.plant.local" },
                              { "host": "10.0.0.20", "port": 1884 } ],
          "failover_after": 3, "primary_return_s": 0 }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `backup_brokers` | array | `[]` | Up to 3 backup brokers, in order, after `mqtt.host`. Each one has `host` (necessary, not empty) and `port` (default: `mqtt.port`). All the brokers use the same credentials, `client_id`, keepalive and `tls`. With TLS, the gateway examines the name against the `host` of each broker, or `tls.server_name` |
| `failover_after` | integer | `3` | 1 to 20. The number of failed attempts in sequence on a broker before the gateway goes to the next broker. After the last broker, it goes to the first broker |
| `primary_return_s` | integer | `0` | 0 to 86400. `0` = the gateway stays on a backup broker until it fails. With N > 0, the gateway ends a stable session on a backup broker after N s (NDEATH). Then it tries the primary broker one time. If that attempt fails, it goes to the first backup broker |

These are failed attempts:

- a TCP or TLS connection that fails;
- a CONNACK that refuses the connection;
- a SUBSCRIBE that the broker refuses or does not answer;
- a session that stops before one keepalive (60 s maximum) after the NBIRTH.

A stable session sets the count to zero. Each new broker starts with the minimum backoff. For the primary host and the list of brokers, refer to [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md#the-primary-host-application).

## `sparkplug`

| Key | Type | Default | Rule |
|---|---|---|---|
| `group_id` | text | — | Necessary. 1 to 32 bytes |
| `edge_node_id` | text | — | Necessary. 1 to 32 bytes |
| `primary_host_id` | text | `""` | Empty = no primary host. Refer to [The primary host application](sparkplug-messages-and-metrics.md#the-primary-host-application) |
| `device_prefix` | text | `""` | 0 to 96 bytes. Refer to [The device prefix](#the-device-prefix) |
| `naming` | `"sparkplug"` or `"umh"` | `"sparkplug"` | `"umh"` selects the UMH preset. Refer to [Name checks for UMH Core](#name-checks-for-umh-core) |
| `diagnostics_s` | integer | `60` | 0, or 10 to 86400. The period of the NDATA with the node diagnostics, in seconds. 0 = only in the NBIRTH |
| `store_forward` | bool or object | `false` | Refer to [`sparkplug.store_forward`](#sparkplugstore_forward) |

### The device prefix

```json
"sparkplug": { "group_id": "acme", "edge_node_id": "acme:berlin:packaging:gw01",
               "device_prefix": "acme:berlin:packaging:line3" }
```

- `device_prefix` has location levels with `:` between them. No level is empty: no `:` at the start or the end, and no `::`.
- It has the characters of a Sparkplug ID. The number of levels has no limit, other than the length.
- The `device_id` of each device is `<device_prefix>:<name>`. Without the key, it is the name. The DBIRTH, DDATA, DDEATH and DCMD topics use the `device_id`.
- With a `group_id` and an `edge_node_id` of 32 bytes, the `device_id` can have 78 bytes.
- The metric names do not get the prefix.

For the reasons, refer to [Sparkplug names and the UNS](../explanation/sparkplug-names-and-the-uns.md).

### Name checks for UMH Core

The validation finds the names that UMH Core and other UNS hosts change with no error. Sparkplug permits these names, thus they are warnings. The dry run, the shell `config check`, the boot log and the web UI show them, each with its JSON path.

- The gateway divides `edge_node_id`, `device_prefix` and each device `name` at `:`. Each level must match `[A-Za-z0-9][A-Za-z0-9_-]*`.
- The gateway divides each tag `name` at `/`. Each segment must match `[A-Za-z0-9][A-Za-z0-9._-]*`.
- These items also give a warning:
  - a `primary_host_id` that is not empty;
  - a device `type`;
  - a tag of the type `bytes`.

With `"naming": "umh"`, the name rules become errors. The other warnings stay warnings. The preset also changes some metrics. Refer to [UMH names and topics](umh-names-and-topics.md) and [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md#the-umh-preset).

### `sparkplug.store_forward`

| Key | Type | Default | Rule |
|---|---|---|---|
| `store_forward` | bool or object | `false` | `false`: without a session, the gateway keeps only the last value of each metric. `true`: the gateway keeps each change in a ring buffer, and publishes the changes with `is_historical = true` in the next session |

The object form, with the optional spill to a microSD card:

```json
"sparkplug": { "group_id": "Plant1", "edge_node_id": "GW01",
               "store_forward": { "enabled": true, "sd": { "enabled": false, "max_kb": 131072 } } }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `true` | The same as `store_forward: true` |
| `sd.enabled` | bool | `false` | Moves the oldest samples to the microSD card when the RAM buffer is three quarters full. It requires `enabled` |
| `sd.max_kb` | integer | `131072` | 64 to 4 194 304 (128 MB by default). The space that the gateway uses on the card |

> **Caution: Read the GPIO45 warning in [`idf/README.md` §6.5](../../idf/README.md#65-slow-broker-and-command-limits) before you set `sd.enabled`.** GPIO45 (D0 of the card) is a strapping pin. A pull-up on D0 can stop the boot of the board.

For the buffer, the card, the retention and the duplicates, refer to [Store & forward](store-and-forward.md).

## `rs485`

| Key | Type | Default | Rule |
|---|---|---|---|
| `baud` | integer | `9600` | 1200 to 115200 |
| `parity` | `none`, `even` or `odd` | `none` | |
| `stop_bits` | integer | `1` | 1 or 2 |
| `response_timeout_ms` | integer | `200` | 10 to 5000 |
| `retries` | integer | `2` | 0 to 5 |
| `inter_frame_delay_ms` | integer | `5` | 0 to 1000. The delay between two transactions |

The UART always uses 8 data bits. The gateway keeps the silence of 3.5 characters between the frames.

## `devices`

An array of up to 16 devices.

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `true` | `false` = the device stays in the configuration, but the gateway does not poll it. Refer to [A disabled device](#a-disabled-device) |
| `name` | text | — | Necessary. 1 to 39 bytes |
| `unit_id` | integer | — | Necessary. 1 to 247 |
| `poll_ms` | integer | `1000` | 50 to 3 600 000 |
| `offline_after` | integer | `3` | 1 to 1000. After this number of failed cycles in sequence, the device is offline (DDEATH). The first cycle that reads all the blocks makes it online again (DBIRTH) |
| `slow_poll_ms` | integer | 10 × `poll_ms` (maximum 3 600 000) | 50 to 3 600 000, and not less than `poll_ms`. The poll interval of the tags with `scan: "slow"` |
| `block_reads` | bool | `true` | `false` = one request for each tag. Use it for a slave that answers with an exception if a block contains registers that it does not map |
| `max_gap` | integer | `0` | 0 to 1999. The number of unused registers or bits that a block can include to join two tags of the same area. Not permitted with `block_reads: false` |
| `write_multiple` | bool | `false` | `true` = the gateway writes one register with FC16 and one coil with FC15, in place of FC06 and FC05. Use it for a slave that does not accept FC06 or FC05. A write of two or more registers always uses FC16 |
| `type` | text | `""` | The device type, as a Sparkplug template. Refer to [Device types](#device-types) |
| `tags` | array | — | Necessary. 1 to 64 tags |

The defaults of `block_reads`, `max_gap` and `write_multiple` come from the build options `CONFIG_GW_MB_BLOCK_READS`, `CONFIG_GW_MB_BLOCK_MAX_GAP` and `CONFIG_GW_MB_FORCE_MULTI_WRITE`. A block never has more than 125 registers or 2000 bits.

In the web UI, `slow_poll_ms`, `block_reads`, `max_gap` and `write_multiple` are in the panel **More device settings** of each device.

### A disabled device

```json
{ "name": "Meter2", "unit_id": 3, "enabled": false, "tags": [ … ] }
```

- The gateway sends no request to a disabled device. The device uses no time of the bus.
- The device has no DBIRTH, no DDATA and no DDEATH. The gateway discards a DCMD for it.
- The validation examines a disabled device as any other device. Its name, its tags and the device limits count.
- A disabled device does not count as offline: it does not set the Modbus fault of the LED or the buzzer alarm `device_offline`.
- The Status tab of the web UI, the shell command `devices` and `GET /api/status` give the state `disabled`.
- Modbus TCP requests to its unit still go to the bus.

Use it, for example, for a slave that is not connected yet during the commissioning.

### Fast and slow scan

```json
{ "name": "Meter1", "unit_id": 1, "poll_ms": 1000, "slow_poll_ms": 30000, "tags": [
    { "name": "Voltage", "area": "input",   "address": 0,  "type": "float32" },
    { "name": "Serial",  "area": "holding", "address": 50, "type": "string", "length": 8, "scan": "slow" } ] }
```

- The gateway reads a `fast` tag each `poll_ms`, and a `slow` tag each `slow_poll_ms`.
- Fast tags and slow tags are never in the same read block.
- A cycle that reads only the fast tags fails if a request gets no response.
- A device becomes online only after a cycle that read all its blocks. Thus its DBIRTH contains all the tags. While the device is offline, each cycle includes the slow tags.
- If a cycle fails before it reads the slow tags, the next cycle reads them.
- The gateway polls a device with only slow tags each `slow_poll_ms`.

### Device types

```json
{ "name": "PumpA", "unit_id": 1, "type": "Pump", "tags": [ … ] },
{ "name": "PumpB", "unit_id": 2, "type": "Pump", "tags": [ … ] }
```

- `type` has a maximum of 39 characters, valid UTF-8, without `/ + #`. Empty = no template: one metric for each tag.
- All the devices of one `type` have the same tags: the same names and datatypes, in the same order. The area, the address, the scale and the units can be different. If not, the gateway refuses the configuration with the path `devices[i].type`.
- All the definitions together describe a maximum of 64 tags. Each type counts one time.

For the messages of a template, refer to [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md#templates).

## `devices[].tags`

| Key | Type | Default | Rule |
|---|---|---|---|
| `name` | text | — | Necessary. 1 to 39 bytes. Use `/` between the levels of a path |
| `area` | `coil`, `discrete`, `holding` or `input` | — | Necessary |
| `address` | integer | — | Necessary. 0 to 65535, 0-based. The last register of the tag must not go past 65535 |
| `type` | text | — | Necessary. Refer to [The tag types](#the-tag-types) |
| `word_order` | `big`, `little`, `ABCD`, `CDAB`, `BADC` or `DCBA` | `big` | Refer to [Word orders](#word-orders) |
| `scale` | number | `1` | Not 0. The gateway publishes `raw × scale + offset` |
| `offset` | number | `0` | |
| `deadband` | number | `0` | Not negative. An absolute deadband. The gateway publishes a new value only when it changes by more than the deadband |
| `writable` | bool | `false` | Only for `coil` and `holding`. A host can write the value with a DCMD |
| `scan` | `fast` or `slow` | `fast` | Refer to [Fast and slow scan](#fast-and-slow-scan) |
| `bit` | integer | — | 0 to 15. Refer to [One bit of a register](#one-bit-of-a-register) |
| `length` | integer | — | 1 to 16 registers. Necessary with `type: string` or `type: bytes`, and only with them |
| `units` | text | `""` | Up to 15 characters. The property `engUnit` of the metric. Empty = no property |
| `eng_low` | number | — | The property `engLow` (Float). In the range of a Float (±3.4e38) |
| `eng_high` | number | — | The property `engHigh` (Float). With the two keys, `eng_high` > `eng_low` |
| `write_min` | number | — | The lowest value that a DCMD can write, in the units of the tag (after `scale` and `offset`). Only with `writable: true`. Refer to [Write limits](#write-limits) |
| `write_max` | number | — | The highest value that a DCMD can write. With the two keys, `write_max` ≥ `write_min` |

- A `bool` tag has no `scale` and no `offset`.
- A `string` tag has no `scale`, no `offset` and no `deadband`.
- A `bytes` tag has no `scale`, `offset`, `deadband`, `eng_low` or `eng_high`.
- `eng_low` and `eng_high` are not permitted on `bool`, `string` and `bytes` tags.
- `write_min` and `write_max` are not permitted on `bool`, `string` and `bytes` tags, and not on a tag that is not writable.

In the web UI, the details row of a tag (**More**) shows only the keys that apply to the type of the tag. A key with a value stays visible.

### Write limits

```json
{ "name": "SpeedSetpoint", "area": "holding", "address": 1, "type": "uint16", "scale": 0.1,
  "writable": true, "write_min": 0, "write_max": 1500 }
```

- The gateway refuses a DCMD metric with a value below `write_min` or above `write_max`. It does not write to the device. It writes a warning to the log, and adds 1 to the refusal counter of the device (`write_refused` in `GET /api/status`, `writes refused` in the shell command `devices`).
- The other metrics of the same DCMD still apply.
- Sparkplug has no answer to a command. The next DDATA has the value of the device.
- No key = no limit on that side. A limit can be equal to the other limit.
- For a tag that is a Float in Sparkplug, the gateway compares the values with the precision of a Float. For an `int64` or `uint64` tag, the comparison is exact.
- The limits do not apply to Modbus TCP requests. That path sends the frames to the bus without a change.
- `eng_low` and `eng_high` are only properties of the metric. The gateway does not use them to examine a write.

### The tag types

| Type | Registers | Areas |
|---|---|---|
| `bool` | 1 bit | `coil`, `discrete`. Also `holding` and `input` with `bit` |
| `uint16`, `int16` | 1 | `holding`, `input` |
| `uint32`, `int32`, `float32` | 2 | `holding`, `input` |
| `uint64`, `int64`, `float64` | 4 | `holding`, `input` |
| `string` | `length` | `holding`, `input` |
| `bytes` | `length` | `holding`, `input` |

### Word orders

The letters are the bytes of the value, from the most significant (A) to the least significant, in the order of arrival.

| Value | Meaning |
|---|---|
| `big` = `ABCD` | The most significant word first |
| `little` = `CDAB` | The least significant word first |
| `BADC` | The bytes of each register in the opposite order |
| `DCBA` | All the bytes in the opposite order |

The word order applies to 32 and 64 bits. For 16 bits, only the byte swap applies. It does not apply to `string` and `bytes`, other than the byte order in each register.

### `string` and `bytes`

```json
{ "name": "Serial", "area": "holding", "address": 50,  "type": "string", "length": 8 },
{ "name": "Block",  "area": "holding", "address": 220, "type": "bytes",  "length": 3, "writable": true }
```

- **`string`:** each register holds two ASCII characters. The first character is in the high byte (in the low byte with `BADC` or `DCBA`).
  - The gateway cuts the text at the first NUL, and removes the spaces at the end.
  - It publishes each character that is not printable ASCII as `?`.
  - For a write, it adds NUL characters at the end. It refuses a text that is longer than `2 × length`.
- **`bytes`:** the gateway publishes `length` registers as a Bytes metric. Each register gives two bytes, the high byte first (the low byte first with `BADC` or `DCBA`).
  - The gateway does not change the bytes.
  - Report by exception compares all the bytes.
  - A write needs exactly `2 × length` bytes. The gateway writes them with one FC16.

### One bit of a register

```json
{ "name": "Alarm", "area": "holding", "address": 60, "type": "bool", "bit": 3, "writable": true }
```

- `bit` is necessary with `type: bool` in `holding` or `input`, and only there.
- 0 = the least significant bit of the register, as it arrives.
- With `bit`, `word_order` must be `big`.
- For a write, the gateway reads the register (FC03). Then it writes the full register with the changed bit (FC06, or FC16 with `write_multiple: true`).

### The datatypes in Sparkplug

| Tag type | Sparkplug datatype |
|---|---|
| `bool`, also with `bit` | Boolean (11) |
| `int16` | Int16 (2) |
| `uint16` | UInt16 (6) |
| `int32` | Int32 (3) |
| `uint32` | UInt32 (7) |
| `float32` | Float (9) |
| `int16`, `uint16` or `float32` with `scale` ≠ 1 or `offset` ≠ 0 | Float (9) |
| `int64` | Int64 (4), exact in `long_value` |
| `uint64` | UInt64 (8), exact in `long_value` |
| `float64`. Also `int32`, `uint32`, `int64`, `uint64` or `float64` with `scale` ≠ 1 or `offset` ≠ 0 | Double (10) |
| `string` | String (12) |
| `bytes` | Bytes (17) |

A Float has a 24-bit mantissa. Thus a 32-bit counter with a scale goes into a Double, not into a Float.

## `modbus_tcp`

A Modbus TCP server that gives access to the RTU slaves on the bus.

```json
"modbus_tcp": { "enabled": true, "port": 502, "allowed_clients": ["192.168.1.20", "10.0.5.0/24"],
                "idle_timeout_s": 60 }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `false` | Modbus TCP has no authentication. The server starts only when you enable it |
| `port` | integer | `502` | 1 to 65535 |
| `allowed_clients` | array of texts | `[]` | Up to 8 IPv4 addresses (`a.b.c.d`) or networks (`a.b.c.d/n`, n 0 to 32). The gateway closes a connection from another source without a response. Empty = any client, with a warning in the log |
| `idle_timeout_s` | integer | `60` | 0 to 3600. The gateway closes a connection without requests after this time. 0 = never |

- The gateway sends FC01 to FC06, FC15 and FC16 to the units 1 to 247, with or without a configured device.
- The gateway itself answers with an exception: 01 for another function, 03 for data that is not valid, 0A for the units 0 and 248 to 255. If the unit does not respond after the retries of `rs485`, the exception is 0B.
- The TCP requests go through the task that owns the bus, one at a time, between two poll requests. The priority is: DCMD writes, then Modbus TCP, then the poll.
- The gateway always refuses a connection through the commissioning access point, also when `allowed_clients` contains the client. Refer to [The access point](access-point.md).

## `ota`

```json
"ota": { "enabled": true, "ca_file": "fw-ca.pem" }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `true` | `false` refuses the updates by NCMD and by the web UI, and the button "Restart into `<version>`". In recovery mode, safe mode and commissioning, the web UI can always upload an image |
| `ca_file` | text | `""` | The CA of the server of the images (PEM, in LittleFS, with the name rules of `mqtt.tls.ca_file`). Empty = `mqtt.tls.ca_file` if TLS is on, else the public root CAs of ESP-IDF |

For the update process, refer to [Update the firmware](../how-to/update-the-firmware.md).

## `local_io`

All the keys of `local_io` are optional.

| Key | Type | Default | Rule |
|---|---|---|---|
| `di_debounce_ms` | integer | `20` | 0 to 1000. The debounce of the DIs |
| `failsafe_s` | integer | `0` | 0 to 86400. After this time without a Sparkplug session, each DO goes to its safe state. 0 = a loss of MQTT does not change the DOs |
| `do_failsafe` | array of texts | all `off` | Up to 8 items, `DO/1` first. Each item is `off`, `on` or `hold`. An item that is not there is `off` |
| `counter_save_s` | integer | `600` | 0, or 60 to 86400. The interval for the save of the pulse counters in NVS, only if they changed. 0 = no save |
| `rules` | array | `[]` | Refer to [Local rules](#local-rules) |
| `buzzer` | object | — | Refer to [`local_io.buzzer`](#local_iobuzzer) |

### The safe state of the DOs

```json
"local_io": { "di_debounce_ms": 20, "failsafe_s": 30,
              "do_failsafe": ["off", "on", "hold", "off", "off", "off", "off", "off"] }
```

| Event | `off` and `on` | `hold` |
|---|---|---|
| Restart (power-on, software restart, panic or watchdog) | Go to that state when the configuration is valid. Before that, OFF | Keeps the state that the TCA9554 expander kept. After a power cut, the expander is off: the DO starts as OFF |
| Rejected configuration (recovery mode), or commissioning | OFF | OFF |
| No session for `failsafe_s`, from the boot or from the loss | Go to that state one time | No change |
| The session starts again | No change until the next NCMD. The NBIRTH gives the real state | No change |

Before the gateway loads the configuration, it keeps the `hold` outputs of the last valid configuration (saved in NVS). If the expander did not respond at the boot, the gateway tries again with a backoff. It sets all the DOs to OFF when the expander responds.

### The pulse counters

- Each DI counts its activations: a change from inactive to active, after the debounce. With a debounce of 20 ms, the maximum is approximately 25 Hz.
- At the boot, the gateway restores the counters from the last save.
- An orderly restart saves them first: NCMD Reboot, a new configuration, the `restart` of the shell or of the web UI.
- A power cut, a panic or the watchdog loses the count since the last save.
- An NCMD cannot write the counters. The factory reset sets them to 0.

### Local rules

A local rule links a DI to a DO. The gateway calculates the rules itself, with or without a network or a broker.

```json
"local_io": { "rules": [ { "di": 2, "do": 5, "mode": "follow" },
                         { "di": 3, "do": 6, "mode": "toggle" } ] }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `di` | integer | — | Necessary. 1 to 8 |
| `do` | integer | — | Necessary. 1 to 8. Each DO is in one rule only |
| `mode` | `follow`, `invert` or `toggle` | `follow` | `follow`: DO = DI. `invert`: DO = not DI. `toggle`: each activation of the DI inverts the DO |

- Up to 8 rules.
- A DI change gets to the DO in a few milliseconds, plus the debounce.
- The gateway refuses an NCMD for a DO with a rule. The log tells this, and the NDATA confirms the real state.
- The safe state after a loss of MQTT does not change a DO with a rule.
- At a restart, a DO with a rule starts as `off` or `hold`. `on` is not permitted for a DO with a rule. Then `follow` and `invert` take the value of their DI, and `toggle` starts from that state.

### `local_io.buzzer`

```json
"local_io": { "buzzer": { "alarms": ["config_error", "mqtt_down", "device_offline"],
                          "alarm_after_s": 30 } }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `alarms` | array of texts | `[]` | The alarms that sound: `config_error`, `mqtt_down`, `device_offline`. No item two times. Empty = the buzzer only confirms the BOOT button actions |
| `alarm_after_s` | integer | `30` | 0 to 3600. The time of `mqtt_down` or `device_offline` before the alarm sounds. `config_error` sounds immediately |

- A rejected configuration cannot enable an alarm. Thus `config_error` uses the alarms of the last valid configuration (saved in NVS). After a factory reset, it does not sound.
- While the gateway waits for a valid clock, there is no Sparkplug session. Thus `mqtt_down` also sounds then.

For the patterns, refer to [The status signals](status-signals.md#buzzer).

## `syslog`

A copy of the log to a syslog server.

```json
"syslog": { "host": "192.168.1.20", "port": 514, "level": "info" }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `host` | text | `""` | The name, IPv4 or IPv6 address of the server. Empty = no remote syslog |
| `port` | integer | `514` | 1 to 65535, UDP |
| `level` | `error`, `warning`, `info` or `debug` | `info` | The minimum severity that the gateway sends |

The format is RFC 5424 on UDP (RFC 5426), with one message in each datagram:

- The facility is `local0`.
- HOSTNAME is `edge_node_id`, and APP-NAME is the log module.
- The message has `[meta sequenceId="n"]`.

Example: `<134>1 2026-11-04T12:00:00.123Z GW01 edge - - [meta sequenceId="12"] Sparkplug session up (bdSeq=3)`.

## `web`

The web UI and the REST API.

```json
"web": { "enabled": true, "port": 80, "username": "admin", "password": "a-long-password" }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `true` | `false` = no web server with a valid configuration. The server always starts with a rejected configuration, in commissioning, in the fault mode, and after a push of BOOT |
| `port` | integer | `80` | 1 to 65535, HTTP. With `https: true`, it only redirects to HTTPS |
| `https` | bool | `true` | HTTPS with the self-signed certificate of the gateway (EC P-256). The gateway makes it at the first boot and keeps it in NVS. The console shows its SHA-256 fingerprint at each boot. `false` = plain HTTP |
| `https_port` | integer | `443` | 1 to 65535, not the same as `port` |
| `username` | text | `"admin"` | 1 to 32 visible ASCII characters, without `:` |
| `password` | text (secret) | `""` | Empty, or 8 to 63 printable ASCII characters (0x20 to 0x7E). The password is also the WPA2 key of the access point |

While `web.password` is empty, the gateway accepts `admin` with its commissioning password (random; the console shows it). A push of BOOT then opens the claim window. Refer to [Set the first web password on site](../how-to/set-the-first-web-password.md).

For the routes, refer to [The REST API](rest-api.md).
