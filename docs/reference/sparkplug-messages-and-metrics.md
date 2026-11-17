# Sparkplug messages and metrics

This document lists the Sparkplug B 3.0.0 topics, messages, metrics and commands of the gateway.

- For the configuration keys that change the messages, refer to [The configuration document](configuration.md).
- For the UMH topic of each metric, refer to [UMH names and topics](umh-names-and-topics.md).
- For the results of the Eclipse Sparkplug TCK, refer to [Sparkplug TCK results](sparkplug-tck-results.md).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The host tests and the E2E in QEMU examine the messages. The DIs, the DOs and the expander are compile-only.

## Topics

| Topic | Messages |
|---|---|
| `spBv1.0/<group_id>/<type>/<edge_node_id>` | NBIRTH, NDEATH, NDATA, NCMD |
| `spBv1.0/<group_id>/<type>/<edge_node_id>/<device_id>` | DBIRTH, DDEATH, DDATA, DCMD |
| `spBv1.0/STATE/<primary_host_id>` | STATE of the primary host (the gateway subscribes only) |

The `device_id` is the device name, or `<device_prefix>:<name>` with `sparkplug.device_prefix`. The payload is protobuf, as `sparkplug_b.proto` of Eclipse Tahu specifies. The timestamps are in ms UTC.

## Messages

| Message | QoS | Retain | Contents |
|---|---|---|---|
| NBIRTH | 0 | No | `seq` = 0. All the node metrics, with a name, an alias (not `Node Control/Rebirth`), a datatype and a value |
| NDEATH | 1 | No | The MQTT Will of the connection. Only the metric `bdSeq` (Int64). No `seq` |
| NDATA | 0 | No | The node metrics that changed, by alias, without a name and a datatype |
| DBIRTH | 0 | No | All the tags of one device, with their names, aliases, datatypes, values and properties. Only after the NBIRTH, and only for a device that is online |
| DDEATH | 0 | No | The device went offline |
| DDATA | 0 | No | The tags that changed, by alias, without a name and a datatype. The changes of one poll cycle go in one message |

- **bdSeq:** 0 to 255, in RAM only. It starts at 0 at each boot, and increases by 1 for each MQTT CONNECT. The NBIRTH has the same value as the Will of that connection.
- **seq:** 0 to 255, then 0 again. All the messages of the node use one sequence, except the NDEATH.
- **Aliases:** UInt64, unique in the node.
- Before an intentional disconnection, the gateway publishes the NDEATH.
- If the publish of a birth fails, the gateway stops the session. The next connection sends all the births again.

## Node metrics

The NBIRTH contains these metrics.

| Metric | Alias | Datatype | What it is |
|---|---|---|---|
| `bdSeq` | 1 | Int64 | The birth and death sequence number |
| `Node Control/Rebirth` | — | Boolean | Writable. Always `false` in the NBIRTH. No alias |
| `Node Control/Reboot` | 3 | Boolean | Writable |
| `Node Info/Firmware` | 4 | String | The version and the git hash of the firmware |
| `Node Info/Hardware` | 5 | String | The name of the board |
| `Node Info/Reset Reason` | 6 | String | The cause of the last restart |
| `Node Control/Scan Rate` | 7 | UInt32 | Writable. ms. 0 = each device uses its `poll_ms` |
| `Node Info/Name` | 8 | String | `gateway.name`. Only when it is set. Only in the NBIRTH |
| `Node Info/Location` | 9 | String | `gateway.location`. Only when it is set. Only in the NBIRTH |
| `DI/1` to `DI/8` | 10 to 17 | Boolean | The digital inputs, after the debounce |
| `DO/1` to `DO/8` | 20 to 27 | Boolean | Writable. The digital outputs. Null and BAD quality while the expander does not respond |
| `Counter/1` to `Counter/8` | 30 to 37 | UInt64 | The pulse counters of the DIs |
| `Node Control/Firmware Update` | 40 | String | Writable. Empty in the NBIRTH |
| `Firmware Update/State` | 41 | String | The result of the last update |
| `Firmware Update/Progress` | 42 | UInt32 | % |
| `Node Control/Boot Other Slot` | 43 | Boolean | Writable |
| `Node Info/Uptime` | 50 | UInt64 | The seconds since the boot |
| `Node Info/Free Heap` | 51 | UInt32 | The free bytes of the internal RAM |
| `Node Info/Min Free Heap` | 52 | UInt32 | The minimum free internal RAM since the boot |
| `Node Info/RSSI` | 53 | Int32 | dBm of the Wi-Fi network. Only with a Wi-Fi configuration. Null while the gateway is not associated |
| `Node Info/Modbus` | 54 | DataSet | The Modbus counters, one row for each device |
| `Node Info/Time Synced` | 55 | Boolean | `true` when SNTP synchronized the clock at least one time since the boot |
| `Node Info/Modbus TCP` | 56 | DataSet | Only with `modbus_tcp.enabled`. The Modbus TCP counters |
| `Node Info/Core Dump` | 57 | Boolean | `true` while a core dump is in flash |
| `Node Info/Largest Free Block` | 58 | UInt32 | The largest free block of the internal RAM, in bytes. A low value with much free heap shows a fragmented RAM |
| `Node Info/Free PSRAM` | 59 | UInt32 | The free bytes of the PSRAM |
| `Node Info/Temperature` | 68 | Float | The temperature of the chip, in °C, from its internal sensor. Null when there is no reading, for example in QEMU |

`gateway.comment` is not in the NBIRTH: only the configuration document has it. A change of the name or the location needs a restart, as each change of the configuration. Thus the gateway sends them only in the NBIRTH (`tck-id-topics-nbirth-metrics`).

### Diagnostics

The gateway publishes the diagnostics in an NDATA each `sparkplug.diagnostics_s` seconds (60 by default; 0 = only in the NBIRTH).

- The uptime, the heap, the largest free block, the free PSRAM and the temperature go in each diagnostic NDATA, with their aliases only.
- The Modbus tables go in the NDATA only if a counter or a state changed.
- `Node Info/Time Synced` and `Node Info/Core Dump` go in an NDATA of their own, each time that they change.
- The gateway also publishes the counters of a device that is offline.

The columns of `Node Info/Modbus`:

| Column | Datatype | What it is |
|---|---|---|
| `Device` | String | The device name |
| `Online` | Boolean | The state of the device |
| `Cycles`, `Failed Cycles` | UInt32 | The poll cycles |
| `OK`, `Timeout`, `CRC`, `Frame`, `Exception` | UInt32 | The attempts, with the retries |

`Node Info/Modbus TCP` has the columns `Device` (String), `Unit` (UInt32), `Online` (Boolean), and `OK`, `Timeout`, `CRC`, `Frame`, `Exception` (UInt32). The last row, `(other units)` with unit 0, is for the units that have no device.

A time from the RTC or from the browser is valid, but it is not synchronized. For the clock, refer to [The clock and the RTC battery](clock-and-rtc-battery.md).

## Device metrics

- Each tag is one metric. Its name is the tag `name`. Its alias is 100 + the index of the tag in the configuration, counted across all the devices.
- The datatype comes from the tag `type`, `scale` and `offset`. Refer to [The datatypes in Sparkplug](configuration.md#the-datatypes-in-sparkplug).
- The deadband of each tag applies to the DDATA (report by exception).

### Properties

| Property | Datatype | When |
|---|---|---|
| `engUnit` | String | The tag has `units` |
| `engLow`, `engHigh` | Float | The tag has `eng_low` and `eng_high` |
| `Quality` | Int32 | Each DBIRTH. A DDATA only when it changes |

- `Quality` is 192 (GOOD) when the read got a response. It is 0 (BAD) when the read failed, or when the gateway never read the tag. With BAD, the value is also null.
- The node DOs use the same rule: BAD while the expander does not respond.
- The gateway accepts a command with properties, and ignores the properties.

### Templates

A device with `type` publishes a Sparkplug template instance in place of one metric for each tag.

| Message | Contents |
|---|---|
| NBIRTH | One Template metric (19) for each type: the definition, with `is_definition = true`, no alias and no `template_ref`. Its members have a name, a datatype, a default value, and the properties of the first device of the type |
| DBIRTH | One Template metric, the instance. Its name is the type, its alias is 100 + the first tag of the device, and its `template_ref` is the type. Its members are the tags, by name and without an alias |
| DDATA | The instance by alias, with only the members that changed, by name |
| DCMD | The instance with the members to write, by name. The tag by name as a separate metric is also accepted. A member by alias is not accepted |

The gateway does not use template parameters.

## Commands

### NCMD

| Metric | Datatype | What it does |
|---|---|---|
| `Node Control/Rebirth` | Boolean `true` | Stops the NDATA and DDATA. Sends the NBIRTH and all the DBIRTHs again. `seq` starts again at 0, and the NBIRTH keeps the `bdSeq` of the session |
| `Node Control/Reboot` | Boolean `true` | Orderly restart, with the NDEATH |
| `Node Control/Scan Rate` | UInt32 | One poll interval for all the devices: 50 to 3 600 000 ms. 0 = each device uses its `poll_ms` |
| `Node Control/Firmware Update` | String | Downloads and installs the image at the `https://` URL (255 characters at most) |
| `Node Control/Boot Other Slot` | Boolean `true` | Restarts into the image of the other slot, on trial, after the pre-flight check |
| `DO/1` to `DO/8` | Boolean | Sets the output. A DO with a local rule refuses the command |

`POST /api/rebirth`, the web UI button **Rebirth** and the shell command `rebirth` do the same as `Node Control/Rebirth`. For the firmware commands, refer to [Update the firmware](../how-to/update-the-firmware.md).

### DCMD

The gateway writes the writable metrics to the Modbus device. It finds the tag by alias or by name.

| Rule | Result if the command does not obey it |
|---|---|
| The metric has the name or the alias of the DBIRTH | The gateway discards the command |
| The tag is writable | The gateway discards the command |
| The datatype is the datatype of the DBIRTH. A metric without a datatype uses the datatype of the DBIRTH | The gateway discards the command |
| An integer or Boolean tag gets a value without a fraction | The gateway discards the command |
| The value fits in the register after the inverse scale: `raw = (value − offset) / scale` | The gateway discards the command |

- A command applies all its metrics or none. The log gives the reason of each discard.
- The writes go before the polls in the queue of the bus.
- After the write, the gateway reads the register again and publishes the value in a DDATA. If this read fails, it publishes the value that the device acknowledged.
- The functions: FC05 for a coil, FC06 for a tag of one register, FC16 for a tag of more registers. With the build option `CONFIG_GW_MB_FORCE_MULTI_WRITE`, all the writes use FC15 or FC16.

For the procedure, refer to [Write a value to a Modbus device or a digital output](../how-to/write-to-a-modbus-device.md).

## The primary host application

With `sparkplug.primary_host_id`, the gateway subscribes to `spBv1.0/STATE/<primary_host_id>`, in the same SUBSCRIBE as NCMD and DCMD. Before the NBIRTH, it waits for a Sparkplug 3.0.0 STATE: a retained JSON `{"online": true|false, "timestamp": <ms>}`.

| STATE | Result |
|---|---|
| `online: true` | The gateway publishes the NBIRTH and the DBIRTHs |
| `online: false` during a session | The gateway publishes the NDEATH and disconnects. The next session waits again |
| A `timestamp` less than the last accepted `timestamp` of that broker | The gateway ignores it |
| A payload that is not this JSON, for example the `ONLINE` of Sparkplug 2.x | The gateway ignores it, with a warning |

- The gateway keeps the last `timestamp` for each broker of the list, between the sessions but not after a restart.
- With backup brokers, a STATE `false` makes the gateway connect to the next broker at once. This is not a failed attempt.
- With backup brokers, if the gateway receives no STATE `true` in 30 s, it disconnects. This is a failed attempt on that broker.
- With one broker, the gateway stays connected and waits.

UMH Core publishes a Sparkplug 2.x STATE. Refer to [Connect the gateway to UMH Core](../how-to/connect-to-umh-core.md).

## Store & forward

With `sparkplug.store_forward`, the gateway keeps the changes while it has no session. After the NBIRTH and the DBIRTHs of the next session, it publishes them as NDATA and DDATA, from the oldest to the newest. Each one has `is_historical = true` and the time of the sample. Refer to [Store & forward](store-and-forward.md).

## The UMH preset

With `sparkplug.naming: "umh"`, these items change. Each item obeys Sparkplug B 3.0.0.

- **Device state.** The NBIRTH has one Boolean metric for each device, `Devices/<name>/Online`, with the alias 80 + the index of the device. Each `:` of the name becomes `/`. An NDATA gives each change: `true` before the DBIRTH, `false` before the DDEATH.
- **Plain metrics.** A device with `type` publishes one plain metric for each tag, with the tag aliases. The NBIRTH has no template definitions.
- **No DataSets.** The gateway does not publish `Node Info/Modbus` and `Node Info/Modbus TCP`. The NBIRTH and the diagnostic NDATA have UInt64 node metrics with the sums of all the devices:
  - `Node Info/Modbus/Cycles`, `Failed Cycles`, `OK`, `Timeout`, `CRC`, `Frame` and `Exception` (aliases 60 to 66);
  - `Node Info/Modbus TCP/OK`, `Timeout`, `CRC`, `Frame` and `Exception` (aliases 70 to 74, with `modbus_tcp.enabled`).

  The other node metrics, for example `Node Info/Largest Free Block` and `Node Info/Temperature`, do not change.

For the reasons, refer to [Sparkplug names and the UNS](../explanation/sparkplug-names-and-the-uns.md).
