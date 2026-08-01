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
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "Z80.h"
#include "z80inst.h"
#include "z80_benchmark.h"
#include "m68k.h"
#include "gwenesis_bus.h"
#include "ym2612.h"
#include "gwenesis_sn76489.h"
#include "gwenesis_savestate.h"

/* Set to 1 to use ARM assembly Z80 core, 0 for C core */
#define USE_Z80_ARM_ASM 1

#if USE_Z80_ARM_ASM
#include "z80_arm.h"
#endif

/* Always optimize Z80 functions for speed - critical path */
#pragma GCC optimize("Ofast")

static volatile int bus_ack = 0;
static volatile int reset = 0;
static volatile int reset_once = 0;
volatile int zclk = 0;
static int initialized = 0;

unsigned char *Z80_RAM;

/* Z80 ROM bank cache - 64KB (2 banks) cached in fast SRAM 
 * This allows games that switch between two banks to stay cached */
#define Z80_BANK_CACHE_SIZE 0x8000  /* 32KB per bank */
#define Z80_BANK_CACHE_COUNT 2      /* Number of cached banks */
static uint8_t __attribute__((aligned(4))) z80_bank_cache[Z80_BANK_CACHE_COUNT][Z80_BANK_CACHE_SIZE];
static int z80_bank_cache_tags[Z80_BANK_CACHE_COUNT] = {-1, -1};  /* LRU cache tags */
static int z80_bank_cache_lru = 0;  /* Next slot to replace */

/* Make cpu and current_timeslice globally accessible for assembly optimization */
Z80 cpu;
int current_timeslice = 0;

void ResetZ80(register Z80 *R);

#define Z80_INST_DISABLE_LOGGING 1

#if !Z80_INST_DISABLE_LOGGING
#include <stdarg.h>
void z80_log(const char *subs, const char *fmt, ...) {
  extern int frame_counter;
  extern int scan_line;

  va_list va;

  printf("%06d:%03d :[%s]", frame_counter, scan_line, subs);

  va_start(va, fmt);
  vfprintf(stdout, fmt, va);
  va_end(va);
  printf("\n");
}
#else
	#define z80_log(...)  do {} while(0)
#endif

// Bank register used by Z80 to access M68K Memory space 1 BANK=32KByte
volatile int Z80_BANK;


void z80_start() {
    z80_benchmark_init();
    cpu.IPeriod = 1;
    cpu.ICount = 0;
    cpu.Trace = 0;
    cpu.Trap = 0x0009;
    ResetZ80(&cpu);
    reset=1;
    reset_once=0;
    bus_ack=0;
    zclk=0;
    /* The bank register is part of the Z80 subsystem's reset state.
     *
     * It is BSS, so on the master it happens to be zero for the first
     * game and nobody noticed it is never cleared. The C2 slave does not
     * reboot between ROM loads, so it inherited the previous game's
     * bank: its Z80 then read ROM through the wrong 32 KB window from
     * the very first frame, took a different branch, and diverged —
     * while every register and the cycle count still matched exactly,
     * which is what made this so hard to see. Loading a second game on
     * the master without a reboot has the same latent bug. */
    Z80_BANK = 0;
    z80_bank_cache_tags[0] = -1;  /* Invalidate cache on start */
    z80_bank_cache_tags[1] = -1;
    z80_bank_cache_lru = 0;
}

void z80_pulse_reset() {
  ResetZ80(&cpu);
  z80_bank_cache_tags[0] = -1;  /* Invalidate cache on reset */
  z80_bank_cache_tags[1] = -1;
  z80_bank_cache_lru = 0;
}

/* External Z80 enable flag from main.c */
extern bool z80_enabled;

/* Z80 cycles actually asked for and actually executed, cumulative.
 * Shared by both halves, so master-local and slave-side rates can be
 * compared directly: a slave that executes fewer cycles per frame is a
 * music driver that ticks slower. */
#ifdef SOUND_CAPTURE
uint32_t z80_cycles_req, z80_cycles_exec, z80_run_calls;
#define Z80_COUNT(x) do { x; } while (0)
#else
#define Z80_COUNT(x) do { } while (0)
#endif

void z80_run(int target) {
    Z80_COUNT(z80_run_calls++);

  // Skip Z80 execution if disabled (for performance)
  if (!z80_enabled) {
    zclk = target;  // Advance clock without execution
    return;
  }

  // we are in advance,nothind to do
  current_timeslice = 0;
  if (zclk >= target) {
 // z80_log("z80_skip time","%1d%1d%1d||zclk=%d,tgt=%d",reset_once,bus_ack,reset, zclk, target);
    return;
  }

  current_timeslice = target - zclk;

  /* If we have less than one Z80 cycle worth of time, do nothing and
     accumulate until we can run at least 1 cycle. This avoids calling the
     core with RunCycles=0 on very small sync deltas. */
  if (current_timeslice < Z80_FREQ_DIVISOR) {
    return;
  }

  int rem = 0;
  if ((reset_once == 1) && (bus_ack == 0) && (reset == 0)) {

    /* If Z80 is HALTed and no interrupt is pending, it effectively just burns
       cycles until an interrupt arrives. Fast-forward time without executing. */
    if ((cpu.IFF & IFF_HALT) && (cpu.IRequest == INT_NONE)) {
      zclk = target;
      return;
    }

    int cycles_to_run = current_timeslice / Z80_FREQ_DIVISOR;
    Z80_COUNT(z80_cycles_req += (uint32_t)cycles_to_run);
    Z80_COUNT(z80_cycles_exec += (uint32_t)cycles_to_run);
#if Z80_BENCHMARK
    uint64_t bench_start = z80_benchmark_start();
#endif

   // z80_log("z80_run", "%1d%1d%1d||zclk=%d,tgt=%d",reset_once, bus_ack, reset, zclk, target);
#if USE_Z80_ARM_ASM
    rem = z80_arm_exec(cycles_to_run);
#else
    rem = ExecZ80(&cpu, cycles_to_run);
#endif

#if Z80_BENCHMARK
    z80_benchmark_end(bench_start, cycles_to_run - rem);
#endif
  }

  Z80_COUNT(z80_cycles_exec -= (uint32_t)rem);
  zclk = target - rem * Z80_FREQ_DIVISOR;
}

/* Per-frame signature of the Z80's architectural state.
 *
 * The point is differential testing: the same ROM run with the Z80 on
 * the master (known good) and on the slave must produce the same
 * sequence of these. The first frame where they differ localises the
 * divergence exactly, instead of inferring a cause from how the audio
 * sounds. Registers plus the bank register only — no host pointers or
 * cycle counters, which legitimately differ between the two builds. */
uint32_t z80_state_signature(void) {
    uint32_t h = 2166136261u;
#define SIGMIX(v) do { h ^= (uint32_t)(v); h *= 16777619u; } while (0)
    SIGMIX(cpu.PC.W);  SIGMIX(cpu.SP.W);
    SIGMIX(cpu.AF.W);  SIGMIX(cpu.BC.W);
    SIGMIX(cpu.DE.W);  SIGMIX(cpu.HL.W);
    SIGMIX(cpu.IX.W);  SIGMIX(cpu.IY.W);
    SIGMIX(cpu.AF1.W); SIGMIX(cpu.BC1.W);
    SIGMIX(cpu.DE1.W); SIGMIX(cpu.HL1.W);
    SIGMIX(cpu.IFF);   SIGMIX(cpu.I);
    SIGMIX(Z80_BANK);
#undef SIGMIX
    return h;
}

void z80_state_regs(uint32_t out[6]) {
    out[0] = (uint32_t)cpu.PC.W | ((uint32_t)cpu.SP.W << 16);
    out[1] = (uint32_t)cpu.AF.W | ((uint32_t)cpu.BC.W << 16);
    out[2] = (uint32_t)cpu.DE.W | ((uint32_t)cpu.HL.W << 16);
    out[3] = (uint32_t)cpu.IX.W | ((uint32_t)cpu.IY.W << 16);
    out[4] = (uint32_t)Z80_BANK;
    out[5] = (uint32_t)zclk;
}

void z80_sync(void) {
  /*
  get M68K cycles 
  Execute cycles on z80 to sync with m68K
  */

  z80_run(m68k_cycles_master());
}

void z80_set_memory(unsigned char *buffer)
{
    Z80_RAM = buffer;
    initialized = 1;
}

void z80_write_ctrl(unsigned int address, unsigned int value) {
  z80_sync();

  if (address == 0x1100) // BUSREQ
  {
    // Bus request. Z80 bus on hold.
    if (value) {
      bus_ack = 1;


    // Bus request cancel. Z80 runs.
    } else {
            bus_ack = 0;
    }

  } else if (address == 0x1200) // RESET
  {
    if (value == 0) {
      reset = 1;
    } else {
      z80_pulse_reset();
      reset = 0;
      reset_once = 1;
    }
  }
}

unsigned int z80_read_ctrl(unsigned int address) {

  z80_sync();

  if (address == 0x1100) {

    z80_log(__FUNCTION__,"RUNNING = %d ", bus_ack ? 0 : 1);
    return bus_ack == 1 ? 0 : 1;

  } else if (address == 0x1101) {
    return 0x00;

  } else if (address == 0x1200) {

    z80_log(__FUNCTION__,"RESET = %d ", reset );
    return reset;

  } else if (address == 0x1201) {
    return 0x00;
  }
  return 0xFF;
}

void z80_irq_line(unsigned int value)
{
    if (reset_once == 0) return;

    if (value)
        cpu.IRequest = INT_IRQ;
    else
        cpu.IRequest = INT_NONE;

    z80_log(__FUNCTION__,"Interrupt = %d ", value);

}

#if 0

word z80_get_reg(int reg_i) {
    switch(reg_i) {
        case 0: return cpu.AF.W; break;
        case 1: return cpu.BC.W; break;
        case 2: return cpu.DE.W; break;
        case 3: return cpu.HL.W; break;
        case 4: return cpu.IX.W; break;
        case 5: return cpu.IY.W; break;
        case 6: return cpu.PC.W; break;
        case 7: return cpu.SP.W; break;
    }
}
#endif

/********************************************
 * Z80 Bank
 ********************************************/

/* Invalidate the Z80 bank cache (call on reset or DMA) */
void z80_bank_cache_invalidate(void) {
    z80_bank_cache_tags[0] = -1;
    z80_bank_cache_tags[1] = -1;
    z80_bank_cache_lru = 0;
}

/* Find or allocate a cache slot for the given bank, returns slot index */
static inline int z80_bank_cache_get_slot(int bank) {
    /* Check if already cached */
    if (z80_bank_cache_tags[0] == bank) return 0;
    if (z80_bank_cache_tags[1] == bank) return 1;
    
    /* Cache miss - fill next slot (simple round-robin replacement) */
    extern unsigned char* ROM_DATA;
    unsigned int base_addr = bank << 15;
    
    /* Only cache if within ROM range (< 8MB) */
    if (base_addr < 0x800000) {
        int slot = z80_bank_cache_lru;
        z80_bank_cache_lru = 1 - slot;  /* Toggle 0<->1 */
        
        /* Copy 32KB from ROM (PSRAM) to cache (SRAM) */
        const uint32_t *src = (const uint32_t *)(ROM_DATA + base_addr);
        uint32_t *dst = (uint32_t *)z80_bank_cache[slot];
        for (int i = 0; i < Z80_BANK_CACHE_SIZE / 4; i++) {
            dst[i] = src[i];
        }
        z80_bank_cache_tags[slot] = bank;
        return slot;
    }
    return -1;  /* Not cacheable */
}

unsigned int zbankreg_mem_r8(unsigned int address)
{
      z80_log(__FUNCTION__,"Z80 bank read pointer : %06x", Z80_BANK);

    return Z80_BANK;
}

/* Make this function non-inline and non-static so assembly can call it */
void zbankreg_mem_w8(unsigned int value) {
  Z80_BANK >>= 1;
  Z80_BANK |= (value & 1) << 8;
  z80_log(__FUNCTION__,"Z80 bank points to: %06x", Z80_BANK << 15);
  /* No need to invalidate - 2-way cache handles bank switching */
  return;
}

/* Make these functions non-inline and non-static so assembly can call them */
unsigned int zbank_mem_r8(unsigned int address)
{
    address &= 0x7FFF;
    
    /* Check if we're reading from ROM (bank in ROM range) */
    unsigned int full_addr = address | (Z80_BANK << 15);
    if (full_addr < 0x800000) {
        /* ROM read - use 2-way cache */
        int slot = z80_bank_cache_get_slot(Z80_BANK);
        if (slot >= 0) {
            /* Read from cache with byte swap for big-endian ROM */
            return z80_bank_cache[slot][address ^ 1];
        }
    }
    
    /* Non-ROM access (RAM mirror, etc) - use slow path */
    z80_log(__FUNCTION__,"Z80 bank read: %06x", full_addr);
    return m68k_read_memory_8(full_addr);
}

void zbank_mem_w8(unsigned int address, unsigned int value) {
  address &= 0x7FFF;
  address |= (Z80_BANK << 15);

  z80_log(__FUNCTION__,"Z80 bank write %06x: %02x", address, value);
  m68k_write_memory_8(address, value);

}

// TODO ??
/*
unsigned int zvdp_mem_r8(unsigned int address)
{
    if (address >= 0x7F00 && address < 0x7F20)
        return vdp_mem_r8(address);
    return 0xFF;
}

void zvdp_mem_w8(unsigned int address, unsigned int value)
{
    if (address >= 0x7F00 && address < 0x7F20)
        vdp_mem_w8(address, value);
}

*/

word LoopZ80(register Z80 *R)
{
    return 0;
}

/* 
 * Z80 memory access functions now implemented in assembly (z80_mem_opt.S)
 * for maximum performance. These functions are called thousands of times
 * per frame and are critical bottlenecks.
 */
extern byte RdZ80(register word Addr);
extern void WrZ80(register word Addr, register byte Value);

extern int system_clock;

byte InZ80(register word Port) {return 0;}
void OutZ80(register word Port, register byte Value) {;}
void PatchZ80(register Z80 *R) {;}
void DebugZ80(register Z80 *R) {;}

void gwenesis_z80inst_save_state() {
    SaveState* state;
    state = saveGwenesisStateOpenForWrite("z80inst");
    saveGwenesisStateSetBuffer(state, "cpu", &cpu, sizeof(Z80));
    saveGwenesisStateSet(state, "bus_ack", bus_ack);
    saveGwenesisStateSet(state, "reset", reset);
    saveGwenesisStateSet(state, "reset_once", reset_once);
    saveGwenesisStateSet(state, "zclk", zclk);
    saveGwenesisStateSet(state, "initialized", initialized);
    saveGwenesisStateSet(state, "Z80_BANK", Z80_BANK);
    saveGwenesisStateSet(state, "current_timeslice",current_timeslice);
}

void gwenesis_z80inst_load_state() {
    SaveState* state = saveGwenesisStateOpenForRead("z80inst");
    saveGwenesisStateGetBuffer(state, "cpu", &cpu, sizeof(Z80));
    bus_ack = saveGwenesisStateGet(state, "bus_ack");
    reset = saveGwenesisStateGet(state, "reset");
    reset_once = saveGwenesisStateGet(state, "reset_once");
    zclk = saveGwenesisStateGet(state, "zclk");
    initialized = saveGwenesisStateGet(state, "initialized");
    Z80_BANK = saveGwenesisStateGet(state, "Z80_BANK");
    current_timeslice = saveGwenesisStateGet(state, "current_timeslice");

}

