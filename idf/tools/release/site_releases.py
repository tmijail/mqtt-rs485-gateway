#!/usr/bin/env python3
"""Make the copy of the firmware releases for the documentation site (issue #219).

    gh release list --limit 100 --json tagName,isDraft,isPrerelease,publishedAt > releases.json
    python3 idf/tools/release/site_releases.py --list releases.json --tags
    gh release download v0.2.0 --dir releases/v0.2.0            # for each tag of --tags
    python3 idf/tools/release/site_releases.py --list releases.json --releases releases \\
        --out site/firmware

A browser cannot download a GitHub Release asset from another origin (no CORS headers), so the
install tool on the documentation site reads the images from a copy on the same site. The
workflow docs.yml makes this copy on each build.

--list is the JSON of "gh release list". The script keeps the releases with a tag
v0.<minor>.<patch> (the TAG_RE of make_manifest.py) that are not drafts. --tags prints these tags,
one on each line. With --releases and --out, the script examines the folder <releases>/<tag> of
each kept release: SHA256SUMS, and manifest.json against the tag and the image files. Then it
copies the six files of each release into <out>/<tag>/ and writes <out>/index.json, the list of
the releases for the install tool. The formats are in docs/reference/release-manifest.md.

A check that fails gives an error text and the exit status 1; the script then writes nothing.
Only the Python standard library.
"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import make_manifest as mm  # noqa: E402

INDEX_FORMAT = 1                # the format number of index.json
IMAGES = ("bootloader.bin", "partition-table.bin", "ota_data_initial.bin",
          "rs485_spb_gateway.bin")
FILES = IMAGES + ("manifest.json", "SHA256SUMS")
PART_NAMES = {part for part, _, _ in mm.PARTS}
CHIP_NAMES = {chip for chip, _ in mm.CHIPS.values()}
SUM_LINE = re.compile(r"([0-9a-f]{64}) [ *](.+)")   # the format of sha256sum (text or binary)


class SiteError(Exception):
    """One or more checks failed: problems has one text for each."""

    def __init__(self, problems):
        self.problems = [problems] if isinstance(problems, str) else list(problems)
        super().__init__("\n".join(self.problems))


def version_key(tag):
    """The sort key of a release tag: (minor, patch) as numbers."""
    m = mm.TAG_RE.fullmatch(tag)
    return int(m.group(1)), int(m.group(2))


def read_list(path):
    """The kept releases of the JSON of "gh release list": dicts with tag, published and
    prerelease, in the sequence of the file. Raise SiteError with each problem."""
    try:
        data = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    except OSError as e:
        raise SiteError(f"cannot read the release list {path}: {e.strerror}") from None
    except ValueError as e:
        raise SiteError(f"the release list {path} is not JSON: {e}") from None
    if not isinstance(data, list):
        raise SiteError(f"the release list {path} is not a JSON array")
    problems = []
    kept = []
    seen = set()
    for n, entry in enumerate(data):
        tag = entry.get("tagName") if isinstance(entry, dict) else None
        if not isinstance(tag, str):
            problems.append(f"the release list: entry {n} has no tagName")
            continue
        if not isinstance(entry.get("isDraft"), bool):
            problems.append(f"the release list: {tag!r} has no isDraft (true or false)")
            continue
        if entry["isDraft"] or not mm.TAG_RE.fullmatch(tag):
            continue        # a draft, or not a release tag v0.<minor>.<patch>
        if tag in seen:
            problems.append(f"the release list has the tag {tag} two times")
            continue
        seen.add(tag)
        published = entry.get("publishedAt")
        prerelease = entry.get("isPrerelease")
        if not isinstance(published, str) or not published:
            problems.append(f"the release list: {tag} has no publishedAt")
        if not isinstance(prerelease, bool):
            problems.append(f"the release list: {tag} has no isPrerelease (true or false)")
        kept.append({"tag": tag, "published": published, "prerelease": prerelease})
    if problems:
        raise SiteError(problems)
    return kept


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def check_release(folder, tag):
    """Examine the six files of the release @tag in @folder. Give {file name: SHA-256} of the
    six files, or raise SiteError with each problem."""
    folder = pathlib.Path(folder)
    where = tag
    if not folder.is_dir():
        raise SiteError(f"{where}: the folder {folder} does not exist: download the release")
    # a plain file: a link could copy a file from outside the release folder
    missing = [name for name in FILES
               if not (folder / name).is_file() or (folder / name).is_symlink()]
    if missing:
        raise SiteError(f"{where}: the release has no file {', '.join(missing)}")
    digests = {name: sha256(folder / name) for name in FILES}
    problems = []

    # SHA256SUMS: each line is a known file with its SHA-256, and each other file has a line
    listed = {}
    text = (folder / "SHA256SUMS").read_text(encoding="utf-8", errors="replace")
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        m = SUM_LINE.fullmatch(line.rstrip("\r"))
        if not m:
            problems.append(f"{where}: SHA256SUMS line {n} is not a line of sha256sum")
            continue
        digest, name = m.groups()
        if name not in FILES[:-1]:
            problems.append(f"{where}: SHA256SUMS line {n} names {name!r}, which is not a file "
                            "of a release")
        elif name in listed:
            problems.append(f"{where}: SHA256SUMS names {name} two times")
        else:
            listed[name] = digest
            if digest != digests[name]:
                problems.append(f"{where}: the SHA-256 of {name} is not the SHA-256 of "
                                "SHA256SUMS: the file is damaged or wrong")
    for name in FILES[:-1]:
        if name not in listed:
            problems.append(f"{where}: SHA256SUMS does not name {name}")

    # manifest.json: the release of this tag, for this chip, with each image file
    try:
        manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    except ValueError as e:
        raise SiteError(problems + [f"{where}: manifest.json is not JSON: {e}"]) from None
    if not isinstance(manifest, dict):
        raise SiteError(problems + [f"{where}: manifest.json is not a JSON object"])
    expected = {"format": mm.FORMAT, "name": mm.PROJECT, "version": tag[1:]}
    for key, value in expected.items():
        if manifest.get(key) != value:
            problems.append(f"{where}: manifest.json has the {key} {manifest.get(key)!r}, "
                            f"not {value!r}")
    if manifest.get("chip") not in CHIP_NAMES:
        problems.append(f"{where}: manifest.json has the chip {manifest.get('chip')!r}, not "
                        f"{' or '.join(sorted(CHIP_NAMES))}")
    parts = manifest.get("parts")
    if not isinstance(parts, list):
        raise SiteError(problems + [f"{where}: manifest.json has no list parts"])
    names, files = [], []
    for n, part in enumerate(parts):
        if not isinstance(part, dict):
            problems.append(f"{where}: manifest.json, part {n} is not a JSON object")
            continue
        name, file = part.get("name"), part.get("file")
        names.append(name)
        files.append(file)
        label = f"{where}: manifest.json, part {name!r}"
        if name not in PART_NAMES:
            problems.append(f"{label}: not a part of a release ({', '.join(sorted(PART_NAMES))})")
        if file not in IMAGES:
            # a plain name of the release folder only: no path, no "..", no other file
            problems.append(f"{label}: the file {file!r} is not an image file of a release "
                            f"({', '.join(IMAGES)})")
            continue
        offset, size, digest = part.get("offset"), part.get("size"), part.get("sha256")
        if not isinstance(offset, int) or isinstance(offset, bool) or offset < 0:
            problems.append(f"{label}: the offset {offset!r} is not a number of 0 or more")
        actual = (folder / file).stat().st_size
        if size != actual or isinstance(size, bool):
            problems.append(f"{label}: the size {size!r} is not the size of {file} ({actual})")
        if digest != digests[file]:
            problems.append(f"{label}: the sha256 is not the SHA-256 of {file}")
    if sorted(n for n in names if isinstance(n, str)) != sorted(PART_NAMES) or \
            len(names) != len(PART_NAMES):
        problems.append(f"{where}: manifest.json does not have each part one time: "
                        f"{', '.join(sorted(PART_NAMES))}")
    if sorted(f for f in files if isinstance(f, str)) != sorted(IMAGES) or \
            len(files) != len(IMAGES):
        problems.append(f"{where}: manifest.json does not name each image file one time: "
                        f"{', '.join(IMAGES)}")
    if problems:
        raise SiteError(problems)
    return digests


def plan(list_path, releases_dir):
    """Examine the kept releases of @list_path in @releases_dir. Give the entries of index.json
    (newest first) and the SHA-256 of the files of each release. Raise SiteError with each
    problem of each release."""
    releases = read_list(list_path)
    problems = []
    digests = {}
    for r in releases:
        try:
            digests[r["tag"]] = check_release(pathlib.Path(releases_dir) / r["tag"], r["tag"])
        except SiteError as e:
            problems += e.problems
    if problems:
        raise SiteError(problems)
    releases.sort(key=lambda r: version_key(r["tag"]), reverse=True)
    index = [{"tag": r["tag"], "version": r["tag"][1:], "published": r["published"],
              "prerelease": r["prerelease"], "manifest": f"{r['tag']}/manifest.json"}
             for r in releases]
    return index, digests


def clear_output(out_dir):
    """Make @out_dir empty. It may hold only what an earlier run wrote: index.json and folders
    v0.<minor>.<patch> with the files of a release."""
    out_dir = pathlib.Path(out_dir)
    if not out_dir.exists():
        return
    if not out_dir.is_dir():
        raise SiteError(f"the output {out_dir} is not a folder")
    other = []
    for x in out_dir.iterdir():
        if x.name == "index.json" and x.is_file():
            continue
        if (x.is_dir() and not x.is_symlink() and mm.TAG_RE.fullmatch(x.name)
                and all(y.name in FILES and y.is_file() for y in x.iterdir())):
            continue
        other.append(x.name)
    if other:
        raise SiteError(f"the output folder {out_dir} has other files ({', '.join(sorted(other))})"
                        ": give an empty folder")
    for x in out_dir.iterdir():
        if x.is_dir():
            shutil.rmtree(x)
        else:
            x.unlink()


def write_site(index, digests, releases_dir, out_dir):
    """Copy the six files of each release into @out_dir/<tag>/ and write @out_dir/index.json."""
    out_dir = pathlib.Path(out_dir)
    clear_output(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for r in index:
        dest = out_dir / r["tag"]
        dest.mkdir()
        for name in FILES:
            data = (pathlib.Path(releases_dir) / r["tag"] / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != digests[r["tag"]][name]:
                raise SiteError(f"{r['tag']}: {name} changed while the script examined it")
            (dest / name).write_bytes(data)
    text = json.dumps({"format": INDEX_FORMAT, "releases": index}, indent=2) + "\n"
    (out_dir / "index.json").write_text(text, encoding="utf-8")


def summary(index):
    """A Markdown table of the copy, for the output and the job summary."""
    if not index:
        return "### Firmware on the site\n\nNo release: index.json has an empty list."
    lines = ["### Firmware on the site", "", "| Tag | Published | Pre-release |", "|---|---|---|"]
    lines += [f"| {r['tag']} | {r['published']} | {'yes' if r['prerelease'] else 'no'} |"
              for r in index]
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--list", type=pathlib.Path, required=True,
                    help="the JSON of gh release list --json "
                         "tagName,isDraft,isPrerelease,publishedAt")
    ap.add_argument("--tags", action="store_true",
                    help="print the tags of the kept releases, one on each line, and stop")
    ap.add_argument("--releases", type=pathlib.Path,
                    help="the folder with one folder <tag> for each kept release")
    ap.add_argument("--out", type=pathlib.Path,
                    help="the output folder, for example site/firmware")
    a = ap.parse_args(argv)
    try:
        if a.tags:
            for r in read_list(a.list):
                print(r["tag"])
            return 0
        if a.releases is None or a.out is None:
            ap.error("--releases and --out are necessary (or give --tags)")
        index, digests = plan(a.list, a.releases)
        write_site(index, digests, a.releases, a.out)
    except SiteError as e:
        for problem in e.problems:
            print(f"site_releases: error: {problem}", file=sys.stderr)
        return 1
    print(summary(index))
    return 0


if __name__ == "__main__":
    sys.exit(main())
