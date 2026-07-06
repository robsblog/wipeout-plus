#ifndef DEBRIS_H
#define DEBRIS_H

// Ship-hit debris: physics-driven glowing chunks + sparks blasted off a ship
// when a weapon hits it, thrown onto the track by gravity, bouncing a couple
// of times, rolling out and burning out on the surface; plus a soft smoke puff
// at the hit. Own isolated pool -- does not touch the additive particle system
// used for fire / track-hit effects.

#include "../types.h"
#include "track.h"

void debris_init(void);   // reset the pool (per race)
void debris_update(void); // per-frame: gravity, bounce, rest/roll-out, burn-out
void debris_draw(void);   // per-frame: additive embers + normal-blended smoke

// Blast a burst of debris off a hit ship. origin = hit position, base_velocity
// = the struck ship's velocity (debris inherits a fraction), section = the
// ship's current track section (seed for the surface query; may be NULL).
void debris_spawn_burst(vec3_t origin, vec3_t base_velocity, section_t *section);

#endif
