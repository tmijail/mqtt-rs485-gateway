# Build the documentation site

This procedure tells how to build the documentation site from `docs/`, and how to examine it on your computer before you push a change. The site is at <https://mijail.ar/mqtt-rs485-gateway/>.

Zensical builds the site. Its configuration is `zensical.toml` at the root of the repository. The version is pinned in `idf/tools/docs/requirements-site.txt`.

## Before you start

You need Python 3.11 or later.

## Build and examine the site

1. Go to the root of the repository.
2. Make a virtual environment:

    ```bash
    python3 -m venv ~/docs-venv
    ```

3. Install the pinned version of Zensical:

    ```bash
    ~/docs-venv/bin/pip install -r idf/tools/docs/requirements-site.txt
    ```

4. Start the local server:

    ```bash
    ~/docs-venv/bin/zensical serve
    ```

5. Open <http://localhost:8000/> in a browser.
6. Change a document. The server builds the site again, and the browser shows the change.
7. Stop the server with Ctrl+C.
8. Do the build of the CI:

    ```bash
    ~/docs-venv/bin/zensical build --clean --strict
    ```

9. Make sure that the build gives `No issues found`. A warning stops the CI job `docs`.

The build writes the site into `site/` and a cache into `.cache/`. Git ignores the two folders.

A local build has no folder `site/firmware/`, the copy of the release images. Only the workflow `docs.yml` makes it. To make it on your computer, refer to [Add the release images to a local build](#add-the-release-images-to-a-local-build).

## Add the release images to a local build

Do this procedure when you work on a page that reads `firmware/index.json`, for example the install tool. For the folder and the format of `index.json`, refer to [The copy on the documentation site](../reference/release-manifest.md#the-copy-on-the-documentation-site).

You need the GitHub CLI `gh`, with a login (`gh auth status`).

1. Do the build of the site:

    ```bash
    ~/docs-venv/bin/zensical build --clean --strict
    ```

2. Get the list of the releases:

    ```bash
    gh release list --repo tmijail/mqtt-rs485-gateway --limit 100 \
        --json tagName,isDraft,isPrerelease,publishedAt > /tmp/releases.json
    ```

3. Download each release that the script keeps:

    ```bash
    for tag in $(python3 idf/tools/release/site_releases.py --list /tmp/releases.json --tags); do
      gh release download "$tag" --repo tmijail/mqtt-rs485-gateway --dir "/tmp/releases/$tag"
    done
    ```

4. Examine the releases and copy them into the site:

    ```bash
    python3 idf/tools/release/site_releases.py --list /tmp/releases.json \
        --releases /tmp/releases --out site/firmware
    ```

5. Make sure that the script shows the table of the releases, and no error line.
6. Serve the folder `site/`:

    ```bash
    python3 -m http.server 8001 --directory site
    ```

7. Open <http://localhost:8001/firmware/index.json> in a browser.

`zensical build --clean` deletes `site/`, also `site/firmware/`. After each build, do step 4 again.

With no release, the script writes an `index.json` with an empty list. To examine a page with a release before the first release, use a release build of your computer:

1. Do a release build, and run `make_manifest.py` on it. Refer to [Do the checks on your computer](make-a-release.md#do-the-checks-on-your-computer). Use `--out /tmp/releases/v0.1.0`, with the version of `GW_VERSION`.
2. Write a list with this release into `/tmp/releases.json`:

    ```json
    [{"tagName": "v0.1.0", "isDraft": false, "isPrerelease": true, "publishedAt": "2026-11-30T12:00:00Z"}]
    ```

3. Do steps 4 to 7 of the procedure above.

## Add a document

1. Put the document in the folder of its Diátaxis type: `tutorials/`, `how-to/`, `reference/` or `explanation/`.
2. Add the path of the document to `nav` in `zensical.toml`.
3. Add a link to the document in `docs/README.md`.
4. Do the build with `--strict` again.

Zensical does not warn about a document that is not in `nav`. The site then has no menu entry for it.

## Write links that work on GitHub and on the site

- Link to another document with the relative path of its `.md` file.
- Link to a file outside `docs/` with its full GitHub URL: `https://github.com/tmijail/mqtt-rs485-gateway/blob/main/<path>`. Use `tree` in place of `blob` for a folder. The site does not contain the files outside `docs/`.
- Put an empty line before each heading. On the site, a heading directly after a table becomes a row of the table.
- Do not put `<` or `>` in a heading that a link uses. GitHub and the site make different anchors from these characters. Use `{name}` in place of `<name>`.

The anchors of the site are the same as the anchors of GitHub (`toc.slugify` in `zensical.toml`). Thus a link to an anchor, for example `#no--wundef-in-the-firmware`, works in the two places.

## Publish the site

The workflow `.github/workflows/docs.yml` builds the site on each push and on each pull request. After the Zensical build, it copies the release images into `site/firmware/`. On a push to `main`, the job `docs-deploy` publishes the site to GitHub Pages. At the end of each release, the workflow builds the site from `main` again and publishes it with the new release. Refer to [When the site gets a new release](../reference/release-manifest.md#when-the-site-gets-a-new-release).

For the first publication, an owner of the repository sets **Settings → Pages → Build and deployment → Source** to **GitHub Actions**.
