// Dependency-free UART access for Bun.
//
// The `serialport` npm package ships a native NAPI addon that calls
// uv_default_loop(), which Bun does not implement (oven-sh/bun#18546), so it
// panics with SIGTRAP as soon as a port is opened. A tty is just a file, so we
// open it with node:fs and let `stty` set the line discipline.
import { spawnSync } from "node:child_process";
import {
  closeSync,
  constants,
  createReadStream,
  openSync,
  readdirSync,
  write as fsWrite,
} from "node:fs";

export type PortInfo = {
  path: string;
  manufacturer: string;
  serialNumber: string;
  vendorId: string;
  productId: string;
};

const isMac = process.platform === "darwin";
// macOS: use the callout (cu.*) nodes, never tty.* (those block on carrier detect).
const DEVICE_RE = isMac
  ? /^cu\.(usbmodem|usbserial|wchusbserial|SLAB_USBtoUART)/
  : /^tty(ACM|USB)\d+$/;

export function listPorts(): PortInfo[] {
  return readdirSync("/dev")
    .filter((n) => DEVICE_RE.test(n))
    .sort()
    .map((n) => ({
      path: "/dev/" + n,
      manufacturer: "",
      serialNumber: "",
      vendorId: "",
      productId: "",
    }));
}

/** Minimal serial port: `isOpen`, `write(wire, cb)` and `close()`, plus line/error/close callbacks. */
export class Uart {
  isOpen = false;
  private fd = -1;
  private rs: ReturnType<typeof createReadStream> | null = null;
  private buf = "";
  onLine: (line: string) => void = () => {};
  onError: (e: Error) => void = () => {};
  onClose: () => void = () => {};

  constructor(
    readonly path: string,
    readonly baudRate = 115200,
  ) {}

  /** Drop an incomplete line before protocol recovery attempts; complete lines already delivered stay delivered. */
  discardPartialLine() {
    this.buf = "";
  }

  open() {
    const r = spawnSync("stty", [
      isMac ? "-f" : "-F",
      this.path,
      String(this.baudRate),
      "raw",
      "-echo",
      "-hupcl",
      "clocal",
      "cread",
    ]);
    if (r.status !== 0)
      throw new Error(
        `stty failed for ${this.path}: ${String(r.stderr).trim() || "exit " + r.status}`,
      );
    this.fd = openSync(this.path, constants.O_RDWR | constants.O_NOCTTY);
    this.isOpen = true;
    const rs = createReadStream("", {
      fd: this.fd,
      autoClose: false,
      highWaterMark: 256,
    });
    this.rs = rs;
    rs.on("data", (chunk) => {
      this.buf += chunk.toString("utf8");
      let i: number;
      while ((i = this.buf.indexOf("\n")) >= 0) {
        const line = this.buf.slice(0, i);
        this.buf = this.buf.slice(i + 1);
        this.onLine(line);
      }
    });
    // Device unplugged or read failed: EIO / EOF.
    rs.on("error", (e) => {
      if (this.isOpen) this.onError(e);
    });
    rs.on("end", () => {
      if (this.isOpen) this.onError(new Error("UART closed by device"));
    });
  }

  write(data: string, cb?: (e?: Error | null) => void) {
    if (!this.isOpen) return cb?.(new Error("UART is not open"));
    fsWrite(this.fd, data, (e) => cb?.(e));
  }

  close() {
    if (!this.isOpen) return;
    this.isOpen = false;
    this.rs?.destroy();
    this.rs = null;
    try {
      closeSync(this.fd);
    } catch {}
    this.fd = -1;
    this.buf = "";
    this.onClose();
  }
}
