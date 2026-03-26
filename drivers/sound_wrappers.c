/*
 * frank-genesis - Sound Chip Write Wrappers
 *
 * When AUDIO_USE_REALTIME=1, these wrappers intercept sound chip writes
 * and route them to the Core 1 command queue for autonomous processing.
 *
 * This file should be compiled INSTEAD of the direct sound chip code
 * when using real-time audio.
 */

#include "audio_realtime.h"

#if AUDIO_USE_REALTIME

#include <stdint.h>

// These are the wrapper functions that intercept all sound chip writes
// and route them to Core 1 via the command queue.

// Original functions are renamed with _direct suffix
extern void YM2612Write_direct(unsigned int a, unsigned int v, int target);
extern void gwenesis_SN76489_Write_direct(int data, int target);

// Wrapper for YM2612Write - routes to Core 1
void YM2612Write(unsigned int a, unsigned int v, int target) {
    audio_rt_ym2612_write((uint8_t)(a & 3), (uint8_t)v, (uint32_t)target);
}

// Wrapper for gwenesis_SN76489_Write - routes to Core 1
void gwenesis_SN76489_Write(int data, int target) {
    audio_rt_sn76489_write((uint8_t)data, (uint32_t)target);
}

#endif // AUDIO_USE_REALTIME
