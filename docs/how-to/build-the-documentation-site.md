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

The workflow `.github/workflows/docs.yml` builds the site on each push and on each pull request. On a push to `main`, the job `docs-deploy` publishes it to GitHub Pages.

For the first publication, an owner of the repository sets **Settings → Pages → Build and deployment → Source** to **GitHub Actions**.
