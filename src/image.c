/* SPDX-License-Identifier: GPL-2.0-only */
#include "image.h"

#include <stdio.h>
#include <string.h>

/*
 * Four 12-bit samples per six bytes. The packing is little-nibble first:
 * the low nibble of b0 is the top nibble of the first sample, the high nibble
 * of b0 joins b3 for the second, and b5 plays the same role for the last two.
 */
static void unpack4(const uint8_t *b, uint16_t p[4])
{
    p[0] = (uint16_t)(((b[0] & 0x0fu) << 8) | b[1]);
    p[1] = (uint16_t)((b[3] << 4) | (b[0] >> 4));
    p[2] = (uint16_t)(((b[5] & 0x0fu) << 8) | b[2]);
    p[3] = (uint16_t)((b[4] << 4) | (b[5] >> 4));
}

int gx51_image_decode(const uint8_t *plain, size_t len, uint16_t *out)
{
    uint16_t wire[GX51_IMAGE_WIRE_WIDTH * GX51_IMAGE_WIRE_HEIGHT];
    const uint8_t *packed;
    size_t i;
    size_t o = 0;
    size_t x;
    size_t y;

    if (plain == NULL || out == NULL || len != GX51_IMAGE_PLAIN_LEN)
        return -1;

    packed = plain + GX51_IMAGE_HEADER_LEN;
    for (i = 0; i + 6u <= GX51_IMAGE_PACKED_LEN; i += 6u) {
        uint16_t p[4];

        unpack4(packed + i, p);
        wire[o++] = p[0];
        wire[o++] = p[1];
        wire[o++] = p[2];
        wire[o++] = p[3];
    }
    if (o != GX51_IMAGE_WIRE_WIDTH * GX51_IMAGE_WIRE_HEIGHT)
        return -1;

    /* The sensor streams columns: wire[x * 88 + y] is pixel (x, y). */
    for (y = 0; y < GX51_IMAGE_HEIGHT; y++)
        for (x = 0; x < GX51_IMAGE_WIDTH; x++)
            out[y * GX51_IMAGE_WIDTH + x] =
                wire[x * GX51_IMAGE_WIRE_WIDTH + y];
    return 0;
}

void gx51_image_stats(const uint16_t *px, size_t count,
                      struct gx51_image_stats *st)
{
    uint32_t hist[4096];
    uint32_t sum = 0;
    size_t i;
    size_t seen = 0;

    if (px == NULL || st == NULL || count == 0)
        return;

    memset(hist, 0, sizeof(hist));
    st->min = 0x0fffu;
    st->max = 0u;
    for (i = 0; i < count; i++) {
        uint16_t v = (uint16_t)(px[i] & 0x0fffu);

        hist[v]++;
        sum += v;
        if (v < st->min)
            st->min = v;
        if (v > st->max)
            st->max = v;
    }
    st->mean = sum / (uint32_t)count;
    st->nonzero = 0;
    for (i = 0; i < count; i++)
        if (px[i] != st->min)
            st->nonzero++;

    /* Percentiles come from the histogram, each scanned from zero so a
     * break in one does not truncate the other. */
    st->p1 = st->min;
    seen = 0;
    for (i = 0; i < 4096u; i++) {
        seen += hist[i];
        if (seen * 100u >= count) {
            st->p1 = (uint16_t)i;
            break;
        }
    }
    st->p99 = st->max;
    seen = 0;
    for (i = 0; i < 4096u; i++) {
        seen += hist[i];
        if (seen * 100u >= count * 99u) {
            st->p99 = (uint16_t)i;
            break;
        }
    }
}

int gx51_image_write_pgm(const char *path, const uint16_t *px)
{
    uint8_t row[GX51_IMAGE_WIDTH];
    struct gx51_image_stats st;
    uint16_t lo;
    uint16_t hi;
    size_t x;
    size_t y;
    FILE *f;

    if (path == NULL || px == NULL)
        return -1;

    gx51_image_stats(px, GX51_IMAGE_PIXELS, &st);
    /* Stretch on the 1st/99th percentile: a raw max-based scale turns a
     * uniform background into white and hides the ridge modulation. */
    lo = st.p1;
    hi = st.p99;
    if (hi <= lo) {
        lo = st.min;
        hi = st.max;
    }
    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    fprintf(f, "P5\n%u %u\n255\n", GX51_IMAGE_WIDTH, GX51_IMAGE_HEIGHT);
    for (y = 0; y < GX51_IMAGE_HEIGHT; y++) {
        for (x = 0; x < GX51_IMAGE_WIDTH; x++) {
            uint16_t v = (uint16_t)(px[y * GX51_IMAGE_WIDTH + x] & 0x0fffu);

            row[x] = (uint8_t)(hi > lo
                                   ? (v <= lo ? 0u
                                              : (v >= hi ? 255u
                                                         : ((uint32_t)(v - lo) *
                                                            255u) /
                                                               (hi - lo)))
                                   : 0u);
        }
        if (fwrite(row, 1, GX51_IMAGE_WIDTH, f) != GX51_IMAGE_WIDTH) {
            fclose(f);
            return -1;
        }
    }
    return fclose(f) == 0 ? 0 : -1;
}

int gx51_image_write_diff_pgm(const char *path, const uint16_t *a,
                              const uint16_t *b)
{
    uint8_t row[GX51_IMAGE_WIDTH];
    size_t x;
    size_t y;
    FILE *f;

    if (path == NULL || a == NULL || b == NULL)
        return -1;
    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    fprintf(f, "P5\n%u %u\n255\n", GX51_IMAGE_WIDTH, GX51_IMAGE_HEIGHT);
    for (y = 0; y < GX51_IMAGE_HEIGHT; y++) {
        for (x = 0; x < GX51_IMAGE_WIDTH; x++) {
            int d = (int)a[y * GX51_IMAGE_WIDTH + x] -
                    (int)b[y * GX51_IMAGE_WIDTH + x];

            d += 2048;
            if (d < 0)
                d = 0;
            if (d > 4095)
                d = 4095;
            row[x] = (uint8_t)(d / 16);
        }
        if (fwrite(row, 1, GX51_IMAGE_WIDTH, f) != GX51_IMAGE_WIDTH) {
            fclose(f);
            return -1;
        }
    }
    return fclose(f) == 0 ? 0 : -1;
}
