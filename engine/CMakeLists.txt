cmake_minimum_required(VERSION 3.16)

# Engine library — SCUMM v0-v5 interpreter, post-OSystem-pivot.
#
# Three groups of source files:
#   1. SCUMMVM TRANSCRIBED — engine logic, verbatim from scummvm-upstream
#      with only boundary edits (header swap, scummvm_compat.h include).
#      ALL of these compile in the same build.
#   2. SCUMMVM common/graphics — supporting infrastructure (String, Path,
#      File, Surface, Palette, etc.) verbatim from scummvm-upstream.
#   3. THUMBY-NATIVE — audio (DOSBox OPL + iMUSE), platform abstraction,
#      LFL/LEC chunk readers.  Stays as-is across the pivot.
#
# Files NOT in the build (legacy hand-written engine that's being replaced):
#   src/engine.cpp, vm.cpp, opcodes.cpp, opcodes_v4.cpp,
#   src/{room,object,costume,charset,string}_legacy.cpp,
#   src/walkbox.cpp, src/scummvm_compat.cpp, src/actor_compat.cpp.
# These will be deleted in Phase 9 once the pivot drives runtime.

add_library(thumbyscummby_engine STATIC
    # ---- Group 3: Thumby-native (kept across pivot) ----
    # Audio stack — DOSBox OPL2 + iMUSE sequencer.
    src/opl2.cpp
    src/dbopl.cpp
    src/adlib.cpp
    src/imuse.cpp
    src/audio_mix.cpp
    # LFL / LEC chunk readers (used as storage backend by OSystem_Thumby).
    src/chunk.cpp
    src/small_chunk.cpp
    src/master_index.cpp
    src/smap.cpp
    src/resource.cpp

    # ---- Group 2: scummvm common/graphics ----
    src/common/str.cpp
    src/common/str-base.cpp
    src/common/str-enc.cpp
    src/common/path.cpp
    src/common/hashmap.cpp
    src/common/util.cpp
    src/common/textconsole.cpp
    src/common/error.cpp
    src/common/memorypool.cpp
    # stream.cpp uses dynamic_cast — needs -frtti or rewrite. Skip until
    # we can confirm v4-DOS engine actually uses BufferedReadStream.
    # src/common/stream.cpp
    src/common/punycode.cpp
    src/graphics/surface.cpp
    src/graphics/palette.cpp
    src/graphics/managed_surface.cpp

    # ---- Group 1: scummvm engines/scumm/ — transcribed ----
    # Currently enabled (build clean):
    src/util.cpp
    src/boxes.cpp
    src/usage_bits.cpp
    src/camera.cpp
    src/actor.cpp
    src/base-costume.cpp
    src/costume.cpp
    # The following files have additional cone deps (Graphics::Font /
    # MacGui::getFontByScummId / ScummFile / VirtualMachineState members
    # that aren't yet stubbed).  Activated one-by-one as stubs land.
    # src/vars.cpp           # pulls scumm/{music,scumm_v0,scumm_v8}.h, he/* — many subclasses
    # src/charset.cpp        # needs Graphics::Font + MacGui::getFontByScummId
    # src/string.cpp         # depends on charset
    # src/object.cpp         # needs full ObjectData ctor + many ScummEngine methods
    # src/room.cpp           # needs initBGBuffers + setupRoomSubBlocks
    src/palette.cpp        # ColorCycle / palManip storage on ScummEngine
    # src/gfx.cpp            # needs Gdi class real def + virtscr alloc
    # src/sound.cpp          # replaced by audio_shim.cpp
    # src/verbs.cpp          # depends on charset + script_v5
    # src/vars.cpp           # full setupScummVars body
    # src/input.cpp          # depends on Common::EventManager wiring
    # src/cursor.cpp         # needs cursor.h impl
    # src/script.cpp         # vm.slot[] + ScriptSlot etc.
    # src/script_v4.cpp      # depends on script.cpp
    # src/script_v5.cpp      # ~80 v5 opcodes
    # src/scumm.cpp          # ScummEngine::init/go bodies
    # src/saveload.cpp       # out of MVP

    # ---- Bridge implementations (will shrink as pivot completes) ----
    src/osystem_thumby.cpp
    src/scummvm_stubs.cpp
    # audio_shim.cpp lands here in Phase 7.
)

target_include_directories(thumbyscummby_engine PUBLIC include)

target_compile_features(thumbyscummby_engine PUBLIC cxx_std_17)
target_compile_options(thumbyscummby_engine PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    # scummvm transcribed files use `#pragma mark` (Xcode folding markers).
    -Wno-unknown-pragmas
    # scummvm common-string ctors are noisy; silence one-off warning.
    -Wno-reorder
    -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections
)

# scummvm-upstream/common/forbidden.h blocks libc symbols.  Our boundary
# code (platform/, audio_mix, chunk readers) talks libc directly — disable.
target_compile_definitions(thumbyscummby_engine PUBLIC
    FORBIDDEN_SYMBOL_ALLOW_ALL=1
    HAVE_CONFIG_H=1
)
