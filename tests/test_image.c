/* SPDX-License-Identifier: GPL-2.0-only */
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

static void pack4(uint8_t b[6], unsigned p0, unsigned p1, unsigned p2,
                  unsigned p3)
{
    b[0] = (uint8_t)(((p0 >> 8) & 0x0fu) | ((p1 & 0x0fu) << 4));
    b[1] = (uint8_t)(p0 & 0xffu);
    b[2] = (uint8_t)(p2 & 0xffu);
    b[3] = (uint8_t)((p1 >> 4) & 0xffu);
    b[4] = (uint8_t)((p3 >> 4) & 0xffu);
    b[5] = (uint8_t)(((p2 >> 8) & 0x0fu) | ((p3 & 0x0fu) << 4));
}

static void test_packing(void)
{
    static uint8_t plain[GX51_IMAGE_PLAIN_LEN];
    static uint16_t px[GX51_IMAGE_PIXELS];
    unsigned g;
    unsigned i;

    memset(plain, 0, sizeof(plain));
    for (g = 0; g < GX51_IMAGE_PACKED_LEN / 6u; g++) {
        unsigned base = (g * 4u) & 0x0fffu;

        pack4(plain + GX51_IMAGE_HEADER_LEN + g * 6u, base,
              (base + 1u) & 0x0fffu, (base + 2u) & 0x0fffu,
              (base + 3u) & 0x0fffu);
    }

    check("decode accepts the measured length",
          gx51_image_decode(plain, sizeof(plain), px) == 0);
    check("decode rejects a short buffer",
          gx51_image_decode(plain, sizeof(plain) - 1u, px) != 0);

    /* Wire index for pixel (x, y) is x * 88 + y; sample a few corners. */
    for (i = 0; i < 4u; i++) {
        unsigned x = i == 0 ? 0u : i == 1 ? 79u : i == 2 ? 0u : 79u;
        unsigned y = i == 0 ? 0u : i == 1 ? 0u : i == 2 ? 63u : 63u;
        unsigned wire = x * GX51_IMAGE_WIRE_WIDTH + y;
        unsigned expect = wire & 0x0fffu;
        char label[48];

        snprintf(label, sizeof(label), "pixel (%u,%u) transposes", x, y);
        check(label, px[y * GX51_IMAGE_WIDTH + x] == expect);
    }
}

static void test_stats(void)
{
    static uint16_t px[GX51_IMAGE_PIXELS];
    struct gx51_image_stats st;
    size_t i;

    memset(px, 0, sizeof(px));
    gx51_image_stats(px, GX51_IMAGE_PIXELS, &st);
    check("blank frame has zero range", st.min == 0u && st.max == 0u);
    check("blank frame has no live pixels", st.nonzero == 0u);

    for (i = 0; i < GX51_IMAGE_PIXELS; i++)
        px[i] = (uint16_t)(i & 0x0fffu);
    gx51_image_stats(px, GX51_IMAGE_PIXELS, &st);
    check("ramp reports its endpoints", st.min == 0u && st.max == 4095u);
    check("ramp mean is sane", st.mean > 1600u && st.mean < 1900u);
}

/* Real capture, when one is present: the interesting part is the range. */
static void test_capture(void)
{
    FILE *f = fopen("tls-image-plain.bin", "rb");
    static uint8_t plain[GX51_IMAGE_PLAIN_LEN];
    static uint16_t px[GX51_IMAGE_PIXELS];
    struct gx51_image_stats st;
    size_t n;

    if (f == NULL) {
        printf("capture: skipped (no tls-image-plain.bin)\n");
        return;
    }
    n = fread(plain, 1, sizeof(plain), f);
    fclose(f);
    if (n != GX51_IMAGE_PLAIN_LEN) {
        printf("capture: skipped (%zu bytes)\n", n);
        return;
    }
    check("capture decodes", gx51_image_decode(plain, n, px) == 0);
    gx51_image_stats(px, GX51_IMAGE_PIXELS, &st);
    printf("capture: min=%u max=%u mean=%u live=%zu\n", st.min, st.max,
           st.mean, st.nonzero);
}

int main(void)
{
    test_packing();
    test_stats();
    test_capture();

    if (failures) {
        printf("image tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("image tests: ok\n");
    return 0;
}
