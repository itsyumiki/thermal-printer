// Dependency-free UART access for Bun.
//
// The `serialport` npm package ships a native NAPI addon that calls
// uv_default_loop(), which Bun does not implement (oven-sh/bun#18546), so it
// panics with SIGTRAP as soon as a port is opened. A tty is just a file, so we
// open it with node:fs and let `stty` set the line discipline.
import { spawn } from "node:child_process";
import {
  closeSync,
  constants,
  createReadStream,
  open as fsOpen,
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

/** A hung stty or tty open must not wedge the connect loop; give up and let the caller retry. */
export const OPEN_TIMEOUT_MS = 5000;
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
  private cancelled = false;
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

  /** Run stty in a child process without blocking the event loop (a hang is killed after OPEN_TIMEOUT_MS). */
  private configureLine(): Promise<void> {
    return new Promise((resolve, reject) => {
      const child = spawn(
        "stty",
        [
          isMac ? "-f" : "-F",
          this.path,
          String(this.baudRate),
          "raw",
          "-echo",
          "-hupcl",
          "clocal",
          "cread",
        ],
        { stdio: ["ignore", "ignore", "pipe"] },
      );
      let stderr = "";
      child.stderr!.on("data", (d) => (stderr += d));
      const timer = setTimeout(() => {
        child.kill("SIGKILL");
        reject(
          new Error(
            `stty did not finish within ${OPEN_TIMEOUT_MS} ms for ${this.path}`,
          ),
        );
      }, OPEN_TIMEOUT_MS);
      child.on("error", (e) => {
        clearTimeout(timer);
        reject(e);
      });
      child.on("close", (code) => {
        clearTimeout(timer);
        if (code === 0) resolve();
        else
          reject(
            new Error(
              `stty failed for ${this.path}: ${stderr.trim() || "exit " + code}`,
            ),
          );
      });
    });
  }

  /** Open the tty on the thread pool; a late success after the timeout is closed again. */
  private openFd(): Promise<number> {
    return new Promise((resolve, reject) => {
      let settled = false;
      const timer = setTimeout(() => {
        settled = true;
        reject(
          new Error(
            `opening ${this.path} did not finish within ${OPEN_TIMEOUT_MS} ms`,
          ),
        );
      }, OPEN_TIMEOUT_MS);
      fsOpen(this.path, constants.O_RDWR | constants.O_NOCTTY, (e, fd) => {
        clearTimeout(timer);
        if (settled) {
          if (!e) closeSync(fd);
          return;
        }
        settled = true;
        if (e) reject(e);
        else resolve(fd);
      });
    });
  }

  /** Configure and open the port. Never blocks the event loop, so HTTP/WebSocket stay responsive meanwhile. */
  async open(): Promise<void> {
    this.cancelled = false;
    await this.configureLine();
    if (this.cancelled) throw new Error("UART open cancelled");
    const fd = await this.openFd();
    if (this.cancelled) {
      closeSync(fd);
      throw new Error("UART open cancelled");
    }
    this.fd = fd;
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
    this.cancelled = true; // an open still in flight gives up when it next checks
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
