# Flash the firmware from the browser

This page installs or repairs the firmware of the gateway through the USB cable. The tool on this page reads the board, flashes a release and shows the access data of the gateway. The computer needs no tools: use Chrome or Edge on a computer.

- To update a gateway whose web UI opens, do not use this page. Use the card **Firmware** of the web UI: refer to [Update the firmware](update-the-firmware.md). That update examines the image first, and it can go back to the earlier image.
- For the files of a release and what a flash writes, refer to [The release files and the manifest](../reference/release-manifest.md).
- For the access data and the commissioning password, refer to [Commissioning and secrets](../reference/commissioning-and-secrets.md#the-command-access).

> **Warning: Do not use this tool as verified for the board.** The firmware has never run on the board (#1, #18). Nobody flashed a board with this tool. The unit tests examine only its logic, without a browser and without a board (#219).

> **Warning: Disconnect the loads from the outputs before the flash.** The polarity of the outputs is not verified on the board. A wrong polarity can energize an output after the flash.

<div id="gw-flash" markdown="0"><noscript><p><b>The tool needs JavaScript.</b> Turn on JavaScript, or flash with esptool: refer to <a href="#flash-with-esptool">Flash with esptool</a>.</p></noscript></div>

## Before you start

You need:

- Chrome or Edge on a computer. A phone, a tablet and the other browsers have no serial port for a web page (Web Serial). The page then gives the image files only.
- The USB-C cable of the board. The cable must have data lines.
- No driver. The USB-C connector goes to the USB of the ESP32-S3, and Windows 10 or later, macOS and Linux have its driver.
- For an install or a repair: a release on this site. The tool selects the last release. You can select an earlier release.

The tool uses the port of the board only while this page is open. Close a serial monitor or another program that uses the port.

## Read the board

1. Connect the board to the computer with the USB-C cable.
2. Click **Connect the board**.
3. Select the serial port of the board in the list of the browser. Click **Connect**.
4. Wait for the panel. The page listens to the board and asks for the access data:
    - A board that does not answer takes approximately 5 s.
    - A gateway that starts can take up to 30 s.

The connection can restart a gateway that runs. The panel then shows one of these states:

| The panel shows | What it tells | Go to |
|---|---|---|
| "This board did not answer as a gateway" | A new board, a gateway whose firmware does not start, or a board in the download mode | [Install a new board](#install-a-new-board) or [Repair a gateway](#repair-a-gateway) |
| "The gateway runs. …" | The access data and the firmware of the gateway | [Open the web UI](#open-the-web-ui) |
| "The gateway has no address." | The gateway has no network address | [The gateway has no address](#the-gateway-has-no-address) |
| "The configuration turns the web UI off." | The configuration has no web UI | [The web UI is off](#the-web-ui-is-off) |
| "The gateway is in fault mode." | The gateway does not start its functions | [Recover a gateway](recover-the-gateway.md) |

## Install a new board

**Install** erases all the flash, then writes the firmware. The gateway then starts in commissioning mode, with no configuration. Use it for a new board, or to start again from zero.

> **Caution: Install erases the configuration and the passwords of a gateway.** The page tells when the board answered as a gateway. To keep the configuration, use [Repair a gateway](#repair-a-gateway).

1. Do the steps of [Read the board](#read-the-board).
2. Click **Install or repair**. For a new board, you can also click **Install now** while the page reads the board.
3. Select **Install**.
4. Keep the last release in **Version**, or select an earlier release.
5. Disconnect the loads from the outputs. Select **The loads are disconnected.**
6. Click **Install release** *version*.
7. Wait until the page reads the board again. Keep the cable connected, and keep the tab in view.
8. Make sure that the panel shows "The gateway runs. It waits for its first configuration."

Then do the steps of [Open the web UI](#open-the-web-ui).

## Repair a gateway

**Repair** writes only the firmware. The configuration and the passwords stay. Use it only for a gateway whose web UI does not open in any mode. Refer to [The web UI is not available in any mode](recover-the-gateway.md#the-web-ui-is-not-available-in-any-mode).

A flash over USB has no pre-flight check, no trial and no rollback. An earlier release can be unable to read the stored configuration. Thus use the last release for a repair. The page gives a warning when you select an earlier release.

1. Do the steps of [Read the board](#read-the-board).
2. Click **Install or repair**. If the panel shows the gateway, click **The web UI does not open in any mode: repair with a USB flash**.
3. Select **Repair**.
4. Keep the last release in **Version**.
5. Disconnect the loads from the outputs. Select **The loads are disconnected.**
6. Click **Repair with release** *version*.
7. Wait until the page reads the board again. Keep the cable connected, and keep the tab in view.
8. Make sure that the panel shows the gateway and the firmware of the release.

## Open the web UI

The box **Access data** gives the address, the user, the password and the fingerprint of the certificate. The page reads them with the shell command `access` of the gateway.

1. Click **Open the web UI**. The web UI opens in a new tab.
2. The browser gives a warning about the certificate. Open the details of the certificate.
3. Compare its SHA-256 fingerprint with the box **Access data**. Then accept the certificate.
4. Log in with the user and the password of the box **Access data**.

    When the box shows "A web password is set", log in with your web password.

5. For a new gateway: in the tab **Configuration**, set the configuration. In the card **Web interface**, set a web password. Click **Validate**, then click **Save and restart**.

The page reads the board again after the restart, with no click. The commissioning password is not valid after you set a web password. For the full first configuration, refer to [The first gateway on the bench](../tutorials/first-gateway-on-the-bench.md).

## Reset the web password

When nobody knows the web password, the page can remove it. The configuration and the other secrets stay.

1. Do the steps of [Read the board](#read-the-board).
2. In the panel, click **Reset the web password**. The console tab has the same button.
3. Click **Remove the web password**. The gateway restarts.
4. Make sure that the panel shows the commissioning password. Log in with it.
5. Set a new web password in the web UI.

Until you set the new web password, the commissioning password is also the Wi-Fi password of the access point. The gateway refuses the command during the trial of a new firmware. For the other ways, refer to [Nobody knows the web password](recover-the-gateway.md#nobody-knows-the-web-password).

## The gateway has no address

1. Connect the Ethernet cable. Make sure that the network has a DHCP server.
2. Wait. The page reads the board again each 5 s.

If no network is available, use the access point: refer to [Get on the access point on a later visit](get-on-the-access-point.md).

## The web UI is off

1. Push BOOT until you hear 1 beep (at 2 s), then release it. The gateway opens the access point and its web UI.
2. Click **Read again**.

> **Caution: Release BOOT before 10 s.** At 10 s, the gateway gives 2 beeps. A release after the 2 beeps does a factory reset.

For the other ways, refer to [The web interface is off](recover-the-gateway.md#the-web-interface-is-off).

## The list has no serial port

1. Use a USB cable with data lines. A cable for power only does not work.
2. On Windows, open the Device Manager. Make sure that **Ports (COM & LPT)** shows a **USB Serial Device** when the cable is connected.
3. Connect the cable again. Click **Connect the board** again.

On Linux, your user must have the permission for the serial port, for example the group `dialout`.

## The flash does not start

The page shows "The flash did not start" when the board does not go into its download mode. Nothing is erased. Put the board in the download mode by hand.

> **Caution: Do not hold BOOT while the gateway has power.** A push of 2 to 10 s opens the access point. A push of more than 10 s does a factory reset at the release.

1. Remove all the power: the USB cable, PoE and the terminal block.
2. Hold the BOOT button.
3. Connect the USB cable. Then release BOOT.
4. Click **Try the flash again**.

When the page shows "The flash stopped", the flash of the board changed. Keep the cable connected, and click **Try the flash again**. The gateway possibly does not start until a flash is complete.

## Use the console

The tab **Console** shows the log of the gateway and its shell. Type a command, then click **Send**. For the commands, type `help`. The tab **Files** gives the image files of the selected release.

The page and the console use the same port. After a restart of the gateway, the page reads the board again.

## Flash with esptool

Without Chrome or Edge, flash with esptool from a computer. The tab **Files** gives the files and the command for the selected release.

1. Download the four image files, `manifest.json` and `SHA256SUMS` of the release.
2. Examine the files: `sha256sum -c SHA256SUMS`.
3. For a new board, erase the flash: `python -m esptool --chip esp32s3 erase-flash`. The erase removes the configuration and the passwords.
4. Write the files with the command of the tab **Files**. Refer also to [What a flash of a release writes and keeps](../reference/release-manifest.md#what-a-flash-of-a-release-writes-and-keeps).

To build the firmware yourself, refer to [Build the firmware](build-the-firmware.md).
