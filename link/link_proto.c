/*
 * frank-genesis — C2 inter-processor sound link
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * link_proto.c — control-frame helpers shared by both halves.
 *
 * Ported from frank_core2/firmware/common/link_proto.c; the bulk
 * pattern generator and verifier that firmware used to characterise the
 * wire are dropped, since the emulator sends real data rather than test
 * patterns.
 */

#include "link_proto.h"

#include <string.h>

/* Bitwise CRC-32 (IEEE, reflected). It only ever runs over 128-byte
 * control frames and, once per ROM load, over the ROM image, so a table
 * would buy nothing worth the 1 KiB of RAM. */
uint32_t link_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void link_frame_build(void *frame, uint16_t op, uint32_t seq,
                      uint32_t arg0, uint32_t arg1,
                      const void *payload, uint32_t payload_len) {
    uint8_t *f = (uint8_t *)frame;

    memset(f, 0, LINK_CTRL_BYTES);

    link_hdr_t *h = (link_hdr_t *)f;
    h->magic     = LINK_MAGIC;
    h->op        = op;
    h->proto_ver = LINK_PROTO_VER;
    h->seq       = seq;
    h->arg0      = arg0;
    h->arg1      = arg1;
    h->crc       = 0;

    if (payload && payload_len) {
        if (payload_len > LINK_PAYLOAD_BYTES) payload_len = LINK_PAYLOAD_BYTES;
        memcpy(f + sizeof(link_hdr_t), payload, payload_len);
    }

    h->crc = link_crc32(f, LINK_CTRL_BYTES);
}

int link_frame_check(const void *frame) {
    const uint8_t *f = (const uint8_t *)frame;
    const link_hdr_t *h = (const link_hdr_t *)f;

    if (h->magic != LINK_MAGIC)         return 0;
    if (h->proto_ver != LINK_PROTO_VER) return 0;

    /* Recompute over a copy with the crc field zeroed. The caller's
     * buffer is the live DMA landing area, so we must not mutate it;
     * 128 bytes of stack is cheaper than a resumable CRC. */
    uint32_t want = h->crc;

    uint8_t scratch[LINK_CTRL_BYTES];
    memcpy(scratch, f, LINK_CTRL_BYTES);
    ((link_hdr_t *)scratch)->crc = 0;

    return link_crc32(scratch, LINK_CTRL_BYTES) == want;
}
