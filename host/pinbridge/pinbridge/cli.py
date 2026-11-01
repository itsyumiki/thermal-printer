import argparse
import sys

from .board import Board


def main(argv=None):
    p = argparse.ArgumentParser(prog="pinbridge")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("repl")
    r.add_argument("--port")
    r.add_argument("--timeout", type=float, default=0.5)
    for name in ("gui", "webui"):
        g = sub.add_parser(name, help="launch the local browser-based GPIO console")
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
            "--no-browser",
            action="store_true",
            help="do not open a browser automatically",
        )
    a = p.parse_args(argv)
    if a.cmd in ("gui", "webui"):
        try:
            from .webui import run_webui
        except ImportError as e:
            p.error(
                f"WebUI dependencies unavailable; reinstall pinbridge with its declared dependencies: {e}"
            )
        run_webui(
            a.port,
            a.mock,
            host=a.host,
            http_port=a.http_port,
            open_browser=not a.no_browser,
        )
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
