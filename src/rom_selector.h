/*
 * ROM Selector - Start screen for frank-genesis
 * Allows user to browse and select Genesis/Megadrive ROMs from SD card
 */
#ifndef ROM_SELECTOR_H
#define ROM_SELECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Maximum length of ROM filename (including path)
#define MAX_ROM_PATH 128

/**
 * Display ROM selection screen and wait for user to select a ROM
 * @param selected_rom_path Buffer to store the selected ROM path
 * @param buffer_size Size of the buffer
 * @param screen_buffer Pointer to the screen buffer (320x240 8-bit indexed)
 * @return true if ROM was selected, false if user canceled
 */
bool rom_selector_show(char *selected_rom_path, size_t buffer_size, uint8_t *screen_buffer);

/**
 * Display SD card error screen (blocks forever)
 * @param screen_buffer Pointer to the screen buffer (320x240 8-bit indexed)
 * @param error_code The FRESULT error code from f_mount
 */
void rom_selector_show_sd_error(uint8_t *screen_buffer, int error_code);

#endif // ROM_SELECTOR_H
