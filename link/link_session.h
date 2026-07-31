/*
 * frank-genesis — C2 inter-processor sound link
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * link_session.h — doorbell-sequenced control and bulk exchanges over
 * link_bus.
 *
 * Adapted from frank_core2/firmware/common/link_session.c. That firmware
 * measured the wire, so its bulk phases stream a ring buffer forever and
 * never care where a block lands. The emulator moves real payloads of
 * exact sizes — a ROM chunk, a frame's events, 888 samples — so the bulk
 * calls here are linear: one buffer, one length, delivered exactly once.
 *
 * Both doorbells return to low after every exchange, so a desynchronised
 * pair recovers by simply timing out and starting the next one.
 */
#ifndef LINK_SESSION_H
#define LINK_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_bus.h"
#include "link_proto.h"

/* Control frames are small and fixed; a stalled peer should be noticed
 * in well under a video frame during steady state, but boot-time probes
 * want more patience. Callers override via handshake_timeout_us. */
#define LINK_CTRL_TIMEOUT_US       100000u
#define LINK_HANDSHAKE_TIMEOUT_US  2000000u

typedef struct {
    link_t   *link;
    uint8_t  *ctrl_tx;    /* LINK_CTRL_BYTES, 4-byte aligned */
    uint8_t  *ctrl_rx;    /* LINK_CTRL_BYTES, 4-byte aligned */
    uint32_t  seq;

    /* Doorbell patience in microseconds; 0 selects the default. The
     * steady-state frame exchange drops this to a couple of
     * milliseconds so a slave that has died costs one frame of audio
     * rather than stalling the emulator. */
    uint32_t  handshake_timeout_us;
} link_session_t;

/* ---------------- Master side ---------------- */

bool link_m_send_ctrl(link_session_t *s, uint16_t op,
                      uint32_t arg0, uint32_t arg1,
                      const void *payload, uint32_t payload_len);

/* Receive one control frame into s->ctrl_rx. Validates magic and CRC. */
bool link_m_recv_ctrl(link_session_t *s);

/* Linear bulk transfers. `bytes` must be a multiple of 4 and the buffer
 * 4-byte aligned: the PIO FIFOs are 32 bits wide and autopull/autopush
 * are word-sized, so a partial word would desynchronise framing. */
bool link_m_bulk_send(link_session_t *s, const void *buf, size_t bytes);
bool link_m_bulk_recv(link_session_t *s, void *buf, size_t bytes);

/* ---------------- Slave side ---------------- */

/* Wait for the master's doorbell and receive one control frame. Returns
 * the opcode, or 0 on timeout / bad frame. */
uint16_t link_s_wait_ctrl(link_session_t *s, uint32_t timeout_us);

bool link_s_send_ctrl(link_session_t *s, uint16_t op,
                      uint32_t arg0, uint32_t arg1,
                      const void *payload, uint32_t payload_len);

bool link_s_bulk_recv(link_session_t *s, void *buf, size_t bytes);
bool link_s_bulk_send(link_session_t *s, const void *buf, size_t bytes);

/* Header of the frame most recently received into s->ctrl_rx. */
static inline const link_hdr_t *link_rx_hdr(const link_session_t *s) {
    return (const link_hdr_t *)s->ctrl_rx;
}

#endif /* LINK_SESSION_H */
