/*
 * frank-genesis — C2 slave: emulator glue
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The sound cores (z80inst.c, ym2612.c, gwenesis_sn76489.c and the Z80
 * assembly) are compiled unmodified from src/. On the master they sit
 * beside a 68K, a VDP and main.c, which own the globals they reach for.
 * The slave has none of those, so this file supplies the same names with
 * slave-side meanings.
 *
 * Getting these wrong is silent: the cores would compile and run, and
 * produce subtly wrong audio. Each one is therefore spelled out.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "slave_sound.h"

/* =====================================================================
 * Time
 *
 * The sound cores ask "what time is it?" through m68k_cycles_master() so
 * they can catch up their synthesis to the moment of a register write.
 * On the slave that answer is the timestamp of the event currently being
 * replayed — which is exactly what makes running a frame late equivalent
 * to running inline. Everything else follows from this one substitution.
 * ===================================================================== */
int slave_replay_clock = 0;

int m68k_cycles_master(void) {
    return slave_replay_clock;
}

/* main.c owns these on the master; the VDP drives them. Nothing on the
 * slave renders, so they exist only because the sound cores log with
 * them and z80inst.c reads scan_line in its debug path. */
int system_clock = 0;
int scan_line    = 0;
int frame_counter = 0;

/* =====================================================================
 * ROM
 *
 * zbank_mem_r8() reads the 68K address space through the Z80's bank
 * register, which is why the slave needs the whole ROM rather than just
 * the sound driver. The image is uploaded once per game into PSRAM and
 * ROM_DATA points at it; the 2x32 KB SRAM bank cache in z80inst.c then
 * works exactly as it does on the master.
 * ===================================================================== */
unsigned char *ROM_DATA = NULL;
static uint32_t rom_bytes = 0;

void slave_rom_set(unsigned char *base, uint32_t bytes) {
    ROM_DATA  = base;
    rom_bytes = bytes;
}

/* Counters for accesses the slave cannot serve, reported back to the
 * master so a game that depends on them is diagnosable rather than
 * merely quiet. */
uint32_t slave_foreign_reads  = 0;
uint32_t slave_foreign_writes = 0;

/* The Z80 reaching into 68K address space through its bank register.
 *
 * ROM is served from PSRAM. Anything else — chiefly 68K work RAM at
 * 0xE00000 — lives on the master and is not reachable from here. Open
 * bus (0xFF) is what an unmapped Genesis read yields, so returning it
 * is at least the same shape of wrong, and the counter makes it
 * visible. Sound drivers overwhelmingly use Z80 RAM plus ROM, which is
 * why this is a counter and not a link round trip. */
unsigned int m68k_read_memory_8(unsigned int address) {
    if (ROM_DATA && address < rom_bytes) {
        /* ROM is stored big-endian, as on the master. */
        return ROM_DATA[address ^ 1];
    }
    slave_foreign_reads++;
    return 0xFF;
}

void m68k_write_memory_8(unsigned int address, unsigned int value) {
    (void)address;
    (void)value;
    slave_foreign_writes++;
}

/* =====================================================================
 * Audio buffers
 *
 * Same shape as main.c's: the chips render into these and the frame
 * assembler mixes them. Single-buffered here — the slave hands the
 * mixed result straight to the link and starts the next frame clean,
 * so there is no second consumer to double-buffer against.
 * ===================================================================== */
int16_t slave_sn76489_buffer_mem[SLAVE_AUDIO_BUFFER_SIZE];
int16_t slave_ym2612_buffer_mem[SLAVE_AUDIO_BUFFER_SIZE];

int16_t *gwenesis_sn76489_buffer = slave_sn76489_buffer_mem;
int16_t *gwenesis_ym2612_buffer  = slave_ym2612_buffer_mem;

volatile int sn76489_index = 0;
volatile int sn76489_clock = 0;
volatile int ym2612_index  = 0;
volatile int ym2612_clock  = 0;

/* z80inst.c gates execution on this. The per-chip mute flags it does
 * *not* need — ym2612_fm_enabled and friends live inside ym2612.c and
 * come across in the config message — and the master's own
 * ym2612_enabled / sn76489_enabled gates stay on the master, where they
 * already sit in front of the code that emits the events. */
bool z80_enabled = true;
