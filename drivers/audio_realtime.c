/*
 * frank-genesis - Autonomous Real-Time Audio System with Independent Timing
 *
 * This module implements a fully autonomous audio system where:
 * - Core 1 runs its own hardware timer for audio sample generation
 * - Sound chips (YM2612/SN76489) are emulated on Core 1 with precise timing
 * - Core 0 sends register writes to Core 1 via a lock-free SPSC queue
 * - Audio generation is completely independent of video frame timing
 * - DMA double-buffering ensures glitch-free playback
 *
 * Architecture:
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │ Core 0 (Emulation)                                                  │
 * │  ┌─────────────┐     ┌──────────────┐                               │
 * │  │ M68K / Z80  │────▶│ Register     │                               │
 * │  │ Emulation   │     │ Write Queue  │──────────────────────┐        │
 * │  └─────────────┘     └──────────────┘                      │        │
 * └────────────────────────────────────────────────────────────│────────┘
 *                                                              │
 *                                                              ▼
 * ┌────────────────────────────────────────────────────────────────────┐
 * │ Core 1 (Audio - Independent Timing)                                │
 * │  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐          │
 * │  │ HW Timer     │───▶│ Sound Chip   │───▶│ Ring Buffer  │──▶ DMA   │
 * │  │ (53280 Hz)   │    │ YM2612+PSG   │    │ (4 buffers)  │──▶ I2S   │
 * │  └──────────────┘    └──────────────┘    └──────────────┘          │
 * └────────────────────────────────────────────────────────────────────┘
 */

// Include audio.h first for i2s_config_t definition
#include "audio.h"
#include "audio_realtime.h"
#include "board_config.h"
#include "audio_i2s.pio.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/timer.h"

// Genesis sound chip headers (Core 1 has its own instances)
#include "sound/ym2612.h"
#include "sound/gwenesis_sn76489.h"

//=============================================================================
// Runtime Mode Flag
//=============================================================================

volatile bool audio_realtime_mode = false;

//=============================================================================
// Configuration Constants
//=============================================================================

#define AUDIO_DMA_IRQ DMA_IRQ_1
#define AUDIO_DMA_CH_A 10
#define AUDIO_DMA_CH_B 11

// Timer alarm for audio sample generation
#define AUDIO_TIMER_NUM 1
#define AUDIO_TIMER_IRQ TIMER1_IRQ_0

// Sample generation timing
// We generate samples in chunks for efficiency
#define SAMPLES_PER_CHUNK 64
#define SAMPLES_PER_FRAME 888  // 53280 Hz / 60 fps = 888 samples per frame
// Use precise calculation to avoid integer truncation errors
// 64 samples at 53280 Hz = 1201.2 µs per chunk
#define CHUNK_PERIOD_US  ((SAMPLES_PER_CHUNK * 1000000ULL) / AUDIO_RT_SAMPLE_RATE)  // 1201 µs

// Sound chip clock dividers (Genesis timing)
// AUDIO_FREQ_DIVISOR from gwenesis_bus.h is 1009 - that's what YM2612 uses
#define YM2612_CLOCK_DIVIDER 1009
#define SN76489_CLOCK_DIVIDER 1009

//=============================================================================
// Ring Buffer for DMA
//=============================================================================

#define RING_BUFFER_COUNT 4
#define RING_BUFFER_SAMPLES AUDIO_RING_SAMPLES_PER_BUFFER

// Stereo sample buffers (L/R interleaved as 32-bit words)
static uint32_t __attribute__((aligned(4))) 
    ring_buffers[RING_BUFFER_COUNT][RING_BUFFER_SAMPLES];

// Ring buffer state
static volatile uint32_t ring_write_idx = 0;  // Next buffer to write
static volatile uint32_t ring_read_idx = 0;   // Next buffer for DMA to read
static volatile uint32_t ring_count = 0;      // Buffers available for DMA

// Current write position within the active buffer
static volatile uint32_t buffer_sample_idx = 0;

//=============================================================================
// Command Queue (SPSC - Single Producer Single Consumer)
//=============================================================================

static audio_cmd_t __attribute__((aligned(4))) 
    cmd_queue[AUDIO_CMD_QUEUE_SIZE];
static volatile uint32_t cmd_write_idx = 0;  // Core 0 writes here
static volatile uint32_t cmd_read_idx = 0;   // Core 1 reads here

//=============================================================================
// DAC Sample Buffer - stores DAC writes for proper playback timing
//=============================================================================

#define DAC_BUFFER_SIZE 2048  // Enough for ~2 frames of DAC data
static int16_t dac_sample_buffer[DAC_BUFFER_SIZE];
static volatile uint32_t dac_write_idx = 0;
static volatile uint32_t dac_read_idx = 0;
static volatile bool dac_enabled = false;

// Sample-and-hold DAC value (set from command processing on Core 1)
static volatile int32_t dac_sample_hold = 0;
static volatile bool dac_is_enabled = false;
static volatile uint8_t dac_address_shadow = 0;  // Track last address written

//=============================================================================
// Audio State
//=============================================================================

static PIO audio_pio;
static uint audio_sm;
static int dma_channel_a = -1;
static int dma_channel_b = -1;
static uint32_t dma_transfer_count;

static volatile bool audio_rt_running = false;
static volatile bool audio_rt_enabled = true;
static volatile int master_volume = 100;

// Statistics
static audio_rt_stats_t stats = {0};

// Timing state
static volatile uint64_t next_sample_time_us = 0;
static volatile uint32_t audio_cycle_accumulator = 0;

// Sound chip state (local to Core 1)
// These shadow the main instances for lock-free operation
static volatile bool sound_chips_initialized = false;

// Intermediate sample buffer for mixing
static int16_t __attribute__((aligned(4))) mix_buffer[SAMPLES_PER_CHUNK * 2];

// Low-pass filter state
static int32_t lpf_state_l = 0;
static int32_t lpf_state_r = 0;
#define LPF_ALPHA 128  // 128/256 = 0.5 - gentler filtering for less distortion

//=============================================================================
// I2S Configuration
//=============================================================================

static i2s_config_t i2s_config;

static void audio_rt_init_i2s(void) {
    i2s_config.sample_freq = AUDIO_RT_SAMPLE_RATE;
    i2s_config.channel_count = 2;
    i2s_config.data_pin = I2S_DATA_PIN;
    i2s_config.clock_pin_base = I2S_CLOCK_PIN_BASE;
    i2s_config.pio = pio0;
    i2s_config.sm = 0;
    i2s_config.dma_channel = AUDIO_DMA_CH_A;
    i2s_config.dma_trans_count = RING_BUFFER_SAMPLES;
    i2s_config.dma_buf = NULL;
    i2s_config.volume = 0;
    
    audio_pio = i2s_config.pio;
    dma_transfer_count = i2s_config.dma_trans_count;
    
    // Reset PIO0
    reset_block(RESETS_RESET_PIO0_BITS);
    unreset_block_wait(RESETS_RESET_PIO0_BITS);
    
    // Clear DMA IRQ flags
    dma_hw->ints1 = (1u << AUDIO_DMA_CH_A) | (1u << AUDIO_DMA_CH_B);
    
    // Configure GPIO for PIO
    gpio_set_function(i2s_config.data_pin, GPIO_FUNC_PIO0);
    gpio_set_function(i2s_config.clock_pin_base, GPIO_FUNC_PIO0);
    gpio_set_function(i2s_config.clock_pin_base + 1, GPIO_FUNC_PIO0);
    
    gpio_set_drive_strength(i2s_config.data_pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(i2s_config.clock_pin_base, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(i2s_config.clock_pin_base + 1, GPIO_DRIVE_STRENGTH_12MA);
    
    // Claim state machine
    audio_sm = pio_claim_unused_sm(audio_pio, true);
    i2s_config.sm = audio_sm;
    
    // Add PIO program
    uint offset = pio_add_program(audio_pio, &audio_i2s_program);
    audio_i2s_program_init(audio_pio, audio_sm, offset, 
                           i2s_config.data_pin, i2s_config.clock_pin_base);
    
    // Clear FIFOs
    pio_sm_clear_fifos(audio_pio, audio_sm);
    
    // Set clock divider
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t divider = sys_clk * 4 / i2s_config.sample_freq;
    pio_sm_set_clkdiv_int_frac(audio_pio, audio_sm, divider >> 8u, divider & 0xffu);
}

//=============================================================================
// DMA IRQ Handler
//=============================================================================

static void __isr audio_rt_dma_irq_handler(void) {
    uint32_t ints = dma_hw->ints1;
    
    // Handle channel A completion
    if ((dma_channel_a >= 0) && (ints & (1u << dma_channel_a))) {
        dma_hw->ints1 = (1u << dma_channel_a);
        
        // Advance read index if we have buffers available
        if (ring_count > 0) {
            ring_read_idx = (ring_read_idx + 1) % RING_BUFFER_COUNT;
            ring_count--;
        } else {
            stats.buffer_underruns++;
        }
        
        // Re-arm channel A with next buffer (or repeat current if underrun)
        uint32_t next_read = ring_read_idx;
        if (ring_count == 0) {
            // Underrun - output silence
            memset(ring_buffers[next_read], 0, RING_BUFFER_SAMPLES * sizeof(uint32_t));
        }
        dma_channel_set_read_addr(dma_channel_a, ring_buffers[next_read], false);
        dma_channel_set_trans_count(dma_channel_a, dma_transfer_count, false);
        
        stats.irq_count++;
    }
    
    // Handle channel B completion
    if ((dma_channel_b >= 0) && (ints & (1u << dma_channel_b))) {
        dma_hw->ints1 = (1u << dma_channel_b);
        
        if (ring_count > 0) {
            ring_read_idx = (ring_read_idx + 1) % RING_BUFFER_COUNT;
            ring_count--;
        } else {
            stats.buffer_underruns++;
        }
        
        uint32_t next_read = ring_read_idx;
        if (ring_count == 0) {
            memset(ring_buffers[next_read], 0, RING_BUFFER_SAMPLES * sizeof(uint32_t));
        }
        dma_channel_set_read_addr(dma_channel_b, ring_buffers[next_read], false);
        dma_channel_set_trans_count(dma_channel_b, dma_transfer_count, false);
        
        stats.irq_count++;
    }
}

//=============================================================================
// DMA Setup
//=============================================================================

static void audio_rt_init_dma(void) {
    // Abort and unclaim channels
    dma_channel_abort(AUDIO_DMA_CH_A);
    dma_channel_abort(AUDIO_DMA_CH_B);
    while (dma_channel_is_busy(AUDIO_DMA_CH_A) || dma_channel_is_busy(AUDIO_DMA_CH_B)) {
        tight_loop_contents();
    }
    
    dma_channel_unclaim(AUDIO_DMA_CH_A);
    dma_channel_unclaim(AUDIO_DMA_CH_B);
    dma_channel_claim(AUDIO_DMA_CH_A);
    dma_channel_claim(AUDIO_DMA_CH_B);
    dma_channel_a = AUDIO_DMA_CH_A;
    dma_channel_b = AUDIO_DMA_CH_B;
    
    // Configure DMA channels in ping-pong chain
    dma_channel_config cfg_a = dma_channel_get_default_config(dma_channel_a);
    channel_config_set_read_increment(&cfg_a, true);
    channel_config_set_write_increment(&cfg_a, false);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_a, pio_get_dreq(audio_pio, audio_sm, true));
    channel_config_set_chain_to(&cfg_a, dma_channel_b);
    
    dma_channel_config cfg_b = dma_channel_get_default_config(dma_channel_b);
    channel_config_set_read_increment(&cfg_b, true);
    channel_config_set_write_increment(&cfg_b, false);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_b, pio_get_dreq(audio_pio, audio_sm, true));
    channel_config_set_chain_to(&cfg_b, dma_channel_a);
    
    // Initialize with first two buffers (silence)
    memset(ring_buffers, 0, sizeof(ring_buffers));
    
    dma_channel_configure(dma_channel_a, &cfg_a,
        &audio_pio->txf[audio_sm], ring_buffers[0], dma_transfer_count, false);
    dma_channel_configure(dma_channel_b, &cfg_b,
        &audio_pio->txf[audio_sm], ring_buffers[1], dma_transfer_count, false);
    
    // Set up IRQ handler
    irq_set_exclusive_handler(AUDIO_DMA_IRQ, audio_rt_dma_irq_handler);
    irq_set_priority(AUDIO_DMA_IRQ, 0x40);  // Higher priority for audio
    irq_set_enabled(AUDIO_DMA_IRQ, true);
    
    dma_hw->ints1 = (1u << dma_channel_a) | (1u << dma_channel_b);
    dma_channel_set_irq1_enabled(dma_channel_a, true);
    dma_channel_set_irq1_enabled(dma_channel_b, true);
}

//=============================================================================
// Command Queue Operations
//=============================================================================

static inline bool cmd_queue_is_empty(void) {
    return cmd_read_idx == cmd_write_idx;
}

static inline bool cmd_queue_is_full(void) {
    return ((cmd_write_idx + 1) & AUDIO_CMD_QUEUE_MASK) == cmd_read_idx;
}

static inline uint32_t cmd_queue_count(void) {
    return (cmd_write_idx - cmd_read_idx) & AUDIO_CMD_QUEUE_MASK;
}

// Push command (Core 0 only)
static bool cmd_queue_push(const audio_cmd_t *cmd) {
    if (cmd_queue_is_full()) {
        stats.queue_overflows++;
        return false;
    }
    
    cmd_queue[cmd_write_idx] = *cmd;
    __dmb();  // Ensure write is visible before updating index
    cmd_write_idx = (cmd_write_idx + 1) & AUDIO_CMD_QUEUE_MASK;
    
    uint32_t depth = cmd_queue_count();
    if (depth > stats.max_queue_depth) {
        stats.max_queue_depth = depth;
    }
    
    return true;
}

// Pop command (Core 1 only)
static bool cmd_queue_pop(audio_cmd_t *cmd) {
    if (cmd_queue_is_empty()) {
        return false;
    }
    
    *cmd = cmd_queue[cmd_read_idx];
    __dmb();  // Ensure read completes before updating index
    cmd_read_idx = (cmd_read_idx + 1) & AUDIO_CMD_QUEUE_MASK;
    stats.commands_processed++;
    
    return true;
}

//=============================================================================
// Core 0 API - Send Commands to Core 1
//=============================================================================

void audio_rt_ym2612_write(uint8_t port, uint8_t data, uint32_t cycles) {
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_YM2612_WRITE,
        .port = port,
        .data = data,
        .reserved = 0,
        .timestamp = cycles
    };
    cmd_queue_push(&cmd);
}

// DAC sample write - goes directly into DAC ring buffer (not command queue)
void audio_rt_dac_write(uint8_t sample) {
    // Convert 8-bit unsigned to 14-bit signed (same as YM2612 DAC)
    int16_t dac_value = ((int)sample - 0x80) << 6;
    
    // Write to DAC ring buffer
    uint32_t next_idx = (dac_write_idx + 1) % DAC_BUFFER_SIZE;
    if (next_idx != dac_read_idx) {  // Check for overflow
        dac_sample_buffer[dac_write_idx] = dac_value;
        __dmb();
        dac_write_idx = next_idx;
    }
    // If buffer full, we drop the sample (shouldn't happen normally)
}

void audio_rt_sn76489_write(uint8_t data, uint32_t cycles) {
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_SN76489_WRITE,
        .port = 0,
        .data = data,
        .reserved = 0,
        .timestamp = cycles
    };
    cmd_queue_push(&cmd);
}

void audio_rt_frame_sync(uint32_t frame_cycles) {
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_FRAME_SYNC,
        .port = 0,
        .data = 0,
        .reserved = 0,
        .timestamp = frame_cycles
    };
    cmd_queue_push(&cmd);
}

void audio_rt_reset(void) {
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_RESET,
        .port = 0,
        .data = 0,
        .reserved = 0,
        .timestamp = 0
    };
    cmd_queue_push(&cmd);
}

void audio_rt_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 128) volume = 128;
    
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_VOLUME,
        .port = 0,
        .data = (uint8_t)volume,
        .reserved = 0,
        .timestamp = 0
    };
    cmd_queue_push(&cmd);
}

void audio_rt_set_enabled(bool enabled) {
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_ENABLE,
        .port = 0,
        .data = enabled ? 1 : 0,
        .reserved = 0,
        .timestamp = 0
    };
    cmd_queue_push(&cmd);
}

//=============================================================================
// Core 1 - Sound Chip Emulation (runs at audio rate)
//=============================================================================

// Forward declarations of direct chip update functions
// These bypass the realtime_mode check to avoid infinite recursion
extern void YM2612Write_internal(unsigned int a, unsigned int v, int target);
extern void gwenesis_SN76489_Write_internal(int data, int target);

// Accessor functions for YM2612 DAC state (defined in ym2612.c)
extern int ym2612_get_dacout(void);
extern int ym2612_get_dacen(void);

// DAC sample capture during command processing
#define DAC_CAPTURE_SIZE 128
static int16_t dac_capture[DAC_CAPTURE_SIZE];
static uint32_t dac_capture_count = 0;
static bool dac_enabled_this_chunk = false;

// Process ALL pending commands from Core 0
// Captures DAC writes in order for proper playback
static void process_all_commands(void) {
    audio_cmd_t cmd;
    
    // Reset DAC capture for this chunk
    dac_capture_count = 0;
    dac_enabled_this_chunk = false;
    
    while (cmd_queue_pop(&cmd)) {
        switch (cmd.type) {
            case AUDIO_CMD_YM2612_WRITE:
                // Before calling internal write, check if this will be a DAC write
                // Port 1 data write to address 0x2A
                {
                    static uint8_t last_address = 0;
                    if (cmd.port == 0) {
                        last_address = cmd.data;
                    } else if (cmd.port == 1) {
                        if (last_address == 0x2A && dac_capture_count < DAC_CAPTURE_SIZE) {
                            // Capture DAC value before it gets overwritten
                            int16_t dac_val = ((int)cmd.data - 0x80) << 6;
                            dac_capture[dac_capture_count++] = dac_val;
                        } else if (last_address == 0x2B) {
                            dac_enabled_this_chunk = (cmd.data & 0x80) != 0;
                        }
                    }
                }
                YM2612Write_internal(cmd.port, cmd.data, 0);
                break;
                
            case AUDIO_CMD_SN76489_WRITE:
                gwenesis_SN76489_Write_internal(cmd.data, 0);
                break;
                
            case AUDIO_CMD_RESET:
                YM2612ResetChip();
                gwenesis_SN76489_Reset();
                lpf_state_l = 0;
                lpf_state_r = 0;
                dac_sample_hold = 0;
                dac_is_enabled = false;
                dac_address_shadow = 0;
                break;
                
            case AUDIO_CMD_VOLUME:
                master_volume = cmd.data;
                break;
                
            case AUDIO_CMD_ENABLE:
                audio_rt_enabled = (cmd.data != 0);
                break;
                
            case AUDIO_CMD_FRAME_SYNC:
            case AUDIO_CMD_NOP:
            default:
                break;
        }
    }
}

// Generate audio samples for one chunk - frame-gated
static void generate_samples(int16_t *buffer, uint32_t count) {
    extern int16_t *gwenesis_ym2612_buffer;
    extern volatile int ym2612_index;
    extern volatile int ym2612_clock;
    
    extern int16_t *gwenesis_sn76489_buffer;
    extern volatile int sn76489_index;
    extern volatile int sn76489_clock;
    
    static int16_t current_dac = 0;
    static bool dac_on = false;
    static int32_t samples_budget = 0;  // How many samples we're allowed to generate
    
    audio_cmd_t cmd;
    
    // Process ALL pending commands
    while (cmd_queue_pop(&cmd)) {
        switch (cmd.type) {
            case AUDIO_CMD_YM2612_WRITE: {
                static uint8_t addr_latch = 0;
                if (cmd.port == 0) {
                    addr_latch = cmd.data;
                } else if (cmd.port == 1) {
                    if (addr_latch == 0x2A) {
                        current_dac = ((int)cmd.data - 0x80) << 6;
                    } else if (addr_latch == 0x2B) {
                        dac_on = (cmd.data & 0x80) != 0;
                    }
                }
                YM2612Write_internal(cmd.port, cmd.data, 0);
                break;
            }
            case AUDIO_CMD_SN76489_WRITE:
                gwenesis_SN76489_Write_internal(cmd.data, 0);
                break;
            case AUDIO_CMD_RESET:
                YM2612ResetChip();
                gwenesis_SN76489_Reset();
                current_dac = 0;
                dac_on = false;
                samples_budget = 0;
                break;
            case AUDIO_CMD_VOLUME:
                master_volume = cmd.data;
                break;
            case AUDIO_CMD_ENABLE:
                audio_rt_enabled = (cmd.data != 0);
                break;
            case AUDIO_CMD_FRAME_SYNC:
                // Each frame allows us to generate SAMPLES_PER_FRAME more samples
                samples_budget += SAMPLES_PER_FRAME;
                // Cap to prevent runaway accumulation
                if (samples_budget > SAMPLES_PER_FRAME * 3) {
                    samples_budget = SAMPLES_PER_FRAME * 3;
                }
                break;
            default:
                break;
        }
    }
    
    // Determine how many samples we can actually generate
    uint32_t to_generate = count;
    if (samples_budget <= 0) {
        // No budget - output silence (hold last filtered value)
        for (uint32_t i = 0; i < count; i++) {
            buffer[i * 2] = (int16_t)lpf_state_l;
            buffer[i * 2 + 1] = (int16_t)lpf_state_l;
        }
        return;
    }
    
    if ((int32_t)to_generate > samples_budget) {
        to_generate = (uint32_t)samples_budget;
    }
    
    // Generate FM/PSG samples
    ym2612_index = 0;
    ym2612_clock = 0;
    sn76489_index = 0;
    sn76489_clock = 0;
    
    int target_clock = to_generate * YM2612_CLOCK_DIVIDER;
    ym2612_run(target_clock);
    gwenesis_SN76489_run(target_clock);
    
    int ym_count = ym2612_index;
    int sn_count = sn76489_index;
    
    // Mix the generated samples
    for (uint32_t i = 0; i < to_generate; i++) {
        int32_t sample = 0;
        
        if (i < (uint32_t)ym_count) {
            sample += gwenesis_ym2612_buffer[i];
        }
        
        if (dac_on) {
            sample += current_dac >> 1;
        }
        
        if (i < (uint32_t)sn_count) {
            sample += gwenesis_sn76489_buffer[i];
        }
        
        sample = (sample * master_volume) >> 7;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        
        lpf_state_l = (lpf_state_l * (256 - LPF_ALPHA) + sample * LPF_ALPHA) >> 8;
        
        buffer[i * 2] = (int16_t)lpf_state_l;
        buffer[i * 2 + 1] = (int16_t)lpf_state_l;
    }
    
    // Fill remainder with silence if we couldn't generate enough
    for (uint32_t i = to_generate; i < count; i++) {
        buffer[i * 2] = (int16_t)lpf_state_l;
        buffer[i * 2 + 1] = (int16_t)lpf_state_l;
    }
    
    samples_budget -= to_generate;
    stats.total_samples += to_generate;
}

// Write samples to ring buffer
static void write_to_ring_buffer(const int16_t *samples, uint32_t count) {
    uint32_t *write_buf = ring_buffers[ring_write_idx];
    
    for (uint32_t i = 0; i < count && buffer_sample_idx < RING_BUFFER_SAMPLES; i++) {
        // Pack stereo samples into 32-bit word
        int16_t left = samples[i * 2];
        int16_t right = samples[i * 2 + 1];
        write_buf[buffer_sample_idx++] = ((uint32_t)(uint16_t)right << 16) | (uint16_t)left;
    }
    
    // Check if buffer is full
    if (buffer_sample_idx >= RING_BUFFER_SAMPLES) {
        // Buffer is complete, advance write index
        __dmb();
        
        uint32_t irq_state = save_and_disable_interrupts();
        ring_write_idx = (ring_write_idx + 1) % RING_BUFFER_COUNT;
        ring_count++;
        restore_interrupts(irq_state);
        
        buffer_sample_idx = 0;
        stats.buffers_filled++;
    }
}

//=============================================================================
// Core 1 Main Loop - Independent Timing
//=============================================================================

void audio_rt_core1_main(void) {
    // Allow core 0 to pause this core during flash operations
    multicore_lockout_victim_init();
    
    // Initialize I2S
    audio_rt_init_i2s();
    
    // Initialize DMA
    audio_rt_init_dma();
    
    // NOTE: Sound chips are already initialized by Core 0 in init_genesis()
    // Core 1 just uses them - no re-initialization needed
    // Core 0 sends register writes via command queue, Core 1 generates samples
    
    sound_chips_initialized = true;
    
    // Pre-fill ring buffer with silence
    memset(ring_buffers, 0, sizeof(ring_buffers));
    ring_write_idx = 2;  // Start writing at buffer 2
    ring_read_idx = 0;   // DMA starts reading at buffer 0
    ring_count = 2;      // 2 buffers ready for DMA
    buffer_sample_idx = 0;
    
    // Enable PIO state machine
    pio_sm_set_enabled(audio_pio, audio_sm, true);
    
    // Start DMA
    dma_channel_start(dma_channel_a);
    
    audio_rt_running = true;
    
#if ENABLE_LOGGING
    printf("Audio RT: Core 1 started, sample rate: %u Hz\n", AUDIO_RT_SAMPLE_RATE);
#endif
    
    // Main audio loop - generate samples when buffer space available
    // Tied to DMA consumption, not a timer
    while (1) {
        // Check if we have room in the ring buffer
        // ring_count tracks how many buffers are filled and waiting for DMA
        // We generate when there's room (ring_count < RING_BUFFER_COUNT - 1)
        if (ring_count < RING_BUFFER_COUNT - 1) {
            // Generate a chunk of samples
            if (audio_rt_enabled) {
                generate_samples(mix_buffer, SAMPLES_PER_CHUNK);
            } else {
                // Silence
                memset(mix_buffer, 0, SAMPLES_PER_CHUNK * 2 * sizeof(int16_t));
            }
            
            // Write to ring buffer
            write_to_ring_buffer(mix_buffer, SAMPLES_PER_CHUNK);
        } else {
            // Buffer full - wait for DMA to consume
            tight_loop_contents();
        }
    }
}

//=============================================================================
// Initialization API
//=============================================================================

bool audio_rt_init(void) {
    // Reset state
    memset(&stats, 0, sizeof(stats));
    cmd_write_idx = 0;
    cmd_read_idx = 0;
    ring_write_idx = 0;
    ring_read_idx = 0;
    ring_count = 0;
    buffer_sample_idx = 0;
    audio_cycle_accumulator = 0;
    audio_rt_running = false;
    audio_rt_enabled = true;
    master_volume = 100;
    lpf_state_l = 0;
    lpf_state_r = 0;
    sound_chips_initialized = false;
    
    return true;
}

void audio_rt_shutdown(void) {
    audio_rt_running = false;
    
    // Stop PIO
    if (audio_pio && audio_sm >= 0) {
        pio_sm_set_enabled(audio_pio, audio_sm, false);
    }
    
    // Stop DMA
    irq_set_enabled(AUDIO_DMA_IRQ, false);
    
    if (dma_channel_a >= 0) {
        dma_channel_set_irq1_enabled(dma_channel_a, false);
        dma_channel_abort(dma_channel_a);
        dma_hw->ints1 = (1u << dma_channel_a);
        dma_channel_unclaim(dma_channel_a);
        dma_channel_a = -1;
    }
    
    if (dma_channel_b >= 0) {
        dma_channel_set_irq1_enabled(dma_channel_b, false);
        dma_channel_abort(dma_channel_b);
        dma_hw->ints1 = (1u << dma_channel_b);
        dma_channel_unclaim(dma_channel_b);
        dma_channel_b = -1;
    }
}

bool audio_rt_is_running(void) {
    return audio_rt_running;
}

audio_rt_stats_t audio_rt_get_stats(void) {
    return stats;
}

void audio_rt_reset_stats(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    memset(&stats, 0, sizeof(stats));
    restore_interrupts(irq_state);
}

i2s_config_t* audio_rt_get_i2s_config(void) {
    return &i2s_config;
}
