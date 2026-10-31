#!/bin/sh
# Copy the shared uart-bridge core into every app that uses it (byte-identical copies).
# Edit firmware/shared/uart-bridge/ only, then run this, then scripts/check-shared-core.sh.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SH=$ROOT/firmware/shared/uart-bridge
for app in ble-uart-serial classic-spp-uart-serial; do
    d=$ROOT/firmware/apps/$app/src
    [ -d "$ROOT/firmware/apps/$app" ] || continue
    mkdir -p "$d"
    cp "$SH/uart_bridge.c" "$SH/uart_bridge.h" "$d/"
    echo "synced -> $d"
done
