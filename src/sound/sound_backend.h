/*
 * frank-genesis — sound backend seam
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every path by which the rest of the emulator reaches the sound
 * subsystem — the Z80, the YM2612, the SN76489 and Z80 RAM — goes
 * through this header.
 *
 * On M1 and M2 these are `static inline` wrappers straight onto the
 * chip functions, so the generated code is identical to calling them
 * directly. The seam costs those boards nothing; the M2 binary is
 * byte-for-byte the same before and after it was introduced.
 *
 * On C2 the whole subsystem lives on the second RP2350 and these become
 * real calls into the link backend, which turns each one into a
 * timestamped event. See docs/C2_SOUND_SPLIT.md.
 *
 * The timestamp arguments are not decoration: they are what makes
 * replaying the stream a frame later equivalent to running the chips
 * inline. Never drop one, and never pass a value that is not
 * m68k_cycles_master() at the moment of the access.
 */
#ifndef SOUND_BACKEND_H
#define SOUND_BACKEND_H

/* C2 normally offloads sound to the slave. Building with
 * -DC2_LOCAL_SOUND=1 keeps the chips on the master instead, so the same
 * board, DAC and ROM can produce a reference recording to compare the
 * offloaded path against. Not a shipping configuration — a measurement
 * tool. */
#if defined(BOARD_C2) && !defined(C2_LOCAL_SOUND)

/* ---- C2: the sound subsystem is on the slave ---- */

void         sound_ym_write(unsigned int port, unsigned int value, int cycles);
unsigned int sound_ym_read(int cycles);
void         sound_psg_write(unsigned int value, int cycles);

void         sound_zram_write(unsigned int offset, unsigned int value);
unsigned int sound_zram_read(unsigned int offset);

void         sound_z80_ctrl_write(unsigned int address, unsigned int value);
unsigned int sound_z80_ctrl_read(unsigned int address);
void         sound_z80_irq(unsigned int level);

/* Time markers. `sound_z80_run` says "the Z80 may advance to this
 * cycle"; `sound_frame_end` closes the frame and is where the master
 * exchanges the event stream for the previous frame's samples. */
void         sound_z80_run(int target);
void         sound_frame_end(int audio_target_clock);

#else /* M1, M2, or C2 with C2_LOCAL_SOUND: run the chips inline */

#include "gwenesis_sn76489.h"
#include "ym2612.h"
#include "z80inst.h"

extern unsigned char ZRAM[];

static inline void sound_ym_write(unsigned int port, unsigned int value, int cycles) {
    YM2612Write(port, value, cycles);
}

static inline unsigned int sound_ym_read(int cycles) {
    return YM2612Read(cycles);
}

static inline void sound_psg_write(unsigned int value, int cycles) {
    gwenesis_SN76489_Write(value, cycles);
}

#if defined(SOUND_CAPTURE) && defined(C2_LOCAL_SOUND)
/* Staleness probe. The offloaded build answers a 68K read of Z80 RAM
 * from a mirror that is only refreshed once per frame; here the read is
 * live. zram_stale_snap models that mirror (previous frame's contents,
 * plus the master's own writes since), so comparing it against the live
 * byte says whether the mirror would have lied, and on which frame it
 * first would have. */
#include <stdint.h>
extern unsigned char zram_stale_snap[];
extern uint32_t zram_stale_reads, zram_stale_diffs, zram_stale_first_frame;
extern uint32_t snd_cap_count;

static inline void sound_zram_write(unsigned int offset, unsigned int value) {
    ZRAM[offset] = value;
    /* The real mirror merges the master's own writes on top of the
     * slave's snapshot, so the models must too or they overstate how
     * stale the mirror is. */
    { extern unsigned char zram_snap1[], zram_snap2[];
      zram_stale_snap[offset & 0x1FFF] = (unsigned char)value;
      zram_snap1[offset & 0x1FFF] = (unsigned char)value;
      zram_snap2[offset & 0x1FFF] = (unsigned char)value; }
}

static inline unsigned int sound_zram_read(unsigned int offset) {
#ifdef SOUND_CAPTURE
    { extern uint32_t seam_frame; extern uint16_t seam_zram_reads[];
      if (seam_frame < 512) seam_zram_reads[seam_frame]++; }
#endif
    unsigned int live = ZRAM[offset];
    zram_stale_reads++;
    {
        extern unsigned char zram_snap1[], zram_snap2[];
        extern uint32_t stale1_first, stale2_first, stale1_hits, stale2_hits, stale_reads;
        extern uint32_t seam_frame;
        stale_reads++;
        if (zram_snap1[offset & 0x1FFF] != (unsigned char)live) {
            if (stale1_hits == 0) stale1_first = seam_frame;
            stale1_hits++;
        }
        if (zram_snap2[offset & 0x1FFF] != (unsigned char)live) {
            if (stale2_hits == 0) stale2_first = seam_frame;
            stale2_hits++;
        }
    }
    if (zram_stale_snap[offset & 0x1FFF] != (unsigned char)live) {
        if (zram_stale_diffs == 0) zram_stale_first_frame = snd_cap_count;
        zram_stale_diffs++;
    }
    return live;
}
#else
static inline void sound_zram_write(unsigned int offset, unsigned int value) {
    ZRAM[offset] = value;
}

static inline unsigned int sound_zram_read(unsigned int offset) {
    return ZRAM[offset];
}
#endif

static inline void sound_z80_ctrl_write(unsigned int address, unsigned int value) {
    z80_write_ctrl(address, value);
}

static inline unsigned int sound_z80_ctrl_read(unsigned int address) {
#ifdef SOUND_CAPTURE
    { extern uint32_t seam_frame; extern uint16_t seam_ctrl_reads[];
      if (seam_frame < 512) seam_ctrl_reads[seam_frame]++; }
#endif
    unsigned int v = z80_read_ctrl(address);   /* syncs the Z80 first */
#if defined(SOUND_CAPTURE) && defined(C2_LOCAL_SOUND)
    { extern void snd_trace_run(void); snd_trace_run(); }
#endif
    return v;
}

static inline void sound_z80_irq(unsigned int level) {
    z80_irq_line(level);
}

static inline void sound_z80_run(int target) {
    z80_run(target);
#if defined(SOUND_CAPTURE) && defined(C2_LOCAL_SOUND)
    { extern void snd_trace_run(void); snd_trace_run(); }
#endif
}

static inline void sound_frame_end(int audio_target_clock) {
    gwenesis_SN76489_run(audio_target_clock);
    ym2612_run(audio_target_clock);
}

#endif /* BOARD_C2 */

#endif /* SOUND_BACKEND_H */
