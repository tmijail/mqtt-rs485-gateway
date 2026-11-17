# The REST API

This document lists the routes of the REST API of the gateway, its authentication and its responses.

- For the configuration document that `GET` and `PUT /api/config` use, refer to [The configuration document](configuration.md).
- For the procedures, refer to [Change, back up and restore the configuration](../how-to/change-the-configuration.md) and [Update the firmware](../how-to/update-the-firmware.md).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The E2E in QEMU examines the routes.

## Authentication

- All the routes require HTTP Basic with `web.username` and `web.password`, over HTTPS when `web.https` is on.
- The only exceptions are the routes of the claim window: `GET /claim`, `GET /api/claim` and `POST /api/claim`.
- While `web.password` is empty, the gateway accepts `admin` with its commissioning password.
- The responses are JSON.

**Limit of attempts.** After 5 failed logins in sequence from one address, that address gets `429` with `Retry-After` for 30 s.

- During that time, the gateway does not examine the password, also not the correct one.
- Each new lockout in sequence makes the time two times longer, up to 15 min.
- A correct login removes the lockout.

## Writes and CSRF

Each `PUT`, `POST` and `DELETE` must have the header `X-Requested-With`, with any value. The web UI sends `gateway`.

The client can also send `Origin`, or `Referer` when there is no `Origin`. Then it must be this gateway: the scheme of the connection and the `Host` of the request.

- The gateway compares them in a normal form. The name is in lower case and without the final dot, IPv6 is between brackets, and the port is decimal. The default port of the scheme is the same as no port.
- Any name of the gateway is correct, if the page and the request use the same name: the IP, `<hostname>.local`, a DNS name of the plant.
- A malformed `Host` or `Origin`, and `Origin: null`, are not correct.
- Otherwise, the response is `403`, before the password check. It does not count as a failed login.

A client that is not a browser (`curl`, a script) does not send `Origin`. The header `X-Requested-With` is sufficient:

```sh
curl -k -u admin:<web password> -H "X-Requested-With: curl" -X PUT \
     --data-binary @config.json "https://<ip>/api/config"
```

**Large bodies.** A request without credentials, or from another site, can have a body of more than 1 KB. Then the gateway sends the response (`401`, `403`, `429`) without a read of the full body, and closes the connection. A client that continues to send can see a closed connection and not the response. The gateway reads the full body of an authenticated request from the same origin, also when it refuses it (`400`, `409`, `413`, `415`).

## Routes

### Configuration

| Method and route | What it does |
|---|---|
| `GET /api/config` | Exports the saved configuration, with the secrets masked. `?download=1` sends it as a file |
| `PUT /api/config` (or `POST`) | Validates the document, saves it and restarts in order (NDEATH). `?dry_run=1` only validates |
| `PUT /api/config?restore_prev=1` | No body. The configuration from before the last firmware update replaces the stored one. Then the gateway restarts |

The responses of `PUT /api/config`:

| Status | Body or cause |
|---|---|
| `200` | `{"ok":true,"saved":false,"warnings":[{"path":"devices[0].name","message":"…"}],"warning_count":1}`. After a save: `"saved":true,"restart":true`. The response has the first 50 warnings. `warning_count` gives the number of all the warnings |
| `400` | `{"ok":false,"error":"…","path":"devices[0].tags[1].type","line":0,"column":0}`. With `restore_prev`: the firmware refuses the earlier document. Also when a PEM file that the document names is not correct (refer to [The PEM files](#the-pem-files)) |
| `404` | With `restore_prev`: there is no earlier document |
| `409` | A new firmware image is on trial: `testing <version>: N s left` |
| `413` | The document is larger than `CONFIG_GW_CONFIG_MAX_BYTES` (128 KiB) |

### Operation

| Method and route | What it does |
|---|---|
| `GET /api/status` | The live state, also in recovery mode, in commissioning and in the fault mode. Refer to [`GET /api/status`](#get-apistatus) |
| `POST /api/restart` | Restarts in order |
| `POST /api/rebirth` | Sends the NBIRTH and all the DBIRTHs again, with the same bdSeq. `202 {"ok":true,"rebirth":true}`. `409` when there is no Sparkplug session |
| `POST /api/time` | Sets the clock and the RTC. Refer to [`POST /api/time`](#post-apitime) |
| `POST /api/modbus/read` | One Modbus test read. Refer to [`POST /api/modbus/read`](#post-apimodbusread) |
| `GET /api/wifi/scan` | The Wi-Fi networks near the gateway. `?new=1` starts a scan. Refer to [`GET /api/wifi/scan`](#get-apiwifiscan) |

### The PEM files

| Method and route | What it does |
|---|---|
| `GET /api/files` | The files in the LittleFS partition that the configuration can name: what each file holds. Never the content of a file. Refer to [`GET /api/files`](#get-apifiles) |
| `PUT /api/files?name=<name>` | Loads a PEM file. The body is the PEM text. `&replace=1` replaces a file with the same name. Refer to [`PUT /api/files`](#put-apifiles) |

The files are for `mqtt.tls.ca_file`, `mqtt.tls.cert_file`, `mqtt.tls.key_file` and `ota.ca_file`. A load does not change the configuration. Select the file in the configuration, then save it.

A save or a dry run of `PUT /api/config` examines the files that the document names:

- Each file exists, and `ca_file` and `cert_file` hold certificates, `key_file` an unencrypted private key.
- The client key agrees with the client certificate.
- Otherwise, the response is `400` with the path of the key, for example `{"error":"mqtt.tls.key_file: the key does not agree with the certificate gw01.crt","path":"mqtt.tls.key_file"}`.

### Firmware

| Method and route | What it does |
|---|---|
| `GET /api/firmware` | The state of the update and of the two slots. Refer to [`GET /api/firmware`](#get-apifirmware) |
| `POST /api/firmware` | `application/json` with `{"url": "https://…"}` downloads the image. `application/octet-stream` with the image as the body uploads it. `"force": true` or `?force=1` forces the image into recovery mode |
| `POST /api/firmware/other` | Restarts into the image of the other slot, on trial, after the pre-flight check. `?force=1` forces. `202 {"ok":true,"restart":true}` |

The errors of the firmware routes:

| Status | Cause |
|---|---|
| `404` | `/other`: the other slot is empty, or it failed its verification |
| `409` | A trial is in progress (`testing <version>: N s left`), a transfer is in progress, or the pre-flight check refuses the image. The error lists the reasons |
| `415` | `POST /api/firmware` with another `Content-Type` |

### Access point and claim window

| Method and route | What it does |
|---|---|
| `GET /claim` | The page of the claim window. No credential. `GET /` without an `Authorization` header goes to it (`303`) while the window is open, for a client of the access point |
| `GET /api/claim` | The state of the claim window for this client. No credential |
| `POST /api/claim` | Sets the first web password. No credential, but the CSRF check and the limit of attempts apply |
| `POST /api/ap/stop` | Stops the access point approximately 2 s after the response. `202 {"ok":true,"stopping":true}`. `409` when the access point does not run |

For `GET /api/claim` and `POST /api/claim`, refer to [The claim window](#the-claim-window). For the access point, refer to [The access point](access-point.md).

### microSD card

| Method and route | What it does |
|---|---|
| `POST /api/card/format?confirm=1` | Starts a format of the card in the background, and returns at once. It deletes all the data on the card |

| Status | Cause |
|---|---|
| `202` | `{"ok":true,"started":true}` |
| `400` | No `?confirm=1` |
| `409` | A format runs (`reason`: `busy`) |
| `501` | The build has no card, or no PSRAM (`reason`: `no_support`, `no_psram`) |
| `503` | No memory for the format task (`reason`: `no_memory`) |

A card that does not answer ends the format as `failed` (`card_format` in `GET /api/status`). Refer to [Store & forward](store-and-forward.md#the-card-and-its-format).

### Core dump

| Method and route | What it does |
|---|---|
| `GET /api/coredump` | Downloads the core dump, as it is in the partition (`application/octet-stream`). `404` when there is no dump. `409` when the partition holds no readable dump |
| `DELETE /api/coredump` | Erases the core dump. `200 {"ok":true,"erased":true}`. `404` when there is no dump |
| `GET /api/coredump/summary` | `present`, `state` (`none`, `valid`, `corrupt`), `size` and `reset_reason`. For a valid dump also `task`, `pc`, `backtrace`, `backtrace_corrupted`, `exc_cause`, `exc_vaddr`, `reason` and `app_elf_sha256` |

> **Caution: Keep a downloaded core dump in a safe place.** The dump can contain secrets in clear text.

For the procedure, refer to [Get and decode a core dump](../how-to/decode-a-core-dump.md).

## Request and response details

### `POST /api/modbus/read`

The read goes through the task that owns the bus, as the shell `mbread` does. The route only reads.

| Field | Necessary | Rule |
|---|---|---|
| `unit` | Yes | 1 to 247 |
| `area` | Yes | `coil`, `discrete`, `holding` or `input` |
| `address` | Yes | 0 to 65535 |
| `count` | No | 1 to 16. Default: 1, or the size of the type |
| `type` | No | A tag type. `bool` only for `coil` and `discrete`. `string` and `bytes` read `count` registers |
| `word_order` | No | Only with `type` |

Example response:

```json
{"ok":true,"unit":1,"area":"input","address":0,"count":2,"values":[17254,32768],
 "type":"float32","word_order":"ABCD","value":230.5}
```

With the type `int64` or `uint64`, the response also has `value_text`, the same integer as a text.

| Status | Cause |
|---|---|
| `400` | The request is not correct |
| `409` | Recovery mode or commissioning: the bus does not run |
| `502` | `"result"`: `crc`, `frame`, or `exception` with the code in `"exception"` |
| `503` | The bus does not run the read in time, or another test read waits |
| `504` | `"result":"timeout"` |

### `GET /api/wifi/scan`

The list of the Wi-Fi networks near the gateway, for the button "Find networks" of the web UI. The scan also works when `network.wifi.enabled` is `false`: the radio starts for the scan, and stops after it.

- `?new=1` starts a scan, unless a scan runs or the last result is younger than 10 s.
- A request without `?new=1` only reads the state.
- A scan takes approximately 3 s. The gateway uses 2.4 GHz only.
- QEMU has no radio. In the emulator, the list is fixed, and `simulated` is `true`.

| Status | Body |
|---|---|
| `200` | `{"state":"done","age_s":2,"simulated":false,"networks":[…]}`, or `{"state":"idle","networks":[]}` before the first scan |
| `202` | `{"state":"scanning"}`: send the request again after 1 s |
| `404` | The build has no Wi-Fi radio |
| `503` | `{"state":"failed","error":"…"}`: the radio cannot scan now |

Each network has these fields. The list is strongest first, with one item for each SSID (the strongest access point) and no hidden network.

| Field | Content |
|---|---|
| `ssid` | The name of the network |
| `rssi` | The signal, in dBm |
| `channel` | The Wi-Fi channel |
| `security` | `Open`, `WEP`, `WPA`, `WPA/WPA2`, `WPA2`, `WPA2/WPA3`, `WPA3`, `WPA-Enterprise`, `WPA2-Enterprise`, `WPA3-Enterprise`, `WPA2/WPA3-Enterprise`, `WAPI`, `OWE` or `Unknown` |
| `supported` | `true` when the gateway can join the network: open, or WPA2 or WPA3 with a password. An enterprise network (802.1X) is not supported |

### `GET /api/files`

```json
{"files":[{"name":"ca.pem","size":1302,"kind":"certificates","count":1,
           "certs":[{"subject":"CN=ACME Plant Root CA","not_after":"2034-03-01","ca":true}]},
          {"name":"gw01.key","size":227,"kind":"key","key_type":"EC P-256",
           "agrees_with":["gw01.crt"]}],
 "total_bytes":1529,"max_file_bytes":16384}
```

| Field | Content |
|---|---|
| `name`, `size` | The file name and its size in bytes |
| `kind` | `certificates`, `key` (an unencrypted private key) or `other` |
| `count`, `certs` | For certificates: the number of certificates, and the first 4 with the subject, the last day of validity (UTC) and whether the certificate is a CA |
| `key_type` | For a key: for example `EC P-256` or `RSA 2048` |
| `agrees_with` | For a key: the certificate files whose first certificate has the public key of this key |

The list has up to 16 files. It leaves out `config.json` and the other files of the gateway.

### `PUT /api/files`

`PUT /api/files?name=<name>`, with the PEM text as the body (`Content-Type: application/x-pem-file`). The name rules are the rules of `mqtt.tls.ca_file`, and the name must not end with `.json`.

| Status | Body or cause |
|---|---|
| `201` | `{"ok":true,"file":{…}}`: the file, with the fields of `GET /api/files` |
| `400` | The name is not correct, or the body is not PEM certificates or an unencrypted PEM private key |
| `409` | A file with this name exists, and the request has no `replace=1` |
| `413` | The body is larger than 16 KiB |
| `507` | The gateway cannot write the file, for example because the LittleFS partition is full |

```sh
curl -k -u admin:<web password> -H "X-Requested-With: curl" -X PUT \
     --data-binary @ca.pem "https://<ip>/api/files?name=ca.pem"
```

### `POST /api/time`

Sets the clock of the gateway and the RTC to the time of the browser. Use it on a network without a time server.

- Body: `{"utc_ms": <ms since 1970-01-01T00:00:00Z>}`, an integer from 2025 to 2099.
- `200 {"ok":true,"utc_ms":…,"source":"browser"}`.
- `400` for a body that is not correct. `409` when SNTP synchronized the clock.
- `Node Info/Time Synced` stays `false`.
- In normal mode, the gateway then opens its MQTT session if it waited for a valid clock.
- The route also operates in recovery mode and in commissioning.

### The claim window

**`GET /api/claim`** gives the state of the claim window for this client:

```json
{"open":true,"allowed":true,"left_s":540,"username":"admin"}
```

- `allowed` is `true` only for a client of the access point.
- `left_s` and `username` are only for that client.
- `username` is `""` in recovery mode and in commissioning: any user name is correct.

### `POST /api/claim`

Body: `{"password": "…"}`, with the rule of `web.password`: 8 to 63 printable ASCII characters.

```json
{"ok":true,"username":"admin","ap":{"ssid":"GW-Setup-A1B2C3","url":"https://192.168.4.1/","secure_in_s":5},
 "plant":["https://10.0.0.5/"],"mdns":"https://gateway.local/"}
```

- The password is valid at once, and the window closes.
- `ap.url` is the address of this connection. 5 s after the response, the access point changes to WPA2 with the new password.
- `plant` has the addresses of the gateway on Ethernet and on the Wi-Fi station. `mdns` is `null` when mDNS is off.
- The gateway stores only the hash and the PMK, in NVS. The configuration document does not change.

| Status | Cause |
|---|---|
| `400` | The password or the body is not correct |
| `403` | A web password is set, no window is open, or the client is not a client of the access point. These refusals count as failed logins |
| `503` | NVS does not store the password. The window stays open |

### `GET /api/firmware`

| Field | What it is |
|---|---|
| `other`, `other_slot`, `other_state`, `other_why` | The other slot. `other_state` is `empty`, `previous`, `new`, `rejected` or `invalid` |
| `can_boot_other` | The button "Restart into `<version>`" is available |
| `phase`, `state`, `detail`, `progress`, `bytes`, `total` | The update. For the values of `state`, refer to [Update the firmware](../how-to/update-the-firmware.md#make-sure-that-the-update-is-complete) |
| `trial`, `trial_left_s`, `expected_mode` | The trial of a new image |
| `boot_count`, `safe_mode` | The crash counter and safe mode |
| `cfg_prev` | The configuration from before the update: `version`, `saved`, `schema`. `version` is empty when there is none |
| `cfg_prev_restore` | Recovery mode: the web UI shows the restore button |

### `GET /api/status`

| Field | What it is |
|---|---|
| `mode` | `normal`, `recovery`, `commissioning` or `fault` |
| `fault` | The hardware fault in the mode `fault`, for example a PSRAM that the boot did not find. Else empty |
| `firmware`, `reset_reason`, `uptime_s` | The firmware version, the cause of the last restart, and the seconds since the boot |
| `config` | `ok`, `source`, `error`, `path` |
| `network` | `up`, `interfaces` |
| `mqtt` | `connected` |
| `sparkplug` | `group_id`, `edge_node_id`, `diagnostics_s`. Only with a valid configuration |
| `syslog`, `heap` | The syslog client and the memory |
| `devices` | Refer to the text below the table |
| `io.di`, `io.do` | The bits of the DIs and DOs (bit n = channel n+1). `io.do` is `null` while the expander does not respond |
| `io.do_unknown`, `io.expander` | The expander does not respond. `ready`, `retrying` and `attempts` |
| `io.failsafe` | The DOs are in their safe state, because there is no Sparkplug session |
| `io.rule_do` | The mask of the DOs that a local rule controls |
| `io.counters` | The 8 pulse counters |
| `buzzer.alarm`, `buzzer.enabled` | The alarm that sounds now (`none`, `device_offline`, `mqtt_down`, `config_error`), and the enabled alarms |
| `time.utc_s`, `time.synced`, `time.valid`, `time.source`, `time.waiting` | The time (s UTC). `synced`: SNTP synchronized it. `valid`: the time is valid for timestamps. `source`: `sntp`, `rtc`, `browser` or `none`. `waiting`: the MQTT session waits for a valid clock |
| `time.rtc` | `present`, `lost` (the RTC lost the time: examine its battery), `status` of the last read or write, and `stores` |
| `card` (optional) | The microSD card. `state`: `unsupported`, `unchecked`, `absent`, `not_mounted`, `mounted`, `formatting` or `formatted`. `detail` tells why. When known: `name`, `size_mb`, `fat`, `cluster`, `free_mb` and `other_entries` |
| `spill` (optional, normal mode) | The spill to the card. `state`: `off`, `not_started`, `on`, `failed` or `stopped`, with `detail` and the counters of the shell `spill` line |
| `card_format` (optional) | The format of the card. `state`: `idle`, `running`, `done` or `failed` |
| `core_dump` (optional) | `present`, `state` (`none`, `valid`, `corrupt`) and `size` |
| `ap` (optional) | The access point. Refer to [The access point](access-point.md) |

For each device, `devices` has `online`, `cycles`, `failed`, `ok`, `timeout`, `crc`, `frame`, `exception` and `tags`. Each tag has `name`, `valid` and `value`.

- The value is a number, an exact integer for `int64` and `uint64`, a text for `string`, and hexadecimal for `bytes`.
- An `int64` or `uint64` tag also has `value_text`, the same integer as a text. A JSON reader that keeps each number as a double (for example a web browser) rounds an integer above 2^53.

A client that does not know the optional fields can ignore them.
