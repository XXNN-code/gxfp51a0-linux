#!/usr/bin/env python3
"""Convert the PGM files gx51-probe writes into upscaled PNGs.

No third-party packages: PGM is read by hand and the PNG is built from zlib
chunks, so this runs on a bare Python 3 install.

    ./tools/pgm2png.py bg.pgm finger.pgm finger-diff.pgm
    ./tools/pgm2png.py --scale 12 finger.pgm
"""

import argparse
import struct
import sys
import zlib


def read_pgm(path):
    data = open(path, "rb").read()
    fields = []
    pos = 0
    while len(fields) < 4:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while pos < len(data) and data[pos : pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    if fields[0] != b"P5":
        raise ValueError("%s: not a binary PGM (P5)" % path)
    width, height = int(fields[1]), int(fields[2])
    pixels = data[pos : pos + width * height]
    if len(pixels) != width * height:
        raise ValueError("%s: short pixel data" % path)
    return width, height, pixels


def write_png(path, width, height, pixels, scale, contrast):
    out_w, out_h = width * scale, height * scale
    if contrast:
        pixels = stretch(pixels)
    rows = bytearray()
    for y in range(out_h):
        rows.append(0)  # filter type: none
        line = pixels[(y // scale) * width : (y // scale) * width + width]
        for x in range(out_w):
            rows.append(line[x // scale])

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    blob = b"\x89PNG\r\n\x1a\n"
    blob += chunk(b"IHDR", struct.pack(">IIBBBBB", out_w, out_h, 8, 0, 0, 0, 0))
    blob += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    blob += chunk(b"IEND", b"")
    open(path, "wb").write(blob)
    return out_w, out_h, len(blob)


def stretch(line):
    lo, hi = min(line), max(line)
    if hi <= lo:
        return line
    return bytes(0 if v <= lo else 255 if v >= hi else (v - lo) * 255 // (hi - lo)
                 for v in line)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pgm", nargs="+")
    ap.add_argument("--scale", type=int, default=8)
    ap.add_argument("--no-contrast", action="store_true")
    args = ap.parse_args()

    for path in args.pgm:
        width, height, pixels = read_pgm(path)
        png = path.rsplit(".", 1)[0] + ".png"
        w, h, size = write_png(png, width, height, pixels, args.scale,
                              not args.no_contrast)
        print("%s -> %s (%dx%d, %d bytes)" % (path, png, w, h, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
