#!/usr/bin/env python3
"""Self-test of site_releases.py: fake releases give the copy for the documentation site and
index.json, and each check refuses a damaged release (issue #219).

    python3 idf/tools/release/test_site_releases.py

Only the Python standard library. make_manifest.py writes the files of each fake release, from
the fake build of test_make_manifest.py.
"""
import contextlib
import hashlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import make_manifest as mm  # noqa: E402
import site_releases as sr  # noqa: E402
import test_make_manifest as tm  # noqa: E402


class Case(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = pathlib.Path(tmp.name)
        self.releases = self.root / "releases"
        self.out = self.root / "site" / "firmware"
        self.list_path = self.root / "releases.json"
        self.entries = []

    def release(self, tag, published="2026-11-30T12:00:00Z", prerelease=True, draft=False,
                files=True):
        """Add a release to the list, and write its six files with make_manifest.py."""
        self.entries.append({"tagName": tag, "isDraft": draft, "isPrerelease": prerelease,
                             "publishedAt": published})
        if files:
            b = tm.FakeBuild(self.root / "builds" / tag, version=tag[1:])
            manifest, sources = b.write().plan()
            mm.write_release(manifest, sources, self.releases / tag)
        return self.releases / tag

    def write_list(self):
        self.list_path.write_text(json.dumps(self.entries))
        return self.list_path

    def plan(self):
        return sr.plan(self.write_list(), self.releases)

    def refused(self, text):
        """The checks refuse the releases, and one problem has @text."""
        with self.assertRaises(sr.SiteError) as cm:
            self.plan()
        self.assertTrue(any(text in p for p in cm.exception.problems), cm.exception.problems)
        return cm.exception.problems

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = sr.main(["--list", str(self.write_list()), *args])
        return rc, out.getvalue(), err.getvalue()


class Good(Case):
    def test_index_and_copy(self):
        self.release("v0.1.0", published="2026-11-01T10:00:00Z")
        self.release("v0.2.0", published="2026-11-30T12:00:00Z", prerelease=False)
        rc, out, err = self.run_main("--releases", str(self.releases), "--out", str(self.out))
        self.assertEqual(rc, 0, err)
        self.assertIn("| v0.2.0 | 2026-11-30T12:00:00Z | no |", out)
        index = json.loads((self.out / "index.json").read_text())
        self.assertEqual(index, {"format": 1, "releases": [
            {"tag": "v0.2.0", "version": "0.2.0", "published": "2026-11-30T12:00:00Z",
             "prerelease": False, "manifest": "v0.2.0/manifest.json"},
            {"tag": "v0.1.0", "version": "0.1.0", "published": "2026-11-01T10:00:00Z",
             "prerelease": True, "manifest": "v0.1.0/manifest.json"}]})
        for tag in ("v0.1.0", "v0.2.0"):
            self.assertEqual(sorted(p.name for p in (self.out / tag).iterdir()), sorted(sr.FILES))
            for name in sr.FILES:
                self.assertEqual((self.out / tag / name).read_bytes(),
                                 (self.releases / tag / name).read_bytes())
            # the manifest path of index.json is relative to index.json
            manifest = json.loads((self.out / tag / "manifest.json").read_text())
            self.assertEqual(manifest["version"], tag[1:])

    def test_no_release(self):
        """The site has no release yet: an empty list, and the step succeeds."""
        rc, out, _ = self.run_main("--releases", str(self.releases), "--out", str(self.out))
        self.assertEqual(rc, 0)
        self.assertIn("No release", out)
        self.assertEqual(json.loads((self.out / "index.json").read_text()),
                         {"format": 1, "releases": []})
        self.assertEqual([p.name for p in self.out.iterdir()], ["index.json"])
        self.assertFalse(self.releases.exists())

    def test_newest_first_by_number(self):
        """The sequence is by the numbers of the version, not by the date or the text."""
        for tag, day in (("v0.9.0", "03"), ("v0.10.0", "01"), ("v0.2.11", "04"),
                         ("v0.2.3", "05"), ("v0.10.2", "02")):
            self.release(tag, published=f"2026-11-{day}T00:00:00Z")
        index, _ = self.plan()
        self.assertEqual([r["tag"] for r in index],
                         ["v0.10.2", "v0.10.0", "v0.9.0", "v0.2.11", "v0.2.3"])

    def test_drafts_and_other_tags_are_dropped(self):
        self.release("v0.1.0")
        self.release("v0.2.0", draft=True, files=False)
        for tag in ("v1.0", "v1.0.0", "v0.1", "v0.3.0-rc1", "v0.01.0", "nightly"):
            self.release(tag, files=False)
        index, _ = self.plan()
        self.assertEqual([r["tag"] for r in index], ["v0.1.0"])
        rc, out, _ = self.run_main("--tags")
        self.assertEqual((rc, out), (0, "v0.1.0\n"))

    def test_tags_of_an_empty_list(self):
        self.assertEqual(self.run_main("--tags"), (0, "", ""))

    def test_other_folders_and_files_are_not_copied(self):
        folder = self.release("v0.1.0")
        (folder / "notes.txt").write_text("x")
        (self.releases / "v0.0.9").mkdir()          # not in the list
        self.assertEqual(self.run_main("--releases", str(self.releases), "--out",
                                       str(self.out))[0], 0)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ["index.json", "v0.1.0"])
        self.assertNotIn("notes.txt", [p.name for p in (self.out / "v0.1.0").iterdir()])

    def test_sha256sums_in_binary_mode(self):
        folder = self.release("v0.1.0")
        sums = folder / "SHA256SUMS"
        sums.write_text(sums.read_text().replace("  ", " *"))
        self.plan()

    def test_second_run_replaces_the_copy(self):
        self.release("v0.1.0")
        self.release("v0.2.0")
        self.assertEqual(self.run_main("--releases", str(self.releases), "--out",
                                       str(self.out))[0], 0)
        self.entries.pop()                          # v0.2.0 is not in the list now
        self.assertEqual(self.run_main("--releases", str(self.releases), "--out",
                                       str(self.out))[0], 0)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ["index.json", "v0.1.0"])

    def test_output_folder_with_other_files(self):
        self.release("v0.1.0")
        self.out.mkdir(parents=True)
        (self.out / "page.html").write_text("x")
        rc, _, err = self.run_main("--releases", str(self.releases), "--out", str(self.out))
        self.assertEqual(rc, 1)
        self.assertIn("has other files (page.html)", err)
        self.assertEqual([p.name for p in self.out.iterdir()], ["page.html"])


class Damaged(Case):
    def test_refusal_writes_nothing(self):
        self.release("v0.2.0")
        folder = self.release("v0.1.0")
        (folder / "rs485_spb_gateway.bin").write_bytes(b"\0" * 10)
        rc, _, err = self.run_main("--releases", str(self.releases), "--out", str(self.out))
        self.assertEqual(rc, 1)
        self.assertIn("site_releases: error: v0.1.0: the SHA-256 of rs485_spb_gateway.bin "
                      "is not the SHA-256 of SHA256SUMS", err)
        self.assertFalse(self.out.exists())

    def test_release_not_downloaded(self):
        self.release("v0.1.0", files=False)
        self.refused("v0.1.0: the folder")

    def test_missing_file(self):
        for name in sr.FILES:
            with self.subTest(name=name):
                self.setUp()
                (self.release("v0.1.0") / name).unlink()
                self.refused(f"v0.1.0: the release has no file {name}")

    def test_file_is_a_link(self):
        folder = self.release("v0.1.0")
        target = self.root / "outside.bin"
        (folder / "bootloader.bin").rename(target)
        (folder / "bootloader.bin").symlink_to(target)
        self.refused("v0.1.0: the release has no file bootloader.bin")

    def test_each_damaged_file(self):
        for name in sr.IMAGES + ("manifest.json",):
            with self.subTest(name=name):
                self.setUp()
                path = self.release("v0.1.0") / name
                path.write_bytes(path.read_bytes() + b"\0")
                self.refused(f"the SHA-256 of {name} is not the SHA-256 of SHA256SUMS")

    def test_sha256sums_without_a_file(self):
        folder = self.release("v0.1.0")
        sums = folder / "SHA256SUMS"
        sums.write_text("".join(x + "\n" for x in sums.read_text().splitlines()
                                if not x.endswith("manifest.json")))
        self.refused("SHA256SUMS does not name manifest.json")

    def test_sha256sums_with_another_file(self):
        folder = self.release("v0.1.0")
        with open(folder / "SHA256SUMS", "a") as f:
            f.write(f"{'0' * 64}  ../index.html\n{'0' * 64}  storage.bin\n")
        problems = self.refused("names '../index.html', which is not a file of a release")
        self.assertTrue(any("'storage.bin'" in p for p in problems), problems)

    def test_sha256sums_two_times_one_file(self):
        folder = self.release("v0.1.0")
        sums = folder / "SHA256SUMS"
        sums.write_text(sums.read_text() + sums.read_text().splitlines()[0] + "\n")
        self.refused("SHA256SUMS names bootloader.bin two times")

    def test_sha256sums_bad_line(self):
        folder = self.release("v0.1.0")
        with open(folder / "SHA256SUMS", "a") as f:
            f.write("not a line\n")
        self.refused("SHA256SUMS line 6 is not a line of sha256sum")

    def manifest(self, change, tag="v0.1.0"):
        """Change manifest.json of a release with @change(dict), and write SHA256SUMS again so
        that only the check of the manifest can find the problem."""
        folder = self.release(tag)
        manifest = json.loads((folder / "manifest.json").read_text())
        change(manifest)
        text = json.dumps(manifest, indent=2) + "\n"
        (folder / "manifest.json").write_text(text)
        sums = (folder / "SHA256SUMS").read_text().splitlines()
        sums[-1] = f"{hashlib.sha256(text.encode()).hexdigest()}  manifest.json"
        (folder / "SHA256SUMS").write_text("\n".join(sums) + "\n")

    def test_manifest_of_another_version(self):
        self.manifest(lambda m: m.update(version="0.0.9"))
        self.refused("v0.1.0: manifest.json has the version '0.0.9', not '0.1.0'")

    def test_manifest_of_a_development_build(self):
        self.manifest(lambda m: m.update(version="0.1.0+g0123abcd"))
        self.refused("has the version '0.1.0+g0123abcd'")

    def test_manifest_format_name_chip(self):
        for key, value, text in (("format", 2, "has the format 2, not 1"),
                                 ("name", "hello_world", "has the name 'hello_world'"),
                                 ("chip", "ESP32", "has the chip 'ESP32', not ESP32-S3")):
            with self.subTest(key=key):
                self.setUp()
                self.manifest(lambda m, k=key, v=value: m.update({k: v}))
                self.refused(text)

    def test_manifest_part_with_a_path(self):
        for file in ("../../index.html", "sub/bootloader.bin", "/etc/passwd", "storage.bin",
                     None):
            with self.subTest(file=file):
                self.setUp()
                self.manifest(lambda m, f=file: m["parts"][0].update(file=f))
                problems = self.refused(f"the file {file!r} is not an image file of a release")
                self.assertTrue(any("does not name each image file one time" in p
                                    for p in problems), problems)

    def test_manifest_size_and_sha256(self):
        self.manifest(lambda m: m["parts"][3].update(size=m["parts"][3]["size"] + 1))
        self.refused("part 'app': the size")
        self.setUp()
        self.manifest(lambda m: m["parts"][2].update(sha256="0" * 64))
        self.refused("part 'otadata': the sha256 is not the SHA-256 of ota_data_initial.bin")

    def test_manifest_offset(self):
        self.manifest(lambda m: m["parts"][1].update(offset="0x8000"))
        self.refused("part 'partition-table': the offset '0x8000' is not a number")

    def test_manifest_parts(self):
        self.manifest(lambda m: m["parts"].pop())
        problems = self.refused("does not have each part one time")
        self.assertTrue(any("does not name each image file one time" in p for p in problems))
        self.setUp()
        self.manifest(lambda m: m["parts"][0].update(name="factory"))
        self.refused("part 'factory': not a part of a release")
        self.setUp()
        self.manifest(lambda m: m.update(parts={}))
        self.refused("manifest.json has no list parts")

    def test_manifest_not_json(self):
        folder = self.release("v0.1.0")
        (folder / "manifest.json").write_text("{")
        self.refused("v0.1.0: manifest.json is not JSON")

    def test_each_release_gives_its_problems(self):
        self.manifest(lambda m: m.update(version="0.0.1"), tag="v0.1.0")
        self.manifest(lambda m: m.update(version="0.0.2"), tag="v0.2.0")
        problems = self.refused("v0.1.0: manifest.json has the version '0.0.1'")
        self.assertTrue(any("v0.2.0: manifest.json has the version '0.0.2'" in p
                            for p in problems), problems)


class ReleaseList(Case):
    def test_not_json(self):
        self.list_path.write_text("[")
        with self.assertRaises(sr.SiteError) as cm:
            sr.read_list(self.list_path)
        self.assertIn("is not JSON", str(cm.exception))
        self.list_path.write_text("{}")
        with self.assertRaises(sr.SiteError) as cm:
            sr.read_list(self.list_path)
        self.assertIn("is not a JSON array", str(cm.exception))

    def test_bad_entries(self):
        self.entries = [{"isDraft": False}, {"tagName": "v0.1.0"},
                        {"tagName": "v0.2.0", "isDraft": False},
                        {"tagName": "v0.3.0", "isDraft": False, "isPrerelease": True,
                         "publishedAt": "x"},
                        {"tagName": "v0.3.0", "isDraft": False, "isPrerelease": True,
                         "publishedAt": "x"}]
        rc, _, err = self.run_main("--tags")
        self.assertEqual(rc, 1)
        self.assertIn("entry 0 has no tagName", err)
        self.assertIn("'v0.1.0' has no isDraft", err)
        self.assertIn("v0.2.0 has no publishedAt", err)
        self.assertIn("v0.2.0 has no isPrerelease", err)
        self.assertIn("has the tag v0.3.0 two times", err)

    def test_the_tag_rule_of_make_manifest(self):
        """One rule for a release tag: the TAG_RE of make_manifest.py."""
        self.assertIs(sr.mm.TAG_RE, mm.TAG_RE)
        self.assertEqual(sr.version_key("v0.12.3"), (12, 3))

    def test_files_of_make_manifest(self):
        """The six files are the files that make_manifest.py writes."""
        b = tm.FakeBuild(self.root)
        manifest, sources = b.write().plan()
        mm.write_release(manifest, sources, b.out)
        self.assertEqual(sorted(p.name for p in b.out.iterdir()), sorted(sr.FILES))
        self.assertEqual([p["file"] for p in manifest["parts"]], list(sr.IMAGES))


if __name__ == "__main__":
    unittest.main()
