/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GX51_SESSION_H
#define GX51_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "gx51.h"
#include "image.h"
#include "tls.h"

/*
 * One capture session: transport plus TLS plus the command recipes.
 *
 * This is the sequence the libfprint driver runs. tools/gx51-probe keeps its
 * own copy because it interleaves diagnostics with every step; the shared
 * bottom layer (transport, protocol, target, pmk, tls, image) is the same
 * code in both.
 */
struct gx51_session {
    struct gx51_dev dev;
    struct gx51_tls *tls;
    struct gx51_cal cal;
    char version[64];
    int have_tls;
    int verbose;
};

/* Reset the MCU, open SPI/IRQ, replay DriverState:Install, read the firmware
 * string. Leaves the session closed on failure. */
int gx51_session_open(struct gx51_session *s, const char *spi_path);

/* Tear down TLS (if any), release SPI/IRQ/GPIO. Safe to call twice. */
void gx51_session_close(struct gx51_session *s);

/* Recover the factory PMK, reset out of the factory staging state, configure
 * the target, request TLS, run the handshake and confirm the tunnel with D4.
 * On success the sensor is armed and ready for the background recipe. */
int gx51_session_start_tls(struct gx51_session *s);

/* One FDT sample: touch flag, per-zone counts and their mean. */
int gx51_session_sample(struct gx51_session *s, uint8_t *touch, int *zones,
                        int *mean);

/* Decode one raw FDT report frame (tag 0x36 or 0x32) without touching the
 * sensor. Exposed so the parser can be tested against recorded frames. */
int gx51_session_fdt_decode(const uint8_t *frame, size_t len, uint8_t *touch,
                            int zones[6], int *zone_count, int *mean);

/* Poll FDT until at least `zones_min` zones read as touched, or timeout. */
int gx51_session_wait_touch(struct gx51_session *s, int timeout_ms,
                            int zones_min, int *mean);
/* Poll FDT until the sensor reports no contact and an idle-level mean. */
int gx51_session_wait_clear(struct gx51_session *s, int timeout_ms);

/* Build the background (ImageBase) reference. Must run once per session with
 * the sensor clear, before the first GET_IMAGE. */
int gx51_session_background(struct gx51_session *s);

/* Arm the capture path for a finger that is already down, ask for the image
 * and decode it into `px` (GX51_IMAGE_PIXELS samples). */
int gx51_session_capture(struct gx51_session *s, uint16_t *px);

/* Leave the sensor in a state where the next capture starts cleanly. */
void gx51_session_cleanup(struct gx51_session *s);

/* Wake the MCU (0f 00 00 0e). Required after the tunnel is confirmed. */
int gx51_session_wakeup(struct gx51_session *s);

#endif
