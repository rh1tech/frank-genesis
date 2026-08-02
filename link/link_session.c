/*
 * frank-genesis — C2 inter-processor sound link
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "link_session.h"

uint32_t link_hs_arm_us, link_hs_xfer_us, link_hs_drop_us, link_hs_n;

#include "pico/stdlib.h"
#include "pico/time.h"

#include <string.h>

static inline uint32_t hs_timeout(const link_session_t *s) {
    return s->handshake_timeout_us ? s->handshake_timeout_us
                                   : LINK_HANDSHAKE_TIMEOUT_US;
}

/* Time budget for a bulk phase, derived from the current wire rate with
 * a 4x cushion, so a slow divider or a stalled peer fails cleanly
 * instead of hanging the emulator. */
static uint32_t bulk_timeout_us(const link_t *l, size_t bytes) {
    uint32_t rate = link_byte_rate(l);
    if (!rate) return LINK_HANDSHAKE_TIMEOUT_US;

    uint64_t us = ((uint64_t)bytes * 1000000ull) / rate;
    us = us * 4 + 10000ull;
    if (us > 10000000ull) us = 10000000ull;
    return (uint32_t)us;
}

/* ================================================================== */
/* Master                                                             */
/* ================================================================== */

bool link_m_send_ctrl(link_session_t *s, uint16_t op,
                      uint32_t arg0, uint32_t arg1,
                      const void *payload, uint32_t payload_len) {
    link_frame_build(s->ctrl_tx, op, ++s->seq, arg0, arg1, payload, payload_len);

    /* "Master ready to send." */
    uint64_t h0 = time_us_64();
    link_db_set(s->link, true);
    if (!link_db_wait(s->link, true, hs_timeout(s))) {
        link_db_set(s->link, false);
        return false;
    }
    uint64_t h1 = time_us_64();

    link_use_ctrl_rate(s->link);
    link_tx_start(s->link, s->ctrl_tx, LINK_CTRL_BYTES);
    bool ok = link_tx_finish(s->link, LINK_CTRL_TIMEOUT_US);
    uint64_t h2 = time_us_64();

    link_db_set(s->link, false);
    if (!link_db_wait(s->link, false, hs_timeout(s))) return false;
    uint64_t h3 = time_us_64();

    /* Where a control send actually goes: waiting for the peer to say it
     * is armed, our own transfer, then waiting for it to drop. */
    link_hs_arm_us  += (uint32_t)(h1 - h0);
    link_hs_xfer_us += (uint32_t)(h2 - h1);
    link_hs_drop_us += (uint32_t)(h3 - h2);
    link_hs_n++;

    return ok;
}

bool link_m_recv_ctrl(link_session_t *s) {
    /* Arm before saying "go" so the receive shift counter is aligned
     * with the first word the slave pushes. */
    link_rx_arm(s->link, s->ctrl_rx, LINK_CTRL_BYTES);

    link_db_set(s->link, true);
    if (!link_db_wait(s->link, true, hs_timeout(s))) {
        link_db_set(s->link, false);
        link_rx_abort(s->link);
        return false;
    }

    bool ok = (link_rx_wait(s->link, LINK_CTRL_TIMEOUT_US) == 0);

    link_db_set(s->link, false);
    link_db_wait(s->link, false, hs_timeout(s));

    return ok && link_frame_check(s->ctrl_rx);
}

bool link_m_bulk_send(link_session_t *s, const void *buf, size_t bytes) {
    uint32_t tmo = bulk_timeout_us(s->link, bytes);

    /* The slave arms its receiver, then raises DB_SM. */
    link_db_set(s->link, true);
    if (!link_db_wait(s->link, true, hs_timeout(s))) {
        link_db_set(s->link, false);
        return false;
    }

    link_use_bulk_rate(s->link);
    link_tx_start(s->link, buf, bytes);
    bool ok = link_tx_finish(s->link, tmo);

    link_db_set(s->link, false);
    link_db_wait(s->link, false, tmo);

    return ok;
}

bool link_m_bulk_recv(link_session_t *s, void *buf, size_t bytes) {
    uint32_t tmo = bulk_timeout_us(s->link, bytes);

    link_rx_arm(s->link, buf, bytes);

    link_db_set(s->link, true);
    bool ok = (link_rx_wait(s->link, tmo) == 0);

    /* The slave raises DB_SM once its transmit has drained. */
    if (ok) ok = link_db_wait(s->link, true, hs_timeout(s));

    link_db_set(s->link, false);
    link_db_wait(s->link, false, hs_timeout(s));

    if (!ok) link_rx_abort(s->link);
    return ok;
}

/* ================================================================== */
/* Slave                                                              */
/* ================================================================== */

uint16_t link_s_wait_ctrl(link_session_t *s, uint32_t timeout_us) {
    if (!link_db_wait(s->link, true, timeout_us)) return 0;

    link_rx_arm(s->link, s->ctrl_rx, LINK_CTRL_BYTES);

    link_db_set(s->link, true);                  /* "armed" */
    bool ok = (link_rx_wait(s->link, LINK_CTRL_TIMEOUT_US) == 0);

    /* Master drops DB_MS once its frame has drained. */
    link_db_wait(s->link, false, hs_timeout(s));
    link_db_set(s->link, false);

    if (!ok || !link_frame_check(s->ctrl_rx)) {
        link_rx_abort(s->link);
        return 0;
    }
    return link_rx_hdr(s)->op;
}

bool link_s_send_ctrl(link_session_t *s, uint16_t op,
                      uint32_t arg0, uint32_t arg1,
                      const void *payload, uint32_t payload_len) {
    link_frame_build(s->ctrl_tx, op, ++s->seq, arg0, arg1, payload, payload_len);

    if (!link_db_wait(s->link, true, hs_timeout(s))) return false;

    link_use_ctrl_rate(s->link);
    link_tx_start(s->link, s->ctrl_tx, LINK_CTRL_BYTES);
    bool ok = link_tx_finish(s->link, LINK_CTRL_TIMEOUT_US);

    link_db_set(s->link, true);                  /* "sent" */
    link_db_wait(s->link, false, hs_timeout(s));
    link_db_set(s->link, false);

    return ok;
}

bool link_s_bulk_recv(link_session_t *s, void *buf, size_t bytes) {
    uint32_t tmo = bulk_timeout_us(s->link, bytes);

    link_rx_arm(s->link, buf, bytes);

    if (!link_db_wait(s->link, true, hs_timeout(s))) {
        link_rx_abort(s->link);
        return false;
    }
    link_db_set(s->link, true);                  /* "armed, send" */

    bool ok = (link_rx_wait(s->link, tmo) == 0);

    link_db_wait(s->link, false, tmo);
    link_db_set(s->link, false);

    if (!ok) link_rx_abort(s->link);
    return ok;
}

bool link_s_bulk_send(link_session_t *s, const void *buf, size_t bytes) {
    uint32_t tmo = bulk_timeout_us(s->link, bytes);

    /* The master arms its receiver and then raises DB_MS. */
    if (!link_db_wait(s->link, true, hs_timeout(s))) return false;

    link_use_bulk_rate(s->link);
    link_tx_start(s->link, buf, bytes);
    bool ok = link_tx_finish(s->link, tmo);

    link_db_set(s->link, true);                  /* "sent" */
    link_db_wait(s->link, false, hs_timeout(s));
    link_db_set(s->link, false);

    return ok;
}
