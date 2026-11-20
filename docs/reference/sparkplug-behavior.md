# Sparkplug behavior

This document gives the choices of the gateway where the Sparkplug B 3.0.0 specification permits a choice (MAY), or sets no rule. Each item gives the reason and, when a rule applies, its `tck-id`.

- For the topics, the messages, the metrics and the commands, refer to [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md).
- For the limits of the gateway, refer to [Known limitations](known-limitations.md).
- For the Eclipse Sparkplug TCK, refer to [The Sparkplug TCK](#the-sparkplug-tck-edge-node-profile-176).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The host tests, the decode with the official proto, the TCK and the E2E in QEMU examine the behaviour.

The gateway obeys each MUST, MUST NOT, SHOULD and SHOULD NOT of the specification for an Edge Node. A deviation from them is a bug, not a choice.

## The session

### bdSeq (SPB-3, #161)

- The bdSeq is in RAM only. It is 0 at each boot, and it increases by exactly 1 for each MQTT CONNECT packet (`tck-id-topics-nbirth-bdseq-increment`).
- Reason: a connection attempt writes nothing to the flash.
- An attempt that sends no CONNECT packet uses no value. Examples: a DNS, TCP or TLS failure.
- Choice: an attempt with an unknown result keeps its value, for example a CONNECT write that failed, or no CONNACK. The next CONNECT gets the next value.
- Reason: the broker can publish the Will of that attempt. A host must not take it for the NDEATH of a newer session.
- After a restart, the bdSeq is 0 again, also when the session before the restart had 0. The broker publishes the Will of the old connection when the new connection with the same `client_id` takes the session. Thus the old NDEATH arrives before the new NBIRTH.

### No session without a valid clock (TIME-1, #160)

Decision of the product owner (#160): the gateway opens no MQTT session until the clock is valid. There is no time limit.

- Reason: after the CONNECT, an Edge Node must publish its NBIRTH (`tck-id-message-flow-edge-node-birth-publish-connect`). The NBIRTH timestamp is the time of the publish, in UTC (`tck-id-topics-nbirth-timestamp`, `tck-id-payloads-nbirth-timestamp`).
- All the timestamps are in UTC (`tck-id-payloads-timestamp-in-UTC`, `tck-id-payloads-metric-timestamp-in-UTC`). A clock that counts from 1970 cannot give them.
- `gw_node` also refuses an NBIRTH without a valid clock, as a last guard (`test_node`).
- The wait uses no bdSeq. The first CONNECT after the wait has bdSeq 0.
- A value that the gateway read before the clock was valid gets the time of the message in the first births. It does not get its 1970 time.
- The clock cannot become not valid again. If this occurred, the node would send no NDATA or DDATA, and a rebirth would end the session.
- The cost: the hosts do not see the node until the clock is valid. The changes before that are not in the history.

For the sources of the clock and the signals, refer to [The clock and the RTC battery](clock-and-rtc-battery.md).

### No spontaneous rebirth

The gateway sends a rebirth only on request: the NCMD `Node Control/Rebirth`, the shell command `rebirth` or `POST /api/rebirth`. When SNTP synchronizes a clock that the RTC or the browser set, the gateway sends no rebirth. The timestamps were already valid.

### Failed births (#71, #164)

- The session is up only when the NBIRTH and all the DBIRTHs went out. If the connection drops immediately after the SUBACK, the gateway starts no births.
- If the publish of a birth fails while the connection stays open, the gateway ends the session. It publishes an NDEATH if the hosts saw the NBIRTH. Then it disconnects.
- The next attempt has a new CONNECT, a new bdSeq and all the births. This applies to the births of a new session and to a rebirth.
- An NBIRTH that went into the socket immediately before a cut can be lost on the network. The host finds this from the bdSeq of the next session.

### A clean session on each connection (#186)

- Each connection is a clean session: Clean Session = 1 with MQTT 3.1.1, and Clean Start with Session Expiry Interval 0 with MQTT 5.
- esp-mqtt 1.1.0 keeps its outbox after a disconnection. At the next CONNECT, it sends each item again, also to another broker of the list.
- Thus an explicit QoS 1 NDEATH could go out again, with DUP 1 and the old bdSeq (`tck-id-operational-behavior-primary-application-state-with-multiple-servers-walk`).
- When the outbox is not empty at a reconnection, the gateway replaces the MQTT client with a new one. The log gives `… new MQTT client, nothing of the old session goes out again`.
- The data messages are QoS 0, and they never go into the outbox. The upstream report is #244.

### MQTT 5 (MQTT-8, #155)

- The gateway uses only the MQTT 5 functions that Sparkplug needs: Clean Start, Session Expiry Interval 0, Will Delay Interval 0, and the reason codes of CONNACK and SUBACK.
- A reason code of 0x80 or more is a rejection, as in MQTT 3.1.1. The gateway does not use topic aliases, message expiry, user properties or shared subscriptions.
- Each DISCONNECT of the gateway has the reason code 0x04, Disconnect with Will Message (`tck-id-payloads-ndeath-will-message-publisher-disconnect-mqtt50`).
- This applies to an NCMD Reboot, a restart, a change of broker or interface, and a session that the gateway closes.
- Thus the broker also publishes the NDEATH of the Will. After an explicit NDEATH, the hosts get two NDEATH messages with the same bdSeq. The second one has no new data.
- When the gateway closes a session before the NBIRTH, the hosts get an NDEATH with a bdSeq that no NBIRTH had. A host ignores it.

### The Maximum Packet Size of the broker (#73, #76)

- The gateway obeys the Maximum Packet Size that the broker gives in the CONNACK. It does not send a larger message.
- The log gives the topic and the size, one time per minute at most. The shell `status` gives a `limit` line with the count and the last topic.
- A DBIRTH that does not go out leaves its device not born: the device has no DDATA. The host does not see the device until you make its tags smaller or increase the limit.
- The gateway tries that DBIRTH one time in each session, and one more time at each rebirth. It does not build it again at each poll cycle.
- An NBIRTH that does not fit closes the session, as a rejected SUBSCRIBE does.
- The gateway does not read the Receive Maximum, because the publishes are QoS 0.

## The primary host application (SPB-13)

For the STATE messages and the results, refer to [The primary host application](sparkplug-messages-and-metrics.md#the-primary-host-application).

- While the gateway waits for the STATE `online: true`, the MQTT session is up and subscribed, but the node is not born. There is no NBIRTH or DATA message, and the gateway discards NCMD and DCMD.
- With one broker, a host that never publishes its STATE makes the node wait with no time limit. The specification requires this. The shell gives `host primary 'X' not seen yet, NBIRTH waits for it`.
- The gateway keeps the last accepted timestamp for each broker of the list (#157, `tck-id-operational-behavior-host-application-multi-server-timestamp`). Reason: the host keeps one STATE timestamp for each MQTT server.
- These timestamps are in RAM. After a restart, the gateway accepts any STATE.
- A valid offline STATE is not a failed attempt. With backup brokers, the gateway goes to the next broker at once and ignores `failover_after` (#156, `tck-id-operational-behavior-edge-node-termination-host-offline-reconnect`).
- If the host is offline on all the brokers, the gateway goes from one broker to the next after the minimum backoff. This does not count for the "no broker reachable" state of the interface (NET-4), because the network operates.
- The gateway reads only the STATE of Sparkplug 3.0.0. It ignores the `ONLINE` and `OFFLINE` texts of Sparkplug 2.x. For UMH Core, refer to [Connect the gateway to UMH Core](../how-to/connect-to-umh-core.md).

## The devices and their state (MB-9, #61, #214)

- A device starts offline. It goes online, or back online, only after a full cycle with responses. Thus its DBIRTH never has values that the gateway did not read.
- A cycle fails when a request has no response after the retries. A Modbus exception counts as a response. The cycle stops there, and does not use the timeout on the other blocks.
- The failed cycles count for `offline_after`. Thus the DDEATH arrives in `offline_after` cycles, also when the other blocks of the device respond.
- The tags of the blocks that the gateway did not read keep the value and the timestamp of their last read. The gateway does not report them as fresh. If the problem continues, the DDEATH invalidates all of them.
- The gateway does not use the quality STALE (500). A device that stops its responses goes through DDEATH, not through old values.

### The probe of an offline device (#61)

- Each probe sends only the first request of the cycle, with one attempt and no retries.
- The next probe is at 2, 4, 8 … × `poll_ms`, up to `CONFIG_GW_MB_OFFLINE_PROBE_MAX_MS` (10 s by default; never less than `poll_ms`).
- If the device responds, the full cycle continues in the same pass. The DBIRTH arrives at most one maximum interval plus one cycle after the device responds again.
- With the default values, a dead device uses the bus for 200 ms each 10 s.
- These devices are offline: a device after `offline_after` failed cycles in sequence, and a device that never responded since the boot, from its first cycle.
- The cost: if the first request of a slow slave is lost, its DBIRTH arrives 2, 4 … × `poll_ms` later, not one cycle later.

### A disabled device (#214)

- A device with `enabled: false` stays in the configuration, and the validation examines it. The poll never selects it. It is never online, so it has no DBIRTH, DDATA or DDEATH.
- A rebirth sends the DBIRTHs "if applicable" (`tck-id-operational-behavior-data-commands-rebirth-action-2`): a disabled device has none.
- The gateway discards a DCMD for it, with the log line `DCMD for disabled device '<name>' discarded`. Reason: a host cannot know its metrics without a DBIRTH (`tck-id-operational-behavior-data-commands-dcmd-metric-name`).
- A disabled device does not count as offline for the LED, the buzzer alarm `device_offline` and the shell line `Modbus n/m devices online, k disabled`.
- Modbus TCP requests to its unit still go to the bus. With the UMH preset, its metric `Devices/<name>/Online` stays `false`.

## Quality and properties (SPB-16)

- A failed read gives `is_null` with `Quality` 0 (BAD) in the DDATA. The next good read gives the value with `Quality` 192 (GOOD).
- The DBIRTH has `Quality` in each metric. A DDATA has it only when it changes. A host that misses a DDATA gets the quality again with the next change or a rebirth.
- The gateway ignores the properties in an NCMD or a DCMD. A property string of more than 63 characters stops the decode, and the gateway discards all of the command.

## Commands

- The gateway discards a DCMD to an offline device. It does not put it in the queue of the bus.
- Each write is retried as any other transaction. FC05, FC06, FC15 and FC16 are idempotent. But a slave that executes the write and does not respond receives the write two times.
- For the size limits of a command, refer to [Known limitations](known-limitations.md#commands).

### Write limits (#196)

- The host must send a compatible value (`tck-id-operational-behavior-data-commands-dcmd-metric-value`). Sparkplug has no answer to a command. Thus the gateway discards only the metric out of the limits, as each metric that is not valid. It writes the other metrics of the command, and the next DDATA has the value of the device.
- The comparison is exact for `int64` and `uint64` tags: the limit is a double, and the gateway compares its integer part.
- For a tag that is a Float in Sparkplug, the comparison uses the Float precision. Thus the limit 0.1 accepts the Float 0.1.
- NaN is never in the limits.
- The validation refuses the limits on `bool`, `string` and `bytes` tags, and on a tag that is not writable.

### `Node Control/Rebirth` has no alias (#153)

The NBIRTH gives `Node Control/Rebirth` without an alias (`tck-id-operational-behavior-data-commands-rebirth-name-aliases`). Thus each host sends the rebirth request by name. The alias 2 stays reserved, and the gateway also accepts an NCMD with the alias 2 as a rebirth request.

### `Node Control/Scan Rate` (SPB-15)

- The value replaces the poll period, it does not scale it. A value from 50 to 3 600 000 ms replaces the `poll_ms` of all the devices.
- It also replaces their `slow_poll_ms` when that value is smaller (MB-12). The offline devices do their backoff from the current period.
- A shorter period applies at once. A longer period applies after the poll that is already scheduled.
- The gateway discards a value out of the range or not an integer, with a log line. An NDATA then gives the current value.
- The gateway does not keep the value: after a restart, it is 0 again. Reason: the configuration is the source of truth, and each NCMD would wear the flash.

## Diagnostics (SPB-17, #81)

- `Node Info/Free Heap` is the internal RAM. The PSRAM does not count, because the internal RAM is the memory that becomes full.
- The Modbus TCP counters of each unit go in a separate DataSet, `Node Info/Modbus TCP` (alias 56), only with `modbus_tcp.enabled`.
- That table has one more row for the units without a device: up to 17 rows. With 16 rows, it adds approximately 1.6 KB to each NDATA that has it.

## Templates (SPB-18)

- Choice (MAY): the NBIRTH has a definition only for each type that a device uses, also a disabled device. A type that no device uses has no definition.
- Reason: `tck-id-payloads-template-definition-nbirth` asks for the definitions of the instances only. More definitions make the NBIRTH larger. The limit of 64 tags in the definitions counts only these types.
- The gateway uses no template version and no nested templates. It does not go into a member that is a template in a command.
- There are no templates for the node metrics (DI, DO, counters).
- A device with `type` publishes one instance, not one metric per tag. For example, a host that subscribed to `Meter1/Voltage` sees `Meter1/<type>/Voltage` (in Ignition: a UDT instance). The `type` is optional for each device.
- The properties of the definition and of each instance come from the tags of the type (#213).
- No test used a real Sparkplug host such as Ignition. The tests use the official proto, the rules of the specification and the Sparkplug TCK.

### The template parameter `Unit ID` (#251)

Each template has one parameter: the Modbus unit ID of the device. A parameter is a fixed value of an instance that is not a measurement. A host such as Ignition can show it as a property of the UDT instance.

- The value comes from `devices[].unit_id`. There is no configuration key for the parameter.
- **The name is `Unit ID`.** The specification asks only for a UTF-8 text (`tck-id-payloads-template-parameter-name-required`, `tck-id-payloads-template-parameter-name-type`). `Unit ID` is the label of the key `unit_id` in the web UI. It has the form of the other Sparkplug names of the gateway, for example `Scan Rate` and `Failed Cycles`.
- **The type is Int32**, as the property `Quality`. Int32 is a basic datatype, and its value goes in `int_value` (`tck-id-payloads-template-parameter-type-value`, `tck-id-payloads-template-parameter-value`).
- **Each definition has the parameter**, with its name and its type (`tck-id-payloads-template-definition-parameters`, `tck-id-payloads-template-parameter-type-req`).
- Choice (MAY, `tck-id-payloads-template-definition-parameters-default`): the definition gives the default 0.
- Reason: 0 is not the unit ID of a device (1 to 247). Thus a host can see that an instance gave no value. The TCK 3.0.0 also examines `tck-id-payloads-template-parameter-value` on a definition, and a parameter without a value does not pass.
- Choice (MAY, `tck-id-payloads-template-instance-parameters`): the instance of each DBIRTH gives the unit ID of its device. Thus no instance uses the default.
- A rebirth gives the parameter again, in the NBIRTH and in each DBIRTH.
- Choice (MAY): the instance of a DDATA has no parameters, also in a store & forward replay.
- Reason: the unit ID changes only with a new configuration, and a new configuration restarts the gateway. The DBIRTH gave the value, and a DDATA has only the changes.
- The specification gives the default of a definition to an instance without a parameter value "in the NBIRTH or DBIRTH messages". Thus the default does not apply to the instance of a DDATA.
- The gateway ignores the parameters of an instance in a DCMD. A parameter is not a tag, so a DCMD cannot write it. The gateway writes the members of that DCMD as usual.
- With the UMH preset there are no templates, so there are no parameters.

## DataSet, Bytes and File (SPB-19)

- Bytes and File hold 64 B at most per value, in a fixed buffer on the stack of the codec. The gateway does not publish a longer value: the payload fails, with a log line.
- The gateway does not decode a command with a longer value, and discards all of the command.
- File has no `metadata` (name, size, md5), and the gateway does not split a file into parts. There is no file transfer, because REL-2 requires bounded memory.
- The columns of a DataSet can be numeric, Boolean, String or Text only. The proto also does not accept Bytes, DataSet or Template in a cell.
- A cell text of more than 63 characters makes the encode of that message fail, with a log line (#87). The only source, the device names, is shorter.
- At the decode, the gateway gives the names and the cells through callbacks, and keeps the first 16 types.

## Names, the device prefix and the UMH preset

For the reasons, refer to [Sparkplug names and the UNS](../explanation/sparkplug-names-and-the-uns.md). For the name limits, refer to [UMH names and topics](umh-names-and-topics.md).

### The device prefix (#172)

- With `sparkplug.device_prefix`, the `device_id` is `<device_prefix>:<name>` in each DBIRTH, DDATA, DDEATH and DCMD topic. A DCMD must use the full `device_id`.
- The prefix has no fixed number of levels, because UMH Core permits a location path of free depth.
- The engine writes the prefix into the topic buffer of the message (`GW_TOPIC_MAX`, 160 B). It keeps no copy of each `device_id`, so the publish path has no allocation. The validation makes sure that the longest topic fits.
- The metric names do not get the prefix. The `Device` column of `Node Info/Modbus` and `Node Info/Modbus TCP` has the name without the prefix.
- The prefix is in the configuration hash of the microSD ring. Refer to [When the gateway starts a new ring](store-and-forward.md#when-the-gateway-starts-a-new-ring).

### Name checks (#169)

- Choice (MAY): Sparkplug permits these names. The ID rules give only the `/`, `+` and `#` limits (`tck-id-topic-structure-namespace-valid-group-id`, `tck-id-topic-structure-namespace-valid-edge-node-id`, `tck-id-topic-structure-namespace-valid-device-id`).
- Thus the default behaviour accepts the names, with a warning. Only the UMH preset, which the user selects, refuses them.
- `gw_config_lint()` (`gw_cfglint.c`) runs only at the validation, never in the publish path. It allocates nothing: it gives each finding to a callback.
- `PUT /api/config` gives the first 50 warnings and `warning_count`. The shell `config check` and `config load` give 50 warnings at most. The boot log gives the first 16 and the count.
- With the UMH preset, a `primary_host_id`, a `type` and a `bytes` tag stay warnings. UMH cannot read them, but another host of the same broker can.

### The UMH preset (#173, #174)

- **Device state.** Choice (MAY): the specification permits node metrics with any name. The preset puts the state of each device in a node metric, because UMH Core does not use the DDEATH (benthos-umh `sparkplug_b_input.go`).
- The value is the Modbus state of the device (MB-9). It is `true` also when the DBIRTH is too large for the broker (#76).
- Store & forward does not keep a change of the device state. The next NBIRTH gives the current state.
- Each state metric adds approximately 32 B plus the length of the device name to the NBIRTH. 16 device names of 39 characters add approximately 1.1 KB.
- **Plain metrics.** A device with `type` publishes plain metrics. The NBIRTH has no template definition, because no device is an instance (`tck-id-payloads-template-definition-nbirth`).
- **Sums, not one metric for each device.** The NBIRTH and the diagnostic NDATA have the sums of the Modbus counters, as UInt64. A sum of 16 UInt32 counters can be larger than 2^32.
- Reason for the sums: 16 devices × 12 counters would add 192 metrics. The counters of each device stay in the shell (`devices`), in `GET /api/status` and in the DataSets without the preset.
- The host test `test_umh_worst_case_nbirth_fits` encodes the worst NBIRTH of the preset in a buffer of 6 KB, the payload buffer of the QEMU build without PSRAM. It has 16 devices with names of 39 characters, RSSI, Modbus TCP and `ota.mqtt`.
- The preset is in the configuration hash of the microSD ring. Reason: a replay publishes a typed device as an instance or as plain metrics.

## The Sparkplug TCK (Edge Node profile, #176)

The optional E2E scenario `tck` runs the Edge Node profile of the Eclipse Sparkplug TCK 3.0.0 against the firmware, with MQTT 3.1.1 and MQTT 5. For the procedure, refer to [Run the Sparkplug TCK against the gateway](../how-to/run-the-sparkplug-tck.md). For the steps of the scenario, refer to [The E2E scenarios](e2e-scenarios.md#tck-the-sparkplug-tck-176).

### UInt32 in `int_value`: two FAILs of the TCK 3.0.0

`tck-id-topics-dbirth-metrics` and `tck-id-operational-behavior-data-publish-dbirth-values` fail on a UInt32 tag of a device (UInt8, UInt16 and UInt32 too). With UInt32 node metrics, `tck-id-topics-nbirth-metrics` and `tck-id-operational-behavior-data-publish-nbirth-values` fail in the same way.

- The two rules ask for "the metric name, datatype, and current value". They name no value field.
- The datatype table of the specification (chapter Payloads) gives "UInt32: Google Protocol Buffer Type: uint32". In the proto, `int_value` is the only uint32 field of the `value`. The gateway puts a UInt8, UInt16 or UInt32 value in `int_value` (`spb.c`).
- `Utils.hasValue()` of the TCK 3.0.0 (`tck/.../test/common/Utils.java`) accepts only `long_value` for UInt32. Thus the TCK finds "no value" in a correct metric.
- On receive, the gateway accepts a UInt32 value in `int_value` and in `long_value`.

The reference implementation does not agree with itself. Tahu Java, C and JavaScript write UInt32 in `long_value`, and Tahu Python writes it in `int_value`. The specification text did not change.

- eclipse-sparkplug/sparkplug#102 recorded the conflict. It was closed with a change of the Tahu Java decoder only (eclipse-tahu/tahu#202): the decoder reads the two fields, and the encoder still writes `long_value`.
- eclipse-sparkplug/sparkplug#469 corrected the TCK on the branch `3.x`, but no TCK was released from that branch. eclipse-sparkplug/sparkplug#529 put UInt32 back on `long_value` on `develop` (the v4 schema). eclipse-sparkplug/sparkplug#630 was closed without a merge.

Choices:

- A tag of a device keeps its datatype, for example UInt32 for a `uint32` register, with the value in `int_value`. The gateway obeys the datatype table of the specification.
- The unsigned node metrics are UInt64, with the value in `long_value`: `Node Control/Scan Rate`, `Node Info/Free Heap`, `Node Info/Min Free Heap`, `Node Info/Largest Free Block`, `Node Info/Free PSRAM` and `Firmware Update/Progress`. The specification permits any datatype for them, and UInt64 agrees with the specification and with the TCK. Thus the NBIRTH gets no FAIL (decision of the product owner, 2026-11-19).

The harness accepts these FAILs only when it proves this cause from the captured messages (`uint32_cause` in `KNOWN_RULES`, `idf/tools/e2e/tck.py`). Each metric must have a name, a datatype and the value field of the table, and the only metrics without the field that the TCK wants must be UInt32 metrics with `int_value`. A different fault of the same assertion still fails.

A host with a Tahu Java decoder from before eclipse-tahu/tahu#202 (2022) reads UInt32 only from `long_value`. Such a host does not read the UInt32 tags of the gateway (for example Ignition MQTT Engine 4.0.5). It reads the node metrics, because they are UInt64.

### Template parameters (#251)

The TCK examines the parameter assertions only when a definition has a parameter. Each definition of the gateway has the parameter `Unit ID` (refer to [The template parameter `Unit ID`](#the-template-parameter-unit-id-251)). Thus the TCK examines them:

| Test | Assertions |
|---|---|
| SendDataTest | `tck-id-payloads-template-definition-parameters`, `tck-id-payloads-template-definition-parameters-default`, `tck-id-payloads-template-instance-parameters` |
| SendComplexDataTest | `tck-id-payloads-template-parameter-name-required`, `-name-type`, `-value-type`, `-type-value`, `-type-req` and `-value` |

The TCK user guide says that templates are an optional group, and that a product that uses the group must pass all its assertions. The gateway uses templates. Thus a run of the scenario `tck` must show PASS for each assertion of the table, not NOT EXECUTED. The harness makes the check of the test fail when one of them is not PASS (`REQUIRED_PASS` in `idf/tools/e2e/tck.py`).

The TCK 3.0.0 has a defect in `Utils.hasValue(Parameter)` (`tck/.../test/common/Utils.java`). It compares the type of the parameter with the field numbers of the proto, not with the datatypes. For Int32 the two numbers are the same (3), so the function gives the correct answer for `Unit ID`. The TCK uses the function only in a debug log line, so it changes no result.
