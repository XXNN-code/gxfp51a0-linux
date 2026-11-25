#!/bin/sh
# Capture a labelled batch of frames for offline scoring.
#
#   sudo ./tools/capture-group.sh index 10    # a finger that will be enrolled
#   sudo ./tools/capture-group.sh other 5     # a finger that must NOT match
#   sudo AVG=4 ./tools/capture-group.sh index3 5   # average 4 exposures/press
#
# Frames land in captures/<group>/<group>-NN.pgm, which is the naming
# tools/test-match relies on to tell genuine pairs from impostor ones.
# fprintd holds the spidev and the reset GPIO, so it is stopped for the
# duration and restarted on exit.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
GROUP=${1:?usage: $0 <group> <frames>}
FRAMES=${2:-5}
DIR="$ROOT/captures/$GROUP"

[ "$(id -u)" = 0 ] || { echo "run me with sudo" >&2; exit 1; }
[ -x "$ROOT/build/gx51-probe" ] || { echo "build first: make" >&2; exit 1; }

mkdir -p "$DIR" "$DIR/junk"
cd "$DIR"

restart_fprintd=0
if systemctl is-active --quiet fprintd; then
    systemctl stop fprintd
    restart_fprintd=1
fi
trap '[ "$restart_fprintd" = 1 ] && systemctl start fprintd' EXIT INT TERM

echo "== capturing $FRAMES frames into $DIR =="
echo "== group name: $GROUP (used as the label in tools/test-match) =="
"$ROOT/build/gx51-probe" --tls-image --frames "$FRAMES" --avg "${AVG:-1}"

# probe names its output finger-NN.pgm regardless of the group, and also
# leaves the last frame plus background derivatives behind; keep everything
# out of the <group>-NN namespace except the real captures.
next=1
for f in $(ls finger-*.pgm 2>/dev/null | sort); do
    while [ -e "$(printf '%s-%02d.pgm' "$GROUP" "$next")" ]; do
        next=$((next + 1))
    done
    mv "$f" "$(printf '%s-%02d.pgm' "$GROUP" "$next")"
    echo "== $(printf '%s-%02d.pgm' "$GROUP" "$next") =="
    next=$((next + 1))
done
for f in finger.pgm finger-diff.pgm bg.pgm; do
    [ -e "$f" ] && mv "$f" junk/
done

echo "== $DIR now holds: =="
ls "$DIR"/*.pgm 2>/dev/null | wc -l
