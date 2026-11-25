#!/usr/bin/env python3
"""Wire the goodix51a0 driver into an extracted libfprint tree.

Idempotent: running it twice changes nothing. Used by tools/build-libfprint.sh.

    ./tools/patch-libfprint.py /path/to/libfprint-v1.94.9
"""

import sys
import os
import shutil

DRIVER = "goodix51a0"

MESON_SOURCES = """    'goodix51a0' :
        [ 'drivers/goodix51a0/goodix51a0.c',
          'drivers/goodix51a0/transport.c',
          'drivers/goodix51a0/protocol.c',
          'drivers/goodix51a0/target.c',
          'drivers/goodix51a0/config_base.c',
          'drivers/goodix51a0/pmk.c',
          'drivers/goodix51a0/tls.c',
          'drivers/goodix51a0/image.c',
          'drivers/goodix51a0/session.c',
          'drivers/goodix51a0/sha256.c',
          'drivers/goodix51a0/aes128.c' ],
"""


def edit(path, transform):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    new = transform(text)
    if new != text:
        with open(path, "w", encoding="utf-8") as f:
            f.write(new)
        print("patched %s" % path)
    else:
        print("unchanged %s" % path)


def patch_libfprint_meson(text):
    if "'%s' :" % DRIVER in text:
        return text
    marker = "driver_sources = {\n"
    if marker not in text:
        raise SystemExit("driver_sources table not found")
    return text.replace(marker, marker + MESON_SOURCES, 1)


def patch_root_meson(text):
    if text.count("'%s'" % DRIVER) >= 3:
        return text

    # 1. ship it in the default driver set
    if "'focaltech_moc',\n]" in text and \
       "'%s',\n]" % DRIVER not in text:
        text = text.replace("'focaltech_moc',\n]",
                            "'focaltech_moc',\n    '%s',\n]" % DRIVER, 1)

    # 2. it is endian safe (all wire accesses are byte-wise)
    marker = "endian_independent_drivers = virtual_drivers + [\n"
    if marker in text and "'%s',\n" % DRIVER not in text.split(marker)[1][:400]:
        text = text.replace(marker, marker + "    '%s',\n" % DRIVER, 1)

    # 3. helpers: udev for spidev discovery, openssl for the PSK tunnel
    marker = "    'elanspi' : [ 'udev' ],\n"
    if marker in text:
        text = text.replace(
            marker, marker + "    '%s' : [ 'openssl', 'udev' ],\n" % DRIVER, 1)

    return text


def patch_drop_cpp(text):
    """libfprint declares C++ unconditionally, but only examples/cpp-test.cpp
    uses it. Without a C++ compiler meson refuses to configure the whole
    project, so drop the language and that one example."""
    text = text.replace("project('libfprint', [ 'c', 'cpp' ]",
                        "project('libfprint', [ 'c' ]", 1)
    text = text.replace("cpp = meson.get_compiler('cpp')\n", "", 1)
    text = text.replace("add_project_arguments(common_cflags, language: 'cpp')\n",
                        "", 1)
    return text


def patch_examples_meson(text):
    marker = """executable('cpp-test',
    'cpp-test.cpp',
    dependencies: libfprint_dep,
)
"""
    return text.replace(marker, "", 1)


def patch_match_debug(text):
    """Log how many minutiae each side of a BZ3 comparison actually carries.

    Without this a "score 0" is indistinguishable from "the threshold is too
    high"; the counts make it obvious immediately.
    """
    marker = '      fp_dbg ("score %d/%d", score, bz3_threshold);'
    if marker not in text or "probe %d minutiae" in text:
        return text
    text = text.replace(
        """      gstruct = g_ptr_array_index (template->prints, i);
      score = bozorth_to_gallery (probe_len, pstruct, gstruct);""",
        """      gstruct = g_ptr_array_index (template->prints, i);
      fp_dbg ("bz3 compare: probe %d minutiae vs gallery %d",
              pstruct->nrows, gstruct->nrows);
      score = bozorth_to_gallery (probe_len, pstruct, gstruct);""",
        1,
    )
    return text


def patch_bozorth_min_minutiae(text):
    """Lower Bozorth3's computable-minimum floor.

    bozorth3.c returns ZERO_MATCH_SCORE outright when either side has fewer
    than MIN_COMPUTABLE_BOZORTH_MINUTIAE (10) minutiae -- the score is never
    computed, so no threshold can help. The GX51A0's 4.0x3.2 mm window yields
    8-9 per frame (measured over 10 stored captures: 8,9,3,2,9,9,8,9,13,9), so
    every comparison, including a genuine one, scored 0. Four still rejects the
    2-3 minutiae frames, which carry no usable structure.
    """
    marker = "#define MIN_COMPUTABLE_BOZORTH_MINUTIAE"
    out = []
    for line in text.splitlines(True):
        if line.startswith(marker) and line.split()[-1] == "10":
            line = "#define MIN_COMPUTABLE_BOZORTH_MINUTIAE\t4\n"
        out.append(line)
    return "".join(out)


def patch_rmtest_dist(text):
    """Shrink the false-minutiae boundary margin for small sensors.

    MAX_RMTEST_DIST_V2 culls every minutia within 16 px of an invalid region.
    On the GX51A0 the ridged area is only 80x64 and sits inside a 128x128
    canvas, so that margin eats the entire outer ring and leaves ~8 minutiae
    per frame. Bozorth3's score ceiling is set by the number of consistent
    point pairs, so 8 minutiae can never reach bz3_threshold 24. At 4 px the
    genuinely unreliable edge points still go, the perimeter survives.
    """
    marker = "#define MAX_RMTEST_DIST_V2"
    out = []
    for line in text.splitlines(True):
        if line.startswith(marker) and line.split()[-1] == "16":
            line = "#define MAX_RMTEST_DIST_V2       4\n"
        out.append(line)
    return "".join(out)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    root = sys.argv[1]
    if not os.path.isdir(os.path.join(root, "libfprint")):
        raise SystemExit("%s does not look like a libfprint tree" % root)

    edit(os.path.join(root, "meson.build"), patch_root_meson)
    edit(os.path.join(root, "libfprint", "meson.build"), patch_libfprint_meson)
    edit(os.path.join(root, "libfprint", "fpi-print.c"), patch_match_debug)
    edit(os.path.join(root, "libfprint", "nbis", "include", "bozorth.h"),
         patch_bozorth_min_minutiae)
    edit(os.path.join(root, "libfprint", "nbis", "include", "lfs.h"),
         patch_rmtest_dist)

    # C++ is optional for us; only strip it when no compiler is installed so
    # the patch stays a no-op on machines that have one.
    if shutil.which("g++") is None and shutil.which("c++") is None:
        print("no C++ compiler found, dropping the cpp language and example")
        edit(os.path.join(root, "meson.build"), patch_drop_cpp)
        edit(os.path.join(root, "examples", "meson.build"), patch_examples_meson)
    return 0


if __name__ == "__main__":
    sys.exit(main())
