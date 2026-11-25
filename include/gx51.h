/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GX51_H
#define GX51_H

#include <stddef.h>
#include <stdint.h>

#define GX51_SPI_SPEED_HZ 1000000u
#define GX51_RESET_LINE 264u
#define GX51_IRQ_LINE 48u
#define GX51_MAX_BODY 4096u
#define GX51_MEM_READ_MAX 256u
#define GX51_PMK_LEN 48u
#define GX51_READ_CHUNK 4096u

#define GX51_PKT_PLAIN 0xa0u
#define GX51_PKT_TLS 0xb0u
#define GX51_CMD_ACK 0xb0u
#define GX51_CMD_FW_VERSION 0xa8u

#define GX51_CMD_SOFT_RESET 0xa2u
#define GX51_CMD_CHIP_ID 0x82u
#define GX51_CMD_OTP 0xa6u
#define GX51_CMD_IDLE 0x70u
#define GX51_CMD_REG_WRITE 0x80u
#define GX51_CMD_UPLOAD_CONFIG 0x90u

#define GX51_CONFIG_LEN 256u

struct gx51_dev {
    int gpio_fd;
    int irq_fd;
    int spi_fd;
    char spi_path[64];
};

int gx51_gpio_open(struct gx51_dev *dev);
int gx51_reset(struct gx51_dev *dev);
int gx51_spi_open(struct gx51_dev *dev, const char *path);
int gx51_irq_open(struct gx51_dev *dev);
int gx51_wait_irq(struct gx51_dev *dev, unsigned int timeout_ms);
int gx51_wait_idle(struct gx51_dev *dev, unsigned int timeout_ms);
int gx51_wakeup_mcu(struct gx51_dev *dev);
void gx51_close(struct gx51_dev *dev);

int gx51_write_frame(struct gx51_dev *dev, uint8_t type,
                     const uint8_t *body, size_t len);
int gx51_read_frame(struct gx51_dev *dev, uint8_t *type,
                    uint8_t *body, size_t cap, size_t *len);
int gx51_read_frame_to(struct gx51_dev *dev, unsigned int irq_ms,
                       uint8_t *type, uint8_t *body, size_t cap, size_t *len);

size_t gx51_build_body(uint8_t *out, size_t cap, uint8_t command,
                       const uint8_t *payload, size_t payload_len);
uint8_t gx51_body_checksum(const uint8_t *body, size_t len);
int gx51_parse_ack(const uint8_t *body, size_t len, uint8_t command,
                   uint8_t *status);
int gx51_parse_version(const uint8_t *body, size_t len,
                       char *version, size_t cap);
int gx51_valid_body(const uint8_t *body, size_t len);

int gx51_mem_read(struct gx51_dev *dev, uint32_t addr, uint32_t len,
                  uint8_t *out, size_t cap, size_t *out_len);
int gx51_mem_payload(const uint8_t *frame, size_t len,
                     const uint8_t **data, size_t *data_len);
int gx51_pmk_from_record(const uint8_t *rec, size_t len,
                         uint8_t pmk[GX51_PMK_LEN]);
int gx51_fetch_pmk(struct gx51_dev *dev, uint8_t pmk[GX51_PMK_LEN]);

extern const uint8_t gx51_base_config[GX51_CONFIG_LEN];

struct gx51_cal {
    uint16_t tcode;
    uint8_t fdt_delta;
    uint16_t dac_main;
    uint8_t dac1;
    uint8_t dac2;
    uint8_t dac3;
};

int gx51_derive_cal(const uint8_t otp[64], struct gx51_cal *cal);
int gx51_patch_config(uint8_t cfg[GX51_CONFIG_LEN],
                      const struct gx51_cal *cal);
int gx51_configure(struct gx51_dev *dev);
/* Same as gx51_configure(), but keeps the OTP-derived calibration so callers
 * can replay the DAC writes inside a capture recipe. */
int gx51_configure_ex(struct gx51_dev *dev, struct gx51_cal *cal_out);

#endif
