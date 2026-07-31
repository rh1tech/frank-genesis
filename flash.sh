#!/bin/bash
#
# flash.sh — flash frank-genesis to a connected board.
#
# Default path is SWD with a Raspberry Pi Debug Probe, which is what the
# FRANK Core 2 (C2) board wants: its two RP2350s have separate USB-C
# ports and separate BOOTSEL buttons, and USB-BOOTSEL flashing needs a
# button press exactly when the firmware has wedged and you are
# iterating fastest. SWD does not care what the target is doing.
#
# The probe can sit on either MCU's SWD header (C2: J1 = master U3,
# J3 = slave U6; both are pin 1 = SWDIO, 2 = GND, 3 = SWCLK). This
# script reads SYSINFO.PACKAGE_SEL over the wire to say which one it
# found, and refuses to write the emulator to the slave unless asked —
# frank-genesis is master-side firmware.
#
# Usage:
#   ./flash.sh                       # SWD, auto-detect target
#   ./flash.sh build/frank-genesis.elf
#   ./flash.sh --slave               # flash the C2 sound slave (probe on J3)
#   ./flash.sh --usb [firmware]      # USB BOOTSEL via picotool instead
#   ./flash.sh --reset-only          # just reset the attached target
#   ./flash.sh --force               # ignore the master/slave package check
#
set -uo pipefail

cd "$(dirname "$0")"

MODE="swd"
FORCE=0
RESET_ONLY=0
SLAVE=0
FIRMWARE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --usb)        MODE="usb" ;;
        --swd)        MODE="swd" ;;
        --slave)      SLAVE=1 ;;
        --reset-only) RESET_ONLY=1 ;;
        --force)      FORCE=1 ;;
        -h|--help)
            sed -n '3,23p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        -*)
            echo "Error: unknown option '$1'" >&2
            exit 1 ;;
        *)
            FIRMWARE="$1" ;;
    esac
    shift
done

#-----------------------------------------------------------------------
# Locate the firmware. SWD wants the ELF (it carries the load addresses
# and lets addr2line resolve a fault later); picotool wants the UF2.
#-----------------------------------------------------------------------
resolve_firmware() {
    local want_ext="$1"
    local base

    if [ -n "$FIRMWARE" ]; then
        base="${FIRMWARE%.*}"
    elif [ "$SLAVE" = "1" ]; then
        base="./slave/build/frank-genesis-slave"
    else
        base="./build/frank-genesis"
    fi

    if [ -f "${base}.${want_ext}" ]; then
        echo "${base}.${want_ext}"
        return 0
    fi
    # Fall back to whatever was actually named or built.
    local ext
    for ext in elf uf2; do
        if [ -f "${base}.${ext}" ]; then
            echo "${base}.${ext}"
            return 0
        fi
    done
    return 1
}

#-----------------------------------------------------------------------
# USB BOOTSEL path (picotool)
#-----------------------------------------------------------------------
if [ "$MODE" = "usb" ]; then
    if [ "$RESET_ONLY" = "1" ]; then
        echo "Rebooting via picotool..."
        exec picotool reboot -f
    fi

    FW=$(resolve_firmware uf2) || {
        echo "Error: firmware not found (looked for ./build/frank-genesis.{uf2,elf})" >&2
        echo "Run ./build.sh first." >&2
        exit 1
    }

    echo "Flashing over USB BOOTSEL: $FW"
    picotool load -f "$FW" && picotool reboot -f
    exit $?
fi

#-----------------------------------------------------------------------
# SWD path (OpenOCD + CMSIS-DAP)
#-----------------------------------------------------------------------
command -v openocd >/dev/null 2>&1 || {
    echo "Error: openocd not found. Install it (brew install openocd) or use --usb." >&2
    exit 1
}

OPENOCD_ARGS=(-f interface/cmsis-dap.cfg -c "adapter speed 5000" -f target/rp2350.cfg)

if [ "$RESET_ONLY" = "1" ]; then
    echo "Resetting target over SWD..."
    exec openocd "${OPENOCD_ARGS[@]}" -c "init" -c "reset run" -c "exit"
fi

FW=$(resolve_firmware elf) || {
    echo "Error: firmware not found (looked for ./build/frank-genesis.{elf,uf2})" >&2
    echo "Run ./build.sh first." >&2
    exit 1
}

if [ "${FW##*.}" != "elf" ]; then
    echo "Error: SWD flashing needs the ELF; got '$FW'." >&2
    echo "Build it, or pass --usb to flash the UF2 over BOOTSEL." >&2
    exit 1
fi

# Identify what the probe is actually attached to. SYSINFO.PACKAGE_SEL
# bit 0: 1 = RP2350A (QFN-60, the C2 slave), 0 = RP2350B (QFN-80, the
# C2 master and the M1/M2 boards). Halting first keeps the read from
# racing running code that is reconfiguring the bus.
SYSINFO_PACKAGE_SEL=0x40000004

probe_output=$(openocd "${OPENOCD_ARGS[@]}" \
    -c "init" -c "halt" -c "mdw ${SYSINFO_PACKAGE_SEL}" -c "resume" -c "exit" 2>&1)

if ! echo "$probe_output" | grep -q "Examination succeed"; then
    echo "Error: no SWD target found. Check the Debug Probe cable and target power." >&2
    echo "$probe_output" | tail -20 >&2
    exit 1
fi

package_word=$(echo "$probe_output" | awk '/^0x40000004:/ {print $2; exit}')

if [ -n "$package_word" ] && [ $(( 0x${package_word} & 1 )) -eq 1 ]; then
    TARGET_DESC="RP2350A (QFN-60) — the C2 sound slave"
    IS_SLAVE=1
else
    TARGET_DESC="RP2350B (QFN-80) — master / M1 / M2"
    IS_SLAVE=0
fi

echo "SWD target: ${TARGET_DESC}"

# The two images are not interchangeable: the master drives HDMI, SD and
# I2S from an RP2350B pinout, the slave runs the sound subsystem on an
# RP2350A. Writing one to the other half wastes a flash cycle and leaves
# a board that looks broken for no visible reason, so check first.
if [ "$FORCE" != "1" ]; then
    if [ "$SLAVE" = "1" ] && [ "$IS_SLAVE" = "0" ]; then
        echo >&2
        echo "Refusing to flash: --slave was given but the probe is on the" >&2
        echo "RP2350B master. Move it to the slave header (C2: J3)." >&2
        exit 1
    fi
    if [ "$SLAVE" = "0" ] && [ "$IS_SLAVE" = "1" ]; then
        echo >&2
        echo "Refusing to flash: frank-genesis is master-side firmware and the" >&2
        echo "probe is on an RP2350A. Move the probe to the master header" >&2
        echo "(C2: J1), or pass --slave to flash the sound slave instead." >&2
        exit 1
    fi
fi

echo "Flashing over SWD: $FW"
openocd "${OPENOCD_ARGS[@]}" -c "program $FW verify reset exit"
