# C2 sound split — moving Z80, YM2612 and SN76489 to the slave

Design reference for the dual-RP2350 FRANK Core 2 (`BOARD_VARIANT=C2`)
build. The master keeps the 68K, the VDP, video, SD, input and the I2S
DAC; the slave takes the entire sound subsystem.

**M1 and M2 must be unaffected.** Everything here is behind a compile-time
seam; on those boards the sound path compiles to exactly what it does
today.

## Why this split is tractable

The sound subsystem already talks to the rest of the emulator through ten
call sites, and every one of them is already timestamped with
`m68k_cycles_master()`. That timestamp is what makes a deferred, replayed
event stream equivalent to running the chips inline.

**Master → sound. Fire-and-forget, ordered, timestamped.**

| What | Site |
|---|---|
| Z80 RAM write | `bus.c:604` (+16-bit variant) |
| `YM2612Write` | `bus.c:610`, 16-bit path ~`:700` |
| `SN76489_Write` | `bus.c:619`, `:700`, `vdp_mem.c:896` |
| BUSREQ / RESET | `bus.c:600` → `z80_write_ctrl` |
| Z80 bank register | `Z80_BANK_ADDR` write path |
| Z80 IRQ line | `z80_irq_line()`, vblank edge |
| Z80 time pump | `main.c:862` `z80_run()` — becomes a timestamp marker |

**Sound → master. Needs data back — the whole difficulty.**

| What | Site | Resolution |
|---|---|---|
| `ZRAM[addr]` read | `bus.c:484` | per-frame 8 KB snapshot + write journal |
| `YM2612Read()` | `bus.c:487` | master-side timer shadow |
| `z80_read_ctrl()` | `bus.c:480` | **free** — pure master-side state |

`z80_read_ctrl` returns only `bus_ack` and `reset`, both of which are set
exclusively by the master's own `z80_write_ctrl`. It never needs the
slave. That removes what would otherwise be the most frequent round trip,
since games poll BUSREQ hard.

`zvdp_mem_r8/w8` is commented out in `z80inst.c`, so the Z80 never reaches
the VDP. An entire class of reverse dependency does not exist.

## Ownership

**Slave:** Z80 core + 8 KB Z80 RAM + YM2612 + SN76489, plus a full ROM
copy in its 8 MB PSRAM. `zbank_mem_r8()` reads ROM through the bank
register, so the slave needs the whole image; the existing 2×32 KB SRAM
bank cache in `z80inst.c` ports over unchanged and does the same job
against the slave's PSRAM.

**Master:** everything else, including the I2S DAC — it is wired to master
GPIO 9/10/11 — so audio comes back over the link.

**The slave does not mix.** It returns the two chip buffers raw and mono,
exactly as the chips rendered them. Mixing there looked cheaper but is
wrong: `audio_submit()` mixes, applies `master_volume`, time-stretches the
frame to 888 samples against the *measured* wall-clock interval since the
last submission, duplicates to stereo and applies the startup mute — all
master-side state the slave cannot know. Returning raw buffers leaves
`drivers/audio.c` completely untouched and costs the same bytes on the
wire (2 × 888 × 2 B either way).

## Sync model: one-frame-lag pipeline

Core 0 appends timestamped events to a lock-free ring as the 68K runs.
Core 1 ships the completed frame's event list and collects the *previous*
frame's 888 mixed samples. **The 68K never blocks on the link.**

```
frame N   core0: 68K + VDP run, events -> ring
          core1: ship frame N events
                 collect frame N-1 samples -> I2S

slave     replay frame N-1 stream in timestamp order
          Z80 + YM + PSG + mix -> 888 stereo samples
          snapshot 8 KB Z80 RAM
```

Added audio latency is one frame (~16.7 ms) on top of the existing
double-buffering. This is sound only — video and input are untouched, so
control latency does not change.

The lag is safe precisely because the Z80 can influence the 68K through
only two channels, ZRAM contents and YM status, and both are made
frame-stale deliberately below. Nothing else observes the Z80.

## The two reads

### YM2612 status — master-side timer shadow

`YM2612Read()` returns `ym2612.OPN.ST.status & 0xff`: timer A and timer B
overflow flags. Those follow from writes to registers 0x24–0x27 plus
elapsed cycles. No FM synthesis is involved, so the master runs a small
timer model fed by the same writes it is already forwarding and answers
locally. No round trip.

### 68K reads of Z80 RAM — snapshot plus journal

The slave ships all 8 KB of Z80 RAM back each frame. The master cannot
simply overwrite its mirror with it: the snapshot was taken at a known
emulated timestamp, and the 68K has since written bytes of its own. So the
master keeps a small journal of its post-snapshot ZRAM writes and replays
them over each incoming snapshot. It never reads back its own stale bytes,
and Z80-authored bytes are at most one frame old.

A driver-ready flag polled by the 68K therefore resolves up to one frame
later than on hardware. The polling loop still terminates; it just spins
for another frame.

## Link budget

Against 48 MB/s per direction, measured error-free on this board:

| Traffic | Per frame | Per second |
|---|---|---|
| Event stream (worst case ~2000 events × 8 B) | 16 KB | 0.96 MB/s |
| Z80 RAM snapshot | 8 KB | 0.49 MB/s |
| Mixed stereo audio (888 × 2 × 2 B) | 3.5 KB | 0.21 MB/s |
| **Total** | **~28 KB** | **~1.7 MB/s (3.5%)** |

The link is not the constraint anywhere. ROM upload at load time is bound
by the slave's PSRAM write speed (~12.9 MiB/s measured), so roughly 0.3 s
for a 4 MB ROM — one-time, during the existing load screen.

## Resource allocation (master)

| Resource | Owner |
|---|---|
| PIO0 | I2S audio + PS/2 keyboard |
| PIO1 | HDMI video (2 SMs) |
| **PIO2** | **link TX + RX — entirely free today** |
| Core 0 | 68K, VDP, event production |
| Core 1 | link exchange + I2S submission |

The link's bus B is GPIO30–39, so its PIO instance needs
`pio_set_gpio_base(16)`. That is a per-instance setting and PIO2 is ours
alone, so it cannot collide with HDMI on PIO1. `nespad` also claims PIO1,
but C2 has no pad header and never initialises it.

## Keeping M1/M2 intact

- The bus calls a `sound_backend_*` seam instead of `YM2612Write` and
  friends. On M1/M2 those are `static inline` wrappers over the existing
  calls — identical generated code, identical path.
- `z80_mem_opt.S` calls `YM2612Write` directly from assembly. That is
  fine: **on C2 the master does not compile the Z80 at all**, and the
  slave uses the same assembly verbatim.
- If the slave does not answer at boot, the C2 master falls back to the
  local backend and plays sound itself, exactly as M1/M2 do. A C2 board
  with an unflashed slave is degraded, not silent.

## Build layout

```
murmgenesis/
  src/            shared: sound/, cpus/Z80/ — built into both halves
  link/           shared: link_bus.{c,h,pio}, link_proto.{c,h}
  slave/          CMakeLists -> frank-genesis-slave
  build.sh        BOARD_VARIANT=C2 builds BOTH halves
  flash.sh        --slave targets the slave SWD header (J3)
```

Both halves must be built at the same `CPU_SPEED`: the receiving PIO
program has to complete its loop inside the transmitter's byte period, and
each side derives that from its own system clock. Mismatched clocks give a
link that works in one direction and drops bytes in the other.

## Measuring the win

`main.c` already accumulates `m68k_time`, `z80_time` and `sound_time` and
prints them every 300 frames. Take a baseline on a real ROM before the
split so the result is a measurement rather than an assumption — what
moves to the slave is exactly `z80_time + sound_time`.

## Verified on hardware

Aladdin (2 MB), both halves at 504 MHz:

| Measure | Result |
|---|---|
| Frame exchanges | 1243 in 30 s, **zero** failures |
| Event overflows | 0 |
| Probe failures | 0 |
| ROM upload | 64/64 chunks, CRC matched |
| Audio | 71.7% of energy in the 150-600 Hz melody band, 63% of full scale |
| Master profile | m68k 6.17 s, vdp 0.25 s, **z80 0.025 s, sound 0.021 s** |

Z80 plus sound is 0.7% of tracked emulation time on the master — event
emission and frame bookkeeping only. The chips themselves run entirely
on the slave.

## Bugs this cost, and what they looked like

Every one of these produced silence or a hang, and almost none pointed
at its own cause. Recorded because the same traps are waiting in any
similar split.

1. **Clock mismatch** (504 vs 252 MHz). Only the transmitter is divided,
   so bulk arrived at twice the rate the slave could sample. Control
   frames survived — they use a slower divider — so it read as a
   protocol bug rather than a wire-rate one.
2. **Missing `set_flash_timings()`** on the slave. Fixing (1) by raising
   its clock then ran the QSPI out of spec and it faulted before `main()`.
3. **Z80 RAM streamed as events.** A driver upload is 8 KB of byte
   writes in one frame against a 4096-event ring; it truncated and the
   slave ran a corrupt driver.
4. **`AUDIO_FREQ_DIVISOR` shadowed** by a local `#ifndef ... #define 60`
   fallback where the real value is 1009. Compiled cleanly, and nothing
   about the symptom pointed at a `#define`.
5. **`zclk` not reset per frame.** The Z80 ran exactly one frame; the FM
   then emitted constant DC, which reads as a mixing fault.
6. **Over-permissive control decode.** `(address & 0x1F00) == 0x1200`
   where the original compares exactly, turning stray writes in 0xA112xx
   into reset pulses. The Z80 sat at PC=0 while every other indicator
   said it was running.
7. **ZRAM dirty-bitmap race.** Clearing the map after sending discarded
   bits set during the send, silently dropping bytes from the driver
   upload.
8. **`ym.clock` not reset per frame** — same as (5), in the master's
   timer shadow. Froze the YM status flags, and the 68K polling them ran
   off into unmapped memory.

Two of these, (5) and (8), are the same mistake in the two places a
cycle count is carried across frames. If a third such counter is ever
added, reset it with the others in `main.c` at frame start.

### A debugging trap worth knowing

Halting an RP2350 with OpenOCD **clears CPACR**. The next `gpio_put`
(CP0 on RP2350) or 64-bit timestamp (VFP) then takes a UsageFault with
`CFSR = NOCP` and escalates to a HardFault. The symptom is a core that
appears to have crashed on its own moments after you looked at it, which
is thoroughly misleading. Both halves re-assert CPACR — the slave in its
serve loop, the master once per frame — and any debugger script should
restore it before resuming.

## Phases

1. Port the link layer; master and slave handshake and exchange a ping.
2. Add the sound backend seam. No behaviour change anywhere, M1/M2/C2.
3. Slave firmware: ROM upload, event replay, mixer, snapshot.
4. Master link backend: event ring, frame exchange, timer shadow, journal,
   fallback.
5. Savestates, settings forwarding, reset and ROM-change handling.
