"""Desktop GPIO utility UI for jlpin v2.

Layout follows the same-owner gpiodemo desktop controller's compact, resizable
GPIO/log split, bank tabs, bulk toolbar, connection controls, and serial console.
Pin capabilities and denied entries remain sourced from the Pinbridge generated map.
"""

from __future__ import annotations

import argparse
import tkinter as tk
from collections import deque
from datetime import datetime
from tkinter import messagebox, simpledialog, ttk

from ..board import Board
from ..fake import FakeSerial
from ..pins_gen import CFG_HASH, DENIED, PINS
from .adapter import DANGEROUS_PINS, modes_for_pin, output_confirmation_required

# Only pins with confirmed table entries are rendered. DENIED entries are visible
# for documentation but never receive interactive GPIO controls.
PIN_INFO = {p[0]: p for p in PINS}
ALL_KNOWN = sorted(
    (*PIN_INFO, *DENIED),
    key=lambda n: (n[:2], int(n[2:]) if n[2:].isdigit() else 99, n),
)
SYSTEM_PINS = frozenset(DENIED)
DANGEROUS_PINS = frozenset(DANGEROUS_PINS)

MODE_LABELS = {
    "hiz": "High-Z",
    "in": "Input",
    "up": "Input + pull-up",
    "dn": "Input + pull-down",
    "lo": "Output LOW",
    "hi": "Output HIGH",
}
MODE_CHOICES = {
    "High-Z": "hiz",
    "Input": "in",
    "Pull-up": "up",
    "Pull-down": "dn",
    "Output LOW": "lo",
    "Output HIGH": "hi",
}
MODE_DISPLAY = {v: k for k, v in MODE_CHOICES.items()}


def _description(pin: str) -> str:
    if pin in DENIED:
        return DENIED[pin]
    return PIN_INFO[pin][7]


def _caps(pin: str) -> str:
    if pin in DENIED:
        return "RESERVED"
    caps = set(PIN_INFO[pin][3])
    labels = []
    if "in" in caps:
        labels.append("IN")
    if "out" in caps:
        labels.append("OUT")
    if "pu" in caps:
        labels.append("PU")
    if "pd" in caps:
        labels.append("PD")
    return " / ".join(labels) or "—"


def run_gui(port=None, mock=False):
    root = tk.Tk()
    root.title("Pinbridge — GPIO Controller")
    root.geometry("1420x820")
    root.minsize(980, 600)
    root.configure(bg="#242424")

    style = ttk.Style(root)
    try:
        style.theme_use("clam")
    except tk.TclError:
        pass
    style.configure(
        ".", background="#2b2b2b", foreground="#dce4ee", fieldbackground="#343638"
    )
    style.configure("TFrame", background="#2b2b2b")
    style.configure("TLabel", background="#2b2b2b", foreground="#dce4ee")
    style.configure(
        "TButton", padding=(8, 4), background="#3a3a3a", foreground="#dce4ee"
    )
    style.map(
        "TButton",
        background=[("active", "#464646"), ("disabled", "#303030")],
        foreground=[("disabled", "#888888")],
    )
    style.configure(
        "TCombobox",
        padding=(5, 3),
        fieldbackground="#343638",
        foreground="#dce4ee",
        arrowcolor="#dce4ee",
    )
    style.map(
        "TCombobox",
        fieldbackground=[("readonly", "#343638")],
        foreground=[("readonly", "#dce4ee")],
    )
    style.configure(
        "Treeview",
        background="#242424",
        fieldbackground="#242424",
        foreground="#dce4ee",
        rowheight=26,
        bordercolor="#474747",
    )
    style.configure(
        "Treeview.Heading", background="#333333", foreground="#dce4ee", padding=(5, 5)
    )
    style.configure(
        "LevelHigh.TLabel", background="#245c45", foreground="#e8fff4", padding=(3, 4)
    )
    style.configure(
        "LevelLow.TLabel", background="#3a3a3a", foreground="#dce4ee", padding=(3, 4)
    )
    style.configure(
        "LevelUnknown.TLabel",
        background="#303030",
        foreground="#b0b0b0",
        padding=(3, 4),
    )
    style.map("Treeview", background=[("selected", "#31516b")])
    style.configure("TNotebook", background="#242424", borderwidth=0)
    style.configure(
        "TNotebook.Tab", background="#333333", foreground="#dce4ee", padding=(10, 5)
    )
    style.map("TNotebook.Tab", background=[("selected", "#464646")])

    confirmed: set[str] = set()
    board: Board | None = None
    event_queue: deque = deque(maxlen=1000)
    command_history: list[str] = []
    history_index: int | None = None
    debug_enabled = False
    connected = False
    closing = False
    pin_vars: dict[str, dict[str, tk.Variable]] = {}
    row_widgets: dict[str, list[tuple[tk.Widget, int]]] = {}
    listen_active: set[str] = set()
    last_modes: dict[str, str] = {pin: "UNSET" for pin in PIN_INFO}
    last_levels: dict[str, str] = {pin: "—" for pin in PIN_INFO}

    # Connection controls: device selection is separate from connecting.
    outer = ttk.Frame(root, padding=8)
    outer.pack(fill="both", expand=True)
    conn = ttk.Frame(outer, padding=(8, 6))
    conn.pack(fill="x", pady=(0, 6))
    ttk.Label(conn, text="Serial port").pack(side="left", padx=(0, 6))
    port_var = tk.StringVar(value=port or "")
    port_box = ttk.Combobox(conn, textvariable=port_var, width=24, state="readonly")
    port_box.pack(side="left", padx=(0, 5))
    status_var = tk.StringVar(value="Mock mode" if mock else "Disconnected")
    connect_button = ttk.Button(conn, text="Connect")
    connect_button.pack(side="left", padx=3)
    refresh_button = ttk.Button(conn, text="Refresh ports")
    refresh_button.pack(side="left", padx=3)
    ttk.Separator(conn, orient="vertical").pack(side="left", fill="y", padx=8)
    ttk.Label(conn, text="Device").pack(side="left", padx=(0, 5))
    ttk.Label(conn, textvariable=status_var).pack(side="left", fill="x", expand=True)
    ttk.Label(
        conn,
        text="USB only · head disconnected · motor/battery supply off",
        foreground="#d6a84b",
    ).pack(side="right", padx=4)

    actions = ttk.Frame(outer, padding=(8, 5))
    actions.pack(fill="x", pady=(0, 6))
    action_buttons: dict[str, ttk.Button] = {}
    for label in (
        "Handshake",
        "Status",
        "Pin map",
        "Version / config",
        "Help",
        "Debug: off",
        "Reset device",
    ):
        btn = ttk.Button(actions, text=label)
        btn.pack(side="left", padx=(0, 5))
        action_buttons[label] = btn
    ttk.Label(actions, text="Watchdog ms").pack(side="left", padx=(8, 4))
    watchdog_var = tk.StringVar(value="5000")
    ttk.Entry(actions, textvariable=watchdog_var, width=8).pack(side="left")
    watchdog_button = ttk.Button(actions, text="Set watchdog")
    watchdog_button.pack(side="left", padx=5)

    panes = ttk.Panedwindow(outer, orient="horizontal")
    panes.pack(fill="both", expand=True)
    gpio_pane = ttk.Frame(panes, padding=8)
    log_pane = ttk.Frame(panes, padding=8)
    panes.add(gpio_pane, weight=4)
    panes.add(log_pane, weight=1)

    # Bulk controls mirror gpiodemo's scope/mode/overwrite/action toolbar.
    bulk = ttk.LabelFrame(gpio_pane, text="Bulk controls", padding=6)
    bulk.pack(fill="x", pady=(0, 6))
    ttk.Label(bulk, text="Scope").grid(row=0, column=0, padx=(0, 4), sticky="w")
    scope_var = tk.StringVar(value="ALL known pins")
    scope_box = ttk.Combobox(
        bulk,
        textvariable=scope_var,
        state="readonly",
        width=16,
        values=("ALL known pins", "Port A", "Port B", "Port C"),
    )
    scope_box.grid(row=0, column=1, padx=(0, 8), sticky="w")
    ttk.Label(bulk, text="Mode").grid(row=0, column=2, padx=(0, 4), sticky="w")
    bulk_mode_var = tk.StringVar(value="Input")
    bulk_mode_box = ttk.Combobox(
        bulk,
        textvariable=bulk_mode_var,
        state="readonly",
        width=18,
        values=("High-Z", "Input", "Pull-up", "Pull-down", "Output LOW", "Output HIGH"),
    )
    bulk_mode_box.grid(row=0, column=3, padx=(0, 8), sticky="w")
    overwrite_var = tk.BooleanVar(value=False)
    ttk.Checkbutton(
        bulk, text="Overwrite configured pins", variable=overwrite_var
    ).grid(row=0, column=4, padx=4, sticky="w")
    bulk_apply = ttk.Button(bulk, text="Apply mode")
    bulk_apply.grid(row=0, column=5, padx=3)
    bulk_actions = ttk.Frame(bulk)
    bulk_actions.grid(row=1, column=0, columnspan=6, sticky="w", pady=(6, 0))
    bulk_read_button = ttk.Button(bulk_actions, text="Read")
    bulk_read_button.pack(side="left", padx=(0, 4))
    bulk_listen_button = ttk.Button(bulk_actions, text="Listen")
    bulk_listen_button.pack(side="left", padx=4)
    bulk_stop_button = ttk.Button(bulk_actions, text="Stop listening")
    bulk_stop_button.pack(side="left", padx=4)
    bulk_high_button = ttk.Button(bulk_actions, text="Set HIGH")
    bulk_high_button.pack(side="left", padx=4)
    bulk_low_button = ttk.Button(bulk_actions, text="Set LOW")
    bulk_low_button.pack(side="left", padx=4)
    reset_all_button = ttk.Button(bulk_actions, text="RESET ALL (safe state)")
    reset_all_button.pack(side="left", padx=(10, 0))

    notebook = ttk.Notebook(gpio_pane)
    notebook.pack(fill="both", expand=True)
    tab_specs = [
        ("All", ALL_KNOWN),
        ("Port A", [p for p in ALL_KNOWN if p.startswith("PA")]),
        ("Port B", [p for p in ALL_KNOWN if p.startswith("PB")]),
        ("Port C", [p for p in ALL_KNOWN if p.startswith("PC")]),
        ("System", [p for p in ALL_KNOWN if p in SYSTEM_PINS]),
    ]

    def make_pin_tab(tab_name: str, names: list[str]):
        tab = ttk.Frame(notebook, padding=4)
        notebook.add(tab, text=tab_name)
        canvas = tk.Canvas(tab, bg="#242424", highlightthickness=0)
        yscroll = ttk.Scrollbar(tab, orient="vertical", command=canvas.yview)
        xscroll = ttk.Scrollbar(tab, orient="horizontal", command=canvas.xview)
        canvas.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        canvas.grid(row=0, column=0, sticky="nsew")
        yscroll.grid(row=0, column=1, sticky="ns")
        xscroll.grid(row=1, column=0, sticky="ew")
        tab.rowconfigure(0, weight=1)
        tab.columnconfigure(0, weight=1)
        table = ttk.Frame(canvas)
        win = canvas.create_window((0, 0), window=table, anchor="nw")
        table.bind(
            "<Configure>", lambda _e, c=canvas: c.configure(scrollregion=c.bbox("all"))
        )
        canvas.bind(
            "<Configure>",
            lambda e, c=canvas, w=win: c.itemconfigure(w, width=max(e.width, 1080)),
        )
        headers = (
            ("Pin", 0, 10),
            ("Description", 1, 52),
            ("Capabilities", 2, 16),
            ("Mode", 3, 18),
            ("Level", 4, 8),
            ("Read / Write", 5, 14),
            ("Listener", 6, 12),
        )
        for title, col, width in headers:
            ttk.Label(
                table, text=title, font=("TkDefaultFont", 9, "bold"), width=width
            ).grid(row=0, column=col, padx=4, pady=(2, 5), sticky="w")
        for i, pin in enumerate(names, 1):
            enabled = pin in PIN_INFO
            info = PIN_INFO.get(pin)
            alias = info[6] if info else None
            name = pin + (f" ({alias})" if alias else "")
            if pin in DANGEROUS_PINS:
                name = "⚠ " + name
            ttk.Label(table, text=name, width=10, font=("TkFixedFont", 9)).grid(
                row=i, column=0, padx=4, pady=2, sticky="w"
            )
            ttk.Label(
                table, text=_description(pin), width=52, wraplength=380, justify="left"
            ).grid(row=i, column=1, padx=4, pady=2, sticky="w")
            ttk.Label(
                table,
                text=_caps(pin),
                width=16,
                foreground="#b0b0b0" if not enabled else "#dce4ee",
            ).grid(row=i, column=2, padx=4, pady=2, sticky="w")
            if pin not in pin_vars:
                pin_vars[pin] = {
                    "mode": tk.StringVar(value="UNSET"),
                    "level": tk.StringVar(value="—"),
                    "listen": tk.BooleanVar(value=False),
                }
            vars_ = pin_vars[pin]
            if enabled:
                choices = ["UNSET"] + [MODE_DISPLAY[m] for m in modes_for_pin(pin)]
                mode = ttk.Combobox(
                    table,
                    textvariable=vars_["mode"],
                    values=choices,
                    state="readonly",
                    width=17,
                )
                mode.grid(row=i, column=3, padx=4, pady=2, sticky="ew")
                mode.bind("<<ComboboxSelected>>", lambda _e, p=pin: mode_selected(p))
                level_label = ttk.Label(
                    table, textvariable=vars_["level"], width=8, anchor="center"
                )
                level_label.grid(row=i, column=4, padx=4, pady=2, sticky="ew")
                row_widgets.setdefault(pin, []).append((level_label, 0))
                rw = ttk.Frame(table)
                rw.grid(row=i, column=5, padx=4, pady=2, sticky="w")
                read_button = ttk.Button(
                    rw, text="Read", command=lambda p=pin: read_pin(p), width=9
                )
                read_button.pack(side="left")
                write_button = ttk.Button(
                    rw,
                    text="Write HIGH",
                    command=lambda p=pin: write_pin(p, None),
                    width=12,
                )
                write_button.pack(side="left", padx=(3, 0))
                listener = ttk.Frame(table)
                listener.grid(row=i, column=6, padx=4, pady=2, sticky="w")
                listen_button = ttk.Button(
                    listener,
                    text="Listen",
                    command=lambda p=pin: set_listener(p, True),
                    width=9,
                )
                listen_button.pack(side="left")
                stop_button = ttk.Button(
                    listener,
                    text="Stop",
                    command=lambda p=pin: set_listener(p, False),
                    width=7,
                    state="disabled",
                )
                stop_button.pack(side="left", padx=(3, 0))
                row_widgets.setdefault(pin, []).append((read_button, 4))
                row_widgets.setdefault(pin, []).append((write_button, 1))
                row_widgets.setdefault(pin, []).append((listen_button, 2))
                row_widgets.setdefault(pin, []).append((stop_button, 3))
            else:
                ttk.Label(table, text="RESERVED", width=17, foreground="#d6a84b").grid(
                    row=i, column=3, padx=4, pady=2, sticky="w"
                )
                ttk.Label(table, text="—", width=8, anchor="center").grid(
                    row=i, column=4, padx=4, pady=2
                )
                ttk.Label(
                    table, text="System pin; controls disabled", foreground="#888888"
                ).grid(row=i, column=5, columnspan=2, padx=4, pady=2, sticky="w")
        return tab

    for tab_name, names in tab_specs:
        make_pin_tab(tab_name, names)

    # Serial log remains adjacent to the GPIO controls and shares a draggable divider.
    ttk.Label(log_pane, text="Serial log", font=("TkDefaultFont", 12, "bold")).pack(
        anchor="w", pady=(0, 5)
    )
    log_toolbar = ttk.Frame(log_pane)
    log_toolbar.pack(fill="x", pady=(0, 5))
    timestamps_var = tk.BooleanVar(value=True)
    autoscroll_var = tk.BooleanVar(value=True)
    timestamps_check = ttk.Checkbutton(
        log_toolbar,
        text="Timestamps",
        variable=timestamps_var,
        command=lambda: redraw_log(),
    )
    timestamps_check.pack(side="left")
    ttk.Checkbutton(log_toolbar, text="Auto-scroll", variable=autoscroll_var).pack(
        side="left", padx=4
    )
    ttk.Button(log_toolbar, text="Clear", command=lambda: clear_log()).pack(
        side="right"
    )
    log_text = tk.Text(
        log_pane,
        wrap="none",
        bg="#202020",
        fg="#dce4ee",
        insertbackground="#ffffff",
        selectbackground="#31516b",
        font=("TkFixedFont", 9),
        relief="flat",
        undo=False,
    )
    log_scroll_y = ttk.Scrollbar(log_pane, orient="vertical", command=log_text.yview)
    log_scroll_x = ttk.Scrollbar(log_pane, orient="horizontal", command=log_text.xview)
    log_text.configure(
        yscrollcommand=log_scroll_y.set,
        xscrollcommand=log_scroll_x.set,
        state="disabled",
    )
    log_text.pack(side="left", fill="both", expand=True)
    log_scroll_y.pack(side="left", fill="y")
    log_scroll_x.pack(side="bottom", fill="x")
    command_row = ttk.Frame(log_pane)
    command_row.pack(fill="x", pady=(7, 0))
    raw_var = tk.StringVar()
    raw_entry = ttk.Entry(command_row, textvariable=raw_var, font=("TkFixedFont", 9))
    raw_entry.pack(side="left", fill="x", expand=True)
    send_button = ttk.Button(command_row, text="Send", command=lambda: send_raw())
    send_button.pack(side="left", padx=(4, 0))

    log_entries: deque[tuple[str, str]] = deque(maxlen=2000)

    def _format_log_entry(stamp: str, message: str) -> str:
        prefix = f"[{stamp}] " if timestamps_var.get() else ""
        return prefix + message.rstrip("\n") + "\n"

    def redraw_log():
        log_text.configure(state="normal")
        log_text.delete("1.0", "end")
        for stamp, message in log_entries:
            log_text.insert("end", _format_log_entry(stamp, message))
        log_text.configure(state="disabled")
        if autoscroll_var.get():
            log_text.see("end")

    def log(message: str):
        stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        log_entries.append((stamp, message.rstrip("\n")))
        log_text.configure(state="normal")
        log_text.insert("end", _format_log_entry(stamp, message))
        # Bound retained log size so long listener sessions do not grow without limit.
        if int(log_text.index("end-1c").split(".")[0]) > 2000:
            log_text.delete("1.0", "500.0")
        log_text.configure(state="disabled")
        if autoscroll_var.get():
            log_text.see("end")

    def clear_log():
        log_entries.clear()
        log_text.configure(state="normal")
        log_text.delete("1.0", "end")
        log_text.configure(state="disabled")

    def show_error(exc, title="Pinbridge"):
        status_var.set(f"Error: {exc}")
        log(f"ERROR {type(exc).__name__}: {exc}")
        messagebox.showerror(title, str(exc), parent=root)

    def record_reply(command: str, parts: list[str]):
        log(f"> {command}")
        log("< ok" + (" " + " ".join(parts) if parts else ""))
        status_var.set("Connected · last command: " + command.split()[0].upper())

    def run_command(command: str, *, quiet=False):
        if board is None or not connected:
            raise RuntimeError("Connect to a Pinbridge device first")
        parts = board.command(command)
        if not quiet:
            record_reply(command, parts)
        return parts

    def refresh_ports():
        values = []
        try:
            from serial.tools import list_ports

            values = sorted(p.device for p in list_ports.comports())
        except Exception as exc:
            log(f"Port scan unavailable: {exc}")
        if port and port not in values:
            values.insert(0, port)
        port_box.configure(values=values)
        if port_var.get() not in values:
            port_var.set(values[0] if values else "")
        if not values and not mock:
            status_var.set("No serial ports found · choose Refresh ports")

    def update_pin_state(pin: str, mode: str | None = None, level: str | None = None):
        if pin not in PIN_INFO:
            return
        if mode is not None:
            last_modes[pin] = mode
            pin_vars[pin]["mode"].set(
                MODE_DISPLAY.get(mode, mode.upper()) if mode != "UNSET" else "UNSET"
            )
        if level is not None:
            last_levels[pin] = str(level)
            pin_vars[pin]["level"].set(
                "HIGH" if str(level) == "1" else "LOW" if str(level) == "0" else "—"
            )
        mode_now = last_modes[pin]
        for widget, kind in row_widgets.get(pin, []):
            if kind == 0:
                widget.configure(
                    style=(
                        "LevelHigh.TLabel"
                        if last_levels[pin] == "1"
                        else "LevelLow.TLabel"
                        if last_levels[pin] == "0"
                        else "LevelUnknown.TLabel"
                    )
                )
            elif kind == 4:
                widget.configure(
                    state="normal"
                    if mode_now in ("in", "up", "dn", "lo", "hi")
                    else "disabled"
                )
            elif kind == 1:
                if mode_now in ("lo", "hi"):
                    widget.configure(
                        text="Write LOW" if last_levels[pin] == "1" else "Write HIGH",
                        state="normal",
                    )
                else:
                    widget.configure(text="Write HIGH", state="disabled")
            elif kind == 2:
                widget.configure(
                    state="normal"
                    if mode_now in ("in", "up", "dn") and pin not in listen_active
                    else "disabled"
                )
            elif kind == 3:
                widget.configure(state="normal" if pin in listen_active else "disabled")

    def sync_status():
        parts = run_command("STA", quiet=True)
        log("> STA")
        log("< ok" + (" " + " ".join(parts) if parts else ""))
        found = set()
        for token in parts:
            if "=" not in token:
                continue
            pin, mode = token.split("=", 1)
            pin = pin.upper()
            if pin in PIN_INFO:
                found.add(pin)
                update_pin_state(
                    pin,
                    mode.lower(),
                    "1"
                    if mode.lower() == "hi"
                    else "0"
                    if mode.lower() == "lo"
                    else last_levels[pin],
                )
        for pin in PIN_INFO:
            if pin not in found:
                update_pin_state(pin, "UNSET", "—")

    def connect_device():
        nonlocal board, connected
        if connected:
            disconnect_device()
            return
        candidate = None
        try:
            if mock:
                candidate = Board(serial_obj=FakeSerial())
            else:
                selected = port_var.get().strip()
                if not selected:
                    raise ValueError("Select a serial port first")
                candidate = Board(selected)
            candidate.on_event = lambda event: event_queue.append(event)
            candidate.open()
            board = candidate
            connected = True
            connect_button.configure(text="Disconnect")
            port_box.configure(state="disabled")
            refresh_button.configure(state="disabled")
            log(
                f"Connected to {'mock device' if mock else port_var.get()} · jlpin v2 · cfg {CFG_HASH}"
            )
            status_var.set("Connected · no pin modes changed at startup")
            sync_status()
        except Exception as exc:
            if candidate is not None:
                try:
                    candidate.close()
                except Exception:
                    pass
            board = None
            connected = False
            connect_button.configure(text="Connect")
            port_box.configure(state="readonly")
            refresh_button.configure(state="normal")
            show_error(exc, "Connection failed")

    def disconnect_device():
        nonlocal board, connected
        old = board
        board = None
        connected = False
        if old is not None:
            try:
                old.rst()
                log("< ok (RST safe state)")
            except Exception as exc:
                log(f"RST on disconnect failed: {exc}")
            old.close()
        connect_button.configure(text="Connect")
        port_box.configure(state="readonly")
        refresh_button.configure(state="normal")
        for pin in PIN_INFO:
            listen_active.discard(pin)
            update_pin_state(pin, "UNSET", "—")
        status_var.set("Disconnected · no output state restored")

    def run_action(label: str, command: str | None = None):
        try:
            if label == "Handshake" or label == "Version / config":
                parts = run_command("HEL")
                messagebox.showinfo(
                    label,
                    "Protocol: "
                    + " ".join(parts[:3])
                    + "\nConfig hash: "
                    + CFG_HASH
                    + "\n"
                    + (parts[3] if len(parts) > 3 else ""),
                    parent=root,
                )
            elif label == "Status":
                parts = run_command("STA", quiet=True)
                log("> STA")
                log("< ok" + (" " + " ".join(parts) if parts else ""))
                sync_status()
                messagebox.showinfo(
                    "Status", "\n".join(parts) or "No configured pins", parent=root
                )
            elif label == "Pin map":
                lines = [
                    f"Configuration hash: {CFG_HASH}",
                    "",
                    "Known pin map (reserved entries are disabled):",
                ]
                lines.extend(
                    f"{p:5}  {_caps(p):10}  {_description(p)}" for p in ALL_KNOWN
                )
                messagebox.showinfo("Pin map", "\n".join(lines), parent=root)
            elif label == "Help":
                messagebox.showinfo(
                    "Pinbridge command help",
                    "Commands: HEL, STA, MOD PIN MODE, GET [PINS], SET PIN=0|1 [...], LSN PIN, LSN off [PIN], DBG on|off, WDG MS, RST.\n\nReserved pins are shown for reference but excluded from the firmware allow-list. Use USB power only, with the head disconnected and motor/battery supply off.",
                    parent=root,
                )
            elif label == "Debug: off":
                parts = run_command("DBG on" if not debug_enabled else "DBG off")
                nonlocal_debug = not debug_enabled
                set_debug_state(nonlocal_debug)
            elif label == "Reset device":
                if messagebox.askyesno(
                    "Reset device",
                    "Send RST and return all configured pins to high impedance?",
                    parent=root,
                ):
                    run_command("RST")
                    listen_active.clear()
                    for p in PIN_INFO:
                        update_pin_state(p, "UNSET", "—")
            elif command:
                run_command(command)
        except Exception as exc:
            show_error(exc)

    def set_debug_state(enabled: bool):
        nonlocal debug_enabled
        debug_enabled = enabled
        action_buttons["Debug: off"].configure(
            text="Debug: on" if enabled else "Debug: off"
        )
        status_var.set("Debug output " + ("enabled" if enabled else "disabled"))

    def mode_selected(pin: str):
        value = pin_vars[pin]["mode"].get()
        if value == "UNSET":
            return
        mode = MODE_CHOICES.get(value)
        if not mode:
            return
        try:
            if output_confirmation_required(pin, mode, confirmed):
                if pin == "PB11":
                    typed = simpledialog.askstring(
                        "PB11 heater-strobe hazard",
                        "Type PB11 to enable this output:",
                        parent=root,
                    )
                    if typed != "PB11":
                        pin_vars[pin]["mode"].set(
                            MODE_DISPLAY.get(last_modes[pin], "UNSET")
                        )
                        return
                elif not messagebox.askyesno(
                    "Dangerous output",
                    f"{pin} is assigned to a stepper/printhead signal. Enable output {mode.upper()}?",
                    parent=root,
                ):
                    pin_vars[pin]["mode"].set(
                        MODE_DISPLAY.get(last_modes[pin], "UNSET")
                    )
                    return
                confirmed.add(pin)
            run_command(f"MOD {pin} {mode}")
            update_pin_state(
                pin, mode, "1" if mode == "hi" else "0" if mode == "lo" else "—"
            )
        except Exception as exc:
            pin_vars[pin]["mode"].set(MODE_DISPLAY.get(last_modes[pin], "UNSET"))
            show_error(exc)

    def read_pin(pin: str):
        try:
            mode = last_modes[pin]
            if mode == "UNSET":
                raise RuntimeError(f"{pin} is not configured; choose a mode first")
            parts = run_command(f"GET {pin}")
            for item in parts:
                if "=" in item:
                    p, val = item.split("=", 1)
                    update_pin_state(p.upper(), level=val)
        except Exception as exc:
            show_error(exc)

    def write_pin(pin: str, value: int | None):
        try:
            if last_modes[pin] not in ("lo", "hi"):
                raise RuntimeError(f"Set {pin} to Output LOW or Output HIGH first")
            if value is None:
                value = 0 if last_levels[pin] == "1" else 1
            run_command(f"SET {pin}={value}")
            update_pin_state(pin, "hi" if value else "lo", str(value))
        except Exception as exc:
            show_error(exc)

    def set_listener(pin: str, enabled: bool):
        try:
            if enabled and last_modes[pin] not in ("in", "up", "dn"):
                raise RuntimeError(f"{pin} must be in an input mode before listening")
            run_command(f"LSN {pin}" if enabled else f"LSN off {pin}")
            if enabled:
                listen_active.add(pin)
            else:
                listen_active.discard(pin)
            update_pin_state(pin)
            log(f"Listener {'started' if enabled else 'stopped'} for {pin}")
        except Exception as exc:
            show_error(exc)

    def selected_pins(*, include_denied=False):
        scope = scope_var.get()
        pins = list(PIN_INFO)
        if scope in ("Port A", "Port B", "Port C"):
            prefix = {"Port A": "PA", "Port B": "PB", "Port C": "PC"}[scope]
            pins = [p for p in pins if p.startswith(prefix)]
        return pins

    def confirm_dangerous_bulk(pins: list[str], mode: str | None = None) -> bool:
        risky = [p for p in pins if p in DANGEROUS_PINS]
        if not risky:
            return True
        if "PB11" in risky and mode in ("lo", "hi"):
            typed = simpledialog.askstring(
                "PB11 heater-strobe hazard",
                f"Bulk output includes PB11. Type PB11 to continue ({mode.upper()}):",
                parent=root,
            )
            if typed != "PB11":
                return False
            confirmed.add("PB11")
        others = [p for p in risky if p != "PB11"]
        if others and not messagebox.askyesno(
            "Dangerous bulk GPIO operation",
            f"This operation includes driver pins: {', '.join(others)}. Continue?",
            parent=root,
        ):
            return False
        confirmed.update(risky)
        return True

    def apply_bulk_mode():
        mode = MODE_CHOICES.get(bulk_mode_var.get())
        if not mode:
            return
        pins = selected_pins()
        if not overwrite_var.get():
            pins = [p for p in pins if last_modes[p] == "UNSET"]
        pins = [p for p in pins if mode in modes_for_pin(p)]
        if not pins:
            status_var.set("No eligible pins in scope")
            return
        if mode in ("lo", "hi") and not confirm_dangerous_bulk(pins, mode):
            return
        if not messagebox.askyesno(
            "Apply mode",
            f"Apply {mode.upper()} to {len(pins)} eligible pin(s)?",
            parent=root,
        ):
            return
        errors = []
        for pin in pins:
            try:
                run_command(f"MOD {pin} {mode}")
                update_pin_state(
                    pin, mode, "1" if mode == "hi" else "0" if mode == "lo" else "—"
                )
            except Exception as exc:
                errors.append(f"{pin}: {exc}")
        status_var.set(
            f"Applied {mode.upper()} to {len(pins) - len(errors)}/{len(pins)} pins"
        )
        if errors:
            messagebox.showwarning(
                "Bulk operation completed with errors", "\n".join(errors), parent=root
            )

    def bulk_read():
        pins = [p for p in selected_pins() if last_modes[p] != "UNSET"]
        if not pins:
            status_var.set("No configured pins to read")
            return
        for pin in pins:
            try:
                parts = run_command(f"GET {pin}")
                for item in parts:
                    if "=" in item:
                        p, value = item.split("=", 1)
                        update_pin_state(p.upper(), level=value)
            except Exception as exc:
                log(f"GET {pin} failed: {exc}")

    def bulk_listen(enabled: bool):
        pins = [p for p in selected_pins() if last_modes[p] in ("in", "up", "dn")]
        if not pins:
            status_var.set("No input-mode pins in scope")
            return
        if not enabled:
            active = [p for p in pins if p in listen_active]
            if not active:
                status_var.set("No active listeners in scope")
                return
            try:
                run_command("LSN off " + " ".join(active))
                listen_active.difference_update(active)
                for p in active:
                    update_pin_state(p)
            except Exception as exc:
                show_error(exc)
            return
        errors = []
        for pin in pins:
            if pin in listen_active:
                continue
            try:
                run_command(f"LSN {pin}")
                listen_active.add(pin)
                update_pin_state(pin)
            except Exception as exc:
                errors.append(f"{pin}: {exc}")
        if errors:
            messagebox.showwarning(
                "Some listeners failed", "\n".join(errors), parent=root
            )

    def bulk_set(value: int):
        pins = [p for p in selected_pins() if last_modes[p] in ("lo", "hi")]
        if not pins:
            status_var.set("No output-mode pins in scope")
            return
        if not confirm_dangerous_bulk(pins, "hi" if value else "lo"):
            return
        if not messagebox.askyesno(
            "Bulk output",
            f"Set {len(pins)} configured output(s) {'HIGH' if value else 'LOW'}?",
            parent=root,
        ):
            return
        # The protocol permits up to 15 arguments; keep each atomic SET under that limit.
        errors = []
        for start in range(0, len(pins), 14):
            batch = pins[start : start + 14]
            try:
                run_command("SET " + " ".join(f"{p}={value}" for p in batch))
                for p in batch:
                    update_pin_state(p, "hi" if value else "lo", str(value))
            except Exception as exc:
                errors.append(str(exc))
        if errors:
            messagebox.showwarning("Bulk output errors", "\n".join(errors), parent=root)

    def reset_all():
        if not messagebox.askyesno(
            "RESET ALL",
            "Return all configured pins to high impedance and stop listeners?",
            parent=root,
        ):
            return
        try:
            run_command("RST")
            listen_active.clear()
            for p in PIN_INFO:
                update_pin_state(p, "UNSET", "—")
            status_var.set("Safe state requested · no outputs restored")
        except Exception as exc:
            show_error(exc)

    def send_raw():
        nonlocal history_index
        command = raw_var.get().strip()
        if not command:
            return
        command_history.append(command)
        if len(command_history) > 200:
            del command_history[:-200]
        history_index = None
        raw_var.set("")
        try:
            parts = run_command(command)
            # Reflect state changes from raw commands when they match known pin operations.
            tokens = command.split()
            if tokens and tokens[0].upper() == "RST":
                listen_active.clear()
                for p in PIN_INFO:
                    update_pin_state(p, "UNSET", "—")
            elif (
                len(tokens) == 3
                and tokens[0].upper() == "MOD"
                and tokens[1].upper() in PIN_INFO
            ):
                update_pin_state(
                    tokens[1].upper(),
                    tokens[2].lower(),
                    "1"
                    if tokens[2].lower() == "hi"
                    else "0"
                    if tokens[2].lower() == "lo"
                    else "—",
                )
            elif tokens and tokens[0].upper() == "STA":
                sync_status()
            elif tokens and tokens[0].upper() in ("GET",):
                for item in parts:
                    if "=" in item:
                        pin, value = item.split("=", 1)
                        update_pin_state(pin.upper(), level=value)
            elif tokens and tokens[0].upper() == "SET":
                for item in tokens[1:]:
                    if "=" in item:
                        pin, value = item.split("=", 1)
                        update_pin_state(
                            pin.upper(), "hi" if value == "1" else "lo", value
                        )
            elif tokens and tokens[0].upper() == "LSN":
                if len(tokens) > 1 and tokens[1].lower() == "off":
                    if len(tokens) == 2:
                        listen_active.clear()
                    else:
                        listen_active.difference_update(p.upper() for p in tokens[2:])
                else:
                    listen_active.update(
                        p.upper() for p in tokens[1:] if p.upper() in PIN_INFO
                    )
                for pin in PIN_INFO:
                    update_pin_state(pin)
            elif tokens and tokens[0].upper() == "DBG" and len(tokens) == 2:
                set_debug_state(tokens[1].lower() == "on")
        except Exception as exc:
            log(f"> {command}\n< ERROR {type(exc).__name__}: {exc}")
            status_var.set(f"Command failed: {exc}")
        raw_entry.focus_set()

    def history_key(event):
        nonlocal history_index
        if not command_history:
            return "break"
        if event.keysym == "Up":
            history_index = (
                len(command_history) - 1
                if history_index is None
                else max(0, history_index - 1)
            )
        else:
            history_index = (
                len(command_history)
                if history_index is None
                else min(len(command_history), history_index + 1)
            )
        raw_var.set(
            command_history[history_index]
            if history_index < len(command_history)
            else ""
        )
        raw_entry.icursor("end")
        return "break"

    def poll_events():
        if closing:
            return
        while event_queue:
            kind, value = event_queue.popleft()
            log(f"EVENT {kind}: {value}")
            if kind == "pin":
                try:
                    payload = str(value).lstrip("!")
                    pin, rest = payload.split("=", 1)
                    level = rest.split()[0]
                    update_pin_state(pin.upper(), level=level)
                except (ValueError, IndexError):
                    pass
            elif kind == "watchdog":
                status_var.set("WATCHDOG TRIPPED · outputs reset; review before acting")
                listen_active.clear()
                for p in PIN_INFO:
                    update_pin_state(p, "UNSET", "—")
                messagebox.showwarning(
                    "Watchdog",
                    "The board watchdog tripped. Pin states were cleared in the UI; verify the device before acting.",
                    parent=root,
                )
            elif kind == "reset":
                status_var.set("BOARD RESET DETECTED · state unknown")
                for p in PIN_INFO:
                    update_pin_state(p, "UNSET", "—")
            elif kind == "debug":
                log(str(value))
        root.after(100, poll_events)

    def on_close():
        nonlocal closing
        closing = True
        disconnect_device()
        root.destroy()

    refresh_button.configure(command=refresh_ports)
    connect_button.configure(command=connect_device)
    action_buttons["Handshake"].configure(command=lambda: run_action("Handshake"))
    action_buttons["Status"].configure(command=lambda: run_action("Status"))
    action_buttons["Pin map"].configure(command=lambda: run_action("Pin map"))
    action_buttons["Version / config"].configure(
        command=lambda: run_action("Version / config")
    )
    action_buttons["Help"].configure(command=lambda: run_action("Help"))
    action_buttons["Debug: off"].configure(command=lambda: run_action("Debug: off"))
    action_buttons["Reset device"].configure(command=lambda: run_action("Reset device"))
    watchdog_button.configure(command=lambda: set_watchdog())
    bulk_apply.configure(command=apply_bulk_mode)
    bulk_read_button.configure(command=bulk_read)
    bulk_listen_button.configure(command=lambda: bulk_listen(True))
    bulk_stop_button.configure(command=lambda: bulk_listen(False))
    bulk_high_button.configure(command=lambda: bulk_set(1))
    bulk_low_button.configure(command=lambda: bulk_set(0))
    reset_all_button.configure(command=reset_all)
    raw_entry.bind("<Return>", lambda _e: send_raw())
    raw_entry.bind("<Up>", history_key)
    raw_entry.bind("<Down>", history_key)

    def set_watchdog():
        try:
            ms = int(watchdog_var.get())
            if ms != 0 and not 100 <= ms <= 3600000:
                raise ValueError("Use 0 to disable or 100–3600000 milliseconds")
            run_command(f"WDG {ms}")
        except Exception as exc:
            show_error(exc, "Watchdog setting")

    def scope_changed(_event=None):
        # Select the corresponding tab for convenient visual feedback; the scope
        # remains an independent selector for future changes.
        target = scope_var.get()
        for idx, (name, _pins) in enumerate(tab_specs):
            if name == target:
                notebook.select(idx)
                break

    scope_box.bind("<<ComboboxSelected>>", scope_changed)
    notebook.bind("<<NotebookTabChanged>>", lambda _e: None)

    refresh_ports()
    if mock:
        port_var.set("Mock device")
        port_box.configure(values=("Mock device",))
        connect_device()
    elif port:
        port_var.set(port)
        connect_device()
    root.protocol("WM_DELETE_WINDOW", on_close)
    poll_events()
    root.mainloop()


def main(argv=None):
    parser = argparse.ArgumentParser(prog="python -m pinbridge gui")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--port", help="serial port, e.g. /dev/ttyACM0 or COM3")
    group.add_argument(
        "--mock", action="store_true", help="use an in-memory fake device"
    )
    args = parser.parse_args(argv)
    run_gui(args.port, args.mock)
