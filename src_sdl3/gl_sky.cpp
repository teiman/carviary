/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// REDO: recode skybox
// gl_sky.c all sky related stuff
//
#include "quakedef.h"
#include "gl_render.h"
#include "gl_profiler.h"

typedef struct {
	float x, y, z;
	float s, t;
} sky_vertex_t;

static DynamicVBO skybox_vbo;
static qboolean   skybox_vbo_ready = false;

static void Skybox_EnsureVBO(void)
{
	if (skybox_vbo_ready) return;
	if (!R_EnsureSpriteShader()) return;

	DynamicVBO_Init(&skybox_vbo, 6 * sizeof(sky_vertex_t));
	DynamicVBO_SetAttrib(&skybox_vbo, 0, 3, GL_FLOAT, GL_FALSE, sizeof(sky_vertex_t), offsetof(sky_vertex_t, x));
	DynamicVBO_SetAttrib(&skybox_vbo, 1, 2, GL_FLOAT, GL_FALSE, sizeof(sky_vertex_t), offsetof(sky_vertex_t, s));
	skybox_vbo_ready = true;
}

int		solidskytexture;
int		alphaskytexture;
float	speedscale;		// for top sky and bottom sky

// ---------------------------------------------------------------------------
// Sun / moon overlay. Plan C: drawn ON TOP of all the sky layers as an
// additive billboard, after R_DrawSky finishes. A soft glow around a tight
// white core gives a moon-ish look. No depth-gating tricks, no per-poly
// iteration -- a single billboard at a fixed world direction drawn last.
// ---------------------------------------------------------------------------
static GLShader R_SkySunShader;
static GLint    R_SkySunShader_u_mvp        = -1;
static GLint    R_SkySunShader_u_center     = -1;
static GLint    R_SkySunShader_u_cam_right  = -1;
static GLint    R_SkySunShader_u_cam_up     = -1;
static GLint    R_SkySunShader_u_radius     = -1;
static GLint    R_SkySunShader_u_sun_color  = -1;
static GLint    R_SkySunShader_u_halo       = -1;   // halo softness factor
static GLint    R_SkySunShader_u_bloom      = -1;
static qboolean skysun_shader_ok = false;
static GLuint   skysun_vao = 0;

cvar_t r_sky_sun           = {"r_sky_sun",           "1"};
cvar_t r_sky_sun_strength  = {"r_sky_sun_strength",  "1.3"};
// Angular size of the disc core, in fraction of the billboard quad.
// (unused now; kept so external cvar_t references remain valid)
cvar_t r_sky_sun_tight     = {"r_sky_sun_tight",     "0.25"};
// Pale cool white: leans slightly blue so the overall feel is moon-like
// even under dark cloud banks.
cvar_t r_sky_sun_r         = {"r_sky_sun_r",         "0.92"};
cvar_t r_sky_sun_g         = {"r_sky_sun_g",         "0.94"};
cvar_t r_sky_sun_b         = {"r_sky_sun_b",         "1.00"};
// 0 = sun does not feed the bloom pipeline (crisp disc, no halo beyond
// what the shader itself draws). 1 = feeds fb_mask, inheriting the
// global glow radius. Independent of r_fb_glow / r_fb_glow_radius.
cvar_t r_sky_sun_bloom     = {"r_sky_sun_bloom",     "1"};

static qboolean R_EnsureSkySunShader (void)
{
	if (skysun_shader_ok) return true;
	const char *vs =
		"#version 330 core\n"
		"uniform mat4  u_mvp;\n"
		"uniform vec3  u_center;\n"
		"uniform vec3  u_cam_right;\n"
		"uniform vec3  u_cam_up;\n"
		"uniform float u_radius;\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    int c = gl_VertexID;\n"
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
		"uniform vec3  u_sun_color;\n"
		"uniform float u_halo;\n"
		"uniform float u_bloom;\n"
		"layout(location = 0) out vec4 frag_color;\n"
		"layout(location = 1) out vec4 frag_fbmask;\n"
		"void main() {\n"
		"    vec2 d = v_uv - 0.5;\n"
		"    float r = length(d) * 2.0;\n"
		"    if (r > 1.0) discard;\n"
		"    float core = 1.0 - smoothstep(0.18, 0.32, r);\n"
		"    float halo = exp(-r * r * u_halo) * 0.55;\n"
		"    float m = max(core, halo);\n"
		"    vec3 col = u_sun_color * m;\n"
		"    frag_color  = vec4(col, m);\n"
		"    // fb_mask only when bloom is requested. Zero otherwise so the\n"
		"    // post-FX pipeline gives the sun no halo beyond its own.\n"
		"    frag_fbmask = (u_bloom > 0.5)\n"
		"                  ? vec4(col, gl_FragCoord.z)\n"
		"                  : vec4(0.0);\n"
		"}\n";
	char err[1024];
	if (!GLShader_Build(&R_SkySunShader, vs, fs, err, sizeof(err))) {
		Con_Printf("sky sun shader failed: %s\n", err);
		return false;
	}
	R_SkySunShader_u_mvp        = GLShader_Uniform(&R_SkySunShader, "u_mvp");
	R_SkySunShader_u_center     = GLShader_Uniform(&R_SkySunShader, "u_center");
	R_SkySunShader_u_cam_right  = GLShader_Uniform(&R_SkySunShader, "u_cam_right");
	R_SkySunShader_u_cam_up     = GLShader_Uniform(&R_SkySunShader, "u_cam_up");
	R_SkySunShader_u_radius     = GLShader_Uniform(&R_SkySunShader, "u_radius");
	R_SkySunShader_u_sun_color  = GLShader_Uniform(&R_SkySunShader, "u_sun_color");
	R_SkySunShader_u_halo       = GLShader_Uniform(&R_SkySunShader, "u_halo");
	R_SkySunShader_u_bloom      = GLShader_Uniform(&R_SkySunShader, "u_bloom");
	skysun_shader_ok = true;
	return true;
}

// ---------------------------------------------------------------------------
// Dedicated shader for the near (alpha) sky layer. Same attribute layout as
// the sprite shader (vec3 pos, vec2 tc) so it plugs into the existing VBO,
// but the fragment stage multiplies the sampled alpha by a large-scale fbm
// noise scrolled over time. The effect: big soft "windows" drift across
// the cloud layer, revealing the far sky underneath. When no window is
// present, the cloud layer behaves exactly as before.
// ---------------------------------------------------------------------------
static GLShader R_SkyAlphaShader;
static GLint    R_SkyAlphaShader_u_mvp      = -1;
static GLint    R_SkyAlphaShader_u_tex      = -1;
static GLint    R_SkyAlphaShader_u_time     = -1;
static GLint    R_SkyAlphaShader_u_scroll   = -1;   // matches speedscale
static qboolean skyalpha_shader_ok         = false;

static qboolean R_EnsureSkyAlphaShader (void)
{
	if (skyalpha_shader_ok) return true;

	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in vec3 a_pos;\n"
		"layout(location = 1) in vec2 a_tc;\n"
		"uniform mat4 u_mvp;\n"
		"out vec2 v_tc;\n"
		"out vec2 v_world_xy;\n"      // large-scale coord for the noise
		"void main() {\n"
		"    v_tc = a_tc;\n"
		"    // Use a dir-ish proxy for the noise coord: the (s,t) already\n"
		"    // encodes the sky dome projection (see EmitSkyPolys_Pass),\n"
		"    // so feeding it to noise makes the windows track the sky dome.\n"
		"    v_world_xy = a_tc;\n"
		"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
		"}\n";

	const char *fs =
		"#version 330 core\n"
		"in vec2 v_tc;\n"
		"in vec2 v_world_xy;\n"
		"uniform sampler2D u_tex;\n"
		"uniform float u_time;\n"
		"uniform float u_scroll;\n"
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
		"    vec4 tex = texture(u_tex, v_tc);\n"
		"    vec2 np = v_world_xy * 4.5 + vec2(u_time * 0.09, -u_time * 0.07);\n"
		"    float n = fbm(np);\n"
		"    float window = smoothstep(0.35, 0.60, n);\n"
		"    frag_color = vec4(tex.rgb, tex.a * window);\n"
		"    frag_fbmask = vec4(0.0);\n"
		"}\n";

	char err[512];
	if (!GLShader_Build(&R_SkyAlphaShader, vs, fs, err, sizeof(err))) {
		Con_Printf("sky_alpha shader failed: %s\n", err);
		return false;
	}
	R_SkyAlphaShader_u_mvp    = GLShader_Uniform(&R_SkyAlphaShader, "u_mvp");
	R_SkyAlphaShader_u_tex    = GLShader_Uniform(&R_SkyAlphaShader, "u_tex");
	R_SkyAlphaShader_u_time   = GLShader_Uniform(&R_SkyAlphaShader, "u_time");
	R_SkyAlphaShader_u_scroll = GLShader_Uniform(&R_SkyAlphaShader, "u_scroll");
	skyalpha_shader_ok = true;
	return true;
}


char	skyname[256];
char	*suf[6] = {"rt", "bk", "lf", "ft", "up", "dn"};
int		skytexture[6];

/*
=============
EmitSkyPolys

Classic Quake animated sky: solid layer scrolling slowly + alpha layer
scrolling at double speed overlaid on top.
=============
*/
typedef struct { float x, y, z; float s, t; } sky_poly_vtx_t;
#define SKYPOLY_MAX_VERTS 4096
static sky_poly_vtx_t skypoly_soup[SKYPOLY_MAX_VERTS];
static DynamicVBO     skypoly_vbo;
static qboolean       skypoly_vbo_ready = false;

static void Skypoly_EnsureVBO(void)
{
	if (skypoly_vbo_ready) return;
	DynamicVBO_Init(&skypoly_vbo, sizeof(skypoly_soup));
	DynamicVBO_SetAttrib(&skypoly_vbo, 0, 3, GL_FLOAT, GL_FALSE, sizeof(sky_poly_vtx_t), offsetof(sky_poly_vtx_t, x));
	DynamicVBO_SetAttrib(&skypoly_vbo, 1, 2, GL_FLOAT, GL_FALSE, sizeof(sky_poly_vtx_t), offsetof(sky_poly_vtx_t, s));
	skypoly_vbo_ready = true;
}

static void EmitSkyPolys_Pass (msurface_t *s, float speed)
{
	glpoly_t	*p;
	float		*v;
	int			i;
	float		r, t;
	vec3_t		dir;
	float		length;

	speedscale = realtime * speed;
	speedscale -= (int)speedscale & ~127;

	static sky_poly_vtx_t fan[256];
	int n = 0;

	for (p = s->polys; p; p = p->next)
	{
		int nv = p->numverts;
		if (nv > 256) nv = 256;

		v = p->verts[0];
		for (i = 0; i < nv; ++i, v += VERTEXSIZE)
		{
			VectorSubtract(v, r_origin, dir);
			dir[2] *= 3;	// flatten the sphere

			length = 378 / Length(dir);

			dir[0] *= length;
			dir[1] *= length;

			r = (speedscale + dir[0]) * 0.0078125f;
			t = (speedscale + dir[1]) * 0.0078125f;

			fan[i].x = v[0]; fan[i].y = v[1]; fan[i].z = v[2];
			fan[i].s = r;    fan[i].t = t;
		}

		for (int k = 1; k < nv - 1; ++k)
		{
			if (n + 3 > SKYPOLY_MAX_VERTS) goto flush;
			skypoly_soup[n++] = fan[0];
			skypoly_soup[n++] = fan[k];
			skypoly_soup[n++] = fan[k + 1];
		}
	}

flush:
	if (n == 0) return;

	DynamicVBO_Upload(&skypoly_vbo, skypoly_soup, (GLsizei)(n * sizeof(sky_poly_vtx_t)));
	DynamicVBO_Bind(&skypoly_vbo);
	Prof_CountDraw(n);
	glDrawArrays(GL_TRIANGLES, 0, n);
}

void EmitSkyPolys (msurface_t *s)
{
	if (!R_EnsureSpriteShader()) return;
	Skypoly_EnsureVBO();

	float mvp[16];
	R_CurrentMVP(mvp);

	GLShader_Use(&R_SpriteShader);
	glUniformMatrix4fv(R_SpriteShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform4f       (R_SpriteShader_u_color, 1.0f, 1.0f, 1.0f, 1.0f);
	glUniform1i       (R_SpriteShader_u_tex, 0);

	// Pass 1: solid sky, slow scroll.
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, solidskytexture);
	EmitSkyPolys_Pass(s, 1.6f);

	// Pass 2: alpha sky, double-speed scroll, blended over pass 1. Uses a
	// dedicated shader that multiplies the sampled alpha by a large-scale
	// drifting fbm, so soft "windows" open and close over time, revealing
	// the (blurrier) far layer underneath.
	GLboolean blend_was_on = glIsEnabled(GL_BLEND);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	if (R_EnsureSkyAlphaShader()) {
		GLShader_Use(&R_SkyAlphaShader);
		glUniformMatrix4fv(R_SkyAlphaShader_u_mvp, 1, GL_FALSE, mvp);
		glUniform1i       (R_SkyAlphaShader_u_tex, 0);
		glUniform1f       (R_SkyAlphaShader_u_time, (float)realtime);
		glUniform1f       (R_SkyAlphaShader_u_scroll, 0.0f);
	}

	glBindTexture(GL_TEXTURE_2D, alphaskytexture);
	EmitSkyPolys_Pass(s, 3.2f);

	if (!blend_was_on) glDisable(GL_BLEND);

	glBindVertexArray(0);
	glUseProgram(0);
}


/*
==================
R_LoadSkys
==================
*/
void R_LoadSkys (void)
{
	int		i;
	char	name[64];

	for (i=0 ; i<6 ; i++)
	{
		_snprintf (name, sizeof(name),"gfx/env/%s%s", skyname, suf[i]);
		skytexture[i] = loadtextureimage(name, false ,true);
		if (skytexture[i] == 0)
		{
			_snprintf (name, sizeof(name),"gfx/env/tomazsky%s", suf[i]);
			skytexture[i] = loadtextureimage(name, true ,true);
		}
	}
}
void R_SetSkyBox (char *sky)
{
	strcpy(skyname, sky);
	R_LoadSkys ();
}

void LoadSky_f (void)
{
	switch (Cmd_Argc())
	{
	case 1:
		if (skyname[0])
			Con_Printf("Current sky: %s\n", skyname);
		else
			Con_Printf("Error: No skybox has been set\n");
		break;
	case 2:
		R_SetSkyBox(Cmd_Argv(1));
		break;
	default:
		Con_Printf("Usage: loadsky skyname\n");
		break;
	}
}
// Tomaz - Skybox End

/*
=================
R_DrawSky
=================
*/
static void Sky_DrawSun_Billboard (void);

void R_DrawSky (msurface_t *s)
{
	for ( ; s ; s=s->texturechain)
		EmitSkyPolys (s);
}

// Public: call at the end of the frame (after all 3D / post-fx, before
// HUD). Plan D: hit-test against sky surfaces along the fixed sun_dir;
// if the ray reaches sky, draw the sun billboard with depth test off on
// top of the scene. Does NOT call itself from R_DrawSky because we want
// it to stomp over any occluder -- the hit-test is what gates visibility.
void Sky_DrawSun (void)
{
	Sky_DrawSun_Billboard();
}

// ---------------------------------------------------------------------------
// Ray-walk the world BSP and return the FIRST world surface any portion of
// the ray [start, end] crosses (including both sky and non-sky). The caller
// then tests whether that first surface is a sky surface -- only in that
// case should the sun be visible. This is stricter than "sky somewhere
// along the ray", which would wrongly pierce through walls.
// ---------------------------------------------------------------------------
static msurface_t *Sky_TraceFirstSurface_r (mnode_t *node, vec3_t start, vec3_t end)
{
	if (node->contents < 0) return NULL;

	float front = PlaneDiff(start, node->plane);
	float back  = PlaneDiff(end,   node->plane);
	if ((back < 0) == (front < 0))
		return Sky_TraceFirstSurface_r (node->children[front < 0], start, end);

	float frac = front / (front - back);
	vec3_t mid = {
		start[0] + (end[0] - start[0]) * frac,
		start[1] + (end[1] - start[1]) * frac,
		start[2] + (end[2] - start[2]) * frac,
	};

	// Search the near side first: if anything hits, that is the first
	// surface the ray crosses and we're done -- do NOT keep looking on
	// the far side.
	msurface_t *near_hit = Sky_TraceFirstSurface_r (node->children[front < 0], start, mid);
	if (near_hit) return near_hit;

	// Surfaces on this node: check whether `mid` lies inside any of them.
	// Sky surfaces are SURF_DRAWTILED too, so texturemins/extents don't
	// have meaningful ST bounds. For sky we accept the hit unconditionally
	// (any sky poly on this split plane). For non-sky we require a proper
	// ST containment test as before.
	msurface_t *surf = cl.worldmodel->surfaces + node->firstsurface;
	for (int i = 0; i < node->numsurfaces; ++i, ++surf) {
		if (surf->flags & SURF_DRAWSKY) {
			// Sky polys span huge extents; if we crossed this node's
			// plane AND any sky poly is attached, treat as hit.
			return surf;
		}
		if (surf->flags & SURF_DRAWTILED) continue;  // non-sky tiled = sub-models w/o ST
		int ds = (int)(DotProduct(mid, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
		int dt = (int)(DotProduct(mid, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);
		if (ds < surf->texturemins[0] || dt < surf->texturemins[1]) continue;
		ds -= surf->texturemins[0];
		dt -= surf->texturemins[1];
		if (ds > surf->extents[0] || dt > surf->extents[1]) continue;
		return surf;
	}

	return Sky_TraceFirstSurface_r (node->children[front >= 0], mid, end);
}

// Fires a ray from the eye along `dir`; returns true only if the very
// first world surface it reaches is a sky surface. Anything else (wall,
// ceiling, floor) counts as "sun not visible".
static qboolean Sky_IsSkyInDirection (const vec3_t dir, float max_len)
{
	if (!cl.worldmodel) return false;
	vec3_t start, end;
	VectorCopy(r_refdef.vieworg, start);
	end[0] = start[0] + dir[0] * max_len;
	end[1] = start[1] + dir[1] * max_len;
	end[2] = start[2] + dir[2] * max_len;
	msurface_t *s = Sky_TraceFirstSurface_r (cl.worldmodel->nodes, start, end);
	return (s && (s->flags & SURF_DRAWSKY));
}

// Camera-aligned billboard along the fixed sun direction, additively
// blended on top of the already-rendered sky. Tight white core + soft
// halo. The bloom post-FX does the heavy lifting on the glow.
static void Sky_DrawSun_Billboard (void)
{
	if (!r_sky_sun.value) return;
	if (!R_EnsureSkySunShader()) return;
	if (!skysun_vao) glGenVertexArrays(1, &skysun_vao);

	float mvp[16];
	R_CurrentMVP(mvp);

	vec3_t sun_dir = { 0.45f, 0.25f, 0.85f };
	float len = sqrtf(sun_dir[0]*sun_dir[0] + sun_dir[1]*sun_dir[1] + sun_dir[2]*sun_dir[2]);
	sun_dir[0] /= len; sun_dir[1] /= len; sun_dir[2] /= len;

	// Visibility: sun must be in front of the camera AND the ray from the
	// eye must reach a sky surface without anything else in the way.
	float facing = vpn[0]*sun_dir[0] + vpn[1]*sun_dir[1] + vpn[2]*sun_dir[2];
	qboolean target_visible = (facing > 0.0f) && Sky_IsSkyInDirection(sun_dir, 16384.0f);

	// Fade in (fast) when target_visible, fade out (very fast) when not.
	// Animated as a 0..1 factor across frames with per-frame dt.
	static float vis_now  = 0.0f;
	static float last_time = 0.0f;
	float now = (float)realtime;
	float dt  = now - last_time;
	if (dt < 0.0f || dt > 0.2f) dt = 0.016f;
	last_time = now;
	const float FADE_IN_PER_SEC  = 8.0f;   // reaches full in ~0.125 s
	const float FADE_OUT_PER_SEC = 20.0f;  // fades in ~0.05 s
	if (target_visible) {
		vis_now += FADE_IN_PER_SEC * dt;
		if (vis_now > 1.0f) vis_now = 1.0f;
	} else {
		vis_now -= FADE_OUT_PER_SEC * dt;
		if (vis_now < 0.0f) vis_now = 0.0f;
	}
	if (vis_now <= 0.0f) return;

	const float DIST = 1500.0f;
	vec3_t center = {
		r_origin[0] + sun_dir[0] * DIST,
		r_origin[1] + sun_dir[1] * DIST,
		r_origin[2] + sun_dir[2] * DIST,
	};
	float radius = DIST * 0.07f;    // ~4 deg half-angle

	// Attenuation from last frame's pixel under the sun centre. Reading
	// the framebuffer here is a 1-pixel glReadPixels -- negligible but
	// one frame late, which looks fine for a slow-moving sun.
	//
	// attenuation = 1.0 when underlying pixel is black
	// attenuation = 0.7 when it's white (we cap at 30% reduction)
	static float prev_attn = 1.0f;
	{
		// Project sun_dir onto screen with this frame's MVP (to read the
		// pixel *of last frame*, which was drawn with a near-identical
		// projection -- one frame offset is invisible).
		float sp[3] = { r_origin[0] + sun_dir[0]*100.0f,
		                r_origin[1] + sun_dir[1]*100.0f,
		                r_origin[2] + sun_dir[2]*100.0f };
		float cx = mvp[0]*sp[0] + mvp[4]*sp[1] + mvp[8] *sp[2] + mvp[12];
		float cy = mvp[1]*sp[0] + mvp[5]*sp[1] + mvp[9] *sp[2] + mvp[13];
		float cw = mvp[3]*sp[0] + mvp[7]*sp[1] + mvp[11]*sp[2] + mvp[15];
		if (cw > 0.001f) {
			float nx = cx / cw;
			float ny = cy / cw;
			int cx_px = (int)((nx * 0.5f + 0.5f) * (float)vid.width);
			int cy_px = (int)((ny * 0.5f + 0.5f) * (float)vid.height);
			// Read an NxN block around the centre and average it.
			// This makes the attenuation react to clouds covering the
			// wider area of the sun, not just the 1-pixel centre.
			const int HALF = 10;             // 21x21 block
			int x0 = cx_px - HALF, y0 = cy_px - HALF;
			int w  = HALF * 2 + 1;
			int h  = HALF * 2 + 1;
			if (x0 < 0) { w += x0; x0 = 0; }
			if (y0 < 0) { h += y0; y0 = 0; }
			if (x0 + w > (int)vid.width)  w = (int)vid.width  - x0;
			if (y0 + h > (int)vid.height) h = (int)vid.height - y0;
			if (w > 0 && h > 0) {
				static unsigned char block[32*32*4];
				glReadBuffer(GL_BACK);
				glReadPixels(x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, block);
				unsigned long sum_r = 0, sum_g = 0, sum_b = 0;
				int count = w * h;
				for (int i = 0; i < count; ++i) {
					sum_r += block[i*4 + 0];
					sum_g += block[i*4 + 1];
					sum_b += block[i*4 + 2];
				}
				float avg_r = (float)sum_r / (float)count;
				float avg_g = (float)sum_g / (float)count;
				float avg_b = (float)sum_b / (float)count;
				float lum   = (avg_r * 0.299f + avg_g * 0.587f + avg_b * 0.114f) / 255.0f;
				// Aggressive amplification: any sky brightness above the
				// base tint pushes toward full attenuation quickly.
				float l = lum * 4.5f;
				if (l > 1.0f) l = 1.0f;
				float k = l * l * (3.0f - 2.0f * l);
				prev_attn = 1.0f - 0.90f * k;
			}
		}
	}
	float k = vis_now * prev_attn;
	float sr = r_sky_sun_r.value * r_sky_sun_strength.value * k;
	float sg = r_sky_sun_g.value * r_sky_sun_strength.value * k;
	float sb = r_sky_sun_b.value * r_sky_sun_strength.value * k;

	GLboolean blend_was = glIsEnabled(GL_BLEND);
	GLboolean dtest_was = glIsEnabled(GL_DEPTH_TEST);
	GLboolean dmask_was; glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask_was);
	GLint bs, bd; glGetIntegerv(GL_BLEND_SRC_RGB, &bs); glGetIntegerv(GL_BLEND_DST_RGB, &bd);

	GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
	if (cull_was) glDisable(GL_CULL_FACE);
	if (!blend_was) glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);   // additive
	// Plan D: depth test OFF. The sun draws unconditionally over whatever
	// is on screen. We already proved via the ray-test that sky is in
	// that direction so nothing in the way matters.
	if (dtest_was) glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);

	GLShader_Use(&R_SkySunShader);
	glUniformMatrix4fv(R_SkySunShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform3f(R_SkySunShader_u_center,    center[0], center[1], center[2]);
	glUniform3f(R_SkySunShader_u_cam_right, vright[0], vright[1], vright[2]);
	glUniform3f(R_SkySunShader_u_cam_up,    vup[0],    vup[1],    vup[2]);
	glUniform1f(R_SkySunShader_u_radius,    radius);
	glUniform3f(R_SkySunShader_u_sun_color, sr, sg, sb);
	glUniform1f(R_SkySunShader_u_halo,      8.0f);
	glUniform1f(R_SkySunShader_u_bloom,     r_sky_sun_bloom.value);

	glBindVertexArray(skysun_vao);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	glDepthMask(dmask_was);
	if (dtest_was) glEnable(GL_DEPTH_TEST);
	if (cull_was) glEnable(GL_CULL_FACE);
	glBlendFunc((GLenum)bs, (GLenum)bd);
	if (!blend_was) glDisable(GL_BLEND);
	glBindVertexArray(0);
	glUseProgram(0);
}


/*
==============
R_DrawSkyBox
==============
*/
int	skytexorder[6] = {0,2,1,3,4,5};

// Layout of the 6 skybox faces. Each face is 4 corners in TL/TR/BR/BL order
// matching the original (s,t) pairs: (0.998,0.002) (0.998,0.998) (0.002,0.998) (0.002,0.002).
// Coordinates are relative to vieworg; we add vieworg at upload time so the sky follows the camera.
static const float skybox_faces[6][4][3] = {
	// face 3: front  (skytexorder index 3 -> suf[3] "ft")
	{ { 3072, -3072,  3072}, { 3072, -3072, -3072}, { 3072,  3072, -3072}, { 3072,  3072,  3072} },
	// face 2: back   (suf[1] "bk")
	{ {-3072,  3072,  3072}, {-3072,  3072, -3072}, {-3072, -3072, -3072}, {-3072, -3072,  3072} },
	// face 0: right  (suf[0] "rt")
	{ { 3072,  3072,  3072}, { 3072,  3072, -3072}, {-3072,  3072, -3072}, {-3072,  3072,  3072} },
	// face 1: left   (suf[2] "lf")
	{ {-3072, -3072,  3072}, {-3072, -3072, -3072}, { 3072, -3072, -3072}, { 3072, -3072,  3072} },
	// face 4: up     (suf[4] "up")
	{ { 3072, -3072,  3072}, { 3072,  3072,  3072}, {-3072,  3072,  3072}, {-3072, -3072,  3072} },
	// face 5: down   (suf[5] "dn")
	{ { 3072,  3072, -3072}, { 3072, -3072, -3072}, {-3072, -3072, -3072}, {-3072,  3072, -3072} },
};

// Matches the texture index used for face i in the original code (skytexorder[3], [2], [0], [1], [4], [5]).
static const int skybox_face_tex[6] = { 3, 2, 0, 1, 4, 5 };

void R_DrawSkyBox (void)
{
	Skybox_EnsureVBO();
	if (!skybox_vbo_ready)
		return;

	float mvp[16];
	R_CurrentMVP(mvp);

	GLShader_Use(&R_SpriteShader);
	glUniformMatrix4fv(R_SpriteShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform4f       (R_SpriteShader_u_color, 1.0f, 1.0f, 1.0f, 1.0f);
	glUniform1i       (R_SpriteShader_u_tex, 0);

	DynamicVBO_Bind(&skybox_vbo);

	const float s_lo = 0.001953f, s_hi = 0.998047f;
	float vx = r_refdef.vieworg[0], vy = r_refdef.vieworg[1], vz = r_refdef.vieworg[2];

	for (int f = 0; f < 6; ++f)
	{
		const float (*c)[3] = skybox_faces[f];
		sky_vertex_t v[6];
		// corners: TL=c[0](s_hi,s_lo), TR=c[1](s_hi,s_hi), BR=c[2](s_lo,s_hi), BL=c[3](s_lo,s_lo)
		v[0] = { c[0][0]+vx, c[0][1]+vy, c[0][2]+vz, s_hi, s_lo };
		v[1] = { c[1][0]+vx, c[1][1]+vy, c[1][2]+vz, s_hi, s_hi };
		v[2] = { c[2][0]+vx, c[2][1]+vy, c[2][2]+vz, s_lo, s_hi };
		v[3] = { c[0][0]+vx, c[0][1]+vy, c[0][2]+vz, s_hi, s_lo };
		v[4] = { c[2][0]+vx, c[2][1]+vy, c[2][2]+vz, s_lo, s_hi };
		v[5] = { c[3][0]+vx, c[3][1]+vy, c[3][2]+vz, s_lo, s_lo };

		glBindTexture(GL_TEXTURE_2D, skytexture[skytexorder[skybox_face_tex[f]]]);
		DynamicVBO_Upload(&skybox_vbo, v, sizeof(v));
		Prof_CountDraw(6);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	glBindVertexArray(0);
	glUseProgram(0);
}

//===============================================================

/*
=============
R_InitSky

A sky texture is 256*128, with the right side being a masked overlay
==============
*/
void R_InitSky (byte *src, int bytesperpixel)
{
	int			i, j, p;
	unsigned	trans[128*128];
	unsigned	transpix;
	int			r, g, b;
	unsigned	*rgba;

//	if (isDedicated)
//		return;

	if (bytesperpixel == 4)
	{
		for (i = 0;i < 128;i++)
			for (j = 0;j < 128;j++)
				trans[(i*128) + j] = src[i*256+j+128];
	}
	else
	{
		// make an average value for the back to avoid
		// a fringe on the top level
		r = g = b = 0;
		for (i=0 ; i<128 ; i++)
			for (j=0 ; j<128 ; j++)
			{
				p = src[i*256 + j + 128];
				rgba = &d_8to24table[p];
				trans[(i*128) + j] = *rgba;
				r += ((byte *)rgba)[0];
				g += ((byte *)rgba)[1];
				b += ((byte *)rgba)[2];
			}

		((byte *)&transpix)[0] = r/(128*128);
		((byte *)&transpix)[1] = g/(128*128);
		((byte *)&transpix)[2] = b/(128*128);
		((byte *)&transpix)[3] = 0;
	}

	if (!solidskytexture)
	{
		GLuint id = 0;
		glGenTextures(1, &id);
		solidskytexture = (int)id;
	}
	glBindTexture (GL_TEXTURE_2D, solidskytexture);
	glTexImage2D (GL_TEXTURE_2D, 0, gl_solid_format, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, trans);
	// Blurred back layer (the distant sky). Mipmap + LOD bias + hard MIN
	// LOD floor so the GPU is forced to a small mip regardless of
	// screen-space texel ratio. Bias alone only kicks in when the
	// footprint math asks for it; MIN_LOD guarantees a minimum blur.
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 3.4f);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD,   2.55f); // ~15% less than 3.0


	if (bytesperpixel == 4)
	{
		for (i = 0;i < 128;i++)
			for (j = 0;j < 128;j++)
				trans[(i*128) + j] = src[i*256+j];
	}
	else
	{
		for (i=0 ; i<128 ; i++)
			for (j=0 ; j<128 ; j++)
			{
				p = src[i*256 + j];
				if (p == 0)
					trans[(i*128) + j] = transpix;
				else
					trans[(i*128) + j] = d_8to24table[p];
			}
	}

	if (!alphaskytexture)
	{
		GLuint id = 0;
		glGenTextures(1, &id);
		alphaskytexture = (int)id;
	}
	glBindTexture (GL_TEXTURE_2D, alphaskytexture);
	glTexImage2D (GL_TEXTURE_2D, 0, gl_alpha_format, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, trans);
	// Gentle blur on the near layer so it doesn't look pixel-crisp next to
	// the softened back layer.
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 2.0f);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD,   1.6f);
}

