/*
 * Settings Menu Implementation
 * Uses the same 5x7 font as ROM selector for consistency
 */
#include "settings.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "HDMI.h"
#include "board_config.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "nespad/nespad.h"
#include "ps2kbd/ps2kbd_wrapper.h"
#include "audio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// USB HID gamepad support
#ifdef USB_HID_ENABLED
#include "usbhid/usbhid.h"
#endif

// Screen dimensions
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

// Font constants (same as ROM selector)
#define FONT_WIDTH 6    // 5px glyph + 1px spacing
#define FONT_HEIGHT 7
#define LINE_HEIGHT 10  // Compact spacing for settings

// UI layout
#define MENU_TITLE_Y 20
#define MENU_START_Y 40
#define MENU_X 40
#define VALUE_X 200  // X position for values

// Colors (8-bit 3-3-2 RGB palette)
#define COLOR_BLACK 1    // Near-black (not 0 for HDMI compatibility)
#define COLOR_WHITE 63   // Bright white
#define COLOR_YELLOW 48  // Yellow for highlight
#define COLOR_GRAY 42    // Medium gray
#define COLOR_RED 32     // Red

// Menu items
typedef enum {
    MENU_CPU_FREQ,
    MENU_PSRAM_FREQ,
    MENU_Z80,
    MENU_AUDIO,
    MENU_FM_SOUND,
    MENU_CHANNELS,
    MENU_CRT_EFFECT,
    MENU_CRT_DIM,
    MENU_FRAMESKIP,
    MENU_GAMEPAD2,
    MENU_SEPARATOR,  // Visual separator
    MENU_SAVE_RESTART,
    MENU_RESTART,
    MENU_CANCEL,
    MENU_ITEM_COUNT
} menu_item_t;

// Channel submenu items
typedef enum {
    CHAN_FM1,
    CHAN_FM2,
    CHAN_FM3,
    CHAN_FM4,
    CHAN_FM5,
    CHAN_FM6,
    CHAN_PSG,
    CHAN_SEPARATOR,
    CHAN_BACK,
    CHAN_ITEM_COUNT
} channel_menu_item_t;

// Global settings instance
settings_t g_settings = {
    .cpu_freq = 504,
    .psram_freq = 166,
    .fm_sound = true,
    .dac_sound = true,
    .crt_effect = false,
    .crt_dim = 60,
    .z80_enabled = true,
    .audio_enabled = true,
    .channel_mask = 0x7F,  // All 7 channels enabled (bits 0-6)
    .frameskip = 3,  // Default: high (30fps)
    .gamepad2_mode = GAMEPAD2_MODE_NES,  // Default: NES gamepad 2
    .browser_path = {0},
    .browser_file = {0}
};

// Frameskip level names
static const char* frameskip_names[] = {"NONE", "LOW", "MEDIUM", "HIGH", "EXTREME"};
#define FRAMESKIP_MAX_LEVEL 4

// Gamepad 2 mode names
static const char* gamepad2_mode_names[] = {"NES", "KEYBOARD", "USB", "DISABLED"};
#define GAMEPAD2_MODE_MAX 3

// Local copy for editing
static settings_t edit_settings;

// CRT dim values
static const uint8_t crt_dim_values[] = {10, 20, 30, 40, 50, 60, 70, 80, 90};
#define CRT_DIM_COUNT (sizeof(crt_dim_values) / sizeof(crt_dim_values[0]))

// 5x7 font glyphs (copied from rom_selector.c)
static const uint8_t *glyph_5x7(char ch) {
    static const uint8_t glyph_space[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t glyph_dot[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    static const uint8_t glyph_hyphen[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static const uint8_t glyph_underscore[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};
    static const uint8_t glyph_colon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
    static const uint8_t glyph_slash[7] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00};
    static const uint8_t glyph_percent[7] = {0x11, 0x02, 0x04, 0x08, 0x11, 0x00, 0x00};
    static const uint8_t glyph_lparen[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
    static const uint8_t glyph_rparen[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
    static const uint8_t glyph_less[7] = {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02};
    static const uint8_t glyph_greater[7] = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08};
    
    static const uint8_t glyph_0[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    static const uint8_t glyph_1[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static const uint8_t glyph_2[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    static const uint8_t glyph_3[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    static const uint8_t glyph_4[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    static const uint8_t glyph_5[7] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    static const uint8_t glyph_6[7] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_7[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    static const uint8_t glyph_8[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_9[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
    
    static const uint8_t glyph_A[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t glyph_B[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    static const uint8_t glyph_C[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    static const uint8_t glyph_D[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    static const uint8_t glyph_E[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static const uint8_t glyph_F[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t glyph_G[7] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_H[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t glyph_I[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    static const uint8_t glyph_J[7] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
    static const uint8_t glyph_K[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    static const uint8_t glyph_L[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    static const uint8_t glyph_M[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    static const uint8_t glyph_N[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static const uint8_t glyph_O[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_P[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t glyph_Q[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    static const uint8_t glyph_R[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    static const uint8_t glyph_S[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static const uint8_t glyph_T[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t glyph_U[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_V[7] = {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
    static const uint8_t glyph_W[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
    static const uint8_t glyph_X[7] = {0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11};
    static const uint8_t glyph_Y[7] = {0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t glyph_Z[7] = {0x1F, 0x02, 0x04, 0x08, 0x10, 0x10, 0x1F};
    
    int c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    
    switch (c) {
        case ' ': return glyph_space;
        case '.': return glyph_dot;
        case '-': return glyph_hyphen;
        case '_': return glyph_underscore;
        case ':': return glyph_colon;
        case '/': return glyph_slash;
        case '%': return glyph_percent;
        case '(': return glyph_lparen;
        case ')': return glyph_rparen;
        case '<': return glyph_less;
        case '>': return glyph_greater;
        case '0': return glyph_0;
        case '1': return glyph_1;
        case '2': return glyph_2;
        case '3': return glyph_3;
        case '4': return glyph_4;
        case '5': return glyph_5;
        case '6': return glyph_6;
        case '7': return glyph_7;
        case '8': return glyph_8;
        case '9': return glyph_9;
        case 'A': return glyph_A;
        case 'B': return glyph_B;
        case 'C': return glyph_C;
        case 'D': return glyph_D;
        case 'E': return glyph_E;
        case 'F': return glyph_F;
        case 'G': return glyph_G;
        case 'H': return glyph_H;
        case 'I': return glyph_I;
        case 'J': return glyph_J;
        case 'K': return glyph_K;
        case 'L': return glyph_L;
        case 'M': return glyph_M;
        case 'N': return glyph_N;
        case 'O': return glyph_O;
        case 'P': return glyph_P;
        case 'Q': return glyph_Q;
        case 'R': return glyph_R;
        case 'S': return glyph_S;
        case 'T': return glyph_T;
        case 'U': return glyph_U;
        case 'V': return glyph_V;
        case 'W': return glyph_W;
        case 'X': return glyph_X;
        case 'Y': return glyph_Y;
        case 'Z': return glyph_Z;
        default: return glyph_space;
    }
}

// Draw a single character
static void draw_char(uint8_t *screen, int x, int y, char ch, uint8_t color) {
    const uint8_t *rows = glyph_5x7(ch);
    for (int row = 0; row < FONT_HEIGHT; ++row) {
        int yy = y + row;
        if (yy < 0 || yy >= SCREEN_HEIGHT) continue;
        uint8_t bits = rows[row];
        for (int col = 0; col < 5; ++col) {
            int xx = x + col;
            if (xx < 0 || xx >= SCREEN_WIDTH) continue;
            if (bits & (1u << (4 - col))) {
                screen[yy * SCREEN_WIDTH + xx] = color;
            }
        }
    }
}

// Draw text string
static void draw_text(uint8_t *screen, int x, int y, const char *text, uint8_t color) {
    for (const char *p = text; *p; ++p) {
        draw_char(screen, x, y, *p, color);
        x += FONT_WIDTH;
    }
}

// Fill rectangle
static void fill_rect(uint8_t *screen, int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    
    if (color == 0) color = 1;  // Avoid index 0 for HDMI compatibility
    
    for (int yy = y; yy < y + h; ++yy) {
        memset(&screen[yy * SCREEN_WIDTH + x], color, (size_t)w);
    }
}

// Draw horizontal line
static void draw_hline(uint8_t *screen, int x, int y, int w, uint8_t color) {
    if (y < 0 || y >= SCREEN_HEIGHT) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
    if (w <= 0) return;
    memset(&screen[y * SCREEN_WIDTH + x], color, (size_t)w);
}

// Get index of current CRT dim value
static int get_crt_dim_index(uint8_t dim) {
    for (int i = 0; i < (int)CRT_DIM_COUNT; i++) {
        if (crt_dim_values[i] == dim) return i;
    }
    return 5;  // Default to 60%
}

// Get menu item label
static const char* get_menu_label(menu_item_t item) {
    switch (item) {
        case MENU_CPU_FREQ:     return "RP2350 FREQ";
        case MENU_PSRAM_FREQ:   return "PSRAM FREQ";
        case MENU_Z80:          return "Z80";
        case MENU_AUDIO:        return "AUDIO";
        case MENU_FM_SOUND:     return "FM SOUND";
        case MENU_CHANNELS:     return "CHANNELS";
        case MENU_CRT_EFFECT:   return "CRT EFFECT";
        case MENU_CRT_DIM:      return "CRT DIM";
        case MENU_FRAMESKIP:    return "FRAMESKIP";
        case MENU_GAMEPAD2:     return "GAMEPAD 2";
        case MENU_SEPARATOR:    return "";
        case MENU_SAVE_RESTART: return "SAVE AND RESTART";
        case MENU_RESTART:      return "RESTART WITHOUT SAVING";
        case MENU_CANCEL:       return "CANCEL";
        default:                return "";
    }
}

// Get menu item value string
static void get_menu_value(menu_item_t item, char *buf, size_t size) {
    switch (item) {
        case MENU_CPU_FREQ:
            snprintf(buf, size, "< %d MHZ >", edit_settings.cpu_freq);
            break;
        case MENU_PSRAM_FREQ:
            snprintf(buf, size, "< %d MHZ >", edit_settings.psram_freq);
            break;
        case MENU_Z80:
            snprintf(buf, size, "< %s >", edit_settings.z80_enabled ? "ENABLED" : "DISABLED");
            break;
        case MENU_AUDIO:
            snprintf(buf, size, "< %s >", edit_settings.audio_enabled ? "ENABLED" : "DISABLED");
            break;
        case MENU_FM_SOUND:
            if (edit_settings.audio_enabled) {
                snprintf(buf, size, "< %s >", edit_settings.fm_sound ? "ON" : "OFF");
            } else {
                snprintf(buf, size, "---");
            }
            break;
        case MENU_CHANNELS:
            // Submenu - no value displayed here
            buf[0] = '\0';
            break;
        case MENU_CRT_EFFECT:
            snprintf(buf, size, "< %s >", edit_settings.crt_effect ? "ON" : "OFF");
            break;
        case MENU_CRT_DIM:
            if (edit_settings.crt_effect) {
                snprintf(buf, size, "< %d%% >", edit_settings.crt_dim);
            } else {
                snprintf(buf, size, "---");
            }
            break;
        case MENU_FRAMESKIP:
            snprintf(buf, size, "< %s >", frameskip_names[edit_settings.frameskip]);
            break;
        case MENU_GAMEPAD2:
            snprintf(buf, size, "< %s >", gamepad2_mode_names[edit_settings.gamepad2_mode]);
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

// Change setting value (left/right)
static void change_setting(menu_item_t item, int direction) {
    switch (item) {
        case MENU_CPU_FREQ:
            if (direction < 0 && edit_settings.cpu_freq == 504) {
                edit_settings.cpu_freq = 378;
            } else if (direction > 0 && edit_settings.cpu_freq == 378) {
                edit_settings.cpu_freq = 504;
            }
            break;
            
        case MENU_PSRAM_FREQ:
            if (direction < 0 && edit_settings.psram_freq == 166) {
                edit_settings.psram_freq = 133;
            } else if (direction > 0 && edit_settings.psram_freq == 133) {
                edit_settings.psram_freq = 166;
            }
            break;
            
        case MENU_Z80:
            edit_settings.z80_enabled = !edit_settings.z80_enabled;
            break;
            
        case MENU_AUDIO:
            edit_settings.audio_enabled = !edit_settings.audio_enabled;
            break;
            
        case MENU_FM_SOUND:
            if (edit_settings.audio_enabled) {
                edit_settings.fm_sound = !edit_settings.fm_sound;
            }
            break;
            
        case MENU_CHANNELS:
            // Handled separately - opens submenu
            break;
            
        case MENU_CRT_EFFECT:
            edit_settings.crt_effect = !edit_settings.crt_effect;
            break;
            
        case MENU_CRT_DIM:
            if (edit_settings.crt_effect) {
                int idx = get_crt_dim_index(edit_settings.crt_dim);
                if (direction < 0 && idx > 0) {
                    edit_settings.crt_dim = crt_dim_values[idx - 1];
                } else if (direction > 0 && idx < (int)CRT_DIM_COUNT - 1) {
                    edit_settings.crt_dim = crt_dim_values[idx + 1];
                }
            }
            break;
            
        case MENU_FRAMESKIP:
            if (direction < 0 && edit_settings.frameskip > 0) {
                edit_settings.frameskip--;
            } else if (direction > 0 && edit_settings.frameskip < FRAMESKIP_MAX_LEVEL) {
                edit_settings.frameskip++;
            }
            break;
            
        case MENU_GAMEPAD2:
            if (direction < 0 && edit_settings.gamepad2_mode > 0) {
                edit_settings.gamepad2_mode--;
            } else if (direction > 0 && edit_settings.gamepad2_mode < GAMEPAD2_MODE_MAX) {
                edit_settings.gamepad2_mode++;
            }
            break;
            
        default:
            break;
    }
}

// Check if menu item is selectable
static bool is_selectable(menu_item_t item) {
    if (item == MENU_SEPARATOR) return false;
    if (item == MENU_CRT_DIM && !edit_settings.crt_effect) return false;
    if (item == MENU_FM_SOUND && !edit_settings.audio_enabled) return false;
    if (item == MENU_CHANNELS && !edit_settings.audio_enabled) return false;
    return true;
}

// Get next selectable item
static int get_next_selectable(int current, int direction) {
    int next = current;
    do {
        next += direction;
        if (next < 0) next = MENU_ITEM_COUNT - 1;
        if (next >= MENU_ITEM_COUNT) next = 0;
    } while (!is_selectable((menu_item_t)next) && next != current);
    return next;
}

//=============================================================================
// Channel Submenu
//=============================================================================

static const char* get_channel_label(channel_menu_item_t item) {
    switch (item) {
        case CHAN_FM1:       return "FM CHANNEL 1";
        case CHAN_FM2:       return "FM CHANNEL 2";
        case CHAN_FM3:       return "FM CHANNEL 3";
        case CHAN_FM4:       return "FM CHANNEL 4";
        case CHAN_FM5:       return "FM CHANNEL 5";
        case CHAN_FM6:       return "FM CHANNEL 6 / DAC";
        case CHAN_PSG:       return "PSG";
        case CHAN_SEPARATOR: return "";
        case CHAN_BACK:      return "BACK";
        default:             return "";
    }
}

static void get_channel_value(channel_menu_item_t item, char *buf, size_t size) {
    switch (item) {
        case CHAN_FM1:
        case CHAN_FM2:
        case CHAN_FM3:
        case CHAN_FM4:
        case CHAN_FM5:
        case CHAN_FM6:
        case CHAN_PSG:
            snprintf(buf, size, "< %s >", CHANNEL_ENABLED(edit_settings.channel_mask, item) ? "ON" : "OFF");
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

static bool is_channel_selectable(channel_menu_item_t item) {
    return item != CHAN_SEPARATOR;
}

static int get_next_channel_selectable(int current, int direction) {
    int next = current;
    do {
        next += direction;
        if (next < 0) next = CHAN_ITEM_COUNT - 1;
        if (next >= CHAN_ITEM_COUNT) next = 0;
    } while (!is_channel_selectable((channel_menu_item_t)next) && next != current);
    return next;
}

static void draw_channel_menu(uint8_t *screen, int selected) {
    // Clear screen
    fill_rect(screen, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    
    // Draw title
    const char *title = "AUDIO CHANNELS";
    int title_width = (int)strlen(title) * FONT_WIDTH;
    int title_x = (SCREEN_WIDTH - title_width) / 2;
    draw_text(screen, title_x, MENU_TITLE_Y, title, COLOR_WHITE);
    
    // Draw separator under title
    draw_hline(screen, 40, MENU_TITLE_Y + FONT_HEIGHT + 4, SCREEN_WIDTH - 80, COLOR_GRAY);
    
    // Draw menu items
    int y = MENU_START_Y;
    char value_buf[32];
    
    for (int i = 0; i < CHAN_ITEM_COUNT; i++) {
        channel_menu_item_t item = (channel_menu_item_t)i;
        
        if (item == CHAN_SEPARATOR) {
            draw_hline(screen, 40, y + LINE_HEIGHT / 2, SCREEN_WIDTH - 80, COLOR_GRAY);
            y += LINE_HEIGHT;
            continue;
        }
        
        bool is_selected = (i == selected);
        bool is_enabled = is_channel_selectable(item);
        
        uint8_t label_color = is_selected ? COLOR_YELLOW : (is_enabled ? COLOR_WHITE : COLOR_GRAY);
        uint8_t value_color = is_selected ? COLOR_YELLOW : COLOR_WHITE;
        
        if (is_selected) {
            draw_text(screen, MENU_X - 12, y, ">", COLOR_YELLOW);
        }
        
        const char *label = get_channel_label(item);
        draw_text(screen, MENU_X, y, label, label_color);
        
        get_channel_value(item, value_buf, sizeof(value_buf));
        if (value_buf[0]) {
            int value_width = (int)strlen(value_buf) * FONT_WIDTH;
            int value_x = SCREEN_WIDTH - MENU_X - value_width;
            if (!is_enabled) value_color = COLOR_GRAY;
            draw_text(screen, value_x, y, value_buf, value_color);
        }
        
        y += LINE_HEIGHT;
    }
    
    // Help text
    const char *help = "A: TOGGLE  B: BACK";
    int help_width = (int)strlen(help) * FONT_WIDTH;
    int help_x = (SCREEN_WIDTH - help_width) / 2;
    draw_text(screen, help_x, SCREEN_HEIGHT - 20, help, COLOR_GRAY);
}

// Show channel submenu, returns true if user pressed Back
static bool show_channel_submenu(uint8_t *screen_buffer) {
    int selected = 0;
    uint32_t prev_buttons = 0;
    uint32_t hold_counter = 0;
    const uint32_t REPEAT_DELAY = 10;
    const uint32_t REPEAT_RATE = 3;
    
    draw_channel_menu(screen_buffer, selected);
    
    // Wait for A button release to prevent leaking into first menu item
    while (true) {
        nespad_read();
        if (!(nespad_state & (DPAD_A | DPAD_START))) break;
        sleep_ms(20);
    }
    prev_buttons = DPAD_A | DPAD_START;  // Consider them as "just released"
    
    while (true) {
        nespad_read();
        uint32_t buttons = nespad_state;
        
        // Poll PS/2 keyboard and get state
        ps2kbd_tick();
        uint16_t kbd_state = ps2kbd_get_state();
        
#ifdef USB_HID_ENABLED
        // Merge USB keyboard state with PS/2 keyboard state
        kbd_state |= usbhid_get_kbd_state();
#endif
        
        // Merge keyboard state into buttons
        if (kbd_state & KBD_STATE_UP)    buttons |= DPAD_UP;
        if (kbd_state & KBD_STATE_DOWN)  buttons |= DPAD_DOWN;
        if (kbd_state & KBD_STATE_LEFT)  buttons |= DPAD_LEFT;
        if (kbd_state & KBD_STATE_RIGHT) buttons |= DPAD_RIGHT;
        if (kbd_state & KBD_STATE_A)     buttons |= DPAD_A;
        if (kbd_state & KBD_STATE_B)     buttons |= DPAD_B;
        if (kbd_state & KBD_STATE_START) buttons |= DPAD_START;
        if (kbd_state & KBD_STATE_ESC)   buttons |= DPAD_B;  // ESC = back
        
#ifdef USB_HID_ENABLED
        usbhid_task();
        if (usbhid_gamepad_connected()) {
            usbhid_gamepad_state_t gp;
            usbhid_get_gamepad_state(&gp);
            if (gp.dpad & 0x01) buttons |= DPAD_UP;
            if (gp.dpad & 0x02) buttons |= DPAD_DOWN;
            if (gp.dpad & 0x04) buttons |= DPAD_LEFT;
            if (gp.dpad & 0x08) buttons |= DPAD_RIGHT;
            if (gp.buttons & 0x01) buttons |= DPAD_A;
            if (gp.buttons & 0x02) buttons |= DPAD_B;
        }
#endif
        
        uint32_t buttons_pressed = buttons & ~prev_buttons;
        bool up_repeat = false, down_repeat = false;
        
        if (buttons & (DPAD_UP | DPAD_DOWN)) {
            hold_counter++;
            if (hold_counter > REPEAT_DELAY) {
                if ((hold_counter - REPEAT_DELAY) % REPEAT_RATE == 0) {
                    if (buttons & DPAD_UP) up_repeat = true;
                    if (buttons & DPAD_DOWN) down_repeat = true;
                }
            }
        } else {
            hold_counter = 0;
        }
        
        prev_buttons = buttons;
        bool needs_redraw = false;
        
        // Navigation
        if ((buttons_pressed & DPAD_UP) || up_repeat) {
            selected = get_next_channel_selectable(selected, -1);
            needs_redraw = true;
        }
        if ((buttons_pressed & DPAD_DOWN) || down_repeat) {
            selected = get_next_channel_selectable(selected, 1);
            needs_redraw = true;
        }
        
        // Toggle or select
        if (buttons_pressed & (DPAD_A | DPAD_LEFT | DPAD_RIGHT)) {
            channel_menu_item_t item = (channel_menu_item_t)selected;
            if (item == CHAN_BACK) {
                sleep_ms(100);
                return true;
            } else if (item >= CHAN_FM1 && item <= CHAN_PSG) {
                // Toggle channel using bitmask
                bool current = CHANNEL_ENABLED(edit_settings.channel_mask, item);
                edit_settings.channel_mask = CHANNEL_SET(edit_settings.channel_mask, item, !current);
                // Sync dac_sound with channel 6
                if (item == CHAN_FM6) {
                    edit_settings.dac_sound = CHANNEL_ENABLED(edit_settings.channel_mask, CHAN_FM6);
                }
                needs_redraw = true;
            }
        }
        
        // B = back
        if (buttons_pressed & DPAD_B) {
            sleep_ms(100);
            return true;
        }
        
        if (needs_redraw) {
            draw_channel_menu(screen_buffer, selected);
        }
        
        sleep_ms(50);
    }
}

// Draw the entire settings menu
static void draw_settings_menu(uint8_t *screen, int selected) {
    // Clear screen
    fill_rect(screen, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    
    // Draw title
    const char *title = "SETTINGS";
    int title_width = (int)strlen(title) * FONT_WIDTH;
    int title_x = (SCREEN_WIDTH - title_width) / 2;
    draw_text(screen, title_x, MENU_TITLE_Y, title, COLOR_WHITE);
    
    // Draw separator under title
    draw_hline(screen, 40, MENU_TITLE_Y + FONT_HEIGHT + 4, SCREEN_WIDTH - 80, COLOR_GRAY);
    
    // Draw menu items
    int y = MENU_START_Y;
    char value_buf[32];
    
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        menu_item_t item = (menu_item_t)i;
        
        if (item == MENU_SEPARATOR) {
            // Draw separator line
            draw_hline(screen, 40, y + LINE_HEIGHT / 2, SCREEN_WIDTH - 80, COLOR_GRAY);
            y += LINE_HEIGHT;
            continue;
        }
        
        bool is_selected = (i == selected);
        bool is_enabled = is_selectable(item);
        
        uint8_t label_color = is_selected ? COLOR_YELLOW : (is_enabled ? COLOR_WHITE : COLOR_GRAY);
        uint8_t value_color = is_selected ? COLOR_YELLOW : COLOR_WHITE;
        
        // Draw selection indicator
        if (is_selected) {
            draw_text(screen, MENU_X - 12, y, ">", COLOR_YELLOW);
        }
        
        // Draw label
        const char *label = get_menu_label(item);
        draw_text(screen, MENU_X, y, label, label_color);
        
        // Draw value (for settings items)
        get_menu_value(item, value_buf, sizeof(value_buf));
        if (value_buf[0]) {
            // Right-align value
            int value_width = (int)strlen(value_buf) * FONT_WIDTH;
            int value_x = SCREEN_WIDTH - MENU_X - value_width;
            
            // Dim the value for disabled items
            if (!is_enabled) value_color = COLOR_GRAY;
            draw_text(screen, value_x, y, value_buf, value_color);
        }
        
        y += LINE_HEIGHT;
    }
    
    // Draw help text at bottom
    const char *help = "UP/DOWN: SELECT   LEFT/RIGHT: CHANGE   A: CONFIRM";
    int help_width = (int)strlen(help) * FONT_WIDTH;
    int help_x = (SCREEN_WIDTH - help_width) / 2;
    draw_text(screen, help_x, SCREEN_HEIGHT - 20, help, COLOR_GRAY);
}

// Parse INI file line
static bool parse_ini_line(const char *line, const char *key, char *value, size_t value_size) {
    // Skip whitespace
    while (*line == ' ' || *line == '\t') line++;
    
    // Check if line starts with key
    size_t key_len = strlen(key);
    if (strncasecmp(line, key, key_len) != 0) return false;
    
    line += key_len;
    
    // Skip whitespace and equals sign
    while (*line == ' ' || *line == '\t') line++;
    if (*line != '=') return false;
    line++;
    while (*line == ' ' || *line == '\t') line++;
    
    // Copy value (strip trailing whitespace/newline)
    size_t i = 0;
    while (*line && *line != '\n' && *line != '\r' && i < value_size - 1) {
        value[i++] = *line++;
    }
    value[i] = '\0';
    
    // Strip trailing whitespace
    while (i > 0 && (value[i-1] == ' ' || value[i-1] == '\t')) {
        value[--i] = '\0';
    }
    
    return true;
}

void settings_load(void) {
    FIL file;
    char line[128];
    char value[64];
    
    // Set defaults first
    g_settings.cpu_freq = 504;
    g_settings.psram_freq = 166;
    g_settings.fm_sound = true;
    g_settings.dac_sound = true;
    g_settings.crt_effect = false;
    g_settings.crt_dim = 60;
    g_settings.z80_enabled = true;
    g_settings.audio_enabled = true;
    g_settings.channel_mask = 0x7F;  // All channels on
    g_settings.frameskip = 3;  // Default: high
    g_settings.gamepad2_mode = GAMEPAD2_MODE_NES;  // Default: NES
    g_settings.browser_path[0] = '\0';
    g_settings.browser_file[0] = '\0';
    
    FRESULT res = f_open(&file, "/genesis/settings.ini", FA_READ);
    if (res != FR_OK) {
        // Try uppercase
        res = f_open(&file, "/GENESIS/settings.ini", FA_READ);
        if (res != FR_OK) {
            return;  // No settings file, use defaults
        }
    }
    
    // Read line by line
    while (f_gets(line, sizeof(line), &file)) {
        if (parse_ini_line(line, "cpu_freq", value, sizeof(value))) {
            int freq = atoi(value);
            if (freq == 378 || freq == 504) {
                g_settings.cpu_freq = (uint16_t)freq;
            }
        }
        else if (parse_ini_line(line, "psram_freq", value, sizeof(value))) {
            int freq = atoi(value);
            if (freq == 133 || freq == 166) {
                g_settings.psram_freq = (uint16_t)freq;
            }
        }
        else if (parse_ini_line(line, "z80", value, sizeof(value))) {
            g_settings.z80_enabled = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
        }
        else if (parse_ini_line(line, "audio", value, sizeof(value))) {
            g_settings.audio_enabled = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
        }
        else if (parse_ini_line(line, "fm_sound", value, sizeof(value))) {
            g_settings.fm_sound = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
        }
        else if (parse_ini_line(line, "crt_effect", value, sizeof(value))) {
            g_settings.crt_effect = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
        }
        else if (parse_ini_line(line, "crt_dim", value, sizeof(value))) {
            int dim = atoi(value);
            if (dim >= 10 && dim <= 90) {
                g_settings.crt_dim = (uint8_t)dim;
            }
        }
        else if (parse_ini_line(line, "channel_1", value, sizeof(value))) {
            bool en = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
            g_settings.channel_mask = CHANNEL_SET(g_settings.channel_mask, 0, en);
        }
        else if (parse_ini_line(line, "channel_2", value, sizeof(value))) {
            bool en = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
            g_settings.channel_mask = CHANNEL_SET(g_settings.channel_mask, 1, en);
        }
        else if (parse_ini_line(line, "channel_3", value, sizeof(value))) {
            bool en = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
            g_settings.channel_mask = CHANNEL_SET(g_settings.channel_mask, 2, en);
        }
        else if (parse_ini_line(line, "channel_4", value, sizeof(value))) {
            bool en = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
            g_settings.channel_mask = CHANNEL_SET(g_settings.channel_mask, 3, en);
        }
        else if (parse_ini_line(line, "channel_5", value, sizeof(value))) {
            bool en = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
            g_settings.channel_mask = CHANNEL_SET(g_settings.channel_mask, 4, en);
        }
        else if (parse_ini_line(line, "channel_6", value, sizeof(value))) {
            bool en = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
            g_settings.channel_mask = CHANNEL_SET(g_settings.channel_mask, 5, en);
            g_settings.dac_sound = en;  // Sync dac_sound
        }
        else if (parse_ini_line(line, "psg", value, sizeof(value))) {
            bool en = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
            g_settings.channel_mask = CHANNEL_SET(g_settings.channel_mask, 6, en);
        }
        else if (parse_ini_line(line, "frameskip", value, sizeof(value))) {
            int level = atoi(value);
            if (level >= 0 && level <= FRAMESKIP_MAX_LEVEL) {
                g_settings.frameskip = (uint8_t)level;
            }
        }
        else if (parse_ini_line(line, "gamepad2", value, sizeof(value))) {
            if (strcasecmp(value, "nes") == 0 || strcmp(value, "0") == 0) {
                g_settings.gamepad2_mode = GAMEPAD2_MODE_NES;
            } else if (strcasecmp(value, "keyboard") == 0 || strcmp(value, "1") == 0) {
                g_settings.gamepad2_mode = GAMEPAD2_MODE_KEYBOARD;
            } else if (strcasecmp(value, "usb") == 0 || strcmp(value, "2") == 0) {
                g_settings.gamepad2_mode = GAMEPAD2_MODE_USB;
            } else if (strcasecmp(value, "disabled") == 0 || strcmp(value, "3") == 0) {
                g_settings.gamepad2_mode = GAMEPAD2_MODE_DISABLED;
            }
        }
        else if (parse_ini_line(line, "browser_path", value, sizeof(value))) {
            strncpy(g_settings.browser_path, value, sizeof(g_settings.browser_path) - 1);
            g_settings.browser_path[sizeof(g_settings.browser_path) - 1] = '\0';
        }
        else if (parse_ini_line(line, "browser_file", value, sizeof(value))) {
            strncpy(g_settings.browser_file, value, sizeof(g_settings.browser_file) - 1);
            g_settings.browser_file[sizeof(g_settings.browser_file) - 1] = '\0';
        }
    }
    
    f_close(&file);
}

bool settings_save(void) {
    FIL file;
    UINT bw;
    char buf[768];

    // Ensure genesis directory exists
    f_mkdir("/genesis");

    FRESULT res = f_open(&file, "/genesis/settings.ini", FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        return false;
    }

    // Write settings in INI format
    int written = snprintf(buf, sizeof(buf),
        "; FRANK Genesis Settings\n"
        "; This file is auto-generated. Edit with care.\n"
        "\n"
        "cpu_freq = %d\n"
        "psram_freq = %d\n"
        "z80 = %s\n"
        "audio = %s\n"
        "fm_sound = %s\n"
        "crt_effect = %s\n"
        "crt_dim = %d\n"
        "frameskip = %d\n"
        "gamepad2 = %s\n"
        "\n"
        "; Audio Channels\n"
        "channel_1 = %s\n"
        "channel_2 = %s\n"
        "channel_3 = %s\n"
        "channel_4 = %s\n"
        "channel_5 = %s\n"
        "channel_6 = %s\n"
        "psg = %s\n"
        "\n"
        "; File browser state (auto-saved; do not edit manually)\n"
        "browser_path = %s\n"
        "browser_file = %s\n",
        g_settings.cpu_freq,
        g_settings.psram_freq,
        g_settings.z80_enabled ? "on" : "off",
        g_settings.audio_enabled ? "on" : "off",
        g_settings.fm_sound ? "on" : "off",
        g_settings.crt_effect ? "on" : "off",
        g_settings.crt_dim,
        g_settings.frameskip,
        gamepad2_mode_names[g_settings.gamepad2_mode],
        CHANNEL_ENABLED(g_settings.channel_mask, 0) ? "on" : "off",
        CHANNEL_ENABLED(g_settings.channel_mask, 1) ? "on" : "off",
        CHANNEL_ENABLED(g_settings.channel_mask, 2) ? "on" : "off",
        CHANNEL_ENABLED(g_settings.channel_mask, 3) ? "on" : "off",
        CHANNEL_ENABLED(g_settings.channel_mask, 4) ? "on" : "off",
        CHANNEL_ENABLED(g_settings.channel_mask, 5) ? "on" : "off",
        CHANNEL_ENABLED(g_settings.channel_mask, 6) ? "on" : "off",
        g_settings.browser_path,
        g_settings.browser_file);

    if (written < 0 || (size_t)written >= sizeof(buf)) {
        f_close(&file);
        return false;
    }

    res = f_write(&file, buf, (UINT)written, &bw);
    f_close(&file);

    return (res == FR_OK && bw == (UINT)written);
}

// External audio control flags from main.c and ym2612.c
extern bool sn76489_enabled;
extern bool ym2612_enabled;
extern bool ym2612_fm_enabled;
extern bool ym2612_dac_enabled;
extern bool ym2612_channel_enabled[6];  // Per-channel mute flags

// External Z80 control
extern bool z80_enabled;

// External master audio control
extern bool audio_enabled;

// External CRT control from HDMI.c
extern void graphics_set_crt_effect(bool enabled, uint8_t dim_percent);

// External frameskip control from main.c
extern void set_frameskip_level(uint8_t level);

void settings_apply_runtime(void) {
    // Apply settings that can be changed without restart
    
    // Master audio control - when disabled, skip all audio generation
    audio_enabled = g_settings.audio_enabled;
    
    if (!g_settings.audio_enabled) {
        // Audio disabled - mute everything for max performance
        ym2612_enabled = false;
        sn76489_enabled = false;
    } else {
        // Audio enabled - apply individual channel settings
        ym2612_enabled = true;
        ym2612_fm_enabled = g_settings.fm_sound;
        ym2612_dac_enabled = CHANNEL_ENABLED(g_settings.channel_mask, 5);  // Channel 6
        sn76489_enabled = CHANNEL_ENABLED(g_settings.channel_mask, 6);     // PSG
        
        // Apply per-channel mute settings
        for (int i = 0; i < 6; i++) {
            ym2612_channel_enabled[i] = CHANNEL_ENABLED(g_settings.channel_mask, i);
        }
    }
    
    // Z80 control
    z80_enabled = g_settings.z80_enabled;
    
    // CRT settings
    graphics_set_crt_effect(g_settings.crt_effect, g_settings.crt_dim);
    
    // Frameskip
    set_frameskip_level(g_settings.frameskip);
}

settings_result_t settings_menu_show(uint8_t *screen_buffer) {
    return settings_menu_show_with_restore(screen_buffer, NULL);
}

settings_result_t settings_menu_show_with_restore(uint8_t *screen_buffer, uint8_t *saved_screen) {
    // Note: If saved_screen is provided, caller is responsible for saving screen content
    // before calling this function. We only use it for restore on cancel.
    
    // Save current audio state and mute while in settings menu
    bool saved_ym2612_enabled = ym2612_enabled;
    bool saved_ym2612_fm_enabled = ym2612_fm_enabled;
    bool saved_ym2612_dac_enabled = ym2612_dac_enabled;
    bool saved_sn76489_enabled = sn76489_enabled;
    
    // Disable audio output completely while in settings menu
    audio_set_enabled(false);
    
    // Also mute the emulated sound chips
    ym2612_enabled = false;
    ym2612_fm_enabled = false;
    ym2612_dac_enabled = false;
    sn76489_enabled = false;
    
    // Flush silence to DMA buffer to stop any repeating sound
    audio_flush_silence();
    
    // Copy current settings to edit buffer
    memcpy(&edit_settings, &g_settings, sizeof(settings_t));
    
    int selected = 0;
    uint32_t prev_buttons = 0;
    uint32_t hold_counter = 0;
    const uint32_t REPEAT_DELAY = 10;
    const uint32_t REPEAT_RATE = 3;
    
    // Clear screen and draw settings menu
    memset(screen_buffer, COLOR_BLACK, SCREEN_WIDTH * SCREEN_HEIGHT);
    draw_settings_menu(screen_buffer, selected);
    
    // Small delay to let display settle
    sleep_ms(100);
    
    // Wait for all keys to be released before starting input loop
    // This prevents ESC from immediately closing the menu when it was used to open it
    while (true) {
        nespad_read();
        ps2kbd_tick();
        uint16_t kbd_state = ps2kbd_get_state();
#ifdef USB_HID_ENABLED
        kbd_state |= usbhid_get_kbd_state();
        usbhid_task();
#endif
        if (nespad_state == 0 && kbd_state == 0) break;
        sleep_ms(10);
    }
    
    while (true) {
        // Read gamepad
        nespad_read();
        uint32_t buttons = nespad_state;
        
        // Poll PS/2 keyboard and get state
        ps2kbd_tick();
        uint16_t kbd_state = ps2kbd_get_state();
        
#ifdef USB_HID_ENABLED
        // Merge USB keyboard state with PS/2 keyboard state
        kbd_state |= usbhid_get_kbd_state();
#endif
        
        // Merge keyboard state into buttons
        if (kbd_state & KBD_STATE_UP)    buttons |= DPAD_UP;
        if (kbd_state & KBD_STATE_DOWN)  buttons |= DPAD_DOWN;
        if (kbd_state & KBD_STATE_LEFT)  buttons |= DPAD_LEFT;
        if (kbd_state & KBD_STATE_RIGHT) buttons |= DPAD_RIGHT;
        if (kbd_state & KBD_STATE_A)     buttons |= DPAD_A;
        if (kbd_state & KBD_STATE_B)     buttons |= DPAD_B;
        if (kbd_state & KBD_STATE_START) buttons |= DPAD_START;
        if (kbd_state & KBD_STATE_ESC)   buttons |= DPAD_B;  // ESC = cancel/back
        
#ifdef USB_HID_ENABLED
        // Poll USB and merge USB gamepad state
        usbhid_task();
        if (usbhid_gamepad_connected()) {
            usbhid_gamepad_state_t gp;
            usbhid_get_gamepad_state(&gp);
            
            if (gp.dpad & 0x01) buttons |= DPAD_UP;
            if (gp.dpad & 0x02) buttons |= DPAD_DOWN;
            if (gp.dpad & 0x04) buttons |= DPAD_LEFT;
            if (gp.dpad & 0x08) buttons |= DPAD_RIGHT;
            if (gp.buttons & 0x01) buttons |= DPAD_A;
            if (gp.buttons & 0x40) buttons |= DPAD_START;
        }
#endif
        
        // Detect button press
        uint32_t buttons_pressed = buttons & ~prev_buttons;
        
        // Key repeat for up/down/left/right
        bool up_repeat = false, down_repeat = false;
        bool left_repeat = false, right_repeat = false;
        
        if (buttons & (DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT)) {
            hold_counter++;
            if (hold_counter > REPEAT_DELAY && ((hold_counter - REPEAT_DELAY) % REPEAT_RATE == 0)) {
                if (buttons & DPAD_UP) up_repeat = true;
                if (buttons & DPAD_DOWN) down_repeat = true;
                if (buttons & DPAD_LEFT) left_repeat = true;
                if (buttons & DPAD_RIGHT) right_repeat = true;
            }
        } else {
            hold_counter = 0;
        }
        prev_buttons = buttons;
        
        bool needs_redraw = false;
        
        // Navigate up
        if ((buttons_pressed & DPAD_UP) || up_repeat) {
            selected = get_next_selectable(selected, -1);
            needs_redraw = true;
        }
        
        // Navigate down
        if ((buttons_pressed & DPAD_DOWN) || down_repeat) {
            selected = get_next_selectable(selected, 1);
            needs_redraw = true;
        }
        
        // Change value left
        if ((buttons_pressed & DPAD_LEFT) || left_repeat) {
            if (selected < MENU_SEPARATOR) {
                change_setting((menu_item_t)selected, -1);
                needs_redraw = true;
            }
        }
        
        // Change value right
        if ((buttons_pressed & DPAD_RIGHT) || right_repeat) {
            if (selected < MENU_SEPARATOR) {
                change_setting((menu_item_t)selected, 1);
                needs_redraw = true;
            }
        }
        
        // Confirm selection
        if (buttons_pressed & (DPAD_A | DPAD_START)) {
            switch ((menu_item_t)selected) {
                case MENU_SAVE_RESTART:
                    // Copy edited settings to global
                    memcpy(&g_settings, &edit_settings, sizeof(settings_t));
                    return SETTINGS_RESULT_SAVE_RESTART;
                    
                case MENU_RESTART:
                    return SETTINGS_RESULT_RESTART;
                    
                case MENU_CHANNELS:
                    // Open channel submenu
                    if (edit_settings.audio_enabled) {
                        show_channel_submenu(screen_buffer);
                        draw_settings_menu(screen_buffer, selected);
                    }
                    break;
                    
                case MENU_CANCEL: {
                    // Wait for all buttons to be released for multiple consecutive reads
                    int release_count = 0;
                    while (release_count < 5) {
                        sleep_ms(50);
                        nespad_read();
                        if (nespad_state & (DPAD_A | DPAD_B | DPAD_START | DPAD_SELECT)) {
                            release_count = 0;  // Reset if any button pressed
                        } else {
                            release_count++;  // Count consecutive releases
                        }
                    }
                    // Extra delay after confirmed release
                    sleep_ms(100);
                    // Restore audio state
                    ym2612_enabled = saved_ym2612_enabled;
                    ym2612_fm_enabled = saved_ym2612_fm_enabled;
                    ym2612_dac_enabled = saved_ym2612_dac_enabled;
                    sn76489_enabled = saved_sn76489_enabled;
                    audio_set_enabled(true);
                    return SETTINGS_RESULT_CANCEL;
                }
                    
                default:
                    // For settings items, toggle or change
                    change_setting((menu_item_t)selected, 1);
                    needs_redraw = true;
                    break;
            }
        }
        
        // B button or Select+Start = cancel
        if (buttons_pressed & DPAD_B) {
            // Wait for all buttons to be released for multiple consecutive reads
            int release_count = 0;
            while (release_count < 5) {
                sleep_ms(50);
                nespad_read();
                if (nespad_state & (DPAD_A | DPAD_B | DPAD_START | DPAD_SELECT)) {
                    release_count = 0;  // Reset if any button pressed
                } else {
                    release_count++;  // Count consecutive releases
                }
            }
            // Extra delay after confirmed release
            sleep_ms(100);
            // Restore audio state
            ym2612_enabled = saved_ym2612_enabled;
            ym2612_fm_enabled = saved_ym2612_fm_enabled;
            ym2612_dac_enabled = saved_ym2612_dac_enabled;
            sn76489_enabled = saved_sn76489_enabled;
            audio_set_enabled(true);
            return SETTINGS_RESULT_CANCEL;
        }
        
        if (needs_redraw) {
            draw_settings_menu(screen_buffer, selected);
        }
        
        sleep_ms(50);
    }
    
    // Shouldn't reach here, but just in case - caller will handle screen restore
    // Restore audio state
    ym2612_enabled = saved_ym2612_enabled;
    ym2612_fm_enabled = saved_ym2612_fm_enabled;
    ym2612_dac_enabled = saved_ym2612_dac_enabled;
    sn76489_enabled = saved_sn76489_enabled;
    audio_set_enabled(true);
    return SETTINGS_RESULT_CANCEL;
}

bool settings_check_hotkey(void) {
    // Check if Start+Select are both pressed (gamepad), or ESC on keyboard
    nespad_read();
    
    bool start_select = (nespad_state & DPAD_SELECT) && (nespad_state & DPAD_START);
    
    // Check PS/2 keyboard for ESC
    ps2kbd_tick();
    uint16_t kbd_state = ps2kbd_get_state();
    
#ifdef USB_HID_ENABLED
    // Merge USB keyboard state
    kbd_state |= usbhid_get_kbd_state();
#endif
    
    if (kbd_state & KBD_STATE_ESC) {
        start_select = true;
    }
    
#ifdef USB_HID_ENABLED
    if (!start_select && usbhid_gamepad_connected()) {
        usbhid_gamepad_state_t gp;
        usbhid_get_gamepad_state(&gp);
        // Start=0x40, Select=0x80
        start_select = (gp.buttons & 0x40) && (gp.buttons & 0x80);
    }
#endif
    
    return start_select;
}
