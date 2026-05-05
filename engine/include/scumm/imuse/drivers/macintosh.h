#ifndef SCUMM_IMUSE_DRIVERS_MACINTOSH_H_STUB
#define SCUMM_IMUSE_DRIVERS_MACINTOSH_H_STUB
#include "audio/mididrv.h"
namespace Scumm { class ScummEngine; }
class IMuseDriver_Macintosh : public MidiDriver {
public:
    IMuseDriver_Macintosh(Scumm::ScummEngine *, Audio::Mixer *, byte) {}
};
#endif
