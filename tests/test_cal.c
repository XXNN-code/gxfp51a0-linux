/* SPDX-License-Identifier: GPL-2.0-only */
#include "gx51.h"

#include <stdio.h>
#include <string.h>

/* Real OTP read from this MateBook 13, firmware GF_ST411SEC_APP_14108. */
static const uint8_t otp[64] = {
    0x44, 0x4b, 0x38, 0x39, 0x30, 0x36, 0x2e, 0x01,
    0xba, 0x7c, 0x6c, 0x9e, 0x5c, 0xb7, 0x74, 0x07,
    0x01, 0x07, 0x05, 0x02, 0x7d, 0x6a, 0xfa, 0x27,
    0x41, 0x01, 0x50, 0x0c, 0xf1, 0x8f, 0x70, 0x0c,
    0x07, 0x41, 0x30, 0x34, 0xe6, 0x8f, 0xe3, 0xfc,
    0x01, 0x94, 0xbd, 0x42, 0x02, 0xbd, 0xb7, 0xb9,
    0xb7, 0xb6, 0xb7, 0xb9, 0xb7, 0xb6, 0x39, 0x5a,
    0xe6, 0x19, 0xe3, 0x1c, 0xe5, 0x88, 0xfa, 0x2e,
};

/* Transport stubs: these tests never touch hardware. */
int gx51_write_frame(struct gx51_dev *dev, uint8_t type,
                     const uint8_t *body, size_t len)
{
    (void)dev;
    (void)type;
    (void)body;
    (void)len;
    return -1;
}

int gx51_read_frame(struct gx51_dev *dev, uint8_t *type,
                    uint8_t *body, size_t cap, size_t *len)
{
    (void)dev;
    (void)type;
    (void)body;
    (void)cap;
    (void)len;
    return -1;
}

int gx51_wait_irq(struct gx51_dev *dev, unsigned int timeout_ms)
{
    (void)dev;
    (void)timeout_ms;
    return -1;
}

int gx51_wait_idle(struct gx51_dev *dev, unsigned int timeout_ms)
{
    (void)dev;
    (void)timeout_ms;
    return -1;
}

static int failures;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

int main(void)
{
    struct gx51_cal cal;
    uint8_t cfg[GX51_CONFIG_LEN];
    uint8_t bad[64];

    check("derive_cal", gx51_derive_cal(otp, &cal) == 0);
    check("tcode 256", cal.tcode == 256);
    check("fdt_delta 31", cal.fdt_delta == 31);
    check("dac_main 0x0b78", cal.dac_main == 0x0b78);
    check("dac1 0xb9", cal.dac1 == 0xb9);
    check("dac2 0xb7", cal.dac2 == 0xb7);
    check("dac3 0xb6", cal.dac3 == 0xb6);

    memcpy(bad, otp, sizeof(bad));
    bad[60] ^= 0x01;
    check("reject corrupted OTP", gx51_derive_cal(bad, &cal) < 0);

    memcpy(cfg, gx51_base_config, sizeof(cfg));
    check("base config patch", gx51_patch_config(cfg, &cal) == 0);
    check("patched config differs", memcmp(cfg, gx51_base_config,
                                           sizeof(cfg)) != 0);

    /* Second patch must be idempotent on an already patched config. */
    check("patch idempotent", gx51_patch_config(cfg, &cal) == 0);

    if (failures != 0) {
        printf("cal tests: %d failed\n", failures);
        return 1;
    }
    printf("cal tests: ok\n");
    return 0;
}
