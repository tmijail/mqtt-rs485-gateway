#!/usr/bin/env python3
"""Self-test of make_manifest.py: a small fake build gives the manifest, and each check refuses
a build that breaks it (issue #279).

    python3 idf/tools/release/test_make_manifest.py

Only the Python standard library. The fake build has the partition table of idf/partitions.csv.
"""
import contextlib
import hashlib
import io
import json
import pathlib
import re
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import make_manifest as mm  # noqa: E402

IDF_DIR = pathlib.Path(__file__).resolve().parents[2]
PARTITIONS = IDF_DIR / "partitions.csv"
BOARD_SDKCONFIG = """\
CONFIG_IDF_TARGET="esp32s3"
CONFIG_BOOTLOADER_OFFSET_IN_FLASH=0x0
CONFIG_PARTITION_TABLE_OFFSET=0x8000
# CONFIG_GW_IO_SIMULATED is not set
# CONFIG_NVS_ENCRYPTION is not set
# CONFIG_SECURE_BOOT is not set
CONFIG_SECURE_BOOT_V2_RSA_SUPPORTED=y
CONFIG_BOOT_ROM_LOG_ALWAYS_ON=y
"""


def table_bin(rows):
    """A partition table binary, as gen_esp32part.py writes it: the entries, the MD5 entry and
    0xFF up to 0xC00 bytes."""
    data = b"".join(mm.PT_ENTRY.pack(mm.PT_MAGIC, r["type"], r["subtype"], r["offset"], r["size"],
                                     r["name"].encode(), 0) for r in rows)
    data += mm.PT_MD5_MAGIC + b"\xff" * 14 + hashlib.md5(data).digest()
    return data.ljust(0xC00, b"\xff")


def image(chip_id=9, body=b""):
    """The header of an ESP image (esp_image_header_t) and one segment header, then @body."""
    return (struct.pack("<BBBBI4sH", 0xE9, 1, 2, 0x4F, 0x40000000, b"\xee\0\0\0", chip_id)
            .ljust(24, b"\0")
            + struct.pack("<II", 0x3C000020, len(body)) + body)


def app_image(version="0.1.0", project="rs485_spb_gateway", chip_id=9, pad=4096):
    desc = (struct.pack("<IIII", mm.APP_DESC_MAGIC, 0, 0, 0) + version.encode().ljust(32, b"\0")
            + project.encode().ljust(32, b"\0"))
    return image(chip_id, desc.ljust(256, b"\0") + b"\xa5" * pad)


class FakeBuild:
    """A build directory with the files that make_manifest.py reads. A test changes one item
    before write()."""

    def __init__(self, root, version="0.1.0"):
        self.dir = pathlib.Path(root) / "build"
        self.out = pathlib.Path(root) / "release"
        self.version = version
        self.project = "rs485_spb_gateway"
        self.target = "esp32s3"
        self.sdkconfig = BOARD_SDKCONFIG
        self.rows = mm.read_partitions_csv(PARTITIONS)
        self.files = {
            "bootloader/bootloader.bin": image(body=b"\x11" * 5000),
            "partition_table/partition-table.bin": None,   # from self.rows, in write()
            "ota_data_initial.bin": b"\xff" * 0x2000,
            "storage.bin": b"\x22" * 0x1000,
            "rs485_spb_gateway.bin": app_image(version),
        }
        self.args = {
            "flash_settings": {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"},
            "flash_files": {"0x0": "bootloader/bootloader.bin",
                            "0x8000": "partition_table/partition-table.bin",
                            "0xf000": "ota_data_initial.bin", "0x820000": "storage.bin",
                            "0x20000": "rs485_spb_gateway.bin"},
            "bootloader": {"offset": "0x0", "file": "bootloader/bootloader.bin",
                           "encrypted": "false"},
            "partition-table": {"offset": "0x8000",
                                "file": "partition_table/partition-table.bin",
                                "encrypted": "false"},
            "otadata": {"offset": "0xf000", "file": "ota_data_initial.bin",
                        "encrypted": "false"},
            "storage": {"offset": "0x820000", "file": "storage.bin", "encrypted": "false"},
            "app": {"offset": "0x20000", "file": "rs485_spb_gateway.bin", "encrypted": "false"},
            "extra_esptool_args": {"after": "hard-reset", "before": "default-reset",
                                   "stub": True, "chip": "esp32s3"},
        }

    def write(self):
        if self.files["partition_table/partition-table.bin"] is None:
            self.files["partition_table/partition-table.bin"] = table_bin(self.rows)
        for name, data in self.files.items():
            path = self.dir / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.dir / "sdkconfig").write_text(self.sdkconfig)
        (self.dir / "flasher_args.json").write_text(json.dumps(self.args))
        (self.dir / "project_description.json").write_text(json.dumps({
            "project_name": self.project, "project_version": self.version,
            "target": self.target, "config_file": str(self.dir / "sdkconfig")}))
        return self

    def plan(self, **kw):
        return mm.plan(self.dir, kw.pop("partitions", PARTITIONS), **kw)


class Case(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = pathlib.Path(tmp.name)
        self.b = FakeBuild(self.root)

    def refused(self, text, **kw):
        """The checks refuse the build, and one problem has @text."""
        with self.assertRaises(mm.ManifestError) as cm:
            self.b.write().plan(**kw)
        self.assertTrue(any(text in p for p in cm.exception.problems), cm.exception.problems)
        return cm.exception.problems


class GoodBuild(Case):
    def test_manifest(self):
        manifest, sources = self.b.write().plan()
        self.assertEqual(manifest["format"], 1)
        self.assertEqual(manifest["name"], "rs485_spb_gateway")
        self.assertEqual(manifest["version"], "0.1.0")
        self.assertEqual(manifest["chip"], "ESP32-S3")
        self.assertEqual(manifest["flash"], {"mode": "dio", "freq": "80m", "size": "16MB"})
        self.assertEqual([(p["name"], p["file"], p["offset"], p["partition"])
                          for p in manifest["parts"]],
                         [("bootloader", "bootloader.bin", 0x0, None),
                          ("partition-table", "partition-table.bin", 0x8000, None),
                          ("otadata", "ota_data_initial.bin", 0xF000, "otadata"),
                          ("app", "rs485_spb_gateway.bin", 0x20000, "ota_0")])
        for part, source in zip(manifest["parts"], sources, strict=True):
            data = source.read_bytes()
            self.assertEqual(part["size"], len(data))
            self.assertEqual(part["sha256"], hashlib.sha256(data).hexdigest())

    def test_no_part_for_the_data_of_the_owner(self):
        """A flash without an erase keeps the configuration and the secrets."""
        manifest, _ = self.b.write().plan()
        self.assertEqual([p["name"] for p in manifest["keep"]],
                         ["nvs", "phy_init", "ota_1", "storage", "coredump"])
        self.assertEqual(manifest["keep"][0], {"name": "nvs", "offset": 0x9000, "size": 0x6000})
        self.assertNotIn("storage.bin", [p["file"] for p in manifest["parts"]])
        kept = {p["name"] for p in manifest["keep"]}
        self.assertFalse(kept & {p["partition"] for p in manifest["parts"]})

    def test_output_directory(self):
        manifest, sources = self.b.write().plan()
        mm.write_release(manifest, sources, self.b.out)
        self.assertEqual(sorted(p.name for p in self.b.out.iterdir()),
                         ["SHA256SUMS", "bootloader.bin", "manifest.json",
                          "ota_data_initial.bin", "partition-table.bin",
                          "rs485_spb_gateway.bin"])
        self.assertEqual(json.loads((self.b.out / "manifest.json").read_text()), manifest)
        sums = (self.b.out / "SHA256SUMS").read_text().splitlines()
        self.assertEqual(len(sums), 5)
        for line in sums:
            digest, name = line.split("  ")
            self.assertEqual(hashlib.sha256((self.b.out / name).read_bytes()).hexdigest(),
                             digest)
        # a second run writes the same files again
        mm.write_release(manifest, sources, self.b.out)
        (self.b.out / "old.bin").write_bytes(b"x")
        with self.assertRaises(mm.ManifestError) as cm:
            mm.write_release(manifest, sources, self.b.out)
        self.assertIn("old.bin", str(cm.exception))

    def test_main(self):
        self.b.write()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = mm.main(["--build-dir", str(self.b.dir), "--out", str(self.b.out),
                          "--tag", "v0.1.0"])
        self.assertEqual(rc, 0)
        self.assertIn("| app | `rs485_spb_gateway.bin` | `0x20000` |", out.getvalue())
        self.assertTrue((self.b.out / "manifest.json").exists())

    def test_main_refusal_writes_nothing(self):
        self.b.sdkconfig += "CONFIG_GW_IO_SIMULATED=y\n"
        self.b.write()
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = mm.main(["--build-dir", str(self.b.dir), "--out", str(self.b.out)])
        self.assertEqual(rc, 1)
        self.assertIn("make_manifest: error: sdkconfig has CONFIG_GW_IO_SIMULATED=y",
                      err.getvalue())
        self.assertFalse(self.b.out.exists())

    def test_sdkconfig_argument(self):
        other = self.root / "sdkconfig.other"
        other.write_text(BOARD_SDKCONFIG + "CONFIG_GW_IO_SIMULATED=y\n")
        self.refused("sdkconfig.other has CONFIG_GW_IO_SIMULATED=y", sdkconfig=other)

    def test_development_version(self):
        """The CI makes the manifest of each build: a version with the git hash is permitted
        when the build is not for a release."""
        self.b = FakeBuild(self.root, version="0.1.0+g0123abcd")
        manifest, _ = self.b.write().plan()
        self.assertEqual(manifest["version"], "0.1.0+g0123abcd")


class NotTheBoardBuild(Case):
    def test_qemu_build(self):
        self.b.sdkconfig += "CONFIG_GW_IO_SIMULATED=y\n"
        self.refused("CONFIG_GW_IO_SIMULATED=y (a QEMU build")

    def test_each_refused_option(self):
        for option in mm.REFUSED_OPTIONS:
            with self.subTest(option=option):
                self.b = FakeBuild(self.root)
                self.b.sdkconfig += f"CONFIG_{option}=y\n"
                self.refused(f"CONFIG_{option}=y")

    def test_option_that_is_not_set(self):
        self.b.sdkconfig += "# CONFIG_SECURE_FLASH_ENC_ENABLED is not set\n"
        self.b.write().plan()

    def test_list_has_each_efuse_option_of_the_host_test(self):
        """The options of check_efuse_options.cmake (CLAUDE.md, Security constraints)."""
        text = (IDF_DIR / "test/host/check_efuse_options.cmake").read_text()
        block = re.search(r"set\(GW_EFUSE_OPTIONS(.*?)\n\n", text, re.DOTALL).group(1)
        options = re.findall(r"^\s+([A-Z0-9_]+)", block, re.MULTILINE)
        self.assertGreaterEqual(len(options), 10)
        self.assertEqual([o for o in options if o not in mm.REFUSED_OPTIONS], [])

    def test_other_project(self):
        self.b.project = "hello_world"
        self.b.files["rs485_spb_gateway.bin"] = app_image(project="hello_world")
        self.refused("the build is of the project 'hello_world'")

    def test_other_target(self):
        self.b.target = "esp32"
        self.refused("the build is for the target 'esp32'")

    def test_other_chip_in_flasher_args(self):
        self.b.args["extra_esptool_args"]["chip"] = "esp32c3"
        self.refused("chip 'esp32c3'")

    def test_sdkconfig_of_another_target(self):
        self.b.sdkconfig = self.b.sdkconfig.replace('"esp32s3"', '"esp32"')
        self.refused("it is not the sdkconfig of the build")

    def test_image_of_another_chip(self):
        self.b.files["bootloader/bootloader.bin"] = image(chip_id=5, body=b"\x11" * 64)
        self.refused("the part bootloader is an image for the chip ID 5")

    def test_not_an_image(self):
        self.b.files["rs485_spb_gateway.bin"] = b"\0" * 4096
        self.refused("the part app is not an image")

    def test_application_of_another_version(self):
        self.b.files["rs485_spb_gateway.bin"] = app_image(version="0.0.9")
        self.refused("the application is 'rs485_spb_gateway' '0.0.9'")

    def test_encrypted_part(self):
        self.b.args["app"]["encrypted"] = "true"
        self.refused("the part app is for an encrypted flash")

    def test_missing_files(self):
        self.b.write()
        (self.b.dir / "ota_data_initial.bin").unlink()
        with self.assertRaises(mm.ManifestError) as cm:
            self.b.plan()
        self.assertIn("cannot read", str(cm.exception))
        (self.b.dir / "flasher_args.json").unlink()
        with self.assertRaises(mm.ManifestError) as cm:
            self.b.plan()
        self.assertIn("flasher_args.json", str(cm.exception))

    def test_build_without_otadata(self):
        del self.b.args["otadata"]
        self.refused("flasher_args.json has no part 'otadata'")


class Places(Case):
    def test_application_not_at_ota_0(self):
        self.b.args["app"]["offset"] = "0x10000"
        self.refused("the build puts the part app at 0x10000, and the partition ota_0 is at "
                     "0x20000")

    def test_otadata_not_at_its_partition(self):
        self.b.args["otadata"]["offset"] = "0xd000"
        self.refused("the build puts the part otadata at 0xd000, and the partition otadata is "
                     "at 0xf000")

    def test_bootloader_not_at_the_offset_of_the_sdkconfig(self):
        self.b.args["bootloader"]["offset"] = "0x1000"
        self.refused("the build puts the part bootloader at 0x1000, and "
                     "CONFIG_BOOTLOADER_OFFSET_IN_FLASH is 0x0")

    def test_partition_table_not_at_the_offset_of_the_sdkconfig(self):
        self.b.sdkconfig = self.b.sdkconfig.replace("OFFSET=0x8000", "OFFSET=0xc000")
        self.refused("the build puts the part partition-table at 0x8000, and "
                     "CONFIG_PARTITION_TABLE_OFFSET is 0xc000")

    def test_sdkconfig_without_the_offsets(self):
        self.b.sdkconfig = 'CONFIG_IDF_TARGET="esp32s3"\n'
        problems = self.refused("has no CONFIG_BOOTLOADER_OFFSET_IN_FLASH")
        self.assertTrue(any("has no CONFIG_PARTITION_TABLE_OFFSET" in p for p in problems))

    def test_application_larger_than_its_slot(self):
        self.b.files["rs485_spb_gateway.bin"] = app_image(pad=0x400000)
        problems = self.refused("and the partition ota_0 has only 4194304")
        self.assertTrue(any("overlaps the partition ota_1, which must stay" in p
                            for p in problems), problems)

    def test_bootloader_overlaps_the_partition_table_and_nvs(self):
        self.b.files["bootloader/bootloader.bin"] = image(body=b"\x11" * 0x9800)
        problems = self.refused("the parts bootloader and partition-table overlap")
        self.assertTrue(any("the part bootloader" in p and "overlaps the partition nvs" in p
                            for p in problems), problems)

    def test_partition_table_of_the_build_is_not_the_source(self):
        for change in ({"offset": 0x30000}, {"size": 0x300000}, {"name": "app0"},
                       {"subtype": 0x00}):
            with self.subTest(change=change):
                self.b = FakeBuild(self.root)
                rows = [dict(r) for r in self.b.rows]
                rows[3].update(change)
                self.b.files["partition_table/partition-table.bin"] = table_bin(rows)
                self.refused("the partition table of the build is not the table of "
                             "partitions.csv")

    def test_partition_table_without_a_partition_of_the_source(self):
        self.b.files["partition_table/partition-table.bin"] = table_bin(self.b.rows[:-1])
        self.refused("Only in the source: coredump")

    def test_not_a_partition_table(self):
        self.b.files["partition_table/partition-table.bin"] = b"\x00" * 0xC00
        self.refused("is not a partition")

    def test_source_table_without_ota_0(self):
        csv = self.root / "single.csv"
        csv.write_text("nvs, data, nvs, 0x9000, 0x6000\notadata, data, ota, 0xf000, 0x2000\n"
                       "factory, app, factory, 0x20000, 1M\n")
        self.refused("does not have one partition of the type app, ota_0", partitions=csv)

    def test_source_table_moves_nvs_below_a_part(self):
        """The table of the source and of the build agree, and a part still writes nvs."""
        csv = self.root / "moved.csv"
        csv.write_text(PARTITIONS.read_text().replace("0x9000,   0x6000", "0x8000,   0x7000"))
        self.b.rows = mm.read_partitions_csv(csv)
        self.refused("the part partition-table (0x8000, 3072 bytes) overlaps the partition "
                     "nvs, which must stay", partitions=csv)

    def test_partition_after_the_end_of_the_flash(self):
        self.b.args["flash_settings"]["flash_size"] = "8MB"
        self.refused("after the end of the flash (8MB)")

    def test_otadata_not_blank(self):
        self.b.files["ota_data_initial.bin"] = b"\x01" + b"\xff" * 0x1FFF
        self.refused("the part otadata is not blank")

    def test_build_flashes_a_file_that_the_script_does_not_know(self):
        self.b.args["flash_files"]["0x11000"] = "phy_init_data.bin"
        self.refused("the build also writes phy_init_data.bin at 0x11000 (partition phy_init)")

    def test_storage_image_is_left_out_on_purpose(self):
        self.assertIn("storage", mm.LEFT_OUT)
        manifest, _ = self.b.write().plan()
        self.assertNotIn(0x820000, [p["offset"] for p in manifest["parts"]])


class Tag(Case):
    def test_check_tag(self):
        self.assertEqual(mm.check_tag("v0.1.0", "0.1.0", "x"), [])
        self.assertEqual(mm.check_tag("v0.12.3", "0.12.3", "x"), [])
        for tag in ("v1.0", "v1.0.0", "0.1.0", "v0.1", "v0.1.0-rc1", "v0.01.0", "v0.1.0.1",
                    "V0.1.0", "v0.1.x", ""):
            with self.subTest(tag=tag):
                problems = mm.check_tag(tag, tag[1:], "x")
                self.assertEqual(len(problems), 1)
                self.assertIn("is not a release tag", problems[0])
        self.assertIn("does not agree with the version 0.1.0",
                      mm.check_tag("v0.2.0", "0.1.0", "x")[0])

    def test_source_version_of_the_repository(self):
        """idf/CMakeLists.txt gives a version that a tag v0.<minor>.<patch> can have."""
        version = mm.source_version(IDF_DIR / "CMakeLists.txt")
        self.assertEqual(mm.check_tag("v" + version, version, "x"), [])

    def cmakelists(self, version):
        path = self.root / "CMakeLists.txt"
        path.write_text(f'cmake_minimum_required(VERSION 3.22)\nset(GW_VERSION "{version}")\n')
        return path

    def test_release_build_with_its_tag(self):
        manifest, _ = self.b.write().plan(tag="v0.1.0", cmakelists=self.cmakelists("0.1.0"))
        self.assertEqual(manifest["version"], "0.1.0")

    def test_dry_run_of_a_release_build(self):
        self.b.write().plan(release=True, cmakelists=self.cmakelists("0.1.0"))

    def test_tag_of_another_version(self):
        self.refused("the tag v0.2.0 does not agree with the version 0.1.0",
                     tag="v0.2.0", cmakelists=self.cmakelists("0.1.0"))

    def test_tag_v1_is_reserved(self):
        self.b = FakeBuild(self.root, version="1.0")
        self.refused("the tag 'v1.0' is not a release tag", tag="v1.0",
                     cmakelists=self.cmakelists("1.0"))

    def test_build_that_is_not_a_release_build(self):
        self.b = FakeBuild(self.root, version="0.1.0+g0123abcd")
        for kw in ({"tag": "v0.1.0"}, {"release": True}):
            with self.subTest(kw=kw):
                self.refused("the build has the version 0.1.0+g0123abcd, and a release of "
                             "this source has the version 0.1.0: build the release with "
                             "-D GW_RELEASE=ON", cmakelists=self.cmakelists("0.1.0"), **kw)

    def test_release_build_of_another_source_version(self):
        self.refused("the build has the version 0.1.0, and a release of this source has the "
                     "version 0.2.0", release=True, cmakelists=self.cmakelists("0.2.0"))

    def test_source_version_that_cannot_have_a_tag(self):
        self.b = FakeBuild(self.root, version="1.2.0")
        self.refused("the tag 'v1.2.0' is not a release tag", release=True,
                     cmakelists=self.cmakelists("1.2.0"))

    def test_tag_only(self):
        cm = str(self.cmakelists("0.3.1"))
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(mm.main(["--tag", "v0.3.1", "--tag-only", "--cmakelists", cm]), 0)
            self.assertEqual(mm.main(["--tag", "v0.3.2", "--tag-only", "--cmakelists", cm]), 1)
            self.assertEqual(mm.main(["--tag", "v1.0", "--tag-only", "--cmakelists", cm]), 1)
        self.assertIn("the tag v0.3.1 agrees with GW_VERSION 0.3.1", out.getvalue())
        self.assertIn("the tag v0.3.2 does not agree with the version 0.3.1", err.getvalue())
        self.assertIn("the tag 'v1.0' is not a release tag", err.getvalue())

    def test_cmakelists_without_the_version(self):
        path = self.root / "CMakeLists.txt"
        path.write_text("project(x)\n")
        with self.assertRaises(mm.ManifestError):
            mm.source_version(path)


class Parsers(unittest.TestCase):
    def test_partitions_csv_of_the_repository(self):
        rows = mm.read_partitions_csv(PARTITIONS)
        self.assertEqual(mm.find_partition(rows, "app", "ota_0")["name"], "ota_0")
        self.assertEqual(mm.find_partition(rows, "data", "ota")["name"], "otadata")
        self.assertEqual(mm.read_partitions_bin(table_bin(rows), "t"), rows)

    def test_numbers(self):
        self.assertEqual(mm.number("0x20000", "n"), 0x20000)
        self.assertEqual(mm.number("4096", "n"), 4096)
        self.assertEqual(mm.number("24K", "n"), 24 * 1024)
        self.assertEqual(mm.number("4M", "n"), 4 * 1024 * 1024)
        with self.assertRaises(mm.ManifestError):
            mm.number("big", "n")

    def test_csv_row_without_an_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            csv = pathlib.Path(tmp) / "auto.csv"
            csv.write_text("nvs, data, nvs, , 0x6000\n")
            with self.assertRaises(mm.ManifestError) as cm:
                mm.read_partitions_csv(csv)
            self.assertIn("auto.csv line 1", str(cm.exception))

    def test_overlap_counts_full_sectors(self):
        self.assertFalse(mm.overlap((0x0, 0x8000), (0x8000, 0xC00)))
        self.assertTrue(mm.overlap((0x0, 0x8001), (0x8000, 0xC00)))
        self.assertTrue(mm.overlap((0x8000, 0xC00), (0x8C00, 0x400)))
        self.assertFalse(mm.overlap((0x8000, 0xC00), (0x9000, 0x6000)))


if __name__ == "__main__":
    unittest.main()
