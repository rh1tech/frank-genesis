/*
 * frank-genesis - Autonomous Real-Time Audio System
 *
 * This module implements a fully autonomous audio system where:
 * - Core 1 runs sound chip emulation and audio generation independently
 * - DMA IRQs trigger buffer refills automatically
 * - Core 0 sends register writes to Core 1 via a lock-free command queue
 * - Audio timing is decoupled from video frame timing
 *
 * Benefits:
 * - Consistent audio even when video frames are slow
 * - No audio pops/clicks from frame timing jitter
 * - Maximum performance - Core 0 never waits for audio
 */
#ifndef AUDIO_REALTIME_H
#define AUDIO_REALTIME_H

#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// Compile-time switch
//=============================================================================

// Define AUDIO_USE_REALTIME=1 to use the new autonomous audio system
// Define AUDIO_USE_REALTIME=0 or leave undefined to use legacy frame-synced audio
#ifndef AUDIO_USE_REALTIME
#define AUDIO_USE_REALTIME 1
#endif

//=============================================================================
// Runtime Mode Selection
//=============================================================================

// Global flag to enable real-time audio mode
// When true: writes go to command queue for Core 1
// When false: writes go directly to sound chip (legacy mode)
extern volatile bool audio_realtime_mode;

// Check if we should route writes to Core 1
static inline bool audio_use_realtime(void) {
    return audio_realtime_mode;
}

//=============================================================================
// Configuration
//=============================================================================

// Ring buffer size (number of stereo sample frames)
// Larger = more latency but more resilient to timing jitter
// 4 buffers x 888 samples = ~60ms latency at 53kHz
#define AUDIO_RING_BUFFER_COUNT 4
#define AUDIO_RING_SAMPLES_PER_BUFFER 888

// Command queue size (power of 2 for fast modulo)
// Each command is 8 bytes, so 512 commands = 4KB
#define AUDIO_CMD_QUEUE_SIZE 512
#define AUDIO_CMD_QUEUE_MASK (AUDIO_CMD_QUEUE_SIZE - 1)

// Sample rate (Genesis NTSC = ~53kHz)
#define AUDIO_RT_SAMPLE_RATE 53280

//=============================================================================
// Sound Chip Command Types
//=============================================================================

typedef enum {
    AUDIO_CMD_NOP = 0,          // No operation
    AUDIO_CMD_YM2612_WRITE,     // YM2612 register write
    AUDIO_CMD_SN76489_WRITE,    // SN76489 data write
    AUDIO_CMD_RESET,            // Reset all sound chips
    AUDIO_CMD_VOLUME,           // Set master volume
    AUDIO_CMD_ENABLE,           // Enable/disable audio output
    AUDIO_CMD_FRAME_SYNC,       // Frame boundary marker (for timing)
    AUDIO_CMD_DAC_SAMPLE,       // Direct DAC sample (bypasses YM2612)
} audio_cmd_type_t;

// Command structure (8 bytes for alignment)
typedef struct __attribute__((packed, aligned(4))) {
    uint8_t type;               // audio_cmd_type_t
    uint8_t port;               // YM2612: port (0-3), SN76489: unused
    uint8_t data;               // Register value
    uint8_t reserved;           // Padding
    uint32_t timestamp;         // M68K cycle count when command was issued
} audio_cmd_t;

//=============================================================================
// Statistics (for debugging/profiling)
//=============================================================================

typedef struct {
    uint32_t buffers_filled;    // Total buffers filled
    uint32_t buffer_underruns;  // DMA ran out of data
    uint32_t commands_processed;// Total commands processed
    uint32_t queue_overflows;   // Commands dropped due to full queue
    uint32_t irq_count;         // DMA IRQ count
    uint32_t max_queue_depth;   // Maximum queue depth seen
    uint64_t total_samples;     // Total samples generated
} audio_rt_stats_t;

//=============================================================================
// API Functions
//=============================================================================

// Initialize the real-time audio system (call from main, before Core 1 launch)
bool audio_rt_init(void);

// Shutdown audio system
void audio_rt_shutdown(void);

// Check if audio system is running
bool audio_rt_is_running(void);

// Core 1 entry point - runs the autonomous audio loop
void audio_rt_core1_main(void);

//=============================================================================
// Sound Chip Command Interface (called from Core 0)
//=============================================================================

// Send YM2612 register write to audio core
// port: 0=addr0, 1=data0, 2=addr1, 3=data1
// cycles: current M68K cycle count for timing
void audio_rt_ym2612_write(uint8_t port, uint8_t data, uint32_t cycles);

// Send DAC sample directly (bypasses command queue for low latency)
void audio_rt_dac_write(uint8_t sample);

// Send SN76489 data write to audio core
void audio_rt_sn76489_write(uint8_t data, uint32_t cycles);

// Mark frame boundary (for timing synchronization)
void audio_rt_frame_sync(uint32_t frame_cycles);

// Reset sound chips (call when loading new ROM)
void audio_rt_reset(void);

// Set master volume (0-128, 100 = normal)
void audio_rt_set_volume(int volume);

// Enable/disable audio output
void audio_rt_set_enabled(bool enabled);

// Get current audio statistics
audio_rt_stats_t audio_rt_get_stats(void);

// Reset statistics
void audio_rt_reset_stats(void);

//=============================================================================
// Direct access for legacy compatibility
//=============================================================================

// Get I2S config (for volume control, etc.)
// Requires audio.h to be included for i2s_config_t definition
#ifdef _AUDIO_H_INCLUDED_
struct i2s_config_t;
#endif

#endif // AUDIO_REALTIME_H
