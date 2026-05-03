cmake_minimum_required(VERSION 3.16)

# Engine library - OS-independent SCUMM v5 interpreter.
# Linked against by host_sdl/ and (later) device_pico/.

add_library(thumbyscummby_engine STATIC
    src/chunk.cpp
    src/small_chunk.cpp
    src/master_index.cpp
    src/room.cpp
    src/smap.cpp
    src/resource.cpp
    src/vm.cpp
    src/opcodes.cpp
    src/opcodes_examples.cpp
    src/opcodes_v4.cpp
    src/actor.cpp
    src/costume.cpp
    src/walkbox.cpp
    src/object.cpp
    src/charset.cpp
    src/string.cpp
    src/opl2.cpp
    src/adlib.cpp
    src/imuse.cpp
    src/audio_mix.cpp
    src/engine.cpp
)

target_include_directories(thumbyscummby_engine PUBLIC include)

target_compile_features(thumbyscummby_engine PUBLIC cxx_std_17)
target_compile_options(thumbyscummby_engine PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections
)
