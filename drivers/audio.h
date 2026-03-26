/*
 * frank-genesis - Simple DMA-based I2S Audio Driver for RP2350
 * Based on pico-megadrive audio driver by Vincent Mistler
 * 
 * This is a much simpler approach than pico_audio_i2s:
 * - Direct PIO + DMA without the complex audio_buffer system
 * - Single DMA buffer with blocking wait
 * - Lower overhead, better for real-time emulation
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <hardware/pio.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>

// Audio sample rate - Genesis NTSC runs at ~53267 Hz
#define AUDIO_SAMPLE_RATE 53280  // 888 samples/frame × 60 fps

// Audio buffer size - enough for both NTSC (~888) and PAL (~1061) with headroom
#define AUDIO_BUFFER_SAMPLES 1120

// I2S configuration structure
typedef struct {
    uint32_t sample_freq;        
    uint16_t channel_count; 
    uint8_t  data_pin;
    uint8_t  clock_pin_base;
    PIO      pio;
    uint8_t  sm; 
    uint8_t  dma_channel;
    uint16_t dma_trans_count;
    uint16_t *dma_buf;
    uint8_t  volume;  // 0 = max volume, higher = quieter (shift amount)
} i2s_config_t;

// Get default I2S configuration
i2s_config_t i2s_get_default_config(void);

// Initialize I2S with the given configuration
void i2s_init(i2s_config_t *config);

// Write samples to I2S via DMA (non-blocking after first call)
// samples: pointer to stereo samples (interleaved L/R as 32-bit words)
void i2s_dma_write(i2s_config_t *config, const int16_t *samples);

// Adjust volume (0 = loudest, 16 = quietest)
void i2s_volume(i2s_config_t *config, uint8_t volume);
void i2s_increase_volume(i2s_config_t *config);
void i2s_decrease_volume(i2s_config_t *config);

//=============================================================================
// High-level audio API (wraps I2S)
//=============================================================================

// Initialize audio system
bool audio_init(void);

// Shutdown audio system
void audio_shutdown(void);

// Check if audio is initialized
bool audio_is_initialized(void);

// Submit mixed audio buffer to I2S (call once per frame)
// This mixes YM2612 and SN76489 outputs and sends to I2S
void audio_submit(void);

// Set master volume (0-128)
void audio_set_volume(int volume);

// Get current master volume
int audio_get_volume(void);

// Enable/disable audio
void audio_set_enabled(bool enabled);

// Check if audio is enabled
bool audio_is_enabled(void);

// Flush silence to audio buffer to stop any repeating sound
void audio_flush_silence(void);

// Debug function
void audio_debug_buffer_values(void);

// Get the I2S config for direct access (for Core 1 loop)
i2s_config_t* audio_get_i2s_config(void);

#endif // AUDIO_H
