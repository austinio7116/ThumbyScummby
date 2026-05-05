// Stub — Clickteam Install Creator decompression (HE games).  Out of scope.
#ifndef COMMON_COMPRESSION_CLICKTEAM_H_STUB
#define COMMON_COMPRESSION_CLICKTEAM_H_STUB
#include "common/scummsys.h"
#include "common/path.h"
namespace Common {
class ClickteamInstaller {
public:
    static ClickteamInstaller *open(const Path &) { return nullptr; }
    SeekableReadStream *createReadStreamForMember(const Path &) { return nullptr; }
};
}
#endif
