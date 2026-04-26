#!/bin/bash
rm -rf ./build
mkdir build
cd build

# Always build for M2 board variant by default.
BOARD_VARIANT="${BOARD_VARIANT:-M2}"

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
