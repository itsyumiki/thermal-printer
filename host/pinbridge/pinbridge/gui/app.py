"""Small rewritten Tk GUI for jlpin v2.

Provenance: same-owner AGPLv3 gpiodemo/master GUI concept (commit 1224fb6^).
Widgets and all protocol interactions here are new; old SAM4E pin map/wire layer
are intentionally not reused. This file is GPLv3-or-later with the host package.
"""

from __future__ import annotations

import argparse
import tkinter as tk
from tkinter import messagebox, simpledialog, ttk

from ..board import Board
from ..fake import FakeSerial
from ..pins_gen import PINS
from .adapter import DANGEROUS_PINS, modes_for_pin, output_confirmation_required

WARNING = (
    "SAFETY: USB power only; thermal head disconnected; motor/battery supply OFF. "
    "USB-CDC round trips are not precise timing and cannot produce safe heater or step waveforms."
)


def run_gui(port=None, mock=False):
    try:
        import customtkinter as ctk
        root = ctk.CTk()
    except ImportError:
        root = tk.Tk()
    root.title("pinbridge-gui — jlpin v2")
    root.geometry("900x650")
    banner = tk.Label(
        root,
        text=WARNING,
        bg="#8b1e1e",
        fg="white",
        wraplength=860,
        justify="left",
        padx=8,
        pady=8,
    )
    banner.pack(fill="x")
    toolbar = tk.Frame(root)
    toolbar.pack(fill="x", padx=8, pady=5)
    status = tk.StringVar(value="Connected mock" if mock else "Opening USB CDC…")
    tk.Label(toolbar, textvariable=status).pack(side="left", fill="x", expand=True)
    confirmed = set()
    board = Board(port, serial_obj=FakeSerial() if mock else None)
    try:
        board.open()
    except Exception as exc:
        messagebox.showerror("pinbridge connection failed", str(exc), parent=root)
        root.destroy()
        return
    event_log = tk.Listbox(root, height=5)
    event_log.pack(fill="x", padx=8, pady=4)
    rows = {}
    ui_events = []
    board.on_event = ui_events.append
    canvas = tk.Canvas(root, highlightthickness=0)
    scroll = ttk.Scrollbar(root, orient="vertical", command=canvas.yview)
    inner = ttk.Frame(canvas)
    inner.bind(
        "<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all"))
    )
    canvas.create_window((0, 0), window=inner, anchor="nw")
    canvas.configure(yscrollcommand=scroll.set)
    canvas.pack(side="left", fill="both", expand=True)
    scroll.pack(side="right", fill="y")
    for col, title in enumerate(
        ("Pin", "Capabilities / mode", "Read", "Set mode", "Toggle output", "Listen", "State")
    ):
        ttk.Label(inner, text=title, font=("TkDefaultFont", 10, "bold")).grid(
            row=0, column=col, padx=6, pady=4, sticky="w"
        )

    def report_error(exc):
        status.set(f"ERROR: {exc}")
        messagebox.showerror("pinbridge command failed", str(exc), parent=root)

    def change_mode(pin, modevar, statevar):
        mode = modevar.get()
        if mode in ("lo", "hi") and output_confirmation_required(pin, mode, confirmed):
            prompt = f"Pin {pin} is a dangerous motor/print-head signal. Confirm output mode {mode}."
            if pin == "PB11":
                typed = simpledialog.askstring(
                    "PB11 heater-strobe hazard",
                    "Type PB11 to confirm enabling this output:",
                    parent=root,
                )
                if typed != "PB11":
                    statevar.set("untouched")
                    return
            elif not messagebox.askyesno(
                "Dangerous output", prompt + " Continue?", parent=root
            ):
                statevar.set("untouched")
                return
            confirmed.add(pin)
        try:
            board.mod(pin, mode)
            statevar.set(mode)
            status.set(f"{pin}: mode {mode}")
        except Exception as exc:
            report_error(exc)

    def read_pin(pin, statevar):
        try:
            reply = board.get(pin)
            statevar.set(reply[0].split("=", 1)[1])
            status.set(f"{pin}: {reply[0]}")
        except Exception as exc:
            report_error(exc)

    def toggle_pin(pin, modevar, statevar):
        if modevar.get() not in ("lo", "hi"):
            messagebox.showinfo(
                "Output mode required", f"Set {pin} to lo or hi first.", parent=root
            )
            return
        try:
            old = board.get(pin)[0].split("=", 1)[1]
            new = 0 if old == "1" else 1
            board.set(**{pin: new})
            statevar.set(str(new))
            status.set(f"{pin}: {new}")
        except Exception as exc:
            report_error(exc)

    def listen_pin(pin, listenvar, statevar):
        try:
            board.command(("LSN " + pin) if listenvar.get() else ("LSN off " + pin))
            status.set(f"{pin}: listener {'on' if listenvar.get() else 'off'}")
        except Exception as exc:
            report_error(exc)

    for i, pinrow in enumerate(PINS, start=1):
        pin, _, _, caps, _, _, alias, _ = pinrow
        label = pin + (f" ({alias})" if alias else "")
        if pin in DANGEROUS_PINS:
            label = "⚠ " + label
        ttk.Label(inner, text=label).grid(row=i, column=0, sticky="w", padx=6, pady=3)
        modevar = tk.StringVar(value="hiz")
        # UI begins untouched: no MOD is sent until a user explicitly acts.
        statevar = tk.StringVar(value="untouched")
        listenvar = tk.BooleanVar(value=False)
        ttk.Label(inner, text=", ".join(caps)).grid(row=i, column=1, sticky="w", padx=6)
        ttk.Combobox(
            inner,
            textvariable=modevar,
            values=modes_for_pin(pin),
            state="readonly",
            width=7,
        ).grid(row=i, column=1, sticky="e", padx=6)
        ttk.Button(
            inner, text="GET", command=lambda p=pin, s=statevar: read_pin(p, s)
        ).grid(row=i, column=2, padx=3)
        ttk.Button(
            inner,
            text="MOD",
            command=lambda p=pin, m=modevar, s=statevar: change_mode(p, m, s),
        ).grid(row=i, column=3, padx=3)
        ttk.Checkbutton(
            inner,
            text="LSN",
            variable=listenvar,
            command=lambda p=pin, l=listenvar, s=statevar: listen_pin(p, l, s),
        ).grid(row=i, column=4, padx=3)
        ttk.Label(inner, textvariable=statevar, width=14).grid(
            row=i, column=5, sticky="w", padx=6
        )
        rows[pin] = (modevar, statevar, listenvar)

    def reset_all():
        try:
            board.rst()
            for modevar, statevar, listenvar in rows.values():
                modevar.set("hiz")
                statevar.set("untouched")
                listenvar.set(False)
            status.set("RESET ALL: safe state requested; no outputs restored")
        except Exception as exc:
            report_error(exc)

    ttk.Button(toolbar, text="RESET ALL (safe state)", command=reset_all).pack(
        side="right"
    )

    def poll_events():
        # Board queues ! events when no on_event callback is installed.
        pending = list(ui_events)
        ui_events.clear()
        pending.extend((kind, value) for kind, value in list(board._events) if kind in ("pin", "watchdog", "reset"))
        for kind, value in pending:
            event_log.insert(tk.END, f"{kind}: {value}")
            if event_log.size() > 100: event_log.delete(0)
            if kind == "pin":
                status.set("PIN EVENT: " + value)
                try:
                    pin, rest = value[1:].split("=", 1)
                    val = rest.split()[0]
                    if pin in rows:
                        rows[pin][1].set(val)
                except ValueError:
                    pass
            elif kind == "watchdog":
                status.set("WATCHDOG TRIPPED — outputs reset; review before acting")
                for _, statevar, _ in rows.values():
                    statevar.set("untouched")
                messagebox.showwarning(
                    "Watchdog",
                    "Board watchdog tripped. Outputs were reset; UI state cleared.",
                    parent=root,
                )
            elif kind == "reset":
                status.set(
                    "BOARD RESET DETECTED — state unknown; do not auto-restore outputs"
                )
                for _, statevar, _ in rows.values():
                    statevar.set("untouched")
        root.after(100, poll_events)

    def on_close():
        try:
            board.rst()
        except Exception:
            pass
        board.close()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    status.set("Connected — all pins untouched; no startup configuration sent")
    poll_events()
    root.mainloop()


def main(argv=None):
    parser = argparse.ArgumentParser(prog="python -m pinbridge gui")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--port")
    group.add_argument("--mock", action="store_true")
    args = parser.parse_args(argv)
    run_gui(args.port, args.mock)
