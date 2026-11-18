# Run the E2E tests in Docker (Windows)

This guide runs the full QEMU E2E tests on a Windows computer, in the same ESP-IDF image as the CI. It also works on Linux and macOS with Docker. For the reasons of this procedure, refer to [The E2E environment on a Windows computer](../explanation/e2e-environment.md). For the measured durations, refer to [E2E durations](../reference/e2e-durations.md).

## Before you start

- Install Docker Desktop with the WSL 2 engine.
- Give the Docker VM sufficient CPUs. `PARALLEL` groups need at least 2 × `PARALLEL` CPUs (`idf/README.md` §4.5). On WSL 2, the VM gets all the CPUs and up to half of the memory by default. The default was sufficient on the test computer (24 threads, 32 GB).
- Make sure that approximately 20 GB of disk are free. The image is 8.6 GB, and the volume grows to approximately 3 GB for each variant that you run.
- Use PowerShell for the commands below.

CAUTION: Do not run the commands from Git Bash without `MSYS_NO_PATHCONV=1`. Git Bash changes Linux paths such as `/work/repo` into Windows paths, and the commands then fail.

## 1. Build the image

1. Start Docker Desktop.
2. Open PowerShell in the root folder of the repository.
3. Build the image:

   ```powershell
   docker build -t gw-e2e idf/tools/e2e
   ```

The first build downloads `espressif/idf:v6.1` (8.5 GB when unpacked; 7 minutes on the test computer). Do this step again when `idf/tools/e2e/Dockerfile` or `requirements.txt` changes.

## 2. Put the repository into a Docker volume

1. Make a volume for the work:

   ```powershell
   docker volume create gw-e2e
   ```

2. Clone the repository from the Windows folder into the volume:

   ```powershell
   docker run --rm -v "${PWD}:/src:ro" -v gw-e2e:/work gw-e2e bash -c "git config --global --add safe.directory '*' && git clone -q /src /work/repo"
   ```

The clone contains only committed work. Commit your changes before you clone or update.

To test a different commit later, update the clone to the commit that is checked out in Windows:

```powershell
docker run --rm -v "${PWD}:/src:ro" -v gw-e2e:/work gw-e2e bash -c "git config --global --add safe.directory '*' && cd /work/repo && git fetch -q /src HEAD && git checkout -q --detach FETCH_HEAD && git log --oneline -1"
```

## 3. Run the full E2E tests

Each command builds the QEMU images of one variant in the volume and runs the scenarios of that variant. The full E2E is the two variants (issue #135). The container activates ESP-IDF when it starts.

1. Run the octal PSRAM variant (the configuration of the board, all scenarios but three):

   ```powershell
   docker run --rm --privileged -v gw-e2e:/work -w /work/repo/idf -e E2E_WORKDIR=/work/run-psram -e E2E_PSRAM=1 gw-e2e tools/e2e/run_e2e.sh
   ```

2. Run the default variant (no PSRAM, only the three OTA trial scenarios):

   ```powershell
   docker run --rm --privileged -v gw-e2e:/work -w /work/repo/idf -e E2E_WORKDIR=/work/run-default gw-e2e tools/e2e/run_e2e.sh
   ```

The last line of the output gives the result, for example `181/181 checks passed`. To run only some scenarios, add `--only` at the end, for example `tools/e2e/run_e2e.sh --only ota,https`. `--only` selects the scenarios in each variant.

## 4. Run the CI groups in parallel (faster)

`run_ci_groups.sh` builds the images of one variant one time. Then it runs the CI groups of that variant at the same time, each group in its own network namespace. The PSRAM variant has five groups, and the default variant has one group (`CI_GROUPS` in `idf/tools/e2e/scenarios.py`).

1. Run the groups of the PSRAM variant. Set `PARALLEL` to 5 only when the Docker VM has 10 CPUs or more. Otherwise, set it to the number of CPUs divided by 2.

   ```powershell
   docker run --rm --privileged -v gw-e2e:/work -w /work/repo/idf -e PARALLEL=5 -e E2E_PSRAM=1 gw-e2e tools/e2e/run_ci_groups.sh /work/groups
   ```

2. Run the group of the default variant:

   ```powershell
   docker run --rm --privileged -v gw-e2e:/work -w /work/repo/idf gw-e2e tools/e2e/run_ci_groups.sh /work/groups
   ```

The output ends with the summary table of the CI. A change is done only when both variants pass (refer to `CLAUDE.md`).

CAUTION: Do not run more than one variant at the same time. A host with high load makes the real-time checks slower, and QEMU can crash in the OTA scenarios (`idf/README.md` §6.10).

## 5. Copy the results to Windows

The logs, `results.json` and `qemu-exits.txt` stay in the volume. To examine them in Windows, copy them to a folder outside the repository:

```powershell
docker run --rm -v gw-e2e:/work -v "$env:USERPROFILE\gw-e2e-out:/out" gw-e2e bash -c "cd /work && cp --parents run-default/*.log run-default/results.json run-default/qemu-exits.txt /out/"
```

These files are approximately 1.5 MB for each run. A full work directory is approximately 800 MB, because it also contains the flash images.

## Remove everything

1. Remove the volume:

   ```powershell
   docker volume rm gw-e2e
   ```

2. Remove the image:

   ```powershell
   docker image rm gw-e2e
   ```

## Troubleshooting

- **`network: host loopback` at the start of the run.** The container has no `--privileged`. One run at a time is correct in that mode. Parallel runs need `--privileged`.
- **No `core` file in `qemu/`.** In the Docker Desktop VM, `core_pattern` sends core dumps to a WSL handler. The harness says so in its first line, and it continues without the QEMU trace.
- **Windows has low memory during a run.** The Docker VM keeps its memory while it runs. Close other large programs, or set a lower limit in `%USERPROFILE%\.wslconfig` (`[wsl2]`, `memory=12GB`). Then restart WSL with `wsl --shutdown`.
- **A run fails only on a slow or loaded computer.** Add `-e E2E_TIMEOUT_SCALE=2` to the `docker run` command. Do not change the checks.
