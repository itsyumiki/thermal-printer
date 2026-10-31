#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
UPDATE=0
if [ "${1:-}" = "--update" ]; then UPDATE=1; elif [ "$#" -ne 0 ]; then echo "usage: $0 [--update]" >&2; exit 2; fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
GEN=firmware/apps/pinbridge/tools/gen_pins.py
REAL=firmware/apps/pinbridge/pins.toml
TEST=firmware/apps/pinbridge/tests/pins_test.toml
python3 "$GEN" "$REAL" --c "$TMP/pb_pins.h" --py "$TMP/pins_gen.py" --md "$TMP/pins_table.md"
python3 "$GEN" "$TEST" --c "$TMP/pb_pins_test.h" --py "$TMP/pins_test_gen.py"
check() {
  src=$1 dst=$2
  if [ "$UPDATE" = 1 ]; then cp "$src" "$dst"; elif ! diff -u "$dst" "$src"; then echo "stale generated file: $dst (run scripts/check-pinbridge.sh --update)" >&2; return 1; fi
}
check "$TMP/pb_pins.h" firmware/apps/pinbridge/src/pb_pins.h
check "$TMP/pins_gen.py" host/pinbridge/pinbridge/pins_gen.py
check "$TMP/pins_table.md" firmware/apps/pinbridge/pins_table.md
check "$TMP/pb_pins_test.h" firmware/apps/pinbridge/tests/c/pb_pins_test.h
check "$TMP/pins_test_gen.py" host/pinbridge/pinbridge/pins_test_gen.py
test -d firmware/apps/pinbridge/tests/transcripts
echo "pinbridge generated files: OK"
