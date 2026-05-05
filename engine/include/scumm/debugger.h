// Stub — scumm/debugger lives in scummvm but we don't ship console UI.
#ifndef SCUMM_DEBUGGER_H_STUB
#define SCUMM_DEBUGGER_H_STUB
#include "scummvm_compat.h"
namespace GUI { class Debugger {}; }
namespace Scumm {
class ScummDebugger : public GUI::Debugger {
public:
    ScummDebugger(ScummEngine *) {}
    virtual ~ScummDebugger() {}
};
}
#endif
