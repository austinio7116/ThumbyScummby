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
# Game selection.
#
# One firmware = one game's resources (.incbin'd via data_section.S).
# Override via `-DTHUMBYSCUMMBY_GAME=<name>` at configure time, or via
# the build_device.sh wrapper:  `./build_device.sh mi1`.
#
# Each value here maps to:
#   - A data dir under <repo>/data/ that pack_device.py reads from.
#   - A TSB_GAME_<NAME> compile-time define consumed in main.cpp to
#     pick the DetectorResult (game id, version, filenames, etc.).
#   - A per-game UF2 output name so multiple variants can coexist on
#     disk (firmware_thumbyscummby_<game>.uf2).
# ----------------------------------------------------------------------
set(THUMBYSCUMMBY_GAME "indy4" CACHE STRING
    "Game to embed in this firmware (mi1|mi2|indy3|indy4)")
set_property(CACHE THUMBYSCUMMBY_GAME PROPERTY STRINGS mi1 mi2 indy3 indy4)

if (THUMBYSCUMMBY_GAME STREQUAL "mi1")
    set(DATA_DIR    ${CMAKE_CURRENT_LIST_DIR}/../data/mi1_vga)
    set(GAME_DEFINE "TSB_GAME_MI1=1")
elseif (THUMBYSCUMMBY_GAME STREQUAL "mi2")
    # MI2 floppy: monkey2.000 / monkey2.001 in an HD install layout.
    set(DATA_DIR    ${CMAKE_CURRENT_LIST_DIR}/../data/mi2/MONKEY2)
    set(GAME_DEFINE "TSB_GAME_MI2=1")
elseif (THUMBYSCUMMBY_GAME STREQUAL "indy3")
    # Indy 3 (Last Crusade) EGA: standard LFL layout, SCUMM v3.
    set(DATA_DIR    ${CMAKE_CURRENT_LIST_DIR}/../data/indy3)
    set(GAME_DEFINE "TSB_GAME_INDY3=1")
elseif (THUMBYSCUMMBY_GAME STREQUAL "indy4")
    set(DATA_DIR    ${CMAKE_CURRENT_LIST_DIR}/../data/atlantis/install/ATLANTIS)
    set(GAME_DEFINE "TSB_GAME_INDY4=1")
else()
    message(FATAL_ERROR
        "Unknown THUMBYSCUMMBY_GAME='${THUMBYSCUMMBY_GAME}' "
        "(valid values: mi1, mi2, indy3, indy4)")
endif()
message(STATUS "ThumbyScummby game: ${THUMBYSCUMMBY_GAME}  (DATA_DIR=${DATA_DIR})")

# ----------------------------------------------------------------------
# Data source: three modes.
#
# 1. Standalone, TSB_DATA_FATFS=OFF (default):
#    pack_device.py emits a TSDB blob; data_section.S .incbin's it;
#    platform shim returns flash pointers indexed by TOC slot.
#
# 2. Standalone, TSB_DATA_FATFS=ON:
#    build_fat_image.py emits a FAT12/16 volume; fat_section.S
#    .incbin's it; our own diskio (fatfs/tsb_flash_disk.c) serves
#    sectors from the baked-in image.  Step 1 of the
#    THUMBYONE_SLOT_PLAN — proves the FatFs seam works.
#
# 3. THUMBYONE_SLOT_MODE=scumm:
#    Built as a slot under ThumbyOne.  Shared FAT volume lives at
#    THUMBYONE_FAT_OFFSET; ThumbyOne's shared diskio handles real
#    flash erase/program.  No .incbin'd image — ThumbyScummby
#    reads /scumm/<game>/... from the shared FAT.  Slot mode
#    implicitly forces TSB_DATA_FATFS=ON.
# ----------------------------------------------------------------------
option(TSB_DATA_FATFS
    "Use FatFs-mounted FAT image instead of TSDB blob for game data"
    OFF)

if (THUMBYONE_SLOT_MODE)
    # Slot mode: always FatFs, using ThumbyOne's shared FAT.  No
    # local image generation; the parent provides FatFs sources.
    set(TSB_DATA_FATFS ON)
    set(TSB_THUMBYONE_SLOT ON)
    message(STATUS "ThumbyScummby: building as ThumbyOne slot "
                   "(shared FAT at THUMBYONE_FAT_OFFSET)")
endif()

if (TSB_THUMBYONE_SLOT)
    # Slot mode: no local data baked in.  Game data lives in
    # ThumbyOne's shared FAT volume — the user (or preload
    # pipeline) puts files there separately.
elseif (NOT TSB_DATA_FATFS)
    set(DATA_BLOB ${CMAKE_CURRENT_BINARY_DIR}/data_blob.bin)
    set(PACK_TOOL ${CMAKE_CURRENT_LIST_DIR}/../tools/pack_device.py)

    # Glob the source files so the blob rebuilds if any of them change.
    # pack_device.py auto-detects v4 floppy (000.LFL + DISK*.LEC + 9xx.LFL)
    # vs v5 HD-installed (<BASE>.000 + <BASE>.001).
    file(GLOB DATA_SOURCES
        ${DATA_DIR}/000.LFL
        ${DATA_DIR}/DISK01.LEC ${DATA_DIR}/DISK02.LEC
        ${DATA_DIR}/DISK03.LEC ${DATA_DIR}/DISK04.LEC
        ${DATA_DIR}/901.LFL ${DATA_DIR}/902.LFL
        ${DATA_DIR}/903.LFL ${DATA_DIR}/904.LFL
        ${DATA_DIR}/*.000 ${DATA_DIR}/*.001)

    add_custom_command(
        OUTPUT ${DATA_BLOB}
        COMMAND ${CMAKE_COMMAND} -E echo "Packing game data blob..."
        COMMAND python3 ${PACK_TOOL} ${DATA_DIR} ${DATA_BLOB}
        DEPENDS ${PACK_TOOL} ${DATA_SOURCES}
        COMMENT "Packing data blob -> ${DATA_BLOB}"
        VERBATIM
    )
    add_custom_target(tsb_data_blob DEPENDS ${DATA_BLOB})
else()
    set(FAT_IMAGE ${CMAKE_CURRENT_BINARY_DIR}/fat_image.bin)
    set(FAT_TOOL  ${CMAKE_CURRENT_LIST_DIR}/../tools/build_fat_image.py)

    file(GLOB DATA_SOURCES
        ${DATA_DIR}/000.LFL
        ${DATA_DIR}/DISK01.LEC ${DATA_DIR}/DISK02.LEC
        ${DATA_DIR}/DISK03.LEC ${DATA_DIR}/DISK04.LEC
        ${DATA_DIR}/901.LFL ${DATA_DIR}/902.LFL
        ${DATA_DIR}/903.LFL ${DATA_DIR}/904.LFL
        ${DATA_DIR}/*.000 ${DATA_DIR}/*.001)

    # The FAT image's /scumm/<dir> matches THUMBYSCUMMBY_GAME so the
    # runtime path mirrors what the picker will ultimately expect.
    add_custom_command(
        OUTPUT ${FAT_IMAGE}
        COMMAND ${CMAKE_COMMAND} -E echo "Building FAT image..."
        COMMAND python3 ${FAT_TOOL}
                ${DATA_DIR} ${THUMBYSCUMMBY_GAME} ${FAT_IMAGE}
        DEPENDS ${FAT_TOOL} ${DATA_SOURCES}
        COMMENT "Building FAT image -> ${FAT_IMAGE}"
        VERBATIM
    )
    add_custom_target(tsb_fat_image DEPENDS ${FAT_IMAGE})

    message(STATUS "ThumbyScummby data source: FatFs image (TSB_DATA_FATFS=ON)")
endif()

# ----------------------------------------------------------------------
# Executable — data source toggles which support files are compiled.
# ----------------------------------------------------------------------
if (TSB_THUMBYONE_SLOT)
    # Slot mode: FatFs sources + diskio come from ThumbyOne (parent
    # exports THUMBYONE_FATFS_SRC / THUMBYONE_COMMON_FS_SRC).  No
    # local .S file — we read from the shared FAT in real flash.
    set(DATA_SECTION_SRC
        ${THUMBYONE_FATFS_SRC}
        ${THUMBYONE_COMMON_FS_SRC}
    )
elseif (NOT TSB_DATA_FATFS)
    set(DATA_SECTION_SRC data_section.S)
else()
    set(DATA_SECTION_SRC
        fat_section.S
        fatfs/ff.c
        fatfs/ffsystem.c
        fatfs/ffunicode.c
        fatfs/tsb_flash_disk.c
    )
endif()

add_executable(thumbyscummby
    main.cpp
    platform_pico.cpp
    save_backend.cpp
    config_backend.cpp
    lcd_gc9107.c
    buttons.c
    audio_pwm.c
    ${DATA_SECTION_SRC}
)

if (TSB_THUMBYONE_SLOT)
    target_include_directories(thumbyscummby PRIVATE
        ${THUMBYONE_FATFS_INCLUDE}
        ${THUMBYONE_COMMON_INCLUDE}
    )
elseif (NOT TSB_DATA_FATFS)
    # .incbin in data_section.S resolves "data_blob.bin" relative to its
    # include path. Point the assembler at the build dir where the pack
    # step writes it.
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/data_section.S
        PROPERTIES
        COMPILE_FLAGS "-I${CMAKE_CURRENT_BINARY_DIR}"
        OBJECT_DEPENDS ${DATA_BLOB}
    )
    add_dependencies(thumbyscummby tsb_data_blob)
else()
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/fat_section.S
        PROPERTIES
        COMPILE_FLAGS "-I${CMAKE_CURRENT_BINARY_DIR}"
        OBJECT_DEPENDS ${FAT_IMAGE}
    )
    add_dependencies(thumbyscummby tsb_fat_image)

    target_include_directories(thumbyscummby PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/fatfs
    )
endif()

target_include_directories(thumbyscummby PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../engine/include
)

target_compile_definitions(thumbyscummby PRIVATE
    THUMBY_DEVICE=1
    ${GAME_DEFINE}
    # Pico-SDK defaults to PICO_HEAP_SIZE=0x800 (2KB) — way too small
    # for scummvm.  RP2350 has 520 KB SRAM; static + .bss is ~140 KB;
    # stack is ~4 KB.  0x58000 (352 KB) is the largest size that fits
    # without the linker complaining "cannot move location counter
    # backwards".
    PICO_HEAP_SIZE=0x58000
)

if (TSB_DATA_FATFS)
    target_compile_definitions(thumbyscummby PRIVATE TSB_DATA_FATFS=1)
endif()

if (TSB_THUMBYONE_SLOT)
    target_compile_definitions(thumbyscummby PRIVATE
        THUMBYONE_SLOT_MODE=1
        TSB_THUMBYONE_SLOT=1
        # P8's CMakeLists sets pico_platform INTERFACE
        # PICO_STACK_SIZE=0x4000 which propagates to every target in
        # the unified build.  That blows our SCRATCH_X core1-stack
        # reservation.  Pin both back to the modest values DOOM
        # uses — we only run audio on core0 (no multicore in our
        # build), so core1 stack is reserved-but-unused.
        PICO_STACK_SIZE=0x2000
        PICO_CORE1_STACK_SIZE=0x800)
    # Compile ThumbyOne's handoff + LED sources into the slot
    # directly — same pattern ThumbyDOOM uses.  thumbyone_handoff.c
    # turns off the LED before rebooting via thumbyone_led_off().
    target_sources(thumbyscummby PRIVATE
        ${THUMBYONE_COMMON_SRC}
        ${THUMBYONE_COMMON_LED_SRC})
    target_include_directories(thumbyscummby PRIVATE
        ${THUMBYONE_COMMON_LED_INCLUDE})
    target_link_libraries(thumbyscummby PRIVATE hardware_pio)
endif()

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

if (TSB_THUMBYONE_SLOT)
    pico_set_uf2_family(thumbyscummby "absolute")
endif()
pico_add_extra_outputs(thumbyscummby)

# Per-game UF2 name so MI1 / Indy 4 / etc. coexist in the build tree.
set_target_properties(thumbyscummby PROPERTIES
    OUTPUT_NAME "firmware_thumbyscummby_${THUMBYSCUMMBY_GAME}"
)
