/*
Genesis-Plus-GX Z80 CPU integration for frank-genesis
Alternative Z80 implementation using the GPX core

*** EXPERIMENTAL - Use Z80_CORE=GPX to enable ***
This is a pure C implementation that is ~31% slower than the default
ARM assembly-optimized core but may be useful for debugging or porting.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
*/

/* Define to enable GPX-specific benchmarking label */
#define USE_Z80_GPX 1

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "z80_gpx.h"
#include "z80inst.h"
#include "z80_benchmark.h"
#include "m68k.h"
#include "gwenesis_bus.h"
#include "ym2612.h"
#include "gwenesis_sn76489.h"
#include "gwenesis_savestate.h"

#pragma GCC optimize("Ofast")

static volatile int bus_ack = 0;
static volatile int reset = 0;
static volatile int reset_once = 0;
volatile int zclk = 0;
static int initialized = 0;

unsigned char *Z80_RAM;

/* Current timeslice for sync */
int current_timeslice = 0;

/* Starting cycle count for timing calculations during execution
 * This needs to be reset when zclk is reset at frame boundaries */
static unsigned int z80_start_cycles = 0;

/* Called from main.c when zclk is reset at frame start */
void z80_reset_timing(void) {
    z80_start_cycles = Z80.cycles;
}

/* IRQ callback - not used for Genesis */
static int irq_callback(int irqline) {
    (void)irqline;
    return 0x38;  /* RST 38h for IM1 */
}

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

/* Memory access functions for GPX Z80 */
static unsigned char z80_gpx_readmem_func(unsigned int address);
static void z80_gpx_writemem_func(unsigned int address, unsigned char data);
static unsigned char z80_gpx_readport_func(unsigned int port);
static void z80_gpx_writeport_func(unsigned int port, unsigned char data);


void z80_start() {
    z80_benchmark_init();
    
    /* Initialize memory access function pointers */
    z80_readmem = z80_gpx_readmem_func;
    z80_writemem = z80_gpx_writemem_func;
    z80_readport = z80_gpx_readport_func;
    z80_writeport = z80_gpx_writeport_func;
    
    /* Set up read map for direct RAM access (optional optimization) */
    /* For now, use callback-based access for simplicity */
    for (int i = 0; i < 64; i++) {
        z80_readmap[i] = NULL;
        z80_writemap[i] = NULL;
    }
    
    z80_gpx_init(NULL, irq_callback);
    z80_gpx_reset();
    
    reset = 1;
    reset_once = 0;
    bus_ack = 0;
    zclk = 0;
    Z80.cycles = 0;
}

void z80_pulse_reset() {
    z80_gpx_reset();
    Z80.cycles = 0;
}

void z80_run(int target) {
    /* We are in advance, nothing to do */
    current_timeslice = 0;
    if (zclk >= target) {
        return;
    }

    current_timeslice = target - zclk;

    /* If we have less than one Z80 cycle worth of time, do nothing */
    if (current_timeslice < Z80_FREQ_DIVISOR) {
        return;
    }

    if ((reset_once == 1) && (bus_ack == 0) && (reset == 0)) {
        /* If Z80 is HALTed and no interrupt is pending, fast-forward */
        if (Z80.halt && !Z80.irq_state) {
            zclk = target;
            return;
        }

        /* GPX Z80 cycle tables are pre-multiplied by 15, so Z80.cycles is in master clock units.
         * We should run for current_timeslice master cycles directly (no division).
         */
        unsigned int master_cycles_to_run = current_timeslice;
        
        /* Save starting cycle count for timing in memory callbacks */
        z80_start_cycles = Z80.cycles;
        unsigned int start_cycles = Z80.cycles;
        
#if Z80_BENCHMARK
        uint64_t bench_start = z80_benchmark_start();
#endif
        
        /* Run the CPU - GPX z80_run runs until cycles >= target (in master clock units) */
        z80_gpx_run(Z80.cycles + master_cycles_to_run);
        
        /* Calculate remaining cycles (in master clock units) */
        unsigned int cycles_run = Z80.cycles - start_cycles;

#if Z80_BENCHMARK
        z80_benchmark_end(bench_start, cycles_run / Z80_FREQ_DIVISOR);  /* Report actual Z80 cycles for benchmark */
#endif

        int rem = (int)master_cycles_to_run - (int)cycles_run;
        if (rem < 0) rem = 0;
        
        zclk = target - rem;
    } else {
        zclk = target;
    }
}

void z80_sync(void) {
    z80_run(m68k_cycles_master());
}

void z80_set_memory(unsigned char *buffer) {
    Z80_RAM = buffer;
    initialized = 1;
    
    /* Set up direct read map for RAM (first 8KB, mirrored to 0x2000) */
    /* Each map entry covers 1KB (64 entries * 1KB = 64KB) */
    for (int i = 0; i < 8; i++) {
        z80_readmap[i] = Z80_RAM + (i * 1024);
    }
    /* Mirror at 0x2000-0x3FFF */
    for (int i = 8; i < 16; i++) {
        z80_readmap[i] = Z80_RAM + ((i - 8) * 1024);
    }
}

void z80_write_ctrl(unsigned int address, unsigned int value) {
    z80_sync();

    if (address == 0x1100) /* BUSREQ */
    {
        if (value) {
            bus_ack = 1;
        } else {
            bus_ack = 0;
        }
    } else if (address == 0x1200) /* RESET */
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
        z80_log(__FUNCTION__,"RESET = %d ", reset);
        return reset;
    } else if (address == 0x1201) {
        return 0x00;
    }
    return 0xFF;
}

void z80_irq_line(unsigned int value) {
    if (reset_once == 0) return;

    if (value) {
        z80_gpx_set_irq_line(ASSERT_LINE);
    } else {
        z80_gpx_set_irq_line(CLEAR_LINE);
    }

    z80_log(__FUNCTION__,"Interrupt = %d ", value);
}

/********************************************
 * Z80 Bank
 ********************************************/

unsigned int zbankreg_mem_r8(unsigned int address) {
    z80_log(__FUNCTION__,"Z80 bank read pointer : %06x", Z80_BANK);
    return Z80_BANK;
}

void zbankreg_mem_w8(unsigned int value) {
    Z80_BANK >>= 1;
    Z80_BANK |= (value & 1) << 8;
    z80_log(__FUNCTION__,"Z80 bank points to: %06x", Z80_BANK << 15);
}

unsigned int zbank_mem_r8(unsigned int address) {
    address &= 0x7FFF;
    address |= (Z80_BANK << 15);
    z80_log(__FUNCTION__,"Z80 bank read: %06x", address);
    return m68k_read_memory_8(address);
}

void zbank_mem_w8(unsigned int address, unsigned int value) {
    address &= 0x7FFF;
    address |= (Z80_BANK << 15);
    z80_log(__FUNCTION__,"Z80 bank write %06x: %02x", address, value);
    m68k_write_memory_8(address, value);
}

/********************************************
 * Memory access functions for GPX Z80
 ********************************************/

/********************************************
 * Helper to calculate current Z80 timing in master clock cycles
 * Formula matches OLD Z80: zclk + (cycles_executed_in_timeslice * Z80_FREQ_DIVISOR)
 * 
 * current_timeslice = total master cycles we should run this z80_run call
 * z80_start_cycles = Z80.cycles value at start of z80_run
 * Z80.cycles - z80_start_cycles = Z80 cycles executed so far in this timeslice
 ********************************************/

static inline int z80_get_current_timing(void) {
    /* GPX Z80 cycle tables are already pre-multiplied by 15 (Z80_FREQ_DIVISOR)
     * So Z80.cycles is already in master clock units - don't multiply again!
     */
    unsigned int master_cycles_executed = Z80.cycles - z80_start_cycles;
    return zclk + (int)master_cycles_executed;
}

static unsigned char z80_gpx_readmem_func(unsigned int address) {
    address &= 0xFFFF;
    
    /* 0x0000-0x1FFF: Z80 RAM (8KB) */
    if (address < 0x2000) {
        return Z80_RAM[address];
    }
    
    /* 0x2000-0x3FFF: Z80 RAM mirror */
    if (address < 0x4000) {
        return Z80_RAM[address & 0x1FFF];
    }
    
    /* 0x4000-0x5FFF: YM2612 */
    if (address < 0x6000) {
        return YM2612Read(z80_get_current_timing());
    }
    
    /* 0x6000-0x60FF: Bank register */
    if (address >= 0x6000 && address < 0x6100) {
        return zbankreg_mem_r8(address);
    }
    
    /* 0x7F00-0x7FFF: VDP (only in this specific range, not 0x8000+) */
    if (address >= 0x7F00 && address < 0x8000) {
        return 0xFF; /* VDP not implemented for Z80 */
    }
    
    /* 0x8000-0xFFFF: M68K banked memory */
    if (address >= 0x8000) {
        return zbank_mem_r8(address);
    }
    
    return 0xFF;
}

static void z80_gpx_writemem_func(unsigned int address, unsigned char data) {
    address &= 0xFFFF;
    
    /* 0x0000-0x1FFF: Z80 RAM (8KB) */
    if (address < 0x2000) {
        Z80_RAM[address] = data;
        return;
    }
    
    /* 0x2000-0x3FFF: Z80 RAM mirror */
    if (address < 0x4000) {
        Z80_RAM[address & 0x1FFF] = data;
        return;
    }
    
    /* 0x4000-0x5FFF: YM2612 */
    if (address < 0x6000) {
        YM2612Write(address & 3, data, z80_get_current_timing());
        return;
    }
    
    /* 0x6000-0x60FF: Bank register */
    if (address >= 0x6000 && address < 0x6100) {
        zbankreg_mem_w8(data);
        return;
    }
    
    /* 0x7F00-0x7F1F: PSG */
    if (address >= 0x7F00 && address < 0x7F20) {
        gwenesis_SN76489_Write(data, z80_get_current_timing());
        return;
    }
    
    /* 0x8000-0xFFFF: M68K banked memory */
    if (address >= 0x8000) {
        zbank_mem_w8(address, data);
        return;
    }
}

static unsigned char z80_gpx_readport_func(unsigned int port) {
    (void)port;
    return 0xFF;
}

static void z80_gpx_writeport_func(unsigned int port, unsigned char data) {
    (void)port;
    (void)data;
}

/********************************************
 * Save State
 ********************************************/

void gwenesis_z80inst_save_state() {
    SaveState* state;
    state = saveGwenesisStateOpenForWrite("z80inst");
    saveGwenesisStateSetBuffer(state, "cpu", &Z80, sizeof(Z80_Regs));
    saveGwenesisStateSet(state, "bus_ack", bus_ack);
    saveGwenesisStateSet(state, "reset", reset);
    saveGwenesisStateSet(state, "reset_once", reset_once);
    saveGwenesisStateSet(state, "zclk", zclk);
    saveGwenesisStateSet(state, "initialized", initialized);
    saveGwenesisStateSet(state, "Z80_BANK", Z80_BANK);
    saveGwenesisStateSet(state, "current_timeslice", current_timeslice);
}

void gwenesis_z80inst_load_state() {
    SaveState* state = saveGwenesisStateOpenForRead("z80inst");
    saveGwenesisStateGetBuffer(state, "cpu", &Z80, sizeof(Z80_Regs));
    bus_ack = saveGwenesisStateGet(state, "bus_ack");
    reset = saveGwenesisStateGet(state, "reset");
    reset_once = saveGwenesisStateGet(state, "reset_once");
    zclk = saveGwenesisStateGet(state, "zclk");
    initialized = saveGwenesisStateGet(state, "initialized");
    Z80_BANK = saveGwenesisStateGet(state, "Z80_BANK");
    current_timeslice = saveGwenesisStateGet(state, "current_timeslice");
}

/********************************************
 * Memory access for M68K (when accessing Z80 space)
 ********************************************/

void z80_write_memory_8(unsigned int address, unsigned int value) {
    z80_sync();
    address &= 0x1FFF;
    Z80_RAM[address] = value;
}

void z80_write_memory_16(unsigned int address, unsigned int value) {
    z80_sync();
    address &= 0x1FFF;
    Z80_RAM[address] = (value >> 8) & 0xFF;
    Z80_RAM[address + 1] = value & 0xFF;
}

unsigned int z80_read_memory_8(unsigned int address) {
    z80_sync();
    address &= 0x1FFF;
    return Z80_RAM[address];
}

unsigned int z80_read_memory_16(unsigned int address) {
    z80_sync();
    address &= 0x1FFF;
    return (Z80_RAM[address] << 8) | Z80_RAM[address + 1];
}
