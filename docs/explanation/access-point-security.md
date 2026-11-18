# The security of the access point

This document tells why the commissioning access point of the gateway operates as it does since issue #187. It gives the residual risks and compares the design with comparable projects.

- For the procedures, refer to [Set the first web password on site](../how-to/set-the-first-web-password.md) and [Get on the access point on a later visit](../how-to/get-on-the-access-point.md).
- For the keys, the times and the signals, refer to [The access point](../reference/access-point.md).
- For the choices of the implementation, refer to [The secrets in NVS](../reference/commissioning-and-secrets.md#the-secrets-in-nvs) and [What only the board can test](../reference/commissioning-and-secrets.md#what-only-the-board-can-test).

## The problem

The board is in an electrical panel. On site, the operator almost never connects a computer to the USB-C connector. Thus a secret that only the serial console shows is not available to the operator.

Before issue #187, the WPA2 key of the access point was the commissioning password, and only the console showed it. After the first web password, the operator could not use the access point: the commissioning password did not open the web UI, and the claim window did not open again. Also, the access point stayed open for the full claim window after the claim, and it ran for 30 min or with no limit.

## Why the WPA2 key is the web password

The operator knows the web password. Thus the operator can get on the access point without the console, also when the plant network is down.

The other possible keys have problems:

- **A fixed default key** is the same on all gateways. Anyone who reads the source code knows it. The default key `wled1234` of WLED is an example of this problem.
- **A key per device on a label** needs a factory step that reads the key and prints the label. Teltonika and ESP RainMaker do this. An open-source project that anyone can build and flash has no factory.
- **The commissioning password** is unique per device, but only the console shows it.
- **A key that the owner sets in the configuration** (for example `network.ap.password`) is a second secret to keep. The product owner put it out of the scope of issue #187.

ETSI EN 303 645, provision 5.1-1, requires a password that is unique per device or that the user defines. This applies in all states except the factory default. The web password is a password that the user defines. Before the first web password, the commissioning password is unique per device.

A person who knows the web password gets on the access point. This gives no new access, because that person can already use the web UI from the plant network.

## Why the gateway keeps only the PMK

The gateway must not keep a password in clear text. For the web UI, it keeps only a salted hash of the web password (PBKDF2-HMAC-SHA256). But the access point needs a WPA2 key, and the hash of the web UI is not a WPA2 key.

WPA2-PSK does not use the passphrase directly. Both sides calculate a Pairwise Master Key (PMK) from it: PBKDF2-HMAC-SHA1 of the passphrase, with the SSID as the salt, 4096 iterations, 32 bytes (IEEE 802.11i). The 4-way handshake uses only the PMK. Thus the gateway calculates the PMK one time, when the web password gets a new value, and keeps only the PMK. The SoftAP of ESP-IDF gets the PMK as 64 hexadecimal characters.

The PMK has these properties:

- **It is not the web password.** The PMK opens the access point, but it does not open the web UI. The web UI asks for the password and compares it with the hash.
- **It resists a brute-force attack approximately as much as the hash of the web UI.** Both are PBKDF2 with thousands of iterations. A weak web password is weak in both.
- **It depends on the SSID.** The SSID comes from the MAC address (`GW-Setup-XXXXXX`), thus it does not change, and the PMK stays correct. If the SSID becomes configurable, the gateway must calculate the PMK again, and for this it needs the password.
- **It permits WPA2 only.** WPA3 (SAE) needs the passphrase at each connection, not the PMK.

The calculation takes some time (approximately 16 000 SHA-1 compressions in software). The gateway does it only when the password changes, never in the polling, encoding or publish path.

Nobody has examined on the board that the SoftAP of ESP-IDF v6.1 accepts a PMK of 64 hexadecimal characters (#130 item N9, campaign #18). The documentation of ESP-IDF does not say it. The code of ESP-IDF shows it: `hostapd_derive_psk` in `components/wpa_supplicant/src/ap/ap_config.c` copies the PMK without PBKDF2.

## Why the rule of the password is ASCII

The web password is also the WPA2 passphrase. A WPA2 passphrase has 8 to 63 printable ASCII characters (0x20 to 0x7E), as hostapd and wpa_supplicant define it. A key of 64 characters is a PMK in hexadecimal, not a passphrase.

Other characters are not safe. Android converts them to UTF-8, and the behaviour of iOS and Windows is not known. Thus the same letter can give a different PMK on a different phone, and the phone cannot connect. One rule for the web password and for the WPA2 key prevents a password that opens the web UI but not the access point.

## Why the access point stops when nobody needs it

An interface that nobody uses is an attack surface with no use. ETSI EN 303 645, provision 5.6-1, requires that the device disables unused interfaces. Thus the access point stops:

- on request, when the operator completes the work (the button "Close the access point");
- after 5 min without a client, in all modes;
- in normal mode, 30 min after the push of BOOT, also with a client.

After a stop, only a new push of BOOT starts the access point again, and a person who can push the button has physical access. The only other start is the boot of a mode without a valid configuration (recovery, commissioning, safe mode, the PSRAM fault mode).

## Why the idle stop also closes the claim window

The claim window accepts only the clients of the access point. Without the access point, nobody can use the window. Thus, when the access point stops, the window closes too, also before the end of its 10 min (decision of 2026-11-14 in #187).

This has these effects:

- **The open period is shorter.** When no client connects in 5 min after the push, the operator is probably not at the panel. The open access point then stops, and nobody else can claim the gateway later in the window.
- **The LED tells the truth.** The pattern of the claim window (blue and white) stops when nobody can use the window.
- **The operator has the full window when connected.** While a client is connected, the idle time does not apply. The access point stays on until the claim or the end of the window.

## Why a forgotten web password needs a factory reset

When a web password is set, the claim window never opens again. A password recovery that keeps the configuration is refused (decision of #120, confirmed in #187).

The claim window is for a gateway that has no owner credential yet. A recovery through the window would let any person who can push the button replace the password of the owner. That person would keep the configuration and its secrets, for example the MQTT password. The gateway would continue its work, and the owner would see no change.

A factory reset also needs only the button. But it erases the configuration and the secrets, thus the gateway stops its work, and the owner sees it. After the reset, the operator sets a new first web password and loads the configuration again.

The owner with a computer at the panel has a path that keeps the configuration: the shell (`config load`) on the USB console. The shell has no password, because the console needs physical access. The same physical access lets a person read the flash and flash the board.

## Residual risks

### The open period

From the push of BOOT to the claim, the access point has no key. This period is usually 1 to 3 min. It is 10 min at most, or 5 min when no client connects. Before issue #187, it was 10 min in all cases.

During this period:

- **Another person can claim the gateway.** A person in the radio range can connect and set the password before the operator. After the claim, the operator must make sure that the password opens the web UI. If it does not, do a factory reset.
- **Open Wi-Fi does not encrypt the traffic.** HTTPS protects the password, but nobody compares the fingerprint of the certificate (next item).

### A false access point

Before the claim, the operator does not see the fingerprint of the certificate: only the console shows it. A person in the radio range can operate a false access point with the same name and read the new password (man in the middle).

After the claim, WPA2-PSK also authenticates the gateway to the client. A false access point without the PMK cannot complete the 4-way handshake. This decreases the risk of a false access point with the same name on a later visit. A person who knows the web password can still operate a false access point, but that person can already use the web UI.

### Physical access

The NVS of the board is not encrypted (#133). A person with physical access and a USB cable can read the PMK and the other secrets from the flash. That person can also flash the board. The project does not burn eFuses on the board, thus the owner can always flash and read the board (#133).

### Other limits

- **No PAKE.** Matter and ESP-IDF provisioning authenticate the device with a PAKE (a password-authenticated key exchange). The gateway does not. No ESP firmware that we found shows the fingerprint of the TLS certificate to the operator.
- **No test with a radio.** QEMU has no radio. The E2E examines the life, the key and the status of a simulated access point. The board test is #130 item N9.

## What comparable projects do

This is the research of 2026-11-14 in #187:

| Item | Projects |
|---|---|
| A short window that a physical action opens, and that closes at the end of its time | ESPHome, Tasmota (3 min), WLED (5 min), Shelly (15 min), Matter (3 to 15 min) |
| The window closes after a successful commissioning | Matter, ESP-IDF `wifi_provisioning`, ESPHome, Shelly |
| An open access point that stays on after the commissioning | A published vulnerability of Shelly Gen4 (Pen Test Partners). Shelly promised to close it outside the window |
| A fixed default key | Criticized (WLED `wled1234`) |
| A key per device on a label | Needs a factory step (Teltonika, ESP RainMaker) |
| The device authenticates itself to the operator | Matter and ESP-IDF provisioning, with a PAKE. No ESP firmware that we found shows the TLS fingerprint |

The gateway follows the common pattern: a physical action opens a short window, and the window closes at the claim. It does not keep an open access point after the claim, and it has no fixed default key.

Sources:

- ESPHome: <https://esphome.io/components/wifi/> and <https://github.com/esphome/esphome/blob/dev/esphome/components/provisioning/provisioning.h>
- Tasmota: <https://tasmota.github.io/docs/Commands/> and <https://tasmota.github.io/docs/Buttons-and-Switches/>
- WLED: <https://github.com/wled/WLED/blob/main/wled00/const.h> and <https://github.com/wled/WLED/blob/main/wled00/wled.cpp>
- Shelly: <https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/WiFi> and <https://pentestpartners.com/security-blog/shelly-iot-door-controller-config-fail-leaving-your-garage-home-and-security-exposed>
- Matter: <https://github.com/project-chip/connectedhomeip/blob/master/src/app/server/CommissioningWindowManager.cpp>
- ESP-IDF provisioning: <https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/api-reference/provisioning/wifi_provisioning.html>
- ETSI EN 303 645 v3.1.3: <https://www.etsi.org/deliver/etsi_en/303600_303699/303645/03.01.03_60/en_303645v030103p.pdf>
- WPA2 passphrase: <https://git.w1.fi/cgit/hostap/plain/hostapd/hostapd.conf>
- ESP-IDF hostapd PMK: <https://github.com/espressif/esp-idf/blob/master/components/wpa_supplicant/src/ap/ap_config.c>
