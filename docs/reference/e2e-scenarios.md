# The E2E scenarios

This document gives each scenario of the E2E harness and what it asserts. The test bench, the retry rules and the limits of QEMU are in [The E2E test bench](e2e-harness.md). To run one scenario, refer to [Run the tests](../how-to/run-the-tests.md#run-one-e2e-scenario).

The list of the scenarios, their order and their `--only` names are in `SCENARIOS` of `idf/tools/e2e/scenarios.py`. Each check has a name in `results.json`. The names start with the requirement ID (for example `MB-6`, `SPB-13`) or the issue number. The details below say what the checks assert, not the measured values: the harness prints the values in each check.

## Scenarios of the full runs

| Scenario | `--only` | Variant | Nodes |
|---|---|---|---|
| `failsafe` | `failsafe` | PSRAM | `GW03` |
| `network` | `network` | PSRAM | `GW02` |
| `web` | `web` | PSRAM | `GW04`, `GW05`, `GW06` |
| `reset` | `reset` | PSRAM | `GW07`, `GW08` |
| `tls` | `tls` | PSRAM | `GW12`–`GW16`, `GW25`, `GW29`, `GW30` |
| `failover` | `failover` | PSRAM | `GW17` |
| `mqtt5` | `mqtt5` | PSRAM | `GW18` |
| `max_packet` | `mqtt5` | PSRAM | `GW26` |
| `phost` | `phost` | PSRAM | `GW19`, `GW48` |
| `sf` | `sf` | PSRAM | `GW20` |
| `sf spill` | `sf` | PSRAM | `GW23` |
| `stack margin` | `sf` | PSRAM | `GW27` |
| `https` | `https` | PSRAM | `GW22` |
| `time` | `time` | PSRAM | `GW31` |
| FW-1 OTA by NCMD | `ota`, `ota-fw1`, `ota-fw1-ncmd` | PSRAM | `GW40` |
| FW-1 OTA by web upload | `ota`, `ota-fw1`, `ota-fw1-web` | PSRAM | `GW45` |
| issue #112 park in a long RS485 transaction | `ota`, `ota-park` | Default | `GW47` |
| issue #97 OTA trial wiring | `ota`, `ota-wiring` | Default | — |
| issue #144 OTA self-check | `ota`, `ota-trial` | Default | `GW43` |
| issues #67/#94 NVS migration | `nvs` | PSRAM | `GW41` |
| issue #109 full unencrypted NVS | `nvs` | PSRAM | `GW44` |
| issue #94 OTA to NVS encryption | `nvs` | PSRAM | `GW42`, `GW49` |
| `slow` | `slow` | PSRAM | `GW21` |
| issue #135 no PSRAM | `nopsram` | PSRAM | — |
| `refuse` | `refuse` | PSRAM | — |
| `main` | `main` | PSRAM | `GW01` |

`refuse` runs as a separate scenario only when `--only` names it and not `main`. `main` includes it. An unknown `--only` name stops the harness with an error.

## Checks of each run

The harness makes these checks one time per run, after the scenarios:

- **6. payloads decode:** each captured message decodes with the official `sparkplug_b.proto` and passes the rules of `spb_decode.py --check` (refer to [Tests](tests.md#sparkplug-decode-check)). A run needs at least one captured message.
- **#105:** no message reached an observer of an attempt that had ended.
- **#92:** the fault monitor ran without an error of its own.
- **#111:** each park of core 1 ended in its bound, and no park was refused.
- **SPB-5:** the seq is continuous in each session.
- **REL-4:** the syslog `sequenceId` is continuous in each boot (only with `main`).
- **The harness completed every scenario.**

## main

The main boot of `GW01`, with the slave, the main broker and the shell.

- **Session (§7.1, SPB-3, #161):** NBIRTH with seq 0 and a bdSeq of 0 at the boot. A killed TCP connection gives the NDEATH with the same bdSeq. The bdSeq increases by 1 for each CONNECT packet, and attempts without a CONNECT keep it. After a power cycle and after NCMD Reboot, the bdSeq is 0 again.
- **Broker restart and frozen broker:** the will at the close, a backoff that increases, then NBIRTH and the DBIRTHs. With the broker stopped for 20 s (SIGSTOP), the keepalive declares the session dead, and the new session stays alive.
- **MQTT-2 (#5, #11, #89):** a broker that cuts each connection after the CONNACK gives a backoff that increases. A refused SUBACK (0x80) gives no NBIRTH: the gateway sends DISCONNECT and tries again. The check waits up to 15 s for the first three refused connections to end. It requires a DISCONNECT in each, and a backoff that increases strictly. The backoff returns to the minimum only after a session that lasted one keepalive.
- **#71:** a connection cut after the SUBACK gives no "session up". **#72:** the shell answers a command before the configuration loads.
- **Devices (§7.2, MB-9, #61):** a slave that stops gives DDEATH in its bound, and DBIRTH when it is back. A device that never answers gets one probe attempt with backoff from the boot (2–4–8–10 s), and a healthy device keeps its 1 s period.
- **Modbus (MB-4, MB-6, MB-11, MB-12, MB-14, #38):** the MB-6 types in the DBIRTH with exact 64-bit values. DCMD writes: one FC16 for a 64-bit value, read-modify-write of a register bit, a string. Exact read requests with `max_gap` per device, 60 registers with one FC03. The slow scan of 3 s. A DCMD written between two requests of a cycle that is in progress.
- **SPB-15:** `Node Control/Scan Rate`: an NCMD changes the poll period, a value below the minimum is discarded, 0 returns to the configuration.
- **Modbus TCP (MB-15, #69, #81, #216, #234, #120):** reads and writes through port 502, exception 0B for a dead unit, FC 0x2B answered by the gateway. The counters in the shell and the `Node Info/Modbus TCP` DataSet. The local unit 100 (DIs, DOs, counters, run hours). A client through the access point gets no answer.
- **Shell (CFG-6, #68, REL-5, MB-13):** `devices`, `config` and `status`. `Node Info/Reset Reason`.
- **REL-1, #3, #12, #62, #65, #88:** the Sparkplug task feeds the watchdog after each birth. One `panic` saves a core dump that decodes, and the restart has the reset reason `panic`. Three panics while a Modbus response is in transit: each boot comes back.
- **#132:** the core dump through `Node Info/Core Dump`, `/api/status`, `GET /api/coredump/summary`, `GET` and `DELETE /api/coredump` (CSRF) and the shell `coredump`.
- **#15 task affinity:** `rs485_bus` on core 1; `sparkplug`, `tcpip`, `mqtt_task`, `emac_rx` and `sys_evt` on core 0.
- **Local I/O (§7.3, IO-3, IO-4, IO-6, IO-7, #204, #210, #211, #217, #218):** a DI change in NDATA in less than 100 ms. NCMD on a DO. The safe state after `failsafe_s` without a session, the `hold` output after a software restart, and OFF after a cold boot. Pulse counters saved every 60 s and at an orderly restart. The buzzer alarms. The rules `follow` and `toggle`, and an NCMD refused on a rule DO. `POST /api/do`, pulse DOs, invert and debounce, run hours, fast counters.
- **Commands (§7.4, §7.5, SPB-10, SPB-11):** DCMD Setpoint 12.3 writes 123 with FC06, and DDATA confirms it. A float32 DCMD is one FC16. NCMD Rebirth gives NBIRTH (seq 0, same bdSeq) and each DBIRTH. DCMD by alias, a DCMD on a read-only metric discarded with a log, an NCMD cut in the middle applies nothing. NCMD Reboot publishes one explicit NDEATH (QoS 1).
- **Data (SPB-6, SPB-7, SPB-12, SPB-16, SPB-17):** a DBIRTH of 3 772 B (larger than the 2 KB buffer of esp-mqtt). DDATA only by exception, only with alias, with the deadband. The properties `engUnit`, `engLow`, `engHigh` and `Quality`; a failed read gives `is_null` with Quality 0. The diagnostics in the NBIRTH and every `diagnostics_s`.
- **Time (TIME-1, TIME-2, TIME-3, #160):** UTC timestamps from SNTP. The RTC keeps the time across NCMD Reboot without SNTP. `Time Synced`. No birth timestamp before 2020 with a valid clock.
- **NET-6:** host name `gw01` from `edge_node_id`, and the `_https._tcp` service. QEMU cannot test the mDNS answers.
- **REL-4:** the log lines reach the syslog receiver as RFC 5424, also the warnings while the broker is down.
- **#75:** `/api/status` over HTTPS. **#85:** the configuration, the engine and its payload buffer are in PSRAM.

## failsafe

CFG-1, CFG-2, CFG-3, CFG-6, IO-2. The gateway boots with five rejected `config.json` files: 131 073 B, empty, a syntax error, a schema error, an erased partition.

- Nothing starts: no RS485 UART, no MQTT, no NBIRTH, no Modbus request. The check uses a positive signal: the line `nothing is started`, and 6 s later the answer of the shell `status`.
- The LED shows the error, and the ERROR log gives the reason with the path, the line or the column.
- The DOs go off before the configuration loads (IO-2).
- The network starts with DHCP. The web UI and `/api/status` answer with the commissioning password and the reason. `POST /api/modbus/read` and `POST /api/rebirth` answer 409 (#182, #171).
- The test exports the document, corrects it and saves it through the API. The gateway restarts and publishes NBIRTH.
- Without a file, the gateway uses the embedded configuration.

## network

Node `GW02`, a separate boot with another configuration.

- **NET-4:** a configuration with `network.wifi` boots in the build without radio, gives a warning, and publishes NBIRTH through Ethernet.
- **NET-7:** IPv6 link-local, SLAAC and static addresses, and `ping fec0::2`.
- **MB-15:** with `allowed_clients: ["192.0.2.0/24"]`, the gateway closes a connection from 10.0.2.2 without an answer.
- **SPB-18:** the template definition `Pump` in the NBIRTH, one instance in each of two DBIRTHs, a DCMD on a member.
- **SPB-19:** a `bytes` tag with a NUL in the DBIRTH, and a DCMD of Bytes as one FC16.
- **#201:** two SNTP servers.

## web

Nodes `GW04`, `GW05`, `GW06`: the REST API and the web UI.

- **CFG-4:** 401 without credentials, masked secrets, 400 with the JSON path or the line and column, `dry_run` without a save, 413 above 131 072 B. A valid `PUT` gives an orderly restart with the new node.
- **CFG-4 (#169, #172):** name warnings with their path, errors with the UMH preset, and a device prefix of five levels.
- **CFG-4 (#171):** `POST /api/rebirth` needs the CSRF header, then gives NBIRTH and the DBIRTHs.
- **CFG-6 (#182):** `POST /api/modbus/read` through the bus owner: plain and typed reads, a timeout as 504, refused requests.
- **CFG-6:** `config show` with masked secrets, `mbread`, `rebirth`, `config check` and `config load` in the shell.
- **CFG-7:** the secrets move from `config.json` to the encrypted NVS at the first boot. No secret is in plaintext in the flash image.
- **CFG-3 (#167, #170, #191, #192, #193, #194, #213, #221):** the lists of the form hold each value of the configuration, and an exported document is valid. The form has `maxlength`, help texts and the topic preview. The page has no `prompt()` and no `confirm()`. A new web password needs a second field. The controls of #221.
- **#226:** the served page has the IPv4 normalization of the Configuration tab. The validation refuses `192.168.001.010` in `network.dns` and `010.000.000.001/08` in `modbus_tcp.allowed_clients`, at their paths. It accepts the values that the web UI writes: `192.168.1.10` and `10.0.0.1/8`.
- **#199, #200, #206, #207, #208, #212, #216:** the live log and its levels, the gateway labels, identify, the simulated Wi-Fi scan, the heap and temperature in `/api/status`, the local unit without `local_writes`.

## reset

Nodes `GW07`, `GW08`: the BOOT button, commissioning and the access point.

- **CFG-5:** `boot 1` held for 10 s gives the factory reset: a warning at 2 s, an orderly NDEATH, commissioning mode, no block of the old configuration in the partition. The old web password gives 401, and the new commissioning password works.
- **NET-5:** a 2–10 s press starts the access point.
- **#120 claim window:** no window with a web password set. In commissioning, the gesture opens the window for its time. Only a client of the access point can set the password, with the CSRF header. After the set, the window is closed. The password is in no log line.
- **#187:** the WPA2 PMK of the access point is the web password, and a factory reset removes it. `POST /api/ap/stop`, and the stop after the idle time.
- **#203, #209:** the factory reset by the shell and the API, and the restart into safe mode on request.
- **#74:** a boot with the DO expander absent: the retry gets an answer, and all DOs are off.
- **#192, #193:** `ap.request_via_ap` is true after `claimsim 600 eth`.

## tls

MQTT-4, MQTT-5 and the memory checks of #80, #84 and #91.

- **MQTT-4:** TLS 1.3 with the CA from LittleFS, and the name verified against the SAN. A wrong `server_name` and an expired certificate (#70) give a clear log line, and no session. A CA file that is not in LittleFS gives no connection attempt.
- **MQTT-5:** mutual TLS 1.2 with a client certificate. Without the client certificate, the broker cuts the handshake.
- **#221:** PEM files loaded from the web UI, the limits, `DELETE /api/files/<name>`, and the check that the client key agrees with the client certificate.
- **#80 (`GW29`):** the memory thresholds at the worst case (refer to [Memory thresholds](e2e-harness.md#memory-thresholds)), and the stacks of the web tasks (#110).
- **#91:** the validated document prints into one block of its size.
- **#84 (`GW30`):** `validation hold 12000` keeps the validation at its peak while the relay cuts the MQTT connection. The TLS 1.3 reconnection completes during the validation, and the `PUT` answers 200.

## failover, mqtt5 and max_packet

- **failover (MQTT-6, `GW17`):** the primary broker is down at the boot, so NBIRTH goes to the backup. The return to the primary after `primary_return_s`. A second failure of the primary.
- **mqtt5 (MQTT-8, `GW18`):** an MQTT 5 session, NCMD Rebirth through the v5 subscription, and the will at a cut. #155: NCMD Reboot sends DISCONNECT with the reason code 0x04. #164: a failed NBIRTH of a rebirth ends the session. #186: the new session does not send the NDEATH of the old session again.
- **max_packet (`GW26`):** #73: a DBIRTH larger than the Maximum Packet Size of the broker (2 600 B) is not published, and the connection stays. #76: that DBIRTH is not built again at each change. #78: a `config check` half pasted during the boot does not hold the boot back.

## phost

SPB-13 primary host, nodes `GW19` and `GW48`.

- A retained STATE `false` holds the NBIRTH. `true` gives NBIRTH. An old `false` has no effect. `false` gives NDEATH and a disconnect, and the next session waits.
- **#144 item 6, #223:** without `ota.mqtt`, the NBIRTH has no `Node Control/Boot Other Slot` and no `Node Control/Firmware Update`, and the gateway refuses the NCMD.
- **With backup brokers (#156, #157, #186):** a valid STATE `false` moves to the next broker at once (`tck-id-operational-behavior-edge-node-termination-host-offline-reconnect`). Each broker has its own STATE timestamp. Nothing of the old MQTT session goes out after the new CONNECT (`tck-id-operational-behavior-primary-application-state-with-multiple-servers-walk`).

## sf, sf spill and stack margin

- **sf (SPB-14, `GW20`):** the relay removes the broker while values change. At the return: NBIRTH, DBIRTH, then historical DDATA and NDATA with `is_historical`, in timestamp order. The ring holds 1 000 samples in PSRAM in this build (40 000 on the board).
- **sf spill (`GW23`), #31, #122, #124, #151, #152:** a LittleFS directory takes the place of the card. The boot deletes old files in bounded steps without a replay. An outage of 40 s spills to the card, and all samples come back in order. A power cut while offline: the recovery finds the ring, and the replay sends each sample one time.
- **#136:** a planned restart while offline moves the RAM ring to the card, and the samples come back one time.
- **#145, #141:** a change outside the configuration hash keeps the ring. A tag rename starts a new ring, and the RAM ring is not moved.
- **#139, #137:** `POST /api/card/format` with its auth, CSRF and confirmation, the status objects, and a simulated format in its own task.
- **stack margin (`GW27`):** DCMDs on a template member and on Bytes, a diagnostic NDATA, and a replay of an instance. The `sparkplug` stack keeps at least 1 KB free.

## https

Node `GW22`.

- **#64:** the certificate generated at the first boot, its fingerprint on the console, HTTP only redirects (301 for GET, 308 for PUT), and the login limit per address (429 with `Retry-After`).
- **#100 CSRF:** each write without `X-Requested-With`, or from another origin, gets 403, also with a wrong password. The web UI still works.
- **#106:** a refused 1.5 MB upload is cut early, and a `GET` on another session answers fast. An authenticated upload that is not an image is discarded completely.
- **#110:** a slowloris client does not stop a `GET`, and the stack thresholds of `httpd`. `--web-stress N` adds N rounds of cut sessions, `PUT`s and uploads.
- **#205:** the web certificate of the plant: `GET`, `dry_run`, load, restart, and `POST /api/web/certificate/new`.

## time

Issue #160, node `GW31`. Without SNTP and with the RTC OS bit set:

- NBIRTH and DBIRTH go out, but no NDATA or DDATA comes. `/api/status` gives `time.valid` false.
- `POST /api/time` refuses a request without the CSRF header, a wrong password, a time before 2025 and a bad body. With the time of the browser, it gives a rebirth with UTC timestamps.
- A late SNTP gives a rebirth with `Time Synced` true. `POST /api/time` over a clock that SNTP synchronises gives 409.
- No NDATA or DDATA has a timestamp before 2020.

## slow

Issue #59, node `GW21`. The relay runs at 700 B/s. A publish that goes past the budget (2 s in the QEMU build) is cut. The connection closes with no panic, and a new NBIRTH comes on the fast link.

## FW-1 OTA by NCMD

Node `GW40`, with its own broker and HTTPS image server.

- The NBIRTH has `Node Control/Firmware Update` and `Firmware Update/State`.
- Refused without a restart: an `http://` URL, an image that fails the checksum or signature, an image signed with another key, a URL over 255 characters (#87).
- **#111:** a rejected web upload while the bus task floods the console: the parks of core 1 end, and the flood continues.
- **#98:** a download cut at 300 000 bytes gives a `failed` state with the count, and no restart.
- An update by NCMD with the CA from `ota.ca_file`: progress, `testing`, then `installed` by the self-check. After a power cut, the image stays.

## FW-1 OTA by web upload

Node `GW45`, with its own boot (#233), so the upload goes into `ota_1`.

- **#215:** an upload with a wrong `X-Image-SHA256` is refused, with no restart.
- During a trial, the other-slot button, NCMD `Boot Other Slot` and a configuration save get 409. The self-check installs the image.
- A panic during the trial of the next update makes the bootloader boot the earlier image. The NBIRTH gives the cause.
- **#144:** the "Restart into" button (`POST /api/firmware/other`) boots the image of the other slot on trial.

## OTA trial scenarios (default variant)

- **issue #112 park in a long RS485 transaction (`GW47`):** with a 5 s timeout, 5 retries and a silent `Meter1`, core 1 parks in the middle of the transaction, between two attempts.
- **issue #97 OTA trial wiring:** #144 items 1 to 3: an image from normal mode that rejects the configuration goes back at once (in less than half of the self-check time). An image from recovery mode, or forced, stays. The pre-flight check refuses before any write. An image installed from commissioning stays after a power cut. #146: the configuration from before an update, and the button of the earlier version.
- **issue #144 OTA self-check (`GW43`):** item 1: an image whose broker is down is installed by its self-check. Item 4: three crashes in a row start safe mode, and safe mode accepts a new image.

## NVS scenarios

- **issues #67/#94 NVS migration (`GW41`):** an unencrypted NVS meets the encrypted build, and the harness cuts the power at four points of the migration. The last boot completes it with `(0 skipped)`, connects with the password from the NVS, and leaves nothing in plaintext. #97: the first boot burns KEY5 in the eFuse file of QEMU, and no later boot does it again. #104, #108, #109: a boot with the copy out of reach opens no NVS and formats nothing. `config load discard-nvs-copy` is the way out.
- **issue #109 full unencrypted NVS (`GW44`):** an unencrypted NVS with no free page is still migrated.
- **issue #94 OTA to NVS encryption (`GW42`):** an OTA from the build without NVS encryption. The trial image keeps the NVS unencrypted. A panic rolls back, and the old image connects with its password. With the broker up, the self-check installs the image, and that boot migrates. #102: an update back to the build without NVS encryption is refused before the image is set to boot. This scenario needs `build-qemu-plain`.

## issue #135 no PSRAM

The PSRAM build on QEMU without the PSRAM model. The gateway runs the fault mode: the log, the shell and `/api/status` give the fault. The network and the HTTPS server run. There is no configuration, no polling, no MQTT and no panic.

## Optional scenarios

A full run and the CI groups do not run these scenarios (`OPTIONAL` in `scenarios.py`). Each runs only when `--only` names it. Without its tool, it shows `[SKIP]`.

### umh: UMH Core interoperability (#175)

The scenario runs the real benthos-umh v0.16.2 (commit `0d3a203`), the program of the UMH Core bridges. It needs `E2E_BENTHOS_UMH=<binary>`. It takes approximately 25 s.

The harness reads the input section and the `tag_processor` of the bridge from [Connect the gateway to UMH Core](../how-to/connect-to-umh-core.md). Thus the test uses the configuration that the document gives. Only the output changes: files of JSON lines in place of `uns: {}`.

- Bridge A: the configuration of the how-to (`role: "secondary_active"`), with three outputs: the input, the `tag_processor`, and the `tag_processor` without the timestamp line.
- Bridge B: `include_edge_node_in_location: true` and `secondary_passive`.
- Gateway `acme:berlin:gw01`: the UMH preset, store & forward, a relay, a `device_prefix` of six levels, a device with each scalar type and a `bytes` tag, a typed device, and a device that never answers.
- Gateway `acme:berlin:gw02`: the same devices without the UMH preset.

The checks assert what v0.16.2 does, also its limits. Thus a new UMH version that changes its behaviour makes a check fail. The measured behaviour is in [UMH names and topics](umh-names-and-topics.md) and in the how-to. The last result was 15/15 in each variant (merge b3ec3e7, before #160).

Not tested: the downsampler (`late_policy`), the `uns` output into Redpanda, the role `primary` (#177), and a DDEATH.

### tck: the Sparkplug TCK (#176)

The scenario runs the seven tests of the Edge Node profile of the Eclipse Sparkplug TCK 3.0.0, with MQTT 3.1.1 and MQTT 5. It needs `E2E_TCK_DIR` (from `tools/e2e/tck_setup.sh`). The procedure is in [Run the Sparkplug TCK against the gateway](../how-to/run-the-sparkplug-tck.md), and the results are in [Sparkplug TCK results](sparkplug-tck-results.md).

### downgrade: an earlier image on newer data (#148)

A rollback or a manual downgrade runs an earlier image on the data that a later image wrote. A library update can change a format that the earlier image cannot read: the LittleFS disk version, the NVS page format, `otadata`, the app descriptor or the partition table.

1. The image under test boots on a new flash. It saves a configuration with an MQTT password and a web password, and runs in normal mode.
2. The harness writes the app image of the earlier release over `ota_0`. The bootloader, `otadata`, the NVS and LittleFS stay as the later image left them.
3. The earlier image must start in normal mode with the same configuration (`GET /api/config`) and the secrets. Its broker requires the MQTT password, and the web UI must accept the web password. The bdSeq is in RAM only (#161), so the scenario does not examine it.

The scenario needs `E2E_DOWNGRADE_REF=<git ref>`. Before the tag `v1.0`, use the commit before the change. Each change that updates `littlefs` or ESP-IDF must pass this test in the two variants. If the earlier image cannot read the data, the pre-flight check must refuse that downgrade (refer to [The pre-flight check](firmware-updates.md#the-pre-flight-check)).

The last run used the reference `761edf8`, the last commit before the NVS records of #144. It gave 7/7 in each variant. The earlier image ignores the NVS keys that it does not know (`fw_last`, `cfg_prev`). It reads `lkg_net` as a raw struct with a size check, so it does not use the new record.
