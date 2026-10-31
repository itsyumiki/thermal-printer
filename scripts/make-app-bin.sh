#!/bin/sh
# Turn sdk.elf into app.bin: the same sections, in the same order, as the
# SDK's own cpu/br23/tools/download.sh (which we can't use, see docs/build-system.md).
#
#   sh scripts/make-app-bin.sh <dir containing sdk.elf> <output app.bin>
set -eu

DIR=${1:?usage: make-app-bin.sh <dir with sdk.elf> <app.bin>}
OUT=$(realpath -m "${2:?usage: make-app-bin.sh <dir with sdk.elf> <app.bin>}")
OBJCOPY=${OBJCOPY:-/opt/jieli/pi32v2/bin/objcopy}
RTZ=${RTZ:-/opt/utils/remove_tailing_zeros}

cd "$DIR"
[ -f sdk.elf ] || { echo "make-app-bin: $DIR/sdk.elf missing" >&2; exit 1; }

$OBJCOPY -O binary -j .text      sdk.elf text.bin
$OBJCOPY -O binary -j .data      sdk.elf data.bin
$OBJCOPY -O binary -j .data_code sdk.elf data_code.bin

for pair in aec:aeco fm:fmo mp3:mp3o wma:wmao; do
    name=${pair%%:*}; raw=${pair##*:}
    $OBJCOPY -O binary -j ".overlay_$name" sdk.elf "$raw.bin"
    $RTZ -i "$raw.bin" -o "$name.bin" -mark ff
done
for name in wav ape flac m4a amr dts; do
    $OBJCOPY -O binary -j ".overlay_$name" sdk.elf "$name.bin"
done

# Overlay banks only exist in apps that use them; this firmware has none.
if $OBJCOPY -O binary -j .overlay_bank0 sdk.elf bank0.bin 2>/dev/null && [ -s bank0.bin ]; then
    echo "make-app-bin: sdk.elf has overlay banks; this script does not handle them" >&2
    exit 1
fi

cat text.bin data.bin data_code.bin aec.bin wav.bin ape.bin flac.bin m4a.bin \
    amr.bin dts.bin fm.bin mp3.bin wma.bin >"$OUT"
echo "   text $(wc -c <text.bin) + data $(wc -c <data.bin) -> $OUT ($(wc -c <"$OUT") bytes)"
