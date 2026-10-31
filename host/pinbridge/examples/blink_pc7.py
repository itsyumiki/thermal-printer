"""Blink the confirmed PC7 LED; stop with Ctrl-C."""

import time

from pinbridge import Board

with Board() as b:
    b.mod("led", "lo")
    try:
        level = 0
        while True:
            level ^= 1
            b.set(led=level)
            time.sleep(0.5)
    finally:
        b.rst()
