# Run the Sparkplug TCK against the gateway

This guide runs the Edge Node profile of the Eclipse Sparkplug TCK 3.0.0 against the real firmware in QEMU. The E2E harness runs the TCK as the optional scenario `tck`. For the last results and the reason for each failure, refer to `idf/README.md` §4.6. For the E2E environment in Docker, refer to [Run the E2E tests in Docker](run-e2e-in-docker.md).

The TCK is a HiveMQ extension. It examines each MQTT packet of the broker, it simulates the Sparkplug host application, and it gives one result for each `tck-id`. The scenario does the steps of the operator that the TCK web console asks for. It boots the gateway, changes a Modbus register, sets a DI, stops the Modbus slave and restarts the gateway.

## Before you start

- Do the steps of [Run the E2E tests in Docker](run-e2e-in-docker.md) until the clone is in the volume `gw-e2e`. On Linux with a local ESP-IDF, you can use the same commands without Docker.
- Make sure that the computer can download from `download.eclipse.org` and `github.com`. The setup downloads approximately 330 MB.
- Make sure that port 1883 is free in the network of the harness. The TCK extension connects to `tcp://localhost:1883`, and you cannot change this address. The harness runs in its own network namespace when it can, so the port is free there.

## 1. Install the TCK

1. Run the setup script. It downloads a Java 21 JDK, HiveMQ CE 2025.5 and the TCK 3.0.0, and it checks their SHA-256:

   ```powershell
   docker run --rm -v gw-e2e:/work -w /work/repo/idf gw-e2e tools/e2e/tck_setup.sh /work/tck
   ```

2. Make sure that the last line is `TCK ready in /work/tck`.

The script installs nothing outside `/work/tck`. It puts the TCK extension into `/work/tck/hivemq/extensions/sparkplug-tck`.

## 2. Run the TCK

1. Run the scenario with `E2E_TCK_DIR`:

   ```powershell
   docker run --rm --privileged -v gw-e2e:/work -w /work/repo/idf -e E2E_WORKDIR=/work/run-tck -e E2E_TCK_DIR=/work/tck gw-e2e tools/e2e/run_e2e.sh --only tck
   ```

2. Wait for the last line, for example `17/19 checks passed`. There is one check for each test and MQTT version, and the five checks that the harness makes after each run.

The scenario runs the seven Edge Node tests two times: one time with MQTT 3.1.1 and one time with MQTT 5. Each test has a new HiveMQ and a new boot of the gateway. On the test computer, the 14 tests took approximately 4.5 minutes, plus the build of the QEMU image.

CAUTION: Do not run the scenario without `E2E_TCK_DIR`. The scenario then shows `[SKIP]` and runs no test. This is correct for a computer without the TCK, but it gives no evidence.

To use images that you built before, add `-e E2E_PREBUILT=<dir>` and `-e E2E_PLAIN=0`. To run some of the tests, add these variables:

| Variable | Value | Default |
|---|---|---|
| `E2E_TCK_DIR` | The directory of step 1 | Not set: `[SKIP]` |
| `E2E_TCK_TESTS` | Test names, with commas between them, for example `SessionEstablishmentTest,PrimaryHostTest` | All seven tests |
| `E2E_TCK_PROTOCOLS` | `3.1.1`, `5`, or `3.1.1,5` | `3.1.1,5` |

## 3. Read the results

1. Find the check of each test in the output:

   ```text
   [PASS] Issue #176 TCK edge SendDataTest (MQTT 5): ... 41 PASS, 0 FAIL (0 known), 3 NOT EXECUTED
   ```

2. Find the result of each assertion in the lines that start with `[TCK]`. A failure has the text of the specification after `--`.
3. Open the work directory (`/work/run-tck` in the example) for more data:
   - `tck-results.json`: one record for each assertion of each test, with its status and its text.
   - `tck-<test>-mqtt<version>.result.txt`: the summary that the TCK published on `SPARKPLUG_TCK/RESULT`.
   - `tck-<test>-mqtt<version>/hivemq.out`: the HiveMQ log, with the steps of the TCK test.
   - `uart0-<test>-mqtt<version>.log`: the console log of the gateway.

A check fails when an assertion is FAIL and the harness cannot explain it. The harness explains a FAIL only when it can prove a known cause from the messages that it captured (`KNOWN_RULES` in `tools/e2e/tck.py`). Such a FAIL shows `(known: <reason>)`, and it does not make the check fail.

A NOT EXECUTED result is not a failure. It applies to an assertion that the test cannot examine, for example the MQTT 5 assertion in the MQTT 3.1.1 run, or the template parameters that the gateway does not use.

## 4. Make a new TCK installation

Do this procedure when you want a different TCK or HiveMQ version.

1. Change the URL and the SHA-256 in `tools/e2e/tck_setup.sh`.
2. Delete the directory of step 1.
3. Do step 1 and step 2 again.
4. Compare the results with `idf/README.md` §4.6.
