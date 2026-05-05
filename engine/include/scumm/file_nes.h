// Stub — NES disk image reader.  Out of scope.
#ifndef SCUMM_FILE_NES_H_STUB
#define SCUMM_FILE_NES_H_STUB
#include "scumm/file.h"
namespace Scumm {
class ScummNESFile : public BaseScummFile {
public:
    int64 pos() const override { return 0; }
    int64 size() const override { return 0; }
    bool seek(int64, int) override { return false; }
    bool open(const Common::Path &) override { return false; }
    bool openSubFile(const Common::Path &) override { return false; }
    bool eos() const override { return true; }
    uint32 read(void *, uint32) override { return 0; }
};
}
#endif
