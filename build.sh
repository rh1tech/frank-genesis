#!/bin/bash

# Locate the Pico SDK. An exported PICO_SDK_PATH wins, but only if it
# actually contains an SDK — a stale export pointing at a moved
# directory is a common way to get a confusing CMake failure.
sdk_is_valid() { [ -n "${1:-}" ] && [ -f "$1/pico_sdk_init.cmake" ]; }
if ! sdk_is_valid "${PICO_SDK_PATH:-}"; then
    for candidate in "$HOME/pico/pico-sdk" "$HOME/pico-sdk" \
                     "$HOME/Documents/pico/pico-sdk" "/opt/pico-sdk"; do
        if sdk_is_valid "$candidate"; then
            export PICO_SDK_PATH="$candidate"
            break
        fi
    done
fi
if ! sdk_is_valid "${PICO_SDK_PATH:-}"; then
    echo "Error: could not find the Pico SDK. Set PICO_SDK_PATH to a checkout" >&2
    echo "       containing pico_sdk_init.cmake." >&2
    exit 1
fi
echo "Pico SDK: $PICO_SDK_PATH"

rm -rf ./build
mkdir build
cd build

# Always build for M2 board variant by default.
BOARD_VARIANT="${BOARD_VARIANT:-M2}"

case "$BOARD_VARIANT" in
    M1|M2|C2) ;;
    *) echo "Error: BOARD_VARIANT must be M1, M2 or C2 (got '$BOARD_VARIANT')" >&2; exit 1 ;;
esac

# C2 (FRANK Core 2) has no NES pad header, so USB HID is the only input
# path — the CMakeLists forces it on regardless of what is passed here.
if [ "$BOARD_VARIANT" = "C2" ] && [ "$USB_HID_ENABLED" = "0" ]; then
    echo "Note: USB_HID_ENABLED=0 ignored on C2 — HID is the only input path"
    USB_HID_ENABLED=1
fi

# USB HID support is enabled by default. Set USB_HID_ENABLED=0 to disable.
CMAKE_OPTS="-DPICO_PLATFORM=rp2350 -DBOARD_VARIANT=${BOARD_VARIANT} -DUSB_HID_ENABLED=1"
if [ "$USB_HID_ENABLED" = "0" ]; then
    CMAKE_OPTS="-DPICO_PLATFORM=rp2350 -DBOARD_VARIANT=${BOARD_VARIANT}"
    echo "Building WITHOUT USB HID Host support (USB for debug output)"
else
    echo "Building with USB HID Host support (UART for debug output)"
fi
echo "BOARD_VARIANT=${BOARD_VARIANT}"

# Optional tuning: run Z80 every N scanlines (more aggressive = larger N)
if [ -n "$Z80_SLICE_LINES" ]; then
    CMAKE_OPTS="$CMAKE_OPTS -DZ80_SLICE_LINES=$Z80_SLICE_LINES"
    echo "Z80_SLICE_LINES=$Z80_SLICE_LINES"
fi

# Optional Z80 core selection: OLD (original) or GPX (Genesis-Plus-GX)
if [ -n "$Z80_CORE" ]; then
    CMAKE_OPTS="$CMAKE_OPTS -DZ80_CORE=$Z80_CORE"
    echo "Z80_CORE=$Z80_CORE"
fi

# Line interlacing: render every other line (halves VDP time, some quality loss)
# Set LINE_INTERLACE=1 to enable
if [ "$LINE_INTERLACE" = "1" ]; then
    CMAKE_OPTS="$CMAKE_OPTS -DLINE_INTERLACE=1"
    echo "LINE_INTERLACE=1 (rendering every other line)"
fi

# Frame skip level: 0=60fps, 1=50fps (default), 2=40fps, 3=30fps, 4=20fps
# Set FRAMESKIP_LEVEL=N to change
if [ -n "$FRAMESKIP_LEVEL" ]; then
    CMAKE_OPTS="$CMAKE_OPTS -DFRAMESKIP_LEVEL=$FRAMESKIP_LEVEL"
    echo "FRAMESKIP_LEVEL=$FRAMESKIP_LEVEL"
fi

# CRT scanline effect: dims every other scanline for retro look
# Set CRT_SCANLINES=1 to enable
if [ "$CRT_SCANLINES" = "1" ]; then
    CMAKE_OPTS="$CMAKE_OPTS -DCRT_SCANLINES=1"
    echo "CRT_SCANLINES=1 (retro CRT effect)"
    # Optional: CRT_DIM_PERCENT=50 (0-100, default 50)
    if [ -n "$CRT_DIM_PERCENT" ]; then
        CMAKE_OPTS="$CMAKE_OPTS -DCRT_DIM_PERCENT=$CRT_DIM_PERCENT"
        echo "CRT_DIM_PERCENT=$CRT_DIM_PERCENT"
    fi
fi

cmake $CMAKE_OPTS ..
make -j4

# C2 is two firmwares. Build both from one command so the halves cannot
# drift apart: they share the wire protocol and the sound cores, and a
# master talking to a stale slave is a confusing failure to debug.
if [ "$BOARD_VARIANT" = "C2" ]; then
    cd ..
    echo
    echo "=== Building C2 sound slave ==="
    # Both halves MUST run the same system clock. The receiving PIO
    # program has to finish its loop inside the transmitter's byte
    # period and each side derives that from its own clock, so a 504 MHz
    # master talking to a 252 MHz slave transmits bulk data at twice the
    # rate the slave can sample. Control frames survive (they use a
    # slower divider); bulk silently loses bytes.
    #
    # Default matches the master's CMake default rather than the slave's.
    SLAVE_CPU_SPEED="${CPU_SPEED:-504}"
    SLAVE_OPTS="-DPICO_PLATFORM=rp2350 -DCPU_SPEED=${SLAVE_CPU_SPEED}"
    echo "Slave CPU_SPEED=${SLAVE_CPU_SPEED} (must match the master)"
    [ -n "${Z80_CORE:-}" ]  && SLAVE_OPTS="$SLAVE_OPTS -DZ80_CORE=$Z80_CORE"

    cmake -S slave -B slave/build $SLAVE_OPTS || exit 1
    cmake --build slave/build -j4 || exit 1

    echo
    echo "Master: ./build/frank-genesis.uf2"
    echo "Slave:  ./slave/build/frank-genesis-slave.uf2"
fi
