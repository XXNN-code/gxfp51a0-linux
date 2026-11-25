#!/usr/bin/env python3
"""Decode a stored GET_IMAGE plaintext into a PGM at the sensor's full 88x80.

The driver only copies the 80x64 corner of the wire raster on the assumption
that the rest is padding. This decoder ignores that assumption so the whole
raster can be eyeballed: if the 8 extra columns and 16 extra rows carry real
ridges, the ROI is 27% larger than what the driver feeds to MINDTCT.

    ./tools/frame2pgm.py plain-finger.bin -o /tmp/wire.pgm --transpose
"""

import argparse
import sys

WIRE_W = 88
WIRE_H = 80


def unpack4(b):
    p0 = ((b[0] & 0x0F) << 8) | b[1]
    p1 = (b[3] << 4) | (b[0] >> 4)
    p2 = ((b[5] & 0x0F) << 8) | b[2]
    p3 = (b[4] << 4) | (b[5] >> 4)
    return p0, p1, p2, p3


def decode(path, header, tail):
    raw = open(path, "rb").read()
    end = len(raw) - tail if tail else len(raw)
    packed = raw[header:end]
    wire = []
    for i in range(0, len(packed) - 5, 6):
        wire.extend(unpack4(packed[i : i + 6]))
    return wire


def to_matrix(wire, transpose):
    if transpose:
        # wire[column * 88 + row]: 80 columns of 88 samples each. The driver
        # takes rows 0..63 of every column, so the extra 24 rows per column
        # (16 in the transposed view) are what this script is here to show.
        return (
            [wire[c * WIRE_W + r] for r in range(WIRE_W)] for c in range(WIRE_H)
        ), WIRE_W, WIRE_H
    return (
        [wire[y * WIRE_W : y * WIRE_W + WIRE_W] for y in range(WIRE_H)],
        WIRE_W,
        WIRE_H,
    )


def write_pgm(path, rows, w, h):
    flat = [v for r in rows for v in r]
    lo, hi = min(flat), max(flat)
    span = hi - lo or 1
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        f.write(bytes((v - lo) * 255 // span for v in flat))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("frame")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--header", type=int, default=8)
    ap.add_argument("--tail", type=int, default=5)
    ap.add_argument("--transpose", action="store_true")
    args = ap.parse_args(argv)

    wire = decode(args.frame, args.header, args.tail)
    if len(wire) != WIRE_W * WIRE_H:
        print("decoded %d samples, expected %d" % (len(wire), WIRE_W * WIRE_H))
        return 1
    rows, w, h = to_matrix(wire, args.transpose)
    write_pgm(args.out, rows, w, h)
    print("%s -> %s (%dx%d)" % (args.frame, args.out, w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
