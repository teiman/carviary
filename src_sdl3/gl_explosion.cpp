// gl_explosion.cpp -- replacement for the original per-pixel
// R_ParticleExplosion, which in carviary was effectively invisible.
//
// When the server sends a TE_EXPLOSION / TE_TAREXPLOSION we spawn:
//   - 1 core flash: a large view-aligned billboard that flashes hot-white,
//     cools to orange, then to deep smoke in ~0.9 s.
//   - ~16 trail puffs: billboards flung in random directions from the
//     origin with velocity, each leaving behind a cooling smoke puff as
//     it travels. Gives the "fingered" shape of an explosion.
//   - ~28 sparks: GL_POINTS shooting out with gravity + drag, short-lived.
//
// Everything emits into the fb_mask so the bloom pass amplifies the
// brightness, which is how you end up actually SEEING the explosion in
// dark Quake interiors.

#include "quakedef.h"
#include "gl_render.h"
#include <stdint.h>

extern vec3_t vright, vup, vpn;
extern float TraceLine (vec3_t start, vec3_t end, vec3_t impact, vec3_t normal);

// ---------------------------------------------------------------------------
// Tunables.
// ---------------------------------------------------------------------------
#define EXPL_MAX                  64     // concurrent explosions
#define EXPL_CORE_RADIUS         48.0f
#define EXPL_CORE_LIFE            0.9f
#define EXPL_SPARKS              20     // -30% vs original (was 28)
#define EXPL_SPARK_MIN_DIST      50.0f  // spawn offset from blast center
#define EXPL_SPARK_SPEED        220.0f
#define EXPL_SPARK_LIFE           0.85f
#define EXPL_SPARK_GRAVITY       90.0f
#define EXPL_SPARK_POINT_PX       3.5f

#define MAX_PUFFS               2048
#define MAX_SPARKS              1024

// Three-phase smoke sequence replaces the traveler/secondary system.
//   PHASE 1: bright puffs that flash white-yellow and cool to grey quickly.
//   PHASE 2: bigger grey puffs that fly outward fast and fade.
//   PHASE 3: a few dark-grey puffs that rise slowly and linger.
#define EXPL_PHASE1_PUFFS        10
#define EXPL_PHASE1_RADIUS        6.0f
#define EXPL_PHASE1_GROW         22.0f
#define EXPL_PHASE1_LIFE          0.45f
#define EXPL_PHASE1_SPEED        40.0f

#define EXPL_PHASE2_PUFFS        12
#define EXPL_PHASE2_RADIUS       18.0f
#define EXPL_PHASE2_GROW         44.0f
#define EXPL_PHASE2_LIFE          0.036f   // 1/5 again (was 0.18, originally 0.9)
#define EXPL_PHASE2_SPEED       140.0f

#define EXPL_PHASE3_PUFFS        10
#define EXPL_PHASE3_RADIUS        7.0f
#define EXPL_PHASE3_GROW         15.0f
#define EXPL_PHASE3_LIFE          4.0f    // linger longer
#define EXPL_PHASE3_RISE         33.8f    // +30% upward drift

// Flash (screen blend via cl.cshifts[CSHIFT_BONUS]).
#define EXPL_FLASH_MAX_DIST    1000.0f
#define EXPL_FLASH_MAX_PERCENT    55.0f   // peak strength when right next to it
#define EXPL_FLASH_FOV_BIAS       0.10f   // minimum dot(dir, vpn) to see it at all
#define EXPL_FLASH_R              255
#define EXPL_FLASH_G              240
#define EXPL_FLASH_B              180

// ---------------------------------------------------------------------------
// Puff kinds: drive the color ramp + alpha envelope inside the shader.
//   0 = PHASE1 bright (hot-white -> grey, short life, small)
//   1 = PHASE2 mid grey (grey, medium life, medium size, flies fast)
//   2 = PHASE3 dark grey (dark grey, long life, rises slowly)
#define PUFF_KIND_BRIGHT   0
#define PUFF_KIND_MID      1
#define PUFF_KIND_DARK     2

typedef struct {
	vec3_t pos;
	vec3_t vel;
	float  spawn_time;
	float  life;
	float  drag;     // per-puff drag coefficient
	float  rise;     // per-puff upward acceleration
	int    kind;     // PUFF_KIND_*
	float  radius;
	float  radius_grow;
	qboolean alive;
} puff_t;

typedef struct {
	vec3_t pos;
	vec3_t vel;
	float  spawn_time;
	float  seed;
	qboolean alive;
} spark_t;

typedef struct {
	vec3_t pos;
	float  spawn_time;
	qboolean alive;
	int    tint;       // 0 = orange (rocket), 1 = magenta (tarbaby)
} core_t;

static core_t  g_cores[EXPL_MAX];
static int     g_core_head = 0;

// Pending shrapnel: 0.2s after an explosion we fire 12 short tracelines
// outward and register a gunshot decal wherever each ray hits geometry.
// Gives every blast a scatter of bullet-hole marks on nearby walls.
#define EXPL_SHRAPNEL_DELAY    0.2f
#define EXPL_SHRAPNEL_RAYS     12
#define EXPL_SHRAPNEL_RANGE  200.0f
typedef struct {
	vec3_t origin;
	float  trigger_time;
	qboolean alive;
} shrapnel_t;
static shrapnel_t g_shrapnel[EXPL_MAX];
static int        g_shrapnel_head = 0;

extern void Gunshot_Register (const vec3_t pos);
static float frand01 (void);
static float frand_range (float lo, float hi);

static void Explosion_ScheduleShrapnel (const vec3_t origin)
{
	shrapnel_t *s = &g_shrapnel[g_shrapnel_head];
	g_shrapnel_head = (g_shrapnel_head + 1) % EXPL_MAX;
	VectorCopy(origin, s->origin);
	s->trigger_time = (float)realtime + EXPL_SHRAPNEL_DELAY;
	s->alive = true;
}

static void Explosion_ProcessShrapnel (float now)
{
	for (int i = 0; i < EXPL_MAX; ++i) {
		shrapnel_t *s = &g_shrapnel[i];
		if (!s->alive) continue;
		if (now < s->trigger_time) continue;
		s->alive = false;

		// Fire N rays in roughly-even directions across the sphere.
		for (int k = 0; k < EXPL_SHRAPNEL_RAYS; ++k) {
			float theta = frand_range(0.0f, 6.2831853f);
			float z     = frand_range(-0.95f, 0.95f);
			float xy    = sqrtf(1.0f - z*z);
			vec3_t dir  = { cosf(theta)*xy, sinf(theta)*xy, z };
			vec3_t end  = {
				s->origin[0] + dir[0] * EXPL_SHRAPNEL_RANGE,
				s->origin[1] + dir[1] * EXPL_SHRAPNEL_RANGE,
				s->origin[2] + dir[2] * EXPL_SHRAPNEL_RANGE,
			};
			vec3_t impact, normal;
			vec3_t start_nc = { s->origin[0], s->origin[1], s->origin[2] };
			float frac = TraceLine(start_nc, end, impact, normal);
			// TraceLine returns fraction < 1 on hit.
			if (frac < 1.0f)
				Gunshot_Register(impact);
		}
	}
}
static puff_t  g_puffs[MAX_PUFFS];
static int     g_puff_head = 0;
static spark_t g_sparks[MAX_SPARKS];
static int     g_spark_head = 0;

// Deterministic RNG.
static unsigned int g_rng = 0x1B3D7F99u;
static float frand01 (void)
{
	unsigned int x = g_rng;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	g_rng = x;
	return (float)(x & 0xFFFFFF) / (float)0x1000000;
}
static float frand_range (float lo, float hi) { return lo + (hi - lo) * frand01(); }

// ---------------------------------------------------------------------------
// Core-flash shader: huge soft disc, color cools over time.
// ---------------------------------------------------------------------------
static GLShader R_CoreShader;
static GLint    R_CoreShader_u_mvp       = -1;
static GLint    R_CoreShader_u_center    = -1;
static GLint    R_CoreShader_u_cam_right = -1;
static GLint    R_CoreShader_u_cam_up    = -1;
static GLint    R_CoreShader_u_radius    = -1;
static GLint    R_CoreShader_u_age01     = -1;
static GLint    R_CoreShader_u_tint      = -1;
static qboolean core_ok = false;

// Puff shader: like gunshot smoke, color ramps hot->dark->smoke.
static GLShader R_PuffShader;
static GLint    R_PuffShader_u_mvp       = -1;
static GLint    R_PuffShader_u_center    = -1;
static GLint    R_PuffShader_u_cam_right = -1;
static GLint    R_PuffShader_u_cam_up    = -1;
static GLint    R_PuffShader_u_radius    = -1;
static GLint    R_PuffShader_u_age01     = -1;
static GLint    R_PuffShader_u_seed      = -1;
static GLint    R_PuffShader_u_kind      = -1;   // 0 = primary, 1 = secondary (trail)
static qboolean puff_ok = false;

// Spark shader (GL_POINTS).
static GLShader R_SparkShader_X;
static GLint    R_SparkShader_u_mvp      = -1;
static GLint    R_SparkShader_u_time     = -1;
static GLint    R_SparkShader_u_life     = -1;
static GLint    R_SparkShader_u_point_sz = -1;
static qboolean spark_ok = false;

static GLuint quad_vao = 0, quad_vbo = 0;
static GLuint spark_vao = 0, spark_vbo = 0;

static qboolean R_EnsureCoreShader (void)
{
	if (core_ok) return true;
	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in float a_corner;\n"
		"uniform mat4  u_mvp;\n"
		"uniform vec3  u_center;\n"
		"uniform vec3  u_cam_right;\n"
		"uniform vec3  u_cam_up;\n"
		"uniform float u_radius;\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    int c = int(a_corner);\n"
		"    vec2 uv;\n"
		"    if      (c == 0 || c == 3) uv = vec2(0.0, 0.0);\n"
		"    else if (c == 1)           uv = vec2(1.0, 0.0);\n"
		"    else if (c == 2 || c == 4) uv = vec2(1.0, 1.0);\n"
		"    else                       uv = vec2(0.0, 1.0);\n"
		"    v_uv = uv;\n"
		"    vec2 o = (uv - 0.5) * 2.0 * u_radius;\n"
		"    vec3 pos = u_center + u_cam_right * o.x + u_cam_up * o.y;\n"
		"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
		"}\n";
	const char *fs =
		"#version 330 core\n"
		"in vec2 v_uv;\n"
		"uniform float u_age01;\n"   // 0 fresh, 1 expired
		"uniform int   u_tint;\n"    // 0 orange, 1 magenta
		"layout(location = 0) out vec4 frag_color;\n"
		"layout(location = 1) out vec4 frag_fbmask;\n"
		"void main() {\n"
		"    vec2 d = v_uv - 0.5;\n"
		"    float r2 = dot(d, d);\n"
		"    if (r2 > 0.25) discard;\n"
		"    float edge = 1.0 - smoothstep(0.00, 0.25, r2);\n"
		"    // Color ramp: hot white -> orange -> deep red -> dark smoke.\n"
		"    vec3 hot    = (u_tint == 1) ? vec3(1.00, 0.70, 1.00)\n"
		"                                : vec3(1.00, 0.95, 0.75);\n"
		"    vec3 warm   = (u_tint == 1) ? vec3(0.85, 0.10, 0.90)\n"
		"                                : vec3(1.00, 0.45, 0.10);\n"
		"    vec3 cool   = (u_tint == 1) ? vec3(0.30, 0.00, 0.35)\n"
		"                                : vec3(0.35, 0.07, 0.03);\n"
		"    vec3 smoke  = vec3(0.05, 0.04, 0.04);\n"
		"    vec3 col;\n"
		"    if (u_age01 < 0.18) {\n"
		"        col = mix(hot, warm, u_age01 / 0.18);\n"
		"    } else if (u_age01 < 0.55) {\n"
		"        col = mix(warm, cool, (u_age01 - 0.18) / 0.37);\n"
		"    } else {\n"
		"        col = mix(cool, smoke, (u_age01 - 0.55) / 0.45);\n"
		"    }\n"
		"    // Alpha envelope: very quick rise, long-ish fade.\n"
		"    float a_env = smoothstep(0.0, 0.05, u_age01) *\n"
		"                  (1.0 - smoothstep(0.4, 1.0, u_age01));\n"
		"    float alpha = edge * a_env;\n"
		"    frag_color  = vec4(col, alpha);\n"
		"    // Emitters during hot phase feed the bloom mask.\n"
		"    float emit = (1.0 - smoothstep(0.0, 0.35, u_age01));\n"
		"    frag_fbmask = vec4(col * alpha * emit, gl_FragCoord.z);\n"
		"}\n";
	char err[512];
	if (!GLShader_Build(&R_CoreShader, vs, fs, err, sizeof(err))) {
		Con_Printf("explosion core shader failed: %s\n", err);
		return false;
	}
	R_CoreShader_u_mvp       = GLShader_Uniform(&R_CoreShader, "u_mvp");
	R_CoreShader_u_center    = GLShader_Uniform(&R_CoreShader, "u_center");
	R_CoreShader_u_cam_right = GLShader_Uniform(&R_CoreShader, "u_cam_right");
	R_CoreShader_u_cam_up    = GLShader_Uniform(&R_CoreShader, "u_cam_up");
	R_CoreShader_u_radius    = GLShader_Uniform(&R_CoreShader, "u_radius");
	R_CoreShader_u_age01     = GLShader_Uniform(&R_CoreShader, "u_age01");
	R_CoreShader_u_tint      = GLShader_Uniform(&R_CoreShader, "u_tint");
	core_ok = true;
	return true;
}

static qboolean R_EnsurePuffShader (void)
{
	if (puff_ok) return true;
	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in float a_corner;\n"
		"uniform mat4  u_mvp;\n"
		"uniform vec3  u_center;\n"
		"uniform vec3  u_cam_right;\n"
		"uniform vec3  u_cam_up;\n"
		"uniform float u_radius;\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    int c = int(a_corner);\n"
		"    vec2 uv;\n"
		"    if      (c == 0 || c == 3) uv = vec2(0.0, 0.0);\n"
		"    else if (c == 1)           uv = vec2(1.0, 0.0);\n"
		"    else if (c == 2 || c == 4) uv = vec2(1.0, 1.0);\n"
		"    else                       uv = vec2(0.0, 1.0);\n"
		"    v_uv = uv;\n"
		"    vec2 o = (uv - 0.5) * 2.0 * u_radius;\n"
		"    vec3 pos = u_center + u_cam_right * o.x + u_cam_up * o.y;\n"
		"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
		"}\n";
	const char *fs =
		"#version 330 core\n"
		"in vec2 v_uv;\n"
		"uniform float u_age01;\n"
		"uniform float u_seed;\n"
		"uniform int   u_kind;\n"      // 0 bright, 1 mid grey, 2 dark grey (rising)
		"layout(location = 0) out vec4 frag_color;\n"
		"layout(location = 1) out vec4 frag_fbmask;\n"
		"float hash21(vec2 p) {\n"
		"    p = fract(p * vec2(123.34, 456.21));\n"
		"    p += dot(p, p + 45.32);\n"
		"    return fract(p.x * p.y);\n"
		"}\n"
		"float vnoise(vec2 p) {\n"
		"    vec2 i = floor(p);\n"
		"    vec2 f = fract(p);\n"
		"    vec2 u = f * f * (3.0 - 2.0 * f);\n"
		"    float a = hash21(i);\n"
		"    float b = hash21(i + vec2(1.0, 0.0));\n"
		"    float c = hash21(i + vec2(0.0, 1.0));\n"
		"    float d = hash21(i + vec2(1.0, 1.0));\n"
		"    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);\n"
		"}\n"
		"float fbm(vec2 p) {\n"
		"    float s = 0.0, a = 0.55;\n"
		"    for (int i = 0; i < 3; ++i) {\n"
		"        s += vnoise(p) * a;\n"
		"        p = p * 2.07 + 11.3;\n"
		"        a *= 0.55;\n"
		"    }\n"
		"    return s;\n"
		"}\n"
		"void main() {\n"
		"    // Distort the sample coordinate with a per-puff noise so the\n"
		"    // silhouette loses its perfectly circular shape. Distortion\n"
		"    // grows with age -> the young puff reads as a compact blob,\n"
		"    // the old one as a ragged cloud.\n"
		"    vec2 seed_off = vec2(u_seed * 17.0, u_seed * 31.0);\n"
		"    float nA = fbm(v_uv * 2.6 + seed_off) - 0.5;\n"
		"    float nB = fbm(v_uv * 2.6 + seed_off + 13.1) - 0.5;\n"
		"    float distort = 0.06 + 0.22 * u_age01;\n"
		"    vec2 uv_d = v_uv + vec2(nA, nB) * distort;\n"
		"\n"
		"    vec2 d  = uv_d - 0.5;\n"
		"    float r2 = dot(d, d);\n"
		"    if (r2 > 0.25) discard;\n"
		"    float base = 1.0 - smoothstep(0.04, 0.25, r2);\n"
		"\n"
		"    // Fine grain noise for the body so edges look 'dirty' and not\n"
		"    // a clean alpha fall-off. Older puffs pick up more grain.\n"
		"    float grain = fbm(uv_d * 3.4 + seed_off + u_age01 * 1.3);\n"
		"    float mask  = base * mix(0.85, 0.45 + 0.55 * grain, u_age01);\n"
		"\n"
		"    vec3 col;\n"
		"    float peak;\n"
		"    float a_env;\n"
		"    float emit = 0.0;\n"
		"    if (u_kind == 0) {\n"
		"        // PHASE 1: hot yellow-white -> white -> mid-grey, FAST.\n"
		"        vec3 hot   = vec3(1.00, 0.95, 0.55);\n"
		"        vec3 white = vec3(0.92, 0.92, 0.92);\n"
		"        vec3 grey  = vec3(0.55, 0.55, 0.55);\n"
		"        if      (u_age01 < 0.10) col = mix(hot, white, u_age01 / 0.10);\n"
		"        else if (u_age01 < 0.30) col = mix(white, grey, (u_age01 - 0.10) / 0.20);\n"
		"        else                     col = grey;\n"
		"        a_env = smoothstep(0.0, 0.05, u_age01) *\n"
		"                (1.0 - smoothstep(0.55, 1.0, u_age01));\n"
		"        peak  = 0.85;\n"
		"        emit  = 1.0 - smoothstep(0.0, 0.10, u_age01);\n"
		"    } else if (u_kind == 1) {\n"
		"        // PHASE 2: medium grey throughout, flying outward.\n"
		"        vec3 grey_hi = vec3(0.60, 0.60, 0.60);\n"
		"        vec3 grey_lo = vec3(0.32, 0.32, 0.32);\n"
		"        col = mix(grey_hi, grey_lo, u_age01);\n"
		"        a_env = smoothstep(0.0, 0.08, u_age01) *\n"
		"                (1.0 - smoothstep(0.5, 1.0, u_age01));\n"
		"        peak  = 0.55;\n"
		"    } else {\n"
		"        // PHASE 3: dark grey, rising slowly, long fade.\n"
		"        vec3 dark_hi = vec3(0.12, 0.12, 0.12);\n"
		"        vec3 dark_lo = vec3(0.02, 0.02, 0.02);\n"
		"        col = mix(dark_hi, dark_lo, u_age01);\n"
		"        a_env = smoothstep(0.0, 0.15, u_age01) *\n"
		"                (1.0 - smoothstep(0.55, 1.0, u_age01));\n"
		"        peak  = 0.60;\n"
		"    }\n"
		"    float alpha = mask * a_env * peak;\n"
		"    if (alpha < 0.01) discard;\n"
		"    frag_color = vec4(col, alpha);\n"
		"    frag_fbmask = vec4(col * alpha * emit, gl_FragCoord.z);\n"
		"}\n";
	char err[512];
	if (!GLShader_Build(&R_PuffShader, vs, fs, err, sizeof(err))) {
		Con_Printf("explosion puff shader failed: %s\n", err);
		return false;
	}
	R_PuffShader_u_mvp       = GLShader_Uniform(&R_PuffShader, "u_mvp");
	R_PuffShader_u_center    = GLShader_Uniform(&R_PuffShader, "u_center");
	R_PuffShader_u_cam_right = GLShader_Uniform(&R_PuffShader, "u_cam_right");
	R_PuffShader_u_cam_up    = GLShader_Uniform(&R_PuffShader, "u_cam_up");
	R_PuffShader_u_radius    = GLShader_Uniform(&R_PuffShader, "u_radius");
	R_PuffShader_u_age01     = GLShader_Uniform(&R_PuffShader, "u_age01");
	R_PuffShader_u_seed      = GLShader_Uniform(&R_PuffShader, "u_seed");
	R_PuffShader_u_kind      = GLShader_Uniform(&R_PuffShader, "u_kind");
	puff_ok = true;
	return true;
}

static qboolean R_EnsureExplSparkShader (void)
{
	if (spark_ok) return true;
	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in vec3  a_pos;\n"
		"layout(location = 1) in float a_age01;\n"
		"uniform mat4  u_mvp;\n"
		"uniform float u_point_sz;\n"
		"out float v_age01;\n"
		"void main() {\n"
		"    v_age01 = a_age01;\n"
		"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
		"    gl_PointSize = max(1.0, u_point_sz * (1.0 - a_age01 * 0.5));\n"
		"}\n";
	const char *fs =
		"#version 330 core\n"
		"in float v_age01;\n"
		"layout(location = 0) out vec4 frag_color;\n"
		"layout(location = 1) out vec4 frag_fbmask;\n"
		"void main() {\n"
		"    vec2 d = gl_PointCoord - vec2(0.5);\n"
		"    float r2 = dot(d, d);\n"
		"    if (r2 > 0.25) discard;\n"
		"    float edge = 1.0 - smoothstep(0.15, 0.25, r2);\n"
		"    vec3 hot  = vec3(1.00, 0.90, 0.40);\n"
		"    vec3 cold = vec3(0.85, 0.20, 0.05);\n"
		"    vec3 col  = mix(hot, cold, v_age01);\n"
		"    float life_a = (1.0 - v_age01);\n"
		"    float alpha  = edge * life_a * life_a;\n"
		"    frag_color  = vec4(col, alpha);\n"
		"    frag_fbmask = vec4(col * alpha, gl_FragCoord.z);\n"
		"}\n";
	char err[512];
	if (!GLShader_Build(&R_SparkShader_X, vs, fs, err, sizeof(err))) {
		Con_Printf("explosion spark shader failed: %s\n", err);
		return false;
	}
	R_SparkShader_u_mvp      = GLShader_Uniform(&R_SparkShader_X, "u_mvp");
	R_SparkShader_u_point_sz = GLShader_Uniform(&R_SparkShader_X, "u_point_sz");
	spark_ok = true;
	return true;
}

static void Quad_EnsureVAO (void)
{
	if (quad_vao) return;
	float corners[6] = { 0, 1, 2, 3, 4, 5 };
	glGenVertexArrays(1, &quad_vao);
	glGenBuffers(1, &quad_vbo);
	glBindVertexArray(quad_vao);
	glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 1, GL_FLOAT, GL_FALSE, sizeof(float), (void*)0);
	glBindVertexArray(0);
}

// Streaming spark VBO: rebuilt each frame with (pos, age01) per live spark.
typedef struct { float x, y, z; float age01; } spark_vtx_t;
static void Spark_EnsureVAO (void)
{
	if (spark_vao) return;
	glGenVertexArrays(1, &spark_vao);
	glGenBuffers(1, &spark_vbo);
	glBindVertexArray(spark_vao);
	glBindBuffer(GL_ARRAY_BUFFER, spark_vbo);
	glBufferData(GL_ARRAY_BUFFER, MAX_SPARKS * sizeof(spark_vtx_t), NULL, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(spark_vtx_t), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(spark_vtx_t), (void*)offsetof(spark_vtx_t, age01));
	glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
static puff_t *Explosion_AllocPuff (void)
{
	puff_t *p = &g_puffs[g_puff_head];
	g_puff_head = (g_puff_head + 1) % MAX_PUFFS;
	memset(p, 0, sizeof(*p));
	return p;
}

static void Explosion_SpawnThreePhase (const vec3_t origin)
{
	// Phase 1: bright, small, fast cool.
	for (int i = 0; i < EXPL_PHASE1_PUFFS; ++i) {
		puff_t *p = Explosion_AllocPuff();
		p->pos[0] = origin[0] + frand_range(-4.0f, 4.0f);
		p->pos[1] = origin[1] + frand_range(-4.0f, 4.0f);
		p->pos[2] = origin[2] + frand_range(-4.0f, 4.0f);
		float theta = frand_range(0.0f, 6.2831853f);
		float z = frand_range(-0.7f, 0.7f);
		float xy = sqrtf(1.0f - z*z);
		float spd = EXPL_PHASE1_SPEED * frand_range(0.5f, 1.2f);
		p->vel[0] = cosf(theta) * xy * spd;
		p->vel[1] = sinf(theta) * xy * spd;
		p->vel[2] = z * spd;
		p->drag = frand_range(0.85f, 0.94f);
		p->rise = 0.0f;
		p->life = EXPL_PHASE1_LIFE * frand_range(0.8f, 1.2f);
		p->kind = PUFF_KIND_BRIGHT;
		p->radius      = EXPL_PHASE1_RADIUS * frand_range(0.8f, 1.2f);
		p->radius_grow = EXPL_PHASE1_GROW;
		p->spawn_time  = (float)realtime;
		p->alive = true;
	}

	// Phase 2 (mid grey) disabled: dense generic puffs obscured the view.
	// Keep the tunables / shader branch wired up in case we want to revive
	// this phase later with a noise-textured cloud look.

	// Phase 3: dark grey, slow-rising, lingering smoke.
	for (int i = 0; i < EXPL_PHASE3_PUFFS; ++i) {
		puff_t *p = Explosion_AllocPuff();
		p->pos[0] = origin[0] + frand_range(-12.5f, 12.5f);
		p->pos[1] = origin[1] + frand_range(-12.5f, 12.5f);
		p->pos[2] = origin[2] + frand_range(-5.0f, 7.5f);
		p->vel[0] = frand_range(-12.5f, 12.5f);
		p->vel[1] = frand_range(-12.5f, 12.5f);
		p->vel[2] = EXPL_PHASE3_RISE * frand_range(0.7f, 1.3f);
		p->drag = frand_range(0.93f, 0.98f);   // gentle drag so they keep drifting up
		p->rise = 0.0f;                         // rise is in the initial vel
		p->life = EXPL_PHASE3_LIFE * frand_range(0.85f, 1.2f);
		p->kind = PUFF_KIND_DARK;
		p->radius      = EXPL_PHASE3_RADIUS * frand_range(0.9f, 1.2f);
		p->radius_grow = EXPL_PHASE3_GROW;
		p->spawn_time  = (float)realtime + frand_range(0.15f, 0.35f);
		p->alive = true;
	}
}

static void Explosion_SpawnSparks (const vec3_t origin)
{
	for (int i = 0; i < EXPL_SPARKS; ++i) {
		spark_t *s = &g_sparks[g_spark_head];
		g_spark_head = (g_spark_head + 1) % MAX_SPARKS;
		float theta = frand_range(0.0f, 6.2831853f);
		float z = frand_range(-0.9f, 0.9f);
		float xy = sqrtf(1.0f - z*z);
		vec3_t dir = { cosf(theta) * xy, sinf(theta) * xy, z };
		// Spawn at a radius >= EXPL_SPARK_MIN_DIST from the origin so the
		// cinders look flung outward rather than piling at the blast core.
		float r = EXPL_SPARK_MIN_DIST + frand_range(0.0f, 20.0f);
		s->pos[0] = origin[0] + dir[0] * r;
		s->pos[1] = origin[1] + dir[1] * r;
		s->pos[2] = origin[2] + dir[2] * r;
		float spd = EXPL_SPARK_SPEED * frand_range(0.5f, 1.1f);
		s->vel[0] = dir[0] * spd;
		s->vel[1] = dir[1] * spd;
		s->vel[2] = dir[2] * spd + 30.0f;    // slight upward bias
		s->spawn_time = (float)realtime;
		s->seed = frand01();
		s->alive = true;
	}
}

// ---------------------------------------------------------------------------
// Public: called from CL_ParseTEnt.
// ---------------------------------------------------------------------------
// Screen flash when an explosion goes off within sight and field-of-view.
// Uses cl.cshifts[CSHIFT_BONUS] like the pickup flash. Strength falls off
// with distance (0 at EXPL_FLASH_MAX_DIST) and with the cosine of the
// angle between the view direction and the explosion (0 when behind camera).
// A short raycast check prevents flashing through walls.
static void Explosion_TriggerFlash (const vec3_t origin)
{
	if (!cl.worldmodel) return;   // no map -> bail

	vec3_t to_expl;
	VectorSubtract(origin, r_refdef.vieworg, to_expl);
	float dist2 = DotProduct(to_expl, to_expl);
	float max_d = EXPL_FLASH_MAX_DIST;
	if (dist2 >= max_d * max_d) return;
	float dist = sqrtf(dist2);
	if (dist < 1.0f) dist = 1.0f;

	// Angle factor: dot(normalize(to_expl), vpn). vpn is camera forward.
	vec3_t dir = { to_expl[0] / dist, to_expl[1] / dist, to_expl[2] / dist };
	float dot = dir[0]*vpn[0] + dir[1]*vpn[1] + dir[2]*vpn[2];
	if (dot < EXPL_FLASH_FOV_BIAS) return;   // behind/off-axis -> no flash

	// Occlusion check: if a wall is between the camera and the explosion,
	// we still want SOME flash (bright light round the corner), so require
	// only that the ray reaches at least 80% of the way.
	vec3_t impact, normal;
	// TraceLine's sig isn't const-correct.
	vec3_t start_nc = { r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2] };
	vec3_t end_nc   = { origin[0], origin[1], origin[2] };
	float frac = TraceLine(start_nc, end_nc, impact, normal);
	if (frac < 0.8f) return;    // blocked by geometry

	float near_k = 1.0f - (dist / max_d);    // 1 near, 0 far
	near_k *= near_k;                         // quadratic falloff
	float fov_k  = (dot - EXPL_FLASH_FOV_BIAS) / (1.0f - EXPL_FLASH_FOV_BIAS);
	float percent = EXPL_FLASH_MAX_PERCENT * near_k * fov_k;
	if (percent < 2.0f) return;

	// Additive to existing bonus flash: a chain of explosions should stack.
	int prev = (int)cl.cshifts[CSHIFT_BONUS].percent;
	int total = prev + (int)percent;
	if (total > 120) total = 120;

	cl.cshifts[CSHIFT_BONUS].destcolor[0] = EXPL_FLASH_R;
	cl.cshifts[CSHIFT_BONUS].destcolor[1] = EXPL_FLASH_G;
	cl.cshifts[CSHIFT_BONUS].destcolor[2] = EXPL_FLASH_B;
	cl.cshifts[CSHIFT_BONUS].percent      = total;
}

void Explosion_Spawn (const vec3_t origin, int tint /* 0=rocket, 1=tarbaby */)
{
	core_t *c = &g_cores[g_core_head];
	g_core_head = (g_core_head + 1) % EXPL_MAX;
	VectorCopy(origin, c->pos);
	c->spawn_time = (float)realtime;
	c->tint  = tint;
	c->alive = true;
	Explosion_SpawnThreePhase(origin);
	Explosion_SpawnSparks(origin);
	Explosion_TriggerFlash(origin);
	Explosion_ScheduleShrapnel(origin);
}

// ---------------------------------------------------------------------------
void Explosion_Draw (void)
{
	if (!R_EnsureCoreShader())       return;
	if (!R_EnsurePuffShader())       return;
	if (!R_EnsureExplSparkShader()) return;
	Quad_EnsureVAO();
	Spark_EnsureVAO();

	float now = (float)realtime;
	static float prev = 0.0f;
	float dt = now - prev;
	if (dt < 0.0f || dt > 0.2f) dt = 0.016f;
	prev = now;

	// Fire any scheduled shrapnel rays whose delay has elapsed. Each hit
	// registers a gunshot decal at the impact point.
	Explosion_ProcessShrapnel(now);

	// Step puffs.
	for (int i = 0; i < MAX_PUFFS; ++i) {
		puff_t *p = &g_puffs[i];
		if (!p->alive) continue;
		float age = now - p->spawn_time;
		if (age < 0.0f) continue;
		if (age >= p->life) { p->alive = false; continue; }
		p->pos[0] += p->vel[0] * dt;
		p->pos[1] += p->vel[1] * dt;
		p->pos[2] += p->vel[2] * dt;
		p->vel[0] *= p->drag;
		p->vel[1] *= p->drag;
		p->vel[2] *= p->drag;
		p->vel[2] += p->rise * dt;
	}

	// Step sparks.
	for (int i = 0; i < MAX_SPARKS; ++i) {
		spark_t *s = &g_sparks[i];
		if (!s->alive) continue;
		float age = now - s->spawn_time;
		if (age >= EXPL_SPARK_LIFE) { s->alive = false; continue; }
		s->pos[0] += s->vel[0] * dt;
		s->pos[1] += s->vel[1] * dt;
		s->pos[2] += s->vel[2] * dt;
		s->vel[2] -= EXPL_SPARK_GRAVITY * dt;
		s->vel[0] *= 0.96f; s->vel[1] *= 0.96f; s->vel[2] *= 0.98f;
	}

	float mvp[16];
	R_CurrentMVP(mvp);

	// Shared state setup.
	GLboolean blend_was = glIsEnabled(GL_BLEND);
	GLboolean cull_was  = glIsEnabled(GL_CULL_FACE);
	GLboolean dt_was    = glIsEnabled(GL_DEPTH_TEST);
	GLboolean dmask_was; glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask_was);
	GLint src_rgb, dst_rgb, src_a, dst_a;
	glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);

	if (!blend_was) glEnable(GL_BLEND);
	if (!dt_was)    glEnable(GL_DEPTH_TEST);
	if (cull_was)   glDisable(GL_CULL_FACE);
	glDepthMask(GL_FALSE);

	// ---- Core flash pass (alpha-over) ----
	glBindVertexArray(quad_vao);
	GLShader_Use(&R_CoreShader);
	glUniformMatrix4fv(R_CoreShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform3f(R_CoreShader_u_cam_right, vright[0], vright[1], vright[2]);
	glUniform3f(R_CoreShader_u_cam_up,    vup[0],    vup[1],    vup[2]);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	for (int i = 0; i < EXPL_MAX; ++i) {
		core_t *c = &g_cores[i];
		if (!c->alive) continue;
		float age = now - c->spawn_time;
		if (age >= EXPL_CORE_LIFE) { c->alive = false; continue; }
		float age01 = age / EXPL_CORE_LIFE;
		// Size grows fast during the flash, then stays.
		float r = EXPL_CORE_RADIUS * (0.6f + 1.4f * (1.0f - (1.0f - age01)*(1.0f - age01)));
		glUniform3f(R_CoreShader_u_center, c->pos[0], c->pos[1], c->pos[2]);
		glUniform1f(R_CoreShader_u_radius, r);
		glUniform1f(R_CoreShader_u_age01,  age01);
		glUniform1i(R_CoreShader_u_tint,   c->tint);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	// ---- Trail puff pass ----
	GLShader_Use(&R_PuffShader);
	glUniformMatrix4fv(R_PuffShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform3f(R_PuffShader_u_cam_right, vright[0], vright[1], vright[2]);
	glUniform3f(R_PuffShader_u_cam_up,    vup[0],    vup[1],    vup[2]);
	for (int i = 0; i < MAX_PUFFS; ++i) {
		puff_t *p = &g_puffs[i];
		if (!p->alive) continue;
		float age = now - p->spawn_time;
		if (age < 0.0f || age >= p->life) continue;
		float age01 = age / p->life;
		float r = p->radius + (p->radius_grow - p->radius) * age01;
		glUniform3f(R_PuffShader_u_center, p->pos[0], p->pos[1], p->pos[2]);
		glUniform1f(R_PuffShader_u_radius, r);
		glUniform1f(R_PuffShader_u_age01,  age01);
		glUniform1f(R_PuffShader_u_seed,
		            (float)((unsigned)(uintptr_t)p & 0xFFFFu) / 65535.0f);
		glUniform1i(R_PuffShader_u_kind,   p->kind);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	// ---- Sparks pass (GL_POINTS, additive) ----
	int nsparks = 0;
	static spark_vtx_t sverts[MAX_SPARKS];
	for (int i = 0; i < MAX_SPARKS; ++i) {
		spark_t *s = &g_sparks[i];
		if (!s->alive) continue;
		float age = now - s->spawn_time;
		if (age >= EXPL_SPARK_LIFE) continue;
		sverts[nsparks].x = s->pos[0];
		sverts[nsparks].y = s->pos[1];
		sverts[nsparks].z = s->pos[2];
		sverts[nsparks].age01 = age / EXPL_SPARK_LIFE;
		nsparks++;
	}
	if (nsparks > 0) {
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glEnable(GL_PROGRAM_POINT_SIZE);
		GLShader_Use(&R_SparkShader_X);
		glUniformMatrix4fv(R_SparkShader_u_mvp, 1, GL_FALSE, mvp);
		glUniform1f(R_SparkShader_u_point_sz, EXPL_SPARK_POINT_PX);
		glBindVertexArray(spark_vao);
		glBindBuffer(GL_ARRAY_BUFFER, spark_vbo);
		glBufferSubData(GL_ARRAY_BUFFER, 0, nsparks * sizeof(spark_vtx_t), sverts);
		glDrawArrays(GL_POINTS, 0, nsparks);
		glDisable(GL_PROGRAM_POINT_SIZE);
	}

	// Restore.
	glDepthMask(dmask_was);
	if (!dt_was)  glDisable(GL_DEPTH_TEST);
	if (cull_was) glEnable(GL_CULL_FACE);
	glBlendFunc(src_rgb, dst_rgb);
	if (!blend_was) glDisable(GL_BLEND);
	glBindVertexArray(0);
	glUseProgram(0);
}

void Explosion_Init (void)
{
	// No cvars or commands yet.
}
