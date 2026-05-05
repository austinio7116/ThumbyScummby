// Stub — GUI MessageDialog forwards to scummvm_compat.h's stub.
#ifndef GUI_MESSAGE_H_STUB
#define GUI_MESSAGE_H_STUB
#include "scummvm_compat.h"
namespace GUI {
inline int InfoMsgID(const char *) { return 0; }
enum {
    kMessageOK = 0,
    kMessageCancel = 1,
};
class TimedMessageDialog : public MessageDialog {
public:
    TimedMessageDialog(const Common::U32String &m, uint /*ms*/) : MessageDialog(m) {}
};
}
#endif
