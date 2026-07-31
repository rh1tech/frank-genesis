#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"

/*
 * Board Configuration Variants:
 * 
 * BOARD_M1 - M1 GPIO layout
 * BOARD_M2 - M2 GPIO layout
 * BOARD_C2 - FRANK Core 2 master (RP2350B, U3)
 *
 * PSRAM pin is auto-detected based on chip package:
 *   RP2350B: GPIO47 (for M1, M2 and C2)
 *   RP2350A: GPIO19 (M1) or GPIO8 (M2)
 *
 * M1 GPIO Layout:
 *   HDMI: CLKN=6, CLKP=7, D0N=8, D0P=9, D1N=10, D1P=11, D2N=12, D2P=13
 *   SD:   CLK=2, CMD=3, DAT0=4, DAT3=5
 *   PS/2: CLK=0, DATA=1
 *   I2S:  DATA=26, CLK=27, LRCK=28
 * 
 * M2 GPIO Layout:
 *   HDMI: CLKN=12, CLKP=13, D0N=14, D0P=15, D1N=16, D1P=17, D2N=18, D2P=19
 *   SD:   CLK=6, CMD=7, DAT0=4, DAT3=5
 *   PS/2: CLK=2, DATA=3
 *   I2S:  DATA=9, CLK=10, LRCK=11
 *
 * C2 GPIO Layout (FRANK Core 2 master, RP2350B / U3):
 *   HDMI: CLKN=12, CLKP=13, D0N=14, D0P=15, D1N=16, D1P=17, D2N=18, D2P=19
 *   SD:   CLK=6, CMD=7, DAT0=4, DAT3=5
 *   I2S:  DATA=9, CLK=10, LRCK=11
 *   PSRAM CS=47, WS2812 status LED=46, UART0 TX=0/RX=1
 *   PS/2: CLK=2, DATA=3 (spare pads, no connector fitted)
 *   GPIO20..43 are reserved for the inter-processor link to the slave,
 *   so C2 has no NES/SNES pad pins — gamepads arrive over USB HID.
 *   The HDMI/SD/I2S/PSRAM pins are identical to M2 (Murmulator 2.0),
 *   which is why the drivers drop in unmodified.
 *
 * CPU/PSRAM Speed (set via CMake -DCPU_SPEED=xxx -DPSRAM_SPEED=xxx):
 *   252 MHz - no overclock (default for stable operation)
 *   378 MHz - medium overclock
 *   504 MHz - high overclock
 */

// Default to M1 if no config specified
#if !defined(BOARD_M1) && !defined(BOARD_M2) && !defined(BOARD_C2)
#define BOARD_M1
#endif

//=============================================================================
// CPU/PSRAM Speed Defaults (can be overridden via CMake)
//=============================================================================
#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 252
#endif

#ifndef CPU_VOLTAGE
#define CPU_VOLTAGE VREG_VOLTAGE_1_50
#endif

#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 133
#endif

//=============================================================================
// PSRAM Pin Auto-Detection
//=============================================================================

// PSRAM pin for RP2350A variants
#if defined(BOARD_M1)
#define PSRAM_PIN_RP2350A 19
#elif defined(BOARD_C2)
// C2's master is always the B package; the A-package CS below is the
// slave's (U5), reached only if this image is ever run on the slave.
#define PSRAM_PIN_RP2350A 0
#else
#define PSRAM_PIN_RP2350A 8
#endif

// PSRAM pin for RP2350B (always GPIO47)
#define PSRAM_PIN_RP2350B 47

// Runtime function to get PSRAM pin based on chip package
static inline uint get_psram_pin(void) {
    // Check if RP2350A (bit 0 set) or RP2350B (bit 0 clear)
    uint32_t package_sel = *((io_ro_32*)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    if (package_sel & 1) {
        // RP2350A - use board-specific pin
        return PSRAM_PIN_RP2350A;
    } else {
        // RP2350B - always GPIO47
        return PSRAM_PIN_RP2350B;
    }
}

//=============================================================================
// M1 Layout Configuration
//=============================================================================
#ifdef BOARD_M1

// HDMI Pins
#define HDMI_PIN_CLKN 6
#define HDMI_PIN_CLKP 7
#define HDMI_PIN_D0N  8
#define HDMI_PIN_D0P  9
#define HDMI_PIN_D1N  10
#define HDMI_PIN_D1P  11
#define HDMI_PIN_D2N  12
#define HDMI_PIN_D2P  13

#define HDMI_BASE_PIN HDMI_PIN_CLKN

// SD Card Pins
#define SDCARD_PIN_CLK    2
#define SDCARD_PIN_CMD    3
#define SDCARD_PIN_D0     4
#define SDCARD_PIN_D3     5

// PS/2 Keyboard Pins
#define PS2_PIN_CLK  0
#define PS2_PIN_DATA 1

// PS/2 Mouse Pins
#define PS2_MOUSE_CLK  14
#define PS2_MOUSE_DATA 15

// I2S Audio Pins
#define I2S_DATA_PIN       26
#define I2S_CLOCK_PIN_BASE 27

#endif // BOARD_M1

//=============================================================================
// M2 Layout Configuration
//=============================================================================
#ifdef BOARD_M2

// HDMI Pins
#define HDMI_PIN_CLKN 12
#define HDMI_PIN_CLKP 13
#define HDMI_PIN_D0N  14
#define HDMI_PIN_D0P  15
#define HDMI_PIN_D1N  16
#define HDMI_PIN_D1P  17
#define HDMI_PIN_D2N  18
#define HDMI_PIN_D2P  19

#define HDMI_BASE_PIN HDMI_PIN_CLKN

// SD Card Pins
#define SDCARD_PIN_CLK    6
#define SDCARD_PIN_CMD    7
#define SDCARD_PIN_D0     4
#define SDCARD_PIN_D3     5

// PS/2 Keyboard Pins
#define PS2_PIN_CLK  2
#define PS2_PIN_DATA 3

// PS/2 Mouse Pins
#define PS2_MOUSE_CLK  0
#define PS2_MOUSE_DATA 1

// I2S Audio Pins
#define I2S_DATA_PIN       9
#define I2S_CLOCK_PIN_BASE 10

#endif // BOARD_M2

//=============================================================================
// C2 Layout Configuration (FRANK Core 2 master, RP2350B / U3)
//
// Pin numbers come from the KiCad netlist via
// frank_core2/firmware/common/frank_core2_board.h.
//=============================================================================
#ifdef BOARD_C2

// HDMI Pins (J5, same relative order as M2)
#define HDMI_PIN_CLKN 12
#define HDMI_PIN_CLKP 13
#define HDMI_PIN_D0N  14
#define HDMI_PIN_D0P  15
#define HDMI_PIN_D1N  16
#define HDMI_PIN_D1P  17
#define HDMI_PIN_D2N  18
#define HDMI_PIN_D2P  19

#define HDMI_BASE_PIN HDMI_PIN_CLKN

// SD Card Pins (J7, SPI0)
#define SDCARD_PIN_CLK    6
#define SDCARD_PIN_CMD    7
#define SDCARD_PIN_D0     4
#define SDCARD_PIN_D3     5

// PS/2 Keyboard Pins — spare pads, no connector on this board. The
// driver pulls both lines up, so init on unrouted pins is inert.
#define PS2_PIN_CLK  2
#define PS2_PIN_DATA 3

// PS/2 Mouse Pins — spare pads, no connector.
#define PS2_MOUSE_CLK  44
#define PS2_MOUSE_DATA 45

// I2S Audio Pins (U8, TDA1387T)
#define I2S_DATA_PIN       9
#define I2S_CLOCK_PIN_BASE 10

// WS2812B status LED (LD1) via 330R
#define LED_WS2812_PIN 46

// Inter-processor link to the slave (RP2350A / U6) occupies GPIO20..42.
// The pin map lives in link/link_pins.h, shared by both halves so the
// two builds cannot drift apart. Nothing else may claim those pins.

#endif // BOARD_C2

#endif // BOARD_CONFIG_H
