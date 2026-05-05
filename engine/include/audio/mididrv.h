// Stub — we bypass scummvm's MIDI/iMUSE.  This header just has to exist.
#ifndef AUDIO_MIDIDRV_H_STUB
#define AUDIO_MIDIDRV_H_STUB
#include "common/scummsys.h"

enum MidiDriverFlags {
    MDT_NONE         = 0,
    MDT_PCSPK        = 1 << 0,
    MDT_CMS          = 1 << 1,
    MDT_PCJR         = 1 << 2,
    MDT_ADLIB        = 1 << 3,
    MDT_C64          = 1 << 4,
    MDT_AMIGA        = 1 << 5,
    MDT_APPLEIIGS    = 1 << 6,
    MDT_TOWNS        = 1 << 7,
    MDT_PC98         = 1 << 8,
    MDT_SEGACD       = 1 << 9,
    MDT_GM           = 1 << 10,
    MDT_MT32         = 1 << 11,
    MDT_MIDI         = MDT_GM | MDT_MT32,
    MDT_MACINTOSH    = 1 << 13,
    MDT_PREFER_MT32  = 1 << 14,
    MDT_PREFER_GM    = 1 << 15,
    MDT_PREFER_FLUID = 1 << 16,
};

// Audio CD manager stub — Status struct used for save/load.
class AudioCDManager {
public:
    struct Status {
        bool playing = false;
        int  track = 0, start = 0, duration = 0, numLoops = 0;
        int  volume = 0, balance = 0;
    };
};

class MidiDriver_BASE {
public:
    virtual ~MidiDriver_BASE() {}
    virtual void send(uint32 b) {}
    virtual void sysEx(const byte *msg, uint16 length) {}
    virtual void metaEvent(byte type, byte *data, uint16 length) {}
};

class MidiDriver : public MidiDriver_BASE {
public:
    enum { MERR_CANNOT_CONNECT = 1, MERR_DEVICE_NOT_AVAILABLE = 2 };
    enum DeviceHandle { kDeviceHandleNone = 0 };
    enum DeviceStringType { kDriverName = 0 };
    virtual int open() { return 0; }
    virtual void close() {}
    virtual bool isOpen() const { return false; }
    virtual uint32 getBaseTempo() { return 1000000 / 60; }
    virtual class MidiChannel *allocateChannel() { return nullptr; }
    virtual class MidiChannel *getPercussionChannel() { return nullptr; }
};

class MidiChannel {
public:
    virtual ~MidiChannel() {}
};

#endif
