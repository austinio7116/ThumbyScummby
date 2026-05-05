cmake_minimum_required(VERSION 3.16)

# Engine library - OS-independent SCUMM v5 interpreter.
# Linked against by host_sdl/ and (later) device_pico/.

add_library(thumbyscummby_engine STATIC
    src/chunk.cpp
    src/small_chunk.cpp
    src/master_index.cpp
    src/room_legacy.cpp
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
    # object.cpp is now scummvm-transcribed — bodies depend on too many
    # subsystems.  Legacy is at object_legacy.cpp.
    src/object_legacy.cpp
    # charset.cpp / string.cpp now hold scummvm-transcribed versions —
    # bodies depend on charset_v7 / Mac GUI / Graphics::Font and other
    # subsystems we don't have.  Legacy versions live at *_legacy.cpp
    # until step 12 lights up the dependencies.
    src/charset_legacy.cpp
    src/string_legacy.cpp
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
    #
    # script.cpp + script_v5.cpp + script_v4.cpp transcribed but not built
    # yet — their ~80 v5 opcodes call into charset (step 6), sound
    # (deferred), gfx (step 12), and the object-name table (step 8)
    # which aren't transcribed.  Headers (scumm_v5.h, scumm_v4.h)
    # provide the ScummEngine_v5 / ScummEngine_v4 class declarations
    # which transcribed code can reference.  Legacy vm.cpp +
    # opcodes.cpp + opcodes_v4.cpp keep driving runtime opcode dispatch
    # until the cascade above is closed.
    #
    # charset.cpp + string.cpp transcribed but not built — bodies use
    # Graphics::Font / ConfMan / Mac GUI etc.  charset_legacy.cpp +
    # string_legacy.cpp drive runtime text rendering.
    #
    # verbs.cpp transcribed but not built — depends on charset / gfx /
    # script_v5 transcribed bodies.  No legacy fallback (verbs are stub
    # in our existing op_verbOps); panel won't render until enabled.
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
