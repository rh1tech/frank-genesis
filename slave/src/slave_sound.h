/*
 * frank-genesis — C2 slave: sound subsystem
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SLAVE_SOUND_H
#define SLAVE_SOUND_H

#include <stdint.h>

#include "link_proto.h"

/* Matches AUDIO_BUFFER_SIZE in the master's main.c. The chips render
 * into buffers of this size; only the first `*_samples` entries of each
 * are sent back. */
#define SLAVE_AUDIO_BUFFER_SIZE 2048

/* Set by the replay loop to the timestamp of the event being applied.
 * m68k_cycles_master() returns it, which is how the sound cores catch
 * their synthesis up to the right emulated moment. */
extern int slave_replay_clock;

/* Accesses into 68K address space the slave cannot serve, reported to
 * the master so a game that depends on them is diagnosable. */
extern uint32_t slave_foreign_reads;
extern uint32_t slave_foreign_writes;

extern int16_t slave_sn76489_buffer_mem[SLAVE_AUDIO_BUFFER_SIZE];
extern int16_t slave_ym2612_buffer_mem[SLAVE_AUDIO_BUFFER_SIZE];

/* Point the Z80's banked reads at the ROM image in PSRAM. */
void slave_rom_set(unsigned char *base, uint32_t bytes);

/* Bring up the Z80, the YM2612 and the SN76489. */
void slave_sound_init(void);

/* Full reset, as on a console reset or a new ROM. */
void slave_sound_reset(void);

/* Mirror the master's settings so both halves agree about muting. */
void slave_sound_config(const link_sound_config_t *cfg);

/* Replay one frame's events, then render the frame's audio.
 * Fills reply->ym_samples / sn_samples; the buffers themselves are
 * slave_ym2612_buffer_mem and slave_sn76489_buffer_mem. */
/* Replay a mid-frame chunk. Starts the frame if this is its first
 * communication, so the per-frame clock reset happens exactly once
 * whether or not the master needed to synchronise early. */
uint32_t slave_zram_peek(unsigned int offset);

void slave_sound_chunk(const link_event_t *events, uint32_t count);

void slave_sound_run_frame(const link_event_t *events, uint32_t count,
                           int audio_target_clock,
                           link_frame_reply_t *reply);

/* The live Z80 RAM, for the per-frame snapshot the master reads back. */
const uint8_t *slave_zram(void);

/* Apply the master's dirty-masked 68K writes to Z80 RAM. */
void slave_zram_apply(const uint32_t *bitmap, const uint8_t *data);

#endif /* SLAVE_SOUND_H */
