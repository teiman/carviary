// gl_blood.cpp -- blood splatter replacement for svc_particle + TE_BLOOD.
//
// When an alias entity is hit, the server sends a particle effect with
// palette color index 73 (dark red) or 225 (bright red / meaty). The stock
// particle code paints tiny specks that are nearly invisible. We hijack
// the call and instead spawn a handful of ballistic droplet "jets" of
// blood that fly outward from the hit, fall with gravity, and lightly
// bounce on first contact with the world.

#include "quakedef.h"
#include "gl_render.h"
#include <stdint.h>

extern vec3_t vright, vup;
extern float TraceLine (vec3_t start, vec3_t end, vec3_t impact, vec3_t normal);

// ---------------------------------------------------------------------------
// Tunables.
// ---------------------------------------------------------------------------
#define BLOOD_MAX_DROPLETS      1024
// Jet counts -- now ~1/3 of the previous 2..4. Each burst rolls its own
// size: mostly 1 jet, sometimes 0 extras = almost invisible splash; a few
// bursts still go large. Varies the "feel" so bursts don't look identical.
#define BLOOD_JETS_MIN             1
#define BLOOD_JETS_MAX             2

#define BLOOD_PER_JET_MIN          4
#define BLOOD_PER_JET_MAX         12
#define BLOOD_JET_SPEED_MIN      40.0f
#define BLOOD_JET_SPEED_MAX     180.0f    // wider range so some jets reach far
#define BLOOD_JET_CONE_MIN        0.15f   // tight vs. scattered per-burst
#define BLOOD_JET_CONE_MAX        0.55f
#define BLOOD_LIFE_MIN            0.8f
#define BLOOD_LIFE_MAX            2.2f
#define BLOOD_GRAVITY           280.0f    // u/s^2 downward
#define BLOOD_BOUNCE_K            0.25f   // fraction of speed kept after bounce
#define BLOOD_POINT_PX            6.0f    // draw size peak

static cvar_t r_blood_debug = {"r_blood_debug", "0"};

// ---------------------------------------------------------------------------
typedef struct {
	vec3_t pos;
	vec3_t vel;
	float  spawn_time;
	float  life;
	float  seed;
	qboolean bounced;   // already used its one bounce
	qboolean alive;
} droplet_t;

static droplet_t g_drops[BLOOD_MAX_DROPLETS];
static int       g_drop_head = 0;

// Deterministic-ish RNG.
static unsigned int g_rng = 0x7A5C0DE1u;
static float frand01 (void)
{
	unsigned int x = g_rng;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	g_rng = x;
	return (float)(x & 0xFFFFFF) / (float)0x1000000;
}
static float frand_range (float lo, float hi) { return lo + (hi - lo) * frand01(); }

// ---------------------------------------------------------------------------
// Shader. GL_POINTS with a radial falloff; deep red tint, darkens with age.
// ---------------------------------------------------------------------------
static GLShader R_BloodShader;
static GLint    R_BloodShader_u_mvp      = -1;
static GLint    R_BloodShader_u_point_sz = -1;
static qboolean blood_shader_ok = false;

static GLuint blood_vao = 0, blood_vbo = 0;
typedef struct { float x, y, z; float age01; } blood_vtx_t;

static qboolean R_EnsureBloodShader (void)
{
	if (blood_shader_ok) return true;
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
		"    // Droplets visibly grow early (blood splash), then shrink.\n"
		"    float s = 0.7 + 0.6 * (1.0 - abs(a_age01 - 0.2) * 3.0);\n"
		"    gl_PointSize = max(1.0, u_point_sz * clamp(s, 0.4, 1.4));\n"
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
		"    // Desaturated, muted red-brown. Not a saturated 'paint' red;\n"
		"    // this reads as dried/matte blood. Small green/blue channel\n"
		"    // keeps it from looking like a pure red signal.\n"
		"    vec3 bright = vec3(0.28, 0.04, 0.04);\n"
		"    vec3 deep   = vec3(0.12, 0.02, 0.02);\n"
		"    vec3 col = mix(bright, deep, clamp(v_age01 * 1.3, 0.0, 1.0));\n"
		"    float alpha = edge * (1.0 - smoothstep(0.8, 1.0, v_age01));\n"
		"    frag_color  = vec4(col, alpha);\n"
		"    // No bloom for blood.\n"
		"    frag_fbmask = vec4(0.0);\n"
		"}\n";
	char err[512];
	if (!GLShader_Build(&R_BloodShader, vs, fs, err, sizeof(err))) {
		Con_Printf("blood shader failed: %s\n", err);
		return false;
	}
	R_BloodShader_u_mvp      = GLShader_Uniform(&R_BloodShader, "u_mvp");
	R_BloodShader_u_point_sz = GLShader_Uniform(&R_BloodShader, "u_point_sz");
	blood_shader_ok = true;
	return true;
}

static void Blood_EnsureVAO (void)
{
	if (blood_vao) return;
	glGenVertexArrays(1, &blood_vao);
	glGenBuffers(1, &blood_vbo);
	glBindVertexArray(blood_vao);
	glBindBuffer(GL_ARRAY_BUFFER, blood_vbo);
	glBufferData(GL_ARRAY_BUFFER, BLOOD_MAX_DROPLETS * sizeof(blood_vtx_t), NULL, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(blood_vtx_t), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(blood_vtx_t), (void*)offsetof(blood_vtx_t, age01));
	glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Returns true when the given palette color index should be treated as
// blood. Quake conventions: 73 is the standard blood color; some legacy
// code (lightning + flesh) uses indices in the 70s range. 225 is sometimes
// sent by gib/chunk effects.
// ---------------------------------------------------------------------------
static qboolean Blood_IsBloodColor (int color)
{
	return (color >= 73 && color <= 79) || (color == 225);
}

// Spawn a single droplet starting at `origin` with velocity `vel`.
static void Blood_SpawnDroplet (const vec3_t origin, const vec3_t vel)
{
	droplet_t *d = &g_drops[g_drop_head];
	g_drop_head = (g_drop_head + 1) % BLOOD_MAX_DROPLETS;
	VectorCopy(origin, d->pos);
	VectorCopy(vel,    d->vel);
	d->spawn_time = (float)realtime;
	d->life    = frand_range(BLOOD_LIFE_MIN, BLOOD_LIFE_MAX);
	d->seed    = frand01();
	d->bounced = false;
	d->alive   = true;
}

// Pick a random unit vector in the cone around `axis` with half-aperture
// proportional to BLOOD_JET_CONE (0 = axis, 1 = hemisphere).
static void Blood_RandomDirInCone (const vec3_t axis, float spread, vec3_t out)
{
	// Random point on a disk in the plane perpendicular to `axis`, then
	// project onto a sphere by lifting with some axis component.
	vec3_t up_ref = {0, 0, 1};
	if (fabsf(axis[2]) > 0.9f) { up_ref[0] = 1; up_ref[1] = 0; up_ref[2] = 0; }
	vec3_t u, v;
	CrossProduct(up_ref, axis, u);
	float lu = sqrtf(DotProduct(u, u));
	if (lu < 1e-4f) {
		u[0] = 1; u[1] = 0; u[2] = 0;
	} else {
		u[0] /= lu; u[1] /= lu; u[2] /= lu;
	}
	CrossProduct(axis, u, v);
	float theta = frand_range(0.0f, 6.2831853f);
	float r     = frand01() * spread;
	float sx = cosf(theta) * r;
	float sy = sinf(theta) * r;
	float sz = sqrtf(fmaxf(0.0f, 1.0f - r*r));
	out[0] = axis[0] * sz + u[0] * sx + v[0] * sy;
	out[1] = axis[1] * sz + u[1] * sx + v[1] * sy;
	out[2] = axis[2] * sz + u[2] * sx + v[2] * sy;
	float l = sqrtf(DotProduct(out, out));
	if (l > 0.0f) { out[0]/=l; out[1]/=l; out[2]/=l; }
}

// Main entry: spawn a blood burst at `origin` biased toward `direction`.
// Each burst rolls a set of random parameters (jet count, cone spread,
// per-jet speed and drop count) so consecutive bursts look different.
static void Blood_SpawnBurst (const vec3_t origin, const vec3_t direction, int count)
{
	vec3_t axis;
	VectorCopy(direction, axis);
	float len = sqrtf(DotProduct(axis, axis));
	if (len < 0.05f) {
		axis[0] = frand_range(-0.3f, 0.3f);
		axis[1] = frand_range(-0.3f, 0.3f);
		axis[2] = 1.0f;
		len = sqrtf(DotProduct(axis, axis));
	}
	axis[0] /= len; axis[1] /= len; axis[2] /= len;

	// Per-burst size lottery: most bursts are small (1 jet), a minority
	// are larger. Squaring the roll biases toward the low end.
	float size_roll = frand01();
	size_roll = size_roll * size_roll;   // bias toward 0
	int n_jets = BLOOD_JETS_MIN +
	             (int)(size_roll * (BLOOD_JETS_MAX - BLOOD_JETS_MIN + 1));
	if (n_jets > BLOOD_JETS_MAX) n_jets = BLOOD_JETS_MAX;

	// Per-burst cone spread: some bursts tight, some scattered.
	float burst_cone = frand_range(BLOOD_JET_CONE_MIN, BLOOD_JET_CONE_MAX);
	// Per-burst speed scale: some bursts weak (close blood), some far-flung.
	float burst_speed_k = frand_range(0.6f, 1.4f);

	int total_drops = 0;
	for (int j = 0; j < n_jets; ++j) {
		vec3_t jet_dir;
		Blood_RandomDirInCone(axis, burst_cone, jet_dir);
		// Per-jet speed adds more variance on top of the burst's scale.
		float jet_spd = frand_range(BLOOD_JET_SPEED_MIN, BLOOD_JET_SPEED_MAX)
		              * burst_speed_k;
		// Per-jet drop count: lower bound follows size_roll so tiny bursts
		// also have fewer drops per jet.
		int drops_lo = BLOOD_PER_JET_MIN;
		int drops_hi = BLOOD_PER_JET_MIN +
		               (int)((BLOOD_PER_JET_MAX - BLOOD_PER_JET_MIN) *
		                     (0.3f + 0.7f * size_roll));
		int drops = drops_lo + (int)(frand01() * (drops_hi - drops_lo + 1));
		for (int k = 0; k < drops; ++k) {
			vec3_t dir;
			Blood_RandomDirInCone(jet_dir, 0.22f, dir);
			float spd = jet_spd * frand_range(0.55f, 1.25f);
			vec3_t vel = { dir[0] * spd, dir[1] * spd, dir[2] * spd };
			Blood_SpawnDroplet(origin, vel);
			total_drops++;
		}
	}

	if (r_blood_debug.value) {
		Con_Printf("blood: origin(%.1f %.1f %.1f) dir(%.2f %.2f %.2f) count=%d -> %d jets (cone=%.2f spd_k=%.2f) %d drops\n",
		           origin[0], origin[1], origin[2],
		           axis[0], axis[1], axis[2], count,
		           n_jets, burst_cone, burst_speed_k, total_drops);
	}
}

// Public: called from R_RunParticleEffect BEFORE the stock fallback runs.
// Returns true if we handled it (caller should skip the stock effect).
qboolean Blood_InterceptParticleEffect (vec3_t origin, vec3_t direction, int color, int count)
{
	if (!Blood_IsBloodColor(color)) return false;
	Blood_SpawnBurst(origin, direction, count);
	return true;
}

// ---------------------------------------------------------------------------
void Blood_Draw (void)
{
	if (!R_EnsureBloodShader()) return;
	Blood_EnsureVAO();

	float now = (float)realtime;
	static float prev = 0.0f;
	float dt = now - prev;
	if (dt < 0.0f || dt > 0.2f) dt = 0.016f;
	prev = now;

	// Step droplets.
	for (int i = 0; i < BLOOD_MAX_DROPLETS; ++i) {
		droplet_t *d = &g_drops[i];
		if (!d->alive) continue;
		float age = now - d->spawn_time;
		if (age >= d->life) { d->alive = false; continue; }

		vec3_t prev_pos;
		VectorCopy(d->pos, prev_pos);
		d->pos[0] += d->vel[0] * dt;
		d->pos[1] += d->vel[1] * dt;
		d->pos[2] += d->vel[2] * dt;
		d->vel[2] -= BLOOD_GRAVITY * dt;
		d->vel[0] *= 0.99f;
		d->vel[1] *= 0.99f;

		// Collision: trace from previous to current position. On first
		// contact, bounce with low elasticity, then stop bouncing (blood
		// sticks after the first impact).
		if (!d->bounced) {
			vec3_t impact, normal;
			float frac = TraceLine(prev_pos, d->pos, impact, normal);
			if (frac < 1.0f) {
				// Reflect velocity around the surface normal with very
				// low restitution; kill most of the horizontal speed too.
				float vdotn = DotProduct(d->vel, normal);
				d->vel[0] -= 2.0f * vdotn * normal[0];
				d->vel[1] -= 2.0f * vdotn * normal[1];
				d->vel[2] -= 2.0f * vdotn * normal[2];
				d->vel[0] *= BLOOD_BOUNCE_K;
				d->vel[1] *= BLOOD_BOUNCE_K;
				d->vel[2] *= BLOOD_BOUNCE_K;
				VectorCopy(impact, d->pos);
				// Nudge slightly off the surface to avoid being restuck.
				d->pos[0] += normal[0] * 0.5f;
				d->pos[1] += normal[1] * 0.5f;
				d->pos[2] += normal[2] * 0.5f;
				d->bounced = true;
			}
		}
	}

	// Collect live droplets into a single draw.
	static blood_vtx_t verts[BLOOD_MAX_DROPLETS];
	int n = 0;
	for (int i = 0; i < BLOOD_MAX_DROPLETS; ++i) {
		droplet_t *d = &g_drops[i];
		if (!d->alive) continue;
		float age = now - d->spawn_time;
		if (age >= d->life) continue;
		verts[n].x = d->pos[0];
		verts[n].y = d->pos[1];
		verts[n].z = d->pos[2];
		verts[n].age01 = age / d->life;
		n++;
	}
	if (n == 0) return;

	float mvp[16];
	R_CurrentMVP(mvp);

	GLboolean blend_was = glIsEnabled(GL_BLEND);
	GLboolean dt_was    = glIsEnabled(GL_DEPTH_TEST);
	GLboolean dmask_was; glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask_was);
	GLint src_rgb, dst_rgb, src_a, dst_a;
	glGetIntegerv(GL_BLEND_SRC_RGB,   &src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB,   &dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);

	if (!blend_was) glEnable(GL_BLEND);
	if (!dt_was)    glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_PROGRAM_POINT_SIZE);

	GLShader_Use(&R_BloodShader);
	glUniformMatrix4fv(R_BloodShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform1f(R_BloodShader_u_point_sz, BLOOD_POINT_PX);

	glBindVertexArray(blood_vao);
	glBindBuffer(GL_ARRAY_BUFFER, blood_vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, n * sizeof(blood_vtx_t), verts);
	glDrawArrays(GL_POINTS, 0, n);

	glDisable(GL_PROGRAM_POINT_SIZE);
	glDepthMask(dmask_was);
	if (!dt_was)    glDisable(GL_DEPTH_TEST);
	glBlendFunc(src_rgb, dst_rgb);
	if (!blend_was) glDisable(GL_BLEND);
	glBindVertexArray(0);
	glUseProgram(0);
}

void Blood_Init (void)
{
	Cvar_RegisterVariable(&r_blood_debug);
}
