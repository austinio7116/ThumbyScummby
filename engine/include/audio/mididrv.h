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
    bool open() { return false; }
    bool open(const Common::String &) { return false; }
    void close() {}
    void play(int, int, int, int, bool = false, bool = false) {}
    void stop() {}
    bool isPlaying() { return false; }
    void setVolume(byte) {}
    void setBalance(int8) {}
    void update() {}
    Status getStatus() { return Status(); }
};

class MidiDriver_BASE {
public:
    virtual ~MidiDriver_BASE() {}
    virtual void send(uint32 b) {}
    virtual void sysEx(const byte *msg, uint16 length) {}
    virtual void metaEvent(byte type, byte *data, uint16 length) {}
};

// MusicType — return type of MidiDriver::getMusicType().  Distinct from
// MidiDriverFlags (which is a bitmask of devices to try).
enum MusicType {
    MT_INVALID  = -1,
    MT_AUTO     = 0,
    MT_NULL     = 1,
    MT_PCSPK    = 2,
    MT_PCJR     = 3,
    MT_CMS      = 4,
    MT_ADLIB    = 5,
    MT_C64      = 6,
    MT_AMIGA    = 7,
    MT_APPLEIIGS = 8,
    MT_TOWNS    = 9,
    MT_PC98     = 10,
    MT_SEGACD   = 11,
    MT_GM       = 12,
    MT_MT32     = 13,
    MT_GS       = 14,
    MT_MAC      = 15,
    MT_MACINTOSH = MT_MAC,
};

class MidiDriver : public MidiDriver_BASE {
public:
    enum { MERR_CANNOT_CONNECT = 1, MERR_DEVICE_NOT_AVAILABLE = 2 };
    typedef uint32 DeviceHandle;
    enum DeviceStringType { kDriverName = 0, kDriverId = 1 };
    virtual int open() { return 0; }
    virtual void close() {}
    virtual bool isOpen() const { return false; }
    virtual uint32 getBaseTempo() { return 1000000 / 60; }
    virtual class MidiChannel *allocateChannel() { return nullptr; }
    virtual class MidiChannel *getPercussionChannel() { return nullptr; }

    // Pretend AdLib is the active device so scummvm's setupMusic() picks
    // the AdLib code path (-> _sound->_musicType = MDT_ADLIB).  Our
    // audio_shim.cpp routes Sound::startSound straight to imuse +
    // dbopl, so the actual MidiDriver isn't used; we just need scummvm
    // to *believe* a usable AdLib device exists.  Returning MT_NULL
    // makes setupMusic mark the engine as silent and the boot/title
    // scripts never start the theme music (audit: 2026-05-06).
    static DeviceHandle detectDevice(int /*flags*/) { return 0; }
    static MusicType getMusicType(DeviceHandle /*handle*/) { return MT_ADLIB; }
    static Common::String getDeviceString(DeviceHandle, DeviceStringType) {
        return Common::String();
    }
    // Return a singleton fake driver whose property() / open() / send()
    // are all no-ops.  scumm.cpp:setupMusic dereferences this to call
    // ->property(...), so it must NOT be null.  Our actual audio path
    // bypasses MidiDriver entirely (audio_shim.cpp routes Sound::startSound
    // straight into imuse_start_sound + dbopl).
    static MidiDriver *createMidi(DeviceHandle);

    enum {
        PROP_OLD_ADLIB           = 1,
        PROP_SCUMM_OPL3          = 2,
        PROP_CHANNEL_MASK        = 3,
        PROP_TIMER               = 4,
        PROP_USER_VOLUME_SCALING = 5,
    };
    virtual uint32 property(int /*prop*/, uint32 /*value*/) { return 0; }
};

class MidiChannel {
public:
    virtual ~MidiChannel() {}
};

#endif
