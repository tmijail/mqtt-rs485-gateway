# The REST API

This document lists the routes of the REST API of the gateway, its authentication and its responses.

- For the configuration document that `GET` and `PUT /api/config` use, refer to [The configuration document](configuration.md).
- For the firmware update process, the trial and the pre-flight check, refer to [Firmware updates](firmware-updates.md). For the security facts of the web server, refer to [Commissioning and secrets](commissioning-and-secrets.md).
- For the procedures, refer to [Change, back up and restore the configuration](../how-to/change-the-configuration.md), [Update the firmware](../how-to/update-the-firmware.md), [Restart into safe mode or do a factory reset](../how-to/restart-into-safe-mode-or-reset.md), [Read the live log](../how-to/read-the-live-log.md) and [Manage the certificates](../how-to/manage-the-certificates.md).

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
- The gateway keeps the state of 8 addresses. Behind a NAT, all clients share one address. For the other limits, refer to [Commissioning and secrets](commissioning-and-secrets.md#the-limits-of-the-web-server).

## Writes and CSRF

Each `PUT`, `POST` and `DELETE` must have the header `X-Requested-With`, with any value. The web UI sends `gateway`.

The client can also send `Origin`, or `Referer` when there is no `Origin`. Then it must be this gateway: the scheme of the connection and the `Host` of the request.

- The gateway compares them in a normal form. The name is in lower case and without the final dot, IPv6 is between brackets, and the port is decimal. The default port of the scheme is the same as no port.
- Any name of the gateway is correct, if the page and the request use the same name: the IP, `<hostname>.local`, a DNS name of the plant.
- A malformed `Host` or `Origin`, and `Origin: null`, are not correct. Malformed means, for example, a space, `@`, two `:`, a port outside 1 to 65535, or a bracket that is not closed.
- A `Referer` of more than 255 characters, or an `Origin` of more than 127 characters, is not correct.
- Otherwise, the response is `403`, before the password check. It does not count as a failed login.

The gateway does not answer `OPTIONS`, and it sends no CORS headers. Thus a page of another site cannot add the header `X-Requested-With`. A `GET` changes nothing, and the gateway does not examine its origin. Another site can send a `GET`, but cannot read the response. For the limits of this check, for example DNS rebinding, refer to [The security of the web UI and the claim window](../explanation/web-and-claim-security.md#why-the-csrf-check-has-this-form).

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
| `PUT /api/config?restore_prev=1` | No body. The configuration from before the last firmware update replaces the stored one. Then the gateway restarts. Refer to [Firmware updates](firmware-updates.md#the-configuration-from-before-the-update) |
| `PUT /api/config?discard_nvs_copy=1` | Formats LittleFS and saves the document. Use it only when LittleFS does not mount and can hold the copy of an NVS migration (QEMU build). The secrets of the copy are lost. Refer to [Commissioning and secrets](commissioning-and-secrets.md#the-nvs-migration-in-the-qemu-build) |

The responses of `PUT /api/config`:

| Status | Body or cause |
|---|---|
| `200` | `{"ok":true,"saved":false,"warnings":[{"path":"devices[0].name","message":"…"}],"warning_count":1}`. After a save: `"saved":true,"restart":true`. The response has the first 50 warnings. `warning_count` gives the number of all the warnings |
| `400` | `{"ok":false,"error":"…","path":"devices[0].tags[1].type","line":0,"column":0}`. With `restore_prev`: the firmware refuses the earlier document. Also when a PEM file that the document names is not correct (refer to [The PEM files](#the-pem-files)) |
| `404` | With `restore_prev`: there is no earlier document |
| `409` | A new firmware image is on trial: `testing <version>: N s left` |
| `413` | The document is larger than `CONFIG_GW_CONFIG_MAX_BYTES` (128 KiB) |
| `503` | There is not sufficient memory to parse the document |

### Operation

| Method and route | What it does |
|---|---|
| `GET /api/status` | The live state, also in recovery mode, in commissioning and in the fault mode. Refer to [`GET /api/status`](#get-apistatus) |
| `POST /api/restart` | Restarts in order. `202 {"ok":true,"restart":true}`. `?safe=1` restarts into safe mode. Refer to [Safe mode on request](#safe-mode-on-request) |
| `POST /api/identify` | For 10 s, the status LED shows red, green and blue in turn, and the buzzer beeps. `202 {"ok":true,"identify_left_s":10}`. `409` with `identify_left_s` while an identify runs: the time does not start again |
| `POST /api/factory-reset?confirm=1` | Does a factory reset, then restarts in commissioning. Refer to [`POST /api/factory-reset`](#post-apifactory-reset) |
| `GET /api/log` | The last lines of the log. Refer to [`GET /api/log`](#get-apilog) |
| `GET /api/log/level`, `POST /api/log/level` | The log level of each tag. Refer to [The log level of a tag](#the-log-level-of-a-tag) |
| `POST /api/rebirth` | Sends the NBIRTH and all the DBIRTHs again, with the same bdSeq. `202 {"ok":true,"rebirth":true}`. `409` when there is no Sparkplug session |
| `POST /api/time` | Sets the clock and the RTC. Refer to [`POST /api/time`](#post-apitime) |
| `POST /api/modbus/read` | One Modbus test read. Refer to [`POST /api/modbus/read`](#post-apimodbusread) |
| `POST /api/do` | Sets a DO on or off. Refer to [`POST /api/do`](#post-apido) |
| `GET /api/wifi/scan` | The Wi-Fi networks near the gateway. `?new=1` starts a scan. Refer to [`GET /api/wifi/scan`](#get-apiwifiscan) |

### The PEM files

| Method and route | What it does |
|---|---|
| `GET /api/files` | The files in the LittleFS partition that the configuration can name: what each file holds, and the keys that use it. Never the content of a file. Refer to [`GET /api/files`](#get-apifiles) |
| `PUT /api/files?name=<name>` | Loads or replaces a PEM file, or a certificate and its key together. `?dry_run=1` only examines. Refer to [`PUT /api/files`](#put-apifiles) |
| `DELETE /api/files/{name}` | Removes a file that the saved configuration does not use. Refer to [`DELETE /api/files/{name}`](#delete-apifilesname) |

The files are for `mqtt.tls.ca_file`, `mqtt.tls.cert_file`, `mqtt.tls.key_file` and `ota.ca_file`. A load does not change the configuration. Select the file in the configuration, then save it. A replaced file keeps its name: the gateway uses the new content after the next restart.

The gateway keeps 16 PEM files at most, and each file has 16384 bytes at most.

A save or a dry run of `PUT /api/config` examines the files that the document names:

- Each file exists, and `ca_file` and `cert_file` hold certificates, `key_file` an unencrypted private key.
- The client key agrees with the client certificate.
- Otherwise, the response is `400` with the path of the key, for example `{"error":"mqtt.tls.key_file: the key does not agree with the certificate gw01.crt","path":"mqtt.tls.key_file"}`.

### The HTTPS certificate of the web UI

| Method and route | What it does |
|---|---|
| `GET /api/web/certificate` | The certificate that the web server uses, and the certificate that waits for the restart. Never a key. Refer to [`GET /api/web/certificate`](#get-apiwebcertificate) |
| `PUT /api/web/certificate` | Loads the certificate of the plant and its key. It waits for the restart. `?dry_run=1` only examines. Refer to [`PUT /api/web/certificate`](#put-apiwebcertificate) |
| `POST /api/web/certificate/new` | Makes a new key and a self-signed certificate for the hostname. It waits for the restart. `201` with the certificate, as in `PUT` |
| `DELETE /api/web/certificate/pending` | Discards the certificate that waits for the restart. `200 {"ok":true,"discarded":true}`. `404` when no certificate waits |

The web server uses a new certificate only after a restart. Until then, it uses the certificate of this boot. A second load, or a new certificate, replaces the certificate that waits.

### Firmware

| Method and route | What it does |
|---|---|
| `GET /api/firmware` | The state of the update and of the two slots. Refer to [`GET /api/firmware`](#get-apifirmware) |
| `POST /api/firmware` | `application/json` with `{"url": "https://…", "sha256": "…", "force": true}` downloads the image. Only `url` is necessary. `application/octet-stream` with the image as the body uploads it, with the optional header `X-Image-SHA256`. `"force": true` or `?force=1` forces the image into recovery mode. Refer to [`POST /api/firmware`](#post-apifirmware) |
| `POST /api/firmware/other` | Restarts into the image of the other slot, on trial, after the pre-flight check. `?force=1` forces. `202 {"ok":true,"restart":true}` |

The errors of the firmware routes:

| Status | Cause |
|---|---|
| `400` | `POST /api/firmware`: the body or the URL is not valid, or the SHA-256 does not have 64 hexadecimal characters |
| `404` | `/other`: the other slot is empty, or it failed its verification |
| `409` | A trial is in progress (`testing <version>: N s left`), a transfer is in progress, or the pre-flight check refuses the image. The error lists the reasons |
| `415` | `POST /api/firmware` with another `Content-Type` |
| `422` | An upload whose SHA-256 does not agree with `X-Image-SHA256`. The gateway did not install the image |

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

### `POST /api/do`

Sets a digital output, as an NCMD does. Only the DOs: the web UI does not write to the Modbus devices.

- Body: `{"do": 3, "on": true}`. `do` is 1 to 8.
- The command goes through the queue of the task that owns the expander, the same queue as an NCMD. The next NDATA gives the new state to the Sparkplug host.
- `on: true` on a pulse DO (`local_io.do_pulse_ms`) starts its pulse.
- The safe state rules do not change: after `local_io.failsafe_s` without a session, the DO goes to its safe state.
- The Status tab uses this route. Its first change in a page session asks for a confirmation.

| Status | Cause |
|---|---|
| `200` | `{"ok":true,"do":3,"on":true,"state":true}`. `state` is the state that the gateway read back from the expander |
| `400` | The body is not correct |
| `409` | A local rule controls the DO (`local_io.rules`). Or recovery mode, commissioning or the fault mode: the local I/O does not run |
| `503` | The expander does not respond, or the task did not answer in time |

### `GET /api/wifi/scan`

The list of the Wi-Fi networks near the gateway, for the button "Find networks" of the web UI. The scan also works when `network.wifi.enabled` is `false`: the radio is on at each boot, as the entropy source of the random values (#267).

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
           "certs":[{"subject":"CN=ACME Plant Root CA","not_after":"2034-03-01","ca":true}],
           "used_by":["mqtt.tls.ca_file"]},
          {"name":"gw01.key","size":227,"kind":"key","key_type":"EC P-256",
           "agrees_with":["gw01.crt"],"used_by":["mqtt.tls.key_file"]}],
 "count":2,"total_bytes":1529,"max_files":16,"max_file_bytes":16384,"config_known":true,
 "fs_total_bytes":1048576,"fs_used_bytes":28672}
```

| Field | Content |
|---|---|
| `name`, `size` | The file name and its size in bytes |
| `kind` | `certificates`, `key` (an unencrypted private key) or `other` |
| `count`, `certs` | For certificates: the number of certificates, and the first 4 with the subject, the last day of validity (UTC) and whether the certificate is a CA |
| `key_type` | For a key: for example `EC P-256` or `RSA 2048` |
| `agrees_with` | For a key: the certificate files whose first certificate has the public key of this key |
| `used_by` | The keys of the saved configuration that name the file, for example `mqtt.tls.ca_file`. An empty list = the file is not in use |
| `count`, `total_bytes` | The number of files and their bytes |
| `max_files`, `max_file_bytes` | The limits: 16 files, 16384 bytes for each file |
| `config_known` | `false` when the gateway cannot read the saved configuration. Then `used_by` is empty, and `DELETE` refuses each file |
| `fs_total_bytes`, `fs_used_bytes` | The size of the LittleFS partition and the bytes in use |

The list has up to 16 files. It leaves out `config.json` and the other files of the gateway.

### `PUT /api/files`

`PUT /api/files?name=<name>`, with the PEM text as the body (`Content-Type: application/x-pem-file`). The name rules are the rules of `mqtt.tls.ca_file`, and the name must not end with `.json`.

| Query parameter | Effect |
|---|---|
| `name` | The name of the file in the gateway. Necessary |
| `key_name` | The body holds a certificate and its private key. The gateway separates the key block: the certificates go to `name` and the key to `key_name`. The key must agree with the first certificate |
| `replace=1` | A file with the same name is replaced. Without it, the response is `409` |
| `dry_run=1` | The gateway examines the body and writes nothing. The response is `200` |

The gateway also compares the load with the saved configuration:

- When a key of the configuration names the file, the new content must be of the same kind. For example, a private key cannot replace the file of `mqtt.tls.ca_file`.
- When the load replaces the file of `mqtt.tls.cert_file` or `mqtt.tls.key_file`, the client key must still agree with the client certificate. To change the two, load them together with `key_name`.

| Status | Body or cause |
|---|---|
| `200` | `dry_run=1`: `{"ok":true,"saved":false,"files":[…]}`. Each file has the fields of `GET /api/files` and `"replaced"` |
| `201` | `{"ok":true,"saved":true,"files":[…],"file":{…}}`. `file` is the first file |
| `400` | The name is not correct, the body is not PEM certificates or an unencrypted PEM private key, the key does not agree, or the content does not agree with the key of the configuration. `path` gives that key, for example `mqtt.tls.ca_file`. A body with a certificate and a key without `key_name` is also refused |
| `409` | A file with this name exists, and `replace=1` is not in the query |
| `413` | A file is larger than 16384 bytes |
| `507` | The gateway holds 16 files, or there is not sufficient space in the LittleFS partition, or the write failed |

```sh
curl -k -u admin:<web password> -H "X-Requested-With: curl" -X PUT \
     --data-binary @ca.pem "https://<ip>/api/files?name=ca.pem"
cat gw01.crt gw01.key | curl -k -u admin:<web password> -H "X-Requested-With: curl" -X PUT \
     --data-binary @- "https://<ip>/api/files?name=gw01.crt&key_name=gw01.key&replace=1"
```

### `DELETE /api/files/{name}`

| Status | Body or cause |
|---|---|
| `200` | `{"ok":true,"removed":true}` |
| `404` | The file is not in the LittleFS partition |
| `409` | The saved configuration names the file: `{"ok":false,"error":"…","path":"mqtt.tls.ca_file","used_by":["mqtt.tls.ca_file"]}`. Select another file, save, then remove it. Also when the gateway cannot read the saved configuration |

### `GET /api/web/certificate`

```json
{"https":true,"hostname":"gw01",
 "current":{"source":"first-boot","subject":"CN=gw01, O=RS485 Sparkplug gateway",
            "issuer":"CN=gw01, O=RS485 Sparkplug gateway","sans":"DNS:gw01.local,DNS:gw01",
            "not_before":"2025-01-01","not_after":"2099-12-31","sha256":"A7:3C:…:15",
            "chain":1,"self_signed":true,"key_type":"EC P-256","hostname_in_sans":true},
 "pending":null,"max_cert_bytes":3999,"max_key_bytes":3999}
```

| Field | Content |
|---|---|
| `https` | `true` when the web server uses HTTPS now (`web.https`) |
| `current` | The certificate that the web server uses. With `web.https: false`, the stored certificate. `null` when there is none |
| `pending` | The certificate that waits for the restart, with the same fields. `null` when there is none |
| `source` | `first-boot` (the gateway made it at its first boot), `new` (the gateway made it on request), `loaded` (the certificate of the plant) |
| `sans` | The DNS names and the IP addresses of the certificate |
| `sha256` | The SHA-256 fingerprint of the first certificate, as the browser shows it |
| `chain` | The number of certificates in the PEM |
| `hostname_in_sans` | `true` when a DNS name of the SANs is `network.hostname`, `<hostname>.local` or `<hostname>.<domain>` |
| `max_cert_bytes`, `max_key_bytes` | The limits of `PUT`: the PEM of the certificate with its chain, and of the key |

### `PUT /api/web/certificate`

The body holds the certificate of the plant (PEM, with its chain) and its unencrypted private key, one after the other (`Content-Type: application/x-pem-file`). The gateway separates the key block, parses the certificates and the key with mbedTLS, and makes sure that the key agrees with the first certificate. The key stays in NVS with the secrets.

| Status | Body or cause |
|---|---|
| `200` | `?dry_run=1`: `{"ok":true,"saved":false,"certificate":{…}}`, with the fields of `GET` |
| `201` | `{"ok":true,"saved":true,"restart":true,"certificate":{…}}`: the certificate waits for the restart |
| `400` | No private key, more than one key, a certificate that does not parse, or a key that does not agree |
| `413` | The certificate with its chain, or the key, has more than 3999 bytes |
| `507` | The gateway cannot store it in NVS. Also when the NVS has not sufficient free space for the certificate, its key and their copy at the restart |

```sh
cat gw01-plant.pem gw01-plant.key | curl -k -u admin:<web password> -H "X-Requested-With: curl" \
     -X PUT --data-binary @- "https://<ip>/api/web/certificate"
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

### Safe mode on request

`POST /api/restart?safe=1` restarts the gateway into safe mode. The gateway does not poll and does not connect to MQTT. The configuration stays.

- In normal mode, the restart is in order: the gateway first sends the NDEATH.
- Safe mode ends after 60 min, or at the next restart (`POST /api/restart`, the shell `restart`, a power cycle).
- `GET /api/status` gives `safe_mode`: `active`, `cause` (`request` or `crashes`) and `left_s`.

| Status | Cause |
|---|---|
| `202` | `{"ok":true,"restart":true,"safe_mode":true}` |
| `409` | The gateway is in safe mode now, a new firmware image is on trial, or a firmware update is in progress |

### `POST /api/factory-reset`

The factory reset erases the same data as the BOOT button held for 10 s:

- the configuration;
- the secrets, for example the MQTT password;
- the certificate of the web UI;
- the web password;
- the pulse counters of the DIs.

Then the gateway restarts in commissioning mode, 0.5 s after the response. The restart also sets the Modbus counters to 0, because the gateway does not store them. The access point starts with a new commissioning password.

| Status | Cause |
|---|---|
| `202` | `{"ok":true,"erased":true,"restart":"commissioning"}`. `erased` is `false` when a step of the reset failed. The gateway restarts also then, and the next boot does the remaining steps again (refer to [Commissioning and secrets](commissioning-and-secrets.md#the-sequence-of-the-factory-reset)) |
| `400` | No `?confirm=1` |
| `409` | The firmware refused the factory reset. Nothing is erased |

### `GET /api/log`

The gateway keeps the last log lines in a ring of 16 KB in PSRAM (`CONFIG_GW_LOG_RING_KB`), from the boot on. Each line has a number: the first line after the boot is 1. The web UI asks only while its card Log is open.

`?since=<n>` gives the lines after the number `n`. Without it, the response has all the lines in the ring.

```json
{"since":120,"last":125,"next":126,"first":20,"lost":0,"count":106,"bytes":9874,
 "size":16384,"dropped":0,"lines":["I (35210) web: identify requested through the REST API"]}
```

| Field | What it is |
|---|---|
| `last` | The number of the last line in `lines`. Ask again with `?since=<last>` |
| `next`, `first` | The number of the next line, and of the oldest line in the ring |
| `lost` | The lines after `since` that the ring does not have any more: it was full |
| `count`, `bytes`, `size` | The lines and the bytes in the ring, and its size |
| `dropped` | The lines that the ring did not get, because another task held it |

When `since` is not lower than `next`, the gateway restarted. Then `since` is `0` in the response, and `lines` has all the lines of the ring. `503` when the ring has no memory.

### The log level of a tag

The level of a tag is the lowest severity that the tag writes to the log: the console, the syslog copy and the ring. The gateway does not save a change. A restart sets each tag to the level of the build (`info`). `syslog.level` filters only the copy to the syslog server.

**`GET /api/log/level`** gives the levels and the tags:

```json
{"build":"info","max":"verbose","levels":["none","error","warn","info","debug","verbose"],
 "tags":["ap","bus","edge","mb","poll","web","esp-tls"],"changed":[{"tag":"poll","level":"debug"}]}
```

- `max` is the highest level that the build keeps (`CONFIG_LOG_MAXIMUM_LEVEL`).
- `tags` has the tags of the firmware and the tags that the log showed since the boot, up to 48.
- `changed` has the tags with a level that is not the level of the build. `GET /api/status` gives the same list as `log_levels`.

**`POST /api/log/level`** with `{"tag": "poll", "level": "debug"}` sets the level of a tag. The tag `*` sets the level of all the tags that have no level of their own. `{"reset": true}` sets all the tags to the level of the build again.

| Status | Cause |
|---|---|
| `200` | `{"ok":true,"tag":"poll","level":"debug"}`, or `{"ok":true,"reset":true}` |
| `400` | The body, the tag or the level is not correct |
| `409` | The table of the tags is full |

### `GET /api/firmware`

| Field | What it is |
|---|---|
| `running`, `slot` | The version of the firmware that runs, and its slot (`ota_0` or `ota_1`) |
| `other`, `other_slot`, `other_state`, `other_why` | The other slot. `other_state` is `empty`, `previous`, `new`, `rejected` or `invalid` |
| `can_boot_other` | The button "Restart into `<version>`" is available |
| `mqtt` | The value of `ota.mqtt`: `true` when a Sparkplug host can start an update or a restart into the other slot. In recovery mode it is `false` |
| `phase`, `state`, `detail`, `progress`, `bytes`, `total` | The update. `phase` is `idle`, `downloading`, `restarting`, `failed`, `testing`, `installed` or `rolled_back`. For the values of `state`, refer to [Update the firmware](../how-to/update-the-firmware.md#make-sure-that-the-update-is-complete) |
| `sha256_expected`, `sha256_file` | Only when the last update failed because the SHA-256 did not agree: the SHA-256 that the request gave, and the SHA-256 of the file. 64 small hexadecimal characters each |
| `trial`, `trial_left_s`, `expected_mode` | The trial of a new image |
| `boot_count`, `safe_mode` | The crash counter and safe mode. The cause of safe mode and its time left are in `GET /api/status` |
| `cfg_prev` | The configuration from before the update: `version`, `saved`, `schema`. `version` is empty when there is none |
| `cfg_prev_restore` | Recovery mode: the web UI shows the restore button |

### `POST /api/firmware`

A download from a URL:

```json
{"url": "https://files.example.com/gw/rs485_spb_gateway-0.2.0.bin",
 "sha256": "17bf8431d18e8d424fb300303419bebde3749ad415de75afdaf4ecb1776030dd", "force": false}
```

| Key | Rule |
|---|---|
| `url` | Necessary. An `https://` URL, 255 characters at most |
| `sha256` | Optional. The SHA-256 of the image file: 64 hexadecimal characters, capital or small letters. Empty = no check |
| `force` | Optional. `true` installs an image that the pre-flight check refuses. The image must then reach recovery mode |

The answer is `202 {"ok":true,"started":true}`. The result comes later in `GET /api/firmware`.

An upload: the body is the image, with `Content-Type: application/octet-stream` and `Content-Length`. The optional header `X-Image-SHA256` gives the SHA-256 of the file. `?force=1` forces the image. The answer is `200 {"ok":true,"restart":true}`. The upload runs in the task of the web server: the web UI does not answer other requests until the upload ends.

With a SHA-256, the gateway calculates the SHA-256 of the image that it wrote, before it sets the image as the boot image. When the two values do not agree, the gateway does not install the image, and the firmware that runs continues to run. The state is `failed: image <version>: the SHA-256 of the file does not agree`. An upload then gets this answer:

```json
{"ok":false,"error":"image 0.2.0: the SHA-256 of the file does not agree","path":"","line":0,"column":0,
 "sha256_expected":"17bf8431…6030dd","sha256_file":"a1542ecc…5fef64b"}
```

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
| `syslog` | The syslog client |
| `heap` | `internal_free`, `internal_min`, `internal_largest` (the largest free block of the internal RAM) and `psram_free`, in bytes |
| `temperature_c` | The temperature of the chip, in °C. `null` when there is no reading of the sensor, for example in QEMU |
| `gateway` | `name` and `location` of the configuration. Not the comment |
| `identify_left_s` | The seconds left of an identify. `0` when none runs |
| `safe_mode` | `active`, `cause` (`request` or `crashes`, empty when not active) and `left_s` (the seconds to the restart into normal mode) |
| `log_levels` | The tags with a changed log level: `[{"tag":"poll","level":"debug"}]` |
| `devices` | Refer to the text below the table |
| `io.di`, `io.do` | The bits of the DIs and DOs (bit n = channel n+1). `io.do` is `null` while the expander does not respond |
| `io.do_unknown`, `io.expander` | The expander does not respond. `ready`, `retrying` and `attempts` |
| `io.failsafe` | The DOs are in their safe state, because there is no Sparkplug session |
| `io.rule_do` | The mask of the DOs that a local rule controls |
| `io.rules` | The local rules: `[{"di":2,"do":5,"mode":"follow"}]`. Empty when the mode is not normal |
| `io.counters` | The 8 pulse counters |
| `io.fast` | The mask of the DIs that a PCNT unit counts (`local_io.fast_counters`) |
| `io.pulse_ms` | The pulse time of each DO, `DO/1` first. 0 = a normal DO |
| `io.run_hours` | `{"di":[…],"do":[…]}`: the run hours in seconds, 8 values each. `null` for a channel that is not in `local_io.run_hours` |
| `buzzer.alarm`, `buzzer.enabled` | The alarm that sounds now (`none`, `device_offline`, `mqtt_down`, `config_error`), and the enabled alarms |
| `time.utc_s`, `time.synced`, `time.valid`, `time.source`, `time.waiting` | The time (s UTC). `synced`: SNTP synchronized it. `valid`: the time is valid for timestamps. `source`: `sntp`, `rtc`, `browser` or `none`. `waiting`: the MQTT session waits for a valid clock |
| `time.rtc` | `present`, `lost` (the RTC lost the time: examine its battery), `status` of the last read or write, and `stores` |
| `card` (optional) | The microSD card. `state`: `unsupported`, `unchecked`, `absent`, `not_mounted`, `mounted`, `formatting` or `formatted`. `detail` tells why. When known: `name`, `size_mb`, `fat`, `cluster`, `free_mb` and `other_entries` |
| `spill` (optional, normal mode) | The spill to the card. `state`: `off`, `not_started`, `on`, `failed` or `stopped`, with `detail` and the counters of the shell `spill` line |
| `card_format` (optional) | The format of the card. `state`: `idle`, `running`, `done` or `failed` |
| `core_dump` (optional) | `present`, `state` (`none`, `valid`, `corrupt`) and `size` |
| `ap` (optional) | The access point. Refer to [The access point](access-point.md) |

For each device, `devices` has `name`, `unit_id`, `enabled`, `online`, `cycles`, `failed`, `ok`, `timeout`, `crc`, `frame`, `exception`, `write_refused` and `tags`. Each tag has `name`, `valid` and `value`.

- `enabled`: `false` for a device with `enabled: false` in the configuration. The gateway does not poll it, so its counters stay at 0 and its tags are not valid.
- `write_refused`: the number of DCMD writes since the start that were out of the write limits of a tag (`write_min`, `write_max`). The Status tab shows it in the State cell of the device when it is more than 0.

- The value is a number, an exact integer for `int64` and `uint64`, a text for `string`, and hexadecimal for `bytes`.
- An `int64` or `uint64` tag also has `value_text`, the same integer as a text. A JSON reader that keeps each number as a double (for example a web browser) rounds an integer above 2^53.

A client that does not know the optional fields can ignore them.
