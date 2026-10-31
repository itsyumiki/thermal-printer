#!/usr/bin/env python3
"""Apply a firmware app definition (firmware/apps/<name>) to a checked-out SDK tree.

    apply_app.py <app dir> <sdk dir>

The app definition is firmware/apps/<name>/app.conf plus the source files it
refers to (conventionally under src/). Directives: target, set, copy (see the
app.conf of blink-pc7 for an annotated example).

Prints the make target of the app on stdout (last line) so build scripts can use it.
Everything else goes to stderr. Fails loudly if a `set` line does not match
exactly one #define, so a changed SDK can't silently build the wrong thing.
"""

import re
import shutil
import sys
from pathlib import Path


def fail(msg):
    sys.exit(f"apply_app: {msg}")


def set_define(path, macro, value):
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        rf"^(?P<head>[ \t]*#[ \t]*define[ \t]+{re.escape(macro)}[ \t]+)(?P<old>[^\s/]+)(?P<tail>.*)$",
        re.MULTILINE,
    )
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        fail(f"{path}: expected exactly one '#define {macro}', found {len(matches)}")
    m = matches[0]
    new_line = f"{m['head']}{value}{m['tail']}"
    path.write_text(text[: m.start()] + new_line + text[m.end() :], encoding="utf-8")
    print(f"  set {path.name}: {macro} {m['old']} -> {value}", file=sys.stderr)


def main():
    if len(sys.argv) != 3:
        fail("usage: apply_app.py <app dir> <sdk dir>")
    app, sdk = Path(sys.argv[1]), Path(sys.argv[2])
    conf = app / "app.conf"
    if not conf.is_file():
        fail(f"{conf} not found")
    if not (sdk / "Makefile").is_file():
        fail(f"{sdk} does not look like the SDK")

    target = None
    copies = []
    for n, raw in enumerate(conf.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = line.split()
        if words[0] == "target" and len(words) == 2:
            target = words[1]
        elif words[0] == "set" and len(words) == 4:
            f = sdk / words[1]
            if not f.is_file():
                fail(f"{conf}:{n}: {words[1]} does not exist in the SDK")
            set_define(f, words[2], words[3])
        elif words[0] == "copy" and len(words) == 3:
            copies.append((n, app / words[1], sdk / words[2], words[2]))
        else:
            fail(f"{conf}:{n}: cannot parse {raw!r}")
    if target is None:
        fail(f"{conf}: no 'target' line")

    for n, src, dst, shown in copies:
        if not src.is_file():
            fail(f"{conf}:{n}: {src} does not exist")
        print(f"  copy {src.relative_to(app)} -> {shown}"
              f"{'' if dst.exists() else ' (new file)'}", file=sys.stderr)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)

    overlay = app / "overlay"
    if overlay.is_dir():
        for src in sorted(p for p in overlay.rglob("*") if p.is_file()):
            rel = src.relative_to(overlay)
            dst = sdk / rel
            if not dst.exists():
                print(f"  overlay adds new file {rel}", file=sys.stderr)
            else:
                print(f"  overlay replaces {rel}", file=sys.stderr)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)

    print(target)


if __name__ == "__main__":
    main()
