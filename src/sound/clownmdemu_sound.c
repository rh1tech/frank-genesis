/*
 * clownmdemu sound wrapper for frank-genesis
 * 
 * Alternative sound system using clownmdemu's accurate FM/PSG implementation.
 * This provides cycle-accurate YM2612 and SN76489 emulation.
 */

#include "clownmdemu_sound.h"

#include <string.h>
#include <stdio.h>

// Use C99 integers for clowncommon compatibility
#define CC_USE_C99_INTEGERS

// Include clownmdemu FM and PSG
#include "fm.h"
#include "psg.h"

//=============================================================================
// Constants - Genesis timing
//=============================================================================

// NTSC Master clock: 53.693175 MHz
#define MASTER_CLOCK_NTSC 53693175

// M68K clock divider: 7
#define M68K_CLOCK_DIVIDER 7
#define M68K_CLOCK_NTSC (MASTER_CLOCK_NTSC / M68K_CLOCK_DIVIDER)

// Z80 clock divider: 15  
#define Z80_CLOCK_DIVIDER 15
#define Z80_CLOCK_NTSC (MASTER_CLOCK_NTSC / Z80_CLOCK_DIVIDER)

// FM sample rate divider (from fm.h): PRESCALER * CHANNELS * OPERATORS = 6 * 6 * 4 = 144
// FM sample rate: M68K_CLOCK / 144 ≈ 53267 Hz
#define FM_SAMPLE_RATE_NTSC (M68K_CLOCK_NTSC / FM_SAMPLE_RATE_DIVIDER)

// PSG sample rate: Z80_CLOCK / 16 ≈ 223722 Hz
#define PSG_SAMPLE_RATE_DIVIDER_PSG 16
#define PSG_SAMPLE_RATE_NTSC (Z80_CLOCK_NTSC / PSG_SAMPLE_RATE_DIVIDER_PSG)

// Target output sample rate (matches I2S)
#define OUTPUT_SAMPLE_RATE 53280

// Samples per frame at 60fps
#define SAMPLES_PER_FRAME 888

// M68K cycles per frame (NTSC)
#define M68K_CYCLES_PER_FRAME 127137  // (M68K_CLOCK_NTSC / 60.27fps)

//=============================================================================
// State
//=============================================================================

static FM fm_state;
static PSG psg_state;
static bool clown_initialized = false;

// Temporary buffers for sound generation
static int16_t __attribute__((aligned(4))) fm_buffer[SAMPLES_PER_FRAME * 2];  // Stereo
static int16_t __attribute__((aligned(4))) psg_buffer[SAMPLES_PER_FRAME];     // Mono

// Output buffer that matches gwenesis API
static int16_t __attribute__((aligned(4))) clown_output_buffer[SAMPLES_PER_FRAME * 2];

// Cycle accumulators for accurate timing
static uint32_t fm_leftover_cycles = 0;
static uint32_t psg_leftover_cycles = 0;

// Track last frame cycles for update
static uint32_t last_frame_cycles = 0;

//=============================================================================
// Initialization
//=============================================================================

void clown_sound_init(void) {
    // Initialize FM with default configuration
    FM_Configuration fm_config = {
        .fm_channels_disabled = {false, false, false, false, false, false},
        .dac_channel_disabled = false,
        .ladder_effect_disabled = false  // Enable ladder effect for authentic sound
    };
    FM_Initialise(&fm_state, &fm_config);
    
    // Initialize PSG with default configuration  
    PSG_Configuration psg_config = {
        .tone_disabled = {false, false, false},
        .noise_disabled = false
    };
    PSG_Initialise(&psg_state, &psg_config);
    
    fm_leftover_cycles = 0;
    psg_leftover_cycles = 0;
    last_frame_cycles = 0;
    clown_initialized = true;
    
#ifdef ENABLE_LOGGING
    printf("clownmdemu sound: initialized (FM rate ~%d Hz)\n", FM_SAMPLE_RATE_NTSC);
#endif
}

void clown_sound_reset(void) {
    // Re-initialize both chips
    FM_Configuration fm_config = {
        .fm_channels_disabled = {false, false, false, false, false, false},
        .dac_channel_disabled = false,
        .ladder_effect_disabled = false
    };
    FM_Initialise(&fm_state, &fm_config);
    
    PSG_Configuration psg_config = {
        .tone_disabled = {false, false, false},
        .noise_disabled = false
    };
    PSG_Initialise(&psg_state, &psg_config);
    
    fm_leftover_cycles = 0;
    psg_leftover_cycles = 0;
    last_frame_cycles = 0;
}

//=============================================================================
// Register writes
//=============================================================================

static int ym2612_write_count = 0;
static int keyon_count = 0;
static uint8_t last_address = 0;
static int fm_log_count = 0;

void clown_ym2612_write(uint8_t addr, uint8_t data) {
    if (!clown_initialized) return;
    
    ym2612_write_count++;
    
    // addr: 0 = port 0 address, 1 = port 0 data, 2 = port 1 address, 3 = port 1 data
    uint8_t port = (addr >> 1) & 1;
    
    if (addr & 1) {
        // Data write
        // Log first 30 writes
        if (fm_log_count < 30) {
            printf("[FM] port%d reg=0x%02X data=0x%02X\n", port, last_address, data);
            fm_log_count++;
        }
        // Track key-on events (register $28)
        if (last_address == 0x28 && (data & 0xF0)) {
            keyon_count++;
        }
        FM_DoData(&fm_state, data);
    } else {
        // Address write
        last_address = data;
        FM_DoAddress(&fm_state, port, data);
    }
}

uint8_t clown_ym2612_read(void) {
    if (!clown_initialized) return 0;
    // Return status register
    return fm_state.state.status;
}

void clown_psg_write(uint8_t data) {
    if (!clown_initialized) return;
    static int psg_writes = 0;
    psg_writes++;
    if (psg_writes <= 20) {
        printf("[PSG] write: 0x%02X\n", data);
    }
    PSG_DoCommand(&psg_state, data);
}

//=============================================================================
// Main update function
//=============================================================================

int clown_sound_update(int16_t *buffer, uint32_t m68k_cycles) {
    if (!clown_initialized) return 0;
    
    // Calculate how many FM samples to generate based on cycles
    // FM runs at M68K clock rate, outputs samples at M68K_CLOCK / FM_SAMPLE_RATE_DIVIDER
    uint32_t total_fm_cycles = fm_leftover_cycles + m68k_cycles;
    uint32_t fm_samples = total_fm_cycles / FM_SAMPLE_RATE_DIVIDER;
    fm_leftover_cycles = total_fm_cycles % FM_SAMPLE_RATE_DIVIDER;
    
    // Cap to buffer size
    if (fm_samples > SAMPLES_PER_FRAME) {
        fm_samples = SAMPLES_PER_FRAME;
    }
    
    if (fm_samples == 0) {
        return 0;
    }
    
    // Clear FM buffer
    memset(fm_buffer, 0, fm_samples * 2 * sizeof(int16_t));
    
    // Generate FM samples (stereo)
    FM_OutputSamples(&fm_state, fm_buffer, fm_samples);
    
    // Clear PSG buffer
    memset(psg_buffer, 0, fm_samples * sizeof(int16_t));
    
    // Generate PSG samples - use fm_samples count for simpler mixing
    // PSG_Update adds to existing buffer
    PSG_Update(&psg_state, psg_buffer, fm_samples);
    
    // Mix FM (stereo) and PSG (mono) to output buffer
    // FM is already at correct volume from FM_OutputSamples
    // PSG needs to be added to both channels
    for (uint32_t i = 0; i < fm_samples; i++) {
        // Get samples
        int32_t fm_l = fm_buffer[i * 2];
        int32_t fm_r = fm_buffer[i * 2 + 1];
        int32_t psg = psg_buffer[i] / 4;  // PSG is quite loud, reduce it
        
        // Mix
        int32_t out_l = fm_l + psg;
        int32_t out_r = fm_r + psg;
        
        // Clamp
        if (out_l > 32767) out_l = 32767;
        if (out_l < -32768) out_l = -32768;
        if (out_r > 32767) out_r = 32767;
        if (out_r < -32768) out_r = -32768;
        
        buffer[i * 2] = (int16_t)out_l;
        buffer[i * 2 + 1] = (int16_t)out_r;
    }
    
    return fm_samples;
}

int clown_sound_get_samples_per_frame(void) {
    return SAMPLES_PER_FRAME;
}

//=============================================================================
// Compatibility layer for gwenesis bus
// These functions allow the existing bus code to call clownmdemu instead
//=============================================================================

#ifdef USE_CLOWNMDEMU_SOUND

// Buffer pointers that match gwenesis API
int16_t *clown_ym2612_buffer_ptr = clown_output_buffer;
int16_t *clown_sn76489_buffer_ptr = clown_output_buffer;

// Sample indices for gwenesis compatibility
volatile int clown_ym2612_index = 0;
volatile int clown_sn76489_index = 0;
volatile int clown_ym2612_clock = 0;
volatile int clown_sn76489_clock = 0;

// YM2612Write replacement - called from bus
void YM2612Write_clown(unsigned int addr, unsigned int value, int cycles) {
    (void)cycles;  // clownmdemu handles timing internally
    clown_ym2612_write((uint8_t)addr, (uint8_t)value);
}

// YM2612Read replacement
unsigned int YM2612Read_clown(int cycles) {
    (void)cycles;
    return clown_ym2612_read();
}

// SN76489 Write replacement
void gwenesis_SN76489_Write_clown(int data, int cycles) {
    (void)cycles;
    clown_psg_write((uint8_t)data);
}

// Run sound chips and fill buffer - called at end of frame
static int debug_frame = 0;
void clown_sound_run_frame(int m68k_cycles) {
    int samples = clown_sound_update(clown_output_buffer, m68k_cycles);
    clown_ym2612_index = samples;
    clown_sn76489_index = samples;
    
    // Debug: print every 60 frames
    debug_frame++;
    if ((debug_frame % 60) == 0) {
        int32_t max_out = 0, max_fm = 0, max_psg = 0;
        for (int i = 0; i < samples * 2; i++) {
            int32_t s = clown_output_buffer[i];
            if (s < 0) s = -s;
            if (s > max_out) max_out = s;
            s = fm_buffer[i];
            if (s < 0) s = -s;
            if (s > max_fm) max_fm = s;
        }
        for (int i = 0; i < samples; i++) {
            int32_t s = psg_buffer[i];
            if (s < 0) s = -s;
            if (s > max_psg) max_psg = s;
        }
        printf("[CLOWN] fr=%d FM=%ld PSG=%ld OUT=%ld wr=%d keyon=%d\n",
               debug_frame, max_fm, max_psg, max_out, ym2612_write_count, keyon_count);
        ym2612_write_count = 0;
        keyon_count = 0;
    }
}

#endif // USE_CLOWNMDEMU_SOUND
