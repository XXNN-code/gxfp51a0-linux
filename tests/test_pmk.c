/* SPDX-License-Identifier: GPL-2.0-only */
/* Offline checks for the crypto primitives and the PMK record parser. */
#include "gx51.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(const char *name, int ok)
{
    printf("%s: %s\n", name, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

static void hex(const char *s, uint8_t *out, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned int v;

        if (sscanf(s + i * 2, "%2x", &v) != 1) {
            fprintf(stderr, "bad hex at %zu\n", i);
            exit(2);
        }
        out[i] = (uint8_t)v;
    }
}

static void test_sha256(void)
{
    static const uint8_t abc[] = {'a', 'b', 'c'};
    uint8_t want[32];
    uint8_t got[32];

    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        want, sizeof(want));
    gx51_sha256(abc, sizeof(abc), got);
    check("sha256(abc)", memcmp(want, got, sizeof(want)) == 0);
}

static void test_aes_block(void)
{
    uint8_t key[16], ct[16], want[16], got[16], rk[176];

    hex("000102030405060708090a0b0c0d0e0f", key, sizeof(key));
    hex("69c4e0d86a7b0430d8cdb78070b4c55a", ct, sizeof(ct));
    hex("00112233445566778899aabbccddeeff", want, sizeof(want));

    gx51_aes128_expand(key, rk);
    gx51_aes128_decrypt_block(rk, ct, got);
    check("aes128 decrypt block", memcmp(want, got, sizeof(want)) == 0);
}

static void test_aes_cbc(void)
{
    uint8_t key[16], iv[16], ct[32], want[32], got[32];

    hex("2b7e151628aed2a6abf7158809cf4f3c", key, sizeof(key));
    hex("000102030405060708090a0b0c0d0e0f", iv, sizeof(iv));
    hex("7649abac8119b246cee98e9b12e9197d"
        "5086cb9b507219ee95db113a917678b2", ct, sizeof(ct));
    hex("6bc1bee22e409f96e93d7e117393172a"
        "ae2d8a571e03ac9c9eb76fac45af8e51", want, sizeof(want));

    check("aes128 cbc decrypt",
          gx51_aes128_cbc_decrypt(key, iv, ct, got, sizeof(ct)) == 0 &&
          memcmp(want, got, sizeof(want)) == 0);
}

static void test_reject_bad_marker(void)
{
    uint8_t rec[96];
    uint8_t pmk[GX51_PMK_LEN];

    memset(rec, 0x5a, sizeof(rec));
    check("random record rejected",
          gx51_pmk_from_record(rec, sizeof(rec), pmk) < 0);
    check("short record rejected",
          gx51_pmk_from_record(rec, 64u, pmk) < 0);
}

/*
 * Optional hardware captures written by `gx51-probe --staging`. Device
 * specific, never committed, so the test skips when they are absent.
 */
static size_t find_f2_frame(const uint8_t *raw, size_t len, size_t *off)
{
    size_t i;

    for (i = 0; i + 5u <= len; i++) {
        const uint8_t *data;
        size_t data_len;

        if (raw[i] != 0xf2u)
            continue;
        if (gx51_mem_payload(raw + i, len - i, &data, &data_len) < 0)
            continue;
        *off = i;
        return data_len;
    }
    return 0;
}

static void test_real_record(const char *path, size_t skip)
{
    FILE *f = fopen(path, "rb");
    uint8_t raw[512];
    uint8_t pmk[GX51_PMK_LEN];
    uint8_t digest[32];
    static const char want_prefix[] = "30176ca18f9472ed";
    char label[96];
    char hex_prefix[17];
    const uint8_t *data;
    size_t data_len;
    size_t off = 0;
    size_t n;

    if (f == NULL) {
        printf("real record %s: skipped (missing)\n", path);
        return;
    }
    n = fread(raw, 1, sizeof(raw), f);
    fclose(f);

    data_len = find_f2_frame(raw, n, &off);
    snprintf(label, sizeof(label), "%s frame parsed", path);
    check(label, data_len > 0u);

    snprintf(label, sizeof(label), "%s holds the 80-byte record", path);
    check(label, data_len >= skip + 80u);
    if (failures)
        return;

    check(path, gx51_mem_payload(raw + off, n - off, &data, &data_len) == 0);

    snprintf(label, sizeof(label), "%s record decrypts", path);
    check(label, gx51_pmk_from_record(data + skip, data_len - skip, pmk) == 0);
    if (failures)
        return;

    gx51_sha256(pmk, sizeof(pmk), digest);
    snprintf(hex_prefix, sizeof(hex_prefix),
             "%02x%02x%02x%02x%02x%02x%02x%02x",
             digest[0], digest[1], digest[2], digest[3],
             digest[4], digest[5], digest[6], digest[7]);
    snprintf(label, sizeof(label), "%s PMK fingerprint", path);
    check(label, strcmp(hex_prefix, want_prefix) == 0);
}

static void test_frame_parser(void)
{
    uint8_t payload[80];
    uint8_t frame[96];
    size_t len;
    const uint8_t *data;
    size_t data_len;

    memset(payload, 0x42, sizeof(payload));
    len = gx51_build_body(frame, sizeof(frame), 0xf2u, payload,
                          sizeof(payload));
    check("f2 frame built", len == 84u);
    check("f2 payload extracted",
          gx51_mem_payload(frame, len, &data, &data_len) == 0 &&
          data_len == sizeof(payload) &&
          memcmp(data, payload, sizeof(payload)) == 0);

    frame[len - 1] ^= 0xffu;
    check("f2 bad checksum rejected",
          gx51_mem_payload(frame, len, &data, &data_len) < 0);
}

/*
 * The 0x08008008 capture is an 80-byte read, so it carries only 72 payload
 * bytes - enough to prove that the record starts eight bytes past the base,
 * not enough to decrypt four CBC blocks.
 */
static void test_record_offset(void)
{
    FILE *a = fopen("f2-08008000.bin", "rb");
    FILE *b = fopen("f2-08008008.bin", "rb");
    uint8_t rawa[512], rawb[512];
    const uint8_t *da, *db;
    size_t na, nb, la, lb, offa = 0, offb = 0;
    size_t i;
    int match = 1;

    if (a == NULL || b == NULL) {
        printf("record offset: skipped (missing capture)\n");
        if (a != NULL)
            fclose(a);
        if (b != NULL)
            fclose(b);
        return;
    }
    na = fread(rawa, 1, sizeof(rawa), a);
    nb = fread(rawb, 1, sizeof(rawb), b);
    fclose(a);
    fclose(b);

    la = find_f2_frame(rawa, na, &offa);
    lb = find_f2_frame(rawb, nb, &offb);
    if (la < 8u || lb == 0u) {
        printf("record offset: skipped (bad capture)\n");
        return;
    }
    gx51_mem_payload(rawa + offa, na - offa, &da, &la);
    gx51_mem_payload(rawb + offb, nb - offb, &db, &lb);

    if (lb > la - 8u)
        lb = la - 8u;
    for (i = 0; i < lb; i++) {
        if (da[8u + i] != db[i])
            match = 0;
    }
    check("record starts 8 bytes past the base", lb > 0u && match);
}

int main(void)
{
    test_sha256();
    test_aes_block();
    test_aes_cbc();
    test_reject_bad_marker();
    test_frame_parser();
    test_real_record("f2-08008000.bin", 8u);
    test_record_offset();

    if (failures) {
        printf("pmk tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("pmk tests: ok\n");
    return 0;
}
