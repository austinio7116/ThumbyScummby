// Stub — InfoDialog already declared in scummvm_compat.h for our path.
#ifndef SCUMM_DIALOGS_H_STUB
#define SCUMM_DIALOGS_H_STUB
#include "scummvm_compat.h"
namespace Scumm {
class ScummMenuDialog : public InfoDialog {
public:
    ScummMenuDialog(ScummEngine *vm) : InfoDialog(vm, 0) {}
};
class LoomTownsDifficultyDialog : public InfoDialog {
public:
    LoomTownsDifficultyDialog() : InfoDialog(nullptr, 0) {}
    int getSelectedDifficulty() const { return 0; }
};
}
#endif
