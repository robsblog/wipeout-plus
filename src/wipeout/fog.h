#ifndef FOG_H
#define FOG_H

#include "../types.h"

// Fog-volume subsystem: at track load it picks a sparse set of fog zones from
// the track sections (dips and long/straight runs), and for nearby zones it
// holds and draws soft, alpha-blended billboard "puffs" -- localized fog banks,
// NOT a full-track blanket.

void fog_load(void);   // pick zones from g.track at track load
void fog_init(void);   // reset per-race puff state
void fog_update(void); // per-frame: activate/deactivate zones, integrate puffs
void fog_draw(void);   // per-frame: draw active puffs (alpha billboards)

#endif
