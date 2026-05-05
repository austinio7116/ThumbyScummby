#ifndef SCUMM_PLAYERS_MAC_NEW_H_STUB
#define SCUMM_PLAYERS_MAC_NEW_H_STUB
#include "scummvm_compat.h"
namespace Scumm {
class MacSound {
public:
    enum { kQualityLowest = 0, kQualityHigh = 1 };
    static MusicEngine *createPlayer(ScummEngine *) { return nullptr; }
};
}
#endif
