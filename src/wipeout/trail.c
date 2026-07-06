#include <math.h>

#include "../mem.h"
#include "../system.h"
#include "../utils.h"
#include "../render.h"

#include "ship.h"
#include "game.h"
#include "trail.h"

// --- Tuning (starting values) ------------------------------------------------

#define TRAIL_MAX        768
#define TRAIL_NEAR_DIST  (RENDER_FADEOUT_NEAR * 0.6f) // only trail near/visible ships
#define TRAIL_INTERVAL   0.014f  // seconds between spawns (per engine)
#define TRAIL_LIFE_MIN   0.22f   // fade-out time at low speed (short streak)
#define TRAIL_LIFE_MAX   1.05f   // fade-out time at top speed (long, slow-fading streak)
#define TRAIL_SIZE       230.0f  // billboard size at birth (tight = light, not smoke)
#define TRAIL_BACK       350.0f  // backward drift from the engine (u/s)
#define TRAIL_JITTER     45.0f   // spawn spread (small = coherent streak)
#define TRAIL_ALPHA      0.26f   // additive brightness at birth
#define TRAIL_R          170     // bright cyan-white light
#define TRAIL_G          215
#define TRAIL_B          255

// Speed gating from per-frame movement (world units / second), computed the
// same way for every ship (player and AI use different ->speed scales). Below
// MIN no trail (no standstill pile-up); at/above REF the trail is full length.
#define TRAIL_SPEED_MIN  9000.0f
#define TRAIL_SPEED_REF  52000.0f

typedef struct {
	vec3_t pos;
	vec3_t vel;
	float  life;     // counts down to 0
	float  max_life; // lifetime it was born with (for fade)
	float  size;
} trail_t;

static trail_t trails[TRAIL_MAX];
static float trail_spawn_acc = 0.0f;
static vec3_t trail_prev_pos[NUM_PILOTS];
static bool trail_prev_valid = false;

void trail_init(void) {
	for (int i = 0; i < TRAIL_MAX; i++) {
		trails[i].life = 0.0f;
	}
	trail_spawn_acc = 0.0f;
	trail_prev_valid = false;
}

static bool trail_ship_near(ship_t *ship) {
	vec3_t d = vec3_sub(g.camera.position, ship->position);
	return vec3_len_sq(d) < TRAIL_NEAR_DIST * TRAIL_NEAR_DIST;
}

void trail_update(void) {
	float dt = (float)system_tick();

	for (int i = 0; i < TRAIL_MAX; i++) {
		trail_t *t = &trails[i];
		if (t->life <= 0.0f) {
			continue;
		}
		t->life -= dt;
		t->pos = vec3_add(t->pos, vec3_mulf(t->vel, dt));
	}

	// Per-frame world movement speed for every ship (uniform across player/AI),
	// updated every frame regardless of the spawn interval.
	float ship_speed[NUM_PILOTS];
	for (int i = 0; i < NUM_PILOTS; i++) {
		vec3_t p = g.ships[i].position;
		if (trail_prev_valid && dt > 0.0001f) {
			ship_speed[i] = vec3_len(vec3_sub(p, trail_prev_pos[i])) / dt;
		}
		else {
			ship_speed[i] = 0.0f;
		}
		trail_prev_pos[i] = p;
	}
	trail_prev_valid = true;

	trail_spawn_acc += dt;
	if (trail_spawn_acc < TRAIL_INTERVAL) {
		return;
	}
	trail_spawn_acc = 0.0f;

	for (int i = 0; i < NUM_PILOTS; i++) {
		ship_t *ship = &g.ships[i];
		if (!trail_ship_near(ship)) {
			continue;
		}
		// Scale with speed: no trail at a standstill (no additive pile-up),
		// longer streak the faster you go.
		float speed_factor = (ship_speed[i] - TRAIL_SPEED_MIN) /
			(TRAIL_SPEED_REF - TRAIL_SPEED_MIN);
		if (speed_factor <= 0.0f) {
			continue;
		}
		if (speed_factor > 1.0f) { speed_factor = 1.0f; }
		float life = TRAIL_LIFE_MIN + speed_factor * (TRAIL_LIFE_MAX - TRAIL_LIFE_MIN);

		vec3_t fwd = vec3_normalize(vec3_sub(ship_nose(ship), ship->position));
		for (int e = 0; e < 3; e++) {
			if (ship->exhaust_plume[e].v == NULL) {
				continue;
			}
			int slot = -1;
			for (int k = 0; k < TRAIL_MAX; k++) {
				if (trails[k].life <= 0.0f) { slot = k; break; }
			}
			if (slot < 0) { break; }
			trail_t *t = &trails[slot];
			t->pos = vec3_add(ship_exhaust_world(ship, e),
				vec3(rand_float(-TRAIL_JITTER, TRAIL_JITTER),
				     rand_float(-TRAIL_JITTER, TRAIL_JITTER),
				     rand_float(-TRAIL_JITTER, TRAIL_JITTER)));
			t->vel = vec3_mulf(fwd, -TRAIL_BACK); // stream out behind the engine
			t->life = life;
			t->max_life = life;
			t->size = TRAIL_SIZE;
		}
	}
}

void trail_draw(void) {
	render_set_model_mat(&mat4_identity());
	render_set_depth_write(false);
	render_set_blend_mode(RENDER_BLEND_LIGHTER);

	uint16_t tex = render_glow_texture(); // smooth bright sprite -> light, not smoke
	for (int i = 0; i < TRAIL_MAX; i++) {
		trail_t *t = &trails[i];
		if (t->life <= 0.0f) {
			continue;
		}
		float f = t->life / t->max_life;         // 1 at birth -> 0 at death
		float a = f * TRAIL_ALPHA;
		int s = (int)(t->size * (0.5f + 0.5f * f)); // shrink as it fades
		render_push_sprite(t->pos, vec2i(s, s),
			rgba(TRAIL_R, TRAIL_G, TRAIL_B, (uint8_t)(a * 255.0f)), tex);
	}

	render_set_blend_mode(RENDER_BLEND_NORMAL);
	render_set_depth_write(true);
}
