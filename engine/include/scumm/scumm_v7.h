// Stub — v7 (FT/DIG) out of MVP scope.
#ifndef SCUMM_V7_H_STUB
#define SCUMM_V7_H_STUB
#include "scummvm_compat.h"
#include "scumm/scumm_v6.h"
namespace Scumm {
class ScummEngine_v7 : public ScummEngine_v6 {
public:
    ScummEngine_v7(OSystem *syst, const DetectorResult &dr) : ScummEngine_v6(syst, dr) {}
};
}
#endif
