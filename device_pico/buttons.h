/*
 * ThumbyScummby — Thumby Color physical button reader.
 *
 * GPIO map (active low, internal pull-ups):
 *   GP0  LEFT     GP1  UP       GP2  RIGHT     GP3  DOWN
 *   GP21 A        GP25 B        GP6  LB        GP22 RB
 *   GP26 MENU
 *
 * Adapted from ThumbyDOOM/ThumbyNES button code; we expose raw bools
 * because the engine wants per-button state, not a packed mask.
 */
#ifndef THUMBYSCUMMBY_BUTTONS_H
#define THUMBYSCUMMBY_BUTTONS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void buttons_init(void);

struct buttons_state {
    bool left, right, up, down;
    bool a, b, lb, rb, menu;
};

void buttons_read(struct buttons_state *out);

#ifdef __cplusplus
}
#endif

#endif
