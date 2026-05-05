// Stub — AKOS (v6+ costume format).  Out of scope.
#ifndef SCUMM_AKOS_H_STUB
#define SCUMM_AKOS_H_STUB
#include "scumm/base-costume.h"
namespace Scumm {
class AkosCostumeLoader : public BaseCostumeLoader {
public:
    AkosCostumeLoader(ScummEngine *vm) : BaseCostumeLoader(vm) {}
    void loadCostume(int) override {}
    bool increaseAnims(Actor *) override { return false; }
    void costumeDecodeData(Actor *, int, uint) override {}
};
class AkosRenderer : public BaseCostumeRenderer {
public:
    AkosRenderer(ScummEngine *vm) : BaseCostumeRenderer(vm) {}
    void setPalette(uint16 *) override {}
    void setFacing(const Actor *) override {}
    void setCostume(int, int) override {}
    byte drawLimb(const Actor *, int) override { return 0; }
};
}
#endif
