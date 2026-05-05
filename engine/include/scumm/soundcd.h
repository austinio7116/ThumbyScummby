// Stub — CD audio out of MVP scope.  Methods called from scumm/sound.h
// inlines need to exist; bodies are no-op.
#ifndef SCUMM_SOUNDCD_H_STUB
#define SCUMM_SOUNDCD_H_STUB
#include "scummvm_compat.h"
namespace Scumm {
class SoundCD {
public:
    SoundCD(ScummEngine *) {}
    virtual ~SoundCD() {}
    bool isRolandLoom() const { return false; }
    int  pollCD() const { return 0; }
    void updateCD() {}
    void stopCD() {}
    void stopCDTimer() {}
    void playCDTrack(int, int, int, int) {}
    int  getCurrentCDSound() const { return 0; }
    void restoreCDAudioAfterLoad(const AudioCDManager::Status &) {}
};
}
#endif
