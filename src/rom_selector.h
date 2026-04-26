/*
 * ROM Selector - Start screen for frank-genesis
 * Welcome screen + file browser for Genesis/Megadrive ROMs on SD card.
 */
#ifndef ROM_SELECTOR_H
#define ROM_SELECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Maximum length of ROM filename (including path)
#define MAX_ROM_PATH 160

// Result of the initial /genesis scan. Used to tailor the no-ROMs notice
// and decide whether to surface a fallback file browser at SD root.
typedef enum {
    ROM_SCAN_OK = 0,         // At least one Genesis ROM found in /genesis
    ROM_SCAN_NO_SD,          // SD mount failed
    ROM_SCAN_NO_GENESIS_DIR, // SD mounted but /genesis directory missing
    ROM_SCAN_NO_ROMS,        // /genesis exists but has no Genesis ROMs
} rom_scan_result_t;

/**
 * Display the welcome/splash screen.
 * Shows starfield + logo + version. Waits for a button press or auto-
 * continues after ~10 seconds.
 * @param screen_buffer Pointer to the 320x240 8-bit indexed framebuffer
 */
void welcome_screen_show(uint8_t *screen_buffer);

/**
 * Display "no ROMs" notice with instructions, then wait for user input
 * before falling back to the file browser. Call only when the /genesis
 * scan reported NO_GENESIS_DIR or NO_ROMS.
 * @param screen_buffer Pointer to the 320x240 8-bit indexed framebuffer
 * @param result Scan result used to tailor the message
 */
void rom_selector_no_roms_notice(uint8_t *screen_buffer, rom_scan_result_t result);

/**
 * Display ROM selection screen and wait for user to select a ROM.
 * Now implemented as a file browser (directory navigation, paging,
 * persistence across boots). Starts at /genesis by default, or at the
 * last visited directory if one was saved.
 * @param selected_rom_path Buffer to store the selected ROM path
 * @param buffer_size Size of the buffer
 * @param screen_buffer Pointer to the screen buffer (320x240 8-bit indexed)
 * @return true if ROM was selected, false if user canceled
 */
bool rom_selector_show(char *selected_rom_path, size_t buffer_size, uint8_t *screen_buffer);

/**
 * Display SD card error screen (blocks forever).
 * @param screen_buffer Pointer to the screen buffer (320x240 8-bit indexed)
 * @param error_code The FRESULT error code from f_mount
 */
void rom_selector_show_sd_error(uint8_t *screen_buffer, int error_code);

#endif // ROM_SELECTOR_H
