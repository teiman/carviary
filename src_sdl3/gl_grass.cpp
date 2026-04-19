// gl_grass.cpp -- "grow_grass" experiment.
//
// Shoots a traceline from the player's view, finds the world surface hit,
// and marks its texture (texture_t::grass = 1). R_EmitTextureChains picks
// up the flag and routes those surfaces through R_WorldGrassShader instead
// of R_WorldOpaqueShader / R_WorldFenceShader.
//
// See docs/experiment_grass.md for the design.

#include "quakedef.h"
#include "gl_render.h"
#include "bsp_render.h"   // BLOCK_WIDTH / BLOCK_HEIGHT for lightmap UV math
#include <stdint.h>

extern GLuint lightmap_textures[]; // defined in gl_rsurf.cpp

// ---------------------------------------------------------------------------
// Grass shader (iteration 1: same layout as world_opaque, with a green tint).
// Keeps the pipeline minimal so we can verify the per-texture routing works
// before investing in blade geometry.
// ---------------------------------------------------------------------------
GLShader R_WorldGrassShader;
GLint    R_WorldGrassShader_u_mvp      = -1;
GLint    R_WorldGrassShader_u_tex      = -1;
GLint    R_WorldGrassShader_u_lightmap = -1;
GLint    R_WorldGrassShader_u_alpha    = -1;
static qboolean world_grass_ok = false;

// Shares the world vertex layout. Re-declared here as a literal so we don't
// need to export `world_vs_src` from gl_render.cpp.
static const char *grass_vs_src =
	"#version 330 core\n"
	"layout(location = 0) in vec3 a_pos;\n"
	"layout(location = 1) in vec2 a_tc;\n"
	"layout(location = 2) in vec2 a_lmtc;\n"
	"uniform mat4 u_mvp;\n"
	"out vec2 v_tc;\n"
	"out vec2 v_lmtc;\n"
	"void main() {\n"
	"    v_tc = a_tc;\n"
	"    v_lmtc = a_lmtc;\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

qboolean R_EnsureWorldGrassShader (void)
{
	if (world_grass_ok) return true;

	const char *fs =
		"#version 330 core\n"
		"in vec2 v_tc;\n"
		"in vec2 v_lmtc;\n"
		"uniform sampler2D u_tex;\n"
		"uniform sampler2D u_lightmap;\n"
		"uniform float u_alpha;\n"
		"out vec4 frag_color;\n"
		"void main() {\n"
		"    vec4 diffuse = texture(u_tex, v_tc);\n"
		"    vec4 lm = texture(u_lightmap, v_lmtc);\n"
		"    vec3 rgb = diffuse.rgb * lm.rgb;\n"
		"    // Tint toward grass green. Iteration 1: simple bias, no blades yet.\n"
		"    rgb = mix(rgb, rgb * vec3(0.55, 1.15, 0.55), 0.7);\n"
		"    frag_color = vec4(rgb, diffuse.a * u_alpha);\n"
		"}\n";

	char err[512];
	if (!GLShader_Build(&R_WorldGrassShader, grass_vs_src, fs, err, sizeof(err))) {
		Con_Printf("world_grass shader failed: %s\n", err);
		return false;
	}
	R_WorldGrassShader_u_mvp      = GLShader_Uniform(&R_WorldGrassShader, "u_mvp");
	R_WorldGrassShader_u_tex      = GLShader_Uniform(&R_WorldGrassShader, "u_tex");
	R_WorldGrassShader_u_lightmap = GLShader_Uniform(&R_WorldGrassShader, "u_lightmap");
	R_WorldGrassShader_u_alpha    = GLShader_Uniform(&R_WorldGrassShader, "u_alpha");
	world_grass_ok = true;
	return true;
}

// ---------------------------------------------------------------------------
// Resolve which world surface a point lies on.
// Walks the BSP from start -> end, and when it crosses a node plane, checks
// the surfaces belonging to that node for ST bounds containing the hit.
// Returns NULL if nothing found. Modeled after RecursiveLightPoint in
// gl_rlight.cpp.
// ---------------------------------------------------------------------------
static msurface_t *SurfaceAtHit_r (mnode_t *node, vec3_t start, vec3_t end)
{
	if (node->contents < 0)
		return NULL;

	float front = PlaneDiff(start, node->plane);
	float back  = PlaneDiff(end,   node->plane);

	if ((back < 0) == (front < 0))
		return SurfaceAtHit_r (node->children[front < 0], start, end);

	float frac = front / (front - back);
	vec3_t mid = {
		start[0] + (end[0] - start[0]) * frac,
		start[1] + (end[1] - start[1]) * frac,
		start[2] + (end[2] - start[2]) * frac,
	};

	// Front side first (we want the first hit, not the last).
	msurface_t *s = SurfaceAtHit_r (node->children[front < 0], start, mid);
	if (s) return s;

	msurface_t *surf = cl.worldmodel->surfaces + node->firstsurface;
	for (int i = 0; i < node->numsurfaces; ++i, ++surf) {
		if (surf->flags & SURF_DRAWTILED)
			continue; // sky / tiled surfaces: no valid texture bounds
		int ds = (int)(DotProduct(mid, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
		int dt = (int)(DotProduct(mid, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);
		if (ds < surf->texturemins[0] || dt < surf->texturemins[1]) continue;
		ds -= surf->texturemins[0];
		dt -= surf->texturemins[1];
		if (ds > surf->extents[0] || dt > surf->extents[1]) continue;
		return surf;
	}

	return SurfaceAtHit_r (node->children[front >= 0], mid, end);
}

// TraceLine is in chase.cpp; declare here to avoid pulling in that header.
extern float TraceLine (vec3_t start, vec3_t end, vec3_t impact, vec3_t normal);

// Resolves the surface under the player's view, or NULL if nothing is
// in sight. Used by grow_grass and grow_grass_probe.
static msurface_t *Grass_SurfaceUnderCrosshair (void)
{
	if (!cl.worldmodel) return NULL;

	vec3_t start, end, impact, normal;
	VectorCopy(r_refdef.vieworg, start);
	VectorMA(start, 8192.0f, vpn, end);

	float frac = TraceLine(start, end, impact, normal);
	if (frac >= 1.0f) return NULL;

	// Re-trace the BSP just to get the surface; TraceLine only returns a
	// point + plane normal. Use a tiny extension past the hit so mid-plane
	// resolution is stable.
	vec3_t probe_end;
	VectorMA(impact, 1.0f, vpn, probe_end);
	return SurfaceAtHit_r (cl.worldmodel->nodes, start, probe_end);
}

static texture_t *Grass_TextureUnderCrosshair (int *out_surf_flags)
{
	msurface_t *surf = Grass_SurfaceUnderCrosshair();
	if (!surf) return NULL;
	if (out_surf_flags) *out_surf_flags = surf->flags;
	return surf->texinfo->texture;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
static void GrowGrass_f (void)
{
	if (cls.state != ca_connected) {
		Con_Printf("grow_grass: not in a map\n");
		return;
	}

	int flags = 0;
	texture_t *t = Grass_TextureUnderCrosshair(&flags);
	if (!t) {
		Con_Printf("grow_grass: no surface in sight\n");
		return;
	}
	if (flags & (SURF_DRAWSKY | SURF_DRAWTURB)) {
		Con_Printf("grow_grass: hit sky/water, ignored\n");
		return;
	}
	if (t->grass) {
		Con_Printf("grow_grass: '%s' already grass\n", t->name);
		return;
	}
	t->grass = 1;
	Con_Printf("grow_grass: marked '%s'\n", t->name);
}

static void GrowGrassClear_f (void)
{
	if (!cl.worldmodel) return;
	int n = 0;
	for (int i = 0; i < cl.worldmodel->numtextures; ++i) {
		texture_t *t = cl.worldmodel->textures[i];
		if (!t) continue;
		if (t->grass) { t->grass = 0; n++; }
	}
	Con_Printf("grow_grass_clear: cleared %d texture(s)\n", n);
}

static void GrowGrassList_f (void)
{
	if (!cl.worldmodel) return;
	int n = 0;
	for (int i = 0; i < cl.worldmodel->numtextures; ++i) {
		texture_t *t = cl.worldmodel->textures[i];
		if (!t || !t->grass) continue;
		Con_Printf("  %s\n", t->name);
		n++;
	}
	if (n == 0) Con_Printf("grow_grass_list: none\n");
	else        Con_Printf("grow_grass_list: %d texture(s)\n", n);
}

// ===========================================================================
// Grass blades (iteration 2).
//
// For each texture marked as grass, we build -- on first draw -- a static VBO
// with a bunch of small vertical quads ("blades") scattered over every surface
// that uses that texture. The blades stay untextured for now: the fragment
// shader just draws a green vertical gradient with alpha discard near the
// edges.
//
// Blade layout in the VBO: 6 verts per blade (two triangles, no IBO).
// Each vert carries the blade's world-space base position, its yaw rotation,
// and a corner code 0..3. The vertex shader uses the corner code to offset
// the vert sideways + upward into the final quad position.
// ===========================================================================

#define GRASS_BLADE_HEIGHT_UNITS  14.0f  // ~1/4 of player height, base value
#define GRASS_BLADE_WIDTH_UNITS    2.4f  // wider
#define GRASS_POINT_SPACING_UNITS  6.5f

// A blade is now a triangle: BL, BR, TIP (three verts per blade, no quad).
// The shape tapers to a real point instead of a clipped rectangle.
typedef struct {
	float base_x, base_y, base_z;
	float yaw;      // rotation of the base edge around +Z, radians
	float corner;   // 0=BL, 1=BR, 2=TIP
	float height;   // per-blade height variation
	float lean_x;   // per-blade tip offset in world XY (both components)
	float lean_y;
	float lmtc_s;   // lightmap UV sampled at the blade's base
	float lmtc_t;
	float phase;    // per-blade wind phase offset (radians)
	float density;  // 0..1 patch density at this blade (used to darken sparse ones)
} grass_vtx_t;

// One draw range per (texture, lightmap page). We have to split blades by
// lightmap because a single texture in the worldmodel is typically spread
// across many lightmap pages, and we bind one sampler2D per draw.
typedef struct {
	int    first_vert;    // index into the shared VBO
	int    num_verts;     // multiple of 3
	int    lightmap_page; // lightmaptexturenum
} grass_range_t;

// Per-texture blade VBO + metadata. One of these per texture in the world
// model; only populated (vbo != 0) once the texture is marked as grass and
// seen by the renderer.
typedef struct {
	GLuint         vao;
	GLuint         vbo;
	int            num_verts; // total, 3 per blade
	int            built;     // non-zero once Grass_BuildBlades has run
	grass_range_t *ranges;    // one per (texture, lightmap) group
	int            num_ranges;
} grass_tex_data_t;

static grass_tex_data_t *grass_tex_data = NULL; // indexed by worldmodel texture index
static int              grass_tex_data_count = 0;

// Shader for blade rendering.
static GLShader R_GrassBladesShader;
static GLint    R_GrassBladesShader_u_mvp         = -1;
static GLint    R_GrassBladesShader_u_half_width  = -1;
static GLint    R_GrassBladesShader_u_lightmap    = -1;
static GLint    R_GrassBladesShader_u_time        = -1;
static GLint    R_GrassBladesShader_u_wind_amp    = -1;
static GLint    R_GrassBladesShader_u_wind_dir    = -1;
static GLint    R_GrassBladesShader_u_wave_speed  = -1;
static GLint    R_GrassBladesShader_u_wave_freq   = -1;
static GLint    R_GrassBladesShader_u_gust_speed  = -1;
static qboolean grass_blades_shader_ok    = false;

static qboolean R_EnsureGrassBladesShader (void)
{
	if (grass_blades_shader_ok) return true;

	// Vertex shader: blade base pos, yaw, a 0..3 corner code, a per-blade
	// height, and a lightmap UV sampled at the base.
	//   corner bit 0 (x): -1 (left)  / +1 (right)
	//   corner bit 1 (y):  0 (base)  /  1 (top)
	// Wind model:
	//   - Global direction (u_wind_dir). Every blade bends toward the same
	//     vector; they don't oscillate independently.
	//   - Traveling wave: phase depends on the blade's world position dotted
	//     with the wind direction, so you see gusts roll across the field.
	//   - Asymmetric envelope: wave ^ 2 biases the bend toward +wind_dir
	//     (blades live slightly bent, spring back faster than they bend out).
	//     Real grass does not oscillate symmetrically around a tense
	//     vertical -- it leans into the wind and the recovery is snappy.
	//   - Low-frequency gust modulation scales the global amplitude.
	//   - Per-blade `a_phase` is still used, but only as a tiny jitter so
	//     neighboring blades don't snap to exactly the same position.
	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in vec3  a_base;\n"
		"layout(location = 1) in float a_yaw;\n"
		"layout(location = 2) in float a_corner;\n"  // 0=BL, 1=BR, 2=TIP
		"layout(location = 3) in float a_height;\n"
		"layout(location = 4) in vec2  a_lean;\n"
		"layout(location = 5) in vec2  a_lmtc;\n"
		"layout(location = 6) in float a_phase;\n"
		"layout(location = 7) in float a_density;\n"
		"uniform mat4  u_mvp;\n"
		"uniform float u_half_width;\n"
		"uniform float u_time;\n"
		"uniform float u_wind_amp;\n"
		"uniform vec2  u_wind_dir;\n"                 // normalized in world XY
		"uniform float u_wave_speed;\n"
		"uniform float u_wave_freq;\n"                // spatial frequency (1/units)
		"uniform float u_gust_speed;\n"
		"out float v_t;\n"
		"out vec2  v_lmtc;\n"
		"out float v_density;\n"
		"\n"
		"vec2 wind_tip_offset(vec2 base_xy) {\n"
		"    // Phase of the traveling wave at this blade's base.\n"
		"    float x = u_time * u_wave_speed\n"
		"            - dot(base_xy, u_wind_dir) * u_wave_freq\n"
		"            + a_phase * 0.15;\n"
		"    float w = 0.5 + 0.5 * sin(x);\n"    // [0,1]
		"    // Asymmetric shape: lingers low, spikes fast when bent.\n"
		"    w = w * w;\n"
		"    // Slow global gusts.\n"
		"    float gust = 0.6 + 0.4 * sin(u_time * u_gust_speed);\n"
		"    // Always displaces toward +wind_dir, never opposite. A small\n"
		"    // baseline keeps a constant bend even in the lull.\n"
		"    float amp = u_wind_amp * (0.25 + gust * w);\n"
		"    return u_wind_dir * amp;\n"
		"}\n"
		"\n"
		"void main() {\n"
		"    int c = int(a_corner);\n"
		"    vec3 pos;\n"
		"    if (c == 2) {\n"
		"        vec2 wind = wind_tip_offset(a_base.xy);\n"
		"        pos = a_base + vec3(a_lean.x + wind.x,\n"
		"                            a_lean.y + wind.y,\n"
		"                            a_height);\n"
		"        v_t = 1.0;\n"
		"    } else {\n"
		"        float side = (c == 0) ? -1.0 : 1.0;\n"
		"        float cs = cos(a_yaw), sn = sin(a_yaw);\n"
		"        vec2 sideOffs = vec2(side * u_half_width * cs,\n"
		"                             side * u_half_width * sn);\n"
		"        pos = a_base + vec3(sideOffs.x, sideOffs.y, 0.0);\n"
		"        v_t = 0.0;\n"
		"    }\n"
		"    v_lmtc = a_lmtc;\n"
		"    v_density = a_density;\n"
		"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
		"}\n";

	// Fragment shader: blade silhouette via side taper + alpha discard, then
	// a dark-to-slightly-lighter gradient (Quake palette-ish greens), tinted
	// by the surface's lightmap so the blades darken in shaded areas.
	const char *fs =
		"#version 330 core\n"
		"in float v_t;\n"
		"in vec2  v_lmtc;\n"
		"in float v_density;\n"
		"uniform sampler2D u_lightmap;\n"
		"out vec4 frag_color;\n"
		"void main() {\n"
		"    // Base greens. Original palette x 0.8 x 0.85 x 0.80 ~ 0.544 of source.\n"
		"    vec3 rgb = mix(vec3(0.04352, 0.08160, 0.02720),\n"
		"                   vec3(0.11968, 0.18496, 0.06528), v_t);\n"
		"    // Sparse areas read as slightly darker grass (drier / shaded).\n"
		"    // density 1 -> 1.0, density 0 -> 0.80 (another 20% on top).\n"
		"    float shade = mix(0.80, 1.0, v_density);\n"
		"    vec3 lm = texture(u_lightmap, v_lmtc).rgb;\n"
		"    frag_color = vec4(rgb * lm * shade, 1.0);\n"
		"}\n";

	char err[512];
	if (!GLShader_Build(&R_GrassBladesShader, vs, fs, err, sizeof(err))) {
		Con_Printf("grass blades shader failed: %s\n", err);
		return false;
	}
	R_GrassBladesShader_u_mvp         = GLShader_Uniform(&R_GrassBladesShader, "u_mvp");
	R_GrassBladesShader_u_half_width  = GLShader_Uniform(&R_GrassBladesShader, "u_half_width");
	R_GrassBladesShader_u_lightmap    = GLShader_Uniform(&R_GrassBladesShader, "u_lightmap");
	R_GrassBladesShader_u_time        = GLShader_Uniform(&R_GrassBladesShader, "u_time");
	R_GrassBladesShader_u_wind_amp    = GLShader_Uniform(&R_GrassBladesShader, "u_wind_amp");
	R_GrassBladesShader_u_wind_dir    = GLShader_Uniform(&R_GrassBladesShader, "u_wind_dir");
	R_GrassBladesShader_u_wave_speed  = GLShader_Uniform(&R_GrassBladesShader, "u_wave_speed");
	R_GrassBladesShader_u_wave_freq   = GLShader_Uniform(&R_GrassBladesShader, "u_wave_freq");
	R_GrassBladesShader_u_gust_speed  = GLShader_Uniform(&R_GrassBladesShader, "u_gust_speed");
	grass_blades_shader_ok = true;
	return true;
}

// Cheap deterministic hash -> float in [0, 1). Used to pick blade positions
// and yaws consistently across frames without a RNG.
static float hash01 (unsigned int x)
{
	x = (x ^ 61) ^ (x >> 16);
	x = x + (x << 3);
	x = x ^ (x >> 4);
	x = x * 0x27d4eb2d;
	x = x ^ (x >> 15);
	return (float)(x & 0xFFFFFF) / (float)0x1000000;
}

// Hash a 2D integer cell coordinate -> float in [0, 1).
static float hash2d01 (int ix, int iy)
{
	unsigned int h = (unsigned int)ix * 0x9E3779B1u
	               ^ (unsigned int)iy * 0x85EBCA77u;
	return hash01(h);
}

// 2D value noise in [0, 1). Cell grid with bilinear smoothstep interpolation
// of hashed corner values. `cell_size` is the world-space period of the
// coarsest variation; larger = bigger patches.
// Looks like low-pass Gaussian blobs viewed from above.
static float grass_noise01 (float x, float y, float cell_size)
{
	float fx = x / cell_size;
	float fy = y / cell_size;
	int ix = (int)floorf(fx);
	int iy = (int)floorf(fy);
	float u = fx - (float)ix;
	float v = fy - (float)iy;
	// Smoothstep for softer transitions (matches the "gaussian-ish" look).
	u = u * u * (3.0f - 2.0f * u);
	v = v * v * (3.0f - 2.0f * v);

	float a = hash2d01(ix,     iy    );
	float b = hash2d01(ix + 1, iy    );
	float c = hash2d01(ix,     iy + 1);
	float d = hash2d01(ix + 1, iy + 1);
	float ab = a + (b - a) * u;
	float cd = c + (d - c) * u;
	return ab + (cd - ab) * v;
}

// Compute the lightmap UV for an arbitrary world-space point on a surface.
// Same formula used in BuildSurfaceDisplayList for vertex coords.
static void Grass_LightmapUV (msurface_t *s, const float pos[3], float *out_s, float *out_t)
{
	mtexinfo_t *ti = s->texinfo;
	float us = DotProduct(pos, ti->vecs[0]) + ti->vecs[0][3];
	us -= s->texturemins[0];
	us += s->light_s * 16 + 8;
	us /= BLOCK_WIDTH * 16.0f;

	float ut = DotProduct(pos, ti->vecs[1]) + ti->vecs[1][3];
	ut -= s->texturemins[1];
	ut += s->light_t * 16 + 8;
	ut /= BLOCK_HEIGHT * 16.0f;

	*out_s = us;
	*out_t = ut;
}

// Scatter N blade base points over a single surface polygon. Fan triangulation
// of the polygon, N_i points per triangle proportional to its area. Samples
// lightmap UV at each base so the shader can shade the blade like the ground.
static int Grass_ScatterOverSurface (msurface_t *s, grass_vtx_t *out, int out_capacity)
{
	glpoly_t *p = s->polys;
	if (!p || p->numverts < 3) return 0;

	// Total-area estimate: sum of fan-triangle areas.
	float total_area = 0.0f;
	float *v0 = p->verts[0];
	for (int i = 1; i < p->numverts - 1; ++i) {
		float *va = p->verts[i];
		float *vb = p->verts[i + 1];
		float ax = va[0] - v0[0], ay = va[1] - v0[1], az = va[2] - v0[2];
		float bx = vb[0] - v0[0], by = vb[1] - v0[1], bz = vb[2] - v0[2];
		float cx = ay*bz - az*by;
		float cy = az*bx - ax*bz;
		float cz = ax*by - ay*bx;
		total_area += 0.5f * sqrtf(cx*cx + cy*cy + cz*cz);
	}

	int target_blades = (int)(total_area / (GRASS_POINT_SPACING_UNITS * GRASS_POINT_SPACING_UNITS));
	if (target_blades < 1) return 0;

	int emitted = 0;
	// Seed mixes surface ptr with an incrementing counter, then hash_01 again
	// for each draw of entropy. Different surfaces get visibly different
	// distributions.
	unsigned int hseed = ((unsigned int)((uintptr_t)s) * 2654435761u) ^ 0x9E3779B9u;

	for (int i = 1; i < p->numverts - 1; ++i) {
		float *va = p->verts[i];
		float *vb = p->verts[i + 1];
		float ax = va[0] - v0[0], ay = va[1] - v0[1], az = va[2] - v0[2];
		float bx = vb[0] - v0[0], by = vb[1] - v0[1], bz = vb[2] - v0[2];
		float cx = ay*bz - az*by;
		float cy = az*bx - ax*bz;
		float cz = ax*by - ay*bx;
		float tri_area = 0.5f * sqrtf(cx*cx + cy*cy + cz*cz);

		int n_tri = (int)((tri_area / total_area) * target_blades + 0.5f);

		// Oversample candidates so that a 2D noise field can *increase* the
		// local density. We generate GRASS_DENSITY_MAX * n_tri candidates
		// per triangle and keep each with probability proportional to a
		// per-location patch value. The baseline probability is 1/MAX, so
		// sparse regions end up at the original density; dense regions
		// approach MAX x density.
		//
		// Contrast: a raw value-noise sample is ~Gaussian around 0.5, which
		// gives mushy middle tones everywhere. Pushing the noise through a
		// smoothstep stretches it toward 0 and 1, producing visible empty
		// patches and visible tufts instead of uniform fuzz.
		// Oversampling factor: at most GRASS_DENSITY_MAX times the baseline
		// density in the densest clumps. The minimum acceptance in the
		// sparsest regions is set separately (below) so we can dial min and
		// max independently.
		const int   GRASS_DENSITY_MAX = 13;
		const float GRASS_MIN_ACCEPT  = 0.073f; // sparse regions get ~7% of candidates
		int n_candidates = n_tri * GRASS_DENSITY_MAX;

		for (int k = 0; k < n_candidates; ++k) {
			if (emitted * 3 + 3 > out_capacity) goto done;
			float u  = hash01(hseed++);
			float vv = hash01(hseed++);
			if (u + vv > 1.0f) { u = 1.0f - u; vv = 1.0f - vv; }
			float pos[3] = {
				v0[0] + ax * u + bx * vv,
				v0[1] + ay * u + by * vv,
				v0[2] + az * u + bz * vv,
			};

			// Patch density field in world XY. Small cell sizes so clumps
			// are compact (roughly foot-sized) and densely packed.
			float n  = grass_noise01(pos[0], pos[1], 21.6f);
			float n2 = grass_noise01(pos[0] + 73.3f, pos[1] - 51.7f, 6.48f);
			float density = 0.70f * n + 0.30f * n2; // [0, 1)

			// Contrast curve: very narrow smoothstep window [0.43, 0.67] so
			// only a thin middle band transitions gradually; the rest of the
			// field collapses cleanly to fully sparse or fully packed.
			float d = density;
			if      (d < 0.43f) d = 0.0f;
			else if (d > 0.67f) d = 1.0f;
			else {
				d = (d - 0.43f) / (0.67f - 0.43f);
				d = d * d * (3.0f - 2.0f * d); // smoothstep
			}

			// Accept probability. GRASS_MIN_ACCEPT at d=0 (sparse zones),
			// 1.0 at d=1 (densest clumps keep every candidate -> MAX x base).
			float accept_p = GRASS_MIN_ACCEPT + (1.0f - GRASS_MIN_ACCEPT) * d;
			if (hash01(hseed++) > accept_p) continue;
			float yaw  = hash01(hseed++) * 6.2831853f;
			// Height variation: base +/- 35%.
			float h = GRASS_BLADE_HEIGHT_UNITS * (0.65f + 0.7f * hash01(hseed++));

			// Per-blade tip lean. Random direction, magnitude between 25%
			// and 60% of the blade's height so the tip leans noticeably
			// without looking flattened.
			float lean_angle = hash01(hseed++) * 6.2831853f;
			float lean_mag   = h * (0.25f + 0.35f * hash01(hseed++));
			float lean_x = cosf(lean_angle) * lean_mag;
			float lean_y = sinf(lean_angle) * lean_mag;

			float phase = hash01(hseed++) * 6.2831853f;

			float lms, lmt;
			Grass_LightmapUV(s, pos, &lms, &lmt);

			grass_vtx_t *o = &out[emitted * 3];
			#define GRASS_VTX(IDX, CORNER) do { \
				o[IDX].base_x = pos[0]; o[IDX].base_y = pos[1]; o[IDX].base_z = pos[2]; \
				o[IDX].yaw = yaw; o[IDX].corner = (float)(CORNER); \
				o[IDX].height = h; \
				o[IDX].lean_x = lean_x; o[IDX].lean_y = lean_y; \
				o[IDX].lmtc_s = lms; o[IDX].lmtc_t = lmt; \
				o[IDX].phase = phase; \
				o[IDX].density = d; \
			} while (0)
			// Single triangle: BL, BR, TIP
			GRASS_VTX(0, 0); GRASS_VTX(1, 1); GRASS_VTX(2, 2);
			#undef GRASS_VTX
			emitted++;
		}
	}
done:
	return emitted;
}

// Temporary structure: per-surface scatter output, later sorted by lightmap.
typedef struct {
	int lightmap;
	int vert_count;      // 3 per blade (BL, BR, TIP)
	int scratch_offset;  // into `scratch`, measured in verts
} grass_surf_chunk_t;

// Build the blade VBO for one texture (first use, lazy). Walks every world
// surface using this texture, scatters blades over it, then groups them by
// lightmap page so each draw only needs one lightmap bind.
static void Grass_BuildBlades (int tex_index)
{
	if (tex_index < 0 || tex_index >= grass_tex_data_count) return;
	grass_tex_data_t *td = &grass_tex_data[tex_index];
	if (td->built) return;
	td->built = 1;

	texture_t *target = cl.worldmodel->textures[tex_index];
	if (!target) return;

	// Per-texture scatter budget. Generous: a full e1m1 covered in ground1_6
	// produced ~74k blades with the current tuning, and dense maps can push
	// well past that. Sized so overflow is essentially impossible in normal
	// play. 200k * 3 verts * 52 bytes = ~30 MB BSS.
	enum { MAX_BLADES = 200000 };
	static grass_vtx_t scratch[MAX_BLADES * 3];
	static grass_surf_chunk_t chunks[8192];
	int vert_count = 0;
	int chunk_count = 0;
	int overflow_surfaces = 0; // surfaces that couldn't place any more blades

	for (int i = 0; i < cl.worldmodel->numsurfaces; ++i) {
		msurface_t *s = &cl.worldmodel->surfaces[i];
		if (!s->texinfo || s->texinfo->texture != target) continue;
		if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) continue;
		int remaining_capacity = MAX_BLADES * 3 - vert_count;
		if (remaining_capacity < 3) { overflow_surfaces++; continue; }
		int added = Grass_ScatterOverSurface(s, scratch + vert_count, remaining_capacity);
		if (added == 0) continue;
		if (chunk_count >= 8192) { overflow_surfaces++; continue; }
		chunks[chunk_count].lightmap       = s->lightmaptexturenum;
		chunks[chunk_count].scratch_offset = vert_count;
		chunks[chunk_count].vert_count     = added * 3;
		chunk_count++;
		vert_count += added * 3;
	}
	if (overflow_surfaces > 0) {
		Con_Printf("grass: WARNING '%s' ran out of blade buffer on %d surface(s); "
		           "raise MAX_BLADES in gl_grass.cpp\n",
		           target->name, overflow_surfaces);
	}
	if (vert_count == 0) return;

	// Sort chunks by lightmap so blades sharing a page become one range.
	// Simple in-place insertion sort -- chunk counts stay low.
	for (int i = 1; i < chunk_count; ++i) {
		grass_surf_chunk_t key = chunks[i];
		int j = i - 1;
		while (j >= 0 && chunks[j].lightmap > key.lightmap) {
			chunks[j + 1] = chunks[j];
			j--;
		}
		chunks[j + 1] = key;
	}

	// Rebuild a linear buffer in the new order and collapse adjacent same-lm
	// chunks into one draw range.
	static grass_vtx_t repacked[MAX_BLADES * 3];
	int out_cursor = 0;
	grass_range_t *ranges = (grass_range_t *)malloc(sizeof(grass_range_t) * chunk_count);
	int num_ranges = 0;
	for (int i = 0; i < chunk_count; ) {
		int j = i;
		int first = out_cursor;
		int lm = chunks[i].lightmap;
		while (j < chunk_count && chunks[j].lightmap == lm) {
			memcpy(repacked + out_cursor, scratch + chunks[j].scratch_offset,
			       chunks[j].vert_count * sizeof(grass_vtx_t));
			out_cursor += chunks[j].vert_count;
			j++;
		}
		ranges[num_ranges].first_vert    = first;
		ranges[num_ranges].num_verts     = out_cursor - first;
		ranges[num_ranges].lightmap_page = lm;
		num_ranges++;
		i = j;
	}

	glGenVertexArrays(1, &td->vao);
	glGenBuffers(1, &td->vbo);
	glBindVertexArray(td->vao);
	glBindBuffer(GL_ARRAY_BUFFER, td->vbo);
	glBufferData(GL_ARRAY_BUFFER, out_cursor * sizeof(grass_vtx_t), repacked, GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(grass_vtx_t), (void*)offsetof(grass_vtx_t, base_x));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(grass_vtx_t), (void*)offsetof(grass_vtx_t, yaw));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(grass_vtx_t), (void*)offsetof(grass_vtx_t, corner));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(grass_vtx_t), (void*)offsetof(grass_vtx_t, height));
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(grass_vtx_t), (void*)offsetof(grass_vtx_t, lean_x));
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(grass_vtx_t), (void*)offsetof(grass_vtx_t, lmtc_s));
	glEnableVertexAttribArray(6);
	glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(grass_vtx_t), (void*)offsetof(grass_vtx_t, phase));
	glEnableVertexAttribArray(7);
	glVertexAttribPointer(7, 1, GL_FLOAT, GL_FALSE, sizeof(grass_vtx_t), (void*)offsetof(grass_vtx_t, density));
	glBindVertexArray(0);

	td->num_verts  = out_cursor;
	td->ranges     = ranges;
	td->num_ranges = num_ranges;
	Con_DPrintf("grow_grass: built %d blades in %d ranges for '%s'\n",
	            out_cursor / 3, num_ranges, target->name);
}

// Ensure per-texture data array matches the worldmodel. Called by the render
// path the first time blades are drawn. Rebuilds if the map changed.
static void Grass_EnsureTexData (void)
{
	if (!cl.worldmodel) return;
	// Key on (worldmodel pointer, numtextures). The pointer is stable for a
	// map's lifetime; re-keying on numtextures alone could, in theory, match
	// a different world that happens to share texture count.
	static model_t *grass_last_worldmodel = NULL;
	if (grass_tex_data
	    && grass_last_worldmodel == cl.worldmodel
	    && grass_tex_data_count == cl.worldmodel->numtextures)
		return;

	// Free existing buffers first.
	if (grass_tex_data) {
		for (int i = 0; i < grass_tex_data_count; ++i) {
			if (grass_tex_data[i].vbo) glDeleteBuffers(1, &grass_tex_data[i].vbo);
			if (grass_tex_data[i].vao) glDeleteVertexArrays(1, &grass_tex_data[i].vao);
		}
		free(grass_tex_data);
	}
	grass_tex_data_count = cl.worldmodel->numtextures;
	grass_tex_data = (grass_tex_data_t *)calloc(grass_tex_data_count, sizeof(grass_tex_data_t));
	grass_last_worldmodel = cl.worldmodel;
}

// Public: called from R_EmitTextureChains as a second pass, after the base
// world surfaces have been drawn. Walks all grass-marked textures, lazy-
// builds their blade VBOs on first use, and emits one draw per texture.
void Grass_DrawBlades (void)
{
	if (!cl.worldmodel) return;
	if (!R_EnsureGrassBladesShader()) return;

	// Cheap early-out: any texture marked grass?
	int any = 0;
	for (int i = 0; i < cl.worldmodel->numtextures; ++i) {
		texture_t *t = cl.worldmodel->textures[i];
		if (t && t->grass) { any = 1; break; }
	}
	if (!any) return;

	Grass_EnsureTexData();

	float mvp[16];
	R_CurrentMVP(mvp);

	GLShader_Use(&R_GrassBladesShader);
	glUniformMatrix4fv(R_GrassBladesShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform1f(R_GrassBladesShader_u_half_width, GRASS_BLADE_WIDTH_UNITS * 0.5f);
	glUniform1i(R_GrassBladesShader_u_lightmap,   1);   // texture unit 1
	glUniform1f(R_GrassBladesShader_u_time,       (float)realtime);

	// Wind model parameters. All coherent across the field.
	//   wind_amp    = max units the tip can be pushed in wind_dir direction.
	//   wind_dir    = horizontal unit vector the wind blows toward.
	//   wave_speed  = how fast the traveling wave sweeps the field (rad/s).
	//   wave_freq   = spatial frequency in world units (rad/unit). Smaller =
	//                 longer waves (slower spatial variation).
	//   gust_speed  = slow modulation of amplitude for gust/lull cycles.
	glUniform1f(R_GrassBladesShader_u_wind_amp,   4.0f);
	glUniform2f(R_GrassBladesShader_u_wind_dir,   0.7071f, 0.7071f);
	glUniform1f(R_GrassBladesShader_u_wave_speed, 1.8f);
	glUniform1f(R_GrassBladesShader_u_wave_freq,  0.04f);
	glUniform1f(R_GrassBladesShader_u_gust_speed, 0.35f);

	// Grass blades are solid (alpha discard, no blending). Keep depth write on
	// so later passes cull against them.
	GLboolean blend_was = glIsEnabled(GL_BLEND);
	if (blend_was) glDisable(GL_BLEND);

	int last_lm = -1;
	for (int i = 0; i < cl.worldmodel->numtextures; ++i) {
		texture_t *t = cl.worldmodel->textures[i];
		if (!t || !t->grass) continue;
		Grass_BuildBlades(i);
		grass_tex_data_t *td = &grass_tex_data[i];
		if (td->num_verts == 0) continue;
		glBindVertexArray(td->vao);
		for (int r = 0; r < td->num_ranges; ++r) {
			grass_range_t *rng = &td->ranges[r];
			if (rng->lightmap_page != last_lm) {
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, lightmap_textures[rng->lightmap_page]);
				last_lm = rng->lightmap_page;
			}
			glDrawArrays(GL_TRIANGLES, rng->first_vert, rng->num_verts);
		}
	}

	glActiveTexture(GL_TEXTURE0);
	if (blend_was) glEnable(GL_BLEND);
	glBindVertexArray(0);
	glUseProgram(0);
}

// Textures that should be treated as grass automatically at every level
// load. Matched case-insensitively against texture_t::name (which the
// loader already lowercases). Extend as you find more candidates.
static const char *grass_auto_names[] = {
	"ground1_6",
	NULL
};


void Grass_OnNewMap (void)
{
	if (!cl.worldmodel) return;
	int marked = 0;
	for (int i = 0; i < cl.worldmodel->numtextures; ++i) {
		texture_t *t = cl.worldmodel->textures[i];
		if (!t) continue;
		for (const char **n = grass_auto_names; *n; ++n) {
			if (!strcmp(t->name, *n)) { t->grass = 1; marked++; break; }
		}
	}
	if (marked > 0)
		Con_DPrintf("grass: auto-marked %d texture(s) on map load\n", marked);
}

void Grass_Init (void)
{
	Cmd_AddCommand("grow_grass",       GrowGrass_f);
	Cmd_AddCommand("grow_grass_clear", GrowGrassClear_f);
	Cmd_AddCommand("grow_grass_list",  GrowGrassList_f);
}
