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
// r_surf.c: surface-related refresh code

#include "quakedef.h"
#include "bsp_render.h"
#include "gl_render.h"
#include "gl_mat4.h"
#include "gl_profiler.h"

// Dream warp: per-vertex 3D-noise displacement in the world shaders. Controls
// the world geometry "breathing" effect. 0 = disabled. Registered from
// gl_rmisc.cpp via R_Init so it survives between levels.
cvar_t r_dream_amp = {"r_dream_amp", "0"};

// World rendering: one streaming VBO reused per surface draw. Each surface is
// a fan of N verts; we re-triangulate to N-2 triangles on upload.
typedef struct {
	float x, y, z;
	float s, t;
	float lm_s, lm_t;
} world_vtx_t;

static DynamicVBO world_vbo;
static qboolean   world_vbo_ready = false;

// Static world VBO: every opaque world + brush-entity surface pre-triangulated
// once at level load. The BSP geometry never changes, so there's no reason to
// re-upload every frame. Built by R_BuildStaticWorldVBO from R_NewMap.
static GLuint world_static_vao = 0;
static GLuint world_static_vbo = 0;
static int    world_static_ready = 0;
static int    world_static_nverts = 0;

// Batched world draws emit one glDrawArrays per (texture, lightmap) group.
// An ad_dm1-sized frame yields ~15-20k post-triangulation verts per texture
// at peak, so we size for that. 65536 keeps a safe margin.
#define WORLD_MAX_VERTS 65536

extern "C" void World_StreamFrameEnd (void)
{
	if (world_vbo_ready)
		DynamicVBO_NextFrame(&world_vbo);
}

static void World_EnsureVBO(void)
{
	if (world_vbo_ready) return;
	// Persistent-mapped ring with 3 segments. Each segment gets one frame's
	// worth of transient geometry (currently used only by the brush-entity
	// dynamic path; the static world path goes through world_static_vbo).
	// Size for WORLD_MAX_VERTS per segment.
	DynamicVBO_InitPersistent(&world_vbo,
		DYNAMIC_VBO_PERSIST_SEGMENTS * WORLD_MAX_VERTS * sizeof(world_vtx_t));
	DynamicVBO_SetAttrib(&world_vbo, 0, 3, GL_FLOAT, GL_FALSE, sizeof(world_vtx_t), offsetof(world_vtx_t, x));
	DynamicVBO_SetAttrib(&world_vbo, 1, 2, GL_FLOAT, GL_FALSE, sizeof(world_vtx_t), offsetof(world_vtx_t, s));
	DynamicVBO_SetAttrib(&world_vbo, 2, 2, GL_FLOAT, GL_FALSE, sizeof(world_vtx_t), offsetof(world_vtx_t, lm_s));
	world_vbo_ready = true;
}

GLuint		lightmap_textures[MAX_LIGHTMAPS];
int			lightmap_bytes = 4;
int			lightmap_format = GL_RGBA;

unsigned	blocklights[18*18*3]; // Tomaz - Lit Support

int			active_lightmaps;

glpoly_t	*lightmap_polys[MAX_LIGHTMAPS];
qboolean	lightmap_modified[MAX_LIGHTMAPS];
glRect_t	lightmap_rectchange[MAX_LIGHTMAPS];
byte		lightmaps[4*MAX_LIGHTMAPS*BLOCK_WIDTH*BLOCK_HEIGHT];

// Burn overlay: 4 bytes per lumel (darken, add_r, add_g, add_b).
//   darken: subtract from lumel brightness (0..255). Explosions write here.
//   add_rgb: added to the final color (0..255). Lightning blasts write here
//           to leave a bright blue glow on the wall.
// Same page/row layout as `lightmaps[]`. R_BuildLightMap applies both during
// the store step.
#define BURN_STRIDE 4
static byte burn_atlas[BURN_STRIDE*MAX_LIGHTMAPS*BLOCK_WIDTH*BLOCK_HEIGHT];

void R_BuildLightMap (msurface_t *surf, byte *dest, int stride);

void R_ClearBurnMarks (void)
{
	memset(burn_atlas, 0, sizeof(burn_atlas));
}

// Expand the dirty-rect of a lightmap page to cover a newly written
// sub-rectangle. Follows the same math as R_RenderDynamicLightmaps so
// the uploaded region stays correct.
static void Burn_DirtyPage (int page, int s0, int t0, int smax, int tmax)
{
	lightmap_modified[page] = true;
	glRect_t *rect = &lightmap_rectchange[page];
	if (t0 < rect->t) {
		if (rect->h) rect->h += rect->t - t0;
		rect->t = t0;
	}
	if (s0 < rect->l) {
		if (rect->w) rect->w += rect->l - s0;
		rect->l = s0;
	}
	if (rect->w + rect->l < s0 + smax)
		rect->w = (s0 + smax) - rect->l;
	if (rect->h + rect->t < t0 + tmax)
		rect->h = (t0 + tmax) - rect->t;
}

// Core stamp: writes both a darken amount and an additive color tint to the
// burn atlas and re-runs R_BuildLightMap for affected faces. `darken` and
// `add_rgb` are in 0..255 peak values at the stamp center; falloff is
// quadratic to 0 at the edge. Pass 0 to either to skip it.
void Burn_StampCoreEx (const vec3_t pos, float radius,
                       int darken_peak,
                       int add_r_peak, int add_g_peak, int add_b_peak)
{
	float intensity = 1.0f;   // legacy arg retained for callers below
	(void)intensity;
	if (darken_peak <= 0 && add_r_peak <= 0 && add_g_peak <= 0 && add_b_peak <= 0) return;
	if (radius <= 0.0f || !cl.worldmodel) return;

	model_t *m = cl.worldmodel;
	float r2 = radius * radius;

	for (int si = 0; si < m->numsurfaces; ++si) {
		msurface_t *s = &m->surfaces[si];
		if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) continue;
		if (!s->plane) continue;

		mplane_t *pl = s->plane;
		float d = pl->normal[0]*pos[0] + pl->normal[1]*pos[1] + pl->normal[2]*pos[2] - pl->dist;
		if (s->flags & SURF_PLANEBACK) d = -d;
		if (d < -radius || d > radius) continue;

		float n_signed = (s->flags & SURF_PLANEBACK) ? -d : d;
		vec3_t proj = {
			pos[0] - pl->normal[0] * n_signed,
			pos[1] - pl->normal[1] * n_signed,
			pos[2] - pl->normal[2] * n_signed,
		};

		mtexinfo_t *tex = s->texinfo;
		float ds = DotProduct(proj, tex->vecs[0]) + tex->vecs[0][3] - s->texturemins[0];
		float dt = DotProduct(proj, tex->vecs[1]) + tex->vecs[1][3] - s->texturemins[1];
		float ext_s = (float)s->extents[0];
		float ext_t = (float)s->extents[1];
		if (ds < -radius || ds > ext_s + radius) continue;
		if (dt < -radius || dt > ext_t + radius) continue;

		int smax = (s->extents[0] >> 4) + 1;
		int tmax = (s->extents[1] >> 4) + 1;
		float center_s = ds / 16.0f;
		float center_t = dt / 16.0f;
		float effective_r2 = r2 - d*d;
		if (effective_r2 <= 0.0f) continue;
		float eff_lr = sqrtf(effective_r2) / 16.0f;

		int lmin_s = (int)floorf(center_s - eff_lr);
		int lmax_s = (int)ceilf (center_s + eff_lr);
		int lmin_t = (int)floorf(center_t - eff_lr);
		int lmax_t = (int)ceilf (center_t + eff_lr);
		if (lmin_s < 0) lmin_s = 0;
		if (lmin_t < 0) lmin_t = 0;
		if (lmax_s > smax - 1) lmax_s = smax - 1;
		if (lmax_t > tmax - 1) lmax_t = tmax - 1;
		if (lmin_s > lmax_s || lmin_t > lmax_t) continue;

		int page = s->lightmaptexturenum;
		int page_base = page * BLOCK_WIDTH * BLOCK_HEIGHT * BURN_STRIDE;
		qboolean touched = false;
		float inv_r = 1.0f / eff_lr;

		for (int lt = lmin_t; lt <= lmax_t; ++lt) {
			for (int ls = lmin_s; ls <= lmax_s; ++ls) {
				float du = (float)ls - center_s;
				float dv = (float)lt - center_t;
				float r_norm = sqrtf(du*du + dv*dv) * inv_r;
				if (r_norm >= 1.0f) continue;
				float k = 1.0f - r_norm;
				k = k * k;   // quadratic falloff

				int off = page_base +
				          ((s->light_t + lt) * BLOCK_WIDTH + (s->light_s + ls)) * BURN_STRIDE;
				byte *bb = &burn_atlas[off];

				if (darken_peak > 0) {
					int add = (int)(darken_peak * k);
					int nv = bb[0] + add;
					if (nv > 255) nv = 255;
					if (nv > bb[0]) { bb[0] = (byte)nv; touched = true; }
				}
				if (add_r_peak > 0) {
					int add = (int)(add_r_peak * k);
					int nv = bb[1] + add;
					if (nv > 255) nv = 255;
					if (nv > bb[1]) { bb[1] = (byte)nv; touched = true; }
				}
				if (add_g_peak > 0) {
					int add = (int)(add_g_peak * k);
					int nv = bb[2] + add;
					if (nv > 255) nv = 255;
					if (nv > bb[2]) { bb[2] = (byte)nv; touched = true; }
				}
				if (add_b_peak > 0) {
					int add = (int)(add_b_peak * k);
					int nv = bb[3] + add;
					if (nv > 255) nv = 255;
					if (nv > bb[3]) { bb[3] = (byte)nv; touched = true; }
				}
			}
		}

		if (touched) {
			byte *base = lightmaps + page * lightmap_bytes * BLOCK_WIDTH * BLOCK_HEIGHT;
			base += (s->light_t * BLOCK_WIDTH + s->light_s) * lightmap_bytes;
			R_BuildLightMap(s, base, BLOCK_WIDTH * lightmap_bytes);
			Burn_DirtyPage(page, s->light_s, s->light_t, smax, tmax);
		}
	}
}

// Darkening stamp only (explosions).
void Burn_Stamp (const vec3_t pos, float radius, float intensity)
{
	if (intensity <= 0.0f) return;
	if (intensity > 1.0f) intensity = 1.0f;
	int darken = (int)(intensity * 255.0f);
	Burn_StampCoreEx(pos, radius, darken, 0, 0, 0);
}

// Colored-glow stamp (lightning and similar). `intensity` in [0, 1] scales
// the peak add_rgb values. The color is added to the lumel without any
// darkening, so the affected area turns brighter with the given tint.
void Burn_StampColored (const vec3_t pos, float radius, float intensity,
                       int r, int g, int b)
{
	if (intensity <= 0.0f) return;
	if (intensity > 1.0f) intensity = 1.0f;
	int rr = (int)(intensity * (float)r);
	int gg = (int)(intensity * (float)g);
	int bb = (int)(intensity * (float)b);
	Burn_StampCoreEx(pos, radius, 0, rr, gg, bb);
}

int			allocated[MAX_LIGHTMAPS][BLOCK_WIDTH];

msurface_t  *skychain		= NULL;
msurface_t  *waterchain		= NULL;

/*
===============
R_AddDynamicLights
===============
*/
void R_AddDynamicLights (msurface_t *surf)
{
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t	*tex;
	// Tomaz - Lit Support Begin
	int			cred, cgreen, cblue, brightness;
	unsigned	*bl;
	// Tomaz - Lit Support End

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	tex = surf->texinfo;

	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		if ( !(surf->dlightbits & (1<<lnum) ) )
			continue;		// not lit by this light

		dlight_t *dl = &cl_dlights[lnum];
		rad = dl->radius;
		dist = PlaneDiff (dl->origin, surf->plane);
		rad -= fabs(dist);
		minlight = dl->minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = dl->origin[i] - surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];

		// Tomaz - Lit Support Begin
		bl		= blocklights;
		cred	= (int)(dl->color[0] * 256.0f);
		cgreen	= (int)(dl->color[1] * 256.0f);
		cblue	= (int)(dl->color[2] * 256.0f);
		// Tomaz - Lit Support End

		// Spotlight cone setup. We need per-luxel gating because BSP
		// surfaces (especially floors) are routinely larger than the
		// cone footprint, so a single per-surface test either lights
		// the whole floor or none of it.
		//
		// Reconstruct each luxel's world position as:
		//   P(s,t) = origin_world + s_step * s  +  t_step * t
		// where s_step/t_step are texinfo's s/t axes projected onto the
		// surface plane (remove the normal component so we walk along
		// the surface, not off it) and scaled so one (s,t) unit = 16
		// world units.
		qboolean cone_on = (dl->cone_outer <= 1.0f);
		vec3_t s_step, t_step, origin_world;
		float cone_denom = dl->cone_inner - dl->cone_outer;
		if (cone_denom < 1e-4f) cone_denom = 1e-4f;
		if (cone_on) {
			// Project tex axes onto the plane.
			float *n = surf->plane->normal;
			float ds_n = DotProduct(tex->vecs[0], n);
			float dt_n = DotProduct(tex->vecs[1], n);
			vec3_t sp, tp;
			for (i = 0; i < 3; ++i) {
				sp[i] = tex->vecs[0][i] - n[i] * ds_n;
				tp[i] = tex->vecs[1][i] - n[i] * dt_n;
			}
			// texinfo's s axis produces texels-per-world-unit. After
			// projecting, |sp| ~= 1 (a unit world step along s). We
			// want one (s,t) luxel = 16 world units, so multiply by 16.
			float sp_len2 = DotProduct(sp, sp);
			float tp_len2 = DotProduct(tp, tp);
			float sp_inv = sp_len2 > 1e-8f ? 1.0f / sp_len2 : 0.0f;
			float tp_inv = tp_len2 > 1e-8f ? 1.0f / tp_len2 : 0.0f;
			// sp points in the world direction where (sp . tex->vecs[0])
			// increases by 1 per unit texel; we want world motion per
			// luxel (16 texels), so sp * 16 / |sp|^2 gives that.
			for (i = 0; i < 3; ++i) {
				s_step[i] = sp[i] * 16.0f * sp_inv;
				t_step[i] = tp[i] * 16.0f * tp_inv;
			}
			// World origin of the (s=0, t=0) luxel corner. The texinfo
			// projection puts impact at world position corresponding to
			// texel coords (local[0] + surf->texturemins[0],
			//                local[1] + surf->texturemins[1]), so walk
			// back from impact by (local[0]/16, local[1]/16) luxels.
			float s0 = local[0] / 16.0f;
			float t0 = local[1] / 16.0f;
			for (i = 0; i < 3; ++i) {
				origin_world[i] = impact[i] - s_step[i]*s0 - t_step[i]*t0;
			}
		}

		for (t = 0 ; t<tmax ; t++)
		{
			td = local[1] - t*16;
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = local[0] - s*16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
				{
					brightness = rad - dist;

					if (cone_on) {
						float px = origin_world[0] + s_step[0]*s + t_step[0]*t;
						float py = origin_world[1] + s_step[1]*s + t_step[1]*t;
						float pz = origin_world[2] + s_step[2]*s + t_step[2]*t;
						float dx = px - dl->origin[0];
						float dy = py - dl->origin[1];
						float dz = pz - dl->origin[2];
						float l2 = dx*dx + dy*dy + dz*dz;
						if (l2 > 1e-4f) {
							float inv = 1.0f / sqrtf(l2);
							float cdot = (dx*dl->cone_dir[0]
							            + dy*dl->cone_dir[1]
							            + dz*dl->cone_dir[2]) * inv;
							float k;
							if      (cdot <= dl->cone_outer) k = 0.0f;
							else if (cdot >= dl->cone_inner) k = 1.0f;
							else {
								k = (cdot - dl->cone_outer) / cone_denom;
								k = k * k * (3.0f - 2.0f * k);
							}
							brightness = (int)(brightness * k);
						} else {
							brightness = 0;
						}
					}

					bl[0] += brightness * cred;
					bl[1] += brightness * cgreen;
					bl[2] += brightness * cblue;
				}
				bl += 3;
			}
		}
	}
}

/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
// Counter read by gl_exposure.cpp to confirm the rebuild path is firing.
int R_BuildLightMap_Counter = 0;

void R_BuildLightMap (msurface_t *surf, byte *dest, int stride)
{
	R_BuildLightMap_Counter++;
	int			smax, tmax;
	int			t;
	int			i, j, size, blocksize;
	byte		*lightmap;
	unsigned	scale;
	int			maps;
	unsigned	*bl;

	surf->cached_dlight = (surf->dlightframe == r_framecount);

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax*tmax;
	blocksize = size*3;
	lightmap = surf->samples;

// set to full bright if no light data
	if (!cl.worldmodel->lightdata)
	{
		// Tomaz - Lit Support Begin
		memset (blocklights, 255, blocksize*sizeof(int));
		// Tomaz - Lit Support End
		goto store;
	}

// clear to no light
	// Tomaz - Lit Support Begin
	memset (blocklights, 0, blocksize*sizeof(int));
	// Tomaz - Lit Support End

// add all the lightmaps
	if (lightmap)
		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
			 maps++)
		{
			scale = d_lightstylevalue[surf->styles[maps]];
			surf->cached_light[maps] = scale;	// 8.8 fraction
			// Tomaz - Lit Support Begin
			bl = blocklights;
			for (i=0 ; i<size ; i++)
			{
				bl[0]		+= lightmap[0] * scale;
				bl[1]		+= lightmap[1] * scale;
				bl[2]		+= lightmap[2] * scale;

				bl			+= 3;
				lightmap	+= 3;
			}
			// Tomaz - Lit Support End
		}

// add all the dynamic lights
	if (surf->dlightframe == r_framecount)
		R_AddDynamicLights (surf);

// bound, invert, and shift
store:
	bl = blocklights;
	stride -= smax * lightmap_bytes;
	// Walk the corresponding row of burn_atlas. Burn is indexed the same way
	// as the lightmap page: (light_t + i) rows, (light_s + j) columns within
	// page surf->lightmaptexturenum.
	int burn_page_base = surf->lightmaptexturenum * BLOCK_WIDTH * BLOCK_HEIGHT * BURN_STRIDE;
	for (i=0 ; i<tmax ; i++, dest += stride)
	{
		int row_off = burn_page_base +
		              ((surf->light_t + i) * BLOCK_WIDTH + surf->light_s) * BURN_STRIDE;
		for (j=0 ; j<smax ; j++)
		{
			// Tomaz - Lit Support Begin
			t = bl[0] >> 7;if (t > 255) t = 255;dest[0] = t;
			t = bl[1] >> 7;if (t > 255) t = 255;dest[1] = t;
			t = bl[2] >> 7;if (t > 255) t = 255;dest[2] = t;
			if (lightmap_bytes > 3)	dest[3] = 255;

			// Burn overlay: darken then add colored glow. Clamped to 255.
			byte *bb = &burn_atlas[row_off + j * BURN_STRIDE];
			byte darken = bb[0];
			if (darken) {
				unsigned keep = 255u - darken;
				dest[0] = (byte)((dest[0] * keep) >> 8);
				dest[1] = (byte)((dest[1] * keep) >> 8);
				dest[2] = (byte)((dest[2] * keep) >> 8);
			}
			if (bb[1] | bb[2] | bb[3]) {
				int nr = dest[0] + bb[1]; if (nr > 255) nr = 255;
				int ng = dest[1] + bb[2]; if (ng > 255) ng = 255;
				int nb = dest[2] + bb[3]; if (nb > 255) nb = 255;
				dest[0] = (byte)nr;
				dest[1] = (byte)ng;
				dest[2] = (byte)nb;
			}

			bl		+= 3;
			dest	+= lightmap_bytes;
			// Tomaz - Lit Support End
		}
	}
}

/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation (texture_t *base)
{
	int		reletive;
	int		count;

	if (currententity->frame)
	{
		if (base->alternate_anims)
			base = base->alternate_anims;
	}
	
	if (!base->anim_total)
		return base;

	reletive = (int)(cl.time*10) % base->anim_total;

	count = 0;	
	while (base->anim_min > reletive || base->anim_max <= reletive)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("R_TextureAnimation: broken cycle");
		if (++count > 100)
			Sys_Error ("R_TextureAnimation: infinite cycle");
	}

	return base;
}


/*
=============================================================

	BRUSH MODELS

=============================================================
*/

int oldtexture;
extern	int		solidskytexture;
extern	int		alphaskytexture;
extern	float	speedscale;		// for top sky and bottom sky


/*
====================
R_DrawBrushMTex

Opaque BSP surface: diffuse * lightmap in one pass via shader (units 0 and 1).
Fence textures ({) use alpha discard in the fragment shader instead of
GL_ALPHA_TEST. Fullbright mask is additive-blended on top when present.
====================
*/
int		causticstexture[32];	// Tomaz - Underwater Caustics
void R_DrawBrushMTex (msurface_t *s)
{
	glpoly_t	*p;
	float		*v;
	int			i;
	texture_t	*t;
	glRect_t	*theRect;
	qboolean	fence;

	p = s->polys;

	t = R_TextureAnimation (s->texinfo->texture);
	fence = t->transparent;

	World_EnsureVBO();
	if (fence) { if (!R_EnsureWorldFenceShader()) return; }
	else       { if (!R_EnsureWorldOpaqueShader()) return; }

	// Upload the lightmap region that was marked dirty since last frame.
	i = s->lightmaptexturenum;
	if (lightmap_modified[i])
	{
		lightmap_modified[i] = false;
		theRect = &lightmap_rectchange[i];
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, lightmap_textures[i]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, theRect->t, BLOCK_WIDTH, theRect->h, lightmap_format, GL_UNSIGNED_BYTE, lightmaps+(i* BLOCK_HEIGHT + theRect->t) *BLOCK_WIDTH*lightmap_bytes);
		theRect->l = BLOCK_WIDTH;
		theRect->t = BLOCK_HEIGHT;
		theRect->h = 0;
		theRect->w = 0;
	}

	// Bind diffuse to unit 0 and lightmap to unit 1.
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, t->gl_texturenum);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, lightmap_textures[s->lightmaptexturenum]);
	glActiveTexture(GL_TEXTURE0);

	// Triangulate the convex polygon as a fan -> N-2 triangles.
	static world_vtx_t soup[WORLD_MAX_VERTS];
	int n = 0;
	int numverts = p->numverts;
	if (numverts > WORLD_MAX_VERTS / 3) numverts = WORLD_MAX_VERTS / 3;

	// Collect the verts once.
	static world_vtx_t fan[WORLD_MAX_VERTS];
	v = p->verts[0];
	for (int k = 0; k < numverts; ++k, v += VERTEXSIZE)
	{
		fan[k].x = v[0]; fan[k].y = v[1]; fan[k].z = v[2];
		fan[k].s = v[3]; fan[k].t = v[4];
		fan[k].lm_s = v[5]; fan[k].lm_t = v[6];
	}
	for (int k = 1; k < numverts - 1; ++k)
	{
		soup[n++] = fan[0];
		soup[n++] = fan[k];
		soup[n++] = fan[k + 1];
	}

	float mvp[16];
	R_CurrentMVP(mvp);

	GLShader *sh = fence ? &R_WorldFenceShader : &R_WorldOpaqueShader;
	GLint u_mvp = fence ? R_WorldFenceShader_u_mvp : R_WorldOpaqueShader_u_mvp;
	GLint u_tex = fence ? R_WorldFenceShader_u_tex : R_WorldOpaqueShader_u_tex;
	GLint u_lm  = fence ? R_WorldFenceShader_u_lightmap : R_WorldOpaqueShader_u_lightmap;
	GLint u_a   = fence ? R_WorldFenceShader_u_alpha : R_WorldOpaqueShader_u_alpha;

	GLShader_Use(sh);
	glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
	glUniform1i(u_tex, 0);
	glUniform1i(u_lm, 1);
	glUniform1f(u_a, 1.0f);
	glUniform1f(fence ? R_WorldFenceShader_u_dream_amp  : R_WorldOpaqueShader_u_dream_amp,  r_dream_amp.value);
	glUniform1f(fence ? R_WorldFenceShader_u_dream_time : R_WorldOpaqueShader_u_dream_time, 0.0f);

	DynamicVBO_Upload(&world_vbo, soup, (GLsizei)(n * sizeof(world_vtx_t)));
	DynamicVBO_Bind(&world_vbo);
	Prof_CountDraw(n);
	glDrawArrays(GL_TRIANGLES, 0, n);

	// Underwater caustics: brighten the surface with a cycling mask when the
	// surface is flagged SURF_UNDERWATER and the cvar is on.
	if (gl_caustics.value && (s->flags & SURF_UNDERWATER))
		EmitCausticsPolys(s);

	// Fullbright mask: additive pass over the lit surface.
	if (t->fullbrights != -1 && gl_fbr.value)
	{
		if (R_EnsureWorldFullbrightShader())
		{
			// Save/restore blend state so we never corrupt the caller's setup.
			GLboolean blend_was_on = glIsEnabled(GL_BLEND);
			GLint     blend_src, blend_dst;
			glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src);
			glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst);

			glEnable(GL_BLEND);
			glBlendFunc(GL_ONE, GL_ONE);
			// Ensure the fullbright pass hits the same pixels as the lit pass.
			glDepthFunc(GL_LEQUAL);

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, t->fullbrights);

			GLShader_Use(&R_WorldFullbrightShader);
			glUniformMatrix4fv(R_WorldFullbrightShader_u_mvp, 1, GL_FALSE, mvp);
			glUniform1i(R_WorldFullbrightShader_u_tex, 0);

			// Route fullbright fragments into the glow mask too.
			PostFX_BeginFullbrightMask();
			DynamicVBO_Bind(&world_vbo);
			Prof_CountDraw(n);
			glDrawArrays(GL_TRIANGLES, 0, n);
			PostFX_EndFullbrightMask();

			glBlendFunc((GLenum)blend_src, (GLenum)blend_dst);
			if (!blend_was_on) glDisable(GL_BLEND);
		}
	}

	// Leave GL in a clean state for the next (possibly legacy) consumer:
	// unit 0 active, unit 1 unbound, VAO/program unbound.
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(0);
	glUseProgram(0);
}


extern qboolean hl_map;

/*
================
R_DrawBrushMTexTrans
================
*/
void R_DrawBrushMTexTrans (msurface_t *s, float alpha)
{
	glpoly_t	*p;
	float		*v;
	int			i;
	texture_t	*t;
	glRect_t	*theRect;
	qboolean	fence;

	p = s->polys;
	t = R_TextureAnimation (s->texinfo->texture);
	fence = hl_map || t->transparent;

	World_EnsureVBO();
	if (fence) { if (!R_EnsureWorldFenceShader()) return; }
	else       { if (!R_EnsureWorldOpaqueShader()) return; }

	// Upload the lightmap region that was marked dirty since last frame.
	i = s->lightmaptexturenum;
	if (lightmap_modified[i])
	{
		lightmap_modified[i] = false;
		theRect = &lightmap_rectchange[i];
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, lightmap_textures[i]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, theRect->t, BLOCK_WIDTH, theRect->h, lightmap_format, GL_UNSIGNED_BYTE, lightmaps+(i* BLOCK_HEIGHT + theRect->t) *BLOCK_WIDTH*lightmap_bytes);
		theRect->l = BLOCK_WIDTH;
		theRect->t = BLOCK_HEIGHT;
		theRect->h = 0;
		theRect->w = 0;
	}

	// Bind diffuse to unit 0 and lightmap to unit 1.
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, t->gl_texturenum);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, lightmap_textures[s->lightmaptexturenum]);
	glActiveTexture(GL_TEXTURE0);

	// Triangulate the convex polygon as a fan -> N-2 triangles.
	static world_vtx_t soup[WORLD_MAX_VERTS];
	int n = 0;
	int numverts = p->numverts;
	if (numverts > WORLD_MAX_VERTS / 3) numverts = WORLD_MAX_VERTS / 3;

	static world_vtx_t fan[WORLD_MAX_VERTS];
	v = p->verts[0];
	for (int k = 0; k < numverts; ++k, v += VERTEXSIZE)
	{
		fan[k].x = v[0]; fan[k].y = v[1]; fan[k].z = v[2];
		fan[k].s = v[3]; fan[k].t = v[4];
		fan[k].lm_s = v[5]; fan[k].lm_t = v[6];
	}
	for (int k = 1; k < numverts - 1; ++k)
	{
		soup[n++] = fan[0];
		soup[n++] = fan[k];
		soup[n++] = fan[k + 1];
	}

	float mvp[16];
	R_CurrentMVP(mvp);

	GLShader *sh = fence ? &R_WorldFenceShader : &R_WorldOpaqueShader;
	GLint u_mvp = fence ? R_WorldFenceShader_u_mvp : R_WorldOpaqueShader_u_mvp;
	GLint u_tex = fence ? R_WorldFenceShader_u_tex : R_WorldOpaqueShader_u_tex;
	GLint u_lm  = fence ? R_WorldFenceShader_u_lightmap : R_WorldOpaqueShader_u_lightmap;
	GLint u_a   = fence ? R_WorldFenceShader_u_alpha : R_WorldOpaqueShader_u_alpha;

	// Save/restore blend so we never corrupt the caller's setup.
	GLboolean blend_was_on = glIsEnabled(GL_BLEND);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	GLShader_Use(sh);
	glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
	glUniform1i(u_tex, 0);
	glUniform1i(u_lm, 1);
	glUniform1f(u_a, alpha);

	DynamicVBO_Upload(&world_vbo, soup, (GLsizei)(n * sizeof(world_vtx_t)));
	DynamicVBO_Bind(&world_vbo);
	Prof_CountDraw(n);
	glDrawArrays(GL_TRIANGLES, 0, n);

	if (!blend_was_on) glDisable(GL_BLEND);

	// Leave GL clean for the next (possibly legacy) consumer.
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(0);
	glUseProgram(0);
}

/*
================
R_RenderDynamicLightmaps
Multitexture
================
*/
void R_RenderDynamicLightmaps (msurface_t *s)
{
	byte		*base;
	int			maps;
	glRect_t    *theRect;
	int			smax, tmax;

	c_brush_polys++;

	if (s->flags & ( SURF_DRAWSKY | SURF_DRAWTURB ) )
		return;
		
	s->polys->chain = lightmap_polys[s->lightmaptexturenum];
	lightmap_polys[s->lightmaptexturenum] = s->polys;

	// check for lightmap modification
	for (maps = 0; maps < MAXLIGHTMAPS && s->styles[maps] != 255; maps++)
		if (d_lightstylevalue[s->styles[maps]] != s->cached_light[maps])
			goto dynamic;

	if (s->dlightframe == r_framecount	// dynamic this frame
		|| s->cached_dlight)			// dynamic previously
	{
dynamic:
		if (r_dynamic.value)
		{
			lightmap_modified[s->lightmaptexturenum] = true;
			theRect = &lightmap_rectchange[s->lightmaptexturenum];

			if (s->light_t < theRect->t) 
			{
				if (theRect->h)
					theRect->h += theRect->t - s->light_t;

				theRect->t = s->light_t;
			}

			if (s->light_s < theRect->l) 
			{
				if (theRect->w)
					theRect->w += theRect->l - s->light_s;

				theRect->l = s->light_s;
			}

			smax = (s->extents[0]>>4)+1;
			tmax = (s->extents[1]>>4)+1;

			if ((theRect->w + theRect->l) < (s->light_s + smax))
				theRect->w = (s->light_s-theRect->l)+smax;

			if ((theRect->h + theRect->t) < (s->light_t + tmax))
				theRect->h = (s->light_t-theRect->t)+tmax;

			base = lightmaps + s->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
			base += s->light_t * BLOCK_WIDTH * lightmap_bytes + s->light_s * lightmap_bytes;

			R_BuildLightMap (s, base, BLOCK_WIDTH*lightmap_bytes);
		}
	}
}

/*
================
R_DrawWaterSurfaces

Batched water path. Groups waterchain surfaces by texture (maps can have any
number of distinct water/lava/slime/teleporter textures), triangulates every
surface into a single soup, uploads once, then emits one glDrawArrays per
texture. Mirrors what R_EmitTextureChains does for opaque world surfaces.
================
*/
// Defined in gl_warp.cpp. Same layout, C++ linkage (matches gl_warp.cpp).
typedef struct { float x, y, z; float s, t; } warp_vtx_t;
extern warp_vtx_t warp_soup[16384];
extern DynamicVBO warp_vbo;
extern void Warp_EnsureVBO(void);
extern int  Water_AppendSurface(msurface_t *s, warp_vtx_t *dst, int *n_inout, int cap);
#define WARP_MAX_VERTS 16384

typedef struct {
	int        first;
	int        count;
	texture_t *t;
} water_draw_range_t;

// Per-water-surface bitmask: which of the 4 AABB sides in tangent space are
// exterior (touching wall/floor) vs interior (shared with another water
// face). Bit order: 0=min_s (left), 1=max_s (right), 2=min_t (bottom),
// 3=max_t (top). A cleared bit means "don't darken this side".
//
// Indexed by (surf - cl.worldmodel->surfaces). Built lazily from R_NewMap.
static byte  *g_water_shore_mask = NULL;
static int    g_water_shore_mask_count = 0;

void Water_BuildShoreMasks (void)
{
	if (g_water_shore_mask) { free(g_water_shore_mask); g_water_shore_mask = NULL; }
	g_water_shore_mask_count = 0;
	if (!cl.worldmodel) return;

	model_t *m = cl.worldmodel;
	g_water_shore_mask_count = m->numsurfaces;
	g_water_shore_mask = (byte *)calloc(g_water_shore_mask_count, 1);
	// Default to all sides exterior. If we can't classify for some reason
	// we still darken everything, which is the pre-fix behavior.
	memset(g_water_shore_mask, 0x0F, g_water_shore_mask_count);

	// Build edge reference counts among water faces only. Two faces sharing
	// a BSP edge => that edge is interior to the water volume.
	int nedges = m->numedges;
	if (nedges <= 0) return;
	byte *edge_water_count = (byte *)calloc(nedges, 1);

	for (int i = 0; i < m->numsurfaces; ++i) {
		msurface_t *s = &m->surfaces[i];
		if (!(s->flags & SURF_DRAWTURB)) continue;
		for (int e = 0; e < s->numedges; ++e) {
			int se = m->surfedges[s->firstedge + e];
			int ei = se < 0 ? -se : se;
			if (ei > 0 && ei < nedges && edge_water_count[ei] < 255)
				edge_water_count[ei]++;
		}
	}

	// For each water face, classify each edge: if shared with another
	// water face AND roughly aligned with one of the 4 AABB sides in
	// tangent space, clear that side's bit in the mask.
	for (int i = 0; i < m->numsurfaces; ++i) {
		msurface_t *s = &m->surfaces[i];
		if (!(s->flags & SURF_DRAWTURB)) continue;

		mtexinfo_t *tex = s->texinfo;
		// Face AABB in tangent space.
		float tmin_s =  1e30f, tmax_s = -1e30f;
		float tmin_t =  1e30f, tmax_t = -1e30f;
		for (int e = 0; e < s->numedges; ++e) {
			int se = m->surfedges[s->firstedge + e];
			int ei = se < 0 ? -se : se;
			medge_t *ed = &m->edges[ei];
			for (int k = 0; k < 2; ++k) {
				float *v = m->vertexes[ed->v[k]].position;
				float tx = DotProduct(v, tex->vecs[0]) + tex->vecs[0][3];
				float ty = DotProduct(v, tex->vecs[1]) + tex->vecs[1][3];
				if (tx < tmin_s) tmin_s = tx; if (tx > tmax_s) tmax_s = tx;
				if (ty < tmin_t) tmin_t = ty; if (ty > tmax_t) tmax_t = ty;
			}
		}
		float range_s = tmax_s - tmin_s;
		float range_t = tmax_t - tmin_t;
		if (range_s < 1.0f) range_s = 1.0f;
		if (range_t < 1.0f) range_t = 1.0f;
		// Tolerance for "is edge aligned with a side": 5% of the AABB.
		float tol_s = range_s * 0.05f;
		float tol_t = range_t * 0.05f;

		byte mask = 0x0F;   // start all-exterior, clear shared sides
		for (int e = 0; e < s->numedges; ++e) {
			int se = m->surfedges[s->firstedge + e];
			int ei = se < 0 ? -se : se;
			if (ei <= 0 || ei >= nedges) continue;
			if (edge_water_count[ei] < 2) continue;   // not shared => exterior

			// Get the edge's two endpoints in tangent space.
			medge_t *ed = &m->edges[ei];
			float *v0 = m->vertexes[ed->v[0]].position;
			float *v1 = m->vertexes[ed->v[1]].position;
			float tx0 = DotProduct(v0, tex->vecs[0]) + tex->vecs[0][3];
			float ty0 = DotProduct(v0, tex->vecs[1]) + tex->vecs[1][3];
			float tx1 = DotProduct(v1, tex->vecs[0]) + tex->vecs[0][3];
			float ty1 = DotProduct(v1, tex->vecs[1]) + tex->vecs[1][3];

			// Vertical edge (constant S) aligned with min_s or max_s?
			if (fabsf(tx0 - tx1) < tol_s) {
				float midx = 0.5f * (tx0 + tx1);
				if (fabsf(midx - tmin_s) < tol_s) mask &= ~0x01;
				if (fabsf(midx - tmax_s) < tol_s) mask &= ~0x02;
			}
			// Horizontal edge (constant T) aligned with min_t or max_t?
			if (fabsf(ty0 - ty1) < tol_t) {
				float midy = 0.5f * (ty0 + ty1);
				if (fabsf(midy - tmin_t) < tol_t) mask &= ~0x04;
				if (fabsf(midy - tmax_t) < tol_t) mask &= ~0x08;
			}
		}
		g_water_shore_mask[i] = mask;
	}

	free(edge_water_count);
}

// Fetch the shore mask computed by Water_BuildShoreMasks. Returns 0x0F
// (all sides exterior) as a safe default if not built yet.
static byte Water_GetShoreMask (msurface_t *surf)
{
	if (!g_water_shore_mask) return 0x0F;
	ptrdiff_t idx = surf - cl.worldmodel->surfaces;
	if (idx < 0 || idx >= g_water_shore_mask_count) return 0x0F;
	return g_water_shore_mask[idx];
}

void R_DrawWaterSurfaces (void)
{
	if (!waterchain) return;

	// Restore the world modelview (brush entity transforms may have left
	// their own transform on the stack).
	mat4_t world;
	memcpy(world.m, r_world_matrix, sizeof(world.m));
	MatStack_Load(&r_modelview, &world);

	float alpha = (r_wateralpha.value < 1.0f) ? r_wateralpha.value : 1.0f;
	if (!R_EnsureWorldWaterShader()) { waterchain = NULL; return; }
	Warp_EnsureVBO();

	// ---- collect phase ----
	// One draw call per surface (needed for per-face shoreline uniforms).
	// Surfaces are grouped only to minimize diffuse texture rebinds.
	enum { MAX_WATER_SURFS = 4096 };
	static msurface_t *surfs[MAX_WATER_SURFS];
	static int         surf_first[MAX_WATER_SURFS];
	static int         surf_count[MAX_WATER_SURFS];
	int nsurfs = 0;

	for (msurface_t *s = waterchain; s; s = s->texturechain) {
		if (nsurfs >= MAX_WATER_SURFS) break;
		surfs[nsurfs++] = s;
	}
	waterchain = NULL;

	// ---- triangulate & record per-surface offsets ----
	int nverts = 0;
	for (int i = 0; i < nsurfs; ++i)
	{
		if (nverts >= WARP_MAX_VERTS - 30) { nsurfs = i; break; }
		int before = nverts;
		Water_AppendSurface(surfs[i], warp_soup, &nverts, WARP_MAX_VERTS);
		surf_first[i] = before;
		surf_count[i] = nverts - before;
	}

	if (nverts == 0) return;

	// ---- upload phase: ring-streaming (no orphan stall) ----
	Prof_BeginSection (PROF_CPU_WATER_UPLOAD);
	GLsizei byte_offset = DynamicVBO_UploadStream(&warp_vbo, warp_soup,
	                                              (GLsizei)(nverts * sizeof(warp_vtx_t)));
	GLsizei vertex_offset = byte_offset / (GLsizei)sizeof(warp_vtx_t);
	Prof_EndSection (PROF_CPU_WATER_UPLOAD);
	DynamicVBO_Bind(&warp_vbo);

	float mvp[16];
	R_CurrentMVP(mvp);

	GLboolean blend_was_on = glIsEnabled(GL_BLEND);
	if (alpha < 1.0f) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	GLShader_Use(&R_WorldWaterShader);
	glUniformMatrix4fv(R_WorldWaterShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform1i       (R_WorldWaterShader_u_tex, 0);
	glUniform1f       (R_WorldWaterShader_u_time, (float)realtime);
	glUniform1f       (R_WorldWaterShader_u_alpha, alpha);
	extern cvar_t r_water_fx;
	qboolean water_fx_on = (r_water_fx.value != 0.0f);
	glUniform1f       (R_WorldWaterShader_u_shore_width,    48.0f);
	glUniform1f       (R_WorldWaterShader_u_shore_strength, water_fx_on ? 0.6f  : 0.0f);
	glUniform1f       (R_WorldWaterShader_u_cloud_scale,    200.0f);
	glUniform1f       (R_WorldWaterShader_u_cloud_amp,      water_fx_on ? 0.8f  : 0.0f);
	glUniform1f       (R_WorldWaterShader_u_cloud_speed,    1.33f);

	glActiveTexture(GL_TEXTURE0);
	texture_t *last_tex = NULL;
	for (int i = 0; i < nsurfs; ++i)
	{
		if (surf_count[i] == 0) continue;
		msurface_t *s = surfs[i];

		// Rebind diffuse only on change.
		texture_t *t = s->texinfo->texture;
		if (t != last_tex) {
			glBindTexture(GL_TEXTURE_2D, t->gl_texturenum);
			last_tex = t;
		}

		// Face AABB in tangent (texinfo) space.
		mtexinfo_t *tex = s->texinfo;
		float tmin_s =  1e30f, tmax_s = -1e30f;
		float tmin_t =  1e30f, tmax_t = -1e30f;
		for (glpoly_t *p = s->polys; p; p = p->next) {
			float *v = p->verts[0];
			for (int k = 0; k < p->numverts; ++k, v += VERTEXSIZE) {
				float tx = DotProduct(v, tex->vecs[0]) + tex->vecs[0][3];
				float ty = DotProduct(v, tex->vecs[1]) + tex->vecs[1][3];
				if (tx < tmin_s) tmin_s = tx; if (tx > tmax_s) tmax_s = tx;
				if (ty < tmin_t) tmin_t = ty; if (ty > tmax_t) tmax_t = ty;
			}
		}

		byte mask = Water_GetShoreMask(s);
		float ext_min_s = (mask & 0x01) ? 1.0f : 0.0f;
		float ext_max_s = (mask & 0x02) ? 1.0f : 0.0f;
		float ext_min_t = (mask & 0x04) ? 1.0f : 0.0f;
		float ext_max_t = (mask & 0x08) ? 1.0f : 0.0f;

		glUniform2f(R_WorldWaterShader_u_tc_min, tmin_s, tmin_t);
		glUniform2f(R_WorldWaterShader_u_tc_max, tmax_s, tmax_t);
		glUniform4f(R_WorldWaterShader_u_shore_ext,
		            ext_min_s, ext_max_s, ext_min_t, ext_max_t);

		Prof_CountDraw(surf_count[i]);
		glDrawArrays(GL_TRIANGLES, vertex_offset + surf_first[i], surf_count[i]);
	}

	if (!blend_was_on && alpha < 1.0f) glDisable(GL_BLEND);
	glBindVertexArray(0);
	glUseProgram(0);
}

float	r_world_matrix[16];

static void R_EmitTextureChains (model_t *texmodel); // forward decl; def below

/*
=================
R_SetupBrushPolys
=================
*/
void R_SetupBrushPolys (entity_t *e)
{
	int			k;
	vec3_t		mins, maxs;
	int			i;
	msurface_t	*psurf;
	float		dot;
	mplane_t	*pplane;
	model_t		*clmodel;
	qboolean	rotated;

	clmodel = e->model;

	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		rotated = true;
		for (i=0 ; i<3 ; i++)
		{
			mins[i] = e->origin[i] - clmodel->radius;
			maxs[i] = e->origin[i] + clmodel->radius;
		}
	}
	else
	{
		rotated = false;
		VectorAdd (e->origin, clmodel->mins, mins);
		VectorAdd (e->origin, clmodel->maxs, maxs);
	}

	if (R_CullBox (mins, maxs))
		return;

	memset (lightmap_polys, 0, sizeof(lightmap_polys));

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg);

	if (rotated)
	{
		vec3_t	temp;
		vec3_t	forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

// calculate dynamic lighting for bmodel if it's not an
// instanced model
	if (clmodel->firstmodelsurface != 0)
	{
		for (k=0 ; k<MAX_DLIGHTS ; k++)
		{
			if ((cl_dlights[k].die < cl.time) ||
				(!cl_dlights[k].radius))
				continue;

			R_MarkLightsNoVis (&cl_dlights[k], 1<<k, clmodel->nodes + clmodel->hulls[0].firstclipnode);
		}
	}

	MatStack_Push(&r_modelview);

	MatStack_MulTranslate(&r_modelview, e->origin[0], e->origin[1], e->origin[2]);
	MatStack_MulRotate(&r_modelview, e->angles[1], 0, 0, 1);
	MatStack_MulRotate(&r_modelview, e->angles[0], 0, 1, 0);	// stupid quake bug
	MatStack_MulRotate(&r_modelview, e->angles[2], 1, 0, 0);

	//
	// draw texture
	//
	// Opaque surfaces are chained onto their texture's texturechain and
	// emitted in one pass by R_EmitTextureChains below -- same path as the
	// world model, which cuts brush-entity draws from ~1 per surface to
	// ~1 per (texture, lightmap).
	//
	// Surfaces on alpha-blended entities or fence textures still go through
	// the per-surface R_DrawBrushMTexTrans path, because they need blend
	// state the opaque shader can't provide. Those are usually few.
	qboolean entity_alpha = (e->alpha != 1);
	for (i=0 ; i<clmodel->nummodelsurfaces ; i++, psurf++)
	{
	// find which side of the node we are on
		pplane = psurf->plane;

		dot = PlaneDiff (modelorg, pplane);

	// draw the polygon
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			R_RenderDynamicLightmaps(psurf);

			if (entity_alpha || psurf->texinfo->texture->transparent)
			{
				R_DrawBrushMTexTrans (psurf, e->alpha);
			}
			else
			{
				// Chain onto the (animated) texture for batched emit below.
				texture_t *t = R_TextureAnimation (psurf->texinfo->texture);
				psurf->texturechain = t->texturechain;
				t->texturechain = psurf;
			}
		}
	}

	// Emit batched opaque draws for this entity, then pop its transform.
	// R_EmitTextureChains reads MVP from the matrix stack, so the entity's
	// translation/rotation we just pushed is applied to the draws.
	// External brush models (b_shell0.bsp etc.) have their own textures[]
	// arrays -- we iterate those so their chains actually get drawn.
	R_EmitTextureChains (clmodel);

	MatStack_Pop(&r_modelview);
}

/*
=============================================================

	WORLD MODEL

=============================================================
*/

/*
================
R_RecursiveWorldNode
================
*/
void R_RecursiveWorldNode (mnode_t *node)
{
	int			c, side;
	mplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	double		dot;

	if (node->contents == CONTENTS_SOLID)
		return;

	if (node->visframe != r_visframecount)
		return;
	if (R_CullBox (node->mins, node->maxs))
		return;

// if a leaf node, draw stuff
	if (node->contents < 0)
	{
		pleaf = (mleaf_t *)node;

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				(*mark)->visframe = r_framecount;
				mark++;
			} while (--c);
		}

	// deal with model fragments in this leaf
		if (pleaf->efrags)
			R_StoreEfrags (&pleaf->efrags);

		return;
	}

// node is just a decision point, so go down the apropriate sides

// find which side of the node we are on
	plane = node->plane;
	dot = PlaneDiff (modelorg, plane);

	if (dot >= 0)
		side = 0;
	else
		side = 1;

// recurse down the children, front side first
	R_RecursiveWorldNode (node->children[side]);

// draw stuff
	c = node->numsurfaces;

	if (c)
	{
		surf = cl.worldmodel->surfaces + node->firstsurface;

		if (dot < 0 -BACKFACE_EPSILON)
			side = SURF_PLANEBACK;
		else if (dot > BACKFACE_EPSILON)
			side = 0;
		{
			for ( ; c ; c--, surf++)
			{
				if (surf->visframe != r_framecount)
					continue;

				// don't backface underwater surfaces, because they warp
				if (!(surf->flags & SURF_UNDERWATER) && ( (dot < 0) ^ !!(surf->flags & SURF_PLANEBACK)) )
					continue;		// wrong side

				if (surf->flags & SURF_DRAWSKY)
				{
					surf->texturechain = skychain;
					skychain = surf;
				}
				else if (surf->flags & SURF_DRAWTURB)
				{
					surf->texturechain = waterchain;
					waterchain = surf;
				}
				else
				{
					// Batched path: chain the surface onto its (possibly animated)
					// texture. A single draw per texture is emitted after the walk.
					texture_t *t = R_TextureAnimation (surf->texinfo->texture);
					surf->texturechain = t->texturechain;
					t->texturechain = surf;
				}

				R_RenderDynamicLightmaps(surf);
			}
		}
	}

// recurse down the back side
	R_RecursiveWorldNode (node->children[!side]);
}

extern char skyname[];

// ---------------------------------------------------------------------------
// Batched world draw. After R_RecursiveWorldNode has chained every visible
// opaque surface onto its texture_t::texturechain, we walk texture by texture
// and emit one draw per (texture, lightmap) group.
//
// Why batch by lightmap page too: every surface stores lightmap data in one of
// MAX_LIGHTMAPS (=512) 128x128 pages. The shader binds the lightmap to unit 1
// and the diffuse to unit 0. Different lightmaps => different binding => can't
// share a draw. In practice each texture covers a handful of lightmap pages.
//
// The heavy lifting -- lightmap upload, fullbright pass, caustics -- is reused
// from the per-surface path but invoked at batch granularity.
// ---------------------------------------------------------------------------

static int surf_triangulate (msurface_t *s, world_vtx_t *out)
{
	glpoly_t *p = s->polys;
	int numverts = p->numverts;
	if (numverts > WORLD_MAX_VERTS / 3) numverts = WORLD_MAX_VERTS / 3;
	if (numverts < 3) return 0;

	static world_vtx_t fan[WORLD_MAX_VERTS];
	float *v = p->verts[0];
	for (int k = 0; k < numverts; ++k, v += VERTEXSIZE)
	{
		fan[k].x = v[0]; fan[k].y = v[1]; fan[k].z = v[2];
		fan[k].s = v[3]; fan[k].t = v[4];
		fan[k].lm_s = v[5]; fan[k].lm_t = v[6];
	}

	int n = 0;
	for (int k = 1; k < numverts - 1; ++k)
	{
		out[n++] = fan[0];
		out[n++] = fan[k];
		out[n++] = fan[k + 1];
	}
	return n;
}

static void upload_lightmap_if_dirty (int lmidx)
{
	if (!lightmap_modified[lmidx]) return;
	lightmap_modified[lmidx] = false;
	glRect_t *theRect = &lightmap_rectchange[lmidx];
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, lightmap_textures[lmidx]);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, theRect->t, BLOCK_WIDTH, theRect->h,
		lightmap_format, GL_UNSIGNED_BYTE,
		lightmaps+(lmidx * BLOCK_HEIGHT + theRect->t) * BLOCK_WIDTH * lightmap_bytes);
	theRect->l = BLOCK_WIDTH;
	theRect->t = BLOCK_HEIGHT;
	theRect->h = 0;
	theRect->w = 0;
}

// qsort comparator for an array of msurface_t* by lightmaptexturenum
static int cmp_surf_by_lm (const void *a, const void *b)
{
	msurface_t *sa = *(msurface_t * const *)a;
	msurface_t *sb = *(msurface_t * const *)b;
	return sa->lightmaptexturenum - sb->lightmaptexturenum;
}

// Per-draw range, refers to a subrange of the single world-soup upload.
typedef struct {
	int        first;        // first vertex in world_soup
	int        count;        // triangle vertex count (multiple of 3)
	int        lightmap;     // lightmap page to bind
	texture_t *t;            // diffuse texture + shader selection
} world_draw_range_t;

// ---------------------------------------------------------------------------
// R_BuildStaticWorldVBO -- called from R_NewMap. Pre-triangulates every opaque
// surface of the worldmodel (including brush-entity submodels) and uploads a
// single GL_STATIC_DRAW buffer. Each msurface stores its vertex range in
// vbo_first/vbo_count so the render path just emits glDrawArrays -- no per-
// frame upload, no per-frame triangulation.
//
// Sky and SURF_DRAWTURB (water/lava) are skipped: they have their own paths.
// ---------------------------------------------------------------------------
static void R_BuildStaticWorldVBO (void)
{
	if (!cl.worldmodel) return;

	// Dispose any previous buffer (level reload).
	if (world_static_vbo) { glDeleteBuffers(1, &world_static_vbo); world_static_vbo = 0; }
	if (world_static_vao) { glDeleteVertexArrays(1, &world_static_vao); world_static_vao = 0; }
	world_static_ready = 0;
	world_static_nverts = 0;

	// Enumerate every distinct brush-model surface array we need to batch.
	// - The worldmodel owns the master array; its own inline submodels (`*N`)
	//   point into it, so we skip those to avoid double-counting.
	// - External BSPs (b_shell0.bsp, b_nail0.bsp, b_explob.bsp, ...) have
	//   their own surfaces[] arrays. Without this, their surfaces get
	//   vbo_count=0 and R_EmitTextureChains silently drops them -- that is
	//   what made ammo/health boxes invisible.
	model_t *models[MAX_MODELS];
	int      num_models = 0;
	models[num_models++] = cl.worldmodel;
	for (int j = 1; j < MAX_MODELS; ++j) {
		model_t *mp = cl.model_precache[j];
		if (!mp) break;
		if (mp == cl.worldmodel) continue;
		if (mp->type != mod_brush) continue;
		if (mp->name[0] == '*') continue; // inline submodel: shares worldmodel surfaces
		models[num_models++] = mp;
	}

	// Count total triangulated verts needed, mark non-batched surfaces.
	int total_verts = 0;
	int batched_surfs = 0;
	for (int mi = 0; mi < num_models; ++mi)
	{
		model_t *m = models[mi];
		for (int i = 0; i < m->numsurfaces; ++i)
		{
			msurface_t *s = &m->surfaces[i];
			s->vbo_first = 0;
			s->vbo_count = 0;
			if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) continue;
			glpoly_t *p = s->polys;
			if (!p) continue;
			int nv = p->numverts;
			if (nv < 3) continue;
			total_verts += (nv - 2) * 3;
			batched_surfs++;
		}
	}

	if (total_verts == 0) {
		Con_Printf("R_BuildStaticWorldVBO: no surfaces to batch\n");
		return;
	}

	world_vtx_t *soup = (world_vtx_t *)malloc(total_verts * sizeof(world_vtx_t));
	if (!soup) {
		Con_Printf("R_BuildStaticWorldVBO: malloc(%d bytes) failed\n",
		           (int)(total_verts * sizeof(world_vtx_t)));
		return;
	}

	int cursor = 0;
	for (int mi = 0; mi < num_models; ++mi)
	{
		model_t *m = models[mi];
		for (int i = 0; i < m->numsurfaces; ++i)
		{
			msurface_t *s = &m->surfaces[i];
			if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) continue;
			glpoly_t *p = s->polys;
			if (!p) continue;
			int nv = p->numverts;
			if (nv < 3) continue;

			// Build the fan.
			world_vtx_t fan[256];
			int take = (nv > 256) ? 256 : nv;
			float *v = p->verts[0];
			for (int k = 0; k < take; ++k, v += VERTEXSIZE) {
				fan[k].x = v[0]; fan[k].y = v[1]; fan[k].z = v[2];
				fan[k].s = v[3]; fan[k].t = v[4];
				fan[k].lm_s = v[5]; fan[k].lm_t = v[6];
			}

			s->vbo_first = cursor;
			for (int k = 1; k < take - 1; ++k) {
				soup[cursor++] = fan[0];
				soup[cursor++] = fan[k];
				soup[cursor++] = fan[k + 1];
			}
			s->vbo_count = cursor - s->vbo_first;
		}
	}

	// Upload once.
	glGenVertexArrays(1, &world_static_vao);
	glGenBuffers(1, &world_static_vbo);
	glBindVertexArray(world_static_vao);
	glBindBuffer(GL_ARRAY_BUFFER, world_static_vbo);
	glBufferData(GL_ARRAY_BUFFER, cursor * sizeof(world_vtx_t), soup, GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(world_vtx_t),
	                      (const void *)offsetof(world_vtx_t, x));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(world_vtx_t),
	                      (const void *)offsetof(world_vtx_t, s));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(world_vtx_t),
	                      (const void *)offsetof(world_vtx_t, lm_s));
	glBindVertexArray(0);

	free(soup);
	world_static_nverts = cursor;
	world_static_ready = 1;
	Con_Printf("R_BuildStaticWorldVBO: %d verts, %d surfaces, %.2f MB\n",
	           cursor, batched_surfs,
	           cursor * sizeof(world_vtx_t) / (1024.0f * 1024.0f));
}

// Public hook called from R_NewMap.
void R_OnNewMap_BuildWorldVBO (void)
{
	R_BuildStaticWorldVBO();
}

static void R_EmitTextureChains (model_t *texmodel)
{
	if (!cl.worldmodel) return;
	if (!world_static_ready) return; // R_NewMap didn't run yet; nothing to draw
	if (!texmodel) texmodel = cl.worldmodel;

	// Walk each texture's chain, group surfaces by lightmap page, and emit a
	// glMultiDrawArrays per (texture, lightmap) group against the static VBO.
	// No uploads, no triangulation, no CPU soup copies. Each surface keeps
	// its own (vbo_first, vbo_count) range from R_BuildStaticWorldVBO.

	glBindVertexArray(world_static_vao);

	enum { SURF_BATCH_MAX = 8192 };
	static msurface_t *surfs[SURF_BATCH_MAX];
	static GLint      mda_firsts[SURF_BATCH_MAX];
	static GLsizei    mda_counts[SURF_BATCH_MAX];

	float mvp[16];
	R_CurrentMVP(mvp);

	texture_t *last_t            = NULL;
	int        last_shader_kind  = -1; // 0=opaque, 1=fence, 2=grass
	int        last_lm           = -1;

	// Remember ranges we'll re-emit for fullbright in a second pass.
	struct fb_batch_t { texture_t *t; int n; };
	static struct fb_batch_t fb_batches[1024];
	static GLint   fb_firsts[SURF_BATCH_MAX];
	static GLsizei fb_counts[SURF_BATCH_MAX];
	int fb_batch_count = 0;
	int fb_draw_count  = 0;

	for (int tnum = 0; tnum < texmodel->numtextures; ++tnum)
	{
		texture_t *base = texmodel->textures[tnum];
		if (!base || !base->texturechain) continue;
		texture_t *t = base;

		int nsurfs = 0;
		for (msurface_t *s = t->texturechain; s && nsurfs < SURF_BATCH_MAX; s = s->texturechain)
			surfs[nsurfs++] = s;
		t->texturechain = NULL;
		if (nsurfs == 0) continue;
		qsort(surfs, nsurfs, sizeof(surfs[0]), cmp_surf_by_lm);

		qboolean fence    = t->transparent != 0;
		qboolean grass    = t->grass != 0 && !fence; // grass path doesn't apply to fence textures
		qboolean wants_fb = (t->fullbrights != -1 && gl_fbr.value);
		int fb_this_tex_start = fb_draw_count;

		// Shader/texture bind: only when the texture OR the shader kind changes.
		// Three kinds share the same world vertex layout so switching between
		// them is just a shader bind + uniform setup, no VAO change.
		int shader_kind = grass ? 2 : (fence ? 1 : 0);
		if (t != last_t || shader_kind != last_shader_kind)
		{
			GLShader *sh;
			GLint u_mvp, u_tex, u_lm, u_a;
			if (grass) {
				if (!R_EnsureWorldGrassShader()) continue;
				sh    = &R_WorldGrassShader;
				u_mvp = R_WorldGrassShader_u_mvp;
				u_tex = R_WorldGrassShader_u_tex;
				u_lm  = R_WorldGrassShader_u_lightmap;
				u_a   = R_WorldGrassShader_u_alpha;
			} else if (fence) {
				if (!R_EnsureWorldFenceShader()) continue;
				sh    = &R_WorldFenceShader;
				u_mvp = R_WorldFenceShader_u_mvp;
				u_tex = R_WorldFenceShader_u_tex;
				u_lm  = R_WorldFenceShader_u_lightmap;
				u_a   = R_WorldFenceShader_u_alpha;
			} else {
				if (!R_EnsureWorldOpaqueShader()) continue;
				sh    = &R_WorldOpaqueShader;
				u_mvp = R_WorldOpaqueShader_u_mvp;
				u_tex = R_WorldOpaqueShader_u_tex;
				u_lm  = R_WorldOpaqueShader_u_lightmap;
				u_a   = R_WorldOpaqueShader_u_alpha;
			}
			GLShader_Use(sh);
			glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
			glUniform1i(u_tex, 0);
			glUniform1i(u_lm, 1);
			glUniform1f(u_a, 1.0f);
			// Dream warp uniforms. Each of the three world shaders
			// (opaque/fence/grass) has its own pair; set them per the
			// currently-selected shader_kind.
			GLint u_damp  = grass ? R_WorldGrassShader_u_dream_amp
			              : fence ? R_WorldFenceShader_u_dream_amp
			                      : R_WorldOpaqueShader_u_dream_amp;
			GLint u_dtime = grass ? R_WorldGrassShader_u_dream_time
			              : fence ? R_WorldFenceShader_u_dream_time
			                      : R_WorldOpaqueShader_u_dream_time;
			glUniform1f(u_damp,  r_dream_amp.value);
			glUniform1f(u_dtime, 0.0f);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, t->gl_texturenum);
			last_t = t;
			last_shader_kind = shader_kind;
			last_lm = -1;
		}
		else
		{
			// Shader/texture unchanged (animated textures sharing a base);
			// still must ensure the diffuse is bound for this frame.
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, t->gl_texturenum);
		}

		int gstart = 0;
		while (gstart < nsurfs)
		{
			int lm = surfs[gstart]->lightmaptexturenum;
			int gend = gstart + 1;
			while (gend < nsurfs && surfs[gend]->lightmaptexturenum == lm) gend++;

			if (lm != last_lm)
			{
				upload_lightmap_if_dirty(lm);
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, lightmap_textures[lm]);
				glActiveTexture(GL_TEXTURE0);
				last_lm = lm;
			}

			// Build the firsts[]/counts[] arrays for this (texture, lightmap)
			// group and emit ONE glMultiDrawArrays -- single driver call for
			// N surfaces.
			int n = 0;
			int total_verts = 0;
			for (int i = gstart; i < gend && n < SURF_BATCH_MAX; ++i)
			{
				msurface_t *s = surfs[i];
				if (s->vbo_count <= 0) continue; // not batched (shouldn't happen for world opaque)
				mda_firsts[n] = s->vbo_first;
				mda_counts[n] = s->vbo_count;
				n++;
				total_verts += s->vbo_count;

				if (wants_fb && fb_draw_count < SURF_BATCH_MAX) {
					fb_firsts[fb_draw_count] = s->vbo_first;
					fb_counts[fb_draw_count] = s->vbo_count;
					fb_draw_count++;
				}
			}
			if (n > 0)
			{
				Prof_CountDraw(total_verts);
				glMultiDrawArrays(GL_TRIANGLES, mda_firsts, mda_counts, n);
			}
			gstart = gend;
		}

		if (wants_fb && fb_draw_count > fb_this_tex_start && fb_batch_count < 1024)
		{
			fb_batches[fb_batch_count].t = t;
			fb_batches[fb_batch_count].n = fb_draw_count - fb_this_tex_start;
			fb_batch_count++;
		}

		// Caustics still per-surface: unusual path, very few surfaces normally.
		if (gl_caustics.value)
		{
			for (int i = 0; i < nsurfs; ++i)
				if (surfs[i]->flags & SURF_UNDERWATER)
					EmitCausticsPolys(surfs[i]);
		}
	}

	// ---- fullbright pass ----
	if (fb_batch_count > 0 && R_EnsureWorldFullbrightShader())
	{
		glBindVertexArray(world_static_vao);

		GLboolean blend_was_on = glIsEnabled(GL_BLEND);
		GLint     blend_src, blend_dst;
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst);

		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
		glDepthFunc(GL_LEQUAL);
		GLShader_Use(&R_WorldFullbrightShader);
		glUniformMatrix4fv(R_WorldFullbrightShader_u_mvp, 1, GL_FALSE, mvp);
		glUniform1i(R_WorldFullbrightShader_u_tex, 0);

		PostFX_BeginFullbrightMask();
		int draw_cursor = 0;
		for (int b = 0; b < fb_batch_count; ++b)
		{
			texture_t *t = fb_batches[b].t;
			int n = fb_batches[b].n;
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, t->fullbrights);
			int total_verts = 0;
			for (int i = 0; i < n; ++i) total_verts += fb_counts[draw_cursor + i];
			Prof_CountDraw(total_verts);
			glMultiDrawArrays(GL_TRIANGLES, fb_firsts + draw_cursor, fb_counts + draw_cursor, n);
			draw_cursor += n;
		}
		PostFX_EndFullbrightMask();

		glBlendFunc((GLenum)blend_src, (GLenum)blend_dst);
		if (!blend_was_on) glDisable(GL_BLEND);
	}

	glBindVertexArray(0);
}

/*
=============
R_DrawWorld
=============
*/

void R_DrawWorld (void)
{
	entity_t	ent;

	Prof_BeginSection (PROF_WORLD_OPAQUE);

	memset (&ent, 0, sizeof(ent));
	ent.model = cl.worldmodel;

	VectorCopy (r_refdef.vieworg, modelorg);

	currententity = &ent;

	memset (lightmap_polys, 0, sizeof(lightmap_polys));

	R_RecursiveWorldNode (cl.worldmodel->nodes);

	// Emit one draw per (texture, lightmap) group, batching the many per-
	// surface draws the old path produced. This is the whole point of the
	// batching refactor.
	R_EmitTextureChains (cl.worldmodel);

	// Grass blades (iteration 2). World-only: must not run inside brush-
	// entity draws, or each visible brush entity would re-emit the full
	// blade VBO under its transform.
	Grass_DrawBlades();

	if (skychain)
	{
		if (skyname[0])
		{
			R_DrawSkyBox ();
		}
		else
		{
			R_DrawSky(skychain);
		}
		skychain = NULL;
	}

	Prof_EndSection (PROF_WORLD_OPAQUE);
}


/*
===============
R_MarkLeaves
===============
*/
void R_MarkLeaves (void)
{
	byte	*vis;
	mnode_t	*node;
	int		i;
	byte	solid[4096];

	if (r_oldviewleaf == r_viewleaf && !r_novis.value)
		return;

	r_visframecount++;
	r_oldviewleaf = r_viewleaf;

	if (r_novis.value)
	{
		vis = solid;
		memset (solid, 0xff, (cl.worldmodel->numleafs+7)>>3);
	}
	else
		vis = Mod_LeafPVS (r_viewleaf, cl.worldmodel);
		
	for (i=0 ; i<cl.worldmodel->numleafs ; i++)
	{
		if (vis[i>>3] & (1<<(i&7)))
		{
			node = (mnode_t *)&cl.worldmodel->leafs[i+1];
			do
			{
				if (node->visframe == r_visframecount)
					break;
				node->visframe = r_visframecount;
				node = node->parent;
			} while (node);
		}
	}
}



/*
=============================================================================

  LIGHTMAP ALLOCATION

=============================================================================
*/

// returns a texture number and the position inside it
int AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;
	int		texnum;

	for (texnum=0 ; texnum<MAX_LIGHTMAPS ; texnum++)
	{
		best = BLOCK_HEIGHT;

		for (i=0 ; i<BLOCK_WIDTH-w ; i++)
		{
			best2 = 0;

			for (j=0 ; j<w ; j++)
			{
				if (allocated[texnum][i+j] >= best)
					break;
				if (allocated[texnum][i+j] > best2)
					best2 = allocated[texnum][i+j];
			}
			if (j == w)
			{	// this is a valid spot
				*x = i;
				*y = best = best2;
			}
		}

		if (best + h > BLOCK_HEIGHT)
			continue;

		for (i=0 ; i<w ; i++)
			allocated[texnum][*x + i] = best + h;

		return texnum;
	}

	Host_Error ("AllocBlock: full, %i unable to find room for %i by %i lightmap",texnum, w, h);
	return 0;
}


mvertex_t	*r_pcurrentvertbase;
model_t		*currentmodel;

int	nColinElim;

/*
================
BuildSurfaceDisplayList
================
*/
void BuildSurfaceDisplayList (msurface_t *fa)
{
	int			i, lindex, lnumverts;
	medge_t		*pedges, *r_pedge;
	int			vertpage;
	float		*vec;
	float		s, t;
	glpoly_t	*poly;

// reconstruct the polygon
	fa->visframe = 0;
	pedges = currentmodel->edges;
	lnumverts = fa->numedges;
	vertpage = 0;

	//
	// draw texture
	//
	poly = (glpoly_t *)Hunk_Alloc (sizeof(glpoly_t) + (lnumverts-4) * VERTEXSIZE*sizeof(float));
	poly->next = fa->polys;
	poly->flags = fa->flags;
	fa->polys = poly;
	poly->numverts = lnumverts;

	for (i=0 ; i<lnumverts ; i++)
	{
		lindex = currentmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			vec = r_pcurrentvertbase[r_pedge->v[0]].position;
		}
		else
		{
			r_pedge = &pedges[-lindex];
			vec = r_pcurrentvertbase[r_pedge->v[1]].position;
		}
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s /= fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t /= fa->texinfo->texture->height;

		VectorCopy (vec, poly->verts[i]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;

		//
		// lightmap texture coordinates
		//
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += fa->light_s*16;
		s += 8;
		s /= BLOCK_WIDTH*16; //fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += fa->light_t*16;
		t += 8;
		t /= BLOCK_HEIGHT*16; //fa->texinfo->texture->height;

		poly->verts[i][5] = s;
		poly->verts[i][6] = t;
	}

	//
	// remove co-linear points - Ed
	//
	if (!gl_keeptjunctions.value && !(fa->flags & SURF_UNDERWATER) )
	{
		for (i = 0 ; i < lnumverts ; ++i)
		{
			vec3_t v1, v2;
			float *prev, *cur, *next;

			prev = poly->verts[(i + lnumverts - 1) % lnumverts];
			cur = poly->verts[i];
			next = poly->verts[(i + 1) % lnumverts];

			VectorSubtract( cur, prev, v1 );
			VectorNormalize( v1 );
			VectorSubtract( next, prev, v2 );
			VectorNormalize( v2 );

			// skip co-linear points
			#define COLINEAR_EPSILON 0.001
			if ((fabs( v1[0] - v2[0] ) <= COLINEAR_EPSILON) &&
				(fabs( v1[1] - v2[1] ) <= COLINEAR_EPSILON) && 
				(fabs( v1[2] - v2[2] ) <= COLINEAR_EPSILON))
			{
				int j;
				for (j = i + 1; j < lnumverts; ++j)
				{
					int k;
					for (k = 0; k < VERTEXSIZE; ++k)
						poly->verts[j - 1][k] = poly->verts[j][k];
				}
				--lnumverts;
				++nColinElim;
				// retry next vertex next time, which is now current vertex
				--i;
			}
		}
	}
	poly->numverts = lnumverts;

}

/*
========================
GL_CreateSurfaceLightmap
========================
*/
void GL_CreateSurfaceLightmap (msurface_t *surf)
{
	int		smax, tmax;
	byte	*base;

	if (surf->flags & (SURF_DRAWSKY|SURF_DRAWTURB))
		return;

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;

	surf->lightmaptexturenum = AllocBlock (smax, tmax, &surf->light_s, &surf->light_t);
	base = lightmaps + surf->lightmaptexturenum*lightmap_bytes*BLOCK_WIDTH*BLOCK_HEIGHT;
	base += (surf->light_t * BLOCK_WIDTH + surf->light_s) * lightmap_bytes;
	R_BuildLightMap (surf, base, BLOCK_WIDTH*lightmap_bytes);
}


/*
==================
GL_BuildLightmaps

Builds the lightmap texture
with all the surfaces from all brush models
==================
*/
void GL_BuildLightmaps (void)
{
	int		i, j;
	model_t	*m;

	memset (allocated, 0, sizeof(allocated));

	r_framecount = 1;		// no dlightcache

	if (!lightmap_textures[0])
		glGenTextures(MAX_LIGHTMAPS, lightmap_textures);

	for (j=1 ; j<MAX_MODELS ; j++)
	{
		m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			continue;
		r_pcurrentvertbase = m->vertexes;
		currentmodel = m;
		for (i=0 ; i<m->numsurfaces ; i++)
		{
			GL_CreateSurfaceLightmap (m->surfaces + i);
			if ( m->surfaces[i].flags & SURF_DRAWTURB )
				continue;
			if ( m->surfaces[i].flags & SURF_DRAWSKY )
				continue;
			BuildSurfaceDisplayList (m->surfaces + i);
		}
	}
	// Lightmaps live on texture unit 1 (unit 0 is diffuse). Switch to unit 1
	// so the binds/uploads below target the right slot.
	glActiveTexture(GL_TEXTURE1);

	//
	// upload all lightmaps that were filled
	//
	for (i=0 ; i<MAX_LIGHTMAPS ; i++)
	{
		if (!allocated[i][0])
			break;		// no more used
		lightmap_modified[i] = false;
		lightmap_rectchange[i].l = BLOCK_WIDTH;
		lightmap_rectchange[i].t = BLOCK_HEIGHT;
		lightmap_rectchange[i].w = 0;
		lightmap_rectchange[i].h = 0;
		glBindTexture (GL_TEXTURE_2D, lightmap_textures[i]);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		// Core profile rejects the legacy numeric internal format (was 4);
		// GL_RGBA8 is the modern sized format matching lightmap_format=GL_RGBA.
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8,
			BLOCK_WIDTH, BLOCK_HEIGHT, 0, lightmap_format, GL_UNSIGNED_BYTE, lightmaps+i*BLOCK_WIDTH*BLOCK_HEIGHT*lightmap_bytes);
	}

	glActiveTexture(GL_TEXTURE0);
}
