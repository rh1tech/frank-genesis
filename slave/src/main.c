/*
 * frank-genesis — C2 slave firmware
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs the Genesis sound subsystem — Z80, YM2612, SN76489 — on the
 * second RP2350 of a FRANK Core 2 board, driven entirely by an event
 * stream from the master. See docs/C2_SOUND_SPLIT.md.
 *
 * There is no video, no SD and no user input here. The only things this
 * firmware talks to are the link and its own PSRAM.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/runtime_init.h"
#include "hardware/structs/watchdog.h"

#include "link_bus.h"
#include "link_pins.h"
#include "link_proto.h"
#include "link_session.h"
#include "psram_init.h"

#include "slave_sound.h"

#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 252
#endif
#ifndef CPU_VOLTAGE
#define CPU_VOLTAGE VREG_VOLTAGE_1_50
#endif
#ifndef SLAVE_FW_VERSION
#define SLAVE_FW_VERSION 0x0100   /* 1.0 */
#endif

/* Flash QMI timing for overclocked operation.
 *
 * Copied from the master's main.c, and not optional: the XIP flash
 * divider is derived from the system clock, so raising the core to
 * 504 MHz without widening it runs the QSPI far out of spec and the
 * slave faults before it reaches main(). Omitting this is what turned
 * "match the two clocks" into a slave that would not boot at all.
 */
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

/* PSRAM is mapped at the XIP CS1 window; the ROM image lives at its
 * base. 8 MB covers every Genesis ROM. */
#define PSRAM_BASE   ((unsigned char *)0x11000000u)
#define PSRAM_BYTES  (8u * 1024u * 1024u)

/* ---- Link state ---- */
static uint8_t __attribute__((aligned(4))) ctrl_tx[LINK_CTRL_BYTES];
static uint8_t __attribute__((aligned(4))) ctrl_rx[LINK_CTRL_BYTES];
static link_t         link;
static link_session_t session;

/* Event landing area. The master truncates rather than exceeding this,
 * so a frame is never split across exchanges. */
static link_event_t __attribute__((aligned(4))) events[LINK_MAX_EVENTS];

/* ROM upload staging. Chunks land here and are copied into PSRAM: DMA
 * straight into the PSRAM XIP window would mix link traffic with QMI
 * refills on the same bus, and a 32 KiB bounce is cheap. */
static uint8_t __attribute__((aligned(4))) rom_chunk[LINK_ROM_CHUNK_BYTES];

/* Step counters, so a failed upload says which step failed rather than
 * just leaving rom_received_bytes at zero. */
uint32_t n_hello, n_rom_begin, n_rom_chunk, n_rom_chunk_bulkfail;
uint32_t n_rom_end, n_frame, n_zram_block, n_zram_bulkfail, n_ev_bulkfail;
uint32_t last_chunk_off, last_chunk_len;

static uint32_t rom_expected_bytes;
static uint32_t rom_received_bytes;
static uint32_t frame_overflows;

/* ---- Heartbeat ---- */
static volatile uint32_t heartbeat_period_ms = 1000;
static bool     fs_reset_requested;
static uint64_t hb_next_toggle_us;
static bool     hb_level;

/* Heartbeat and FS sampling from the serve loop rather than a timer
 * interrupt.
 *
 * The interrupt version faulted: GCC compiles pico_time's 64-bit
 * timestamp maths onto VFP registers, so the SDK's alarm handler starts
 * with `vpush {d8}`. If an alarm ever fires while the core is early in
 * boot — after crt0 but before the runtime enables CP10/CP11 — that
 * instruction takes a UsageFault (NOCP) and escalates to a HardFault.
 * With no alarms in flight there is no such window, and the serve loop
 * runs at least once a second because link_s_wait_ctrl times out.
 */
static void heartbeat_tick(void) {
    uint64_t now = time_us_64();
    if (now >= hb_next_toggle_us) {
        uint32_t period = heartbeat_period_ms;
        if (period < 50) period = 50;
        hb_next_toggle_us = now + (uint64_t)period * 500u;  /* half period */
        hb_level = !hb_level;
        gpio_put(S_LED_PIN, hb_level);
    }

    if (link_fs_get(&link)) fs_reset_requested = true;
}

/* Boot accounting, kept in watchdog scratch so it survives a reset and
 * can tell an unexpected reboot from a fault that never rebooted. */
#define BOOT_MAGIC 0x43325342u   /* "C2SB" */

static uint32_t boot_count;
static uint32_t cpacr_reasserts;

/* Coprocessor access this firmware depends on: CP0 is the GPIO
 * coprocessor that gpio_put() compiles to on RP2350, CP10/CP11 are the
 * VFP that GCC uses for 64-bit maths in pico_time. CP7 is the RCP and
 * is set for us. */
#define CPACR_NEEDED 0x00F00303u

/* Something on this board clears CPACR back to CP7-only at runtime
 * without resetting the core — observed with the boot counter in
 * watchdog scratch unchanged across the event, and a debugger write to
 * CPACR being undone again later. Whatever the cause, executing a
 * coprocessor instruction afterwards is an instant UsageFault (NOCP)
 * that escalates to HardFault and wedges the slave.
 *
 * Re-asserting the bits costs a compare and an occasional register
 * write per serve-loop iteration, and the counter says how often it
 * actually happens so the underlying cause stays visible rather than
 * being papered over. */
static inline void cpacr_ensure(void) {
    volatile uint32_t *cpacr = (volatile uint32_t *)0xE000ED88u;
    if ((*cpacr & CPACR_NEEDED) != CPACR_NEEDED) {
        *cpacr |= CPACR_NEEDED;
        __asm volatile ("dsb; isb" ::: "memory");
        cpacr_reasserts++;
    }
}

static void boot_count_init(void) {
    if (watchdog_hw->scratch[6] != BOOT_MAGIC) {
        watchdog_hw->scratch[6] = BOOT_MAGIC;
        watchdog_hw->scratch[7] = 0;
    }
    boot_count = ++watchdog_hw->scratch[7];
}

static void fill_node_info(link_node_info_t *info) {
    memset(info, 0, sizeof(*info));

    uint32_t package_sel =
        *((io_ro_32 *)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));

    info->package_is_a = (package_sel & 1u) ? 1 : 0;
    info->fw_version   = SLAVE_FW_VERSION;
    info->sys_clk_hz   = clock_get_hz(clk_sys);
    info->psram_bytes  = PSRAM_BYTES;
    info->proto_ver    = LINK_PROTO_VER;
}

/* ---- Opcode handlers ---- */

static uint32_t __attribute__((aligned(4))) zram_bitmap[LINK_ZRAM_BYTES / 32];
static uint8_t  __attribute__((aligned(4))) zram_block[LINK_ZRAM_BYTES];

static void handle_frame(const link_hdr_t *h) {
    uint32_t count = h->arg0;
    uint32_t audio_target = h->arg1;

    if (count > LINK_MAX_EVENTS) {
        count = LINK_MAX_EVENTS;
        frame_overflows++;
    }

    /* The master follows every FRAME with a ZRAM_BLOCK control frame
     * saying whether its 68K touched Z80 RAM this frame. Driver uploads
     * are 8 KB of byte writes; as events they would swamp the ring. */
    if (link_s_wait_ctrl(&session, LINK_CTRL_TIMEOUT_US) != LINK_OP_ZRAM_BLOCK) {
        return;
    }
    n_zram_block++;
    if (link_rx_hdr(&session)->arg0) {
        if (!link_s_bulk_recv(&session, zram_bitmap, LINK_ZRAM_BYTES / 8) ||
            !link_s_bulk_recv(&session, zram_block, LINK_ZRAM_BYTES)) {
            n_zram_bulkfail++;
            return;
        }
        slave_zram_apply(zram_bitmap, zram_block);
    }

    /* Events arrive as one bulk transfer straight after the header. */
    if (count) {
        if (!link_s_bulk_recv(&session, events,
                              count * sizeof(link_event_t))) {
            n_ev_bulkfail++;
            return;
        }
    }

    link_frame_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.seq = h->seq;

    slave_sound_run_frame(events, count, (int)audio_target, &reply);

    reply.zram_bytes = LINK_ZRAM_BYTES;
    reply.overflows  = frame_overflows;

    if (!link_s_send_ctrl(&session, LINK_OP_FRAME_ACK, 0, 0,
                          &reply, sizeof(reply))) {
        return;
    }

    /* Then the payload, in the order the reply describes: YM samples,
     * PSG samples, Z80 RAM. */
    if (reply.ym_samples) {
        link_s_bulk_send(&session, slave_ym2612_buffer_mem,
                         reply.ym_samples * sizeof(int16_t));
    }
    if (reply.sn_samples) {
        link_s_bulk_send(&session, slave_sn76489_buffer_mem,
                         reply.sn_samples * sizeof(int16_t));
    }
    link_s_bulk_send(&session, slave_zram(), LINK_ZRAM_BYTES);
}

static void handle_rom_chunk(const link_hdr_t *h) {
    uint32_t offset = h->arg0;
    uint32_t len    = h->arg1;

    if (len > LINK_ROM_CHUNK_BYTES) len = LINK_ROM_CHUNK_BYTES;

    n_rom_chunk++;
    last_chunk_off = offset;
    last_chunk_len = len;

    bool ok = link_s_bulk_recv(&session, rom_chunk, LINK_ALIGN4(len));
    if (!ok) n_rom_chunk_bulkfail++;

    if (ok && offset + len <= PSRAM_BYTES) {
        memcpy(PSRAM_BASE + offset, rom_chunk, len);
        rom_received_bytes = offset + len;
    }

    link_s_send_ctrl(&session, LINK_OP_ROM_CHUNK_ACK,
                     rom_received_bytes, ok ? 0 : 1, NULL, 0);
}

static void serve_one(void) {
    /* Patience here is generous: between games the master may be idle
     * for minutes, and a slave that gives up would just spin. */
    uint16_t op = link_s_wait_ctrl(&session, 1000000);
    if (!op) return;

    const link_hdr_t *h = link_rx_hdr(&session);

    switch (op) {
    case LINK_OP_HELLO: {
        link_node_info_t info;
        fill_node_info(&info);
        link_s_send_ctrl(&session, LINK_OP_HELLO_ACK, 0, 0,
                         &info, sizeof(info));
        heartbeat_period_ms = 200;      /* "serving" */
        n_hello++;
        break;
    }

    case LINK_OP_PING:
        link_s_send_ctrl(&session, LINK_OP_PONG, h->arg0, 0, NULL, 0);
        break;

    case LINK_OP_RESET:
        slave_sound_reset();
        frame_overflows = 0;
        link_s_send_ctrl(&session, LINK_OP_RESET_ACK, 0, 0, NULL, 0);
        break;

    case LINK_OP_CONFIG: {
        link_sound_config_t cfg;
        memcpy(&cfg, ctrl_rx + sizeof(link_hdr_t), sizeof(cfg));
        slave_sound_config(&cfg);
        link_s_send_ctrl(&session, LINK_OP_CONFIG_ACK, 0, 0, NULL, 0);
        break;
    }

    case LINK_OP_ROM_BEGIN:
        n_rom_begin++;
        rom_expected_bytes = h->arg0;
        rom_received_bytes = 0;
        slave_sound_reset();
        link_s_send_ctrl(&session, LINK_OP_ROM_BEGIN_ACK,
                         PSRAM_BYTES, 0, NULL, 0);
        break;

    case LINK_OP_ROM_CHUNK:
        handle_rom_chunk(h);
        break;

    case LINK_OP_ROM_END: {
        n_rom_end++;
        slave_rom_set(PSRAM_BASE, rom_received_bytes);
        /* CRC the image as stored, so a corrupted upload is caught here
         * rather than as mysteriously wrong music later. */
        uint32_t crc = link_crc32(PSRAM_BASE, rom_received_bytes);
        slave_sound_reset();
        link_s_send_ctrl(&session, LINK_OP_ROM_END_ACK, crc,
                         rom_received_bytes, NULL, 0);
        break;
    }

    case LINK_OP_FRAME:
        n_frame++;
        handle_frame(h);
        break;

    default:
        break;
    }
}

int main(void) {
#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ);
    sleep_ms(100);
#endif
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        set_sys_clock_khz(252 * 1000, true);
    }

    /* Make sure CP0 (the GPIO coprocessor), CP10 and CP11 (VFP) are on.
     *
     * The SDK normally does this from its per-core preinit array, and
     * when the core is stepped from a debugger reset that is exactly
     * what happens — CPACR reads 0x00F0C303 all the way to main. Free
     * running, this board reaches main with CPACR at 0x0000C000: CP7
     * only. Every gpio_put() then takes a UsageFault (CFSR NOCP), since
     * gpio_put compiles to a CP0 instruction on RP2350, and so does any
     * VFP register use — which is how pico_time's 64-bit maths is
     * compiled, so an alarm handler faults too.
     *
     * Asserting it here is idempotent and costs one register write. */
    runtime_init_per_core_enable_coprocessors();
    cpacr_ensure();

    stdio_init_all();

    gpio_init(S_LED_PIN);
    gpio_set_dir(S_LED_PIN, GPIO_OUT);

    boot_count_init();

    printf("\nfrank-genesis C2 slave — sound subsystem (boot #%lu)\n",
           (unsigned long)boot_count);
    printf("Sys clock: %lu MHz\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000));

    /* PSRAM before anything else touches the XIP windows. */
    psram_init(S_PSRAM_CS_PIN);
    printf("PSRAM: mapped at %p\n", PSRAM_BASE);

    /* Bus A is our receiver (GPIO1..10), bus B our transmitter
     * (GPIO11..20). FS is an input on this side. */
    link_init(&link, LINK_PIO_SLAVE,
              S_LINK_B_DATA_BASE, S_LINK_A_DATA_BASE,
              S_LINK_DB_OUT, S_LINK_DB_IN, S_LINK_FS, false);

    memset(&session, 0, sizeof(session));
    session.link    = &link;
    session.ctrl_tx = ctrl_tx;
    session.ctrl_rx = ctrl_rx;

    printf("Link: PIO0 claimed, %lu KiB/s\n",
           (unsigned long)(link_byte_rate(&link) / 1024));

    slave_sound_init();
    printf("Sound: Z80 + YM2612 + SN76489 ready\n");

    /* An 8 s watchdog underneath everything: the slave recovers from a
     * hang whether or not the master notices. Kicked in the serve loop,
     * which is the only place that should ever block for long. */
    watchdog_enable(8000, 1);

    printf("Serving. boot #%lu, watchdog reason %08lx\n",
           (unsigned long)boot_count,
           (unsigned long)watchdog_hw->reason);

    for (;;) {
        cpacr_ensure();
        heartbeat_tick();

        if (fs_reset_requested) {
            printf("FS reset requested by master\n");
            watchdog_reboot(0, 0, 0);
            for (;;) tight_loop_contents();
        }
        serve_one();
        watchdog_update();
    }
}
