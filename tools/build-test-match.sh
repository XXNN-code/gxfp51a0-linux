#!/bin/sh
# Build the offline matcher. It links the real Bozorth3 sources from the
# patched libfprint tree (so MIN_COMPUTABLE_BOZORTH_MINUTIAE matches what the
# installed library enforces) against the installed libfprint for MINDTCT.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/build/libfprint-v1.94.9"
STAGE="$ROOT/build/stage/usr/local"

[ -d "$SRC" ] || { echo "run tools/build-libfprint.sh first" >&2; exit 1; }
[ -f "$STAGE/include/libfprint-2/fprint.h" ] || { echo "libfprint is not staged" >&2; exit 1; }

mkdir -p "$ROOT/build"
gcc -O2 -g -o "$ROOT/build/test-match" \
    "$ROOT/tools/test-match.c" \
    "$SRC"/libfprint/nbis/bozorth3/*.c \
    -I"$STAGE/include/libfprint-2" \
    -I"$SRC/libfprint/nbis/include" \
    -I"$SRC/libfprint" \
    $(pkg-config --cflags gio-2.0) \
    -L"$STAGE/lib/x86_64-linux-gnu" -lfprint-2 \
    $(pkg-config --libs gio-2.0) -lm

echo "built build/test-match"
echo "run with: LD_LIBRARY_PATH=$STAGE/lib/x86_64-linux-gnu ./build/test-match <captures>"
