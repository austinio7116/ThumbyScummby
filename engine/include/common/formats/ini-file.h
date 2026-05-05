#ifndef COMMON_FORMATS_INI_FILE_H_STUB
#define COMMON_FORMATS_INI_FILE_H_STUB
#include "common/scummsys.h"
#include "common/str.h"
#include "common/list.h"
namespace Common {
class INIFile {
public:
    typedef Common::List<Common::String> SectionList;
    bool getKey(const String &, const String &, String &) const { return false; }
    bool hasKey(const String &, const String &) const { return false; }
};
}
#endif
