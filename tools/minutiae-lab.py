#!/usr/bin/env python3
"""Offline parameter lab for the GX51A0 minutiae problem.

MINDTCT reports "No minutiae found" on the native 80x64 frame. Before touching
the driver, this script writes a set of upscaled / pre-smoothed variants of the
stored captures into /tmp so tools/test-minutiae can be run over all of them and
the variant with the best detection rate wins.

    ./tools/minutiae-lab.py finger-*.pgm
    LD_LIBRARY_PATH=build/stage/usr/local/lib/x86_64-linux-gnu \
        ./build/test-minutiae /tmp/gx51lab/*.pgm
"""

import os
import sys

OUT = "/tmp/gx51lab"


def read_pgm(path):
    data = open(path, "rb").read()
    fields, pos = [], 0
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
    assert fields[0] == b"P5", "%s: not P5" % path
    w, h = int(fields[1]), int(fields[2])
    return w, h, list(data[pos : pos + w * h])


def write_pgm(path, w, h, px):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        f.write(bytes(max(0, min(255, int(round(p)))) for p in px))


def nearest(px, w, h, f):
    return [px[(y // f) * w + x // f] for y in range(h * f) for x in range(w * f)]


def bilinear(px, w, h, f):
    ow, oh = w * f, h * f
    out = []
    for y in range(oh):
        sy = (y + 0.5) / f - 0.5
        y0 = int(sy // 1)
        fy = sy - y0
        if y0 < 0:
            y0, fy = 0, 0.0
        y1 = min(y0 + 1, h - 1)
        for x in range(ow):
            sx = (x + 0.5) / f - 0.5
            x0 = int(sx // 1)
            fx = sx - x0
            if x0 < 0:
                x0, fx = 0, 0.0
            x1 = min(x0 + 1, w - 1)
            p00 = px[y0 * w + x0]
            p01 = px[y0 * w + x1]
            p10 = px[y1 * w + x0]
            p11 = px[y1 * w + x1]
            top = p00 + (p01 - p00) * fx
            bot = p10 + (p11 - p10) * fx
            out.append(top + (bot - top) * fy)
    return out


def gauss3(px, w, h, passes=1):
    """Separable 1-2-1 blur; smooths the pixel grid so the binariser sees
    continuous ridges instead of the sensor's blocky sampling."""
    cur = [float(p) for p in px]
    for _ in range(passes):
        tmp = [0.0] * (w * h)
        for y in range(h):
            for x in range(w):
                l = cur[y * w + max(x - 1, 0)]
                r = cur[y * w + min(x + 1, w - 1)]
                tmp[y * w + x] = (l + 2 * cur[y * w + x] + r) / 4.0
        out = [0.0] * (w * h)
        for y in range(h):
            up = tmp[max(y - 1, 0) * w : max(y - 1, 0) * w + w]
            mid = tmp[y * w : y * w + w]
            dn = tmp[min(y + 1, h - 1) * w : min(y + 1, h - 1) * w + w]
            for x in range(w):
                out[y * w + x] = (up[x] + 2 * mid[x] + dn[x]) / 4.0
        cur = out
    return cur


def local_equalize(px, w, h, win=16):
    """Per-window min/max stretch: the sensor's illumination falls off towards
    the edges, which leaves ridges there below MINDTCT's contrast floor."""
    out = [0.0] * (w * h)
    r = win // 2
    for y in range(h):
        for x in range(w):
            ys, ye = max(y - r, 0), min(y + r + 1, h)
            xs, xe = max(x - r, 0), min(x + r + 1, w)
            lo, hi = 255.0, 0.0
            for yy in range(ys, ye):
                row = yy * w
                for xx in range(xs, xe):
                    v = px[row + xx]
                    if v < lo:
                        lo = v
                    if v > hi:
                        hi = v
            out[y * w + x] = 0.0 if hi <= lo else (px[y * w + x] - lo) * 255.0 / (hi - lo)
    return out


def main(argv):
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    for path in argv:
        w, h, px = read_pgm(path)
        stem = os.path.basename(path).rsplit(".", 1)[0]
        for f in (2, 3):
            base = bilinear(px, w, h, f)
            ow, oh = w * f, h * f
            write_pgm("%s/%s-b%d.pgm" % (OUT, stem, f), ow, oh, base)
            write_pgm("%s/%s-b%ds.pgm" % (OUT, stem, f),
                      ow, oh, gauss3(base, ow, oh))
            eq = local_equalize(base, ow, oh)
            write_pgm("%s/%s-b%de.pgm" % (OUT, stem, f), ow, oh, eq)
            write_pgm("%s/%s-b%des.pgm" % (OUT, stem, f),
                      ow, oh, gauss3(eq, ow, oh))
        write_pgm("%s/%s-n2.pgm" % (OUT, stem), w * 2, h * 2, nearest(px, w, h, 2))
    print("wrote variants to %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
