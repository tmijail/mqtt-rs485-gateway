// Issue #219: the releases on the documentation site. The workflow Docs copies the images of each
// release v0.x.y to firmware/<tag>/ and writes firmware/index.json (#219, the copy of the
// releases; the format is in docs/reference/release-manifest.md). Pure functions: the unit test is
// idf/test/web/test_flash_tool.mjs.

const TAG = /^v0\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;
const PARTS = ["bootloader", "partition-table", "otadata", "app"];
const SECTOR = 4096;

// "0.2.0" or "0.2.0+g1a2b3c4d.dirty" -> {major, minor, patch, build}; null for another text.
export function parseVersion(text) {
    const m = /^(\d+)\.(\d+)\.(\d+)(?:\+(\S*))?$/.exec(String(text || "").trim());
    if (!m) {
        return null;
    }
    return { major: Number(m[1]), minor: Number(m[2]), patch: Number(m[3]), build: m[4] || null };
}

// <0, 0 or >0. The build part (the commit hash) does not count.
export function compareVersions(a, b) {
    const x = parseVersion(a);
    const y = parseVersion(b);
    if (!x || !y) {
        return 0;
    }
    return x.major - y.major || x.minor - y.minor || x.patch - y.patch;
}

function plainRelative(path) {
    return typeof path === "string" && path !== "" && !path.startsWith("/") &&
        !/^[a-z][a-z0-9+.-]*:/i.test(path) && !path.split("/").includes("..");
}

// index.json -> {releases: [...newest first]} or {error}.
export function readIndex(json) {
    if (!json || typeof json !== "object" || json.format !== 1 || !Array.isArray(json.releases)) {
        return { error: "The list of the releases has a format that this page does not know." };
    }
    const releases = [];
    for (const r of json.releases) {
        if (!r || !TAG.test(r.tag) || r.version !== r.tag.slice(1) || !plainRelative(r.manifest)) {
            continue;   // never offer a release that the copy step did not write
        }
        releases.push({ tag: r.tag, version: r.version, published: r.published || null,
            prerelease: r.prerelease !== false, manifest: r.manifest });
    }
    releases.sort((a, b) => compareVersions(b.version, a.version));
    return { releases };
}

// manifest.json (docs/reference/release-manifest.md, format 1) -> {manifest} or {errors: [...]}.
export function checkManifest(m, version) {
    const errors = [];
    if (!m || typeof m !== "object") {
        return { errors: ["The manifest is not a JSON object."] };
    }
    if (m.format !== 1) {
        return { errors: [`The manifest has the format ${m.format}. This page knows only format 1.`] };
    }
    if (m.name !== "rs485_spb_gateway") {
        errors.push(`The manifest is for the firmware "${m.name}", not rs485_spb_gateway.`);
    }
    if (m.chip !== "ESP32-S3") {
        errors.push(`The manifest is for the chip ${m.chip}, not ESP32-S3.`);
    }
    if (version !== undefined && m.version !== version) {
        errors.push(`The manifest has the version ${m.version}, not ${version}.`);
    }
    const parts = Array.isArray(m.parts) ? m.parts : [];
    const names = parts.map((p) => p && p.name);
    if (names.length !== PARTS.length || PARTS.some((n) => !names.includes(n))) {
        errors.push("The manifest does not have the four parts bootloader, partition-table, otadata and app.");
    }
    const areas = [];
    for (const p of parts) {
        if (!p || typeof p.file !== "string" || !/^[A-Za-z0-9._-]+$/.test(p.file) || p.file.startsWith(".")) {
            errors.push("A part of the manifest has a file name that is not a plain name.");
            continue;
        }
        if (!Number.isInteger(p.offset) || p.offset < 0 || p.offset % SECTOR !== 0) {
            errors.push(`The part ${p.name} has an offset that is not at the start of a flash sector.`);
        }
        if (!Number.isInteger(p.size) || p.size <= 0) {
            errors.push(`The part ${p.name} has no size.`);
        }
        if (typeof p.sha256 !== "string" || !/^[0-9a-f]{64}$/.test(p.sha256)) {
            errors.push(`The part ${p.name} has no SHA-256.`);
        }
        areas.push({ name: p.name, start: p.offset, end: p.offset + p.size });
    }
    for (const k of Array.isArray(m.keep) ? m.keep : []) {
        if (k && Number.isInteger(k.offset) && Number.isInteger(k.size)) {
            areas.push({ name: k.name, start: k.offset, end: k.offset + k.size, keep: true });
        }
    }
    for (let i = 0; i < areas.length; i++) {
        for (let j = i + 1; j < areas.length; j++) {
            const a = areas[i];
            const b = areas[j];
            if (!(a.keep && b.keep) && a.start < b.end && b.start < a.end) {
                errors.push(`The part ${a.keep ? b.name : a.name} writes into ${a.keep ? a.name : b.name}.`);
            }
        }
    }
    if (errors.length > 0) {
        return { errors };
    }
    const sorted = [...parts].sort((a, b) => a.offset - b.offset);
    return { manifest: { ...m, parts: sorted } };
}

// The value of the option "integrity" of fetch() for a SHA-256 in hexadecimal.
export function integrityOf(hex) {
    let bin = "";
    for (let i = 0; i < hex.length; i += 2) {
        bin += String.fromCharCode(parseInt(hex.slice(i, i + 2), 16));
    }
    return "sha256-" + btoa(bin);
}

export function hexOf(buffer) {
    return Array.from(new Uint8Array(buffer), (b) => b.toString(16).padStart(2, "0")).join("");
}

// The esptool command for the files of a manifest (the tab Files, and a browser with no serial port).
export function esptoolCommand(m) {
    const f = m.flash || {};
    const opts = [f.mode && `--flash-mode ${f.mode}`, f.freq && `--flash-freq ${f.freq}`,
        f.size && `--flash-size ${f.size}`].filter(Boolean).join(" ");
    const files = m.parts.map((p) => `0x${p.offset.toString(16)} ${p.file}`).join(" ");
    return `python -m esptool --chip esp32s3 write-flash ${opts} ${files}`.replace(/\s+/g, " ");
}

// What the panel tells about the firmware of the board against the last release.
export function firmwareNote(firmware, latest) {
    const v = parseVersion(firmware);
    if (!v) {
        return "This page does not know this version.";
    }
    if (v.build !== null) {
        return "A build that is not a release.";
    }
    if (!latest) {
        return "";
    }
    const c = compareVersions(firmware, latest.version);
    if (c === 0) {
        return "This is the last release.";
    }
    return c < 0 ? `The last release is ${latest.version}.` : "This version is newer than the last release on this site.";
}

// A repair with a release that is older than the firmware on the board can be unable to read the
// stored configuration (no pre-flight check over USB, docs/reference/firmware-updates.md).
export function olderThanBoard(release, firmware) {
    return parseVersion(firmware) !== null && compareVersions(release.version, firmware) < 0;
}
