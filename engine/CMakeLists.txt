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
    # costume.cpp is now the transcribed scummvm version — see deferred
    # comment in transcribed-from-scummvm-upstream block below.  The
    # legacy ByleRLE costume parser/renderer lives at costume_legacy.cpp
    # until step 12 lights up scummvm's drawCostume + Surface stack.
    src/costume_legacy.cpp
    src/walkbox.cpp
    src/object.cpp
    src/charset.cpp
    src/string.cpp
    src/opl2.cpp
    src/dbopl.cpp
    src/adlib.cpp
    src/imuse.cpp
    src/audio_mix.cpp
    src/engine.cpp
    # Hand-written compat shim implementations (singleton, room-change sync,
    # legacy free-function adapters that wrap transcribed Actor methods).
    src/scummvm_compat.cpp
    src/actor_compat.cpp
    # --- Transcribed from scummvm-upstream ---
    # Each entry below is a near-verbatim copy of the canonical ScummVM
    # source file with only the boundary edits described in scummvm_compat.h
    # (header swap, version-specific #if 0 blocks, namespace alias).
    src/util.cpp
    src/boxes.cpp
    # base-costume.cpp + costume.cpp transcribed but not built yet — their
    # bodies depend on gfx.cpp's full VirtScreen / Graphics::Surface
    # implementation (step 12).  Headers are active so transcribed actor.h
    # method signatures resolve; legacy actor_render_all keeps drawing
    # for now.
)

target_include_directories(thumbyscummby_engine PUBLIC include)

target_compile_features(thumbyscummby_engine PUBLIC cxx_std_17)
target_compile_options(thumbyscummby_engine PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    # Transcribed scummvm-upstream files use `#pragma mark` (Xcode IDE
    # folding markers) — gcc/clang emit -Wunknown-pragmas. Silence so
    # transcribed-as-is files don't generate noise.
    -Wno-unknown-pragmas
    -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections
)
