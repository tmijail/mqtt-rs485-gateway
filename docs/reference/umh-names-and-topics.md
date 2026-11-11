# UMH names and topics

This reference gives the limits of the Sparkplug names of the gateway and the characters that UMH Core keeps. It also gives the UMH topic of each metric.

- For the procedure, refer to [Connect the gateway to UMH Core](../how-to/connect-to-umh-core.md).
- For the reasons, refer to [Sparkplug names and the UNS](../explanation/sparkplug-names-and-the-uns.md).

The UMH data is from benthos-umh **v0.16.2** (commit `0d3a203`). Nobody tested it against a real UMH Core (#175).

## Name limits of the gateway

The gateway counts the length in bytes. For ASCII text, one character is one byte.

| Configuration key | Length | Characters that the gateway rejects | Use in UMH |
|---|---|---|---|
| `sparkplug.group_id` | 1 to 32 | `/`, `+`, `#`, control characters | Only the subscription filter of the bridge (`identity.group_id`) |
| `sparkplug.edge_node_id` | 1 to 32 | `/`, `+`, `#`, control characters | The location of the node metrics. With `include_edge_node_in_location: true`, also the start of the location of the device metrics |
| `sparkplug.primary_host_id` | 0 to 32 | `/`, `+`, `#`, control characters | Keep it empty. UMH cannot be the primary host (#177) |
| `sparkplug.device_prefix` | 0 to 96. No empty level | `/`, `+`, `#`, control characters | The start of the location of the device metrics. The gateway puts it before each device name, with `:` between them |
| `devices[].name` | 1 to 39 | `/`, `+`, `#`, control characters | The end of the location of the device metrics |
| The `device_id` (`<device_prefix>:<name>`, or the name) | The longest device topic must fit in 159 bytes: 78 bytes with a `group_id` and an `edge_node_id` of 32 bytes | As above | The location of the device metrics |
| `devices[].type` | 1 to 39, or absent | `/`, `+`, `#`, control characters | Do not use it. UMH shows no value for a template instance |
| `devices[].tags[].name` (the metric name) | 1 to 39 | None. The name must be unique in the device | The virtual path and the tag name |

The sources are `GW_NAME_SIZE` (40), `GW_ID_SIZE` (33), `GW_PREFIX_SIZE` (97) and `GW_TOPIC_SIZE` (160) in `idf/components/gw_core/include/gw_config.h`, and `check_topic_id()` in `gw_config.c`. The Sparkplug rules are `tck-id-topic-structure-namespace-valid-group-id`, `tck-id-topic-structure-namespace-valid-edge-node-id` and `tck-id-topic-structure-namespace-valid-device-id`.

## Characters for UMH

| Character | Effect in UMH | Use |
|---|---|---|
| `A-Z`, `a-z`, `0-9`, `-`, `_` | UMH keeps them | Use them |
| `:` in a `device_id` or an `edge_node_id` | A new location level (`.`) | Use it between the levels |
| `.` in a `device_id` or an `edge_node_id` | A new location level | Do not use it in a level |
| `_` at the start of a level | UMH reads the level as the data contract | Do not use it |
| `/` in a metric name | A new level of the virtual path. UMH uses it if the name has no `:` | Use it between the levels of a virtual path |
| `:` in a metric name | A new level of the virtual path. UMH examines `:` first | Do not use it |
| Space, and all other characters | UMH changes them to `_` | Do not use them |

UMH also changes two or more `.` in sequence to one `.`. It removes `.` at the start and at the end of a name.

## UMH topic

UMH makes the topic `umh.v1.<location_path>.<data_contract>.<virtual_path>.<tag_name>`:

- `location_path`: the `device_id` (with the `device_prefix`) for device metrics, or the `edge_node_id` for node metrics, with `.` in place of `:`.
- `data_contract`: the value that the bridge sets. The how-to uses `_raw`.
- `virtual_path`: the part of the metric name before the last separator. It is absent if the name has no separator.
- `tag_name`: the part of the metric name after the last separator.

The tables below use the node `acme:berlin:packaging:gw01` and the device `meter1` with the `device_prefix` `acme:berlin:packaging:line3`. Thus the `device_id` is `acme:berlin:packaging:line3:meter1`. The column "Bridge example" tells what the `tag_processor` of the how-to does.

## Node metrics

The gateway publishes these metrics in the NBIRTH, and their changes in NDATA. Source: `idf/components/gw_core/src/gw_node.c`.

| Metric | Datatype | Present | UMH topic (after `umh.v1.acme.berlin.packaging.gw01._raw.`) | Bridge example |
|---|---|---|---|---|
| `bdSeq` | Int64 | Always | `bdSeq` | Dropped |
| `Node Control/Rebirth` | Boolean | Always | `Node_Control.Rebirth` | Dropped |
| `Node Control/Reboot` | Boolean | Always | `Node_Control.Reboot` | Dropped |
| `Node Control/Scan Rate` | UInt32 | Always | `Node_Control.Scan_Rate` | Dropped |
| `Node Control/Firmware Update` | String | Always | `Node_Control.Firmware_Update` | Dropped |
| `Firmware Update/State` | String | Always | `Firmware_Update.State` | Dropped |
| `Firmware Update/Progress` | UInt32 | Always | `Firmware_Update.Progress` | Dropped |
| `Node Info/Firmware` | String | Always | `Node_Info.Firmware` | Kept |
| `Node Info/Hardware` | String | Always | `Node_Info.Hardware` | Kept |
| `Node Info/Reset Reason` | String | Always | `Node_Info.Reset_Reason` | Kept |
| `DI/1` to `DI/8` | Boolean | Always | `DI.1` to `DI.8` | Kept |
| `DO/1` to `DO/8` | Boolean | Always. Null when the state of the DO expander is not known | `DO.1` to `DO.8` | Kept. A null value is dropped |
| `Counter/1` to `Counter/8` | UInt64 | Always | `Counter.1` to `Counter.8` | Kept |
| `Node Info/Uptime` | UInt64 | Always | `Node_Info.Uptime` | Kept |
| `Node Info/Free Heap` | UInt32 | Always | `Node_Info.Free_Heap` | Kept |
| `Node Info/Min Free Heap` | UInt32 | Always | `Node_Info.Min_Free_Heap` | Kept |
| `Node Info/RSSI` | Int32 | Only with `network.wifi`. Null without a Wi-Fi connection | `Node_Info.RSSI` | Kept. A null value is dropped |
| `Node Info/Modbus` | DataSet | With one or more devices | None: UMH gives no value for a DataSet | Dropped |
| `Node Info/Modbus TCP` | DataSet | Only with `modbus_tcp.enabled` | None: UMH gives no value for a DataSet | Dropped |
| `Node Info/Time Synced` | Boolean | Always | `Node_Info.Time_Synced` | Kept |
| A template definition for each device `type` | Template | Only with devices that have a `type` | None: UMH gives no value for a Template | Dropped |

`Node Info/Uptime`, `Free Heap`, `Min Free Heap` and `RSSI` also go in an NDATA each `sparkplug.diagnostics_s` (60 s by default, 0 = never). The DataSets go in this NDATA only when a counter changed.

## Device metrics

The DBIRTH and the DDATA of a device hold one metric for each tag of the device. The metric name is the tag `name`.

| Tag | Datatype | UMH topic (after `umh.v1.acme.berlin.packaging.line3.meter1._raw.`) | Bridge example |
|---|---|---|---|
| `Voltage` | From the tag `type` (`docs/SPEC.md` §6) | `Voltage` | Kept |
| `L1/Current` | From the tag `type` | `L1.Current` | Kept |
| A tag with a failed read, or of an offline device | Null, with the Quality BAD | No new value | Dropped |
| A tag of the type `bytes` | Bytes | None: UMH gives no value for Bytes | Dropped |
| All tags of a device with a `type` | One Template instance | None: UMH gives no value for a Template | Dropped |

UMH ignores the properties of each tag (`engUnit`, `engLow`, `engHigh`, `Quality`). A DDEATH gives no UMH topic, and the tags of the device keep their last value in UMH.
