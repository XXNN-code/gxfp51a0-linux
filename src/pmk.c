/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Factory PMK recovery.
 *
 * The sensor exposes its flash through command 0xf2. The factory record sits
 * at one of three 0x1000-aligned bases; its payload begins 8 bytes past the
 * base with the 80 bytes that matter:
 *   16-byte salt, then four AES-128-CBC blocks holding
 *   00 0d | be32(48) | 48-byte PMK | 10 bytes of trailing data
 *
 * key = SHA-256(salt || 48 zero bytes || seed)[0..15], IV = salt.
 *
 * Everything here is derived from this device's own flash; no key material is
 * hardcoded except the public seed constant and nothing is written to disk.
 */
#define _GNU_SOURCE
#include "gx51.h"
#include "crypto.h"

#include <string.h>
#include <unistd.h>

/* Public fallback seed, identical on the 14115 reference and this 14108 unit. */
static const uint8_t factory_seed[16] = {
    0x5c, 0xba, 0x6e, 0x25, 0x81, 0x95, 0x18, 0xde,
    0x2d, 0x53, 0xe9, 0x6d, 0xc0, 0x34, 0x7a, 0xb0,
};

static const uint32_t factory_bases[] = {
    0x08008000u, 0x08004000u, 0x0800a000u,
};

/* Eight-byte record header precedes the salt in every factory slot. */
#define GX51_FACTORY_HDR 8u

/*
 * An F2 reply is a single frame body [f2][len16 = payload+1][payload][ck],
 * sometimes preceded by a six-byte ACK. The payload starts at byte 3 and the
 * len16 field counts it plus the trailing checksum.
 */
int gx51_mem_payload(const uint8_t *frame, size_t len,
                     const uint8_t **data, size_t *data_len)
{
    if (frame == NULL || data == NULL || data_len == NULL ||
        len < 5u || frame[0] != 0xf2u || !gx51_valid_body(frame, len))
        return -1;

    *data = frame + 3;
    *data_len = len - 4u;
    return 0;
}

int gx51_mem_read(struct gx51_dev *dev, uint32_t addr, uint32_t len,
                  uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t body[12];
    uint8_t rx[GX51_MEM_READ_MAX + 16u];
    uint8_t type;
    const uint8_t *data;
    size_t data_len;
    uint32_t wire;
    size_t n;
    int i;
    int ret;

    if (dev == NULL || out == NULL || out_len == NULL ||
        addr < 0x08000000u || len == 0u || len > GX51_MEM_READ_MAX)
        return -1;

    wire = addr - 0x08000000u;
    body[0] = 0xf2u;
    body[1] = 0x09u;
    body[2] = 0x00u;
    body[3] = (uint8_t)wire;
    body[4] = (uint8_t)(wire >> 8);
    body[5] = (uint8_t)(wire >> 16);
    body[6] = (uint8_t)(wire >> 24);
    body[7] = (uint8_t)(len & 0xffu);
    body[8] = (uint8_t)((len >> 8) & 0xffu);
    body[9] = (uint8_t)((len >> 16) & 0xffu);
    body[10] = (uint8_t)((len >> 24) & 0xffu);
    body[11] = gx51_body_checksum(body, 11u);

    /* Right after a config upload the line can still be settling, so this wait
     * is generous compared with the command path. */
    if (gx51_wait_idle(dev, 500) < 0)
        return -1;
    if (gx51_write_frame(dev, GX51_PKT_PLAIN, body, sizeof(body)) < 0)
        return -1;

    for (i = 0; i < 4; i++) {
        usleep(8000);
        ret = gx51_read_frame_to(dev, 400u, &type, rx, sizeof(rx), &n);
        if (ret < 0 || type != GX51_PKT_PLAIN)
            return -1;

        /* The ACK carries no data; the record follows it in the next frame. */
        if (n == 6u && rx[0] == GX51_CMD_ACK)
            continue;

        if (gx51_mem_payload(rx, n, &data, &data_len) < 0)
            continue;
        if (data_len > cap)
            data_len = cap;
        memcpy(out, data, data_len);
        *out_len = data_len;
        return 0;
    }

    return -1;
}

int gx51_pmk_from_record(const uint8_t *rec, size_t len,
                         uint8_t pmk[GX51_PMK_LEN])
{
    uint8_t input[80];
    uint8_t digest[32];
    uint8_t plain[64];
    uint32_t declared;

    if (rec == NULL || pmk == NULL || len < 80u)
        return -1;

    memset(input, 0, sizeof(input));
    memcpy(input, rec, 16);
    memcpy(input + 64, factory_seed, sizeof(factory_seed));
    gx51_sha256(input, sizeof(input), digest);

    if (gx51_aes128_cbc_decrypt(digest, rec, rec + 16, plain,
                                sizeof(plain)) < 0)
        return -1;

    if (plain[0] != 0x00u || plain[1] != 0x0du)
        return -1;
    declared = ((uint32_t)plain[2] << 24) | ((uint32_t)plain[3] << 16) |
               ((uint32_t)plain[4] << 8) | (uint32_t)plain[5];
    if (declared != GX51_PMK_LEN)
        return -1;

    memcpy(pmk, plain + 6, GX51_PMK_LEN);
    memset(plain, 0, sizeof(plain));
    memset(digest, 0, sizeof(digest));
    return 0;
}

int gx51_fetch_pmk(struct gx51_dev *dev, uint8_t pmk[GX51_PMK_LEN])
{
    uint8_t rec[96];
    size_t n = 0;
    size_t i;
    int pass;

    if (dev == NULL || pmk == NULL)
        return -1;

    /* The F2 read is the most timing-sensitive exchange in the whole flow; a
     * second pass over all slots costs 60 ms and removes the flakiness that a
     * single cold attempt shows after configure. */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < sizeof(factory_bases) / sizeof(factory_bases[0]);
             i++) {
            n = 0;
            if (gx51_mem_read(dev, factory_bases[i] + GX51_FACTORY_HDR, 96u,
                              rec, sizeof(rec), &n) == 0 &&
                gx51_pmk_from_record(rec, n, pmk) == 0) {
                memset(rec, 0, sizeof(rec));
                return 0;
            }
            usleep(20000);
        }
    }

    memset(rec, 0, sizeof(rec));
    return -1;
}
