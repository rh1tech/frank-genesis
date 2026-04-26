/*
 * Settings Menu - Runtime configuration for frank-genesis
 * Allows user to adjust CPU/PSRAM frequencies, audio, and display options
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

// Settings structure stored in genesis/settings.ini
typedef struct {
    uint16_t cpu_freq;      // RP2350 frequency: 504 (default), 378
    uint16_t psram_freq;    // PSRAM frequency: 166 (default), 133
    bool fm_sound;          // FM (YM2612) sound: true (default), false
    bool dac_sound;         // DAC sound: true (default), false
    bool crt_effect;        // CRT scanlines: false (default), true
    uint8_t crt_dim;        // CRT dim percentage: 10-90, default 60
    bool z80_enabled;       // Z80 CPU: true (default), false
    bool audio_enabled;     // Master audio: true (default), false
    uint8_t channel_mask;   // Channel enable bitmask: bits 0-5 = FM 1-6, bit 6 = PSG
    uint8_t frameskip;      // Frameskip level: 0=none, 1=low, 2=medium, 3=high (default), 4=extreme
    uint8_t gamepad2_mode;  // Gamepad 2 mode: 0=NES, 1=keyboard, 2=USB, 3=disabled

    // File-browser persistence: last visited directory and selected file.
    // Used so the browser reopens where the user left off across cold boots.
    char browser_path[128];
    char browser_file[96];
} settings_t;

// Gamepad 2 mode values
#define GAMEPAD2_MODE_NES      0  // Second NES/SNES gamepad (default)
#define GAMEPAD2_MODE_KEYBOARD 1  // Keyboard controls P2 instead of P1
#define GAMEPAD2_MODE_USB      2  // USB gamepad controls P2, NES controls P1
#define GAMEPAD2_MODE_DISABLED 3  // Gamepad 2 disabled

// Helper macros for channel_mask
#define CHANNEL_ENABLED(mask, ch) (((mask) >> (ch)) & 1)
#define CHANNEL_SET(mask, ch, en) ((en) ? ((mask) | (1 << (ch))) : ((mask) & ~(1 << (ch))))

// Global settings instance (loaded at startup)
extern settings_t g_settings;

// Settings menu result
typedef enum {
    SETTINGS_RESULT_CANCEL,         // User pressed cancel
    SETTINGS_RESULT_SAVE_RESTART,   // Save settings and restart
    SETTINGS_RESULT_RESTART,        // Restart without saving
} settings_result_t;

/**
 * Load settings from SD card (genesis/settings.ini)
 * If file doesn't exist, uses defaults
 */
void settings_load(void);

/**
 * Save current settings to SD card (genesis/settings.ini)
 * @return true if saved successfully
 */
bool settings_save(void);

/**
 * Apply settings that can be changed at runtime
 * (audio enable/disable flags)
 */
void settings_apply_runtime(void);

/**
 * Display settings menu and wait for user interaction
 * @param screen_buffer Pointer to screen buffer (320x240 8-bit indexed)
 * @return Result indicating what action to take
 */
settings_result_t settings_menu_show(uint8_t *screen_buffer);

/**
 * Display settings menu with screen save/restore on cancel
 * @param screen_buffer Pointer to screen buffer (320x240 8-bit indexed)
 * @param saved_screen Buffer to save current screen (must be 320*240 bytes), or NULL to skip
 * @return Result indicating what action to take
 */
settings_result_t settings_menu_show_with_restore(uint8_t *screen_buffer, uint8_t *saved_screen);

/**
 * Check if Start+Select is pressed (call this during emulation loop)
 * @return true if settings menu should be opened
 */
bool settings_check_hotkey(void);

#endif // SETTINGS_H
