# Update the firmware

This procedure tells how to install a new firmware image on a gateway through the network, how to make sure that the update is complete, and how to go back to the earlier image.

- For the full process, the trial and the rollback, refer to [`idf/README.md` §3.6](../../idf/README.md#36-firmware-updates-fw-1).
- The owner of the board can always flash it over USB as well. The project burns no eFuses (#133).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The E2E tests the updates, the trial and the rollback in QEMU. The images come from the firmware in QEMU.

## Before you start

You need:

- The new image `rs485_spb_gateway.bin`. Build it with `idf.py build` in `idf/`. The file is in `idf/build/`.
- Access to the web UI, with the web password.
- For a download: a web server with HTTPS that the gateway can reach. The gateway needs a valid clock to examine the certificate of the server.

> **Caution: Update one pilot gateway first.** An image that passes its self-check but cannot connect to the broker of a site stays installed. Then a person must go back to the earlier image in the web UI.

## How an update operates

1. The gateway writes the new image into the other slot of the flash. The running image continues to operate.
2. Before it writes past the header, the gateway does a pre-flight check. It refuses an image of another project, or an image that cannot read the stored configuration, the NVS or the LittleFS partition.
3. At the end, the gateway verifies the image (checksum and SHA-256), sends the NDEATH, and restarts into the new image.
4. The new image runs on trial. It must operate for 60 s in the expected mode. Then it confirms itself.
5. If the trial fails, or if a reset occurs during the trial, the gateway goes back to the earlier image.

During the trial (approximately 60 s), the gateway refuses a configuration save and a new update (HTTP 409).

## Install an image from the computer

1. Open the web UI. Stay on the tab **Status**.
2. In the card **Firmware**, click **Upload an image**, and select `rs485_spb_gateway.bin`.
3. Accept the question "Install … and restart?".
4. Wait for the message "Image installed: restarting into the new version."
5. Wait approximately 30 s. Then load the page again.
6. Do the steps of "Make sure that the update is complete".

   ![The card 'Firmware' of the Status tab: the running version, its slot, the state of the last update, and the other slot. Below are the field for the URL of an image with the button 'Download and install', the field 'Upload an image', and the check box 'Force (into recovery mode)'.](../images/firmware-card.png)

## Install an image from a web server

1. Put the image on the web server.
2. In the card **Firmware**, type the URL of the image (`https://…`, 255 characters at most).
3. Click **Download and install**.
4. Read the progress in the card. At the end, the gateway restarts.
5. Do the steps of "Make sure that the update is complete".

The gateway examines the certificate of the server with `ota.ca_file`. If that key is not set, it uses `mqtt.tls.ca_file`, then the public CA bundle of ESP-IDF.

A Sparkplug host can start the same download. It sends the NCMD `Node Control/Firmware Update` with the URL as a String. The host reads the result in `Firmware Update/State` and `Firmware Update/Progress`.

## Make sure that the update is complete

1. Wait 60 s after the restart.
2. In the card **Firmware**, make sure that the state is `installed <version>`.
3. Make sure that the card **Network and MQTT** shows the broker as connected.
4. Make sure that the card **Modbus devices** shows each device online.

| State | Meaning |
|---|---|
| `downloading <version>` | The transfer is in progress |
| `restarting into <version>` | The gateway restarts into the new image |
| `testing <version>` | The new image is on trial. Wait |
| `installed <version>` | The new image confirmed itself. The update is complete |
| `failed: <cause>` | The update did not occur. The earlier image runs. Read the cause |
| `rolled back from <version>: <cause>` | The new image failed its trial. The earlier image runs again |

## Go back to the earlier image

Do this when the new image operates but does not do what you need, for example when it cannot connect to the broker of the site.

1. In the card **Firmware**, click **Restart into `<version>`**. The button shows the version in the other slot.
2. Accept the question.
3. Wait for the restart and the trial (60 s).
4. Make sure that the card shows the earlier version.

You can also upload the earlier image. The earlier image possibly cannot read a configuration that you saved after the update. Then the gateway restores the configuration from before the update. It does this only if the configuration did not change after the first save with the new image. Else it refuses the image and gives the changed paths.

## If the gateway refuses the image

| Message | What to do |
|---|---|
| `the configuration needs schema N, the image reads M: <paths>` | The image is older than the stored configuration. Remove the keys at these paths, or use force (below) |
| NVS format or LittleFS version lower | The image cannot read the stored data. Use a newer image |
| The image is "unknown" | The image is from before the pre-flight check. Use force only if you accept its risks |
| `testing <version>: N s left` | A trial is in progress. Wait, then try again |

**Force.** Select **Force (into recovery mode)**, then install the image again. The gateway then installs the image even if the pre-flight check refuses it. The new image must only reach recovery mode. In recovery mode, correct the configuration in the web UI.

> **Caution: Do not force an image with a lower NVS format or LittleFS version, unless you accept the loss of the data.** Such an image cannot read the NVS or `/cfg`, and it can erase them. Then you lose the secrets and the configuration.

## If the web UI is not available

Flash the image over USB: refer to [`idf/README.md` §3.2](../../idf/README.md#32-firmware). For the other paths, refer to [Recover a gateway](recover-the-gateway.md).
