/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GX51_TLS_H
#define GX51_TLS_H

#include <sys/types.h>

#include "gx51.h"

struct gx51_tls;

/*
 * Host is the TLS server, the sensor is the client. The only suite the sensor
 * offers is PSK-AES128-CBC-SHA256 (0x00ae) on TLS 1.2, keyed by the factory
 * PMK. 0x00ae is CBC, not GCM - the GCM suite is 0x00a8 and is not offered.
 */
struct gx51_tls *gx51_tls_start(struct gx51_dev *dev,
                                const uint8_t pmk[GX51_PMK_LEN],
                                unsigned int timeout_ms);
ssize_t gx51_tls_read(struct gx51_tls *tls, uint8_t *buf, size_t len);
ssize_t gx51_tls_write(struct gx51_tls *tls, const uint8_t *buf, size_t len);
const char *gx51_tls_error(const struct gx51_tls *tls);
int gx51_tls_timed_out(const struct gx51_tls *tls);
void gx51_tls_set_timeout(struct gx51_tls *tls, unsigned int timeout_ms);
void gx51_tls_stop(struct gx51_tls *tls);

#endif
