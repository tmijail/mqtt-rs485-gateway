# The security of the web UI and the claim window

This document tells why the web UI, the REST API and the claim window have their security rules, and which risks stay. For the rules themselves, refer to [Commissioning and secrets](../reference/commissioning-and-secrets.md) and [The REST API](../reference/rest-api.md). For the access point, refer to [The security of the access point](access-point-security.md).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The behaviour in this document comes from the E2E in QEMU.

## The basis: physical access is full access

The project burns no eFuses (#133). Thus a person with physical access to the board can read the flash and flash a new image. The security of the gateway does not try to stop that person. It must not give more access than physical access gives.

- The shell has no password, because the console needs the USB-C connector.
- The NVS of the board is not encrypted. A dump of the flash shows the secrets. For the reasons, refer to [The features that burn eFuses](no-efuse-features.md).
- A push of BOOT can open the claim window, because the same person can flash the board.

## Why the first trust comes from the fingerprint

At the first boot, no other party certifies the certificate of the gateway. The gateway makes a self-signed certificate, and the console shows its fingerprint. The operator compares that fingerprint with the fingerprint in the browser. A printed label with a key or a fingerprint needs a factory step, and an open-source project has no factory.

The certificate does not name the IP address, because DHCP can change it. A plant that wants no browser warning loads its own certificate, signed by the plant CA.

## Why the claim window has these rules

The claim window lets an installer set the first web password with a phone, without a computer at the panel (#120).

| Rule | Reason |
|---|---|
| 10 min, and a new push starts the time again | Sufficient to connect a phone and type a password. A short time decreases the time for a claim by another person |
| One claim, then the window never opens again | One claim for each window gives no second chance to a person who comes after the operator. A recovery through the window would let any person at the panel replace the password of the owner, and keep the configuration and its secrets |
| A restart closes the window | A restart must not keep a window that nobody sees |
| Only a client of the access point | A person on the plant network must not take the gateway while the window is open. To use the window, a person must be in the radio range during the window |
| The rule examines the local address and the peer | lwIP of ESP-IDF sends the answers of a connection by its source address (`ip4_route_src_hook`). A host on Ethernet that sends to `192.168.4.1` gets no answer. The check of the peer is a second barrier |
| The access point is open from the push to the claim | Before the claim, the operator has no key. The commissioning password is only on the console |
| WPA2 with the new password 5 s after the claim | After the claim, the operator knows the key. The 5 s let the browser receive the response with the message to connect again |
| The success page tells how to connect again | The change to WPA2 disconnects the operator. Without the text, the operator does not know why, or how to connect again |
| The window opens in all modes | The web server runs in all modes, and the first password is necessary in all of them |
| The window needs the access point | Without the access point, nobody can use the window |
| The LED pattern and the beeps | The operator at the panel sees that the window is open, and hears that somebody used it. No beep tells the operator to push again |
| A refusal because of the state counts as a failed login | An attacker cannot try the route without a limit |
| The log never records the password | The log can go to a syslog server (REL-4) |

## Why the CSRF check has this form

Each write must have the header `X-Requested-With`, and the `Origin` or `Referer` must be the gateway (#100). A page of another site cannot add the header without a CORS preflight, and the gateway answers no `OPTIONS` request.

- **The check runs before the password check.** Thus a request from another site does not test credentials, and does not count as a failed login. Another site cannot block the operator.
- **The names are compared in a normal form (#106).** Any name of the gateway is correct, if the page and the request use the same name. The IP in one and `gw.local` in the other are two origins, and this is correct: the browser also sees two origins, and the page of the gateway never sends that request.
- **A `GET` is not examined.** A `GET` changes nothing. Another site can send it, but cannot read the response.
- **DNS rebinding.** A site that makes its own name resolve to the IP of the gateway sends the same `Host` and `Origin`. Thus the CSRF check does not stop it. Two other items stop it. The certificate is not for that name, thus the browser asks the user before it accepts it. The browser keeps the password for each origin, thus it does not send the password to that site. The server does not compare the `Host` with a list of its own names.

Nobody examined the check in a real browser. The E2E sends the requests that a browser would send.

## Why the web UI is for a trusted network

`esp_http_server` has one task for each server. One client can keep that task busy while it sends its request. The gateway decreases this time, but it cannot prevent it. Thus the web UI and the API are for a trusted network, a VPN or a management network.

**The stall watchdog (#110).** A slow client (slowloris) can send one header byte each 2 s. Each read waits up to 5 s, thus the server never releases that client. The task `web_guard` closes the sessions of a server that does not return to its loop in 8 s. It is a separate task with a low priority, not an `esp_timer` callback. The `esp_timer` task also samples the DIs at 1 kHz, and it must not block. In QEMU, the watchdog made the wait of a normal `GET` much shorter.

**Only the session in the handshake (#291).** `esp_https_server` makes the TLS handshake of a new session in the task of the server. The session gets its TLS context only after the handshake. Thus a session without a TLS context is the session that holds the task, and the watchdog closes only that session. The sessions of the other clients stay open. After the handshake, the watchdog cannot find the session that holds the task, and it closes all the sessions.

**TLS 1.2 and TLS 1.3 (#291).** The product owner decided that the web server accepts TLS 1.2 and TLS 1.3 (2026-11-27). mbedTLS reads each ClientHello in its TLS 1.3 code first. It gives a ClientHello without TLS 1.3 to its TLS 1.2 code, which reads the same record again. The dynamic TLS buffers of ESP-IDF v6.1 (`CONFIG_MBEDTLS_DYNAMIC_BUFFER`, #80) released that record before the TLS 1.2 code read it. The server then waited for a record that the client never sent, and the watchdog closed all the sessions. `gw_webtls.c` keeps the buffer while mbedTLS keeps the message in it. The dynamic buffers stay, because an idle TLS session then keeps no record buffers.

**The bodies of rejected requests (#106).** After a response, `esp_http_server` reads and discards the remaining body in pieces of 32 B, in its single task. A large `POST` without credentials could keep the web busy for the full upload, again and again.

- Some rejections are not trusted: no credentials were examined yet, or the request comes from another site. With a body of more than 1 KB, the server then stops the read and closes the session at once.
- For a rejection of an authenticated request from the same origin, the server reads the full body. A cut would give the web UI a network error in place of the code and the reason. The cost is that an authenticated client can keep the web busy for the time of its upload.
- In QEMU, with a slow upload on the HTTP port, a `GET` waited much longer without the cut than with it. On HTTPS, the `GET` did not wait also without the cut, and the cause is not known.

**The limit of attempts (#64).** The limit stops a fast password search from one address. It does not stop many addresses. Behind a NAT, one user who makes errors blocks the other users for some time.
