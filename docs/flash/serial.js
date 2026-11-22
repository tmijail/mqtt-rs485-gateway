// Issue #219: the serial port of the board, with Web Serial. The console of the gateway is UART0
// at 115200 baud (CONFIG_ESP_CONSOLE_UART_BAUDRATE). The page does not change DTR and RTS: on a
// board with the usual automatic reset, a change can restart the gateway.

export const BAUD = 115200;

export class SerialLink {
    // onText(text): text from the board. onEnd(error): the read stopped (cable out, port closed).
    constructor(port, { onText, onEnd }) {
        this.port = port;
        this.onText = onText;
        this.onEnd = onEnd;
        this.reader = null;
        this.closing = false;
        this.done = null;
        this.encoder = new TextEncoder();
    }

    async open() {
        await this.port.open({ baudRate: BAUD, bufferSize: 64 * 1024 });
        this.closing = false;
        this.done = this.loop();
    }

    async loop() {
        const decoder = new TextDecoder();
        let error = null;
        while (this.port.readable && !this.closing) {
            this.reader = this.port.readable.getReader();
            try {
                for (;;) {
                    const { value, done } = await this.reader.read();
                    if (done) {
                        break;
                    }
                    this.onText(decoder.decode(value, { stream: true }));
                }
            } catch (e) {
                // A framing or parity error ends this reader only: the next loop gets a new one.
                // A lost device ends the loop, because port.readable is then null.
                error = e;
            } finally {
                this.reader.releaseLock();
                this.reader = null;
            }
        }
        if (!this.closing) {
            this.onEnd(error);
        }
    }

    async write(text) {
        if (!this.port.writable) {
            return;
        }
        const writer = this.port.writable.getWriter();
        try {
            await writer.write(this.encoder.encode(text));
        } finally {
            writer.releaseLock();
        }
    }

    // Release the port: for esptool-js, or at the end.
    async close() {
        this.closing = true;
        if (this.reader) {
            await this.reader.cancel().catch(() => {});
        }
        if (this.done) {
            await this.done;
        }
        await this.port.close().catch(() => {});
    }
}

export function portLabel(port) {
    const info = port && port.getInfo ? port.getInfo() : {};
    const hex = (n) => n.toString(16).padStart(4, "0");
    if (info.usbVendorId === undefined) {
        return "serial port";
    }
    const chips = { 0x1a86: "CH34x", 0x10c4: "CP210x", 0x0403: "FTDI", 0x303a: "ESP32-S3 USB" };
    const name = chips[info.usbVendorId] || "USB";
    return `${name} ${hex(info.usbVendorId)}:${hex(info.usbProductId)}`;
}
