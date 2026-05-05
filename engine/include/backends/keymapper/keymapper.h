// Stub — scummvm Keymapper for input mapping.  We use platform::poll_input
// directly; never need to open a Keymap.
#ifndef BACKENDS_KEYMAPPER_KEYMAPPER_H_STUB
#define BACKENDS_KEYMAPPER_KEYMAPPER_H_STUB
#include "scummvm_compat.h"
namespace Common {
class Keymap {
public:
    enum KeymapType { kKeymapTypeGlobal = 0, kKeymapTypeGui = 1, kKeymapTypeGame = 2 };
    Keymap(KeymapType, const String &, const U32String &) {}
    void addAction(class Action *) {}
    void setEnabled(bool) {}
};
class Keymapper {
public:
    void addCustomBackendActions(const String &, Array<Keymap *> &) {}
    void enableKeymap(const String &, bool) {}
    Keymap *getKeymap(const String &) const { return nullptr; }
};
class Action {
public:
    Action(const char *, const U32String &) {}
    void setCustomEngineActionEvent(int) {}
    void addDefaultInputMapping(const String &) {}
    void addKeyEvent(const KeyState &) {}
    void allowKbdRepeats() {}
    void setKeyEvent(const KeyState &) {}
};
}
#endif
