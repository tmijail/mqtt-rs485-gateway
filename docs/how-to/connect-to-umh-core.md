# Connect the gateway to UMH Core

This procedure tells how to send the data of the gateway to the Unified Namespace (UNS) of UMH Core (United Manufacturing Hub). UMH Core reads Sparkplug B with the `sparkplug_b` input of benthos-umh.

- For the reasons of the name rules, refer to [Sparkplug names and the UNS](../explanation/sparkplug-names-and-the-uns.md).
- For the name limits and the UMH topic of each metric, refer to [UMH names and topics](../reference/umh-names-and-topics.md).

> **Warning: Do not use this procedure with a different version of benthos-umh before you test it.** The interoperability E2E (#175, `--only umh`) tested this procedure with benthos-umh **v0.16.2** (commit `0d3a203`) and the firmware in QEMU. It used the input and the `tag_processor` of this document. Another version can operate differently. Nobody tested a full UMH Core installation (the `uns` output, the Topic Browser, the downsampler). [The E2E scenarios](../reference/e2e-scenarios.md#umh-umh-core-interoperability-175) gives the checks of that test.

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

| Option | Gateway: the card **MQTT** of the tab **Configuration** | UMH Core |
|---|---|---|
| A. No TLS on a protected network | **Encrypt (TLS)** off, **Port** 1883 | `tcp://<broker>:1883` |
| B. A listener on the loopback interface for UMH, TLS for the gateway | **Encrypt (TLS)** on. In **CA file**, select your private CA | `tcp://127.0.0.1:1883`. UMH Core and the broker must run on the same computer |
| C. A broker certificate from a public CA | **Encrypt (TLS)** on. In **CA file**, select `(none: public root CAs)` | `ssl://<broker DNS name>:8883` |

For the TLS steps and the load of the CA file, refer to [Connect to a broker with TLS](connect-to-a-broker-with-tls.md).

Option C works only when a CA that the two sides trust signs the broker certificate:

- Without a CA file (`mqtt.tls.ca_file: ""`), the gateway uses the public CA bundle of ESP-IDF (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN`). This bundle is compile-only: the E2E uses its own CA.
- The broker needs a public DNS name. The gateway compares the name with **Broker** (`mqtt.host`), or with **Server name** (`mqtt.tls.server_name`).
- On a plant network with no access from the Internet, a CA such as Let's Encrypt can sign the certificate with the DNS-01 challenge. Nobody examined this option with the gateway yet.
- TLS needs a valid time on the gateway (SNTP or the RTC). Refer to [Known limitations](../reference/known-limitations.md#tls-mqtt-4-mqtt-5).

## Select the names

UMH makes the location path of a device from its Sparkplug `device_id`. The gateway makes the `device_id` from `sparkplug.device_prefix` and the device name: `<device_prefix>:<name>`.

1. Write the location of each device as levels, for example `acme`, `berlin`, `packaging`, `line3`, `meter1`.
2. Put the levels that are the same for all devices of the gateway in `sparkplug.device_prefix`, with `:` between them. For example: `acme:berlin:packaging:line3`.
3. Put the remaining levels in the device name, for example `meter1`. If the devices are on two lines, use `line3:meter1` and `line4:meter1`, and keep `line3` out of the prefix.
4. Use only `A-Z`, `a-z`, `0-9`, `-` and `_` in a level.
5. Do not use spaces. UMH changes each space to `_`.
6. Do not use `.` in a level. UMH reads `.` as one more level.
7. Do not start a level with `_`. UMH reads such a level as the data contract.
8. Keep each device name at 39 characters or less. The full `device_id` must fit in the topic. With a `group_id` and an `edge_node_id` of 32 bytes, the limit is 78 bytes (refer to the [reference](../reference/umh-names-and-topics.md#name-limits-of-the-gateway)).
9. Write the `edge_node_id` in the same form, for example `acme:berlin:packaging:gw01`. Keep it at 32 characters or less.
10. In tag names, use `/` between the levels of a virtual path. For example: `L1/Current`.
11. Do not use `:` in tag names. UMH splits a name at the last `:` before it examines `/`.

## Configure the gateway

Do these steps in the tab **Configuration** of the web UI. For the cards and the bar at the bottom of the window, refer to [Change the configuration in the web UI](change-the-configuration.md#change-the-configuration-in-the-web-ui).

1. Open the card **Sparkplug**.
2. In **Naming**, select `umh`. The UMH preset refuses the names that UMH would change. It also publishes the state of each device and plain metrics that UMH can read.
3. In **Group ID**, type the group, for example `acme`. UMH does not use it in the location, but the bridge subscribes to it.
4. In **Edge node ID**, type the location of the gateway, for example `acme:berlin:packaging:gw01`.
5. Keep **Primary host ID** empty.
6. In **Device prefix (location)**, click **+ level** one time for each common level of the location. Type one level in each field, for example `acme`, `berlin`, `packaging` and `line3`. The gateway puts `:` between the levels.
7. In **Store & forward**, select **Keep the changes without a session** if you want the data of a broker failure.
8. Open the card **MQTT**. Make sure that **Protocol** is `3.1.1` (the default).
9. Open the card **Devices and tags**. In the **Name** of each device, type the remaining levels of its location, for example `meter1`.
10. Below each device, read the Sparkplug topic and the UMH topic. Compare the UMH topic with the location that you want. A level in red starts with `_`: correct it.
11. If you want, select a **Device type** for a device (refer to [Add devices of one type](add-devices-of-one-type.md)). With the UMH preset, a device with a type publishes plain metrics, not a template instance.
12. Do not use tags of the type `bytes`. UMH does not show them.
13. Do not use `uint64` or `int64` tags for values larger than 2^53 (9 007 199 254 740 992). The `tag_processor` rounds them (refer to [Examine the result](#examine-the-result)).
14. In the bar at the bottom of the window, click **Validate**. Read the warnings, and correct each one.
15. Click **Save and restart**.

In the tab **JSON**, in `PUT /api/config` or in the shell command `config load`, the same settings are these keys (refer to [Save and export](../reference/configuration.md#save-and-export)):

```json
"mqtt":      { "protocol": "3.1.1" },
"sparkplug": { "group_id": "acme", "edge_node_id": "acme:berlin:packaging:gw01",
               "device_prefix": "acme:berlin:packaging:line3", "naming": "umh",
               "primary_host_id": "", "store_forward": true },
"devices":   [ { "name": "meter1" } ]
```

The other keys of the `mqtt` object and of each device do not change for UMH. For the device keys, refer to [Add a Modbus device and its tags](add-a-modbus-device.md).

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
| `role: "secondary_active"` | The bridge sends a rebirth request when it gets DATA with no BIRTH, for example after a restart of UMH. The default `secondary_passive` never sends it. The tags then show as `alias_<n>`, and a negative integer can show as a large number, because NDATA and DDATA have no datatype (#159) |
| `include_edge_node_in_location: false` | The `device_id` (the `device_prefix` and the device name) already holds the full location. With `true`, UMH puts the `edge_node_id` before it |
| `return null` | A deliberate drop in the `tag_processor` |
| `msg.meta.timestamp_ms = msg.meta.spb_timestamp` | Without this line, UMH uses the arrival time. The store & forward samples (`is_historical`) then all get the time of the replay (measured, #175) |

Do not use the role `primary`. It publishes the Sparkplug 2.x STATE text (`ONLINE`), and the gateway accepts only the 3.0 JSON STATE (#177).

## Examine the result

1. Open the Topic Browser of UMH.
2. Find the location of the device, for example `acme.berlin.packaging.line3.meter1`.
3. Compare the topics with the table below. The table uses the device `meter1` with the tags `Voltage`, `L1/Current` and `Run`.
4. In the tab **Status** of the web UI, in the card **Network and MQTT**, click **Rebirth**. The shell command `rebirth` and `POST /api/rebirth` do the same.
5. Make sure that the topics show the values again, with no `alias_<n>` tag.

| Metric | UMH topic |
|---|---|
| Device tag `Voltage` | `umh.v1.acme.berlin.packaging.line3.meter1._raw.Voltage` |
| Device tag `L1/Current` | `umh.v1.acme.berlin.packaging.line3.meter1._raw.L1.Current` |
| Device tag `Run` | `umh.v1.acme.berlin.packaging.line3.meter1._raw.Run` |
| Node metric `DI/1` | `umh.v1.acme.berlin.packaging.gw01._raw.DI.1` |
| Node metric `Devices/meter1/Online` (UMH preset) | `umh.v1.acme.berlin.packaging.gw01._raw.Devices.meter1.Online` |
| Node metric `Node Info/Uptime` | `umh.v1.acme.berlin.packaging.gw01._raw.Node_Info.Uptime` |

The E2E of #175 measured these items with v0.16.2:

- Each value of the types `bool`, `int16`, `uint16`, `int32`, `uint32`, `float32`, `float64` and `string` gets to the UNS exact, also negative values.
- A `uint64` or `int64` value gets to the UNS exact up to 2^53. The `tag_processor` runs JavaScript, where each number is a double. Thus 9007199254740993 becomes 9007199254740992.
- After a restart of UMH, the bridge sends the rebirth request in less than 1 s. The topics come back with their names.

The [reference](../reference/umh-names-and-topics.md#node-metrics) gives all the metrics.

## Use Modbus TCP as a second path

The `modbus` input of UMH uses only TCP (`TCP`, `RTUoverTCP`, `ASCIIoverTCP`). It cannot use serial RTU. The Modbus TCP server of the gateway lets UMH poll the RTU devices directly. Refer to [`modbus_tcp`](../reference/configuration.md#modbus_tcp).

1. In the tab **Configuration** of the web UI, open the card **Modbus TCP**.
2. Select **Enabled**. Keep **Port** empty for the default port 502.
3. In **Allowed clients**, click **+ client**. Type the address of UMH Core, for example `192.168.1.20`.
4. In the bar at the bottom of the window, click **Validate**. Then click **Save and restart**.
5. In UMH, set `controller` of the `modbus` input to `tcp://<gateway>:502`.
6. Set `transmissionMode` to `TCP`.
7. Set `slaveIDs` to the unit IDs of the devices.

In the tab **JSON**, steps 1 to 3 are `"modbus_tcp": { "enabled": true, "allowed_clients": ["192.168.1.20"] }`.

This path has these limits:

- There is no report by exception. UMH reads each value at each poll.
- There is no store & forward. Each value has the time of the UMH read.
- There is no authentication. `allowed_clients` only filters the IP addresses.
- Each request goes through the task that owns the bus, between two poll requests. Thus it adds load to the RS485 bus.

## If there is a problem

| Symptom | Probable cause | Action |
|---|---|---|
| The tags show as `alias_<n>` | The bridge has no BIRTH of the node, for example after a restart of UMH | Set `role: "secondary_active"`. Click **Rebirth** in the tab **Status** of the web UI. The `tag_processor` of this document drops the `alias_<n>` tags |
| A negative value shows as a large number, for example 4294967295 for -1 | The bridge has no BIRTH of the node. NDATA and DDATA have no datatype, thus the bridge cannot know that the integer has a sign | Set `role: "secondary_active"`. Click **Rebirth** in the tab **Status** of the web UI |
| A device has no data | The device has a `type`, and the UMH preset is off. The tags are `bytes`. The device is offline: its values are null | In the card **Sparkplug**, set **Naming** to `umh`. Use other tag types. Read `Devices/<name>/Online`, or the card **Modbus devices** in the tab **Status** |
| A level is at the wrong place | The name has a space, a `.` or a level that starts with `_` | In the card **Sparkplug**, set **Naming** to `umh`. Correct each name that the validation refuses. The metadata `spb_original_device_id` shows the name before UMH changed it |
| An enterprise has the name of the gateway, for example `GW01` | The `edge_node_id` is a short name. UMH uses it as the location of the node metrics | In the card **Sparkplug**, type the location in **Edge node ID**. Or drop the node metrics in the bridge |
| The device is the enterprise, for example `umh.v1.Meter1` | The `device_id` is not a location. Two gateways with `Meter1` then write to the same topic | In the card **Sparkplug**, put the location of the devices in **Device prefix (location)** |
| The gateway sends nothing, and the web UI shows "Waiting for a valid clock" | The gateway has no valid time: no SNTP, no RTC time and no time from the browser. It does not connect to the broker before it has one (#160) | Make sure that the gateway gets SNTP. Or, in the tab **Status** of the web UI, click **Set the time from the browser**. Type `status` on the gateway shell: the `time` and `clock` lines show the source and what to do |
| After each power cut, the gateway waits for a valid clock, and the web UI tells you to examine the RTC battery | The RTC lost the time: the holder has no cell, or the cell is empty | Install a rechargeable 3 V cell (ML1220). Do not use a CR1220. Refer to [Fit the RTC battery](fit-the-rtc-battery.md) |
| A large counter shows a different last digit | A `uint64` or `int64` value is larger than 2^53. The JavaScript of the `tag_processor` rounds it | Keep the value at 2^53 or less |
| The samples of a broker failure have the time of the replay | The `tag_processor` has no `timestamp_ms` line | Add `msg.meta.timestamp_ms = msg.meta.spb_timestamp` |
| After a broker failure, the UNS shows an old value for a short time | The gateway sends the DBIRTH with the current value first, then the stored samples (`is_historical`). The last message of the topic is then the last stored sample | Use the timestamps, not the order of arrival. The next change of the value corrects the last value |
| The gateway sends no NBIRTH, and the shell says that it waits for the primary host | `sparkplug.primary_host_id` is not empty | In the card **Sparkplug**, delete the text of **Primary host ID** |

The UMH downsampler has the option `late_policy` (`passthrough` or `drop`) for samples that arrive out of order. A replay of store & forward sends samples that are older than the DBIRTH. Nobody examined the effect of `late_policy` on them: the E2E of #175 has no downsampler.
