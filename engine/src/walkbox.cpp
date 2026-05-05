// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby — walkbox graph + pathfinding.
//
// Distilled from scummvm-upstream/engines/scumm/boxes.cpp:
//   - getBoxBaseAddr / getBoxCoordinates : box record layout
//   - checkXYInBoxBounds                 : point-in-quad test
//   - areBoxesNeighbors                  : edge-shared test (16 rotations)
//   - calcItineraryMatrix                : Floyd-Warshall over neighbor graph
//
// Box record (v4 small-header, GF_SMALL_HEADER): 20 bytes, fields LE:
//   int16 ulx, uly, urx, ury, lrx, lry, llx, lly
//   uint8 mask, flags
//   uint16 scale

#include "walkbox.h"
#include "platform.h"

#include <string.h>

namespace tsb {

constexpr int BOX_REC_SIZE = 20;

static int abs_int(int v) { return v < 0 ? -v : v; }

// Parse num_boxes from BOXD payload. v4 small-header MI1 starts with a
// uint16 LE count; some titles use a single byte. Detect by sanity: if the
// would-be 16-bit count produces a record-table that fits the payload, use
// 16-bit; else fall back to 8-bit.
static int parse_num_boxes(Span p, int *header_size) {
    if (p.size < 2) {
        *header_size = 0;
        return 0;
    }
    int as_u16 = (int)read_le16(p.data);
    int as_u8  = p.data[0];
    // Try uint16 first
    if (as_u16 > 0 && as_u16 <= MAX_BOXES &&
        p.size >= (size_t)2 + (size_t)as_u16 * BOX_REC_SIZE) {
        *header_size = 2;
        return as_u16;
    }
    // Try uint8
    if (as_u8 > 0 && as_u8 <= MAX_BOXES &&
        p.size >= (size_t)1 + (size_t)as_u8 * BOX_REC_SIZE) {
        *header_size = 1;
        return as_u8;
    }
    *header_size = 0;
    return 0;
}

static bool point_in_box(const WalkBox *b, int x, int y) {
    // Bounding-box reject
    if (x < b->ulx && x < b->urx && x < b->lrx && x < b->llx) return false;
    if (x > b->ulx && x > b->urx && x > b->lrx && x > b->llx) return false;
    if (y < b->uly && y < b->ury && y < b->lry && y < b->lly) return false;
    if (y > b->uly && y > b->ury && y > b->lry && y > b->lly) return false;

    // For each oriented side (CW: ul→ur→lr→ll→ul), the point must be on the
    // correct (right-hand) side. cross = (b-a) × (p-a); for CW polygons
    // we require cross <= 0 for "inside".
    auto cross = [](int ax, int ay, int bx, int by, int px, int py) {
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    };
    int c1 = cross(b->ulx, b->uly, b->urx, b->ury, x, y);
    int c2 = cross(b->urx, b->ury, b->lrx, b->lry, x, y);
    int c3 = cross(b->lrx, b->lry, b->llx, b->lly, x, y);
    int c4 = cross(b->llx, b->lly, b->ulx, b->uly, x, y);
    if (c1 <= 0 && c2 <= 0 && c3 <= 0 && c4 <= 0) return true;
    if (c1 >= 0 && c2 >= 0 && c3 >= 0 && c4 >= 0) return true;
    return false;
}

// Test whether two boxes share an edge. Equivalent to ScummVM
// `areBoxesNeighbors` (scummvm-upstream/engines/scumm/boxes.cpp:1094):
// upstream rotates each box 4 ways (16 comparisons) and matches the
// rotated upper sides. We instead iterate the same 4×4 = 16 directed
// edge pairs and apply a general collinear-segment overlap test, so
// oblique edges (e.g. ramp / diagonal walkboxes) are handled the same
// as axis-aligned ones rather than being ignored.
static bool boxes_share_edge(const WalkBox *a, const WalkBox *b) {
    if ((a->flags & BOX_FLAG_INVISIBLE) || (b->flags & BOX_FLAG_INVISIBLE))
        return false;

    struct Pt { int32_t x, y; };
    auto cross_z = [](Pt u, Pt v) -> int64_t {
        return (int64_t)u.x * v.y - (int64_t)u.y * v.x;
    };
    Pt av[4] = {
        {a->ulx, a->uly}, {a->urx, a->ury},
        {a->lrx, a->lry}, {a->llx, a->lly},
    };
    Pt bv[4] = {
        {b->ulx, b->uly}, {b->urx, b->ury},
        {b->lrx, b->lry}, {b->llx, b->lly},
    };

    for (int i = 0; i < 4; i++) {
        Pt p1 = av[i];
        Pt p2 = av[(i + 1) & 3];
        Pt d  = { p2.x - p1.x, p2.y - p1.y };
        if (d.x == 0 && d.y == 0) continue;             // degenerate edge
        for (int j = 0; j < 4; j++) {
            Pt q1 = bv[j];
            Pt q2 = bv[(j + 1) & 3];

            // Two segments share a sub-edge iff (1) all 4 points are
            // collinear with edge p1->p2 (cross product of d with the
            // vector to each q is 0) and (2) their projections onto d
            // overlap. The projection just dot-products with d; sign
            // and ordering are preserved by integer arithmetic.
            Pt qd1 = { q1.x - p1.x, q1.y - p1.y };
            Pt qd2 = { q2.x - p1.x, q2.y - p1.y };
            if (cross_z(d, qd1) != 0 || cross_z(d, qd2) != 0) continue;

            int64_t len2 = (int64_t)d.x * d.x + (int64_t)d.y * d.y;  // > 0
            int64_t t_p1 = 0;
            int64_t t_p2 = len2;
            int64_t t_q1 = (int64_t)d.x * qd1.x + (int64_t)d.y * qd1.y;
            int64_t t_q2 = (int64_t)d.x * qd2.x + (int64_t)d.y * qd2.y;
            int64_t p_lo = t_p1 < t_p2 ? t_p1 : t_p2;
            int64_t p_hi = t_p1 < t_p2 ? t_p2 : t_p1;
            int64_t q_lo = t_q1 < t_q2 ? t_q1 : t_q2;
            int64_t q_hi = t_q1 < t_q2 ? t_q2 : t_q1;
            // Overlap means the closed intervals intersect; a single
            // shared corner point still counts as a shared edge to
            // upstream (ScummVM treats touching boxes as neighbours
            // for path-finding adjacency).
            if (p_lo <= q_hi && q_lo <= p_hi) return true;
        }
    }
    return false;
}

bool walkbox_load(Span boxd_payload, Span /*boxm_payload*/, WalkboxGraph *out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < MAX_BOXES * MAX_BOXES; i++) out->matrix[i] = INVALID_BOX;

    if (boxd_payload.empty()) {
        platform::log("walkbox_load: empty BOXD payload\n");
        return false;
    }

    int hdr = 0;
    int n   = parse_num_boxes(boxd_payload, &hdr);
    if (n <= 0) {
        platform::log("walkbox_load: cannot determine num_boxes (size=%zu, "
                      "first 4 bytes %02x %02x %02x %02x)\n",
                      boxd_payload.size,
                      boxd_payload.data[0],
                      boxd_payload.size > 1 ? boxd_payload.data[1] : 0,
                      boxd_payload.size > 2 ? boxd_payload.data[2] : 0,
                      boxd_payload.size > 3 ? boxd_payload.data[3] : 0);
        return false;
    }
    if (n > MAX_BOXES) n = MAX_BOXES;
    out->num_boxes = n;

    const uint8_t *p = boxd_payload.data + hdr;
    for (int i = 0; i < n; i++) {
        WalkBox &b = out->boxes[i];
        b.ulx = read_le16s(p +  0);
        b.uly = read_le16s(p +  2);
        b.urx = read_le16s(p +  4);
        b.ury = read_le16s(p +  6);
        b.lrx = read_le16s(p +  8);
        b.lry = read_le16s(p + 10);
        b.llx = read_le16s(p + 12);
        b.lly = read_le16s(p + 14);
        b.mask  = p[16];
        b.flags = p[17];
        b.scale = read_le16(p + 18);
        p += BOX_REC_SIZE;
    }

    // Build neighbor (adjacency-distance) matrix locally on stack — at most
    // 64×64 bytes. Then Floyd-Warshall to fill in shortest paths and a
    // "next hop" table.
    static uint8_t adj[MAX_BOXES * MAX_BOXES];
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int idx = i * MAX_BOXES + j;
            if (i == j) {
                adj[idx] = 0;
                out->matrix[idx] = (uint8_t)j;
            } else if (boxes_share_edge(&out->boxes[i], &out->boxes[j])) {
                adj[idx] = 1;
                out->matrix[idx] = (uint8_t)j;
            } else {
                adj[idx] = 255;
                out->matrix[idx] = INVALID_BOX;
            }
        }
    }

    for (int k = 0; k < n; k++) {
        for (int i = 0; i < n; i++) {
            if (i == k) continue;
            uint8_t d_ik = adj[i * MAX_BOXES + k];
            if (d_ik == 255) continue;
            for (int j = 0; j < n; j++) {
                if (i == j) continue;
                uint8_t d_kj = adj[k * MAX_BOXES + j];
                if (d_kj == 255) continue;
                int sum = (int)d_ik + (int)d_kj;
                if (sum > 254) sum = 254;
                if (sum < adj[i * MAX_BOXES + j]) {
                    adj[i * MAX_BOXES + j] = (uint8_t)sum;
                    out->matrix[i * MAX_BOXES + j] =
                        out->matrix[i * MAX_BOXES + k];
                }
            }
        }
    }

    out->valid = true;
    platform::log("walkbox_load: %d boxes parsed\n", n);
    return true;
}

uint8_t walkbox_mask_for_box(const WalkboxGraph *g, int box_id) {
    if (!g || !g->valid) return 0;
    if (box_id < 0 || box_id >= g->num_boxes) return 0;
    return g->boxes[box_id].mask;
}

uint8_t walkbox_at(const WalkboxGraph *g, int x, int y) {
    if (!g || !g->valid) return INVALID_BOX;
    // Iterate in reverse (high-numbered boxes win — matches v5 logic).
    for (int i = g->num_boxes - 1; i >= 0; i--) {
        const WalkBox &b = g->boxes[i];
        if (b.flags & BOX_FLAG_INVISIBLE) continue;
        if (point_in_box(&b, x, y)) return (uint8_t)i;
    }
    return INVALID_BOX;
}

uint8_t walkbox_next(const WalkboxGraph *g, uint8_t from, uint8_t to) {
    if (!g || !g->valid) return INVALID_BOX;
    if (from >= g->num_boxes || to >= g->num_boxes) return INVALID_BOX;
    if (from == to) return to;
    return g->matrix[from * MAX_BOXES + to];
}

// Project (px, py) onto a line segment (ax, ay)-(bx, by). Standard
// closest-point-on-segment formula in integer arithmetic (no sqrt).
static void closest_pt_on_seg(int ax, int ay, int bx, int by,
                              int px, int py, int *ox, int *oy) {
    int dx = bx - ax;
    int dy = by - ay;
    int len2 = dx * dx + dy * dy;
    if (len2 == 0) { *ox = ax; *oy = ay; return; }
    int dotp = (px - ax) * dx + (py - ay) * dy;
    // t = dotp / len2 in [0,1]; clamp.
    if (dotp <= 0)      { *ox = ax; *oy = ay; return; }
    if (dotp >= len2)   { *ox = bx; *oy = by; return; }
    *ox = ax + (int)((int64_t)dotp * dx / len2);
    *oy = ay + (int)((int64_t)dotp * dy / len2);
}

// Provided by engine.cpp — pointer to the current room's WalkboxGraph,
// or nullptr if the room has no walkboxes (e.g. inventory screen).
extern WalkboxGraph *engine_active_walkbox_graph();

bool walkbox_contains(int box_id, int x, int y) {
    WalkboxGraph *g = engine_active_walkbox_graph();
    if (!g || !g->valid) return false;
    if (box_id < 0 || box_id >= g->num_boxes) return false;
    return point_in_box(&g->boxes[box_id], x, y);
}

void walkbox_set_flags(int box_id, uint8_t flags) {
    WalkboxGraph *g = engine_active_walkbox_graph();
    if (!g || !g->valid) return;
    if (box_id < 0 || box_id >= g->num_boxes) return;
    g->boxes[box_id].flags = flags;
}

void walkbox_set_scale(int box_id, uint16_t scale) {
    WalkboxGraph *g = engine_active_walkbox_graph();
    if (!g || !g->valid) return;
    if (box_id < 0 || box_id >= g->num_boxes) return;
    g->boxes[box_id].scale = scale;
}

// Mirrors ScummEngine::createBoxMatrix (boxes.cpp): rebuild the
// itinerary matrix using the current set of boxes/flags. We re-run the
// same Floyd-Warshall as walkbox_load, on the existing box records.
void walkbox_recompute_matrix() {
    WalkboxGraph *g = engine_active_walkbox_graph();
    if (!g || !g->valid) return;
    int n = g->num_boxes;
    static uint8_t adj[MAX_BOXES * MAX_BOXES];
    for (int i = 0; i < MAX_BOXES * MAX_BOXES; i++) g->matrix[i] = INVALID_BOX;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int idx = i * MAX_BOXES + j;
            if (i == j) { adj[idx] = 0; g->matrix[idx] = (uint8_t)j; }
            else if (boxes_share_edge(&g->boxes[i], &g->boxes[j])) {
                adj[idx] = 1; g->matrix[idx] = (uint8_t)j;
            } else {
                adj[idx] = 255; g->matrix[idx] = INVALID_BOX;
            }
        }
    }
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < n; i++) {
            if (i == k) continue;
            uint8_t d_ik = adj[i * MAX_BOXES + k];
            if (d_ik == 255) continue;
            for (int j = 0; j < n; j++) {
                if (i == j) continue;
                uint8_t d_kj = adj[k * MAX_BOXES + j];
                if (d_kj == 255) continue;
                int sum = (int)d_ik + (int)d_kj;
                if (sum > 254) sum = 254;
                if (sum < adj[i * MAX_BOXES + j]) {
                    adj[i * MAX_BOXES + j] = (uint8_t)sum;
                    g->matrix[i * MAX_BOXES + j] =
                        g->matrix[i * MAX_BOXES + k];
                }
            }
        }
    }
}

// Mirror of ScummEngine::checkXYInBoxBounds (boxes.cpp:520) — defers to
// our point-in-quad test once box id is bounds-checked.
bool walkbox_xy_in_box(const WalkboxGraph *g, int box_id, int x, int y) {
    if (!g || !g->valid) return false;
    if (box_id < 0 || box_id >= g->num_boxes) return false;
    return point_in_box(&g->boxes[box_id], x, y);
}

// Direct port of Actor::adjustXYToBeInBox (scummvm-upstream/actor.cpp:1983-2088).
// We omit the v4-specific _lastValidX/Y carry-over (an undefined-behaviour
// quirk used to recover from a single odd MI1 case in scummvm); for our
// MI1 VGA Floppy target this only affects bug #2377-style edge cases.
uint8_t walkbox_adjust_xy(const WalkboxGraph *g, int dst_x, int dst_y,
                          int *out_x, int *out_y) {
    if (!g || !g->valid || g->num_boxes == 0) {
        *out_x = dst_x; *out_y = dst_y;
        return INVALID_BOX;
    }

    static const int threshold_table[] = { 30, 80, 0 };
    int abr_x = dst_x, abr_y = dst_y;
    uint8_t abr_box = INVALID_BOX;

    for (int t_idx = 0; t_idx < 3; t_idx++) {
        int threshold = threshold_table[t_idx];
        int num_boxes = g->num_boxes - 1;
        int best_dist = 0xFFFF;     // v4-6: 16-bit init
        uint8_t best_box = INVALID_BOX;

        // Iterate backwards over all boxes, smallest distance wins.
        for (int box = num_boxes; box >= 0; box--) {
            const WalkBox &b = g->boxes[box];
            if (b.flags & BOX_FLAG_INVISIBLE) continue;
            // (kBoxPlayerOnly skipped — v4 MI1 doesn't flip player-only.)

            // Quick reject: skip if (x,y) is more than `threshold` pixels
            // from the box's bounding rect on either axis. Mirrors
            // boxes.cpp `inBoxQuickReject`.
            if (threshold > 0) {
                int min_x = b.ulx, max_x = b.ulx;
                if (b.urx < min_x) min_x = b.urx; else if (b.urx > max_x) max_x = b.urx;
                if (b.lrx < min_x) min_x = b.lrx; else if (b.lrx > max_x) max_x = b.lrx;
                if (b.llx < min_x) min_x = b.llx; else if (b.llx > max_x) max_x = b.llx;
                int min_y = b.uly, max_y = b.uly;
                if (b.ury < min_y) min_y = b.ury; else if (b.ury > max_y) max_y = b.ury;
                if (b.lry < min_y) min_y = b.lry; else if (b.lry > max_y) max_y = b.lry;
                if (b.lly < min_y) min_y = b.lly; else if (b.lly > max_y) max_y = b.lly;
                if (dst_x < min_x - threshold || dst_x > max_x + threshold) continue;
                if (dst_y < min_y - threshold || dst_y > max_y + threshold) continue;
            }

            // Inside? Done — exact match short-circuits the loop.
            if (point_in_box(&b, dst_x, dst_y)) {
                *out_x = dst_x; *out_y = dst_y;
                return (uint8_t)box;
            }

            int cx, cy;
            walkbox_closest_pt(&b, dst_x, dst_y, &cx, &cy);
            int dx = dst_x - cx, dy = dst_y - cy;
            int tmp_dist = dx * dx + dy * dy;

            if (tmp_dist < best_dist) {
                abr_x = cx; abr_y = cy;
                if (tmp_dist == 0) {
                    *out_x = abr_x; *out_y = abr_y;
                    return (uint8_t)box;
                }
                best_dist = tmp_dist;
                best_box = (uint8_t)box;
            }
        }

        if (threshold == 0 || threshold * threshold >= best_dist) {
            abr_box = best_box;
            *out_x = abr_x; *out_y = abr_y;
            return abr_box;
        }
    }

    *out_x = abr_x; *out_y = abr_y;
    return abr_box;
}

// Direct port of Actor::findPathTowards (scummvm-upstream/boxes.cpp:815-944).
// We work on local copies of the four corner points so we can rotate them
// in place — the algorithm tries all 4×4 combinations of rotated boxes,
// looking for a shared horizontal or vertical edge.
bool walkbox_find_path_towards(const WalkboxGraph *g, int box1nr, int box2nr,
                               int box3nr,
                               int cur_x, int cur_y,
                               int dest_x, int dest_y,
                               int *foundPath_x, int *foundPath_y) {
    if (!g || !g->valid) return false;
    if (box1nr < 0 || box1nr >= g->num_boxes) return false;
    if (box2nr < 0 || box2nr >= g->num_boxes) return false;

    // BoxCoords mirrors scummvm's BoxCoords (ul, ur, lr, ll). We rotate
    // these by overwriting each iteration.
    struct Pt { int x, y; };
    Pt b1ul = { g->boxes[box1nr].ulx, g->boxes[box1nr].uly };
    Pt b1ur = { g->boxes[box1nr].urx, g->boxes[box1nr].ury };
    Pt b1lr = { g->boxes[box1nr].lrx, g->boxes[box1nr].lry };
    Pt b1ll = { g->boxes[box1nr].llx, g->boxes[box1nr].lly };
    Pt b2ul = { g->boxes[box2nr].ulx, g->boxes[box2nr].uly };
    Pt b2ur = { g->boxes[box2nr].urx, g->boxes[box2nr].ury };
    Pt b2lr = { g->boxes[box2nr].lrx, g->boxes[box2nr].lry };
    Pt b2ll = { g->boxes[box2nr].llx, g->boxes[box2nr].lly };

    auto SWAP_INT = [](int &a, int &b) { int t = a; a = b; b = t; };

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            // Vertical-edge case
            if (b1ul.x == b1ur.x && b1ul.x == b2ul.x && b1ul.x == b2ur.x) {
                int flag = 0;
                if (b1ul.y > b1ur.y) { SWAP_INT(b1ul.y, b1ur.y); flag |= 1; }
                if (b2ul.y > b2ur.y) { SWAP_INT(b2ul.y, b2ur.y); flag |= 2; }

                if (b1ul.y > b2ur.y || b2ul.y > b1ur.y ||
                    ((b1ur.y == b2ul.y || b2ur.y == b1ul.y) &&
                     b1ul.y != b1ur.y && b2ul.y != b2ur.y)) {
                    if (flag & 1) SWAP_INT(b1ul.y, b1ur.y);
                    if (flag & 2) SWAP_INT(b2ul.y, b2ur.y);
                } else {
                    int pos = cur_y;
                    if (box2nr == box3nr) {
                        int diffX = dest_x - cur_x;
                        int diffY = dest_y - cur_y;
                        int boxDiffX = b1ul.x - cur_x;
                        if (diffX != 0) {
                            int t;
                            diffY *= boxDiffX;
                            t = diffY / diffX;
                            if (t == 0 && (diffY <= 0 || diffX <= 0)
                                       && (diffY >= 0 || diffX >= 0))
                                t = -1;
                            pos = cur_y + t;
                        }
                    }
                    int q = pos;
                    if (q < b2ul.y) q = b2ul.y;
                    if (q > b2ur.y) q = b2ur.y;
                    if (q < b1ul.y) q = b1ul.y;
                    if (q > b1ur.y) q = b1ur.y;
                    if (q == pos && box2nr == box3nr) return true;
                    *foundPath_x = b1ul.x;
                    *foundPath_y = q;
                    return false;
                }
            }

            // Horizontal-edge case
            if (b1ul.y == b1ur.y && b1ul.y == b2ul.y && b1ul.y == b2ur.y) {
                int flag = 0;
                if (b1ul.x > b1ur.x) { SWAP_INT(b1ul.x, b1ur.x); flag |= 1; }
                if (b2ul.x > b2ur.x) { SWAP_INT(b2ul.x, b2ur.x); flag |= 2; }

                if (b1ul.x > b2ur.x || b2ul.x > b1ur.x ||
                    ((b1ur.x == b2ul.x || b2ur.x == b1ul.x) &&
                     b1ul.x != b1ur.x && b2ul.x != b2ur.x)) {
                    if (flag & 1) SWAP_INT(b1ul.x, b1ur.x);
                    if (flag & 2) SWAP_INT(b2ul.x, b2ur.x);
                } else {
                    int pos;
                    if (box2nr == box3nr) {
                        int diffX = dest_x - cur_x;
                        int diffY = dest_y - cur_y;
                        int boxDiffY = b1ul.y - cur_y;
                        pos = cur_x;
                        if (diffY != 0) {
                            pos += diffX * boxDiffY / diffY;
                        }
                    } else {
                        pos = cur_x;
                    }
                    int q = pos;
                    if (q < b2ul.x) q = b2ul.x;
                    if (q > b2ur.x) q = b2ur.x;
                    if (q < b1ul.x) q = b1ul.x;
                    if (q > b1ur.x) q = b1ur.x;
                    if (q == pos && box2nr == box3nr) return true;
                    *foundPath_x = q;
                    *foundPath_y = b1ul.y;
                    return false;
                }
            }

            // Rotate box1 corners (CW): ul → ur → lr → ll → ul.
            Pt tmp = b1ul;
            b1ul = b1ur; b1ur = b1lr; b1lr = b1ll; b1ll = tmp;
        }
        // Rotate box2 corners
        Pt tmp = b2ul;
        b2ul = b2ur; b2ur = b2lr; b2lr = b2ll; b2ll = tmp;
    }
    return false;
}

void walkbox_closest_pt(const WalkBox *box, int px, int py,
                        int *out_x, int *out_y) {
    int best_x = box->ulx, best_y = box->uly;
    int best_d2 = (px - best_x) * (px - best_x) + (py - best_y) * (py - best_y);

    int verts[4][2] = {
        {box->ulx, box->uly},
        {box->urx, box->ury},
        {box->lrx, box->lry},
        {box->llx, box->lly},
    };
    for (int i = 0; i < 4; i++) {
        int x1 = verts[i][0],   y1 = verts[i][1];
        int x2 = verts[(i+1)&3][0], y2 = verts[(i+1)&3][1];
        int cx, cy;
        closest_pt_on_seg(x1, y1, x2, y2, px, py, &cx, &cy);
        int dx = px - cx, dy = py - cy;
        int d2 = dx*dx + dy*dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best_x = cx; best_y = cy;
        }
    }
    *out_x = best_x;
    *out_y = best_y;
    (void)abs_int;
}

}  // namespace tsb
