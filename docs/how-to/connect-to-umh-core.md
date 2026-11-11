# Connect the gateway to UMH Core

This procedure tells how to send the data of the gateway to the Unified Namespace (UNS) of UMH Core (United Manufacturing Hub). UMH Core reads Sparkplug B with the `sparkplug_b` input of benthos-umh.

- For the reasons of the name rules, refer to [Sparkplug names and the UNS](../explanation/sparkplug-names-and-the-uns.md).
- For the name limits and the UMH topic of each metric, refer to [UMH names and topics](../reference/umh-names-and-topics.md).

> **Warning: Do not use this procedure as tested. Nobody tested it against a real UMH Core.** The interoperability E2E (#175) does not exist yet. The source of the data about UMH is the source code and the documentation of benthos-umh **v0.16.2** (commit `0d3a203`). Another version can operate differently.

> **Caution: Do not use UMH Core as the only copy of data that you must keep.** UMH cannot be the primary host of the gateway (#177). If UMH stops and the broker continues, the gateway continues to publish, and UMH loses that data. The [explanation](../explanation/sparkplug-names-and-the-uns.md#no-primary-host-data-can-be-lost) gives the details.

## Before you start

You need:

- UMH Core with benthos-umh v0.16.2.
- An MQTT broker, for example Mosquitto. UMH Core has no MQTT broker.
- A broker listener that the `sparkplug_b` input can use. Refer to [Select the broker listener](#select-the-broker-listener).
- The location of each Modbus device in your plant, for example enterprise, site, area, line and device.

## Select the broker listener

The `sparkplug_b` input of v0.16.2 has these limits:

- It uses MQTT 3.1.1 only (the Eclipse Paho Go client).
- It has no option for a private CA, and no option for a client certificate. Mutual TLS is not possible.
- A broker URL with the scheme `ssl://` uses TLS with the system CAs of the UMH container.

Select one of these options:

| Option | Gateway | UMH Core |
|---|---|---|
| A. No TLS on a protected network | `mqtt.tls` off, port 1883 | `tcp://<broker>:1883` |
| B. A listener on the loopback interface for UMH, TLS for the gateway | `mqtt.tls.enabled: true` with your private CA in `mqtt.tls.ca_file` | `tcp://127.0.0.1:1883`. UMH Core and the broker must run on the same computer |
| C. A broker certificate from a public CA | `mqtt.tls.enabled: true`, `mqtt.tls.ca_file: ""` | `ssl://<broker DNS name>:8883` |

Option C works only when a CA that the two sides trust signs the broker certificate:

- An empty `mqtt.tls.ca_file` makes the gateway use the public CA bundle of ESP-IDF (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN`). This bundle is compile-only: the E2E uses its own CA.
- The broker needs a public DNS name. The gateway compares the name with `mqtt.host`, or with `mqtt.tls.server_name`.
- On a plant network with no access from the Internet, a CA such as Let's Encrypt can sign the certificate with the DNS-01 challenge. Nobody examined this option with the gateway yet.
- TLS needs a valid time on the gateway (SNTP or the RTC). Refer to `idf/README.md` §6.5.1.

## Select the names

UMH makes the location path of a device from its Sparkplug `device_id`. The gateway makes the `device_id` from `sparkplug.device_prefix` and the device name: `<device_prefix>:<name>`.

1. Write the location of each device as levels, for example `acme`, `berlin`, `packaging`, `line3`, `meter1`.
2. Put the levels that are the same for all devices of the gateway in `sparkplug.device_prefix`, with `:` between them. For example: `acme:berlin:packaging:line3`.
3. Put the remaining levels in the device name, for example `meter1`. If the devices are on two lines, use `line3:meter1` and `line4:meter1`, and keep `line3` out of the prefix.
4. Use only `A-Z`, `a-z`, `0-9`, `-` and `_` in a level.
5. Do not use spaces. UMH changes each space to `_`.
6. Do not use `.` in a level. UMH reads `.` as one more level.
7. Do not start a level with `_`. UMH reads such a level as the data contract.
8. Keep each device name at 39 characters or less. The full `device_id` must fit in the topic: 78 bytes or more, refer to the [reference](../reference/umh-names-and-topics.md#name-limits-of-the-gateway).
9. Write the `edge_node_id` in the same form, for example `acme:berlin:packaging:gw01`. Keep it at 32 characters or less.
10. In tag names, use `/` between the levels of a virtual path. For example: `L1/Current`.
11. Do not use `:` in tag names. UMH splits a name at the last `:` before it examines `/`.

## Configure the gateway

1. Set `sparkplug.group_id`. UMH does not use it in the location, but the bridge subscribes to it.
2. Set `sparkplug.edge_node_id` to the location of the gateway.
3. Set `sparkplug.device_prefix` to the common levels of the location. Give each device the remaining levels as its `name`.
4. Keep `sparkplug.primary_host_id` empty.
5. Do not give a device the key `type`. UMH does not show a device that is a template instance.
6. Do not use tags of the type `bytes`. UMH does not show them.
7. Keep `mqtt.protocol` at `3.1.1` (the default).
8. Set `sparkplug.store_forward` to `true` if you want the data of a broker failure.

An example `config.json` for option A:

```json
{
  "network": { "dhcp": true },
  "ntp":     { "server": "pool.ntp.org" },
  "mqtt":    { "host": "192.168.1.10", "port": 1883, "client_id": "",
               "username": "", "password": "", "keepalive_s": 30, "protocol": "3.1.1" },
  "sparkplug": { "group_id": "acme", "edge_node_id": "acme:berlin:packaging:gw01",
                 "device_prefix": "acme:berlin:packaging:line3",
                 "primary_host_id": "", "store_forward": true },
  "rs485":   { "baud": 9600, "parity": "none", "stop_bits": 1,
               "response_timeout_ms": 200, "retries": 2, "inter_frame_delay_ms": 5 },
  "local_io": { "di_debounce_ms": 20 },
  "devices": [
    {
      "name": "meter1", "unit_id": 1, "poll_ms": 1000,
      "offline_after": 3,
      "tags": [
        { "name": "Voltage",    "area": "input", "address": 0, "type": "float32",
          "word_order": "big", "deadband": 0.1, "units": "V" },
        { "name": "L1/Current", "area": "input", "address": 2, "type": "float32",
          "word_order": "big", "deadband": 0.01, "units": "A" },
        { "name": "Run",        "area": "coil",  "address": 0, "type": "bool" }
      ]
    }
  ]
}
```

Load the configuration with the web UI, `PUT /api/config` or the shell command `config load` (`idf/README.md` §3.4).

## Configure the UMH bridge

Make a bridge in UMH Core, and put the input and the pipeline of this example in its read flow. The format of the bridge in the UMH configuration file changes between UMH versions. Thus this document gives only the benthos-umh part.

```yaml
input:
  sparkplug_b:
    mqtt:
      urls: ["tcp://192.168.1.10:1883"]
      client_id: "umh-sparkplug-acme"
    identity:
      group_id: "acme"
    role: "secondary_active"
    include_edge_node_in_location: false

pipeline:
  processors:
    - tag_processor:
        defaults: |
          // Keep only the metrics of BIRTH and DATA messages
          var type = msg.meta.spb_message_type;
          if (type !== "NBIRTH" && type !== "DBIRTH" &&
              type !== "NDATA" && type !== "DDATA") {
            return null;
          }
          // Drop null values, DataSet, Template and Bytes (UMH gives no value)
          if (msg.meta.umh_conversion_status !== "success") {
            return null;
          }
          // Drop the session and command metrics of the node, and unknown aliases
          var name = msg.meta.spb_metric_name || "";
          if (name === "bdSeq" || name.indexOf("Node Control/") === 0 ||
              name.indexOf("Firmware Update/") === 0 || name.indexOf("alias_") === 0) {
            return null;
          }
          msg.meta.location_path = msg.meta.umh_location_path;
          msg.meta.data_contract = "_raw";
          msg.meta.tag_name = msg.meta.umh_tag_name;
          if (msg.meta.umh_virtual_path) {
            msg.meta.virtual_path = msg.meta.umh_virtual_path;
          }
          // Use the Sparkplug timestamp of the sample, not the arrival time
          msg.meta.timestamp_ms = msg.meta.spb_timestamp;
          msg.payload = msg.payload.value;
          return msg;

output:
  uns: {}
```

Each key in this example is in benthos-umh v0.16.2:

| Key or line | Why |
|---|---|
| `identity.group_id` | The bridge subscribes to `spBv1.0/<group_id>/#` only |
| `role: "secondary_active"` | The bridge sends a rebirth request when it gets DATA with no BIRTH, for example after a restart of UMH. The default `secondary_passive` never sends it, and the tags then show as `alias_<n>` |
| `include_edge_node_in_location: false` | The `device_id` (the `device_prefix` and the device name) already holds the full location. With `true`, UMH puts the `edge_node_id` before it |
| `return null` | A deliberate drop in the `tag_processor` |
| `msg.meta.timestamp_ms = msg.meta.spb_timestamp` | Without this line, UMH uses the arrival time. The store & forward samples (`is_historical`) then get the time of the replay |

Do not use the role `primary`. It publishes the Sparkplug 2.x STATE text (`ONLINE`), and the gateway accepts only the 3.0 JSON STATE (#177).

## Examine the result

1. Open the Topic Browser of UMH.
2. Find the location of the device, for example `acme.berlin.packaging.line3.meter1`.
3. Compare the topics with the table below.
4. On the gateway shell, type `rebirth`.
5. Make sure that the topics show the values again, with no `alias_<n>` tag.

| Metric | UMH topic |
|---|---|
| Device tag `Voltage` | `umh.v1.acme.berlin.packaging.line3.meter1._raw.Voltage` |
| Device tag `L1/Current` | `umh.v1.acme.berlin.packaging.line3.meter1._raw.L1.Current` |
| Device tag `Run` | `umh.v1.acme.berlin.packaging.line3.meter1._raw.Run` |
| Node metric `DI/1` | `umh.v1.acme.berlin.packaging.gw01._raw.DI.1` |
| Node metric `Node Info/Uptime` | `umh.v1.acme.berlin.packaging.gw01._raw.Node_Info.Uptime` |

The [reference](../reference/umh-names-and-topics.md#node-metrics) gives all the metrics. A button for the rebirth in the web UI (`POST /api/rebirth`, #171) is planned.

## Use Modbus TCP as a second path

The `modbus` input of UMH uses only TCP (`TCP`, `RTUoverTCP`, `ASCIIoverTCP`). It cannot use serial RTU. The Modbus TCP server of the gateway (MB-15) lets UMH poll the RTU devices directly.

1. In `config.json`, add `"modbus_tcp": { "enabled": true, "port": 502, "allowed_clients": ["192.168.1.20"] }`.
2. Replace `192.168.1.20` with the address of UMH Core.
3. In UMH, set `controller` of the `modbus` input to `tcp://<gateway>:502`.
4. Set `transmissionMode` to `TCP`.
5. Set `slaveIDs` to the unit IDs of the devices.

This path has these limits:

- There is no report by exception. UMH reads each value at each poll.
- There is no store & forward. Each value has the time of the UMH read.
- There is no authentication. `allowed_clients` only filters the IP addresses.
- Each request goes through the task that owns the bus, between two poll requests. Thus it adds load to the RS485 bus.

## If there is a problem

| Symptom | Probable cause | Action |
|---|---|---|
| The tags show as `alias_<n>` | The bridge has no BIRTH of the node, for example after a restart of UMH | Set `role: "secondary_active"`. Type `rebirth` on the gateway shell |
| A device has no data | The device has a `type`. The tags are `bytes`. The device is offline: its values are null | Remove `type`. Use other tag types. Type `devices` on the gateway shell |
| A level is at the wrong place | The name has a space, a `.` or a level that starts with `_` | Correct the name. The metadata `spb_original_device_id` shows the name before UMH changed it |
| An enterprise has the name of the gateway, for example `GW01` | The `edge_node_id` is a short name. UMH uses it as the location of the node metrics | Write the `edge_node_id` as a location, or drop the node metrics in the bridge |
| The device is the enterprise, for example `umh.v1.Meter1` | The `device_id` is not a location. Two gateways with `Meter1` then write to the same topic | Set `sparkplug.device_prefix` to the location of the devices |
| The timestamps are near 1970 | The gateway published before it had a valid time | Make sure that the gateway gets SNTP. Read `Node Info/Time Synced`. The fix of #160 is planned |
| The samples of a broker failure have the time of the replay | The `tag_processor` has no `timestamp_ms` line | Add `msg.meta.timestamp_ms = msg.meta.spb_timestamp` |
| The gateway sends no NBIRTH, and the shell says that it waits for the primary host | `sparkplug.primary_host_id` is not empty | Set `primary_host_id` to `""` |

The UMH downsampler has the option `late_policy` (`passthrough` or `drop`) for samples that arrive out of order. Nobody examined its effect on a replay of store & forward yet (#175).
