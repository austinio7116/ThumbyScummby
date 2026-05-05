// Stub — v6 (DOTT/Sam&Max/FOA-CD) out of MVP scope.
//
// Transcribed cpp files (palette.cpp, etc.) define method bodies on
// ScummEngine_v6 even though we don't ship the v6 game subclass.
// Declarations here let those bodies parse; they're never called at
// runtime because we only instantiate ScummEngine_v5.
#ifndef SCUMM_V6_H_STUB
#define SCUMM_V6_H_STUB
#include "scummvm_compat.h"
#include "scumm/scumm_v5.h"
namespace Scumm {
class ScummEngine_v6 : public ScummEngine_v5 {
public:
    ScummEngine_v6(OSystem *syst, const DetectorResult &dr) : ScummEngine_v5(syst, dr) {}

    // Methods overridden in transcribed cpp files.  Declared here so the
    // bodies parse.  Bodies never invoked (we only run v5).
    virtual void palManipulateInit(int resID, int start, int end, int time);
};
}
#endif
