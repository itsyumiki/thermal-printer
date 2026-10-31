import argparse
import sys

from .board import Board


def main(argv=None):
    p = argparse.ArgumentParser(prog="pinbridge")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("repl")
    r.add_argument("--port")
    r.add_argument("--timeout", type=float, default=0.5)
    a = p.parse_args(argv)
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
