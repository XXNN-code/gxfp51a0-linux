#!/usr/bin/env python3
"""Align a set of captures by translation and average them.

The single-frame noise floor is what breaks ridges: a press yields 8-10
minutiae, and the ones that are missing are usually at points where the
ridge broke up. Averaging N *aligned* frames should recover them, whereas
averaging unaligned ones (probe --avg) smears the ridges instead.

Usage:
  ./tools/align-avg.py captures/index/finger-*.pgm -o /tmp/aligned.pgm
  ./tools/align-avg.py ... --mode mosaic    # union of coverage, not intersection
"""

import sys
import os

MAX_SHIFT = 14


def read_pgm(path):
    d = open(path, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3][: w * h]
    return w, h, [[px[y * w + x] for x in range(w)] for y in range(h)]


def write_pgm(path, w, h, rows):
    flat = [v for r in rows for v in r]
    s = sorted(flat)
    n = len(s)
    lo, hi = s[n // 100], s[n * 99 // 100]
    if hi <= lo:
        lo, hi = s[0], s[-1]
    span = (hi - lo) or 1
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        f.write(bytes(max(0, min(255, int((v - lo) * 255 / span))) for v in flat))


def normalise(rows):
    flat = [v for r in rows for v in r]
    mean = sum(flat) / len(flat)
    var = sum((v - mean) ** 2 for v in flat) / len(flat)
    sd = var ** 0.5 or 1.0
    return [[(v - mean) / sd for v in r] for r in rows]


def ncc(a, b, dx, dy, w, h, step=2):
    """Correlation of a against b shifted by (dx,dy)."""
    s = 0.0
    n = 0
    for y in range(0, h, step):
        yy = y + dy
        if yy < 0 or yy >= h:
            continue
        ra = a[y]
        rb = b[yy]
        for x in range(0, w - 0, step):
            xx = x + dx
            if xx < 0 or xx >= w:
                continue
            s += ra[x] * rb[xx]
            n += 1
    return s / n if n else -2.0


def best_shift(ref, img, w, h):
    best = (-2.0, 0, 0)
    for dy in range(-MAX_SHIFT, MAX_SHIFT + 1, 2):
        for dx in range(-MAX_SHIFT, MAX_SHIFT + 1, 2):
            c = ncc(ref, img, dx, dy, w, h)
            if c > best[0]:
                best = (c, dx, dy)
    c0, cx, cy = best
    for dy in range(cy - 1, cy + 2):
        for dx in range(cx - 1, cx + 2):
            c = ncc(ref, img, dx, dy, w, h, step=1)
            if c > c0:
                c0, best = c, (c, dx, dy)
    return best


def main(argv):
    paths = []
    out = "/tmp/aligned.pgm"
    i = 1
    while i < len(argv):
        if argv[i] == "-o" and i + 1 < len(argv):
            out = argv[i + 1]
            i += 2
            continue
        if not argv[i].startswith("-"):
            paths.append(argv[i])
        i += 1
    if len(paths) < 2:
        print("need at least two pgm files", file=sys.stderr)
        return 2

    imgs = []
    for p in paths:
        w, h, rows = read_pgm(p)
        imgs.append((w, h, rows))
    w, h = imgs[0][0], imgs[0][1]

    norm = [normalise(rows) for _, _, rows in imgs]
    ref = norm[len(norm) // 2]
    ref_name = os.path.basename(paths[len(paths) // 2])

    shifts = []
    for p, n in zip(paths, norm):
        c, dx, dy = best_shift(ref, n, w, h)
        shifts.append((dx, dy, c))
        print("%-28s shift (%+d,%+d) corr %.3f" %
              (os.path.basename(p), dx, dy, c))

    # Keep frames that actually overlap the reference; a frame that had to move
    # more than half the window is a different part of the finger, and blending
    # it in only adds ghosting.
    keep = [i for i, s in enumerate(shifts) if abs(s[0]) <= MAX_SHIFT and s[1] < 99]
    print("keeping %d of %d frames" % (len(keep), len(imgs)))

    acc = [[0.0] * w for _ in range(h)]
    cnt = [[0] * w for _ in range(h)]
    for i in keep:
        dx, dy, _ = shifts[i]
        rows = imgs[i][2]
        for y in range(h):
            yy = y + dy
            if yy < 0 or yy >= h:
                continue
            for x in range(w):
                xx = x + dx
                if xx < 0 or xx >= w:
                    continue
                acc[yy][xx] += rows[y][x]
                cnt[yy][xx] += 1

    out_rows = []
    gaps = 0
    for y in range(h):
        r = []
        for x in range(w):
            if cnt[y][x]:
                r.append(acc[y][x] / cnt[y][x])
            else:
                r.append(0.0)
                gaps += 1
        out_rows.append(r)
    print("coverage holes: %d px (%.1f%%)" % (gaps, 100.0 * gaps / (w * h)))

    if gaps:
        # Fill holes with the covered mean so MINDTCT sees a flat background
        # instead of a black border, which it would cull as a frame edge.
        flat = [v for r, c in zip(out_rows, cnt) for v, cc in zip(r, c) if cc]
        fill = sum(flat) / len(flat) if flat else 0.0
        for y in range(h):
            for x in range(w):
                if not cnt[y][x]:
                    out_rows[y][x] = fill

    write_pgm(out, w, h, out_rows)
    print("wrote %s (%dx%d from %d aligned frames)" % (out, w, h, len(keep)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
