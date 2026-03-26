cmake_minimum_required(VERSION 3.13)

# Set board to Pico 2 (RP2350)
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

# Board configuration variant (M1, M2)
set(BOARD_VARIANT "M1" CACHE STRING "Board variant: M1 or M2")

# CPU overclock configuration (252, 378, 504 MHz)
set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz: 252, 378, 504")

# PSRAM speed configuration (84, 100, 133, 166 MHz target)
set(PSRAM_SPEED "166" CACHE STRING "PSRAM max frequency in MHz: 84, 100, 133, 166")

# Z80 scheduling: run the Z80 core less often to reduce interpreter overhead.
# Higher values generally improve performance but can reduce timing fidelity for
# some PCM-heavy drivers.
set(Z80_SLICE_LINES "16" CACHE STRING "Run Z80 every N scanlines (default 16)")

# Z80 CPU selection (default: OLD for best performance)
#   OLD = Marat Fayzullin's core with ARM assembly optimizations (~8.5 MHz effective)
#   GPX = Genesis-Plus-GX pure C (EXPERIMENTAL, ~31% slower but useful for debugging)
set(Z80_CORE "OLD" CACHE STRING "Z80 core: OLD (default) or GPX (experimental)")

# M68K CPU selection: OLD = original with ASM opts, GPX = Genesis-Plus-GX pure C
set(M68K_CORE "OLD" CACHE STRING "M68K core: OLD (asm optimized) or GPX (Genesis-Plus-GX)")

# Line interlacing: render every other line to halve VDP rendering time
# 0 = off (default), 1 = on (some visual quality loss)
set(LINE_INTERLACE "0" CACHE STRING "Line interlacing: 0=off, 1=on")

# Frame skip level: controls how many frames are rendered vs skipped
# 0 = 60fps (no skip), 1 = 50fps, 2 = 40fps, 3 = 30fps (default), 4 = 20fps
set(FRAMESKIP_LEVEL "3" CACHE STRING "Frameskip level: 0-4")

# CRT scanline effect: dims every other scanline for retro look
# 0 = off, 1 = on (default, creates CRT-like appearance, masks low FPS)
set(CRT_SCANLINES "1" CACHE STRING "CRT scanlines: 0=off, 1=on")

# CRT dim brightness percentage (only used if CRT_SCANLINES=1)
set(CRT_DIM_PERCENT "60" CACHE STRING "CRT dim line brightness: 0-100")

# Logging (debug output via UART) - disable for release builds
set(ENABLE_LOGGING "1" CACHE STRING "Enable debug logging: 0=off, 1=on")

# CPU voltage selection based on speed
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

message(STATUS "Board: ${BOARD_VARIANT}, CPU: ${CPU_SPEED} MHz, PSRAM: ${PSRAM_SPEED} MHz, Voltage: ${CPU_VOLTAGE}")

include(pico_sdk_import.cmake)

project(frank-genesis C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Inject version.txt as a compile-time string.
file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" FRANK_GENESIS_VERSION_RAW)
string(STRIP "${FRANK_GENESIS_VERSION_RAW}" FRANK_GENESIS_VERSION_RAW)
string(REGEX REPLACE "[ \t]+" "." FRANK_GENESIS_VERSION "${FRANK_GENESIS_VERSION_RAW}")

pico_sdk_init()

# Add fatfs
add_subdirectory(src/fatfs)
# Add drivers
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/nespad)
add_subdirectory(drivers/ps2kbd)

# Create a library for other drivers
add_library(drivers
    drivers/HDMI.c
    drivers/hdmi_scanline.S
    drivers/psram_init.c
    drivers/psram_allocator.c
    drivers/audio.c
)

# Generate PIO header for I2S audio
pico_generate_pio_header(drivers ${CMAKE_CURRENT_LIST_DIR}/drivers/audio_i2s.pio)

target_include_directories(drivers PUBLIC drivers src ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(drivers PRIVATE
    BOARD_${BOARD_VARIANT}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    CRT_SCANLINES=${CRT_SCANLINES}
    CRT_DIM_PERCENT=${CRT_DIM_PERCENT}
)

target_link_libraries(drivers pico_stdlib hardware_dma hardware_pio hardware_spi)

# Gwenesis emulator sources (common)
set(GWENESIS_SOURCES
    # Bus
    src/bus/gwenesis_bus.c
    # IO
    src/io/gwenesis_io.c
    # Sound (common)
    src/sound/gwenesis_sn76489.c
    src/sound/ym2612.c
    src/sound/ym2612_opt.S
    # VDP
    src/vdp/gwenesis_vdp_gfx.c
    src/vdp/gwenesis_vdp_mem.c
    src/vdp/gwenesis_vdp_gfx_opt.S
    src/vdp/gwenesis_vdp_sprite_opt.S
    src/vdp/gwenesis_vdp_line_opt.S
    # Savestate
    src/savestate/gwenesis_savestate.c
)

# Select Z80 core based on configuration
if(Z80_CORE STREQUAL "GPX")
    message(STATUS "Using Genesis-Plus-GX Z80 core (pure C)")
    list(APPEND GWENESIS_SOURCES 
        src/cpus/Z80_GPX/z80_gpx.c
        src/sound/z80inst_gpx.c
        src/sound/z80_benchmark.c
    )
    set(Z80_INCLUDE_DIR src/cpus/Z80_GPX)
    add_compile_definitions(USE_Z80_GPX=1)
else()
    message(STATUS "Using original Z80 core (with ASM optimizations)")
    list(APPEND GWENESIS_SOURCES 
        src/cpus/Z80/Z80.c
        src/cpus/Z80/z80_arm.S
        src/sound/z80inst.c
        src/sound/z80_mem_opt.S
        src/sound/z80_benchmark.c
    )
    set(Z80_INCLUDE_DIR src/cpus/Z80)
endif()

# Select M68K core based on configuration
if(M68K_CORE STREQUAL "GPX")
    message(STATUS "Using Genesis-Plus-GX M68K core (pure C)")
    list(APPEND GWENESIS_SOURCES src/cpus/M68K_GPX/m68kcpu.c)
    set(M68K_INCLUDE_DIR src/cpus/M68K_GPX)
else()
    message(STATUS "Using original M68K core (with ASM optimizations)")
    list(APPEND GWENESIS_SOURCES 
        src/cpus/M68K/m68kcpu.c
        src/cpus/M68K/m68k_memory_opt.S
    )
    set(M68K_INCLUDE_DIR src/cpus/M68K)
endif()

# Apply aggressive optimizations to M68K CPU
set_source_files_properties(src/cpus/M68K/m68kcpu.c src/cpus/M68K_GPX/m68kcpu.c PROPERTIES
    COMPILE_FLAGS "-O3 -ffast-math -funroll-loops -finline-functions"
)

# Apply aggressive optimizations to Z80 GPX CPU
set_source_files_properties(src/cpus/Z80_GPX/z80_gpx.c PROPERTIES
    COMPILE_FLAGS "-O3 -ffast-math -funroll-loops -finline-functions"
)

add_executable(frank-genesis
    src/main.c
    src/rom_selector.c
    src/settings.c
    ${GWENESIS_SOURCES}
)

target_include_directories(frank-genesis PRIVATE
    src
    src/bus
    ${M68K_INCLUDE_DIR}
    ${Z80_INCLUDE_DIR}
    src/io
    src/sound
    src/vdp
    src/savestate
    src/fatfs
    drivers
    drivers/sdcard
)

target_compile_definitions(frank-genesis PRIVATE
    BOARD_${BOARD_VARIANT}
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    PICO_ON_DEVICE=1
    FRANK_GENESIS_VERSION="${FRANK_GENESIS_VERSION}"
    # Gwenesis configuration
    ROM_SWAP=0
    BUS_DISABLE_LOGGING=1
    VDP_GFX_DISABLE_LOGGING=1
    ENABLE_LOGGING=${ENABLE_LOGGING}
    # M68K configuration (Genesis-Plus-GX)
    LSB_FIRST=1
    # I2S Audio configuration - use PIO0 to avoid conflict with HDMI on PIO1
    PICO_AUDIO_I2S_PIO=0
    PICO_AUDIO_I2S_DMA_IRQ=1
    Z80_SLICE_LINES=${Z80_SLICE_LINES}
    # Performance tuning options
    LINE_INTERLACE=${LINE_INTERLACE}
    FRAMESKIP_LEVEL=${FRAMESKIP_LEVEL}
)

# Set peripheral pins based on board variant
if(BOARD_VARIANT STREQUAL "M1")
    target_compile_definitions(frank-genesis PRIVATE
        # I2S Audio
        PICO_AUDIO_I2S_DATA_PIN=26
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=27
        # SD Card
        SDCARD_PIN_SPI0_CS=5
        SDCARD_PIN_SPI0_SCK=2
        SDCARD_PIN_SPI0_MOSI=3
        SDCARD_PIN_SPI0_MISO=4
        # NES/SNES Gamepad
        NESPAD_GPIO_CLK=14
        NESPAD_GPIO_LATCH=15
        NESPAD_GPIO_DATA=16
    )
elseif(BOARD_VARIANT STREQUAL "M2")
    target_compile_definitions(frank-genesis PRIVATE
        # I2S Audio
        PICO_AUDIO_I2S_DATA_PIN=9
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=10
        # SD Card
        SDCARD_PIN_SPI0_CS=5
        SDCARD_PIN_SPI0_SCK=6
        SDCARD_PIN_SPI0_MOSI=7
        SDCARD_PIN_SPI0_MISO=4
        # NES/SNES Gamepad
        NESPAD_GPIO_CLK=20
        NESPAD_GPIO_LATCH=21
        NESPAD_GPIO_DATA=26
    )
endif()

target_link_libraries(frank-genesis
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_pwm
    hardware_irq
    sdcard
    fatfs
    drivers
    nespad
    ps2kbd
)

# USB configuration: stdio vs HID Host (mutually exclusive)
if(USB_HID_ENABLED)
    # USB HID Host mode - disable USB stdio, use UART instead
    pico_enable_stdio_usb(frank-genesis 0)
    pico_enable_stdio_uart(frank-genesis 1)
    message(STATUS "USB HID Host enabled - using UART for stdio")
    
    # Add USB HID driver
    add_subdirectory(drivers/usbhid)
    target_link_libraries(frank-genesis usbhid)
else()
    # USB Device mode for stdio
    pico_enable_stdio_usb(frank-genesis 1)
    pico_enable_stdio_uart(frank-genesis 0)
endif()

pico_add_extra_outputs(frank-genesis)
