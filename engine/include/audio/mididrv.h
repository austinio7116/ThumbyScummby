// Stub — we bypass scummvm's MIDI/iMUSE.  This header just has to exist.
#ifndef AUDIO_MIDIDRV_H_STUB
#define AUDIO_MIDIDRV_H_STUB
#include "common/scummsys.h"

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
