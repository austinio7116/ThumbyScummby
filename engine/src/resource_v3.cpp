/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// ThumbyScummby port of the small subset of upstream
// engines/scumm/resource_v3.cpp that the linker needs: the no-op
// readRoomsOffsets override (v3 has no per-room offset table
// inside the master file — every room is its own .LFL on disk) and
// the v3 charset loader.  Mirrors upstream commit a8e08be.

#include "scumm/scumm_v3.h"
#include "scumm/resource.h"
#include "common/file.h"
#include "common/str.h"

namespace Scumm {

void ScummEngine_v3::readRoomsOffsets() {
    // V3 stores each room as its own NN.LFL file, so there's no
    // packed offset table inside the master to read.  Intentionally
    // empty — matches upstream.
}

void ScummEngine_v3::loadCharset(int no) {
    uint32 size;
    memset(_charsetData, 0, sizeof(_charsetData));

    assertRange(0, no, 2, "charset");
    closeRoom();

    Common::File file;
    char buf[20];

    Common::sprintf_s(buf, "%02d.LFL", 99 - no);
    file.open(Common::Path(buf));

    if (!file.isOpen()) {
        error("loadCharset(%d): Missing file charset: %s", no, buf);
    }

    size = file.readUint16LE();
    file.read(_res->createResource(rtCharset, no, size), size);
}

} // End of namespace Scumm
