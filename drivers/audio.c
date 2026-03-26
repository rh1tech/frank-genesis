/*
 * frank-genesis - I2S Audio Driver with Chained Double Buffer DMA
 *
 * Uses two DMA channels in ping-pong configuration:
 * - Channel A plays buffer 0, then triggers channel B
 * - Channel B plays buffer 1, then triggers channel A
 *
 * Each channel completion raises DMA_IRQ_1; the IRQ handler re-arms the
 * completed channel (reset read addr + transfer count) and marks its buffer
 * free for the CPU to refill.
 */

#include "audio.h"
#include "board_config.h"
#include "audio_i2s.pio.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"  // For memory barriers
#include "hardware/irq.h"
#include "hardware/resets.h"  // For reset_block

// Genesis sound chip headers
#include "sound/ym2612.h"
#include "sound/gwenesis_sn76489.h"

//=============================================================================
// State - Chained double buffer (ping-pong) DMA
//=============================================================================

// NOTE: HDMI uses DMA_IRQ_0 with an exclusive handler.
// Audio uses DMA_IRQ_1 to avoid conflicts.

#define AUDIO_DMA_IRQ DMA_IRQ_1

// Fixed DMA channels for audio (keep away from dynamically-claimed HDMI channels)
#define AUDIO_DMA_CH_A 10
#define AUDIO_DMA_CH_B 11

#define DMA_BUFFER_COUNT 2
// One DMA word is one stereo frame (packed L/R int16).
// AUDIO_BUFFER_SAMPLES is sized to cover NTSC/PAL with headroom.
#define DMA_BUFFER_MAX_SAMPLES AUDIO_BUFFER_SAMPLES

static uint32_t __attribute__((aligned(4))) dma_buffers[DMA_BUFFER_COUNT][DMA_BUFFER_MAX_SAMPLES];

// Bitmask of buffers the CPU is allowed to write (1 = free)
static volatile uint32_t dma_buffers_free_mask = 0;

// Pre-roll: fill both buffers before starting playback
#define PREROLL_BUFFERS 2
static volatile int preroll_count = 0;

static int dma_channel_a = -1;
static int dma_channel_b = -1;
static PIO audio_pio;
static uint audio_sm;
static uint32_t dma_transfer_count;

static volatile bool audio_running = false;

static void audio_dma_irq_handler(void);

//=============================================================================
// I2S Implementation
//=============================================================================

i2s_config_t i2s_get_default_config(void) {
    // Use 888 samples per frame for 60fps audio timing
    // This matches TARGET_SAMPLES_NTSC for consistent DMA transfers
    i2s_config_t config = {
        .sample_freq = AUDIO_SAMPLE_RATE,
        .channel_count = 2,
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_CLOCK_PIN_BASE,
        .pio = pio0,
        .sm = 0,
        .dma_channel = 0,
        .dma_trans_count = 888,
        .dma_buf = NULL,
        .volume = 0,
    };
    return config;
}

void i2s_init(i2s_config_t *config) {
#if ENABLE_LOGGING
    printf("Audio: Initializing I2S with chained double-buffer DMA...\n");
    printf("Audio: Sample rate: %u Hz, DMA buffer size: %lu frames\n",
           (unsigned)config->sample_freq, (unsigned long)config->dma_trans_count);
#endif
    
    audio_pio = config->pio;
    dma_transfer_count = config->dma_trans_count;
    
    // CRITICAL: Full hardware reset of PIO0 (but NOT DMA - HDMI uses DMA!)
    reset_block(RESETS_RESET_PIO0_BITS);
    unreset_block_wait(RESETS_RESET_PIO0_BITS);
    
    // Clear audio DMA IRQ flags (IRQ1)
    dma_hw->ints1 = (1u << AUDIO_DMA_CH_A) | (1u << AUDIO_DMA_CH_B);
    
    // Configure GPIO for PIO
    gpio_set_function(config->data_pin, GPIO_FUNC_PIO0);
    gpio_set_function(config->clock_pin_base, GPIO_FUNC_PIO0);
    gpio_set_function(config->clock_pin_base + 1, GPIO_FUNC_PIO0);
    
    gpio_set_drive_strength(config->data_pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(config->clock_pin_base, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(config->clock_pin_base + 1, GPIO_DRIVE_STRENGTH_12MA);
    
    // Claim state machine
    audio_sm = pio_claim_unused_sm(audio_pio, true);
    config->sm = audio_sm;
#if ENABLE_LOGGING
    printf("Audio: Using PIO0 SM%d\n", audio_sm);
#endif
    
    // Add PIO program
    uint offset = pio_add_program(audio_pio, &audio_i2s_program);
    audio_i2s_program_init(audio_pio, audio_sm, offset, 
                           config->data_pin, config->clock_pin_base);
    
    // Drain the TX FIFO
    pio_sm_clear_fifos(audio_pio, audio_sm);
    
    // Set clock divider for sample rate
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t divider = sys_clk * 4 / config->sample_freq;
    pio_sm_set_clkdiv_int_frac(audio_pio, audio_sm, divider >> 8u, divider & 0xffu);
#if ENABLE_LOGGING
        printf("Audio: Clock divider: %u.%u (sys=%lu MHz)\n",
            (unsigned)(divider >> 8u), (unsigned)(divider & 0xffu), (unsigned long)(sys_clk / 1000000));
#endif
    
    // Validate transfer count fits our static buffers
    dma_transfer_count = config->dma_trans_count;
    if (dma_transfer_count == 0) dma_transfer_count = 1;
    if (dma_transfer_count > DMA_BUFFER_MAX_SAMPLES) dma_transfer_count = DMA_BUFFER_MAX_SAMPLES;
    config->dma_trans_count = (uint16_t)dma_transfer_count;

    // Initialize DMA buffers with silence
    memset(dma_buffers, 0, sizeof(dma_buffers));
    config->dma_buf = (uint16_t *)(void *)dma_buffers[0];

    // Use fixed DMA channels for audio
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
    config->dma_channel = (uint8_t)dma_channel_a;
#if ENABLE_LOGGING
    printf("Audio: Using DMA channels %d/%d (IRQ=%d)\n", dma_channel_a, dma_channel_b, AUDIO_DMA_IRQ);
#endif

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

    dma_channel_configure(
        dma_channel_a,
        &cfg_a,
        &audio_pio->txf[audio_sm],
        dma_buffers[0],
        dma_transfer_count,
        false
    );

    dma_channel_configure(
        dma_channel_b,
        &cfg_b,
        &audio_pio->txf[audio_sm],
        dma_buffers[1],
        dma_transfer_count,
        false
    );

    // Set up DMA IRQ1 handler (avoid HDMI's DMA_IRQ_0 exclusive handler)
    irq_set_exclusive_handler(AUDIO_DMA_IRQ, audio_dma_irq_handler);
    irq_set_priority(AUDIO_DMA_IRQ, 0x80);
    irq_set_enabled(AUDIO_DMA_IRQ, true);

    // Enable IRQ1 for both channels
    dma_hw->ints1 = (1u << dma_channel_a) | (1u << dma_channel_b);
    dma_channel_set_irq1_enabled(dma_channel_a, true);
    dma_channel_set_irq1_enabled(dma_channel_b, true);
    
    // Enable PIO state machine
    pio_sm_set_enabled(audio_pio, audio_sm, true);
    
    // Initialize state
    preroll_count = 0;
    dma_buffers_free_mask = (1u << DMA_BUFFER_COUNT) - 1u; // both free
    audio_running = false;

#if ENABLE_LOGGING
    printf("Audio: I2S ready (double buffer DMA with %d buffer pre-roll)\n", PREROLL_BUFFERS);
#endif
}

void i2s_dma_write_count(i2s_config_t *config, const int16_t *samples, uint32_t sample_count) {
    if (sample_count > dma_transfer_count) sample_count = dma_transfer_count;
    if (sample_count == 0) sample_count = 1;

    // Wait for a free buffer, then claim it (atomically vs DMA IRQ)
    uint8_t buf_index = 0;
    while (true) {
        uint32_t irq_state = save_and_disable_interrupts();
        uint32_t free_mask = dma_buffers_free_mask;

        if (!audio_running) {
            // Pre-roll fills buffer 0 then buffer 1 to preserve ordering
            buf_index = (uint8_t)preroll_count;
            if (buf_index < DMA_BUFFER_COUNT && (free_mask & (1u << buf_index))) {
                dma_buffers_free_mask &= ~(1u << buf_index);
                restore_interrupts(irq_state);
                break;
            }
        } else {
            if (free_mask) {
                buf_index = (free_mask & 1u) ? 0 : 1;
                dma_buffers_free_mask &= ~(1u << buf_index);
                restore_interrupts(irq_state);
                break;
            }
        }

        restore_interrupts(irq_state);
        tight_loop_contents();
    }

    uint32_t *write_ptr = dma_buffers[buf_index];
    int16_t *write_ptr16 = (int16_t *)(void *)write_ptr;
    
    if (config->volume == 0) {
        memcpy(write_ptr, samples, sample_count * sizeof(uint32_t));
    } else {
        // Volume adjustment
        for (uint32_t i = 0; i < sample_count * 2; i++) {
            write_ptr16[i] = samples[i] >> config->volume;
        }
    }

    // Pad remainder with silence to keep DMA transfer size stable
    if (sample_count < dma_transfer_count) {
        memset(&write_ptr[sample_count], 0, (dma_transfer_count - sample_count) * sizeof(uint32_t));
    }
    
    // Memory barrier to ensure writes are visible before DMA reads
    __dmb();
    
    if (!audio_running) {
        preroll_count++;
        if (preroll_count >= PREROLL_BUFFERS) {
            // Both buffers are filled and queued; start playback on channel A
            dma_channel_start(dma_channel_a);
            audio_running = true;
        }
    }
}

void i2s_dma_write(i2s_config_t *config, const int16_t *samples) {
    i2s_dma_write_count(config, samples, dma_transfer_count);
}

void i2s_volume(i2s_config_t *config, uint8_t volume) {
    if (volume > 16) volume = 16;
    config->volume = volume;
}

void i2s_increase_volume(i2s_config_t *config) {
    if (config->volume > 0) config->volume--;
}

void i2s_decrease_volume(i2s_config_t *config) {
    if (config->volume < 16) config->volume++;
}

//=============================================================================
// High-level Audio API
//=============================================================================

static bool audio_initialized = false;
static bool audio_enabled = true;
static int master_volume = 100;  // 0-128
static i2s_config_t i2s_config;

// External audio control flags from main.c
extern bool sn76489_enabled;  // PSG/DAC sound enable
extern bool ym2612_enabled;   // FM sound enable

// Startup mute: output silence for first N frames to let hardware settle
#define STARTUP_FADE_FRAMES 120  // 2 seconds at 60fps
static int startup_frame_counter = 0;

// Low-pass filter state (declared here so audio_init can reset it)
static int32_t lpf_state = 0;

// Mixed stereo buffer
static int16_t __attribute__((aligned(4))) mixed_buffer[AUDIO_BUFFER_SAMPLES * 2];

// Pre-mixed mono buffer (before time-stretching)
static int16_t __attribute__((aligned(4))) premix_buffer[AUDIO_BUFFER_SAMPLES];

// Time tracking for FPS-independent audio
static uint64_t last_audio_time_us = 0;
static bool first_audio_frame = true;

static inline int16_t clamp_s16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

bool audio_init(void) {
    // ALWAYS reinitialize - don't trust static state after hard reset
    // Reset all state variables first
    audio_initialized = false;
    audio_running = false;
    lpf_state = 0;
    dma_channel_a = -1;
    dma_channel_b = -1;
    preroll_count = 0;
    dma_buffers_free_mask = (1u << DMA_BUFFER_COUNT) - 1u;
    startup_frame_counter = 0;
    last_audio_time_us = 0;
    first_audio_frame = true;
    
    i2s_config = i2s_get_default_config();
    i2s_init(&i2s_config);
    
    audio_initialized = true;
    lpf_state = 0;
    
    return true;
}

void audio_shutdown(void) {
    if (!audio_initialized) return;

    // Stop producing new audio and stop the PIO state machine first
    audio_running = false;
    pio_sm_set_enabled(audio_pio, audio_sm, false);

    // Disable DMA IRQ and per-channel IRQ generation (audio uses IRQ1)
    irq_set_enabled(AUDIO_DMA_IRQ, false);

    if (dma_channel_a >= 0) {
        dma_channel_set_irq1_enabled(dma_channel_a, false);
        dma_channel_abort(dma_channel_a);
        // Clear any pending IRQ flag
        dma_hw->ints1 = (1u << dma_channel_a);
        dma_channel_unclaim(dma_channel_a);
        dma_channel_a = -1;
    }

    if (dma_channel_b >= 0) {
        dma_channel_set_irq1_enabled(dma_channel_b, false);
        dma_channel_abort(dma_channel_b);
        // Clear any pending IRQ flag
        dma_hw->ints1 = (1u << dma_channel_b);
        dma_channel_unclaim(dma_channel_b);
        dma_channel_b = -1;
    }

    // Mark both buffers free again
    dma_buffers_free_mask = (1u << DMA_BUFFER_COUNT) - 1u;
    preroll_count = 0;

    audio_initialized = false;
}

bool audio_is_initialized(void) {
    return audio_initialized;
}

// Target samples per frame for consistent DMA timing
#define TARGET_SAMPLES_NTSC 888
#define FADE_SAMPLES 32  // Samples to crossfade at frame boundary (increased for smoother transition)

// Simple 1-pole low-pass filter coefficient (256 = no filter, lower = more smoothing)
// 192/256 = 0.75 - stronger filtering to reduce clicks
#define LPF_ALPHA 192

// Click detection threshold - suppress sudden jumps greater than this
#define CLICK_THRESHOLD 2000

// 3-sample history for median filter (catches single-sample noise spikes)
static int16_t sample_history[3] = {0, 0, 0};
static int history_idx = 0;

// Simple median of 3 values
static inline int16_t median3(int16_t a, int16_t b, int16_t c) {
    if (a > b) {
        if (b > c) return b;      // a > b > c
        else if (a > c) return c; // a > c >= b
        else return a;            // c >= a > b
    } else {
        if (a > c) return a;      // b >= a > c
        else if (b > c) return c; // b > c >= a
        else return b;            // c >= b >= a
    }
}

// Soft limiter threshold - compress signals above this to reduce clicks
#define SOFT_LIMIT_THRESHOLD 12000
#define SOFT_LIMIT_RATIO 2  // Compress by 2:1 above threshold

// External: saved sample counts from Core 0
extern volatile int saved_ym_samples;
extern volatile int saved_sn_samples;
extern volatile int16_t last_frame_sample;

// External: read buffer pointers from Core 0 (double-buffered)
extern int16_t *audio_read_sn76489;
extern int16_t *audio_read_ym2612;

// Soft limiter function - compresses loud signals
static inline int16_t soft_limit(int32_t sample) {
    if (sample > SOFT_LIMIT_THRESHOLD) {
        // Compress positive signals above threshold
        return SOFT_LIMIT_THRESHOLD + (sample - SOFT_LIMIT_THRESHOLD) / SOFT_LIMIT_RATIO;
    } else if (sample < -SOFT_LIMIT_THRESHOLD) {
        // Compress negative signals below threshold
        return -SOFT_LIMIT_THRESHOLD + (sample + SOFT_LIMIT_THRESHOLD) / SOFT_LIMIT_RATIO;
    }
    return (int16_t)sample;
}

// Debug: track audio stats
static uint32_t audio_frame_count = 0;
static uint32_t click_count = 0;
static int32_t max_jump = 0;
static int16_t prev_sample = 0;
static int32_t first_click_idx = -1;
static int16_t click_before = 0, click_after = 0;

static void audio_dma_irq_handler(void) {
    uint32_t ints = dma_hw->ints1;
    uint32_t mask = 0;
    if (dma_channel_a >= 0) mask |= (1u << dma_channel_a);
    if (dma_channel_b >= 0) mask |= (1u << dma_channel_b);
    ints &= mask;
    if (!ints) return;

    if ((dma_channel_a >= 0) && (ints & (1u << dma_channel_a))) {
        dma_hw->ints1 = (1u << dma_channel_a);
        dma_channel_set_read_addr(dma_channel_a, dma_buffers[0], false);
        dma_channel_set_trans_count(dma_channel_a, dma_transfer_count, false);
        dma_buffers_free_mask |= 1u;
    }

    if ((dma_channel_b >= 0) && (ints & (1u << dma_channel_b))) {
        dma_hw->ints1 = (1u << dma_channel_b);
        dma_channel_set_read_addr(dma_channel_b, dma_buffers[1], false);
        dma_channel_set_trans_count(dma_channel_b, dma_transfer_count, false);
        dma_buffers_free_mask |= 2u;
    }
}

void audio_submit(void) {
    if (!audio_initialized) return;
    
    // Calculate elapsed time since last audio submit
    uint64_t now_us = time_us_64();
    uint32_t elapsed_us;
    if (first_audio_frame || last_audio_time_us == 0) {
        elapsed_us = 16667;  // Assume 60 FPS for first frame
        first_audio_frame = false;
    } else {
        elapsed_us = (uint32_t)(now_us - last_audio_time_us);
        // Clamp to reasonable range (10ms to 40ms = 100 to 25 FPS)
        if (elapsed_us < 10000) elapsed_us = 10000;
        if (elapsed_us > 40000) elapsed_us = 40000;
    }
    last_audio_time_us = now_us;
    
    // Memory barrier to ensure we see Core 0's writes
    __dmb();
    
    // Read buffer pointers and counts ONCE with local copies
    int16_t *ym_buffer = audio_read_ym2612;
    int16_t *sn_buffer = audio_read_sn76489;
    int ym_samples = saved_ym_samples;
    int sn_samples = saved_sn_samples;
    
    // Memory barrier after reading shared data
    __dmb();
    
    // Sanity check: make sure sample counts are reasonable
    if (ym_samples < 0 || ym_samples > AUDIO_BUFFER_SAMPLES) ym_samples = 0;
    if (sn_samples < 0 || sn_samples > AUDIO_BUFFER_SAMPLES) sn_samples = 0;
    
    int available = (ym_samples > sn_samples) ? ym_samples : sn_samples;
    if (available > AUDIO_BUFFER_SAMPLES) available = AUDIO_BUFFER_SAMPLES;
    
    if (!audio_enabled || available == 0 || !ym_buffer || !sn_buffer) {
        // Output silence - fade to zero to avoid click
        for (int i = 0; i < TARGET_SAMPLES_NTSC; i++) {
            int16_t sample = (lpf_state * 250) >> 8;
            lpf_state = sample;
            mixed_buffer[i * 2] = sample;
            mixed_buffer[i * 2 + 1] = sample;
        }
        i2s_dma_write_count(&i2s_config, mixed_buffer, TARGET_SAMPLES_NTSC);
        return;
    }
    
    // PHASE 1: Mix both sound chips into premix_buffer (mono)
    for (int i = 0; i < available; i++) {
        int32_t mixed = 0;
        if (ym2612_enabled && i < ym_samples) mixed += ym_buffer[i];
        if (sn76489_enabled && i < sn_samples) mixed += sn_buffer[i];
        mixed = (mixed * master_volume) >> 7;
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        premix_buffer[i] = (int16_t)mixed;
    }
    
    // PHASE 2: Time-stretch to 888 samples using nearest-neighbor (FAST)
    // This ensures correct playback speed regardless of emulation FPS
    // At 60 FPS: available=888, elapsed=16.67ms -> 1:1 mapping
    // At 40 FPS: available=888, elapsed=25ms -> stretch 888 to fill 888 output
    //            (we always output 888 samples, but they represent 'elapsed' time)
    // 
    // The key: we scale the READ position, not the write.
    // step = available / 888 (in 16.16 fixed point)
    // When FPS is slow, step < 1.0, so we repeat samples (stretch)
    // When FPS is fast, step > 1.0, so we skip samples (compress)
    
    uint32_t step = ((uint32_t)available << 16) / TARGET_SAMPLES_NTSC;
    uint32_t pos = 0;
    int16_t last_sample = premix_buffer[0];
    
    for (int i = 0; i < TARGET_SAMPLES_NTSC; i++) {
        uint32_t idx = pos >> 16;
        if (idx >= (uint32_t)available) idx = available - 1;
        last_sample = premix_buffer[idx];
        mixed_buffer[i * 2] = last_sample;
        mixed_buffer[i * 2 + 1] = last_sample;
        pos += step;
    }
    
    // STARTUP MUTE: Output pure silence for first N frames
    if (startup_frame_counter < STARTUP_FADE_FRAMES) {
        memset(mixed_buffer, 0, TARGET_SAMPLES_NTSC * 2 * sizeof(int16_t));
        startup_frame_counter++;
    }
    
    last_frame_sample = last_sample;
    audio_frame_count++;
    
    i2s_dma_write_count(&i2s_config, mixed_buffer, TARGET_SAMPLES_NTSC);
}

void audio_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 128) volume = 128;
    master_volume = volume;
}

int audio_get_volume(void) {
    return master_volume;
}

void audio_set_enabled(bool enabled) {
    audio_enabled = enabled;
}

bool audio_is_enabled(void) {
    return audio_enabled;
}

void audio_flush_silence(void) {
    // Send a few frames of silence to clear the DMA buffer
    // This prevents the last audio sample from repeating
    if (!audio_initialized) return;
    
    for (int frame = 0; frame < 3; frame++) {
        for (int i = 0; i < TARGET_SAMPLES_NTSC * 2; i++) {
            mixed_buffer[i] = 0;
        }
        i2s_dma_write_count(&i2s_config, mixed_buffer, TARGET_SAMPLES_NTSC);
    }
}

void audio_debug_buffer_values(void) {
#if ENABLE_LOGGING
    printf("Audio: ym=%d sn=%d\n", ym2612_index, sn76489_index);
#endif
}

i2s_config_t* audio_get_i2s_config(void) {
    return &i2s_config;
}
