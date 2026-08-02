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

/* The 68K lives here, so the true cycle count is available directly. */
extern int m68k_cycles_master(void);

#if ENABLE_LOGGING
#define LOG(...) printf(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

/* =====================================================================
 * Event production
 * ===================================================================== */

static bool frame_in_flight;
uint32_t zram_block_fallbacks;
/* A driver upload is thousands of byte writes in one frame. Letting
 * those into the ring truncates the tail of the SAME frame's RUN_UNTIL
 * and IRQ events, which is far worse than mis-ordering an upload the
 * Z80 is held out of anyway: it desynchronises the Z80's schedule. Cap
 * what ZRAM may take and spill the rest to the block. */
#define ZRAM_EV_BUDGET 1024u
static uint32_t zram_ev_frame;
static bool     zram_apply_marked;
static link_event_t events[2][LINK_MAX_EVENTS];
static uint32_t     event_count[2];
/* How many of each buffer's events the slave already has.
 *
 * The slave has to execute a frame's worth of Z80 either way; what cost
 * us was *when*. A 68K read of Z80 RAM made the slave catch up the whole
 * frame so far in one go while core 0 waited — measured at 2.7 ms a
 * sync, 19.4% of the frame. Core 1 is idle about half a frame, so it
 * hands the slave what has accumulated as the frame runs and core 0
 * never waits for it. A read then only has to replay the little that is
 * left. */
static volatile uint32_t event_sent[2];
static uint32_t     pending_sent;
uint32_t            link_pushes, link_push_us;

/* Both cores drive the link now, so they must not interleave. */
static volatile int link_busy;
static inline void link_lock(void) {
    while (__atomic_exchange_n(&link_busy, 1, __ATOMIC_ACQUIRE))
        tight_loop_contents();
}
static inline void link_unlock(void) {
    __atomic_store_n(&link_busy, 0, __ATOMIC_RELEASE);
}

/* A whole frame is only about 44 events, so this has to be small or it
 * never fires: the point is to keep the slave close in *time*, and each
 * RUN_UNTIL is what lets it advance. */
#ifndef LINK_PUSH_THRESHOLD
#define LINK_PUSH_THRESHOLD 1u
#endif
/* Mid-frame flush.
 *
 * With the slave able to replay a partial frame, the event ring no
 * longer has to hold a whole frame: when it fills, hand it over and
 * carry on. That removes the bulk Z80 RAM block the driver upload used
 * to need (8 KB of byte writes against a 4096-event ring), and with it
 * the ordering hazard of a block that arrives after the events which
 * reference it. */
uint32_t link_mirror_waits, link_mirror_wait_us, link_mirror_timeouts;
uint32_t link_sync_us;
extern uint32_t link_syncs, link_sync_fails;
volatile uint32_t core0_frame;                 /* frame core 0 is running */
volatile uint32_t mirror_gen = 0xFFFFFFFFu;    /* last frame merged in */
static bool link_flush_chunk(uint16_t peek_off, uint8_t *out);

static volatile int event_write;      /* buffer core 0 is filling */
static uint32_t     event_overflows;

/* Per-type totals, for working out what actually fills a frame. */
uint32_t sound_link_ev_stats[16];

static inline bool emit(uint8_t type, uint16_t addr, uint8_t val, int cycles) {
    uint32_t n = event_count[event_write];

    sound_link_ev_stats[type & 15]++;

    /* Full: hand the buffer over and keep going, rather than truncating
     * the tail of the frame. */
    if (n >= LINK_MAX_EVENTS) {
        if (!link_flush_chunk(0, NULL)) {
            event_overflows++;
            return false;
        }
        /* The ring did not shrink — the slave merely has the events now.
         * Compact what it already holds so there is room to keep going. */
        {
            int w2 = event_write;
            uint32_t sent2 = event_sent[w2];
            if (sent2 == 0) { event_overflows++; return false; }
            memmove(events[w2], &events[w2][sent2],
                    (event_count[w2] - sent2) * sizeof(link_event_t));
            event_count[w2] -= sent2;
            event_sent[w2]   = 0;
        }
        n = event_count[event_write];
    }

    link_event_t *e = &events[event_write][n];
    e->cycles = (uint32_t)cycles;
    e->type   = type;
    e->val    = val;
    e->addr   = addr;
    event_count[event_write] = n + 1;
    return true;
}

/* Core 0 drives the link here, mid-frame. That is safe only while core 1
 * is not using it, which is exactly the window after it has finished the
 * previous frame's exchange — the same condition the mirror needed. */
__attribute__((unused))
static bool link_core1_idle(void) {
    if (core0_frame == 0) return true;
    uint32_t need = core0_frame - 1;
    if ((int32_t)(mirror_gen - need) >= 0) return true;

    uint64_t t0 = time_us_64();
    link_mirror_waits++;
    while ((int32_t)(mirror_gen - need) < 0) {
        if (time_us_64() - t0 > 20000) {      /* slave gone: do not hang */
            link_mirror_timeouts++;
            link_mirror_wait_us += (uint32_t)(time_us_64() - t0);
            return false;
        }
        tight_loop_contents();
    }
    link_mirror_wait_us += (uint32_t)(time_us_64() - t0);
    return true;
}

/* Core 1: give the slave whatever has piled up, so it keeps pace with
 * the master's frame instead of catching up in one lump later. */
void sound_link_push(void) {
    if (!link_master_connected()) return;
    {
        int w = event_write;
        if (event_count[w] - event_sent[w] < LINK_PUSH_THRESHOLD) return;
    }

    uint64_t t0 = time_us_64();
    link_lock();
    int      w = event_write;          /* re-read: core 0 may have flipped */
    uint32_t n = event_count[w];
    uint32_t sent = event_sent[w];
    if (n > sent && link_master_connected() &&
        link_master_sync_peek(&events[w][sent], n - sent, 0, NULL)) {
        event_sent[w] = n;
        link_pushes++;
    }
    link_unlock();
    link_push_us += (uint32_t)(time_us_64() - t0);
}

static bool link_flush_chunk(uint16_t peek_off, uint8_t *out) {
    if (!link_master_connected()) return false;

    uint64_t t0 = time_us_64();
    link_lock();
    int      w    = event_write;
    uint32_t n    = event_count[w];
    uint32_t sent = event_sent[w];
    bool ok = link_master_connected() &&
              link_master_sync_peek(&events[w][sent], n - sent, peek_off, out);
    if (ok) event_sent[w] = n;
    link_unlock();

    if (!ok) return false;
    link_syncs++;
    link_sync_us += (uint32_t)(time_us_64() - t0);
    return true;
}

/* Mirror freshness. The 68K polls Z80 RAM for the sound driver's replies,
 * and the slave only produces frame N's Z80 writes after the master has
 * finished frame N — so the mirror is inherently behind. Measured on the
 * local-sound build: a mirror holding the *previous* frame's end state
 * answers every read correctly until frame 154, while a two-frame-old one
 * first lies at frame 108 — exactly where the two builds diverge.
 *
 * Core 0 runs a frame ahead of core 1, so without help the mirror is two
 * frames old whenever a read lands early in a frame. These let a read
 * wait for the one-frame-old state it needs. Reads are rare (0.3 per
 * frame measured), so the stall is rare too. */
static   uint32_t pending_frame_no;
static   uint32_t inflight_frame_no;

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
static uint32_t zram_dirty[LINK_ZRAM_BYTES / 32];   /* since last merge  */

/* Per-frame dirty bitmap, double-buffered exactly like the event ring.
 *
 * A single bitmap races: core 0 keeps writing Z80 RAM while core 1 is
 * sending, and clearing the map afterwards discards any bit set during
 * the send. Those bytes are already in the mirror, so they are never
 * marked again and never travel — silently dropped. That matters most
 * during a driver upload, where losing a handful of bytes out of 8 KB
 * leaves the slave running corrupt Z80 code that does nothing audible. */
static uint32_t zram_dirty_buf[2][LINK_ZRAM_BYTES / 32];
static volatile int zram_dirty_write;
uint32_t       *zram_frame_dirty;          /* the buffer being sent */
uint8_t        *zram_frame_data = zram_mirror;
static volatile bool zram_any[2];
uint32_t        zram_dirty_bytes;          /* cumulative, for diagnosis */

/* 68K writes to Z80 RAM are NOT streamed as events.
 *
 * A game uploading its sound driver writes the whole 8 KB in a single
 * frame — measured at 8476 writes against a 4096-event ring, which
 * truncated the upload and left the slave running a corrupt driver.
 * Byte events are simply the wrong shape for a bulk memcpy.
 *
 * Instead the writes accumulate in the mirror with a per-frame dirty
 * bitmap, and the frame exchange ships the bitmap plus the mirror as one
 * block. The slave applies only the marked bytes, so bytes its own Z80
 * wrote are left alone.
 *
 * Ordering against Z80 execution is safe: the 68K holds BUSREQ across
 * these writes (that is what the BUSREQ events are), so the Z80 is
 * halted while they happen and cannot observe the difference between
 * applying them per-write and applying them at frame start. */
#ifdef SOUND_CAPTURE
/* Tests the assumption above: a write made while the Z80 is NOT halted
 * IS observable, because the slave only applies the block at a frame
 * boundary while the master applies it at the cycle it happened. */
uint32_t link_zram_w_total, link_zram_w_unsafe;
uint32_t link_zram_w_unsafe_first = 0xFFFFFFFFu;
static int z80_bus_ack;   /* tentative decl; defined below */
#endif

void sound_zram_write(unsigned int offset, unsigned int value) {
    offset &= (LINK_ZRAM_BYTES - 1);

#ifdef SOUND_CAPTURE
    link_zram_w_total++;
    if (!z80_bus_ack) {
        extern uint32_t snd_cap_count;
        if (link_zram_w_unsafe == 0) link_zram_w_unsafe_first = snd_cap_count;
        link_zram_w_unsafe++;
    }
#endif

    /* The mirror answers the 68K's own reads, so it is updated either
     * way. */
    zram_mirror[offset] = (uint8_t)value;
    zram_dirty[offset >> 5] |= 1u << (offset & 31);

    /* Timestamped, like every other access through this seam. The block
     * path applies a whole frame's writes at once, before the slave
     * replays anything — so the slave's Z80 saw them from cycle 0 of the
     * frame while the master's only saw them at the cycle the 68K got
     * round to writing. Sending them in-stream removes that skew.
     *
     * The block stays as the fallback for the one case it was built
     * for: a driver upload is 8 KB of byte writes in a single frame and
     * would truncate the ring. Losing ordering on a bulk upload is
     * harmless — the Z80 is held in reset across it — whereas losing
     * the tail of the stream is not. */
    if (!emit(LINK_EV_ZRAM_WRITE, (uint16_t)offset, (uint8_t)value,
              m68k_cycles_master())) {
        zram_block_fallbacks++;          /* link down: nothing to do */
    }
    zram_dirty_bytes++;
}

/* How hard the 68K reads Z80 RAM, and where. If it polls a handful of
 * addresses hard, it is waiting on the Z80 — and every such read is
 * answered from a mirror that is up to a frame stale in both
 * directions, which is the remaining way this split can go wrong. */
uint32_t link_zram_reads;
#ifdef SOUND_CAPTURE
/* Frame index of the very first 68K read of Z80 RAM. The mirror those
 * reads come from is only refreshed once per frame, so if this lands on
 * the frame the two runs start to diverge, the staleness is the cause. */
uint32_t link_zram_first_frame = 0xFFFFFFFFu;
#endif
volatile bool zram_read_since_snapshot = true;   /* ask once at startup */
uint16_t link_zram_hot_addr;
uint32_t link_zram_hot_hits;

unsigned int sound_zram_read(unsigned int offset) {
    offset &= (LINK_ZRAM_BYTES - 1);

    /* Answer as of *now*, not from a frame-old mirror: ship what this
     * frame has emitted so far and read the byte back from the slave,
     * whose Z80 has then run exactly as far as the master's would have. */
    {
        uint8_t val = 0;
        if (link_flush_chunk((uint16_t)offset, &val)) {
            zram_mirror[offset] = val;
            link_zram_reads++;
            return val;
        }
    }
#ifdef SOUND_CAPTURE
    { extern uint32_t seam_frame; extern uint16_t seam_zram_reads[];
      if (seam_frame < 512) seam_zram_reads[seam_frame]++; }
#endif

#ifdef SOUND_CAPTURE
    if (link_zram_reads == 0) {
        extern uint32_t snd_cap_count;
        link_zram_first_frame = snd_cap_count;
    }
#endif
    link_zram_reads++;
    zram_read_since_snapshot = true;
    if (offset == link_zram_hot_addr) {
        link_zram_hot_hits++;
    } else if (link_zram_hot_hits == 0) {
        link_zram_hot_addr = (uint16_t)offset;
    } else {
        link_zram_hot_hits--;      /* Boyer-Moore majority vote */
    }

    return zram_mirror[offset];
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

/* AUDIO_FREQ_DIVISOR is 1009, and it must come from the real header.
 *
 * A local "#ifndef ... #define 60" fallback here compiled perfectly and
 * was silently wrong by a factor of ~17: the PSG rendered 14933 samples
 * a frame instead of 888 and sat pinned at its buffer clamp, and the
 * YM timer shadow would have ticked ~17x fast and reported nonsense
 * status. Neither failure points anywhere near a #define. */
#include "gwenesis_bus.h"

static struct {
    int32_t TA, TAL, TAC;
    int32_t TB, TBL, TBC;
    uint8_t mode;
    uint8_t status;
    int32_t clock;       /* master cycles already accounted for */
    uint16_t addr_latch; /* 9-bit: bank 1 is 0x100 | reg       */
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

    /* Mirror ym2612.c's latching exactly (YM2612Write, case 0 / case 2):
     * there is ONE address latch, port 0 stores `v` and port 2 stores
     * `v | 0x100`, and BOTH data ports write through whichever was set
     * last.
     *
     * Treating port 2 as another port-0 latch — which is what this did —
     * means a bank-1 address write followed by a data write lands on the
     * bank-0 timer registers instead of bank 1. The timer model then
     * drifts from the chip, the 68K polls a status byte that never
     * matches reality, and the game runs off into unmapped memory. */
    if (port == 0) { ym.addr_latch = (uint16_t)value; return; }
    if (port == 2) { ym.addr_latch = (uint16_t)value | 0x100u; return; }

    uint8_t v = (uint8_t)value;

    /* Only bank 0 carries the timers; 0x124.. are ordinary registers. */
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
    /* Still tracked so the audit can report on it, but no longer the
     * source of truth for reads. */
    ym_shadow_write(port, value, cycles);
    emit(LINK_EV_YM_WRITE, (uint16_t)(port & 3), (uint8_t)value, cycles);
}

/* Status byte as last reported by the real chip on the slave, plus a
 * count of how hard the 68K polls it. */
volatile uint8_t  link_ym_status_real;
uint32_t          link_ym_reads;

unsigned int sound_ym_read(int cycles) {
    (void)cycles;
    link_ym_reads++;

    /* Return what the real YM2612 last reported, not a local model.
     *
     * The shadow could never work: it only saw the 68K's writes, and
     * sound drivers program the timer registers (0x24-0x27) from the
     * Z80, whose writes happen entirely on the slave. Audited against
     * the chip it disagreed on 73% of frames — always reporting no
     * timer overflow, because it never saw a timer being started. The
     * 68K paces music on that bit.
     *
     * This is up to one frame stale. The timer flags latch until the
     * driver clears them via register 0x27, so a late "set" is normally
     * harmless; a 68K that spins on a transition *within* one frame
     * would need a real round trip, which link_ym_reads will tell us. */
    return link_ym_status_real;
}

/* The modelled value, kept only so the audit can keep reporting on it. */
uint8_t sound_ym_shadow_status(void) { return ym.status; }

void sound_psg_write(unsigned int value, int cycles) {
    emit(LINK_EV_PSG_WRITE, 0, (uint8_t)value, cycles);
}

/* BUSREQ and RESET are answered entirely from master-side state: on the
 * master today they only set and read these two flags, so the slave is
 * never consulted. That matters more than it looks — games poll BUSREQ
 * hard, and a round trip here would dominate the link. */
static int z80_bus_ack;
static int z80_reset_held;
static int last_run_until;

void sound_z80_ctrl_write(unsigned int address, unsigned int value) {
    /* Exact addresses, exactly as z80inst.c's z80_write_ctrl() decodes
     * them. This was `(address & 0x1F00) == 0x1200`, which looks
     * harmlessly more permissive and is not: a byte write anywhere in
     * 0xA112xx — odd-address halves of a word write, for instance —
     * became a reset pulse the real decoder ignores. The Z80 was reset
     * several times a frame, so it sat at PC=0 forever while every other
     * indicator (reset released, bus granted, zclk advancing a full
     * frame) said it was running normally. */
    /* Timestamp these properly. z80_write_ctrl() begins with z80_sync(),
     * which runs the Z80 up to m68k_cycles_master() *before* the bus
     * state changes — so the cycle count is part of the semantics, not
     * decoration. Emitting 0 made that sync a no-op on the slave: the
     * Z80 was at the wrong point whenever the 68K took or released the
     * bus, which shifts everything the sound driver does afterwards. */
    int now = m68k_cycles_master();

    if (address == 0x1100) {
        z80_bus_ack = value ? 1 : 0;
        emit(LINK_EV_BUSREQ, 0, (uint8_t)(value ? 1 : 0), now);
    } else if (address == 0x1200) {
        z80_reset_held = value ? 0 : 1;
        emit(LINK_EV_RESET_LINE, 0, (uint8_t)(value ? 1 : 0), now);
    }
}

unsigned int sound_z80_ctrl_read(unsigned int address) {
#ifdef SOUND_CAPTURE
    { extern uint32_t seam_frame; extern uint16_t seam_ctrl_reads[];
      if (seam_frame < 512) seam_ctrl_reads[seam_frame]++; }
#endif
    /* z80_read_ctrl() starts with z80_sync(), so on the master every 68K
     * poll of BUSREQ or RESET advances the Z80 to the current cycle.
     * Answering from a local flag without emitting anything left the
     * slave's Z80 running a different number of cycles per frame — the
     * differential trace showed the two diverging and re-converging
     * rather than drifting, which is the signature of a scheduling
     * difference rather than bad data.
     *
     * Emit the same time marker the master's sync would have produced.
     * Deduplicated against the last one, because games poll these
     * addresses in tight loops and an event per poll would swamp the
     * ring for no benefit — only an advance in time carries meaning. */
    int now = m68k_cycles_master();
    if (now > last_run_until) {
        last_run_until = now;
        emit(LINK_EV_RUN_UNTIL, 0, 0, now);
    }

    address &= 0xFFFF;
    if (address == 0x1100) return z80_bus_ack ? 0 : 1;
    if (address == 0x1101) return 0x00;
    if (address == 0x1200) return z80_reset_held;
    if (address == 0x1201) return 0x00;
    return 0xFF;
}

void sound_z80_irq(unsigned int level) {
    emit(LINK_EV_IRQ, 0, (uint8_t)(level ? 1 : 0), m68k_cycles_master());
}

void sound_z80_run(int target) {
    /* Pure time marker: it is what reproduces the master's Z80
     * scheduling on the slave. */
    if (target > last_run_until) last_run_until = target;
    emit(LINK_EV_RUN_UNTIL, 0, 0, target);
}

/* =====================================================================
 * Frame boundary
 * ===================================================================== */

/* Remembered so the link can re-prime a slave that rejoined.
 *
 * The slave loses its ROM and all chip state whenever it reboots — a
 * watchdog, a reflash, or a power glitch. Uploading only at ROM-load
 * time meant such a slave stayed silent until the user manually
 * reloaded the game, with nothing on screen to say why. Re-uploading on
 * reconnect costs ~0.3 s of audio once, and turns a dead-until-reloaded
 * slave into one that heals itself. */
static void ym_shadow_reset(void);

static const uint8_t   *rom_ptr;
static uint32_t         rom_len;
static link_sound_config_t rom_cfg;

void sound_link_set_rom(const uint8_t *rom, uint32_t bytes,
                        const link_sound_config_t *cfg) {
    rom_ptr = rom;
    rom_len = bytes;
    rom_cfg = *cfg;
}

/* Filled by the exchange, consumed by main.c's audio path. */
int16_t  link_ym_samples_buf[LINK_MAX_SAMPLES];
int16_t  link_sn_samples_buf[LINK_MAX_SAMPLES];
volatile uint32_t link_ym_sample_count;
volatile uint32_t link_sn_sample_count;

uint32_t link_audio_frames, link_audio_silent, link_audio_clipped;
uint32_t link_audio_shortframes;
uint32_t link_xchg_max_us, link_xchg_over8ms, link_xchg_over16ms;
uint64_t link_xchg_total_us;
uint16_t link_audio_last_peak;

static volatile int      pending_buffer = -1;   /* buffer awaiting send */
static volatile uint32_t pending_target;

static volatile bool     pending_zram;
static uint32_t          frame_seq;

/* Coprocessors this build depends on: CP0 is the GPIO coprocessor that
 * gpio_put() compiles to on RP2350, CP10/CP11 are the VFP that GCC uses
 * for 64-bit maths. Halting the core with a debugger clears CPACR, and
 * the next such instruction then takes a UsageFault (CFSR NOCP) that
 * escalates to a HardFault — so attaching a debugger to a running
 * emulator would kill it. Re-asserting once per frame costs a compare
 * and keeps the master inspectable. The slave does the same in its
 * serve loop. */
#define CPACR_NEEDED 0x00F00303u

static inline void cpacr_ensure(void) {
    volatile uint32_t *cpacr = (volatile uint32_t *)0xE000ED88u;
    if ((*cpacr & CPACR_NEEDED) != CPACR_NEEDED) {
        *cpacr |= CPACR_NEEDED;
        __asm volatile ("dsb; isb" ::: "memory");
    }
}

void sound_frame_end(int audio_target_clock) {
    cpacr_ensure();

    /* The YM timer shadow runs on the same per-frame cycle base as the
     * chips: main.c zeroes system_clock, zclk, ym2612_clock and
     * sn76489_clock at the top of every frame, so m68k_cycles_master()
     * counts 0..896040 within a frame and then restarts.
     *
     * Leaving ym.clock at the end of the previous frame makes
     * ym_shadow_run() take its `target <= ym.clock` early return for the
     * whole of the next frame, and every frame after — the timer flags
     * freeze at whatever they were. A 68K polling YM status for timer
     * overflow then spins or branches wrongly, which is how a stuck
     * title card and a run-away PC come out of a sound-only change.
     *
     * Exactly the bug that stopped the slave's Z80 after one frame, in
     * the one other place a cycle count is accumulated across frames. */
    ym.clock = 0;
    last_run_until = 0;
    zram_ev_frame  = 0;
    zram_apply_marked = false;

    /* Hand the finished buffer to core 1 and start filling the other.
     * If core 1 has not drained the previous one yet the emulator is
     * outrunning the slave; dropping this frame's events would
     * desynchronise the chips, so we wait — the same backpressure
     * main.c already applies when core 1 is behind. */
    while (pending_buffer >= 0) tight_loop_contents();

    int zw = zram_dirty_write;

    pending_target   = (uint32_t)audio_target_clock;
    pending_frame_no = core0_frame++;
    pending_zram     = zram_any[zw];
    zram_frame_dirty = zram_dirty_buf[zw];
    link_lock();                 /* keep a push from straddling the flip */
    pending_sent     = event_sent[event_write];
    pending_buffer   = event_write;
    __dmb();

    /* Flip both rings together so core 0 never writes into what core 1
     * is about to transmit. */
    event_write = 1 - event_write;
    event_count[event_write] = 0;
    event_sent[event_write]  = 0;
    link_unlock();

    zram_dirty_write = 1 - zw;
    memset(zram_dirty_buf[1 - zw], 0, sizeof(zram_dirty_buf[0]));
    zram_any[1 - zw] = false;
}

/* Called from core 1. Ships the pending frame and collects the slave's
 * reply. Returns false if the exchange failed, in which case the caller
 * outputs silence for this frame rather than stale samples. */
bool sound_link_exchange(void) {
    int buf = pending_buffer;
    if (buf < 0) return false;

    /* While the link is down, probe occasionally so a slave that booted
     * late, was reflashed, or rebooted on its own rejoins without the
     * user having to reset the master. Once a second is often enough to
     * feel immediate and rare enough that a genuinely absent slave costs
     * almost nothing.
     *
     * A slave that rejoins mid-game has no ROM and no chip state, so it
     * cannot simply resume: the caller re-uploads at the next ROM load.
     * Until then the exchange keeps failing and audio stays silent,
     * which is the honest outcome rather than replaying stale samples. */
    if (!link_master_connected()) {
        /* This runs on core 1, and core 0 spins on audio_done waiting for
         * it (main.c:1026). Anything slow here freezes the emulator, so
         * the recovery path must stay cheap: a 2 ms probe, and the
         * expensive 2 MB re-upload only after one actually succeeds,
         * with a wall-clock cooldown so a slave that is answering but
         * failing cannot stall the game several seconds out of every
         * few. Time-based, not frame-based: frames stop advancing while
         * core 0 is blocked, which would defeat a frame counter. */
        static uint64_t retry_after_us;
        uint64_t now = time_us_64();

        if (now >= retry_after_us) {
            retry_after_us = now + 1000000;      /* 1 s between probes */
            /* 50 ms, not 2 ms. The slave may be partway through a
             * doorbell wait left over from an exchange the master
             * abandoned, and will not return to its serve loop for tens
             * of milliseconds. Probing faster than that can never
             * resynchronise the pair — the master gives up before the
             * slave is listening again. Bounded by the 1 s cooldown, so
             * a genuinely absent slave costs 5% of core 1, not a stall. */
            if (link_master_probe(50000, NULL) && rom_ptr && rom_len) {
                retry_after_us = now + 5000000;  /* 5 s if re-priming */
                /* A slave that just rejoined has no ROM and no chip
                 * state, so it cannot simply resume mid-stream. Prime it
                 * exactly as a fresh ROM load would. */
                LOG("Link: slave back — re-uploading %lu KB ROM\n",
                    (unsigned long)(rom_len >> 10));
                if (link_master_upload_rom(rom_ptr, rom_len) &&
                    link_master_send_config(&rom_cfg)) {
                    retry_after_us = 0;
                    ym_shadow_reset();
                    memset(zram_dirty, 0, sizeof(zram_dirty));
                    memset(zram_dirty_buf[zram_dirty_write], 0xFF,
                           sizeof(zram_dirty_buf[0]));
                    zram_any[zram_dirty_write] = true;  /* resend all of Z80 RAM */
                    LOG("Link: slave re-primed\n");
                }
            }
        }

        pending_buffer = -1;
        frame_in_flight = false;
        mirror_gen = core0_frame;      /* release readers; link is down */
        link_ym_sample_count = 0;
        link_sn_sample_count = 0;
        return false;
    }

    uint64_t t_start = time_us_64();

    link_lock();

    /* Collect the frame sent last time before shipping this one: the
     * slave has had a whole frame of master emulation to produce it, so
     * this normally returns immediately instead of blocking core 1 for
     * the slave's compute. */
    bool ok = true;
    if (frame_in_flight) {
        ok = link_master_frame_collect(link_ym_samples_buf, link_sn_samples_buf,
                                       (uint32_t *)&link_ym_sample_count,
                                       (uint32_t *)&link_sn_sample_count,
                                       zram_merge);
        frame_in_flight = false;
        if (ok) { __dmb(); mirror_gen = inflight_frame_no; }
    } else {
        link_ym_sample_count = 0;
        link_sn_sample_count = 0;
        ok = false;              /* first frame: nothing to play yet */
    }

    if (link_master_connected() &&
        link_master_frame_send(&events[buf][pending_sent],
                               event_count[buf] - pending_sent,
                               pending_zram, (int)pending_target, ++frame_seq)) {
        frame_in_flight = true;
        inflight_frame_no = pending_frame_no;
    }

#if !LINK_PIPELINE
    /* Collect immediately: the mirror then reflects the frame just sent
     * rather than the one before it. Costs the slave's compute inline. */
    if (frame_in_flight) {
        ok = link_master_frame_collect(link_ym_samples_buf, link_sn_samples_buf,
                                       (uint32_t *)&link_ym_sample_count,
                                       (uint32_t *)&link_sn_sample_count,
                                       zram_merge);
        frame_in_flight = false;
        if (ok) { __dmb(); mirror_gen = inflight_frame_no; }
    }
#endif

    link_unlock();

    {
        uint32_t dt = (uint32_t)(time_us_64() - t_start);
        link_xchg_total_us += dt;
        if (dt > link_xchg_max_us) link_xchg_max_us = dt;
        if (dt > 8000)  link_xchg_over8ms++;
        if (dt > 16000) link_xchg_over16ms++;
    }

    /* Characterise the audio actually arriving, per frame. If the break
     * is in the samples rather than the transport, it shows up here as
     * silent or clipped frames — measurable without a capture device. */
    if (ok) {
        uint32_t n = link_ym_sample_count;
        if (n > LINK_MAX_SAMPLES) n = LINK_MAX_SAMPLES;
        int32_t peak = 0;
        for (uint32_t i = 0; i < n; i++) {
            int32_t v = link_ym_samples_buf[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        uint32_t sn = link_sn_sample_count;
        if (sn > LINK_MAX_SAMPLES) sn = LINK_MAX_SAMPLES;
        int32_t speak = 0;
        for (uint32_t i = 0; i < sn; i++) {
            int32_t v = link_sn_samples_buf[i];
            if (v < 0) v = -v;
            if (v > speak) speak = v;
        }

        link_audio_frames++;
        if (peak < 32 && speak < 32)        link_audio_silent++;
        if (peak > 30000 || speak > 30000)  link_audio_clipped++;
        if (n != 888 || sn != 888)          link_audio_shortframes++;
        link_audio_last_peak = (uint16_t)peak;
    }

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
    memset(zram_dirty_buf, 0, sizeof(zram_dirty_buf));
    zram_any[0] = zram_any[1] = false;
    zram_dirty_write = 0;
    zram_frame_dirty = zram_dirty_buf[0];
    zram_dirty_bytes = 0;

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
