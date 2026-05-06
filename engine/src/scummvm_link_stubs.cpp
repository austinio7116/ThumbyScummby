// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — link-time stubs for scummvm symbols whose .cpp files
// we don't compile.  Bodies are no-op; --gc-sections strips the dead
// ones at link time.
//
// Categories below match the dependency tree of files we DO compile.
// When a method gets a real body elsewhere (audio_shim.cpp,
// scummvm_compat.cpp, osystem_thumby.cpp), remove from here.

#include "scummvm_compat.h"

#include "scumm/scumm.h"
#include "scumm/scumm_v0.h"
#include "scumm/scumm_v2.h"
#include "scumm/scumm_v5.h"
#include "scumm/scumm_v6.h"
#include "scumm/he/intern_he.h"
#include "scumm/file.h"
#include "scumm/resource.h"
#include "scumm/charset.h"
#include "scumm/actor.h"

#include "common/system.h"
#include "common/mutex.h"
#include "common/random.h"
#include "common/config-manager.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/macresman.h"
#include "common/archive.h"
#include "common/savefile.h"
#include "common/ustr.h"
#include "common/textconsole.h"
#include "common/rendermode.h"
#include "engines/engine.h"
#include "engines/savestate.h"
#include "engines/util.h"
#include "graphics/cursorman.h"
#include "audio/timestamp.h"
#include "audio/mididrv.h"

// Mutex/StackLock bodies live in scummvm_stubs.cpp.

// ============================================================================
// Common::RandomSource — minimal LCG.
// ============================================================================
namespace Common {
RandomSource::RandomSource(const String &) : _randSeed(1) {}
uint RandomSource::getRandomNumber(uint max) {
    _randSeed = _randSeed * 1103515245u + 12345u;
    return (_randSeed >> 16) % (max ? max + 1 : 1);
}
uint RandomSource::getRandomNumberRng(uint min, uint max) {
    return min + getRandomNumber(max > min ? max - min : 0);
}
int  RandomSource::getRandomNumberRngSigned(int min, int max) {
    return (int)(min + getRandomNumber(max > min ? (uint)(max - min) : 0));
}
}

// ============================================================================
// Common::ConfigManager — singleton, all getters return defaults.
// ============================================================================
namespace Common {
ConfigManager::ConfigManager() {}
bool ConfigManager::getBool(const String &, const String &) const   { return false; }
int  ConfigManager::getInt (const String &, const String &) const   { return 0; }
const String &ConfigManager::get(const String &) const { static String s; return s; }
const String &ConfigManager::get(const String &, const String &) const { static String s; return s; }
Path   ConfigManager::getPath(const String &, const String &) const { return Path(); }
bool   ConfigManager::hasKey(const String &) const                  { return false; }
bool   ConfigManager::hasKey(const String &, const String &) const  { return false; }
void   ConfigManager::setBool(const String &, bool, const String &) {}
void   ConfigManager::setInt (const String &, int, const String &)  {}
void   ConfigManager::set    (const String &, const String &)       {}
void   ConfigManager::set    (const String &, const String &, const String &) {}
void   ConfigManager::removeKey(const String &, const String &)     {}
void   ConfigManager::registerDefault(const String &, const String &) {}
void   ConfigManager::registerDefault(const String &, const char *) {}
void   ConfigManager::registerDefault(const String &, int)          {}
void   ConfigManager::registerDefault(const String &, bool)         {}
void   ConfigManager::registerDefault(const String &, const Path &) {}
void   ConfigManager::flushToDisk() {}
}
template<> Common::ConfigManager *Common::Singleton<Common::ConfigManager>::_singleton = nullptr;

// ============================================================================
// Common::SearchManager / SearchSet — class is mostly inline in
// archive.h (SearchSet has inline ctor); just provide a few non-inline.
// ============================================================================
namespace Common {
SearchManager::SearchManager() {}
void SearchManager::clear() {}                        // anchor for vtable
void SearchSet::add(const String &, Archive *, int, bool) {}
void SearchSet::addSubDirectoriesMatching(const FSNode &, String, bool, int, int, bool) {}
}
template<> Common::SearchManager *Common::Singleton<Common::SearchManager>::_singleton = nullptr;

// ============================================================================
// Common::FSNode
// ============================================================================
namespace Common {
FSNode::FSNode() {}
FSNode::FSNode(const Path &) {}
FSNode::~FSNode() {}
FSNode FSNode::getChild(const String &) const { return FSNode(); }
bool   FSNode::exists() const      { return false; }
bool   FSNode::isDirectory() const { return false; }
bool   FSNode::isReadable() const  { return false; }
String FSNode::getName() const     { return String(); }
Path   FSNode::getPath() const     { return Path(); }
bool   FSNode::getChildren(FSList &, ListMode, bool) const { return false; }
SeekableReadStream *FSNode::createReadStream() const { return nullptr; }
SeekableWriteStream *FSNode::createWriteStream(bool) const { return nullptr; }
U32String FSNode::getDisplayName() const { return U32String(); }
String FSNode::getFileName() const { return String(); }
String FSNode::getRealName() const { return String(); }
SeekableReadStream *FSNode::createReadStreamForAltStream(AltStreamType) const { return nullptr; }
Path FSNode::getPathInArchive() const { return Path(); }
void FSNode::listChildren(ArchiveMemberList &, const char *) const {}

ArchiveMember::~ArchiveMember() {}
bool ArchiveMember::isInMacArchive() const { return false; }
U32String ArchiveMember::getDisplayName() const { return U32String(); }
bool ArchiveMember::isDirectory() const { return false; }
void ArchiveMember::listChildren(ArchiveMemberList &, const char *) const {}
// AltStream interface — not used in our path.
}  // close namespace Common briefly for #include
#include "platform.h"
#include "common/memstream.h"
namespace Common {

// Common::File — backed by tsb::platform::data_* chunk readers.  scummvm
// opens 000.LFL / DISK01.LEC..DISK04.LEC / 901.LFL..904.LFL by name; we
// resolve those via platform helpers.  All other paths fail.
File::File()  : _handle(nullptr) {}
File::~File() { delete _handle; }

bool File::open(const Path &p) {
    String name = p.baseName();
    tsb::platform::log("File::open trying name='%s'\n", name.c_str());
    tsb::Span s{};
    if (name.equalsIgnoreCase("000.LFL"))      s = tsb::platform::data_master_index();
    else if (name.equalsIgnoreCase("DISK01.LEC")) s = tsb::platform::data_disk(1);
    else if (name.equalsIgnoreCase("DISK02.LEC")) s = tsb::platform::data_disk(2);
    else if (name.equalsIgnoreCase("DISK03.LEC")) s = tsb::platform::data_disk(3);
    else if (name.equalsIgnoreCase("DISK04.LEC")) s = tsb::platform::data_disk(4);
    else if (name.equalsIgnoreCase("901.LFL"))    s = tsb::platform::data_helper(901);
    else if (name.equalsIgnoreCase("902.LFL"))    s = tsb::platform::data_helper(902);
    else if (name.equalsIgnoreCase("903.LFL"))    s = tsb::platform::data_helper(903);
    else if (name.equalsIgnoreCase("904.LFL"))    s = tsb::platform::data_helper(904);
    if (s.data && s.size) {
        delete _handle;
        _handle = new MemoryReadStream((const byte *)s.data, (uint32)s.size);
        _name = name;
        return true;
    }
    return false;
}
bool File::open(const Path &p, Archive &) { return open(p); }
bool File::open(const FSNode &)           { return false; }
bool File::open(SeekableReadStream *, const String &) { return false; }
bool File::exists(const Path &p) {
    File f; bool ok = f.open(p); return ok;
}
void File::close() { delete _handle; _handle = nullptr; _name.clear(); }
bool File::isOpen() const { return _handle != nullptr; }
int64 File::pos() const   { return _handle ? _handle->pos() : 0; }
int64 File::size() const  { return _handle ? _handle->size() : 0; }
bool File::seek(int64 offs, int whence) { return _handle ? _handle->seek(offs, whence) : false; }
bool File::eos() const    { return _handle ? _handle->eos() : true; }
uint32 File::read(void *buf, uint32 sz) { return _handle ? _handle->read(buf, sz) : 0; }
void File::clearErr() { if (_handle) _handle->clearErr(); }
bool File::err() const { return _handle ? _handle->err() : false; }

DumpFile::DumpFile() {}
DumpFile::~DumpFile() {}
bool   DumpFile::open(const Path &, bool)   { return false; }
bool   DumpFile::open(const FSNode &)       { return false; }
void   DumpFile::close()                    {}
bool   DumpFile::isOpen() const             { return false; }
int64  DumpFile::pos() const                { return 0; }
int64  DumpFile::size() const               { return 0; }
bool   DumpFile::seek(int64, int)           { return false; }
bool   DumpFile::flush()                    { return false; }
uint32 DumpFile::write(const void *, uint32){ return 0; }
void   DumpFile::clearErr()                 {}
bool   DumpFile::err() const                { return false; }

MacResManager::MacResManager() {}
MacResManager::~MacResManager() {}
bool   MacResManager::open(const Path &)              { return false; }
bool   MacResManager::open(const Path &, Archive &)   { return false; }
bool   MacResManager::exists(const Path &)            { return false; }
void   MacResManager::close()                          {}
bool   MacResManager::hasResFork() const               { return false; }
uint32 MacResManager::getResLength(uint32, uint16)     { return 0; }
SeekableReadStream *MacResManager::getResource(uint32, uint16) { return nullptr; }
SeekableReadStream *MacResManager::openFileOrDataFork(const Path &) { return nullptr; }
String MacResManager::getResName(uint32, uint16) const { return String(); }
}

// ============================================================================
// Common::SeekableReadStream / ReadStream
// ============================================================================
namespace Common {
char *SeekableReadStream::readLine(char *, size_t, bool) { return nullptr; }
String SeekableReadStream::readLine(bool) { return String(); }
String ReadStream::readString(char, size_t) { return String(); }
uint32 computeStreamMD5AsString(ReadStream &, uint32, bool (*)(void *, int), void *) { return 0; }
}

// ============================================================================
// Common::U32String — encoding methods skipped (str-enc.cpp not compiled).
// ============================================================================
namespace Common {
U32String::U32String(const char *, CodePage)        : BaseString<value_type>() {}
U32String::U32String(const String &, CodePage)      : BaseString<value_type>() {}
U32String  U32String::format(const char *, ...)     { return U32String(); }
U32String  U32String::formatInternal(const U32String *, ...) { return U32String(); }
U32String &U32String::operator=(const U32String &)  { return *this; }
U32String &U32String::operator=(U32String &&)       { return *this; }
// String::decode and U32String::encode live in str-enc.cpp.
}

// ============================================================================
// Common::parseRenderMode
// ============================================================================
namespace Common {
RenderMode parseRenderMode(const String &) { return kRenderDefault; }
}

// ============================================================================
// Common::memsetN / encoding tables — empty data.
// ============================================================================
namespace Common {
void memset16(uint16 *p, uint16 v, size_t n) { while (n--) *p++ = v; }
void memset32(uint32 *p, uint32 v, size_t n) { while (n--) *p++ = v; }

// Encoding tables — uint16[128] per scummvm-upstream/common/enc-internal.h.
// `extern` to give external linkage (default for `const` is internal).
extern const uint16 kASCIIConversionTable[128]            = {0};
extern const uint16 kLatin1ConversionTable[128]           = {0};
extern const uint16 kLatin2ConversionTable[128]           = {0};
extern const uint16 kDos850ConversionTable[128]           = {0};
extern const uint16 kDos862ConversionTable[128]           = {0};
extern const uint16 kDos866ConversionTable[128]           = {0};
extern const uint16 kISO5ConversionTable[128]             = {0};
extern const uint16 kMacRomanConversionTable[128]         = {0};
extern const uint16 kMacCentralEuropeConversionTable[128] = {0};
extern const uint16 kWindows1250ConversionTable[128]      = {0};
extern const uint16 kWindows1251ConversionTable[128]      = {0};
extern const uint16 kWindows1252ConversionTable[128]      = {0};
extern const uint16 kWindows1253ConversionTable[128]      = {0};
extern const uint16 kWindows1254ConversionTable[128]      = {0};
extern const uint16 kWindows1255ConversionTable[128]      = {0};
extern const uint16 kWindows1256ConversionTable[128]      = {0};
extern const uint16 kWindows1257ConversionTable[128]      = {0};
}

// ============================================================================
// debug() / debugN() / gDebugLevel — text routed to platform::log.
// ============================================================================
// gDebugLevel: scummvm convention is "0 = silent for debug(N), printed
// only if level <= gDebugLevel".  We default to 0 = silent, since the
// per-opcode / per-readvar / per-writeVar trace is too noisy for normal
// runs.  Set to 9 to see everything.
int gDebugLevel = 0;
void debug(const char *fmt, ...) {
    if (gDebugLevel < 1) return;
    va_list ap; va_start(ap, fmt); char buf[512]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    tsb::platform::log("%s\n", buf);
}
void debug(int level, const char *fmt, ...) {
    if (level > gDebugLevel) return;
    va_list ap; va_start(ap, fmt); char buf[512]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    tsb::platform::log("%s\n", buf);
}
void debugN(const char *fmt, ...) {
    if (gDebugLevel < 1) return;
    va_list ap; va_start(ap, fmt); char buf[512]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    tsb::platform::log("%s", buf);
}
void debugN(int level, const char *fmt, ...) {
    if (level > gDebugLevel) return;
    va_list ap; va_start(ap, fmt); char buf[512]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    tsb::platform::log("%s", buf);
}
namespace tsb {
// debugC: scummvm filters by channel mask (kDebugScripts/Opcodes/Vars/etc).
// We silence by default — these prints are extremely noisy (per-opcode).
// Flip kDebugCEnable to true to enable.
static const bool kDebugCEnable = false;
void debugC(int, const char *fmt, ...) {
    if (!kDebugCEnable) return;
    va_list ap; va_start(ap, fmt); char buf[512]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    platform::log("%s\n", buf);
}
void debugC(int, int, const char *fmt, ...) {
    if (!kDebugCEnable) return;
    va_list ap; va_start(ap, fmt); char buf[512]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    platform::log("%s\n", buf);
}
}

// ============================================================================
// OSystem base — most virtual methods overridden in OSystem_Thumby.
// ============================================================================
OSystem::OSystem() {}
OSystem::~OSystem() {}
void OSystem::initBackend() {}
void OSystem::addSysArchivesToSearchSet(Common::SearchSet &, int) {}
Common::SeekableReadStream *OSystem::createConfigReadStream() { return nullptr; }
Common::WriteStream *OSystem::createConfigWriteStream() { return nullptr; }
void OSystem::fatalError() { abort(); }
Common::Path OSystem::getDefaultConfigFileName() { return Common::Path(); }
class FilesystemFactory *OSystem::getFilesystemFactory() { return nullptr; }
Common::Rect OSystem::getSafeOverlayArea(int16 *, int16 *) const { return Common::Rect(); }
Common::SaveFileManager *OSystem::getSavefileManager() { return nullptr; }
Common::String OSystem::getSystemLanguage() const { return Common::String(); }
Common::TimerManager *OSystem::getTimerManager() { return nullptr; }
bool OSystem::isConnectionLimited() { return false; }
void OSystem::updateStartSettings(const Common::String &, Common::String &,
                                   Common::HashMap<Common::String, Common::String, Common::IgnoreCase_Hash, Common::IgnoreCase_EqualTo> &,
                                   Common::Array<Common::String> &) {}

// ============================================================================
// Engine base
// ============================================================================
// Anchor for MetaEngine vtable + bodies for all non-inline non-pure
// virtuals.  Without these, GCC emits no vtable.
void MetaEngine::getSavegameThumbnail(Graphics::Surface &) {}
int MetaEngine::findEmptySaveSlot(const char *) { return -1; }
void MetaEngine::deleteInstance(Engine *, const DetectedGame &, const void *) {}
SaveStateList MetaEngine::listSaves(const char *) const { return SaveStateList(); }
bool MetaEngine::removeSaveState(const char *, int) const { return false; }
SaveStateDescriptor MetaEngine::querySaveMetaInfos(const char *, int) const { return SaveStateDescriptor(); }
Common::String MetaEngine::getSavegameFile(int, const char *) const { return Common::String(); }
Common::Array<Common::Keymap *> MetaEngine::initKeymaps(const char *) const { return Common::Array<Common::Keymap *>(); }
void MetaEngine::registerDefaultSettings(const Common::String &) const {}
GUI::OptionsContainerWidget *MetaEngine::buildEngineOptionsWidget(GUI::GuiObject *, const Common::String &, const Common::String &) const { return nullptr; }
Common::AchievementsPlatform MetaEngine::getAchievementsPlatform(const Common::String &) const { return Common::UNK_ACHIEVEMENTS; }
const Common::AchievementsInfo MetaEngine::getAchievementsInfo(const Common::String &) const { return Common::AchievementsInfo(); }
bool MetaEngine::hasFeature(MetaEngineFeature) const { return false; }

// Stub MetaEngine — autosave-rename block in ScummEngine::go() calls
// querySaveMetaInfos/listSaves; we return empty lists so the block is a no-op.
namespace {
class StubMetaEngine : public MetaEngine {
public:
    const char *getName() const override { return "thumbyscummby"; }
    Common::Error createInstance(OSystem *, Engine **, const DetectedGame &, const void *) override { return Common::kNoError; }
    void getSavegameThumbnail(Graphics::Surface &) override {}
    void deleteInstance(Engine *, const DetectedGame &, const void *) override {}
    SaveStateList listSaves(const char *) const override { return SaveStateList(); }
    bool removeSaveState(const char *, int) const override { return false; }
    SaveStateDescriptor querySaveMetaInfos(const char *, int) const override { return SaveStateDescriptor(); }
    Common::String getSavegameFile(int, const char *) const override { return Common::String(); }
    Common::Array<Common::Keymap *> initKeymaps(const char *) const override { return Common::Array<Common::Keymap *>(); }
    void registerDefaultSettings(const Common::String &) const override {}
    GUI::OptionsContainerWidget *buildEngineOptionsWidget(GUI::GuiObject *, const Common::String &, const Common::String &) const override { return nullptr; }
    Common::AchievementsPlatform getAchievementsPlatform(const Common::String &) const override { return Common::UNK_ACHIEVEMENTS; }
    const Common::AchievementsInfo getAchievementsInfo(const Common::String &) const override { return Common::AchievementsInfo(); }
    bool hasFeature(MetaEngineFeature) const override { return false; }
};
StubMetaEngine s_stub_metaengine;
}

Engine::Engine(OSystem *syst) : _system(syst),
    _mixer(syst ? syst->getMixer() : nullptr),
    _eventMan(syst ? syst->getEventManager() : nullptr),
    _saveFileMan(nullptr),
    _metaEngine(&s_stub_metaengine),
    _timer(nullptr) {}
Engine::~Engine() {}
bool Engine::canLoadGameStateCurrently(Common::U32String *) { return false; }
bool Engine::canSaveGameStateCurrently(Common::U32String *) { return false; }
Common::Error Engine::loadGameState(int) { return Common::kNoError; }
Common::Error Engine::saveGameState(int, const Common::String &, bool) { return Common::kNoError; }
void Engine::pauseEngineIntern(bool) {}
void Engine::errorString(const char *, char *, int) {}
bool Engine::dirCanBeGameAddOn(const Common::FSDirectory &) const { return false; }
bool Engine::dirMustBeGameAddOn(const Common::FSDirectory &) const { return false; }
bool Engine::shouldQuit()                               { return false; }
void Engine::quitGame()                                 {}
PauseToken Engine::pauseEngine()                        { return PauseToken(); }
bool Engine::enhancementEnabled(int32)                  { return false; }
void Engine::syncSoundSettings()                        {}
void Engine::initializePath(const Common::FSNode &)     {}
void Engine::flipMute()                                 {}
bool Engine::existExtractedCDAudioFiles(uint)           { return false; }
bool Engine::isDataAndCDAudioReadFromSameCD()           { return false; }
void Engine::warnMissingExtractedCDAudio()              {}
bool Engine::gameTypeHasAddOns() const                  { return false; }
Common::Error Engine::loadGameStream(Common::SeekableReadStream *) { return Common::kNoError; }
Common::Error Engine::saveGameStream(Common::WriteStream *, bool) { return Common::kNoError; }
void Engine::setTotalPlayTime(uint32)                   {}
int  Engine::runDialog(GUI::Dialog &)                   { return 0; }
void Engine::openMainMenuDialog()                       {}

PauseToken::~PauseToken() {}
void PauseToken::clear() {}

void GUIErrorMessage(const Common::String &, const char *) {}

// ============================================================================
// SaveStateDescriptor
// ============================================================================
SaveStateDescriptor::SaveStateDescriptor() {}
SaveStateDescriptor::SaveStateDescriptor(const MetaEngine *, int) {}
SaveStateDescriptor::SaveStateDescriptor(const MetaEngine *, int, const Common::String &) {}
SaveStateDescriptor::SaveStateDescriptor(const MetaEngine *, int, const Common::U32String &) {}
bool SaveStateDescriptor::isAutosave() const { return false; }
bool SaveStateDescriptor::isValid() const { return false; }

// ============================================================================
// Graphics::CursorManager / Primitives — runtime path doesn't reach.
// ============================================================================
namespace Graphics {
void CursorManager::replaceCursor(const void *, uint, uint, int, int, uint32, bool, const PixelFormat *, const byte *) {}
bool CursorManager::showMouse(bool) { return false; }
CursorManager::~CursorManager() {}
}
template<> Graphics::CursorManager *Common::Singleton<Graphics::CursorManager>::_singleton = nullptr;

// initGraphics() lives in engines/util.cpp.
void initGraphics(int, int) {}
void initGraphics(int, int, const Graphics::PixelFormat *) {}

// Graphics::FontTowns::getCharFMTChunk - static.
namespace Graphics {
int FontTowns::getCharFMTChunk(uint16) { return 0; }
}

// ----------------------------------------------------------------------
// Vtable anchors — for classes whose .cpp we don't link, vtable lives
// where the first non-inline virtual is defined.  Placing one here.
// ----------------------------------------------------------------------
namespace Common {
class MemoryReadStream;
}
// Force out-of-line bodies: declare-and-define a single virtual per class
// here.  Each class's other virtuals come from inline impls in the header.
namespace Common {
// Archive base class — anchor.
bool Archive::isPathDirectory(const Path &) const { return false; }
int  Archive::listMembers(ArchiveMemberList &) const { return 0; }
int  Archive::listMatchingMembers(ArchiveMemberList &, const Path &, bool) const { return 0; }
const ArchiveMemberPtr Archive::getMember(const Path &) const { return ArchiveMemberPtr(); }
SeekableReadStream *Archive::createReadStreamForMemberAltStream(const Path &, AltStreamType) const { return nullptr; }
char Archive::getPathSeparator() const { return '/'; }
bool Archive::getChildren(const Path &, Common::Array<Common::String> &, ListMode, bool) const { return false; }

// SearchSet inherits Archive — wired to our chunk readers so
// scummvm's SearchMan.createReadStreamForMember finds 000.LFL etc.
SeekableReadStream *SearchSet::createReadStreamForMember(const Path &p) const {
    String name = p.baseName();
    tsb::Span s{};
    if (name.equalsIgnoreCase("000.lfl"))      s = tsb::platform::data_master_index();
    else if (name.equalsIgnoreCase("disk01.lec")) s = tsb::platform::data_disk(1);
    else if (name.equalsIgnoreCase("disk02.lec")) s = tsb::platform::data_disk(2);
    else if (name.equalsIgnoreCase("disk03.lec")) s = tsb::platform::data_disk(3);
    else if (name.equalsIgnoreCase("disk04.lec")) s = tsb::platform::data_disk(4);
    else if (name.equalsIgnoreCase("901.lfl"))    s = tsb::platform::data_helper(901);
    else if (name.equalsIgnoreCase("902.lfl"))    s = tsb::platform::data_helper(902);
    else if (name.equalsIgnoreCase("903.lfl"))    s = tsb::platform::data_helper(903);
    else if (name.equalsIgnoreCase("904.lfl"))    s = tsb::platform::data_helper(904);
    if (s.data && s.size)
        return new MemoryReadStream((const byte *)s.data, (uint32)s.size);
    return nullptr;
}
SeekableReadStream *SearchSet::createReadStreamForMemberNext(const Path &, const Archive *) const { return nullptr; }
bool SearchSet::hasFile(const Path &) const { return false; }
int  SearchSet::listMembers(ArchiveMemberList &) const { return 0; }
int  SearchSet::listMatchingMembers(ArchiveMemberList &, const Path &, bool) const { return 0; }
const ArchiveMemberPtr SearchSet::getMember(const Path &) const { return ArchiveMemberPtr(); }
void SearchSet::clear() {}                            // anchor for vtable
bool SearchSet::isPathDirectory(const Path &) const { return false; }
SeekableReadStream *SearchSet::createReadStreamForMemberAltStream(const Path &, AltStreamType) const { return nullptr; }
bool SearchSet::getChildren(const Path &, Common::Array<Common::String> &, Archive::ListMode, bool) const { return false; }
}

// Actor_v0/v2/v3/v7/HE — concrete subclasses we never instantiate.
// They have lots of virtuals but bodies are in their .cpp files we
// don't compile.  --gc-sections should strip them, but the linker
// still wants a vtable somewhere.  Override one harmless virtual per
// subclass to anchor.
namespace tsb {
// vtables for Actor_v0/v2/v3/v7 — never instantiated.  Don't anchor;
// linker errors are about unresolved vtable refs from never-called code.
}

// Audio::Timestamp ctor — used by NullMixer.
namespace Audio {
Timestamp::Timestamp(uint, uint) : _secs(0), _numFrames(0), _framerate(22050), _framerateFactor(1) {}
}

// Common::EventManager + EventDispatcher anchors.
namespace Common {
EventManager::~EventManager() {}
EventDispatcher::EventDispatcher() {}
EventDispatcher::~EventDispatcher() {}
}

// Common::MemoryReadStream — vtable anchor.  Provide real `read` body.
namespace Common {
uint32 MemoryReadStream::read(void *dataPtr, uint32 dataSize) {
    if (_pos + dataSize > _size) {
        dataSize = _size - _pos;
        _eos = true;
    }
    if (dataSize) memcpy(dataPtr, _ptr + _pos, dataSize);
    _pos += dataSize;
    return dataSize;
}
// pos / size are inline in memstream.h.
bool MemoryReadStream::seek(int64 offs, int whence) {
    switch (whence) {
    case SEEK_SET: _pos = (uint32)offs; break;
    case SEEK_CUR: _pos += (uint32)offs; break;
    case SEEK_END: _pos = _size + (uint32)offs; break;
    }
    _eos = false;
    return _pos <= _size;
}
}

// PauseToken move-assign.
void PauseToken::operator=(PauseToken &&) {}
void PauseToken::operator=(const PauseToken &) {}

// ============================================================================
// scumm/file — bodies for ScummFile / ScummDiskImage / ScummSteamFile /
// ScummPAKFile; we use chunk readers, not these classes.
// ============================================================================
// (Provided by upstream file.cpp / file_engine.cpp / file_nes.cpp now.)

// ============================================================================
// scumm — Actor / ActorHE / Actor_v0 methods that are in transcribed cpps
// we don't link.
// ============================================================================
namespace tsb {
// runActorTalkScript: real impl now lives in actor.cpp (talk pipeline).
void Actor_v0::limbFrameCheck(int) {}
ActorHE::ActorHE(ScummEngine *vm, int id) : Actor(vm, id) {}
}

// ============================================================================
// ScummEngine — methods whose .cpps we don't compile.
// ============================================================================
namespace tsb {
// Save/load — out of MVP.  Bodies match real signatures in scumm.h.
Common::Error ScummEngine::loadGameState(int) { return Common::kNoError; }
Common::Error ScummEngine::saveGameState(int, const Common::String &, bool) { return Common::kNoError; }
bool ScummEngine::canLoadGameStateCurrently(Common::U32String *) { return false; }
bool ScummEngine::canSaveGameStateCurrently(Common::U32String *) { return false; }
bool ScummEngine::loadState(int, bool) { return false; }
bool ScummEngine::loadState(int, bool, Common::String &) { return false; }
bool ScummEngine::saveState(int, bool, Common::String &) { return false; }
bool ScummEngine::saveState(Common::SeekableWriteStream *, bool) { return false; }
void ScummEngine::requestLoad(int) {}
bool ScummEngine::changeSavegameName(int, char *) { return false; }
bool ScummEngine::getSavegameName(int, Common::String &) { return false; }
Common::String ScummEngine::makeSavegameName(const Common::String &, int, bool) { return Common::String(); }
void ScummEngine::listSavegames(bool *, int) {}
Common::SeekableReadStream *ScummEngine::openSaveFileForReading(int, bool, Common::String &) { return nullptr; }
Common::SeekableWriteStream *ScummEngine::openSaveFileForWriting(int, bool, Common::String &) { return nullptr; }
int  ScummEngine::checkSoundEngineSaveDataSize(Serializer &) { return 0; }
void ScummEngine::saveLoadWithSerializer(Common::Serializer &) {}

// Banner / GUI — out of MVP.
Common::KeyState ScummEngine::showBannerAndPause(int, int, const char *, ...) { return Common::KeyState(); }
bool ScummEngine::showBannerAndPauseForTextInput(int, const char *, Common::String &, uint) { return false; }
Common::KeyState ScummEngine::showOldStyleBannerAndPause(const char *, int, int) { return Common::KeyState(); }
Common::KeyState ScummEngine::printMessageAndPause(const char *, int, int, bool) { return Common::KeyState(); }
void ScummEngine::clearBanner() {}
int  ScummEngine::getBannerColor(int) { return 0; }
void ScummEngine::initBanners() {}
void ScummEngine::setBannerColors(int, byte, byte, byte) {}
void ScummEngine::showMainMenu() {}
void ScummEngine::showDraftsInventory() {}
void ScummEngine::setUpMainMenuControls() {}
const char *ScummEngine::getGUIString(int) { return ""; }
int  ScummEngine::getGUIStringHeight(const char *) { return 0; }
int  ScummEngine::getGUIStringWidth(const char *)  { return 0; }
void ScummEngine::getSliderString(int, int, char *, int) {}
void ScummEngine::drawGUIText(const char *, Common::Rect *, int, int, int, bool) {}
void ScummEngine::queryQuit(bool) {}
void ScummEngine::queryRestart() {}
void ScummEngine::toggleVoiceMode() {}
int  ScummEngine::getMusicVolume() { return 0; }
int  ScummEngine::getSFXVolume()   { return 0; }
int  ScummEngine::getSpeechVolume() { return 0; }
void ScummEngine::setMusicVolume(int) {}
void ScummEngine::setSFXVolume(int) {}
void ScummEngine::setSpeechVolume(int) {}

// Resource indexing — bodies in upstream resource.cpp now.

// Mac / NES paths.
void ScummEngine::mac_drawBufferToScreen(const byte *, int, int, int, int, int, bool) {}
void ScummEngine::mac_drawIndy3TextBox() {}
void ScummEngine::mac_drawStripToScreen(VirtScreen *, int, int, int, int, int) {}
void ScummEngine::mac_scaleCursor(byte *&, int &, int &, int &, int &) {}
void ScummEngine::mac_toggleSmoothing() {}
void ScummEngine::mac_undrawIndy3CreditsText() {}
void ScummEngine::mac_undrawIndy3TextBox() {}
void ScummEngine::playNESTitleScreens() {}

// Misc.
// actorTalk / resetV1ActorTalkColor are defined in actor.cpp now.
// processActors / resetActorBgs / setActorRedrawFlags / redrawAllActors
// have real implementations in actor.cpp now (enabled along with
// drawActorCostume + prepareDrawActorCostume to bring sparkles, clouds,
// and the rest of the actor rendering online).
bool ScummEngine::hasFeature(Engine::EngineFeature) const { return false; }
// generateFilename: real impl from scummvm-upstream/scumm/metaengine.cpp:53.
// We only support v4 path (MI1).
Common::Path ScummEngine::generateFilename(int room) const {
    Common::String result;
    if (_game.version == 4) {
        if (room == 0 || room >= 900)
            result = Common::String::format("%03d.lfl", room);
        else
            result = Common::String::format("disk%02d.lec",
                                            _res->_types[rtRoom][room]._roomno);
    }
    return Common::Path(result, Common::Path::kNoSeparator);
}
int  ScummEngine::readSoundResource(uint16) { return 0; }
// readSoundResourceSmallHeader is implemented in audio_shim.cpp.
// verifyMI2MacBootScript lives in resource.cpp.

// Playback (debug demo recording).
void ScummEngine::Playback::reset() {}
bool ScummEngine::Playback::startPlayback(ScummEngine *) { return false; }
void ScummEngine::Playback::playbackPump(ScummEngine *) {}
void ScummEngine::Playback::mi2DemoArmPlaybackByRoom(ScummEngine *) {}
void ScummEngine::Playback::mi2DemoPlaybackJumpRoom(ScummEngine *, int) {}
bool ScummEngine::Playback::tryLoadPlayback(ScummEngine *, const Common::Path &) { return false; }
}

// ============================================================================
// ScummEngine version subclasses — never instantiated, --gc-sections strips.
// ============================================================================
namespace tsb {
// v0 / v2 / v3old / v6 / v60he / v70he method bodies
// stopTalk / setTalkingActor / getTalkingActor: real impls in actor.cpp.

// ScummEngine_v5::canSaveGameStateCurrently is non-virtual override in v5.h

// v0
bool ScummEngine_v0::areBoxesNeighbors(int, int) { return false; }
int  ScummEngine_v0::checkSoundEngineSaveDataSize(Serializer &) { return 0; }
void ScummEngine_v0::decodeParseString() {}
void ScummEngine_v0::drawSentenceLine() {}
uint ScummEngine_v0::fetchScriptWord() { return 0; }
int  ScummEngine_v0::getActiveObject() { return 0; }
int  ScummEngine_v0::getVarOrDirectWord(byte) { return 0; }
void ScummEngine_v0::o_endCutscene() {}
void ScummEngine_v0::resetSentence() {}
void ScummEngine_v0::saveLoadWithSerializer(Common::Serializer &) {}
void ScummEngine_v0::setupOpcodes() {}

// v5
int  ScummEngine_v5::checkSoundEngineSaveDataSize(Serializer &) { return 0; }
// readMAXS lives in resource.cpp.
void ScummEngine_v5::saveLoadWithSerializer(Common::Serializer &) {}

// v4 — most bodies in resource_v4.cpp; banner/menu stubs here.
int  ScummEngine_v4::getBannerColor(int) { return 0; }
void ScummEngine_v4::setUpMainMenuControls() {}
}

// ============================================================================
// MidiDriver — fake driver returned by createMidi().
// scumm.cpp::setupMusic() dereferences createMidi(...)->property(...), so
// it must NOT be null.  Our actual audio path bypasses MidiDriver entirely
// (audio_shim.cpp routes Sound::startSound → imuse_start_sound + dbopl).
// ============================================================================
namespace {
class FakeMidiDriver : public MidiDriver {
public:
    int open() override { return 0; }
    void close() override {}
    bool isOpen() const override { return true; }
    uint32 getBaseTempo() override { return 1000000 / 60; }
    MidiChannel *allocateChannel() override { return nullptr; }
    MidiChannel *getPercussionChannel() override { return nullptr; }
    uint32 property(int /*prop*/, uint32 /*value*/) override { return 0; }
    void send(uint32 /*b*/) override {}
};
}

MidiDriver *MidiDriver::createMidi(MidiDriver::DeviceHandle) {
    static FakeMidiDriver s_fake;
    return &s_fake;
}
