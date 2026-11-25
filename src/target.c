/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "gx51.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define OTP_LEN 64u

static const uint8_t idle_body[] = {0x70, 0x03, 0x00, 0x14, 0x00, 0x23};
static const uint8_t soft_reset_body[] = {0xa2, 0x03, 0x00, 0x01, 0x14, 0xf0};
static const uint8_t chip_id_body[] = {0x82, 0x06, 0x00, 0x00,
                                       0x00, 0x00, 0x04, 0x00, 0x1e};
static const uint8_t otp_body[] = {0xa6, 0x03, 0x00, 0x00, 0x00, 0x01};

static int body_ok(const uint8_t *body, size_t len)
{
    uint16_t wire_len;

    if (body == NULL || len < 4)
        return 0;
    wire_len = (uint16_t)body[1] | ((uint16_t)body[2] << 8);
    return (size_t)wire_len + 3u == len &&
           body[len - 1] == gx51_body_checksum(body, len - 1);
}

static uint8_t crc8_goodix(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    size_t i;
    unsigned bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8u; bit++)
            crc = (uint8_t)((crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u)
                                          : (uint8_t)(crc << 1));
    }
    return crc;
}

static uint8_t otp_crc_join(const uint8_t otp[OTP_LEN],
                            const uint8_t *idx, size_t n)
{
    uint8_t tmp[32];
    size_t i;

    if (n > sizeof(tmp))
        return 0;
    for (i = 0; i < n; i++)
        tmp[i] = otp[idx[i]];
    return (uint8_t)~crc8_goodix(tmp, n);
}

int gx51_derive_cal(const uint8_t otp[OTP_LEN], struct gx51_cal *cal)
{
    static const uint8_t cp_idx[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                     36, 37, 38, 39};
    static const uint8_t mt_idx[] = {20, 21, 22, 23, 24, 25, 26, 27, 29, 30,
                                     31, 32, 33, 34, 35, 40, 41, 42, 43, 44,
                                     45, 46, 47, 48, 49, 54, 55};
    static const uint8_t ft_idx[] = {11, 12, 13, 14, 15, 16, 17, 18, 19, 28,
                                     50, 51, 52, 53, 56, 57, 58, 59, 62};
    unsigned raw_delta;
    unsigned a, b, c;

    if (otp == NULL || cal == NULL)
        return -1;

    if (otp_crc_join(otp, cp_idx, sizeof(cp_idx)) != otp[60] ||
        otp_crc_join(otp, mt_idx, sizeof(mt_idx)) != otp[63] ||
        otp_crc_join(otp, ft_idx, sizeof(ft_idx)) != otp[61] ||
        (uint8_t)~crc8_goodix(otp + 50, 4u) != otp[62] ||
        (uint8_t)~crc8_goodix(otp + 46, 4u) != otp[22] ||
        memcmp(otp + 46, otp + 50, 4u) != 0)
        return -1;
    if (otp[42] == 0u || otp[42] != (uint8_t)~otp[43])
        return -1;

    cal->tcode = (uint16_t)(((otp[42] >> 4) + 1u) * 16u + 64u);
    raw_delta = (unsigned)(((otp[42] & 0x0fu) + 2u) * 25600u /
                           cal->tcode / 3u);
    cal->fdt_delta = (uint8_t)((raw_delta >> 4) & 0xffu);

    a = otp[27] & 3u;
    b = (otp[27] >> 2) & 3u;
    c = (otp[27] >> 4) & 3u;
    (void)((a == c || a == b) ? a : (c == b ? c : 0u));

    cal->dac_main = (uint16_t)(((uint16_t)otp[46] << 4) | 8u);
    cal->dac1 = otp[47];
    cal->dac2 = otp[48];
    cal->dac3 = otp[49];
    return 0;
}

static uint16_t config_checksum(const uint8_t *cfg)
{
    uint32_t sum = 0xa5a5u;
    size_t i;

    for (i = 0; i + 1 < GX51_CONFIG_LEN - 2u; i += 2u)
        sum += (uint32_t)cfg[i] | ((uint32_t)cfg[i + 1u] << 8);
    return (uint16_t)(0u - sum);
}

static int config_checksum_valid(const uint8_t *cfg)
{
    uint16_t stored = (uint16_t)cfg[GX51_CONFIG_LEN - 2u] |
                      ((uint16_t)cfg[GX51_CONFIG_LEN - 1u] << 8);

    return stored == config_checksum(cfg);
}

static void config_fix_checksum(uint8_t *cfg)
{
    uint16_t sum = config_checksum(cfg);

    cfg[GX51_CONFIG_LEN - 2u] = (uint8_t)sum;
    cfg[GX51_CONFIG_LEN - 1u] = (uint8_t)(sum >> 8);
}

static int replace_section_value(uint8_t *cfg, unsigned section,
                                 uint16_t tag, uint16_t value)
{
    size_t table = 1u + section * 2u;
    size_t base;
    size_t size;
    size_t off;

    if (section >= 8u || table + 1u >= GX51_CONFIG_LEN)
        return -1;
    base = cfg[table];
    size = cfg[table + 1u];
    if ((size % 4u) != 0u || base + size > GX51_CONFIG_LEN - 2u)
        return -1;
    for (off = base; off + 3u < base + size; off += 4u) {
        uint16_t found = (uint16_t)cfg[off] |
                         ((uint16_t)cfg[off + 1u] << 8);

        if (found == tag) {
            cfg[off + 2u] = (uint8_t)value;
            cfg[off + 3u] = (uint8_t)(value >> 8);
            return 0;
        }
    }
    return -1;
}

int gx51_patch_config(uint8_t cfg[GX51_CONFIG_LEN],
                      const struct gx51_cal *cal)
{
    if (!config_checksum_valid(cfg))
        return -1;
    if (replace_section_value(cfg, 2u, 0x005cu, cal->tcode) < 0 ||
        replace_section_value(cfg, 3u, 0x005cu, cal->tcode) < 0 ||
        replace_section_value(cfg, 4u, 0x005cu, cal->tcode) < 0 ||
        replace_section_value(cfg, 2u, 0x0082u,
                              (uint16_t)(((uint16_t)cal->fdt_delta << 8) |
                                         0x80u)) < 0)
        return -1;
    config_fix_checksum(cfg);
    return config_checksum_valid(cfg) ? 0 : -1;
}

/*
 * ACK is level-high on GPIO48. The line must be low before a new command is
 * clocked out, otherwise the previous frame still owns the bus and the
 * command is swallowed. A swallowed command (no IRQ at all) is safe to
 * replay; a malformed or rejected ACK is final.
 *
 * The MCU answers slowly right after an idle transition (measured: a 0x80
 * write that follows 0x70 sometimes needs well over 100 ms), so the ACK
 * window is 250 ms and a swallowed command is replayed twice before giving
 * up. Stale frames left over from an earlier command are drained first so a
 * late reply cannot be mistaken for the current one.
 */
#define GX51_ACK_MS 250u
#define GX51_CMD_ATTEMPTS 3

static void drain_stale(struct gx51_dev *dev)
{
    uint8_t rx[GX51_MAX_BODY];
    uint8_t type;
    size_t n;
    int i;

    for (i = 0; i < 2; i++) {
        if (gx51_wait_irq(dev, 10) < 0)
            return;
        if (gx51_read_frame(dev, &type, rx, sizeof(rx), &n) <= 0)
            return;
        fprintf(stderr, "configure: dropped stale frame %zu bytes\n", n);
    }
}

static int send_wait_ack(struct gx51_dev *dev, const uint8_t *body,
                         size_t len, uint8_t cmd)
{
    uint8_t rx[GX51_MAX_BODY];
    uint8_t type;
    size_t n;
    uint8_t status;
    int attempt;

    drain_stale(dev);

    for (attempt = 0; attempt < GX51_CMD_ATTEMPTS; attempt++) {
        if (gx51_wait_idle(dev, 50) < 0) {
            fprintf(stderr, "configure: cmd 0x%02x blocked, line still high\n",
                    cmd);
            return -1;
        }
        if (gx51_write_frame(dev, GX51_PKT_PLAIN, body, len) < 0)
            return -1;

        if (gx51_wait_irq(dev, GX51_ACK_MS) < 0) {
            if (attempt + 1 < GX51_CMD_ATTEMPTS) {
                fprintf(stderr,
                        "configure: cmd 0x%02x no ACK in %u ms, replay %d\n",
                        cmd, GX51_ACK_MS, attempt + 1);
                usleep(8000);
                continue;
            }
            fprintf(stderr, "configure: cmd 0x%02x no ACK\n", cmd);
            return -1;
        }

        if (gx51_read_frame(dev, &type, rx, sizeof(rx), &n) <= 0)
            continue;
        if (type == GX51_PKT_PLAIN &&
            gx51_parse_ack(rx, n, cmd, &status) == 0)
            return 0;
        fprintf(stderr, "configure: cmd 0x%02x bad ACK %02x %02x %02x %02x\n",
                cmd, n > 0 ? rx[0] : 0, n > 1 ? rx[1] : 0,
                n > 2 ? rx[2] : 0, n > 4 ? rx[4] : 0);
        return -1;
    }
    return -1;
}

static int read_reply(struct gx51_dev *dev, uint8_t cmd,
                      uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t rx[GX51_MAX_BODY];
    uint8_t type;
    size_t n;
    int attempt;

    /* The ACK IRQ cycle must deassert before the response is armed. */
    usleep(8000);

    for (attempt = 0; attempt < 4; attempt++) {
        if (gx51_read_frame(dev, &type, rx, sizeof(rx), &n) <= 0)
            continue;
        if (type != GX51_PKT_PLAIN || n > cap || rx[0] != cmd ||
            !body_ok(rx, n))
            continue;
        memcpy(out, rx, n);
        *out_len = n;
        return 0;
    }
    return -1;
}

/*
 * 0x80 payload, from the vendor builder: {0x00, addr_lo, addr_hi, val_lo,
 * val_hi}. The leading 0x00 is a selector, not padding - without it the
 * address and value shift one byte left and the trim lands in the wrong
 * register. The MCU still ACKs, which is why this cost a whole evening.
 */
static int write_reg(struct gx51_dev *dev, uint16_t addr, uint16_t value)
{
    uint8_t body[9] = {0x80, 0x06, 0x00, 0x00, (uint8_t)addr,
                       (uint8_t)(addr >> 8), (uint8_t)value,
                       (uint8_t)(value >> 8), 0};
    int ret;

    body[8] = gx51_body_checksum(body, 8u);
    ret = send_wait_ack(dev, body, sizeof(body), GX51_CMD_REG_WRITE);
    fprintf(stderr, "configure: reg 0x%04x = 0x%04x %s\n", addr, value,
            ret == 0 ? "ack" : "FAILED");
    return ret;
}

/* Read back one 16-bit register so a write that was merely ACKed cannot be
 * mistaken for a write that took effect. */
static int read_reg(struct gx51_dev *dev, uint16_t addr, uint16_t *value)
{
    uint8_t payload[5] = {0x00, (uint8_t)addr, (uint8_t)(addr >> 8),
                          0x02, 0x00};
    uint8_t body[16];
    uint8_t rx[64];
    size_t n;
    size_t len;

    len = gx51_build_body(body, sizeof(body), GX51_CMD_CHIP_ID, payload,
                          sizeof(payload));
    if (len == 0)
        return -1;
    if (send_wait_ack(dev, body, len, GX51_CMD_CHIP_ID) < 0)
        return -1;
    if (read_reply(dev, GX51_CMD_CHIP_ID, rx, sizeof(rx), &n) < 0)
        return -1;
    if (n < 6u || !body_ok(rx, n))
        return -1;
    *value = (uint16_t)rx[3] | ((uint16_t)rx[4] << 8);
    return 0;
}

static int soft_reset(struct gx51_dev *dev)
{
    uint8_t rx[32];
    size_t n;
    uint32_t code;

    if (send_wait_ack(dev, soft_reset_body, sizeof(soft_reset_body),
                      GX51_CMD_SOFT_RESET) < 0)
        return -1;
    if (read_reply(dev, GX51_CMD_SOFT_RESET, rx, sizeof(rx), &n) < 0)
        return -1;
    if (n != 7u)
        return -1;
    code = ((uint32_t)rx[3] << 16) | ((uint32_t)rx[4] << 8) | rx[5];
    fprintf(stderr, "configure: soft reset code 0x%06x\n", code);
    return 0;
}

static int read_chip_id(struct gx51_dev *dev, uint16_t *chip_id)
{
    uint8_t rx[32];
    size_t n;

    if (send_wait_ack(dev, chip_id_body, sizeof(chip_id_body),
                      GX51_CMD_CHIP_ID) < 0)
        return -1;
    if (read_reply(dev, GX51_CMD_CHIP_ID, rx, sizeof(rx), &n) < 0)
        return -1;
    if (n != 8u)
        return -1;
    *chip_id = (uint16_t)rx[4] | ((uint16_t)rx[5] << 8);
    return 0;
}

static int read_otp(struct gx51_dev *dev, uint8_t otp[OTP_LEN])
{
    uint8_t rx[128];
    size_t n;

    if (send_wait_ack(dev, otp_body, sizeof(otp_body), GX51_CMD_OTP) < 0)
        return -1;
    if (read_reply(dev, GX51_CMD_OTP, rx, sizeof(rx), &n) < 0)
        return -1;
    if (n != OTP_LEN + 4u)
        return -1;
    memcpy(otp, rx + 3, OTP_LEN);
    return 0;
}

static int upload_config(struct gx51_dev *dev, uint8_t *cfg)
{
    uint8_t body[GX51_CONFIG_LEN + 4u];
    uint8_t rx[16];
    size_t n;

    body[0] = GX51_CMD_UPLOAD_CONFIG;
    body[1] = 0x01;
    body[2] = 0x01;
    memcpy(body + 3, cfg, GX51_CONFIG_LEN);
    body[GX51_CONFIG_LEN + 3u] = gx51_body_checksum(body, GX51_CONFIG_LEN + 3u);

    if (send_wait_ack(dev, body, sizeof(body), GX51_CMD_UPLOAD_CONFIG) < 0)
        return -1;
    if (read_reply(dev, GX51_CMD_UPLOAD_CONFIG, rx, sizeof(rx), &n) < 0)
        return -1;
    fprintf(stderr, "configure: config response status 0x%02x\n", rx[3]);
    return rx[3] == 0x01u ? 0 : -1;
}

int gx51_configure_ex(struct gx51_dev *dev, struct gx51_cal *cal_out)
{
    struct gx51_cal cal;
    uint8_t otp[OTP_LEN];
    uint8_t cfg[GX51_CONFIG_LEN];
    uint16_t chip_id = 0;

    if (dev == NULL)
        return errno = EINVAL, -1;

    if (soft_reset(dev) < 0) {
        fprintf(stderr, "configure: soft reset failed\n");
        return -1;
    }
    if (read_chip_id(dev, &chip_id) < 0) {
        fprintf(stderr, "configure: chip id read failed\n");
        return -1;
    }
    fprintf(stderr, "configure: chip id 0x%04x\n", chip_id);
    if (chip_id != 0x2504u)
        fprintf(stderr, "configure: warning chip id differs from 0x2504\n");

    if (read_otp(dev, otp) < 0) {
        fprintf(stderr, "configure: OTP read failed\n");
        return -1;
    }
    if (gx51_derive_cal(otp, &cal) < 0) {
        fprintf(stderr, "configure: OTP rejected by CRC/consistency checks\n");
        return -1;
    }
    fprintf(stderr, "configure: tcode=%u fdt_delta=%u dac=0x%03x/%02x/%02x/%02x\n",
            cal.tcode, cal.fdt_delta, cal.dac_main, cal.dac1, cal.dac2,
            cal.dac3);

    memcpy(cfg, gx51_base_config, sizeof(cfg));
    if (gx51_patch_config(cfg, &cal) < 0) {
        fprintf(stderr, "configure: config patch failed\n");
        return -1;
    }

    if (soft_reset(dev) < 0) {
        fprintf(stderr, "configure: second soft reset failed\n");
        return -1;
    }
    if (send_wait_ack(dev, idle_body, sizeof(idle_body), GX51_CMD_IDLE) < 0) {
        fprintf(stderr, "configure: idle failed\n");
        return -1;
    }
    /* The MCU needs a moment to settle in idle before it accepts register
     * writes; without this gap the first 0x80 is intermittently swallowed. */
    usleep(30000);
    if (write_reg(dev, 0x0220u, cal.dac_main) < 0 ||
        write_reg(dev, 0x0236u, cal.dac1) < 0 ||
        write_reg(dev, 0x0238u, cal.dac2) < 0 ||
        write_reg(dev, 0x023au, cal.dac3) < 0) {
        fprintf(stderr, "configure: DAC register write failed\n");
        return -1;
    }
    {
        static const uint16_t addr[4] = {0x0220u, 0x0236u, 0x0238u, 0x023au};
        uint16_t want[4] = {cal.dac_main, cal.dac1, cal.dac2, cal.dac3};
        size_t i;

        for (i = 0; i < 4u; i++) {
            uint16_t got = 0;

            if (read_reg(dev, addr[i], &got) < 0) {
                fprintf(stderr, "configure: reg 0x%04x readback failed\n",
                        addr[i]);
                continue;
            }
            fprintf(stderr,
                    "configure: reg 0x%04x readback 0x%04x (wrote 0x%04x) %s\n",
                    addr[i], got, want[i], got == want[i] ? "match" : "MISMATCH");
        }
    }
    usleep(10000);
    if (upload_config(dev, cfg) < 0) {
        fprintf(stderr, "configure: config upload rejected\n");
        return -1;
    }
    fprintf(stderr, "configure: target configured\n");
    if (cal_out != NULL)
        *cal_out = cal;
    return 0;
}

int gx51_configure(struct gx51_dev *dev)
{
    return gx51_configure_ex(dev, NULL);
}
