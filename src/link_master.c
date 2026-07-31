/*
 * frank-genesis — C2 master side of the inter-processor sound link
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "link_master.h"

#include "link_bus.h"
#include "link_pins.h"
#include "link_session.h"

#include "pico/stdlib.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>

#if ENABLE_LOGGING
#define LOG(...) printf(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

/* Control frames land in DMA, so they must be word aligned. */
static uint8_t __attribute__((aligned(4))) ctrl_tx[LINK_CTRL_BYTES];
static uint8_t __attribute__((aligned(4))) ctrl_rx[LINK_CTRL_BYTES];

/* Z80 RAM snapshot landing area. DMA writes here, then the backend
 * merges it under its own recent writes. */
static uint8_t __attribute__((aligned(4))) zram_snapshot[LINK_ZRAM_BYTES];

static link_t         link;
static link_session_t session;
static bool           initialized;
static bool           connected;
static uint32_t       last_foreign_reads;

/* Steady-state patience. One NTSC frame is 16.7 ms; a slave that has
 * stopped answering should cost a frame of audio and be noticed, not
 * stall the emulator behind a two-second handshake timeout. */
#define LINK_FRAME_TIMEOUT_US 8000u

void link_master_init(void) {
    if (initialized) return;

    /* Bus A is our transmitter (GPIO20..29), bus B our receiver
     * (GPIO30..39). FS is an output on this side. */
    link_init(&link, LINK_PIO_MASTER,
              M_LINK_A_DATA_BASE, M_LINK_B_DATA_BASE,
              M_LINK_DB_OUT, M_LINK_DB_IN, M_LINK_FS, true);

    memset(&session, 0, sizeof(session));
    session.link    = &link;
    session.ctrl_tx = ctrl_tx;
    session.ctrl_rx = ctrl_rx;

    initialized = true;
    LOG("Link: PIO2 claimed, TX base=%d RX base=%d, %lu KiB/s\n",
        M_LINK_A_DATA_BASE, M_LINK_B_DATA_BASE,
        (unsigned long)(link_byte_rate(&link) / 1024));
}

bool link_master_probe(uint32_t timeout_us, link_node_info_t *info) {
    if (!initialized) return false;

    session.handshake_timeout_us = timeout_us;

    bool ok = link_m_send_ctrl(&session, LINK_OP_HELLO, 0, 0, NULL, 0) &&
              link_m_recv_ctrl(&session) &&
              link_rx_hdr(&session)->op == LINK_OP_HELLO_ACK;

    if (ok && info) {
        memcpy(info, ctrl_rx + sizeof(link_hdr_t), sizeof(*info));
    }

    session.handshake_timeout_us = 0;
    connected = ok;
    return ok;
}

bool link_master_connected(void) {
    return connected;
}

bool link_master_ping(uint32_t *rtt_us) {
    if (!initialized) return false;

    absolute_time_t t0 = get_absolute_time();

    bool ok = link_m_send_ctrl(&session, LINK_OP_PING, 0, 0, NULL, 0) &&
              link_m_recv_ctrl(&session) &&
              link_rx_hdr(&session)->op == LINK_OP_PONG;

    if (ok && rtt_us) {
        int64_t us = absolute_time_diff_us(t0, get_absolute_time());
        *rtt_us = us > 0 ? (uint32_t)us : 0;
    }
    if (!ok) connected = false;
    return ok;
}

bool link_master_reset_sound(void) {
    if (!connected) return false;

    bool ok = link_m_send_ctrl(&session, LINK_OP_RESET, 0, 0, NULL, 0) &&
              link_m_recv_ctrl(&session) &&
              link_rx_hdr(&session)->op == LINK_OP_RESET_ACK;
    if (!ok) connected = false;
    return ok;
}

bool link_master_send_config(const link_sound_config_t *cfg) {
    if (!connected) return false;

    bool ok = link_m_send_ctrl(&session, LINK_OP_CONFIG, 0, 0, cfg, sizeof(*cfg)) &&
              link_m_recv_ctrl(&session) &&
              link_rx_hdr(&session)->op == LINK_OP_CONFIG_ACK;
    if (!ok) connected = false;
    return ok;
}

bool link_master_upload_rom(const uint8_t *rom, uint32_t bytes) {
    if (!connected) return false;

    /* Boot-time patience: the slave erases nothing here, but it does
     * copy each chunk into PSRAM, which is the slow half of this. */
    session.handshake_timeout_us = LINK_HANDSHAKE_TIMEOUT_US;

    bool ok = link_m_send_ctrl(&session, LINK_OP_ROM_BEGIN, bytes, 0, NULL, 0) &&
              link_m_recv_ctrl(&session) &&
              link_rx_hdr(&session)->op == LINK_OP_ROM_BEGIN_ACK;

    if (ok && link_rx_hdr(&session)->arg0 < bytes) {
        LOG("Link: ROM %lu KB exceeds slave PSRAM %lu KB\n",
            (unsigned long)(bytes >> 10),
            (unsigned long)(link_rx_hdr(&session)->arg0 >> 10));
        ok = false;
    }

    for (uint32_t off = 0; ok && off < bytes; off += LINK_ROM_CHUNK_BYTES) {
        uint32_t len = bytes - off;
        if (len > LINK_ROM_CHUNK_BYTES) len = LINK_ROM_CHUNK_BYTES;

        /* Bulk lengths must be whole words. */
        uint32_t wire_len = LINK_ALIGN4(len);

        ok = link_m_send_ctrl(&session, LINK_OP_ROM_CHUNK, off, len, NULL, 0) &&
             link_m_bulk_send(&session, rom + off, wire_len) &&
             link_m_recv_ctrl(&session) &&
             link_rx_hdr(&session)->op == LINK_OP_ROM_CHUNK_ACK &&
             link_rx_hdr(&session)->arg1 == 0;
    }

    if (ok) {
        uint32_t our_crc = link_crc32(rom, bytes);

        ok = link_m_send_ctrl(&session, LINK_OP_ROM_END, our_crc, 0, NULL, 0) &&
             link_m_recv_ctrl(&session) &&
             link_rx_hdr(&session)->op == LINK_OP_ROM_END_ACK;

        if (ok && link_rx_hdr(&session)->arg0 != our_crc) {
            LOG("Link: ROM CRC mismatch — ours %08lx, slave %08lx\n",
                (unsigned long)our_crc,
                (unsigned long)link_rx_hdr(&session)->arg0);
            ok = false;
        }
    }

    session.handshake_timeout_us = 0;
    if (!ok) connected = false;
    return ok;
}

bool link_master_frame(const link_event_t *events, uint32_t count,
                       int audio_target, uint32_t seq,
                       int16_t *ym_out, int16_t *sn_out,
                       uint32_t *ym_count, uint32_t *sn_count,
                       void (*zram_merge)(const uint8_t *snapshot)) {
    if (!connected) return false;

    /* Steady state runs on a short leash: a slave that has died should
     * cost one frame of audio, not stall the emulator. */
    session.handshake_timeout_us = LINK_FRAME_TIMEOUT_US;

    bool ok = link_m_send_ctrl(&session, LINK_OP_FRAME, count,
                               (uint32_t)audio_target, NULL, 0);

    if (ok && count) {
        ok = link_m_bulk_send(&session, events, count * sizeof(link_event_t));
    }

    if (ok) {
        ok = link_m_recv_ctrl(&session) &&
             link_rx_hdr(&session)->op == LINK_OP_FRAME_ACK;
    }

    if (ok) {
        link_frame_reply_t reply;
        memcpy(&reply, ctrl_rx + sizeof(link_hdr_t), sizeof(reply));

        uint32_t ym = reply.ym_samples;
        uint32_t sn = reply.sn_samples;
        if (ym > LINK_MAX_SAMPLES) ym = LINK_MAX_SAMPLES;
        if (sn > LINK_MAX_SAMPLES) sn = LINK_MAX_SAMPLES;

        if (ok && ym) {
            ok = link_m_bulk_recv(&session, ym_out,
                                  LINK_ALIGN4(ym * sizeof(int16_t)));
        }
        if (ok && sn) {
            ok = link_m_bulk_recv(&session, sn_out,
                                  LINK_ALIGN4(sn * sizeof(int16_t)));
        }
        if (ok && reply.zram_bytes) {
            ok = link_m_bulk_recv(&session, zram_snapshot, LINK_ZRAM_BYTES);
            if (ok && zram_merge) zram_merge(zram_snapshot);
        }

        if (ok) {
            if (ym_count) *ym_count = ym;
            if (sn_count) *sn_count = sn;
        }

        /* Surface anything the slave could not do rather than letting it
         * show up only as sound that is subtly wrong. */
        if (reply.foreign_reads != last_foreign_reads) {
            last_foreign_reads = reply.foreign_reads;
            LOG("Link: slave saw %lu unservable 68K reads\n",
                (unsigned long)reply.foreign_reads);
        }
    }

    (void)seq;
    session.handshake_timeout_us = 0;
    if (!ok) connected = false;
    return ok;
}

void link_master_request_slave_reset(void) {
    if (!initialized) return;

    /* Held long enough that the slave's timer interrupt cannot miss it,
     * short enough not to stall a frame if called from the idle path. */
    link_fs_set(&link, true);
    sleep_ms(250);
    link_fs_set(&link, false);
    connected = false;
}
