# Commissioning and secrets

This document gives the facts about the access to a gateway. It covers the modes, the claim window, the HTTPS certificate, the secrets, the factory reset, the shell and the limits of the web server.

- For the procedures, refer to [Set the first web password on site](../how-to/set-the-first-web-password.md), [Recover a gateway](../how-to/recover-the-gateway.md), [Restart into safe mode or do a factory reset](../how-to/restart-into-safe-mode-or-reset.md) and [Manage the certificates](../how-to/manage-the-certificates.md).
- For the access point, refer to [The access point](access-point.md). For the routes, refer to [The REST API](rest-api.md).
- For the reasons of the rules, refer to [The security of the web UI and the claim window](../explanation/web-and-claim-security.md) and [The security of the access point](../explanation/access-point-security.md).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The E2E examines these functions in QEMU. The radio of the access point, the BOOT button, the LED, the buzzer and the mDNS responses are compile-only.

## The modes

A mode is the state that the boot reached. For the table of the modes and the LED patterns, refer to [The status signals](status-signals.md#modes).

| Mode | What it means for the access |
|---|---|
| `normal` | The configuration is valid. The web server starts only with `web.enabled: true`. A push of BOOT for 2 to 10 s starts it until the next restart |
| `recovery` | The configuration is rejected, or safe mode, or a factory reset that is not complete (refer to [The sequence of the factory reset](#the-sequence-of-the-factory-reset)). The web server always starts. The saved web password is valid with any user name, because the user name is in the rejected document |
| `commissioning` | After a factory reset, or at the first boot of a board with a blank `storage` partition (refer to [A board with a blank `storage` partition](#a-board-with-a-blank-storage-partition)). The gateway does not use the embedded example configuration or its Sparkplug identity. It waits for a configuration from the web UI or the shell, with a new commissioning password. The saved web password, if there is one, is valid with any user name |
| `fault` | The boot did not find the PSRAM. Only the network, the web server, the shell and the access point start |

For recovery mode, refer to [The configuration document](configuration.md#load-and-recovery-mode). For safe mode, refer to [Firmware updates](firmware-updates.md#the-boot-loop-counter-and-safe-mode).

## Access to the web UI and the REST API

- The web UI and the API use HTTPS by default (`web.https`). The HTTP port redirects to HTTPS. The only exception is the captive portal for the clients of the access point: a guide page with no form and no secret (#274). Refer to [The access point](access-point.md#the-captive-portal).
- All routes require HTTP Basic with `web.username` and `web.password`. The only exceptions are the routes of the claim window.
- While no web password is set, use the user `admin` and the **commissioning password**.
- The commissioning password has 12 random characters, and it is different on each gateway. The gateway makes it at the first boot and after a factory reset, and keeps it in NVS.
- The console shows the commissioning password at each boot: `web: no web password set; user 'admin', commissioning password '…'`. The firmware writes this line with `printf`. Thus the line never goes to syslog or to the live log of the web UI.
- The shell command `access` shows the commissioning password and the other access data again on request. Refer to [The command `access`](#the-command-access).
- After 5 failed logins in sequence from one address, the gateway blocks that address. For the times, refer to [The REST API](rest-api.md#authentication).

There are three paths to the first web password:

| Path | Document |
|---|---|
| The claim window: a phone at the panel, no computer | [Set the first web password on site](../how-to/set-the-first-web-password.md) |
| The USB console: read the commissioning password (the boot line, or the command `access`), then set `web.password` in the web UI | [Your first gateway](../tutorials/first-gateway-on-the-bench.md) |
| On the bench: load a configuration with `web.password` through the shell (`config load`), then install the gateway | [Change, back up and restore the configuration](../how-to/change-the-configuration.md) |

The web password is also the WPA2 key of the access point. Its rule is 8 to 63 printable ASCII characters (0x20 to 0x7E). Refer to [The access point](access-point.md#the-rule-of-the-web-password).

## The HTTPS certificate

At the first boot, and after a factory reset, the gateway makes a self-signed certificate (EC P-256). The console shows its SHA-256 fingerprint at each boot: `web: HTTPS certificate SHA-256 fingerprint AB:CD:…`. Compare it with the fingerprint in the browser before you accept the certificate.

- The certificate has `CN=<hostname>` and the SANs `<hostname>.local` and `<hostname>`. It does not have the IP address, because DHCP can change the address.
- The self-signed certificate is valid until 2099.
- A change of the hostname does not change the certificate. The card **Certificates** then gives a warning. Make a new self-signed certificate.
- The card **Certificates** loads the certificate of the plant, or makes a new self-signed certificate. Refer to [Manage the certificates](../how-to/manage-the-certificates.md).
- There is no automatic renewal. Load a new certificate of the plant before its last day.
- The gateway examines a certificate with mbedTLS before it keeps it: the PEM parses, and the key agrees with the first certificate.
- The REST API never gives a key, and the log never shows one.

**Warnings of the card (decisions of the product owner).** The card gives a warning when the SANs do not contain the hostname (`<hostname>`, `<hostname>.local` or `<hostname>.<domain>`). It gives a second warning when the certificate does not name the address of the page. The warnings block nothing.

**Where the gateway keeps it.** The certificate and its key are in NVS, with the secrets (#64).

| NVS keys | Content |
|---|---|
| `web_cert`, `web_key`, `web_src` | The certificate that the web server uses, its key and its source |
| `web_cert_n`, `web_key_n`, `web_src_n` | A new certificate that waits for the restart |

- At the next start of the HTTPS server, the gateway examines the new certificate again. Then it moves it to the keys of the certificate in use, and erases the keys of the new certificate.
- The button **Discard** (`DELETE /api/web/certificate/pending`) erases the new certificate before the restart.
- An NVS string has 4000 bytes at most. Thus the certificate with its chain has 3999 bytes at most, and the key also.

**The NVS space check.** The NVS partition has 24 KiB: 6 pages of 126 entries of 32 bytes. NVS keeps one page free. Before the gateway stores a new certificate, it calculates the free entries that it needs (`gw_webcert_nvs_need`):

- the entries of the certificate, the key and the source;
- the same entries again for their move at the next boot, because NVS writes a new string before it erases the old one;
- one more page, for the fragmentation and for the other writes.

If the NVS has not sufficient free entries, the response is `507`, and a certificate that waits stays. Without this check, a full NVS would also stop the writes of the local I/O state. In QEMU, the NVS has approximately 570 free entries with the certificate of the first boot. A chain of 3.5 KB with an RSA 2048 key needs approximately 440 entries. A certificate of 3999 bytes with a key of 3999 bytes needs approximately 610: it cannot wait for the restart.

## The PEM files

The PEM files for MQTT TLS and for the image server stay in the LittleFS partition, not in NVS. esp-mqtt needs the full client key in memory. For the keys that name the files, refer to [`mqtt.tls`](configuration.md#mqtttls). For the routes, refer to [The REST API](rest-api.md#the-pem-files).

- The limits are 16 files (`GW_PEM_FILES_MAX`) and 16384 bytes for each file (`GW_PEM_FILE_MAX_BYTES`), in `gw_certs.h` (decision of the product owner, #221).
- The `storage` partition has 1 MiB (256 blocks of 4 KiB). LittleFS gives one block or more to each file.
- The worst case is approximately 800 KiB, less than the partition: 16 files of 16 KiB, three documents of 128 KiB (`config.json`, `config.prev.json` and `config.next.json`), and a temporary copy of a file during a write.
- A replaced file keeps its name, thus the configuration does not change. The gateway uses the new content after the next restart. When the saved configuration uses the file, the new content must be of the same kind, and the client key must still agree with the client certificate.
- The replace dialog has no Restart button (decision of the product owner). Thus the operator can replace some files and restart one time.
- The gateway refuses to remove a file that the saved configuration uses (`409`).

## The claim window (CFG-8)

While no web password is set, a push of BOOT for 2 to 10 s opens a claim window (#120). During the window, a client of the access point can set the first web password at `https://192.168.4.1/claim`, without a credential. For the routes, refer to [The REST API](rest-api.md#the-claim-window). For the key of the access point during the window, refer to [The access point](access-point.md#the-key-in-each-state).

| Item | Rule |
|---|---|
| Time | 10 min (`CONFIG_GW_WEB_CLAIM_WINDOW_MIN`, 1 to 30). A new push of BOOT starts the time again |
| After a set | The window closes, and it never opens again. A factory reset (CFG-5) is the only way to remove the password |
| Restart | The window closes. Its state is only in RAM |
| Access point stops | The window closes (#187). The access point stops on request or after its idle time |
| Interfaces | Only a client of the access point. A client on Ethernet or on the Wi-Fi station gets `403` |
| The rule of the client (`gw_claim_from_ap`) | The local address of the TCP connection is the address of the access point, and the peer is in its subnet. No other interface has a subnet that contains the peer. IPv6 is refused |
| A plant network on `192.168.4.0/24` | The route back to the client is not clear, thus the gateway refuses the claim |
| Other routes | No change: they require the credential |
| Modes | All modes: normal, recovery, commissioning and fault |
| A build without `CONFIG_GW_SOFTAP` | The window does not open. The log tells this |
| Login block | The route has the CSRF check and the limit of attempts. A refusal because of the state (a password is set, no window, not a client of the access point) counts as a failed login |
| Log | The log records when the window opens and closes, and each refusal with the address. It never records the password |
| Success page | It tells the operator to connect again to `GW-Setup-XXXXXX` with the new password. It gives the address of the access point, the addresses on the plant network and the mDNS name |
| LED and buzzer | Refer to [The status signals](status-signals.md) |

**A password set through the window is only in NVS.** The configuration document does not change. At the next boot, a document without `web.password` takes the stored hash and PMK. A later `PUT /api/config` or `config load` without the key `web.password` removes the password and its PMK, as for each secret. The web UI exports `********`, which keeps them.

For the residual risks of the window (a claim by another person, the open period, a false access point), refer to [The security of the access point](../explanation/access-point-security.md#residual-risks).

## The secrets in NVS

These secrets are in NVS, never in `/cfg/config.json` (CFG-7):

| Secret | Form |
|---|---|
| `mqtt.password` | Text |
| `network.wifi.password` | Text |
| `web.password` | A hash: PBKDF2-HMAC-SHA256, 2048 iterations, random salt |
| The key of the access point | The PMK of `web.password` (`ap_pmk`). Refer to [The access point](access-point.md#secrets) |
| The commissioning password | Text, 12 random characters |
| The HTTPS certificate and its key | Refer to [The HTTPS certificate](#the-https-certificate) |

For the rules of a secret in a loaded document (`"********"`, a new text, `""`, no key), refer to [The configuration document](configuration.md#the-secrets).

- **The PMK.** The gateway calculates it only when `web.password` gets a new value: the claim, `PUT /api/config`, `config load`, or a `config.json` with the password in clear text at boot. The calculation is approximately 16 000 SHA-1 compressions in software. It never occurs in the polling, encoding or publish path. The log gives its time: `storage: WPA2 key of the access point … calculated in N ms`.
- **The hash and the PMK are two NVS entries** (`web_hash` and `ap_pmk`). At a change or a removal of the web password, the gateway erases the PMK first. Then it writes or erases the hash, and writes the new PMK last (#269). After a power cut, the stored PMK is thus the PMK of the stored password, or there is no PMK. Refer to [The access point](access-point.md#a-web-password-without-a-pmk).
- **The SSID is the salt of the PMK.** The SSID comes from the MAC address and does not change, thus the PMK stays correct.
- **Secrets in clear text in `config.json`.** The firmware accepts them, for example from `idf.py flash`. At boot, it moves them to NVS. Then it **erases the full LittleFS partition**, because LittleFS is copy-on-write and the old blocks would keep the text. Then it writes the file again without the secrets. During this time, the gateway has a commissioning mark: a power cut does not leave it with the example identity.
- **RAM and the core dump.** The secrets are in RAM while the gateway runs. A core dump can contain them. `GET /api/coredump` needs the web password, and the shell command `coredump dump` needs physical access. Keep a downloaded dump as safe as the passwords, and erase it from the gateway after the download.
- The NVS holds no bdSeq. The bdSeq is in RAM and starts at 0 at each boot (#161).

**Encryption of the NVS.**

| Build | NVS | eFuses |
|---|---|---|
| The default build, for the board (`idf.py set-target esp32s3 build`) | **Not encrypted.** The secrets are not in the JSON, but a person can read them from a dump of the flash. The shell gives `secrets  NVS NOT encrypted` | Burns no eFuse. You can flash any board, and flash it again |
| The QEMU builds for the main E2E scenarios (`build-qemu`, `build-qemu-psram`) | Not encrypted, as on the board (#261) | Burn no eFuse |
| The secure QEMU build (`sdkconfig.qemu-secure`, E2E only) | Encrypted with the HMAC scheme (`CONFIG_NVS_ENCRYPTION`, `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`), key in the eFuse block KEY5. The shell gives `secrets  NVS encrypted (HMAC key in eFuse KEY5)` | Burns KEY5 in the emulated eFuse file of each run. Nothing real is burned |

The project does not encrypt the NVS on the board (decision of #133). The HMAC key in eFuse is permanent, and the owner of the board must keep full control of it. For the reasons, refer to [The features that burn eFuses](../explanation/no-efuse-features.md). The secure QEMU build keeps the NVS encryption and its migration (decision of #133), because the optional owner profile of #255 can use them. For that migration, refer to [The NVS migration in the QEMU build](#the-nvs-migration-in-the-qemu-build).

## The random values

The gateway makes its random values with the random number generator (RNG) of the ESP32-S3 (#267). ESP-IDF gives a guarantee of true random numbers only while one of two entropy sources is on:

- the SAR ADC source (`bootloader_random_enable()`);
- the radio: the RF subsystem of the Wi-Fi driver, after `esp_wifi_start()` and not in a sleep mode.

The internal 8 MHz oscillator is always on, but it has no guarantee alone. mbedTLS gets each random value directly from the RNG (`esp_fill_random()`), and it keeps no seed from an earlier time. Thus the source that is on at the time of the request is the source of the value.

| Value | When the gateway makes it | Entropy source on the board |
|---|---|---|
| The commissioning password | At the boot, when the NVS has none: the first boot, and the boot after a factory reset | The SAR ADC source |
| The salt of the web password | At the boot, when `config.json` has `web.password` in clear text | The SAR ADC source |
| The salt of the web password | At the claim, `PUT /api/config` and `config load` | The radio |
| The key and the serial number of the HTTPS certificate | When the web server starts without a certificate, and with the button for a new self-signed certificate | The radio |
| The random values of each TLS session: the web UI, MQTT and the image server | At each connection | The radio |

**The sequence of the boot.**

| Step | What the gateway does |
|---|---|
| 1 | It starts the NVS. With the NVS encryption (the secure QEMU build only), ESP-IDF makes the HMAC key with its own on and off of the SAR ADC source |
| 2 | It turns the SAR ADC source on. It makes the commissioning password, and it loads the configuration |
| 3 | It turns the SAR ADC source off. The source must be off before the Wi-Fi driver, the ADC and the temperature sensor start |
| 4 | It starts the Wi-Fi driver in station mode. The station does not connect, and no access point starts. The power save is off: `WIFI_PS_NONE`, and no modem sleep while the station has no connection (`sta_disconnected_pm`). The radio stays on until the next restart |
| 5 | It starts the temperature sensor, the web server and the other parts |

- The radio starts at each boot, in each mode: normal, recovery, safe mode, commissioning and fault. It starts also when `network.wifi.enabled` is `false`, and also when Ethernet does not start.
- The radio stays on when the Wi-Fi backup, a scan or the access point stops. The Wi-Fi backup, the scan and the access point add and remove only their interface.
- No configuration key turns the radio off (decision of the product owner, #267).
- The log gives the source of each value, for example `rng: the commissioning password: made with the SAR ADC entropy source`. When no source with a guarantee is on, the log gives a warning.

Limits:

- When the Wi-Fi driver does not start, no source with a guarantee is on after step 3. The log gives `rng: the radio did not start`.
- A gateway that stops at the boot because it has no memory for the configuration keeps the SAR ADC source on. Only the shell runs.
- QEMU has no radio. After step 3, no source with a guarantee is on. Refer to [The E2E test bench](e2e-harness.md#limits-of-qemu).

## The factory reset (CFG-5)

The factory reset erases the full LittleFS partition and the NVS:

- the configuration, and the PEM files;
- the secrets, the web password and its PMK;
- the commissioning password and the HTTPS certificate;
- the pulse counters and the run hours of the local I/O.

The restart also sets the Modbus counters to 0, because they are only in RAM. The gateway then starts in commissioning mode, with a new commissioning password and a new certificate.

| Start | Rule |
|---|---|
| BOOT for 10 s at the panel | The firmware reads the button only after the boot, and it must see the button released one time. GPIO0 is a strapping pin: a push at power-on starts the download mode of the ROM. At 2 s, the LED changes to white. At 10 s, the gateway gives two beeps and erases |
| The web UI: the card **Restart and reset** of the Status tab, with the word `ERASE` | The same reset as BOOT (#203) |
| `POST /api/factory-reset?confirm=1` | The same reset. Refer to [The REST API](rest-api.md#post-apifactory-reset) |
| The shell command `factory-reset` | It asks for the word `ERASE` |

- The LED shows white during the reset. The buzzer task stops during a long flash erase, thus the two beeps sound fully before the erase.
- From a distance, the gateway erases, gives the answer, and restarts 0.5 s later.
- A commit hook can refuse the reset. Then nothing is erased (`409`).
- On real flash, the erase of the 1 MiB LittleFS partition takes some seconds (in steps of 64 KB).
- In QEMU, the emulator-only shell commands `boot 1` and `boot 0` push and release the button.

### The sequence of the factory reset

The reset erases two stores, LittleFS and the NVS, and an erase is not atomic. The commissioning mark is in the NVS, thus it cannot stay during the two erases. The gateway keeps a mark "the reset is not complete" in one store while it erases the other store (#266). After step 1, a power cut at each moment leaves a mark, and the next boot completes the reset.

| Step | What the gateway does | Where the mark is after the step |
|---|---|---|
| 1 | It writes the intent to the NVS (the key `freset` of the namespace `gw`) | The NVS |
| 2 | It erases the full LittleFS partition, then formats it | The NVS |
| 3 | It writes the mark file `/cfg/.factory-reset` to LittleFS | The NVS and LittleFS |
| 4 | It erases the full NVS, then starts it again. This also erases the intent | LittleFS |
| 5 | It writes the commissioning mark to the NVS (the key `factory` of the namespace `gw`) | LittleFS |
| 6 | It deletes the mark file | No mark: the reset is complete |

- If the NVS is not usable, or it refuses the intent (for example a full NVS), the gateway erases the NVS before step 1.
- A step that fails stops the sequence. The marks stay. The gateway restarts, and `POST /api/factory-reset` gives `"erased":false`.
- The sequence is in `gw_core` (`gw_freset.h`). `gw_storage` has only the operation of each step.

**The boot after a power cut.** The boot reads the two marks before it selects the mode:

| The boot finds | What the boot does |
|---|---|
| The intent in the NVS | Steps 2 to 6 again |
| Only the mark file | Steps 4 to 6 again |
| No mark | Nothing. No factory reset started, or each one is complete |

- The boot examines the intent first. While the NVS has the intent, the erase of the NVS did not complete, and a second erase of LittleFS loses nothing.
- With the mark file, the boot always erases the NVS again. An NVS that is partly erased can have old secrets, and a commissioning mark of an earlier reset.
- After the last step, the gateway restarts. That boot starts in commissioning mode, with a new commissioning password and a new certificate.
- The LED shows white during the steps. The log gives `storage: factory reset: a power cut or an error stopped it; this boot completes it from the erase of LittleFS` (or `of the NVS`).

**The state after a power cut:**

| Moment of the cut | State at the next boot |
|---|---|
| Before the intent is in the NVS (step 1) | The state before the reset. Nothing is erased. Do the factory reset again |
| From the end of step 1 until step 4 erases the intent | The boot does steps 2 to 6, then the gateway starts in commissioning mode |
| From that moment until the end of step 6 | The boot does steps 4 to 6, then the gateway starts in commissioning mode |

- No moment gives the embedded example configuration in normal mode. No old secret stays after the boot completes the reset.
- The gateway uses the embedded example configuration only when all these conditions are true. This is the state of a board after its first flash.
    - The boot finds no mark of a factory reset that is not complete.
    - The NVS has no commissioning mark.
    - LittleFS mounts, and it has no `config.json`.

**A reset that the boot does not complete.** The marks stay when a step fails again at the boot, and in safe mode.

- When a step fails at the boot, the gateway starts in recovery mode. The cause is `a factory reset is not complete (see the log): restart the gateway to complete it, or do the factory reset again`. Each boot tries the steps again.
- Safe mode does not do the steps, because a step can be the cause of the crashes. The first boot that is not in safe mode does them.
- While a mark is there, the gateway reads no configuration, and it refuses a configuration save (`409`).
- A flash of the `storage` partition does not remove the intent, because `idf.py flash` does not erase the NVS. The next boot erases the flashed configuration, and the gateway starts in commissioning mode. Flash the configuration again after that boot.

**Limits.**

- A reset that starts with an NVS that is not usable erases the NVS first. A power cut during that erase leaves the configuration, with all, a part or none of the old NVS. Do the factory reset again.
- An NVS with a hardware fault cannot hold the intent. The reset then continues without it, because the owner must be able to erase the configuration. Only the short time between the format of LittleFS and the mark file has no mark.
- The host test `test_freset` examines a power cut at each step, with each part result of an erase, also in the boots that follow. The E2E examines two cut points in QEMU, where no flash write is cut in the middle. The test on the board is item F6 of #128.

## A board with a blank `storage` partition

A flash that writes no LittleFS image leaves the `storage` partition blank after a full erase. The install tool of #219 does this, so that a flash without an erase keeps the configuration (#280).

- At the boot, when LittleFS does not mount and the partition is blank, the gateway writes the commissioning mark to the NVS. Then it formats the partition, and it starts in commissioning mode.
- The gateway formats the partition only after the NVS has the mark. Thus this board never uses the embedded example configuration.
- If the NVS cannot keep the mark, the gateway does not format the partition. It starts in recovery mode.
- The partition is blank when its first two blocks are erased.
- A partition with data that does not mount is not blank. The gateway does not format it at the boot, because it can contain a configuration. It starts in recovery mode.
- The gateway does not do this while a factory reset is not complete. The steps of the factory reset format the partition.
- A flash with `idf.py flash` writes the LittleFS image of `fs/`. That board uses the embedded example configuration, as before.

## The shell

- The shell has no password. The console needs physical access (USB-C), the same access that the BOOT button and the flash need.
- `CONFIG_GW_SHELL_WRITE_COMMANDS=n` gives a read-only console.
- The REPL starts immediately after NVS and the BOOT button, before the configuration loads (`shell: console ready`) (#72). A command that needs a part that did not start yet tells this, for example `mbread: not available yet: the RS485 bus is starting`. `status`, `devices` and `config` tell that the configuration is loading.
- Text that you type before the application starts (in the bootloader) is lost.
- `config load` reads the pasted document until its outer object closes. It stops after 60 s without data. The console read has no echo.
- While you paste, the shell does not take the lock of the shell context (#78). Thus the boot and the other tasks continue. The shell takes the lock only to validate, save and restart.
- `mbread` goes through the bus queue (MB-16): it waits until the current transaction ends. A device that does not exist gives `timeout` or `framing error`, as a function of the other devices on the bus.
- The shell gives the counters of the web server: `web … logins failed N, refused while blocked N, blocks N`, `bodies cut N` and `stalls N`.

### The command `access`

The command `access` gives the data that a person needs to open the web UI (#280). It changes nothing. The install tool of #219 reads its answer.

- The answer goes only to the console, in one write. It never goes to syslog or to the live log of the web UI.
- The answer has the commissioning password only while no web password is set.
- The answer has a fixed form: one `key value` for each line, between two marker lines.

```text
access-data v1
mode commissioning
firmware 0.2.0
hostname gw01
web on https 443
login commissioning
user admin
commissioning-password k7Qm2xPa9TbE
fingerprint 4F:9A:21:…
if eth up 192.168.1.57
if wifi off
if ap off
url https://192.168.1.57/
url https://gw01.local/
access-data end
```

| Key | Value |
|---|---|
| `mode` | `normal`, `recovery`, `commissioning`, `fault`, or `starting` while the configuration loads |
| `firmware` | The firmware version |
| `hostname` | The hostname. The line is absent when the gateway does not know it |
| `web` | `on`, then `https` or `http`, then the port of the web UI. `off` when no web server runs |
| `login` | `commissioning`: the user `admin` with the commissioning password. `web-password`: the user of the line `user` with the web password. `web-password-any-user`: the web password with any user name (recovery, commissioning and fault mode) |
| `user` | The user name. The line is absent with `web-password-any-user` |
| `commissioning-password` | Only with `login commissioning` |
| `fingerprint` | The SHA-256 fingerprint of the HTTPS certificate in use. The line is absent without HTTPS |
| `if` | One line for each of `eth`, `wifi` and `ap`: `off`, `no-link`, `no-address`, or `up` and the IPv4 address |
| `url` | One line for each IPv4 address of the web UI. Then the mDNS name, when mDNS is on and Ethernet or Wi-Fi has an address |
| `url-none` | Only when there is no `url` line. `web-off`: no web server runs. `no-address`: no interface has an address |

- A later firmware can add a key. A program that reads the answer ignores a key that it does not know.
- A change of the meaning of a key changes the number of the first line.
- A control character in a value from the configuration shows as `?`. Thus a value cannot make a line.

### The command `web-password remove`

The command `web-password remove` is for a gateway whose web password nobody knows (#280).

- It removes the hash of the web password and the PMK of the access point from the NVS. It erases the PMK first (#269).
- The configuration document and the other secrets stay.
- The gateway then restarts. After the restart, the commissioning password is valid again, and the command `access` shows it.
- The key of the access point is the commissioning password until a new web password is set.
- With no web password, the command changes nothing, and the gateway does not restart.
- During the trial of a new firmware, the command refuses. A restart then would start the earlier firmware again.
- It is a write command: a build with `CONFIG_GW_SHELL_WRITE_COMMANDS=n` does not have it.
- The command gives no new access. A person at the console can do the same with `config load`, and can flash the board. The claim window still cannot change a web password that is set (#120).

## The limits of the web server

**The web UI is for a trusted network.** Do not expose the web UI or the API to the Internet or to a network with hostile clients. Use a VPN or a management network. One client can keep the single task of `esp_http_server` busy while it sends its request. For the reasons, refer to [The security of the web UI and the claim window](../explanation/web-and-claim-security.md#why-the-web-ui-is-for-a-trusted-network).

| Item | Value |
|---|---|
| HTTPS connections at the same time | 3. With `lru_purge_enable`, a new session replaces the oldest session |
| mbedTLS memory for each TLS session | Approximately 40 KB, in PSRAM |
| Stack of the HTTPS server | 10 KB. The handshake and the validation of the configuration run in the same task |
| The HTTP redirect | A second server: 3 KB of stack, 2 connections |
| Addresses in the limit of attempts | 8. The gateway uses again the oldest address that is not blocked |
| A web upload of a firmware image | It runs in the task of the server, and blocks the web UI until it ends |

**The limit of attempts (#64).**

- Many different addresses, or false addresses on the local network, can each continue to try at the permitted rate.
- Behind a NAT, all clients share one address. A user who makes an error blocks the other users for some time.
- The log records the failures: `wrong credentials` with the address, and `blocked for N s`.
- The server keeps only the SHA-256 of the last accepted `Authorization` header. Thus it does not calculate PBKDF2 again for each request.

**The stall watchdog (#110).** The task `web_guard` (low priority, 2.5 KB static stack) decreases the time that a slow client holds the web server. It does not prevent it.

- Each 1 s, the task asks each server, through its control socket, to set a mark in its loop. The handlers of the firmware also set the mark at their start and at their end. The discard of the body of an authenticated rejection sets it with each piece of data.
- When a server has no mark for more than `CONFIG_GW_WEB_STALL_S` (8 s) outside a handler, the task does a `shutdown()` on the sessions of that server. It closes only the sockets whose local port is the port of that server.
- This releases the server from a slow TLS handshake, from a slowloris client, or from a client that stopped in a request.
- The log gives `… server held for more than 8 s by a request that does not arrive`.

Limits of the watchdog:

- It also closes the idle sessions of the same server. The browser connects again.
- An attacker that opens one slow connection after another can continue to hold the web server, 8 s at a time.
- There is no limit of connections for each address. The block is on the task, not on the sessions.
- A lower `recv_wait_timeout` does not help, because the time limit applies to each read, not to each request.

**Bodies of rejected requests (#106).** For the rules, refer to [The REST API](rest-api.md#writes-and-csrf). An authenticated client from the same origin can keep the web busy for the time of its upload. Each read of the discard waits up to `recv_wait_timeout` (5 s) when the client stops. When lwIP closes a socket with unread data, it sends a reset. Thus a client that continues to send can see a closed connection, not the response.

## The NVS migration in the QEMU build

> **Caution: Do not add work that depends on the NVS encryption or on its migration.** Both are only in the secure QEMU build (`sdkconfig.qemu-secure`, decision of #133), until the work on #255 starts.

The secure QEMU build encrypts the NVS. Its first encrypted boot moves all entries of an unencrypted NVS to the encrypted NVS (#67, #94). The decision logic is pure C with host tests (`gw_nvsraw`, `gw_nvsmig_decide`, `gw_nvsmig_may_erase`).

The sequence:

1. The firmware reads the keys of the encrypted NVS. At the first encrypted boot, it makes them (this burns KEY5). If this step fails, it changes nothing, and that boot continues with the unencrypted NVS.
2. It copies all entries to `/cfg/.nvsmig` in LittleFS (a temporary file, `fsync` and `rename`). It erases the NVS only when all entries are in the copy.
3. It writes the mark `gw/_gw_erase_mark` into the unencrypted NVS. It erases the partition, and erases the page of the mark last.
4. It initializes the encrypted NVS and writes the entries. It reads them again and compares them. Then it writes the mark `gw/nvsmig_ok` with the CRC of the copy.
5. It deletes the copy, then the mark.

The safety rules:

- A power cut in step 3 or 4 leaves the copy. The next boot continues from the copy (`NVS migration resumed: N entries moved into the encrypted NVS`).
- The firmware deletes a copy only when it read the full copy and the copy fails the check. It keeps a copy that it cannot read, or a copy in a format that it does not know.
- **When the boot cannot know the contents of the partition, it runs without NVS.** It also runs without NVS when a copy waits that it will not apply. Then there are no saved secrets, and an MQTT password does not connect. The shell gives `secrets  NVS UNAVAILABLE this boot: <reason>`.
- The firmware never initializes in clear text a partition that can be encrypted. It never runs on a partition that is half erased.
- With `NO_FREE_PAGES` or `NEW_VERSION_FOUND`, IDF erases the partition in all cases. Only then does the migration continue with the available data.
- A partition with pages in clear text and encrypted pages (`MIXED`) counts as unencrypted. Only a build without encryption writes entries in clear text, thus they are the newest. The remaining encrypted entries are lost.
- A page counts as clear text when its entries with a valid CRC are not fewer than its bad entries (`plain_ok >= bad`). A damaged string or blob that still reads as one item counts as one bad item (#107).
- An image on trial and safe mode do not migrate (#144). After the confirmation of the image, the gateway restarts one time, and that boot migrates.
- The gateway rejects by OTA an image that does not declare an encrypted NVS, also with force (#102). Refer to [Firmware updates](firmware-updates.md#images-that-would-lose-the-encrypted-nvs).

When LittleFS does not mount after a power cut, and the NVS looks like an interrupted migration, that boot has no NVS (#104, #109). The gateway does not format LittleFS then, because the only copy can be there. It rejects a configuration save. These are the ways out:

- A boot in which LittleFS mounts again. The migration completes from the copy.
- An intentional save that discards the copy: `config load discard-nvs-copy` in the shell, or `PUT /api/config?discard_nvs_copy=1`. The secrets that were only in the copy are lost. Save the document without new secrets, then load the secrets later.
- A factory reset.

Sometimes the gateway can never read the copy: damaged blocks, or a format of another firmware (for example, version 1 of a development image). Then each boot has no NVS and a new commissioning password. Do a factory reset.

Residual risks:

- The copy holds the secrets in clear text in LittleFS. Without flash encryption, the blocks of the deleted file stay until LittleFS uses them again. Thus `gw_sdkconfig_check` rejects NVS encryption without flash encryption outside the QEMU build (#103). The E2E examines that no clear text stays in the `nvs` partition, but not in `storage`.
- Without a LittleFS that mounts, the migration does not occur. A configuration save formats LittleFS.
- The opposite direction has no migration. An image without NVS encryption cannot read the encrypted NVS. If you flash such an image through USB, the NVS is lost.
- Only the host tests examine the error paths that destroy nothing, because QEMU cannot inject these errors. In QEMU, the E2E runs the power cuts and the rollback.

## What only the board can test

- The radio of the access point (NET-5). The QEMU build runs a simulated access point with the same life, key and status. For the emulator-only commands, refer to [The access point](access-point.md#shell-commands).
- The PMK as the key of the SoftAP (#187). The documentation of ESP-IDF v6.1 does not say that the SoftAP accepts a PMK of 64 hexadecimal characters. In `libnet80211.a`, `wifi_softap_set_config` calls `hexstr2bin` and `wifi_pmk_is_valid`, and `hostapd_derive_psk` copies the PMK without PBKDF2. The board test is #130 item N9.
- The mDNS responses (NET-6). In QEMU, the responder starts, but `open_eth` has no multicast filter. Nobody examined the filter of the W5500 (`add_mac_filter`) on the board.
- The claim window over the radio. The E2E runs the window over the emulated Ethernet with the emulator-only command `claimsim <s> eth`. The host test `test_claim` examines the rule with the addresses of the board.
- The captive portal with a real phone (#274). The E2E examines the DNS answers, the redirects and the guide page over the emulated Ethernet (`claimsim <s> eth`). Only a real phone shows if Android and iOS open the window, and what the button does there. The board test is #130.
- The CSRF check in a real browser. The E2E sends the requests that a browser would send.
- The entropy sources of the random values (#267). QEMU has no radio, and its RNG gives no evidence about the chip. The board tests are #126 item L18 (the random values with the radio on) and #130 (the station without a connection).
