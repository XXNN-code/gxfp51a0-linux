/* SPDX-License-Identifier: GPL-2.0-only */
#include "gx51.h"

#include <string.h>

uint8_t gx51_body_checksum(const uint8_t *body, size_t len)
{
    uint8_t sum = 0;
    size_t i;

    for (i = 0; i < len; i++)
        sum = (uint8_t)(sum + body[i]);
    return (uint8_t)(0xaau - sum);
}

size_t gx51_build_body(uint8_t *out, size_t cap, uint8_t command,
                       const uint8_t *payload, size_t payload_len)
{
    size_t body_len = payload_len + 4u;
    uint16_t wire_len = (uint16_t)(payload_len + 1u);

    if (out == NULL || (payload == NULL && payload_len != 0) ||
        body_len > cap || wire_len > 0xffu)
        return 0;

    out[0] = command;
    out[1] = (uint8_t)(wire_len & 0xffu);
    out[2] = (uint8_t)(wire_len >> 8);
    if (payload_len != 0)
        memcpy(out + 3, payload, payload_len);
    out[body_len - 1] = gx51_body_checksum(out, body_len - 1);
    return body_len;
}

int gx51_valid_body(const uint8_t *body, size_t len)
{
    uint16_t wire_len;

    if (body == NULL || len < 4)
        return 0;
    wire_len = (uint16_t)body[1] | ((uint16_t)body[2] << 8);
    return (size_t)wire_len + 3u == len &&
           body[len - 1] == gx51_body_checksum(body, len - 1);
}

int gx51_parse_ack(const uint8_t *body, size_t len, uint8_t command,
                   uint8_t *status)
{
    if (status == NULL || len != 6 || body[0] != GX51_CMD_ACK ||
        body[3] != command || !gx51_valid_body(body, len))
        return -1;

    *status = body[4];
    return (body[4] & 1u) != 0 ? 0 : -1;
}

int gx51_parse_version(const uint8_t *body, size_t len,
                       char *version, size_t cap)
{
    size_t payload_len;
    size_t n = 0;

    if (version == NULL || cap == 0 || body == NULL ||
        body[0] != GX51_CMD_FW_VERSION || len < 5 ||
        !gx51_valid_body(body, len))
        return -1;

    payload_len = len - 4u;
    while (n < payload_len && n + 1 < cap &&
           body[3 + n] >= 0x20u && body[3 + n] <= 0x7eu) {
        version[n] = (char)body[3 + n];
        n++;
    }

    if (n == 0)
        return -1;
    while (n > 0 && version[n - 1] == ' ')
        n--;
    version[n] = '\0';
    return (int)n;
}
