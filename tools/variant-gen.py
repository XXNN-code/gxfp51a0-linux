"""Generate image-processing variants of stored captures for MINDTCT testing.

The sensor yields 8-10 minutiae per frame at 80x64, which is too few for
Bozorth3 to separate a genuine print from an impostor one. This script builds
candidate pre-processings (padding, blur, local contrast, resampling) so the
count can be measured before any of them is wired into the driver.

    python3 tools/variant-gen.py captures/index/finger-01.pgm -o /tmp/var
"""
import math
import os
import sys


def read_pgm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields = []
    i = 0
    while len(fields) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1
    w, h = int(fields[1]), int(fields[2])
    return w, h, list(data[i:i + w * h])


def write_pgm(path, w, h, px):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        f.write(bytes(max(0, min(255, int(round(p)))) for p in px))


def pad(px, w, h, size, fill=None):
    mean = sum(px) // len(px) if fill is None else fill
    out = [mean] * (size * size)
    ox, oy = (size - w) // 2, (size - h) // 2
    for y in range(h):
        row = px[y * w:(y + 1) * w]
        out[(oy + y) * size + ox:(oy + y) * size + ox + w] = row
    return out, size, size


def bilinear(px, w, h, factor):
    ow, oh = w * factor, h * factor
    out = [0] * (ow * oh)
    for y in range(oh):
        sy = (y + 0.5) / factor - 0.5
        y0 = max(0, min(h - 1, int(math.floor(sy))))
        y1 = min(h - 1, y0 + 1)
        fy = max(0.0, min(1.0, sy - y0))
        for x in range(ow):
            sx = (x + 0.5) / factor - 0.5
            x0 = max(0, min(w - 1, int(math.floor(sx))))
            x1 = min(w - 1, x0 + 1)
            fx = max(0.0, min(1.0, sx - x0))
            a = px[y0 * w + x0] * (1 - fx) + px[y0 * w + x1] * fx
            b = px[y1 * w + x0] * (1 - fx) + px[y1 * w + x1] * fx
            out[y * ow + x] = a * (1 - fy) + b * fy
    return out, ow, oh


def box_blur(px, w, h, radius=1):
    tmp = [0] * (w * h)
    out = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            lo, hi = max(0, x - radius), min(w - 1, x + radius)
            s = sum(px[y * w + i] for i in range(lo, hi + 1))
            tmp[y * w + x] = s / (hi - lo + 1)
    for y in range(h):
        for x in range(w):
            lo, hi = max(0, y - radius), min(h - 1, y + radius)
            s = sum(tmp[i * w + x] for i in range(lo, hi + 1))
            out[y * w + x] = s / (hi - lo + 1)
    return out


def local_contrast(px, w, h, tile=16, strength=2.0):
    """Normalise each tile to zero mean, unit spread, then rescale.

    Fingerprint images carry a slowly varying background pedestal; MINDTCT
    works on the ridge modulation, so flattening it locally can restore blocks
    that would otherwise fall below its contrast floor.
    """
    out = list(px)
    for ty in range(0, h, tile):
        for tx in range(0, w, tile):
            idx = [(y * w + x)
                   for y in range(ty, min(ty + tile, h))
                   for x in range(tx, min(tx + tile, w))]
            vals = [px[i] for i in idx]
            mean = sum(vals) / len(vals)
            var = sum((v - mean) ** 2 for v in vals) / len(vals)
            sd = math.sqrt(var) or 1.0
            for i in idx:
                out[i] = (px[i] - mean) / sd * 32.0 * strength / 2.0 + 128
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    outdir = "/tmp/variants"
    args = sys.argv[1:]
    if "-o" in args:
        k = args.index("-o")
        outdir = args[k + 1]
        del args[k:k + 2]
    os.makedirs(outdir, exist_ok=True)

    for path in args:
        w, h, px = read_pgm(path)
        stem = os.path.splitext(os.path.basename(path))[0]

        p128, w1, h1 = pad(px, w, h, 128)
        variants = {"pad128": (p128, w1, h1)}

        variants["pad128-blur"] = (box_blur(p128, w1, h1), w1, h1)
        variants["pad128-lc"] = (local_contrast(p128, w1, h1), w1, h1)
        b, bw, bh = bilinear(p128, w1, h1, 2)
        variants["pad128-x2"] = (b, bw, bh)
        variants["pad128-x2-blur"] = (box_blur(b, bw, bh), bw, bh)
        b3, b3w, b3h = bilinear(p128, w1, h1, 3)
        variants["pad128-x3"] = (b3, b3w, b3h)
        p160, w2, h2 = pad(px, w, h, 160)
        variants["pad160"] = (p160, w2, h2)
        c, cw, ch = bilinear(p160, w2, h2, 2)
        variants["pad160-x2"] = (c, cw, ch)

        for name, (data, vw, vh) in variants.items():
            write_pgm(os.path.join(outdir, "%s-%s.pgm" % (stem, name)),
                      vw, vh, data)
        print("%s: %d variants" % (stem, len(variants)))


if __name__ == "__main__":
    main()
