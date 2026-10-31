"""In-memory stand-in for the Pico, speaking the same line protocol as
firmware/src/transporter_proto.c, so br23t's flows can be tested without hardware.

It mirrors the firmware's refusals: writes only for whole sectors inside
[0x3000, 0x63000), only when the chip identity matches, CRC-32 checks on payloads.
It records every sector write in `.writes`.
"""
import zlib

WRITE_MIN, WRITE_END, SECTOR = 0x3000, 0x63000, 0x1000
GOOD_INFO = "OK key=3496 type=3 id=856013"


def crc32(b):
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


class FakeSerial:
    def __init__(self, flash, loader=True, info=GOOD_INFO):
        self.flash = bytearray(flash)
        self.loader = loader
        self.info = info
        self.fail_write_at = None      # sector address whose write returns ERR write
        self.writes = []               # sector addresses, in order
        self.timeout = 5
        self._in = bytearray()
        self._out = bytearray()

    # --- pyserial surface used by br23t
    def reset_input_buffer(self):
        self._out.clear()

    def readline(self):
        i = self._out.find(b"\n")
        if i < 0:
            line, self._out = bytes(self._out), bytearray()
            return line
        line = bytes(self._out[:i + 1])
        del self._out[:i + 1]
        return line

    def read(self, n):
        data = bytes(self._out[:n])
        del self._out[:n]
        return data

    def write(self, data):
        self._in += data
        while True:
            i = self._in.find(b"\n")
            if i < 0:
                return
            argv = self._in[:i].decode().split()
            need = 0
            if argv and argv[0] == "wsec":
                need = SECTOR
            elif argv and argv[0] in ("ldr", "memw"):
                need = int(argv[2], 0)
            if len(self._in) < i + 1 + need:
                return
            payload = bytes(self._in[i + 1:i + 1 + need])
            del self._in[:i + 1 + need]
            self._handle(argv, payload)

    # --- protocol
    def _say(self, text):
        self._out += text.encode() + b"\n"

    def _handle(self, argv, payload):
        cmd = argv[0] if argv else ""
        if cmd == "ping":
            self._say("PONG")
        elif cmd == "status":
            self._say(f"OK uboot=1 v15=0 loader={int(self.loader)} loader_running=0")
        elif cmd == "ldr":
            if crc32(payload) != int(argv[3], 16):
                return self._say("ERR crc")
            self.loader = True
            self._say("OK")
        elif cmd == "info":
            self._say(self.info if self.loader else "ERR info")
        elif cmd == "read":
            addr, n = int(argv[1], 0), int(argv[2], 0)
            if not self.loader:
                return self._say("ERR loader")
            if n == 0 or addr >= len(self.flash) or n > len(self.flash) - addr:
                return self._say("ERR range")
            data = bytes(self.flash[addr:addr + n])
            self._say(f"DATA {n}")
            self._out += data
            self._say(f"END {crc32(data):08X}")
        elif cmd == "wsec":
            addr = int(argv[1], 0)
            if crc32(payload) != int(argv[2], 16):
                return self._say("ERR crc")
            if addr < WRITE_MIN or addr >= WRITE_END or addr % SECTOR:
                return self._say("ERR range")
            if not self.loader:
                return self._say("ERR loader")
            if self.info != GOOD_INFO:
                return self._say("ERR identity")
            if addr == self.fail_write_at:
                return self._say("ERR write")
            self.flash[addr:addr + SECTOR] = payload
            self.writes.append(addr)
            self._say("OK")
        elif cmd == "runapp":
            self._say("OK" if self.loader else "ERR no-loader")
        else:
            self._say("ERR unknown")
