/*
 * frank-genesis — C2 master side of the inter-processor sound link
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The master owns the 68K, the VDP, video, SD, input and the I2S DAC.
 * The slave owns the Z80, the YM2612, the SN76489 and the mixer. This
 * module is the master's half of that conversation.
 *
 * Everything here is C2-only. On M1/M2 the file is not compiled and the
 * sound backend calls the chips directly, exactly as it always has.
 * See docs/C2_SOUND_SPLIT.md.
 */
#ifndef LINK_MASTER_H
#define LINK_MASTER_H

#include <stdbool.h>
#include <stdint.h>

#include "link_proto.h"

/* Claim PIO2 and two DMA channels, configure the link pins. Safe to
 * call once, early — it does not need the slave to be alive. */
void link_master_init(void);

/* Exchange HELLO/HELLO_ACK. Returns true if the slave answered, and
 * fills `info` when non-NULL. A false return is not fatal: the caller
 * falls back to running the sound chips locally. */
bool link_master_probe(uint32_t timeout_us, link_node_info_t *info);

/* True once a probe has succeeded and no exchange has failed since. */
bool link_master_connected(void);

/* Slave identity from the last successful probe (false if never probed). */
bool link_master_last_info(link_node_info_t *out);

/* Measured wire rate in bytes per second, 0 if the link is not up. */
uint32_t link_master_byte_rate(void);

/* Round-trip latency probe, for diagnostics. Returns false if the slave
 * did not answer; `rtt_us` is filled on success. */
bool link_master_ping(uint32_t *rtt_us);

/* Ask a wedged slave to reboot by pulsing FS. The slave samples FS from
 * a timer interrupt rather than its main loop, so this still works when
 * its foreground is stuck — which is exactly when it is worth having. */
void link_master_request_slave_reset(void);

/* Reset the slave's Z80 and sound chips. */
bool link_master_reset_sound(void);

/* Mirror the master's settings so both halves agree about muting. */
bool link_master_send_config(const link_sound_config_t *cfg);

/* Upload a ROM image into the slave's PSRAM. The Z80 reads the 68K
 * address space through its bank register, so the slave needs the whole
 * image, not just the sound driver. Returns false if the slave's CRC of
 * what it stored disagrees with ours — a corrupted upload is far easier
 * to diagnose here than as inexplicably wrong music later. */
bool link_master_upload_rom(const uint8_t *rom, uint32_t bytes);

/* One frame's exchange, split so the slave's compute overlaps the
 * master's emulation instead of blocking core 1 on it.
 *
 * _send ships `count` events and returns as soon as they are on the
 * wire. _collect takes the reply for the frame most recently sent:
 * audio, and the Z80 RAM snapshot handed to `zram_merge` so the caller
 * can merge it under its own newer writes. Call _collect for frame N at
 * the top of frame N+1, by which time the slave is already waiting to
 * hand it over. */
/* Mid-frame synchronisation: replay `count` events on the slave now and
 * return Z80 RAM byte `offset` as of that point. */
bool link_master_sync_peek(const link_event_t *events, uint32_t count,
                           uint16_t offset, uint8_t *out);

bool link_master_frame_send(const link_event_t *events, uint32_t count,
                            bool zram_dirty, int audio_target, uint32_t seq);
bool link_master_frame_collect(int16_t *ym_out, int16_t *sn_out,
                               uint32_t *ym_count, uint32_t *sn_count,
                               void (*zram_merge)(const uint8_t *snapshot));

#endif /* LINK_MASTER_H */
