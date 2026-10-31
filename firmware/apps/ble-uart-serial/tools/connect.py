#!/usr/bin/env python3
"""connect.py - connect to JL-UART-BLE, dump GATT, subscribe ae02, write ae01.
Usage: uv run --with bleak connect.py [seconds]   (default 40)

Dev helper used for the first hardware test (macOS, bleak). Text typed into the
board's CDC serial port shows up here as ae02 notifications; the line written to
ae01 comes out of the CDC port."""
import asyncio, sys
from bleak import BleakScanner, BleakClient

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 40
U = lambda s: f"0000{s}-0000-1000-8000-00805f9b34fb"

async def main():
    dev = await BleakScanner.find_device_by_name("JL-UART-BLE", timeout=15)
    if not dev:
        sys.exit("JL-UART-BLE not found")
    async with BleakClient(dev) as c:
        print("connected, MTU", c.mtu_size)
        for s in c.services:
            print("service", s.uuid)
            for ch in s.characteristics:
                print("   ", ch.uuid, ch.properties)
        await c.start_notify(U("ae02"), lambda _, b: print("notify (PC->BLE):", bytes(b)))
        await c.write_gatt_char(U("ae01"), b"hello from mac\n", response=False)
        print("wrote ae01; now type in the serial terminal for", SECS, "s")
        await asyncio.sleep(SECS)

asyncio.run(main())
