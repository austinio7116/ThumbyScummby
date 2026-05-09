// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — audio_shim: Sound class bodies that forward to imuse_*.
//
// scumm/sound.cpp is NOT in our build (its dep cone — iMUSE / MidiDriver_AdLib /
// Audio::Mixer streams — would drag in 5K+ LOC of scummvm audio).  We provide
// minimal bodies for Sound's non-inline methods so the linker is happy, and
// route the methods that matter (startSound, stopSound, isSoundRunning,
// processSound) to our DOSBox-OPL2 + iMUSE sequencer.
//
// Our imuse.cpp drives the OPL2 emulator from the SCUMM 'AD' (AdLib FM)
// sub-chunks inside SOUN/SO resources.  scumm.cpp's Sound::startSound(int)
// just needs to find the SOUN payload in the resource manager and hand it
// to imuse_start_sound(id, span).
//
// For non-MI1 paths (talkSound, CD music, etc) we no-op — those paths
// don't fire on v4-MI1-AdLib runtime.

#include "scummvm_compat.h"
#include "scumm/sound.h"
#include "scumm/scumm.h"
#include "scumm/resource.h"
#include "common/serializer.h"
#include "imuse.h"          // our imuse_start_sound / imuse_stop_sound / etc

namespace tsb {

// ----- Resource loader for v4 sounds -------------------------------------
// Upstream scummvm has scumm/sound.cpp:readSoundResourceSmallHeader() that
// runs convertADResource() to wrap raw AD payloads in a synthesized ADL
// chunk for the upstream iMUSE engine.  We don't link upstream sound.cpp
// (we bypass scummvm's iMUSE in favour of our DOSBox-OPL2 stack), so when
// scumm/resource.cpp:695 calls readSoundResourceSmallHeader for an
// rtSound load, the symbol resolves to this minimal version: read the
// entire SO chunk verbatim into the rtSound buffer.  imuse_start_sound
// (imuse.cpp:autodetect) finds the inner AD/WA/SO/RO/MThd payload by
// scanning, so we don't need to pre-slice it.
int ScummEngine::readSoundResourceSmallHeader(ResId idx) {
    // ScummEngine::loadResource has already done seek(+8) over the LFLF
    // wrapper and a peek-then-rewind of the SO chunk's size+tag.  The
    // file pointer is now at the start of the SO chunk header.  Re-read
    // size, rewind, and slurp the full `size` bytes.
    int64 pos = _fileHandle->pos();
    uint32 size = _fileHandle->readUint32LE();
    _fileHandle->seek(pos, SEEK_SET);
    byte *dst = _res->createResource(rtSound, idx, size);
    if (dst)
        _fileHandle->read(dst, size);
    return 1;
}

// ----- Sound ctor / dtor -------------------------------------------------
Sound::Sound(ScummEngine *parent, Audio::Mixer * /*mixer*/, bool /*useReplacementAudioTracks*/)
    : _vm(parent),
      _soundCD(nullptr),
      _soundSE(nullptr),
      _mixer(nullptr) {
    // string.cpp:1277 dereferences _talkChannelHandle even when no talkie is
    // active.  Upstream Sound::Sound allocates one (sound.cpp:85); mirror
    // that here so the deref doesn't segfault.  Our NullMixer ignores the
    // handle anyway.
    _talkChannelHandle = new Audio::SoundHandle();
    // Our imuse_init() is called from main.cpp before the engine starts,
    // so nothing to do here.
}

Sound::~Sound() {
    delete _talkChannelHandle;
}

// ----- Core sound dispatch -----------------------------------------------
void Sound::startSound(int sound, int /*heOffset*/, int /*heChannel*/,
                       int /*heFlags*/, int /*heFreq*/, int /*hePan*/,
                       int /*heVol*/) {
    // Resolve the SOUN resource and hand its full chunk (with header) to imuse.
    if (!_vm || sound <= 0) return;
    // Mirror upstream Sound::startSound (sound.cpp:120-126): set VAR_LAST_SOUND
    // so scripts polling that var see the active sound id.
    if (_vm->VAR_LAST_SOUND != 0xFF)
        _vm->VAR(_vm->VAR_LAST_SOUND) = sound;
    const byte *ptr = _vm->getResourceAddress(rtSound, sound);
    if (!ptr) return;
    // Resource size is opaque in our shim; imuse_start_sound autodetects format.
    // We pass a generous upper bound — imuse parses TLV chunks and stops on EOF.
    Span span;
    span.data = ptr;
    span.size = 0xFFFFFF;        // best effort
    imuse_start_sound(sound, span);
}

void Sound::stopSound(int sound) {
    if (sound <= 0) return;
    imuse_stop_sound(sound);
}

void Sound::stopAllSounds() {
    imuse_stop_all();
}

int Sound::isSoundRunning(int sound) const {
    return imuse_is_running(sound) ? 1 : 0;
}

bool Sound::isSoundInUse(int sound) const {
    return imuse_is_running(sound);
}

bool Sound::isSoundInQueue(int /*sound*/) const {
    return false;
}

void Sound::soundKludge(int * /*list*/, int /*num*/) {}

void Sound::processSound() {
    // imuse advances on a microsecond budget driven by the platform audio
    // callback — nothing to do per-frame here.
}

void Sound::pauseSounds(bool /*pause*/) {}

void Sound::setupSound() {}

void Sound::addSoundToQueue(int sound, int /*offset*/, int /*channel*/,
                            int /*flags*/, int /*freq*/, int /*pan*/,
                            int /*volume*/) {
    // Most v4 paths call addSoundToQueue then later processSoundQueues —
    // the queueing is the iMUSE deferred-start protocol.  We start
    // immediately; imuse handles only one song at a time and replaces.
    startSound(sound);
}

// ----- Stubs for methods scumm.cpp / opcode bodies might touch ----------
void Sound::processSoundQueues()                        {}
void Sound::processSfxQueues()                          {}
void Sound::triggerSound(int /*soundID*/)               {}
void Sound::startTalkSound(uint32, uint32, int, Audio::SoundHandle *) {}
void Sound::stopTalkSound()                             {}
bool Sound::isMouthSyncOff(uint /*pos*/)                { return true; }
void Sound::talkSound(uint32, uint32, int, int)         {}
bool Sound::isSfxFileCompressed()                       { return false; }
bool Sound::hasSfxFile() const                          { return false; }
void Sound::extractSyncsFromDiMUSEMarker(const char *)  {}
void Sound::setupSfxFile()                              {}
bool Sound::isSfxFinished() const                       { return true; }
void Sound::incrementSpeechTimer()                      {}
void Sound::resetSpeechTimer()                          {}
void Sound::startSpeechTimer()                          {}
void Sound::stopSpeechTimer()                           {}
bool Sound::speechIsPlaying()                           { return false; }
void Sound::saveLoadWithSerializer(Common::Serializer &s) {
	// Real bodies in scummvm sound.cpp:1247.  _soundCD is nullptr in our
	// build (no SoundCD), so we sync a literal 0 through the same VER
	// gate so save layout matches upstream.
	int16 cd_track = 0;
	s.syncAsSint16LE(cd_track, VER(35));
	s.syncAsSint16LE(_currentMusic, VER(35));
}
void Sound::restoreAfterLoad()                          {}
bool Sound::isAudioDisabled()                           { return false; }

// Drives VAR_MUSIC_TIMER from our iMUSE state.  scumm.cpp:3079 calls this
// from scummLoop, once per frame.  The boot/title scripts wait on this var
// to time out the Lucasfilm logo / theme tune before the gameplay starts.
//
// Mirrors scummvm-upstream sound.cpp:2206 — non-CD branch:
//   VAR(VAR_MUSIC_TIMER) = _musicEngine->getMusicTimer() * timer_freq / 240
// We bypass _musicEngine (which is nullptr in our build because IMuse::create
// returns null — we don't link scummvm's iMUSE) and read straight from our
// imuse_get_music_timer(), which already uses upstream's
// `parser_ticks * 2 / PPQN` formula (imuse.cpp:773).
void Sound::updateMusicTimer() {
    if (!_vm) return;
    if (_vm->VAR_MUSIC_TIMER == 0xFF) return;
    const int t = imuse_get_music_timer();
    _vm->VAR(_vm->VAR_MUSIC_TIMER) =
        (int32)((double)t * _vm->getTimerFrequency() / 240.0);
}
bool Sound::shouldInjectMISEAudio() const               { return false; }
void Sound::startRemasteredSpeech(const char *, uint16, uint16, uint16) {}

// ----- Dummy data members declared in scumm/sound.h ---------------------
// (Fields are public/protected and zero-init in Sound::Sound; nothing to add)

}  // namespace tsb
