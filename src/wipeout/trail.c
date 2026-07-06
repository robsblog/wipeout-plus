#include <math.h>

#include "../mem.h"
#include "../system.h"
#include "../utils.h"
#include "../render.h"

#include "ship.h"
#include "game.h"
#include "trail.h"

// --- Tuning (starting values) ------------------------------------------------

#define TRAIL_MAX        512
#define TRAIL_NEAR_DIST  (RENDER_FADEOUT_NEAR * 0.6f) // only trail near/visible ships
#define TRAIL_INTERVAL   0.020f  // seconds between spawns (per engine)
#define TRAIL_LIFE       0.45f   // fade-out time
#define TRAIL_SIZE       340.0f  // billboard size at birth
#define TRAIL_BACK       500.0f  // backward drift from the engine (u/s)
#define TRAIL_JITTER     120.0f  // spawn spread
#define TRAIL_ALPHA      0.30f   // additive brightness at birth
#define TRAIL_R          150     // cool blue-white, echoing the engine plume
#define TRAIL_G          190
#define TRAIL_B          255

typedef struct {
	vec3_t pos;
	vec3_t vel;
	float  life; // counts down to 0
	float  size;
} trail_t;

static trail_t trails[TRAIL_MAX];
static float trail_spawn_acc = 0.0f;

void trail_init(void) {
	for (int i = 0; i < TRAIL_MAX; i++) {
		trails[i].life = 0.0f;
	}
	trail_spawn_acc = 0.0f;
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
			t->life = TRAIL_LIFE;
			t->size = TRAIL_SIZE;
		}
	}
}

void trail_draw(void) {
	render_set_model_mat(&mat4_identity());
	render_set_depth_write(false);
	render_set_blend_mode(RENDER_BLEND_LIGHTER);

	uint16_t tex = render_fog_texture(); // reuse the soft radial sprite
	for (int i = 0; i < TRAIL_MAX; i++) {
		trail_t *t = &trails[i];
		if (t->life <= 0.0f) {
			continue;
		}
		float f = t->life / TRAIL_LIFE;          // 1 at birth -> 0 at death
		float a = f * TRAIL_ALPHA;
		int s = (int)(t->size * (0.4f + 0.6f * f)); // shrink as it fades
		render_push_sprite(t->pos, vec2i(s, s),
			rgba(TRAIL_R, TRAIL_G, TRAIL_B, (uint8_t)(a * 255.0f)), tex);
	}

	render_set_blend_mode(RENDER_BLEND_NORMAL);
	render_set_depth_write(true);
}
