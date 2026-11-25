/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "gx51.h"
#include "crypto.h"
#include "image.h"
#include "tls.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const uint8_t install_body[] = {0x96, 0x03, 0x00, 0x01, 0x00, 0x10};

static int verbose;
/* Set while a helper polls the sensor on a timer: the per-frame trace would
 * otherwise bury the one line that matters. */
static int quiet;
/* How many accepted finger frames --tls-image should collect. */
static int capture_frames = 1;
static int capture_avg = 1;
static struct gx51_cal device_cal;
static int have_cal;

static int run_background_recipe(struct gx51_dev *dev,
                                 const struct gx51_cal *cal);

/*
 * Interactive cue. Printed to /dev/tty as well as stdout: when the probe runs
 * under `| tee` (or any wrapper) stdout is a pipe and the user watching the
 * terminal would otherwise miss the window in which to press the finger.
 */
static void prompt(const char *msg)
{
    FILE *tty = fopen("/dev/tty", "w");

    if (tty != NULL) {
        fprintf(tty, "\n>>> %s\n", msg);
        fflush(tty);
        fclose(tty);
    }
    printf("capture: %s\n", msg);
    fflush(stdout);
}

/* fprintd keeps the device claimed while an action runs, and a second client on
 * the same spidev/gpio usually produces nothing but timeouts, which look like a
 * sensor fault. Warn instead of silently interleaving with it. */
static int fprintd_running(void)
{
    FILE *p = popen("systemctl is-active --quiet fprintd 2>/dev/null", "r");

    if (p == NULL)
        return 0;
    return pclose(p) == 0;
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

static void dump(const char *label, const uint8_t *buf, size_t len)
{
    size_t i;

    if (!verbose)
        return;
    fprintf(stderr, "%s:", label);
    for (i = 0; i < len; i++)
        fprintf(stderr, " %02x", buf[i]);
    fputc('\n', stderr);
}

/* FDT reports are the only evidence of whether the sensor sees a finger, so
 * their bytes are always printed, verbose or not. */
static void dump_hex_always(const char *label, const uint8_t *buf, size_t len)
{
    size_t i;

    printf("%s (%zu):", label, len);
    for (i = 0; i < len; i++)
        printf(" %02x", buf[i]);
    printf("\n");
}

static int send_nop(struct gx51_dev *dev)
{
    static const uint8_t zeroes[4] = {0};
    uint8_t body[8];
    size_t len = gx51_build_body(body, sizeof(body), 0x00, zeroes, 4);

    if (len == 0)
        return -1;
    dump("TX NOP", body, len);
    if (gx51_write_frame(dev, GX51_PKT_PLAIN, body, len) < 0)
        return -1;
    return sleep_us(5000);
}

static int send_install(struct gx51_dev *dev)
{
    dump("TX install", install_body, sizeof(install_body));
    return gx51_write_frame(dev, GX51_PKT_PLAIN, install_body,
                            sizeof(install_body));
}

static int driverstate_install(struct gx51_dev *dev)
{
    int wrapper;
    int attempt;

    if (send_nop(dev) < 0)
        return -1;

    for (wrapper = 0; wrapper < 2; wrapper++) {
        for (attempt = 0; attempt < 2; attempt++) {
            uint8_t body[128];
            uint8_t type;
            uint8_t status;
            size_t len;
            int ret;

            if (send_install(dev) < 0)
                return -1;

            ret = gx51_read_frame(dev, &type, body, sizeof(body), &len);
            if (ret <= 0)
                continue;
            dump("RX install", body, len);
            if (type == GX51_PKT_PLAIN &&
                gx51_parse_ack(body, len, 0x96, &status) == 0)
                return 0;
        }
    }

    return -1;
}

static int read_version_once(struct gx51_dev *dev, char *version, size_t cap)
{
    uint8_t query[8];
    size_t query_len;
    int send;
    int acked = 0;

    if (send_nop(dev) < 0)
        return -1;

    query_len = gx51_build_body(query, sizeof(query), GX51_CMD_FW_VERSION,
                                (const uint8_t[]){0x00, 0x00}, 2);
    if (query_len == 0)
        return -1;

    for (send = 0; send < 2; send++) {
        int read;

        dump("TX version", query, query_len);
        if (gx51_write_frame(dev, GX51_PKT_PLAIN, query, query_len) < 0)
            return -1;

        for (read = 0; read < 3; read++) {
            uint8_t body[128];
            uint8_t type;
            uint8_t status;
            size_t len;
            int ret = gx51_read_frame(dev, &type, body, sizeof(body), &len);

            if (ret == -2) {
                fprintf(stderr, "version: sensor returned all-FF frame\n");
                break;
            }
            if (ret < 0)
                break;
            dump("RX version", body, len);
            if (type != GX51_PKT_PLAIN)
                continue;

            if (gx51_parse_ack(body, len, GX51_CMD_FW_VERSION, &status) == 0)
                acked = 1;
            else if (gx51_parse_version(body, len, version, cap) > 0)
                return 0;
        }

        if (acked) {
            uint8_t body[128];
            uint8_t type;
            size_t len;

            if (sleep_us(8000) < 0)
                return -1;
            if (gx51_read_frame(dev, &type, body, sizeof(body), &len) > 0) {
                dump("RX version", body, len);
                if (type == GX51_PKT_PLAIN &&
                    gx51_parse_version(body, len, version, cap) > 0)
                    return 0;
            }
        }
    }

    return -1;
}

static int read_version(struct gx51_dev *dev, char *version, size_t cap)
{
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        if (read_version_once(dev, version, cap) == 0)
            return 0;
        fprintf(stderr, "version attempt %d failed\n", attempt + 1);
    }
    return -1;
}

/* Full MCU reset in the middle of a run, as the vendor path does after the F2
 * factory-staging extraction. SPI and IRQ are closed across the GPIO264
 * toggle (the vendor never resets through an open transport), then reopened,
 * DriverState:Install is replayed and the firmware string is re-read so the
 * caller can prove the MCU came back on a clean boundary. */
static int hard_reset_cycle(struct gx51_dev *dev, char *version, size_t cap)
{
    const char *path = dev->spi_path[0] != '\0' ? dev->spi_path : NULL;

    if (dev->irq_fd >= 0) {
        close(dev->irq_fd);
        dev->irq_fd = -1;
    }
    if (dev->spi_fd >= 0) {
        close(dev->spi_fd);
        dev->spi_fd = -1;
    }

    if (gx51_reset(dev) < 0) {
        fprintf(stderr, "reset: GPIO264 toggle failed: %s\n", strerror(errno));
        return -1;
    }
    if (gx51_spi_open(dev, path) < 0) {
        fprintf(stderr, "reset: SPI reopen failed (%s): %s\n",
                path != NULL ? path : "auto", strerror(errno));
        return -1;
    }
    if (gx51_irq_open(dev) < 0) {
        fprintf(stderr, "reset: IRQ reopen failed: %s\n", strerror(errno));
        return -1;
    }
    if (driverstate_install(dev) < 0) {
        fprintf(stderr, "reset: DriverState install after reset failed\n");
        return -1;
    }
    return read_version(dev, version, cap);
}

/* Read-only diagnostic transactions. Nothing here writes device state. */
static void dump_always(const char *label, const uint8_t *buf, size_t len)
{
    size_t i;

    printf("%s (%zu):", label, len);
    for (i = 0; i < len; i++)
        printf(" %02x", buf[i]);
    printf("  |");
    for (i = 0; i < len; i++)
        putchar(buf[i] >= 0x20 && buf[i] < 0x7f ? buf[i] : '.');
    printf("|\n");
}

static int transact(struct gx51_dev *dev, const char *label,
                    const uint8_t *body, size_t len, int frames)
{
    int i;

    printf("TX %s:", label);
    for (i = 0; i < (int)len; i++)
        printf(" %02x", body[i]);
    printf("\n");

    if (gx51_write_frame(dev, GX51_PKT_PLAIN, body, len) < 0)
        return -1;

    for (i = 0; i < frames; i++) {
        uint8_t rx[256];
        uint8_t type;
        size_t rx_len;
        int ret = gx51_read_frame(dev, &type, rx, sizeof(rx), &rx_len);

        if (ret == -2) {
            printf("RX %s: all-FF\n", label);
            return -2;
        }
        if (ret < 0)
            return 0;
        if (type == GX51_PKT_PLAIN)
            dump_always(label, rx, rx_len);
    }
    return 0;
}

static void probe_info(struct gx51_dev *dev)
{
    /* reg read addr 0x0000 len 4 */
    static const uint8_t chip_id[] = {0x82, 0x06, 0x00, 0x00,
                                      0x00, 0x00, 0x04, 0x00, 0x1e};
    /* OTP read */
    static const uint8_t otp[] = {0xa6, 0x03, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t mcu_state = 0x55;
    uint8_t mcu[8];
    size_t mcu_len;

    mcu_len = gx51_build_body(mcu, sizeof(mcu), 0xae, &mcu_state, 1);

    printf("--- read-only device info ---\n");
    transact(dev, "chip-id", chip_id, sizeof(chip_id), 3);
    transact(dev, "otp", otp, sizeof(otp), 3);
    if (mcu_len != 0)
        transact(dev, "mcu-state", mcu, mcu_len, 3);
    printf("--- end device info ---\n");
}

/*
 * Read-only F2 memory probe. 0x08020000 is the app-flash window that the
 * reference firmware accepts; the factory slots below it are where the
 * factory record would live if 14108 admits them.
 */
static void f2_read(struct gx51_dev *dev, uint32_t addr, uint16_t req_len)
{
    uint32_t wire = addr - 0x08000000u;
    uint8_t body[12];
    uint8_t rx[512];
    uint8_t type;
    size_t n;
    int i;
    char path[64];
    FILE *out;
    char summary[160] = "";
    size_t used = 0;

    /* Length is a little-endian u32, not a u16. Casting 256 to u8 sent a
     * zero-length read, which is why every earlier sweep was echoed back
     * empty regardless of address. */
    body[0] = 0xf2;
    body[1] = 0x09;
    body[2] = 0x00;
    body[3] = (uint8_t)wire;
    body[4] = (uint8_t)(wire >> 8);
    body[5] = (uint8_t)(wire >> 16);
    body[6] = (uint8_t)(wire >> 24);
    body[7] = (uint8_t)(req_len & 0xffu);
    body[8] = (uint8_t)((req_len >> 8) & 0xffu);
    body[9] = (uint8_t)((req_len >> 16) & 0xffu);
    body[10] = (uint8_t)((req_len >> 24) & 0xffu);
    body[11] = gx51_body_checksum(body, 11u);

    if (gx51_wait_idle(dev, 50) < 0) {
        fprintf(stderr, "f2: line busy at 0x%08x\n", addr);
        return;
    }
    dump("TX f2", body, sizeof(body));
    if (gx51_write_frame(dev, GX51_PKT_PLAIN, body, sizeof(body)) < 0)
        return;

    snprintf(path, sizeof(path), "f2-%08x.bin", addr);
    out = fopen(path, "wb");

    for (i = 0; i < 4; i++) {
        int ret;

        usleep(8000);
        ret = gx51_read_frame(dev, &type, rx, sizeof(rx), &n);
        if (ret == -2) {
            snprintf(summary + used, sizeof(summary) - used, " all-FF");
            break;
        }
        if (ret == -1)
            break;
        if (ret < 0) {
            snprintf(summary + used, sizeof(summary) - used, " hdr-err%d", ret);
            break;
        }
        snprintf(summary + used, sizeof(summary) - used, " len%zu", n);
        used = strlen(summary);
        if (n <= 16)
            dump("RX f2", rx, n);
        else if (rx[0] == 0xf2u && n > 12u)
            snprintf(summary + used, sizeof(summary) - used, " DATA%zu",
                     n - 12u);
        if (out != NULL && fwrite(rx, 1, n, out) != n) {
            fclose(out);
            out = NULL;
        }
    }
    if (out != NULL)
        fclose(out);
    printf("f2 0x%08x%s\n", addr, summary);
}

static void staging_probe(struct gx51_dev *dev, const char *label)
{
    /* Bases the reference firmware actually uses. Only the first 80 bytes
     * (16-byte salt + four CBC blocks) are needed to recover the PMK, and the
     * 14115 boot staging record is only reachable at base + 8. */
    static const struct {
        uint32_t addr;
        uint16_t len;
        const char *note;
    } reads[] = {
        {0x08020000u, 256u, "app flash control"},
        {0x08004000u, 256u, "staging base 1"},
        {0x08008000u, 256u, "staging base 2"},
        {0x0800a000u, 256u, "staging base 3"},
        {0x08004008u, 80u, "pmk block 1"},
        {0x08008008u, 80u, "pmk block 2"},
        {0x0800a008u, 80u, "pmk block 3"},
        {0x20000000u, 256u, "alternate selector"},
    };
    size_t i;

    printf("--- F2 sweep %s ---\n", label);
    for (i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
        printf("f2: %s\n", reads[i].note);
        f2_read(dev, reads[i].addr, reads[i].len);
        sleep_us(20000);
    }
    printf("--- frames written to f2-<addr>.bin ---\n");
}

/*
 * Finger capture attempt.
 *
 * Reply counts matter more than the command bytes here. The reference
 * implementation documents them as: 0x32 (FDT down) -> ACK + 20-byte FDT
 * data report, 0x36 (FDT manual) -> 2, 0xae (MCU state) -> 2, 0x82 -> 2,
 * 0x80 -> 1, NOP -> none. Draining only one reply leaves the IRQ line high
 * and the next command is swallowed, which is exactly how the previous run
 * died before GET_IMAGE was ever clocked out.
 */
static const uint8_t FDT_FINGER[12] = {
    0x80, 0xb1, 0x80, 0xc1, 0x80, 0xa6,
    0x80, 0xb6, 0x80, 0xa5, 0x80, 0xb6,
};
static const uint8_t FDT_DOWN[12] = {
    0x80, 0xb2, 0x80, 0xc2, 0x80, 0xa7,
    0x80, 0xb6, 0x80, 0xa6, 0x80, 0xb6,
};
/* Cold background (ImageBase) recipe zones, from the validated capture path. */
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

static uint8_t big_frame[65536];

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

static void save_blob(const char *name, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(name, "wb");

    if (f == NULL) {
        printf("capture: cannot write %s: %s\n", name, strerror(errno));
        return;
    }
    if (fwrite(buf, 1, len, f) != len) {
        printf("capture: short write on %s\n", name);
        fclose(f);
        return;
    }
    fclose(f);
    printf("capture: wrote %s (%zu bytes)\n", name, len);
}

struct drain {
    int plain;
    int tls;
    size_t last_len;
    uint8_t last_type;
    uint8_t last[24];   /* first bytes of the last plain frame, for reports */
};

#define SAY(...) do { if (!quiet) printf(__VA_ARGS__); } while (0)

/* Send one command and consume the replies it is documented to produce.
 * expected_plain == 0 means "unknown, drain until silence".
 */
static int drain_cmd(struct gx51_dev *dev, const char *label,
                     const uint8_t *body, size_t len,
                     int expected_plain, unsigned int window_ms,
                     struct drain *res)
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

    if (gx51_wait_idle(dev, 50) < 0) {
        SAY("capture: %s -> line busy, not sent\n", label);
        return -1;
    }
    dump(label, body, len);
    if (gx51_write_frame(dev, GX51_PKT_PLAIN, body, len) < 0) {
        SAY("capture: %s -> write failed: %s\n", label, strerror(errno));
        return -1;
    }

    /* NOP has no reply on this sensor and must not be waited on. */
    if (body[0] == 0x00u)
        return 0;

    usleep(15000);

    while (waited < window_ms) {
        errno = 0;
        ret = gx51_read_frame_to(dev, 10, &type, big_frame,
                                 sizeof(big_frame), &n);
        waited += 10;

        if (ret == -1) {
            if (errno == ETIMEDOUT) {
                misses++;
                /* Known-count commands stop on silence, but the FDT report
                 * follows its ACK by 60-88 ms on this sensor, so wait a full
                 * 120 ms before giving up. GET_IMAGE keeps waiting because
                 * its record arrives asynchronously after the ACK. */
                if (expected_plain > 0 && res->plain > 0 && misses >= 12)
                    break;
                continue;
            }
            SAY("capture: %s -> spi error: %s\n", label, strerror(errno));
            return -1;
        }
        if (ret == -2) {
            SAY("capture: %s -> all-FF\n", label);
            break;
        }
        if (ret < 0) {
            SAY("capture: %s -> header error %d\n", label, ret);
            break;
        }

        misses = 0;
        res->last_type = type;
        res->last_len = n;
        if (type == GX51_PKT_PLAIN) {
            size_t keep = n < sizeof(res->last) ? n : sizeof(res->last);

            memcpy(res->last, big_frame, keep);
        }

        if (type == GX51_PKT_TLS) {
            res->tls++;
            SAY("capture: %s -> TLS record %zu bytes\n", label, n);
            if (!quiet) save_blob("tls-record.bin", big_frame, n);
            break;
        }

        res->plain++;
        if (n == 6) {
            uint8_t status = 0;

            dump("  ack", big_frame, n);
            if (gx51_parse_ack(big_frame, n, body[0], &status) == 0)
                SAY("capture: %s -> ack status 0x%02x (%s)\n", label,
                       status, (status & 1u) ? "accepted" : "rejected");
            else if (big_frame[0] == 0xd0u)
                SAY("capture: %s -> device requests TLS (code 0x%02x)\n",
                       label, big_frame[3]);
            else {
                SAY("capture: %s -> frame %zu bytes, unparsed\n",
                       label, n);
                if (!quiet) dump_hex_always("  frame", big_frame, n);
            }
        } else {
            SAY("capture: %s -> plain frame #%d, %zu bytes%s\n", label,
                   res->plain, n, n > 64 ? " (payload)" : "");
            if (!quiet) dump_hex_always("  frame", big_frame, n < 32 ? n : 32);
            if (n > 1024)
                if (!quiet) save_blob("image.bin", big_frame, n);
        }

        if (expected_plain > 0 && res->plain >= expected_plain)
            break;
        usleep(5000);
    }

    if (res->plain == 0 && res->tls == 0)
        SAY("capture: %s -> nothing after %u ms\n", label, waited);
    return 0;
}

static void probe_capture(struct gx51_dev *dev)
{
    static const uint8_t nop[] = {0x00, 0x05, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0xa5};
    static const uint8_t mcu_byte = 0x55;
    static const uint8_t img_payload[] = {0x01, 0x00};
    uint8_t body[32];
    size_t n;
    int attempt;
    int got_any = 0;

    printf("--- finger capture attempt (reply counts per reference) ---\n");

    n = build_fdt(body, sizeof(body), 0x0du, FDT_FINGER);
    if (n != 0)
        drain_cmd(dev, "fdt 0x0d finger-mode", body, n, 2, 400, NULL);
    usleep(20000);

    n = build_fdt(body, sizeof(body), 0x0cu, FDT_FINGER);
    if (n != 0)
        drain_cmd(dev, "fdt 0x0c finger-mode", body, n, 2, 400, NULL);
    usleep(20000);

    drain_cmd(dev, "nop", nop, sizeof(nop), 0, 0, NULL);
    usleep(20000);

    n = gx51_build_body(body, sizeof(body), 0xae, &mcu_byte, 1);
    if (n != 0)
        drain_cmd(dev, "mcu state 0x55", body, n, 2, 200, NULL);
    usleep(20000);

    n = build_fdt(body, sizeof(body), 0x0cu, FDT_DOWN);
    if (n != 0)
        drain_cmd(dev, "fdt 0x0c capture-down", body, n, 2, 400, NULL);

    n = gx51_build_body(body, sizeof(body), 0x20, img_payload,
                        sizeof(img_payload));
    if (n == 0)
        return;

    for (attempt = 1; attempt <= 2; attempt++) {
        struct drain d;

        usleep(20000);
        if (drain_cmd(dev, "get-image", body, n, 0, 1000, &d) < 0)
            return;
        if (d.tls || d.plain) {
            got_any = 1;
            if (d.tls)
                printf("capture: verdict = image is behind TLS, PSK still required\n");
            else if (d.last_len > 1024)
                printf("capture: verdict = plaintext image frame, PSK not needed\n");
            else
                printf("capture: verdict = ACK only, no image record in 1000 ms\n");
            break;
        }
        printf("capture: get-image attempt %d/2 got nothing, replaying\n",
               attempt);
        usleep(8000);
    }

    if (!got_any)
        printf("capture: verdict = no response to GET_IMAGE\n");
}

/*
 * REQTLS: the command Windows sends after target init and before the TLS
 * handshake. If the sensor is the TLS client it answers with a ClientHello
 * record, which reveals the real ciphersuite list on this 14108 unit.
 */
static const uint8_t REQTLS[6] = {0xd0, 0x03, 0x00, 0x00, 0x00, 0xd7};

static void dump_hello(const uint8_t *buf, size_t len)
{
    size_t i, j;

    printf("reqtls: TLS record header %02x %02x %02x %02x %02x\n",
           buf[0], buf[1], buf[2], buf[3], buf[4]);
    if (len < 11)
        return;
    printf("reqtls: handshake type 0x%02x, client version %02x %02x\n",
           buf[5], buf[9], buf[10]);
    printf("reqtls: first 96 bytes:");
    for (i = 0; i < len && i < 96; i++)
        printf(" %02x", buf[i]);
    printf("\n");

    /* Cipher suite vector sits after type(1) ver(2) random(32) sid(1+n). */
    if (len > 43) {
        size_t sid = buf[43];
        size_t off = 44 + sid;

        if (off + 2 <= len) {
            size_t csl = ((size_t)buf[off] << 8) | buf[off + 1];

            printf("reqtls: offered suites:");
            for (j = 0; j + 1 < csl && off + 2 + j + 1 < len; j += 2)
                printf(" %02x%02x", buf[off + 2 + j], buf[off + 3 + j]);
            printf("\n");
        }
    }
}

static void probe_reqtls(struct gx51_dev *dev)
{
    struct drain d;
    unsigned int waited = 0;
    uint8_t type;
    size_t n;
    int ret;

    printf("--- REQTLS probe ---\n");
    if (drain_cmd(dev, "reqtls", REQTLS, sizeof(REQTLS), 1, 600, &d) < 0)
        return;

    for (waited = 0; waited < 4000; waited += 50) {
        errno = 0;
        ret = gx51_read_frame_to(dev, 50, &type, big_frame,
                                 sizeof(big_frame), &n);
        if (ret == -1) {
            if (errno == ETIMEDOUT)
                continue;
            printf("reqtls: spi error: %s\n", strerror(errno));
            return;
        }
        if (ret == -2) {
            printf("reqtls: all-FF after request\n");
            return;
        }
        if (ret < 0) {
            printf("reqtls: header error %d\n", ret);
            return;
        }
        printf("reqtls: frame type 0x%02x len %zu\n", type, n);
        if (type == GX51_PKT_TLS) {
            save_blob("tls-clienthello.bin", big_frame, n);
            dump_hello(big_frame, n);
            return;
        }
        dump("  frame", big_frame, n < 48 ? n : 48);
    }
    printf("reqtls: no TLS record within 4 s\n");
}

/*
 * When the PMK cannot be recovered, print what each factory slot actually
 * returns. Without this the only message is "not recovered", which does not
 * distinguish a blocked read from a record that no longer decrypts.
 */
static void pmk_diagnose(struct gx51_dev *dev)
{
    static const uint32_t bases[] = {0x08008000u, 0x08004000u, 0x0800a000u};
    uint8_t rec[96];
    uint8_t tmp[GX51_PMK_LEN];
    size_t i;

    printf("pmk: rescanning factory slots\n");
    for (i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        size_t n = 0;
        size_t j;

        if (gx51_mem_read(dev, bases[i] + 8u, 96u, rec, sizeof(rec), &n) < 0) {
            printf("pmk: base 0x%08x read failed\n", bases[i]);
            continue;
        }
        printf("pmk: base 0x%08x %zu bytes:", bases[i], n);
        for (j = 0; j < n && j < 16u; j++)
            printf(" %02x", rec[j]);
        printf(" decrypt %s\n",
               gx51_pmk_from_record(rec, n, tmp) == 0 ? "ok" : "fail");
        memset(tmp, 0, sizeof(tmp));
        usleep(20000);
    }
    memset(rec, 0, sizeof(rec));
}

/*
 * Factory PMK: read the 80-byte factory record from flash and decrypt it.
 * The key never leaves memory and is not printed - only a fingerprint of it,
 * so two runs can be compared without exposing the secret.
 */
static void probe_pmk(struct gx51_dev *dev)
{
    uint8_t pmk[GX51_PMK_LEN];
    uint8_t digest[32];
    unsigned int i;

    printf("--- factory PMK ---\n");
    if (gx51_fetch_pmk(dev, pmk) < 0) {
        printf("pmk: not recovered\n");
        pmk_diagnose(dev);
        return;
    }

    gx51_sha256(pmk, sizeof(pmk), digest);
    printf("pmk: recovered, 48 bytes, sha256 ");
    for (i = 0; i < 8; i++)
        printf("%02x", digest[i]);
    printf("\n");
    printf("pmk: TLS-PSK material ready (memory only)\n");
    memset(pmk, 0, sizeof(pmk));
}

/*
 * TLS-PSK handshake. The sensor is the TLS client, so this side plays server:
 * REQTLS tells the sensor to start, it answers with ClientHello, and OpenSSL
 * drives the rest through the SPI BIO.
 *
 * Nothing may be read from the SPI line directly once this returns - every
 * later frame belongs to the TLS session and only OpenSSL may consume it.
 */
static struct gx51_tls *tls_open(struct gx51_dev *dev)
{
    uint8_t pmk[GX51_PMK_LEN];
    char fw[64] = "";
    struct gx51_tls *tls;

    if (gx51_fetch_pmk(dev, pmk) < 0) {
        printf("tls: PMK not recovered, cannot key the session\n");
        pmk_diagnose(dev);
        return NULL;
    }

    /* Vendor order, and the reason earlier runs sat at ACK-only: the F2
     * factory-staging read leaves transient RAM behind that must never reach
     * target initialisation. Reset the MCU, re-install DriverState, re-read the
     * firmware string, then configure targets before REQTLS. */
    printf("tls: post-staging reset before target configure\n");
    if (hard_reset_cycle(dev, fw, sizeof(fw)) < 0) {
        printf("tls: post-staging reset failed: %s\n", strerror(errno));
        memset(pmk, 0, sizeof(pmk));
        return NULL;
    }
    printf("tls: firmware after post-staging reset: %s\n", fw);

    if (gx51_configure_ex(dev, &device_cal) < 0) {
        printf("tls: target configure failed\n");
        memset(pmk, 0, sizeof(pmk));
        return NULL;
    }
    have_cal = 1;

    if (gx51_wait_idle(dev, 50) < 0) {
        printf("tls: line busy, REQTLS not sent\n");
        memset(pmk, 0, sizeof(pmk));
        return NULL;
    }
    dump("TX reqtls", REQTLS, sizeof(REQTLS));
    if (gx51_write_frame(dev, GX51_PKT_PLAIN, REQTLS, sizeof(REQTLS)) < 0) {
        printf("tls: REQTLS write failed: %s\n", strerror(errno));
        memset(pmk, 0, sizeof(pmk));
        return NULL;
    }
    usleep(20000);

    tls = gx51_tls_start(dev, pmk, 8000);
    memset(pmk, 0, sizeof(pmk));
    if (tls == NULL)
        printf("tls: session not established\n");
    return tls;
}

static void probe_tls(struct gx51_dev *dev)
{
    struct gx51_tls *tls;

    printf("--- TLS-PSK session ---\n");
    tls = tls_open(dev);
    if (tls == NULL)
        return;
    printf("tls: verdict = secure channel up\n");
    gx51_tls_stop(tls);
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * First bytes of an application-data record tell us what the sensor is doing:
 * a command frame is [cmd][len16][payload][ck]. Print just enough to identify
 * it without dumping an entire image into the console.
 */
static void show_app_record(const uint8_t *buf, size_t len)
{
    size_t i;

    printf("tls: app data %zu bytes:", len);
    for (i = 0; i < len && i < 24; i++)
        printf(" %02x", buf[i]);
    if (len > 24)
        printf(" ...");
    printf("\n");

    if (len >= 6u && buf[1] == 0x03u && buf[2] == 0x00u) {
        size_t payload = (size_t)buf[3] | ((size_t)buf[4] << 8);

        printf("tls:   frame cmd 0x%02x len %zu\n", buf[0], payload);
    }
}

static uint8_t tls_plain[1u << 20];
static size_t tls_plain_len;

/*
 * Drain application data for up to budget_ms and append it to tls_plain.
 * A timeout at the end of a burst is normal, so it is reported as the end of
 * the stream rather than an error.
 */
static void tls_collect(struct gx51_tls *tls, const char *phase,
                        unsigned int budget_ms)
{
    long long start = now_ms();
    size_t before = tls_plain_len;
    int reads = 0;

    while (tls_plain_len < sizeof(tls_plain) &&
           now_ms() - start < (long long)budget_ms) {
        ssize_t r = gx51_tls_read(tls, tls_plain + tls_plain_len,
                                  sizeof(tls_plain) - tls_plain_len);

        if (r < 0) {
            if (gx51_tls_timed_out(tls)) {
                printf("tls: %s stream idle at %zu bytes\n", phase,
                       tls_plain_len);
            } else {
                printf("tls: %s read failed: %s\n", phase,
                       gx51_tls_error(tls));
            }
            break;
        }
        reads++;
        show_app_record(tls_plain + tls_plain_len, (size_t)r);
        tls_plain_len += (size_t)r;

        /* Once data flows the remaining records arrive promptly; a shorter
         * tail timeout keeps the run from hanging on an idle sensor. */
        gx51_tls_set_timeout(tls, 1500);
    }

    printf("tls: %s -> %d read(s), %zu new bytes\n", phase, reads,
           tls_plain_len - before);
}

/*
 * FDT report body: [tag][len16][status][pad][touchflag][6 x u16 zones][ck].
 * Tag is 0x36 for the manual probe and 0x32 for a finger-down report; both
 * carry the same payload layout.
 */
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

/* One recipe step: send, drain the documented reply count, decode any FDT
 * report. The 30 ms gap matches the validated capture pacing. */
static int step(struct gx51_dev *dev, const char *label, const uint8_t *body,
                size_t len, int replies)
{
    struct drain d;

    if (drain_cmd(dev, label, body, len, replies, 400, &d) < 0)
        return -1;

    if (d.last_type == GX51_PKT_PLAIN && d.last_len <= sizeof(d.last)) {
        uint8_t touchflag = 0;
        uint16_t zones[6];

        if (fdt_parse(d.last, d.last_len, &touchflag, zones) == 0) {
            int k;
            int sum = 0;
            int live = 0;

            for (k = 0; k < 6; k++) {
                sum += zones[k];
                live += zones[k] != 0;
            }
            printf("    fdt: touch=0x%02x zones=%d mean=%d live=%d\n",
                   touchflag, fdt_zone_count(touchflag), sum / 6, live);
        }
    }
    return sleep_us(30000);
}

/* Finger-detection gates built on the FDT report.
 *
 * Before this existed the capture recipe waited a fixed 800 ms after the
 * prompt. That cannot tell "finger never landed" from "finger landed 100 ms
 * late", so every retry was a coin flip. The sensor answers touchflag 0x3f
 * with all six zones set when a finger is down (idle is 0x00, mean about 350;
 * pressed drops the mean by 150+), so the recipe can simply wait for it. */
static int fdt_sample(struct gx51_dev *dev, const uint8_t *body, size_t n,
                      uint8_t *touch, int *mean)
{
    struct drain d;
    uint8_t flags = 0;
    uint16_t zones[6];
    int sum = 0;
    int k;

    quiet = 1;
    if (drain_cmd(dev, "fdt sample", body, n, 2, 250, &d) < 0) {
        quiet = 0;
        return -1;
    }
    quiet = 0;

    if (d.last_type != GX51_PKT_PLAIN || d.last_len > sizeof(d.last) ||
        fdt_parse(d.last, d.last_len, &flags, zones) != 0)
        return -1;

    for (k = 0; k < 6; k++)
        sum += zones[k];
    *touch = flags;
    *mean = sum / 6;
    return 0;
}

static int wait_for_touch(struct gx51_dev *dev, int timeout_ms, int *mean_out)
{
    uint8_t body[32];
    size_t n;
    long long start;
    long long elapsed;
    uint8_t touch = 0;
    int mean = 0;
    int shots = 0;

    n = build_fdt(body, sizeof(body), 0x0du, FDT_FINGER);
    if (n == 0)
        return -1;

    start = now_ms();
    for (;;) {
        elapsed = now_ms() - start;
        if (elapsed >= timeout_ms)
            break;
        if (fdt_sample(dev, body, n, &touch, &mean) == 0 &&
            fdt_zone_count(touch) >= 5) {
            printf("capture: finger detected after %lld ms "
                   "(touch=0x%02x mean=%d)\n", elapsed, touch, mean);
            if (mean_out != NULL)
                *mean_out = mean;
            return 0;
        }
        if (++shots % 5 == 0)
            printf("capture: waiting for finger... touch=0x%02x mean=%d\n",
                   touch, mean);
        usleep(20000);
    }

    printf("capture: no finger after %d ms (touch=0x%02x mean=%d)\n",
           timeout_ms, touch, mean);
    if (mean_out != NULL)
        *mean_out = mean;
    return -1;
}

/* Idle gate for the background pass: the reference ImageBase is only valid
 * when the sensor proves it is unoccupied (touch clear, mean near 350). */
static int wait_for_clear(struct gx51_dev *dev, int timeout_ms)
{
    uint8_t body[32];
    size_t n;
    long long start;
    uint8_t touch = 0xff;
    int mean = 0;
    int shots = 0;

    n = build_fdt(body, sizeof(body), 0x0du, FDT_FINGER);
    if (n == 0)
        return -1;

    start = now_ms();
    for (;;) {
        long long elapsed = now_ms() - start;

        if (elapsed >= timeout_ms)
            break;
        if (fdt_sample(dev, body, n, &touch, &mean) == 0 &&
            fdt_zone_count(touch) == 0 && mean >= 300) {
            printf("capture: sensor clear (touch=0x00 mean=%d)\n", mean);
            return 0;
        }
        if (++shots % 5 == 0)
            printf("capture: waiting for a clear sensor... touch=0x%02x "
                   "mean=%d\n", touch, mean);
        usleep(20000);
    }

    printf("capture: sensor still occupied after %d ms (touch=0x%02x mean=%d); "
           "capturing anyway\n", timeout_ms, touch, mean);
    return -1;
}

static int write_dac(struct gx51_dev *dev, const struct gx51_cal *cal)
{
    struct {
        uint16_t addr;
        uint16_t value;
        const char *name;
    } regs[4] = {
        {0x0220u, cal->dac_main, "dac-main"},
        {0x0236u, cal->dac1, "dac1"},
        {0x0238u, cal->dac2, "dac2"},
        {0x023au, cal->dac3, "dac3"},
    };
    uint8_t body[32];
    /* Vendor 0x80 payload: selector 0x00, then address, then value. */
    uint8_t payload[5];
    size_t i;
    size_t n;

    for (i = 0; i < 4u; i++) {
        payload[0] = 0x00;
        payload[1] = (uint8_t)regs[i].addr;
        payload[2] = (uint8_t)(regs[i].addr >> 8);
        payload[3] = (uint8_t)regs[i].value;
        payload[4] = (uint8_t)(regs[i].value >> 8);
        n = gx51_build_body(body, sizeof(body), 0x80, payload,
                            sizeof(payload));
        if (n == 0 || step(dev, regs[i].name, body, n, 1) < 0)
            return -1;
    }
    return 0;
}

/*
 * FDT poll. The six zone values are the cheapest proof that the analog front
 * end is alive: idle zones read a few hundred counts and a finger pulls them
 * down by about a hundred. All-zero zones next to touch=0x3f means the MCU is
 * answering from a capture path that was never armed - see the D4
 * confirmation in src/tls.c.
 */
static void probe_fdt_poll(struct gx51_dev *dev, int seconds)
{
    struct gx51_tls *tls;
    uint8_t body[32];
    size_t n;
    long long start;
    int round = 0;
    int announced = 0;

    printf("--- FDT poll, %d s ---\n", seconds);
    tls = tls_open(dev);
    if (tls == NULL)
        return;

    n = build_fdt(body, sizeof(body), 0x0du, FDT_FINGER);
    if (n == 0u) {
        gx51_tls_stop(tls);
        return;
    }

    /* Zone reading before anything is armed. In the vendor path the first FDT
     * poll happens here too, right after the handshake - if these zones are
     * already live, the front end is up and any later all-zero round is caused
     * by the capture recipe, not by target initialisation. The tail of the
     * handshake can still hold the line high, so wait for idle first: the
     * vendor does exactly this before its first anchor probe. */
    if (gx51_wait_idle(dev, 250) == 0)
        usleep(100000);
    else
        printf("fdt poll: line still busy after handshake\n");
    step(dev, "fdt pre-arm", body, n, 2);

    if (gx51_wakeup_mcu(dev) < 0) {
        printf("fdt poll: WakeupMCU failed: %s\n", strerror(errno));
    } else {
        printf("fdt poll: WakeupMCU (0f 00 00 0e) sent\n");
    }

    step(dev, "fdt post-wakeup", body, n, 2);

    /* Zones only read live once ImageBase exists, so run the cold background
     * recipe first when OTP calibration is already known. The recipe stops
     * before GET_IMAGE, leaving no pending TLS record on the bus. */
    if (have_cal && run_background_recipe(dev, &device_cal) < 0) {
        printf("fdt poll: background recipe failed, zones may stay at zero\n");
    }

    start = now_ms();
    while (now_ms() - start < (long long)seconds * 1000LL) {
        round++;
        if (!announced && now_ms() - start > 3000) {
            prompt("press a finger on the sensor now");
            announced = 1;
        }
        step(dev, "fdt 0x0d poll", body, n, 2);
    }

    printf("fdt poll: %d round(s)\n", round);
    gx51_tls_stop(tls);
}

/*
 * Cold background capture: bring the analog front end up (NOP, MCU state,
 * FDT boot, NAV, FDT background), replay the OTP DAC trim, then take one
 * image. This is the recipe the vendor path uses to build ImageBase; without
 * it the sensor answers GET_IMAGE with an empty buffer.
 */
static int run_background_recipe(struct gx51_dev *dev,
                                 const struct gx51_cal *cal)
{
    static const uint8_t nop[] = {0x00, 0x05, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0xa5};
    static const uint8_t nav_payload[] = {0x01, 0x00};
    static const uint8_t mcu_byte = 0x55;
    uint8_t body[32];
    uint8_t reg_read[5] = {0x00, 0x82, 0x00, 0x02, 0x00};
    size_t n;

    if (step(dev, "nop", nop, sizeof(nop), 0) < 0)
        return -1;

    n = gx51_build_body(body, sizeof(body), 0xae, &mcu_byte, 1);
    if (n == 0 || step(dev, "mcu state 0x55", body, n, 2) < 0)
        return -1;

    n = build_fdt(body, sizeof(body), 0x0du, FDT_BOOT);
    if (n == 0 || step(dev, "fdt 0x0d boot", body, n, 2) < 0)
        return -1;

    n = gx51_build_body(body, sizeof(body), 0x50, nav_payload,
                        sizeof(nav_payload));
    if (n == 0 || step(dev, "nav", body, n, 2) < 0)
        return -1;

    n = build_fdt(body, sizeof(body), 0x0du, FDT_BACKGROUND);
    if (n == 0 || step(dev, "fdt 0x0d background", body, n, 2) < 0)
        return -1;

    n = gx51_build_body(body, sizeof(body), 0x82, reg_read,
                        sizeof(reg_read));
    if (n == 0 || step(dev, "reg read 0x0082", body, n, 2) < 0)
        return -1;

    return write_dac(dev, cal);
}

/* Finger capture: the sensor must see the down event before GET_IMAGE. */
static int run_finger_recipe(struct gx51_dev *dev)
{
    static const uint8_t nop[] = {0x00, 0x05, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0xa5};
    static const uint8_t mcu_byte = 0x55;
    uint8_t body[32];
    size_t n;

    n = build_fdt(body, sizeof(body), 0x0du, FDT_FINGER);
    if (n == 0 || step(dev, "fdt 0x0d finger-mode", body, n, 2) < 0)
        return -1;

    n = build_fdt(body, sizeof(body), 0x0cu, FDT_FINGER);
    if (n == 0 || step(dev, "fdt 0x0c finger-mode", body, n, 2) < 0)
        return -1;

    if (step(dev, "nop", nop, sizeof(nop), 0) < 0)
        return -1;

    n = gx51_build_body(body, sizeof(body), 0xae, &mcu_byte, 1);
    if (n == 0 || step(dev, "mcu state 0x55", body, n, 2) < 0)
        return -1;

    n = build_fdt(body, sizeof(body), 0x0cu, FDT_DOWN);
    if (n == 0 || step(dev, "fdt 0x0c capture-down", body, n, 2) < 0)
        return -1;

    return 0;
}

/* Cleanup keeps the TLS record sequence healthy for the next capture. */
static void run_cleanup_recipe(struct gx51_dev *dev)
{
    static const uint8_t nav_payload[] = {0x01, 0x00};
    uint8_t body[32];
    size_t n;

    n = build_fdt(body, sizeof(body), 0x0eu, FDT_UP);
    if (n != 0)
        step(dev, "fdt 0x0e up", body, n, 2);
    n = gx51_build_body(body, sizeof(body), 0x50, nav_payload,
                        sizeof(nav_payload));
    if (n != 0)
        step(dev, "nav", body, n, 2);
    n = build_fdt(body, sizeof(body), 0x0cu, FDT_POST_DOWN);
    if (n != 0)
        step(dev, "fdt 0x0c post-down", body, n, 2);
}

/*
 * Encrypted finger capture.
 *
 * Two facts are now measured, not assumed:
 *
 *   1. Commands stay cleartext. Sending NOP or GET_IMAGE inside the tunnel
 *      gets no reply at all; the same bytes as 0xa0 frames work.
 *   2. Only the image direction is encrypted, as a single 0xb0 TLS record.
 *
 * So the sequence is: handshake, arm the sensor with the FDT commands in the
 * clear, ask for the image in the clear, then read the record through OpenSSL.
 */
/*
 * Send GET_IMAGE in the clear and collect the encrypted answer. Commands stay
 * plaintext on this sensor; only the image direction is TLS.
 */
static int request_image(struct gx51_dev *dev, struct gx51_tls *tls,
                         const char *phase)
{
    static const uint8_t get_image[] = {0x20, 0x03, 0x00, 0x01, 0x00, 0x86};

    tls_plain_len = 0;
    if (gx51_wait_idle(dev, 200) < 0) {
        printf("capture: line busy before get-image (%s)\n", phase);
        return -1;
    }
    dump("TX get-image (plain)", get_image, sizeof(get_image));
    if (gx51_write_frame(dev, GX51_PKT_PLAIN, get_image,
                         sizeof(get_image)) < 0) {
        printf("capture: get-image write failed: %s\n", strerror(errno));
        return -1;
    }
    sleep_us(15000);
    gx51_tls_set_timeout(tls, 5000);
    tls_collect(tls, phase, 8000);
    /* Keep the decrypted stream so a capture can be re-decoded offline and so
     * the unit tests run against real data instead of a synthetic ramp. */
    if (tls_plain_len == GX51_IMAGE_PLAIN_LEN) {
        char name[64];

        snprintf(name, sizeof(name), "plain-%s.bin", phase);
        save_blob(name, tls_plain, tls_plain_len);
        save_blob("tls-image-plain.bin", tls_plain, tls_plain_len);
    }
    return (int)tls_plain_len;
}

/* Average several exposures taken during a single press. The ridge modulation
 * sits a few dozen counts above a much larger pedestal, so per-frame read noise
 * turns into broken ridges and therefore into spurious minutiae; averaging is
 * the cheapest way to buy contrast back without touching the analogue path.
 */
static int request_image_average(struct gx51_dev *dev, struct gx51_tls *tls,
                                 uint16_t *dst, int n, const char *phase)
{
    static uint32_t acc[GX51_IMAGE_PIXELS];
    static uint16_t one[GX51_IMAGE_PIXELS];
    int k;
    int good = 0;

    if (n < 1)
        n = 1;
    memset(acc, 0, sizeof(acc));

    for (k = 0; k < n; k++) {
        int got = request_image(dev, tls, phase);

        if (got != (int)GX51_IMAGE_PLAIN_LEN ||
            gx51_image_decode(tls_plain, (size_t)got, one) != 0) {
            if (k == 0)
                return got;
            break;
        }
        {
            struct gx51_image_stats st;

            gx51_image_stats(one, GX51_IMAGE_PIXELS, &st);
            printf("capture: exposure %d/%d range %u mean %u\n", k + 1, n,
                   (unsigned)(st.max - st.min), (unsigned)st.mean);
        }
        for (size_t i = 0; i < GX51_IMAGE_PIXELS; i++)
            acc[i] += one[i];
        good++;
    }

    if (good == 0)
        return -1;
    for (size_t i = 0; i < GX51_IMAGE_PIXELS; i++)
        dst[i] = (uint16_t)(acc[i] / (uint32_t)good);
    if (good < n)
        printf("capture: averaged %d of %d exposures\n", good, n);
    return (int)GX51_IMAGE_PLAIN_LEN;
}

static int report_stats(const uint16_t *px, const char *label,
                        struct gx51_image_stats *st)
{
    gx51_image_stats(px, GX51_IMAGE_PIXELS, st);
    printf("capture: %s min=%u max=%u mean=%u p1=%u p99=%u live=%zu\n",
           label, st->min, st->max, st->mean, st->p1, st->p99, st->nonzero);
    return (int)st->max - (int)st->min;
}

/*
 * Full capture: one finger-off background pass to bring the analog front end
 * up and produce ImageBase, then repeated finger passes until the frame has
 * real contrast.
 *
 * Measured on this unit: a GET_IMAGE issued without the background pass (and
 * without NAV) returns all zeros for both the image and the FDT zones, i.e.
 * the sensor is alive but its measurement path is not started.
 */
static void probe_tls_image(struct gx51_dev *dev)
{
    static uint16_t bg[GX51_IMAGE_PIXELS];
    static uint16_t fg[GX51_IMAGE_PIXELS];
    struct gx51_image_stats sb;
    struct gx51_image_stats sf;
    struct gx51_tls *tls;
    int have_bg = 0;
    int have_fg = 0;
    int range = 0;
    int accepted = 0;
    int min_range = 0x7fffffff;
    int attempt;
    int got;

    printf("--- encrypted capture: background + finger ---\n");
    if (fprintd_running())
        printf("capture: warning: fprintd is running; if the sensor stops "
               "answering, run 'systemctl stop fprintd' and retry\n");
    /* tls_open() now performs the post-staging reset and the full target
     * configure, so device_cal is filled there - do not gate on --configure. */
    tls = tls_open(dev);
    if (tls == NULL)
        return;

    if (gx51_wakeup_mcu(dev) < 0)
        printf("capture: WakeupMCU failed: %s\n", strerror(errno));
    else
        printf("capture: WakeupMCU (0f 00 00 0e) sent\n");

    prompt("background pass - keep the sensor clear");
    wait_for_clear(dev, 8000);
    if (run_background_recipe(dev, &device_cal) < 0) {
        printf("capture: background recipe failed\n");
        goto done;
    }
    got = request_image(dev, tls, "background");
    if (got == (int)GX51_IMAGE_PLAIN_LEN &&
        gx51_image_decode(tls_plain, (size_t)got, bg) == 0) {
        have_bg = 1;
        report_stats(bg, "background", &sb);
        if (gx51_image_write_pgm("bg.pgm", bg) == 0)
            printf("capture: wrote bg.pgm\n");
    } else {
        printf("capture: background frame unusable (%d bytes)\n", got);
    }

    for (attempt = 1; attempt <= 5 * capture_frames; attempt++) {
        printf("capture: finger pass %d/%d (accepted %d/%d)\n", attempt,
               5 * capture_frames, accepted, capture_frames);
        {
            char cue[96];

            snprintf(cue, sizeof(cue),
                     "finger pass %d/%d - press and HOLD a finger on the sensor",
                     attempt, 5 * capture_frames);
            prompt(cue);
        }
        if (wait_for_touch(dev, 12000, NULL) < 0) {
            printf("capture: finger pass %d skipped, no contact\n", attempt);
            continue;
        }
        if (run_finger_recipe(dev) < 0) {
            printf("capture: finger recipe failed\n");
            break;
        }
        got = request_image_average(dev, tls, fg, capture_avg,
                                    "finger");
        prompt("you can lift the finger");
        if (got != (int)GX51_IMAGE_PLAIN_LEN ||
            gx51_image_decode(tls_plain, (size_t)got, fg) != 0) {
            printf("capture: finger frame unusable (%d bytes)\n", got);
            sleep_us(200000);
            continue;
        }
        have_fg = 1;
        range = report_stats(fg, "finger", &sf);
        if (range >= 64) {
            accepted++;
            if (range < min_range)
                min_range = range;
            if (capture_frames > 1) {
                char name[32];

                snprintf(name, sizeof(name), "finger-%02d.pgm", accepted);
                if (gx51_image_write_pgm(name, fg) == 0)
                    printf("capture: wrote %s (range %d)\n", name, range);
            }
            if (accepted >= capture_frames)
                break;
            wait_for_clear(dev, 4000);
            continue;
        }
        printf("capture: frame too flat (range %d), retrying\n", range);
        sleep_us(300000);
    }

    if (have_fg) {
        if (gx51_image_write_pgm("finger.pgm", fg) == 0)
            printf("capture: wrote finger.pgm\n");
        if (have_bg && gx51_image_write_diff_pgm("finger-diff.pgm", fg, bg) == 0)
            printf("capture: wrote finger-diff.pgm\n");
    }

    run_cleanup_recipe(dev);

    if (have_fg && range >= 64)
        printf("capture: verdict = image data present (%d/%d frames accepted, "
               "worst range %d)\n", accepted, capture_frames, min_range);
    else if (have_fg)
        printf("capture: verdict = frames captured but flat (range %d)\n",
               range);
    else
        printf("capture: verdict = no usable frame\n");

done:
    gx51_tls_stop(tls);
}

static void usage(const char *argv0)
{    fprintf(stderr,
            "usage: %s [--spi PATH] [--no-reset] [--info] [--configure]"
            " [--staging] [--image] [--reqtls] [--pmk] [--tls] [--tls-image]"
            " [--fdt-poll N]"
            " [--repeat N] [--verbose]\n"
            "\n"
            "Reads the GXFP51A0 firmware version. Requires root, the sensor\n"
            "bound to spidev, and fprintd stopped.\n"
            "\n"
            "  --info       also run read-only chip id / OTP / MCU state probes\n"
            "  --configure  run target init: OTP, calibration, config upload\n"
            "  --staging    read-only F2 probe of the factory staging record\n"
            "  --image      attempt a plaintext finger capture (no TLS)\n"
            "  --reqtls     send REQTLS and dump the sensor ClientHello\n"
            "  --pmk        recover the factory PMK from flash (read-only)\n"
            "  --tls        recover the PMK and run the TLS-PSK handshake\n"
            "  --tls-image  background pass + finger capture over TLS, writes\n"
            "               bg.pgm / finger.pgm / finger-diff.pgm\n"
            "  --frames N   with --tls-image: collect N accepted finger frames\n"
"  --avg N      average N exposures per press (default 1)\n"
            "  --fdt-poll N poll the FDT zones for N seconds (press a finger)\n"
            "  --repeat N   repeat install + version N times (stability check)\n",
            argv0);
}

int main(int argc, char **argv)
{
    struct gx51_dev dev = {
        .gpio_fd = -1,
        .irq_fd = -1,
        .spi_fd = -1,
        .spi_path = "",
    };
    const char *spi_path = NULL;
    int do_reset = 1;
    int want_info = 0;
    int want_config = 0;
    int want_staging = 0;
    int want_image = 0;
    int want_reqtls = 0;
    int want_pmk = 0;
    int want_tls = 0;
    int want_tls_image = 0;
    int want_fdt_poll = 0;
    int fdt_seconds = 8;
    int repeat = 1;
    char version[64];
    int i;
    int iter;
    int rc = 1;

    /* Line-buffered even when piped into tee, so interactive prompts appear
     * when they are printed rather than when the buffer happens to flush. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--spi") == 0 && i + 1 < argc) {
            spi_path = argv[++i];
        } else if (strcmp(argv[i], "--no-reset") == 0) {
            do_reset = 0;
        } else if (strcmp(argv[i], "--info") == 0) {
            want_info = 1;
        } else if (strcmp(argv[i], "--configure") == 0) {
            want_config = 1;
        } else if (strcmp(argv[i], "--staging") == 0) {
            want_staging = 1;
        } else if (strcmp(argv[i], "--image") == 0) {
            want_image = 1;
        } else if (strcmp(argv[i], "--reqtls") == 0) {
            want_reqtls = 1;
        } else if (strcmp(argv[i], "--pmk") == 0) {
            want_pmk = 1;
        } else if (strcmp(argv[i], "--tls") == 0) {
            want_tls = 1;
        } else if (strcmp(argv[i], "--tls-image") == 0) {
            want_tls_image = 1;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            capture_frames = atoi(argv[++i]);
            if (capture_frames < 1 || capture_frames > 30)
                capture_frames = 1;
        } else if (strcmp(argv[i], "--avg") == 0 && i + 1 < argc) {
            capture_avg = atoi(argv[++i]);
            if (capture_avg < 1 || capture_avg > 16)
                capture_avg = 1;
        } else if (strcmp(argv[i], "--fdt-poll") == 0) {
            want_fdt_poll = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                fdt_seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeat = atoi(argv[++i]);
            if (repeat < 1 || repeat > 100)
                repeat = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (gx51_gpio_open(&dev) < 0) {
        fprintf(stderr,
                "gpio: cannot open INT34BB gpiochip: %s\n"
                "need read/write access to /dev/gpiochipN\n",
                strerror(errno));
        goto out;
    }

    if (do_reset) {
        fprintf(stderr, "reset: GPIO264 HIGH 300 ms, LOW 600 ms\n");
        if (gx51_reset(&dev) < 0) {
            fprintf(stderr, "reset: failed: %s\n", strerror(errno));
            goto out;
        }
    }

    if (gx51_spi_open(&dev, spi_path) < 0) {
        fprintf(stderr,
                "spi: cannot open spidev: %s\n"
                "bind first: modprobe spidev; bind spi-GXFP51A0:00 to spidev\n",
                strerror(errno));
        goto out;
    }
    fprintf(stderr, "spi: %s mode 0 + CS_HIGH, 1 MHz\n", dev.spi_path);

    if (gx51_irq_open(&dev) < 0) {
        fprintf(stderr, "gpio: cannot request IRQ line %u: %s\n",
                GX51_IRQ_LINE, strerror(errno));
        goto out;
    }

    if (want_staging) {
        fprintf(stderr, "stage: F2 sweep after reset\n");
        staging_probe(&dev, "after reset");
    }

    for (iter = 0; iter < repeat; iter++) {
        if (repeat > 1)
            fprintf(stderr, "cycle %d/%d\n", iter + 1, repeat);

        fprintf(stderr, "stage: DriverState install\n");
        if (driverstate_install(&dev) < 0) {
            fprintf(stderr, "install: no ACK from sensor\n");
            goto out;
        }

        fprintf(stderr, "stage: firmware version\n");
        if (read_version(&dev, version, sizeof(version)) < 0) {
            fprintf(stderr, "version: no firmware response\n");
            goto out;
        }

        if (iter == 0 && want_info)
            probe_info(&dev);
    }

    printf("GXFP51A0 firmware: %s\n", version);

    if (want_staging) {
        fprintf(stderr, "stage: F2 sweep after install\n");
        staging_probe(&dev, "after install");
    }


    /* Factory PMK extraction first, target configuration second.
     *
     * The vendor path treats the F2 boot-staging read as a one-way trip: the
     * staging RAM is transient and must never reach target initialisation, so
     * a full MCU reset separates the two. Running configure first (the old
     * order) left every later command ACKed by a live MCU whose analog front
     * end was never armed: FDT zones read 0 and GET_IMAGE returned zeros. */
    if (want_pmk) {
        fprintf(stderr, "stage: factory PMK (boot staging)\n");
        probe_pmk(&dev);

        if (want_config && !(want_tls || want_tls_image || want_fdt_poll)) {
            fprintf(stderr, "stage: post-staging hard reset\n");
            if (hard_reset_cycle(&dev, version, sizeof(version)) < 0) {
                fprintf(stderr, "reset: post-staging cycle failed: %s\n",
                        strerror(errno));
                goto out;
            }
            printf("GXFP51A0 firmware after post-staging reset: %s\n",
                   version);
        }
    }

    if (want_config && !(want_tls || want_tls_image || want_fdt_poll)) {
        fprintf(stderr, "stage: target configure\n");
        if (gx51_configure_ex(&dev, &device_cal) < 0) {
            fprintf(stderr, "configure: failed\n");
            goto out;
        }
        have_cal = 1;
    }

    if (want_tls) {
        fprintf(stderr, "stage: TLS-PSK session\n");
        probe_tls(&dev);
    }

    if (want_fdt_poll) {
        fprintf(stderr, "stage: FDT poll\n");
        probe_fdt_poll(&dev, fdt_seconds);
    }

    if (want_tls_image) {
        fprintf(stderr, "stage: TLS image capture\n");
        probe_tls_image(&dev);
    }

    if (want_reqtls) {
        fprintf(stderr, "stage: REQTLS\n");
        probe_reqtls(&dev);
    }

    if (want_image) {
        fprintf(stderr, "stage: plaintext capture\n");
        probe_capture(&dev);
    }

    rc = 0;

out:
    gx51_close(&dev);
    return rc;
}
