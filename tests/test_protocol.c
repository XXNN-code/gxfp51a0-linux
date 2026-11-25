/* SPDX-License-Identifier: GPL-2.0-only */
#include "gx51.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do {                                                   \
    if (!(cond)) {                                                         \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        failures++;                                                        \
    }                                                                      \
} while (0)

static void test_known_bodies(void)
{
    static const uint8_t nop[] = {0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa5};
    static const uint8_t install[] = {0x96, 0x03, 0x00, 0x01, 0x00, 0x10};
    static const uint8_t query[] = {0xa8, 0x03, 0x00, 0x00, 0x00, 0xff};
    static const uint8_t zeros[2] = {0, 0};
    static const uint8_t nop_payload[4] = {0, 0, 0, 0};
    uint8_t out[32];
    size_t len;

    len = gx51_build_body(out, sizeof(out), 0x00, nop_payload, 4);
    CHECK(len == sizeof(nop));
    CHECK(memcmp(out, nop, sizeof(nop)) == 0);

    len = gx51_build_body(out, sizeof(out), 0x96,
                          (const uint8_t[]){1, 0}, 2);
    CHECK(len == sizeof(install));
    CHECK(memcmp(out, install, sizeof(install)) == 0);

    len = gx51_build_body(out, sizeof(out), GX51_CMD_FW_VERSION, zeros, 2);
    CHECK(len == sizeof(query));
    CHECK(memcmp(out, query, sizeof(query)) == 0);

    CHECK(gx51_build_body(out, 4, 0x00, zeros, 2) == 0);
    CHECK(gx51_build_body(out, sizeof(out), 0x00, NULL, 1) == 0);
}

static void test_ack(void)
{
    uint8_t ack[6];
    uint8_t status = 0;

    ack[0] = GX51_CMD_ACK;
    ack[1] = 3;
    ack[2] = 0;
    ack[3] = GX51_CMD_FW_VERSION;
    ack[4] = 1;
    ack[5] = gx51_body_checksum(ack, 5);

    CHECK(gx51_parse_ack(ack, sizeof(ack), GX51_CMD_FW_VERSION, &status) == 0);
    CHECK(status == 1);

    ack[5] ^= 1;
    CHECK(gx51_parse_ack(ack, sizeof(ack), GX51_CMD_FW_VERSION, &status) != 0);
}

static void test_version(void)
{
    static const char expected[] = "GF_ST411SEC_APP_14115";
    uint8_t body[64];
    char version[64];
    size_t len;

    len = gx51_build_body(body, sizeof(body), GX51_CMD_FW_VERSION,
                          (const uint8_t *)expected, sizeof(expected) - 1);

    CHECK(len == sizeof(expected) - 1 + 4);
    CHECK(gx51_parse_version(body, len, version, sizeof(version)) ==
          (int)(sizeof(expected) - 1));
    CHECK(strcmp(version, expected) == 0);

    body[len - 1] ^= 0xff;
    CHECK(gx51_parse_version(body, len, version, sizeof(version)) < 0);
    CHECK(gx51_parse_version(body, len - 1, version, sizeof(version)) < 0);
}

int main(void)
{
    test_known_bodies();
    test_ack();
    test_version();

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("protocol tests: ok");
    return 0;
}
