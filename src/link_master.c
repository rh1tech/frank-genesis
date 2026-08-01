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
#include "hardware/clocks.h"

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

/* Where the last frame exchange failed, so a link that drops says which
 * step broke instead of just going quiet. */
uint32_t link_fail_stage;

/* ROM upload progress, so a failed upload names the step it died on.
 * 0 = never attempted, 1 = BEGIN sent, 2 = BEGIN acked, 3 = in chunks,
 * 4 = END sent, 5 = END acked, 9 = complete and CRC matched. */
uint32_t link_upload_stage;
uint32_t link_upload_chunk;
uint32_t link_probe_ok, link_probe_fail;

/* The slave does not merely acknowledge a frame — it replays the whole
 * event stream, runs the Z80 and renders the FM and PSG, which is
 * precisely the work we moved off the master. That takes on the order of
 * a frame, so the master must wait for it. An 8 ms budget (my first
 * guess, one NTSC frame's worth of "surely it is quick") dropped the
 * link after ~50 frames. Generous enough for the slave to finish,
 * bounded so a dead slave is still noticed within a few frames. */
#define LINK_FRAME_TIMEOUT_US 100000u

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

    if (ok) {
        link_node_info_t ni;
        memcpy(&ni, ctrl_rx + sizeof(link_hdr_t), sizeof(ni));
        if (info) *info = ni;

        /* Match the wire rate to the slower half.
         *
         * Only the transmitter is divided; the receiver is edge-driven
         * and runs at its own system clock, needing 5 of its cycles per
         * byte. So if the slave is slower than us, we must stretch our
         * bulk bytes by exactly that ratio or it drops them — which
         * looks like working control frames and failing bulk, because
         * control already runs at a slower divider.
         *
         * Building both halves at the same CPU_SPEED is still the
         * intent; this makes a mismatch degrade throughput instead of
         * silently corrupting every transfer. */
        uint32_t ours = clock_get_hz(clk_sys);
        if (ni.sys_clk_hz && ours > ni.sys_clk_hz) {
            float ratio = (float)ours / (float)ni.sys_clk_hz;
            link_set_bulk_clkdiv(&link, ratio);
            LOG("Link: slave at %lu MHz vs our %lu — bulk divider %d.%02d\n",
                (unsigned long)(ni.sys_clk_hz / 1000000),
                (unsigned long)(ours / 1000000),
                (int)ratio, (int)((ratio - (int)ratio) * 100));
        } else {
            link_set_bulk_clkdiv(&link, 1.0f);
        }
    }

    session.handshake_timeout_us = 0;
    connected = ok;
    if (ok) link_probe_ok++; else link_probe_fail++;
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

    link_upload_stage = 1;
    link_upload_chunk = 0;
    bool ok = link_m_send_ctrl(&session, LINK_OP_ROM_BEGIN, bytes, 0, NULL, 0) &&
              link_m_recv_ctrl(&session) &&
              link_rx_hdr(&session)->op == LINK_OP_ROM_BEGIN_ACK;
    if (ok) link_upload_stage = 2;

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

        link_upload_stage = 3;
        link_upload_chunk = off / LINK_ROM_CHUNK_BYTES;
        ok = link_m_send_ctrl(&session, LINK_OP_ROM_CHUNK, off, len, NULL, 0) &&
             link_m_bulk_send(&session, rom + off, wire_len) &&
             link_m_recv_ctrl(&session) &&
             link_rx_hdr(&session)->op == LINK_OP_ROM_CHUNK_ACK &&
             link_rx_hdr(&session)->arg1 == 0;
    }

    if (ok) {
        uint32_t our_crc = link_crc32(rom, bytes);

        link_upload_stage = 4;
        ok = link_m_send_ctrl(&session, LINK_OP_ROM_END, our_crc, 0, NULL, 0) &&
             link_m_recv_ctrl(&session) &&
             link_rx_hdr(&session)->op == LINK_OP_ROM_END_ACK;
        if (ok) link_upload_stage = 5;

        if (ok && link_rx_hdr(&session)->arg0 != our_crc) {
            LOG("Link: ROM CRC mismatch — ours %08lx, slave %08lx\n",
                (unsigned long)our_crc,
                (unsigned long)link_rx_hdr(&session)->arg0);
            ok = false;
        }
    }

    session.handshake_timeout_us = 0;
    if (ok) link_upload_stage = 9;
    if (!ok) connected = false;
    return ok;
}

bool link_master_frame(const link_event_t *events, uint32_t count,
                       bool zram_dirty, int audio_target, uint32_t seq,
                       int16_t *ym_out, int16_t *sn_out,
                       uint32_t *ym_count, uint32_t *sn_count,
                       void (*zram_merge)(const uint8_t *snapshot)) {
    if (!connected) return false;

    /* Steady state runs on a short leash: a slave that has died should
     * cost one frame of audio, not stall the emulator. */
    session.handshake_timeout_us = LINK_FRAME_TIMEOUT_US;

    link_fail_stage = 1;
    bool ok = link_m_send_ctrl(&session, LINK_OP_FRAME, count,
                               (uint32_t)audio_target, NULL, 0);
    /* Always follow with ZRAM_BLOCK so the slave's step sequence is the
     * same whether or not anything changed. */
    if (ok) {
        link_fail_stage = 2;
        ok = link_m_send_ctrl(&session, LINK_OP_ZRAM_BLOCK,
                              zram_dirty ? 1 : 0, 0, NULL, 0);
    }
    if (ok && zram_dirty) {
        extern uint32_t *zram_frame_dirty;
        extern uint8_t *zram_frame_data;
        link_fail_stage = 3;
        ok = link_m_bulk_send(&session, zram_frame_dirty, LINK_ZRAM_BYTES / 8) &&
             link_m_bulk_send(&session, zram_frame_data, LINK_ZRAM_BYTES);
    }

    if (ok && count) {
        link_fail_stage = 4;
        ok = link_m_bulk_send(&session, events, count * sizeof(link_event_t));
    }

    if (ok) {
        link_fail_stage = 5;
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
            link_fail_stage = 0;
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
