// gl_gunshot_decal.cpp -- gunshot impact effects on walls.
//
// Two visual layers:
//
//   1. A small glowing point at the impact site. Hot (yellow-white) at
//      spawn, cooling through orange to deep red to dark in ~6 seconds.
//      Oriented flat against the wall using a surface-normal estimate.
//
//   2. A smoke plume: short-lived particles emitted during the first
//      ~1.5 s of the decal's life, drifting upward and being pushed by
//      a gentle wind vector until they fade out.
//
// Normal estimation: the server sends only an impact position, so we
// probe 18 directions (6 axial + 12 diagonals at 45 deg on the principal
// planes) via TraceLine. The shortest hit's opposite direction is the
// wall normal. Covers essentially every BSP surface in Quake. If the
// probes miss (pos dangling far from geometry), fall back to -vpn.

#include "quakedef.h"
#include "gl_render.h"
#include <stdint.h>

extern float TraceLine (vec3_t start, vec3_t end, vec3_t impact, vec3_t normal);
extern vec3_t vpn, vright, vup;

// ---------------------------------------------------------------------------
// Tunables.
// ---------------------------------------------------------------------------
#define DECAL_RING_SIZE        128
#define DECAL_LIFETIME         6.0f    // total visible seconds
#define GLOW_RADIUS            2.8f    // world units, glow quad half-size
#define PROBE_TRACE_LEN       32.0f
#define WALL_OFFSET            0.25f   // lift glow quad off the wall

#define SMOKE_PARTICLES_MAX    1024
#define SMOKE_EMIT_TIME        4.0f    // seconds after spawn where smoke is emitted
#define SMOKE_EMIT_RATE_START  10.0f   // particles/sec at t=0
#define SMOKE_EMIT_RATE_END     1.5f   // particles/sec at t=SMOKE_EMIT_TIME
#define SMOKE_PARTICLE_LIFE    4.5f
#define SMOKE_SIZE_START       1.6f
#define SMOKE_SIZE_END         8.0f    // grows as it drifts / dissipates
#define SMOKE_RISE_SPEED      12.0f    // units/sec upward (world +z)

// Global ambient wind applied to every live smoke particle. Evolves slowly
// with occasional gusts -- the smoke feels like it's drifting through real
// air, not emitted straight up. Two-layer model:
//   - base drift: a low-frequency sine field for direction changes
//   - gusts: short, higher-amplitude bursts from a decaying event
#define WIND_BASE_SPEED        4.0f   // typical magnitude of base drift (u/s)
#define WIND_GUST_SPEED       18.0f   // peak during a gust (u/s)
#define WIND_GUST_MIN_INTERVAL 4.0f   // seconds between gust events (min)
#define WIND_GUST_MAX_INTERVAL 9.0f   // seconds between gust events (max)
#define WIND_GUST_DURATION     2.5f   // seconds a gust lingers (decays)

// Distance fade. Smoke at u_eye closer than FADE_NEAR is fully opaque;
// beyond FADE_FAR it's fully transparent. Linear roll-off in between.
#define SMOKE_FADE_NEAR       200.0f
#define SMOKE_FADE_FAR       1200.0f

// ---------------------------------------------------------------------------
typedef struct {
	vec3_t pos;
	vec3_t normal;
	float  spawn_time;
	int    method;           // 2 = probe hit, 1 = vpn fallback, 0 = empty slot
	float  next_emit_time;   // next scheduled smoke emission
} decal_t;

typedef struct {
	vec3_t pos;
	vec3_t vel;
	float  spawn_time;
	float  seed;             // for texture/animation decorrelation
	qboolean alive;
} smoke_t;

static decal_t decals[DECAL_RING_SIZE];
static int     decal_head = 0;

static smoke_t smoke[SMOKE_PARTICLES_MAX];
static int     smoke_head = 0;

// Global wind state. `base_dir` is a horizontal unit vector that rotates
// slowly via a low-frequency sine on realtime. Gusts accumulate on top.
static vec3_t  wind_gust_dir   = { 1, 0, 0 };
static float   wind_gust_start = -100.0f;   // seconds; negative = no gust yet
static float   wind_next_gust  = 2.0f;      // when to spawn the next gust

static cvar_t r_gunshot_debug = {"r_gunshot_debug", "0"};

// ---------------------------------------------------------------------------
// Shared hash for scatter / noise seeding.
// ---------------------------------------------------------------------------
static unsigned int g_rng = 0x12345678u;
static float frand01 (void)
{
	unsigned int x = g_rng;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	g_rng = x;
	return (float)(x & 0xFFFFFF) / (float)0x1000000;
}
static float frand_range (float lo, float hi) { return lo + (hi - lo) * frand01(); }

// Compute current world-space wind vector. Mostly horizontal with a tiny
// vertical bias, smooth-changing over seconds, with occasional gusts.
static void Wind_Eval (float now, vec3_t out)
{
	// Base drift direction slowly spins around Z using two incommensurate
	// frequencies so the pattern doesn't audibly loop.
	float a = now * 0.08f;
	float b = now * 0.031f;
	float dirx = cosf(a) * 0.9f + cosf(b * 2.1f) * 0.25f;
	float diry = sinf(a) * 0.9f + sinf(b * 1.7f) * 0.25f;
	float inv = 1.0f / sqrtf(dirx*dirx + diry*diry + 1e-6f);
	dirx *= inv; diry *= inv;
	// Base magnitude also modulates slowly so the wind "breathes".
	float mag = WIND_BASE_SPEED * (0.6f + 0.4f * sinf(now * 0.21f));

	out[0] = dirx * mag;
	out[1] = diry * mag;
	out[2] = 0.0f;

	// Schedule a new gust if due.
	if (now >= wind_next_gust) {
		// Pick a direction biased around the current base drift, plus
		// some scatter so gusts don't always blow the same way.
		float jitter = frand_range(-1.2f, 1.2f);
		float gx = dirx * cosf(jitter) - diry * sinf(jitter);
		float gy = dirx * sinf(jitter) + diry * cosf(jitter);
		wind_gust_dir[0] = gx;
		wind_gust_dir[1] = gy;
		wind_gust_dir[2] = frand_range(-0.1f, 0.2f);
		wind_gust_start  = now;
		wind_next_gust   = now + frand_range(WIND_GUST_MIN_INTERVAL, WIND_GUST_MAX_INTERVAL);
	}

	// Gust envelope: sharp rise, exponential-ish decay.
	float t_g = now - wind_gust_start;
	if (t_g >= 0.0f && t_g < WIND_GUST_DURATION) {
		float u    = t_g / WIND_GUST_DURATION;         // 0..1
		float env  = (1.0f - u) * (1.0f - u);          // fade-out curve
		float rise = 1.0f - (1.0f - u) * (1.0f - u);   // fast rise at start
		// Combined: spike near t=0.15, decay after.
		float k = rise * 0.35f + env * 0.65f;
		float gmag = WIND_GUST_SPEED * k;
		out[0] += wind_gust_dir[0] * gmag;
		out[1] += wind_gust_dir[1] * gmag;
		out[2] += wind_gust_dir[2] * gmag;
	}
}

// ===========================================================================
// Normal estimation (18 probe directions).
// ===========================================================================
static void Decal_EstimateNormal (const vec3_t pos, vec3_t out_normal, int *out_method)
{
	static const float probe_dirs[18][3] = {
		{  1,  0,  0}, { -1,  0,  0},
		{  0,  1,  0}, {  0, -1,  0},
		{  0,  0,  1}, {  0,  0, -1},
		{  0.7071f,  0.7071f,  0}, {  0.7071f, -0.7071f,  0},
		{ -0.7071f,  0.7071f,  0}, { -0.7071f, -0.7071f,  0},
		{  0.7071f,  0,  0.7071f}, {  0.7071f,  0, -0.7071f},
		{ -0.7071f,  0,  0.7071f}, { -0.7071f,  0, -0.7071f},
		{  0,  0.7071f,  0.7071f}, {  0,  0.7071f, -0.7071f},
		{  0, -0.7071f,  0.7071f}, {  0, -0.7071f, -0.7071f},
	};
	float best_dist = PROBE_TRACE_LEN + 0.1f;
	int   best_idx  = -1;
	for (int i = 0; i < 18; ++i) {
		vec3_t start, end, impact, norm;
		start[0] = pos[0] - probe_dirs[i][0] * 0.5f;
		start[1] = pos[1] - probe_dirs[i][1] * 0.5f;
		start[2] = pos[2] - probe_dirs[i][2] * 0.5f;
		end[0]   = start[0] + probe_dirs[i][0] * PROBE_TRACE_LEN;
		end[1]   = start[1] + probe_dirs[i][1] * PROBE_TRACE_LEN;
		end[2]   = start[2] + probe_dirs[i][2] * PROBE_TRACE_LEN;
		float frac = TraceLine(start, end, impact, norm);
		if (frac >= 1.0f) continue;
		float d = frac * PROBE_TRACE_LEN;
		if (d < best_dist) { best_dist = d; best_idx = i; }
	}
	if (best_idx >= 0) {
		out_normal[0] = -probe_dirs[best_idx][0];
		out_normal[1] = -probe_dirs[best_idx][1];
		out_normal[2] = -probe_dirs[best_idx][2];
		*out_method = 2;
		return;
	}
	out_normal[0] = -vpn[0]; out_normal[1] = -vpn[1]; out_normal[2] = -vpn[2];
	*out_method = 1;
}

// Build two orthonormal vectors in the plane perpendicular to `n`.
static void Decal_BuildAxes (const vec3_t n, vec3_t out_right, vec3_t out_up)
{
	vec3_t helper = { 0, 0, 1 };
	if (fabsf(n[2]) > 0.9f) { helper[0] = 1; helper[1] = 0; helper[2] = 0; }
	out_right[0] = n[1] * helper[2] - n[2] * helper[1];
	out_right[1] = n[2] * helper[0] - n[0] * helper[2];
	out_right[2] = n[0] * helper[1] - n[1] * helper[0];
	float l = sqrtf(DotProduct(out_right, out_right));
	if (l < 1e-4f) { out_right[0]=1; out_right[1]=0; out_right[2]=0; l=1; }
	out_right[0] /= l; out_right[1] /= l; out_right[2] /= l;
	out_up[0] = n[1] * out_right[2] - n[2] * out_right[1];
	out_up[1] = n[2] * out_right[0] - n[0] * out_right[2];
	out_up[2] = n[0] * out_right[1] - n[1] * out_right[0];
}

// ===========================================================================
// Glow shader (wall-aligned disk, color cools over time).
// ===========================================================================
static GLShader R_GlowShader;
static GLint    R_GlowShader_u_mvp    = -1;
static GLint    R_GlowShader_u_center = -1;
static GLint    R_GlowShader_u_right  = -1;
static GLint    R_GlowShader_u_up     = -1;
static GLint    R_GlowShader_u_radius = -1;
static GLint    R_GlowShader_u_color  = -1;
static GLint    R_GlowShader_u_alpha  = -1;
static qboolean glow_ok = false;

static GLuint   quad_vao = 0, quad_vbo = 0;

static qboolean R_EnsureGlowShader (void)
{
	if (glow_ok) return true;
	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in float a_corner;\n"
		"uniform mat4  u_mvp;\n"
		"uniform vec3  u_center;\n"
		"uniform vec3  u_right;\n"
		"uniform vec3  u_up;\n"
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
		"    vec3 pos = u_center + u_right * o.x + u_up * o.y;\n"
		"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
		"}\n";
	const char *fs =
		"#version 330 core\n"
		"in vec2 v_uv;\n"
		"uniform vec3  u_color;\n"
		"uniform float u_alpha;\n"
		"out vec4 frag_color;\n"
		"void main() {\n"
		"    vec2 d = v_uv - 0.5;\n"
		"    float r2 = dot(d, d);\n"
		"    if (r2 > 0.25) discard;\n"
		"    // Sharp-ish core, soft edge.\n"
		"    float core = 1.0 - smoothstep(0.0, 0.06, r2);\n"
		"    float halo = 1.0 - smoothstep(0.06, 0.25, r2);\n"
		"    float m = max(core, halo * 0.5);\n"
		"    frag_color = vec4(u_color * m * u_alpha, m * u_alpha);\n"
		"}\n";
	char err[512];
	if (!GLShader_Build(&R_GlowShader, vs, fs, err, sizeof(err))) {
		Con_Printf("gunshot glow shader failed: %s\n", err);
		return false;
	}
	R_GlowShader_u_mvp    = GLShader_Uniform(&R_GlowShader, "u_mvp");
	R_GlowShader_u_center = GLShader_Uniform(&R_GlowShader, "u_center");
	R_GlowShader_u_right  = GLShader_Uniform(&R_GlowShader, "u_right");
	R_GlowShader_u_up     = GLShader_Uniform(&R_GlowShader, "u_up");
	R_GlowShader_u_radius = GLShader_Uniform(&R_GlowShader, "u_radius");
	R_GlowShader_u_color  = GLShader_Uniform(&R_GlowShader, "u_color");
	R_GlowShader_u_alpha  = GLShader_Uniform(&R_GlowShader, "u_alpha");
	glow_ok = true;
	return true;
}

// ===========================================================================
// Smoke shader (view-aligned billboard, age-driven color + size).
// ===========================================================================
static GLShader R_SmokeShader;
static GLint    R_SmokeShader_u_mvp       = -1;
static GLint    R_SmokeShader_u_center    = -1;
static GLint    R_SmokeShader_u_cam_right = -1;
static GLint    R_SmokeShader_u_cam_up    = -1;
static GLint    R_SmokeShader_u_radius    = -1;
static GLint    R_SmokeShader_u_age01     = -1;   // 0 fresh .. 1 expired
static GLint    R_SmokeShader_u_seed      = -1;
static GLint    R_SmokeShader_u_eye       = -1;
static GLint    R_SmokeShader_u_fade_near = -1;
static GLint    R_SmokeShader_u_fade_far  = -1;
static qboolean smoke_ok = false;

static qboolean R_EnsureSmokeShader (void)
{
	if (smoke_ok) return true;
	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in float a_corner;\n"
		"uniform mat4  u_mvp;\n"
		"uniform vec3  u_center;\n"
		"uniform vec3  u_cam_right;\n"
		"uniform vec3  u_cam_up;\n"
		"uniform float u_radius;\n"
		"uniform vec3  u_eye;\n"
		"uniform float u_fade_near;\n"
		"uniform float u_fade_far;\n"
		"out vec2  v_uv;\n"
		"out float v_dist_fade;\n"        // 1 near, 0 far
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
		"    float dist = length(u_center - u_eye);\n"
		"    v_dist_fade = 1.0 - clamp((dist - u_fade_near) / max(u_fade_far - u_fade_near, 1.0), 0.0, 1.0);\n"
		"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
		"}\n";
	const char *fs =
		"#version 330 core\n"
		"in vec2  v_uv;\n"
		"in float v_dist_fade;\n"
		"uniform float u_age01;\n"
		"uniform float u_seed;\n"
		"out vec4 frag_color;\n"
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
		"void main() {\n"
		"    vec2 d = v_uv - 0.5;\n"
		"    float r2 = dot(d, d);\n"
		"    if (r2 > 0.25) discard;\n"
		"    // Soft circular puff with noise to break the outline.\n"
		"    float base = 1.0 - smoothstep(0.05, 0.25, r2);\n"
		"    vec2 np = v_uv * 2.3 + vec2(u_seed * 17.0, u_seed * 31.0);\n"
		"    float n = vnoise(np) * 0.6 + vnoise(np * 2.1 + 7.1) * 0.4;\n"
		"    float mask = base * (0.45 + 0.55 * n);\n"
		"    // Color: darker grey when fresh (still warm near the muzzle),\n"
		"    // lightening and more transparent as it disperses.\n"
		"    float grey = mix(0.25, 0.55, u_age01);\n"
		"    // Alpha envelope: quick build to a modest peak, then fade out\n"
		"    // across the rest of the lifetime. Peak alpha is kept low so\n"
		"    // overlapping puffs don't pile into an opaque cloud.\n"
		"    float alpha_env = smoothstep(0.0, 0.12, u_age01) *\n"
		"                      (1.0 - smoothstep(0.20, 0.95, u_age01));\n"
		"    float alpha = mask * alpha_env * 0.30 * v_dist_fade;\n"
		"    if (alpha < 0.01) discard;\n"
		"    frag_color = vec4(vec3(grey), alpha);\n"
		"}\n";
	char err[512];
	if (!GLShader_Build(&R_SmokeShader, vs, fs, err, sizeof(err))) {
		Con_Printf("gunshot smoke shader failed: %s\n", err);
		return false;
	}
	R_SmokeShader_u_mvp       = GLShader_Uniform(&R_SmokeShader, "u_mvp");
	R_SmokeShader_u_center    = GLShader_Uniform(&R_SmokeShader, "u_center");
	R_SmokeShader_u_cam_right = GLShader_Uniform(&R_SmokeShader, "u_cam_right");
	R_SmokeShader_u_cam_up    = GLShader_Uniform(&R_SmokeShader, "u_cam_up");
	R_SmokeShader_u_radius    = GLShader_Uniform(&R_SmokeShader, "u_radius");
	R_SmokeShader_u_age01     = GLShader_Uniform(&R_SmokeShader, "u_age01");
	R_SmokeShader_u_seed      = GLShader_Uniform(&R_SmokeShader, "u_seed");
	R_SmokeShader_u_eye       = GLShader_Uniform(&R_SmokeShader, "u_eye");
	R_SmokeShader_u_fade_near = GLShader_Uniform(&R_SmokeShader, "u_fade_near");
	R_SmokeShader_u_fade_far  = GLShader_Uniform(&R_SmokeShader, "u_fade_far");
	smoke_ok = true;
	return true;
}

// Shared VAO (6 corner indices) reused by both glow and smoke.
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

// ===========================================================================
// Smoke emission / stepping.
// ===========================================================================
static void Smoke_Spawn (const vec3_t pos, const vec3_t normal)
{
	smoke_t *p = &smoke[smoke_head];
	smoke_head = (smoke_head + 1) % SMOKE_PARTICLES_MAX;
	// Spawn slightly off the wall along the normal so the puff doesn't
	// birth inside geometry.
	p->pos[0] = pos[0] + normal[0] * 1.5f + frand_range(-1.0f, 1.0f);
	p->pos[1] = pos[1] + normal[1] * 1.5f + frand_range(-1.0f, 1.0f);
	p->pos[2] = pos[2] + normal[2] * 1.5f + frand_range(-1.0f, 1.0f);
	// Initial velocity: a bit away from the wall + upward + small random.
	p->vel[0] = normal[0] * 3.0f + frand_range(-2.0f, 2.0f);
	p->vel[1] = normal[1] * 3.0f + frand_range(-2.0f, 2.0f);
	p->vel[2] = normal[2] * 3.0f + SMOKE_RISE_SPEED + frand_range(-1.0f, 1.0f);
	p->spawn_time = (float)realtime;
	p->seed = frand01();
	p->alive = true;
}

// ===========================================================================
// Public: spawn a decal. Called from CL_ParseTEnt / TE_GUNSHOT.
// ===========================================================================
void Gunshot_Register (const vec3_t pos)
{
	decal_t *d = &decals[decal_head];
	decal_head = (decal_head + 1) % DECAL_RING_SIZE;

	VectorCopy(pos, d->pos);
	Decal_EstimateNormal(pos, d->normal, &d->method);
	d->spawn_time = (float)realtime;
	d->next_emit_time = d->spawn_time;

	if (r_gunshot_debug.value) {
		Con_Printf("gunshot: pos(%.1f %.1f %.1f) method=%d normal(%.2f %.2f %.2f)\n",
		           pos[0], pos[1], pos[2], d->method,
		           d->normal[0], d->normal[1], d->normal[2]);
	}
}

// ===========================================================================
// Draw: call once per frame after opaque world / alias / lava, before
// water and other translucents.
// ===========================================================================
void Gunshot_Draw (void)
{
	if (!cl.worldmodel) return;
	if (!R_EnsureGlowShader()) return;
	if (!R_EnsureSmokeShader()) return;
	Quad_EnsureVAO();

	float now = (float)realtime;
	float mvp[16];
	R_CurrentMVP(mvp);

	// ---- step smoke particles + emit new ones from live decals ----
	// Emission cadence: each decal schedules the next emission time; if
	// it's in the past and the decal is still in the smoke-emission
	// window, pop a particle. This keeps emission rate independent of
	// frame rate without needing per-frame integration.
	static float prev_step = 0.0f;
	float dt = now - prev_step;
	if (dt < 0.0f || dt > 0.2f) dt = 0.016f;
	prev_step = now;

	for (int i = 0; i < DECAL_RING_SIZE; ++i) {
		decal_t *d = &decals[i];
		if (d->method == 0) continue;
		float age = now - d->spawn_time;
		if (age >= SMOKE_EMIT_TIME) continue;
		while (now >= d->next_emit_time) {
			Smoke_Spawn(d->pos, d->normal);
			// Emission rate decays linearly from START at t=0 to END at
			// t=SMOKE_EMIT_TIME. Use the age at this emission to pick
			// the spacing so the interval grows as the decal cools.
			float em_age  = d->next_emit_time - d->spawn_time;
			float k       = em_age / SMOKE_EMIT_TIME;
			if (k > 1.0f) k = 1.0f;
			float rate    = SMOKE_EMIT_RATE_START +
			                (SMOKE_EMIT_RATE_END - SMOKE_EMIT_RATE_START) * k;
			if (rate < 0.1f) rate = 0.1f;
			d->next_emit_time += 1.0f / rate;
		}
	}

	vec3_t wind; Wind_Eval(now, wind);
	for (int i = 0; i < SMOKE_PARTICLES_MAX; ++i) {
		smoke_t *p = &smoke[i];
		if (!p->alive) continue;
		float age = now - p->spawn_time;
		if (age >= SMOKE_PARTICLE_LIFE) { p->alive = false; continue; }
		// Wind + gravity-less rise (smoke).
		p->pos[0] += (p->vel[0] + wind[0]) * dt;
		p->pos[1] += (p->vel[1] + wind[1]) * dt;
		p->pos[2] += (p->vel[2] + wind[2]) * dt;
		// Drag so early energy decays.
		p->vel[0] *= 0.94f; p->vel[1] *= 0.94f; p->vel[2] *= 0.98f;
	}

	// ---- GL state setup (shared by glow + smoke) ----
	GLboolean blend_was      = glIsEnabled(GL_BLEND);
	GLboolean cull_was       = glIsEnabled(GL_CULL_FACE);
	GLboolean depth_test_was = glIsEnabled(GL_DEPTH_TEST);
	GLboolean dmask_was; glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask_was);
	GLint src_rgb, dst_rgb, src_a, dst_a;
	glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);

	if (!blend_was) glEnable(GL_BLEND);
	if (!depth_test_was) glEnable(GL_DEPTH_TEST);   // opaque walls/monsters must occlude smoke
	if (cull_was) glDisable(GL_CULL_FACE);
	glDepthMask(GL_FALSE);

	glBindVertexArray(quad_vao);

	// ---- glow pass (additive) ----
	GLShader_Use(&R_GlowShader);
	glUniformMatrix4fv(R_GlowShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform1f(R_GlowShader_u_radius, GLOW_RADIUS);
	glBlendFunc(GL_ONE, GL_ONE);

	for (int i = 0; i < DECAL_RING_SIZE; ++i) {
		decal_t *d = &decals[i];
		if (d->method == 0) continue;
		float age = now - d->spawn_time;
		if (age < 0.0f || age >= DECAL_LIFETIME) continue;
		float age01 = age / DECAL_LIFETIME;

		// Color ramp: yellow-white -> orange -> deep red -> black.
		vec3_t hot  = { 1.00f, 0.85f, 0.35f };
		vec3_t warm = { 1.00f, 0.35f, 0.05f };
		vec3_t cool = { 0.45f, 0.05f, 0.02f };
		vec3_t dark = { 0.08f, 0.01f, 0.00f };
		vec3_t c;
		if (age01 < 0.25f) {
			float u = age01 / 0.25f;
			c[0] = hot[0]+(warm[0]-hot[0])*u;
			c[1] = hot[1]+(warm[1]-hot[1])*u;
			c[2] = hot[2]+(warm[2]-hot[2])*u;
		} else if (age01 < 0.65f) {
			float u = (age01 - 0.25f) / 0.40f;
			c[0] = warm[0]+(cool[0]-warm[0])*u;
			c[1] = warm[1]+(cool[1]-warm[1])*u;
			c[2] = warm[2]+(cool[2]-warm[2])*u;
		} else {
			float u = (age01 - 0.65f) / 0.35f;
			c[0] = cool[0]+(dark[0]-cool[0])*u;
			c[1] = cool[1]+(dark[1]-cool[1])*u;
			c[2] = cool[2]+(dark[2]-cool[2])*u;
		}
		// Overall brightness envelope: bright at birth, fades with age^2.
		float life = 1.0f - age01;
		float alpha = life * life;

		vec3_t right, up;
		Decal_BuildAxes(d->normal, right, up);
		vec3_t center = {
			d->pos[0] + d->normal[0] * WALL_OFFSET,
			d->pos[1] + d->normal[1] * WALL_OFFSET,
			d->pos[2] + d->normal[2] * WALL_OFFSET,
		};
		glUniform3f(R_GlowShader_u_center, center[0], center[1], center[2]);
		glUniform3f(R_GlowShader_u_right,  right[0], right[1], right[2]);
		glUniform3f(R_GlowShader_u_up,     up[0],    up[1],    up[2]);
		glUniform3f(R_GlowShader_u_color,  c[0], c[1], c[2]);
		glUniform1f(R_GlowShader_u_alpha,  alpha);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	// ---- smoke pass (alpha-over) ----
	GLShader_Use(&R_SmokeShader);
	glUniformMatrix4fv(R_SmokeShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform3f(R_SmokeShader_u_cam_right, vright[0], vright[1], vright[2]);
	glUniform3f(R_SmokeShader_u_cam_up,    vup[0],    vup[1],    vup[2]);
	glUniform3f(R_SmokeShader_u_eye, r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2]);
	glUniform1f(R_SmokeShader_u_fade_near, SMOKE_FADE_NEAR);
	glUniform1f(R_SmokeShader_u_fade_far,  SMOKE_FADE_FAR);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	for (int i = 0; i < SMOKE_PARTICLES_MAX; ++i) {
		smoke_t *p = &smoke[i];
		if (!p->alive) continue;
		float age = now - p->spawn_time;
		if (age >= SMOKE_PARTICLE_LIFE) continue;
		float age01 = age / SMOKE_PARTICLE_LIFE;
		// Growth curve: ease-in cubic. Near the origin the puff stays
		// small and only fills out once it has drifted, so the cluster
		// at the impact site doesn't read as a dense blob.
		float growth = age01 * age01 * age01;
		float r = SMOKE_SIZE_START + (SMOKE_SIZE_END - SMOKE_SIZE_START) * growth;

		glUniform3f(R_SmokeShader_u_center, p->pos[0], p->pos[1], p->pos[2]);
		glUniform1f(R_SmokeShader_u_radius, r);
		glUniform1f(R_SmokeShader_u_age01, age01);
		glUniform1f(R_SmokeShader_u_seed,  p->seed);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	glDepthMask(dmask_was);
	if (!depth_test_was) glDisable(GL_DEPTH_TEST);
	if (cull_was) glEnable(GL_CULL_FACE);
	glBlendFunc(src_rgb, dst_rgb);
	if (!blend_was) glDisable(GL_BLEND);
	glBindVertexArray(0);
	glUseProgram(0);
}

void Gunshot_Init (void)
{
	Cvar_RegisterVariable(&r_gunshot_debug);
}
