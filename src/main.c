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
#include "sound/sound_backend.h"

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

// C2 only: the sound subsystem lives on the second RP2350, unless this
// build keeps it local for reference measurements.
#if defined(BOARD_C2) && !defined(C2_LOCAL_SOUND)
#define USE_SOUND_LINK 1
#endif

#ifdef USE_SOUND_LINK
#include "link_master.h"
extern int16_t  link_ym_samples_buf[];
extern int16_t  link_sn_samples_buf[];
extern volatile uint32_t link_ym_sample_count;
extern volatile uint32_t link_sn_sample_count;
extern bool sound_link_exchange(void);
extern void sound_link_backend_reset(void);
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

#ifdef SOUND_CAPTURE
/* Differential PCM capture. The analog capture path is dead, so instead of
 * listening we hash the samples the emulator actually hands to the I2S
 * stage. Running the local-sound build and the offloaded build from the
 * same reset with the same ROM must produce the same stream; the first
 * frame whose hash differs is the frame the offload diverges, which is a
 * far sharper signal than "the sound goes wrong after a while". */
#define SND_CAP_FRAMES     512
#define SND_CAP_PCM_START  300
#define SND_CAP_PCM_FRAMES 2
#define SND_CAP_PCM_LEN    888
uint32_t snd_cap_magic = 0x50434150u;   /* 'PACP' - anchors the dump */
uint32_t snd_cap_count = 0;
uint32_t snd_cap_crc[SND_CAP_FRAMES];
/* Per-frame Z80 state signature for the SAME frame as the hash above.
 * The offloaded build fills this from the slave's frame reply, the
 * local-sound build from its own Z80. If the PCM diverges while this
 * still matches, the fault is in the chips; if this diverges first,
 * the fault is in the Z80 or the event replay feeding it. */
uint32_t snd_cap_z80[SND_CAP_FRAMES];

/* Long-run verification. The per-frame arrays only cover the first ~8 s;
 * a running hash over every frame, sampled at fixed frame counts, checks
 * bit-identity for as long as the board runs without needing the RAM to
 * store it. */
uint32_t snd_cap_run;                    /* FNV over every frame's hash */
uint32_t snd_cap_ckpt[8];
static const uint32_t snd_cap_ckpt_at[8] =
    { 256, 512, 1024, 1536, 2048, 3072, 4096, 6144 };
volatile uint32_t snd_cap_z80_src;
/* Register-level window around the first divergence. PC/SP/AF/BC/DE/HL/
 * IX/IY/BANK/zclk for a span of frames, so the diff says which piece of
 * Z80 state went wrong rather than just that the hash changed. */
#define SND_CAP_REG_START  80
#define SND_CAP_REG_FRAMES 64
uint32_t snd_cap_regs[SND_CAP_REG_FRAMES][6];
volatile uint32_t snd_cap_reg_src[6];

/* Intra-frame trace. Frame-granular signatures say the two Z80s part
 * company on frame SND_TRACE_FRAME but not where inside it; this
 * records (PC, zclk) at every scanline boundary of that one frame, on
 * both halves, so the first differing scanline localises it. */
volatile uint32_t snd_trace_frame = 0xFFFFFFFFu;  /* poke over SWD to arm */

#define SEAM_FRAMES 512
uint32_t seam_frame;
uint16_t seam_ctrl_reads[SEAM_FRAMES];
uint16_t seam_zram_reads[SEAM_FRAMES];
uint32_t snd_trace_pc[128][2];
uint32_t snd_trace_n;
#ifdef C2_LOCAL_SOUND
/* How fresh must the master's Z80-RAM mirror be?
 *
 * The offloaded build cannot give the 68K this frame's Z80 writes at
 * all: the slave only replays frame N after the master has finished it.
 * So the question is how many frames of staleness the game tolerates.
 * snap1 models a mirror holding Z80 RAM as of the end of the previous
 * frame, snap2 the frame before that. The first read each of them would
 * have answered wrongly says which designs are viable. */
unsigned char zram_snap1[8192], zram_snap2[8192];
uint32_t stale1_first = 0xFFFFFFFFu, stale2_first = 0xFFFFFFFFu;
uint32_t stale1_hits, stale2_hits, stale_reads;

void snd_trace_run(void) {
    extern uint32_t master_sig_count;
    extern void z80_state_regs(uint32_t out[6]);
    if (master_sig_count != snd_trace_frame || snd_trace_n >= 128) return;
    uint32_t rr[6];
    z80_state_regs(rr);
    snd_trace_pc[snd_trace_n][0] = rr[0] & 0xFFFFu;   /* PC */
    snd_trace_pc[snd_trace_n][1] = rr[5];             /* zclk */
    snd_trace_n++;
}
unsigned char zram_stale_snap[8192];
uint32_t zram_stale_reads, zram_stale_diffs, zram_stale_first_frame = 0xFFFFFFFFu;
#endif
int16_t  snd_cap_pcm[SND_CAP_PCM_FRAMES * SND_CAP_PCM_LEN];
uint32_t snd_cap_counts[SND_CAP_PCM_FRAMES][2];
#endif

// ROM buffer in PSRAM
#ifdef C2_LOCAL_SOUND
/* Reference trace for the differential test against the slave. */
#define MASTER_SIG_SLOTS 1024
#define MASTER_DBG_FRAMES 48
uint32_t master_dbg[MASTER_DBG_FRAMES][6];
uint32_t master_sig[MASTER_SIG_SLOTS];
uint32_t master_sig_count;
#endif

static uint8_t *rom_buffer = NULL;
static uint32_t rom_size_bytes = 0;
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
    rom_size_bytes = (uint32_t)bytes_read;
    
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
void vdp_render_worker_poll(void);   /* defined with the emulation loop */

static void __scratch_x("sound") sound_core(void) {
    // Allow core 0 to pause this core during flash operations
    multicore_lockout_victim_init();
    
    // Initialize audio on Core 1
    audio_init();
    i2s_wait_hook = vdp_render_worker_poll;   /* draw while waiting on DMA */
    
    // CRITICAL: Warmup period - wait for I2S/DMA to stabilize
    // Don't call audio_submit() - just wait
    LOG("Audio: Warmup delay...\n");
    sleep_ms(500);
    LOG("Audio: Warmup complete\n");
    
    // Signal that we're ready
    sem_release(&render_start_semaphore);
    
    // Core 1 loop - synchronized with Core 0 emulation
    while (1) {
        // Wait for Core 0 to complete a frame, drawing its half of the
        // current frame's lines while that wait would otherwise be idle.
        while (!frame_ready) {
            vdp_render_worker_poll();
            tight_loop_contents();
        }
        frame_ready = false;
        
        // Memory barrier to ensure we see all writes from Core 0
        __dmb();
        
        // Submit samples to I2S - samples were already generated during emulation
#ifdef USE_SOUND_LINK
        // The sound subsystem lives on the slave. Ship this frame's
        // event stream and collect the chips' output, then hand it to
        // the same audio path M1/M2 use. Core 0 is already emulating the
        // next frame while this runs — that is the whole point of doing
        // it here rather than on core 0.
        if (sound_link_exchange()) {
            audio_read_ym2612  = link_ym_samples_buf;
            audio_read_sn76489 = link_sn_samples_buf;
            saved_ym_samples   = (int)link_ym_sample_count;
            saved_sn_samples   = (int)link_sn_sample_count;
        } else {
            // Slave silent or the exchange failed: audio_submit() fades
            // to zero rather than replaying the previous frame.
            saved_ym_samples = 0;
            saved_sn_samples = 0;
        }
        __dmb();
#endif

#ifdef SOUND_CAPTURE
        {
            /* FNV-1a over both chip buffers plus their sample counts. */
            uint32_t h = 2166136261u;
            const int16_t *cy = audio_read_ym2612;
            const int16_t *cs = audio_read_sn76489;
            if (cy) for (int i = 0; i < saved_ym_samples; i++) { h ^= (uint16_t)cy[i]; h *= 16777619u; }
            if (cs) for (int i = 0; i < saved_sn_samples; i++) { h ^= (uint16_t)cs[i]; h *= 16777619u; }
            h ^= (uint32_t)saved_ym_samples * 65599u + (uint32_t)saved_sn_samples;
            snd_cap_run ^= h;
            snd_cap_run *= 16777619u;
            for (int c = 0; c < 8; c++)
                if (snd_cap_count == snd_cap_ckpt_at[c]) snd_cap_ckpt[c] = snd_cap_run;

            if (snd_cap_count < SND_CAP_FRAMES) {
                snd_cap_crc[snd_cap_count] = h;
                snd_cap_z80[snd_cap_count] = snd_cap_z80_src;
            }
            if (snd_cap_count >= SND_CAP_REG_START &&
                snd_cap_count <  SND_CAP_REG_START + SND_CAP_REG_FRAMES) {
                uint32_t rs = snd_cap_count - SND_CAP_REG_START;
                for (int i = 0; i < 6; i++)
                    snd_cap_regs[rs][i] = snd_cap_reg_src[i];
            }
            if (snd_cap_count >= SND_CAP_PCM_START &&
                snd_cap_count <  SND_CAP_PCM_START + SND_CAP_PCM_FRAMES) {
                uint32_t sl = snd_cap_count - SND_CAP_PCM_START;
                int16_t *dst = &snd_cap_pcm[sl * SND_CAP_PCM_LEN];
                for (int i = 0; i < SND_CAP_PCM_LEN; i++)
                    dst[i] = (cy && i < saved_ym_samples) ? cy[i] : 0;
                snd_cap_counts[sl][0] = (uint32_t)saved_ym_samples;
                snd_cap_counts[sl][1] = (uint32_t)saved_sn_samples;
            }
            snd_cap_count++;
        }
#endif

        // This blocks until previous frame's DMA is done
        audio_submit();
        
        // Signal Core 0 that audio is done
        audio_done = true;
    }
}

/* Parallel line rendering.
 *
 * Core 1 sits idle in its frame wait for the whole of PHASE 2 — it has
 * nothing to do until core 0 hands it a finished frame — while core 0
 * spends ~3.9 ms of a 16.3 ms frame drawing lines. Rendering is safe to
 * split: emulation has already finished, so VRAM, CRAM, VSRAM and the
 * registers are stable, and the only per-line state (the two scratch
 * buffers in the VDP) is now per-core.
 *
 * Core 0 draws line 0 itself first: that is where
 * gwenesis_vdp_render_line() computes the plane geometry the rest of the
 * frame reads, so it has to be done before core 1 starts. */
/* Draw each line as the beam reaches it instead of drawing the whole
 * frame afterwards. Comix Zone rewrites VRAM while the beam is in the
 * visible field (measured: 171 writes per frame during active display),
 * so drawing at end of frame gives every line the final contents and its
 * page-fold comes out as banded garbage. */
#ifndef VDP_RASTER_RENDER
#define VDP_RASTER_RENDER 0
#endif

#ifndef VDP_SPLIT_RENDER
#define VDP_SPLIT_RENDER 1
#endif

#ifdef SCREEN_VERIFY
/* Running hash of every rendered frame, sampled at fixed frame counts.
 * Splitting the lines across two cores must produce exactly the same
 * pixels as rendering them all on one, and this is what proves it. */
uint32_t screen_hash_run;
uint32_t screen_hash_ckpt[6];
static const uint32_t screen_hash_at[6] = { 128, 256, 512, 1024, 1536, 2048 };
static uint32_t screen_hash_count;

static void screen_verify_frame(void) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < screen_height; y++) {
        const uint8_t *row = (const uint8_t *)SCREEN[y];
        for (int x = 0; x < screen_width; x++) { h ^= row[x]; h *= 16777619u; }
    }
    screen_hash_run ^= h; screen_hash_run *= 16777619u;
    for (int i = 0; i < 6; i++)
        if (screen_hash_count == screen_hash_at[i]) screen_hash_ckpt[i] = screen_hash_run;
    screen_hash_count++;
}
#endif

volatile int  vdp_job_from, vdp_job_to, vdp_job_step, vdp_job_line;
volatile bool vdp_job_done;
/* Claimed with an atomic exchange so exactly one core runs the job. Core
 * 1 takes it when it can, but it spends most of a frame blocked feeding
 * I2S, so core 0 reclaims it rather than stalling: worst case we are back
 * to rendering everything on core 0, never slower. */
volatile int  vdp_job_claim;

static inline void vdp_render_range(int from, int to, int step) {
    for (int line = from; line < to; line += step) gwenesis_vdp_render_line(line);
}

/* Called from core 1's idle wait. */
static bool vdp_job_take(void) {
    return __atomic_exchange_n(&vdp_job_claim, 0, __ATOMIC_ACQUIRE) == 1;
}

#if VDP_RASTER_RENDER
/* One line in flight: core 1 draws line N while core 0 emulates line
 * N+1. Drawing a line costs about half what emulating one does, so core
 * 1 keeps up and core 0 rarely waits; when core 1 is busy elsewhere
 * (it also feeds I2S and runs the link) core 0 reclaims the line and
 * draws it itself, which is exactly the single-core behaviour.
 *
 * The renderer therefore sees VDP state one line ahead of the beam
 * rather than a whole frame ahead — enough for the mid-frame VRAM
 * rewrites these effects depend on. */
volatile bool vdp_job_busy;

void vdp_render_worker_poll(void) {
    if (!vdp_job_take()) return;
    gwenesis_vdp_render_line(vdp_job_line);
    __dmb();
    vdp_job_done = true;
}

static void vdp_pipeline_sync(void) {
    if (!vdp_job_busy) return;
    if (vdp_job_take()) {
        gwenesis_vdp_render_line(vdp_job_line);   /* core 1 never took it */
    } else {
        while (!vdp_job_done) tight_loop_contents();
    }
    vdp_job_done = false;
    vdp_job_busy = false;
}

static inline void vdp_pipeline_post(int line) {
    vdp_pipeline_sync();
    vdp_job_line = line;
    vdp_job_busy = true;
    __atomic_store_n(&vdp_job_claim, 1, __ATOMIC_RELEASE);
}
#else
void vdp_render_worker_poll(void) {
    if (!vdp_job_take()) return;
    vdp_render_range(vdp_job_from, vdp_job_to, vdp_job_step);
    __dmb();
    vdp_job_done = true;
}
#endif

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
    /* Pump the USB stack while waiting.
     *
     * settings_check_hotkey() refreshes the gamepad and the PS/2 keyboard
     * itself, but the USB keyboard's state only changes when
     * usbhid_task() runs. Without it the key-up for ESC is never
     * processed, so the hotkey reads as held forever and this loop never
     * ends — the menu never appears and the emulator looks hung. C2
     * always builds with USB HID, which is why it shows up there. */
    while (settings_check_hotkey()) {
#ifdef USB_HID_ENABLED
        usbhid_task();
#endif
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
        bool want_settings = settings_check_hotkey();
#ifdef DEBUG_AUTO_SETTINGS
        /* Reproduce the menu entry without a keyboard, so the hang can be
         * caught under the debugger. */
        {
            static bool dbg_fired;
            if (!dbg_fired && frame_counter >= (uint32_t)DEBUG_AUTO_SETTINGS) {
                dbg_fired = true;
                want_settings = true;
            }
        }
#endif
        if (want_settings) {
            // LOCK buttons immediately - no input will reach game until unlocked
            button_lock = true;
            button_state[0] = 0xFF;
            button_state[1] = 0xFF;
            button_state[2] = 0xFF;
            
            // Wait for buttons to be released first (see note above: the
            // USB keyboard needs usbhid_task() pumped or ESC never lifts)
            while (settings_check_hotkey()) {
#ifdef USB_HID_ENABLED
                usbhid_task();
#endif
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
        vdp_palette_frame_begin();      /* base palette is the truth again */
        
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
                sound_z80_run(system_clock + VDP_CYCLES_PER_LINE);
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
            
#if VDP_RASTER_RENDER
            if (render_this_frame && scan_line < screen_height) {
                /* Record the palette in force for this line before the
                 * next line's emulation can split it. */
                if (scan_line < HDMI_MAX_LINES)
                    hdmi_line_lut[scan_line] = vdp_palette_current_lut();
                vdp_pipeline_post(scan_line);
            }
#endif
            scan_line++;
            
            // VBlank
            if (scan_line == screen_height) {
                if (REG1_VBLANK_INTERRUPT != 0) {
                    gwenesis_vdp_status |= STATUS_VIRQPENDING;
                    m68k_set_irq(6);
                }
                // Z80 IRQ for vblank (Z80 runs on Core 0)
                sound_z80_irq(1);
            }
            if (scan_line == screen_height + 1) {
                sound_z80_irq(0);
            }
            
            system_clock += VDP_CYCLES_PER_LINE;
        }
        
        // Generate any remaining audio samples for this frame
        // Fixed 888 samples per NTSC frame (53280 Hz / 60 fps)
        #define TARGET_SAMPLES_PER_FRAME 888
        #define AUDIO_TARGET_CLOCK (TARGET_SAMPLES_PER_FRAME * AUDIO_FREQ_DIVISOR)
        PROFILE_START();
        sound_frame_end(AUDIO_TARGET_CLOCK);

#ifdef C2_LOCAL_SOUND
        {
            extern uint32_t z80_state_signature(void);
            /* Ring, not a prefix: the fault appears tens of seconds in,
             * long after a fixed-size trace from boot has filled up. This
             * always holds the most recent MASTER_SIG_SLOTS frames, and
             * master_sig_count gives the absolute frame number so the two
             * runs can be aligned. */
            if (master_sig_count < MASTER_SIG_SLOTS) {
                extern void z80_state_regs(uint32_t out[6]);
                if (master_sig_count < MASTER_DBG_FRAMES)
                    z80_state_regs(master_dbg[master_sig_count]);
                master_sig[master_sig_count] = z80_state_signature();
#ifdef SOUND_CAPTURE
                snd_cap_z80_src = master_sig[master_sig_count];
                {
                    uint32_t rr[6];
                    z80_state_regs(rr);
                    for (int i = 0; i < 6; i++) snd_cap_reg_src[i] = rr[i];
                }
                {   /* age the modelled mirrors by one frame */
                    extern unsigned char ZRAM[];
                    memcpy(zram_snap2, zram_snap1, sizeof(zram_snap2));
                    memcpy(zram_snap1, ZRAM, sizeof(zram_snap1));
                    memcpy(zram_stale_snap, ZRAM, sizeof(zram_stale_snap));
                }
#endif
            }
            master_sig_count++;
        }
#endif
#ifdef SOUND_CAPTURE
        seam_frame++;
#endif
        PROFILE_END(sound_time);
        
        // ==================================================================
        // PHASE 2: Render the frame AFTER emulation is complete
        // This decouples rendering from emulation timing for stable audio
        // ==================================================================
        if (render_this_frame) {
            PROFILE_START();
            uint64_t render_start_us = time_us_64();
#if VDP_RASTER_RENDER
            vdp_pipeline_sync();        /* retire the last line in flight */
            (void)render_start_us;
#else
#if LINE_INTERLACE
            // Line interlacing: render every other line, then duplicate
            // Alternates between even and odd lines each frame for better quality
            int start_line = (frame_counter & 1);  // 0 or 1
            gwenesis_vdp_render_line(start_line);          /* geometry first */
            {
                int rest = start_line + 2;
                int mid  = rest + (((screen_height - rest) / 2) & ~1);
                vdp_job_from = mid; vdp_job_to = screen_height; vdp_job_step = 2;
                __atomic_store_n(&vdp_job_claim, 1, __ATOMIC_RELEASE);
                vdp_render_range(rest, mid, 2);
                if (vdp_job_take()) {          /* core 1 never got to it */
                    vdp_render_range(vdp_job_from, vdp_job_to, vdp_job_step);
                    __dmb(); vdp_job_done = true;
                }
                while (!vdp_job_done) tight_loop_contents();
                vdp_job_done = false; __dmb();
            }
            // Duplicate rendered lines to adjacent lines (using SCREEN buffer)
            for (int line = start_line; line < screen_height - 1; line += 2) {
                memcpy(SCREEN[line + 1], SCREEN[line], screen_width);
            }
#else
#if VDP_SPLIT_RENDER
            gwenesis_vdp_render_line(0);                   /* geometry first */
            {
                int mid = 1 + (screen_height - 1) / 2;
                vdp_job_from = mid; vdp_job_to = screen_height; vdp_job_step = 1;
                __atomic_store_n(&vdp_job_claim, 1, __ATOMIC_RELEASE);
                vdp_render_range(1, mid, 1);
                if (vdp_job_take()) {          /* core 1 never got to it */
                    vdp_render_range(vdp_job_from, vdp_job_to, vdp_job_step);
                    __dmb(); vdp_job_done = true;
                }
                while (!vdp_job_done) tight_loop_contents();
                vdp_job_done = false; __dmb();
            }
#else
            for (int line = 0; line < screen_height; line++) {
                gwenesis_vdp_render_line(line);
            }
#endif
#endif
#endif
#ifdef SCREEN_VERIFY
            screen_verify_frame();
#endif
            uint32_t render_us = (uint32_t)(time_us_64() - render_start_us);
            PROFILE_END(vdp_time);

#if ENABLE_ADAPTIVE_FRAMESKIP
            // EMA update (1/8 smoothing). Keep a non-zero estimate.
            if (render_cost_ema_us == 0) render_cost_ema_us = render_us ? render_us : FRAMESKIP_RENDER_COST_DEFAULT_US;
            else render_cost_ema_us = (render_cost_ema_us * 7u + (render_us ? render_us : render_cost_ema_us)) / 8u;
#endif
        }
        
        {   /* per-frame peak of mid-frame palette changes */
            extern uint32_t cram_changes_this_frame, cram_changes_max;
            extern uint32_t cram_splits_this_frame, cram_splits_max;
            extern int cram_last_line;
            if (cram_changes_this_frame > cram_changes_max)
                cram_changes_max = cram_changes_this_frame;
            if (cram_splits_this_frame > cram_splits_max)
                cram_splits_max = cram_splits_this_frame;
            cram_changes_this_frame = 0;
            cram_splits_this_frame = 0;
            cram_last_line = -1;
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
        
#ifndef USE_SOUND_LINK
        // Set read buffer pointers for Core 1 (current write buffer becomes read buffer)
        audio_read_sn76489 = gwenesis_sn76489_buffer;
        audio_read_ym2612 = gwenesis_ym2612_buffer;
#endif
        
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

#ifdef USE_SOUND_LINK
    // Bring the inter-processor link up early: it only claims PIO2 and
    // two DMA channels, and doing it before the slave is probed means a
    // slave that boots late still finds a working wire waiting.
    link_master_init();
    {
        link_node_info_t slave_info;
        // Short patience here — a missing slave must cost a moment, not
        // two seconds of a stalled boot. The master falls back to
        // running the sound chips itself.
        if (link_master_probe(200000, &slave_info)) {
            LOG("Link: slave up, %lu MHz, PSRAM %lu MB, fw %u.%u\n",
                (unsigned long)(slave_info.sys_clk_hz / 1000000),
                (unsigned long)(slave_info.psram_bytes >> 20),
                slave_info.fw_version >> 8, slave_info.fw_version & 0xFF);
        } else {
            LOG("Link: DOWN - no slave, sound runs on the master\n");
        }
    }
#endif

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

    // Welcome splash before we touch the SD card — gives the capture card /
    // HDMI a moment to settle and lets the user see we're alive.
    welcome_screen_show((uint8_t *)SCREEN);

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
    
#if AUTOBOOT_LAST_ROM
    /* Development aid, off by default: boot straight into the ROM the
     * browser last opened, so a flash-and-test cycle needs no keypress.
     * Only useful when iterating on something that has to be observed
     * while a game runs. */
    /* Validate before trusting it. Saved settings can be corrupt — a
     * garbage browser_file built a garbage path here and left the
     * firmware stuck inside printf with a nonsense length, which looks
     * like a hang with no obvious cause. Fall back to the browser
     * instead of acting on rubbish. */
    bool autoboot_ok = false;
    {
        size_t n = 0;
        while (n < sizeof(g_settings.browser_file) && g_settings.browser_file[n]) n++;
        if (n > 0 && n < sizeof(g_settings.browser_file)) {
            autoboot_ok = true;
            for (size_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)g_settings.browser_file[i];
                if (c < 0x20 || c > 0x7E) { autoboot_ok = false; break; }
            }
        }
        /* The path must be a sane C string too. */
        size_t p = 0;
        while (p < sizeof(g_settings.browser_path) && g_settings.browser_path[p]) p++;
        if (p >= sizeof(g_settings.browser_path)) autoboot_ok = false;
    }

#ifdef AUTOBOOT_PATH
    /* An explicit path wins over the saved browser position: saved
     * settings can be corrupt, and a development aid that depends on
     * them is useless exactly when the board is in a bad state. */
    if (1) {
        snprintf(selected_rom, sizeof(selected_rom), "%s", AUTOBOOT_PATH);
        LOG("Autoboot (fixed): %s\n", selected_rom);
    } else
#endif
    if (autoboot_ok) {
        snprintf(selected_rom, sizeof(selected_rom), "%s%s%s",
                 g_settings.browser_path,
                 g_settings.browser_path[0] &&
                 g_settings.browser_path[strlen(g_settings.browser_path) - 1] != '/'
                     ? "/" : "",
                 g_settings.browser_file);
        LOG("Autoboot: %s\n", selected_rom);
    } else
#endif
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
    
#ifdef USE_SOUND_LINK
    // Hand the ROM to the sound slave. Its Z80 reads the 68K address
    // space through the bank register, so it needs the whole image, not
    // just the driver. The buffer is already byte-swapped and the slave
    // applies the same ^1 on read, so both halves agree.
    //
    // Retry the probe first: the master may have booted before the
    // slave finished its own PSRAM bring-up, and by the time a ROM has
    // been chosen the slave is certainly up.
    for (int attempt = 0; attempt < 5 && !link_master_connected(); attempt++) {
        link_master_probe(200000, NULL);
    }
    LOG("Link: probe -> %s\n", link_master_connected() ? "connected" : "no answer");

    if (link_master_connected()) {
        LOG("Link: uploading %lu KB ROM to slave...\n",
            (unsigned long)(rom_size_bytes >> 10));

        link_sound_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.z80_enabled       = g_settings.z80_enabled;
        cfg.fm_enabled        = g_settings.fm_sound;
        cfg.dac_enabled       = CHANNEL_ENABLED(g_settings.channel_mask, 5);
        cfg.psg_enabled       = CHANNEL_ENABLED(g_settings.channel_mask, 6);
        cfg.channel_mask      = g_settings.channel_mask;
        cfg.samples_per_frame = 888;

        /* Remember it so the link can re-prime a slave that reboots or
         * gets reflashed without the user having to reload the game. */
        extern void sound_link_set_rom(const uint8_t *, uint32_t,
                                       const link_sound_config_t *);
        sound_link_set_rom(ROM_DATA, rom_size_bytes, &cfg);

        if (link_master_upload_rom(ROM_DATA, rom_size_bytes) &&
            link_master_send_config(&cfg)) {
            LOG("Link: slave ready\n");
        } else {
            LOG("Link: slave setup FAILED - no sound this session\n");
        }
    } else {
        LOG("Link: slave down - no sound this session\n");
    }
    sound_link_backend_reset();
#endif

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
