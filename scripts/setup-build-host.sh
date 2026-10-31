#!/bin/sh
# One-time (idempotent) setup of the Linux build/flash host.
#
#   ssh karna sh -s < scripts/setup-build-host.sh
#
# Needs: Debian/Ubuntu-like Linux on x86_64, sudo, network access.
# Installs: build packages, the JieLi toolchain (/opt/jieli), the post-build
# helper (/opt/utils/remove_tailing_zeros), a pinned clone of the SDK and a
# Python venv for the flashing tools (~/jl/venv).
set -eu

# This script may arrive on stdin, so it can't source env.sh; keep in sync.
JL_HOME=${JL_HOME:-$HOME/jl}
SDK_URL=https://github.com/Jieli-Tech/fw-AC63_BT_SDK.git
SDK_COMMIT=fdc018de81629e21e594d677cf05f87d288be0f0
TOOLCHAIN_URL=https://pkgman.jieliapp.com/s/linux-toolchain
TOOLCHAIN_SHA256=a1545cddbc451a06bac43eb5cd96d831470c42a97c5c0b1f98879a20fc7647bd
POSTBUILD_URL=https://pkgman.jieliapp.com/s/linux-postbuild
POSTBUILD_SHA256=6949d48c4cd82e274d7943d4ca67f214593fdb4f699ef9f51c2ecc78125ab739
JIELI_PREFIX=/opt/jieli
UTILS_PREFIX=/opt/utils

say() { printf '== %s\n' "$*"; }
die() { printf 'setup-build-host: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = Linux ] || die "needs Linux (the JieLi toolchain is a Linux x86_64 binary)"
[ "$(uname -m)" = x86_64 ] || die "needs x86_64, this is $(uname -m)"
sudo -n true 2>/dev/null || die "needs passwordless sudo (or run the sudo steps by hand)"

say "packages"
if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get install -y git make patch python3 python3-venv wget xz-utils faketime >/dev/null
else
    die "no apt-get; install git make patch python3 python3-venv wget xz-utils faketime yourself and re-run"
fi

mkdir -p "$JL_HOME/dl"

fetch() { # url sha256 file
    if [ -f "$3" ] && [ "$(sha256sum "$3" | cut -d' ' -f1)" = "$2" ]; then return 0; fi
    say "download $1"
    wget -q -O "$3" "$1"
    got=$(sha256sum "$3" | cut -d' ' -f1)
    [ "$got" = "$2" ] || die "sha256 mismatch for $1: got $got, expected $2 (upstream changed? see docs/build-system.md)"
}

say "toolchain"
if [ ! -x "$JIELI_PREFIX/pi32v2/bin/clang" ]; then
    fetch "$TOOLCHAIN_URL" "$TOOLCHAIN_SHA256" "$JL_HOME/dl/toolchain.tar.xz"
    sudo mkdir -p "$JIELI_PREFIX"
    sudo tar xJf "$JL_HOME/dl/toolchain.tar.xz" --strip-components=1 -C "$JIELI_PREFIX"
fi
[ -x "$JIELI_PREFIX/pi32v2/bin/clang" ] || die "toolchain install failed"

say "post-build helper"
if [ ! -x "$UTILS_PREFIX/remove_tailing_zeros" ]; then
    fetch "$POSTBUILD_URL" "$POSTBUILD_SHA256" "$JL_HOME/dl/postbuild.tar.xz"
    sudo mkdir -p "$UTILS_PREFIX"
    sudo tar xJf "$JL_HOME/dl/postbuild.tar.xz" --strip-components=1 -C "$UTILS_PREFIX"
fi
[ -x "$UTILS_PREFIX/remove_tailing_zeros" ] || die "post-build helper install failed"

say "SDK clone @ $SDK_COMMIT"
if [ ! -d "$JL_HOME/fw-AC63_BT_SDK/.git" ]; then
    git clone -q --no-checkout "$SDK_URL" "$JL_HOME/fw-AC63_BT_SDK"
fi
git -C "$JL_HOME/fw-AC63_BT_SDK" cat-file -e "$SDK_COMMIT^{commit}" 2>/dev/null ||
    git -C "$JL_HOME/fw-AC63_BT_SDK" fetch -q origin
git -C "$JL_HOME/fw-AC63_BT_SDK" cat-file -e "$SDK_COMMIT^{commit}" ||
    die "commit $SDK_COMMIT not found in $SDK_URL"

say "python venv for the flashing tools"
if [ ! -x "$JL_HOME/venv/bin/python" ]; then
    python3 -m venv "$JL_HOME/venv"
fi
"$JL_HOME/venv/bin/pip" install -q crcmod pyyaml tqdm pycryptodomex

say "versions"
"$JIELI_PREFIX/pi32v2/bin/clang" --version | head -1
git -C "$JL_HOME/fw-AC63_BT_SDK" log -1 --format='SDK commit %h (%s)' "$SDK_COMMIT"
"$JL_HOME/venv/bin/python" --version
say "done. JL_HOME=$JL_HOME"
