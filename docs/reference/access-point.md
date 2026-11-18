# The access point

This document gives the facts about the commissioning access point of the gateway (issue #187): its key, its times, its signals, its REST API and its shell commands.

- For the procedures, refer to [Set the first web password on site](../how-to/set-the-first-web-password.md) and [Get on the access point on a later visit](../how-to/get-on-the-access-point.md).
- For the reasons and the residual risks, refer to [The security of the access point](../explanation/access-point-security.md).
- For the choices of the implementation, refer to [The secrets in NVS](commissioning-and-secrets.md#the-secrets-in-nvs) and [What only the board can test](commissioning-and-secrets.md#what-only-the-board-can-test).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The radio of the access point is compile-only. The QEMU build runs a simulated access point with the same life, key and status, but with no radio. Nobody has examined the PMK as the key of the SoftAP on the board yet (#130 item N9).

## Network and address

| Item | Value |
|---|---|
| Network name (SSID) | `GW-Setup-XXXXXX`. `XXXXXX` is the last 3 bytes of the MAC address of the access point, in hexadecimal |
| Address of the gateway | `192.168.4.1`, the default of ESP-IDF. The subnet is `192.168.4.0/24`. You cannot change it |
| Web UI | `https://192.168.4.1/`. With `web.https` set to `false`: `http://192.168.4.1/`. Plain HTTP only redirects to HTTPS |
| Captive portal | None. Type the address in the browser |
| Security | WPA2-PSK, or open (no key) during a claim window. No WPA3 |
| Modbus TCP | Always refused through the access point (issue #120) |

The web server starts with the access point, also when `web.enabled` is `false`.

## When the access point starts

| Event | Modes |
|---|---|
| A push of BOOT for 2 to 10 s, after the end of the boot | All modes |
| The boot | Only the modes without a valid configuration: recovery, commissioning, safe mode and the PSRAM fault mode |

A push of BOOT while the access point runs starts its times again. A push during the boot has no effect and gives no beep.

## The key in each state

| State | Key of the access point |
|---|---|
| No web password | WPA2 with the commissioning password. Only the serial console shows this password: `ap: WPA2 key = commissioning password '…'` |
| Claim window: no web password, after a push of BOOT | Open (no key), from the push of BOOT to the claim. 5 s after the response of the claim, WPA2 with the new web password. At the end of the window without a claim, WPA2 with the commissioning password |
| Web password set | WPA2 with the web password. The gateway keeps only the PMK of the password |
| Web password set, but no PMK stored | WPA2 with the commissioning password, until the next change of the web password. This applies to a web password that an earlier firmware set |

A change of the key disconnects the clients of the access point.

## The rule of the web password

The web password is also the WPA2 key, thus it obeys the rule of a WPA2 passphrase:

- 8 to 63 characters.
- Each character is printable ASCII, from 0x20 (space) to 0x7E (`~`): the letters A to Z and a to z, the digits, the space and the symbols of a keyboard.
- The same rule applies to `web.password` in the configuration and to `POST /api/claim`.
- A stored password that does not obey this rule stays valid for the web UI until its next change. It has no PMK (see the table above).

## Times

| Time | Value | Option | Modes |
|---|---|---|---|
| Open period (the claim window) | 10 min after the push of BOOT, or less (see the next rows) | `CONFIG_GW_WEB_CLAIM_WINDOW_MIN` (1 to 30), menu `Gateway: web` | All modes, only when no web password is set |
| Change to WPA2 after the claim | 5 s after the response of `POST /api/claim` | `GW_APLIFE_CLAIM_SECURE_MS` (a constant in `gw_aplife.h`, not a Kconfig option) | All modes |
| Idle time: the stop without a client | 5 min. The time starts when the access point starts and when the last client disconnects | `CONFIG_GW_SOFTAP_IDLE_MIN` (1 to 60), menu `Gateway: commissioning access point` | All modes |
| Maximum time | 30 min after the push of BOOT, also with a client | `CONFIG_GW_SOFTAP_TIMEOUT_MIN` (1 to 1440), menu `Gateway: commissioning access point` | Normal mode only. The other modes have no maximum time |
| Stop on request | Approximately 2 s after the response of `POST /api/ap/stop`. Immediately with the shell `ap stop` | None | All modes |

## Stops

| Stop | Cause | `last_stop` in `/api/status` |
|---|---|---|
| On request | The button "Close the access point" of the web UI, `POST /api/ap/stop` or the shell `ap stop` | `request` |
| No client | The idle time without a client | `idle` |
| Maximum time | The end of the maximum time (normal mode only) | `time` |

- A stop also closes an open claim window, also before the end of its 10 min. The window accepts only the clients of the access point.
- While a client is connected, the idle time does not apply. During a claim window, the access point then stays on until the claim or the end of the window.
- A new push of BOOT for 2 to 10 s starts the access point again.
- A restart stops the access point and closes the claim window.

## LED

The patterns of the access point in the table of the status LED (refer to [The status signals](status-signals.md#status-led)):

| State | Pattern |
|---|---|
| Claim window open, or access point open (no key) | Blue and white, alternately (each color 250 ms) |
| Access point on with WPA2 | A short cyan flash (125 ms) each 2 s. Between the flashes, the LED shows the pattern of the gateway, from "Commissioning" to "Normal operation" |

The patterns "Factory reset in progress" (white, continuous) and "BOOT pushed for more than 2 s" (white, blinks at 4 Hz) have a higher priority. The log gives the name of the pattern, for example `led: status: operating, access point`.

## Buzzer

| Event | Beeps |
|---|---|
| BOOT released after 2 to 10 s, after the end of the boot | 1 beep: the access point starts. When no web password is set, the claim window opens |
| BOOT released after 2 to 10 s, during the boot | No beep: the gateway ignores the push. The log gives `BOOT gesture ignored: still starting (no beep)` |
| The claim window set the web password | 1 beep |
| BOOT pushed for 10 s | 2 beeps: the factory reset starts |

One beep is 150 ms of tone and 150 ms of silence.

## The card "Access point" of the web UI

The Status tab of the web UI has a card "Access point". It shows the network name, the key, the number of clients and the time to the stop. When the access point is on, the card has the button "Close the access point". The button opens a confirmation, and then sends `POST /api/ap/stop`. When `request_via_ap` is `true`, the confirmation also tells that the page stops, because the browser uses the access point.

![The card 'Access point' of the Status tab on a phone: the access point GW-Setup is on, its Wi-Fi password is the web password, it has 1 client, and it stops in 28 min. Below is the button 'Close the access point'.](../images/ap-card-phone.png)

The image comes from the firmware in QEMU, with one simulated client.

## The `ap` object of `GET /api/status`

| Key | Type | Value |
|---|---|---|
| `available` | bool | The build has an access point (the radio, or the simulated access point of the QEMU build) |
| `simulated` | bool | The simulated access point of the QEMU build (no radio) |
| `running` | bool | The access point is on |
| `ssid` | text | The network name. It comes from the MAC address, thus it is also known before the first start. `""` only when the gateway cannot read the MAC address |
| `security` | text or `null` | `open` or `wpa2`. `null` when the access point is off |
| `key` | text or `null` | `web_password` or `commissioning_password`. `null` when the access point is off |
| `clients` | integer | The number of clients |
| `left_s` | integer or `null` | The time in seconds to the stop, if no client connects or disconnects. `null` when there is no limit now: a client is connected, and there is no maximum time |
| `open_left_s` | integer | The time in seconds of the open period that is left. 0 when the access point is not open |
| `idle_stop_s` | integer | The idle time in seconds |
| `last_stop` | text | The cause of the last stop: `none` (no stop since the boot), `request`, `idle` or `time` |
| `request_via_ap` | bool | This request came through the access point: the address of the client is in the subnet of the access point (the rule of the claim window). `false` on the Ethernet and on the Wi-Fi station. The QEMU build has no radio: there, it is `true` only after the emulator-only shell command `claimsim … eth` (issue #193) |

Example (normal mode, after a push of BOOT, one client, a request from that client):

```json
"ap": {"available": true, "simulated": false, "running": true, "ssid": "GW-Setup-A1B2C3",
       "security": "wpa2", "key": "web_password", "clients": 1, "left_s": 1680,
       "open_left_s": 0, "idle_stop_s": 300, "last_stop": "none", "request_via_ap": true}
```

## `POST /api/ap/stop`

The request stops the access point. It has the same checks as all the other write requests: HTTP Basic with the web credential, and the CSRF check (the header `X-Requested-With`; `Origin` or `Referer` must be this gateway). The request has no body.

| Status | Body | What it tells |
|---|---|---|
| `202` | `{"ok":true,"stopping":true}` | The access point stops approximately 2 s later. The client gets the response first |
| `401` | | No credential, or a credential that is not correct |
| `403` | | The CSRF check failed |
| `409` | `{"ok":false,"error":"the access point does not run",…}` | The access point is off |
| `429` | | Too many failed logins from this address. The address is blocked for a time |

Example:

```sh
curl -k -u admin:<web password> -X POST -H "X-Requested-With: x" https://192.168.4.1/api/ap/stop
```

## `POST /api/claim`: the items of the access point

A successful claim gives `200` with the object `ap`: `ssid`, `url` (the address of this connection, for example `https://192.168.4.1/`) and `secure_in_s` (5: the seconds to the change to WPA2). `plant` gives the addresses of the gateway on Ethernet and on the Wi-Fi station. `mdns` gives the mDNS name (`null` when mDNS is off). [The REST API](rest-api.md#post-apiclaim) gives the full request and response.

## Shell commands

| Command | Result |
|---|---|
| `status` | The status of the gateway, with one line `ap` (the same line as the command `ap`) |
| `ap` | The state of the access point. For example: `ap       'GW-Setup-A1B2C3' on, WPA2 with the web password, 1 client(s), stops in 1680 s (idle stop 300 s)`, or `ap       off (last stop: on request); a BOOT push of 2-10 s starts it` |
| `ap stop` | Stops the access point immediately. Only with `CONFIG_GW_SHELL_WRITE_COMMANDS` |

The texts of the last stop are `never stopped`, `on request`, `no client` and `end of its maximum time`. During the open period, the line shows `OPEN (no key) for N s more`.

These commands are only in the QEMU build (`CONFIG_GW_IO_SIMULATED`):

| Command | Result |
|---|---|
| `apsim clients <0-10>` | Sets the number of clients of the simulated access point |
| `apsim idle <0-3600>` | Sets the idle time in seconds. 0 sets the Kconfig time again |
| `apsim key` | Shows the start of the SHA-256 of the stored PMK, or `no PMK stored`. It never shows the PMK |
| `claimsim <0-3600> [eth]` | Sets the time of the next claim windows in seconds (0 = the Kconfig time). With `eth`, the emulated Ethernet counts as the access point, for the claim window and for the Modbus TCP gateway |

## Secrets

| Secret | Where | When it changes |
|---|---|---|
| PMK of the web password | NVS, key `ap_pmk`, with the other secrets. 64 hexadecimal characters: PBKDF2-HMAC-SHA1 of the password, with the SSID as the salt, 4096 iterations, 32 bytes | The gateway calculates it only when `web.password` gets a new value: the claim, `PUT /api/config`, `config load`, and a `config.json` with the password in clear text at boot. A document without `web.password` removes it. The factory reset erases it |
| Commissioning password | NVS. 12 random characters | At the first boot and after a factory reset |

The NVS of the board is not encrypted (#133).
