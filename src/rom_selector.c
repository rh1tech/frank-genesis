/*
 * ROM Selector - Welcome screen + file browser for frank-genesis.
 *
 * Flow:
 *   welcome_screen_show() → brief splash with starfield + logo.
 *   rom_selector_show()   → scans /genesis; shows no-roms notice if empty;
 *                           then opens the file browser (rooted at /genesis
 *                           or the last visited directory).
 *
 * Rendering is direct-to-framebuffer (320x240 8-bit indexed). Palette is
 * managed via graphics_set_palette() and kept inside a small reserved band
 * (see PAL_* below) so we don't collide with the Genesis emulator palette.
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
#include <stdlib.h>
#include <ctype.h>

#ifndef FRANK_GENESIS_VERSION
#define FRANK_GENESIS_VERSION "?"
#endif

#ifdef USB_HID_ENABLED
#include "usbhid/usbhid.h"
#endif

// ───────────────────────────────────────────────────────────────────────────
// Screen constants
// ───────────────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

#define FONT_WIDTH       6   // 5px glyph + 1px spacing
#define FONT_HEIGHT      7
#define BOLD_FONT_WIDTH  8   // 7px glyph + 1px spacing
#define BOLD_FONT_HEIGHT 9

// ───────────────────────────────────────────────────────────────────────────
// Palette slots used by all selector screens.
//
// Genesis emulator uses 0..63 during gameplay; while the UI is active we
// overwrite a small set of these slots. graphics_restore_sync_colors() must
// run after every change so HDMI sync symbols stay intact.
// ───────────────────────────────────────────────────────────────────────────
#define PAL_BLACK        1   // near-black backdrop (index 0/1 both near-black)
#define PAL_BG_TINT      2   // deep blue-black welcome/browser background
#define PAL_DARK_GRAY    16  // scrollbar track
#define PAL_RED          32  // accent (errors, warnings)
#define PAL_GRAY         42  // medium gray for hints and body text
#define PAL_YELLOW       48  // reserved for settings (used by settings menu)
#define PAL_CYAN         50  // Genesis bright cyan (accent / buttons)
#define PAL_BLUE         51  // Genesis azure (logo body / header)
#define PAL_NAVY         52  // Genesis deep navy (logo shade)
#define PAL_LIGHT_CYAN   53  // logo highlight / star near-field
#define PAL_STAR_DIM     54  // dim star (far)
#define PAL_STAR_MID     55  // mid star
#define PAL_CART_LIGHT   56  // browser "<DIR>" / file size column
#define PAL_CART_DARK    57  // browser selected-row background
#define PAL_LOGO_SHADOW  58  // soft logo shadow
#define PAL_WHITE        63  // bright white foreground

// ───────────────────────────────────────────────────────────────────────────
// Screen save buffer for the settings menu pop-over.
// Allocated once on first use from PSRAM.
// ───────────────────────────────────────────────────────────────────────────
static uint8_t *saved_screen_buffer = NULL;

// Scan result from the last rom_selector_show() entry.
static rom_scan_result_t g_scan_result = ROM_SCAN_OK;

// Persistent browser state. Seeded from g_settings on first entry and saved
// back on every directory change / ROM launch so we come up where we left off.
static char fb_persist_path[MAX_ROM_PATH];
static int  fb_persist_selected = 0;
static int  fb_persist_scroll = 0;
static bool fb_persist_valid = false;

// ───────────────────────────────────────────────────────────────────────────
// 5x7 font (same glyph table as the original ROM selector)
// ───────────────────────────────────────────────────────────────────────────
static const uint8_t *glyph_5x7(char ch) {
    static const uint8_t g_space[7]   = {0,0,0,0,0,0,0};
    static const uint8_t g_excl[7]    = {0x04,0x04,0x04,0x04,0x00,0x04,0x00};
    static const uint8_t g_dot[7]     = {0,0,0,0,0,0x0C,0x0C};
    static const uint8_t g_hyphen[7]  = {0,0,0,0x1F,0,0,0};
    static const uint8_t g_uscore[7]  = {0,0,0,0,0,0,0x1F};
    static const uint8_t g_colon[7]   = {0,0x0C,0x0C,0,0x0C,0x0C,0};
    static const uint8_t g_slash[7]   = {0x01,0x02,0x04,0x08,0x10,0,0};
    static const uint8_t g_pct[7]     = {0x11,0x02,0x04,0x08,0x11,0,0};
    static const uint8_t g_comma[7]   = {0,0,0,0,0,0x06,0x04};
    static const uint8_t g_lparen[7]  = {0x02,0x04,0x08,0x08,0x08,0x04,0x02};
    static const uint8_t g_rparen[7]  = {0x08,0x04,0x02,0x02,0x02,0x04,0x08};
    static const uint8_t g_less[7]    = {0x02,0x04,0x08,0x10,0x08,0x04,0x02};
    static const uint8_t g_greater[7] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08};
    static const uint8_t g_0[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
    static const uint8_t g_1[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t g_2[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F};
    static const uint8_t g_3[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E};
    static const uint8_t g_4[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
    static const uint8_t g_5[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E};
    static const uint8_t g_6[7] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E};
    static const uint8_t g_7[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08};
    static const uint8_t g_8[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
    static const uint8_t g_9[7] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E};
    static const uint8_t g_A[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t g_B[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
    static const uint8_t g_C[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
    static const uint8_t g_D[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E};
    static const uint8_t g_E[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
    static const uint8_t g_F[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
    static const uint8_t g_G[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E};
    static const uint8_t g_H[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t g_I[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F};
    static const uint8_t g_J[7] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C};
    static const uint8_t g_K[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11};
    static const uint8_t g_L[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
    static const uint8_t g_M[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
    static const uint8_t g_N[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11};
    static const uint8_t g_O[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t g_P[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
    static const uint8_t g_Q[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D};
    static const uint8_t g_R[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
    static const uint8_t g_S[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
    static const uint8_t g_T[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t g_U[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t g_V[7] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04};
    static const uint8_t g_W[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A};
    static const uint8_t g_X[7] = {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11};
    static const uint8_t g_Y[7] = {0x11,0x0A,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t g_Z[7] = {0x1F,0x02,0x04,0x08,0x10,0x10,0x1F};

    int c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    switch (c) {
        case ' ': return g_space;
        case '!': return g_excl;
        case '.': return g_dot;
        case '-': return g_hyphen;
        case '_': return g_uscore;
        case ':': return g_colon;
        case '/': return g_slash;
        case '%': return g_pct;
        case ',': return g_comma;
        case '(': return g_lparen;
        case ')': return g_rparen;
        case '<': return g_less;
        case '>': return g_greater;
        case '0': return g_0; case '1': return g_1; case '2': return g_2;
        case '3': return g_3; case '4': return g_4; case '5': return g_5;
        case '6': return g_6; case '7': return g_7; case '8': return g_8;
        case '9': return g_9;
        case 'A': return g_A; case 'B': return g_B; case 'C': return g_C;
        case 'D': return g_D; case 'E': return g_E; case 'F': return g_F;
        case 'G': return g_G; case 'H': return g_H; case 'I': return g_I;
        case 'J': return g_J; case 'K': return g_K; case 'L': return g_L;
        case 'M': return g_M; case 'N': return g_N; case 'O': return g_O;
        case 'P': return g_P; case 'Q': return g_Q; case 'R': return g_R;
        case 'S': return g_S; case 'T': return g_T; case 'U': return g_U;
        case 'V': return g_V; case 'W': return g_W; case 'X': return g_X;
        case 'Y': return g_Y; case 'Z': return g_Z;
        default: return g_space;
    }
}

static const uint8_t *glyph_bold(char ch) {
    static const uint8_t g_space[9] = {0,0,0,0,0,0,0,0,0};
    static const uint8_t g_A[9] = {0x1C,0x36,0x63,0x63,0x7F,0x63,0x63,0x63,0x63};
    static const uint8_t g_E[9] = {0x7F,0x60,0x60,0x60,0x7E,0x60,0x60,0x60,0x7F};
    static const uint8_t g_F[9] = {0x7F,0x60,0x60,0x60,0x7E,0x60,0x60,0x60,0x60};
    static const uint8_t g_G[9] = {0x3E,0x63,0x60,0x60,0x6F,0x63,0x63,0x63,0x3E};
    static const uint8_t g_I[9] = {0x3E,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3E};
    static const uint8_t g_K[9] = {0x63,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x63};
    static const uint8_t g_N[9] = {0x63,0x73,0x7B,0x7F,0x6F,0x67,0x63,0x63,0x63};
    static const uint8_t g_R[9] = {0x7E,0x63,0x63,0x63,0x7E,0x6C,0x66,0x63,0x63};
    static const uint8_t g_S[9] = {0x3E,0x63,0x60,0x70,0x3E,0x07,0x03,0x63,0x3E};

    int c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    switch (c) {
        case 'A': return g_A; case 'E': return g_E; case 'F': return g_F;
        case 'G': return g_G; case 'I': return g_I; case 'K': return g_K;
        case 'N': return g_N; case 'R': return g_R; case 'S': return g_S;
        default: return g_space;
    }
}

// ───────────────────────────────────────────────────────────────────────────
// Framebuffer primitives
// ───────────────────────────────────────────────────────────────────────────
static inline void fb_pixel(uint8_t *fb, int x, int y, uint8_t color) {
    if ((unsigned)x < SCREEN_WIDTH && (unsigned)y < SCREEN_HEIGHT)
        fb[y * SCREEN_WIDTH + x] = color;
}

static void fb_fill(uint8_t *fb, uint8_t color) {
    // Avoid index 0 — HDMI at 378MHz occasionally glitches on pure black.
    if (color == 0) color = 1;
    memset(fb, color, SCREEN_WIDTH * SCREEN_HEIGHT);
}

static void fill_rect(uint8_t *fb, int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_WIDTH)  w = SCREEN_WIDTH  - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    if (color == 0) color = 1;
    for (int yy = y; yy < y + h; ++yy)
        memset(&fb[yy * SCREEN_WIDTH + x], color, (size_t)w);
}

static void draw_hline(uint8_t *fb, int x, int y, int w, uint8_t color) {
    if (y < 0 || y >= SCREEN_HEIGHT) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
    if (w <= 0) return;
    memset(&fb[y * SCREEN_WIDTH + x], color, (size_t)w);
}

static void draw_char(uint8_t *fb, int x, int y, char ch, uint8_t color) {
    const uint8_t *rows = glyph_5x7(ch);
    for (int row = 0; row < FONT_HEIGHT; ++row) {
        int yy = y + row;
        if ((unsigned)yy >= SCREEN_HEIGHT) continue;
        uint8_t bits = rows[row];
        for (int col = 0; col < 5; ++col) {
            int xx = x + col;
            if ((unsigned)xx >= SCREEN_WIDTH) continue;
            if (bits & (1u << (4 - col)))
                fb[yy * SCREEN_WIDTH + xx] = color;
        }
    }
}

static void draw_text(uint8_t *fb, int x, int y, const char *s, uint8_t color) {
    for (const char *p = s; *p; ++p) {
        draw_char(fb, x, y, *p, color);
        x += FONT_WIDTH;
    }
}

static void draw_text_center(uint8_t *fb, int y, const char *s, uint8_t color) {
    int w = (int)strlen(s) * FONT_WIDTH;
    draw_text(fb, (SCREEN_WIDTH - w) / 2, y, s, color);
}

// Draw text twice: shadow offset by (+1,+1), then foreground glyph on top.
static void draw_text_center_shadow(uint8_t *fb, int y, const char *s,
                                    uint8_t fg, uint8_t shadow) {
    int x = (SCREEN_WIDTH - (int)strlen(s) * FONT_WIDTH) / 2;
    draw_text(fb, x + 1, y + 1, s, shadow);
    draw_text(fb, x,     y,     s, fg);
}

// Bold font drawing for the header title.
static void draw_char_bold(uint8_t *fb, int x, int y, char ch, uint8_t color) {
    const uint8_t *rows = glyph_bold(ch);
    for (int row = 0; row < BOLD_FONT_HEIGHT; ++row) {
        int yy = y + row;
        if ((unsigned)yy >= SCREEN_HEIGHT) continue;
        uint8_t bits = rows[row];
        for (int col = 0; col < 7; ++col) {
            int xx = x + col;
            if ((unsigned)xx >= SCREEN_WIDTH) continue;
            if (bits & (1u << (6 - col)))
                fb[yy * SCREEN_WIDTH + xx] = color;
        }
    }
}

static void draw_text_bold(uint8_t *fb, int x, int y, const char *s, uint8_t color) {
    for (const char *p = s; *p; ++p) {
        draw_char_bold(fb, x, y, *p, color);
        x += BOLD_FONT_WIDTH;
    }
}

// ───────────────────────────────────────────────────────────────────────────
// Palette setup for selector screens
// ───────────────────────────────────────────────────────────────────────────
static void setup_ui_palette(void) {
    graphics_set_palette(0,  0x020202);   // HDMI-safe black
    graphics_set_palette(1,  0x020202);
    graphics_set_palette(PAL_BG_TINT,     0x080C18);  // deep blue-black
    graphics_set_palette(PAL_DARK_GRAY,   0x404040);
    graphics_set_palette(PAL_RED,         0xFF3030);
    graphics_set_palette(PAL_GRAY,        0x909090);
    graphics_set_palette(PAL_YELLOW,      0xFFE030);
    graphics_set_palette(PAL_CYAN,        0x18D4F0);  // Genesis bright cyan
    graphics_set_palette(PAL_BLUE,        0x2470E0);  // Genesis azure
    graphics_set_palette(PAL_NAVY,        0x0A2060);  // deep navy
    graphics_set_palette(PAL_LIGHT_CYAN,  0xA0E8FF);
    graphics_set_palette(PAL_STAR_DIM,    0x3A4458);
    graphics_set_palette(PAL_STAR_MID,    0x808CA8);
    graphics_set_palette(PAL_CART_LIGHT,  0x70C0E8);  // browser <DIR> / size
    graphics_set_palette(PAL_CART_DARK,   0x15305A);  // selected row bg
    graphics_set_palette(PAL_LOGO_SHADOW, 0x1A2840);
    graphics_set_palette(PAL_WHITE,       0xFFFFFF);
    graphics_restore_sync_colors();
}

// ───────────────────────────────────────────────────────────────────────────
// Input polling — reads gamepad, PS/2 keyboard, and USB HID into one mask.
// ───────────────────────────────────────────────────────────────────────────
typedef enum {
    SEL_BTN_UP     = DPAD_UP,
    SEL_BTN_DOWN   = DPAD_DOWN,
    SEL_BTN_LEFT   = DPAD_LEFT,
    SEL_BTN_RIGHT  = DPAD_RIGHT,
    SEL_BTN_A      = DPAD_A,
    SEL_BTN_B      = DPAD_B,
    SEL_BTN_START  = DPAD_START,
    SEL_BTN_SELECT = DPAD_SELECT,
} sel_btn_t;

static uint32_t read_selector_buttons(void) {
    nespad_read();
    uint32_t buttons = nespad_state;

    ps2kbd_tick();
    uint16_t kbd = ps2kbd_get_state();
#ifdef USB_HID_ENABLED
    kbd |= usbhid_get_kbd_state();
#endif
    if (kbd & KBD_STATE_UP)    buttons |= DPAD_UP;
    if (kbd & KBD_STATE_DOWN)  buttons |= DPAD_DOWN;
    if (kbd & KBD_STATE_LEFT)  buttons |= DPAD_LEFT;
    if (kbd & KBD_STATE_RIGHT) buttons |= DPAD_RIGHT;
    if (kbd & KBD_STATE_A)     buttons |= DPAD_A;
    if (kbd & KBD_STATE_B)     buttons |= DPAD_B;
    if (kbd & KBD_STATE_START) buttons |= DPAD_START;
    if (kbd & KBD_STATE_ESC)   buttons |= (DPAD_SELECT | DPAD_START);

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
        if (gp.buttons & 0x40) buttons |= DPAD_START;
    }
#endif
    return buttons;
}

// Frame pacing — the emulator isn't running so we hand-roll a 60Hz loop.
static void selector_tick_16ms(void) { sleep_ms(16); }

// Block until no input is held. Used after every "modal" screen so the
// triggering press doesn't leak into the next screen.
static void wait_buttons_released(void) {
    for (int i = 0; i < 60; ++i) {
        if (read_selector_buttons() == 0) return;
        selector_tick_16ms();
    }
}

// ───────────────────────────────────────────────────────────────────────────
// Demoscene-style animated header, shared across browser / no-roms / SD error
// ───────────────────────────────────────────────────────────────────────────
#define HEADER_Y      16
#define HEADER_HEIGHT 36

static void draw_header(uint8_t *fb, uint32_t phase) {
    const char *title = "FRANK GENESIS";
    int title_width = (int)strlen(title) * BOLD_FONT_WIDTH;
    int title_x = (SCREEN_WIDTH - title_width) / 2;
    int title_y = HEADER_Y + (HEADER_HEIGHT - BOLD_FONT_HEIGHT) / 2;
    int title_left   = title_x - 8;
    int title_right  = title_x + title_width + 8;
    int title_top    = title_y - 5;
    int title_bottom = title_y + BOLD_FONT_HEIGHT + 5;

    // Animated blue/cyan backdrop. Skips the title bounding box so the
    // header text remains stable on a solid plate.
    for (int y = 0; y < HEADER_HEIGHT; ++y) {
        int yy = HEADER_Y + y;
        if ((unsigned)yy >= SCREEN_HEIGHT) continue;
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            if (yy >= title_top && yy < title_bottom &&
                x  >= title_left && x  < title_right) continue;
            uint32_t wave = ((uint32_t)x * 3u + phase) & 0x3F;
            uint32_t tint = (wave + (uint32_t)y) & 0x1F;
            uint8_t c = (tint < 8) ? PAL_NAVY
                      : (tint < 16) ? PAL_BLUE
                      : (tint < 24) ? PAL_CYAN
                                    : PAL_LIGHT_CYAN;
            fb[yy * SCREEN_WIDTH + x] = c;
        }
    }

    // Solid background strip behind the title plus dropshadow.
    fill_rect(fb, title_left, title_top,
              title_right - title_left, title_bottom - title_top, PAL_BG_TINT);
    draw_text_bold(fb, title_x + 1, title_y + 1, title, PAL_NAVY);
    draw_text_bold(fb, title_x,     title_y,     title, PAL_WHITE);
}

static void draw_footer(uint8_t *fb) {
    const char *footer = "CODED BY MIKHAIL MATVEEV";
    int w = (int)strlen(footer) * FONT_WIDTH;
    int x = (SCREEN_WIDTH - w) / 2;
    draw_text(fb, x, SCREEN_HEIGHT - (FONT_HEIGHT + 4), footer, PAL_GRAY);
}

// ───────────────────────────────────────────────────────────────────────────
// Starfield used by the welcome screen.
//
// Each star remembers its previous pixel so we can erase exactly one pixel
// and paint exactly one new pixel per frame, instead of redrawing the whole
// background. That surgical update avoids the HDMI DMA tearing you get from
// a full fb_fill every tick.
// ───────────────────────────────────────────────────────────────────────────
#define STAR_COUNT 72

typedef struct {
    int32_t x, y;    // Q8.8 position
    int16_t vx;      // Q8.8 horizontal velocity (negative = right-to-left)
    int16_t prev_x;  // previous pixel position (for targeted erase), -1 = none
    int16_t prev_y;
    uint8_t color;
    uint8_t _pad;
} star_t;

static star_t g_stars[STAR_COUNT];
static bool   g_stars_ready = false;
static uint32_t g_star_rng = 0xC0FFEE17u;

static uint32_t star_rand(void) {
    g_star_rng = g_star_rng * 1664525u + 1013904223u;
    return g_star_rng;
}

static void init_starfield(void) {
    const uint8_t tier_color[3] = { PAL_STAR_DIM, PAL_STAR_MID, PAL_WHITE };
    const int16_t tier_vx[3]    = { -32, -80, -160 };
    for (int i = 0; i < STAR_COUNT; ++i) {
        int tier = (int)(star_rand() % 3);
        g_stars[i].x = (int32_t)(star_rand() % (SCREEN_WIDTH  << 8));
        g_stars[i].y = (int32_t)(star_rand() % (SCREEN_HEIGHT << 8));
        g_stars[i].vx = tier_vx[tier];
        g_stars[i].color = tier_color[tier];
        g_stars[i].prev_x = -1;
        g_stars[i].prev_y = -1;
    }
    g_stars_ready = true;
}

// Single-pixel erase guard — the logo disk and text areas must not be
// written by stars, or the stars will punch holes through them. Callers
// pass a predicate that returns true when (x,y) is free for the starfield.
static bool star_pixel_allowed(int x, int y,
                               int logo_cx, int logo_cy, int logo_r2,
                               int text_top, int text_bottom) {
    if (y >= text_top && y < text_bottom) return false;
    int dx = x - logo_cx;
    int dy = y - logo_cy;
    if (dx * dx + dy * dy <= logo_r2) return false;
    return true;
}

static void step_starfield(uint8_t *fb,
                           int logo_cx, int logo_cy, int logo_r,
                           int text_top, int text_bottom) {
    if (!g_stars_ready) init_starfield();
    int logo_r2 = (logo_r + 1) * (logo_r + 1);
    for (int i = 0; i < STAR_COUNT; ++i) {
        // Erase previous pixel (restore background tint) if it was drawn.
        if (g_stars[i].prev_x >= 0)
            fb_pixel(fb, g_stars[i].prev_x, g_stars[i].prev_y, PAL_BG_TINT);

        g_stars[i].x += g_stars[i].vx;
        if (g_stars[i].x < 0) g_stars[i].x += (SCREEN_WIDTH << 8);
        else if (g_stars[i].x >= (SCREEN_WIDTH << 8))
            g_stars[i].x -= (SCREEN_WIDTH << 8);

        int sx = g_stars[i].x >> 8;
        int sy = g_stars[i].y >> 8;
        if (star_pixel_allowed(sx, sy, logo_cx, logo_cy, logo_r2,
                               text_top, text_bottom)) {
            fb_pixel(fb, sx, sy, g_stars[i].color);
            g_stars[i].prev_x = (int16_t)sx;
            g_stars[i].prev_y = (int16_t)sy;
        } else {
            g_stars[i].prev_x = -1;
            g_stars[i].prev_y = -1;
        }
    }
}

// ───────────────────────────────────────────────────────────────────────────
// Welcome screen — Genesis-themed controller logo on a starfield backdrop.
//
// 26×12 pixel logo, scaled ×3. The bitmap below sketches a Genesis-ish
// 3-button pad: D-pad on the left, three action buttons (A/B/C) on the right.
// Uses the Genesis palette (blue/cyan instead of NES red/gray) so the
// welcome screen clearly reads "Genesis" rather than "NES".
//
// Pixel legend: 0 = transparent, 1 = black outline, 2 = body navy,
//               3 = body blue, 4 = body light accent, 5 = D-pad fill,
//               6 = A/B/C button (bright cyan), 7 = button ring
// ───────────────────────────────────────────────────────────────────────────
#define LOGO_W 26
#define LOGO_H 12

static const uint8_t genesis_logo[LOGO_H][LOGO_W] = {
    {0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1,0,0},
    {1,4,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1,0},
    {1,3,3,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1,0},
    {1,3,2,2,5,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1,0},
    {1,3,2,5,5,5,2,3,3,3,3,3,3,3,3,7,7,3,7,7,3,7,7,3,1,0},
    {1,3,2,5,5,5,2,3,3,3,3,3,3,3,7,6,6,7,6,6,7,6,6,7,1,0},
    {1,3,2,5,5,5,2,3,3,3,3,3,3,3,7,6,6,7,6,6,7,6,6,7,1,0},
    {1,3,2,2,5,2,2,3,3,3,3,3,3,3,3,7,7,3,7,7,3,7,7,3,1,0},
    {1,3,3,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1,0},
    {0,1,4,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1,0,0},
    {0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
};

static const uint8_t logo_pal[8] = {
    0,              // 0 transparent
    PAL_BLACK,      // 1 outline
    PAL_NAVY,       // 2 body shadow
    PAL_BLUE,       // 3 body blue
    PAL_LIGHT_CYAN, // 4 top highlight
    PAL_WHITE,      // 5 D-pad fill
    PAL_CYAN,       // 6 A/B/C button
    PAL_LIGHT_CYAN, // 7 button ring
};

static void draw_logo_3x(uint8_t *fb, int ox, int oy) {
    for (int y = 0; y < LOGO_H; ++y) {
        for (int x = 0; x < LOGO_W; ++x) {
            uint8_t px = genesis_logo[y][x];
            if (px == 0) continue;
            uint8_t c = logo_pal[px];
            int dx = ox + x * 3;
            int dy = oy + y * 3;
            for (int sy = 0; sy < 3; ++sy)
                for (int sx = 0; sx < 3; ++sx)
                    fb_pixel(fb, dx + sx, dy + sy, c);
        }
    }
}

static void draw_filled_circle(uint8_t *fb, int cx, int cy, int r, uint8_t color) {
    int four_r2 = 4 * r * r;
    for (int y = cy - r; y <= cy + r; ++y) {
        if ((unsigned)y >= SCREEN_HEIGHT) continue;
        int dy = y - cy;
        int limit = four_r2 - 4 * dy * dy;
        if (limit <= 0) continue;
        int dx = 0;
        while ((2 * dx + 3) * (2 * dx + 3) <= limit) dx++;
        int x0 = cx - dx;
        int x1 = cx + dx;
        if (x0 < 0) x0 = 0;
        if (x1 >= SCREEN_WIDTH) x1 = SCREEN_WIDTH - 1;
        if (x0 <= x1) memset(&fb[y * SCREEN_WIDTH + x0], color, x1 - x0 + 1);
    }
}

void welcome_screen_show(uint8_t *screen_buffer) {
    setup_ui_palette();
    g_stars_ready = false;  // fresh pattern on every boot

    // Version + board + clocks. Rendered only on the welcome screen so the
    // file browser can stay uncluttered.
#ifdef BOARD_M2
    const char *board = "M2";
#else
    const char *board = "M1";
#endif
    uint32_t cpu_mhz   = (clock_get_hz(clk_sys) + 500000) / 1000000;
    uint32_t psram_mhz = g_settings.psram_freq;
    char version_str[48];
    snprintf(version_str, sizeof(version_str), "V%s  %s  %u / %u MHZ",
             FRANK_GENESIS_VERSION, board, cpu_mhz, psram_mhz);

    // --- Draw static chrome ONCE. After this, only the starfield pixels
    //     and the blink area are updated per frame. Full-screen memsets
    //     every tick were racing HDMI DMA and causing the moving black line.
    const int cx = SCREEN_WIDTH / 2;
    const int cy = 80;
    const int r  = 46;
    const int blink_y = 220;
    const int blink_h = FONT_HEIGHT + 2;

    fb_fill(screen_buffer, PAL_BG_TINT);

    draw_filled_circle(screen_buffer, cx, cy + 1, r + 1, PAL_LOGO_SHADOW);
    draw_filled_circle(screen_buffer, cx, cy, r, PAL_NAVY);
    draw_filled_circle(screen_buffer, cx, cy, r - 3, PAL_BG_TINT);
    draw_logo_3x(screen_buffer, cx - (LOGO_W * 3) / 2, cy - (LOGO_H * 3) / 2);

    draw_text_center_shadow(screen_buffer, 148, "FRANK GENESIS", PAL_WHITE,  PAL_BLACK);
    draw_text_center_shadow(screen_buffer, 162, version_str,     PAL_CYAN,   PAL_BLACK);
    draw_text_center_shadow(screen_buffer, 184, "BY MIKHAIL MATVEEV", PAL_LIGHT_CYAN, PAL_BLACK);
    draw_text_center_shadow(screen_buffer, 196, "GITHUB.COM/RH1TECH/FRANK-GENESIS",
                            PAL_LIGHT_CYAN, PAL_BLACK);

    // Bounding box of the central text block — starfield won't stamp into it.
    const int text_top = 147;
    const int text_bottom = 205;

    uint32_t frame = 0;
    uint32_t prev_buttons = read_selector_buttons();  // ignore initial state
    bool blink_on = false;

    while (true) {
        // Only the starfield and the PRESS START line change. We erase each
        // star's previous pixel individually, then advance it.
        step_starfield(screen_buffer, cx, cy, r, text_top, text_bottom);

        if (frame >= 120) {
            bool want_on = ((frame / 30) & 1) == 0;
            if (want_on != blink_on) {
                // Repaint just the blink strip — a local 1-line rect, not
                // a full-screen fill, so HDMI stays stable.
                fill_rect(screen_buffer, 0, blink_y - 1, SCREEN_WIDTH, blink_h, PAL_BG_TINT);
                if (want_on)
                    draw_text_center_shadow(screen_buffer, blink_y,
                                            "PRESS START", PAL_WHITE, PAL_BLACK);
                blink_on = want_on;
            }
        }

        frame++;
        selector_tick_16ms();

        if (frame >= 120) {
            uint32_t buttons = read_selector_buttons();
            if (buttons & ~prev_buttons) break;
            prev_buttons = buttons;
        }

        // Auto-continue after ~10 seconds so the unit isn't stuck on the
        // splash if nobody touches the pad.
        if (frame >= 600) break;
    }

    wait_buttons_released();
}

// ───────────────────────────────────────────────────────────────────────────
// No-ROMs notice
// ───────────────────────────────────────────────────────────────────────────
void rom_selector_no_roms_notice(uint8_t *screen_buffer, rom_scan_result_t result) {
    setup_ui_palette();

    const char *line1, *line2;
    switch (result) {
        case ROM_SCAN_NO_GENESIS_DIR:
            line1 = "NO /GENESIS DIRECTORY ON SD CARD";
            line2 = "CREATE A /GENESIS FOLDER";
            break;
        case ROM_SCAN_NO_ROMS:
        default:
            line1 = "NO ROMS FOUND IN /GENESIS";
            line2 = "COPY .MD / .BIN / .GEN / .SMD FILES";
            break;
    }

    uint32_t prev_buttons = read_selector_buttons();
    uint32_t frame = 0;
    uint32_t header_phase = 0;

    while (true) {
        fb_fill(screen_buffer, PAL_BG_TINT);
        draw_header(screen_buffer, header_phase);
        header_phase = (header_phase + 2) & 0x3F;

        draw_text_center(screen_buffer, 72, "ROM LIBRARY EMPTY", PAL_WHITE);
        draw_hline(screen_buffer, 40, 86, SCREEN_WIDTH - 80, PAL_GRAY);

        draw_text_center(screen_buffer, 102, line1, PAL_LIGHT_CYAN);
        draw_text_center(screen_buffer, 116, line2, PAL_LIGHT_CYAN);

        draw_text_center(screen_buffer, 148,
                         "THE FILE BROWSER WILL OPEN NEXT", PAL_GRAY);
        draw_text_center(screen_buffer, 162,
                         "USE IT TO PICK A ROM FROM ANY FOLDER", PAL_GRAY);

        if (((frame / 30) & 1) == 0)
            draw_text_center(screen_buffer, 200,
                             "PRESS ANY BUTTON TO CONTINUE", PAL_WHITE);

        draw_footer(screen_buffer);

        frame++;
        selector_tick_16ms();

        uint32_t buttons = read_selector_buttons();
        if (buttons & ~prev_buttons) break;
        prev_buttons = buttons;

        if (frame >= 600) break;  // auto-continue after ~10s
    }

    wait_buttons_released();
}

// ───────────────────────────────────────────────────────────────────────────
// File browser
//
// Layout is intentionally static — no per-frame animation — so the CPU
// isn't rewriting the entire framebuffer every tick, which was racing HDMI
// DMA and producing a moving black seam.
// ───────────────────────────────────────────────────────────────────────────
#define FB_MAX_ENTRIES     256
#define FB_PATH_Y          8
#define FB_DIVIDER_Y       20
#define FB_LIST_Y          28     // roomy gap under the path bar
#define FB_LINE_H          10     // taller rows so text breathes
#define FB_NAME_X          12
#define FB_FOOTER_Y        (SCREEN_HEIGHT - 14)
#define FB_LIST_BOTTOM     (FB_FOOTER_Y - 6)
#define FB_VISIBLE_LINES   ((FB_LIST_BOTTOM - FB_LIST_Y) / FB_LINE_H)
#define FB_SCROLLBAR_W     4
#define FB_SCROLLBAR_X     (SCREEN_WIDTH - 12)

typedef struct {
    char name[96];
    bool is_dir;
    uint32_t size;
} fb_entry_t;

static fb_entry_t *fb_entries = NULL;     // allocated once from PSRAM
static int fb_entry_count = 0;

// Recognised Genesis/Megadrive ROM extensions.
static bool is_genesis_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".md")  == 0 ||
            strcasecmp(dot, ".bin") == 0 ||
            strcasecmp(dot, ".gen") == 0 ||
            strcasecmp(dot, ".smd") == 0);
}

static int fb_sort_cmp(const void *a, const void *b) {
    const fb_entry_t *ea = (const fb_entry_t *)a;
    const fb_entry_t *eb = (const fb_entry_t *)b;
    // Directories before files
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    const char *sa = ea->name, *sb = eb->name;
    for (;; ++sa, ++sb) {
        int ca = (*sa >= 'a' && *sa <= 'z') ? *sa - 32 : *sa;
        int cb = (*sb >= 'a' && *sb <= 'z') ? *sb - 32 : *sb;
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
}

static int fb_scan_dir(const char *path) {
    fb_entry_count = 0;
    DIR dir;
    if (f_opendir(&dir, path) != FR_OK) return 0;

    int sort_start = 0;
    // ".." entry to walk back up unless we're at SD root.
    if (strlen(path) > 1) {
        strcpy(fb_entries[0].name, "..");
        fb_entries[0].is_dir = true;
        fb_entries[0].size = 0;
        fb_entry_count = 1;
        sort_start = 1;
    }

    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0'
           && fb_entry_count < FB_MAX_ENTRIES) {
        if (fno.fname[0] == '.') continue;
        bool is_dir = (fno.fattrib & AM_DIR) != 0;
        if (!is_dir && !is_genesis_file(fno.fname)) continue;
        strncpy(fb_entries[fb_entry_count].name, fno.fname,
                sizeof(fb_entries[0].name) - 1);
        fb_entries[fb_entry_count].name[sizeof(fb_entries[0].name) - 1] = '\0';
        fb_entries[fb_entry_count].is_dir = is_dir;
        fb_entries[fb_entry_count].size = (uint32_t)fno.fsize;
        fb_entry_count++;
    }
    f_closedir(&dir);
    if (fb_entry_count - sort_start > 1)
        qsort(&fb_entries[sort_start], fb_entry_count - sort_start,
              sizeof(fb_entry_t), fb_sort_cmp);
    return fb_entry_count;
}

static void fb_text_trunc(uint8_t *fb, int x, int y, const char *s,
                          uint8_t color, int max_chars) {
    int len = (int)strlen(s);
    if (len <= max_chars) {
        draw_text(fb, x, y, s, color);
        return;
    }
    int cut = max_chars - 3;
    if (cut < 0) cut = 0;
    for (int i = 0; i < cut && s[i]; ++i)
        draw_char(fb, x + i * FONT_WIDTH, y, s[i], color);
    for (int i = 0; i < 3 && cut + i < max_chars; ++i)
        draw_char(fb, x + (cut + i) * FONT_WIDTH, y, '.', color);
}

// Draw the static chrome: path bar, divider, footer legend, scrollbar track.
// Runs once per directory load, not on every scroll.
static void fb_draw_chrome(uint8_t *screen_buffer, const char *path) {
    fb_fill(screen_buffer, PAL_BG_TINT);

    // Path row.
    int max_path_chars = (SCREEN_WIDTH - 24) / FONT_WIDTH;
    fb_text_trunc(screen_buffer, 12, FB_PATH_Y, path, PAL_CYAN, max_path_chars);
    draw_hline(screen_buffer, 0, FB_DIVIDER_Y, SCREEN_WIDTH, PAL_GRAY);

    // Footer legend (static text; never changes).
    draw_text_center(screen_buffer, FB_FOOTER_Y,
                     "A/START: OPEN   B: BACK   L/R: PAGE", PAL_LIGHT_CYAN);

    // Scrollbar track. Thumb is drawn per-scroll in fb_draw_list.
    if (fb_entry_count > FB_VISIBLE_LINES) {
        int bar_top = FB_LIST_Y - 1;
        int bar_h   = FB_VISIBLE_LINES * FB_LINE_H;
        fill_rect(screen_buffer, FB_SCROLLBAR_X, bar_top,
                  FB_SCROLLBAR_W, bar_h, PAL_DARK_GRAY);
    }
}

// Draw a single list row: fill its background, then stamp columns over it.
// Background + content is ~210 bytes of memset + a handful of glyph writes;
// so short that HDMI DMA never catches a half-drawn row.
static void fb_draw_row(uint8_t *screen_buffer, int slot, int text_right,
                        const fb_entry_t *e, bool is_selected) {
    int y = FB_LIST_Y + slot * FB_LINE_H;

    // Atomic row: one fill_rect paints the row background (selection bar or
    // plain BG), then columns draw on top. There is never a window where
    // this row is blank AND unpainted.
    uint8_t bg_color = is_selected ? PAL_CART_DARK : PAL_BG_TINT;
    fill_rect(screen_buffer, 0, y - 1, text_right, FB_LINE_H, bg_color);

    if (e == NULL) return;  // empty slot — just blanked

    uint8_t text_color = is_selected
                           ? PAL_LIGHT_CYAN
                           : (e->is_dir ? PAL_WHITE : PAL_GRAY);

    int name_x = FB_NAME_X + 6 * FONT_WIDTH;
    if (e->is_dir) {
        draw_text(screen_buffer, FB_NAME_X, y, "<DIR>", PAL_CART_LIGHT);
    } else {
        char sz[8];
        uint32_t kb = (e->size + 1023) / 1024;
        if (kb < 1000) snprintf(sz, sizeof(sz), "%4luK", (unsigned long)kb);
        else           snprintf(sz, sizeof(sz), "%4luM", (unsigned long)(kb / 1024));
        draw_text(screen_buffer, FB_NAME_X, y, sz, PAL_CART_LIGHT);
    }
    int name_max = (text_right - name_x) / FONT_WIDTH;
    if (name_max < 1) name_max = 1;
    fb_text_trunc(screen_buffer, name_x, y, e->name, text_color, name_max);
}

// Redraw rows + scrollbar thumb. No bulk clear — each row carries its own
// background fill, so HDMI never sees the whole list blanked.
static void fb_draw_list(uint8_t *screen_buffer, int selected, int scroll) {
    bool has_scrollbar = fb_entry_count > FB_VISIBLE_LINES;
    int text_right = has_scrollbar ? FB_SCROLLBAR_X - 4 : SCREEN_WIDTH - 8;

    for (int i = 0; i < FB_VISIBLE_LINES; ++i) {
        int idx = scroll + i;
        const fb_entry_t *e = (idx < fb_entry_count) ? &fb_entries[idx] : NULL;
        bool is_selected = (idx == selected) && e != NULL;
        fb_draw_row(screen_buffer, i, text_right, e, is_selected);
    }

    // Scrollbar thumb only — track stays put (painted by chrome). Clear the
    // old thumb strip with track colour, then draw the new thumb over it.
    if (has_scrollbar) {
        int bar_top = FB_LIST_Y - 1;
        int bar_h   = FB_VISIBLE_LINES * FB_LINE_H;
        int thumb_h = bar_h * FB_VISIBLE_LINES / fb_entry_count;
        if (thumb_h < 10) thumb_h = 10;
        int max_scroll = fb_entry_count - FB_VISIBLE_LINES;
        int thumb_y = bar_top;
        if (max_scroll > 0)
            thumb_y += (bar_h - thumb_h) * scroll / max_scroll;
        fill_rect(screen_buffer, FB_SCROLLBAR_X, bar_top,
                  FB_SCROLLBAR_W, bar_h, PAL_DARK_GRAY);
        fill_rect(screen_buffer, FB_SCROLLBAR_X, thumb_y,
                  FB_SCROLLBAR_W, thumb_h, PAL_CYAN);
    }
}

static void fb_save_path(const char *path) {
    strncpy(fb_persist_path, path, sizeof(fb_persist_path) - 1);
    fb_persist_path[sizeof(fb_persist_path) - 1] = '\0';
    strncpy(g_settings.browser_path, path, sizeof(g_settings.browser_path) - 1);
    g_settings.browser_path[sizeof(g_settings.browser_path) - 1] = '\0';
}

// Returns true if a ROM was selected (out_path holds full path).
static bool file_browser_show(uint8_t *screen_buffer, char *out_path, size_t out_size) {
    setup_ui_palette();

    // Allocate the entry list once, lazily — PSRAM is plentiful here.
    if (fb_entries == NULL) {
        fb_entries = (fb_entry_t *)psram_malloc(FB_MAX_ENTRIES * sizeof(fb_entry_t));
        if (fb_entries == NULL) {
            // Last-resort heap fallback; without this the browser can't run.
            fb_entries = (fb_entry_t *)malloc(FB_MAX_ENTRIES * sizeof(fb_entry_t));
            if (fb_entries == NULL) return false;
        }
    }

    char cur_path[MAX_ROM_PATH];
    bool cold_boot_restore = false;
    bool persist_usable = false;

    // Seed from settings.ini on cold boot.
    if (!fb_persist_valid && g_settings.browser_path[0] != '\0') {
        DIR probe;
        if (f_opendir(&probe, g_settings.browser_path) == FR_OK) {
            f_closedir(&probe);
            strncpy(cur_path, g_settings.browser_path, sizeof(cur_path) - 1);
            cur_path[sizeof(cur_path) - 1] = '\0';
            cold_boot_restore = true;
            persist_usable = true;
        }
    }

    // Warm boot — re-use the in-memory path from the last browser visit.
    if (!persist_usable && fb_persist_valid) {
        DIR probe;
        if (f_opendir(&probe, fb_persist_path) == FR_OK) {
            f_closedir(&probe);
            strncpy(cur_path, fb_persist_path, sizeof(cur_path) - 1);
            cur_path[sizeof(cur_path) - 1] = '\0';
            persist_usable = true;
        } else {
            fb_persist_valid = false;
        }
    }

    if (!persist_usable) {
        strcpy(cur_path, "/genesis");
    }

    // Fallback to root if the chosen start directory doesn't exist.
    {
        DIR probe;
        if (f_opendir(&probe, cur_path) != FR_OK) {
            strcpy(cur_path, "/");
        } else {
            f_closedir(&probe);
        }
    }

    fb_scan_dir(cur_path);

    int selected = 0, scroll = 0;
    if (cold_boot_restore && g_settings.browser_file[0] != '\0') {
        for (int i = 0; i < fb_entry_count; ++i) {
            if (strcmp(fb_entries[i].name, g_settings.browser_file) == 0) {
                selected = i;
                break;
            }
        }
        scroll = selected - FB_VISIBLE_LINES / 2;
        if (scroll < 0) scroll = 0;
        if (fb_entry_count > FB_VISIBLE_LINES && scroll > fb_entry_count - FB_VISIBLE_LINES)
            scroll = fb_entry_count - FB_VISIBLE_LINES;
    } else if (persist_usable) {
        selected = fb_persist_selected;
        scroll = fb_persist_scroll;
        if (selected >= fb_entry_count) selected = fb_entry_count > 0 ? fb_entry_count - 1 : 0;
        if (selected < 0) selected = 0;
        if (fb_entry_count > FB_VISIBLE_LINES && scroll > fb_entry_count - FB_VISIBLE_LINES)
            scroll = fb_entry_count - FB_VISIBLE_LINES;
        if (scroll > selected) scroll = selected;
        if (scroll < 0) scroll = 0;
    }
    fb_persist_valid = true;

    // Allocate the screen-save buffer for the settings menu pop-over.
    if (saved_screen_buffer == NULL) {
        saved_screen_buffer = (uint8_t *)psram_malloc(SCREEN_WIDTH * SCREEN_HEIGHT);
    }

    uint32_t prev_buttons = read_selector_buttons();
    uint32_t hold_counter = 0;

    // Two-layer redraw — path bar / divider / footer / scrollbar track are
    // static chrome, drawn once per directory. Scrolling only repaints the
    // list region, so HDMI DMA never sees a blanked framebuffer.
    bool needs_chrome_redraw = true;
    bool needs_list_redraw   = true;

    while (true) {
        if (needs_chrome_redraw) {
            fb_draw_chrome(screen_buffer, cur_path);
            needs_chrome_redraw = false;
            needs_list_redraw = true;
        }
        if (needs_list_redraw) {
            fb_draw_list(screen_buffer, selected, scroll);
            needs_list_redraw = false;
        }

        selector_tick_16ms();

        uint32_t buttons = read_selector_buttons();
        uint32_t pressed = buttons & ~prev_buttons;

        // Key repeat for navigation after 20 frames held (≈333ms).
        if (buttons != 0 && buttons == prev_buttons) {
            hold_counter++;
            if (hold_counter > 20 && (hold_counter % 3) == 0) {
                pressed |= buttons & (DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT);
            }
        } else {
            hold_counter = 0;
        }
        prev_buttons = buttons;

        // Start+Select → settings menu. Keep this check FIRST so the combo
        // never leaks into "A=open" below.
        if ((buttons & DPAD_SELECT) && (buttons & DPAD_START)) {
            while ((read_selector_buttons() & (DPAD_SELECT | DPAD_START)) ==
                   (DPAD_SELECT | DPAD_START)) selector_tick_16ms();

            if (saved_screen_buffer)
                memcpy(saved_screen_buffer, screen_buffer, SCREEN_WIDTH * SCREEN_HEIGHT);

            settings_result_t r = settings_menu_show_with_restore(screen_buffer, saved_screen_buffer);
            switch (r) {
                case SETTINGS_RESULT_SAVE_RESTART:
                    // Save browser position so we return to it after reboot.
                    fb_save_path(cur_path);
                    fb_persist_selected = selected;
                    fb_persist_scroll = scroll;
                    settings_save();
                    watchdog_reboot(0, 0, 10);
                    while (1) tight_loop_contents();
                    break;
                case SETTINGS_RESULT_RESTART:
                    watchdog_reboot(0, 0, 10);
                    while (1) tight_loop_contents();
                    break;
                case SETTINGS_RESULT_CANCEL:
                default:
                    if (saved_screen_buffer)
                        memcpy(screen_buffer, saved_screen_buffer, SCREEN_WIDTH * SCREEN_HEIGHT);
                    setup_ui_palette();  // settings menu overwrote palette slots
                    prev_buttons = read_selector_buttons();
                    // Full chrome+list repaint — the palette rewrite and the
                    // saved-screen copy happened while HDMI was scanning out.
                    needs_chrome_redraw = true;
                    continue;
            }
        }

        int prev_selected = selected;
        int prev_scroll = scroll;

        // B / ESC → nothing to go back to here; treat as "refresh current dir".
        // (We don't exit the selector — there's no fallback behind it.)
        if (pressed & DPAD_B) {
            fb_scan_dir(cur_path);
            if (selected >= fb_entry_count)
                selected = fb_entry_count > 0 ? fb_entry_count - 1 : 0;
            needs_chrome_redraw = true;
        }

        // Navigation.
        if (pressed & DPAD_UP) {
            if (fb_entry_count > 0) {
                selected = (selected > 0) ? selected - 1 : fb_entry_count - 1;
            }
        }
        if (pressed & DPAD_DOWN) {
            if (fb_entry_count > 0) {
                selected = (selected < fb_entry_count - 1) ? selected + 1 : 0;
            }
        }
        if (pressed & DPAD_LEFT) {
            selected -= FB_VISIBLE_LINES;
            if (selected < 0) selected = 0;
        }
        if (pressed & DPAD_RIGHT) {
            selected += FB_VISIBLE_LINES;
            if (selected >= fb_entry_count) selected = fb_entry_count - 1;
            if (selected < 0) selected = 0;
        }

        // Keep selection visible.
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + FB_VISIBLE_LINES)
            scroll = selected - FB_VISIBLE_LINES + 1;
        if (scroll < 0) scroll = 0;

        if (selected != prev_selected || scroll != prev_scroll)
            needs_list_redraw = true;

        // A / Start → open directory or load ROM.
        if ((pressed & (DPAD_A | DPAD_START)) && fb_entry_count > 0
            && !(buttons & DPAD_SELECT)) {
            fb_entry_t *e = &fb_entries[selected];

            if (e->is_dir) {
                char new_path[MAX_ROM_PATH];
                if (strcmp(e->name, "..") == 0) {
                    strncpy(new_path, cur_path, sizeof(new_path) - 1);
                    new_path[sizeof(new_path) - 1] = '\0';
                    char *last_slash = strrchr(new_path, '/');
                    if (last_slash && last_slash != new_path) *last_slash = '\0';
                    else strcpy(new_path, "/");
                } else {
                    size_t plen = strlen(cur_path);
                    if (plen == 1 && cur_path[0] == '/')
                        snprintf(new_path, sizeof(new_path), "/%s", e->name);
                    else
                        snprintf(new_path, sizeof(new_path), "%s/%s", cur_path, e->name);
                }
                if (strlen(new_path) < sizeof(cur_path)) {
                    strcpy(cur_path, new_path);
                    fb_scan_dir(cur_path);
                    selected = 0;
                    scroll = 0;
                    // New path text → full chrome repaint (path bar + scrollbar track).
                    needs_chrome_redraw = true;
                }
            } else if (is_genesis_file(e->name)) {
                // Build full ROM path and persist browser state.
                if (strlen(cur_path) == 1 && cur_path[0] == '/')
                    snprintf(out_path, out_size, "/%s", e->name);
                else
                    snprintf(out_path, out_size, "%s/%s", cur_path, e->name);

                fb_save_path(cur_path);
                fb_persist_selected = selected;
                fb_persist_scroll = scroll;
                strncpy(g_settings.browser_file, e->name,
                        sizeof(g_settings.browser_file) - 1);
                g_settings.browser_file[sizeof(g_settings.browser_file) - 1] = '\0';
                settings_save();
                wait_buttons_released();
                return true;
            }
        }
    }
}

// ───────────────────────────────────────────────────────────────────────────
// Scan /genesis up front so we can show the no-roms notice when appropriate.
// ───────────────────────────────────────────────────────────────────────────
static rom_scan_result_t scan_genesis_dir(void) {
    DIR dir;
    FRESULT r = f_opendir(&dir, "/genesis");
    if (r != FR_OK) {
        r = f_opendir(&dir, "/GENESIS");
        if (r != FR_OK) return ROM_SCAN_NO_GENESIS_DIR;
    }
    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
        if (fno.fname[0] == '.') continue;
        if (fno.fattrib & AM_DIR) continue;
        if (is_genesis_file(fno.fname)) {
            f_closedir(&dir);
            return ROM_SCAN_OK;
        }
    }
    f_closedir(&dir);
    return ROM_SCAN_NO_ROMS;
}

// ───────────────────────────────────────────────────────────────────────────
// Public entry point
// ───────────────────────────────────────────────────────────────────────────
bool rom_selector_show(char *selected_rom_path, size_t buffer_size, uint8_t *screen_buffer) {
    memset(screen_buffer, PAL_BG_TINT, SCREEN_WIDTH * SCREEN_HEIGHT);

    g_scan_result = scan_genesis_dir();

    // If /genesis is empty or missing, surface the reason and then drop
    // into the file browser (rooted at SD root) so the user can still pick
    // a ROM from anywhere on the card.
    if (g_scan_result != ROM_SCAN_OK) {
        rom_selector_no_roms_notice(screen_buffer, g_scan_result);
    }

    return file_browser_show(screen_buffer, selected_rom_path, buffer_size);
}

// ───────────────────────────────────────────────────────────────────────────
// SD card error screen (blocks forever)
// ───────────────────────────────────────────────────────────────────────────
void rom_selector_show_sd_error(uint8_t *screen_buffer, int error_code) {
    setup_ui_palette();
    fb_fill(screen_buffer, PAL_BG_TINT);
    draw_header(screen_buffer, 0);

    draw_text_center(screen_buffer, 72, "SD CARD ERROR", PAL_RED);

    draw_text_center(screen_buffer, 100, "NO SD CARD DETECTED",   PAL_WHITE);
    draw_text_center(screen_buffer, 114, "OR CARD NOT FORMATTED", PAL_WHITE);

    char code_str[32];
    snprintf(code_str, sizeof(code_str), "ERROR CODE: %d", error_code);
    draw_text_center(screen_buffer, 134, code_str, PAL_GRAY);

    draw_text_center(screen_buffer, 164, "PLEASE INSERT A FAT32",  PAL_YELLOW);
    draw_text_center(screen_buffer, 178, "FORMATTED SD CARD",      PAL_YELLOW);
    draw_text_center(screen_buffer, 192, "AND RESET THE DEVICE",   PAL_YELLOW);

    draw_footer(screen_buffer);

    while (1) tight_loop_contents();
}
