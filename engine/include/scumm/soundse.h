// Stub — Special Edition audio out of MVP scope.
#ifndef SCUMM_SOUNDSE_H_STUB
#define SCUMM_SOUNDSE_H_STUB
#include "scummvm_compat.h"
namespace Scumm {
class SoundSE {
public:
    SoundSE(ScummEngine *) {}
    virtual ~SoundSE() {}
    void setupMISEAudioParams(int, int) {}
};
}
#endif
