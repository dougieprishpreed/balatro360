#!/bin/sh
# Checks that math.random & co. match LuaJIT exactly.
#   tools/test_rng.sh [path/to/b360] [runner]   e.g. tools/test_rng.sh out/ppc/b360 qemu-ppc
set -e
cd "$(dirname "$0")/.."
BIN=${1:-out/host/b360}
RUNNER=${2:-}
TMP=$(mktemp -d)
B360_GAME_DIR=tests/rng B360_SAVE_DIR=$TMP $RUNNER $BIN 2>&1 | grep -v '^\[\|^Balatro360' > $TMP/out.txt
if diff -u tests/rng/expected_luajit.txt $TMP/out.txt; then echo "rng: OK ($BIN)"; else echo "rng: MISMATCH ($BIN)"; exit 1; fi
rm -rf $TMP
