# The E2E environment on a Windows computer

This document tells why the project runs the E2E tests on Windows in a Docker container. To run the tests, refer to [Run the E2E tests in Docker](../how-to/run-e2e-in-docker.md).

## What the E2E tests need

The E2E harness (`idf/tools/e2e/e2e.py`) runs the real firmware in the Espressif QEMU. It needs a Linux host with these items:

- ESP-IDF v6.1 and its toolchain, to build the QEMU images.
- `qemu-system-xtensa` from `idf_tools.py`, and `libslirp0`.
- `mosquitto`, `openssl` and a Python venv with `idf/tools/e2e/requirements.txt`.
- Optional: `CAP_SYS_ADMIN`, to put each run in its own network namespace.

None of these items run on Windows directly. The harness uses Linux network namespaces, POSIX signals and Unix shell scripts.

## Why Docker and not a direct install in WSL

Both choices run on the same WSL 2 virtual machine. Docker Desktop on Windows uses a WSL 2 VM for its Linux engine. Thus the speed of the CPU is the same. The project selected Docker for these reasons:

- **The same environment as the CI.** The CI runs every E2E job in `espressif/idf:v6.1`, pinned by digest. `idf/tools/e2e/Dockerfile` starts from the same digest and adds the same packages that the CI jobs install. A local result and a CI result thus come from the same tool versions.
- **No ESP-IDF install.** The image contains ESP-IDF v6.1 and its tools. A direct install in WSL needs a clone of ESP-IDF, `install.sh` and the QEMU tools. Its versions of `mosquitto` and Python then come from the WSL distribution, not from the CI image.
- **Network isolation.** With `--privileged`, the harness can make its own network namespace. Then each run has private ports, also UDP 123, and parallel runs do not collide.
- **Easy removal.** `docker volume rm gw-e2e` and `docker image rm gw-e2e` remove all of it.

A direct install in WSL is also possible: follow `idf/README.md` §3.1 and §4.3 in the WSL distribution. The project did not measure that choice.

## Why the repository goes into a Docker volume

Do not build from the Windows file system. A bind mount of `C:\` into a Linux container goes through a file-sharing layer. An ESP-IDF build reads and writes many thousands of small files, and that layer makes each access slow. A named volume is on the ext4 disk of the VM, so the build runs at the speed of a Linux host.

For this reason, the procedure clones the repository from the Windows folder into the volume. A clone also keeps `.git`, which the build needs: the firmware version contains the git hash.

The clone contains only committed work. To test changes that are not committed, commit them on a branch first, or copy the files into the volume.

## Why `--privileged`

The harness calls `unshare(CLONE_NEWNET)`. A default container does not have the permission for this call, and the harness then uses the loopback of the container. That is correct for one run at a time. But `run_ci_groups.sh` runs groups in parallel, and those runs need private ports. `--privileged` gives the container `CAP_SYS_ADMIN` in the Docker VM only. It gives no permission on Windows.
