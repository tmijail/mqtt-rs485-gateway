# Run the tests

This guide runs the tests of the firmware on a Linux computer: the host tests, the fuzz targets, clang-tidy, the Sparkplug decode check, the linters, CodeQL and the E2E tests in QEMU. For what each test examines, refer to [Tests](../reference/tests.md). On Windows, or without a local ESP-IDF, refer to [Run the E2E tests in Docker](run-e2e-in-docker.md).

A change is done when the host tests pass and the full E2E of the two variants passes.

## Before you start

- Install CMake, GCC 10 or later, binutils and Python 3. The host tests do not need ESP-IDF.
- With another compiler, add `-DGW_RULE_CHECKS=OFF` to the first `cmake` command. Then the three rule checks of the call graph do not run (refer to [The rule checks](../reference/tests.md#the-rule-checks)).
- Make sure that the computer has network access for the first build. CMake downloads Unity and cJSON.
- For the E2E tests, install ESP-IDF v6.1, `libslirp0`, `mosquitto` and `openssl` (refer to [Build the firmware](build-the-firmware.md)).

Run the commands from the root of the repository, unless a step says `idf/`.

## Run the host tests

1. Build and run the host tests:

   ```bash
   cmake -S idf/test/host -B build-host && cmake --build build-host -j && ctest --test-dir build-host --output-on-failure
   ```

2. Make sure that the last line is `100% tests passed`.

To run one suite, use `ctest --test-dir build-host -R test_poll`, or run `build-host/test_poll` directly.

### Run the host tests as the CI does

1. Build and run the host tests at `-O2`:

   ```bash
   cmake -S idf/test/host -B build-o2 -DCMAKE_C_FLAGS=-O2 && cmake --build build-o2 -j && ctest --test-dir build-o2 --output-on-failure
   ```

2. Build and run the host tests with the sanitizers:

   ```bash
   cmake -S idf/test/host -B build-asan -DGW_SANITIZE=ON && cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
   ```

If the sanitizer build crashes at start, set `sudo sysctl -w vm.mmap_rnd_bits=28`. The ASan runtime of GCC 13 needs it.

## Run the fuzz targets

The fuzz targets need clang with libFuzzer and the sanitizer runtimes (on Ubuntu 24.04: `clang-18` and `libclang-rt-18-dev`). For the list of the targets, refer to [Fuzz tests](../reference/tests.md#fuzz-tests).

1. Build the targets and their seed corpus:

   ```bash
   cmake -S idf/test/fuzz -B build-fuzz -DCMAKE_C_COMPILER=clang && cmake --build build-fuzz -j
   ```

2. Run each target for 60 s, as the CI does:

   ```bash
   idf/test/fuzz/run_fuzz.sh build-fuzz 60
   ```

3. Make sure that each line of the table says `passed`.

To run one target, give its name: `idf/test/fuzz/run_fuzz.sh build-fuzz 300 fuzz_spb`. `JOBS=n` sets the number of targets that run at the same time (default: the CPU count).

When a target finds an error, the input is in `build-fuzz/crashes/` and the report is in `build-fuzz/logs/<target>.log`. To run the target on that input again, give the file to the target: `build-fuzz/fuzz_spb build-fuzz/crashes/fuzz_spb-crash-<hash>`. Correct the error in `gw_core`, and add the input to a Unity suite as a regression test.

## Run clang-tidy

clang-tidy needs clang 18 and clang-tidy 18 (on Ubuntu 24.04: `clang-18` and `clang-tidy-18`). For the checks and the rules, refer to [clang-tidy](../reference/tests.md#clang-tidy).

1. Configure the host build with clang. The build is not necessary:

   ```bash
   cmake -S idf/test/host -B build-tidy -DCMAKE_C_COMPILER=clang -DGW_RULE_CHECKS=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
   ```

2. Run clang-tidy on `gw_core` and the host tests:

   ```bash
   run-clang-tidy -p build-tidy -j4 -quiet 'idf/components/gw_core/(src|port)/|idf/test/host/'
   ```

3. Make sure that the output has no line with `error:` and that the exit code is 0.

Correct a real error in the code. When the code can be clearer for the analyzer, change the code. For a false positive, add `/* NOLINTNEXTLINE(<check>): <reason> */` on the line before the result. The CI refuses a `NOLINT` comment without the check and the reason.

## Run the Sparkplug decode check

1. Install `grpcio-tools` and `protobuf`, for example with `idf/tools/e2e/requirements.txt`.
2. Write the payloads and decode them:

   ```bash
   mkdir -p /tmp/spb && build-host/spb_dump /tmp/spb && python3 idf/tools/spb_decode.py --check /tmp/spb
   ```

3. Make sure that the output gives 0 structural errors.
4. Run the self-test of the decode rules: `python3 idf/tools/test_spb_decode.py`.

## Run the linters and the harness self-test

1. Make a venv for the tools:

   ```bash
   python3 -m venv ~/e2e && ~/e2e/bin/pip install -r idf/tools/e2e/requirements.txt -r idf/tools/lint/requirements.txt
   ```

2. Run the self-test of the harness: `python3 idf/tools/e2e/test_harness.py`.
3. Run the linters. Each linter must give 0 results:

   ```bash
   ~/e2e/bin/ruff check idf/tools
   git ls-files -z '*.sh' | xargs -0 ~/e2e/bin/shellcheck
   ~/e2e/bin/actionlint -shellcheck ~/e2e/bin/shellcheck
   ~/e2e/bin/zizmor --offline .github/workflows
   npm ci --prefix idf/tools/lint && idf/tools/lint/node_modules/.bin/eslint --config idf/tools/lint/eslint.config.mjs idf/components/gw_web/www/
   ```

4. Run the unit test of the web UI (#226). All tests must pass:

   ```bash
   node --test idf/test/web/test_ip4norm.mjs
   ```

ESLint needs Node.js 20.19 or later. For a false positive, add `# noqa: <rule>` or `# shellcheck disable=SCnnnn` with the reason. Do not add an ESLint comment to a web page, because the comment goes into the firmware image. Change an option in `eslint.config.mjs` instead.

## Run CodeQL

The workflow `codeql.yml` sends the results of two analyses to the Security tab (#232): the host build and the firmware build. Run CodeQL on the computer to examine a change before the push. For the scope, the model pack and the current alerts, refer to [Code scanning with CodeQL](../reference/tests.md#code-scanning-with-codeql).

### Get the CodeQL bundle

1. Download the CodeQL bundle of the workflow (approximately 700 MB) into a new directory:

   ```bash
   mkdir -p ~/codeql-bundle && curl -L -o ~/codeql-bundle/bundle.tar.gz https://github.com/github/codeql-action/releases/download/codeql-bundle-v2.27.4/codeql-bundle-linux64.tar.gz
   ```

2. Extract the bundle (approximately 1.3 GB): `tar xzf ~/codeql-bundle/bundle.tar.gz -C ~/codeql-bundle`.

The version of the bundle must agree with the pinned `github/codeql-action`. When the workflow gets a new version, change the bundle in step 1. The `bundleVersion` in `src/defaults.json` of the action gives it.

### Run the host analysis

1. Make the database. CodeQL examines only the files that the command compiles, thus use a new build directory:

   ```bash
   rm -rf build-codeql && ~/codeql-bundle/codeql/codeql database create /tmp/cqdb --language=c-cpp --source-root=. --overwrite --command='sh -c "cmake -S idf/test/host -B build-codeql && cmake --build build-codeql -j"'
   ```

2. Run the query suite of the workflow:

   ```bash
   ~/codeql-bundle/codeql/codeql database analyze /tmp/cqdb codeql/cpp-queries:codeql-suites/cpp-security-extended.qls --format=sarif-latest --output=/tmp/codeql.sarif
   ```

3. Remove the results that the workflow removes: `python3 idf/tools/codeql_filter.py /tmp/codeql.sarif`.
4. Show the results:

   ```bash
   jq -r '.runs[].results[] | "\(.ruleId) \(.locations[0].physicalLocation.artifactLocation.uri):\(.locations[0].physicalLocation.region.startLine)"' /tmp/codeql.sarif
   ```

### Run the firmware analysis

1. Activate ESP-IDF:

   ```bash
   . $IDF_PATH/export.sh
   ```

2. Go to `idf/`.
3. Build the firmware in a build directory for CodeQL:

   ```bash
   idf.py -B build-codeql-fw set-target esp32s3 build
   ```

4. Change the time of the C files of the own components and of nanopb. Then ninja compiles them again:

   ```bash
   find main components -name '*.c' -print0 | xargs -0 touch
   ```

5. Make the database. CodeQL traces the second build, without ccache:

   ```bash
   CCACHE_DISABLE=1 ~/codeql-bundle/codeql/codeql database create /tmp/cqdb-fw --language=c-cpp --source-root=.. --overwrite --command='idf.py -B build-codeql-fw build'
   ```

6. Run the query suite with the model pack:

   ```bash
   ~/codeql-bundle/codeql/codeql database analyze /tmp/cqdb-fw codeql/cpp-queries:codeql-suites/cpp-security-extended.qls --additional-packs=../.github/codeql/extensions --model-packs=mqtt-rs485-gateway/esp-idf-models --format=sarif-latest --output=/tmp/codeql-fw.sarif
   ```

7. Remove the results that the workflow removes: `python3 tools/codeql_filter.py /tmp/codeql-fw.sarif`.
8. Show the results with the `jq` command of the host analysis, on `/tmp/codeql-fw.sarif`.

When you change the model pack, add `--rerun` to step 6. Without it, `database analyze` uses the earlier results of the database again.

## Run the full E2E

The full E2E is the full run of the two variants. The PSRAM variant takes approximately 22 min, and the default variant approximately 4 min on a fast computer.

1. Activate ESP-IDF:

   ```bash
   . $IDF_PATH/export.sh
   ```

2. Install QEMU:

   ```bash
   python3 $IDF_PATH/tools/idf_tools.py install qemu-xtensa
   ```

3. Make the venv of the harness, if you did not make it before:

   ```bash
   python3 -m venv ~/e2e && ~/e2e/bin/pip install -r idf/tools/e2e/requirements.txt
   ```

4. Go to `idf/`.
5. Run the PSRAM variant:

   ```bash
   E2E_PSRAM=1 E2E_PYTHON=~/e2e/bin/python tools/e2e/run_e2e.sh
   ```

6. Run the default variant:

   ```bash
   E2E_PYTHON=~/e2e/bin/python tools/e2e/run_e2e.sh
   ```

7. Make sure that the last line of each run gives all checks passed, for example `265/265 checks passed`.

Run the harness as root, or in a container with `--privileged`, to give each run its own network namespace. Without it, the harness uses the host loopback, and UDP 123 must be free.

CAUTION: Do not run the two variants at the same time. A high host load makes the real-time checks slower, and QEMU can crash in the OTA scenarios.

## Run one E2E scenario

1. Find the `--only` name of the scenario in [The E2E scenarios](../reference/e2e-scenarios.md).
2. Go to `idf/`.
3. Run the scenario in the variant of its full run, for example:

   ```bash
   E2E_PSRAM=1 E2E_PYTHON=~/e2e/bin/python tools/e2e/run_e2e.sh --only tls
   ```

`--only` accepts a list, for example `--only ota,https`. It selects the scenarios in each variant.

To measure a change of `.bss`, stack or heap, run `E2E_PSRAM=1 ... --only tls`. The #80 check gives the margins of the internal RAM. Never lower the thresholds.

## Run the CI groups on one computer

`run_ci_groups.sh` builds the images of one variant one time. Then it runs the CI groups of that variant in parallel, and makes the checks of `e2e-summary`.

1. Go to `idf/`.
2. Run the groups of the PSRAM variant:

   ```bash
   E2E_PSRAM=1 E2E_PYTHON=~/e2e/bin/python PARALLEL=2 tools/e2e/run_ci_groups.sh /tmp/gw-groups
   ```

3. Run the group of the default variant:

   ```bash
   E2E_PYTHON=~/e2e/bin/python tools/e2e/run_ci_groups.sh /tmp/gw-groups
   ```

4. Read the summary table at the end of the output.

Set `PARALLEL` to the number of CPUs divided by 2, or less. To run some groups only, add their names after the directory, for example `/tmp/gw-groups nvs-main other`.

## Run an optional scenario

1. Go to `idf/`.
2. Do the steps for the scenario:
   - **UMH Core (`umh`):** download the release binary `benthos-linux-amd64` of benthos-umh v0.16.2 into a scratch directory. Make sure that its SHA-256 is `39a876816578efbd5b36474c079dbdca5ba376ae25cc8453ba52b8e7b46d7c33`. Then run `E2E_BENTHOS_UMH=<binary> E2E_PYTHON=~/e2e/bin/python tools/e2e/run_e2e.sh --only umh`.
   - **Sparkplug TCK (`tck`):** do the steps of [Run the Sparkplug TCK against the gateway](run-the-sparkplug-tck.md).
   - **Downgrade (`downgrade`):** run `E2E_DOWNGRADE_REF=<git ref> E2E_PYTHON=~/e2e/bin/python tools/e2e/run_e2e.sh --only downgrade`. Do it again with `E2E_PSRAM=1`.

Without its tool or its variable, an optional scenario shows `[SKIP]` and gives no evidence.

## Use the other options

| Variable or option | Effect |
|---|---|
| `E2E_WORKDIR=<dir>` | The work directory of the run. The default is a new directory in `/tmp` |
| `E2E_STAGE=<dir>` | Build the images into `<dir>`, and run nothing |
| `E2E_PREBUILT=<dir>` | Run on the images of `<dir>`, with no build |
| `E2E_PLAIN=0` | Skip the build without NVS encryption. The OTA to NVS encryption shows `SKIP` |
| `E2E_POISON=1` | The build with heap poisoning. The CI job `e2e-poison` uses it with `--only ota-fw1` and `E2E_PSRAM=1`, and with `--only ota-park,ota-trial` |
| `E2E_TIMEOUT_SCALE=<n>` or `--timeout-scale <n>` | Multiply the time limits. Use 1.5 or 2 on a slow or loaded computer |
| `E2E_TCG_THREAD=auto\|single\|multi` | The TCG mode of QEMU. Keep `auto` |
| `JOBS=<n>` | The parallel jobs of the builds |
| `--web-stress <n>` | Add `n` rounds of web load to the `https` scenario |
| `--panics <n>` | Repeat the `panic` check of `main` `n` times |
| `--simulate-qemu-death <case>` | Test a retry path of the harness (refer to [The E2E test bench](../reference/e2e-harness.md#self-tests-of-the-harness)) |

## Read the results

1. Read the last line of the output: `N/N checks passed`.
2. If a check failed, find its line with `[FAIL]` in the output.
3. Open the work directory. Read `results.json`, `qemu-exits.txt` and the `uart0-*.log` file of the boot.
4. Examine `retries` in `results.json`. A retry is correct only for a known emulator fault.

If a run fails only on a slow or loaded computer, add `E2E_TIMEOUT_SCALE=1.5`. Do not change what a check asserts. For the retry rules and the result files, refer to [The E2E test bench](../reference/e2e-harness.md).
