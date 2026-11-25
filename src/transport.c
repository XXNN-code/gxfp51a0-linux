/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "gx51.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define GX51_SYSFS "/sys/bus/spi/devices/spi-GXFP51A0:00"

static int fail(int error)
{
    errno = error;
    return -1;
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

static int spi_message(int fd, const void *tx, void *rx, uint32_t len)
{
    struct spi_ioc_transfer xfer = {
        .tx_buf = (uintptr_t)tx,
        .rx_buf = (uintptr_t)rx,
        .len = len,
        .speed_hz = GX51_SPI_SPEED_HZ,
        .bits_per_word = 8,
    };

    return ioctl(fd, SPI_IOC_MESSAGE(1), &xfer) >= 1 ? 0 : -1;
}

static int find_spidev(char *path, size_t cap)
{
    static const char dirpath[] = GX51_SYSFS "/spidev";
    struct dirent *entry;
    DIR *dir = opendir(dirpath);

    if (dir == NULL)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "spidev", 6) != 0)
            continue;
        if (snprintf(path, cap, "/dev/%s", entry->d_name) >= (int)cap) {
            closedir(dir);
            errno = ENAMETOOLONG;
            return -1;
        }
        closedir(dir);
        return access(path, R_OK | W_OK) == 0 ? 0 : -1;
    }

    closedir(dir);
    errno = ENODEV;
    return -1;
}

static int find_gpiochip(void)
{
    int n;

    for (n = 0; n < 16; n++) {
        struct gpiochip_info info = {0};
        char path[32];
        int fd;

        snprintf(path, sizeof(path), "/dev/gpiochip%d", n);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 &&
            strncmp(info.label, "INT34BB", 7) == 0 &&
            info.lines > GX51_RESET_LINE)
            return fd;

        close(fd);
    }

    errno = ENODEV;
    return -1;
}

int gx51_gpio_open(struct gx51_dev *dev)
{
    if (dev == NULL)
        return fail(EINVAL);

    dev->gpio_fd = find_gpiochip();
    return dev->gpio_fd < 0 ? -1 : 0;
}

int gx51_reset(struct gx51_dev *dev)
{
    struct gpio_v2_line_request req = {0};
    struct gpio_v2_line_values value = {.bits = 0, .mask = 1};
    int failed = 0;

    if (dev == NULL || dev->gpio_fd < 0)
        return fail(EBADF);

    req.num_lines = 1;
    req.offsets[0] = GX51_RESET_LINE;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 0;
    req.config.attrs[0].mask = 1;
    strncpy(req.consumer, "gx51-probe", sizeof(req.consumer) - 1);

    if (ioctl(dev->gpio_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0 || req.fd < 0)
        return -1;

    value.bits = 1;
    if (ioctl(req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &value) < 0)
        failed = 1;
    else if (sleep_us(300000) < 0)
        failed = 1;

    /* GPIO264 is active-HIGH reset. Always leave it LOW on every exit path. */
    value.bits = 0;
    if (ioctl(req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &value) < 0)
        failed = 1;
    else if (!failed && sleep_us(600000) < 0)
        failed = 1;

    if (failed)
        ioctl(req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &value);
    close(req.fd);

    return failed ? -1 : 0;
}

int gx51_spi_open(struct gx51_dev *dev, const char *path)
{
    uint8_t mode = SPI_MODE_0 | SPI_CS_HIGH;
    uint8_t bits = 8;
    uint32_t speed = GX51_SPI_SPEED_HZ;

    if (dev == NULL)
        return fail(EINVAL);

    /* Callers that reopen an existing device pass dev->spi_path back in. That
     * aliases the destination, and snprintf() with overlapping source and
     * destination is undefined (glibc produced an empty string, so open()
     * failed with ENOENT). Skip the copy when it is already the same buffer. */
    if (path != NULL && path != dev->spi_path) {
        if (snprintf(dev->spi_path, sizeof(dev->spi_path), "%s", path) >=
            (int)sizeof(dev->spi_path))
            return fail(ENAMETOOLONG);
    } else if (path == NULL &&
               find_spidev(dev->spi_path, sizeof(dev->spi_path)) < 0) {
        return -1;
    }

    dev->spi_fd = open(dev->spi_path, O_RDWR | O_CLOEXEC);
    if (dev->spi_fd < 0)
        return -1;

    if (ioctl(dev->spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(dev->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(dev->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        close(dev->spi_fd);
        dev->spi_fd = -1;
        return -1;
    }

    return 0;
}

int gx51_irq_open(struct gx51_dev *dev)
{
    struct gpio_v2_line_request req = {0};

    if (dev == NULL || dev->gpio_fd < 0)
        return fail(EBADF);

    req.num_lines = 1;
    req.offsets[0] = GX51_IRQ_LINE;
    req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
    strncpy(req.consumer, "gx51-irq", sizeof(req.consumer) - 1);

    if (ioctl(dev->gpio_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0 || req.fd < 0)
        return -1;

    dev->irq_fd = req.fd;
    return 0;
}

void gx51_close(struct gx51_dev *dev)
{
    if (dev == NULL)
        return;
    if (dev->irq_fd >= 0)
        close(dev->irq_fd);
    if (dev->spi_fd >= 0)
        close(dev->spi_fd);
    if (dev->gpio_fd >= 0)
        close(dev->gpio_fd);
    dev->irq_fd = -1;
    dev->spi_fd = -1;
    dev->gpio_fd = -1;
}

int gx51_wait_level(struct gx51_dev *dev, int high, unsigned int timeout_ms)
{
    unsigned int elapsed;

    if (dev->irq_fd < 0)
        return fail(EBADF);

    for (elapsed = 0; elapsed < timeout_ms; elapsed++) {
        struct gpio_v2_line_values value = {.bits = 0, .mask = 1};

        if (ioctl(dev->irq_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &value) < 0)
            return -1;
        if ((int)(value.bits & 1) == high)
            return 0;
        if (sleep_us(1000) < 0)
            return -1;
    }

    errno = ETIMEDOUT;
    return -1;
}

int gx51_wait_irq(struct gx51_dev *dev, unsigned int timeout_ms)
{
    return gx51_wait_level(dev, 1, timeout_ms);
}

int gx51_wait_idle(struct gx51_dev *dev, unsigned int timeout_ms)
{
    return gx51_wait_level(dev, 0, timeout_ms);
}

/*
 * Windows WakeupMCU: one raw four-byte SPI write, no protocol framing and no
 * ACK, followed by a 5 ms settle. The vendor driver issues this between
 * DriverState and the firmware read, and again once the capture context is
 * prepared - that second one is what takes the MCU out of the 0x70 idle state
 * left behind by configuration. Without it every command still ACKs, but the
 * analog front end stays off: FDT zones read zero and images come back as a
 * correctly sized buffer of zeros.
 */
int gx51_wakeup_mcu(struct gx51_dev *dev)
{
    static const uint8_t wake[4] = {0x0f, 0x00, 0x00, 0x0e};

    if (dev == NULL || dev->spi_fd < 0)
        return fail(EINVAL);
    if (spi_message(dev->spi_fd, wake, NULL, sizeof(wake)) < 0)
        return -1;
    return sleep_us(5000);
}

int gx51_write_frame(struct gx51_dev *dev, uint8_t type,
                     const uint8_t *body, size_t len)
{
    uint8_t header[4];
    uint8_t checksum;

    if (dev == NULL || dev->spi_fd < 0 || body == NULL ||
        len == 0 || len > 0xffffu)
        return fail(EINVAL);

    header[0] = type;
    header[1] = (uint8_t)(len & 0xffu);
    header[2] = (uint8_t)((len >> 8) & 0xffu);
    checksum = (uint8_t)(header[0] + header[1] + header[2]);
    header[3] = checksum;

    if (spi_message(dev->spi_fd, header, NULL, sizeof(header)) < 0)
        return -1;
    if (sleep_us(2000) < 0)
        return -1;
    return spi_message(dev->spi_fd, body, NULL, (uint32_t)len);
}

/*
 * Returns body length, or -1 I/O/timeout, -2 silent all-FF header,
 * -3 invalid header checksum, -4 invalid length or packet type.
 */
int gx51_read_frame_to(struct gx51_dev *dev, unsigned int irq_ms,
                       uint8_t *type, uint8_t *body, size_t cap,
                       size_t *len)
{
    uint8_t header[4];
    size_t body_len;
    size_t offset;

    if (dev == NULL || dev->spi_fd < 0 || dev->irq_fd < 0 ||
        type == NULL || body == NULL || len == NULL)
        return fail(EINVAL);

    if (gx51_wait_irq(dev, irq_ms) < 0)
        return -1;

    if (spi_message(dev->spi_fd, NULL, header, sizeof(header)) < 0)
        return -1;

    if (header[0] == 0xff && header[1] == 0xff &&
        header[2] == 0xff && header[3] == 0xff)
        return -2;
    if ((uint8_t)(header[0] + header[1] + header[2]) != header[3])
        return -3;
    if (header[0] != GX51_PKT_PLAIN && header[0] != GX51_PKT_TLS)
        return -4;

    body_len = (size_t)header[1] | ((size_t)header[2] << 8);
    if (body_len == 0 || body_len > cap)
        return -4;

    for (offset = 0; offset < body_len; ) {
        size_t chunk = body_len - offset;

        if (chunk > GX51_READ_CHUNK)
            chunk = GX51_READ_CHUNK;
        if (spi_message(dev->spi_fd, NULL, body + offset,
                        (uint32_t)chunk) < 0)
            return -1;
        offset += chunk;
    }

    *type = header[0];
    *len = body_len;
    return (int)body_len;
}

int gx51_read_frame(struct gx51_dev *dev, uint8_t *type,
                    uint8_t *body, size_t cap, size_t *len)
{
    return gx51_read_frame_to(dev, 1200u, type, body, cap, len);
}
