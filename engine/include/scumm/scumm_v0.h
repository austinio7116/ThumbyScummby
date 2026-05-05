// Stub — v0 (Maniac/Zak older) out of MVP scope.
#ifndef SCUMM_V0_H_STUB
#define SCUMM_V0_H_STUB
#include "scummvm_compat.h"
namespace Scumm {
class ScummEngine_v0 : public ScummEngine {
public:
    ScummEngine_v0(OSystem *syst, const DetectorResult &dr) : ScummEngine(syst, dr) {}
};
class ScummEngine_v2 : public ScummEngine {
public:
    ScummEngine_v2(OSystem *syst, const DetectorResult &dr) : ScummEngine(syst, dr) {}
};
class ScummEngine_v3 : public ScummEngine {
public:
    ScummEngine_v3(OSystem *syst, const DetectorResult &dr) : ScummEngine(syst, dr) {}
};
}
#endif
