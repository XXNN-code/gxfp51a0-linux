/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GX51_IMAGE_H
#define GX51_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Wire format of one GET_IMAGE plaintext, measured on this unit:
 *
 *   [0..2]   frame header: 20 <len16>
 *   [3..7]   5-byte image header (status, counter/timestamp)
 *   [8..]    10560 bytes packed: 6 bytes carry four 12-bit samples
 *   [-5..]   5-byte trailer
 *
 * 10560 / 6 * 4 = 7040 = 88 * 80, so the raster is 80 rows of 88 columns
 * with eight columns of padding. The visible area is the transposed 80x64.
 */
#define GX51_IMAGE_WIRE_WIDTH 88u
#define GX51_IMAGE_WIRE_HEIGHT 80u
#define GX51_IMAGE_WIDTH 80u
#define GX51_IMAGE_HEIGHT 64u
#define GX51_IMAGE_PIXELS (GX51_IMAGE_WIDTH * GX51_IMAGE_HEIGHT)
#define GX51_IMAGE_HEADER_LEN 8u
#define GX51_IMAGE_PACKED_LEN 10560u
#define GX51_IMAGE_TRAILER_LEN 5u
#define GX51_IMAGE_PLAIN_LEN \
    (GX51_IMAGE_HEADER_LEN + GX51_IMAGE_PACKED_LEN + GX51_IMAGE_TRAILER_LEN)

struct gx51_image_stats {
    uint16_t min;
    uint16_t max;
    uint32_t mean;      /* integer mean over all pixels */
    size_t nonzero;     /* pixels not equal to the minimum */
    uint16_t p1;        /* 1st percentile, useful as a black level */
    uint16_t p99;
};

/* Decode one GET_IMAGE plaintext into GX51_IMAGE_PIXELS 12-bit samples. */
int gx51_image_decode(const uint8_t *plain, size_t len, uint16_t *out);

void gx51_image_stats(const uint16_t *px, size_t count,
                      struct gx51_image_stats *st);

/* Write 8-bit PGM, scaling by max() so an all-zero frame stays visibly black. */
int gx51_image_write_pgm(const char *path, const uint16_t *px);

/* Write a PGM of (a - b + 2048) clamped to 12 bits: shows finger minus base. */
int gx51_image_write_diff_pgm(const char *path, const uint16_t *a,
                              const uint16_t *b);

#endif
