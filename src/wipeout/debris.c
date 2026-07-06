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

// Counts per burst
#define DEBRIS_SPARKS_MIN 24
#define DEBRIS_SPARKS_MAX 40
#define DEBRIS_CHUNKS_MIN 6
#define DEBRIS_CHUNKS_MAX 10
#define DEBRIS_SMOKE_MIN  3
#define DEBRIS_SMOKE_MAX  6

// Launch (units/second; debris integrates pos += vel*dt like trail/particle)
#define DEBRIS_SPARK_SPEED   1400.0f
#define DEBRIS_CHUNK_SPEED   800.0f
#define DEBRIS_SPARK_INHERIT 0.25f   // fraction of ship velocity inherited
#define DEBRIS_CHUNK_INHERIT 0.15f

// Lifetimes (seconds)
#define DEBRIS_SPARK_LIFE_MIN 0.8f
#define DEBRIS_SPARK_LIFE_MAX 1.8f
#define DEBRIS_CHUNK_LIFE_MIN 1.2f
#define DEBRIS_CHUNK_LIFE_MAX 2.5f

// Billboard sizes (world units)
#define DEBRIS_SPARK_SIZE_MIN 80.0f
#define DEBRIS_SPARK_SIZE_MAX 150.0f
#define DEBRIS_CHUNK_SIZE_MIN 200.0f
#define DEBRIS_CHUNK_SIZE_MAX 340.0f

// Dev-hit helper: force a hit on the player ship every N seconds.
#define DEBRIS_DEV_HIT_INTERVAL 2.0f

// Physics (SPARK/CHUNK)
#define DEBRIS_GRAVITY      4000.0f  // u/s^2 along -face_normal (toward the road)
#define DEBRIS_RESTITUTION  0.4f     // velocity kept per bounce
#define DEBRIS_REST_SPEED   120.0f   // below this after a bounce -> come to rest
#define DEBRIS_REST_DAMP    0.02f    // fraction of tangential speed kept per second at rest

// Smoke (SMOKE)
#define DEBRIS_SMOKE_LIFE_MIN 1.5f
#define DEBRIS_SMOKE_LIFE_MAX 3.0f
#define DEBRIS_SMOKE_SIZE_MIN 400.0f
#define DEBRIS_SMOKE_SIZE_MAX 700.0f
#define DEBRIS_SMOKE_RISE_MIN 300.0f  // buoyant speed along +face_normal (up)
#define DEBRIS_SMOKE_RISE_MAX 700.0f
#define DEBRIS_SMOKE_INHERIT  0.20f   // fraction of ship velocity inherited
#define DEBRIS_SMOKE_JITTER   200.0f  // small random spread
#define DEBRIS_SMOKE_DRAG     0.5f    // fraction of velocity kept per second
#define DEBRIS_SMOKE_GROW     1.6f    // extra size factor over full life
#define DEBRIS_SMOKE_ALPHA    0.5f    // peak opacity

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

static rgba_t debris_ember_color(float f, uint8_t kind) {
    if (f < 0.0f) { f = 0.0f; }
    if (f > 1.0f) { f = 1.0f; }
    uint8_t r = 255;
    uint8_t g = (uint8_t)(60.0f + 180.0f * f * f);       // greens drop first -> reddening
    uint8_t b = (uint8_t)(20.0f + 180.0f * f * f * f);   // blue only while white-hot
    float a = f / 0.35f;                                  // hold bright, fade at the end
    if (a > 1.0f) { a = 1.0f; }
    if (kind == DEBRIS_KIND_SPARK) { a *= 0.9f; }
    return rgba(r, g, b, (uint8_t)(a * 255.0f));
}

static void debris_spawn_one(uint8_t kind, vec3_t origin, vec3_t vel,
    section_t *section, float max_life, float size, int bounces) {
    if (debris_active >= DEBRIS_MAX) {
        return;
    }
    debris_t *d = &debris_pool[debris_active++];
    d->pos = origin;
    d->vel = vel;
    d->max_life = max_life;
    d->life = max_life;
    d->size = size;
    d->section = section;
    d->kind = kind;
    d->state = DEBRIS_FLYING;
    d->bounces_left = (uint8_t)bounces;
}

void debris_spawn_burst(vec3_t origin, vec3_t base_velocity, section_t *section) {
    if (!save.debris) {
        return;
    }
    if (section == NULL) {
        section = track_nearest_section(origin, vec3(1,1,1), g.track.sections, NULL);
    }

    int sparks = rand_int(DEBRIS_SPARKS_MIN, DEBRIS_SPARKS_MAX);
    for (int i = 0; i < sparks; i++) {
        vec3_t v = vec3_add(vec3_rand(DEBRIS_SPARK_SPEED),
            vec3_mulf(base_velocity, DEBRIS_SPARK_INHERIT));
        debris_spawn_one(DEBRIS_KIND_SPARK, origin, v, section,
            rand_float(DEBRIS_SPARK_LIFE_MIN, DEBRIS_SPARK_LIFE_MAX),
            rand_float(DEBRIS_SPARK_SIZE_MIN, DEBRIS_SPARK_SIZE_MAX),
            rand_int(1, 2));
    }
    int chunks = rand_int(DEBRIS_CHUNKS_MIN, DEBRIS_CHUNKS_MAX);
    for (int i = 0; i < chunks; i++) {
        vec3_t v = vec3_add(vec3_rand(DEBRIS_CHUNK_SPEED),
            vec3_mulf(base_velocity, DEBRIS_CHUNK_INHERIT));
        debris_spawn_one(DEBRIS_KIND_CHUNK, origin, v, section,
            rand_float(DEBRIS_CHUNK_LIFE_MIN, DEBRIS_CHUNK_LIFE_MAX),
            rand_float(DEBRIS_CHUNK_SIZE_MIN, DEBRIS_CHUNK_SIZE_MAX),
            rand_int(1, 2));
    }
    track_face_t *bf = track_section_get_base_face(section);
    vec3_t up = bf->normal; // +normal points up, away from the road -> buoyant
    int puffs = rand_int(DEBRIS_SMOKE_MIN, DEBRIS_SMOKE_MAX);
    for (int i = 0; i < puffs; i++) {
        vec3_t v = vec3_add(
            vec3_mulf(up, rand_float(DEBRIS_SMOKE_RISE_MIN, DEBRIS_SMOKE_RISE_MAX)),
            vec3_mulf(base_velocity, DEBRIS_SMOKE_INHERIT));
        v = vec3_add(v, vec3_rand(DEBRIS_SMOKE_JITTER));
        debris_spawn_one(DEBRIS_KIND_SMOKE, origin, v, section,
            rand_float(DEBRIS_SMOKE_LIFE_MIN, DEBRIS_SMOKE_LIFE_MAX),
            rand_float(DEBRIS_SMOKE_SIZE_MIN, DEBRIS_SMOKE_SIZE_MAX),
            0);
    }
}

static void debris_dev_hit_tick(void) {
    static int dev_hit = -1;
    static float acc = 0.0f;
    if (dev_hit < 0) {
        dev_hit = getenv("WIPEOUT_DEV_HIT") ? 1 : 0;
    }
    if (!dev_hit) {
        return;
    }
    acc += (float)system_tick();
    if (acc < DEBRIS_DEV_HIT_INTERVAL) {
        return;
    }
    acc = 0.0f;
    ship_t *ship = &g.ships[g.pilot];
    debris_spawn_burst(ship->position, ship->velocity, ship->section);
}

void debris_update(void) {
    if (!save.debris) {
        return;
    }
    debris_dev_hit_tick();

    float dt = (float)system_tick();
    if (dt <= 0.0f) {
        return;
    }

    for (int i = 0; i < debris_active; i++) {
        debris_t *d = &debris_pool[i];
        d->life -= dt;
        if (d->life <= 0.0f) {
            debris_pool[i--] = debris_pool[--debris_active];
            continue;
        }

        if (d->kind == DEBRIS_KIND_SMOKE) {
            d->vel = vec3_mulf(d->vel, powf(DEBRIS_SMOKE_DRAG, dt)); // frame-rate independent
            d->pos = vec3_add(d->pos, vec3_mulf(d->vel, dt));
            continue;
        }

        // Refresh the cached section + base face for the surface query.
        d->section = track_nearest_section(d->pos, vec3(1,1,1), d->section, NULL);
        track_face_t *face = track_section_get_base_face(d->section);
        vec3_t n = face->normal;                       // +normal is UP, away from the road
        vec3_t face_point = face->tris[0].vertices[0].pos;

        if (d->state == DEBRIS_FLYING) {
            // Gravity pulls toward the road: along -normal (see Global Constraints).
            d->vel = vec3_add(d->vel, vec3_mulf(n, -DEBRIS_GRAVITY * dt));
            d->pos = vec3_add(d->pos, vec3_mulf(d->vel, dt));

            float h = vec3_distance_to_plane(d->pos, face_point, n);
            if (h < 0.0f) {
                // Clamp back onto the surface (h<0 -> -n*h moves +normal, up).
                d->pos = vec3_sub(d->pos, vec3_mulf(n, h));
                if (d->bounces_left > 0) {
                    d->vel = vec3_mulf(vec3_reflect(d->vel, n, 2), DEBRIS_RESTITUTION);
                    d->bounces_left--;
                }
                else {
                    d->state = DEBRIS_RESTING;
                }
                if (vec3_len(d->vel) < DEBRIS_REST_SPEED) {
                    d->state = DEBRIS_RESTING;
                }
            }
        }
        else { // DEBRIS_RESTING: roll out along the surface, strong friction
            float vn = vec3_dot(d->vel, n);
            d->vel = vec3_sub(d->vel, vec3_mulf(n, vn));        // keep tangential only
            d->vel = vec3_mulf(d->vel, powf(DEBRIS_REST_DAMP, dt)); // frame-rate independent
            d->pos = vec3_add(d->pos, vec3_mulf(d->vel, dt));
            float h = vec3_distance_to_plane(d->pos, face_point, n);
            d->pos = vec3_sub(d->pos, vec3_mulf(n, h));         // stay glued to the surface
        }
    }
}

void debris_draw(void) {
    if (!save.debris || debris_active == 0) {
        return;
    }

    render_set_model_mat(&mat4_identity());
    render_set_depth_write(false);

    // Pass 1: additive embers (sparks + chunks)
    render_set_blend_mode(RENDER_BLEND_LIGHTER);
    render_set_depth_offset(-32.0);
    uint16_t glow = render_glow_texture();
    for (int i = 0; i < debris_active; i++) {
        debris_t *d = &debris_pool[i];
        if (d->kind == DEBRIS_KIND_SMOKE) {
            continue;
        }
        float f = d->life / d->max_life;   // 1 hot -> 0 spent
        rgba_t col = debris_ember_color(f, d->kind);
        float flicker = 0.85f + 0.15f * sinf((float)system_cycle_time() * 40.0f + (float)i);
        int s = (int)(d->size * (d->kind == DEBRIS_KIND_CHUNK ? flicker : 1.0f));
        render_push_sprite(d->pos, vec2i(s, s), col, glow);
    }

    // Pass 2: normal-blended smoke (grey puffs that grow and fade)
    render_set_blend_mode(RENDER_BLEND_NORMAL);
    uint16_t smoke = render_fog_texture();
    for (int i = 0; i < debris_active; i++) {
        debris_t *d = &debris_pool[i];
        if (d->kind != DEBRIS_KIND_SMOKE) {
            continue;
        }
        float f = d->life / d->max_life;   // 1 birth -> 0 death
        float a = f * DEBRIS_SMOKE_ALPHA;
        int s = (int)(d->size * (1.0f + (1.0f - f) * DEBRIS_SMOKE_GROW));
        uint8_t g = (uint8_t)(60.0f + 40.0f * f); // darker as it ages
        render_push_sprite(d->pos, vec2i(s, s),
            rgba(g, g, g, (uint8_t)(a * 255.0f)), smoke);
    }

    render_set_depth_offset(0.0);
    render_set_depth_write(true);
    render_set_blend_mode(RENDER_BLEND_NORMAL);
}
