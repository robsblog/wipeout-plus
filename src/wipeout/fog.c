#include <math.h>

#include "../mem.h"
#include "../system.h"
#include "../render.h"

#include "track.h"
#include "scene.h"
#include "game.h"
#include "fog.h"

// --- Tuning (starting values; final balancing happens in Task 5) -------------

#define FOG_MAX_ZONES      24    // hard cap on selected zones
#define FOG_PUFFS_PER_ZONE 12    // billboards held per zone
#define FOG_MIN_SPACING    8     // sections skipped after a pick (keeps zones sparse)

#define FOG_DIP_WINDOW     3     // +/- sections for the local-maximum-of-y test
#define FOG_STRAIGHT_SPAN  6     // sections examined for the straightness test
#define FOG_STRAIGHT_MAX   0.35f // max accumulated heading change (rad) to count as straight

#define FOG_ZONE_RADIUS    3000.0f // spawn spread / half-extent of a zone
#define FOG_PUFF_SIZE      4000.0f // base billboard size (world units)
#define FOG_PUFF_SIZE_VAR  2000.0f // +/- size variation

#define FOG_PUFF_MAX_ALPHA 0.42f   // per-puff peak opacity (0..1); overlap builds up
#define FOG_FADE_SPEED     1.5f     // alpha ramp rate toward target (1/s)

// Activation distance: a zone is active when the camera is within this range.
#define FOG_ACTIVATE_DIST  (RENDER_FADEOUT_NEAR)

// Spring parameters (infrastructure for Task 4 ship interaction; puffs rest at
// home while no external force acts on them).
#define FOG_SPRING_K       6.0f
#define FOG_SPRING_DAMPING 0.90f

typedef struct {
	vec3_t home;         // rest position
	vec3_t pos;          // current position
	vec3_t vel;          // current velocity
	float  size;         // billboard size (world units)
	float  alpha;        // current opacity 0..1
	float  alpha_target; // target opacity 0..1
} fog_puff_t;

typedef struct {
	vec3_t center;
	float  radius;
	int    section_num;
	bool   active;
	fog_puff_t puffs[FOG_PUFFS_PER_ZONE];
} fog_zone_t;

static fog_zone_t fog_zones[FOG_MAX_ZONES];
static int fog_zone_count = 0;

// Deterministic per-zone PRNG (xorshift32) -- no per-frame rand, seeded from
// the track section number so puff layout is stable across runs.
static uint32_t fog_rng_state = 1;

static void fog_rng_seed(uint32_t seed) {
	fog_rng_state = seed ? seed : 0x9e3779b9u;
}

static float fog_rng_float(float min, float max) {
	uint32_t x = fog_rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	fog_rng_state = x;
	float t = (x & 0xffffff) / (float)0x1000000; // 0..1
	return min + (max - min) * t;
}

// Fill a zone's puffs at deterministic offsets around its center. Offsets are
// biased downward (+Y is DOWN) so banks pool near the ground.
static void fog_zone_spawn_puffs(fog_zone_t *zone) {
	fog_rng_seed((uint32_t)zone->section_num * 2654435761u + 1u);
	for (int i = 0; i < FOG_PUFFS_PER_ZONE; i++) {
		fog_puff_t *p = &zone->puffs[i];
		float ox = fog_rng_float(-zone->radius, zone->radius);
		float oz = fog_rng_float(-zone->radius, zone->radius);
		float oy = fog_rng_float(0.0f, zone->radius * 0.6f); // downward bias
		p->home = vec3_add(zone->center, vec3(ox, oy, oz));
		p->pos  = p->home;
		p->vel  = vec3(0, 0, 0);
		p->size = FOG_PUFF_SIZE + fog_rng_float(-FOG_PUFF_SIZE_VAR, FOG_PUFF_SIZE_VAR);
		p->alpha = 0.0f;
		p->alpha_target = 0.0f;
	}
}

// Heading of a section: normalized direction to the next section center.
static vec3_t fog_section_heading(section_t *s) {
	vec3_t d = vec3_sub(s->next->center, s->center);
	float len = vec3_len(d);
	if (len < 0.0001f) {
		return vec3(0, 0, 0);
	}
	return vec3_mulf(d, 1.0f / len);
}

static void fog_add_zone(section_t *s) {
	if (fog_zone_count >= FOG_MAX_ZONES) {
		return;
	}
	fog_zone_t *zone = &fog_zones[fog_zone_count++];
	zone->center = s->center;
	zone->radius = FOG_ZONE_RADIUS;
	zone->section_num = s->num;
	zone->active = false;
	fog_zone_spawn_puffs(zone);
}

void fog_load(void) {
	fog_zone_count = 0;

	int count = g.track.section_count;
	if (count <= 0) {
		printf("[fog] no track sections; 0 fog zones\n");
		return;
	}

	// Build an ordered section list by walking ->next from the first section,
	// bounded by section_count so junction loops can't run away.
	section_t **ordered = mem_temp_alloc(sizeof(section_t *) * count);
	section_t *s = g.track.sections;
	int n = 0;
	while (s && n < count) {
		ordered[n++] = s;
		s = s->next;
		if (s == g.track.sections) {
			break; // closed loop
		}
	}

	// Select zones on the RISING EDGE of each qualifying region: one zone per
	// contiguous dip or straight run, so a long straight yields a single bank
	// rather than a chain of them. A cooldown after each pick keeps neighbours
	// from crowding. This keeps the set sparse and spread over the whole track.
	bool prev_qualified = false;
	int cooldown = 0;
	for (int i = 0; i < n; i++) {
		if (fog_zone_count >= FOG_MAX_ZONES) {
			break;
		}

		section_t *cur = ordered[i];

		// --- Dip test: strict local maximum of center.y (+Y is DOWN) over a
		// +/- window (a genuine sink the track passes through).
		bool is_dip = true;
		for (int w = -FOG_DIP_WINDOW; w <= FOG_DIP_WINDOW && is_dip; w++) {
			if (w == 0) {
				continue;
			}
			int j = i + w;
			if (j < 0 || j >= n) {
				continue;
			}
			if (ordered[j]->center.y >= cur->center.y) {
				is_dip = false;
			}
		}

		// --- Straight test: low accumulated heading change over a span
		// (part of a long, only gently curving run).
		bool is_straight = false;
		if (i + FOG_STRAIGHT_SPAN < n) {
			float total_angle = 0.0f;
			vec3_t prev_h = fog_section_heading(ordered[i]);
			for (int k = 1; k <= FOG_STRAIGHT_SPAN; k++) {
				vec3_t h = fog_section_heading(ordered[i + k]);
				float d = vec3_dot(prev_h, h);
				if (d > 1.0f) { d = 1.0f; }
				if (d < -1.0f) { d = -1.0f; }
				total_angle += acosf(d);
				prev_h = h;
			}
			is_straight = (total_angle < FOG_STRAIGHT_MAX);
		}

		bool qualified = (is_dip || is_straight);

		// Rising edge + cooldown gate: place at most one zone per region.
		if (qualified && !prev_qualified && cooldown <= 0) {
			fog_add_zone(cur);
			cooldown = FOG_MIN_SPACING;
		}
		if (cooldown > 0) {
			cooldown--;
		}
		prev_qualified = qualified;
	}

	mem_temp_free(ordered);

	printf("[fog] selected %d fog zones of %d sections:", fog_zone_count, n);
	for (int i = 0; i < fog_zone_count; i++) {
		printf(" %d", fog_zones[i].section_num);
	}
	printf("\n");
}

void fog_init(void) {
	for (int i = 0; i < fog_zone_count; i++) {
		fog_zone_t *zone = &fog_zones[i];
		zone->active = false;
		for (int j = 0; j < FOG_PUFFS_PER_ZONE; j++) {
			fog_puff_t *p = &zone->puffs[j];
			p->pos = p->home;
			p->vel = vec3(0, 0, 0);
			p->alpha = 0.0f;
			p->alpha_target = 0.0f;
		}
	}
}

void fog_update(void) {
	float dt = (float)system_tick();
	float activate_sq = FOG_ACTIVATE_DIST * FOG_ACTIVATE_DIST;
	vec3_t cam = g.camera.position;

	for (int i = 0; i < fog_zone_count; i++) {
		fog_zone_t *zone = &fog_zones[i];
		vec3_t diff = vec3_sub(cam, zone->center);
		zone->active = (vec3_len_sq(diff) < activate_sq);

		float target = zone->active ? FOG_PUFF_MAX_ALPHA : 0.0f;
		for (int j = 0; j < FOG_PUFFS_PER_ZONE; j++) {
			fog_puff_t *p = &zone->puffs[j];
			p->alpha_target = target;

			// Spring back toward home (no external force yet -- Task 4).
			vec3_t to_home = vec3_sub(p->home, p->pos);
			p->vel = vec3_add(p->vel, vec3_mulf(to_home, FOG_SPRING_K * dt));
			p->vel = vec3_mulf(p->vel, FOG_SPRING_DAMPING);
			p->pos = vec3_add(p->pos, vec3_mulf(p->vel, dt));

			// Ramp alpha toward target.
			p->alpha += (p->alpha_target - p->alpha) * FOG_FADE_SPEED * dt;
			if (p->alpha < 0.0f) { p->alpha = 0.0f; }
			if (p->alpha > 1.0f) { p->alpha = 1.0f; }
		}
	}
}

void fog_draw(void) {
	if (!save.fog || fog_zone_count == 0) {
		return;
	}

	rgba_t base = scene_fog_color();

	render_set_model_mat(&mat4_identity());
	render_set_depth_write(false);
	render_set_blend_mode(RENDER_BLEND_NORMAL);
	render_set_depth_offset(-32.0);

	uint16_t tex = render_fog_texture();

	for (int i = 0; i < fog_zone_count; i++) {
		fog_zone_t *zone = &fog_zones[i];
		if (!zone->active) {
			continue;
		}
		for (int j = 0; j < FOG_PUFFS_PER_ZONE; j++) {
			fog_puff_t *p = &zone->puffs[j];
			if (p->alpha <= 0.003f) {
				continue;
			}
			uint8_t a = (uint8_t)(p->alpha * 255.0f);
			rgba_t color = rgba(base.r, base.g, base.b, a);
			int s = (int)p->size;
			render_push_sprite(p->pos, vec2i(s, s), color, tex);
		}
	}

	render_set_depth_offset(0.0);
	render_set_depth_write(true);
	render_set_blend_mode(RENDER_BLEND_NORMAL);
}
