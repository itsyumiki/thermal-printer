#!/usr/bin/env python3
"""Build a flashable image for the JieLi BR23 thermal printer by swapping the
app.bin inside the original flash dump.

    patch_app.py --dump dump.bin --app app.bin --out image.bin [--slice-out slice.bin]

Everything except app.bin stays byte-identical to the dump: header, uboot.boot,
isd_config.ini (chip key, pins, oscillator), cfg_tool.bin, tone, VM/BTIF layout.
Layout of the app area (offsets from the dump, key 3496):

    0x3100  app_area_head entry   data CRC16 over [0x3120, 0x3100 + size)
    0x3120  entry list (app.bin, cfg_tool.bin, VM, PRCT, BTIF), 32 bytes each
    0x3220  app.bin data          (room for 388888 bytes; the rest is 0xFF)
    0x62138 cfg_tool.bin data
    0x624c8 "tone" resource block (ends at 0x63578)

The region [0x3100, 0x63578) is scrambled with jl_sfc_cipher(base=0x3100,
key=chip key). The tool never writes to flash; it only produces files.
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

from jl import jl_crc16, jl_sfc_cipher  # noqa: E402  (pure Python, no crcmod)

FLASH_SIZE = 0x80000
SECTOR = 0x1000
APP_BASE = 0x3100  # app_area_head entry (start of the scrambled area)
WRITE_MIN = 0x3000  # sector holding isd_config.ini + app_area_head
WRITE_MAX = 0x63000  # VM storage, BTIF and above are never touched (same as the Pico firmware)
ENTRY = struct.Struct("<HIIBBH16s")  # data_crc, offset, size, flags, resvd, index, name


class Entry:
    def __init__(self, buf, off, base):
        hcrc, hdr = struct.unpack_from("<H30s", buf, off)
        if jl_crc16(hdr) != hcrc:
            raise ValueError(f"entry header CRC mismatch at {off:#x}")
        (
            self.dcrc,
            self.offset,
            self.size,
            self.flags,
            self.resvd,
            self.index,
            self.raw_name,
        ) = ENTRY.unpack(hdr)
        self.hdr_off = off
        self.name = self.raw_name.split(b"\0")[0].decode("ascii")
        self.data_off = self.offset + base

    def store(self, buf):
        hdr = ENTRY.pack(
            self.dcrc,
            self.offset,
            self.size,
            self.flags,
            self.resvd,
            self.index,
            self.raw_name,
        )
        struct.pack_into("<H30s", buf, self.hdr_off, jl_crc16(hdr), hdr)


def sfc(buf, start, end, key):
    jl_sfc_cipher(buf, start, end - start, APP_BASE, key)


def peek_decrypted(raw, off, n, key):
    """Decrypt n bytes at off without touching raw (blocks are 32 bytes from APP_BASE)."""
    a = APP_BASE + ((off - APP_BASE) // 32) * 32
    b = APP_BASE + ((off + n - APP_BASE + 31) // 32) * 32
    tmp = bytearray(len(raw))
    tmp[a:b] = raw[a:b]
    jl_sfc_cipher(tmp, a, b - a, APP_BASE, key)
    return bytes(tmp[off : off + n])


def scrambled_end(raw, key):
    """End of the scrambled area: app_area_head block followed by the tone block."""
    head = Entry(bytearray(peek_decrypted(raw, APP_BASE, 32, key)), 0, 0)
    if head.name != "app_area_head":
        raise ValueError(
            f"expected app_area_head at {APP_BASE:#x}, found {head.name!r} "
            "(wrong chip key or wrong dump?)"
        )
    tone_off = APP_BASE + head.size
    tone = Entry(bytearray(peek_decrypted(raw, tone_off, 32, key)), 0, 0)
    return head, tone_off + tone.size


def walk_entries(dec, head):
    """Entries inside app_area_head, same rules as fwunpack_newfw.py."""
    out, off = [], head.hdr_off + 0x20
    while True:
        e = Entry(dec, off, head.hdr_off)
        out.append(e)
        if e.index != 0:
            return out
        off += 32


def build(dump, app, key):
    if len(dump) != FLASH_SIZE:
        sys.exit(f"patch_app: dump must be {FLASH_SIZE:#x} bytes, got {len(dump):#x}")

    head0, end = scrambled_end(dump, key)
    dec = bytearray(dump)
    sfc(dec, APP_BASE, end, key)

    head = Entry(dec, APP_BASE, 0)
    ents = {e.name: e for e in walk_entries(dec, head)}
    app_e, cfg_e = ents["app.bin"], ents["cfg_tool.bin"]
    area_end = APP_BASE + head.size
    if jl_crc16(dec[APP_BASE + 0x20 : area_end]) != head.dcrc:
        sys.exit("patch_app: app_area_head CRC does not match the dump; refusing")
    if jl_crc16(dec[app_e.data_off : app_e.data_off + app_e.size]) != app_e.dcrc:
        sys.exit("patch_app: app.bin CRC does not match the dump; refusing")

    capacity = cfg_e.data_off - app_e.data_off
    if len(app) > capacity:
        sys.exit(
            f"patch_app: app.bin is {len(app)} bytes, only {capacity} fit before cfg_tool.bin"
        )

    dec[app_e.data_off : app_e.data_off + capacity] = app + b"\xff" * (
        capacity - len(app)
    )
    app_e.size = len(app)
    app_e.dcrc = jl_crc16(app)
    app_e.store(dec)

    head.dcrc = jl_crc16(dec[APP_BASE + 0x20 : area_end])
    head.store(dec)

    out = bytearray(dec)
    sfc(out, APP_BASE, end, key)
    return bytes(out), end, capacity


def verify(out, dump, app, key, end):
    """Independent re-parse of the finished image."""
    dec = bytearray(out)
    sfc(dec, APP_BASE, end, key)
    head = Entry(dec, APP_BASE, 0)
    assert head.name == "app_area_head"
    assert jl_crc16(dec[APP_BASE + 0x20 : APP_BASE + head.size]) == head.dcrc, (
        "area CRC"
    )
    ents = {e.name: e for e in walk_entries(dec, head)}
    a = ents["app.bin"]
    assert bytes(dec[a.data_off : a.data_off + a.size]) == app, "app.bin round trip"
    assert jl_crc16(app) == a.dcrc, "app.bin CRC"

    orig = bytearray(dump)
    sfc(orig, APP_BASE, end, key)
    oents = {e.name: e for e in walk_entries(orig, Entry(orig, APP_BASE, 0))}
    c, oc = ents["cfg_tool.bin"], oents["cfg_tool.bin"]
    assert bytes(dec[c.data_off : c.data_off + c.size]) == bytes(
        orig[oc.data_off : oc.data_off + oc.size]
    ), "cfg_tool.bin changed"
    assert bytes(dec[end - 0x1000 : end]) == bytes(orig[end - 0x1000 : end]), (
        "tone area changed"
    )
    for name in ("VM", "PRCT", "BTIF"):
        e, o = ents[name], oents[name]
        assert (e.offset, e.size, e.flags, e.index) == (
            o.offset,
            o.size,
            o.flags,
            o.index,
        ), name

    diff = [
        x
        for x in range(0, FLASH_SIZE, SECTOR)
        if out[x : x + SECTOR] != dump[x : x + SECTOR]
    ]
    assert diff, "image identical to dump"
    assert diff[0] >= WRITE_MIN and diff[-1] < WRITE_MAX, (
        f"changes outside [{WRITE_MIN:#x}, {WRITE_MAX:#x}): {diff[0]:#x}..{diff[-1]:#x}"
    )
    return diff


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--dump", required=True, help="original 512 KiB flash dump")
    ap.add_argument(
        "--app", required=True, help="new app.bin (text+data from the SDK build)"
    )
    ap.add_argument("--out", required=True, help="full 512 KiB image to write")
    ap.add_argument(
        "--slice-out", help="only the changed sector range, for `write <addr> <file>`"
    )
    ap.add_argument(
        "--key",
        type=lambda x: int(x, 0),
        default=0x3496,
        help="chip key (default 0x3496)",
    )
    args = ap.parse_args()

    dump = Path(args.dump).read_bytes()
    app = Path(args.app).read_bytes()
    out, end, capacity = build(dump, app, args.key)
    diff = verify(out, dump, app, args.key, end)
    Path(args.out).write_bytes(out)

    lo, hi = diff[0], diff[-1] + SECTOR
    print(f"dump  sha256 {hashlib.sha256(dump).hexdigest()}")
    print(f"app   {len(app)} bytes (room for {capacity}), crc16 {jl_crc16(app):04X}")
    print(f"image {args.out}: sha256 {hashlib.sha256(out).hexdigest()}")
    print(
        f"scrambled area [{APP_BASE:#x}, {end:#x}) re-encrypted with key {args.key:04X}"
    )
    print(
        f"{len(diff)} sectors differ, {lo:#x}..{hi:#x}; everything else equals the dump"
    )
    print(
        "self-check passed (CRCs, app.bin round trip, cfg_tool/tone/VM/PRCT/BTIF unchanged)"
    )
    if args.slice_out:
        Path(args.slice_out).write_bytes(out[lo:hi])
        print(
            f"slice {args.slice_out}: {hi - lo:#x} bytes -> write {lo:#x} {args.slice_out}"
        )


if __name__ == "__main__":
    main()
