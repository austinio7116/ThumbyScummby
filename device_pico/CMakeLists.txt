#
# ThumbyScummby — bare-metal RP2350 device build.
#
# Produces firmware_thumbyscummby.uf2 by linking the engine library
# (../engine/) against the Pico SDK + our LCD/button/audio drivers.
# Game data is XOR-decrypted by tools/pack_device.py and .incbin'd
# into flash via data_section.S.
#
cmake_minimum_required(VERSION 3.16)

# Standalone (or future ThumbyOne slot embed) toggle. Same pattern as
# ThumbyDOOM and the other emulator slots.
if (NOT THUMBYONE_SLOT_MODE)
    set(PICO_BOARD tinycircuits_thumby_color_rp2350)
    set(PICO_PLATFORM rp2350)
    include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

    project(thumbyscummby_device C CXX ASM)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    pico_sdk_init()
endif()

# ----------------------------------------------------------------------
# Engine library (cross-compiled for ARM here).
# THUMBY_DEVICE define gates host-only code paths in the engine
# (currently the PPM dump in engine.cpp). The engine sources don't
# touch the Pico SDK directly — everything goes through platform.h.
# ----------------------------------------------------------------------
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../engine
                 ${CMAKE_BINARY_DIR}/engine_arm)

target_compile_definitions(thumbyscummby_engine PUBLIC THUMBY_DEVICE=1)
# Override the host warning level a touch — the engine has known-host
# unused-but-set warnings that aren't worth chasing for the device port.
target_compile_options(thumbyscummby_engine PRIVATE
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-format
)

# ----------------------------------------------------------------------
# Bake the game data blob.
#
# tools/pack_device.py reads data/mi1_vga/, decrypts the LECs, and
# writes a single binary with a 9-entry TOC. The .S below .incbin's
# this file from CMAKE_CURRENT_BINARY_DIR.
# ----------------------------------------------------------------------
set(DATA_BLOB ${CMAKE_CURRENT_BINARY_DIR}/data_blob.bin)
set(DATA_DIR  ${CMAKE_CURRENT_LIST_DIR}/../data/mi1_vga)
set(PACK_TOOL ${CMAKE_CURRENT_LIST_DIR}/../tools/pack_device.py)

# Glob the source files so the blob rebuilds if any of them change.
file(GLOB DATA_SOURCES
    ${DATA_DIR}/000.LFL
    ${DATA_DIR}/DISK01.LEC ${DATA_DIR}/DISK02.LEC
    ${DATA_DIR}/DISK03.LEC ${DATA_DIR}/DISK04.LEC
    ${DATA_DIR}/901.LFL ${DATA_DIR}/902.LFL
    ${DATA_DIR}/903.LFL ${DATA_DIR}/904.LFL)

add_custom_command(
    OUTPUT ${DATA_BLOB}
    COMMAND ${CMAKE_COMMAND} -E echo "Packing MI1 data blob..."
    COMMAND python3 ${PACK_TOOL} ${DATA_DIR} ${DATA_BLOB}
    DEPENDS ${PACK_TOOL} ${DATA_SOURCES}
    COMMENT "Packing data blob -> ${DATA_BLOB}"
    VERBATIM
)
add_custom_target(tsb_data_blob DEPENDS ${DATA_BLOB})

# ----------------------------------------------------------------------
# Executable
# ----------------------------------------------------------------------
add_executable(thumbyscummby
    main.cpp
    platform_pico.cpp
    save_backend.cpp
    config_backend.cpp
    lcd_gc9107.c
    buttons.c
    audio_pwm.c
    data_section.S
)

# .incbin in data_section.S resolves "data_blob.bin" relative to its
# include path. Point the assembler at the build dir where the pack
# step writes it.
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/data_section.S
    PROPERTIES
    COMPILE_FLAGS "-I${CMAKE_CURRENT_BINARY_DIR}"
    OBJECT_DEPENDS ${DATA_BLOB}
)
add_dependencies(thumbyscummby tsb_data_blob)

target_include_directories(thumbyscummby PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../engine/include
)

target_compile_definitions(thumbyscummby PRIVATE
    THUMBY_DEVICE=1
    # Pico-SDK defaults to PICO_HEAP_SIZE=0x800 (2KB) — way too small
    # for scummvm.  The very first calloc in allocateArrays (1000
    # bytes for _objectOwnerTable) blows past it (LIME splash hung).
    # RP2350 has 520 KB SRAM; our static + .bss is ~140 KB.  Give the
    # engine 320 KB of heap.  Stack stays at the default 4 KB
    # SCRATCH_X allocation — scummvm doesn't recurse deep enough to
    # overflow that, and growing it would push us into general SRAM
    # (a separate fix if it bites).
    PICO_HEAP_SIZE=0x50000   # 320 KB
)

target_compile_options(thumbyscummby PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections
    -O2
)

target_link_libraries(thumbyscummby PRIVATE
    thumbyscummby_engine
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_gpio
    hardware_pwm
    hardware_irq
    hardware_clocks
    hardware_sync
    hardware_timer
    hardware_flash
)

# Use the SDK-provided float/double implementations. dbopl's
# InitTables calls pow/sin/log10 at startup; with `none` the SDK
# installs wrapper stubs that call panic() — and the device silently
# hangs. ThumbyDOOM gets away with `none` only because emu8950 ships
# precomputed tables and doesn't call libm at init.
pico_set_float_implementation(thumbyscummby pico)
pico_set_double_implementation(thumbyscummby pico)

# stdio: BOTH off. The Thumby Color's default UART TX is GP0 — which
# is the LEFT button pin (set as GPIO_IN by buttons_init). Enabling
# UART stdio means vprintf fills the TX FIFO, then blocks forever
# Both stdio backends disabled — UART pin is repurposed (LCD), and USB CDC
# enumeration was unreliable at 250 MHz with our config. Instead, the
# platform::log() implementation renders an on-screen text overlay (last
# few log lines on the bottom of the LCD). See platform_pico.cpp.
pico_enable_stdio_usb(thumbyscummby 0)
pico_enable_stdio_uart(thumbyscummby 0)

if (THUMBYONE_SLOT_MODE)
    pico_set_uf2_family(thumbyscummby "absolute")
    pico_add_extra_outputs(thumbyscummby)
else()
    pico_add_extra_outputs(thumbyscummby)
endif()

# Friendlier output name to match the firmware naming convention.
set_target_properties(thumbyscummby PROPERTIES
    OUTPUT_NAME "firmware_thumbyscummby"
)
