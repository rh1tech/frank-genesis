#
# frank-genesis — C2 slave firmware (RP2350A, U6)
#
# Runs the Genesis sound subsystem driven by the master's event stream.
# Shares src/sound and src/cpus/Z80 with the master build so the two
# halves can never emulate the chips differently.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
cmake_minimum_required(VERSION 3.13)

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

# The slave is the RP2350A (QFN-60): 30 GPIOs, PSRAM CS on GPIO0, blue
# LED on GPIO26, UART1 on the J4 header.
set(PICO_BOARD_HEADER_DIRS ${REPO_ROOT}/boards CACHE PATH "" FORCE)
set(PICO_BOARD "frank_core2_slave" CACHE STRING "Pico board type" FORCE)

# Both halves must be built at the same CPU_SPEED: the receiving PIO
# program has to finish its loop inside the transmitter's byte period,
# and each side derives that from its own system clock. Mismatched
# clocks give a link that works one way and drops bytes the other.
set(CPU_SPEED   "252" CACHE STRING "CPU clock in MHz")
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz")

if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

# Z80 core selection must match the master's, since both replay the same
# driver code and a different interpreter is a different emulation.
set(Z80_CORE "OLD" CACHE STRING "Z80 core: OLD or GPX")

message(STATUS "C2 slave: CPU ${CPU_SPEED} MHz, PSRAM ${PSRAM_SPEED} MHz, "
               "vreg ${CPU_VOLTAGE}, Z80 ${Z80_CORE}")

include(${REPO_ROOT}/pico_sdk_import.cmake)
project(frank-genesis-slave C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_subdirectory(${REPO_ROOT}/link link)

# Sound cores, shared verbatim with the master.
set(SLAVE_SOUND_SOURCES
    ${REPO_ROOT}/src/sound/gwenesis_sn76489.c
    ${REPO_ROOT}/src/sound/ym2612.c
    ${REPO_ROOT}/src/sound/ym2612_opt.S
)

if(Z80_CORE STREQUAL "GPX")
    list(APPEND SLAVE_SOUND_SOURCES
        ${REPO_ROOT}/src/cpus/Z80_GPX/z80_gpx.c
        ${REPO_ROOT}/src/sound/z80inst_gpx.c
    )
    set(Z80_INCLUDE_DIR ${REPO_ROOT}/src/cpus/Z80_GPX)
    add_compile_definitions(USE_Z80_GPX=1)
else()
    list(APPEND SLAVE_SOUND_SOURCES
        ${REPO_ROOT}/src/cpus/Z80/Z80.c
        ${REPO_ROOT}/src/cpus/Z80/z80_arm.S
        ${REPO_ROOT}/src/sound/z80inst.c
        ${REPO_ROOT}/src/sound/z80_mem_opt.S
        ${REPO_ROOT}/src/sound/z80_benchmark.c
    )
    set(Z80_INCLUDE_DIR ${REPO_ROOT}/src/cpus/Z80)
endif()

add_executable(frank-genesis-slave
    src/main.c
    src/slave_glue.c
    src/slave_sound.c
    ${SLAVE_SOUND_SOURCES}
    # The sound cores carry their own savestate hooks; linking the
    # serializer here is also what will let the slave take part in
    # savestates rather than being a hole in them.
    ${REPO_ROOT}/src/savestate/gwenesis_savestate.c
    ${REPO_ROOT}/drivers/psram_init.c
)

target_include_directories(frank-genesis-slave PRIVATE
    src
    ${REPO_ROOT}/src
    ${REPO_ROOT}/src/sound
    ${REPO_ROOT}/src/bus
    ${REPO_ROOT}/src/savestate
    ${REPO_ROOT}/src/io
    ${REPO_ROOT}/src/vdp
    # z80inst.c includes m68k.h purely for m68k_cycles_master(); the
    # slave supplies its own definition in slave_glue.c.
    ${REPO_ROOT}/src/cpus/M68K
    ${Z80_INCLUDE_DIR}
    ${REPO_ROOT}/drivers
    ${REPO_ROOT}/link
)

target_compile_definitions(frank-genesis-slave PRIVATE
    BOARD_C2
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    PICO_ON_DEVICE=1
    # Same gwenesis configuration as the master, so the chips behave
    # identically on both halves.
    BUS_DISABLE_LOGGING=1
    VDP_GFX_DISABLE_LOGGING=1
    ENABLE_LOGGING=1
    LSB_FIRST=1
)

target_compile_options(frank-genesis-slave PRIVATE
    -O2
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
)

target_link_libraries(frank-genesis-slave
    pico_stdlib
    hardware_clocks
    hardware_dma
    hardware_gpio
    hardware_pio
    hardware_sync
    hardware_vreg
    hardware_watchdog
    link
)

# Console on UART1 via the J4 header — the slave's USB-C port stays free
# for BOOTSEL, and SWD on J3 is the primary debug route anyway.
pico_enable_stdio_uart(frank-genesis-slave 1)
pico_enable_stdio_usb(frank-genesis-slave 0)

pico_add_extra_outputs(frank-genesis-slave)
