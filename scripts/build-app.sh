#!/bin/sh
# Build one firmware app on the Linux build host. Run via scripts/remote-build.sh
# (or directly on the host):
#
#   sh scripts/build-app.sh <app name> [output dir]
#
# Starts from a fresh clone of the pinned SDK commit every time, applies
# firmware/apps/<app>/ (app.conf + overlay/), builds, and writes to <output dir>
# (default <repo>/out/<app>): app.bin, sdk.elf, sdk.map, build.log, BUILD-INFO.txt
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
APP=${1:?usage: build-app.sh <app name> [output dir]}
OUT=${2:-$ROOT/out/$APP}
JL_HOME=${JL_HOME:-$HOME/jl}
SDK_COMMIT=fdc018de81629e21e594d677cf05f87d288be0f0
JIELI_PREFIX=/opt/jieli
UTILS_PREFIX=/opt/utils

APPDIR=$ROOT/firmware/apps/$APP
SDK_MIRROR=$JL_HOME/fw-AC63_BT_SDK
BUILD=$JL_HOME/build/$APP

die() { printf 'build-app: %s\n' "$*" >&2; exit 1; }
[ -d "$APPDIR" ] || die "no such app: $APPDIR"
[ -d "$SDK_MIRROR/.git" ] || die "SDK clone missing, run scripts/setup-build-host.sh first"
[ -x "$JIELI_PREFIX/pi32v2/bin/clang" ] || die "toolchain missing in $JIELI_PREFIX, run scripts/setup-build-host.sh first"
[ -x "$UTILS_PREFIX/remove_tailing_zeros" ] || die "$UTILS_PREFIX/remove_tailing_zeros missing, run scripts/setup-build-host.sh first"

# The linker opens a lot of files at once.
ulimit -n 8192 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
# cpu/br23/setup.c embeds __DATE__ __TIME__, and this clang (4.0.1) ignores
# SOURCE_DATE_EPOCH. Freezing the clock makes the same sources give a byte-identical app.bin.
run_make() {
    if command -v faketime >/dev/null 2>&1; then
        faketime '2024-01-01 00:00:00' make "$@"
    else
        echo "build-app: 'faketime' not installed, app.bin will contain the build time (not reproducible)" >&2
        make "$@"
    fi
}

echo "== fresh SDK tree in $BUILD"
rm -rf "$BUILD"
mkdir -p "$JL_HOME/build"
git clone -q --shared --no-checkout "$SDK_MIRROR" "$BUILD"
git -C "$BUILD" -c advice.detachedHead=false checkout -q "$SDK_COMMIT"

echo "== applying app '$APP'"
TARGET=$(python3 "$HERE/apply_app.py" "$APPDIR" "$BUILD")
echo "   make target: $TARGET"
if [ "$APP" = pinbridge ]; then
    BUILD_SHA=${PINBRIDGE_BUILD_SHA:-${GITHUB_SHA:-}}
    if [ -z "$BUILD_SHA" ]; then BUILD_SHA=$(git -C "$ROOT" rev-parse HEAD); fi
    BUILD_ID=$(printf '%s' "$BUILD_SHA" | cut -c1-8)
    case "$BUILD_ID" in
        ''|*[!0-9a-fA-F]*) die "invalid source commit SHA for build id: $BUILD_SHA" ;;
    esac
    PB_CORE="$BUILD/apps/spp_and_le/include/pb_core.h"
    [ -f "$PB_CORE" ] || die "applied pb_core.h missing: $PB_CORE"
    sed -i "s/^#define PB_BUILD_ID 0u.*/#define PB_BUILD_ID 0x${BUILD_ID}u/" "$PB_CORE"
    echo "   build id: 0x$(printf '%s' "$BUILD_ID" | tr '[:lower:]' '[:upper:]') (SHA ${BUILD_SHA})"
fi

mkdir -p "$OUT"
LOG=$OUT/build.log
echo "== building (log: $LOG)"
# RUN_POST_SCRIPT=true skips the SDK's own post-build step. That step needs the
# vendor 'host-client' service and fails here; make-app-bin.sh does the part we need.
if ! run_make -C "$BUILD" "$TARGET" RUN_POST_SCRIPT=true >"$LOG" 2>&1; then
    tail -40 "$LOG" >&2
    die "make failed, full log in $LOG"
fi
TOOLS=$BUILD/cpu/br23/tools
[ -f "$TOOLS/sdk.elf" ] || die "sdk.elf was not produced, see $LOG"

echo "== app.bin"
sh "$HERE/make-app-bin.sh" "$TOOLS" "$OUT/app.bin"

cp "$TOOLS/sdk.elf" "$TOOLS/sdk.map" "$OUT/"
{
    echo "app:           $APP"
    echo "built:         $(date -u +%FT%TZ)"
    echo "sdk commit:    $SDK_COMMIT"
    if [ "$APP" = pinbridge ]; then
        echo "source commit: ${BUILD_SHA:-unknown}"
        echo "build id:      0x$(printf '%s' "${BUILD_ID:-00000000}" | tr '[:lower:]' '[:upper:]')"
    fi
    echo "make target:   $TARGET"
    echo "compiler:      $("$JIELI_PREFIX/pi32v2/bin/clang" --version | head -1)"
    echo "app.bin bytes: $(wc -c <"$OUT/app.bin")"
    echo "app.bin sha256: $(sha256sum "$OUT/app.bin" | cut -d' ' -f1)"
    echo "sdk.elf sha256: $(sha256sum "$OUT/sdk.elf" | cut -d' ' -f1)"
    echo "app.conf sha256: $(sha256sum "$APPDIR/app.conf" | cut -d' ' -f1)"
} >"$OUT/BUILD-INFO.txt"
cat "$OUT/BUILD-INFO.txt"
