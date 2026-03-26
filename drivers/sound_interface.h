/*
 * frank-genesis - Sound Chip Write Interface
 *
 * This module provides a unified interface for sound chip writes that can
 * either directly update the chips (legacy mode) or route through the
 * autonomous audio command queue (real-time mode).
 *
 * When AUDIO_USE_REALTIME=1:
 *   - Writes are queued for Core 1 processing
 *   - Sound chips run on Core 1 with independent timing
 *
 * When AUDIO_USE_REALTIME=0:
 *   - Writes go directly to sound chip emulation
 *   - Sound chips run in sync with emulation (legacy behavior)
 */

#ifndef SOUND_INTERFACE_H
#define SOUND_INTERFACE_H

#include <stdint.h>

#include "audio_realtime.h"

// Check if we're using the real-time audio system
#if AUDIO_USE_REALTIME

//=============================================================================
// Real-Time Mode - Route writes through command queue
//=============================================================================

// YM2612 write wrapper
// port: 0=addr0, 1=data0, 2=addr1, 3=data1
static inline void sound_ym2612_write(unsigned int port, unsigned int data, int cycles) {
    audio_rt_ym2612_write((uint8_t)(port & 3), (uint8_t)data, (uint32_t)cycles);
}

// SN76489 write wrapper
static inline void sound_sn76489_write(int data, int cycles) {
    audio_rt_sn76489_write((uint8_t)data, (uint32_t)cycles);
}

// Frame sync (call at end of each frame to help timing)
static inline void sound_frame_sync(int frame_cycles) {
    audio_rt_frame_sync((uint32_t)frame_cycles);
}

// Reset sound chips
static inline void sound_reset(void) {
    audio_rt_reset();
}

#else

//=============================================================================
// Legacy Mode - Direct writes to sound chips
//=============================================================================

#include "sound/ym2612.h"
#include "sound/gwenesis_sn76489.h"

static inline void sound_ym2612_write(unsigned int port, unsigned int data, int cycles) {
    YM2612Write(port, data, cycles);
}

static inline void sound_sn76489_write(int data, int cycles) {
    gwenesis_SN76489_Write(data, cycles);
}

static inline void sound_frame_sync(int frame_cycles) {
    // No-op in legacy mode
    (void)frame_cycles;
}

static inline void sound_reset(void) {
    YM2612ResetChip();
    gwenesis_SN76489_Reset();
}

#endif // AUDIO_USE_REALTIME

#endif // SOUND_INTERFACE_H
