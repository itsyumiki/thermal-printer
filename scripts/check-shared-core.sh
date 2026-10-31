#!/bin/sh
# Verify the uart-bridge core is byte-identical in the shared dir and in every app's src/.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SH=$ROOT/firmware/shared/uart-bridge
rc=0
for f in uart_bridge.c uart_bridge.h; do
    ref=$(sha256sum "$SH/$f" | cut -d' ' -f1)
    echo "$ref  shared/$f"
    for app in ble-uart-serial classic-spp-uart-serial; do
        p=$ROOT/firmware/apps/$app/src/$f
        [ -f "$p" ] || { echo "(absent)  $app/src/$f"; continue; }
        h=$(sha256sum "$p" | cut -d' ' -f1)
        if [ "$h" = "$ref" ]; then echo "$h  $app/src/$f  OK"; else echo "$h  $app/src/$f  MISMATCH"; rc=1; fi
    done
done
exit $rc
