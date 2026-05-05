// Stub — savegame thumbnail support.  Out of scope.
#ifndef GRAPHICS_THUMBNAIL_H_STUB
#define GRAPHICS_THUMBNAIL_H_STUB
#include "common/scummsys.h"
#include "common/stream.h"
#include "graphics/surface.h"
namespace Graphics {
inline bool checkThumbnailHeader(Common::SeekableReadStream &) { return false; }
inline bool createThumbnail(Common::WriteStream &) { return false; }
inline bool createThumbnail(Graphics::Surface &, const byte * = nullptr) { return false; }
inline bool loadThumbnail(Common::SeekableReadStream &, Graphics::Surface *&, bool = true) { return false; }
inline bool saveThumbnail(Common::WriteStream &) { return false; }
inline bool skipThumbnail(Common::SeekableReadStream &) { return false; }
}
#endif
