// Stub — HE resource extractor.  Out of scope.
#ifndef SCUMM_HE_RESOURCE_HE_H_STUB
#define SCUMM_HE_RESOURCE_HE_H_STUB
#include "scummvm_compat.h"
namespace Scumm {
class ResExtractor {
public:
    ResExtractor(ScummEngine *) {}
    virtual ~ResExtractor() {}
    void setCursor(int) {}
};
class MacResExtractor : public ResExtractor {
public:
    MacResExtractor(ScummEngine *vm) : ResExtractor(vm) {}
};
class Win32ResExtractor : public ResExtractor {
public:
    Win32ResExtractor(ScummEngine *vm) : ResExtractor(vm) {}
};
}
#endif
