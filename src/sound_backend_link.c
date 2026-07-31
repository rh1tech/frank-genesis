/*
 * frank-genesis — C2 master: link sound backend
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implements the sound_backend.h seam by turning every access into a
 * timestamped event for the slave, and answers the two reads that would
 * otherwise need a round trip out of master-side state.
 *
 * Threading: core 0 (the emulator) produces events into one of two
 * buffers; core 1 (sound_core) ships the completed buffer and collects
 * the results while core 0 is already emulating the next frame. Core 0
 * never blocks on the link. The buffers flip at the frame boundary,
 * under the frame_ready / audio_done handshake main.c already uses to
 * hand audio to core 1, so no new synchronisation is introduced.
 */

#include "sound/sound_backend.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/sync.h"

#include "link_master.h"
#include "link_proto.h"

#if ENABLE_LOGGING
#define LOG(...) printf(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

/* =====================================================================
 * Event production
 * ===================================================================== */

static link_event_t events[2][LINK_MAX_EVENTS];
static uint32_t     event_count[2];
static volatile int event_write;      /* buffer core 0 is filling */
static uint32_t     event_overflows;

static inline void emit(uint8_t type, uint16_t addr, uint8_t val, int cycles) {
    uint32_t n = event_count[event_write];

    /* Truncate rather than wrap. Losing the tail of one frame's writes
     * degrades that frame's sound; wrapping would reorder the stream and
     * desynchronise the chips for every frame after it. */
    if (n >= LINK_MAX_EVENTS) {
        event_overflows++;
        return;
    }

    link_event_t *e = &events[event_write][n];
    e->cycles = (uint32_t)cycles;
    e->type   = type;
    e->val    = val;
    e->addr   = addr;
    event_count[event_write] = n + 1;
}

/* =====================================================================
 * Z80 RAM mirror
 *
 * The 68K reads Z80 RAM directly (bus.c:484). The authoritative copy
 * lives on the slave, which returns all 8 KB once per frame.
 *
 * The master cannot simply overwrite its mirror with that snapshot: the
 * 68K has kept writing since the snapshot was taken, and those bytes are
 * newer. A dirty bitmap records which bytes the master has written since
 * the last merge; those keep the master's value and everything else
 * takes the slave's. Cleared on each merge, so the window is exactly one
 * frame.
 * ===================================================================== */

static uint8_t  zram_mirror[LINK_ZRAM_BYTES];
static uint32_t zram_dirty[LINK_ZRAM_BYTES / 32];   /* one bit per byte */

void sound_zram_write(unsigned int offset, unsigned int value) {
    offset &= (LINK_ZRAM_BYTES - 1);

    zram_mirror[offset] = (uint8_t)value;
    zram_dirty[offset >> 5] |= 1u << (offset & 31);

    emit(LINK_EV_ZRAM_WRITE, (uint16_t)offset, (uint8_t)value, 0);
}

unsigned int sound_zram_read(unsigned int offset) {
    return zram_mirror[offset & (LINK_ZRAM_BYTES - 1)];
}

/* Merge a fresh snapshot under the master's own recent writes. Word-wise
 * on the bitmap so the common case — a frame in which the 68K touched
 * nothing — costs one compare per 32 bytes. */
static void zram_merge(const uint8_t *snapshot) {
    for (uint32_t w = 0; w < LINK_ZRAM_BYTES / 32; w++) {
        uint32_t dirty = zram_dirty[w];
        uint32_t base  = w * 32;

        if (dirty == 0) {
            memcpy(&zram_mirror[base], &snapshot[base], 32);
            continue;
        }
        for (uint32_t b = 0; b < 32; b++) {
            if (!(dirty & (1u << b))) zram_mirror[base + b] = snapshot[base + b];
        }
        zram_dirty[w] = 0;
    }
}

/* =====================================================================
 * YM2612 status shadow
 *
 * YM2612Read() returns only ym2612.OPN.ST.status: timer A and B overflow
 * flags. Those depend on register writes 0x24..0x27 and elapsed time,
 * not on FM synthesis, so the master can answer without the slave.
 *
 * The logic below mirrors ym2612.c exactly — set_timers(),
 * INTERNAL_TIMER_A() and INTERNAL_TIMER_B() — because a shadow derived
 * from a datasheet instead of from the code we are actually running
 * would drift in precisely the corner cases that make games hang.
 *
 * Timers tick per rendered sample: A once per sample, B by the batch
 * length. Samples advance as (target - clock) / AUDIO_FREQ_DIVISOR,
 * which is the same conversion ym2612_run() uses.
 * ===================================================================== */

#ifndef AUDIO_FREQ_DIVISOR
#define AUDIO_FREQ_DIVISOR 60
#endif

static struct {
    int32_t TA, TAL, TAC;
    int32_t TB, TBL, TBC;
    uint8_t mode;
    uint8_t status;
    int32_t clock;      /* master cycles already accounted for */
    uint8_t addr_latch; /* last address written to port 0/2    */
} ym;

static void ym_shadow_reset(void) {
    memset(&ym, 0, sizeof(ym));
    ym.TAL = 1024;
    ym.TBL = 256 << 4;
    ym.TAC = ym.TAL;
    ym.TBC = ym.TBL;
    /* ym2612.c calls set_timers(0x30) at reset: both timer flags reset,
     * both timers off. */
    ym.mode = 0x30;
}

/* Advance the timers to `target` master cycles. */
static void ym_shadow_run(int target) {
    if (target <= ym.clock) return;

    int32_t samples = (target - ym.clock) / AUDIO_FREQ_DIVISOR;
    if (samples <= 0) return;
    ym.clock += samples * AUDIO_FREQ_DIVISOR;

    /* Timer A: one tick per sample. Stepping the counter down in a loop
     * would be exact but is O(samples); the reload is periodic, so the
     * same result comes from arithmetic when it overflows at all. */
    if (ym.mode & 0x01) {
        if (ym.TAC > samples) {
            ym.TAC -= samples;
        } else {
            if (ym.mode & 0x04) ym.status |= 0x01;
            int32_t rem = samples - ym.TAC;
            if (ym.TAL > 0) rem %= ym.TAL;
            ym.TAC = ym.TAL - rem;
        }
    }

    /* Timer B is stepped by the batch length in ym2612.c, with a
     * do/while reload that tolerates a step larger than the period. */
    if (ym.mode & 0x02) {
        ym.TBC -= samples;
        if (ym.TBC <= 0) {
            if (ym.mode & 0x08) ym.status |= 0x02;
            if (ym.TBL > 0) {
                do { ym.TBC += ym.TBL; } while (ym.TBC <= 0);
            } else {
                ym.TBC = 1;
            }
        }
    }
}

static void ym_shadow_write(unsigned int port, unsigned int value, int cycles) {
    ym_shadow_run(cycles);

    /* Ports 0/2 latch a register address, ports 1/3 write data. Only
     * bank 0 (port 0/1) carries the timer registers. */
    if ((port & 1) == 0) {
        ym.addr_latch = (uint8_t)value;
        return;
    }
    if (port != 1) return;      /* bank 1 has no timer registers */

    uint8_t v = (uint8_t)value;

    switch (ym.addr_latch) {
    case 0x24:
        ym.TA  = (ym.TA & 0x03) | ((int32_t)v << 2);
        ym.TAL = 1024 - ym.TA;
        break;
    case 0x25:
        ym.TA  = (ym.TA & 0x3FC) | (v & 3);
        ym.TAL = 1024 - ym.TA;
        break;
    case 0x26:
        ym.TB  = v;
        ym.TBL = (256 - v) << 4;
        break;
    case 0x27:
        /* set_timers(): load edges reload the counters, bits 4/5 clear
         * the corresponding status flags. */
        if ((v & 1) && !(ym.mode & 1)) ym.TAC = ym.TAL;
        if ((v & 2) && !(ym.mode & 2)) ym.TBC = ym.TBL;
        ym.status &= (uint8_t)(~v >> 4);
        ym.mode = v;
        break;
    default:
        break;
    }
}

/* =====================================================================
 * The seam
 * ===================================================================== */

void sound_ym_write(unsigned int port, unsigned int value, int cycles) {
    ym_shadow_write(port, value, cycles);
    emit(LINK_EV_YM_WRITE, (uint16_t)(port & 3), (uint8_t)value, cycles);
}

unsigned int sound_ym_read(int cycles) {
    ym_shadow_run(cycles);
    return ym.status;
}

void sound_psg_write(unsigned int value, int cycles) {
    emit(LINK_EV_PSG_WRITE, 0, (uint8_t)value, cycles);
}

/* BUSREQ and RESET are answered entirely from master-side state: on the
 * master today they only set and read these two flags, so the slave is
 * never consulted. That matters more than it looks — games poll BUSREQ
 * hard, and a round trip here would dominate the link. */
static int z80_bus_ack;
static int z80_reset_held;

void sound_z80_ctrl_write(unsigned int address, unsigned int value) {
    if ((address & 0x1F00) == 0x1100) {
        z80_bus_ack = value ? 1 : 0;
        emit(LINK_EV_BUSREQ, 0, (uint8_t)(value ? 1 : 0), 0);
    } else if ((address & 0x1F00) == 0x1200) {
        z80_reset_held = value ? 0 : 1;
        emit(LINK_EV_RESET_LINE, 0, (uint8_t)(value ? 1 : 0), 0);
    }
}

unsigned int sound_z80_ctrl_read(unsigned int address) {
    address &= 0xFFFF;
    if (address == 0x1100) return z80_bus_ack ? 0 : 1;
    if (address == 0x1101) return 0x00;
    if (address == 0x1200) return z80_reset_held;
    if (address == 0x1201) return 0x00;
    return 0xFF;
}

void sound_z80_irq(unsigned int level) {
    emit(LINK_EV_IRQ, 0, (uint8_t)(level ? 1 : 0), 0);
}

void sound_z80_run(int target) {
    /* Pure time marker: it is what reproduces the master's Z80
     * scheduling on the slave. */
    emit(LINK_EV_RUN_UNTIL, 0, 0, target);
}

/* =====================================================================
 * Frame boundary
 * ===================================================================== */

/* Filled by the exchange, consumed by main.c's audio path. */
int16_t  link_ym_samples_buf[LINK_MAX_SAMPLES];
int16_t  link_sn_samples_buf[LINK_MAX_SAMPLES];
volatile uint32_t link_ym_sample_count;
volatile uint32_t link_sn_sample_count;

static volatile int      pending_buffer = -1;   /* buffer awaiting send */
static volatile uint32_t pending_target;
static uint32_t          frame_seq;

void sound_frame_end(int audio_target_clock) {
    /* Hand the finished buffer to core 1 and start filling the other.
     * If core 1 has not drained the previous one yet the emulator is
     * outrunning the slave; dropping this frame's events would
     * desynchronise the chips, so we wait — the same backpressure
     * main.c already applies when core 1 is behind. */
    while (pending_buffer >= 0) tight_loop_contents();

    pending_target = (uint32_t)audio_target_clock;
    pending_buffer = event_write;
    __dmb();

    event_write = 1 - event_write;
    event_count[event_write] = 0;
}

/* Called from core 1. Ships the pending frame and collects the slave's
 * reply. Returns false if the exchange failed, in which case the caller
 * outputs silence for this frame rather than stale samples. */
bool sound_link_exchange(void) {
    int buf = pending_buffer;
    if (buf < 0) return false;

    bool ok = link_master_frame(events[buf], event_count[buf],
                                (int)pending_target, ++frame_seq,
                                link_ym_samples_buf, link_sn_samples_buf,
                                (uint32_t *)&link_ym_sample_count,
                                (uint32_t *)&link_sn_sample_count,
                                zram_merge);

    __dmb();
    pending_buffer = -1;

    if (!ok) {
        link_ym_sample_count = 0;
        link_sn_sample_count = 0;
    }
    return ok;
}

void sound_link_backend_reset(void) {
    ym_shadow_reset();

    memset(zram_mirror, 0, sizeof(zram_mirror));
    memset(zram_dirty, 0, sizeof(zram_dirty));

    event_count[0] = event_count[1] = 0;
    event_write    = 0;
    pending_buffer = -1;
    event_overflows = 0;

    z80_bus_ack    = 0;
    z80_reset_held = 0;

    link_ym_sample_count = 0;
    link_sn_sample_count = 0;
}

uint32_t sound_link_event_overflows(void) {
    return event_overflows;
}
