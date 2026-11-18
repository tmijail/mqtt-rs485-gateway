# Get and decode a core dump

This procedure tells how to get the core dump of a gateway after a crash, and how to decode it. You can do it through the network or at the console, without a USB cable. The reasons for the design are in [The core dump](../explanation/core-dump.md).

> **Warning: Keep the dump file as safe as the passwords of the gateway.** The dump is a copy of the RAM at the crash. It can contain the MQTT password, TLS session keys and the private key of the device certificate, in clear text.

> **Warning: Do not use this document as verified data for the board.** The procedure ran only with the firmware in QEMU (issue #132). Nobody tested it on the board (#18).

## Before you start

You need:

- The ELF file of the same build as the firmware that crashed, for example `idf/build/rs485_spb_gateway.elf`. Without the ELF of that build, the decoder cannot read the dump. Keep the ELF of each firmware that you install.
- ESP-IDF v6.1 (`. $IDF_PATH/export.sh`) or the Docker image of `docs/how-to/run-e2e-in-docker.md`. Both have `esp-coredump`.
- The web password of the gateway, or a computer with a USB-C cable for the console.

## Find out if a dump is present

A panic writes the dump to the `coredump` partition, and the gateway restarts. The dump stays until you erase it. Each of these shows a dump:

- The NBIRTH metric `Node Info/Core Dump` is `true`. `Node Info/Reset Reason` gives the cause of the last restart, for example `panic`.
- The Status tab of the web UI shows the line "Core dump in flash".
- `GET /api/status` has `"core_dump": {"present": true, …}`.
- The shell command `status` shows a `coredump` line.

## Read the summary

1. Send this request (use the IP address or the host name of your gateway):

   ```bash
   curl -u admin:PASSWORD --cacert gateway.pem https://GATEWAY/api/coredump/summary
   ```

2. Read the task, the PC, the backtrace and the panic reason in the answer.

At the console, the shell command `coredump` gives the same summary.

The certificate of the gateway is self-signed. Use `--cacert` with the certificate of the gateway. Use `-k` only in a network that you trust.

## Download the dump through the network

1. Send this request:

   ```bash
   curl -u admin:PASSWORD --cacert gateway.pem -o coredump.bin https://GATEWAY/api/coredump
   ```

2. Make sure that `curl` ended without an error. An incomplete transfer means that the download stopped.

You can also use the **Download** button in the Status tab of the web UI.

## Copy the dump from the console

Use this method when the gateway has no network.

1. Connect the computer to the USB-C port of the gateway.
2. Open a terminal program that saves the received text to a file, for example `idf.py monitor` or `picocom --logfile console.log`.
3. Type `coredump dump`.
4. Wait for the line `-----END CORE DUMP-----`.
5. Keep the lines between `-----BEGIN CORE DUMP-----` and `-----END CORE DUMP-----` that have only base64 characters:

   ```bash
   tr -d '\r' < console.log \
     | sed -n '/-----BEGIN CORE DUMP-----/,/-----END CORE DUMP-----/p' \
     | grep -E '^[A-Za-z0-9+/=]+$' > coredump.b64
   ```

The log lines of other tasks can come between the base64 lines. They have spaces, so the `grep` command removes them.

## Decode the dump

1. Make sure that the ELF is from the build that crashed:

   ```bash
   sha256sum idf/build/rs485_spb_gateway.elf
   ```

2. Compare the start of the result with `app_elf_sha256` in the summary. They must be the same.
3. Decode a downloaded file (`coredump.bin`):

   ```bash
   esp-coredump --chip esp32s3 info_corefile --core coredump.bin --core-format raw idf/build/rs485_spb_gateway.elf
   ```

4. Or decode the base64 lines of the console (`coredump.b64`):

   ```bash
   esp-coredump --chip esp32s3 info_corefile --core coredump.b64 --core-format b64 idf/build/rs485_spb_gateway.elf
   ```

5. Read the crashed task, its backtrace with function names and lines, and the other tasks in the output.

For a GDB session on the dump, use `dbg_corefile` in place of `info_corefile`.

## Erase the dump

Erase the dump after you saved it, or when you do not need it. Then the gateway can record the next crash.

1. Send this request:

   ```bash
   curl -u admin:PASSWORD --cacert gateway.pem -X DELETE -H 'X-Requested-With: curl' https://GATEWAY/api/coredump
   ```

2. Make sure that the answer is `{"ok":true,"erased":true}`.

You can also use the shell command `coredump erase`, or the web UI:

1. In the card **Gateway** of the Status tab, click **Erase**.
2. In the confirmation "Erase the core dump?", click the red button **Erase the core dump**. To keep the dump, click **Cancel**.

After the erase, the gateway publishes an NDATA with `Node Info/Core Dump` = `false`.

## Keep the file safe

1. Keep the dump file only on computers that only authorized persons can use.
2. Do not attach the dump to a public issue or forum.
3. Delete the file when the analysis ends.
4. Change the MQTT password and the web password if the file went to a person without authorization.
