/* SPDX-License-Identifier: GPL-2.0-only */
/* Crypto primitives needed by the factory PMK path. No external dependency. */
#ifndef GX51_CRYPTO_H
#define GX51_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define GX51_AES_BLOCK 16u

void gx51_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

void gx51_aes128_expand(const uint8_t key[16], uint8_t rk[176]);
void gx51_aes128_decrypt_block(const uint8_t rk[176], const uint8_t in[16],
                               uint8_t out[16]);

/* len must be a non-zero multiple of 16. in and out may not overlap. */
int gx51_aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                            const uint8_t *in, uint8_t *out, size_t len);

#endif
