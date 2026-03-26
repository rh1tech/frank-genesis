/*
**
** software implementation of Yamaha FM sound generator (YM2612/YM3438)
**
** Original code (MAME fm.c)
**
** Copyright (C) 2001, 2002, 2003 Jarek Burczynski (bujar at mame dot net)
** Copyright (C) 1998 Tatsuyuki Satoh , MultiArcadeMachineEmulator development
**
** Version 1.4 (final beta)
**
** Additional code & fixes by Eke-Eke for Genesis Plus GX
** Ported to frank-genesis with Genesis-Plus-GX improvements
**
*/

#ifndef _H_YM2612_
#define _H_YM2612_

#include <stdbool.h>

/* Genesis-Plus-GX chip types */
enum {
  YM2612_DISCRETE = 0,   /* Discrete chip with 9-bit DAC and ladder effect */
  YM2612_INTEGRATED,     /* Integrated ASIC with 9-bit DAC, no ladder effect */
  YM2612_ENHANCED        /* Enhanced mode with 14-bit DAC (full precision) */
};

extern int16_t *gwenesis_ym2612_buffer;
extern volatile int ym2612_index;
extern volatile int ym2612_clock;

/* External control flags for muting FM vs DAC */
extern bool ym2612_fm_enabled;   /* Mute FM channels 1-6 when false */
extern bool ym2612_dac_enabled;  /* Mute DAC output when false */

/* Per-channel mute flags (channels 0-5 = FM channels 1-6) */
extern bool ym2612_channel_enabled[6];

extern void YM2612Init(void);
extern void YM2612Config(unsigned char type);  /* Genesis-Plus-GX: chip type instead of dac_bits */
extern void YM2612ResetChip(void);
extern void YM2612Write(unsigned int a, unsigned int v, int target);
extern void ym2612_run(int target);
extern unsigned int YM2612Read(int target);

void gwenesis_ym2612_save_state();
void gwenesis_ym2612_load_state();

#endif /* _YM2612_ */
