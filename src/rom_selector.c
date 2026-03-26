/*
 * ROM Selector Implementation
 * Adapted from murmdoom start screen
 */
#include "rom_selector.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "HDMI.h"
#include "board_config.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "nespad/nespad.h"
#include "ps2kbd/ps2kbd_wrapper.h"
#include "settings.h"
#include "psram_allocator.h"
#include <string.h>
#include <stdio.h>

#ifndef FRANK_GENESIS_VERSION
#define FRANK_GENESIS_VERSION "?"
#endif

// USB HID gamepad support
#ifdef USB_HID_ENABLED
#include "usbhid/usbhid.h"
#endif

// Screen dimensions
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

// ROM list management
#define MAX_ROMS 256
typedef struct {
    char filename[64];
    char display_name[64];
} rom_entry_t;

static rom_entry_t rom_list[MAX_ROMS];
static int rom_count = 0;

// Font and UI constants
#define FONT_WIDTH 6    // 5px glyph + 1px spacing
#define FONT_HEIGHT 7
#define LINE_HEIGHT 10
#define VISIBLE_LINES 13  // Fit two more entries while keeping bottom text visible
#define HEADER_Y 16
#define HEADER_HEIGHT 36
#define MENU_X 24
#define MENU_Y 60   // Slightly tighter spacing under header
#define MAX_ROM_NAME_CHARS 40  // Max characters for ROM name display (reduced for scrollbar gap)
#define SCROLLBAR_WIDTH 4
#define SCROLLBAR_X (SCREEN_WIDTH - 24)
#define SCROLLBAR_Y MENU_Y
#define SCROLLBAR_HEIGHT (VISIBLE_LINES * LINE_HEIGHT)
#define MENU_WIDTH (SCROLLBAR_X - MENU_X - 8)  // Gap before scrollbar

#define FOOTER_Y (SCREEN_HEIGHT - (FONT_HEIGHT + 4))
#define INFO_BASE_Y (FOOTER_Y - LINE_HEIGHT)  // Just above footer
#define HELP_Y (INFO_BASE_Y - LINE_HEIGHT)  // Above info text

// Colors for UI elements (8-bit 3-3-2 RGB palette)
#define COLOR_WHITE 63   // 0b00111111 - bright white
#define COLOR_GRAY 42    // 0b00101010 - medium gray
#define COLOR_YELLOW 48  // Use palette index 48 for yellow (safe range)
#define COLOR_RED 32     // Palette index 32 = 0xFF0000 (red)

// Smooth scrollbar animation
static int scrollbar_current_y = 0;  // Current animated position
static int scrollbar_target_y = 0;   // Target position

// Smooth cursor animation
static int cursor_current_y = 0;  // Current animated highlight Y position
static int cursor_target_y = 0;   // Target highlight Y position

// Screen save buffer for settings menu (allocated from PSRAM at runtime)
static uint8_t *saved_screen_buffer = NULL;

// Forward declarations (draw_text defined below)

// Minimal UTF-8 renderer for Cyrillic boot warning
static void draw_text_utf8(uint8_t *screen, int x, int y, const char *text, uint8_t color);
static void draw_warning_splash(uint8_t *screen);

// Bold 7x9 font for title (wider strokes)
#define BOLD_FONT_WIDTH 8   // 7px glyph + 1px spacing  
#define BOLD_FONT_HEIGHT 9

static const uint8_t *glyph_bold(char ch) {
    // 7-pixel wide, 9-pixel tall bold glyphs
    static const uint8_t glyph_space[9] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t glyph_A[9] = {0x1C, 0x36, 0x63, 0x63, 0x7F, 0x63, 0x63, 0x63, 0x63};
    static const uint8_t glyph_E[9] = {0x7F, 0x60, 0x60, 0x60, 0x7E, 0x60, 0x60, 0x60, 0x7F};
    static const uint8_t glyph_F[9] = {0x7F, 0x60, 0x60, 0x60, 0x7E, 0x60, 0x60, 0x60, 0x60};
    static const uint8_t glyph_G[9] = {0x3E, 0x63, 0x60, 0x60, 0x6F, 0x63, 0x63, 0x63, 0x3E};
    static const uint8_t glyph_I[9] = {0x3E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3E};
    static const uint8_t glyph_K[9] = {0x63, 0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x63};
    static const uint8_t glyph_N[9] = {0x63, 0x73, 0x7B, 0x7F, 0x6F, 0x67, 0x63, 0x63, 0x63};
    static const uint8_t glyph_R[9] = {0x7E, 0x63, 0x63, 0x63, 0x7E, 0x6C, 0x66, 0x63, 0x63};
    static const uint8_t glyph_S[9] = {0x3E, 0x63, 0x60, 0x70, 0x3E, 0x07, 0x03, 0x63, 0x3E};

    int c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    
    switch (c) {
        case 'A': return glyph_A;
        case 'E': return glyph_E;
        case 'F': return glyph_F;
        case 'G': return glyph_G;
        case 'I': return glyph_I;
        case 'K': return glyph_K;
        case 'N': return glyph_N;
        case 'R': return glyph_R;
        case 'S': return glyph_S;
        default: return glyph_space;
    }
}

// Draw bold character
static void draw_char_bold(uint8_t *screen, int x, int y, char ch, uint8_t color) {
    const uint8_t *rows = glyph_bold(ch);
    for (int row = 0; row < BOLD_FONT_HEIGHT; ++row) {
        int yy = y + row;
        if (yy < 0 || yy >= SCREEN_HEIGHT) continue;
        uint8_t bits = rows[row];
        for (int col = 0; col < 7; ++col) {
            int xx = x + col;
            if (xx < 0 || xx >= SCREEN_WIDTH) continue;
            if (bits & (1u << (6 - col))) {
                screen[yy * SCREEN_WIDTH + xx] = color;
            }
        }
    }
}

// Draw bold text string
static void draw_text_bold(uint8_t *screen, int x, int y, const char *text, uint8_t color) {
    for (const char *p = text; *p; ++p) {
        draw_char_bold(screen, x, y, *p, color);
        x += BOLD_FONT_WIDTH;
    }
}

// 5x7 font glyphs (from murmdoom)
static const uint8_t *glyph_5x7(char ch) {
    static const uint8_t glyph_space[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t glyph_dot[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    static const uint8_t glyph_hyphen[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static const uint8_t glyph_underscore[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};
    static const uint8_t glyph_colon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
    static const uint8_t glyph_slash[7] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00};
    static const uint8_t glyph_comma[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x04};
    static const uint8_t glyph_lparen[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
    static const uint8_t glyph_rparen[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
    
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
    
    // Lowercase (simpler, same as uppercase for readability)
    int c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    
    switch (c) {
        case ' ': return glyph_space;
        case '.': return glyph_dot;
        case '-': return glyph_hyphen;
        case '_': return glyph_underscore;
        case ':': return glyph_colon;
        case '/': return glyph_slash;
        case ',': return glyph_comma;
        case '(': return glyph_lparen;
        case ')': return glyph_rparen;
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
    
    // Use index 1 instead of 0 for black - index 0 causes HDMI issues at 378MHz
    if (color == 0) color = 1;
    
    for (int yy = y; yy < y + h; ++yy) {
        memset(&screen[yy * SCREEN_WIDTH + x], color, (size_t)w);
        // Brief yield after each row to avoid starving HDMI DMA at 378MHz
        __asm volatile("nop\nnop\nnop\nnop");
    }
}

static void draw_demostyle_header(uint8_t *screen, uint32_t phase) {
    // Calculate title dimensions first to exclude from animation
    const char *title = "FRANK GENESIS";
    int title_width = (int)strlen(title) * BOLD_FONT_WIDTH;
    int title_x = (SCREEN_WIDTH - title_width) / 2;
    int title_y = HEADER_Y + (HEADER_HEIGHT - BOLD_FONT_HEIGHT) / 2;
    int title_left = title_x - 8;
    int title_right = title_x + title_width + 8;
    int title_top = title_y - 5;
    int title_bottom = title_y + BOLD_FONT_HEIGHT + 5;

    // Draw animated background, but skip the title area
    for (int y = 0; y < HEADER_HEIGHT; ++y) {
        int yy = HEADER_Y + y;
        if (yy < 0 || yy >= SCREEN_HEIGHT) continue;

        uint8_t row_phase = (uint8_t)((phase + y * 5) & 0x3F);
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            // Skip title area
            if (yy >= title_top && yy < title_bottom && x >= title_left && x < title_right) {
                continue;
            }
            uint8_t wave = (uint8_t)(((x * 3) + phase) & 0x3F);
            uint8_t color = (uint8_t)(8 + ((wave + (row_phase >> 1)) & 0x1F));
            if (color > 63) color = 63;
            screen[yy * SCREEN_WIDTH + x] = color;
        }

        // Horizontal stripes
        if ((y & 3) == 0) {
            uint8_t *row = &screen[yy * SCREEN_WIDTH];
            for (int x = 0; x < SCREEN_WIDTH; ++x) {
                if (yy >= title_top && yy < title_bottom && x >= title_left && x < title_right) continue;
                if (row[x] <= 55) {
                    row[x] = (uint8_t)(row[x] + 8);
                } else {
                    row[x] = 63;
                }
            }
        }
    }

    // Diagonal stripes, skip title area
    for (int offset = -HEADER_HEIGHT; offset < SCREEN_WIDTH; offset += 18) {
        for (int y = 0; y < HEADER_HEIGHT; ++y) {
            int yy = HEADER_Y + y;
            int xx = offset + y;
            if (yy < 0 || yy >= SCREEN_HEIGHT || xx < 0 || xx >= SCREEN_WIDTH) continue;
            if (yy >= title_top && yy < title_bottom && xx >= title_left && xx < title_right) continue;

            uint8_t *pixel = &screen[yy * SCREEN_WIDTH + xx];
            if (*pixel <= 53) {
                *pixel = (uint8_t)(*pixel + 10);
            } else {
                *pixel = 63;
            }
        }
    }

    // Draw solid background for title (drawn once, never animated)
    fill_rect(screen, title_left, title_top, title_right - title_left, title_bottom - title_top, 0);

    // Draw bold title with shadow
    draw_text_bold(screen, title_x + 1, title_y + 1, title, 20);
    draw_text_bold(screen, title_x, title_y, title, 63);
}

static void draw_info_text(uint8_t *screen) {
    char info_str[64];

    // Board label
#ifdef BOARD_M2
    const char *board_str = "M2";
#else
    const char *board_str = "M1";
#endif

    // Current system clock in MHz (rounded)
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t cpu_mhz = (sys_hz + 500000) / 1000000;

    // PSRAM clock: show the configured max frequency from settings
    uint32_t psram_mhz = g_settings.psram_freq;

    snprintf(info_str, sizeof(info_str), "V%s %s %u MHZ/%u MHZ", FRANK_GENESIS_VERSION, board_str, cpu_mhz, psram_mhz);

    // Center with 5x7 font metrics
    int text_width = (int)(strlen(info_str) * FONT_WIDTH);
    int x = (SCREEN_WIDTH - text_width) / 2;
    if (x < 0) x = 0;

    // Place one medium line above the help text
    int info_y = INFO_BASE_Y - LINE_HEIGHT;
    if (info_y < 0) info_y = 0;

    draw_text(screen, x, info_y, info_str, COLOR_WHITE);
}

static void draw_help_text(uint8_t *screen) {
    const char *help = "UP/DOWN: SELECT   A/START: CONFIRM";
    int text_width = (int)(strlen(help) * FONT_WIDTH);
    int x = (SCREEN_WIDTH - text_width) / 2;
    if (x < 0) x = 0;
    draw_text(screen, x, INFO_BASE_Y, help, COLOR_WHITE);
}

static bool utf8_next_codepoint(const char **p, uint32_t *out_cp) {
    const unsigned char *s = (const unsigned char *)*p;
    if (*s == 0) return false;

    if (s[0] < 0x80) {
        *out_cp = s[0];
        *p += 1;
        return true;
    }

    // 2-byte
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *out_cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *p += 2;
        return true;
    }

    // 3-byte
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *out_cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        *p += 3;
        return true;
    }

    // Unsupported/invalid sequence: skip one byte
    *out_cp = '?';
    *p += 1;
    return true;
}

static const uint8_t *glyph_cyrillic_8x8(uint32_t cp) {
    // Glyph rows use MSB-left bit order (bit 7 = leftmost pixel).
    static const uint8_t glyph_space[8] = {0,0,0,0,0,0,0,0};
    static const uint8_t glyph_period[8] = {0,0,0,0,0,0,0x18,0x18};
    // Make comma clearly different from period (tail down-left)
    static const uint8_t glyph_comma[8]  = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x10};
    static const uint8_t glyph_qmark[8]  = {0x3C,0x42,0x02,0x0C,0x10,0x00,0x10,0x00};

    // Cyrillic glyphs tuned to look like lowercase Russian where possible
    // (so text doesn't read as Latin lookalikes).
    static const uint8_t glyph_E_cap[8]  = {0x7E,0x40,0x40,0x7C,0x40,0x40,0x7E,0x00}; // Е
    static const uint8_t glyph_EE_cap[8] = {0x3C,0x42,0x02,0x1E,0x02,0x42,0x3C,0x00}; // Э

    static const uint8_t glyph_a[8] = {0x00,0x00,0x3C,0x02,0x3E,0x42,0x3E,0x00}; // а
    // б: shorten by 1 pixel at the bottom (avoid looking tall/bold)
    static const uint8_t glyph_b[8] = {0x00,0x00,0x3E,0x20,0x3C,0x22,0x3C,0x00}; // б
    static const uint8_t glyph_v[8] = {0x00,0x00,0x7C,0x42,0x7C,0x42,0x7C,0x00}; // в
    static const uint8_t glyph_g[8] = {0x00,0x00,0x7E,0x40,0x40,0x40,0x40,0x00}; // г
    static const uint8_t glyph_e[8] = {0x00,0x00,0x3C,0x42,0x7E,0x40,0x3C,0x00}; // е
    static const uint8_t glyph_zh[8]= {0x00,0x00,0x5A,0x5A,0x3C,0x5A,0x5A,0x00}; // ж (not used, but safe)
    // и/й: keep shape but shorten by 1px (clear bottom row)
    static const uint8_t glyph_i[8] = {0x00,0x00,0x42,0x46,0x4A,0x52,0x62,0x00}; // и
    static const uint8_t glyph_j[8] = {0x24,0x00,0x42,0x46,0x4A,0x52,0x62,0x00}; // й
    static const uint8_t glyph_k[8] = {0x00,0x00,0x42,0x44,0x78,0x44,0x42,0x00}; // к
    static const uint8_t glyph_l[8] = {0x00,0x00,0x1C,0x24,0x44,0x44,0x44,0x00}; // л
    static const uint8_t glyph_m[8] = {0x00,0x00,0x42,0x66,0x5A,0x42,0x42,0x00}; // м
    // н: use clear "H"-like shape with mid bar
    static const uint8_t glyph_n[8] = {0x00,0x00,0x42,0x42,0x7E,0x42,0x42,0x00}; // н
    static const uint8_t glyph_o[8] = {0x00,0x00,0x3C,0x42,0x42,0x42,0x3C,0x00}; // о
    static const uint8_t glyph_p[8] = {0x00,0x00,0x7C,0x42,0x42,0x7C,0x40,0x40}; // р (descender)
    static const uint8_t glyph_s[8] = {0x00,0x00,0x3C,0x42,0x40,0x42,0x3C,0x00}; // с
    // т: even thinner/lighter (avoid looking bold)
    static const uint8_t glyph_t[8] = {0x00,0x00,0x1C,0x08,0x08,0x08,0x08,0x00}; // т
    static const uint8_t glyph_u[8] = {0x00,0x00,0x42,0x42,0x3E,0x02,0x3C,0x00}; // у
    // ф: smaller/less bold
    static const uint8_t glyph_f[8] = {0x00,0x18,0x3C,0x5A,0x5A,0x3C,0x18,0x00}; // ф
    static const uint8_t glyph_yi[8]= {0x00,0x00,0x42,0x42,0x4A,0x7A,0x7A,0x00}; // ы
    static const uint8_t glyph_ee[8]= {0x00,0x00,0x3C,0x42,0x1E,0x42,0x3C,0x00}; // э
    // я: avoid looking like Latin 'R' by removing left vertical and emphasizing right loop + left leg
    static const uint8_t glyph_ya[8]= {0x00,0x00,0x1E,0x12,0x1E,0x0A,0x12,0x00}; // я
    static const uint8_t glyph_sch[8]={0x00,0x00,0x54,0x54,0x54,0x54,0x7E,0x06}; // щ

    if (cp == ' ') return glyph_space;
    if (cp == '.') return glyph_period;
    if (cp == ',') return glyph_comma;

    // Cyrillic uppercase we need
    switch (cp) {
        // Map capital И to the same glyph as lowercase и to avoid it reading as Latin 'N'
        case 0x0418: return glyph_i;      // И
        case 0x0415: return glyph_E_cap;  // Е
        case 0x042D: return glyph_EE_cap; // Э
        default: break;
    }

    // Cyrillic lowercase used by the warning text
    switch (cp) {
                case 0x043F: {
                    static const uint8_t glyph_pe[8] = {0x00,0x00,0x7E,0x42,0x42,0x42,0x42,0x00}; // п
                    return glyph_pe;
                }
        case 0x0430: return glyph_a;   // а
        case 0x0431: return glyph_b;   // б
        case 0x0432: return glyph_v;   // в
        case 0x0433: return glyph_g;   // г
        case 0x0435: return glyph_e;   // е
        case 0x0438: return glyph_i;   // и
        case 0x0439: return glyph_j;   // й
        case 0x043A: return glyph_k;   // к
        case 0x043B: return glyph_l;   // л
        case 0x043C: return glyph_m;   // м
        case 0x043D: return glyph_n;   // н
        case 0x043E: return glyph_o;   // о
        case 0x0440: return glyph_p;   // р
        case 0x0441: return glyph_s;   // с
        case 0x0442: return glyph_t;   // т
        case 0x0443: return glyph_u;   // у
        case 0x0444: return glyph_f;   // ф
        case 0x044C: {
            static const uint8_t glyph_soft[8] = {0x00,0x00,0x40,0x40,0x7C,0x42,0x7C,0x00}; // ь
            return glyph_soft;
        }
        case 0x044B: return glyph_yi;  // ы
        case 0x044D: return glyph_ee;  // э
        case 0x044F: return glyph_ya;  // я
        case 0x0449: return glyph_sch; // щ
        case 0x0436: return glyph_zh;  // ж (fallback)
        default: break;
    }

    return glyph_qmark;
}

static void draw_char_8x8_msb(uint8_t *screen, int x, int y, const uint8_t glyph[8], uint8_t color) {
    for (int row = 0; row < 8; ++row) {
        int yy = y + row;
        if (yy < 0 || yy >= SCREEN_HEIGHT) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; ++col) {
            int xx = x + col;
            if (xx < 0 || xx >= SCREEN_WIDTH) continue;
            if (bits & (1u << (7 - col))) {
                screen[yy * SCREEN_WIDTH + xx] = color;
            }
        }
    }
}

static void draw_text_utf8(uint8_t *screen, int x, int y, const char *text, uint8_t color) {
    // 8x8 Cyrillic glyphs with 10px line height
    const int CYRILLIC_ADVANCE = 8;
    const int CYRILLIC_LINE_HEIGHT = 10;
    
    int cursor_x = x;
    int cursor_y = y;

    const char *p = text;
    uint32_t cp = 0;
    while (utf8_next_codepoint(&p, &cp)) {
        if (cp == '\n') {
            cursor_x = x;
            cursor_y += CYRILLIC_LINE_HEIGHT;
            continue;
        }
        const uint8_t *glyph = glyph_cyrillic_8x8(cp);
        draw_char_8x8_msb(screen, cursor_x, cursor_y, glyph, color);
        cursor_x += CYRILLIC_ADVANCE;
        if (cursor_x + CYRILLIC_ADVANCE >= SCREEN_WIDTH) {
            cursor_x = x;
            cursor_y += CYRILLIC_LINE_HEIGHT;
        }
    }
}

static void draw_warning_splash(uint8_t *screen) {
    // Cyrillic warning text
    const char *msg =
        "Это тестовая сборка,\n"
        "И она еще сырая.\n"
        "Если все сгорит нафиг,\n"
        "таков путь";

    int text_x = 26;
    int text_y = 80;
    draw_text_utf8(screen, text_x, text_y, msg, COLOR_RED);
}

static void draw_footer(uint8_t *screen) {
    const char *footer = "CODED BY MIKHAIL MATVEEV";

    int total_width = (int)(strlen(footer) * FONT_WIDTH);
    int x = (SCREEN_WIDTH - total_width) / 2;
    if (x < 0) x = 0;

    draw_text(screen, x, FOOTER_Y, footer, COLOR_WHITE);
}


// Scan /genesis directory for ROM files
static void scan_roms(void) {
    DIR dir;
    FILINFO fno;
    rom_count = 0;
    
    FRESULT res = f_opendir(&dir, "/genesis");
    if (res != FR_OK) {
        // Try uppercase
        res = f_opendir(&dir, "/GENESIS");
        if (res != FR_OK) {
            return;
        }
    }
    
    while (rom_count < MAX_ROMS) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;
        
        // Skip directories and hidden files
        if (fno.fattrib & AM_DIR || fno.fname[0] == '.') continue;
        
        // Check for supported extensions
        const char *ext = strrchr(fno.fname, '.');
        if (ext && (strcasecmp(ext, ".md") == 0 || 
                    strcasecmp(ext, ".bin") == 0 ||
                    strcasecmp(ext, ".gen") == 0 ||
                    strcasecmp(ext, ".smd") == 0)) {
            
            strncpy(rom_list[rom_count].filename, fno.fname, sizeof(rom_list[rom_count].filename) - 1);
            rom_list[rom_count].filename[sizeof(rom_list[rom_count].filename) - 1] = '\0';
            
            // Create display name (remove extension, uppercase)
            strncpy(rom_list[rom_count].display_name, fno.fname, sizeof(rom_list[rom_count].display_name) - 1);
            rom_list[rom_count].display_name[sizeof(rom_list[rom_count].display_name) - 1] = '\0';
            
            // Remove extension for display
            char *dot = strrchr(rom_list[rom_count].display_name, '.');
            if (dot) *dot = '\0';
            
            // Convert to uppercase for display
            for (char *p = rom_list[rom_count].display_name; *p; p++) {
                if (*p >= 'a' && *p <= 'z') *p = *p - 'a' + 'A';
            }
            
            rom_count++;
        }
    }
    
    f_closedir(&dir);
}

// Draw scrollbar with smooth animation
static void draw_scrollbar(uint8_t *screen, int scroll_offset, bool animate) {
    if (rom_count <= VISIBLE_LINES) return;  // No scrollbar needed
    
    // Calculate scrollbar thumb position and size
    int thumb_height = (SCROLLBAR_HEIGHT * VISIBLE_LINES) / rom_count;
    if (thumb_height < 10) thumb_height = 10;  // Minimum thumb size
    
    int thumb_max_y = SCROLLBAR_HEIGHT - thumb_height;
    scrollbar_target_y = (thumb_max_y * scroll_offset) / (rom_count - VISIBLE_LINES);
    
    // Smooth interpolation towards target
    if (animate) {
        int diff = scrollbar_target_y - scrollbar_current_y;
        if (diff != 0) {
            // Move 1/2 of the distance for faster animation
            int step = diff / 2;
            if (step == 0) step = (diff > 0) ? 1 : -1;
            scrollbar_current_y += step;
        }
    } else {
        scrollbar_current_y = scrollbar_target_y;
    }
    
    // Draw scrollbar background (dark)
    fill_rect(screen, SCROLLBAR_X, SCROLLBAR_Y, SCROLLBAR_WIDTH, SCROLLBAR_HEIGHT, 16);
    
    // Draw scrollbar thumb (bright)
    fill_rect(screen, SCROLLBAR_X, SCROLLBAR_Y + scrollbar_current_y, SCROLLBAR_WIDTH, thumb_height, COLOR_WHITE);
}

// Check if cursor animation is in progress
static bool cursor_animating(void) {
    return cursor_current_y != cursor_target_y;
}

// Check if any animation is in progress
static bool animation_in_progress(void) {
    return cursor_animating() || (scrollbar_current_y != scrollbar_target_y);
}

// Track previous state for smart redraws
static int prev_scroll_offset = -1;
static int prev_cursor_y = -1;  // Previous cursor Y for dirty rect

// Draw a single ROM row at a given Y position
static void draw_single_row(uint8_t *screen, int row_idx, int scroll_offset, int cursor_y) {
    int rom_idx = scroll_offset + row_idx;
    if (rom_idx >= rom_count || row_idx < 0 || row_idx >= VISIBLE_LINES) return;
    
    int text_y = MENU_Y + row_idx * LINE_HEIGHT;
    int row_top = text_y - 1;  // +1px up offset
    int row_bottom = row_top + LINE_HEIGHT;
    
    // Check if this row overlaps with the highlight bar
    int cursor_top = cursor_y;
    int cursor_bottom = cursor_y + LINE_HEIGHT;
    bool overlaps_cursor = (row_bottom > cursor_top && row_top < cursor_bottom);
    
    // Clear row to black first
    fill_rect(screen, MENU_X - 2, row_top, MENU_WIDTH, LINE_HEIGHT, 0);
    
    // Draw highlight if overlapping
    if (overlaps_cursor) {
        // Draw the portion of highlight that overlaps this row
        int highlight_start = (cursor_top > row_top) ? cursor_top : row_top;
        int highlight_end = (cursor_bottom < row_bottom) ? cursor_bottom : row_bottom;
        fill_rect(screen, MENU_X - 2, highlight_start, MENU_WIDTH, highlight_end - highlight_start, 63);
    }
    
    // Truncate long names with ellipsis
    char display_name[MAX_ROM_NAME_CHARS + 4];
    size_t name_len = strlen(rom_list[rom_idx].display_name);
    if (name_len > MAX_ROM_NAME_CHARS) {
        strncpy(display_name, rom_list[rom_idx].display_name, MAX_ROM_NAME_CHARS - 3);
        display_name[MAX_ROM_NAME_CHARS - 3] = '\0';
        strcat(display_name, "...");
    } else {
        strncpy(display_name, rom_list[rom_idx].display_name, MAX_ROM_NAME_CHARS);
        display_name[MAX_ROM_NAME_CHARS] = '\0';
    }
    
    // Draw text - near-black if overlapping highlight, white otherwise
    draw_text(screen, MENU_X, text_y, display_name, overlaps_cursor ? 1 : 63);
}

// Render ROM menu with smooth pixel-based cursor sliding - dirty rect approach
static void render_rom_menu(uint8_t *screen, int selected, int scroll_offset, bool selection_changed) {
    if (rom_count == 0) {
        if (selection_changed) {
            fill_rect(screen, MENU_X - 2, MENU_Y - 2, MENU_WIDTH + 4, (VISIBLE_LINES * LINE_HEIGHT) + 4, 0);
            draw_text(screen, MENU_X, MENU_Y, "NO ROMS FOUND", 63);
        }
        return;
    }
    
    int visible_idx = selected - scroll_offset;
    
    // Update cursor target (pixel Y position, +1px up)
    cursor_target_y = MENU_Y - 1 + visible_idx * LINE_HEIGHT;
    
    // Check if cursor needs to animate
    bool cursor_moved = (cursor_current_y != cursor_target_y);
    
    // Smooth cursor interpolation - move cursor towards target
    if (cursor_moved) {
        int diff = cursor_target_y - cursor_current_y;
        // Move 1/3 of distance for smooth animation
        int step = diff / 3;
        if (step == 0) step = (diff > 0) ? 1 : -1;
        cursor_current_y += step;
    }
    
    // If scroll offset changed, do full redraw and snap cursor
    if (scroll_offset != prev_scroll_offset) {
        cursor_current_y = cursor_target_y;  // Snap cursor on scroll
        prev_scroll_offset = scroll_offset;
        prev_cursor_y = cursor_current_y;
        
        // Full redraw on scroll change
        for (int i = 0; i < VISIBLE_LINES && (scroll_offset + i) < rom_count; i++) {
            draw_single_row(screen, i, scroll_offset, cursor_current_y);
        }
        draw_scrollbar(screen, scroll_offset, true);
        draw_help_text(screen);
        draw_info_text(screen);
        draw_footer(screen);
        return;
    }
    
    // If cursor hasn't moved at all, just update scrollbar if needed
    if (!cursor_moved && prev_cursor_y == cursor_current_y) {
        if (scrollbar_current_y != scrollbar_target_y) {
            draw_scrollbar(screen, scroll_offset, true);
        }
        // Always refresh overlay text so info/help/footer stay visible
        draw_help_text(screen);
        draw_info_text(screen);
        draw_footer(screen);
        return;
    }
    
    // Calculate which rows are affected by cursor movement (dirty rectangles)
    int old_cursor_row_start = (prev_cursor_y - MENU_Y + 1) / LINE_HEIGHT;
    int old_cursor_row_end = (prev_cursor_y + LINE_HEIGHT - MENU_Y + 1) / LINE_HEIGHT;
    int new_cursor_row_start = (cursor_current_y - MENU_Y + 1) / LINE_HEIGHT;
    int new_cursor_row_end = (cursor_current_y + LINE_HEIGHT - MENU_Y + 1) / LINE_HEIGHT;
    
    // Clamp to valid range
    if (old_cursor_row_start < 0) old_cursor_row_start = 0;
    if (old_cursor_row_end >= VISIBLE_LINES) old_cursor_row_end = VISIBLE_LINES - 1;
    if (new_cursor_row_start < 0) new_cursor_row_start = 0;
    if (new_cursor_row_end >= VISIBLE_LINES) new_cursor_row_end = VISIBLE_LINES - 1;
    
    // Find the range of rows that need redrawing
    int min_row = (old_cursor_row_start < new_cursor_row_start) ? old_cursor_row_start : new_cursor_row_start;
    int max_row = (old_cursor_row_end > new_cursor_row_end) ? old_cursor_row_end : new_cursor_row_end;
    
    // Only redraw the affected rows
    for (int i = min_row; i <= max_row && (scroll_offset + i) < rom_count; i++) {
        draw_single_row(screen, i, scroll_offset, cursor_current_y);
    }
    
    prev_cursor_y = cursor_current_y;
    
    // Draw scrollbar with smooth animation
    draw_scrollbar(screen, scroll_offset, true);

    // Draw help, info and footer overlays
    draw_help_text(screen);
    draw_info_text(screen);
    draw_footer(screen);
}

// Display SD card error screen (blocks forever)
void rom_selector_show_sd_error(uint8_t *screen_buffer, int error_code) {
    // Clear screen to near-black
    memset(screen_buffer, 1, SCREEN_WIDTH * SCREEN_HEIGHT);
    
    // Draw header
    draw_demostyle_header(screen_buffer, 0);
    
    // Error message
    const char *title = "SD CARD ERROR";
    int title_width = (int)strlen(title) * FONT_WIDTH;
    int title_x = (SCREEN_WIDTH - title_width) / 2;
    draw_text(screen_buffer, title_x, MENU_Y, title, COLOR_RED);
    
    // Error details
    const char *msg1 = "NO SD CARD DETECTED";
    const char *msg2 = "OR CARD NOT FORMATTED";
    
    int msg1_width = (int)strlen(msg1) * FONT_WIDTH;
    int msg1_x = (SCREEN_WIDTH - msg1_width) / 2;
    draw_text(screen_buffer, msg1_x, MENU_Y + 20, msg1, COLOR_WHITE);
    
    int msg2_width = (int)strlen(msg2) * FONT_WIDTH;
    int msg2_x = (SCREEN_WIDTH - msg2_width) / 2;
    draw_text(screen_buffer, msg2_x, MENU_Y + 32, msg2, COLOR_WHITE);
    
    // Error code
    char code_str[32];
    snprintf(code_str, sizeof(code_str), "ERROR CODE: %d", error_code);
    int code_width = (int)strlen(code_str) * FONT_WIDTH;
    int code_x = (SCREEN_WIDTH - code_width) / 2;
    draw_text(screen_buffer, code_x, MENU_Y + 52, code_str, COLOR_GRAY);
    
    // Instructions
    const char *inst1 = "PLEASE INSERT A FAT32";
    const char *inst2 = "FORMATTED SD CARD";
    const char *inst3 = "AND RESET THE DEVICE";
    
    int inst1_width = (int)strlen(inst1) * FONT_WIDTH;
    int inst1_x = (SCREEN_WIDTH - inst1_width) / 2;
    draw_text(screen_buffer, inst1_x, MENU_Y + 80, inst1, COLOR_YELLOW);
    
    int inst2_width = (int)strlen(inst2) * FONT_WIDTH;
    int inst2_x = (SCREEN_WIDTH - inst2_width) / 2;
    draw_text(screen_buffer, inst2_x, MENU_Y + 92, inst2, COLOR_YELLOW);
    
    int inst3_width = (int)strlen(inst3) * FONT_WIDTH;
    int inst3_x = (SCREEN_WIDTH - inst3_width) / 2;
    draw_text(screen_buffer, inst3_x, MENU_Y + 104, inst3, COLOR_YELLOW);
    
    // Draw footer
    draw_footer(screen_buffer);
    
    // Block forever - no input handling
    while (1) {
        tight_loop_contents();
    }
}

bool rom_selector_show(char *selected_rom_path, size_t buffer_size, uint8_t *screen_buffer) {
#if ENABLE_LOGGING
    printf("ROM Selector: Starting...\n");
#endif

    // Allocate screen save buffer from PSRAM for settings menu
    if (saved_screen_buffer == NULL) {
        saved_screen_buffer = (uint8_t *)psram_malloc(SCREEN_WIDTH * SCREEN_HEIGHT);
        if (saved_screen_buffer == NULL) {
#if ENABLE_LOGGING
            printf("Warning: Could not allocate screen save buffer\n");
#endif
        }
    }

    // Scan ROMs first while screen is blank
    memset(screen_buffer, 1, SCREEN_WIDTH * SCREEN_HEIGHT);
    scan_roms();
    
#if 0  // Warning splash disabled for now
    // Show warning splash
    draw_warning_splash(screen_buffer);
    for (int i = 0; i < 3000; ++i) {
        sleep_ms(1);
    }
#endif
    
    // Go directly to ROM selector - no clearing in between
#if ENABLE_LOGGING
    printf("ROM Selector: Found %d ROMs\n", rom_count);
#endif
#if ENABLE_LOGGING
    for (int i = 0; i < rom_count && i < 10; i++) {
        printf("  %d: %s\n", i, rom_list[i].display_name);
    }
#endif
    
    int selected = 0;
    int scroll_offset = 0;
    uint32_t header_phase = 0;

    draw_demostyle_header(screen_buffer, header_phase);
    draw_info_text(screen_buffer);

    // Initial render - initialize animation positions
    scrollbar_current_y = 0;
    scrollbar_target_y = 0;
    cursor_current_y = MENU_Y - 1;  // Initialize cursor position (-1 for +1px up offset)
    cursor_target_y = MENU_Y - 1;
    prev_scroll_offset = -1;
    prev_cursor_y = cursor_current_y;
    render_rom_menu(screen_buffer, selected, scroll_offset, true);
    
#if ENABLE_LOGGING
    printf("ROM Selector: Menu rendered, waiting for input...\n");
#endif
    
    // If no ROMs found, wait a bit and return false
    if (rom_count == 0) {
#if ENABLE_LOGGING
        printf("ROM Selector: No ROMs found!\n");
#endif
        sleep_ms(3000);
        return false;
    }
    
    // Wait for input
    int prev_selected = -1;
    int prev_scroll = -1;
    uint32_t prev_buttons = 0;  // Track previous button state
    uint32_t hold_counter = 0;  // For key repeat
    const uint32_t REPEAT_DELAY = 10;  // Initial delay before repeat (10 * 50ms = 500ms)
    const uint32_t REPEAT_RATE = 3;    // Repeat rate (3 * 50ms = 150ms)
    
    // Wait a moment for display to settle
    sleep_ms(100);
    
    while (true) {
        draw_demostyle_header(screen_buffer, header_phase);
        header_phase = (header_phase + 2) & 0x3F;

        // Track if selection changed for scroll offset updates
        bool selection_changed = (selected != prev_selected || scroll_offset != prev_scroll);
        if (selection_changed) {
            prev_selected = selected;
            prev_scroll = scroll_offset;
        }
        
        // Render menu (handles animation internally with smart partial redraws)
        render_rom_menu(screen_buffer, selected, scroll_offset, selection_changed);
        
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
        if (kbd_state & KBD_STATE_A)     buttons |= DPAD_A;      // A key = confirm
        if (kbd_state & KBD_STATE_B)     buttons |= DPAD_B;      // S key = back
        if (kbd_state & KBD_STATE_START) buttons |= DPAD_START;  // Enter = confirm
        if (kbd_state & KBD_STATE_ESC)   buttons |= (DPAD_SELECT | DPAD_START);  // ESC = open settings
        
#ifdef USB_HID_ENABLED
        // Poll USB and merge USB gamepad state
        usbhid_task();
        if (usbhid_gamepad_connected()) {
            usbhid_gamepad_state_t gp;
            usbhid_get_gamepad_state(&gp);
            
            // Merge USB gamepad D-pad with nespad buttons
            if (gp.dpad & 0x01) buttons |= DPAD_UP;
            if (gp.dpad & 0x02) buttons |= DPAD_DOWN;
            if (gp.dpad & 0x04) buttons |= DPAD_LEFT;
            if (gp.dpad & 0x08) buttons |= DPAD_RIGHT;
            // Merge USB gamepad buttons (A=confirm, Start=confirm)
            if (gp.buttons & 0x01) buttons |= DPAD_A;     // A button
            if (gp.buttons & 0x02) buttons |= DPAD_B;     // B button
            if (gp.buttons & 0x40) buttons |= DPAD_START; // Start button
        } else {
            static int not_conn = 0;
            if ((not_conn++ % 100) == 0) {
    #if ENABLE_LOGGING
            printf("USB gamepad not connected\n");
#endif
            }
        }
#endif
        
        // Detect button press (transition from not pressed to pressed)
        uint32_t buttons_pressed = buttons & ~prev_buttons;
        
        // Key repeat logic for up/down
        bool up_repeat = false;
        bool down_repeat = false;
        if (buttons & (DPAD_UP | DPAD_DOWN)) {
            hold_counter++;
            if (hold_counter > REPEAT_DELAY && ((hold_counter - REPEAT_DELAY) % REPEAT_RATE == 0)) {
                if (buttons & DPAD_UP) up_repeat = true;
                if (buttons & DPAD_DOWN) down_repeat = true;
            }
        } else {
            hold_counter = 0;
        }
        prev_buttons = buttons;
        
        // Handle input (on button press or repeat)
        if ((buttons_pressed & DPAD_UP) || up_repeat) {
            selected--;
            if (selected < 0) {
                selected = rom_count - 1;  // Wrap to last
                scroll_offset = (rom_count > VISIBLE_LINES) ? (rom_count - VISIBLE_LINES) : 0;
            } else if (selected < scroll_offset) {
                scroll_offset = selected;
            }
        }
        
        if ((buttons_pressed & DPAD_DOWN) || down_repeat) {
            selected++;
            if (selected >= rom_count) {
                selected = 0;  // Wrap to first
                scroll_offset = 0;
            } else if (selected >= scroll_offset + VISIBLE_LINES) {
                scroll_offset = selected - VISIBLE_LINES + 1;
            }
        }
        
        // Check for Start+Select combo (for settings menu)
        if ((buttons & DPAD_SELECT) && (buttons & DPAD_START)) {
            // Wait for buttons to be released
            while ((nespad_state & DPAD_SELECT) && (nespad_state & DPAD_START)) {
                nespad_read();
                sleep_ms(50);
            }
            
            // Save screen BEFORE showing settings
            if (saved_screen_buffer != NULL) {
                memcpy(saved_screen_buffer, screen_buffer, SCREEN_WIDTH * SCREEN_HEIGHT);
            }
            
            // Set up palette for settings menu
            graphics_set_palette(48, 0xFFFF00);  // Yellow for highlight
            graphics_set_palette(42, 0x808080);  // Gray
            graphics_set_palette(32, 0xFF0000);  // Red
            graphics_restore_sync_colors();
            
            // Show settings menu
            settings_result_t result = settings_menu_show_with_restore(screen_buffer, saved_screen_buffer);
            
            switch (result) {
                case SETTINGS_RESULT_SAVE_RESTART:
                    settings_save();
                    watchdog_reboot(0, 0, 10);
                    while(1) tight_loop_contents();
                    break;
                    
                case SETTINGS_RESULT_RESTART:
                    watchdog_reboot(0, 0, 10);
                    while(1) tight_loop_contents();
                    break;
                    
                case SETTINGS_RESULT_CANCEL:
                default:
                    // Restore saved screen (ROM selector palette unchanged, no need to restore)
                    if (saved_screen_buffer != NULL) {
                        memcpy(screen_buffer, saved_screen_buffer, SCREEN_WIDTH * SCREEN_HEIGHT);
                    }
                    prev_buttons = 0;  // Reset button state tracking
                    // Button wait already done in settings.c
                    break;
            }
            continue;
        }
        
        // Only confirm ROM selection if Start/A is pressed WITHOUT Select
        if (buttons_pressed & (DPAD_A | DPAD_START)) {
            // If Select is also held, this is the settings combo - don't confirm ROM
            if (!(buttons & DPAD_SELECT)) {
                if (rom_count > 0) {
                    // Build full path
                    snprintf(selected_rom_path, buffer_size, "/genesis/%s", rom_list[selected].filename);
                    return true;
                }
            }
        }
        
        sleep_ms(animation_in_progress() ? 16 : 50);  // Faster rate during animation (60fps vs 20fps)
    }
    
    return false;
}
