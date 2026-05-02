# SCUMM v5 Actor / Costume / Walkbox / Render Spec

> Distilled from `scummvm-upstream/engines/scumm/{actor,costume,boxes,gfx}.{cpp,h}`.

## 1. Actor state

Per-actor (typically 16 actors max for v5):

```cpp
struct Actor {
    // identity
    uint8   number;          // 0..15
    uint16  costume;         // costume resource ID
    uint8   room;            // current room
    bool    visible;

    // position
    int16   x, y;            // virtual screen coords (room space)
    int16   elevation;       // additional Z offset

    // facing & motion
    uint16  facing;          // current direction (0-359 degrees)
    uint16  target_facing;
    uint8   moving;          // bitmask: MF_NEW_LEG, MF_IN_LEG, MF_LAST_LEG, MF_TURN
    uint8   walkbox;         // current walkbox ID
    uint16  speedx, speedy;

    // animation frames
    uint8   frame;
    uint8   init_frame, walk_frame, stand_frame;
    uint8   talk_start_frame, talk_stop_frame;
    uint8   anim_speed;
    uint8   anim_progress;

    // costume animation state (per-limb)
    CostumeAnim cost;

    // appearance
    uint8   scalex, scaley;  // 0..255 (0xFF = 100%)
    uint8   talk_color;
    uint8   shadow_mode;
    int16   talk_pos_x, talk_pos_y;
    bool    flip;

    // mask / clipping
    uint8   force_clip;
    uint8   layer;
    bool    ignore_boxes;
    bool    draw_to_back_buf;

    // walking destination
    ActorWalkData walkdata;

    // per-actor palette remap
    uint16  palette[256];

    // sound triggers per frame
    uint16  sound[32];
};
```

Estimated size on RP2350 with packed 8/16-bit fields: **~640 bytes/actor ×
16 actors = ~10 KB**.

### CostumeAnim (per-limb state)

```cpp
struct CostumeAnim {
    uint8   anim_type[16];   // limb -> AKAT_*
    uint16  anim_counter;
    uint8   sound_counter;
    uint8   sound_pos;
    uint16  stopped;          // bitmask of stopped limbs
    uint16  curpos[16];
    uint16  start[16];
    uint16  end[16];
    uint16  frame[16];
};
```

### MoveFlags
```cpp
enum {
    MF_NEW_LEG  = 1,   // need to compute next waypoint
    MF_IN_LEG   = 2,   // currently moving along a segment
    MF_TURN     = 4,   // turning to face direction
    MF_LAST_LEG = 8    // on final segment (direct to dest)
};
```

## 2. Walkbox graph

### BOXD chunk layout (v5)

```
uint8 num_boxes
repeat num_boxes:
    int16  ulx, uly, urx, ury, lrx, lry, llx, lly   (LE)   -- 4 vertices CW
    uint8  mask                                            -- z-plane bits
    uint8  flags
    uint16 scale                                            -- 0..255
```

20 bytes per box; numBoxes < 256.

### BoxFlags
```
0x08  X-flip
0x10  Y-flip
0x20  ignore-scale
0x40  locked (actor cannot enter)
0x80  invisible (no z-clip)
```

### BOXM (matrix) layout (v5)

```
uint8 num_boxes
uint8[num_boxes][num_boxes] next_box_on_path
```

Each entry `matrix[from][to]` = next box ID to walk to in order to reach
`to` from `from` (the closer-to-destination neighbor of `from`). When
`from == to`, the value is `from`.

### Computing the matrix (Floyd-Warshall variant)

ScummVM precomputes this at room load via `calcItineraryMatrix()`:
```
init dist[i][j] = 0 if i==j
init dist[i][j] = 1 if boxes share an edge (any of the 4 edges)
init dist[i][j] = INF otherwise
init next[i][j] = j

for k in 0..num_boxes:
    for i in 0..num_boxes:
        for j in 0..num_boxes:
            if dist[i][k] + dist[k][j] < dist[i][j]:
                dist[i][j] = dist[i][k] + dist[k][j]
                next[i][j] = next[i][k]
```

Two boxes share an edge if any of their 4 edges fully overlaps an edge of
the other (parallel and collinear segments).

Output is a `(NB+1) × NB` table (`scumm/boxes.cpp:984-1035`).

### Pathfinding at runtime

`Actor::startWalkActor(target_x, target_y)`:
1. Find the walkbox containing `(target_x, target_y)` via `getBoxAtPos()`
2. Set `walkdata.dest_x/y/box`, set `moving |= MF_NEW_LEG`
3. Per frame:
   - If `MF_NEW_LEG`: read `matrix[walkbox][dest_box]` → next box
   - Compute "gate" coordinates: the edge between current and next box,
     clipped to the line from current pos to dest
   - Set up `actorWalkStep` to move toward gate point
   - When reached, advance to next box
4. On final box: walk directly to (dest_x, dest_y), set `moving = 0`,
   transition to stand animation

`scumm/actor.cpp:633-682` (actorWalkStep) and `scumm/boxes.cpp:815-944`
(findPathTowards) are the key functions.

## 3. Walking & animation state machine

### Direction calculation
```cpp
int angleFromDelta(int dx, int dy) {
    // 4-direction approximation:
    if (abs(dy) * 2 < abs(dx)) return (dx > 0) ? 90 : 270;
    else                      return (dy > 0) ? 180 : 0;
}
```
0=N, 90=E, 180=S, 270=W. (8-direction games use `atan2` with rounding.)

### Movement integration
```
deltaX = (dest_x - current_x) << 16 / dist  // 16.16 fixed
deltaY = (dest_y - current_y) << 16 / dist
xfrac += deltaX
yfrac += deltaY
position.x += xfrac >> 16; xfrac &= 0xFFFF
position.y += yfrac >> 16; yfrac &= 0xFFFF
```

Per-frame movement scaled by `_walkdata.deltaXFactor` × `scalex/255`.

### Animation frame selection

When `moving & MF_IN_LEG`: play `walk_frame`; when stationary, play
`stand_frame`. Talking: cycle `talk_start_frame` → `talk_stop_frame`.

The actual frame INDEX is fed into the costume's animation table to choose
which limb pictures render.

## 4. Costume render (the actor pixels)

### Algorithm overview (per-limb composition)

```
for limb in 0..15:
    if cost.frame[limb] == 0xFF: skip
    pic = costume.limb_pic[limb][cost.frame[limb]]
    pos.x = actor.x + pic.move_x
    pos.y = actor.y + pic.move_y
    decode_byle_rle(pic.data, pos, scale, mask, palette)
```

### ByleRLE decoder (the inner loop)

The codec emits 8-bit pixels in column-major order across a `width × height`
region. Each "control" byte:

```
control = src[0]
color   = control >> shift           ;; shift = 4 for 16-color, 5 for 32-color
length  = control & ((1 << shift) - 1)
if length == 0:
    length = src[1]; src += 1
src += 1
```

Then output `length` pixels of `color`, advancing destination by 1 row each
pixel; when row exhausted, advance to next column.

For each pixel:
- If `color == 0`: output transparent (don't write)
- Else: `out_pixel = actor.palette[color]`
- Mask check: if `mask_buf[x>>3] & (0x80 >> (x&7))`, skip (z-clip)
- Scale check: skip rows/cols according to scale tables (0..255)
- Shadow: if `shadow_mode == 3` and `color < 8`, do
  `out_pixel = shadow_table[(color << 8) | dst[i]]`

Reference: `scumm/base-costume.cpp:286-421` (`byleRLEDecode`),
`scumm/costume.cpp:74-142` (`paintCelByleRLE`),
`scumm/costume.cpp:417-471` (`ClassicCostumeLoader::loadCostume`).

### COST resource layout (recap from spec 01)

```
uint16   size_low_2_bytes
uint8[4] reserved
uint8    num_anim
uint8    format        (low 7 bits = format code; bit 7 = mirror)
uint8    palette[N]    N=16 or 32 indices
uint16   cmds_offs LE
uint16   limb_offs[16] LE
uint16   anim_offs[num_anim] LE
... animation tables ...
... limb data trees ...
... command stream (the cells used by limbs) ...
```

Format codes:
- `0x58` → 16 colors (4-bit), shift=4
- `0x59` → 32 colors (5-bit), shift=5

## 5. Z-buffer (mask planes)

Mask buffers are 1-bit-per-pixel arrays in **8-pixel-wide vertical strips**
matching the room layout. There's:
- One main mask (always present)
- 4 "z-planes" (ZP01-ZP04 from RMIM) for actor depth

When drawing an actor, the engine picks which z-plane to mask against based
on the actor's walkbox `mask` byte. If a pixel position is set in that
plane, the actor pixel is suppressed (revealing the background — i.e. the
actor walks behind a foreground object).

```
mask_byte = mask_buf[(y * num_strips) + (x >> 3)]
mask_bit  = 0x80 >> (x & 7)
suppressed = (mask_byte & mask_bit) != 0
```

Reference: `scumm/base-costume.cpp:285-422` (mask integration in render).

## 6. Actor sort order

When drawing, actors are sorted by `(y - elevation)` ascending — actor with
smallest effective Y first (drawn "behind" larger-Y actors). Same actor's
drawn pixels overwrite earlier pixels at the same coords. This produces
proper Y-depth layering.

## 7. Camera

```cpp
struct Camera {
    int16 cur_x, cur_y;       // current position
    int16 dest_x, dest_y;     // target (for smooth pan)
    int16 last_x, last_y;     // previous (for dirty rect tracking)
    int16 left_trigger, right_trigger;  // edge thresholds
    uint8 follows;             // actor ID to follow (0 = none)
    uint8 mode;                // 0=normal, others=panning, follow, etc.
    bool  moving_to_actor;
};
```

Per-frame:
- If `follows`: set `dest_x = actors[follows].x`, accelerate `cur_x` toward
  `dest_x`
- Clamp to room bounds and the trigger range

## 8. Object draw

Objects are drawn via the same SMAP strip path as room background — they
are rectangular regions of pixels that overlay the background at their
position. Differences from costume render:
- Single image per state (state 0 = invisible, state 1 = visible)
- No scaling, no per-limb composition
- Drawn before actors

```cpp
void drawObject(int obj) {
    OD = objects[obj];
    if (OD.state == 0) return;
    img = getOBIMSubImage(OD.obim_offs, OD.state);
    drawSMAPStrips(img, OD.x, OD.y, OD.w, OD.h);
}
```

## 9. Implementation guidance for our port

### Memory budget

- Actors: 16 × 256 bytes (lean down from ScummVM struct) = **4 KB**
- Walkbox storage: 256 boxes × 24 bytes = **6 KB**
- Box matrix: 256² bytes = **64 KB max** but typically rooms have 10-30 boxes,
  so 30 × 30 = ~900 bytes; **2 KB** budget safe
- Mask planes: 5 × (320×200/8) = 40 KB
- Costume cache (current room actors): pre-decoded? Or on-demand
  decode-into-virtual-screen: zero working set if streamed.

### Walking

For embedded, replace ScummVM's smooth-pan camera with a snap or step-based
camera (matches our 128×128 viewport better anyway).

For pathfinding, precompute the matrix on room load (one-time cost).

### Costume decode

Decode costume directly into the 8bpp room virtual screen. No extra buffer.

### Actor draw order

Single linear sort — bubble or insertion sort over <=16 actors is trivial.

## Reference citations

| Topic | File | Lines |
|---|---|---|
| Actor struct | `actor.h` | 100-355 |
| CostumeData | `actor.h` | 57-88 |
| Box struct (v5) | `boxes.cpp` | 36-80 |
| BoxFlags | `boxes.h` | 35-42 |
| Box matrix Floyd | `boxes.cpp` | 984-1035 |
| getNextBox | `boxes.cpp` | 732-809 |
| findPathTowards | `boxes.cpp` | 815-944 |
| walkActor | `actor.cpp` | 950-1015 |
| calcMovementFactor | `actor.cpp` | 520-576 |
| actorWalkStep | `actor.cpp` | 633-682 |
| ByleRLE decode | `base-costume.cpp` | 286-421 |
| paintCelByleRLE | `costume.cpp` | 74-142 |
| Costume loader | `costume.cpp` | 417-471 |
| getMaskBuffer | `gfx.h` | 374 |
| drawObject | `object.cpp` | 647-758 |
| ObjectData | `object.h` | 64-79 |
| ImageHeader (OBIM) | `object.h` | 138-185 |
| Camera struct | `gfx.h` | 138-158 |
