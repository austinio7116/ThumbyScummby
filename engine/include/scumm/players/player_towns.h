#ifndef SCUMM_PLAYERS_TOWNS_H_STUB
#define SCUMM_PLAYERS_TOWNS_H_STUB
#include "scummvm_compat.h"
namespace Scumm {
class Player_Towns_v1 : public Player_Towns {
public:
    Player_Towns_v1(ScummEngine *, Audio::Mixer *) {}
};
class Player_Towns_v2 : public Player_Towns {
public:
    Player_Towns_v2(ScummEngine *, Audio::Mixer *, IMuse *, bool) {}
};
}
#endif
