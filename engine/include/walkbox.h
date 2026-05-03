// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — walkbox graph + pathfinding for SCUMM v4/v5.
//
// A walkbox is a quadrilateral region of the room where actors may walk.
// Boxes share edges; the engine pre-computes a "next box on shortest path"
// matrix once per room load. At runtime, an actor walking from A to B looks
// up matrix[A][B] to find the next box, computes the gate (edge between
// the two boxes) closest to the destination, and walks there.
//
// V4 BOXD chunk layout (small_header) — 'BX' tag:
//
//   uint16 LE   num_boxes
//   repeat num_boxes:
//       int16 LE  ulx, uly, urx, ury, lrx, lry, llx, lly  (4 vertices CW)
//       uint8     mask
//       uint8     flags
//       uint16 LE scale
//
// (Some v4 games store num_boxes as a single byte. We accept either.)
//
// V4 BOXM chunk ('BM' inside the room sometimes — but in MI1 floppy this
// chunk is absent, and we compute the matrix ourselves via a Floyd-style
// shortest-path algorithm.)

#pragma once

#include "types.h"

namespace tsb {

constexpr uint8_t BOX_FLAG_X_FLIP    = 0x08;
constexpr uint8_t BOX_FLAG_Y_FLIP    = 0x10;
constexpr uint8_t BOX_FLAG_IGNORE_SCALE = 0x20;
constexpr uint8_t BOX_FLAG_LOCKED    = 0x40;
constexpr uint8_t BOX_FLAG_INVISIBLE = 0x80;

constexpr uint8_t INVALID_BOX = 0xFF;

struct WalkBox {
    int16_t  ulx, uly;
    int16_t  urx, ury;
    int16_t  lrx, lry;
    int16_t  llx, lly;
    uint8_t  mask;
    uint8_t  flags;
    uint16_t scale;
};

struct WalkboxGraph {
    int      num_boxes;
    WalkBox  boxes[MAX_BOXES];
    // matrix[from*MAX_BOXES + to] = next box id on shortest path.
    // INVALID_BOX if no path. matrix[i*MAX_BOXES + i] = i.
    uint8_t  matrix[MAX_BOXES * MAX_BOXES];
    bool     valid;
};

// Parse the BOXD payload and build the shortest-path itinerary matrix.
// `boxm_payload` is reserved for future use (precomputed matrix from BOXM
// chunk); pass an empty Span and the matrix is computed via Floyd-Warshall.
bool walkbox_load(Span boxd_payload, Span boxm_payload, WalkboxGraph *out);

// Find the walkbox containing point (x, y). Returns INVALID_BOX if none.
// Searched in reverse order (highest-indexed match wins, matches v5 logic).
uint8_t walkbox_at(const WalkboxGraph *g, int x, int y);

// Look up the next box on the shortest path from `from` to `to`. Returns
// INVALID_BOX if no path. Returns `to` if `from == to`.
uint8_t walkbox_next(const WalkboxGraph *g, uint8_t from, uint8_t to);

// Compute the closest point on `box` to (px, py). Writes result to
// (*out_x, *out_y). Used by the walking code to pick a "gate" between two
// neighboring boxes when transitioning.
void walkbox_closest_pt(const WalkBox *box, int px, int py,
                        int *out_x, int *out_y);

// Mirror of ScummEngine::checkXYInBoxBounds (boxes.cpp): does the point
// (x,y) lie inside the named box of the current room? Used by
// o5_isActorInBox. Returns false if box is out of range or invalid.
bool walkbox_contains(int box_id, int x, int y);

// Set / get walkbox flag bits — mirrors o5_matrixOps case 1 (setBoxFlags)
// and the runtime side of o5_matrixOps case 4 (createBoxMatrix).
void walkbox_set_flags(int box_id, uint8_t flags);
void walkbox_set_scale(int box_id, uint16_t scale);
// Recompute the itinerary matrix — mirrors createBoxMatrix.
void walkbox_recompute_matrix();

}  // namespace tsb
