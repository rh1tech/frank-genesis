/*
 * frank-genesis — C2 slave: sound subsystem
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replays the master's event stream and renders the frame's audio.
 *
 * The whole correctness argument rests on one property: the master
 * emits events in the order it performed them, each stamped with
 * m68k_cycles_master() at the moment of the access, and this loop
 * applies them in that same order with slave_replay_clock set to the
 * stamp. The chips therefore see the same writes at the same emulated
 * times as they would have inline, and running a frame late changes
 * nothing they can observe.
 *
 * In particular the RUN_UNTIL markers reproduce the master's Z80
 * scheduling exactly: it pumps z80_run() every Z80_SLICE_LINES
 * scanlines, so the Z80 is routinely *ahead* of the 68K's current cycle
 * when a 68K write lands. Replaying in program order preserves that
 * interleaving rather than trying to re-derive it.
 */

#include "slave_sound.h"
#include <stdbool.h>

volatile uint32_t slave_trace_frame = 0xFFFFFFFFu;  /* poke over SWD to arm */
uint32_t slave_trace_pc[128][2];
uint32_t slave_trace_n;

extern bool            slave_zram_pending;
extern const uint32_t *slave_zram_bitmap_p;
extern const uint8_t  *slave_zram_block_p;

#include <stdbool.h>
#include <string.h>

#include "gwenesis_sn76489.h"
#include "ym2612.h"
#include "z80inst.h"

/* Owned by gwenesis_bus.c on the master; the slave owns it here. The
 * Z80 assembly reaches it through the Z80_RAM pointer. */
#define SLAVE_ZRAM_SIZE 0x2000
static unsigned char zram[SLAVE_ZRAM_SIZE];

/* Mute flags that live inside ym2612.c. */
extern bool ym2612_fm_enabled;
extern bool ym2612_dac_enabled;
extern bool ym2612_channel_enabled[6];
extern bool z80_enabled;

/* AUDIO_FREQ_DIVISOR is 1009, and it must come from the real header.
 *
 * A local "#ifndef ... #define 60" fallback here compiled perfectly and
 * was silently wrong by a factor of ~17: the PSG rendered 14933 samples
 * a frame instead of 888 and sat pinned at its buffer clamp, and the
 * YM timer shadow would have ticked ~17x fast and reported nonsense
 * status. Neither failure points anywhere near a #define. */
#include "gwenesis_bus.h"

/* Where the chips are actually driven from, so "is the driver idle?" and
 * "is the chip being written but silent?" can be told apart. */
uint32_t n_ym_writes, n_psg_writes, n_zram_applied, n_zram_bytes_applied;

/* Differential-test trace: one Z80 state signature per emulated frame. */
#define SLAVE_SIG_SLOTS 600
uint32_t slave_sig[SLAVE_SIG_SLOTS];
uint32_t slave_sig_count;
uint32_t slave_last_sig;
uint32_t slave_last_regs[6];

const uint8_t *slave_zram(void) {
    return zram;
}

/* Apply the master's 68K writes to Z80 RAM. Only bytes marked in the
 * bitmap are taken, so bytes this slave's own Z80 wrote are preserved —
 * the master's mirror is stale for those. */
/* The authoritative Z80 RAM byte, as of everything replayed so far. */
uint32_t slave_zram_peek(unsigned int offset) {
    return zram[offset & (SLAVE_ZRAM_SIZE - 1)];
}

void slave_zram_apply(const uint32_t *bitmap, const uint8_t *data) {
    n_zram_applied++;
    for (uint32_t w = 0; w < SLAVE_ZRAM_SIZE / 32; w++) {
        uint32_t dirty = bitmap[w];
        if (!dirty) continue;

        uint32_t base = w * 32;
        for (uint32_t b = 0; b < 32; b++) {
            if (dirty & (1u << b)) {
                zram[base + b] = data[base + b];
                n_zram_bytes_applied++;
            }
        }
    }
}

void slave_sound_init(void) {
    memset(zram, 0, sizeof(zram));

    z80_set_memory(zram);
    z80_start();

    YM2612Init();
    YM2612ResetChip();
    YM2612Config(YM2612_DISCRETE);

    /* Same arguments as the master's main.c, so both halves agree on
     * the PSG's clock and the samples-per-second it renders at. */
    gwenesis_SN76489_Init(3579545, 888 * 60, AUDIO_FREQ_DIVISOR, PSG_INTEGRATED);
    gwenesis_SN76489_Reset();

    slave_replay_clock = 0;
}

void slave_sound_reset(void) {
    memset(zram, 0, sizeof(zram));

    /* z80_start(), not z80_pulse_reset().
     *
     * The master reaches a new game through gwenesis_bus_init(), which
     * calls z80_start() — and that clears reset_once, bus_ack and zclk
     * and sets reset=1 as well as resetting the registers.
     * z80_pulse_reset() resets only the registers, so the slave carried
     * the previous game's control state across a ROM load: its Z80 could
     * already be released and executing while the master's was still
     * held in reset waiting for the 68K to release it. The two then run
     * different amounts of code from the very first frame. */
    z80_start();
    YM2612ResetChip();
    gwenesis_SN76489_Reset();

    slave_replay_clock  = 0;
    sn76489_clock = sn76489_index = 0;
    ym2612_clock  = ym2612_index  = 0;

    slave_foreign_reads  = 0;
    slave_foreign_writes = 0;
}

void slave_sound_config(const link_sound_config_t *cfg) {
    z80_enabled        = cfg->z80_enabled ? true : false;
    ym2612_fm_enabled  = cfg->fm_enabled ? true : false;
    ym2612_dac_enabled = cfg->dac_enabled ? true : false;

    for (int i = 0; i < 6; i++) {
        ym2612_channel_enabled[i] = (cfg->channel_mask >> i) & 1;
    }
}

/* Opened by whichever message reaches the slave first for this frame. */
static bool frame_open;
static uint32_t last_cycles;

static void slave_frame_begin(void) {
    if (frame_open) return;
    frame_open    = true;
    last_cycles   = 0;
    zclk          = 0;
    sn76489_clock = 0;
    sn76489_index = 0;
    ym2612_clock  = 0;
    ym2612_index  = 0;
}

static void slave_replay(const link_event_t *events, uint32_t count) {

    for (uint32_t i = 0; i < count; i++) {
        const link_event_t *e = &events[i];

        slave_replay_clock = (int)e->cycles;
        last_cycles = e->cycles;

        switch (e->type) {
        case LINK_EV_RUN_UNTIL:
            z80_run((int)e->cycles);
            if (slave_sig_count == slave_trace_frame && slave_trace_n < 128) {
                extern void z80_state_regs(uint32_t out[6]);
                uint32_t rr[6];
                z80_state_regs(rr);
                slave_trace_pc[slave_trace_n][0] = rr[0] & 0xFFFFu;
                slave_trace_pc[slave_trace_n][1] = rr[5];
                slave_trace_n++;
            }
            break;

        case LINK_EV_YM_WRITE:
            YM2612Write(e->addr & 3, e->val, (int)e->cycles);
            break;

        case LINK_EV_PSG_WRITE:
            gwenesis_SN76489_Write(e->val, (int)e->cycles);
            break;

        case LINK_EV_ZRAM_WRITE:
            zram[e->addr & (SLAVE_ZRAM_SIZE - 1)] = e->val;
            break;

        case LINK_EV_BUSREQ:
            /* 0xA11100. z80_write_ctrl syncs the Z80 first, exactly as
             * on the master. */
            z80_write_ctrl(0x1100, e->val);
            break;

        case LINK_EV_RESET_LINE:
            z80_write_ctrl(0x1200, e->val);
            break;

        case LINK_EV_IRQ:
            z80_irq_line(e->val);
            break;

        case LINK_EV_ZRAM_APPLY:
            /* The master spilled part of this frame's Z80 RAM writes to
             * the bulk block; this is the cycle they happened at. */
            if (slave_zram_pending) {
                slave_zram_apply(slave_zram_bitmap_p, slave_zram_block_p);
                slave_zram_pending = false;
            }
            break;

        case LINK_EV_BANK_WRITE:
            /* The 68K bus path for the bank register is a no-op on the
             * master too — only the Z80 itself writes it, through its
             * own 0x6000 mapping. Kept for completeness. */
            break;

        default:
            break;
        }
    }

}

void slave_sound_chunk(const link_event_t *events, uint32_t count) {
    slave_frame_begin();
    slave_replay(events, count);
}

void slave_sound_run_frame(const link_event_t *events, uint32_t count,
                           int audio_target_clock,
                           link_frame_reply_t *reply) {
    /* The master resets all of these at the top of every frame
     * (main.c:846-851), and the slave must do exactly the same.
     *
     * zclk is the one that matters most and is the easiest to miss. The
     * master restarts its cycle count from zero each frame, so every
     * RUN_UNTIL target is a small number. Leaving zclk at the end of the
     * previous frame (~896040) makes z80_run() take its `zclk >= target`
     * early return for every subsequent frame: the Z80 executes for
     * exactly one frame and then never again. The chips stay powered and
     * keep rendering, so the symptom is not silence but a constant DC
     * level out of the FM — which looks like a mixing bug, not a stopped
     * CPU. */
    slave_frame_begin();
    slave_replay(events, count);

    /* A block with no marker should not happen, but applying it late
     * beats dropping it. */
    if (slave_zram_pending) {
        slave_zram_apply(slave_zram_bitmap_p, slave_zram_block_p);
        slave_zram_pending = false;
    }

    {
        extern uint32_t z80_state_signature(void);
        extern void z80_state_regs(uint32_t out[6]);
        z80_state_regs(slave_last_regs);
        slave_last_sig = z80_state_signature();
        if (slave_sig_count < SLAVE_SIG_SLOTS) {
            slave_sig[slave_sig_count] = slave_last_sig;
        }
        slave_sig_count++;
    }

    /* Close the frame: render whatever the chips still owe. Matches the
     * master's sound_frame_end(). */
    gwenesis_SN76489_run(audio_target_clock);
    ym2612_run(audio_target_clock);

    /* The master models this byte locally to avoid a round trip on every
     * 68K status poll; report the truth so it can be audited. */
    reply->ym_status      = YM2612Read((int)audio_target_clock) & 0xFF;
    reply->z80_sig        = slave_last_sig;
    for (int i = 0; i < 6; i++) reply->z80_dbg[i] = slave_last_regs[i];
    reply->ym_samples     = (uint32_t)ym2612_index;
    reply->sn_samples     = (uint32_t)sn76489_index;
    reply->z80_cycles     = last_cycles;
    frame_open = false;                 /* next message opens a new frame */
    {
        extern uint32_t slave_foreign_last, slave_foreign_min, slave_foreign_max;
        reply->foreign_last_addr = slave_foreign_last;
        reply->foreign_min_addr  = slave_foreign_min;
        reply->foreign_max_addr  = slave_foreign_max;
    }
    reply->foreign_reads  = slave_foreign_reads;
    reply->foreign_writes = slave_foreign_writes;

    if (reply->ym_samples > SLAVE_AUDIO_BUFFER_SIZE)
        reply->ym_samples = SLAVE_AUDIO_BUFFER_SIZE;
    if (reply->sn_samples > SLAVE_AUDIO_BUFFER_SIZE)
        reply->sn_samples = SLAVE_AUDIO_BUFFER_SIZE;
}
