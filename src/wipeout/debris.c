#include <math.h>

#include "../system.h"
#include "../utils.h"
#include "../render.h"

#include "track.h"
#include "ship.h"
#include "game.h"
#include "debris.h"

// --- Tuning (starting values; final balancing is live by the user) ----------

#define DEBRIS_MAX 1024

typedef enum {
    DEBRIS_KIND_SPARK, // small, additive, physics + bounce
    DEBRIS_KIND_CHUNK, // larger, additive, physics + bounce
    DEBRIS_KIND_SMOKE, // soft, normal-blended, buoyant, no bounce
} debris_kind_t;

typedef enum { DEBRIS_FLYING, DEBRIS_RESTING } debris_state_t;

typedef struct {
    vec3_t pos, vel;
    float life, max_life;   // fade = life / max_life
    float size;
    section_t *section;     // cached for the bounce query
    uint8_t kind;           // debris_kind_t
    uint8_t state;          // debris_state_t
    uint8_t bounces_left;   // damped bounces (SPARK/CHUNK)
} debris_t;

static debris_t debris_pool[DEBRIS_MAX];
static int debris_active = 0;

void debris_init(void) {
    debris_active = 0;
}

void debris_spawn_burst(vec3_t origin, vec3_t base_velocity, section_t *section) {
    (void)origin; (void)base_velocity; (void)section;
    // filled in Task 2
}

void debris_update(void) {
    if (!save.debris) {
        return;
    }
    // filled in Task 2 (motion) / Task 3 (physics)
}

void debris_draw(void) {
    if (!save.debris || debris_active == 0) {
        return;
    }
    // filled in Task 2 (embers) / Task 4 (smoke)
}
