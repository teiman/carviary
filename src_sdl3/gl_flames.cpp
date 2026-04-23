// gl_flames.cpp -- procedural flame billboards over lava surfaces.
//
// For every world surface whose texture name matches "*lava*" we scatter a
// set of view-aligned quads above the surface plane. A fragment shader
// paints each quad as a flickering flame using layered 2D noise and a
// vertical color gradient (red -> orange -> yellow -> transparent smoke).
//
// Pipeline mirrors gl_grass.cpp: one static VBO holds all billboard bases;
// the shader constructs the view-aligned quad in world space using the
// camera right/up axes passed as uniforms.

#include "quakedef.h"
#include "gl_render.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Tuning constants. Tuned to look right at Quake's interior scale (player
// ~56 units tall, lava pits typically 128-512 unit squares).
// ---------------------------------------------------------------------------
#define FLAME_WIDTH_UNITS       10.0f   // billboard half-width; final quad is 2x
#define FLAME_HEIGHT_UNITS      48.0f   // billboard height (taller than wide)
#define FLAME_BASE_OFFSET_UP     1.0f   // lift quads slightly above surface
#define FLAME_POINT_SPACING     22.4f   // avg. world distance between billboards -- ~0.8x density vs 20.0
#define FLAME_MAX_BILLBOARDS   80000
#define FLAME_NORMAL_Z_MIN       0.70f  // only surfaces facing mostly up

// Life cycle (seconds). Each billboard picks a random offset inside this
// period so neighbors aren't synchronized. Each flame is only active
// during the last 20% of the cycle (see flame_envelope in the shader),
// so an individual flame is visible for ~0.2 * FLAME_CYCLE_SECONDS.
#define FLAME_CYCLE_SECONDS    96.0f

// ---------------------------------------------------------------------------
// Ember tuning. Embers are cheap GL_POINTS that rise from the lava and fade
// away. Each ember shares a spawn position with one of the flame billboards
// but has its own fast lifecycle, so they appear to be shooting out of the
// flames. Ember density is tied to flame count; we emit N_EMBERS_PER_FLAME
// ember points per flame during VBO construction.
// ---------------------------------------------------------------------------
#define EMBER_PER_FLAME           1       // GL_POINTS per flame
#define EMBER_CYCLE_SECONDS       7.2f    // short lifecycle -> lots of movement
#define EMBER_RISE_UNITS         52.0f    // world units travelled over one cycle
#define EMBER_SWAY_UNITS          5.0f    // horizontal drift amplitude
#define EMBER_POINT_SIZE_PX       3.5f    // fullscreen point size (peak)

// ---------------------------------------------------------------------------
// Vertex layout. One entry per billboard corner (4 corners = 4 vertices per
// flame, drawn as triangle-strip via indexed fan? Simpler: 6 verts/flame
// packed in the VBO as two triangles, no IBO). All corners share the same
// base position; we distinguish them via a corner code 0..3.
// ---------------------------------------------------------------------------
typedef struct {
	float base_x, base_y, base_z;
	float corner;   // 0 = BL, 1 = BR, 2 = TR, 3 = TL
	float seed;     // per-flame random, decorrelates noise phase and size
	float scale_h;  // per-flame height scale (~0.20..0.85)
	float scale_w;  // per-flame width  scale (~0.60..1.30)
} flame_vtx_t;

typedef struct {
	float base_x, base_y, base_z;
	float seed;
} ember_vtx_t;

typedef struct {
	GLuint vao;
	GLuint vbo;
	int    num_verts;
	// Embers share the same per-texture lifecycle bucket; they have their
	// own VAO/VBO because the vertex layout and draw mode are different
	// (GL_POINTS, no corner attribute).
	GLuint ember_vao;
	GLuint ember_vbo;
	int    ember_count;    // number of points
	int    built;
} flame_tex_data_t;

static flame_tex_data_t *flame_tex_data = NULL;
static int               flame_tex_data_count = 0;

// ---------------------------------------------------------------------------
// Shader.
//
// Vertex shader: expands each base position into a view-aligned quad using
// u_cam_right and u_cam_up (world-space camera basis vectors). Each flame
// gets a small horizontal sway from a per-flame seed so neighbors don't
// synchronize.
//
// Fragment shader: procedural fire.
//  - Vertical gradient dominates the color: base deep red -> middle orange ->
//    top yellow -> fade to transparent smoke color at tip.
//  - Two octaves of cheap 2D value noise, scrolled upward with time and
//    seeded per-flame, sculpt the silhouette. Pixels where (y - noise)
//    exceeds a threshold become transparent -- this gives flickery edges.
//  - Gentle side-taper keeps the quad reading as a flame shape and not a
//    rectangle.
// ---------------------------------------------------------------------------
static GLShader R_FlamesShader;
static GLint    R_FlamesShader_u_mvp       = -1;
static GLint    R_FlamesShader_u_cam_right = -1;
static GLint    R_FlamesShader_u_cam_up    = -1;
static GLint    R_FlamesShader_u_half_w    = -1;
static GLint    R_FlamesShader_u_height    = -1;
static GLint    R_FlamesShader_u_time      = -1;
static GLint    R_FlamesShader_u_cycle     = -1;
static qboolean flames_shader_ok          = false;

// Ember shader: GL_POINTS, per-point base + seed, rises + fades each cycle.
static GLShader R_EmbersShader;
static GLint    R_EmbersShader_u_mvp        = -1;
static GLint    R_EmbersShader_u_time       = -1;
static GLint    R_EmbersShader_u_cycle      = -1;
static GLint    R_EmbersShader_u_rise       = -1;
static GLint    R_EmbersShader_u_sway       = -1;
static GLint    R_EmbersShader_u_point_size = -1;
static qboolean embers_shader_ok           = false;

static qboolean R_EnsureEmbersShader (void)
{
	if (embers_shader_ok) return true;

	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in vec3  a_base;\n"
		"layout(location = 1) in float a_seed;\n"
		"uniform mat4  u_mvp;\n"
		"uniform float u_time;\n"
		"uniform float u_cycle;\n"
		"uniform float u_rise;\n"
		"uniform float u_sway;\n"
		"uniform float u_point_size;\n"
		"out float v_life;\n"      // 1 at spawn, 0 at death
		"out float v_heat;\n"      // shifts color from yellow core to dark red
		"void main() {\n"
		"    // Per-ember phase offset by seed so they don't all bunch up.\n"
		"    float ph = fract(u_time / u_cycle + a_seed);\n"
		"\n"
		"    // Life: 1 at ph=0 spawn, 0 at ph=1 death, with a quick early\n"
		"    // peak then steady fade. Visually: bright spark out of the\n"
		"    // flame, darker ember trailing up.\n"
		"    float life = 1.0 - ph;\n"
		"    v_life = life;\n"
		"\n"
		"    // Color heat shifts downward over life: new sparks glow hot\n"
		"    // yellow, old embers fade to dim red.\n"
		"    v_heat = life;\n"
		"\n"
		"    // \"Fat ember\" variant: 1 in 5 embers (seed < 0.2) rises 30%\n"
		"    // higher and is drawn 30% larger. Adds pop without making\n"
		"    // every ember feel inflated.\n"
		"    float fat     = step(a_seed, 0.2);\n"   // 1.0 when seed < 0.2, else 0
		"    float rise_k  = mix(1.0, 1.3, fat);\n"
		"    float size_k  = mix(1.0, 1.3, fat);\n"
		"\n"
		"    // Vertical travel: linear rise. Horizontal drift: two sines at\n"
		"    // different per-ember phases for a gentle wobble.\n"
		"    float dy = ph * u_rise * rise_k;\n"
		"    float dx = sin(u_time * 2.1 + a_seed * 43.0) * u_sway * ph;\n"
		"    float dz = cos(u_time * 1.7 + a_seed * 17.0) * u_sway * ph;\n"
		"    vec3 pos = a_base + vec3(dx, dz, dy);\n"
		"\n"
		"    vec4 clip = u_mvp * vec4(pos, 1.0);\n"
		"    gl_Position = clip;\n"
		"    // Point size shrinks as the ember ages.\n"
		"    gl_PointSize = max(1.0, u_point_size * size_k * (0.3 + 0.7 * life));\n"
		"}\n";

	const char *fs =
		"#version 330 core\n"
		"in float v_life;\n"
		"in float v_heat;\n"
		"layout(location = 0) out vec4 frag_color;\n"
		"layout(location = 1) out vec4 frag_fbmask;\n"
		"void main() {\n"
		"    vec2 d = gl_PointCoord - vec2(0.5);\n"
		"    float r2 = dot(d, d);\n"
		"    if (r2 > 0.25) discard;\n"
		"    float edge = 1.0 - smoothstep(0.15, 0.25, r2);\n"
		"    vec3 hot  = vec3(1.00, 0.88, 0.35);\n"
		"    vec3 cold = vec3(0.85, 0.20, 0.05);\n"
		"    vec3 col  = mix(cold, hot, v_heat);\n"
		"    float alpha = edge * v_life * v_life;\n"
		"    frag_color  = vec4(col, alpha);\n"
		"    // Emissive: RGB premultiplied, alpha = emitter depth.\n"
		"    frag_fbmask = vec4(col * alpha, gl_FragCoord.z);\n"
		"}\n";

	char err[512];
	if (!GLShader_Build(&R_EmbersShader, vs, fs, err, sizeof(err))) {
		Con_Printf("embers shader failed: %s\n", err);
		return false;
	}
	R_EmbersShader_u_mvp        = GLShader_Uniform(&R_EmbersShader, "u_mvp");
	R_EmbersShader_u_time       = GLShader_Uniform(&R_EmbersShader, "u_time");
	R_EmbersShader_u_cycle      = GLShader_Uniform(&R_EmbersShader, "u_cycle");
	R_EmbersShader_u_rise       = GLShader_Uniform(&R_EmbersShader, "u_rise");
	R_EmbersShader_u_sway       = GLShader_Uniform(&R_EmbersShader, "u_sway");
	R_EmbersShader_u_point_size = GLShader_Uniform(&R_EmbersShader, "u_point_size");
	embers_shader_ok = true;
	return true;
}

static qboolean R_EnsureFlamesShader (void)
{
	if (flames_shader_ok) return true;

	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in vec3  a_base;\n"
		"layout(location = 1) in float a_corner;\n"   // 0..3
		"layout(location = 2) in float a_seed;\n"     // [0,1)
		"layout(location = 3) in float a_scale_h;\n"  // per-flame height factor
		"layout(location = 4) in float a_scale_w;\n"  // per-flame width  factor
		"uniform mat4  u_mvp;\n"
		"uniform vec3  u_cam_right;\n"                // world-space right of camera
		"uniform vec3  u_cam_up;\n"                   // world-space up of camera
		"uniform float u_half_w;\n"
		"uniform float u_height;\n"
		"uniform float u_time;\n"
		"uniform float u_cycle;\n"           // seconds per flame lifecycle
		"out vec2  v_uv;\n"                  // [0..1] quad UV
		"out float v_seed;\n"
		"out float v_life;\n"                // lifecycle envelope (0 dead, 1 peak)
		"out float v_phase;\n"               // raw 0..1 phase within the cycle
		"\n"
		"// Each flame is alive only during the last 20% of its cycle; the\n"
		"// other 80% it is fully dead (life=0, size=0, zero fill cost).\n"
		"// This makes any given flame appear roughly 1/5 as often as a\n"
		"// fully-on-always flame -- neighbors flicker in and out with\n"
		"// phase offsets so the pit never goes completely cold.\n"
		"//\n"
		"// Active window (phase in [0.80, 1.00]):\n"
		"//   0.80 - 0.90  slow build-up (smoothstep)\n"
		"//   0.90 - 0.95  at peak\n"
		"//   0.95 - 1.00  rapid fade (cubic)\n"
		"float flame_envelope(float ph) {\n"
		"    if (ph < 0.80) {\n"
		"        return 0.0;\n"
		"    } else if (ph < 0.90) {\n"
		"        float u = (ph - 0.80) / 0.10;\n"
		"        return u * u * (3.0 - 2.0 * u);\n"
		"    } else if (ph < 0.95) {\n"
		"        return 1.0;\n"
		"    } else {\n"
		"        float u = (ph - 0.95) / 0.05;\n"
		"        float k = 1.0 - u;\n"
		"        return k * k * k;\n"
		"    }\n"
		"}\n"
		"\n"
		"void main() {\n"
		"    int c = int(a_corner);\n"
		"    float sx = (c == 0 || c == 3) ? -1.0 : 1.0;\n"
		"    float sy = (c == 0 || c == 1) ?  0.0 : 1.0;\n"
		"    v_uv  = vec2((sx + 1.0) * 0.5, sy);\n"
		"    v_seed = a_seed;\n"
		"\n"
		"    // Per-flame phase in [0,1). Seed shifts neighbors out of sync.\n"
		"    float ph = fract(u_time / u_cycle + a_seed);\n"
		"    v_phase = ph;\n"
		"    float life = flame_envelope(ph);\n"
		"    v_life = life;\n"
		"\n"
		"    // Size follows the envelope so the flame physically grows and\n"
		"    // shrinks, not just fades. Floor at 0 when fully dead. Width\n"
		"    // and height carry independent per-flame scales so the field\n"
		"    // has visible variety (slim tongues, fat stubs, mid).\n"
		"    float hw = u_half_w * a_scale_w * life;\n"
		"    float hh = u_height * a_scale_h * life;\n"
		"\n"
		"    // Tip waver. Amplitude grows as sy^2 so the base is locked to\n"
		"    // the lava and only the upper portion whips around. Two\n"
		"    // harmonics at per-flame seeds keep neighboring tips out of\n"
		"    // sync and make the motion look organic, not a wave.\n"
		"    float t1 = u_time * 3.2 + a_seed * 31.4;\n"
		"    float t2 = u_time * 5.7 + a_seed * 11.7;\n"
		"    float tip_amt = sy * sy;\n"                // 0 at base, 1 at tip
		"    float sway_x = (sin(t1) + 0.5 * sin(t2)) * 4.0 * tip_amt;\n"
		"    float sway_y = cos(t1 * 0.83) * 2.5 * tip_amt;\n"
		"    vec3 pos = a_base\n"
		"             + u_cam_right * (sx * hw + sway_x)\n"
		"             + u_cam_up    * (sy * hh + sway_y);\n"
		"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
		"}\n";

	const char *fs =
		"#version 330 core\n"
		"in vec2  v_uv;\n"
		"in float v_seed;\n"
		"in float v_life;\n"
		"in float v_phase;\n"
		"uniform float u_time;\n"
		"layout(location = 0) out vec4 frag_color;\n"
		"layout(location = 1) out vec4 frag_fbmask;\n"
		"#define FLAMES_DEBUG_SOLID 0\n"
		"\n"
		"// Cheap 2D value noise in [0,1).\n"
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
		"\n"
		"void main() {\n"
		"#if FLAMES_DEBUG_SOLID\n"
		"    frag_color = vec4(1.0, 0.0, 1.0, 1.0);\n"
		"    return;\n"
		"#endif\n"
		"    // UV: (0,0) bottom-left, (1,1) top-right.\n"
		"    vec2 uv = v_uv;\n"
		"    float y = uv.y;          // 0 bottom of flame, 1 tip\n"
		"    float x = uv.x - 0.5;    // centered -0.5..0.5\n"
		"\n"
		"    // Scrolling noise: drifts upward faster than the flame itself.\n"
		"    // Per-flame seed offsets the noise lookup so neighbors differ.\n"
		"    vec2 seed_off = vec2(v_seed * 37.1, v_seed * 71.3);\n"
		"    vec2 np = vec2(uv.x * 3.0 + seed_off.x,\n"
		"                   uv.y * 2.0 - u_time * 1.6 + seed_off.y);\n"
		"    float n = vnoise(np) * 0.6\n"
		"            + vnoise(np * 2.3 + 11.0) * 0.3\n"
		"            + vnoise(np * 4.7 + 23.0) * 0.1;\n"
		"\n"
		"    // Silhouette: the flame thins as it rises; side-taper keeps\n"
		"    // the shape readable even where noise is high.\n"
		"    float side = 1.0 - abs(x) * (1.2 + y * 1.8);\n"
		"    side = clamp(side, 0.0, 1.0);\n"
		"\n"
		"    // Flickering mask: pixels where (y + edge threshold) exceeds\n"
		"    // noise drop out. Adds vertical streaks and jagged tip.\n"
		"    float alpha_mask = smoothstep(0.05, 0.35, side * (1.2 - y) * (0.55 + n * 0.55));\n"
		"\n"
		"    if (alpha_mask < 0.02) discard;\n"
		"\n"
		"    // Color gradient. The near-root region (y ~ 0) is DARK red/brown\n"
		"    // because real flames are cooler right at the base; the middle\n"
		"    // of the flame is hot (yellow/orange), and the tip dissolves to\n"
		"    // dark smoke.\n"
		"    vec3 col_root   = vec3(0.25, 0.05, 0.02);  // dark ember at base\n"
		"    vec3 col_low    = vec3(1.0, 0.25, 0.05);   // deep red-orange\n"
		"    vec3 col_mid    = vec3(1.0, 0.70, 0.15);   // bright orange (hot mid)\n"
		"    vec3 col_hot    = vec3(1.0, 0.95, 0.55);   // yellow-white core\n"
		"    vec3 col_tip    = vec3(0.30, 0.09, 0.04);  // dark smoke\n"
		"\n"
		"    // Build the gradient in three bands:\n"
		"    //   y 0.00 - 0.18  root -> low\n"
		"    //   y 0.18 - 0.55  low  -> mid\n"
		"    //   y 0.55 - 1.00  mid  -> tip\n"
		"    vec3 color = mix(col_root, col_low, smoothstep(0.0,  0.18, y));\n"
		"    color      = mix(color,    col_mid, smoothstep(0.18, 0.55, y));\n"
		"    color      = mix(color,    col_tip, smoothstep(0.70, 1.0,  y));\n"
		"\n"
		"    // Bright yellow core in the middle of the flame (not at base).\n"
		"    float hot = smoothstep(0.18, 0.40, y) * (1.0 - smoothstep(0.55, 0.80, y))\n"
		"              * smoothstep(0.25, 0.8, 1.0 - abs(x)*2.0);\n"
		"    color = mix(color, col_hot, hot * 0.55);\n"
		"\n"
		"    // High-frequency pulse so the whole flame shimmers.\n"
		"    float pulse = 0.85 + 0.15 * sin(u_time * 5.0 + v_seed * 17.0);\n"
		"    color *= pulse;\n"
		"\n"
		"    // Lifecycle modulates alpha AND brightness so fading flames\n"
		"    // visibly dim instead of just shrinking.\n"
		"    color *= (0.4 + 0.6 * v_life);\n"
		"\n"
		"    // Alpha envelope. During the fade portion of the cycle (phase\n"
		"    // > 0.95) we square v_life so transparency drops faster than\n"
		"    // the size -- the flame thins out as it dies, not just shrinks.\n"
		"    float life_alpha = v_phase > 0.95 ? v_life * v_life : v_life;\n"
		"\n"
		"    // Fade alpha toward the tip so it dissolves into the scene.\n"
		"    // Global * 0.80 makes every flame 20%% more transparent overall.\n"
		"    float alpha = alpha_mask\n"
		"                * (1.0 - smoothstep(0.7, 1.0, y))\n"
		"                * life_alpha\n"
		"                * 0.80;\n"
		"    frag_color = vec4(color, alpha);\n"
		"    // Emissive: RGB premultiplied, alpha = emitter depth.\n"
		"    frag_fbmask = vec4(color * alpha, gl_FragCoord.z);\n"
		"}\n";

	char err[512];
	if (!GLShader_Build(&R_FlamesShader, vs, fs, err, sizeof(err))) {
		Con_Printf("flames shader failed: %s\n", err);
		return false;
	}
	R_FlamesShader_u_mvp       = GLShader_Uniform(&R_FlamesShader, "u_mvp");
	R_FlamesShader_u_cam_right = GLShader_Uniform(&R_FlamesShader, "u_cam_right");
	R_FlamesShader_u_cam_up    = GLShader_Uniform(&R_FlamesShader, "u_cam_up");
	R_FlamesShader_u_half_w    = GLShader_Uniform(&R_FlamesShader, "u_half_w");
	R_FlamesShader_u_height    = GLShader_Uniform(&R_FlamesShader, "u_height");
	R_FlamesShader_u_time      = GLShader_Uniform(&R_FlamesShader, "u_time");
	R_FlamesShader_u_cycle     = GLShader_Uniform(&R_FlamesShader, "u_cycle");
	flames_shader_ok = true;
	return true;
}

// ---------------------------------------------------------------------------
// Deterministic hashes (same style as gl_grass.cpp).
// ---------------------------------------------------------------------------
static float f_hash01 (unsigned int x)
{
	x = (x ^ 61) ^ (x >> 16);
	x = x + (x << 3);
	x = x ^ (x >> 4);
	x = x * 0x27d4eb2du;
	x = x ^ (x >> 15);
	return (float)(x & 0xFFFFFF) / (float)0x1000000;
}

// ---------------------------------------------------------------------------
// Is this texture a lava variant? Quake convention: liquid textures start
// with '*' and lava textures contain "lava" in the name (e.g. "*lava1").
// ---------------------------------------------------------------------------
static qboolean Flames_TextureIsLava (texture_t *t)
{
	if (!t) return false;
	if (t->name[0] != '*' && t->name[0] != '!') return false;
	const char *n = t->name;
	for (; *n; ++n) {
		if ((n[0] == 'l' || n[0] == 'L') &&
		    (n[1] == 'a' || n[1] == 'A') &&
		    (n[2] == 'v' || n[2] == 'V') &&
		    (n[3] == 'a' || n[3] == 'A')) return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Scatter N billboard base points over a single surface polygon. Accepts
// only surfaces whose plane normal points mostly up, so we don't spawn
// flames on vertical lava walls.
// ---------------------------------------------------------------------------
// Area-proportional Poisson-like scatter over one polygon of a surface.
// `carry_area` is a running fractional-area accumulator: instead of
// rounding `n = (tri_area / spacing^2) + 0.5`, we add it to a carry and
// emit when the carry crosses 1. That makes the density uniform across
// many small sub-polys, no matter how the warp subdivision chops them.
static int Flames_ScatterPoly (glpoly_t *p, msurface_t *s, unsigned int *hseed_io,
                               float *carry_area, float inv_spacing2,
                               flame_vtx_t *out, int out_capacity, int emitted)
{
	if (!p || p->numverts < 3) return emitted;
	unsigned int hseed = *hseed_io;

	float *v0 = p->verts[0];
	for (int i = 1; i < p->numverts - 1; ++i) {
		float *va = p->verts[i];
		float *vb = p->verts[i + 1];
		float ax = va[0] - v0[0], ay = va[1] - v0[1], az = va[2] - v0[2];
		float bx = vb[0] - v0[0], by = vb[1] - v0[1], bz = vb[2] - v0[2];
		float cx = ay*bz - az*by;
		float cy = az*bx - ax*bz;
		float cz = ax*by - ay*bx;
		float tri_area = 0.5f * sqrtf(cx*cx + cy*cy + cz*cz);

		// Number of flames to place in this triangle, using a carried
		// remainder so we never silently drop tiny fragments.
		*carry_area += tri_area * inv_spacing2;
		int n_tri = (int)(*carry_area);
		*carry_area -= (float)n_tri;

		for (int k = 0; k < n_tri; ++k) {
			if (emitted * 4 + 4 > out_capacity) {
				*hseed_io = hseed;
				return emitted;
			}
			float u  = f_hash01(hseed++);
			float vv = f_hash01(hseed++);
			if (u + vv > 1.0f) { u = 1.0f - u; vv = 1.0f - vv; }
			float pos[3] = {
				v0[0] + ax * u + bx * vv,
				v0[1] + ay * u + by * vv,
				v0[2] + az * u + bz * vv + FLAME_BASE_OFFSET_UP,
			};
			float seed    = f_hash01(hseed++);
			// Independent width and height so the flame zoo includes tall
			// slim tongues, short fat stubs, and everything in between.
			float scale_h = 0.20f + 0.65f * f_hash01(hseed++); // 0.20..0.85
			float scale_w = 0.60f + 0.70f * f_hash01(hseed++); // 0.60..1.30

			flame_vtx_t *o = &out[emitted * 4];
			#define FLAME_VTX(IDX, CORNER) do { \
				o[IDX].base_x = pos[0]; o[IDX].base_y = pos[1]; o[IDX].base_z = pos[2]; \
				o[IDX].corner = (float)(CORNER); \
				o[IDX].seed = seed; \
				o[IDX].scale_h = scale_h; \
				o[IDX].scale_w = scale_w; \
			} while (0)
			FLAME_VTX(0, 0); // BL
			FLAME_VTX(1, 1); // BR
			FLAME_VTX(2, 2); // TR
			FLAME_VTX(3, 3); // TL
			#undef FLAME_VTX
			emitted++;
		}
	}
	*hseed_io = hseed;
	return emitted;
}

static int Flames_ScatterOverSurface (msurface_t *s, flame_vtx_t *out, int out_capacity)
{
	if (!s->polys) return 0;
	// Reject surfaces not facing up.
	float nz = s->plane->normal[2];
	if (s->flags & SURF_PLANEBACK) nz = -nz;
	if (nz < FLAME_NORMAL_Z_MIN) return 0;

	// Walk EVERY poly in the list. SURF_DRAWTURB surfaces are subdivided
	// by GL_SubdivideSurface into many small polys chained through
	// poly->next, and we need to cover all of them -- otherwise only one
	// fragment of each big lava pool receives flames.
	unsigned int hseed = ((unsigned int)((uintptr_t)s) * 2654435761u) ^ 0xBEEF1234u;
	float carry = f_hash01(hseed++);   // random phase so boundaries don't bias to 0
	float inv_spacing2 = 1.0f / (FLAME_POINT_SPACING * FLAME_POINT_SPACING);

	int emitted = 0;
	for (glpoly_t *p = s->polys; p; p = p->next) {
		emitted = Flames_ScatterPoly(p, s, &hseed, &carry, inv_spacing2,
		                              out, out_capacity, emitted);
		if (emitted * 4 + 4 > out_capacity) break;
	}
	return emitted;
}

// ---------------------------------------------------------------------------
// Build the billboard VBO for one texture. One quad is 4 verts stored
// sequentially (BL, BR, TR, TL); we render with glDrawArrays(TRIANGLES)
// using an IBO? Simpler: expand to 6 verts directly in the VBO.
// Actually simpler still: emit 4 verts per quad, draw as GL_TRIANGLES with
// an IBO? We keep it very simple: in the scatter we emit 4 verts per quad,
// then repack to 6 (BL-BR-TR, BL-TR-TL) when building the VBO.
// ---------------------------------------------------------------------------
static void Flames_BuildBillboards (int tex_index)
{
	if (tex_index < 0 || tex_index >= flame_tex_data_count) return;
	flame_tex_data_t *td = &flame_tex_data[tex_index];
	if (td->built) return;
	td->built = 1;

	if (!cl.worldmodel) return;
	texture_t *target = cl.worldmodel->textures[tex_index];
	if (!target) return;

	// Scratch buffers sized for worst case. 4 corners per quad during
	// scatter, then we repack to 6 verts per quad for the VBO.
	static flame_vtx_t scratch[FLAME_MAX_BILLBOARDS * 4];
	int corners = 0;

	for (int i = 0; i < cl.worldmodel->numsurfaces; ++i) {
		msurface_t *s = &cl.worldmodel->surfaces[i];
		if (!s->texinfo || s->texinfo->texture != target) continue;
		if (!(s->flags & SURF_DRAWTURB)) continue;
		int remaining = FLAME_MAX_BILLBOARDS * 4 - corners;
		if (remaining < 4) break;
		int added = Flames_ScatterOverSurface(s, scratch + corners, remaining);
		corners += added * 4;
	}
	if (corners == 0) return;

	int num_quads = corners / 4;
	int num_verts = num_quads * 6;

	// Repack 4-corner quads to 6-vert triangles.
	static flame_vtx_t repacked[FLAME_MAX_BILLBOARDS * 6];
	for (int q = 0; q < num_quads; ++q) {
		flame_vtx_t *src = &scratch[q * 4];
		flame_vtx_t *dst = &repacked[q * 6];
		dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];   // BL BR TR
		dst[3] = src[0]; dst[4] = src[2]; dst[5] = src[3];   // BL TR TL
	}

	glGenVertexArrays(1, &td->vao);
	glGenBuffers(1, &td->vbo);
	glBindVertexArray(td->vao);
	glBindBuffer(GL_ARRAY_BUFFER, td->vbo);
	glBufferData(GL_ARRAY_BUFFER, num_verts * sizeof(flame_vtx_t), repacked, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(flame_vtx_t), (void*)offsetof(flame_vtx_t, base_x));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(flame_vtx_t), (void*)offsetof(flame_vtx_t, corner));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(flame_vtx_t), (void*)offsetof(flame_vtx_t, seed));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(flame_vtx_t), (void*)offsetof(flame_vtx_t, scale_h));
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(flame_vtx_t), (void*)offsetof(flame_vtx_t, scale_w));
	glBindVertexArray(0);

	td->num_verts = num_verts;

	// Ember VBO: emit EMBER_PER_FLAME points per flame using the same base
	// positions. Their seeds are independent of the flame seed so their
	// lifecycle doesn't align with the flame envelope.
	int ember_count = num_quads * EMBER_PER_FLAME;
	if (ember_count > 0) {
		static ember_vtx_t ember_scratch[FLAME_MAX_BILLBOARDS * EMBER_PER_FLAME];
		unsigned int hseed = ((unsigned int)((uintptr_t)target) * 0x9e3779b1u) ^ 0xE41B2077u;
		int ei = 0;
		for (int q = 0; q < num_quads; ++q) {
			flame_vtx_t *src = &scratch[q * 4];
			for (int e = 0; e < EMBER_PER_FLAME; ++e) {
				ember_scratch[ei].base_x = src->base_x;
				ember_scratch[ei].base_y = src->base_y;
				ember_scratch[ei].base_z = src->base_z;
				ember_scratch[ei].seed   = f_hash01(hseed++);
				ei++;
			}
		}
		glGenVertexArrays(1, &td->ember_vao);
		glGenBuffers(1, &td->ember_vbo);
		glBindVertexArray(td->ember_vao);
		glBindBuffer(GL_ARRAY_BUFFER, td->ember_vbo);
		glBufferData(GL_ARRAY_BUFFER, ember_count * sizeof(ember_vtx_t), ember_scratch, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(ember_vtx_t), (void*)offsetof(ember_vtx_t, base_x));
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(ember_vtx_t), (void*)offsetof(ember_vtx_t, seed));
		glBindVertexArray(0);
		td->ember_count = ember_count;
	}

	Con_DPrintf("flames: built %d billboards + %d embers for '%s'\n",
	            num_quads, ember_count, target->name);
}

static void Flames_EnsureTexData (void)
{
	if (!cl.worldmodel) return;
	static model_t *flames_last_wm = NULL;
	if (flame_tex_data
	    && flames_last_wm == cl.worldmodel
	    && flame_tex_data_count == cl.worldmodel->numtextures)
		return;

	if (flame_tex_data) {
		for (int i = 0; i < flame_tex_data_count; ++i) {
			if (flame_tex_data[i].vbo)       glDeleteBuffers(1, &flame_tex_data[i].vbo);
			if (flame_tex_data[i].vao)       glDeleteVertexArrays(1, &flame_tex_data[i].vao);
			if (flame_tex_data[i].ember_vbo) glDeleteBuffers(1, &flame_tex_data[i].ember_vbo);
			if (flame_tex_data[i].ember_vao) glDeleteVertexArrays(1, &flame_tex_data[i].ember_vao);
		}
		free(flame_tex_data);
	}
	flame_tex_data_count = cl.worldmodel->numtextures;
	flame_tex_data = (flame_tex_data_t *)calloc(flame_tex_data_count, sizeof(flame_tex_data_t));
	flames_last_wm = cl.worldmodel;
}

// ---------------------------------------------------------------------------
// Called once per frame from R_DrawWorld, after R_EmitTextureChains. Draws
// every lava surface's flame billboards in one pass per lava texture.
// ---------------------------------------------------------------------------
extern float r_world_matrix[16]; // modelview of the world (gl_rsurf.cpp)
extern vec3_t vpn, vright, vup;  // camera basis in world space (view.c)

void Flames_Draw (void)
{
	if (!cl.worldmodel) return;
	if (!R_EnsureFlamesShader()) return;

	// Cheap early-out: any lava texture in this map?
	int any = 0;
	for (int i = 0; i < cl.worldmodel->numtextures; ++i) {
		if (Flames_TextureIsLava(cl.worldmodel->textures[i])) { any = 1; break; }
	}
	if (!any) return;

	Flames_EnsureTexData();

	float mvp[16];
	R_CurrentMVP(mvp);

	GLShader_Use(&R_FlamesShader);
	glUniformMatrix4fv(R_FlamesShader_u_mvp, 1, GL_FALSE, mvp);
	// Camera right = vright; camera up = -vup in Quake's flipped convention,
	// but the billboard is constructed in world space so we just need two
	// orthonormal axes that span the screen plane. vright and vup work.
	glUniform3f(R_FlamesShader_u_cam_right, vright[0], vright[1], vright[2]);
	glUniform3f(R_FlamesShader_u_cam_up,    vup[0],    vup[1],    vup[2]);
	glUniform1f(R_FlamesShader_u_half_w,    FLAME_WIDTH_UNITS);
	glUniform1f(R_FlamesShader_u_height,    FLAME_HEIGHT_UNITS);
	glUniform1f(R_FlamesShader_u_time,      (float)realtime);
	glUniform1f(R_FlamesShader_u_cycle,     FLAME_CYCLE_SECONDS);

	// Additive blend for emissive-looking fire. Keep depth test so walls
	// occlude, but disable depth write so overlapping flames don't Z-fight.
	// Cull face must be off: our billboards are single-sided quads and the
	// world cull winding won't match them.
	GLboolean blend_was = glIsEnabled(GL_BLEND);
	GLint src_save, dst_save;
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_save);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_save);
	GLboolean depth_mask_was;
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask_was);
	GLboolean cull_was = glIsEnabled(GL_CULL_FACE);

	if (!blend_was) glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glDepthMask(GL_FALSE);
	if (cull_was) glDisable(GL_CULL_FACE);

	for (int i = 0; i < cl.worldmodel->numtextures; ++i) {
		texture_t *t = cl.worldmodel->textures[i];
		if (!Flames_TextureIsLava(t)) continue;
		Flames_BuildBillboards(i);
		flame_tex_data_t *td = &flame_tex_data[i];
		if (td->num_verts == 0) continue;
		glBindVertexArray(td->vao);
		glDrawArrays(GL_TRIANGLES, 0, td->num_verts);
	}

	// Ember pass: GL_POINTS, same additive blend and depth state. We need
	// the vertex shader to be allowed to set gl_PointSize; enable it once.
	if (R_EnsureEmbersShader()) {
		glEnable(GL_PROGRAM_POINT_SIZE);
		GLShader_Use(&R_EmbersShader);
		glUniformMatrix4fv(R_EmbersShader_u_mvp, 1, GL_FALSE, mvp);
		glUniform1f(R_EmbersShader_u_time,       (float)realtime);
		glUniform1f(R_EmbersShader_u_cycle,      EMBER_CYCLE_SECONDS);
		glUniform1f(R_EmbersShader_u_rise,       EMBER_RISE_UNITS);
		glUniform1f(R_EmbersShader_u_sway,       EMBER_SWAY_UNITS);
		glUniform1f(R_EmbersShader_u_point_size, EMBER_POINT_SIZE_PX);
		for (int i = 0; i < cl.worldmodel->numtextures; ++i) {
			texture_t *t = cl.worldmodel->textures[i];
			if (!Flames_TextureIsLava(t)) continue;
			flame_tex_data_t *td = &flame_tex_data[i];
			if (td->ember_count == 0) continue;
			glBindVertexArray(td->ember_vao);
			glDrawArrays(GL_POINTS, 0, td->ember_count);
		}
		glDisable(GL_PROGRAM_POINT_SIZE);
	}

	// Restore state.
	glDepthMask(depth_mask_was);
	if (cull_was) glEnable(GL_CULL_FACE);
	glBlendFunc((GLenum)src_save, (GLenum)dst_save);
	if (!blend_was) glDisable(GL_BLEND);
	glBindVertexArray(0);
	glUseProgram(0);
}

void Flames_Init (void)
{
	// No console commands for now -- the system is fully automatic.
	// Hook point lives in gl_render.h so gl_rsurf can call Flames_Draw().
}
