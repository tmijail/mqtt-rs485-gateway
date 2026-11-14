# Sparkplug names and the UNS

This document tells why the names of the gateway control where its data goes in the Unified Namespace (UNS) of UMH Core. It also tells what UMH drops, and why the gateway keeps it.

- For the procedure, refer to [Connect the gateway to UMH Core](../how-to/connect-to-umh-core.md).
- For the limits and the topics, refer to [UMH names and topics](../reference/umh-names-and-topics.md).

The data about UMH is from benthos-umh **v0.16.2** (commit `0d3a203`). The E2E scenario `umh` (#175) runs that version against the firmware in QEMU and confirms the behaviour in this document (`idf/README.md` §4.6).

## The Sparkplug names

A Sparkplug B topic has three names: `spBv1.0/<group_id>/<type>/<edge_node_id>/<device_id>`. Each metric in the payload has its own name too.

- The specification permits any valid UTF-8 text in the three names, except `+`, `/` and `#` (`tck-id-topic-structure-namespace-valid-group-id`, `tck-id-topic-structure-namespace-valid-edge-node-id`, `tck-id-topic-structure-namespace-valid-device-id`).
- The specification gives no meaning to the names. It does not tell a host to make a location path from them.
- Thus each host makes its own map from the Sparkplug names to its data model.

The gateway uses the names as the configuration gives them. `sparkplug.group_id` is the group, `sparkplug.edge_node_id` is the node, and the `name` of each device is the `device_id`. With `sparkplug.device_prefix`, the `device_id` is `<device_prefix>:<name>`. The name of each tag is the metric name.

## The Parris Method

The Parris Method is a convention for ISA-95 locations in Sparkplug. It puts the upper levels of the location in the `group_id`, with `:` between the levels, for example `Acme:Berlin:Packaging`. The `edge_node_id` and the `device_id` then give the lower levels.

The UMH documentation says that this convention needs a separate state for each group in the host. A plant with many sites then has many groups.

## The Modified Parris Method of UMH

UMH uses a different convention. It calls it the "Modified Parris Method":

- UMH makes the location path only from the `device_id`. It changes each `:` to `.`.
- UMH does not use the `group_id` for the location. The bridge uses it only as the subscription filter.
- For node data (NBIRTH and NDATA), UMH uses the `edge_node_id` as the location.
- UMH splits a metric name into a virtual path and a tag name. It splits at the last `:`. If there is no `:`, it splits at the last `/`, and then at the last `.`.

Thus the full location must be in the `device_id`. The key `sparkplug.device_prefix` holds the levels that are the same for all devices of the gateway, and each device name holds the last levels. The same is true for the node: put the location of the gateway in the `edge_node_id`.

UMH moves away from the fixed levels enterprise, site, area, line and work cell, and it permits a location path of free depth. Thus the prefix is a list of levels with no fixed count. The length of the full `device_id` is the only limit: the longest device topic must fit in 159 bytes.

| Configuration | UMH topic | Result |
|---|---|---|
| Prefix `acme:berlin:packaging:line3`, device `meter1`, node `acme:berlin:packaging:gw01` | `umh.v1.acme.berlin.packaging.line3.meter1._raw.Voltage` | Correct |
| Device `acme:berlin:packaging:line3:meter1` with no prefix | `umh.v1.acme.berlin.packaging.line3.meter1._raw.Voltage` | Correct, but each device name must hold the full location, in 39 characters or less |
| The default `Plant1` / `GW01` / `Meter1` | `umh.v1.Meter1._raw.Voltage` | The device becomes the enterprise. Two gateways with `Meter1` write to the same topic |
| Group `Acme:Berlin:Packaging`, device `Meter1` | `umh.v1.Meter1._raw.Voltage` | The location in the group is lost |
| Device `Acme: Berlin: Line 3: Meter 1` | The location is `Acme` only. `_Berlin` becomes the data contract | Wrong, and UMH gives no error |
| Device with `type` (a template) | No topic. With the UMH preset: a topic for each tag | The device is not visible without the preset |

The embedded default configuration of the gateway stays neutral for all hosts. The UMH example is in the how-to.

## How UMH changes the names

UMH changes each name before it makes the topic:

- It changes `/` and `:` to `.`.
- It changes each character that is not `A-Z`, `a-z`, `0-9`, `.`, `_` or `-` to `_`. A space becomes `_`.
- It changes two or more `.` in sequence to one `.`, and it removes `.` at the start and at the end.
- A location level that starts with `_` is read as the data contract.

The changes give no error. A name with a space or a `_` at the start of a level puts the data at a wrong place, with no warning. The metadata `spb_original_device_id` and `spb_original_metric_name` keep the names before the change.

The gateway accepts these names, because Sparkplug permits them. Other hosts can need them. But the validation gives a warning for each name that UMH changes (#169), and the UMH preset (`sparkplug.naming: "umh"`) refuses such names. The preset is an option that the user selects: it only adds checks, and it obeys the Sparkplug specification.

## The effect of `include_edge_node_in_location`

This option of the `sparkplug_b` input puts the `edge_node_id` before the `device_id` in the location of device data. Node data does not change.

- Use `false` (the default) when the device name holds the full location. With `true`, UMH puts the node name before the location a second time.
- Use `true` for a producer that uses `group_id`, `edge_node_id` and `device_id` as real levels, with short device names. Then devices with the same name on different nodes do not write to the same topic.

The how-to uses `false`, because the `device_prefix` and the device name give the full location.

## What UMH drops, and why the gateway keeps it

In v0.16.2, the `sparkplug_b` input has these limits:

- It gives no value for Template, DataSet and Bytes metrics, and for a metric with `is_null`.
- It ignores the PropertySet of a metric. Thus the UNS does not receive the units and the Quality.
- It cannot send a DCMD. UMH cannot write to a Modbus device through the gateway.
- The default role `secondary_passive` never asks for a rebirth.
- Without a BIRTH in its cache, the input uses the datatype of the DATA metric. The gateway sends no datatype in NDATA and DDATA, because the specification says so (`tck-id-payloads-metric-datatype-not-req`, #159). Thus, until the next BIRTH, a negative integer can show as a large number. The E2E measured -3 as 4294967293, with the name `alias_<n>`. The role `secondary_active` asks for that BIRTH, and the BIRTH came in less than 1 s.
- The `tag_processor` runs JavaScript, where each number is a double. Thus it rounds a 64-bit integer that is larger than 2^53. The input of the bridge has the exact value.

The gateway keeps these items, because they are correct Sparkplug B 3.0 and other hosts use them:

- `Node Info/Modbus` and `Node Info/Modbus TCP` are DataSets with the bus counters (SPB-17).
- A device with `type` is a template instance (SPB-18).
- A tag of the type `bytes` is a Bytes metric (SPB-19).
- Each tag has the properties `engUnit`, `engLow`, `engHigh` and `Quality` (SPB-16).
- A failed read gives a null value with the Quality BAD. UMH drops the null value and keeps the last good value. Thus UMH does not show that the value is old. With the UMH preset, the node metric `Devices/<name>/Online` tells UMH when a device goes offline (#173).

The gateway must obey the Sparkplug specification fully. It does not change its default behaviour for one host. The UMH preset (`sparkplug.naming: "umh"`, #169) adds plain metrics that UMH can read, and keeps the Sparkplug rules:

- A typed device publishes its tags as plain metrics, and the NBIRTH has no template definitions (#174). A definition is necessary only for a template instance, so this obeys the specification.
- The Modbus counters go out as plain node metrics, the sums of all devices (#174).
- Each device has the node metric `Devices/<name>/Online` (#173).

The preset changes nothing that the specification requires. It only selects one of the choices that the specification permits. Thus it is a host option, not a compatibility profile that deviates from the specification.

## No primary host: data can be lost

The role `primary` of the `sparkplug_b` input publishes the STATE of Sparkplug 2.x, a text `ONLINE` or `OFFLINE`. Sparkplug B 3.0 uses a JSON STATE, and the gateway accepts only that (#177). Thus the gateway cannot use UMH as its primary host.

Without a primary host, the gateway publishes when it has a session with the broker:

- The store & forward function (SPB-14) keeps data only when the broker is not available.
- If UMH stops and the broker continues, the gateway continues to publish. UMH does not get this data, also not later.
- Thus there is no guarantee that UMH has all the data.

With a 3.0 primary host, the gateway would stop and keep the data when the host is offline. UMH can do this only when its STATE obeys Sparkplug 3.0.

## Timestamps

Each Sparkplug metric has the time of its sample. UMH uses this time only if the `tag_processor` sets `msg.meta.timestamp_ms` from `msg.meta.spb_timestamp`. Without this line, UMH uses the arrival time.

- Store & forward samples arrive late, with `is_historical`. With the line, the UNS keeps the time of each sample. Without the line, all samples of the replay get the same arrival time. The E2E of #175 measured the two cases.
- The gateway sends the DBIRTH with the current value first, then the stored samples. Thus the UNS receives samples that are older than the last value. A consumer that keeps only the last message shows the last stored sample until the next change.
- Before the gateway has a valid time (SNTP, the RTC or the browser), it does not connect to the broker. Thus the UNS gets no timestamp near 1970 (#160). The first births come when the time becomes valid. A value that the gateway read before that time gets the time of the births. The web UI has a button that sets the time from the browser. The RTC battery keeps the time during a power cut (`docs/how-to/fit-the-rtc-battery.md`).
- The UMH downsampler has the option `late_policy` (`passthrough` or `drop`) for samples that arrive out of order. Nobody examined its effect on a replay yet: the E2E has no downsampler.

## Why the gateway does not make the UMH bridge configuration

The product owner rejected a function that writes the UMH bridge YAML from the gateway configuration. The YAML format of UMH changes, and the generated text would become incorrect. Thus the how-to gives an example, which a person can compare with the current UMH documentation.
