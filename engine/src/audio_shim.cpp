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
#include "imuse.h"          // our imuse_start_sound / imuse_stop_sound / etc

namespace tsb {

// ----- Sound ctor / dtor -------------------------------------------------
Sound::Sound(ScummEngine *parent, Audio::Mixer * /*mixer*/, bool /*useReplacementAudioTracks*/)
    : _vm(parent),
      _soundCD(nullptr),
      _soundSE(nullptr),
      _mixer(nullptr) {
    // Our imuse_init() is called from main.cpp before the engine starts,
    // so nothing to do here.
}

Sound::~Sound() {}

// ----- Core sound dispatch -----------------------------------------------
void Sound::startSound(int sound, int /*heOffset*/, int /*heChannel*/,
                       int /*heFlags*/, int /*heFreq*/, int /*hePan*/,
                       int /*heVol*/) {
    // Resolve the SOUN resource and hand its full chunk (with header) to imuse.
    if (!_vm || sound <= 0) return;
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
void Sound::saveLoadWithSerializer(Common::Serializer &) {}
void Sound::restoreAfterLoad()                          {}
bool Sound::isAudioDisabled()                           { return false; }
void Sound::updateMusicTimer()                          {}
bool Sound::shouldInjectMISEAudio() const               { return false; }
void Sound::startRemasteredSpeech(const char *, uint16, uint16, uint16) {}

// ----- Dummy data members declared in scumm/sound.h ---------------------
// (Fields are public/protected and zero-init in Sound::Sound; nothing to add)

}  // namespace tsb
