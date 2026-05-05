#ifndef BASE_PLUGINS_H_STUB
#define BASE_PLUGINS_H_STUB
#include "common/scummsys.h"
#include "common/str.h"
#include "common/array.h"
enum PluginType {
    PLUGIN_TYPE_ENGINE_DETECTION = 0,
    PLUGIN_TYPE_ENGINE,
    PLUGIN_TYPE_MUSIC,
    PLUGIN_TYPE_DETECTION,
    PLUGIN_TYPE_SCALER,
    PLUGIN_TYPE_MAX,
};
class Plugin {};
typedef Common::Array<const Plugin *> PluginList;
class PluginObject {
public:
    virtual ~PluginObject() {}
    virtual const char *getName() const { return ""; }
    virtual const char *getEngineName() const { return ""; }
};
class MetaEnginePluginObject : public PluginObject {};
#endif
