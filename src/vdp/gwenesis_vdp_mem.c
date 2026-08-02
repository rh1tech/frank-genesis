/*
Gwenesis : Genesis & megadrive Emulator.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with
this program. If not, see <http://www.gnu.org/licenses/>.

__author__ = "bzhxx"
__contact__ = "https://github.com/bzhxx"
__license__ = "GPLv3"

*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "m68k.h"
#include "gwenesis_vdp.h"
#include "gwenesis_io.h"
#include "gwenesis_bus.h"
#include "gwenesis_savestate.h"

#include <assert.h>
#include "gwenesis_sn76489.h"
#include "sound_backend.h"
#include "HDMI.h"

#define RGB888(r, g, b) ((r << 16) | (g << 8) | b)

#include "pico/stdlib.h"
#pragma GCC optimize("Ofast")

#define VDP_MEM_DISABLE_LOGGING 1

#if !VDP_MEM_DISABLE_LOGGING
#include <stdarg.h>
void vdpm_log(const char *subs, const char *fmt, ...) {
  extern int frame_counter;
  extern int scan_line;

  va_list va;

  printf("%06d:%03d :[%s] vc:%04x hc:%04x hv:%04x ", frame_counter, scan_line, subs,gwenesis_vdp_vcounter(),gwenesis_vdp_hcounter(),gwenesis_vdp_hvcounter());

  va_start(va, fmt);
  vfprintf(stdout, fmt, va);
  va_end(va);
  printf("\n");
}
#else
#define vdpm_log(...)
#endif

//#define _DMA_TRACE_

/* Setup VDP Memories */

//extern uint8_t emulator_framebuffer[1024*64];
//unsigned char* VRAM = &emulator_framebuffer[0];
unsigned char VRAM[VRAM_MAX_SIZE];
//unsigned char* VRAM = NULL;

unsigned short CRAM[CRAM_MAX_SIZE]; // CRAM - Palettes
unsigned char SAT_CACHE[SAT_CACHE_MAX_SIZE]; // Sprite cache
unsigned char gwenesis_vdp_regs[REG_SIZE]; // Registers
uint32_t vdp_reg_writes_active, vdp_reg_writes_total;
uint32_t vdp_vsram_writes_active, vdp_cram_writes_active, vdp_vram_writes_active;

/* Shape of the mid-frame palette activity: which CRAM entries change,
 * on how many distinct scanlines, and how many changes in one frame.
 * That decides whether the effect can be reproduced by giving the
 * display a handful of palette banks, or whether it needs a genuinely
 * per-line palette. */
uint32_t cram_addr_mask_lo, cram_addr_mask_hi;   /* which of the 64 entries */
uint8_t  cram_line_hist[240];                    /* lines that saw a change */
uint32_t cram_changes_this_frame, cram_changes_max;
/* Distinct scanlines that see a palette change within ONE frame: that is
 * how many palette states the display would have to hold at once. */
int      cram_last_line = -1;
uint32_t cram_splits_this_frame, cram_splits_max;

/* True while the beam is inside the visible field. Anything a game
 * changes here is a raster effect, and the end-of-frame renderer cannot
 * reproduce it: every line is drawn with whatever the value ended up as. */
static inline int vdp_mid_frame(void) {
    extern int scan_line, screen_height;
    return scan_line > 0 && scan_line < screen_height;
}

/* Raster palette splits.
 *
 * A CRAM write that lands while the beam is in the visible field must not
 * change the colours of the lines already drawn above it. Those writes go
 * to a spare display palette entry instead of the base one, and the lines
 * below the split are pointed at a table that maps the affected index to
 * that spare entry. Entries 0-63 are the base palette and 64-127 are the
 * dim copies the CRT effect uses, so the spares live at 128 and up. */
#define PAL_ALT_BASE   128
#define PAL_ALT_SLOTS  112              /* 128..239; 240-243 are HDMI sync */
#define PAL_MAX_STATES 8

bool     vdp_palette_split_enabled = true;
uint32_t vdp_palette_alt_used, vdp_palette_alt_exhausted, vdp_palette_states_max;

static uint8_t pal_lut[PAL_MAX_STATES][256];
static int     pal_state;               /* which table the beam is under */
static int     pal_alt_next;            /* next free spare entry */
static int     pal_split_line;          /* scanline the current state began */

const uint8_t *vdp_palette_current_lut(void) { return pal_lut[pal_state]; }

/* Called once per frame, before any line is drawn. */
void vdp_palette_frame_begin(void) {
    /* Entries that were redirected to a spare last frame still hold the
     * old colour in the base palette; put the current CRAM value back so
     * the top of this frame starts from the truth. Only the handful that
     * were actually redirected need it. */
    for (int i = 0; i < 64; i++) {
        if (pal_lut[pal_state][i] != (uint8_t)i)
            graphics_set_palette((uint8_t)i,
                RGB888(CRAM_R(CRAM[i]), CRAM_G(CRAM[i]), CRAM_B(CRAM[i])));
    }
    /* Identity over the full byte: the top two bits are sprite/priority
     * flags the display must ignore, so every alias of an index maps to
     * the same colour. */
    for (int i = 0; i < 256; i++) pal_lut[0][i] = (uint8_t)(i & 0x3F);
    pal_state      = 0;
    pal_alt_next   = 0;
    pal_split_line = -1;
}

/* A CRAM entry changed while the beam was inside the visible field. */
static void vdp_palette_split(uint8_t addr, uint16_t value)
{
    extern int scan_line;

    /* Several entries can change on one scanline — that is one split. */
    if (scan_line != pal_split_line) {
        if (pal_state + 1 < PAL_MAX_STATES) {
            memcpy(pal_lut[pal_state + 1], pal_lut[pal_state], 256);
            pal_state++;
            if ((uint32_t)pal_state > vdp_palette_states_max)
                vdp_palette_states_max = (uint32_t)pal_state;
        }
        pal_split_line = scan_line;
    }

    if (pal_alt_next >= PAL_ALT_SLOTS) {
        /* Out of spares: fall back to changing the base entry, which is
         * the old behaviour for this one write rather than losing it. */
        vdp_palette_alt_exhausted++;
        graphics_set_palette(addr, RGB888(CRAM_R(value), CRAM_G(value), CRAM_B(value)));
        return;
    }

    uint8_t slot = (uint8_t)(PAL_ALT_BASE + pal_alt_next++);
    if (pal_alt_next > (int)vdp_palette_alt_used) vdp_palette_alt_used = pal_alt_next;
    /* All four aliases of this index (with the sprite/priority bits) map
     * to the spare. */
    pal_lut[pal_state][addr]        = slot;
    pal_lut[pal_state][addr | 0x40] = slot;
    pal_lut[pal_state][addr | 0x80] = slot;
    pal_lut[pal_state][addr | 0xC0] = slot;
    graphics_set_palette(slot, RGB888(CRAM_R(value), CRAM_G(value), CRAM_B(value)));
}

/* Route a CRAM write either to the base palette or to a spare.
 *
 * Not while the CRT effect is on: that reaches the dim copies by ORing
 * 64 into the index at scanout, which lands on top of the spares at 128
 * and up (128|64 = 192). The two cannot share the palette, so the
 * scanline effect gives way to the one the user asked for. */
static inline void vdp_cram_apply(uint8_t addr, uint16_t value)
{
    if (vdp_palette_split_enabled && !graphics_get_crt_enabled() &&
        vdp_mid_frame())
        vdp_palette_split(addr, value);
    else
        graphics_set_palette(addr, RGB888(CRAM_R(value), CRAM_G(value), CRAM_B(value)));
}

#ifdef VDP_RASTER_PROFILE
#define RASTER_NOTE_CRAM(a, newv) do {                                     \
    if (vdp_mid_frame() && CRAM[a] != (newv)) {                            \
        extern int scan_line;                                              \
        vdp_cram_writes_active++;                                          \
        if ((a) < 32) cram_addr_mask_lo |= 1u << (a);                      \
        else if ((a) < 64) cram_addr_mask_hi |= 1u << ((a) - 32);          \
        if (scan_line < 240 && cram_line_hist[scan_line] < 255)            \
            cram_line_hist[scan_line]++;                                   \
        cram_changes_this_frame++;                                         \
        if (scan_line != cram_last_line) {                                 \
            cram_last_line = scan_line;                                    \
            cram_splits_this_frame++;                                      \
        }                                                                  \
    }                                                                      \
} while (0)
#define RASTER_NOTE_VRAM() do { if (vdp_mid_frame()) vdp_vram_writes_active++; } while (0)
#define RASTER_NOTE_VSRAM() do { if (vdp_mid_frame()) vdp_vsram_writes_active++; } while (0)
#else
#define RASTER_NOTE_CRAM(a, newv) ((void)0)
#define RASTER_NOTE_VRAM()        ((void)0)
#define RASTER_NOTE_VSRAM()       ((void)0)
#endif
uint8_t  vdp_reg_active_mask[64];
unsigned short fifo[FIFO_SIZE]; // Fifo
//uint8_t CRAM222[CRAM_MAX_SIZE * 4];    // CRAM - Palettes
unsigned short VSRAM[VSRAM_MAX_SIZE]; // VSRAM - Scrolling

// Define VDP control code and set initial code
static unsigned char code_reg = 0;
// Define VDP control address and set initial address
static unsigned short address_reg = 0;
// Define VDP control pending and set initial state
int command_word_pending = 0;
// Define VDP status and set initial status value
unsigned short gwenesis_vdp_status = 0x3C00;

extern int scan_line;
extern bool sn76489_enabled;
extern bool audio_enabled;
// Define DMA
//static unsigned int dma_length;
//static unsigned int dma_source;
// Define and set DMA FILL pending as initial state
int dma_fill_pending = 0;

// Define HVCounter latch and set initial state
static int hvcounter_latch = 0;
static int hvcounter_latched = 0;

int hint_pending;


// Define VIDEO MODE
extern int mode_pal;

extern int sprite_overflow;
extern bool sprite_collision;

// Store last address r/w
//static unsigned int gwenesis_vdp_laddress_r=0;
//unsigned int gwenesis_vdp_laddress_w=0;

//static int DMA_RUN=0;

// 16 bits access to VRAM
#define FETCH16(A) ( ( (*(unsigned short *)&VRAM[(A)]) >> 8 ) | ( (*(unsigned short *)&VRAM[(A)]) << 8 ) )


/******************************************************************************
 *
 *  SEGA 315-5313 Reset
 *  Clear all volatile memory
 *
 ******************************************************************************/
int m68k_irq_acked(int irq) {
    /* VINT has higher priority (Fatal Rewind) */
    if (REG1_VBLANK_INTERRUPT && (gwenesis_vdp_status & STATUS_VIRQPENDING)) {
        /* Clear VINT pending flag */
        gwenesis_vdp_status &= ~STATUS_VIRQPENDING;

        if (hint_pending && REG0_LINE_INTERRUPT)
            m68k_set_irq(4);
        else
            m68k_set_irq(0);
    }
    else {
        /* Clear HINT pending flag */
        hint_pending = 0;

        /* Update IRQ status */
        m68k_set_irq(0);
    }

    return M68K_INT_ACK_AUTOVECTOR;
}


void gwenesis_vdp_reset() {
    memset(VRAM, 0, VRAM_MAX_SIZE);
    memset(SAT_CACHE, 0, sizeof(SAT_CACHE));
    memset(CRAM, 0, sizeof(CRAM));
    //    memset(CRAM222, 0, sizeof(CRAM222));
    memset(VSRAM, 0, sizeof(VSRAM));
    memset(gwenesis_vdp_regs, 0, sizeof(gwenesis_vdp_regs));
    command_word_pending = 0;
    address_reg = 0;
    code_reg = 0;
    hint_pending = 0;
    // _vcounter = 0;
    gwenesis_vdp_status = 0x3C00;
    // //line_counter_interrupt = 0;
    hvcounter_latched = 0;

    // register the M68K interrupt
    m68k_set_int_ack_callback(m68k_irq_acked);
}


/******************************************************************************
 *
 *  SEGA 315-5313 HCOUNTER
 *  Process SEGA 315-5313 HCOUNTER based on M68K Cycles
 *
 ******************************************************************************/
//static inline __attribute__((always_inline))
int gwenesis_vdp_hcounter() {
    int mclk = m68k_cycles_run();
    int pixclk;

    // Accurate 9-bit hcounter emulation, from timing posted here:
    // http://gendev.spritesmind.net/forum/viewtopic.php?p=17683#17683
    if (REG12_MODE_H40) {
        pixclk = mclk * 420 / VDP_CYCLES_PER_LINE;
        pixclk += 0xD;
        if (pixclk >= 0x16D)
            pixclk += 0x1C9 - 0x16D;
    }
    else {
        pixclk = mclk * 342 / VDP_CYCLES_PER_LINE;
        pixclk += 0xB;
        if (pixclk >= 0x128)
            pixclk += 0x1D2 - 0x128;
    }

    return pixclk & 0x1FF;
}

/******************************************************************************
 *
 *  SEGA 315-5313 VCOUNTER
 *  Process SEGA 315-5313 VCOUNTER based on M68K Cycles
 *
 ******************************************************************************/
//static inline __attribute__((always_inline))
int gwenesis_vdp_vcounter() {
    int vc = scan_line;
    int VERSION_PAL = gwenesis_vdp_status & 1;

    /*
    if (VERSION_PAL && mode_pal && (vc >= 0x10B))
        vc += 0x1D2 - 0x10B;
    else if (VERSION_PAL && (mode_pal==0) && (vc >= 0x103))
        vc += 0x1CA - 0x103;
    else if ((VERSION_PAL ==0 ) && (vc >= 0xEB))
        vc += 0x1E5 - 0xEB;
    assert(vc < 0x200);
    */
    if (VERSION_PAL && mode_pal && (vc >= 267))
        vc = scan_line - 58;
    else if (VERSION_PAL && (mode_pal == 0) && (vc >= 259))
        vc = scan_line - 42;
    else if ((VERSION_PAL == 0) && (vc >= 235))
        vc = scan_line - 6;
    assert(vc < 0x200);

    // printf("VERSION_PAL:%d , mode_pal:%d,line:%d,vc:%d\n",VERSION_PAL,mode_pal,scan_line,vc);
    return vc;
}

/******************************************************************************
 *
 *  SEGA 315-5313 HVCOUNTER
 *  Process SEGA 315-5313 HVCOUNTER based on HCOUNTER and VCOUNTER
 *
 ******************************************************************************/
//static inline __attribute__((always_inline))
unsigned short gwenesis_vdp_hvcounter() {
    /* H/V Counter */
    if (hvcounter_latched == 1)
        return hvcounter_latch;

    int hc = gwenesis_vdp_hcounter();
    int vc = gwenesis_vdp_vcounter();
    assert(vc < 512);
    assert(hc < 512);

    return ((vc & 0xFF) << 8) | (hc >> 1);
}

//static inline __attribute__((always_inline))
bool vblank(void) {
    int vc = gwenesis_vdp_vcounter();
    //  printf("vc=%d,REG1_DISP_ENABLED=%d,VBLAN?%d\n",vc,REG1_DISP_ENABLED,
    // mode_pal?((vc >= 0xF0) && (vc < 0x1FF)):((vc >= 0xE0) && (vc < 0x1FF)));

    if (REG1_DISP_ENABLED == 0)
        return true;

    if (mode_pal)
        return ((vc >= 0xF0) && (vc < 0x1FF));
    else
        return ((vc >= 0xE0) && (vc < 0x1FF));
}

/******************************************************************************
 *
 *   SEGA 315-5313 Set Register
 *   Write an value to specified register
 *
 ******************************************************************************/
static inline __attribute__((always_inline)) void gwenesis_vdp_register_w(int reg, unsigned char value) {
    // Mode4 is not emulated yet. Anyway, access to registers > 0xA is blocked.
    if ((BIT(gwenesis_vdp_regs[0x1], 2) == 0) && reg > 0xA)
        return;

    /* Raster-effect accounting. The frame is drawn only after all of it
     * has been emulated, so any VDP state a game changes part-way down
     * the screen is lost — every line is drawn with the end-of-frame
     * value. Counting the writes that land during active display says
     * whether a game relies on that. */
#ifdef VDP_RASTER_PROFILE
    {
        extern int scan_line, screen_height;
        vdp_reg_writes_total++;
        if (scan_line > 0 && scan_line < screen_height &&
            gwenesis_vdp_regs[reg] != value) {
            vdp_reg_writes_active++;
            if (reg < 64) vdp_reg_active_mask[reg]++;
        }
    }
#endif
    gwenesis_vdp_regs[reg] = value;
    vdpm_log(__FUNCTION__, "reg:%02d <- %02x", reg, value);


    // Writing a register clear the first command word
    // (see sonic3d intro wrong colors, and vdpfifotesting)
    code_reg &= ~0x3;
    address_reg &= ~0x3FFF;

    switch (reg) {
        case 0:

            if (REG0_HVLATCH && (hvcounter_latched == 0)) {
                hvcounter_latch = gwenesis_vdp_hvcounter();
                hvcounter_latched = 1;
                //printf("HVcounter latched:%x\n",hvcounter_latch);
            }
            else if ((REG0_HVLATCH == 0) && (hvcounter_latched == 1)) {
                //printf("HVcounter released\n");
                hvcounter_latched = 0;
            }

            break;
    }
}

/******************************************************************************
 *
 *  Simulate FIFO
 *
 ******************************************************************************/
static inline __attribute__((always_inline))
void push_fifo(unsigned int value) {
    fifo[3] = fifo[2];
    fifo[2] = fifo[1];
    fifo[1] = fifo[0];
    fifo[0] = value;
}

/******************************************************************************
 *
 *   SEGA 315-5313 VRAM Write
 *   Write an value to VRAM on specified address
 *
 ******************************************************************************/

//static inline __attribute__((always_inline))
void __not_in_flash_func(gwenesis_vdp_vram_write)(unsigned int address, unsigned int value) {
    RASTER_NOTE_VRAM();
    VRAM[address] = value;

    // Update internal SAT Cache
    // used in Castlevania Bloodlines
    if (address >= REG5_SAT_ADDRESS && address < REG5_SAT_ADDRESS + REG5_SAT_SIZE)
        SAT_CACHE[address - REG5_SAT_ADDRESS] = value;
}

static inline __attribute__((always_inline))
unsigned short status_register_r(void) {
    unsigned short status = gwenesis_vdp_status; // & 0xF800;
    // unsigned short status = gwenesis_vdp_status;// & 0xFC00;

    int hc = gwenesis_vdp_hcounter();
    // int vc = gwenesis_vdp_vcounter();

    // TODO: FIFO not emulated
    status |= STATUS_FIFO_EMPTY;

    // VBLANK bit
    if (vblank())
        status |= STATUS_VBLANK;

    // HBLANK bit (see Nemesis doc, as linked in hcounter())
    if (REG12_MODE_H40) {
        if (hc < 0xA || hc >= 0x166)
            status |= STATUS_HBLANK;
    }
    else {
        if (hc < 0x9 || hc >= 0x126)
            status |= STATUS_HBLANK;
    }

    if (sprite_overflow)
        status |= STATUS_SPRITEOVERFLOW;
    if (sprite_collision)
        status |= STATUS_SPRITECOLLISION;

    if (mode_pal)
        status |= STATUS_PAL;

    // reading the status clears the pending flag for command words
    command_word_pending = 0;

    //gwenesis_vdp_status = status;

    // printf("VDP status read:%04X H?%d V?%d line=%d\n",status, status & STATUS_HBLANK ,status & STATUS_VBLANK,scan_line);
    return status;
}

/******************************************************************************
 *
 *   SEGA 315-5313 Get Register
 *   Read an value from specified register
 *
 ******************************************************************************/
unsigned int gwenesis_vdp_get_reg(int reg) {
    return gwenesis_vdp_regs[reg];
}

/******************************************************************************
 *
 *   SEGA 315-5313 DMA Fill
 *   DMA process to fill memory
 *
 ******************************************************************************/
static inline __attribute__((always_inline))
void gwenesis_vdp_dma_fill(unsigned short value) {
    //vdpm_log(__FUNCTION__,"@%x len:%x val:%x",REG21_DMA_SRCADDR_LOW,REG19_DMA_LENGTH,value);
    int dma_length = REG19_DMA_LENGTH;

    // This address is not required for fills,
    // but it's still updated by the DMA engine.
    unsigned short src_addr_low = REG21_DMA_SRCADDR_LOW;

    if (dma_length == 0)
        dma_length = 0xFFFF;

    /*
    vdpm_log(__FUNCTION__, "DMA %s fill: dst:%04x, length:%d, increment:%d, value=%02x",
        (code_reg&0xF)==1 ? "VRAM" : ( (code_reg&0xF)==3 ? "CRAM" : "VSRAM"),
        address_reg, dma_length, REG15_DMA_INCREMENT, value>>8);
        */

    switch (code_reg & 0xF) {
        case 0x1:
            do {
                gwenesis_vdp_vram_write((address_reg ^ 1) & 0xFFFF, value >> 8);
                address_reg += REG15_DMA_INCREMENT;
                src_addr_low++;
            }
            while (--dma_length);
            break;
        case 0x3: // undocumented and buggy, see vdpfifotesting
            do {
                uint8_t addr = (address_reg & 0x7f) >> 1;
                RASTER_NOTE_CRAM(addr, fifo[3]);
                CRAM[addr] = fifo[3];

                vdp_cram_apply(addr, CRAM[addr]);

                address_reg += REG15_DMA_INCREMENT;
                src_addr_low++;
            }
            while (--dma_length);
            break;
        case 0x5: // undocumented and buggy, see vdpfifotesting:
            do {
                RASTER_NOTE_VSRAM();
                VSRAM[(address_reg & 0x7f) >> 1] = fifo[3] & 0x03FF;
                address_reg += REG15_DMA_INCREMENT;
                src_addr_low++;
            }
            while (--dma_length);
            break;
        default:
            printf("Invalid code during DMA fill\n");
    }


    // Clear DMA length at the end of transfer
    gwenesis_vdp_regs[19] = gwenesis_vdp_regs[20] = 0;

    // Update DMA source address after end of transfer
    gwenesis_vdp_regs[21] = src_addr_low & 0xFF;
    gwenesis_vdp_regs[22] = src_addr_low >> 8;

    // gwenesis_vdp_regs[21] = src_addr_low >> 1 & 0xFF;
    // gwenesis_vdp_regs[22] = src_addr_low >> 9 & 0xFF;
    // gwenesis_vdp_regs[23] = src_addr_low >> 17 & 0xFF;
}

/******************************************************************************
 *
 *   SEGA 315-5313 DMA M68K
 *   DMA process to copy from m68k to memory
 *
 ******************************************************************************/
static inline __attribute__((always_inline))
void gwenesis_vdp_dma_m68k() {
    int dma_length = REG19_DMA_LENGTH;

    // This address is not required for fills,
    // but it's still updated by the DMA engine.
    unsigned short src_addr_low = REG21_DMA_SRCADDR_LOW;
    unsigned int src_addr_high = REG23_DMA_SRCADDR_HIGH;
    unsigned int src_addr = (src_addr_high | src_addr_low) << 1;
    unsigned int value;

    if (dma_length == 0)
        dma_length = 0xFFFF;

    /*
    vdpm_log(__FUNCTION__,"DMA M68k->%s copy: src:%04x, dst:%04x, length:%d, increment:%d",
        (code_reg&0xF)==1 ? "VRAM" : ( (code_reg&0xF)==3 ? "CRAM" : "VSRAM"),
        (src_addr_high | src_addr_low) << 1, address_reg, dma_length, REG15_DMA_INCREMENT);
    */

    /* Source is :
        68K_RAM if dma_source_high == 0x00FF : FETCH16RAM(dma_source_low << 1)
        68K_ROM otherwise                    : FETCH16ROM((dma_source_high | dma_source_low) << 1))
    */

    /* Source is 68K RAM */
    if (src_addr & 0x800000) {
        switch (code_reg & 0xF) {
            case 0x1: // dest is VRAM
                do {
                    value = FETCH16RAM(src_addr);
                    push_fifo(value);
                    gwenesis_vdp_vram_write((address_reg) & 0xFFFF, value >> 8);
                    gwenesis_vdp_vram_write((address_reg ^ 1) & 0xFFFF, value & 0xFF);
                    address_reg += REG15_DMA_INCREMENT;
                    src_addr += 2;
                }
                while (--dma_length);
                break;

            case 0x3: // dest is CRAM
                do {
                    value = FETCH16RAM(src_addr);
                    push_fifo(value);
                    uint8_t addr = (address_reg & 0x7f) >> 1;
                RASTER_NOTE_CRAM(addr, value);
                    CRAM[addr] = value;

                    vdp_cram_apply(addr, value);

                    address_reg += REG15_DMA_INCREMENT;
                    src_addr += 2;
                }
                while (--dma_length);
                break;

            case 0x5: // dest is VSRAM

                do {
                    value = FETCH16RAM(src_addr);
                    push_fifo(value);
                    RASTER_NOTE_VSRAM();
                    VSRAM[(address_reg & 0x7f) >> 1] = value & 0x03FF;
                    address_reg += REG15_DMA_INCREMENT;
                    src_addr += 2;
                }
                while (--dma_length);
                break;
            default: // dest in unknown
                break;
        }

        /* source is 68K ROM */
    }
    else {
        // unsigned int dma_source_address = (dma_source_high | dma_source_low) << 1;

        switch (code_reg & 0xF) {
            case 0x1: // dest is VRAM

                do {
                    value = FETCH16ROM(src_addr);
                    push_fifo(value);
                    gwenesis_vdp_vram_write((address_reg) & 0xFFFF, value >> 8);
                    gwenesis_vdp_vram_write((address_reg ^ 1) & 0xFFFF, value & 0xFF);
                    address_reg += REG15_DMA_INCREMENT;
                    src_addr += 2;
                }
                while (--dma_length);
                break;

            case 0x3: // dest is CRAM

                do {
                    value = FETCH16ROM(src_addr);
                    push_fifo(value);
                    uint8_t addr = (address_reg & 0x7f) >> 1;
                RASTER_NOTE_CRAM(addr, value);
                    CRAM[addr] = value;

                    vdp_cram_apply(addr, value);

                    address_reg += REG15_DMA_INCREMENT;
                    src_addr += 2;
                }
                while (--dma_length);
                break;

            case 0x5: // dest is VSRAM

                do {
                    value = FETCH16ROM(src_addr);
                    push_fifo(value);
                    RASTER_NOTE_VSRAM();
                    VSRAM[(address_reg & 0x7f) >> 1] = value & 0x03FF;
                    address_reg += REG15_DMA_INCREMENT;
                    src_addr += 2;
                }
                while (--dma_length);
                break;
            default: // dest in unknown
                break;
        }
    }

    // Update DMA source address after end of transfer
    gwenesis_vdp_regs[21] = src_addr & 0xFF; //src_addr_low & 0xFF;
    gwenesis_vdp_regs[22] = (src_addr >> 8) & 0xFF; //src_addr_low >> 8;

    // Clear DMA length at the end of transfer
    gwenesis_vdp_regs[19] = gwenesis_vdp_regs[20] = 0;
}

/******************************************************************************
 *
 *   SEGA 315-5313 DMA Copy
 *   DMA process to copy from memory to memory
 *
 ******************************************************************************/
static inline __attribute__((always_inline))
void gwenesis_vdp_dma_copy() {
    // DMA_RUN=1;

    int dma_length = REG19_DMA_LENGTH;
    unsigned short src_addr_low = REG21_DMA_SRCADDR_LOW;
    //vdpm_log(__FUNCTION__,"length:%x src:%x",dma_length,src_addr_low);

    do {
        unsigned short value = VRAM[src_addr_low ^ 1];
        gwenesis_vdp_vram_write((address_reg ^ 1) & 0xFFFF, value);

        address_reg += REG15_DMA_INCREMENT;
        src_addr_low++;
    }
    while (--dma_length);

    // Update DMA source address after end of transfer
    gwenesis_vdp_regs[21] = src_addr_low & 0xFF;
    gwenesis_vdp_regs[22] = src_addr_low >> 8;

    // Clear DMA length at the end of transfer
    gwenesis_vdp_regs[19] = gwenesis_vdp_regs[20] = 0;
}

/******************************************************************************
 *
 *   SEGA 315-5313 read data R16
 *   Read an data value from mapped memory on specified address
 *   and return as word
 *
 ******************************************************************************/
static inline __attribute__((always_inline))
unsigned int gwenesis_vdp_read_data_port_16() {
    enum {
        CRAM_BITMASK = 0x0EEE,
        VSRAM_BITMASK = 0x07FF,
        VRAM8_BITMASK = 0x00FF
    };
    unsigned int value;
    command_word_pending = 0;

    //if (code_reg & 1) /* check if write is set */
    // {
    switch (code_reg & 0xF) {
        case 0x0:
            // No byteswapping here
            value = VRAM[(address_reg) & 0xFFFE] << 8;
            value |= VRAM[(address_reg | 1) & 0xFFFF];
            address_reg += REG15_DMA_INCREMENT;
            address_reg &= 0xFFFF;
        //vdpm_log(__FUNCTION__,"%04x",value);

            return value;
        case 0x4:
            if (((address_reg & 0x7f) >> 1) >= 0x28)
                value = VSRAM[0];
            else
                value = VSRAM[(address_reg & 0x7f) >> 1];
            value = (value & VSRAM_BITMASK) | (fifo[3] & ~VSRAM_BITMASK);
            address_reg += REG15_DMA_INCREMENT;
            address_reg &= 0x7F;
        // vdpm_log(__FUNCTION__,"%04x",value);

            return value;
        case 0x8:
            value = CRAM[(address_reg & 0x7f) >> 1];
            value = (value & CRAM_BITMASK) | (fifo[3] & ~CRAM_BITMASK);
            address_reg += REG15_DMA_INCREMENT;
            address_reg &= 0x7F;
        // vdpm_log(__FUNCTION__,"%04x",value);

            return value;
        case 0xC: /* 8-Bit memory access */
            value = VRAM[(address_reg ^ 1) & 0xFFFF];
            value = (value & VRAM8_BITMASK) | (fifo[3] & ~VRAM8_BITMASK);
            address_reg += REG15_DMA_INCREMENT;
            address_reg &= 0xFFFF;
        // vdpm_log(__FUNCTION__,"%04x",value);

            return value;
        default:
            printf("unhandled gwenesis_vdp_read_data_port_16(%x)\n", address_reg);
            return 0xFF;
    }
    // }
    //  return 0x00;
}


/******************************************************************************
 *
 *   SEGA 315-5313 write to control port
 *   Write an control value to SEGA 315-5313 control port
 *
 ******************************************************************************/
static inline __attribute__((always_inline))
void gwenesis_vdp_control_port_write(unsigned int value) {
    //vdpm_log(__FUNCTION__,"%04x",value);

    if (command_word_pending == 1) {
        // second half of the command word
        code_reg &= ~0x3C;
        code_reg |= (value >> 2) & 0x3C;
        address_reg &= 0x3FFF;
        address_reg |= value << 14;
        command_word_pending = 0;
        //vdpm_log(__FUNCTION__,"command word 2nd code:%x address:%x", code_reg, address_reg);


        // DMA trigger
        if (code_reg & (1 << 5)) {
            // Check master DMA enable, otherwise skip
            if (REG1_DMA_ENABLED == 0)
                return;

            // gwenesis_vdp_status |= 0x2;
            switch (REG23_DMA_TYPE) {
                case 0:
                case 1:

                    gwenesis_vdp_dma_m68k();
                    break;

                case 2:

                    // VRAM fill will trigger on next data port write
                    dma_fill_pending = 1;
                    break;

                case 3:

                    gwenesis_vdp_dma_copy();
                    break;
            }
        }
        return;
    }
    if ((value >> 14) == 2) {
        gwenesis_vdp_register_w((value >> 8) & 0x1F, value & 0xFF);
        return;
    }

    // Anything else is treated as first half of the command word
    // We directly update the code reg and address reg
    code_reg &= ~0x3;
    code_reg |= value >> 14;
    address_reg &= ~0x3FFF;
    address_reg |= value & 0x3FFF;
    command_word_pending = 1;
    // vdpm_log(__FUNCTION__,"command word 1st code:%x address:%x", code_reg, address_reg);
}

/******************************************************************************
 *
 *   SEGA 315-5313 write data W16
 *   Write an data value to mapped memory on specified address
 *
 ******************************************************************************/
static inline __attribute__((always_inline))
void gwenesis_vdp_write_data_port_16(unsigned int value) {
    vdpm_log(__FUNCTION__, "%04x", value);

    command_word_pending = 0;

    push_fifo(value);

    switch (code_reg & 0xF) {
        case 0x1: /* VRAM write */
            //vdpm_log(__FUNCTION__,"VRAM write : addr:%x increment:%d value:%04x",
            // address_reg, REG15_DMA_INCREMENT, value);
            gwenesis_vdp_vram_write(address_reg & 0xFFFF, (value >> 8) & 0xFF);
            gwenesis_vdp_vram_write((address_reg ^ 1) & 0xFFFF, (value) & 0xFF);
            address_reg += REG15_DMA_INCREMENT;
            address_reg &= 0xFFFF;

            break;
        case 0x3: /* CRAM write */
            //vdpm_log(__FUNCTION__,"CRAM write : addr:%x increment:%d value:%04x",
            // address_reg, REG15_DMA_INCREMENT, value);
        {
            uint8_t addr = (address_reg & 0x7f) >> 1;
                RASTER_NOTE_CRAM(addr, value);
            CRAM[addr] = value;

            vdp_cram_apply(addr, value);

            address_reg += REG15_DMA_INCREMENT;
            address_reg &= 0xFFFF;

            break;
        }
        case 0x5: /* VSRAM write */
            //vdpm_log(__FUNCTION__,"VSRAM write : addr:%x increment:%d value:%04x",
            //  address_reg, REG15_DMA_INCREMENT, value);
            // printf("write dataport 16: VSRAM@%04x:%04x\n",address_reg,value);
            VSRAM[(address_reg & 0x7f) >> 1] = value & 0X03FF;
            address_reg += REG15_DMA_INCREMENT;
            address_reg &= 0xFFFF;
            break;
        case 0x0:
        case 0x4:
        case 0x8: // Write operation after setting up
            // Makes Compatible with Alladin and Ecco 2
            break;
        case 0x9: // VDP FIFO TEST
            break;
        default:
            printf("VDP Data Port invalid");
    }

    /* if a DMA is scheduled, do it */
    if (dma_fill_pending) {
        dma_fill_pending = 0;
        gwenesis_vdp_dma_fill(value);
        return;
    }
}

/******************************************************************************
 *
 *   SEGA 315-5313 Get Status
 *   Return current VDP Status
 *
 ******************************************************************************/
unsigned int gwenesis_vdp_get_status() {
    return gwenesis_vdp_status;
}


/******************************************************************************
 *
 *   SEGA 315-5313 read from memory R8
 *   Read an value from mapped memory on specified address
 *   and return as byte
 *
 ******************************************************************************/
//static inline
unsigned int gwenesis_vdp_read_memory_8(unsigned int address) {
    unsigned int ret = gwenesis_vdp_read_memory_16(address & ~1);
    if (address & 1)
        return ret & 0xFF;

    // vdpm_log(__FUNCTION__,"%04x : %04x",address,ret);

    return ret >> 8;
}

/******************************************************************************
 *
 *   SEGA 315-5313 read from memory R16
 *   Read an value from mapped memory on specified address
 *   and return as word
 *
 ******************************************************************************/
//static inline
unsigned int gwenesis_vdp_read_memory_16(unsigned int address) {
    address &= 0x1F;

    if (address < 0X4)
        return gwenesis_vdp_read_data_port_16();
    else if (address < 0x8)
        return status_register_r();
    else if (address < 0xf)
        return gwenesis_vdp_hvcounter();
    else
        return 0xff;
}

/******************************************************************************
 *
 *   SEGA 315-5313 write to memory W8
 *   Write an byte value to mapped memory on specified address
 *
 ******************************************************************************/
//static inline
void gwenesis_vdp_write_memory_8(unsigned int address, unsigned int value) {
    gwenesis_vdp_write_memory_16(address & ~1, (value << 8) | value);
}

/******************************************************************************
 *
 *   SEGA 315-5313 write to memory W16
 *   Write an word value to mapped memory on specified address
 *
 ******************************************************************************/
//static inline
extern int system_clock;

void gwenesis_vdp_write_memory_16(unsigned int address, unsigned int value) {
    address = address & 0x1F;

    if (address < 0x4) {
        gwenesis_vdp_write_data_port_16(value);
        return;
    }
    if (address < 0x8) {
        gwenesis_vdp_control_port_write(value);
        return;
    }
    if (address < 0x18) {
        // PSG 8 bits write
        vdpm_log(__FUNCTION__, "PSG sclk=%d,mclk=%d", system_clock, m68k_cycles_master());
        if(audio_enabled && sn76489_enabled)
            sound_psg_write(value, m68k_cycles_master());
        return;
    }
    // UNHANDLED - disabled spam
    // printf("unhandled gwenesis_vdp_write(%x, %x)\n", address, value);
}

void gwenesis_vdp_mem_save_state() {
}

void gwenesis_vdp_mem_load_state() {

}
