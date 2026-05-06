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
    src/lfl_resource.cpp

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
    src/charset.cpp        # text rendering
    # The following pull in heavier deps (SoundHE/DIGI_SND_*/InfoDialog/
    # VerbSlot full def / SaveStateList / Common::EventManager methods).
    # Activated as stubs land.
    src/string.cpp         # _string[6] / decodeParseString
    src/ks_check.cpp       # Korean Hangul Jongsung checker
    src/verbs.cpp          # verb panel
    src/script.cpp         # VM core
    src/script_v4.cpp      # v4 saveLoadVars
    src/script_v5.cpp      # ~80 v5 opcodes
    src/scumm.cpp          # ScummEngine::init / go / setupScumm
    src/vars.cpp           # setupScummVars / resetScummVars
    src/input.cpp          # event handling
    src/cursor.cpp         # mouse cursor
    src/file.cpp           # ScummFile / ScummDiskImage / ScummSteamFile
    src/file_engine.cpp    # ScummEngine::openFile chunk readers
    src/charset-fontdata.cpp  # Built-in font data
    src/bomp.cpp           # decompressBomp / drawBomp
    src/resource.cpp       # ScummEngine::readIndex etc + ResourceManager
    src/resource_v4.cpp    # ScummEngine_v4::readIndexFile
    # OLD lines below
    # src/charset.cpp.old    # needs Graphics::Font + MacGui::getFontByScummId
    # src/string.cpp         # depends on charset
    src/object.cpp         # ObjectData / drawObject
    src/room.cpp           # initBGBuffers + setupRoomSubBlocks
    src/palette.cpp        # ColorCycle / palManip storage on ScummEngine
    src/gfx.cpp            # Gdi / virtscr / drawStripToScreen
    # src/sound.cpp          # bypassed — audio_shim.cpp (Phase 7) replaces it
    # src/verbs.cpp          # depends on charset + script_v5
    # src/vars.cpp           # full setupScummVars body
    # src/input.cpp          # depends on Common::EventManager wiring
    # src/cursor.cpp         # depends on real he/intern_he.h v70he class + bomp.h forward decls
    # src/script.cpp         # vm.slot[] + ScriptSlot etc.
    # src/script_v4.cpp      # depends on script.cpp
    # src/script_v5.cpp      # ~80 v5 opcodes
    # src/scumm.cpp          # ScummEngine::init/go bodies
    # src/saveload.cpp       # out of MVP

    # ---- Bridge implementations (will shrink as pivot completes) ----
    src/osystem_thumby.cpp
    src/scummvm_stubs.cpp
    src/scummvm_link_stubs.cpp  # bulk no-op bodies; --gc-sections strips
    src/audio_shim.cpp     # Sound class bodies forwarding to imuse_*
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
    # Force scummvm_compat.h into every TU before any other include.
    # This puts our `#define Scumm tsb` namespace rewrite + Engine /
    # OSystem / DetectorResult stubs in scope before any scummvm header
    # gets parsed.  Without this, transcribed cpps that include scumm/*.h
    # directly trip on missing types because scummvm_compat.h hasn't been
    # processed yet.
    -include scummvm_compat.h
)

# scummvm-upstream/common/forbidden.h blocks libc symbols.  Our boundary
# code (platform/, audio_mix, chunk readers) talks libc directly — disable.
target_compile_definitions(thumbyscummby_engine PUBLIC
    FORBIDDEN_SYMBOL_ALLOW_ALL=1
    HAVE_CONFIG_H=1
    ENABLE_SCUMM=1            # turns on scumm/file.h's ScummFile class
)
