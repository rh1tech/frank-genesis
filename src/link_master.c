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

/* ROM upload staging.
 *
 * The ROM lives in PSRAM, and DMA-ing the link straight out of the XIP
 * window mixes link traffic with core 0's own ROM fetches on the same
 * QMI. The first upload survives because it runs at ROM-load time with
 * core 0 idle; every reconnect-triggered upload runs mid-game and is
 * silently corrupted — the CPU still computes the right CRC over the
 * same addresses, so the master sends bad bytes while believing them
 * good, the slave rejects on CRC, and the pair loops re-uploading
 * forever with no sound at all.
 *
 * The CPU read is coherent, so bounce each chunk through SRAM and let
 * the DMA touch only SRAM. The slave's receive path already does this
 * for the same reason. */
static uint8_t __attribute__((aligned(4))) rom_tx_chunk[LINK_ROM_CHUNK_BYTES];

static link_t         link;
static link_session_t session;
static bool           initialized;
static bool           connected;
static uint32_t       last_foreign_reads;
uint32_t link_foreign_last, link_foreign_min, link_foreign_max;

uint32_t link_sig_count;

/* Where the last frame exchange failed, so a link that drops says which
 * step broke instead of just going quiet. */
uint32_t link_fail_stage;

/* ROM upload progress, so a failed upload names the step it died on.
 * 0 = never attempted, 1 = BEGIN sent, 2 = BEGIN acked, 3 = in chunks,
 * 4 = END sent, 5 = END acked, 9 = complete and CRC matched. */
uint32_t link_frame_fails;      /* exchanges that did not complete */
uint32_t link_ym_mismatch;      /* frames where the shadow disagreed */
uint32_t link_ym_checked;
uint8_t  link_ym_last_ours, link_ym_last_theirs;
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
        memcpy(rom_tx_chunk, rom + off, len);
        if (wire_len > len) {
            memset(rom_tx_chunk + len, 0, wire_len - len);
        }

        ok = link_m_send_ctrl(&session, LINK_OP_ROM_CHUNK, off, len, NULL, 0) &&
             link_m_bulk_send(&session, rom_tx_chunk, wire_len) &&
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

uint32_t link_sync_fails, link_syncs;
uint32_t link_us_sc, link_us_bs, link_us_rc, link_n_sc;
uint32_t link_us_ctrl, link_us_events, link_us_ack, link_us_samples,
         link_us_snapshot, link_snapshots;

/* The exchange is split so the slave's compute can overlap the master's
 * emulation. Measured: 95% of a 3.9 ms exchange was the master sitting
 * in the FRAME_ACK wait while the slave replayed the frame and rendered
 * its samples. Waiting for that inline hands the time straight back and
 * left the offloaded build running slower (57.6 fps) than the build that
 * does sound locally (60.5). Send frame N, go away and emulate, collect
 * N's reply at the top of frame N+1 by which time it is already
 * waiting. Costs one frame of audio latency (16.7 ms). */
/* Mid-frame: push the events emitted so far and read one Z80 RAM byte as
 * of that point. Called from core 0 between frames' exchanges, when core
 * 1 is idle, so it does not contend for the link. */
bool link_master_sync_peek(const link_event_t *events, uint32_t count,
                           uint16_t offset, uint8_t *out) {
    if (!connected) return false;

    session.handshake_timeout_us = LINK_FRAME_TIMEOUT_US;

    /* No output pointer means the caller only wants the events replayed,
     * not a value back. Tell the slave, so it can acknowledge before it
     * replays instead of making us wait for its Z80 to catch up. */
    uint32_t arg1 = offset | (out ? 0u : LINK_SYNC_NO_PEEK);
    uint64_t p0 = time_us_64();
    bool ok = link_m_send_ctrl(&session, LINK_OP_SYNC, count, arg1, NULL, 0);
    uint64_t p1 = time_us_64();
    if (ok && count) {
        ok = link_m_bulk_send(&session, events, count * sizeof(link_event_t));
    }
    uint64_t p2 = time_us_64();
    if (ok) {
        ok = link_m_recv_ctrl(&session) &&
             link_rx_hdr(&session)->op == LINK_OP_SYNC_ACK;
    }
    uint64_t p3 = time_us_64();
    link_us_sc += (uint32_t)(p1 - p0);
    link_us_bs += (uint32_t)(p2 - p1);
    link_us_rc += (uint32_t)(p3 - p2);
    link_n_sc++;
    if (ok && out) *out = (uint8_t)(link_rx_hdr(&session)->arg0 & 0xFF);

    session.handshake_timeout_us = 0;
    if (!ok) {
        link_sync_fails++;
        link_db_set(&link, false);
        connected = false;
    }
    return ok;
}

bool link_master_frame_send(const link_event_t *events, uint32_t count,
                            bool zram_dirty, int audio_target, uint32_t seq) {
    if (!connected) return false;

    /* Steady state runs on a short leash: a slave that has died should
     * cost one frame of audio, not stall the emulator. */
    session.handshake_timeout_us = LINK_FRAME_TIMEOUT_US;

    /* Where the 3.9 ms/frame actually goes. The offload only pays for
     * itself if the exchange costs less than the sound work it removed;
     * right now it does not, so the phases need to be attributable. */
    uint64_t t_a = time_us_64();
    link_fail_stage = 1;
    bool ok = link_m_send_ctrl(&session, LINK_OP_FRAME, count,
                               (uint32_t)audio_target, NULL, 0);
    /* Always follow with ZRAM_BLOCK so the slave's step sequence is the
     * same whether or not anything changed. */
    if (ok) {
        /* arg1 asks for a Z80 RAM snapshot in the reply.
         *
         * Sending 8 KB back every frame cost a bulk transfer and a
         * doorbell round trip for data the 68K reads about 0.3 times a
         * frame. That is pure latency on core 1, which has a 16.7 ms
         * budget to keep the I2S chain fed — and missing it makes the
         * DMA replay a stale buffer, which is audible. Ask only when the
         * 68K has actually looked at Z80 RAM since the last snapshot. */
        extern volatile bool zram_read_since_snapshot;
        uint32_t want_snapshot = 1u; (void)zram_read_since_snapshot;

        link_fail_stage = 2;
        ok = link_m_send_ctrl(&session, LINK_OP_ZRAM_BLOCK,
                              zram_dirty ? 1 : 0, want_snapshot, NULL, 0);
        if (ok && want_snapshot) zram_read_since_snapshot = false;
    }
    if (ok && zram_dirty) {
        extern uint32_t *zram_frame_dirty;
        extern uint8_t *zram_frame_data;
        link_fail_stage = 3;
        ok = link_m_bulk_send(&session, zram_frame_dirty, LINK_ZRAM_BYTES / 8) &&
             link_m_bulk_send(&session, zram_frame_data, LINK_ZRAM_BYTES);
    }

    uint64_t t_b = time_us_64();          /* ctrl + zram block sent */
    link_us_ctrl += (uint32_t)(t_b - t_a);

    if (ok && count) {
        link_fail_stage = 4;
        ok = link_m_bulk_send(&session, events, count * sizeof(link_event_t));
    }
    uint64_t t_c = time_us_64();          /* events sent */
    link_us_events += (uint32_t)(t_c - t_b);

    (void)seq;
    if (!ok) {
        link_frame_fails++;
        link_db_set(&link, false);
        connected = false;
    }
    return ok;
}

bool link_master_frame_collect(int16_t *ym_out, int16_t *sn_out,
                               uint32_t *ym_count, uint32_t *sn_count,
                               void (*zram_merge)(const uint8_t *snapshot)) {
    if (!connected) return false;

    session.handshake_timeout_us = LINK_FRAME_TIMEOUT_US;
    uint64_t t_c = time_us_64();
    bool ok;

    {
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

        link_us_ack += (uint32_t)(time_us_64() - t_c);
        uint64_t t_d = time_us_64();
        if (ok && ym) {
            ok = link_m_bulk_recv(&session, ym_out,
                                  LINK_ALIGN4(ym * sizeof(int16_t)));
        }
        if (ok && sn) {
            ok = link_m_bulk_recv(&session, sn_out,
                                  LINK_ALIGN4(sn * sizeof(int16_t)));
        }
        link_us_samples += (uint32_t)(time_us_64() - t_d);
        uint64_t t_e = time_us_64();
        if (ok && reply.zram_bytes) {
            ok = link_m_bulk_recv(&session, zram_snapshot, LINK_ZRAM_BYTES);
            if (ok && zram_merge) zram_merge(zram_snapshot);
            link_snapshots++;
        }
        link_us_snapshot += (uint32_t)(time_us_64() - t_e);

        if (ok) {
            if (ym_count) *ym_count = ym;
            if (sn_count) *sn_count = sn;
            link_fail_stage = 0;
        }

        /* Audit the master's YM status shadow against the real chip. */
        {
            extern volatile uint8_t link_ym_status_real;
            extern uint8_t sound_ym_shadow_status(void);
            uint8_t ours = sound_ym_shadow_status();
            link_ym_status_real = (uint8_t)reply.ym_status;   /* publish truth */
            link_ym_checked++;
            if (ours != (uint8_t)reply.ym_status) {
                link_ym_mismatch++;
                link_ym_last_ours   = ours;
                link_ym_last_theirs = (uint8_t)reply.ym_status;
            }
        }

        /* Surface anything the slave could not do rather than letting it
         * show up only as sound that is subtly wrong. */
        link_sig_count++;

#ifdef SOUND_CAPTURE
        {
            extern volatile uint32_t snd_cap_z80_src;
            extern volatile uint32_t snd_cap_reg_src[6];
            if (ok) {
                snd_cap_z80_src = reply.z80_sig;
                for (int i = 0; i < 6; i++) snd_cap_reg_src[i] = reply.z80_dbg[i];
            }
        }
#endif
        link_foreign_last = reply.foreign_last_addr;
        link_foreign_min  = reply.foreign_min_addr;
        link_foreign_max  = reply.foreign_max_addr;

        if (reply.foreign_reads != last_foreign_reads) {
            last_foreign_reads = reply.foreign_reads;
            LOG("Link: slave saw %lu unservable 68K reads\n",
                (unsigned long)reply.foreign_reads);
        }
    }

    session.handshake_timeout_us = 0;

    if (!ok) {
        link_frame_fails++;
        /* Drop both doorbells and let the peer's own timeout expire
         * before anything else is attempted, so the two sides restart
         * from a known state instead of interleaving a half-finished
         * exchange with the next one. */
        link_db_set(&link, false);
        connected = false;
    }
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
