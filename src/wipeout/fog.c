#include <math.h>

#include "../mem.h"
#include "../system.h"
#include "../render.h"

#include "track.h"
#include "scene.h"
#include "ship.h"
#include "game.h"
#include "fog.h"

// --- Tuning (starting values; final balancing happens in Task 5) -------------

#define FOG_MAX_ZONES      18    // hard cap on selected zones
#define FOG_PUFFS_PER_ZONE 96    // billboards per zone; many overlapping = continuous swath
#define FOG_MIN_SPACING    8     // section gap after a zone run (keeps zones sparse)

#define FOG_DIP_WINDOW     3     // +/- sections for the local-maximum-of-y test
#define FOG_STRAIGHT_SPAN  6     // sections examined for the straightness test
#define FOG_STRAIGHT_MAX   0.35f // max accumulated heading change (rad) to count as straight

// A zone is a run of consecutive sections; each puff is anchored to the actual
// track SURFACE under it (base face), so the fog is a thin, dense, flat layer
// that hugs the road as it rises/curves -- true ground fog the ships cut through
// (they hover only ~220u above the surface). +Y is DOWN, so "above" is -Y.
#define FOG_ZONE_SECTIONS  22      // consecutive sections a fog run spans
#define FOG_ZONE_WIDTH     3400.0f // lateral half-extent (across the track)
#define FOG_ALONG_JITTER   1800.0f // along-track scatter around each sampled section
#define FOG_GROUND_RISE    800.0f  // max rise above the road (-Y); t^3 keeps most puffs low
#define FOG_GROUND_SINK    150.0f  // how far it dips below the road surface (+Y)
#define FOG_PUFF_SIZE      5000.0f // base billboard size (world units)
#define FOG_PUFF_SIZE_VAR  1500.0f // +/- size variation

#define FOG_PUFF_MAX_ALPHA 0.32f   // per-puff opacity of a low puff; high puffs fade out
#define FOG_FADE_SPEED     1.5f     // alpha ramp rate toward target (1/s)

// A passing ship punches a transient hole in the fog (a visible wake/cut) that
// refills over ~1/DECAY seconds.
#define FOG_CARVE_RADIUS   3200.0f // ship carves puffs within this range
#define FOG_DISTURB_DECAY  1.3f    // how fast the hole refills (1/s)

// Activation distance: a zone is active when the camera is within this range.
#define FOG_ACTIVATE_DIST  (RENDER_FADEOUT_NEAR)

// Spring parameters: soft + slow to settle so a ship's wake stays visible for a
// moment before the fog closes back in.
#define FOG_SPRING_K       1.6f
#define FOG_SPRING_DAMPING 0.90f

// --- Ship interaction (Task 4; starting values, final balancing in Task 5) ---

// A ship counts as "near/visible" (worth stirring the fog) when it is within
// this range of the camera. Squared-distance test against g.camera.position.
#define FOG_SHIP_NEAR_DIST (RENDER_FADEOUT_NEAR * 0.5f)

// Turbulence: a ship pushes puffs it drives near. Kick direction is the outward
// direction (puff away from ship) blended with the ship's forward, so puffs get
// shoved aside and trail behind. Scaled by proximity; the spring pulls them back.
#define FOG_PUSH_RADIUS    6500.0f  // ship influences puffs within this range
#define FOG_PUSH_ACCEL     150000.0f // kick acceleration at zero distance (u/s^2)
#define FOG_PUSH_FWD_BLEND 0.45f    // 0 = pure outward, 1 = pure ship-forward
#define FOG_PUSH_MAX_DISP  6000.0f  // clamp puff displacement from home (u)

// Additive thruster glow at the exhaust mounts: a bright concentrated core plus
// a softer halo, so it reads as a light illuminating the fog rather than a soft
// wash. Sizes grow slightly with thrust.
#define FOG_GLOW_CORE_SIZE   340.0f  // bright inner core
#define FOG_GLOW_CORE_ALPHA  0.70f
#define FOG_GLOW_HALO_SIZE   820.0f  // soft outer halo
#define FOG_GLOW_HALO_ALPHA  0.30f
#define FOG_GLOW_THRUST_SCALE 0.28f  // extra size per unit of thrust_mag
#define FOG_GLOW_R           255     // warm engine tint
#define FOG_GLOW_G           170
#define FOG_GLOW_B           90

typedef struct {
	vec3_t home;         // rest position
	vec3_t pos;          // current position
	vec3_t vel;          // current velocity
	float  size;         // billboard size (world units)
	float  base_alpha;   // this puff's full opacity (lower puffs are denser)
	float  alpha;        // current opacity 0..1 (fades with zone activation)
	float  alpha_target; // target opacity 0..1
	float  disturb;      // 0..1 transient carve: a passing ship punches a hole
} fog_puff_t;

typedef struct {
	vec3_t center;
	vec3_t forward;      // track direction at the zone (puffs stretch along this)
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

// Average Y of a section's drivable base face -- the actual road surface height
// at that point (+Y is DOWN). The fog anchors to this, not section->center.
static float fog_surface_y(section_t *s) {
	track_face_t *bf = track_section_get_base_face(s);
	return (bf->tris[0].vertices[0].pos.y +
	        bf->tris[0].vertices[1].pos.y +
	        bf->tris[0].vertices[2].pos.y) / 3.0f;
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

// Add a fog run spanning [start, start+span) sections of the ordered list.
// Each puff is placed at a randomly sampled section in the run, anchored to that
// section's road surface with lateral + along-track scatter and a ground-weighted
// height, so the whole run reads as one flat layer following the road.
static void fog_add_zone(section_t **ordered, int n, int start, int span) {
	if (fog_zone_count >= FOG_MAX_ZONES) {
		return;
	}
	fog_zone_t *zone = &fog_zones[fog_zone_count++];
	int mid = start + span / 2;
	if (mid >= n) { mid = n - 1; }
	zone->center = ordered[mid]->center;      // activation anchor (mid of run)
	zone->forward = fog_section_heading(ordered[mid]);
	zone->radius = 0.0f;
	zone->section_num = ordered[start]->num;
	zone->active = false;

	fog_rng_seed((uint32_t)zone->section_num * 2654435761u + 1u);
	for (int i = 0; i < FOG_PUFFS_PER_ZONE; i++) {
		fog_puff_t *p = &zone->puffs[i];
		int k = start + (int)fog_rng_float(0.0f, (float)span);
		if (k >= n) { k = n - 1; }
		section_t *sec = ordered[k];
		vec3_t fwd = fog_section_heading(sec);
		vec3_t lateral = vec3_normalize(vec3_cross(fwd, vec3(0, 1, 0)));
		float along = fog_rng_float(-FOG_ALONG_JITTER, FOG_ALONG_JITTER);
		float lat   = fog_rng_float(-FOG_ZONE_WIDTH, FOG_ZONE_WIDTH);
		float t     = fog_rng_float(0.0f, 1.0f);
		float oy    = FOG_GROUND_SINK - FOG_GROUND_RISE * (t * t * t); // t^3: most puffs hug the road, few rise

		vec3_t base = sec->center;
		base.y = fog_surface_y(sec) + oy;     // anchor to the actual road surface
		p->home = vec3_add(base,
			vec3_add(vec3_mulf(fwd, along), vec3_mulf(lateral, lat)));
		p->pos  = p->home;
		p->vel  = vec3(0, 0, 0);
		p->size = FOG_PUFF_SIZE + fog_rng_float(-FOG_PUFF_SIZE_VAR, FOG_PUFF_SIZE_VAR);
		p->base_alpha = FOG_PUFF_MAX_ALPHA * (1.0f - 0.5f * t); // denser near the road
		p->alpha = 0.0f;
		p->alpha_target = 0.0f;
		p->disturb = 0.0f;
	}
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

		// Rising edge + cooldown gate: start one fog run per qualifying region.
		if (qualified && !prev_qualified && cooldown <= 0) {
			fog_add_zone(ordered, n, i, FOG_ZONE_SECTIONS);
			cooldown = FOG_ZONE_SECTIONS + FOG_MIN_SPACING;
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
			p->disturb = 0.0f;
		}
	}
}

// True if a ship is close enough to the camera to interact with the fog.
static bool fog_ship_is_near(ship_t *ship) {
	float near_sq = FOG_SHIP_NEAR_DIST * FOG_SHIP_NEAR_DIST;
	vec3_t d = vec3_sub(g.camera.position, ship->position);
	return vec3_len_sq(d) < near_sq;
}

// Gather pointers to the near/visible ships. Returns the count.
static int fog_gather_near_ships(ship_t **out) {
	int count = 0;
	for (int i = 0; i < NUM_PILOTS; i++) {
		if (fog_ship_is_near(&g.ships[i])) {
			out[count++] = &g.ships[i];
		}
	}
	return count;
}

void fog_update(void) {
	float dt = (float)system_tick();
	float activate_sq = FOG_ACTIVATE_DIST * FOG_ACTIVATE_DIST;
	vec3_t cam = g.camera.position;

	// Ships that are close enough to stir the fog (computed once per frame).
	ship_t *near_ships[NUM_PILOTS];
	int near_count = fog_gather_near_ships(near_ships);

	for (int i = 0; i < fog_zone_count; i++) {
		fog_zone_t *zone = &fog_zones[i];
		vec3_t diff = vec3_sub(cam, zone->center);
		zone->active = (vec3_len_sq(diff) < activate_sq);

		for (int j = 0; j < FOG_PUFFS_PER_ZONE; j++) {
			fog_puff_t *p = &zone->puffs[j];
			p->alpha_target = zone->active ? p->base_alpha : 0.0f;

			// The carve hole refills over time.
			p->disturb -= FOG_DISTURB_DECAY * dt;
			if (p->disturb < 0.0f) { p->disturb = 0.0f; }

			// Turbulence: nearby ships shove the puffs aside before the spring
			// integrates, and punch a transient hole (disturb) for a visible cut.
			// Direction = outward (puff away from ship) blended with the ship's
			// forward, scaled by proximity. Only for active zones.
			if (zone->active) {
				for (int s = 0; s < near_count; s++) {
					ship_t *ship = near_ships[s];
					vec3_t away = vec3_sub(p->pos, ship->position);
					float dist = vec3_len(away);
					if (dist >= FOG_PUSH_RADIUS || dist < 0.0001f) {
						continue;
					}
					float proximity = 1.0f - (dist / FOG_PUSH_RADIUS);
					vec3_t out_dir = vec3_mulf(away, 1.0f / dist);
					vec3_t fwd = vec3_normalize(
						vec3_sub(ship_nose(ship), ship->position));
					vec3_t dir = vec3_normalize(vec3_add(
						vec3_mulf(out_dir, 1.0f - FOG_PUSH_FWD_BLEND),
						vec3_mulf(fwd, FOG_PUSH_FWD_BLEND)));
					float accel = FOG_PUSH_ACCEL * proximity * proximity;
					p->vel = vec3_add(p->vel, vec3_mulf(dir, accel * dt));

					// Carve: close passes clear the fog, leaving a wake.
					if (dist < FOG_CARVE_RADIUS) {
						float carve = 1.0f - (dist / FOG_CARVE_RADIUS);
						if (carve > p->disturb) { p->disturb = carve; }
					}
				}
			}

			// Spring back toward home (turbulence above pushed it away).
			vec3_t to_home = vec3_sub(p->home, p->pos);
			p->vel = vec3_add(p->vel, vec3_mulf(to_home, FOG_SPRING_K * dt));
			p->vel = vec3_mulf(p->vel, FOG_SPRING_DAMPING);
			p->pos = vec3_add(p->pos, vec3_mulf(p->vel, dt));

			// Clamp displacement so puffs stay within their zone even under a
			// hard turbulence kick.
			vec3_t disp = vec3_sub(p->pos, p->home);
			float disp_len = vec3_len(disp);
			if (disp_len > FOG_PUSH_MAX_DISP) {
				p->pos = vec3_add(p->home,
					vec3_mulf(disp, FOG_PUSH_MAX_DISP / disp_len));
			}

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
			float da = p->alpha * (1.0f - p->disturb); // carved puffs fade out
			if (da <= 0.003f) {
				continue;
			}
			uint8_t a = (uint8_t)(da * 255.0f);
			rgba_t color = rgba(base.r, base.g, base.b, a);
			int s = (int)p->size;
			render_push_sprite(p->pos, vec2i(s, s), color, tex);
		}
	}

	// Additive thruster glow: warm halos at each ship's exhaust mounts. These
	// brighten and tint whatever fog they sit in. Only near/visible ships.
	// Depth-tested so opaque track geometry occludes it correctly; the shield
	// no longer hides it because the shield is drawn without depth writes
	// (see weapons_draw).
	render_set_blend_mode(RENDER_BLEND_LIGHTER);
	rgba_t core_col = rgba(FOG_GLOW_R, FOG_GLOW_G, FOG_GLOW_B, (uint8_t)(FOG_GLOW_CORE_ALPHA * 255.0f));
	rgba_t halo_col = rgba(FOG_GLOW_R, FOG_GLOW_G, FOG_GLOW_B, (uint8_t)(FOG_GLOW_HALO_ALPHA * 255.0f));
	for (int i = 0; i < NUM_PILOTS; i++) {
		ship_t *ship = &g.ships[i];
		if (!fog_ship_is_near(ship)) {
			continue;
		}
		float grow = ship->thrust_mag * FOG_GLOW_THRUST_SCALE;
		int core = (int)(FOG_GLOW_CORE_SIZE + grow);
		int halo = (int)(FOG_GLOW_HALO_SIZE + grow);
		for (int e = 0; e < 3; e++) {
			if (ship->exhaust_plume[e].v == NULL) {
				continue;
			}
			vec3_t wp = ship_exhaust_world(ship, e);
			render_push_sprite(wp, vec2i(halo, halo), halo_col, tex); // soft falloff
			render_push_sprite(wp, vec2i(core, core), core_col, tex); // bright center
		}
	}

	render_set_depth_offset(0.0);
	render_set_depth_write(true);
	render_set_blend_mode(RENDER_BLEND_NORMAL);
}
