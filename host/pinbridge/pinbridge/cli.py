import argparse
import os
import subprocess
import sys
import time
import webbrowser
from pathlib import Path

from .board import Board


def main(argv=None):
    p = argparse.ArgumentParser(prog="pinbridge")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("repl")
    r.add_argument("--port")
    r.add_argument("--timeout", type=float, default=0.5)
    for name in ("gui", "webui"):
        g = sub.add_parser(name, help="launch the local thermal printer control UI")
        gg = g.add_mutually_exclusive_group()
        gg.add_argument("--port", help="serial device path")
        gg.add_argument(
            "--mock", action="store_true", help="use an in-memory fake device"
        )
        g.add_argument(
            "--host",
            default="127.0.0.1",
            help="HTTP bind address (default: localhost only)",
        )
        g.add_argument(
            "--http-port",
            type=int,
            default=8765,
            help="HTTP/WebSocket port (default: 8765)",
        )
        g.add_argument(
            "--debug",
            action="store_true",
            help="log every raw UART frame (TX/RX, unparsed lines, drops) to stdout",
        )
        g.add_argument(
            "--no-browser",
            action="store_true",
            help="do not open a browser automatically",
        )
    a = p.parse_args(argv)
    if a.cmd in ("gui", "webui"):
        # Production UI, protocol, UART manager and the development mock all
        # live in the Bun host so the browser never has a separate Python path.
        bun_dir = Path(__file__).resolve().parents[1] / "bun"
        env = os.environ.copy()
        env["PINBRIDGE_HOST"] = a.host
        env["PINBRIDGE_PORT"] = str(a.http_port)
        if a.debug:
            env["PINBRIDGE_DEBUG"] = "1"
        if a.mock:
            env["PINBRIDGE_MOCK"] = "1"
        if a.port:
            env["PINBRIDGE_UART"] = a.port
        try:
            child = subprocess.Popen(["bun", "run", "server.ts"], cwd=bun_dir, env=env)
        except FileNotFoundError:
            p.error(
                "Bun is required for the production WebUI. Install Bun and run `bun install` in host/pinbridge/bun."
            )
        if not a.no_browser:
            time.sleep(0.8)
            webbrowser.open(f"http://{a.host}:{a.http_port}")
        try:
            child.wait()
        except KeyboardInterrupt:
            child.terminate()
            child.wait(timeout=5)
        return
    with Board(a.port, timeout=a.timeout) as b:
        print("jlpin v2 connected; type protocol commands, Ctrl-D to quit")
        while True:
            try:
                line = input("jlpin> ")
            except EOFError:
                break
            if not line.strip():
                continue
            try:
                print("ok", *b.command(line))
            except Exception as e:
                print(type(e).__name__ + ":", e, file=sys.stderr)


if __name__ == "__main__":
    main()
