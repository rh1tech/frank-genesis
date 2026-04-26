/*
 * frank-genesis - Sega Genesis/Megadrive Emulator for RP2350
 * Based on Gwenesis emulator
 */
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"  // For memory barriers
#include "hardware/dma.h"   // For DMA reset at startup
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#include "board_config.h"
#include "HDMI.h"
#include "psram_init.h"
#include "psram_allocator.h"
#include "ff.h"

// Gwenesis includes
#include "bus/gwenesis_bus.h"
#include "io/gwenesis_io.h"
#include "vdp/gwenesis_vdp.h"

// Enable M68K opcode profiling (must be defined before m68k.h)
#define M68K_OPCODE_PROFILING 1
#include "cpus/M68K/m68k.h"

#include "sound/z80inst.h"
#include "sound/z80_benchmark.h"
#include "sound/ym2612.h"
#include "sound/gwenesis_sn76489.h"

// Audio driver (simple DMA-based I2S)
#include "audio.h"

// Gamepad driver
#include "nespad/nespad.h"

// PS/2 Keyboard support
#include "ps2kbd/ps2kbd_wrapper.h"

// USB HID (gamepad support) - build with USB_HID_ENABLED=1 ./build.sh
#ifdef USB_HID_ENABLED
#include "usbhid/usbhid.h"
#endif

// ROM selector
#include "rom_selector.h"

// Settings menu
#include "settings.h"

//=============================================================================
// Profiling
//=============================================================================

// Simple logging (conditional on ENABLE_LOGGING)
#if ENABLE_LOGGING
#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define LOG(fmt, ...) do {} while(0)
#endif

#define ENABLE_PROFILING 1
#define DISABLE_FRAME_LIMITING 0

// Use assembly-optimized M68K loop (set to 0 to use original C loop for debugging)
#define USE_M68K_FAST_LOOP 0

// Frame skipping (video-only): reduce rendering cost to keep emulation/audio stable.
// The user requested a deterministic pattern (no adaptiveness).
#define ENABLE_ADAPTIVE_FRAMESKIP 0
#define ENABLE_CONSTANT_FRAMESKIP 1

// Line interlacing: render only every other line and duplicate to halve VDP time.
// Set via compile flag: -DLINE_INTERLACE=1 (0=off, 1=on)
#ifndef LINE_INTERLACE
#define LINE_INTERLACE 0
#endif

// Constant frameskip pattern:
// - Pattern length is in frames
// - Bit i (LSB=frame 0) indicates whether to render that frame (1) or skip (0)
// Configurable via -DFRAMESKIP_LEVEL=N where:
//   0 = render all frames (60 fps target)
//   1 = render 5/6 frames (~50 fps)
//   2 = render 4/6 frames (~40 fps)
//   3 = render 3/6 frames (~30 fps) - DEFAULT
//   4 = render 2/6 frames (~20 fps)
#ifndef FRAMESKIP_LEVEL
#define FRAMESKIP_LEVEL 3
#endif

// Frameskip patterns: [len, mask] for each level
// Level 0: render every frame (60fps)
// Level 1: render 5/6 (50fps)
// Level 2: render 4/6 (40fps)
// Level 3: render 3/6 (30fps) - default
// Level 4: render 2/6 (20fps)
static const uint8_t frameskip_patterns[5][2] = {
    {1, 0x01},  // 0: none - render every frame
    {6, 0x1F},  // 1: low - render frames 0-4, skip frame 5
    {6, 0x15},  // 2: medium - render frames 0,2,4 (4/6)
    {6, 0x09},  // 3: high - render frames 0,3 (3/6 = 30fps)
    {6, 0x05},  // 4: extreme - render frames 0,2 (2/6 = 20fps)
};

// Runtime frameskip settings (set from g_settings.frameskip)
static uint32_t frameskip_pattern_len = 6;
static uint32_t frameskip_pattern_mask = 0x09;  // Default: level 3

// Set frameskip level at runtime
void set_frameskip_level(uint8_t level) {
    if (level > 4) level = 3;  // Clamp to valid range
    frameskip_pattern_len = frameskip_patterns[level][0];
    frameskip_pattern_mask = frameskip_patterns[level][1];
}

#if FRAMESKIP_LEVEL == 0
  #define FRAMESKIP_PATTERN_LEN 1u
  #define FRAMESKIP_PATTERN_MASK 0x01u  // render every frame
#elif FRAMESKIP_LEVEL == 1
  #define FRAMESKIP_PATTERN_LEN 6u
  #define FRAMESKIP_PATTERN_MASK 0x1Fu  // 0b01_1111 : render frames 0-4, skip frame 5
#elif FRAMESKIP_LEVEL == 2
  #define FRAMESKIP_PATTERN_LEN 6u
  #define FRAMESKIP_PATTERN_MASK 0x15u  // 0b01_0101 : render frames 0,2,4 (4/6)
#elif FRAMESKIP_LEVEL == 4
  #define FRAMESKIP_PATTERN_LEN 6u
  #define FRAMESKIP_PATTERN_MASK 0x05u  // 0b00_0101 : render frames 0,2 (2/6 = 20fps)
#else  // Default: FRAMESKIP_LEVEL == 3
  #define FRAMESKIP_PATTERN_LEN 6u
  #define FRAMESKIP_PATTERN_MASK 0x09u  // 0b00_1001 : render frames 0,3 (3/6 = 30fps)
#endif
#define FRAMESKIP_MAX_CONSECUTIVE 4
#define FRAMESKIP_MAX_BACKLOG_FRAMES 8
#define FRAMESKIP_RENDER_COST_DEFAULT_US 4000u
// Strong blink protection: never skip the opposite-parity (even/odd) frame after
// rendering. This keeps 60Hz alternating effects (invincibility blinking) visible
// even when we fall back to ~30Hz rendering.
#define FRAMESKIP_STRONG_BLINK_PROTECTION 0

// Stronger blink protection: when frameskipping is active, render in pairs
// (two consecutive frames) when we do render. This avoids aliasing 1-frame
// on/off blinking into "always invisible" when rendering cadence becomes periodic.
#define FRAMESKIP_RENDER_PAIRS_WHEN_SKIPPING 1

// Extra blink protection: break phase lock by occasionally rendering even when
// the controller would skip. This is intentionally lightweight (LCG) and only
// applies while in skip mode.
#define FRAMESKIP_DITHER_RENDER_WHEN_SKIPPING 1
#define FRAMESKIP_DITHER_MASK 0x3u  // 0x3 => ~1/4 chance; larger mask => rarer

// When recovering from skip mode, render a short burst of consecutive frames.
// This increases the chance we capture both phases of longer blink patterns.
#define FRAMESKIP_RENDER_BURST_LEN 4u

// Aggressiveness tuning (higher = skip earlier / recover faster)
// - Threshold divisor: lower means more aggressive skipping.
// - Paydown factor: >1.0 makes each skipped render reduce backlog more.
#define FRAMESKIP_SKIP_THRESHOLD_DIVISOR 4u   // was effectively 2u
#define FRAMESKIP_SKIP_PAYDOWN_NUM 3u
#define FRAMESKIP_SKIP_PAYDOWN_DEN 2u

#if USE_M68K_FAST_LOOP
// Assembly-optimized M68K execution loop
extern void m68k_run_fast(unsigned int cycles);
#endif

// Emulation speed control (in percentage: 100 = normal, 50 = half speed, 150 = 1.5x speed)
#define EMULATION_SPEED_PERCENT 100

#if ENABLE_PROFILING
typedef struct {
    uint64_t m68k_time;
    uint64_t z80_time;
    uint64_t vdp_time;
    uint64_t sound_time;
    uint64_t audio_wait_time;
    uint64_t frame_time;
    uint64_t idle_time;
    uint32_t frame_count;
    uint64_t min_frame_time;
    uint64_t max_frame_time;
    uint32_t slow_frames;  // Frames that took > 17ms
    uint32_t fast_frames;  // Frames that took < 16ms
} profile_stats_t;

static profile_stats_t profile_stats = {0};
static uint64_t profile_frame_start = 0;
static uint64_t profile_section_start = 0;

#define PROFILE_START() profile_section_start = time_us_64()
#define PROFILE_END(stat) profile_stats.stat += (time_us_64() - profile_section_start)
#define PROFILE_FRAME_START() profile_frame_start = time_us_64()
#define PROFILE_FRAME_END() do { \
  uint64_t frame_duration = time_us_64() - profile_frame_start; \
  profile_stats.frame_time += frame_duration; \
  if (profile_stats.frame_count == 0 || frame_duration < profile_stats.min_frame_time) \
    profile_stats.min_frame_time = frame_duration; \
  if (frame_duration > profile_stats.max_frame_time) \
    profile_stats.max_frame_time = frame_duration; \
  if (frame_duration > 17000) profile_stats.slow_frames++; \
  if (frame_duration < 16000) profile_stats.fast_frames++; \
  profile_stats.frame_count++; \
} while(0)

static const char* frameskip_level_names[] = {"NONE", "LOW", "MEDIUM", "HIGH", "EXTREME"};

static void print_profiling_stats(void) {
    if (profile_stats.frame_count == 0) return;
    
    uint64_t total = profile_stats.frame_time;
    uint64_t tracked = profile_stats.m68k_time + profile_stats.z80_time + 
                       profile_stats.vdp_time + profile_stats.sound_time + 
                       profile_stats.audio_wait_time + profile_stats.idle_time;
    uint64_t other = (total > tracked) ? (total - tracked) : 0;
    
    LOG("\n=== Profiling Stats (avg per frame over %u frames) ===\n", profile_stats.frame_count);
    LOG("--- Active Settings ---\n");
    LOG("CPU: %u MHz, PSRAM: %u MHz\n", g_settings.cpu_freq, g_settings.psram_freq);
    LOG("Frameskip: %s, CRT: %s (%u%%)\n", 
        frameskip_level_names[g_settings.frameskip],
        g_settings.crt_effect ? "ON" : "OFF",
        g_settings.crt_dim);
    LOG("Audio: %s, FM: %s, Z80: %s\n",
        g_settings.audio_enabled ? "ON" : "OFF",
        g_settings.fm_sound ? "ON" : "OFF",
        g_settings.z80_enabled ? "ON" : "OFF");
    LOG("Channels: FM1-%c FM2-%c FM3-%c FM4-%c FM5-%c DAC-%c PSG-%c\n",
        CHANNEL_ENABLED(g_settings.channel_mask, 0) ? 'Y' : 'N',
        CHANNEL_ENABLED(g_settings.channel_mask, 1) ? 'Y' : 'N',
        CHANNEL_ENABLED(g_settings.channel_mask, 2) ? 'Y' : 'N',
        CHANNEL_ENABLED(g_settings.channel_mask, 3) ? 'Y' : 'N',
        CHANNEL_ENABLED(g_settings.channel_mask, 4) ? 'Y' : 'N',
        CHANNEL_ENABLED(g_settings.channel_mask, 5) ? 'Y' : 'N',
        CHANNEL_ENABLED(g_settings.channel_mask, 6) ? 'Y' : 'N');
    LOG("--- Timing Breakdown ---\n");
    LOG("M68K execution:  %6lu us (%3d%%)\n", 
        (unsigned long)(profile_stats.m68k_time / profile_stats.frame_count),
        (int)((profile_stats.m68k_time * 100) / total));
    LOG("Z80 execution:   %6lu us (%3d%%)\n", 
        (unsigned long)(profile_stats.z80_time / profile_stats.frame_count),
        (int)((profile_stats.z80_time * 100) / total));
    LOG("VDP rendering:   %6lu us (%3d%%)\n", 
        (unsigned long)(profile_stats.vdp_time / profile_stats.frame_count),
        (int)((profile_stats.vdp_time * 100) / total));
    LOG("Sound chips:     %6lu us (%3d%%)\n", 
        (unsigned long)(profile_stats.sound_time / profile_stats.frame_count),
        (int)((profile_stats.sound_time * 100) / total));
    LOG("Audio wait:      %6lu us (%3d%%)\n", 
        (unsigned long)(profile_stats.audio_wait_time / profile_stats.frame_count),
        (int)((profile_stats.audio_wait_time * 100) / total));
    LOG("Other/overhead:  %6lu us (%3d%%)\n", 
        (unsigned long)(other / profile_stats.frame_count),
        (int)((other * 100) / total));
    LOG("Total frame:     %6lu us (min=%lu, max=%lu)\n", 
        (unsigned long)(total / profile_stats.frame_count),
        (unsigned long)profile_stats.min_frame_time,
        (unsigned long)profile_stats.max_frame_time);
    LOG("Frame rate:      %6.2f fps (target=60.00)\n", 1000000.0 / (total / (float)profile_stats.frame_count));
    LOG("Slow frames: %u (>17ms), Fast: %u (<16ms)\n",
        profile_stats.slow_frames, profile_stats.fast_frames);
    LOG("================================================\n\n");
    
    // Reset stats
    memset(&profile_stats, 0, sizeof(profile_stats));
}
#else
#define PROFILE_START() do {} while(0)
#define PROFILE_END(stat) do {} while(0)
#define PROFILE_FRAME_START() do {} while(0)
#define PROFILE_FRAME_END() do {} while(0)
#define print_profiling_stats() do {} while(0)
#endif

// Screen buffer - 320x240 8-bit indexed (static, not in PSRAM)
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
static uint8_t SCREEN[SCREEN_HEIGHT][SCREEN_WIDTH];

// Screen save buffer for in-game settings menu
static uint8_t *saved_game_screen = NULL;

// Button ignore until timestamp - all buttons forced released until this time
static volatile uint64_t button_ignore_until = 0;

// Simple button lock - when true, ALL buttons forced released
static volatile bool button_lock = false;

// Genesis button state (defined in gwenesis_io.c)
extern unsigned char button_state[];

// Semaphore for render core sync
static semaphore_t render_start_semaphore;

// Sound processing synchronization
static volatile int sound_lines_per_frame = LINES_PER_FRAME_NTSC;
static volatile int sound_screen_height = 224;
static volatile bool frame_ready = false;  // Core 0 signals frame done
static volatile bool audio_done = false;   // Core 1 signals audio submitted

// Saved sample counts for Core 1 (avoids race condition when reading indices)
volatile int saved_ym_samples = 0;
volatile int saved_sn_samples = 0;
volatile int16_t last_frame_sample = 0;  // Last sample for crossfade

// Read buffer pointers for Core 1 (points to completed frame's audio)
int16_t *audio_read_sn76489 = NULL;
int16_t *audio_read_ym2612 = NULL;

// ROM buffer in PSRAM
static uint8_t *rom_buffer = NULL;
// Remove duplicate MAX_ROM_SIZE - it's defined in gwenesis_bus.h

// Gwenesis external variables
extern unsigned char* ROM_DATA;
extern unsigned char M68K_RAM[];
extern unsigned char ZRAM[];
extern unsigned char gwenesis_vdp_regs[];
extern unsigned int gwenesis_vdp_status;
extern int hint_pending;
extern int screen_width;
extern int screen_height;

// Audio buffers - DOUBLE BUFFERED to prevent race conditions
// Core 0 writes to one buffer, Core 1 reads from the other
// Use __not_in_flash to ensure they stay in RAM
// Buffer size: ~888 samples/frame typical, 2048 gives good headroom
#define AUDIO_BUFFER_SIZE 2048
static int16_t __not_in_flash("audio") gwenesis_sn76489_buffer_mem[2][AUDIO_BUFFER_SIZE];
static int16_t __not_in_flash("audio") gwenesis_ym2612_buffer_mem[2][AUDIO_BUFFER_SIZE];

// Current write buffer index (0 or 1) - Core 0 writes here
static volatile int audio_write_buffer = 0;
// Current read buffer index (0 or 1) - Core 1 reads here
static volatile int audio_read_buffer = 0;

// Exported pointers for external access (points to current write buffer)
int16_t *gwenesis_sn76489_buffer = gwenesis_sn76489_buffer_mem[0];
int16_t *gwenesis_ym2612_buffer = gwenesis_ym2612_buffer_mem[0];

volatile int sn76489_index;
volatile int sn76489_clock;

volatile int ym2612_index;
volatile int ym2612_clock;

// Audio enabled flags
bool audio_enabled = true;
bool sn76489_enabled = true;  // PSG/DAC sound
bool ym2612_enabled = true;   // FM sound
extern bool ym2612_fm_enabled;   // FM channels mute (in ym2612.c)
extern bool ym2612_dac_enabled;  // DAC mute (in ym2612.c)
extern bool ym2612_channel_enabled[6];  // Per-channel mute (in ym2612.c)

// Z80 enabled flag
bool z80_enabled = true;

// Timing
int system_clock;
unsigned int lines_per_frame = LINES_PER_FRAME_NTSC;
int scan_line;
unsigned int frame_counter = 0;

// FatFS
static FATFS fs;

// Flash timing configuration for overclocking
#define FLASH_MAX_FREQ_MHZ 88

static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = FLASH_MAX_FREQ_MHZ * 1000000;
    
    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) {
        divisor = 2;
    }
    
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) {
        rxdelay += 1;
    }
    
    qmi_hw->m[0].timing = 0x60007000 |
                        rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                        divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

// Runtime PSRAM frequency setting (used after clock reconfiguration)
static uint16_t runtime_psram_freq = PSRAM_MAX_FREQ_MHZ;

// Reconfigure system clocks based on settings
// This is called after loading settings if they differ from current clocks
static void __no_inline_not_in_flash_func(reconfigure_clocks)(uint16_t cpu_mhz, uint16_t psram_mhz) {
    LOG("Reconfiguring clocks: CPU=%d MHz, PSRAM=%d MHz\n", cpu_mhz, psram_mhz);
    
    // Set voltage based on target CPU speed
    if (cpu_mhz >= 504) {
        vreg_disable_voltage_limit();
        vreg_set_voltage(VREG_VOLTAGE_1_65);
    } else if (cpu_mhz >= 378) {
        vreg_disable_voltage_limit();
        vreg_set_voltage(VREG_VOLTAGE_1_60);
    } else {
        vreg_set_voltage(VREG_VOLTAGE_1_50);
    }
    sleep_ms(10);  // Let voltage settle
    
    // Update flash timings for new clock
    set_flash_timings(cpu_mhz);
    
    // Set system clock
    if (!set_sys_clock_khz(cpu_mhz * 1000, false)) {
        LOG("Failed to set clock to %d MHz, falling back to 252 MHz\n", cpu_mhz);
        set_sys_clock_khz(252 * 1000, true);
    }
    
    // Store runtime PSRAM frequency for psram_init to use
    runtime_psram_freq = psram_mhz;
    
    // Re-initialize PSRAM with new clock settings
    uint psram_pin = get_psram_pin();
    psram_init_with_freq(psram_pin, psram_mhz);
    
    LOG("Clocks reconfigured: CPU=%lu MHz\n", clock_get_hz(clk_sys) / 1000000);
}

// Load ROM from SD card
static bool load_rom(const char *filename) {
    FIL file;
    UINT bytes_read;
    
    LOG("Opening ROM: %s\n", filename);
    
    FRESULT res = f_open(&file, filename, FA_READ);
    if (res != FR_OK) {
        LOG("Failed to open ROM file: %d\n", res);
        return false;
    }
    
    FSIZE_t file_size = f_size(&file);
    LOG("ROM size: %lu bytes\n", (unsigned long)file_size);
    
    if (file_size > MAX_ROM_SIZE) {
        LOG("ROM too large!\n");
        f_close(&file);
        return false;
    }
    
    // Allocate ROM buffer in PSRAM (size based on actual file, rounded up to 64KB)
    size_t alloc_size = (file_size + 0xFFFF) & ~0xFFFF;  // Round up to 64KB boundary
    if (rom_buffer == NULL) {
        rom_buffer = (uint8_t *)psram_malloc(alloc_size);
        if (rom_buffer == NULL) {
            LOG("Failed to allocate ROM buffer (%lu bytes)!\n", (unsigned long)alloc_size);
            f_close(&file);
            return false;
        }
        LOG("Allocated %lu bytes for ROM\n", (unsigned long)alloc_size);
    }
    
    // Read ROM into buffer
    res = f_read(&file, rom_buffer, file_size, &bytes_read);
    f_close(&file);
    
    if (res != FR_OK || bytes_read != file_size) {
        LOG("Failed to read ROM: %d\n", res);
        return false;
    }
    
    LOG("ROM loaded: %lu bytes\n", (unsigned long)bytes_read);
    
    // Byte-swap ROM (Genesis ROMs are big-endian)
    for (size_t i = 0; i < bytes_read; i += 2) {
        uint8_t tmp = rom_buffer[i];
        rom_buffer[i] = rom_buffer[i + 1];
        rom_buffer[i + 1] = tmp;
    }
    
    // Set ROM_DATA to point to our PSRAM buffer
    ROM_DATA = rom_buffer;
    
    return true;
}

// Initialize Genesis emulator
static void genesis_init(void) {
    // Print M68K struct offsets for assembly optimization
#if ENABLE_LOGGING
    printf("M68K struct offsets:\n");
    printf("  cycles:      %zu\n", offsetof(m68ki_cpu_core, cycles));
    printf("  cycle_end:   %zu\n", offsetof(m68ki_cpu_core, cycle_end));
    printf("  dar:         %zu\n", offsetof(m68ki_cpu_core, dar));
    printf("  pc:          %zu\n", offsetof(m68ki_cpu_core, pc));
    printf("  ir:          %zu\n", offsetof(m68ki_cpu_core, ir));
    printf("  stopped:     %zu\n", offsetof(m68ki_cpu_core, stopped));
    printf("  sizeof:      %zu\n", sizeof(m68ki_cpu_core));
#endif
    
    // Clear RAM
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);
    
    // Initialize Z80
    z80_set_memory(ZRAM);
    z80_start();
    z80_pulse_reset();
    
    // Initialize M68K
    m68k_init();
    m68k_pulse_reset();
    
    // Initialize YM2612 with Genesis-Plus-GX improvements
    YM2612Init();
    YM2612ResetChip();  // MUST call reset to clear all registers after init
    YM2612Config(YM2612_DISCRETE);  // Use discrete chip emulation with ladder effect
    
    // Initialize PSG with Genesis-Plus-GX improvements
    gwenesis_SN76489_Init(3579545, 888 * 60, AUDIO_FREQ_DIVISOR, PSG_INTEGRATED);
    gwenesis_SN76489_Reset();
    
    // Initialize VDP
    gwenesis_vdp_reset();
    gwenesis_vdp_set_buffer((uint8_t *)SCREEN);
    
    // Clear screen buffer to avoid garbage (Genesis NTSC is 224 lines, buffer is 240)
    // Use index 1 instead of 0 - index 0 causes HDMI issues at 378MHz
    memset(SCREEN, 1, sizeof(SCREEN));
    
    LOG("Genesis initialized\n");
}

// Set up Genesis palette for HDMI
static void setup_genesis_palette(void) {
    // Genesis uses 9-bit color (3 bits per channel)
    // Initialize all palette entries to black
    // The actual palette will be updated from CRAM during emulation by VDP
    for (int i = 0; i < 256; i++) {
        graphics_set_palette(i, 0x000000);
    }
}

// Sound processing on Core 1 (I2S output only)
// With GWENESIS_AUDIO_ACCURATE=1, sound chips are run during M68K/Z80 emulation
// Core 1 just submits the already-generated samples to I2S DMA
static void __scratch_x("sound") sound_core(void) {
    // Allow core 0 to pause this core during flash operations
    multicore_lockout_victim_init();
    
    // Initialize audio on Core 1
    audio_init();
    
    // CRITICAL: Warmup period - wait for I2S/DMA to stabilize
    // Don't call audio_submit() - just wait
    LOG("Audio: Warmup delay...\n");
    sleep_ms(500);
    LOG("Audio: Warmup complete\n");
    
    // Signal that we're ready
    sem_release(&render_start_semaphore);
    
    // Core 1 loop - synchronized with Core 0 emulation
    while (1) {
        // Wait for Core 0 to complete a frame
        while (!frame_ready) {
            tight_loop_contents();
        }
        frame_ready = false;
        
        // Memory barrier to ensure we see all writes from Core 0
        __dmb();
        
        // Submit samples to I2S - samples were already generated during emulation
        // This blocks until previous frame's DMA is done
        audio_submit();
        
        // Signal Core 0 that audio is done
        audio_done = true;
    }
}

// Main emulation loop
static void __time_critical_func(emulation_loop)(void) {
    // Initialize screen dimensions
    screen_width = 320;
    screen_height = 224;
    int last_screen_width = 0;
    int last_screen_height = 0;
    
    gwenesis_vdp_set_buffer((uint8_t *)SCREEN);
    gwenesis_vdp_render_config();
    
    // Wait for all buttons to be released before starting emulation
    // This prevents Start+Select held during ROM selection from triggering settings immediately
    while (settings_check_hotkey()) {
        sleep_ms(50);
    }
    
    // Frame timing state (used for both pacing and adaptive frame-skip)
    uint64_t first_frame_time = 0;
    uint32_t frame_num = 0;
    uint32_t consecutive_skipped_frames = 0;
    uint64_t frame_work_start_us = 0;

#if ENABLE_ADAPTIVE_FRAMESKIP
    uint32_t frame_budget_us = 16666;
    uint32_t frame_work_us = 0;
    uint32_t audio_wait_us_local = 0;

    // Adaptive frameskip state
    uint32_t backlog_us = 0;                 // accumulated "time behind" (work - budget)
    uint32_t render_cost_ema_us = 0;         // EMA of render cost when we do render
    uint32_t force_render_next = 0;          // number of upcoming frames to force render (burst)
    uint32_t frameskip_rng = 0xC001D00Du;    // simple PRNG state for dithering
#endif

    while (1) {
        // Check for Start+Select hotkey to open settings menu
        if (settings_check_hotkey()) {
            // LOCK buttons immediately - no input will reach game until unlocked
            button_lock = true;
            button_state[0] = 0xFF;
            button_state[1] = 0xFF;
            button_state[2] = 0xFF;
            
            // Wait for buttons to be released first
            while (settings_check_hotkey()) {
                sleep_ms(50);
            }
            
            // Save current screen BEFORE changing anything
            // Note: saved_game_screen allocated in main(), may be NULL if allocation failed
            if (saved_game_screen != NULL) {
                memcpy(saved_game_screen, (uint8_t *)SCREEN, SCREEN_WIDTH * SCREEN_HEIGHT);
            }
            
            // Save current palette before showing settings
            uint64_t saved_palette[64];
            for (int i = 0; i < 64; i++) {
                saved_palette[i] = graphics_get_palette(i);
            }
            
            // Save current screen resolution
            int saved_screen_width = screen_width;
            int saved_screen_height = screen_height;
            
            // Clear screen BEFORE changing palette to avoid showing game with wrong colors
            memset((uint8_t *)SCREEN, 0, SCREEN_WIDTH * SCREEN_HEIGHT);
            
            // Force 320x240 resolution for settings menu
            graphics_set_res(320, 240);
            graphics_set_shift(0, 0);
            
            // Set up palette for settings menu
            for (int i = 0; i < 64; i++) {
                graphics_set_palette(i, 0x020202);
            }
            graphics_set_palette(63, 0xFFFFFF);  // White for text
            graphics_set_palette(48, 0xFFFF00);  // Yellow for highlight
            graphics_set_palette(42, 0x808080);  // Gray
            graphics_set_palette(32, 0xFF0000);  // Red
            graphics_restore_sync_colors();
            
            // Wait a couple frames for DMA to pick up changes
            sleep_ms(50);
            
            // Show settings menu (screen already saved above, pass buffer for restore on cancel)
            settings_result_t result = settings_menu_show_with_restore((uint8_t *)SCREEN, saved_game_screen);
            
            switch (result) {
                case SETTINGS_RESULT_SAVE_RESTART:
                    // Save settings to SD card and restart
                    settings_save();
                    watchdog_reboot(0, 0, 10);
                    while(1) tight_loop_contents();
                    break;
                    
                case SETTINGS_RESULT_RESTART:
                    // Restart without saving
                    watchdog_reboot(0, 0, 10);
                    while(1) tight_loop_contents();
                    break;
                    
                case SETTINGS_RESULT_CANCEL:
                default:
                    // Restore Genesis palette FIRST (before screen is visible)
                    for (int i = 0; i < 64; i++) {
                        graphics_set_palette(i, saved_palette[i]);
                    }
                    graphics_restore_sync_colors();
                    
                    // Restore screen resolution
                    graphics_set_res(saved_screen_width, saved_screen_height);
                    graphics_set_shift(saved_screen_width != 320 ? 32 : 0, saved_screen_height != 240 ? 8 : 0);
                    
                    // Now restore the saved screen (with correct palette already set)
                    if (saved_game_screen != NULL) {
                        memcpy((uint8_t *)SCREEN, saved_game_screen, SCREEN_WIDTH * SCREEN_HEIGHT);
                    }
                    gwenesis_vdp_render_config();
                    last_screen_width = saved_screen_width;
                    last_screen_height = saved_screen_height;
                    
                    // Keep buttons locked, wait for ALL buttons to be released
                    do {
                        sleep_ms(50);
                        nespad_read();
                    } while (nespad_state & (DPAD_A | DPAD_B | DPAD_START | DPAD_SELECT));
                    
                    // Extra delay to ensure clean release
                    sleep_ms(100);
                    
                    // NOW unlock buttons
                    button_lock = false;
                    
                    // Skip to next frame
                    continue;
            }
        }
        
        int hint_counter = gwenesis_vdp_regs[10];
        
        bool is_pal = REG1_PAL;
        // Target frame budget for adaptive frame skipping
    #if ENABLE_ADAPTIVE_FRAMESKIP
        frame_budget_us = 1000000u / (is_pal ? GWENESIS_REFRESH_RATE_PAL : GWENESIS_REFRESH_RATE_NTSC);
    #endif
        screen_width = REG12_MODE_H40 ? 320 : 256;
        screen_height = is_pal ? 240 : 224;
        lines_per_frame = is_pal ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        
        // Only update graphics config when screen dimensions change
        bool force_render = false;
        if (screen_width != last_screen_width || screen_height != last_screen_height) {
            graphics_set_res(screen_width, screen_height);
            graphics_set_shift(screen_width != 320 ? 32 : 0, screen_height != 240 ? 8 : 0);
            gwenesis_vdp_render_config();
            last_screen_width = screen_width;
            last_screen_height = screen_height;
            force_render = true;
        }

        // Decide whether to render this frame (video-only). Emulation + audio run every frame.
        bool render_this_frame = true;

    #if ENABLE_CONSTANT_FRAMESKIP
        // Deterministic render/skip pattern to avoid performance oscillations.
        // Uses runtime-configurable frameskip_pattern_len and frameskip_pattern_mask
        const uint32_t pat_idx = (frameskip_pattern_len ? (frame_num % frameskip_pattern_len) : 0u);
        render_this_frame = ((frameskip_pattern_mask >> pat_idx) & 1u) != 0u;
    #endif
#if ENABLE_ADAPTIVE_FRAMESKIP
        // If we have backlog, prefer skipping render (saves render_cost_ema_us).
        uint32_t estimated_render_cost_us = render_cost_ema_us ? render_cost_ema_us : FRAMESKIP_RENDER_COST_DEFAULT_US;
        if (force_render_next) {
            render_this_frame = true;
            force_render_next--;
        } else {
            if (backlog_us >= (estimated_render_cost_us / FRAMESKIP_SKIP_THRESHOLD_DIVISOR) && estimated_render_cost_us) {
                render_this_frame = false;
            }
        }
#if FRAMESKIP_DITHER_RENDER_WHEN_SKIPPING
        // If we're in skip mode and would skip, occasionally render anyway to avoid
        // getting phase-locked to game blinking patterns.
        if (!render_this_frame) {
            bool in_skip_mode = backlog_us >= (estimated_render_cost_us / FRAMESKIP_SKIP_THRESHOLD_DIVISOR);
            if (in_skip_mode) {
                frameskip_rng = frameskip_rng * 1664525u + 1013904223u;
                if ((frameskip_rng & FRAMESKIP_DITHER_MASK) == 0u) {
                    render_this_frame = true;
                }
            }
        }
#endif
#endif

        // Safety: always render at least once every (FRAMESKIP_MAX_CONSECUTIVE + 1) frames.
        if (consecutive_skipped_frames >= FRAMESKIP_MAX_CONSECUTIVE) {
            render_this_frame = true;
        }
        if (force_render) {
            render_this_frame = true;
        }

#if ENABLE_ADAPTIVE_FRAMESKIP
        // If we choose to skip, immediately reduce backlog by the estimated render cost.
        // This prevents long streaks of skips and makes the controller more stable.
        if (!render_this_frame && backlog_us) {
            uint32_t dec = render_cost_ema_us ? render_cost_ema_us : FRAMESKIP_RENDER_COST_DEFAULT_US;
            // Slightly over-pay (aggressive) to converge faster.
            uint32_t paydown = (dec * FRAMESKIP_SKIP_PAYDOWN_NUM) / FRAMESKIP_SKIP_PAYDOWN_DEN;
            backlog_us = (backlog_us > paydown) ? (backlog_us - paydown) : 0;
        }
#endif

        PROFILE_FRAME_START();
        frame_work_start_us = time_us_64();
        
        // No explicit frame limiting needed - audio DMA wait provides natural pacing
        // When running fast, Core 1 waits for DMA buffer room (~60 FPS)
        // When running slow, no waiting occurs (raw emulation speed)
        
        system_clock = 0;
        scan_line = 0;
        
        // Reset Z80 clock for new frame (now runs on Core 0)
        extern volatile int zclk;
        zclk = 0;
#ifdef USE_Z80_GPX
        // GPX Z80 needs timing reset when zclk is reset
        extern void z80_reset_timing(void);
        z80_reset_timing();
#endif
        
        // Reset sound chip indices for new frame
        sn76489_clock = 0;
        sn76489_index = 0;
        ym2612_clock = 0;
        ym2612_index = 0;
        
        // ==================================================================
        // PHASE 1: Run all emulation first (M68K + Z80 + sound chips)
        // This ensures sound chip state is updated at consistent timing
        // Z80 can be run in larger timeslices to reduce interpreter overhead.
        // This preserves overall playback speed (same total cycles), but may
        // reduce sub-scanline timing fidelity for some PCM-heavy drivers.
        // ==================================================================
        #ifndef Z80_SLICE_LINES
        #define Z80_SLICE_LINES 16
        #endif
        while (scan_line < lines_per_frame) {
            // Run M68K for one line
            PROFILE_START();
#if USE_M68K_FAST_LOOP
            m68k_run_fast(system_clock + VDP_CYCLES_PER_LINE);
#else
            m68k_run(system_clock + VDP_CYCLES_PER_LINE);
#endif
            PROFILE_END(m68k_time);
            
            // Run Z80 in chunks of scanlines to reduce call overhead.
            // Always run on VBlank IRQ edges (screen_height-1 and screen_height)
            // so the Z80 sees the vblank IRQ while asserted. Music drivers in
            // games like Comix Zone and Aladdin tick on this IRQ; missing it
            // silences or desyncs music.
            bool z80_run_due =
                ((scan_line % Z80_SLICE_LINES) == (Z80_SLICE_LINES - 1)) ||
                (scan_line == (lines_per_frame - 1)) ||
                (scan_line == (screen_height - 1)) ||
                (scan_line == screen_height);
            if (z80_run_due) {
                PROFILE_START();
                z80_run(system_clock + VDP_CYCLES_PER_LINE);
                PROFILE_END(z80_time);
            }
            
            // Note: Sound chips are called automatically during YM2612Write/SN76489_Write
            // with GWENESIS_AUDIO_ACCURATE=1 for cycle-accurate timing
            
            // Handle line counter interrupt
            if (scan_line == 0 || scan_line > screen_height) {
                hint_counter = gwenesis_vdp_regs[10];
            }
            
            if (--hint_counter < 0) {
                if (REG0_LINE_INTERRUPT != 0 && scan_line <= screen_height) {
                    hint_pending = 1;
                    if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                        m68k_update_irq(4);
                }
                hint_counter = gwenesis_vdp_regs[10];
            }
            
            scan_line++;
            
            // VBlank
            if (scan_line == screen_height) {
                if (REG1_VBLANK_INTERRUPT != 0) {
                    gwenesis_vdp_status |= STATUS_VIRQPENDING;
                    m68k_set_irq(6);
                }
                // Z80 IRQ for vblank (Z80 runs on Core 0)
                z80_irq_line(1);
            }
            if (scan_line == screen_height + 1) {
                z80_irq_line(0);
            }
            
            system_clock += VDP_CYCLES_PER_LINE;
        }
        
        // Generate any remaining audio samples for this frame
        // Fixed 888 samples per NTSC frame (53280 Hz / 60 fps)
        #define TARGET_SAMPLES_PER_FRAME 888
        #define AUDIO_TARGET_CLOCK (TARGET_SAMPLES_PER_FRAME * AUDIO_FREQ_DIVISOR)
        PROFILE_START();
        gwenesis_SN76489_run(AUDIO_TARGET_CLOCK);
        ym2612_run(AUDIO_TARGET_CLOCK);
        PROFILE_END(sound_time);
        
        // ==================================================================
        // PHASE 2: Render the frame AFTER emulation is complete
        // This decouples rendering from emulation timing for stable audio
        // ==================================================================
        if (render_this_frame) {
            PROFILE_START();
            uint64_t render_start_us = time_us_64();
#if LINE_INTERLACE
            // Line interlacing: render every other line, then duplicate
            // Alternates between even and odd lines each frame for better quality
            int start_line = (frame_counter & 1);  // 0 or 1
            for (int line = start_line; line < screen_height; line += 2) {
                gwenesis_vdp_render_line(line);
            }
            // Duplicate rendered lines to adjacent lines (using SCREEN buffer)
            for (int line = start_line; line < screen_height - 1; line += 2) {
                memcpy(SCREEN[line + 1], SCREEN[line], screen_width);
            }
#else
            for (int line = 0; line < screen_height; line++) {
                gwenesis_vdp_render_line(line);
            }
#endif
            uint32_t render_us = (uint32_t)(time_us_64() - render_start_us);
            PROFILE_END(vdp_time);

#if ENABLE_ADAPTIVE_FRAMESKIP
            // EMA update (1/8 smoothing). Keep a non-zero estimate.
            if (render_cost_ema_us == 0) render_cost_ema_us = render_us ? render_us : FRAMESKIP_RENDER_COST_DEFAULT_US;
            else render_cost_ema_us = (render_cost_ema_us * 7u + (render_us ? render_us : render_cost_ema_us)) / 8u;
#endif
        }
        
        frame_counter++;
        m68k.cycles -= system_clock;

#if Z80_BENCHMARK
        z80_benchmark_frame_end();
#endif

#if M68K_OPCODE_PROFILING
        m68k_check_profile_report();
#endif

        if (render_this_frame) {
            consecutive_skipped_frames = 0;
        } else {
            consecutive_skipped_frames++;
        }

#if ENABLE_ADAPTIVE_FRAMESKIP && FRAMESKIP_RENDER_PAIRS_WHEN_SKIPPING
        // If we rendered while we're in (or recovering from) skip mode, force a short
        // consecutive render burst to increase the odds of catching longer blink patterns.
        if (render_this_frame && !force_render && force_render_next == 0) {
            uint32_t estimated_render_cost_us = render_cost_ema_us ? render_cost_ema_us : FRAMESKIP_RENDER_COST_DEFAULT_US;
            bool in_skip_mode = backlog_us >= (estimated_render_cost_us / FRAMESKIP_SKIP_THRESHOLD_DIVISOR);
            if (in_skip_mode || consecutive_skipped_frames > 0) {
                if (FRAMESKIP_RENDER_BURST_LEN > 1u) {
                    force_render_next = FRAMESKIP_RENDER_BURST_LEN - 1u;
                }
            }
        }
#endif
        
        // Update sound parameters for Core 1
        sound_screen_height = screen_height;
        sound_lines_per_frame = lines_per_frame;
        
        // ==================================================================
        // PHASE 3: Signal Core 1 to submit audio
        // Must wait for previous audio to complete before reusing buffer
        // ==================================================================

        // Compute work time for this frame (emulation + optional render), excluding audio wait.
    #if ENABLE_ADAPTIVE_FRAMESKIP
        frame_work_us = (uint32_t)(time_us_64() - frame_work_start_us);
    #endif
        
        // Wait for previous audio submission to complete 
        // This prevents buffer race condition where Core 0 overwrites audio
        // while Core 1 is still reading it
        PROFILE_START();
        uint64_t audio_wait_start_us = time_us_64();
        while (!audio_done && frame_num > 0) {
            tight_loop_contents();
        }
    #if ENABLE_ADAPTIVE_FRAMESKIP
        audio_wait_us_local = (uint32_t)(time_us_64() - audio_wait_start_us);
    #endif
        PROFILE_END(audio_wait_time);
        audio_done = false;

#if ENABLE_ADAPTIVE_FRAMESKIP
        // Update backlog after the frame's work is complete.
        // If we had to wait for audio, we're not "behind" (audio pacing is active), so pay backlog down.
        {
            int32_t delta = (int32_t)frame_work_us - (int32_t)frame_budget_us;
            if (delta > 0) backlog_us += (uint32_t)delta;
            else {
                uint32_t dec = (uint32_t)(-delta);
                backlog_us = (backlog_us > dec) ? (backlog_us - dec) : 0;
            }

            if (audio_wait_us_local > 500u) {
                backlog_us = 0;
            }

            uint32_t max_backlog_us = frame_budget_us * FRAMESKIP_MAX_BACKLOG_FRAMES;
            if (backlog_us > max_backlog_us) backlog_us = max_backlog_us;
        }
#endif
        
        // Save sample counts for Core 1 BEFORE swapping buffers
        saved_ym_samples = ym2612_index;
        saved_sn_samples = sn76489_index;
        
        // Set read buffer pointers for Core 1 (current write buffer becomes read buffer)
        audio_read_sn76489 = gwenesis_sn76489_buffer;
        audio_read_ym2612 = gwenesis_ym2612_buffer;
        
        // Memory barrier to ensure all writes are visible to Core 1
        __dmb();
        
        // Swap to other buffer for next frame's writes
        audio_write_buffer = 1 - audio_write_buffer;
        gwenesis_sn76489_buffer = gwenesis_sn76489_buffer_mem[audio_write_buffer];
        gwenesis_ym2612_buffer = gwenesis_ym2612_buffer_mem[audio_write_buffer];
        
        // Signal Core 1 to process audio (from read buffer)
        // Core 1's DMA wait provides natural frame pacing when running fast
        frame_ready = true;
        
        frame_num++;
        
        PROFILE_FRAME_END();
        
#ifdef USB_HID_ENABLED
        // Poll USB HID Host for gamepad events
        usbhid_task();
#endif
        
        // Print profiling stats every 300 frames (~5 seconds at 60fps)
        if ((frame_counter % 300) == 0) {
            print_profiling_stats();
        }
    }
}

int main(void) {
    // CRITICAL: Full DMA reset at the very start
    // After warm reset, DMA channels may be running with stale config
    // We can't reset ALL DMA (HDMI uses it), but abort all channels first
    for (int ch = 0; ch < 12; ch++) {
        dma_channel_abort(ch);
    }
    // Wait for all channels to stop
    while (dma_hw->ch[0].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) tight_loop_contents();
    while (dma_hw->ch[1].ctrl_trig & DMA_CH1_CTRL_TRIG_BUSY_BITS) tight_loop_contents();
    // Clear all DMA IRQ flags
    dma_hw->ints0 = 0xFFFF;
    dma_hw->ints1 = 0xFFFF;
    
    // Invalidate XIP cache to ensure clean flash data after reset
    extern void xip_cache_clean_all(void);
    xip_cache_clean_all();
    
    // Early delay to let hardware settle
    for (volatile int i = 0; i < 1000000; i++) { }
    
    // CRITICAL: Force cold-boot behavior on warm resets
    // The reset button doesn't zero .bss like a power-on does
    // Reset all critical audio state immediately
    frame_ready = false;
    audio_done = true;
    last_frame_sample = 0;
    audio_write_buffer = 0;
    sn76489_index = 0;
    sn76489_clock = 0;
    ym2612_index = 0;
    ym2612_clock = 0;
    saved_ym_samples = 0;
    saved_sn_samples = 0;
    audio_read_sn76489 = NULL;
    audio_read_ym2612 = NULL;
    
    // Overclock support
#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ);
    sleep_ms(100);
#endif
    
    // Set system clock
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        set_sys_clock_khz(252 * 1000, true);
    }
    
    stdio_init_all();
    
#ifdef USB_HID_ENABLED
    // Initialize USB HID Host (for USB gamepad support)
    usbhid_init();
    LOG("USB HID Host initialized\n");
#else
    // Startup delay for USB serial console (4 seconds)
    for (int i = 0; i < 8; i++) {
        sleep_ms(500);
    }
#endif
    
    LOG("\n\n");
    LOG("========================================\n");
    LOG("   FRANK Genesis - Genesis for RP2350\n");
    LOG("========================================\n");
    LOG("System Clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    
    // Initialize PSRAM
    LOG("Initializing PSRAM...\n");
    uint psram_pin = get_psram_pin();
    LOG("PSRAM pin: %u\n", psram_pin);
    psram_init(psram_pin);
    psram_reset();
    LOG("PSRAM initialized\n");
    
    // Defer SD card mounting to after graphics init
    // so we can show an error message on screen
    LOG("SD card will be mounted after graphics init\\n");
    
    // Use default settings until we can read from SD card
    // (settings_load() will use defaults if file doesn't exist)
    
    // Clear the screen buffer BEFORE HDMI init - DMA starts scanning immediately
    // Use index 1 instead of 0 - index 0 causes HDMI issues at 378MHz
    memset(SCREEN, 1, sizeof(SCREEN));
    
    // Initialize HDMI on Core 0 - DMA IRQ is timing-critical
    LOG("Initializing HDMI...\n");
    graphics_init(g_out_HDMI);
    
    // Set up screen buffer
    uint8_t *buffer = (uint8_t *)SCREEN;
    graphics_set_buffer(buffer);
    graphics_set_res(SCREEN_WIDTH, SCREEN_HEIGHT);
    graphics_set_shift(0, 0);
    
    // Don't call setup_genesis_palette() yet - we'll do it after ROM selector
    LOG("HDMI initialized\n");
    
    // Initialize semaphore for sound core sync
    sem_init(&render_start_semaphore, 0, 1);
    
    // Zero audio buffers to prevent garbage from previous session after hard reset
    memset(gwenesis_sn76489_buffer_mem, 0, sizeof(gwenesis_sn76489_buffer_mem));
    memset(gwenesis_ym2612_buffer_mem, 0, sizeof(gwenesis_ym2612_buffer_mem));
    
    // Reset buffer pointers to valid zeroed buffers
    gwenesis_sn76489_buffer = gwenesis_sn76489_buffer_mem[0];
    gwenesis_ym2612_buffer = gwenesis_ym2612_buffer_mem[0];
    audio_read_sn76489 = gwenesis_sn76489_buffer_mem[0];
    audio_read_ym2612 = gwenesis_ym2612_buffer_mem[0];
    
    // Launch Core 1 (sound generation + I2S output)
    LOG("Starting sound core...\\n");
    multicore_launch_core1(sound_core);
    
    // Wait for Core 1 to be ready
    sem_acquire_blocking(&render_start_semaphore);
    LOG("Sound core started\\n");
    
    // Initialize gamepad (needed for ROM selector)
    LOG("Initializing gamepad...\n");
#ifdef NESPAD_GPIO_CLK
    if (nespad_begin(clock_get_hz(clk_sys) / 1000, NESPAD_GPIO_CLK, NESPAD_GPIO_DATA, NESPAD_GPIO_LATCH)) {
        LOG("Gamepad initialized (CLK=%d, DATA=%d, LATCH=%d)\n", 
            NESPAD_GPIO_CLK, NESPAD_GPIO_DATA, NESPAD_GPIO_LATCH);
    } else {
        LOG("Failed to initialize gamepad!\n");
    }
#else
    LOG("Gamepad not configured for this board\n");
#endif

    // Initialize PS/2 keyboard
    LOG("Initializing PS/2 keyboard...\n");
    ps2kbd_init();
    LOG("PS/2 keyboard initialized (CLK=%d, DATA=%d)\n", PS2_PIN_CLK, PS2_PIN_DATA);
    
    // Set up a simple palette for ROM selector (before calling it)
    LOG("Setting up ROM selector palette...\n");
    graphics_set_palette(0, 0x020202);      // Very dark (not pure black - HDMI issue at 378MHz)
    graphics_set_palette(1, 0x020202);      // Near-black (same as 0)
    graphics_set_palette(63, 0xFFFFFF);     // White (max visible index with 0x3F mask)
    graphics_set_palette(32, 0xFF0000);     // Red for title
    graphics_set_palette(16, 0x404040);     // Dark gray for scrollbar
    graphics_set_palette(42, 0x808080);     // Medium gray (used by warning splash box)
    graphics_restore_sync_colors();         // Ensure HDMI reserved sync symbols are intact

    // Ensure we don't briefly display uninitialized pixels with the new palette
    // Use index 1 instead of 0 - index 0 causes HDMI issues at 378MHz
    memset(SCREEN, 1, sizeof(SCREEN));
    
    // Now mount SD card (after graphics init so we can show errors)
    LOG("Mounting SD card...\n");
    FRESULT res = f_mount(&fs, "", 1);
    if (res != FR_OK) {
        LOG("Failed to mount SD card: %d\n", res);
        rom_selector_show_sd_error((uint8_t *)SCREEN, res);
        // Never returns
    }
    LOG("SD card mounted\n");
    
    // Load settings from SD card
    LOG("Loading settings...\n");
    settings_load();
    LOG("Settings loaded: CPU=%d MHz, PSRAM=%d MHz, FM=%s, DAC=%s, CRT=%s/%d%%\n",
        g_settings.cpu_freq, g_settings.psram_freq,
        g_settings.fm_sound ? "on" : "off",
        g_settings.dac_sound ? "on" : "off",
        g_settings.crt_effect ? "on" : "off",
        g_settings.crt_dim);
    
    // Apply runtime settings
    graphics_set_crt_effect(g_settings.crt_effect, g_settings.crt_dim);
    
    z80_enabled = g_settings.z80_enabled;
    LOG("Z80: %s\n", z80_enabled ? "enabled" : "disabled");
    
    audio_enabled = g_settings.audio_enabled;
    if (!g_settings.audio_enabled) {
        ym2612_enabled = false;
        sn76489_enabled = false;
        LOG("Audio: DISABLED (max performance mode)\n");
    } else {
        ym2612_enabled = true;
        ym2612_fm_enabled = g_settings.fm_sound;
        ym2612_dac_enabled = CHANNEL_ENABLED(g_settings.channel_mask, 5);
        sn76489_enabled = CHANNEL_ENABLED(g_settings.channel_mask, 6);
        for (int i = 0; i < 6; i++) {
            ym2612_channel_enabled[i] = CHANNEL_ENABLED(g_settings.channel_mask, i);
        }
        LOG("Audio: FM=%s, Channels=0x%02X, PSG=%s\n", 
            g_settings.fm_sound ? "on" : "off",
            g_settings.channel_mask & 0x3F,
            CHANNEL_ENABLED(g_settings.channel_mask, 6) ? "on" : "off");
    }
    
    // Check if CPU/PSRAM frequencies need to change
    uint32_t current_cpu_mhz = clock_get_hz(clk_sys) / 1000000;
    if (g_settings.cpu_freq != current_cpu_mhz || g_settings.psram_freq != PSRAM_MAX_FREQ_MHZ) {
        LOG("Settings require clock reconfiguration (CPU: %lu->%d, PSRAM: %d->%d)\n",
            current_cpu_mhz, g_settings.cpu_freq, PSRAM_MAX_FREQ_MHZ, g_settings.psram_freq);
        reconfigure_clocks(g_settings.cpu_freq, g_settings.psram_freq);
    }
    
    // Show ROM selector
    LOG("Showing ROM selector...\n");
    static char selected_rom[MAX_ROM_PATH];
    
    if (!rom_selector_show(selected_rom, sizeof(selected_rom), (uint8_t *)SCREEN)) {
        LOG("No ROM selected!\n");
        while (1) {
            tight_loop_contents();
        }
    }
    
    // Set up Genesis palette after ROM selection
    setup_genesis_palette();
    graphics_restore_sync_colors();  // Restore HDMI sync after palette init
    
    // Load selected ROM
    LOG("Loading ROM: %s\n", selected_rom);
    if (!load_rom(selected_rom)) {
        LOG("Failed to load ROM: %s\n", selected_rom);
        while (1) {
            tight_loop_contents();
        }
    }
    
    // Initialize emulator
    genesis_init();
    
    // Allocate screen save buffer for in-game settings menu
    saved_game_screen = (uint8_t *)psram_malloc(SCREEN_WIDTH * SCREEN_HEIGHT);
    if (saved_game_screen == NULL) {
        LOG("Warning: Could not allocate screen save buffer\n");
    }
    
    // Audio is initialized on Core 1 (render_core)
    
    LOG("Starting emulation...\n");
    
    // Run emulation
    emulation_loop();
    
    return 0;
}

// Gwenesis button state is defined in gwenesis_io.c
extern unsigned char button_state[];

// Genesis button mapping (button_state bits):
// Bit 0: Up
// Bit 1: Down  
// Bit 2: Left
// Bit 3: Right
// Bit 0: UP
// Bit 1: DOWN  
// Bit 2: LEFT
// Bit 3: RIGHT
// Bit 4: B
// Bit 5: C
// Bit 6: A
// Bit 7: Start
//
// NES Controller mapping (3-button):
// - D-pad → Genesis D-pad
// - NES B → Genesis B
// - NES A → Genesis A  
// - NES A + B → Genesis C
// - NES Start → Genesis Start
//
// SNES Controller mapping (6-button):
// - D-pad → Genesis D-pad
// - SNES B (bottom) → Genesis A (jump)
// - SNES A (right) → Genesis B (primary action)
// - SNES Y (left) → Genesis C (secondary action)
// - SNES X (top) → Genesis C (secondary alt)
// - SNES L → Genesis A (jump alt)
// - SNES R → Genesis B (primary alt)
// - Start → Genesis Start
// - Select+Start → Reset to ROM selector

void gwenesis_io_get_buttons(void) {
    // Simple lock - if locked, all buttons released, period.
    if (button_lock) {
        button_state[0] = 0xFF;
        button_state[1] = 0xFF;
        button_state[2] = 0xFF;
        return;
    }

#ifdef NESPAD_GPIO_CLK
    // Read gamepad state
    nespad_read();
    
    // Debug: track button presses for player 1
    static uint32_t prev_nespad_state = 0;
    uint32_t pressed = nespad_state & ~prev_nespad_state;  // Newly pressed buttons
    
    if (pressed) {
#if ENABLE_LOGGING
        printf("P1 Raw state: 0x%08lX | Pressed: 0x%08lX | ", 
               (unsigned long)nespad_state, (unsigned long)pressed);
        if (pressed & DPAD_UP)     printf("UP ");
        if (pressed & DPAD_DOWN)   printf("DOWN ");
        if (pressed & DPAD_LEFT)   printf("LEFT ");
        if (pressed & DPAD_RIGHT)  printf("RIGHT ");
        if (pressed & DPAD_SELECT) printf("SELECT ");
        if (pressed & DPAD_START)  printf("START ");
        if (pressed & DPAD_A)      printf("A(NES-A/SNES-B) ");
        if (pressed & DPAD_B)      printf("B(NES-B/SNES-Y) ");
        if (pressed & DPAD_Y)      printf("Y(SNES-A) ");
        if (pressed & DPAD_X)      printf("X(SNES-X) ");
        if (pressed & DPAD_LT)     printf("L ");
        if (pressed & DPAD_RT)     printf("R ");
        printf("\n");
#endif
    }
    prev_nespad_state = nespad_state;
    
    // Check for SELECT+START combo to open settings menu
    // Note: The actual menu is shown from the emulation loop to properly pause emulation
    
    // Detect if SNES controller (has extended buttons)
    bool is_snes_pad1 = (nespad_state & (DPAD_X | DPAD_Y | DPAD_LT | DPAD_RT));
    bool is_snes_pad2 = (nespad_state2 & (DPAD_X | DPAD_Y | DPAD_LT | DPAD_RT));
    
    // Map buttons to Genesis controller - Pad 1
    button_state[0] = 0xFF; // Start with all buttons released
    
    // D-pad mapping (same for NES/SNES)
    if (nespad_state & DPAD_UP)    button_state[0] &= ~(1 << 0);
    if (nespad_state & DPAD_DOWN)  button_state[0] &= ~(1 << 1);
    if (nespad_state & DPAD_LEFT)  button_state[0] &= ~(1 << 2);
    if (nespad_state & DPAD_RIGHT) button_state[0] &= ~(1 << 3);
    
    if (is_snes_pad1) {
        // SNES controller - 6-button mapping
        // Note: Bit names don't match physical SNES button labels!
        // DPAD_A bit = Physical SNES B button (bottom)
        // DPAD_B bit = Physical SNES Y button (left)
        // DPAD_Y bit = Physical SNES A button (right)
        // DPAD_X bit = Physical SNES X button (top)
        //
        // Mapping for intuitive gameplay:
        // SNES B (bottom) → Genesis A (jump)
        // SNES A (right) → Genesis B (primary action - shoot)
        // SNES Y (left) → Genesis C (secondary action - special)
        // SNES X (top) → Genesis C (alternate)
        // SNES L → Genesis A (alternate jump)
        // SNES R → Genesis B (alternate shoot)
        if (nespad_state & DPAD_A)  button_state[0] &= ~(1 << 6); // SNES B → Genesis A (jump)
        if (nespad_state & DPAD_Y)  button_state[0] &= ~(1 << 4); // SNES A → Genesis B (shoot)
        if (nespad_state & DPAD_B)  button_state[0] &= ~(1 << 5); // SNES Y → Genesis C (special)
        if (nespad_state & DPAD_X)  button_state[0] &= ~(1 << 5); // SNES X → Genesis C (special alt)
        if (nespad_state & DPAD_LT) button_state[0] &= ~(1 << 6); // SNES L → Genesis A (jump alt)
        if (nespad_state & DPAD_RT) button_state[0] &= ~(1 << 4); // SNES R → Genesis B (shoot alt)
    } else {
        // NES controller - button combos
        bool a_pressed = (nespad_state & DPAD_A);
        bool b_pressed = (nespad_state & DPAD_B);
        
        // A+B combo = Genesis C
        if (a_pressed && b_pressed) {
            button_state[0] &= ~(1 << 5); // A+B = Genesis C
        } else {
            if (b_pressed) button_state[0] &= ~(1 << 4); // B = Genesis B
            if (a_pressed) button_state[0] &= ~(1 << 6); // A = Genesis A
        }
    }
    
    // Only pass Start to game if Select is NOT held (to allow Start+Select hotkey)
    if ((nespad_state & DPAD_START) && !(nespad_state & DPAD_SELECT)) {
        button_state[0] &= ~(1 << 7);
    }
    
    // Map buttons to Genesis controller - Pad 2
    // Only read NES pad 2 when gamepad2_mode is NES (default)
    button_state[1] = 0xFF;
    
    if (g_settings.gamepad2_mode == GAMEPAD2_MODE_NES) {
        // D-pad mapping (same for NES/SNES)
        if (nespad_state2 & DPAD_UP)    button_state[1] &= ~(1 << 0);
        if (nespad_state2 & DPAD_DOWN)  button_state[1] &= ~(1 << 1);
        if (nespad_state2 & DPAD_LEFT)  button_state[1] &= ~(1 << 2);
        if (nespad_state2 & DPAD_RIGHT) button_state[1] &= ~(1 << 3);
        
        if (is_snes_pad2) {
            // SNES controller - 6-button mapping (same as pad 1)
            if (nespad_state2 & DPAD_A)  button_state[1] &= ~(1 << 6); // SNES B → Genesis A (jump)
            if (nespad_state2 & DPAD_Y)  button_state[1] &= ~(1 << 4); // SNES A → Genesis B (shoot)
            if (nespad_state2 & DPAD_B)  button_state[1] &= ~(1 << 5); // SNES Y → Genesis C (special)
            if (nespad_state2 & DPAD_X)  button_state[1] &= ~(1 << 5); // SNES X → Genesis C (special alt)
            if (nespad_state2 & DPAD_LT) button_state[1] &= ~(1 << 6); // SNES L → Genesis A (jump alt)
            if (nespad_state2 & DPAD_RT) button_state[1] &= ~(1 << 4); // SNES R → Genesis B (shoot alt)
        } else {
            // NES controller - button combos
            bool a_pressed2 = (nespad_state2 & DPAD_A);
            bool b_pressed2 = (nespad_state2 & DPAD_B);
            
            // A+B combo = Genesis C
            if (a_pressed2 && b_pressed2) {
                button_state[1] &= ~(1 << 5); // A+B = Genesis C
            } else {
                if (b_pressed2) button_state[1] &= ~(1 << 4); // B = Genesis B
                if (a_pressed2) button_state[1] &= ~(1 << 6); // A = Genesis A
            }
        }
        
        if (nespad_state2 & DPAD_START) button_state[1] &= ~(1 << 7);
    }
#else
    // No gamepad - all buttons released
    button_state[0] = 0xFF;
    button_state[1] = 0xFF;
#endif

#ifdef USB_HID_ENABLED
    // USB gamepad handling based on gamepad2_mode setting
    // Default: USB mirrors NES (both control P1)
    // USB mode: USB controls P2, NES controls P1
    int usb_target_player = (g_settings.gamepad2_mode == GAMEPAD2_MODE_USB) ? 1 : 0;
    
    if (usbhid_gamepad_connected()) {
        usbhid_gamepad_state_t gp;
        usbhid_get_gamepad_state(&gp);
        
        // D-pad from USB gamepad
        if (gp.dpad & 0x01) button_state[usb_target_player] &= ~(1 << 0); // Up
        if (gp.dpad & 0x02) button_state[usb_target_player] &= ~(1 << 1); // Down
        if (gp.dpad & 0x04) button_state[usb_target_player] &= ~(1 << 2); // Left
        if (gp.dpad & 0x08) button_state[usb_target_player] &= ~(1 << 3); // Right
        
        // Buttons from USB gamepad (mapped in process_gamepad_report)
        // bit 0=A, 1=B, 2=C, 3=X, 4=Y, 5=Z, 6=Start, 7=Select/Mode
        if (gp.buttons & 0x01) button_state[usb_target_player] &= ~(1 << 6); // A → Genesis A
        if (gp.buttons & 0x02) button_state[usb_target_player] &= ~(1 << 4); // B → Genesis B
        if (gp.buttons & 0x04) button_state[usb_target_player] &= ~(1 << 5); // C → Genesis C
        if (gp.buttons & 0x40) button_state[usb_target_player] &= ~(1 << 7); // Start → Genesis Start
        
        // SELECT+START combo is now handled in the emulation loop for settings menu
    }
#endif

    // Keyboard handling based on gamepad2_mode setting
    // Default: Keyboard controls P1
    // Keyboard mode: Keyboard controls P2
    int kbd_target_player = (g_settings.gamepad2_mode == GAMEPAD2_MODE_KEYBOARD) ? 1 : 0;
    
    // PS/2 Keyboard input
    ps2kbd_tick();
    uint16_t kbd_state = ps2kbd_get_state();
    
#ifdef USB_HID_ENABLED
    // Merge USB keyboard state with PS/2 keyboard state
    kbd_state |= usbhid_get_kbd_state();
#endif
    
    // Apply keyboard state to the appropriate player
    if (kbd_state & KBD_STATE_UP)    button_state[kbd_target_player] &= ~(1 << 0);  // Up
    if (kbd_state & KBD_STATE_DOWN)  button_state[kbd_target_player] &= ~(1 << 1);  // Down
    if (kbd_state & KBD_STATE_LEFT)  button_state[kbd_target_player] &= ~(1 << 2);  // Left
    if (kbd_state & KBD_STATE_RIGHT) button_state[kbd_target_player] &= ~(1 << 3);  // Right
    if (kbd_state & KBD_STATE_A)     button_state[kbd_target_player] &= ~(1 << 6);  // A key -> Genesis A (bit 6)
    if (kbd_state & KBD_STATE_B)     button_state[kbd_target_player] &= ~(1 << 4);  // S key -> Genesis B (bit 4)
    if (kbd_state & KBD_STATE_C)     button_state[kbd_target_player] &= ~(1 << 5);  // D key -> Genesis C (bit 5)
    if (kbd_state & KBD_STATE_START) button_state[kbd_target_player] &= ~(1 << 7);  // Start
    if (kbd_state & KBD_STATE_SELECT) button_state[kbd_target_player] &= ~(1 << 7); // Select also as Start for P2
    // Note: X, Y, Z, Mode are for 6-button controllers (not yet fully implemented in gwenesis_io)
}
