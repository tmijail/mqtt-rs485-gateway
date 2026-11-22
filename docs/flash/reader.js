// Issue #219: the reader of the board. It listens to the console of the board, asks for the
// access data (the shell command 'access' of #280) and decides what the panel shows. Pure: the
// caller gives the text of the port with feed() and the time with tick(). The unit test is
// idf/test/web/test_flash_tool.mjs.
//
// The rules of the prototype (#219, direction C):
// - No start log of this firmware: "no answer" after QUIET ms.
// - A start log of this firmware: wait for the answer up to START ms after the first line of it.
// - A board in the download mode of the ROM answers never: "no answer" at once.
// - After "no answer", ask again each POLL ms. A late answer shows the panel.
// - A new start log (a restart) makes the reader read the board again, with no click.

import { AccessCollector, classify, cleanLine } from "./access.js";

export const TIMES = {
    FIRST_ASK: 300,    // the first 'access' after the port opens
    ASK: 1000,         // while the reader reads: ask again (the shell can start late)
    QUIET: 5000,       // no start log of this firmware: "no answer" after this time
    START: 30000,      // a start log of this firmware: the longest wait for a final answer
    POLL: 5000,        // "no answer", "no address", "web UI off", "starts": ask again
};

// Lines that only this firmware writes at the start.
const OURS = [
    /Project name:\s+rs485_spb_gateway\b/,
    /\bmain: rs485_spb_gateway\b/,
    /^gw> /,
    /\bshell: console ready\b/,
];
// The first lines of a start: the ROM of the ESP32-S3 writes them at each reset.
const RESTART = [/^ESP-ROM:/, /^rst:0x[0-9a-f]+ \(/];
const DOWNLOAD = /waiting for download/;

export class BoardReader {
    // send(text): writes to the port. onChange(reader): the phase or the data changed.
    // onLine(line): each full line of the console, without the colors.
    constructor({ send, onChange = () => {}, onLine = () => {}, times = TIMES }) {
        this.send = send;
        this.onChange = onChange;
        this.onLine = onLine;
        this.t = times;
        this.partial = "";
        this.reset(0);
    }

    reset(now) {
        this.phase = "reading";      // reading | noanswer | answered
        this.log = "none";           // none | other | ours | download
        this.t0 = now;
        this.oursAt = null;
        this.nextAsk = now + this.t.FIRST_ASK;
        this.data = null;            // the last full answer
        this.state = null;           // classify(data)
        this.reason = null;          // the reason of "no answer"
        this.late = false;           // the answer came after "no answer"
        this.collector = new AccessCollector();
        this.asking = true;
    }

    // Read the board again from the start (the button "Retry" or "Read again", or a restart).
    restart(now) {
        this.reset(now);
        this.onChange(this);
    }

    // Stop or start the asks (the console is busy, a flash has the port).
    setAsking(on, now) {
        this.asking = on;
        if (on) {
            this.nextAsk = now + this.t.FIRST_ASK;
        }
    }

    feed(text, now) {
        const parts = (this.partial + text).split("\n");
        this.partial = parts.pop();
        if (this.partial.length > 4096) {   // a line without an end: drop it
            this.partial = "";
        }
        for (const raw of parts) {
            this.line(cleanLine(raw), now);
        }
        // The prompt has no line end: examine it at once.
        if (/^gw> $/.test(cleanLine(this.partial))) {
            this.seenOurs(now);
        }
    }

    seenOurs(now) {
        if (this.log !== "ours") {
            this.log = "ours";
            this.oursAt = now;
            if (this.phase === "reading") {
                this.onChange(this);
            }
        }
    }

    line(line, now) {
        this.onLine(line);
        if (RESTART.some((r) => r.test(line)) && this.phase !== "reading") {
            this.restart(now);   // the board restarted: read it again, with no click
        }
        if (DOWNLOAD.test(line)) {
            this.log = "download";
            if (this.phase === "reading") {
                this.verdict("download");
            }
            return;
        }
        if (OURS.some((r) => r.test(line))) {
            this.seenOurs(now);
        } else if (this.log === "none" && line.trim() !== "") {
            this.log = "other";
            if (this.phase === "reading") {
                this.onChange(this);
            }
        }
        const r = this.collector.push(line);
        if (r !== null) {
            this.answer(r, now);
        }
    }

    answer(r, now) {
        if (!r.ok) {
            if (this.phase === "reading") {
                this.verdict(r.error === "version" ? "format" : "garbled");
            }
            return;
        }
        this.seenOurs(now);
        this.data = r.data;
        this.state = classify(r.data);
        if (this.state === "starting" && this.phase === "reading") {
            this.onChange(this);
            return;   // ask again until the start is complete
        }
        if (this.phase === "noanswer") {
            this.late = true;
        }
        this.phase = "answered";
        this.nextAsk = now + this.t.POLL;
        this.onChange(this);
    }

    verdict(reason) {
        this.phase = "noanswer";
        this.reason = reason;
        this.onChange(this);
    }

    tick(now) {
        if (this.phase === "reading") {
            if (this.log === "ours") {
                if (now - this.oursAt >= this.t.START) {
                    if (this.data !== null) {        // it answered, but its start is not complete
                        this.phase = "answered";
                        this.nextAsk = now + this.t.POLL;
                        this.onChange(this);
                    } else {
                        this.verdict("ours-silent");
                    }
                    return;
                }
            } else if (now - this.t0 >= this.t.QUIET) {
                this.verdict(this.log === "other" ? "other" : "silent");
                return;
            }
        }
        if (!this.asking || now < this.nextAsk) {
            return;
        }
        if (this.phase === "answered" && !["starting", "noaddr", "weboff"].includes(this.state)) {
            return;   // a final state: no asks, the board writes nothing to the console for them
        }
        this.send("access\r");
        this.nextAsk = now + (this.phase === "reading" ? this.t.ASK : this.t.POLL);
    }

    // 0 to 1: the part of the longest wait that is gone, for the bar of the panel.
    progress(now) {
        if (this.phase !== "reading") {
            return 1;
        }
        if (this.log === "ours") {
            return Math.min(1, (now - this.oursAt) / this.t.START);
        }
        return Math.min(1, (now - this.t0) / this.t.QUIET);
    }
}
