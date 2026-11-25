#!/usr/bin/env python3
"""Decode a captured GXFP51A0 image frame into a PGM.

The sensor's image frame is  [0x20][len16 LE][payload][trailer].  Nothing about
the geometry is confirmed yet, so this tool tries the candidate packings and
row widths, scores each by inter-row correlation, and writes the best one.

A fingerprint is smooth along a ridge; the correct width makes adjacent rows
correlate strongly.  The wrong width scrambles that structure and scores worse.
"""
import argparse
import sys

CANDS_WIDTH = (32, 40, 48, 56, 60, 64, 72, 80, 88, 96, 100, 104, 112, 128)
CANDS_SKIP = (0, 4, 8, 9, 12, 16, 20, 32)


def load_payload(path):
    data = open(path, "rb").read()
    if data and data[0] == 0x20 and len(data) > 3:
        ln = data[1] | (data[2] << 8)
        body = data[: 3 + ln]
        payload = body[3:-1]          # drop the trailing byte
        if len(payload) > 64:
            return payload
    return data


def unpack12(data):
    """6 bytes -> 4 pixels of 12 bit, MSB first, scaled to 8 bit."""
    px = []
    n = len(data) - (len(data) % 6)
    for i in range(0, n, 6):
        b0, b1, b2, b3, b4, b5 = data[i : i + 6]
        px.append(b0)
        px.append(((b1 << 4) | (b2 >> 4)) & 0xFF)
        px.append(((b2 << 8) | b3) >> 4 & 0xFF)
        px.append(((b3 << 12) | (b4 << 4) | (b5 >> 4)) >> 8 & 0xFF)
    return px


def unpack8(data):
    return list(data)


def unpack16(data):
    n = len(data) - (len(data) % 2)
    return [data[i] for i in range(0, n, 2)]


UNPACK = {"12": unpack12, "8": unpack8, "16": unpack16}


def rows(px, width):
    return [px[i : i + width] for i in range(0, len(px) - width + 1, width)]


def score(px, width):
    """Mean |row[i] - row[i+1]| normalised by the frame's own spread."""
    rs = rows(px, width)
    if len(rs) < 4:
        return None
    diffs = 0.0
    count = 0
    for a, b in zip(rs, rs[1:]):
        for x, y in zip(a, b):
            diffs += abs(x - y)
            count += 1
    flat = [v for r in rs for v in r]
    mean = sum(flat) / len(flat)
    var = sum((v - mean) ** 2 for v in flat) / len(flat)
    if var < 1e-9:
        return None                     # blank frame: nothing to score
    return (diffs / count) / (var ** 0.5)


def write_pgm(path, px, width, height):
    with open(path, "wb") as f:
        f.write(("P5\n%d %d\n255\n" % (width, height)).encode())
        f.write(bytes(px[: width * height]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?", default="tls-image-plain.bin")
    ap.add_argument("--format", choices=("12", "8", "16", "auto"), default="auto")
    ap.add_argument("--width", type=int)
    ap.add_argument("--skip", type=int, default=-1)
    ap.add_argument("--out", default="finger.pgm")
    ap.add_argument("--list", action="store_true", help="show all candidates")
    args = ap.parse_args()

    payload = load_payload(args.file)
    nz = sum(1 for b in payload[32:] if b)
    print("payload %d bytes, non-zero %d (%.2f%%)" %
          (len(payload), nz, 100.0 * nz / max(1, len(payload) - 32)))
    if nz < 32:
        print("frame is blank - no fingerprint data to decode")
        return 1

    formats = [args.format] if args.format != "auto" else ["12", "8", "16"]
    skips = [args.skip] if args.skip >= 0 else CANDS_SKIP
    widths = [args.width] if args.width else CANDS_WIDTH

    best = None
    results = []
    for fmt in formats:
        unpack = UNPACK[fmt]
        for skip in skips:
            if skip >= len(payload) - 64:
                continue
            px = unpack(payload[skip:])
            for w in widths:
                s = score(px, w)
                if s is None:
                    continue
                h = len(px) // w
                results.append((s, fmt, skip, w, h, px))

    if not results:
        print("no candidate produced a scoreable image")
        return 1

    results.sort(key=lambda r: r[0])
    if args.list:
        print("%-6s %-5s %-5s %-5s %s" % ("score", "fmt", "skip", "w", "h"))
        for s, fmt, skip, w, h, _ in results[:12]:
            print("%-6.3f %-5s %-5d %-5d %d" % (s, fmt, skip, w, h))

    s, fmt, skip, w, h, px = results[0]
    print("best: score %.3f  format %s  skip %d  %dx%d" % (s, fmt, skip, w, h))
    write_pgm(args.out, px, w, h)
    print("wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
