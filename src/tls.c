/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * TLS-PSK session with the sensor.
 *
 * Measured on GF_ST411SEC_APP_14108: the sensor opens TLS itself. It announces
 * exactly one suite (0x00ae = PSK-AES128-CBC-SHA256) plus the renegotiation
 * SCSV, offers no extensions at all, and sends no PSK identity hint - it puts
 * its identity in ClientKeyExchange instead. The host therefore has to be the
 * TLS server, keyed by the factory PMK.
 *
 * Note the suite is AES-128-CBC-SHA256, not GCM: 0x00ae is CBC, the GCM one is
 * 0x00a8. Offering GCM here fails the handshake with "no shared cipher".
 *
 * Records travel inside ordinary sensor frames of type 0xb0: one frame per
 * TLS record, body is the record verbatim. A plain 6-byte ACK can appear
 * interleaved and is skipped.
 */
#define _GNU_SOURCE
#include "gx51.h"
#include "tls.h"

#include <errno.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GX51_TLS_FRAME_MAX 0x10000u

struct gx51_tls {
    struct gx51_dev *dev;
    SSL_CTX *ctx;
    SSL *ssl;
    uint8_t pmk[GX51_PMK_LEN];
    uint8_t *rx;
    size_t rx_len;      /* bytes held in rx from the current frame */
    size_t rx_pos;      /* consumed bytes of that frame */
    unsigned int timeout_ms;
    size_t frames_rx;
    size_t frames_tx;
    int timed_out;
    char err[160];
};

static BIO_METHOD *bio_method;

static void tls_fail(struct gx51_tls *tls, const char *what)
{
    unsigned long code = ERR_peek_last_error();
    char detail[96] = "";

    if (code != 0ul)
        ERR_error_string_n(code, detail, sizeof(detail));
    snprintf(tls->err, sizeof(tls->err), "%s%s%s", what,
             detail[0] != '\0' ? ": " : "", detail);
    ERR_clear_error();
}

static int tls_bio_write(BIO *bio, const char *in, int len)
{
    struct gx51_tls *tls = BIO_get_data(bio);

    BIO_clear_retry_flags(bio);
    if (tls == NULL || in == NULL || len <= 0 || (size_t)len > 0xffffu)
        return -1;
    if (gx51_wait_idle(tls->dev, 200) < 0) {
        snprintf(tls->err, sizeof(tls->err), "line busy before TLS write");
        return -1;
    }
    if (gx51_write_frame(tls->dev, GX51_PKT_TLS, (const uint8_t *)in,
                         (size_t)len) < 0) {
        snprintf(tls->err, sizeof(tls->err), "TLS write failed: %s",
                 strerror(errno));
        return -1;
    }
    tls->frames_tx++;
    fprintf(stderr, "tls: tx frame %zu bytes (record 0x%02x)\n",
            (size_t)len, (uint8_t)in[0]);
    return len;
}

/*
 * Pull one TLS-bearing frame into rx. OpenSSL reads a record in at least two
 * steps (five-byte header, then the body), so the BIO has to behave like a
 * stream and never hand back more bytes than the caller asked for.
 */
static int tls_fill(struct gx51_tls *tls)
{
    int i;

    tls->timed_out = 0;
    for (i = 0; i < 8; i++) {
        uint8_t type = 0;
        size_t n = 0;
        int ret = gx51_read_frame_to(tls->dev, tls->timeout_ms, &type,
                                     tls->rx, GX51_TLS_FRAME_MAX, &n);

        if (ret == -1) {
            tls->timed_out = 1;
            snprintf(tls->err, sizeof(tls->err),
                     "no TLS record within %u ms (%zu frames read)",
                     tls->timeout_ms, tls->frames_rx);
            return -1;
        }
        if (ret < 0) {
            fprintf(stderr, "tls: rx frame rejected by header check (%d)\n",
                    ret);
            continue;
        }

        tls->frames_rx++;
        fprintf(stderr, "tls: rx frame type 0x%02x len %zu (record 0x%02x)\n",
                type, n, n > 0 ? tls->rx[0] : 0);
        if (type != GX51_PKT_TLS)
            continue;
        /* Type 0xb0 is both "TLS" and the ACK opcode; ACKs are six bytes. */
        if (n == 6u && tls->rx[0] == GX51_CMD_ACK)
            continue;

        tls->rx_len = n;
        tls->rx_pos = 0;
        return 0;
    }

    snprintf(tls->err, sizeof(tls->err), "no usable TLS record");
    return -1;
}

static int tls_bio_read(BIO *bio, char *out, int len)
{
    struct gx51_tls *tls = BIO_get_data(bio);
    size_t avail;
    size_t take;

    BIO_clear_retry_flags(bio);
    if (tls == NULL || out == NULL || len <= 0)
        return -1;

    if (tls->rx_pos >= tls->rx_len) {
        if (tls_fill(tls) < 0) {
            /*
             * A missing record is not a broken session: mark the BIO retryable
             * so SSL_read reports WANT_READ and the session stays usable for a
             * later attempt.
             */
            if (tls->timed_out)
                BIO_set_retry_read(bio);
            return -1;
        }
    }

    avail = tls->rx_len - tls->rx_pos;
    take = (size_t)len < avail ? (size_t)len : avail;
    memcpy(out, tls->rx + tls->rx_pos, take);
    tls->rx_pos += take;
    return (int)take;
}

static long tls_bio_ctrl(BIO *bio, int cmd, long arg1, void *arg2)
{
    (void)bio;
    (void)arg1;
    (void)arg2;

    return cmd == BIO_CTRL_FLUSH ? 1L : 0L;
}

static int tls_bio_new(BIO *bio)
{
    BIO_set_init(bio, 0);
    BIO_set_data(bio, NULL);
    return 1;
}

static int tls_bio_free(BIO *bio)
{
    if (bio == NULL)
        return 0;
    BIO_set_data(bio, NULL);
    BIO_set_init(bio, 0);
    return 1;
}

static int bio_method_init(void)
{
    if (bio_method != NULL)
        return 0;

    bio_method = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK,
                              "gx51-spi");
    if (bio_method == NULL)
        return -1;
    BIO_meth_set_write(bio_method, tls_bio_write);
    BIO_meth_set_read(bio_method, tls_bio_read);
    BIO_meth_set_ctrl(bio_method, tls_bio_ctrl);
    BIO_meth_set_create(bio_method, tls_bio_new);
    BIO_meth_set_destroy(bio_method, tls_bio_free);
    return 0;
}

static unsigned int tls_psk_server_cb(SSL *ssl, const char *identity,
                                      unsigned char *psk,
                                      unsigned int max_psk_len)
{
    struct gx51_tls *tls = SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
    size_t len = GX51_PMK_LEN;

    fprintf(stderr, "tls: client identity \"%s\"\n",
            identity != NULL ? identity : "(none)");
    if (tls == NULL || psk == NULL)
        return 0;
    if (len > max_psk_len)
        len = max_psk_len;
    memcpy(psk, tls->pmk, len);
    return (unsigned int)len;
}

/*
 * The MCU only arms the capture path once the host acknowledges the finished
 * handshake. This is a cleartext command, not a TLS record: a bare 0xd4 with
 * a 0x01 status ACK back. Without it the tunnel still works for small
 * diagnostic frames but FDT zones stay zero and every image is all zeros.
 */
static int tls_confirm_up(struct gx51_tls *tls)
{
    static const uint8_t d4[] = {0xd4, 0x03, 0x00, 0x00, 0x00, 0xd3};
    uint8_t rx[64];
    uint8_t type;
    uint8_t status = 0;
    size_t n;
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        if (gx51_wait_idle(tls->dev, 200) < 0) {
            usleep(20000);
            continue;
        }
        if (gx51_write_frame(tls->dev, GX51_PKT_PLAIN, d4, sizeof(d4)) < 0) {
            snprintf(tls->err, sizeof(tls->err), "D4 write failed: %s",
                     strerror(errno));
            return -1;
        }
        if (gx51_wait_irq(tls->dev, 300) < 0) {
            fprintf(stderr, "tls: D4 no ACK, replaying\n");
            continue;
        }
        if (gx51_read_frame(tls->dev, &type, rx, sizeof(rx), &n) <= 0)
            continue;
        if (type != GX51_PKT_PLAIN)
            continue;
        if (gx51_parse_ack(rx, n, 0xd4u, &status) < 0) {
            fprintf(stderr, "tls: stale frame during D4: %02x %02x %02x\n",
                    n > 0 ? rx[0] : 0, n > 1 ? rx[1] : 0,
                    n > 3 ? rx[3] : 0);
            continue;
        }
        fprintf(stderr, "tls: MCU confirmed tunnel (D4 ack 0x%02x)\n", status);
        return 0;
    }

    snprintf(tls->err, sizeof(tls->err), "D4 confirmation got no ACK");
    return -1;
}

struct gx51_tls *gx51_tls_start(struct gx51_dev *dev,
                                const uint8_t pmk[GX51_PMK_LEN],
                                unsigned int timeout_ms)
{
    struct gx51_tls *tls;
    BIO *bio;
    int ret;

    if (dev == NULL || pmk == NULL || bio_method_init() < 0)
        return NULL;

    tls = calloc(1, sizeof(*tls));
    if (tls == NULL)
        return NULL;

    tls->dev = dev;
    tls->timeout_ms = timeout_ms != 0u ? timeout_ms : 5000u;
    memcpy(tls->pmk, pmk, sizeof(tls->pmk));
    tls->rx = malloc(GX51_TLS_FRAME_MAX);
    if (tls->rx == NULL)
        goto fail;

    tls->ctx = SSL_CTX_new(TLS_server_method());
    if (tls->ctx == NULL) {
        tls_fail(tls, "SSL_CTX_new");
        goto fail;
    }
    if (SSL_CTX_set_min_proto_version(tls->ctx, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(tls->ctx, TLS1_2_VERSION) != 1) {
        tls_fail(tls, "cannot pin TLS 1.2");
        goto fail;
    }
    /* Level 0: PSK-only suites have no certificate chain to size-check. */
    SSL_CTX_set_security_level(tls->ctx, 0);
    if (SSL_CTX_set_cipher_list(tls->ctx,
                                "PSK-AES128-CBC-SHA256:"
                                "PSK-AES128-GCM-SHA256") != 1) {
        tls_fail(tls, "no PSK-AES128 suite available");
        goto fail;
    }
    SSL_CTX_set_psk_server_callback(tls->ctx, tls_psk_server_cb);
    SSL_CTX_set_app_data(tls->ctx, tls);

    tls->ssl = SSL_new(tls->ctx);
    if (tls->ssl == NULL) {
        tls_fail(tls, "SSL_new");
        goto fail;
    }
    SSL_set_app_data(tls->ssl, tls);

    bio = BIO_new(bio_method);
    if (bio == NULL) {
        tls_fail(tls, "BIO_new");
        goto fail;
    }
    BIO_set_data(bio, tls);
    BIO_set_init(bio, 1);
    SSL_set_bio(tls->ssl, bio, bio);   /* SSL owns bio from here */

    ret = SSL_accept(tls->ssl);
    if (ret != 1) {
        int err = SSL_get_error(tls->ssl, ret);
        char msg[96];

        /* Keep the transport-level cause; the SSL layer only reports SYSCALL. */
        if (tls->err[0] != '\0')
            fprintf(stderr, "tls: transport: %s\n", tls->err);
        snprintf(msg, sizeof(msg),
                 "handshake failed (ssl error %d, %zu frames in, %zu out)",
                 err, tls->frames_rx, tls->frames_tx);
        tls_fail(tls, msg);
        goto fail;
    }

    fprintf(stderr, "tls: handshake ok: %s %s\n", SSL_get_version(tls->ssl),
            SSL_get_cipher_name(tls->ssl));

    /*
     * Windows and the reference driver both close the handshake by telling the
     * MCU, in the clear, that the tunnel is up: a bare 0xd4 that must be ACKed.
     * Skip it and the MCU keeps the capture path disabled - every command still
     * ACKs, FDT zones read back as zero and the image record is a correctly
     * sized buffer full of zeros.
     */
    if (tls_confirm_up(tls) < 0) {
        fprintf(stderr, "tls: D4 confirmation FAILED - capture will return "
                        "blank frames\n");
    }
    return tls;

fail:
    fprintf(stderr, "tls: %s\n", tls->err);
    gx51_tls_stop(tls);
    return NULL;
}

ssize_t gx51_tls_read(struct gx51_tls *tls, uint8_t *buf, size_t len)
{
    int ret;

    if (tls == NULL || buf == NULL || len == 0u || len > 0x7fffffffu)
        return -1;

    ret = SSL_read(tls->ssl, buf, (int)len);
    if (ret <= 0) {
        int err = SSL_get_error(tls->ssl, ret);

        if (err == SSL_ERROR_WANT_READ && tls->timed_out) {
            snprintf(tls->err, sizeof(tls->err),
                     "no TLS record within %u ms", tls->timeout_ms);
            return -1;
        }
        {
            char msg[96];

            snprintf(msg, sizeof(msg), "SSL_read failed (ssl error %d)", err);
            tls_fail(tls, msg);
        }
        return -1;
    }
    return ret;
}

ssize_t gx51_tls_write(struct gx51_tls *tls, const uint8_t *buf, size_t len)
{
    int ret;

    if (tls == NULL || buf == NULL || len == 0u || len > 0x7fffffffu)
        return -1;

    ret = SSL_write(tls->ssl, buf, (int)len);
    if (ret <= 0) {
        int err = SSL_get_error(tls->ssl, ret);
        char msg[96];

        snprintf(msg, sizeof(msg), "SSL_write failed (ssl error %d)", err);
        tls_fail(tls, msg);
        return -1;
    }
    return ret;
}

const char *gx51_tls_error(const struct gx51_tls *tls)
{
    return tls == NULL ? "no session" : tls->err;
}

/*
 * A timeout is normal at the end of a data stream: the sensor simply has no
 * more records to send. Callers reading a known-length payload use this to
 * tell "stream finished" apart from a real transport fault.
 */
int gx51_tls_timed_out(const struct gx51_tls *tls)
{
    return tls != NULL && tls->timed_out;
}

void gx51_tls_set_timeout(struct gx51_tls *tls, unsigned int timeout_ms)
{
    if (tls != NULL && timeout_ms != 0u)
        tls->timeout_ms = timeout_ms;
}

void gx51_tls_stop(struct gx51_tls *tls)
{
    if (tls == NULL)
        return;
    if (tls->ssl != NULL)
        SSL_free(tls->ssl);         /* frees the BIO as well */
    if (tls->ctx != NULL)
        SSL_CTX_free(tls->ctx);
    free(tls->rx);
    memset(tls->pmk, 0, sizeof(tls->pmk));
    free(tls);
}
