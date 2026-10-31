#!/usr/bin/env python3
"""br23t - host client for the BR23 Transporter (RP2040 + printer's UBOOT).

    br23t.py [--loader br23loader.bin] status
    br23t.py info                              (chip key + flash id, expect 3496 / 856013)
    br23t.py dump out.bin [--addr 0] [--len 0x80000] [--compare ref.bin]
    br23t.py plan  --app app.bin --dump dump.bin [--out image.bin]     (no hardware)
    br23t.py flash --app app.bin --dump dump.bin [--write] [--run]
    br23t.py restore --dump dump.bin [--write] [--run]                 (put the factory app back)
    br23t.py selftest-write --ref dump.bin --sector 0x50000 [--write]
    br23t.py ramrun IMAGE.bin [--addr 0x12000] [--clear A:N] [--poke A:V] [--read A:N]
    br23t.py runapp                            (leave UBOOT, boot the installed app)
    br23t.py rekey                             (reboot the Pico into USB_KEY mode)

Everything that writes is a dry run unless --write is given. `flash` takes the
app.bin fresh out of the SDK build, puts it into the original dump's app area
(patch_app.py: entry + area CRCs, re-scrambled with the chip key), reads the
printer's flash, and writes only the 4 KiB sectors that differ. The Pico firmware
refuses anything outside [0x3000, 0x63000) and anything that is not a chip with
key 3496 / flash id 856013, whatever this script asks.

The Pico does the USB_KEY entry at power-on, so a command that needs UBOOT waits
for you to switch the printer ON (see --wait). Needs pyserial, except for `plan`.
"""

import argparse
import glob
import hashlib
import os
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import patch_app  # noqa: E402

# The BR23 printer profile. Keep in step with firmware/src/jieli_uboot.h.
FLASH_SIZE = 0x80000            # 512 KiB SPI NOR
SECTOR = 0x1000
WRITE_MIN, WRITE_END = 0x3000, 0x63000   # the only sectors this tool may write
CONFIG_END = 0x3100             # [0x3000, 0x3100) = end of uboot.boot + isd_config: never changes
CHIP_KEY = 0x3496
EXPECT_INFO = "OK key=3496 type=3 id=856013"
LOADER_SIZE = 11264             # jl-uboot-tool's data/loaderblobs/usb/br23loader.bin


def die(msg):
    sys.exit(f"br23t: {msg}")


NO_LOADER = ("the Transporter has no br23loader: pass --loader PATH (or set BR23T_LOADER); "
             "it is jl-uboot-tool's data/loaderblobs/usb/br23loader.bin, 11264 bytes")


# --------------------------------------------------------------------------- link

def find_port(explicit=None):
    try:
        import serial  # late, so `plan` works without pyserial
    except ImportError:
        die("pyserial is missing: run this with `uv run host/br23t.py ...` or `pip install pyserial`")

    candidates = [explicit] if explicit else (
        sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*")))
    for path in candidates:
        try:
            s = serial.Serial(path, 115200, timeout=0.3)
        except serial.SerialException:
            continue
        s.reset_input_buffer()
        s.write(b"ping\n")
        deadline = time.time() + 1.0
        buf = b""
        while time.time() < deadline:
            buf += s.read(64)
            if b"PONG" in buf:
                s.timeout = 5
                return s
        s.close()
    die("no BR23 Transporter data port found (is the Pico plugged in? try --port)")


def request(s, line, timeout=30):
    s.reset_input_buffer()
    s.write(line.encode() + b"\n")
    s.timeout = timeout
    reply = s.readline().decode(errors="replace").strip()
    if not reply:
        die(f"no reply to '{line}'")
    return reply


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def status(s):
    reply = request(s, "status")
    if not reply.startswith("OK"):
        die(reply)
    return dict(kv.split("=") for kv in reply.split()[1:])


def upload_loader(s, path):
    data = Path(path).read_bytes()
    if len(data) != LOADER_SIZE:
        die(f"{path} is {len(data)} bytes, expected {LOADER_SIZE}. This is jl-uboot-tool's "
            "data/loaderblobs/usb/br23loader.bin, not the 24 KiB one from the SDK.")
    for off in range(0, len(data), 4096):
        chunk = data[off:off + 4096]
        s.reset_input_buffer()
        s.write(f"ldr {off} {len(chunk)} {crc32(chunk):08X}\n".encode() + chunk)
        s.timeout = 5
        reply = s.readline().decode(errors="replace").strip()
        if reply != "OK":
            die(f"loader upload failed at {off}: {reply or 'no reply'}")


def ensure_uboot(s, wait=60):
    """The Pico sends the USB_KEY while the printer powers on; wait for UBOOT to mount."""
    if status(s)["uboot"] == "1":
        return
    if wait <= 0:
        die("no UBOOT mounted: switch the printer OFF, then ON (run `rekey` first if the "
            "Pico was started with the printer already on)")
    print(f"waiting up to {wait} s for UBOOT: switch the printer ON now "
          "(OFF first if it is already on) ...", flush=True)
    deadline = time.time() + wait
    while time.time() < deadline:
        time.sleep(0.5)
        if status(s)["uboot"] == "1":
            print("printer is in UBOOT")
            return
    die("no UBOOT appeared. Switch the printer OFF and ON again while the Pico shows the key "
        "running, or run `rekey` and try again.")


def check_identity(s):
    reply = request(s, "info")
    print(reply)
    if reply != EXPECT_INFO:
        if reply.startswith("ERR") and status(s)["loader"] != "1":
            die(NO_LOADER)
        die(f"not the printer (expected {EXPECT_INFO!r}); aborting")


def read_flash(s, addr, length, progress=True):
    reply = request(s, f"read {addr:#x} {length}", timeout=30)
    if reply == "ERR loader":
        die(NO_LOADER)
    if not reply.startswith("DATA "):
        die(reply)
    n = int(reply.split()[1])

    data = bytearray()
    s.timeout = 10
    while len(data) < n:
        chunk = s.read(min(65536, n - len(data)))
        if not chunk:
            die(f"stalled at {len(data):#x} of {n:#x}")
        data += chunk
        if progress:
            print(f"\r{len(data) * 100 // n:3d}%  {len(data):#08x}", end="", flush=True)
    if progress:
        print()

    end = s.readline().decode(errors="replace").strip()
    if end != f"END {crc32(data):08X}":
        die(f"transfer check failed ({end}, local {crc32(data):08X})")
    return bytes(data)


def write_sector(s, a, sec):
    s.reset_input_buffer()
    s.write(f"wsec {a:#x} {crc32(sec):08X}\n".encode() + sec)
    s.timeout = 20
    return s.readline().decode(errors="replace").strip()


def hexlist(addrs):
    return " ".join(f"{a:#07x}" for a in addrs)


# ----------------------------------------------------------------------- commands

def cmd_status(s, args):
    print(request(s, "status"))


def cmd_runapp(s, args):
    print(request(s, "runapp"))


def cmd_rekey(s, args):
    print(request(s, "rekey"))
    print("the Pico is rebooting into USB_KEY mode; switch the printer OFF and ON now")


def cmd_info(s, args):
    ensure_uboot(s, args.wait)
    reply = request(s, "info")
    print(reply)
    if reply.startswith("ERR") and status(s)["loader"] != "1":
        die(NO_LOADER)
    print("matches the printer (3496 / 856013):", "yes" if reply == EXPECT_INFO else "NO")


def cmd_dump(s, args):
    ensure_uboot(s, args.wait)
    t0 = time.time()
    data = read_flash(s, args.addr, args.len)
    Path(args.out).write_bytes(data)
    dt = time.time() - t0
    print(f"saved {args.out}: {len(data)} bytes from {args.addr:#x}, crc32 {crc32(data):08X}, "
          f"{dt:.1f} s ({len(data) / dt / 1024:.0f} KiB/s)")
    if args.compare:
        compare(args.compare, args.addr, data)


def compare(ref_path, addr, data):
    """Per-4 KiB comparison. From WRITE_END up is VM/BTIF/key storage, which the printer
    rewrites when it runs, so differences there are reported, not failures."""
    ref = Path(ref_path).read_bytes()
    bad, changed = [], []
    for off in range(0, len(data), SECTOR):
        a = addr + off
        mine, theirs = data[off:off + SECTOR], ref[a:a + SECTOR]
        if len(theirs) < len(mine):
            break
        if mine != theirs:
            (bad if a < WRITE_END else changed).append(a)
    print(f"compared against {ref_path}")
    if bad:
        print(f"MISMATCH below {WRITE_END:#x}: sectors " + hexlist(bad))
    else:
        print(f"[{addr:#x}, {min(addr + len(data), WRITE_END):#x}) identical")
    if changed:
        print("device-data sectors differing (normal once the printer has run): " + hexlist(changed))
    if bad:
        sys.exit(1)


def plan_image(dump, app, key=CHIP_KEY):
    """Build + self-check the flash image for `app`; no hardware. Returns (image, changed)."""
    if len(dump) != FLASH_SIZE:
        die(f"dump must be {FLASH_SIZE:#x} bytes, got {len(dump):#x}")
    out, end, capacity = patch_app.build(dump, app, key)
    patch_app.verify(out, dump, app, key, end)
    outside = [a for a in list(range(0, WRITE_MIN, SECTOR)) + list(range(WRITE_END, FLASH_SIZE, SECTOR))
               if out[a:a + SECTOR] != dump[a:a + SECTOR]]
    if outside:
        die("image changes sectors outside the write window: " + hexlist(outside))
    changed = [a for a in range(WRITE_MIN, WRITE_END, SECTOR) if out[a:a + SECTOR] != dump[a:a + SECTOR]]
    print(f"app.bin: {len(app)} bytes (room for {capacity}), sha256 {hashlib.sha256(app).hexdigest()[:16]}...")
    if changed:
        print(f"image differs from the dump in {len(changed)} sectors ({changed[0]:#x}..{changed[-1]:#x})")
    else:
        print("image is identical to the dump")
    return out, changed


def cmd_plan(_, args):
    out, _changed = plan_image(args.dump.read_bytes(), args.app.read_bytes(), args.key)
    if args.out:
        args.out.write_bytes(out)
        print(f"wrote {args.out}")


def apply_image(s, img, dump, args):
    """Write the sectors of `img` that differ from the printer's flash, then verify.
    `img` equals `dump` outside [WRITE_MIN, WRITE_END) (checked by the callers)."""
    ensure_uboot(s, args.wait)
    check_identity(s)

    print("reading flash ...")
    now = read_flash(s, 0, FLASH_SIZE)
    if now[:WRITE_MIN] != dump[:WRITE_MIN]:
        die("flash below 0x3000 (header, uboot.boot) differs from --dump. Not touching anything.")
    if now[WRITE_MIN:CONFIG_END] != img[WRITE_MIN:CONFIG_END]:
        die("bytes 0x3000-0x30ff (end of uboot.boot + isd_config) differ between flash and the "
            "image. Not writing.")

    todo = [a for a in range(WRITE_MIN, WRITE_END, SECTOR) if img[a:a + SECTOR] != now[a:a + SECTOR]]
    todo.sort(key=lambda a: (a == WRITE_MIN, a))    # sector 0x3000 (entry list, CRCs) goes last
    if not todo:
        print("flash already holds this image")
    else:
        print(f"{len(todo)} sectors to write: {min(todo):#x}..{max(todo):#x}"
              + ("  (sector 0x3000 last)" if WRITE_MIN in todo else ""))
    if not args.write:
        print("dry run: nothing written (add --write)")
        return

    for i, a in enumerate(todo, 1):
        reply = write_sector(s, a, img[a:a + SECTOR])
        print(f"[{i}/{len(todo)}] sector {a:#07x}: {reply or 'no reply'}")
        if reply != "OK":
            die("sector write failed; the flash is in a mixed state. The chip stays in UBOOT: "
                "do not power-cycle blindly, re-run to retry, or `restore` the factory app.")

    expect = bytearray(now)
    for a in todo:
        expect[a:a + SECTOR] = img[a:a + SECTOR]
    print("final read-back ...")
    final = read_flash(s, 0, FLASH_SIZE)
    if final != bytes(expect):
        bad = [a for a in range(0, FLASH_SIZE, SECTOR) if final[a:a + SECTOR] != expect[a:a + SECTOR]]
        die("read-back DIFFERS in sectors " + hexlist(bad))
    print("read-back matches the expected image")
    if args.run:
        print(request(s, "runapp"))


def cmd_flash(s, args):
    dump = args.dump.read_bytes()
    img, _changed = plan_image(dump, args.app.read_bytes(), args.key)
    apply_image(s, img, dump, args)


def cmd_restore(s, args):
    dump = args.dump.read_bytes()
    if len(dump) != FLASH_SIZE:
        die(f"dump must be {FLASH_SIZE:#x} bytes, got {len(dump):#x}")
    apply_image(s, dump, dump, args)


def cmd_selftest_write(s, args):
    """Rewrite one sector with the bytes it already holds: proves erase/write/verify
    end to end without changing the flash contents."""
    a = args.sector
    if a < WRITE_MIN or a >= WRITE_END or a % SECTOR:
        die(f"sector must be 4 KiB aligned in [{WRITE_MIN:#x}, {WRITE_END:#x})")
    ref = args.ref.read_bytes()
    if len(ref) != FLASH_SIZE:
        die(f"--ref must be a {FLASH_SIZE:#x}-byte dump")

    ensure_uboot(s, args.wait)
    check_identity(s)
    print("reading flash ...")
    before = read_flash(s, 0, FLASH_SIZE)
    if before[:WRITE_END] != ref[:WRITE_END]:
        die("flash below 0x63000 differs from --ref; not writing")
    sec = before[a:a + SECTOR]
    print(f"flash equals --ref; sector {a:#07x} sha256 {hashlib.sha256(sec).hexdigest()[:16]}...")
    if not args.write:
        print("dry run: nothing written (add --write to rewrite this sector with its own bytes)")
        return

    for attempt in range(1, 4):
        reply = write_sector(s, a, sec)
        print(f"sector {a:#07x} attempt {attempt}: {reply or 'no reply'}")
        if reply == "OK":
            break
    else:
        die("sector rewrite failed; the chip stays in UBOOT. Restore with "
            "`br23t.py restore --dump <the original dump> --write`")

    print("final read-back ...")
    after = read_flash(s, 0, FLASH_SIZE)
    if after != before:
        bad = [x for x in range(0, FLASH_SIZE, SECTOR) if after[x:x + SECTOR] != before[x:x + SECTOR]]
        die("read-back DIFFERS in sectors " + hexlist(bad))
    print("read-back EQUALS the pre-write image: erase/write/verify works")


def span(text):
    a, _, b = text.partition(":")
    return int(a, 0), int(b, 0)


def mem_write(s, addr, data):
    for off in range(0, len(data), 4096):
        chunk = data[off:off + 4096]
        s.reset_input_buffer()
        s.write(f"memw {addr + off:#x} {len(chunk)} {crc32(chunk):08X}\n".encode() + chunk)
        s.timeout = 20
        reply = s.readline().decode(errors="replace").strip()
        if reply != "OK":
            die(f"memw {addr + off:#x}: {reply or 'no reply'}")


def mem_read(s, addr, length):
    out = bytearray()
    for off in range(0, length, 4096):
        n = min(4096, length - off)
        reply = request(s, f"memr {addr + off:#x} {n}", timeout=20)
        if reply != f"DATA {n}":
            die(f"memr {addr + off:#x}: {reply}")
        s.timeout = 10
        data = s.read(n)
        end = s.readline().decode(errors="replace").strip()
        if len(data) != n or end != f"END {crc32(data):08X}":
            die(f"memr {addr + off:#x}: transfer check failed ({end})")
        out += data
    return bytes(out)


def cmd_ramrun(s, args):
    """Load a RAM image through the ROM UBOOT1.00 and call it (no loader, no flash access)."""
    ensure_uboot(s, args.wait)
    if status(s).get("loader_running") == "1":
        die("the flash loader is running (it occupies 0x12000); RAM-run needs a fresh ROM UBOOT "
            "session. Switch the printer OFF and ON again first.")
    img = Path(args.image).read_bytes()
    for a, n in args.clear:
        mem_write(s, a, bytes(n))
    for a, v in args.poke:
        mem_write(s, a, (v & 0xFFFFFFFF).to_bytes(4, "little"))
    mem_write(s, args.addr, img)
    back = mem_read(s, args.addr, len(img))
    print(f"loaded {len(img)} B at {args.addr:#x}; read-back {'OK' if back == img else 'MISMATCH'}")
    if back != img:
        die("image read-back mismatch; not jumping")
    reply = request(s, f"jump {args.addr:#x} {args.arg:#x}", timeout=20)
    print(f"jump({args.addr:#x}, arg={args.arg:#06x}): {reply}")
    if not reply.startswith("OK"):
        sys.exit(1)
    for a, n in args.read:
        data = mem_read(s, a, n)
        for off in range(0, n, 16):
            print(f"  {a + off:08x}: {data[off:off + 16].hex(' ')}")


# --------------------------------------------------------------------------- main

def main():
    num = lambda x: int(x, 0)  # noqa: E731
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="data port (default: probe /dev/cu.usbmodem*, /dev/ttyACM*)")
    ap.add_argument("--loader", default=os.environ.get("BR23T_LOADER"),
                    help="br23loader.bin (11264 bytes, jl-uboot-tool), uploaded to the Pico when it "
                         "has none (default: $BR23T_LOADER)")
    ap.add_argument("--wait", type=int, default=60, metavar="SEC",
                    help="how long to wait for you to switch the printer ON (default 60; 0 = don't wait)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("status", "info", "runapp", "rekey"):
        sub.add_parser(name)
    d = sub.add_parser("dump")
    d.add_argument("out")
    d.add_argument("--addr", type=num, default=0)
    d.add_argument("--len", type=num, default=FLASH_SIZE)
    d.add_argument("--compare", help="reference image to compare against")
    p = sub.add_parser("plan", help="build and check the image for an app.bin; no hardware")
    p.add_argument("--app", required=True, type=Path)
    p.add_argument("--dump", required=True, type=Path, help="the ORIGINAL 512 KiB dump")
    p.add_argument("--key", type=num, default=CHIP_KEY)
    p.add_argument("--out", type=Path, help="also write the full image here")
    f = sub.add_parser("flash", help="flash a built app.bin (dry run unless --write)")
    f.add_argument("--app", required=True, type=Path)
    f.add_argument("--dump", required=True, type=Path, help="the ORIGINAL 512 KiB dump")
    f.add_argument("--key", type=num, default=CHIP_KEY)
    f.add_argument("--write", action="store_true", help="actually write (default: dry run)")
    f.add_argument("--run", action="store_true", help="after a verified write, boot the app")
    r = sub.add_parser("restore", help="write the factory app back from the original dump")
    r.add_argument("--dump", required=True, type=Path)
    r.add_argument("--write", action="store_true")
    r.add_argument("--run", action="store_true")
    t = sub.add_parser("selftest-write")
    t.add_argument("--ref", required=True, type=Path, help="the original dump; flash must equal it below 0x63000")
    t.add_argument("--sector", type=num, required=True)
    t.add_argument("--write", action="store_true")
    m = sub.add_parser("ramrun")
    m.add_argument("image")
    m.add_argument("--addr", type=num, default=0x12000)
    m.add_argument("--arg", type=num, default=0)
    m.add_argument("--clear", type=span, action="append", default=[])
    m.add_argument("--poke", type=span, action="append", default=[])
    m.add_argument("--read", type=span, action="append", default=[])
    args = ap.parse_args()

    if args.cmd == "plan":
        return cmd_plan(None, args)

    s = find_port(args.port)
    if args.loader and status(s)["loader"] != "1":
        upload_loader(s, args.loader)
    {"status": cmd_status, "info": cmd_info, "runapp": cmd_runapp, "rekey": cmd_rekey,
     "dump": cmd_dump, "flash": cmd_flash, "restore": cmd_restore,
     "selftest-write": cmd_selftest_write, "ramrun": cmd_ramrun}[args.cmd](s, args)


if __name__ == "__main__":
    main()
