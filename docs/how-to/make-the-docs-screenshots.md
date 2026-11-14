# Make the screenshots of the web UI for the docs

This guide makes the screenshots of the web UI in `docs/images/` again, for example after a change of the web UI. The script `idf/tools/docs_shots.py` starts the firmware in QEMU and opens its web UI in a headless Chromium (Playwright). The list of the shots, their states and their text alternatives is in the script (`SHOTS`).

## Before you start

- Do steps 1 and 2 of [Run the E2E tests in Docker](run-e2e-in-docker.md). You then have the image `gw-e2e` and the volume `gw-e2e` with a clone of the repository.
- Commit your changes. The clone contains only committed work.
- Use PowerShell for the commands below, in the root folder of the repository.

## 1. Build the image

1. Build the image `gw-docs`. It is the image `gw-e2e` with Playwright and Chromium:

   ```powershell
   docker build -t gw-docs idf/tools/docs
   ```

Do this step again when `idf/tools/docs/Dockerfile` or `idf/tools/docs/requirements.txt` changes, or after a new build of `gw-e2e`.

## 2. Update the clone

1. Update the clone in the volume to the commit that is checked out in Windows:

   ```powershell
   docker run --rm -v "${PWD}:/src:ro" -v gw-e2e:/work gw-e2e bash -c "git config --global --add safe.directory '*' && cd /work/repo && git fetch -q /src HEAD && git checkout -q --detach FETCH_HEAD && git log --oneline -1"
   ```

## 3. Make the screenshots

1. Run the script. It builds the QEMU images of the PSRAM variant, and then it makes each shot:

   ```powershell
   docker run --rm --privileged -v gw-e2e:/work -v "${PWD}\docs\images:/out" -w /work/repo/idf -e DOCS_WORKDIR=/work/docs gw-docs tools/docs/make_shots.sh --out /out
   ```

The script writes the images directly into `docs/images` in Windows. It writes an image only when its pixels changed. The last line gives the number of shots and the time.

To make only some shots, add `--only` and the names, for example `--only claim-page-phone,status-page`. To show the names and the text alternatives, add `--list`.

CAUTION: Add `-e DOCS_LOCK=/work/e2e.lock` when a different E2E run can use the same volume at the same time. Two emulators at the same time make the host slower, and QEMU can crash (`idf/README.md` §6.10).

## 4. Examine the images

1. Show the changed images:

   ```powershell
   git status docs/images
   ```

2. Open each changed image, and make sure that it shows the correct page.
3. Put a new image into its document with its text alternative from `--list`, for example `![The page 'Set the web password' on a phone: …](../images/claim-page-phone.png)`.

## Add a shot

1. Add an entry to `SHOTS` in `idf/tools/docs_shots.py`. Give the name, the state, the viewport (`PHONE` or `DESKTOP`), the page and the text alternative.
2. If the shot needs a different state of the gateway, add a function to `STATES`. For a small change of a state (for example a push of BOOT), give a function in `prep`.
3. If the page shows a value that changes at each run, add its path to `FREEZE`. Do the same for a value that only the emulator shows, and use the value of the board.
4. Do step 3 with `--only` and the name of the new shot.
5. Do step 4.

## If a problem occurs

- **The gateway did not reach a steady state.** Read the console log `uart0-<state>.log` in the work folder (`/work/docs`). The file `status-<state>.json` contains the last status.
- **Many images change, but the web UI did not change.** A new Playwright or Chromium version renders the text differently. Make all the screenshots again in one commit.
