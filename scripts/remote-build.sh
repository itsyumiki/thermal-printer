#!/bin/sh
# Build an app on the Linux build host from this (Mac) checkout and fetch the result.
#
#   scripts/remote-build.sh <app name>        e.g. blink-pc7
#
# Result: build/<app>/{app.bin,sdk.elf,sdk.map,build.log,BUILD-INFO.txt}
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
. "$HERE/env.sh"
APP=${1:?usage: remote-build.sh <app name>}

[ -d "$ROOT/firmware/apps/$APP" ] || { echo "no such app: firmware/apps/$APP" >&2; exit 1; }

echo "== sync firmware/ and scripts/ to $BUILD_HOST:$REMOTE_DIR"
# Replace, don't merge: a stale file left from an older layout would silently win.
ssh "$BUILD_HOST" "mkdir -p '$REMOTE_DIR' && rm -rf '$REMOTE_DIR/firmware' '$REMOTE_DIR/scripts'"
(cd "$ROOT" && COPYFILE_DISABLE=1 tar cf - firmware scripts) |
    ssh "$BUILD_HOST" tar xf - -C "$REMOTE_DIR" --warning=no-unknown-keyword

echo "== build on $BUILD_HOST"
ssh "$BUILD_HOST" sh "$REMOTE_DIR/scripts/build-app.sh" "$APP" "$REMOTE_DIR/out/$APP"

echo "== fetch build/$APP"
mkdir -p "$ROOT/build/$APP"
ssh "$BUILD_HOST" tar cf - -C "$REMOTE_DIR/out/$APP" . | tar xf - -C "$ROOT/build/$APP"
ls -l "$ROOT/build/$APP"
