/*
 * clownmdemu sound wrapper for frank-genesis
 * 
 * Alternative sound system using clownmdemu's accurate FM/PSG implementation.
 * Enable with SOUND_ENGINE=CLOWNMDEMU in CMake.
 */
#ifndef CLOWNMDEMU_SOUND_H
#define CLOWNMDEMU_SOUND_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Initialization
//=============================================================================

// Initialize the clownmdemu sound system
void clown_sound_init(void);

// Reset sound chips
void clown_sound_reset(void);

//=============================================================================
// Register writes (called from bus emulation)
//=============================================================================

// YM2612 write (addr: 0-3 for port/data pairs)
void clown_ym2612_write(uint8_t addr, uint8_t data);

// YM2612 read (returns status register)
uint8_t clown_ym2612_read(void);

// SN76489/PSG write
void clown_psg_write(uint8_t data);

//=============================================================================
// Audio generation (called from audio driver)
//=============================================================================

// Generate audio samples for one frame
// buffer: stereo output buffer (interleaved L/R, 16-bit signed)
// cycles: number of M68K cycles this frame (for accurate timing)
// Returns: number of samples generated
int clown_sound_update(int16_t *buffer, uint32_t cycles);

// Get expected samples per frame (for buffer sizing)
int clown_sound_get_samples_per_frame(void);

//=============================================================================
// Compatibility layer for gwenesis bus
// When USE_CLOWNMDEMU_SOUND is defined, these replace the gwenesis functions
//=============================================================================

#ifdef USE_CLOWNMDEMU_SOUND

// Buffer pointers (for audio.c compatibility)
extern int16_t *clown_ym2612_buffer_ptr;
extern int16_t *clown_sn76489_buffer_ptr;

// Sample indices (for audio.c compatibility)
extern volatile int clown_ym2612_index;
extern volatile int clown_sn76489_index;
extern volatile int clown_ym2612_clock;
extern volatile int clown_sn76489_clock;

// Replacement functions for bus
void YM2612Write_clown(unsigned int addr, unsigned int value, int cycles);
unsigned int YM2612Read_clown(int cycles);
void gwenesis_SN76489_Write_clown(int data, int cycles);

// Run sound for a frame (generates samples)
void clown_sound_run_frame(int m68k_cycles);

#endif // USE_CLOWNMDEMU_SOUND

#ifdef __cplusplus
}
#endif

#endif // CLOWNMDEMU_SOUND_H
