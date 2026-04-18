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

		rad = cl_dlights[lnum].radius;
		dist = PlaneDiff (cl_dlights[lnum].origin, surf->plane);
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = cl_dlights[lnum].origin[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];

		// Tomaz - Lit Support Begin
		bl		= blocklights;
		cred	= (int)(cl_dlights[lnum].color[0] * 256.0f);
		cgreen	= (int)(cl_dlights[lnum].color[1] * 256.0f);
		cblue	= (int)(cl_dlights[lnum].color[2] * 256.0f);
		// Tomaz - Lit Support End	

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
				// Tomaz - Lit Support Begin
				{
					brightness = rad - dist;
					bl[0] += brightness * cred;
					bl[1] += brightness * cgreen;
					bl[2] += brightness * cblue;
				}
				bl += 3;
				// Tomaz - Lit Support End
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
void R_BuildLightMap (msurface_t *surf, byte *dest, int stride)
{
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
	for (i=0 ; i<tmax ; i++, dest += stride)
	{
		for (j=0 ; j<smax ; j++)
		{
			// Tomaz - Lit Support Begin
			t = bl[0] >> 7;if (t > 255) t = 255;dest[0] = t;
			t = bl[1] >> 7;if (t > 255) t = 255;dest[1] = t;
			t = bl[2] >> 7;if (t > 255) t = 255;dest[2] = t;
			if (lightmap_bytes > 3)	dest[3] = 255;

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

			DynamicVBO_Bind(&world_vbo);
			Prof_CountDraw(n);
			glDrawArrays(GL_TRIANGLES, 0, n);

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
	// Walk waterchain, bucket surfaces by texture. Maps can have any number
	// of liquid textures (water, lava, slime, teleporter, modded variants).
	enum { MAX_WATER_TEXTURES = 32, MAX_PER_BUCKET = 4096 };
	texture_t *bucket_tex[MAX_WATER_TEXTURES];
	static msurface_t *bsurfs[MAX_WATER_TEXTURES][MAX_PER_BUCKET];
	int bcount[MAX_WATER_TEXTURES];
	int nbuckets = 0;
	for (int i = 0; i < MAX_WATER_TEXTURES; ++i) bcount[i] = 0;

	for (msurface_t *s = waterchain; s; s = s->texturechain)
	{
		texture_t *t = s->texinfo->texture;
		int b;
		for (b = 0; b < nbuckets; ++b) if (bucket_tex[b] == t) break;
		if (b == nbuckets) {
			if (nbuckets >= MAX_WATER_TEXTURES) continue;
			bucket_tex[b] = t;
			nbuckets++;
		}
		if (bcount[b] < MAX_PER_BUCKET)
			bsurfs[b][bcount[b]++] = s;
	}
	waterchain = NULL;

	// Triangulate every bucket into warp_soup back-to-back, recording ranges.
	static water_draw_range_t ranges[MAX_WATER_TEXTURES];
	int nranges = 0;
	int nverts = 0;

	for (int b = 0; b < nbuckets; ++b)
	{
		int range_first = nverts;
		for (int i = 0; i < bcount[b]; ++i)
		{
			if (nverts >= WARP_MAX_VERTS - 30) break;
			Water_AppendSurface(bsurfs[b][i], warp_soup, &nverts, WARP_MAX_VERTS);
		}
		int range_count = nverts - range_first;
		if (range_count > 0) {
			ranges[nranges].first = range_first;
			ranges[nranges].count = range_count;
			ranges[nranges].t     = bucket_tex[b];
			nranges++;
		}
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

	glActiveTexture(GL_TEXTURE0);
	for (int i = 0; i < nranges; ++i)
	{
		glBindTexture(GL_TEXTURE_2D, ranges[i].t->gl_texturenum);
		Prof_CountDraw(ranges[i].count);
		glDrawArrays(GL_TRIANGLES, vertex_offset + ranges[i].first, ranges[i].count);
	}

	if (!blend_was_on && alpha < 1.0f) glDisable(GL_BLEND);
	glBindVertexArray(0);
	glUseProgram(0);
}

float	r_world_matrix[16];

static void R_EmitTextureChains (void); // forward decl; def below

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
	R_EmitTextureChains ();

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

	// Count total triangulated verts needed, mark non-batched surfaces.
	int total_verts = 0;
	int batched_surfs = 0;
	model_t *m = cl.worldmodel;
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

static void R_EmitTextureChains (void)
{
	if (!cl.worldmodel) return;
	if (!world_static_ready) return; // R_NewMap didn't run yet; nothing to draw

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

	texture_t *last_t     = NULL;
	qboolean   last_fence = false;
	int        last_lm    = -1;

	// Remember ranges we'll re-emit for fullbright in a second pass.
	struct fb_batch_t { texture_t *t; int n; };
	static struct fb_batch_t fb_batches[1024];
	static GLint   fb_firsts[SURF_BATCH_MAX];
	static GLsizei fb_counts[SURF_BATCH_MAX];
	int fb_batch_count = 0;
	int fb_draw_count  = 0;

	for (int tnum = 0; tnum < cl.worldmodel->numtextures; ++tnum)
	{
		texture_t *base = cl.worldmodel->textures[tnum];
		if (!base || !base->texturechain) continue;
		texture_t *t = base;

		int nsurfs = 0;
		for (msurface_t *s = t->texturechain; s && nsurfs < SURF_BATCH_MAX; s = s->texturechain)
			surfs[nsurfs++] = s;
		t->texturechain = NULL;
		if (nsurfs == 0) continue;
		qsort(surfs, nsurfs, sizeof(surfs[0]), cmp_surf_by_lm);

		qboolean fence = t->transparent != 0;
		qboolean wants_fb = (t->fullbrights != -1 && gl_fbr.value);
		int fb_this_tex_start = fb_draw_count;

		// Shader/texture bind: only when the texture actually changes.
		if (t != last_t || fence != last_fence)
		{
			if (fence) { if (!R_EnsureWorldFenceShader()) continue; }
			else       { if (!R_EnsureWorldOpaqueShader()) continue; }
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
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, t->gl_texturenum);
			last_t = t;
			last_fence = fence;
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
	R_EmitTextureChains ();

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
