// Issue #219: the flash of a release with esptool-js (the copy of the site, no script from another
// server). The page downloads the four parts of the manifest from the site, and the browser and
// the page examine the SHA-256 of each one before the port changes hands. Then esptool-js puts
// the chip in its download mode, erases the flash (Install only), writes the parts at their
// offsets, examines each part with MD5, and restarts the chip.

import { integrityOf, hexOf } from "./releases.js";
import { md5Hex } from "./md5.js";

const ESPTOOL = new URL("./vendor/esptool-js-0.7.1/esptool.js", import.meta.url).href;
export const FLASH_BAUD = 460800;   // the baud rate of idf.py flash; the ROM starts at 115200

// stage: "download" (the images), "connect" (the download mode of the chip), "chip" (not an
// ESP32-S3), "erase", "write" or "restart". From "erase" on, the flash of the board changed.
export class FlashError extends Error {
    constructor(stage, detail) {
        super(`${stage}: ${detail instanceof Error ? detail.message : detail}`);
        this.stage = stage;
        this.detail = detail instanceof Error ? detail.message : String(detail);
    }
}

// Download the parts of the manifest at manifestUrl. Returns [{part, data}] in the order of the
// manifest (by offset).
export async function fetchParts(manifestUrl, manifest, onPart = () => {}) {
    const out = [];
    for (const part of manifest.parts) {
        onPart(part);
        const url = new URL(part.file, manifestUrl);
        let res;
        try {
            res = await fetch(url, { integrity: integrityOf(part.sha256), cache: "no-cache" });
        } catch (e) {
            // A wrong SHA-256 also comes here: the browser refuses the file (Subresource Integrity).
            throw new FlashError("download", `${part.file}: ${e.message}`);
        }
        if (!res.ok) {
            throw new FlashError("download", `${part.file}: HTTP ${res.status}`);
        }
        const buf = await res.arrayBuffer();
        if (buf.byteLength !== part.size) {
            throw new FlashError("download", `${part.file}: ${buf.byteLength} bytes, the manifest gives ${part.size}`);
        }
        const sha = hexOf(await crypto.subtle.digest("SHA-256", buf));
        if (sha !== part.sha256) {
            throw new FlashError("download", `${part.file}: the SHA-256 is not the SHA-256 of the manifest`);
        }
        out.push({ part, data: new Uint8Array(buf) });
    }
    return out;
}

// Write the parts. erase: Install (erase all the flash first). The port must be closed: esptool-js
// opens it. onStep(stage), onProgress(0..1), log(text).
export async function flashParts({ port, parts, erase, onStep, onProgress, log }) {
    const { ESPLoader, Transport } = await import(ESPTOOL);
    const transport = new Transport(port, false);
    const terminal = { clean() {}, writeLine: (t) => log(t + "\n"), write: (t) => log(t) };
    const loader = new ESPLoader({ transport, baudrate: FLASH_BAUD, romBaudrate: 115200, terminal });
    let stage = "connect";
    try {
        onStep(stage);
        await loader.main("default_reset");
        const chip = loader.chip && loader.chip.CHIP_NAME;
        if (chip !== "ESP32-S3") {
            stage = "chip";
            throw new FlashError("chip", chip || "unknown");
        }
        if (erase) {
            stage = "erase";
            onStep(stage);
            await loader.eraseFlash();
        }
        stage = "write";
        onStep(stage);
        const sizes = parts.map((p) => p.data.length);
        const total = sizes.reduce((a, b) => a + b, 0);
        await loader.writeFlash({
            fileArray: parts.map((p) => ({ data: p.data, address: p.part.offset })),
            flashMode: "keep", flashFreq: "keep", flashSize: "keep",
            eraseAll: false, compress: true,
            reportProgress: (i, written, size) => {
                const before = sizes.slice(0, i).reduce((a, b) => a + b, 0);
                onProgress(Math.min(1, (before + (size > 0 ? written / size : 1) * sizes[i]) / total));
            },
            calculateMD5Hash: md5Hex,
        });
        stage = "restart";
        onStep(stage);
        await loader.after("hard_reset");
    } catch (e) {
        throw e instanceof FlashError ? e : new FlashError(stage, e);
    } finally {
        await transport.disconnect().catch(() => {});
    }
}
