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
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "m68k.h"

#include "ym2612.h"
#include "z80inst.h"
#include "sound_backend.h"
#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_vdp.h"
#include "gwenesis_sn76489.h"
#include "gwenesis_savestate.h"

/* Always optimize bus functions for speed - critical path */
#pragma GCC optimize("Ofast")

#define BUS_DISABLE_LOGGING 1

#if !BUS_DISABLE_LOGGING
#include <stdarg.h>
void bus_log(const char *subs, const char *fmt, ...) {
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
	#define bus_log(...)  do {} while(0)
#endif

// Setup M68k memories ROM & RAM
#if GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0

#include "rom_manager.h"
unsigned char *M68K_RAM=(void *)(uint32_t)(0); // 68K RAM 
#else

// ROM_DATA is a pointer to ROM in PSRAM, set by main.c
unsigned char* ROM_DATA = NULL;
unsigned char M68K_RAM[MAX_RAM_SIZE];    // 68K RAM
#endif

/*******************************************************************************
 * M68K ROM Page Cache - caches hot ROM pages in fast SRAM
 * Uses 60KB (15 x 4KB pages) with direct-mapped addressing
 ******************************************************************************/
#define ROM_CACHE_PAGE_SIZE     4096    /* 4KB per page */
#define ROM_CACHE_PAGE_SHIFT    12      /* log2(4096) */
/* Must be a power of two: the slot index is (page_num & MASK), so with
 * 15 pages the mask was 14, which clears bit 0 — adjacent 4 KB ROM pages
 * then shared a slot and evicted each other, and only 8 of the 15 slots
 * were ever used. Each eviction copies 4 KB out of PSRAM. */
#ifndef ROM_CACHE_NUM_PAGES
/* 8 pages = 32 KB. The broken mask meant only 8 slots were ever
 * reachable, so this is no smaller than what actually shipped, is
 * indexed correctly, and returns 32 KB of SRAM. It measured zero hits
 * and zero misses on Comix Zone — the 68K reaches ROM through the inline
 * FETCH macros — so it is worth removing outright once another game
 * confirms this path is genuinely unused. */
#define ROM_CACHE_NUM_PAGES     8
#endif
#if (ROM_CACHE_NUM_PAGES & (ROM_CACHE_NUM_PAGES - 1)) != 0
#error "ROM_CACHE_NUM_PAGES must be a power of two"
#endif
#define ROM_CACHE_PAGE_MASK     (ROM_CACHE_NUM_PAGES - 1)

/* Hit/miss accounting: a miss copies 4 KB out of PSRAM, so the miss rate
 * is what decides how much of a frame the 68K spends waiting on memory. */
uint32_t rom_cache_hits, rom_cache_misses;

static uint8_t __attribute__((aligned(4))) rom_page_cache[ROM_CACHE_NUM_PAGES][ROM_CACHE_PAGE_SIZE];
static uint32_t rom_cache_tags[ROM_CACHE_NUM_PAGES];  /* Upper address bits for validation */
static uint8_t rom_cache_valid[ROM_CACHE_NUM_PAGES];  /* Valid flags */

/* Initialize ROM cache (call on game load) */
void rom_cache_init(void) {
    for (int i = 0; i < ROM_CACHE_NUM_PAGES; i++) {
        rom_cache_valid[i] = 0;
    }
}

/* Get cached ROM byte - returns from cache or fills cache line */
static inline uint8_t rom_cache_read_8(uint32_t address) {
    uint32_t page_num = address >> ROM_CACHE_PAGE_SHIFT;
    uint32_t cache_slot = page_num & ROM_CACHE_PAGE_MASK;
    uint32_t page_offset = address & (ROM_CACHE_PAGE_SIZE - 1);
    
    /* Check cache hit */
    if (rom_cache_valid[cache_slot] && rom_cache_tags[cache_slot] == page_num) {
        rom_cache_hits++;
        return rom_page_cache[cache_slot][page_offset ^ 1];  /* Byte swap for big-endian */
    }
    rom_cache_misses++;
    
    /* Cache miss - fill the page */
    uint32_t page_base = page_num << ROM_CACHE_PAGE_SHIFT;
    const uint32_t *src = (const uint32_t *)(ROM_DATA + page_base);
    uint32_t *dst = (uint32_t *)rom_page_cache[cache_slot];
    for (int i = 0; i < ROM_CACHE_PAGE_SIZE / 4; i++) {
        dst[i] = src[i];
    }
    rom_cache_tags[cache_slot] = page_num;
    rom_cache_valid[cache_slot] = 1;
    
    return rom_page_cache[cache_slot][page_offset ^ 1];
}

/* Get cached ROM word (16-bit) */
static inline uint16_t rom_cache_read_16(uint32_t address) {
    uint32_t page_num = address >> ROM_CACHE_PAGE_SHIFT;
    uint32_t cache_slot = page_num & ROM_CACHE_PAGE_MASK;
    uint32_t page_offset = address & (ROM_CACHE_PAGE_SIZE - 1);
    
    /* Check cache hit */
    if (rom_cache_valid[cache_slot] && rom_cache_tags[cache_slot] == page_num) {
        rom_cache_hits++;
        return *(uint16_t *)&rom_page_cache[cache_slot][page_offset];
    }
    rom_cache_misses++;
    
    /* Cache miss - fill the page */
    uint32_t page_base = page_num << ROM_CACHE_PAGE_SHIFT;
    const uint32_t *src = (const uint32_t *)(ROM_DATA + page_base);
    uint32_t *dst = (uint32_t *)rom_page_cache[cache_slot];
    for (int i = 0; i < ROM_CACHE_PAGE_SIZE / 4; i++) {
        dst[i] = src[i];
    }
    rom_cache_tags[cache_slot] = page_num;
    rom_cache_valid[cache_slot] = 1;
    
    return *(uint16_t *)&rom_page_cache[cache_slot][page_offset];
}

// Setup Z80 Memory
unsigned char ZRAM[MAX_Z80_RAM_SIZE]; // Z80 RAM
unsigned char TMSS[0x4];
extern unsigned short gwenesis_vdp_status;

// TMSS
int tmss_state = 0;
int tmss_count = 0;

/******************************************************************************
 *
 *   Load a Sega Genesis Cartridge into CPU Memory
 *
 ******************************************************************************/


#if GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0

void load_cartridge()
{
    // Clear all volatile memory
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);

    // Set Z80 Memory as Z80_RAM
    z80_set_memory(ZRAM);

    z80_pulse_reset();
    
    // Initialize ROM cache
    rom_cache_init();

    set_region();

}
#else

void load_cartridge(unsigned char *buffer, size_t size)
{
    // Clear all volatile memory
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);
    memset(ROM_DATA, 0, MAX_ROM_SIZE);

    // Set Z80 Memory as ZRAM
    z80_set_memory(ZRAM);
    z80_pulse_reset();
    
    // Initialize ROM cache
    rom_cache_init();

    // Copy file contents to CPU ROM memory
    memcpy(ROM_DATA, buffer, size);

    #ifdef ROM_SWAP
    bus_log(__FUNCTION__,"--ROM swap mode--");
    for (int i=0; i < size;i+=2 )
    {   
        char z = ROM_DATA[i];
        ROM_DATA[i]=ROM_DATA[i+1];
        ROM_DATA[i+1]=z;
    }
    #endif


    set_region();
}

#endif

/******************************************************************************
 *
 *   Power ON the CPU
 *   Initialize 68K, Z80 and YM2612 Cores
 *
 ******************************************************************************/
void power_on() {
  // Set M68K CPU as original MOTOROLA 68000
  //m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  // Initialize M68K CPU
  m68k_init();
  // Initialize Z80 CPU
  z80_start();
  // Initialize YM2612 chip
  YM2612Init();
  YM2612Config(9);
  // Initialize PSG SN76489 chip
  //CLOCK_NTSC      = 3579545,
  //CLOCK_PAL       = 3546895,
 // CLOCK_NTSC_SMS1 = 3579527

//  if (mode_pal) {
//     gwenesis_SN76489_Init(3546895, GWENESIS_AUDIO_BUFFER_LENGTH_PAL*50,AUDIO_FREQ_DIVISOR, PSG_INTEGRATED);
//   } else{
//     gwenesis_SN76489_Init(3579545, GWENESIS_AUDIO_BUFFER_LENGTH_NTSC*60,AUDIO_FREQ_DIVISOR, PSG_INTEGRATED);
//   }
  
  // Sample rate 22050 Hz: 368 samples/frame * 60 fps
  // Use PSG_INTEGRATED for later Genesis revisions (more common)
  gwenesis_SN76489_Init(3579545, GWENESIS_AUDIO_BUFFER_LENGTH_NTSC*60, AUDIO_FREQ_DIVISOR, PSG_INTEGRATED);

}

/******************************************************************************
 *
 *   Reset the CPU Emulation
 *   Send a pulse reset to 68K, Z80 and YM2612 Cores
 *
 ******************************************************************************/
void reset_emulation() {
  // Send a reset pulse to Z80 CPU
  z80_pulse_reset();
  // Send a reset pulse to Z80 M68K
  m68k_pulse_reset();
  // Send a reset pulse to YM2612 chip
  YM2612ResetChip();
  // Send a reset pulse to SEGA 315-5313 chip
  gwenesis_vdp_reset();
  gwenesis_SN76489_Reset();
}

/******************************************************************************
 *
 *   Set Region
 *   Look at ROM to set console compatible region
 *
 ******************************************************************************/
void set_region()
{    
  /*
    old style : JUE characters
    J : Domestic 60Hz (Asia)
    U : Oversea  60Hz (USA) 
    E : Oversea  50Hz (Europe) 

    new style : 1st character
    bit 0 : +1 Domestic 60Hz (Asia)
    bit 1 : +2 Domestc  50Hz (Asia)
    bit 2:  +4 Oversea  60Hz (USA) 
    bit 3:  +4 Oversea  50Hz (Europe) 
  */

   // extern int mode_pal;

    int country = 0;

    char rom_str[3];

    printf("ROM game  : ");
    for (int j=0; j < 48;j++) printf("%c",(char)FETCH8ROM(0x150+j));
    printf("\n");

    rom_str[0]=FETCH8ROM(0x1F0);
    rom_str[1]=FETCH8ROM(0x1F1);
    rom_str[2]=FETCH8ROM(0x1F2);

    printf("ROM region:%c%c%c (0x%02x 0x%02x 0x%02x)\n", rom_str[0],rom_str[1],rom_str[2],rom_str[0],rom_str[1],rom_str[2]);

    /* from Gens */
    if (!memcmp(rom_str, "eur", 3)) country |= 8;
    else if (!memcmp(rom_str, "EUR", 3)) country |= 8;
    else if (!memcmp(rom_str, "Europe", 3)) country |= 8;
    else if (!memcmp(rom_str, "jap", 3)) country |= 1;
    else if (!memcmp(rom_str, "JAP", 3)) country |= 1;
    else if (!memcmp(rom_str, "usa", 3)) country |= 4;
    else if (!memcmp(rom_str, "USA", 3)) country |= 4;
    else
    {
      int i;
      unsigned char c;

      /* look for each characters */
      for(i = 0; i < 3; i++)
      {
        c = rom_str[i];

        if (c == 'U') country |= 4;
        else if (c == 'E' || c == 'e' ) country |= 8;
        else if (c == 'J' || c == 'j' ) country |= 1;
        else if (c == 'K' || c == 'k' ) country |= 1;
        else if (c < 16) country |= c;
        else if ((c >= '0') && (c <= '9')) country |= c - '0';
        else if ((c >= 'A') && (c <= 'F')) country |= c - 'A' + 10;
      }
    }
    printf("country code=%01x : ",country);
      /* set default console region (USA > EUROPE > JAPAN) */
      /*
      IO REG0	:	MODE 	VMOD 	DISK 	RSV 	VER3 	VER2 	VER1 	VER0
      MODE (R) 	0: Domestic Model
  	            1: Overseas Model
      VMOD (R) 	0: NTSC CPU clock 7.67 MHz
  	            1: PAL CPU clock 7.60 MHz
      */

    /* USA 60Hz*/
    if (country & 4){
      printf("Oversea-NTSC USA 60Hz\n");
      gwenesis_io_set_reg(0, 0x81);
   //   gwenesis_vdp_status &= 0xFFFE;
     // mode_pal = 0;
      return;
    }
    /* EUROPE 50Hz */
    if (country & 8){
      printf("Oversea-PAL Europe 50Hz\n");
      gwenesis_io_set_reg(0, 0xC1);
    //  gwenesis_vdp_status |= 0x1;
      //mode_pal = 1;
      return;
    }
    /* set Asia 60HZ */
    if (country & 1){
      printf("Domestic-NTSC Asia 60Hz\n");
      gwenesis_io_set_reg(0, 0x1);
    //  gwenesis_vdp_status &= 0xFFFE;
      //mode_pal = 0;
      return;
    }
      printf("Oversea-NTSC USA 60Hz no detection>> default mode\n");
      gwenesis_io_set_reg(0, 0x81);
     // gwenesis_vdp_status &= 0xFFFE;
     // mode_pal = 0;

}
/******************************************************************************
 *
 *   Main memory address mapper
 *   Map all main memory region address for CPU program
 *   68K Access to Z80 Memory
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_map_z80_address(unsigned int address) {

  unsigned int range = (address & 0xF000);
  switch (range) {
  case 0:
  case 0x1000:
    return Z80_RAM_ADDR;
  case 0x2000:
  case 0x3000:
    return Z80_RAM_ADDR1K;
  case 0x4000:
    return Z80_YM2612_ADDR;
  case 0x6000:
    return Z80_BANK_ADDR;
  case 0x7000:
    return Z80_SN76489_ADDR;
  default:
    bus_log(__FUNCTION__,"no map Z80 %x",address);
    assert(0);
    return NONE;
  }
}

/******************************************************************************
 *
 *   IO memory address mapper
 *   Map all input/output region address for CPU program
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_map_io_address(unsigned int address)
{
  unsigned int range = (address & 0x1000) ;
  switch (range) {
  case 0:      return IO_CTRL;
  case 0x1000: return Z80_CTRL;
  default:
      // if (address >= 0xa14000 && address < 0xa11404)
      // return (tmss_state == 0) ? TMSS_CTRL : NONE;
      bus_log(__FUNCTION__,"no map io %x",address);

    return NONE;
  }
}

/******************************************************************************
 *
 *   Main memory address mapper
 *   Map all main memory region address for CPU program
 *
 ******************************************************************************/

static inline 
unsigned int gwenesis_bus_map_address(unsigned int address) {
  // Mask address page
  unsigned int range = (address & 0xFF0000) >> 16;

  // Check mask and select memory type
  if (range < 0x80) //        ROM ADDRESS 0x000000 - 0x3FFFFF
    return ROM_ADDR;

  else if (range == 0xA0) // Z80 ADDRESS 0xA00000 - 0xA0FFFF
    return gwenesis_bus_map_z80_address(address);


  else if (range == 0xA1) //                  IO ADDRESS  0xA10000 - 0xA1FFFF
    return gwenesis_bus_map_io_address(address);

  else if (range == 0xC0) // VDP ADDRESS 0xC00000 - 0xDFFFFFF
    return VDP_ADDR;
  else if (range == 0xFF) // RAM ADDRESS 0xE00000 - 0xFFFFFFF
    return RAM_ADDR;
  // If not a valid address return 0
  bus_log(__FUNCTION__,"M68K > ?? unnmap address %x", address);
  //assert(0);
  return NONE;
}
/******************************************************************************
 *
 *   Main read address routine
 *   Write an value to memory mapped on specified address
 *
 ******************************************************************************/
static inline __attribute__((always_inline)) unsigned int gwenesis_bus_read_memory_8(unsigned int address) {
 bus_log(__FUNCTION__,"read8  %x", address);

  /* Fast path: use upper byte to quickly identify memory region */
  unsigned int range = address >> 16;
  
  /* ROM reads (0x000000 - 0x7FFFFF) - most common during gameplay */
  if (range < 0x80) {
    return rom_cache_read_8(address);
  }
  
  /* RAM reads (0xFF0000 - 0xFFFFFF) - second most common */
  if (range == 0xFF) {
    return FETCH8RAM(address);
  }
  
  /* VDP reads (0xC00000 - 0xDFFFFF) */
  if (range == 0xC0) {
    return gwenesis_vdp_read_memory_8(address);
  }

  // Slow path for other addresses
  switch (gwenesis_bus_map_address(address)) {

  case IO_CTRL:
    return gwenesis_io_read_ctrl(address & 0x1F);

  case Z80_CTRL:
    return sound_z80_ctrl_read(address & 0xFFFF);

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    return sound_zram_read(address & 0x1FFF);

  case Z80_YM2612_ADDR:
    return sound_ym_read(m68k_cycles_master());

  case Z80_SN76489_ADDR:
    return 0xff;

  case Z80_BANK_ADDR:
    return 0xff;

  case TMSS_CTRL:
    bus_log(__FUNCTION__,"TMS");
    if (tmss_state == 0)
      return TMSS[address & 0x4];
    return 0xFF;

  default:
     bus_log(__FUNCTION__," default read 8 %x", address);
    return 0x00;
  }
  return 0x00;
}

static inline __attribute__((always_inline)) unsigned int gwenesis_bus_read_memory_16(unsigned int address) {
   bus_log(__FUNCTION__,"read16 %x", address);

  /* Fast path: use upper byte to quickly identify memory region */
  unsigned int range = address >> 16;
  
  /* ROM reads (0x000000 - 0x7FFFFF) - most common during gameplay */
  if (range < 0x80) {
    return rom_cache_read_16(address);
  }
  
  /* RAM reads (0xFF0000 - 0xFFFFFF) - second most common */
  if (range == 0xFF) {
    return FETCH16RAM(address);
  }
  
  /* VDP reads (0xC00000 - 0xDFFFFF) */
  if (range == 0xC0) {
    return gwenesis_vdp_read_memory_16(address);
  }

  // Slow path for other addresses
  switch (gwenesis_bus_map_address(address)) {

  case IO_CTRL:
    return gwenesis_io_read_ctrl(address & 0x1F);

  case Z80_CTRL:
  //  ret_value = z80_read_ctrl(address & 0xFFFF); 
   // return ret_value | ret_value << 8;
    address &=0xFFFF;
        return (sound_z80_ctrl_read(address) << 8) | sound_z80_ctrl_read(address | 1);


  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    return sound_zram_read(address & 0X1FFF) | (sound_zram_read(address & 0X1FFF) << 8);

  case Z80_YM2612_ADDR:
    {
      unsigned int rv = sound_ym_read(m68k_cycles_master());
      return rv | rv << 8;
    }


  case Z80_SN76489_ADDR:
    return 0xff;

  case Z80_BANK_ADDR:
    return 0xff;

  default:
    bus_log(__FUNCTION__,"read mem 16 default %x", address);
    return (gwenesis_bus_read_memory_8(address) << 8) |
           gwenesis_bus_read_memory_8(address + 1);
  }
  return 0x00;
}

/******************************************************************************
 *
 *   Main write address routine
 *   Write an value to memory mapped on specified address
 *
 ******************************************************************************/
static inline __attribute__((always_inline)) void gwenesis_bus_write_memory_8(unsigned int address,
                                              unsigned int value) {
  bus_log(__FUNCTION__,"write8  @%x:%x", address,value);

  /* Fast path for RAM writes (most common during gameplay) */
  unsigned int range = address >> 16;
  if (range == 0xFF) {
    WRITE8RAM(address, value);
    return;
  }

  switch (gwenesis_bus_map_address(address)) {

  case VDP_ADDR:
    gwenesis_vdp_write_memory_16(address & ~1, (value << 8) | value);
    return;

  case RAM_ADDR:
    WRITE8RAM(address, value);
    return;

  case IO_CTRL:
    gwenesis_io_write_ctrl(address & 0x1F, value);
    return;

  case Z80_CTRL:
    sound_z80_ctrl_write(address & 0x1FFF, value);
    return;

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    sound_zram_write(address & 0x1FFF, value);
    return;

  case Z80_YM2612_ADDR:
    bus_log(__FUNCTION__,"CPUZ80PSG8 ,m68kclk= %d", m68k_cycles_master());
    sound_ym_write(address & 0x3, value & 0Xff, m68k_cycles_master());
    static uint32_t ym_bus_writes = 0;
    if ((++ym_bus_writes % 1000) == 0) {
        printf("YM2612 bus writes (8-bit): %lu\n", (unsigned long)ym_bus_writes);
    }
    return;

  case Z80_SN76489_ADDR:
    bus_log(__FUNCTION__,"CPUZ80FM8  ,m68kclk= %d", m68k_cycles_master());
    sound_psg_write(value & 0Xff, m68k_cycles_master());
    static uint32_t sn_bus_writes_8 = 0;
    if ((++sn_bus_writes_8 % 1000) == 0) {
        printf("SN76489 bus writes (8-bit): %lu\n", (unsigned long)sn_bus_writes_8);
    }
    return;

  case Z80_BANK_ADDR:
  //TODO
    return;

  case TMSS_CTRL:

    if (tmss_state == 0) {
      TMSS[address & 0x4] = value;
      tmss_count++;
      if (tmss_count == 4)
        tmss_state = 1;
    }
    return;



  default:
    //printf("write(%x, %x)\n", address, value);
    return;
  }
  return;
}

static inline __attribute__((always_inline)) void gwenesis_bus_write_memory_16(unsigned int address,
                                               unsigned int value) {
  bus_log(__FUNCTION__,"write16  @%x:%x", address,value);

  /* Fast path for RAM writes (most common during gameplay) */
  unsigned int range = address >> 16;
  if (range == 0xFF) {
    WRITE16RAM(address, value);
    return;
  }
  
  /* Fast path for VDP writes (second most common) */
  if (range == 0xC0) {
    gwenesis_vdp_write_memory_16(address, value);
    return;
  }

  switch (gwenesis_bus_map_address(address)) {

  case VDP_ADDR:
    gwenesis_vdp_write_memory_16(address, value);
    return;

  case RAM_ADDR:
    WRITE16RAM(address, value);
    return;

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    /* Must go through the seam like the 8-bit path at write_memory_8:
     * on C2 the Z80's RAM lives on the slave, and a direct store here
     * updates only the master's unused copy, so the byte never reaches
     * the Z80 that actually runs. Expands to the same store on M1/M2. */
    sound_zram_write(address & 0X1FFF, value >> 8);
    return;

  case IO_CTRL:
    gwenesis_io_write_ctrl(address & 0x1F, value);
    return;

  case Z80_CTRL:
    sound_z80_ctrl_write(address & 0xFFFF, value >> 8);
    return;

  case Z80_YM2612_ADDR:
    bus_log(__FUNCTION__,"CZYM16 ,mclk=%d",  m68k_cycles_master());
    sound_ym_write(address & 0x3, value >> 8, m68k_cycles_master());
    static uint32_t ym_bus_writes_16 = 0;
    if ((++ym_bus_writes_16 % 1000) == 0) {
        printf("YM2612 bus writes (16-bit): %lu\n", (unsigned long)ym_bus_writes_16);
    }
    return;

  case Z80_SN76489_ADDR:
    bus_log(__FUNCTION__,"CZSN16 ,mclk=%d", m68k_cycles_master());
    sound_psg_write(value >> 8, m68k_cycles_master());
    static uint32_t sn_bus_writes = 0;
    if ((++sn_bus_writes % 1000) == 0) {
        printf("SN76489 bus writes: %lu\n", (unsigned long)sn_bus_writes);
    }
    return;

  default:
    bus_log(__FUNCTION__,"write mem 16 default %x ", address);
    gwenesis_bus_write_memory_8(address, (value >> 8) & 0xff);
    gwenesis_bus_write_memory_8(address + 1, (value)&0xff);

    return;
  }
  return;
}

/******************************************************************************
 *
 *   68K CPU read address R8
 *   Read an address from memory mapped and return value as byte
 *
 ******************************************************************************/
unsigned int m68k_read_memory_8(unsigned int address)
{
      //  if ((address &  0xFF0000 ) == 0xFF0000) return FETCH8RAM(address);
    return gwenesis_bus_read_memory_8(address);
}

/******************************************************************************
 *
 *   68K CPU read address R16
 *   Read an address from memory mapped and return value as word
 *
 ******************************************************************************/
 unsigned int m68k_read_memory_16(unsigned int address)
{
     //   if ((address &  0xFF0000 ) == 0xFF0000) return FETCH16RAM(address);
    return gwenesis_bus_read_memory_16(address);
}

/******************************************************************************
 *
 *   68K CPU read address R32
 *   Read an address from memory mapped and return value as long
 *
 ******************************************************************************/
 unsigned int m68k_read_memory_32(unsigned int address)
{
  //  if ((address &  0xFF0000 ) == 0xFF0000) return FETCH32RAM(address);
    return (gwenesis_bus_read_memory_16(address) << 16) | gwenesis_bus_read_memory_16(address + 2);
}

/******************************************************************************
 *
 *   68K CPU write address W8
 *   Write an value as byte to memory mapped on specified address
 *
 ******************************************************************************/
void m68k_write_memory_8(unsigned int address, unsigned int value) {
  // if ((address & 0xFF0000) == 0xFF0000) {
  //   WRITE8RAM(address, value);
  //   return;
  // }
  gwenesis_bus_write_memory_8(address, value);
  return;
}

/******************************************************************************
 *
 *   68K CPU write address W16
 *   Write an value as word to memory mapped on specified address
 *
 ******************************************************************************/
void m68k_write_memory_16(unsigned int address, unsigned int value) {
  // if ((address & 0xFF0000) == 0xFF0000) {
  //   WRITE16RAM(address, value);
  //   return;
  // }
  gwenesis_bus_write_memory_16(address, value);
  return;
}
/******************************************************************************
 *
 *   68K CPU write address W32
 *   Write an value as word to memory mapped on specified address
 *
 ******************************************************************************/
void m68k_write_memory_32(unsigned int address, unsigned int value) {

  // if ((address & 0xFF0000) == 0xFF0000) {
  //   WRITE32RAM(address, value);
  //   return;
  // }
  gwenesis_bus_write_memory_16(address, (value >> 16) & 0xffff);
  gwenesis_bus_write_memory_16(address + 2, (value)&0xffff);

  return;
}

unsigned int m68k_read_disassembler_16(unsigned int address)
{
    return m68k_read_memory_16(address);
}
unsigned int m68k_read_disassembler_32(unsigned int address)
{
    return m68k_read_memory_32(address);
}

void gwenesis_bus_save_state() {
  SaveState* state;
  state = saveGwenesisStateOpenForWrite("bus");
  saveGwenesisStateSetBuffer(state, "M68K_RAM", M68K_RAM, MAX_RAM_SIZE);
  saveGwenesisStateSetBuffer(state, "ZRAM", ZRAM, MAX_Z80_RAM_SIZE);
  saveGwenesisStateSetBuffer(state, "TMSS", TMSS, sizeof(TMSS));
  saveGwenesisStateSet(state, "tmss_state", tmss_state);
  saveGwenesisStateSet(state, "tmss_count", tmss_count);
}

void gwenesis_bus_load_state() {
    SaveState* state = saveGwenesisStateOpenForRead("bus");
    saveGwenesisStateGetBuffer(state, "M68K_RAM", M68K_RAM, MAX_RAM_SIZE);
    saveGwenesisStateGetBuffer(state, "ZRAM", ZRAM, MAX_Z80_RAM_SIZE);
    saveGwenesisStateGetBuffer(state, "TMSS", TMSS, sizeof(TMSS));
    tmss_state = saveGwenesisStateGet(state, "tmss_state");
    tmss_count = saveGwenesisStateGet(state, "tmss_count");
}