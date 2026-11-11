# Set the first web password on site

This procedure tells how to set the first web password of an installed gateway without a computer at the panel. It uses the claim window (CFG-8, issue #120). The reasons for the design and the residual risks are in `idf/README.md` §3.5 (The claim window) and §6.2.1.

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The access point and its open period are compile-only. The E2E tests the window in QEMU over the emulated Ethernet.

> **Caution: Stay at the panel until you set the password.** While the window is open, the access point has no key. Another person in the radio range can connect and set the password first.

## Before you start

You need:

- Physical access to the board: the BOOT button and the status LED.
- A phone or a laptop with Wi-Fi and a browser.
- A new web password: at least 8 characters, at most 64 bytes.

The window opens only when no web password is set. This is the case on a new gateway, after a factory reset (CFG-5), and on a gateway that never got a web password. When a web password is set, use it. When you do not know it, refer to `idf/README.md` §3.5 (Get access again when you do not know the web password).

## The signals of the gateway

| Signal | Meaning |
|---|---|
| LED white, blinks fast (from 2 s of the push) | Release the button now for the access point and the window. At 10 s, the gateway does a factory reset |
| 1 beep when you release the button | The access point starts. The claim window opens when no web password is set |
| LED blue and white, alternately | The claim window is open |
| 1 beep after you set the password | The gateway stored the password. The window is closed |
| LED blue, blinks slowly | Commissioning: the gateway has no configuration (after a factory reset) |

## Procedure

1. Push the BOOT button for 2 to 10 s. Release it when the LED blinks white.
2. Make sure that the LED shows blue and white, alternately.
3. On the phone, connect to the Wi-Fi network `GW-Setup-XXXXXX`. The network has no key during the window. `XXXXXX` is the end of the MAC address of the board.
4. Open `https://192.168.4.1/` in the browser. Accept the certificate of the gateway (it is self-signed). The browser goes to the page "Set the web password".
5. Type the new password two times. Click "Set the password".
6. Make sure that the page shows "The web password is set" and the user name. In commissioning and in recovery, any user name is valid.
7. Open `https://192.168.4.1/`. Log in with the user name and the new password. Make sure that the web UI opens.

The window is open for 10 min after the push (`CONFIG_GW_WEB_CLAIM_WINDOW_MIN`). Another push of BOOT starts the time again. For the rest of the 10 min, the access point stays open, and you can continue in the web UI. Then the access point uses WPA2 with the commissioning password again. Continue on the plant network, at `https://<ip>/` or `https://<hostname>.local/`.

## If the page refuses the password

| Message | What to do |
|---|---|
| "No claim window is open" | The time is over, or you did not push BOOT. Do step 1 again |
| "accepts only a client of the access point" | You are on the plant network. Connect to `GW-Setup-XXXXXX` (step 3) |
| "a web password is set" | Somebody set the password. If it was not you, do a factory reset (push BOOT for more than 10 s), then do this procedure again |
| "at least 8 characters" | Type a longer password |
| HTTP 429 | Too many refused attempts from your address. Wait 30 s |

## After the procedure

- Load the configuration of the plant in the web UI. Keep `web.password` as `********`: this keeps the stored password. A configuration without the `web.password` key removes the password.
- The window does not open again while a web password is set. Only a factory reset removes the password.
