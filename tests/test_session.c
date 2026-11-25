/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Offline checks for the capture session. The two FDT reports below were
 * captured from this unit (run-capture-160137.log); they are the cheapest
 * regression guard for the parse offsets, which are easy to shift by one
 * without noticing anything except "zones look wrong".
 */
#define _GNU_SOURCE
#include "session.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char *label, int ok)
{
    printf("%s: %s\n", label, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

/* Idle: touch clear, six zones around 350. */
static const uint8_t FDT_IDLE[] = {
    0x36, 0x11, 0x00, 0x00, 0x01, 0x00, 0x00, 0x61, 0x01, 0x82,
    0x01, 0x44, 0x01, 0x60, 0x01, 0x4a, 0x01, 0x63, 0x01, 0x28,
};

/* Finger down: touch 0x3f, six zones pulled down to a mean of 184. */
static const uint8_t FDT_DOWN[] = {
    0x32, 0x11, 0x00, 0x02, 0x00, 0x3f, 0x00, 0xce, 0x00, 0xde,
    0x00, 0x9e, 0x00, 0xb8, 0x00, 0xa9, 0x00, 0xa6, 0x00, 0xd5,
};

static void test_parse(void)
{
    int zones[6];
    int count = -1;
    int mean = -1;
    uint8_t touch = 0xff;

    check("idle report decodes",
          gx51_session_fdt_decode(FDT_IDLE, sizeof(FDT_IDLE), &touch, zones,
                                  &count, &mean) == 0);
    check("idle touch flag is clear", touch == 0x00);
    check("idle has no touched zone", count == 0);
    check("idle mean is 350", mean == 350);
    check("idle zone 0 is 353", zones[0] == 353);
    check("idle zone 5 is 355", zones[5] == 355);

    check("pressed report decodes",
          gx51_session_fdt_decode(FDT_DOWN, sizeof(FDT_DOWN), &touch, zones,
                                  &count, &mean) == 0);
    check("pressed touch flag is 0x3f", touch == 0x3f);
    check("pressed touches all six zones", count == 6);
    check("pressed mean is 184", mean == 184);

    /* The finger decision in the driver is "5 or more zones": assert the two
     * real reports land on opposite sides of it. */
    touch = 0;
    gx51_session_fdt_decode(FDT_IDLE, sizeof(FDT_IDLE), &touch, NULL, &count,
                            NULL);
    check("idle below touch threshold", count < 5);
    gx51_session_fdt_decode(FDT_DOWN, sizeof(FDT_DOWN), &touch, NULL, &count,
                            NULL);
    check("pressed above touch threshold", count >= 5);
}

static void test_rejects(void)
{
    uint8_t bad[sizeof(FDT_DOWN)];

    memcpy(bad, FDT_DOWN, sizeof(bad));
    bad[sizeof(bad) - 1] ^= 0xff;
    check("bad checksum rejected",
          gx51_session_fdt_decode(bad, sizeof(bad), NULL, NULL, NULL,
                                  NULL) == -1);

    memcpy(bad, FDT_DOWN, sizeof(bad));
    bad[0] = 0x50;
    check("unknown tag rejected",
          gx51_session_fdt_decode(bad, sizeof(bad), NULL, NULL, NULL,
                                  NULL) == -1);

    check("short frame rejected",
          gx51_session_fdt_decode(FDT_DOWN, 12, NULL, NULL, NULL, NULL) == -1);
}

int main(void)
{
    test_parse();
    test_rejects();

    if (failures != 0) {
        printf("session tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("session tests: ok\n");
    return 0;
}
