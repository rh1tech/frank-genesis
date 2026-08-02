/*
 * frank-genesis — C2 inter-processor sound link
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * link_proto.h — wire protocol between the C2 master (68K, VDP, video,
 * I2S) and the C2 slave (Z80, YM2612, SN76489, mixer).
 *
 * Control frames use the doorbell handshake from link_bus.h:
 *
 *   master                              slave
 *   ------                              -----
 *   DB_MS = 1  ------------------------>  sees DB_MS, arms RX
 *   waits for DB_SM  <-----------------   DB_SM = 1  ("armed")
 *   VALID_A=1, stream, VALID_A=0
 *   DB_MS = 0  ------------------------>  RX DMA completes
 *                    <-----------------   DB_SM = 0
 *
 * Nothing depends on the two chips agreeing about absolute time, so the
 * slave can boot seconds after the master and still join cleanly.
 *
 * The steady-state exchange is one LINK_OP_FRAME per emulated frame,
 * carrying that frame's event stream one way and the *previous* frame's
 * audio plus a Z80 RAM snapshot the other. See docs/C2_SOUND_SPLIT.md
 * for why a frame of lag is safe.
 */
#ifndef LINK_PROTO_H
#define LINK_PROTO_H

#include <stdint.h>

#define LINK_MAGIC        0x53474E47u   /* "GNGS" — genesis sound */
#define LINK_PROTO_VER    1u

/* Fixed control-frame size. Must be a multiple of 4 (PIO autopush is
 * 32-bit) and large enough for the biggest payload struct below. */
#define LINK_CTRL_BYTES   128u

/* ---- Opcodes ---- */
enum {
    LINK_OP_HELLO        = 0x0001,  /* M->S: are you there?               */
    LINK_OP_HELLO_ACK    = 0x0002,  /* S->M: payload link_node_info_t     */

    LINK_OP_RESET        = 0x0010,  /* M->S: reset Z80 + sound chips      */
    LINK_OP_RESET_ACK    = 0x0011,

    LINK_OP_CONFIG       = 0x0012,  /* M->S: payload link_sound_config_t  */
    LINK_OP_CONFIG_ACK   = 0x0013,

    /* ROM upload. arg0 = byte offset, arg1 = chunk length. The chunk
     * follows as a bulk transfer; the slave writes it into PSRAM. */
    LINK_OP_ROM_BEGIN    = 0x0020,  /* M->S: arg0 = total ROM bytes       */
    LINK_OP_ROM_BEGIN_ACK= 0x0021,
    LINK_OP_ROM_CHUNK    = 0x0022,  /* M->S: header then bulk data        */
    LINK_OP_ROM_CHUNK_ACK= 0x0023,
    LINK_OP_ROM_END      = 0x0024,  /* M->S: arg0 = CRC-32 of whole ROM   */
    LINK_OP_ROM_END_ACK  = 0x0025,  /* S->M: arg0 = CRC the slave saw     */

    /* Steady state. arg0 = event count, arg1 = frame sequence number. */
    LINK_OP_FRAME        = 0x0030,  /* M->S: header, then the event bulk  */
    LINK_OP_FRAME_ACK    = 0x0031,  /* S->M: payload link_frame_reply_t,
                                     *       then audio + ZRAM bulk       */

    /* Sent immediately after LINK_OP_FRAME. arg0 = 1 when a Z80 RAM
     * block follows (dirty bitmap then the full 8 KB); 0 when nothing
     * changed. 68K writes to Z80 RAM are a bulk memcpy during driver
     * upload, not a stream of register pokes, so they travel as a block
     * rather than as thousands of events. */
    LINK_OP_ZRAM_BLOCK   = 0x0032,

    /* Mid-frame synchronisation.
     *
     * The 68K reads Z80 RAM to see what the sound driver has answered.
     * On one chip it sees the Z80's writes as of the last slice the Z80
     * was run for, within the same frame. Batching a whole frame to the
     * slave cannot reproduce that: the slave only replays frame N after
     * the master has finished it, so its Z80 writes arrive a frame late
     * and the 68K spins on a byte that never changes.
     *
     * SYNC ships the events emitted so far this frame and asks the slave
     * to replay them now; the ack carries the requested Z80 RAM byte as
     * of that point. arg0 = event count to follow, arg1 = byte offset.
     * Measured at ~0.3 reads per frame, so this is rare. */
/* Set in a SYNC's arg1 when the master wants the events replayed but no
 * value back. The slave acknowledges first and replays afterwards, so
 * the master is not held up by the slave's Z80 catching up — which on a
 * DAC-heavy game was 3.4 ms of every frame. */
#define LINK_SYNC_NO_PEEK 0x00010000u

    LINK_OP_SYNC         = 0x0034,
    LINK_OP_SYNC_ACK     = 0x0035,  /* S->M: arg0 = the byte             */

    LINK_OP_PING         = 0x0040,  /* M->S: liveness / latency probe     */
    LINK_OP_PONG         = 0x0041,
};

/* ---- Frame header (24 bytes), followed by payload, zero-padded to
 *      LINK_CTRL_BYTES. ---- */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t op;
    uint16_t proto_ver;
    uint32_t seq;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t crc;      /* CRC-32 of the whole frame with crc treated as 0 */
} link_hdr_t;

#define LINK_PAYLOAD_BYTES (LINK_CTRL_BYTES - sizeof(link_hdr_t))

/* =====================================================================
 * The event stream
 *
 * Every master-side write that reaches the sound subsystem becomes one
 * of these, tagged with the 68K master cycle count at which it happened.
 * The slave replays them in timestamp order, which is what makes running
 * a frame late equivalent to running inline — the chips see the same
 * writes at the same emulated times either way.
 *
 * 8 bytes each. At a worst-case ~2000 events per frame that is 16 KB,
 * against a 48 MB/s link.
 * ===================================================================== */
enum {
    LINK_EV_YM_WRITE     = 0x01,  /* addr = port (0..3), val = data       */
    LINK_EV_PSG_WRITE    = 0x02,  /* val = data                           */
    LINK_EV_ZRAM_WRITE   = 0x03,  /* addr = offset & 0x1FFF, val = data   */
    LINK_EV_BANK_WRITE   = 0x04,  /* val = bank register bit              */
    LINK_EV_BUSREQ       = 0x05,  /* val = 1 request, 0 release           */
    LINK_EV_RESET_LINE   = 0x06,  /* val = 1 released, 0 asserted         */
    LINK_EV_IRQ          = 0x07,  /* val = 1 assert, 0 clear              */
    LINK_EV_RUN_UNTIL    = 0x08,  /* time marker: run the Z80 to `cycles` */
    LINK_EV_ZRAM_APPLY   = 0x09,  /* apply the frame's ZRAM block here.
                                   * Emitted at the cycle of the first
                                   * write that overflowed the event
                                   * budget, so the block's bytes land
                                   * at the point in the frame they were
                                   * actually written rather than before
                                   * the Z80 has run at all.            */
};

typedef struct __attribute__((packed)) {
    uint32_t cycles;   /* m68k_cycles_master() when this happened */
    uint8_t  type;     /* LINK_EV_*                               */
    uint8_t  val;
    uint16_t addr;
} link_event_t;

/* ---- Payload: who am I ---- */
typedef struct __attribute__((packed)) {
    uint8_t  chip_id[8];
    uint8_t  package_is_a;     /* 1 = QFN-60 (RP2350A) — the slave  */
    uint8_t  rp2350_rev;
    uint16_t fw_version;       /* (major << 8) | minor              */
    uint32_t sys_clk_hz;
    uint32_t psram_bytes;      /* 0 if the probe failed             */
    uint32_t proto_ver;
} link_node_info_t;

/* ---- Payload: emulation settings the slave needs ----
 *
 * Mirrors the master's settings so muting and the Z80 enable flag behave
 * identically on both halves. Sent on ROM load and whenever settings
 * change. */
typedef struct __attribute__((packed)) {
    uint8_t  z80_enabled;
    uint8_t  fm_enabled;
    uint8_t  dac_enabled;
    uint8_t  psg_enabled;
    uint8_t  channel_mask;     /* bit per FM channel, as g_settings   */
    uint8_t  region_pal;
    uint16_t reserved;
    uint32_t samples_per_frame;  /* 888 for NTSC                      */
    uint32_t audio_freq_divisor;
} link_sound_config_t;

/* ---- Payload: what came back with LINK_OP_FRAME_ACK ----
 *
 * The slave returns the two chip buffers raw and mono, exactly as the
 * chips rendered them, rather than a mixed stereo frame.
 *
 * Mixing on the slave looked cheaper but is wrong here: audio.c's
 * audio_submit() mixes, applies master_volume, time-stretches the frame
 * to 888 samples against the *measured* wall-clock interval between
 * submissions, duplicates to stereo and applies the startup mute. All of
 * that is master-side state — the slave has no idea how long the
 * master's last frame took. Returning raw buffers leaves audio.c
 * untouched and costs the same bytes on the wire. */
typedef struct __attribute__((packed)) {
    uint32_t seq;              /* which frame these samples belong to  */
    uint32_t ym_samples;       /* mono int16 samples that follow       */
    uint32_t sn_samples;       /* mono int16 samples, after the YM set  */
    uint32_t zram_bytes;       /* 8192, or 0 if unchanged this frame   */
    uint32_t z80_cycles;       /* how far the slave's Z80 actually ran */
    uint32_t foreign_reads;    /* Z80 reads into 68K space we can't serve */
    uint32_t foreign_writes;
    uint32_t overflows;        /* frames whose event list was truncated */
    uint32_t ym_status;        /* the real chip's status byte, for auditing
                                * the master's timer shadow against it   */
    /* Where the Z80 reached outside anything the slave can serve. The
     * region decides the fix: 68K work RAM wants a mirror, I/O or VDP
     * would want something else entirely. */
    /* Packed Z80 registers for the differential test: pc|sp, af|bc,
     * de|hl, ix|iy, bank, zclk. A hash says the two diverged; this says
     * which register did. */
    uint32_t z80_dbg[6];
    uint32_t z80_sig;          /* per-frame Z80 state signature, for the
                                * differential test against the master  */
    uint32_t foreign_last_addr;
    uint32_t foreign_min_addr;
    uint32_t foreign_max_addr;
} link_frame_reply_t;

/* Z80 RAM is 8 KB; the snapshot is the whole thing. */
#define LINK_ZRAM_BYTES     8192u

/* NTSC is 888 samples per frame per chip. The bulk audio payload is two
 * mono int16 buffers, YM first then PSG. */
#define LINK_MAX_SAMPLES    1024u
#define LINK_AUDIO_BYTES    (LINK_MAX_SAMPLES * 2u * sizeof(int16_t))

/* Upper bound on events in one frame. A frame that would exceed this is
 * truncated rather than dropped, and the overflow is counted — losing
 * the tail of a frame's writes degrades sound, whereas losing frame
 * alignment desynchronises the whole stream. */
#define LINK_MAX_EVENTS     4096u
#define LINK_EVENT_BYTES    (LINK_MAX_EVENTS * sizeof(link_event_t))

/* ROM upload chunk.
 *
 * 16 KiB, not the Z80 bank size of 32 KiB: both halves now stage chunks
 * through SRAM rather than letting DMA touch the PSRAM XIP window, and
 * the master cannot spare 32 KiB of static buffer on top of the
 * emulator's own footprint. Only affects how many handshakes a one-time
 * upload costs. */
#define LINK_ROM_CHUNK_BYTES (16u * 1024u)

/* The PIO FIFOs are 32 bits wide and autopull/autopush are word-sized,
 * so every bulk length must be a whole number of words. Sample counts
 * are not naturally aligned, so both sides round the same way from the
 * same count in the reply and stay in step. Buffers therefore need two
 * bytes of slack past the largest count they will ever carry. */
#define LINK_ALIGN4(n) (((n) + 3u) & ~3u)

/* ---- Helpers shared by both firmwares ---- */
uint32_t link_crc32(const void *data, uint32_t len);

/* Build a control frame into `frame` (must be LINK_CTRL_BYTES, 4-byte
 * aligned). Copies `payload_len` bytes of payload and fills in the CRC. */
void link_frame_build(void *frame, uint16_t op, uint32_t seq,
                      uint32_t arg0, uint32_t arg1,
                      const void *payload, uint32_t payload_len);

/* Validate magic, version and CRC. Returns 1 on success. */
int link_frame_check(const void *frame);

#endif /* LINK_PROTO_H */
