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

// Test whether two boxes share an edge. Mirrors ScummVM's areBoxesNeighbors
// in spirit but simplified: brute-force check all 4×4 = 16 edge pairs for
// collinear overlap.
static bool boxes_share_edge(const WalkBox *a, const WalkBox *b) {
    if ((a->flags & BOX_FLAG_INVISIBLE) || (b->flags & BOX_FLAG_INVISIBLE))
        return false;

    struct Pt { int x, y; };
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
        for (int j = 0; j < 4; j++) {
            Pt q1 = bv[j];
            Pt q2 = bv[(j + 1) & 3];

            // Both edges vertical (same x) — check y-overlap
            if (p1.x == p2.x && q1.x == q2.x && p1.x == q1.x) {
                int p_lo = p1.y < p2.y ? p1.y : p2.y;
                int p_hi = p1.y < p2.y ? p2.y : p1.y;
                int q_lo = q1.y < q2.y ? q1.y : q2.y;
                int q_hi = q1.y < q2.y ? q2.y : q1.y;
                if (p_lo <= q_hi && q_lo <= p_hi) return true;
            }
            // Both edges horizontal (same y) — check x-overlap
            else if (p1.y == p2.y && q1.y == q2.y && p1.y == q1.y) {
                int p_lo = p1.x < p2.x ? p1.x : p2.x;
                int p_hi = p1.x < p2.x ? p2.x : p1.x;
                int q_lo = q1.x < q2.x ? q1.x : q2.x;
                int q_hi = q1.x < q2.x ? q2.x : q1.x;
                if (p_lo <= q_hi && q_lo <= p_hi) return true;
            }
            // (For oblique edges we'd need full segment-overlap maths; rare
            // in MI1 walkboxes — typically axis-aligned.)
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
