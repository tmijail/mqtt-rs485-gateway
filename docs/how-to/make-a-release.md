# Make a release

This procedure tells the maintainer how to make a release of the firmware. A release is a GitHub pre-release with the images of the build for the board, `manifest.json` and `SHA256SUMS`. A tag starts the release workflow, and the workflow makes the release.

For the files, the format of the manifest and the checks, refer to [The release files and the manifest](../reference/release-manifest.md).

> **Warning: Do not make the tag `v1.0`.** That tag is for the commit of the first installed gateway, and only the product owner makes it. A release has a tag `v0.<minor>.<patch>`.

> **Warning: Do not use a release as verified firmware for the board.** The firmware has never run on the board (#1, #18).

The install tool in the browser and the copy of the images to the documentation site are not made yet (#219). Until then, the release page of GitHub is the only place of the images.

## Before you start

You need:

- The permission to push a tag to the repository.
- A commit on `main` whose CI passed. The job `firmware` makes the manifest of each push, so a problem of the build shows there first.
- A version that no release has. The version has the form `0.<minor>.<patch>`, for example `0.2.0`.

## 1. Set the version

1. Open `idf/CMakeLists.txt`.
2. Find the line `set(GW_VERSION "...")`.
3. If the line has the version of the new release, go to [2. Do a dry run](#2-do-a-dry-run).
4. Set the new version, for example `set(GW_VERSION "0.2.0")`.
5. Commit the change, and merge it into `main`.
6. Make sure that the CI of the merge commit passes.

The tag of the release is `v` and this version, for example `v0.2.0`.

## 2. Do a dry run

A dry run makes the build and the manifest of a release. It makes no release and no tag.

1. On GitHub, open **Actions**, then the workflow **Release**.
2. Click **Run workflow**, select the branch `main`, and click **Run workflow**.
3. Wait for the end of the job **Build the release images and the manifest**.
4. Read the summary of the job. Make sure that the version is the version of step 1, without a hash.
5. Optional: download the artifact `firmware-release`, and examine the files with `sha256sum -c SHA256SUMS`.

You can also start the dry run from a terminal:

```bash
gh workflow run release.yml --ref main
```

If the job fails, refer to [If the workflow fails](#if-the-workflow-fails).

## 3. Make the tag

1. Get the commit of the release:

    ```bash
    git checkout main && git pull
    ```

2. Make the tag. Use the version of step 1:

    ```bash
    git tag -a v0.2.0 -m "v0.2.0"
    ```

3. Push the tag:

    ```bash
    git push origin v0.2.0
    ```

The push starts the workflow **Release**.

## 4. Examine the release

1. On GitHub, open **Actions**, then the workflow **Release**.
2. Wait for the end of the two jobs.
3. Open the page **Releases** of the repository.
4. Make sure that the pre-release of the tag has these six files: `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin`, `rs485_spb_gateway.bin`, `manifest.json` and `SHA256SUMS`.
5. Optional: download the six files into one folder, and run `sha256sum -c SHA256SUMS` there.
6. Optional: change the text of the release on GitHub. The workflow writes a short text and the list of the changes.

## What the workflow does

1. It makes sure that the tag is `v` and `GW_VERSION`.
2. It builds the firmware for the board with `GW_RELEASE=ON`, in the ESP-IDF image of the CI. The version of this build is `GW_VERSION`, without the git hash.
3. It runs `idf/tools/release/make_manifest.py`. The script examines the build, and writes the four images, `manifest.json` and `SHA256SUMS`.
4. It makes a GitHub pre-release of the tag with the six files.

The manifest has no part for the configuration and the secrets. Refer to [What a flash of a release writes and keeps](../reference/release-manifest.md#what-a-flash-of-a-release-writes-and-keeps).

## If the workflow fails

Find the error line in the log of the job. Then find its cause in this table:

| Error | Cause | Correction |
|---|---|---|
| The workflow does not start | The tag does not have the form `v0.<minor>.<patch>`, for example `v0.2` or `v0.2.0-rc1` | Delete the tag, and make a tag of the correct form |
| `the tag v0.2.0 does not agree with the version 0.1.0 of GW_VERSION` | The commit of the tag has another `GW_VERSION` | Delete the tag. Do [1. Set the version](#1-set-the-version), then make the tag on the new commit |
| `the tag ... is not a release tag` | In a dry run: `GW_VERSION` does not have the form `0.<minor>.<patch>` | Correct `GW_VERSION` |
| `GW_RELEASE=ON: the work tree has changes that are not in a commit` | The build changed a file of `idf/` that git follows, for example `dependencies.lock` | Do the build on your computer, commit the changed file, and make a new tag |
| `the firmware build produced warnings` | The build has a warning | Correct the warning on `main`. The CI job `firmware` shows the same warning |
| `sdkconfig has CONFIG_...=y` | The build is not the build for the board: an option of a QEMU build or an option that burns an eFuse is on | Remove the option from `sdkconfig.defaults` |
| An error about a part, an offset or the partition table | The build does not agree with `idf/partitions.csv` | Refer to [The checks of the script](../reference/release-manifest.md#the-checks-of-the-script) |
| The job **Publish the pre-release** fails: the release exists | The tag has a release already | The workflow never replaces a release. Make a release with a new version |

To delete a tag that has no release:

```bash
git push --delete origin v0.2.0
git tag -d v0.2.0
```

Do not delete or move a tag that has a release. Users can have its images.

## Do the checks on your computer

You can run the build and the script of the workflow on your computer, with ESP-IDF. Refer to [Build and flash the firmware](build-the-firmware.md) for the environment.

1. Make sure that `git status` shows no change in `idf/`.
2. Go to the folder `idf/`.
3. Do the release build in its own folder:

    ```bash
    idf.py -B build-release -D GW_RELEASE=ON set-target esp32s3 build
    ```

4. Run the script:

    ```bash
    python3 tools/release/make_manifest.py --build-dir build-release --out build-release/release --release
    ```

5. Make sure that the script shows the table of the parts, and no error line.

`idf.py set-target` makes a new `sdkconfig` in `idf/`, also for a build in another folder.
