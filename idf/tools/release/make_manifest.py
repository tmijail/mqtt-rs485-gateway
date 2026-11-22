#!/usr/bin/env python3
"""Make the files of a firmware release from a build for the board (issue #279).

    python3 idf/tools/release/make_manifest.py --build-dir idf/build --out idf/build/release
    python3 idf/tools/release/make_manifest.py --build-dir idf/build --out release --tag v0.1.0
    python3 idf/tools/release/make_manifest.py --tag v0.1.0 --tag-only

The script reads flasher_args.json and project_description.json of the build, the sdkconfig of
the build, the partition table of the build and idf/partitions.csv. It writes the part files,
manifest.json and SHA256SUMS into the output directory. The format of the manifest is in
docs/reference/release-manifest.md.

A flash of the parts keeps the configuration and the secrets: the manifest has no part for nvs,
storage (the LittleFS image that "idf.py flash" writes), ota_1, phy_init and coredump.

A check that fails gives an error text and the exit status 1; the script then writes nothing.
Only the Python standard library.
"""
import argparse
import hashlib
import json
import pathlib
import re
import struct
import sys

FORMAT = 1                      # the format number of manifest.json
PROJECT = "rs485_spb_gateway"   # project() of idf/CMakeLists.txt
IDF_DIR = pathlib.Path(__file__).resolve().parents[2]

# target of the build -> (the chip name that esptool gives, the chip ID in an image header)
CHIPS = {"esp32s3": ("ESP32-S3", 9)}

# The parts of a release: (name in the manifest, key in flasher_args.json, (type, subtype) of the
# partition that the part fills; None: the part is not in a partition).
PARTS = (
    ("bootloader", "bootloader", None),
    ("partition-table", "partition-table", None),
    ("otadata", "otadata", ("data", "ota")),
    ("app", "app", ("app", "ota_0")),
)

# The partitions that the build writes with "idf.py flash" and that a release leaves out on
# purpose. storage: the LittleFS image of idf/fs/ would replace the configuration of the owner.
LEFT_OUT = ("storage",)

# The options that a build for the board never has. CONFIG_GW_IO_SIMULATED and
# CONFIG_GW_EMU_NO_PSRAM make a QEMU build. The others are the options of "Security constraints"
# in CLAUDE.md and of idf/test/host/check_efuse_options.cmake: each burns an eFuse on a real chip
# (test_make_manifest.py compares the two lists). The last one makes the gateway refuse an OTA
# image without the signature of one key (#133, #261).
REFUSED_OPTIONS = {
    "GW_IO_SIMULATED": "a QEMU build: it simulates the I/O",
    "GW_EMU_NO_PSRAM": "a QEMU build without PSRAM",
    "SECURE_BOOT": "Secure Boot burns the digest of a key into eFuse",
    "SECURE_FLASH_ENC_ENABLED": "flash encryption burns a key into eFuse",
    "SECURE_DISABLE_ROM_DL_MODE": "it disables the download mode for ever",
    "SECURE_ENABLE_SECURE_ROM_DL_MODE": "it limits the download mode for ever",
    "BOOTLOADER_APP_ANTI_ROLLBACK": "the anti-rollback burns a counter into eFuse",
    "BOOT_ROM_LOG_ALWAYS_OFF": "the log of the ROM is an eFuse",
    "BOOT_ROM_LOG_ON_GPIO_HIGH": "the log of the ROM is an eFuse",
    "BOOT_ROM_LOG_ON_GPIO_LOW": "the log of the ROM is an eFuse",
    "NVS_ENCRYPTION": "the NVS encryption burns an HMAC key into eFuse, or needs flash encryption",
    "NVS_SEC_KEY_PROTECT_USING_HMAC": "the HMAC scheme of the NVS encryption burns a key into eFuse",
    "SECURE_SIGNED_APPS_NO_SECURE_BOOT": "the build for the board accepts unsigned OTA images",
}

# The tag of a release: v0.<minor>.<patch>. The tag v1.0 is for the commit of the first installed
# gateway (CLAUDE.md), so no number other than 0 is permitted as the first one.
TAG_RE = re.compile(r"v0\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")

SECTOR = 0x1000                 # esptool erases full sectors of 4 KB
IMAGE_MAGIC = 0xE9              # esp_image_header_t.magic
APP_DESC_OFFSET = 0x20          # esp_app_desc_t: after the image header and one segment header
APP_DESC_MAGIC = 0xABCD5432
PT_ENTRY = struct.Struct("<2sBBII16sI")   # esp_partition_info_t
PT_MAGIC = b"\xaa\x50"
PT_MD5_MAGIC = b"\xeb\xeb"
PT_TYPES = {"app": 0x00, "data": 0x01}
PT_SUBTYPES = {
    0x00: {"factory": 0x00, "test": 0x20, **{f"ota_{i}": 0x10 + i for i in range(16)}},
    0x01: {"ota": 0x00, "phy": 0x01, "nvs": 0x02, "coredump": 0x03, "nvs_keys": 0x04,
           "efuse": 0x05, "undefined": 0x06, "esphttpd": 0x80, "fat": 0x81, "spiffs": 0x82,
           "littlefs": 0x83},
}


class ManifestError(Exception):
    """One or more checks failed: problems has one text for each."""

    def __init__(self, problems):
        self.problems = [problems] if isinstance(problems, str) else list(problems)
        super().__init__("\n".join(self.problems))


def number(text, what):
    """A number of a partition table or of flasher_args.json: decimal or 0x hex, with K or M."""
    t = str(text).strip().lower()
    scale = 1
    if t.endswith(("k", "m")):
        scale = 0x400 if t.endswith("k") else 0x100000
        t = t[:-1]
    try:
        return int(t, 0) * scale
    except ValueError:
        raise ManifestError(f"{what}: {text!r} is not a number") from None


def read_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except OSError as e:
        raise ManifestError(f"cannot read {path}: {e.strerror}") from None
    except ValueError as e:
        raise ManifestError(f"{path} is not JSON: {e}") from None


def read_bytes(path):
    try:
        return path.read_bytes()
    except OSError as e:
        raise ManifestError(f"cannot read {path}: {e.strerror}") from None


def read_sdkconfig(path):
    """The options of an sdkconfig file, without the CONFIG_ prefix. An option that is not set
    ("# CONFIG_X is not set") is not in the result."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as e:
        raise ManifestError(f"cannot read the sdkconfig {path}: {e.strerror}") from None
    options = {}
    for line in text.splitlines():
        m = re.fullmatch(r"CONFIG_([A-Za-z0-9_]+)=(.*)", line.strip())
        if m:
            options[m.group(1)] = m.group(2).strip('"')
    return options


def read_partitions_csv(path):
    """The rows of a partition table CSV: dicts with name, type, subtype, offset and size (the
    type and the subtype as numbers)."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as e:
        raise ManifestError(f"cannot read the partition table {path}: {e.strerror}") from None
    rows = []
    for n, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(",")]
        where = f"{path.name} line {n}"
        if len(f) < 5 or not f[0] or not f[3] or not f[4]:
            raise ManifestError(f"{where}: a row needs a name, a type, a subtype, an offset "
                                "and a size")
        ptype = PT_TYPES[f[1]] if f[1] in PT_TYPES else number(f[1], f"{where}, type")
        subtypes = PT_SUBTYPES.get(ptype, {})
        subtype = subtypes[f[2]] if f[2] in subtypes else number(f[2], f"{where}, subtype")
        rows.append({"name": f[0], "type": ptype, "subtype": subtype,
                     "offset": number(f[3], f"{where}, offset"),
                     "size": number(f[4], f"{where}, size")})
    if not rows:
        raise ManifestError(f"{path}: the partition table has no partition")
    return rows


def read_partitions_bin(data, what):
    """The rows of a partition table binary (partition-table.bin), as read_partitions_csv()."""
    rows = []
    for at in range(0, len(data) - PT_ENTRY.size + 1, PT_ENTRY.size):
        magic, ptype, subtype, offset, size, name, _flags = PT_ENTRY.unpack_from(data, at)
        if magic != PT_MAGIC:
            if magic not in (PT_MD5_MAGIC, b"\xff\xff"):
                raise ManifestError(f"{what}: the entry at {at:#x} is not a partition")
            break
        rows.append({"name": name.split(b"\0", 1)[0].decode("utf-8", "replace"), "type": ptype,
                     "subtype": subtype, "offset": offset, "size": size})
    if not rows:
        raise ManifestError(f"{what}: not a partition table (no partition)")
    return rows


def find_partition(table, ptype, subtype):
    code = PT_TYPES[ptype]
    sub = PT_SUBTYPES[code][subtype]
    found = [p for p in table if p["type"] == code and p["subtype"] == sub]
    return found[0] if len(found) == 1 else None


def source_version(cmakelists):
    """GW_VERSION of idf/CMakeLists.txt."""
    try:
        text = cmakelists.read_text(encoding="utf-8")
    except OSError as e:
        raise ManifestError(f"cannot read {cmakelists}: {e.strerror}") from None
    m = re.search(r'^set\(GW_VERSION "([^"]*)"\)', text, re.MULTILINE)
    if not m:
        raise ManifestError(f'{cmakelists}: no line set(GW_VERSION "...")')
    return m.group(1)


def check_tag(tag, version, origin):
    """The problems of a release tag for a firmware version (an empty list: none)."""
    if not TAG_RE.fullmatch(tag):
        return [f"the tag {tag!r} is not a release tag: a release has a tag v0.<minor>.<patch> "
                "(the tag v1.0 is for the commit of the first installed gateway)"]
    if tag != "v" + version:
        hint = (": build the release with -D GW_RELEASE=ON" if "+" in version
                else ": set GW_VERSION in idf/CMakeLists.txt, or make the tag v" + version)
        return [f"the tag {tag} does not agree with the version {version} of {origin}{hint}"]
    return []


def cstr(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def overlap(a, b):
    """True when two (offset, size) areas of the flash share a sector."""
    a0, a1 = a[0] // SECTOR, (a[0] + a[1] + SECTOR - 1) // SECTOR
    b0, b1 = b[0] // SECTOR, (b[0] + b[1] + SECTOR - 1) // SECTOR
    return a0 < b1 and b0 < a1


def plan(build_dir, partitions_csv, sdkconfig=None, tag=None, release=False,
         cmakelists=IDF_DIR / "CMakeLists.txt"):
    """Examine the build in @build_dir and give (manifest, sources): the manifest as a dict, and
    the source file of each part, in the sequence of manifest["parts"]. Raise ManifestError with
    each problem that the checks found."""
    build_dir = pathlib.Path(build_dir)
    args = read_json(build_dir / "flasher_args.json")
    desc = read_json(build_dir / "project_description.json")
    problems = []

    # the project, the chip and the version
    name = desc.get("project_name")
    if name != PROJECT:
        problems.append(f"the build is of the project {name!r}, not of {PROJECT!r}")
    target = desc.get("target")
    flash_chip = args.get("extra_esptool_args", {}).get("chip")
    if target not in CHIPS or flash_chip != target:
        raise ManifestError(problems + [
            f"the build is for the target {target!r} (flasher_args.json: chip {flash_chip!r}): "
            f"a release is for {', '.join(CHIPS)}"])
    chip, chip_id = CHIPS[target]
    version = desc.get("project_version")
    if not isinstance(version, str) or not version:
        raise ManifestError(problems + ["project_description.json gives no project_version"])
    if release or tag is not None:
        expected = source_version(pathlib.Path(cmakelists))
        problems += check_tag(tag if tag is not None else "v" + expected, expected,
                              "GW_VERSION in idf/CMakeLists.txt")
        if version != expected:
            hint = ": build the release with -D GW_RELEASE=ON" if "+" in version else ""
            problems.append(f"the build has the version {version}, and a release of this source "
                            f"has the version {expected}{hint}")

    # the build for the board: no option of a QEMU build, and none that burns an eFuse
    config_path = pathlib.Path(sdkconfig or desc.get("config_file") or "")
    if not config_path.name:
        raise ManifestError(problems + ["project_description.json gives no config_file: give "
                                        "the sdkconfig of the build with --sdkconfig"])
    config = read_sdkconfig(config_path)
    for option, reason in REFUSED_OPTIONS.items():
        if config.get(option) == "y":
            problems.append(f"{config_path.name} has CONFIG_{option}=y ({reason}): a release "
                            "has only the build for the board")
    if config.get("IDF_TARGET") != target:
        problems.append(f"{config_path.name} has CONFIG_IDF_TARGET={config.get('IDF_TARGET')!r}"
                        f", and the build is for {target!r}: it is not the sdkconfig of the build")

    # the partition table of the build is the table of the source
    table = read_partitions_csv(pathlib.Path(partitions_csv))
    seen = set()
    for p in table:
        if p["name"] in seen:
            problems.append(f"{pathlib.Path(partitions_csv).name}: two partitions have the name "
                            f"{p['name']!r}")
        seen.add(p["name"])
    settings = args.get("flash_settings", {})
    for key in ("flash_mode", "flash_freq", "flash_size"):
        if not isinstance(settings.get(key), str):
            raise ManifestError(problems + [f"flasher_args.json gives no flash_settings.{key}"])
    flash_size = number(settings["flash_size"].removesuffix("B"), "flash_settings.flash_size")
    for p in table:
        if p["offset"] + p["size"] > flash_size:
            problems.append(f"the partition {p['name']} ends at {p['offset'] + p['size']:#x}, "
                            f"after the end of the flash ({settings['flash_size']})")

    # the parts: the files and their places in the flash
    parts = []
    sources = []
    written = set()
    for part, key, fills in PARTS:
        entry = args.get(key)
        if not isinstance(entry, dict) or "offset" not in entry or "file" not in entry:
            raise ManifestError(problems + [f"flasher_args.json has no part {key!r}: the build "
                                            "does not make it"])
        if str(entry.get("encrypted", "false")).lower() != "false":
            problems.append(f"the part {part} is for an encrypted flash: a release has no "
                            "flash encryption")
        source = build_dir / entry["file"]
        data = read_bytes(source)
        offset = number(entry["offset"], f"flasher_args.json, offset of {key}")
        partition = None
        if fills is not None:
            partition = find_partition(table, *fills)
            if partition is None:
                raise ManifestError(problems + [
                    f"{pathlib.Path(partitions_csv).name} does not have one partition of the "
                    f"type {fills[0]}, {fills[1]} for the part {part}"])
            written.add(partition["name"])
            if offset != partition["offset"]:
                problems.append(
                    f"the build puts the part {part} at {offset:#x}, and the partition "
                    f"{partition['name']} is at {partition['offset']:#x}")
            if len(data) > partition["size"]:
                problems.append(
                    f"the part {part} has {len(data)} bytes, and the partition "
                    f"{partition['name']} has only {partition['size']}")
        parts.append({"name": part, "file": source.name, "offset": offset, "size": len(data),
                      "sha256": hashlib.sha256(data).hexdigest(),
                      "partition": partition["name"] if partition else None})
        sources.append((source, data))
    by_name = {p["name"]: (p, data) for p, (_, data) in zip(parts, sources, strict=True)}
    if len({p["file"] for p in parts}) != len(parts):
        problems.append("two parts have the same file name")

    # the bootloader and the partition table are not in a partition: the sdkconfig has their places
    for part, option in (("bootloader", "BOOTLOADER_OFFSET_IN_FLASH"),
                         ("partition-table", "PARTITION_TABLE_OFFSET")):
        if option not in config:
            problems.append(f"{config_path.name} has no CONFIG_{option}")
        elif by_name[part][0]["offset"] != number(config[option], f"CONFIG_{option}"):
            problems.append(f"the build puts the part {part} at {by_name[part][0]['offset']:#x}"
                            f", and CONFIG_{option} is {config[option]}")
    built = read_partitions_bin(by_name["partition-table"][1],
                                by_name["partition-table"][0]["file"])
    if built != table:
        def rows(a, b):
            return ", ".join(f"{p['name']} ({p['offset']:#x}, {p['size']:#x} bytes)"
                             for p in a if p not in b) or "none"
        problems.append(f"the partition table of the build is not the table of "
                        f"{pathlib.Path(partitions_csv).name}. Only in the build: "
                        f"{rows(built, table)}. Only in the source: {rows(table, built)}")

    # the two images have the header of the chip; the application is the image of this version
    for part in ("bootloader", "app"):
        data = by_name[part][1]
        if len(data) < 24 or data[0] != IMAGE_MAGIC:
            problems.append(f"the part {part} is not an image (no magic byte {IMAGE_MAGIC:#x})")
        elif struct.unpack_from("<H", data, 12)[0] != chip_id:
            problems.append(f"the part {part} is an image for the chip ID "
                            f"{struct.unpack_from('<H', data, 12)[0]}, not for {chip} ({chip_id})")
    app = by_name["app"][1]
    if (len(app) < APP_DESC_OFFSET + 80 or
            struct.unpack_from("<I", app, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC):
        problems.append("the application has no application description (esp_app_desc_t)")
    else:
        app_version = cstr(app[APP_DESC_OFFSET + 16:APP_DESC_OFFSET + 48])
        app_name = cstr(app[APP_DESC_OFFSET + 48:APP_DESC_OFFSET + 80])
        if app_name != str(name)[:31] or app_version != version[:31]:
            problems.append(f"the application is {app_name!r} {app_version!r}, and the build "
                            f"data gives {name!r} {version!r}: build again")
    # a blank otadata: the bootloader then starts ota_0, where the application is
    if by_name["otadata"][1].strip(b"\xff"):
        problems.append("the part otadata is not blank (all 0xFF): the boot must go to ota_0")

    # no part writes a sector of another part or of a partition that must stay
    keep = [p for p in table if p["name"] not in written]
    for i, a in enumerate(parts):
        if a["offset"] + a["size"] > flash_size:
            problems.append(f"the part {a['name']} ends after the end of the flash")
        for b in parts[i + 1:]:
            if overlap((a["offset"], a["size"]), (b["offset"], b["size"])):
                problems.append(f"the parts {a['name']} and {b['name']} overlap")
        for p in keep:
            if overlap((a["offset"], a["size"]), (p["offset"], p["size"])):
                problems.append(f"the part {a['name']} ({a['offset']:#x}, {a['size']} bytes) "
                                f"overlaps the partition {p['name']}, which must stay")

    # each other file that "idf.py flash" writes is one that the release leaves out on purpose
    part_files = {str(args[key]["file"]) for _, key, _ in PARTS}
    for where, file in args.get("flash_files", {}).items():
        if file in part_files:
            continue
        at = number(where, "flasher_args.json, flash_files")
        owner = next((p["name"] for p in table if p["offset"] == at), None)
        if owner not in LEFT_OUT:
            problems.append(f"the build also writes {file} at {at:#x} (partition {owner}): "
                            "add it to PARTS or to LEFT_OUT in make_manifest.py")
    for must in LEFT_OUT:
        if must not in {p["name"] for p in keep}:
            problems.append(f"the partition table has no partition {must} that stays")

    if problems:
        raise ManifestError(problems)
    manifest = {
        "format": FORMAT,
        "name": name,
        "version": version,
        "chip": chip,
        "flash": {"mode": settings["flash_mode"], "freq": settings["flash_freq"],
                  "size": settings["flash_size"]},
        "parts": parts,
        "keep": [{"name": p["name"], "offset": p["offset"], "size": p["size"]} for p in keep],
    }
    return manifest, [source for source, _ in sources]


def write_release(manifest, sources, out_dir):
    """Write the part files, manifest.json and SHA256SUMS into @out_dir."""
    out_dir = pathlib.Path(out_dir)
    names = [p["file"] for p in manifest["parts"]] + ["manifest.json", "SHA256SUMS"]
    if out_dir.exists():
        other = sorted(x.name for x in out_dir.iterdir() if x.name not in names)
        if other:
            raise ManifestError(f"the output directory {out_dir} has other files "
                                f"({', '.join(other)}): give an empty directory")
    out_dir.mkdir(parents=True, exist_ok=True)
    sums = []
    for part, source in zip(manifest["parts"], sources, strict=True):
        data = read_bytes(pathlib.Path(source))
        if hashlib.sha256(data).hexdigest() != part["sha256"]:
            raise ManifestError(f"{source} changed while the script examined the build")
        (out_dir / part["file"]).write_bytes(data)
        sums.append((part["sha256"], part["file"]))
    text = json.dumps(manifest, indent=2) + "\n"
    (out_dir / "manifest.json").write_text(text, encoding="utf-8")
    sums.append((hashlib.sha256(text.encode("utf-8")).hexdigest(), "manifest.json"))
    # the format of sha256sum: "sha256sum -c SHA256SUMS" examines the files
    (out_dir / "SHA256SUMS").write_text("".join(f"{s}  {n}\n" for s, n in sums),
                                        encoding="utf-8")


def summary(manifest):
    """A Markdown table of the release, for the output and the job summary."""
    lines = [f"### {manifest['name']} {manifest['version']} for {manifest['chip']}", "",
             "| Part | File | Offset | Size (bytes) | SHA-256 |", "|---|---|---|---|---|"]
    lines += [f"| {p['name']} | `{p['file']}` | `{p['offset']:#x}` | {p['size']} | "
              f"`{p['sha256']}` |" for p in manifest["parts"]]
    lines += ["", "The release does not write: " +
              ", ".join(f"`{p['name']}`" for p in manifest["keep"]) + "."]
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--build-dir", type=pathlib.Path,
                    help="the build directory of the build for the board (idf/build)")
    ap.add_argument("--out", type=pathlib.Path, help="the output directory")
    ap.add_argument("--partitions", type=pathlib.Path, default=IDF_DIR / "partitions.csv",
                    help="the partition table of the source (default: idf/partitions.csv)")
    ap.add_argument("--sdkconfig", type=pathlib.Path,
                    help="the sdkconfig of the build (default: config_file of "
                         "project_description.json)")
    ap.add_argument("--cmakelists", type=pathlib.Path, default=IDF_DIR / "CMakeLists.txt",
                    help="the file that sets GW_VERSION (default: idf/CMakeLists.txt)")
    ap.add_argument("--release", action="store_true",
                    help="the build must be a release build: its version is GW_VERSION, and "
                         "v<GW_VERSION> is a permitted tag")
    ap.add_argument("--tag", help="the tag of the release: it must be v<GW_VERSION>, and the "
                                  "build must be a release build")
    ap.add_argument("--tag-only", action="store_true",
                    help="examine only --tag against GW_VERSION, without a build")
    a = ap.parse_args(argv)
    try:
        if a.tag_only:
            if a.tag is None:
                ap.error("--tag-only needs --tag")
            version = source_version(a.cmakelists)
            problems = check_tag(a.tag, version, "GW_VERSION in idf/CMakeLists.txt")
            if problems:
                raise ManifestError(problems)
            print(f"the tag {a.tag} agrees with GW_VERSION {version}")
            return 0
        if a.build_dir is None or a.out is None:
            ap.error("--build-dir and --out are necessary")
        manifest, sources = plan(a.build_dir, a.partitions, sdkconfig=a.sdkconfig, tag=a.tag,
                                 release=a.release, cmakelists=a.cmakelists)
        write_release(manifest, sources, a.out)
    except ManifestError as e:
        for problem in e.problems:
            print(f"make_manifest: error: {problem}", file=sys.stderr)
        return 1
    print(summary(manifest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
