/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Capture session: the exact command sequence the driver has to run, in one
 * place. Every step below was established on hardware; see STATUS.md for the
 * evidence behind the ordering.
 *
 * Ordering rules that are easy to get wrong and were all measured:
 *
 *   1. The factory PMK is read through F2, which boots the sensor into a
 *      staging mode. Its RAM contents are transient: the MCU must be reset
 *      before target initialisation, or configure() is accepted (ACK) and
 *      silently discarded.
 *   2. REQTLS only after the target is configured; the handshake must then be
 *      confirmed with D4, otherwise the capture path stays closed and every
 *      image is all zeros.
 *   3. WakeupMCU (0f 00 00 0e) after the tunnel is up, still required.
 *   4. Commands always travel as plaintext 0xa0 frames. Only the image
 *      direction is encrypted, as a single 0xb0 record.
 *   5. Reply counts per command are not optional: draining one reply leaves
 *      the IRQ line high and the next command is swallowed.
 */
/* libfprint builds with -D_GNU_SOURCE already; the standalone Makefile does
 * not, so only define it when the build system has not. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "session.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAY(...)                        \
    do {                                \
        if (s->verbose)                 \
            fprintf(stderr, __VA_ARGS__); \
    } while (0)

static const uint8_t INSTALL_BODY[] = {0x96, 0x03, 0x00, 0x01, 0x00, 0x10};
static const uint8_t REQTLS_BODY[] = {0xd0, 0x03, 0x00, 0x00, 0x00, 0xd7};
static const uint8_t GET_IMAGE[] = {0x20, 0x03, 0x00, 0x01, 0x00, 0x86};

/* FDT zone recipes, from the validated capture path. */
static const uint8_t FDT_FINGER[12] = {
    0x80, 0xb1, 0x80, 0xc1, 0x80, 0xa6,
    0x80, 0xb6, 0x80, 0xa5, 0x80, 0xb6,
};
static const uint8_t FDT_DOWN[12] = {
    0x80, 0xb2, 0x80, 0xc2, 0x80, 0xa7,
    0x80, 0xb6, 0x80, 0xa6, 0x80, 0xb6,
};
static const uint8_t FDT_BOOT[12] = {
    0xb2, 0xb2, 0xc2, 0xc2, 0xa7, 0xa7,
    0xb6, 0xb6, 0xa6, 0xa6, 0xb6, 0xb6,
};
static const uint8_t FDT_BACKGROUND[12] = {
    0x80, 0xb0, 0x80, 0xc0, 0x80, 0xa4,
    0x80, 0xb4, 0x80, 0xa3, 0x80, 0xb4,
};
static const uint8_t FDT_UP[12] = {
    0x80, 0x90, 0x80, 0x9a, 0x80, 0x7b,
    0x80, 0x95, 0x80, 0x8c, 0x80, 0xa3,
};
static const uint8_t FDT_POST_DOWN[12] = {
    0x80, 0xa7, 0x80, 0xb9, 0x80, 0xa3,
    0x80, 0xb5, 0x80, 0xa4, 0x80, 0xb6,
};

/* A whole image is ~10.6 kB and arrives in one record. */
static uint8_t rx_frame[65536];
static uint8_t tls_plain[1u << 20];

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int sleep_us(long us)
{
    struct timespec ts = {
        .tv_sec = us / 1000000L,
        .tv_nsec = (us % 1000000L) * 1000L,
    };

    while (nanosleep(&ts, &ts) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- transport */

static int send_nop(struct gx51_session *s)
{
    static const uint8_t zeroes[4] = {0};
    uint8_t body[8];
    size_t len = gx51_build_body(body, sizeof(body), 0x00, zeroes, 4);

    if (len == 0)
        return -1;
    if (gx51_write_frame(&s->dev, GX51_PKT_PLAIN, body, len) < 0)
        return -1;
    return sleep_us(5000);
}

static int driverstate_install(struct gx51_session *s)
{
    int wrapper;
    int attempt;

    if (send_nop(s) < 0)
        return -1;

    for (wrapper = 0; wrapper < 2; wrapper++) {
        for (attempt = 0; attempt < 2; attempt++) {
            uint8_t body[128];
            uint8_t type;
            uint8_t status;
            size_t len;

            if (gx51_write_frame(&s->dev, GX51_PKT_PLAIN, INSTALL_BODY,
                                 sizeof(INSTALL_BODY)) < 0)
                return -1;
            if (gx51_read_frame(&s->dev, &type, body, sizeof(body), &len) <= 0)
                continue;
            if (type == GX51_PKT_PLAIN &&
                gx51_parse_ack(body, len, 0x96, &status) == 0)
                return 0;
        }
    }
    return -1;
}

static int read_version_once(struct gx51_session *s)
{
    uint8_t query[8];
    size_t query_len;
    int send;
    int acked = 0;

    if (send_nop(s) < 0)
        return -1;

    query_len = gx51_build_body(query, sizeof(query), GX51_CMD_FW_VERSION,
                                (const uint8_t[]){0x00, 0x00}, 2);
    if (query_len == 0)
        return -1;

    for (send = 0; send < 2; send++) {
        int read;

        if (gx51_write_frame(&s->dev, GX51_PKT_PLAIN, query, query_len) < 0)
            return -1;

        for (read = 0; read < 3; read++) {
            uint8_t body[128];
            uint8_t type;
            uint8_t status;
            size_t len;
            int ret = gx51_read_frame(&s->dev, &type, body, sizeof(body), &len);

            if (ret < 0)
                break;
            if (type != GX51_PKT_PLAIN)
                continue;
            if (gx51_parse_ack(body, len, GX51_CMD_FW_VERSION, &status) == 0)
                acked = 1;
            else if (gx51_parse_version(body, len, s->version,
                                        sizeof(s->version)) > 0)
                return 0;
        }

        if (acked) {
            uint8_t body[128];
            uint8_t type;
            size_t len;

            if (sleep_us(8000) < 0)
                return -1;
            if (gx51_read_frame(&s->dev, &type, body, sizeof(body), &len) > 0 &&
                type == GX51_PKT_PLAIN &&
                gx51_parse_version(body, len, s->version,
                                   sizeof(s->version)) > 0)
                return 0;
        }
    }
    return -1;
}

static int read_version(struct gx51_session *s)
{
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        if (read_version_once(s) == 0)
            return 0;
    }
    return -1;
}

/* Reset through a closed transport: the vendor never toggles GPIO264 with SPI
 * or IRQ held open. DriverState:Install has to be replayed afterwards. */
static int hard_reset_cycle(struct gx51_session *s)
{
    const char *path = s->dev.spi_path[0] != '\0' ? s->dev.spi_path : NULL;

    if (s->dev.irq_fd >= 0) {
        close(s->dev.irq_fd);
        s->dev.irq_fd = -1;
    }
    if (s->dev.spi_fd >= 0) {
        close(s->dev.spi_fd);
        s->dev.spi_fd = -1;
    }
    if (gx51_reset(&s->dev) < 0)
        return -1;
    if (gx51_spi_open(&s->dev, path) < 0)
        return -1;
    if (gx51_irq_open(&s->dev) < 0)
        return -1;
    if (driverstate_install(s) < 0)
        return -1;
    return read_version(s);
}

/* Send one command and drain the replies it is documented to produce. */
struct drain {
    int plain;
    int tls;
    size_t last_len;
    uint8_t last_type;
    uint8_t last[32];
};

static int xfer(struct gx51_session *s, const uint8_t *body, size_t len,
                int expected_plain, unsigned int window_ms, struct drain *res)
{
    struct drain local;
    unsigned int waited = 0;
    int misses = 0;
    uint8_t type;
    size_t n;
    int ret;

    if (res == NULL)
        res = &local;
    memset(res, 0, sizeof(*res));

    if (gx51_wait_idle(&s->dev, 50) < 0)
        return -1;
    if (gx51_write_frame(&s->dev, GX51_PKT_PLAIN, body, len) < 0)
        return -1;

    /* NOP has no reply on this sensor and must not be waited on. */
    if (body[0] == 0x00u)
        return 0;

    usleep(15000);

    while (waited < window_ms) {
        errno = 0;
        ret = gx51_read_frame_to(&s->dev, 10, &type, rx_frame,
                                 sizeof(rx_frame), &n);
        waited += 10;

        if (ret == -1) {
            if (errno == ETIMEDOUT) {
                /* The FDT report follows its ACK by 60-88 ms here, so a
                 * 120 ms silence is the real end of a known-count reply. */
                if (expected_plain > 0 && res->plain > 0 && ++misses >= 12)
                    break;
                continue;
            }
            return -1;
        }
        if (ret < 0)
            break;

        misses = 0;
        res->last_type = type;
        res->last_len = n;
        if (type == GX51_PKT_TLS) {
            res->tls++;
            break;
        }
        if (type == GX51_PKT_PLAIN) {
            size_t keep = n < sizeof(res->last) ? n : sizeof(res->last);

            memcpy(res->last, rx_frame, keep);
        }
        res->plain++;

        if (expected_plain > 0 && res->plain >= expected_plain)
            break;
        usleep(5000);
    }
    return 0;
}

/* One recipe step. The 30 ms gap matches the validated capture pacing. */
static int step(struct gx51_session *s, const uint8_t *body, size_t len,
                int replies)
{
    struct drain d;

    if (xfer(s, body, len, replies, 400, &d) < 0)
        return -1;
    return sleep_us(30000);
}

static size_t build_fdt(uint8_t *out, size_t cap, uint8_t state,
                        const uint8_t zones[12])
{
    uint8_t payload[14];
    uint8_t cmd;

    switch (state) {
    case 0x0cu:
        cmd = 0x32u; /* FDT down */
        break;
    case 0x0eu:
        cmd = 0x34u; /* FDT up */
        break;
    default:
        cmd = 0x36u; /* FDT manual */
        break;
    }

    payload[0] = state;
    payload[1] = 0x01;
    memcpy(payload + 2, zones, 12);
    return gx51_build_body(out, cap, cmd, payload, sizeof(payload));
}

/* ------------------------------------------------------------------- FDT */

/* Report body: [tag][len16][status][pad][touchflag][6 x u16][ck]. Tag is 0x36
 * for the manual probe and 0x32 for a finger-down report; same payload. */
static int fdt_parse(const uint8_t *f, size_t len, uint8_t *touchflag,
                     uint16_t zones[6])
{
    size_t wire;
    size_t i;

    if (len < 20u || (f[0] != 0x36u && f[0] != 0x32u))
        return -1;
    wire = (size_t)f[1] | ((size_t)f[2] << 8);
    if (wire + 3u != len || wire < 17u)
        return -1;
    if (f[len - 1u] != gx51_body_checksum(f, len - 1u))
        return -1;

    *touchflag = f[5];
    for (i = 0; i < 6u; i++)
        zones[i] = (uint16_t)f[7u + 2u * i] |
                   ((uint16_t)f[8u + 2u * i] << 8);
    return 0;
}

static int fdt_zone_count(uint8_t touchflag)
{
    int n = 0;

    for (touchflag &= 0x3fu; touchflag != 0u; touchflag >>= 1)
        n += touchflag & 1u;
    return n;
}

int gx51_session_fdt_decode(const uint8_t *frame, size_t len, uint8_t *touch,
                            int zones[6], int *zone_count, int *mean)
{
    uint16_t z[6];
    uint8_t flags = 0;
    int i;
    int sum = 0;

    if (fdt_parse(frame, len, &flags, z) != 0)
        return -1;

    for (i = 0; i < 6; i++) {
        sum += z[i];
        if (zones != NULL)
            zones[i] = z[i];
    }
    if (touch != NULL)
        *touch = flags;
    if (zone_count != NULL)
        *zone_count = fdt_zone_count(flags);
    if (mean != NULL)
        *mean = sum / 6;
    return 0;
}

int gx51_session_sample(struct gx51_session *s, uint8_t *touch, int *zones,
                        int *mean)
{
    uint8_t body[32];
    struct drain d;
    uint16_t z[6];
    uint8_t flags = 0;
    size_t n;
    int i;
    int sum = 0;

    n = build_fdt(body, sizeof(body), 0x0du, FDT_FINGER);
    if (n == 0)
        return -1;
    if (xfer(s, body, n, 2, 250, &d) < 0)
        return -1;
    if (d.last_type != GX51_PKT_PLAIN || d.last_len > sizeof(d.last) ||
        fdt_parse(d.last, d.last_len, &flags, z) != 0)
        return -1;

    for (i = 0; i < 6; i++) {
        sum += z[i];
        if (zones != NULL)
            zones[i] = z[i];
    }
    if (touch != NULL)
        *touch = flags;
    if (mean != NULL)
        *mean = sum / 6;
    return 0;
}

int gx51_session_wait_touch(struct gx51_session *s, int timeout_ms,
                            int zones_min, int *mean)
{
    long long start = now_ms();
    uint8_t touch = 0;
    int m = 0;

    for (;;) {
        if (now_ms() - start >= timeout_ms)
            return -1;
        if (gx51_session_sample(s, &touch, NULL, &m) == 0 &&
            fdt_zone_count(touch) >= zones_min) {
            if (mean != NULL)
                *mean = m;
            return 0;
        }
        usleep(20000);
    }
}

int gx51_session_wait_clear(struct gx51_session *s, int timeout_ms)
{
    long long start = now_ms();
    uint8_t touch = 0xff;
    int m = 0;

    for (;;) {
        if (now_ms() - start >= timeout_ms)
            return -1;
        if (gx51_session_sample(s, &touch, NULL, &m) == 0 &&
            fdt_zone_count(touch) == 0 && m >= 300)
            return 0;
        usleep(20000);
    }
}

/* -------------------------------------------------------------- recipes */

static int write_dac(struct gx51_session *s)
{
    static const struct {
        uint16_t addr;
        uint16_t value;
    } regs[4] = {
        {0x0220u, 0}, {0x0236u, 0}, {0x0238u, 0}, {0x023au, 0},
    };
    uint8_t body[32];
    /* Vendor 0x80 payload: selector 0x00, then address, then value. */
    uint8_t payload[5];
    uint16_t value[4];
    size_t i;
    size_t n;

    value[0] = s->cal.dac_main;
    value[1] = s->cal.dac1;
    value[2] = s->cal.dac2;
    value[3] = s->cal.dac3;

    for (i = 0; i < 4u; i++) {
        payload[0] = 0x00;
        payload[1] = (uint8_t)regs[i].addr;
        payload[2] = (uint8_t)(regs[i].addr >> 8);
        payload[3] = (uint8_t)value[i];
        payload[4] = (uint8_t)(value[i] >> 8);
        n = gx51_build_body(body, sizeof(body), 0x80, payload,
                            sizeof(payload));
        if (n == 0 || step(s, body, n, 1) < 0)
            return -1;
    }
    return 0;
}

int gx51_session_wakeup(struct gx51_session *s)
{
    return gx51_wakeup_mcu(&s->dev);
}

int gx51_session_background(struct gx51_session *s)
{
    static const uint8_t nop[] = {0x00, 0x05, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0xa5};
    static const uint8_t nav_payload[] = {0x01, 0x00};
    static const uint8_t mcu_byte = 0x55;
    uint8_t body[32];
    uint8_t reg_read[5] = {0x00, 0x82, 0x00, 0x02, 0x00};
    size_t n;

    if (step(s, nop, sizeof(nop), 0) < 0)
        return -1;

    n = gx51_build_body(body, sizeof(body), 0xae, &mcu_byte, 1);
    if (n == 0 || step(s, body, n, 2) < 0)
        return -1;

    n = build_fdt(body, sizeof(body), 0x0du, FDT_BOOT);
    if (n == 0 || step(s, body, n, 2) < 0)
        return -1;

    n = gx51_build_body(body, sizeof(body), 0x50, nav_payload,
                        sizeof(nav_payload));
    if (n == 0 || step(s, body, n, 2) < 0)
        return -1;

    n = build_fdt(body, sizeof(body), 0x0du, FDT_BACKGROUND);
    if (n == 0 || step(s, body, n, 2) < 0)
        return -1;

    n = gx51_build_body(body, sizeof(body), 0x82, reg_read, sizeof(reg_read));
    if (n == 0 || step(s, body, n, 2) < 0)
        return -1;

    return write_dac(s);
}

static int finger_recipe(struct gx51_session *s)
{
    static const uint8_t nop[] = {0x00, 0x05, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0xa5};
    static const uint8_t mcu_byte = 0x55;
    uint8_t body[32];
    size_t n;

    n = build_fdt(body, sizeof(body), 0x0du, FDT_FINGER);
    if (n == 0 || step(s, body, n, 2) < 0)
        return -1;

    n = build_fdt(body, sizeof(body), 0x0cu, FDT_FINGER);
    if (n == 0 || step(s, body, n, 2) < 0)
        return -1;

    if (step(s, nop, sizeof(nop), 0) < 0)
        return -1;

    n = gx51_build_body(body, sizeof(body), 0xae, &mcu_byte, 1);
    if (n == 0 || step(s, body, n, 2) < 0)
        return -1;

    n = build_fdt(body, sizeof(body), 0x0cu, FDT_DOWN);
    if (n == 0 || step(s, body, n, 2) < 0)
        return -1;

    return 0;
}

void gx51_session_cleanup(struct gx51_session *s)
{
    static const uint8_t nav_payload[] = {0x01, 0x00};
    uint8_t body[32];
    size_t n;

    n = build_fdt(body, sizeof(body), 0x0eu, FDT_UP);
    if (n != 0)
        step(s, body, n, 2);
    n = gx51_build_body(body, sizeof(body), 0x50, nav_payload,
                        sizeof(nav_payload));
    if (n != 0)
        step(s, body, n, 2);
    n = build_fdt(body, sizeof(body), 0x0cu, FDT_POST_DOWN);
    if (n != 0)
        step(s, body, n, 2);
}

/* ------------------------------------------------------------------- TLS */

int gx51_session_open(struct gx51_session *s, const char *spi_path)
{
    s->dev.gpio_fd = -1;
    s->dev.irq_fd = -1;
    s->dev.spi_fd = -1;
    s->dev.spi_path[0] = '\0';

    if (spi_path != NULL && spi_path[0] != '\0') {
        if (snprintf(s->dev.spi_path, sizeof(s->dev.spi_path), "%s",
                     spi_path) >= (int)sizeof(s->dev.spi_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    if (gx51_gpio_open(&s->dev) < 0)
        goto fail;
    if (gx51_reset(&s->dev) < 0)
        goto fail;
    if (gx51_spi_open(&s->dev, spi_path) < 0)
        goto fail;
    if (gx51_irq_open(&s->dev) < 0)
        goto fail;
    if (driverstate_install(s) < 0) {
        errno = EIO;
        goto fail;
    }
    if (read_version(s) < 0) {
        errno = EIO;
        goto fail;
    }
    return 0;

fail:
    gx51_session_close(s);
    return -1;
}

void gx51_session_close(struct gx51_session *s)
{
    if (s->tls != NULL) {
        gx51_tls_stop(s->tls);
        s->tls = NULL;
    }
    s->have_tls = 0;
    gx51_close(&s->dev);
}

/* Bytes the previous capture never consumed stay in the SPI FIFO and shift the
 * next REQTLS exchange; the MCU then answers nothing and the handshake window
 * expires with zero records read. Reading until the line goes quiet fixes it. */
static void spi_drain(struct gx51_dev *dev)
{
    uint8_t type;
    size_t n;
    int guard;

    for (guard = 0; guard < 64; guard++) {
        if (gx51_read_frame_to(dev, 5, &type, rx_frame, sizeof(rx_frame),
                               &n) < 0)
            break;
    }
}

int gx51_session_start_tls(struct gx51_session *s)
{
    uint8_t pmk[GX51_PMK_LEN];
    int attempt;

    if (s->tls != NULL) {
        gx51_tls_stop(s->tls);
        s->tls = NULL;
        s->have_tls = 0;
    }

    if (gx51_fetch_pmk(&s->dev, pmk) < 0) {
        fprintf(stderr, "gx51: PMK not recovered\n");
        return -1;
    }

    /* A re-activation that follows a capture sometimes finds the MCU deaf:
     * configure acks, REQTLS goes out, and then no TLS record ever arrives.
     * Replaying the post-staging sequence once with the line drained first
     * clears it, and costs 2 s only on that path. */
    for (attempt = 0; attempt < 2; attempt++) {
        /* Staging RAM must not reach target initialisation. */
        if (hard_reset_cycle(s) < 0) {
            fprintf(stderr, "gx51: post-staging reset failed: %s\n",
                    strerror(errno));
            memset(pmk, 0, sizeof(pmk));
            return -1;
        }
        SAY("gx51: firmware after post-staging reset: %s\n", s->version);

        if (gx51_configure_ex(&s->dev, &s->cal) < 0) {
            fprintf(stderr, "gx51: target configure failed\n");
            memset(pmk, 0, sizeof(pmk));
            return -1;
        }

        spi_drain(&s->dev);

        if (gx51_wait_idle(&s->dev, 50) < 0 ||
            gx51_write_frame(&s->dev, GX51_PKT_PLAIN, REQTLS_BODY,
                             sizeof(REQTLS_BODY)) < 0) {
            fprintf(stderr, "gx51: REQTLS failed: %s\n", strerror(errno));
            memset(pmk, 0, sizeof(pmk));
            return -1;
        }
        usleep(50000);

        s->tls = gx51_tls_start(&s->dev, pmk, 8000);
        if (s->tls != NULL)
            break;
        if (attempt == 0) {
            fprintf(stderr, "gx51: MCU silent after REQTLS, re-initialising\n");
            usleep(200000);
        }
    }
    memset(pmk, 0, sizeof(pmk));
    if (s->tls == NULL) {
        fprintf(stderr, "gx51: TLS session not established\n");
        return -1;
    }
    s->have_tls = 1;

    if (gx51_wakeup_mcu(&s->dev) < 0)
        fprintf(stderr, "gx51: WakeupMCU failed: %s\n", strerror(errno));
    return 0;
}

/* Drain application data for up to budget_ms into tls_plain. */
static int tls_collect(struct gx51_session *s, unsigned int budget_ms)
{
    long long start = now_ms();
    size_t len = 0;

    while (len < sizeof(tls_plain) && now_ms() - start < (long long)budget_ms) {
        ssize_t r = gx51_tls_read(s->tls, tls_plain + len,
                                  sizeof(tls_plain) - len);

        if (r < 0) {
            if (gx51_tls_timed_out(s->tls))
                break;
            fprintf(stderr, "gx51: TLS read failed: %s\n",
                    gx51_tls_error(s->tls));
            return -1;
        }
        len += (size_t)r;
        /* Once data flows the rest arrives promptly. */
        gx51_tls_set_timeout(s->tls, 1500);
    }
    return (int)len;
}

int gx51_session_capture(struct gx51_session *s, uint16_t *px)
{
    int got;

    if (s->tls == NULL) {
        fprintf(stderr, "gx51: capture without TLS session\n");
        return -1;
    }
    if (finger_recipe(s) < 0) {
        fprintf(stderr, "gx51: finger recipe failed\n");
        return -1;
    }

    if (gx51_wait_idle(&s->dev, 200) < 0) {
        fprintf(stderr, "gx51: line busy before GET_IMAGE\n");
        return -1;
    }
    if (gx51_write_frame(&s->dev, GX51_PKT_PLAIN, GET_IMAGE,
                         sizeof(GET_IMAGE)) < 0) {
        fprintf(stderr, "gx51: GET_IMAGE write failed: %s\n", strerror(errno));
        return -1;
    }
    sleep_us(15000);
    gx51_tls_set_timeout(s->tls, 5000);

    got = tls_collect(s, 8000);
    if (got < 0)
        return -1;
    if (got != (int)GX51_IMAGE_PLAIN_LEN) {
        fprintf(stderr, "gx51: image record is %d bytes, expected %u\n", got,
                (unsigned)GX51_IMAGE_PLAIN_LEN);
        return -1;
    }
    if (gx51_image_decode(tls_plain, (size_t)got, px) < 0) {
        fprintf(stderr, "gx51: image decode failed\n");
        return -1;
    }
    return 0;
}
