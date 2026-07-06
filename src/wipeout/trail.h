#ifndef TRAIL_H
#define TRAIL_H

// Exhaust trails: a soft, fading additive streak dropped behind each ship's
// engine mounts as it thrusts -- the classic wipEout speed trail. Independent
// of the fog feature; always drawn.

void trail_init(void);   // reset per-race
void trail_update(void); // per-frame: age trail puffs, spawn from thrusting ships
void trail_draw(void);   // per-frame: draw the trail (additive)

#endif
