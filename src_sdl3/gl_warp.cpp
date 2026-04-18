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
#include "quakedef.h"
#include "gl_render.h"

//
// gl_warp.c -- sky and water polygons
//

//
// External
//
extern	model_t	*loadmodel;
extern	cvar_t	gl_subdivide_size;
extern int causticstexture[31];

//
// Integer
//
int		shinytexture;

//
// msurface_t
//
msurface_t	*warpface;

//
// Float
//
float	turbsin[] =
{
	#include "gl_warp_sin.h"
};

//
// Define
//
#define TURBSCALE (40.7436589469704879)

// Shared vertex format for warp/caustics draws.
typedef struct {
	float x, y, z;
	float s, t;
} warp_vtx_t;

// Generous capacity: water surfaces get subdivided up to 64 unit grid,
// generating many small polys per surface chain.
#define WARP_MAX_VERTS 16384
static warp_vtx_t warp_soup[WARP_MAX_VERTS];
static DynamicVBO warp_vbo;
static qboolean   warp_vbo_ready = false;

static void Warp_EnsureVBO(void)
{
	if (warp_vbo_ready) return;
	DynamicVBO_Init(&warp_vbo, sizeof(warp_soup));
	DynamicVBO_SetAttrib(&warp_vbo, 0, 3, GL_FLOAT, GL_FALSE, sizeof(warp_vtx_t), offsetof(warp_vtx_t, x));
	DynamicVBO_SetAttrib(&warp_vbo, 1, 2, GL_FLOAT, GL_FALSE, sizeof(warp_vtx_t), offsetof(warp_vtx_t, s));
	warp_vbo_ready = true;
}


/*
================
BoundPoly
================
*/
void BoundPoly (int numverts, float *verts, vec3_t mins, vec3_t maxs)
{
	int		i, j;
	float	*v;

	mins[0] = mins[1] = mins[2] = 9999;
	maxs[0] = maxs[1] = maxs[2] = -9999;
	v = verts;
	for (i=0 ; i<numverts ; i++)
		for (j=0 ; j<3 ; j++, v++)
		{
			if (*v < mins[j])
				mins[j] = *v;
			if (*v > maxs[j])
				maxs[j] = *v;
		}
}

/*
================
SubdividePolygon
================
*/
void SubdividePolygon (int numverts, float *verts)
{
	int		i, j, k;
	vec3_t	mins, maxs;
	float	m;
	float	*v;
	vec3_t	front[64], back[64];
	int		f, b;
	float	dist[64];
	float	frac;
	glpoly_t	*poly;
	float	s, t;

	if (numverts > 60)
		Sys_Error ("numverts = %i", numverts);

	BoundPoly (numverts, verts, mins, maxs);

	for (i=0 ; i<3 ; i++)
	{
		m = (mins[i] + maxs[i]) * 0.5;
		m = gl_subdivide_size.value * floor (m/gl_subdivide_size.value + 0.5);
		if (maxs[i] - m < 8)
			continue;
		if (m - mins[i] < 8)
			continue;

		// cut it
		v = verts + i;
		for (j=0 ; j<numverts ; j++, v+= 3)
			dist[j] = *v - m;

		// wrap cases
		dist[j] = dist[0];
		v-=i;
		VectorCopy (verts, v);

		f = b = 0;
		v = verts;
		for (j=0 ; j<numverts ; j++, v+= 3)
		{
			if (dist[j] >= 0)
			{
				VectorCopy (v, front[f]);
				f++;
			}
			if (dist[j] <= 0)
			{
				VectorCopy (v, back[b]);
				b++;
			}
			if (dist[j] == 0 || dist[j+1] == 0)
				continue;
			if ( (dist[j] > 0) != (dist[j+1] > 0) )
			{
				// clip point
				frac = dist[j] / (dist[j] - dist[j+1]);
				for (k=0 ; k<3 ; k++)
					front[f][k] = back[b][k] = v[k] + frac*(v[3+k] - v[k]);
				f++;
				b++;
			}
		}

		SubdividePolygon (f, front[0]);
		SubdividePolygon (b, back[0]);
		return;
	}

	poly = (glpoly_t *)Hunk_Alloc (sizeof(glpoly_t) + (numverts-4) * VERTEXSIZE*sizeof(float));
	poly->next = warpface->polys;
	warpface->polys = poly;
	poly->numverts = numverts;
	for (i=0 ; i<numverts ; i++, verts+= 3)
	{
		VectorCopy (verts, poly->verts[i]);
		s = DotProduct (verts, warpface->texinfo->vecs[0]);
		t = DotProduct (verts, warpface->texinfo->vecs[1]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;
	}
}

/*
================
GL_SubdivideSurface

Breaks a polygon up along axial 64 unit
boundaries so that turbulent and sky warps
can be done reasonably.
================
*/
void GL_SubdivideSurface (msurface_t *s)
{
	vec3_t		verts[64];
	int			numverts;
	int			i;
	int			lindex;
	float		*vec;

	warpface = s;

	//
	// convert edges back to a normal polygon
	//
	numverts = 0;
	for (i=0 ; i<s->numedges ; i++)
	{
		lindex = loadmodel->surfedges[s->firstedge + i];

		if (lindex > 0)
			vec = loadmodel->vertexes[loadmodel->edges[lindex].v[0]].position;
		else
			vec = loadmodel->vertexes[loadmodel->edges[-lindex].v[1]].position;
		VectorCopy (vec, verts[numverts]);
		numverts++;
	}

	SubdividePolygon (numverts, verts[0]);
}

//=========================================================

/*			
Emit*****Polys - emits polygons with special effects
*/

/*
=============
EmitWaterPolys

Does a water warp on the pre-fragmented glpoly_t chain. Caller has already
bound the diffuse texture. `alpha` comes from r_wateralpha at the call site.
Warp is computed per-fragment by the `world_water` shader, so the motion
stays smooth regardless of `gl_subdivide_size`.
=============
*/
void EmitWaterPolys (msurface_t *s, float alpha)
{
	glpoly_t	*p;
	float		*v;
	int			i;

	if (!R_EnsureWorldWaterShader()) return;
	Warp_EnsureVBO();

	static warp_vtx_t fan[256];
	int n = 0;

	for (p = s->polys; p; p = p->next)
	{
		int nv = p->numverts;
		if (nv > 256) nv = 256;

		v = p->verts[0];
		for (i = 0; i < nv; ++i, v += VERTEXSIZE)
		{
			fan[i].x = v[0];
			fan[i].y = v[1];
			fan[i].z = v[2];

			if (r_wave.value)
				fan[i].z = v[2] + r_wave.value * sin(v[0]*0.02+realtime) * sin(v[1]*0.02+realtime) * sin(v[2]*0.02+realtime);

			fan[i].s = v[3];
			fan[i].t = v[4];
		}

		for (int k = 1; k < nv - 1; ++k)
		{
			if (n + 3 > WARP_MAX_VERTS) goto flush;
			warp_soup[n++] = fan[0];
			warp_soup[n++] = fan[k];
			warp_soup[n++] = fan[k + 1];
		}
	}

flush:
	if (n == 0) return;

	float mvp[16];
	R_CurrentMVP(mvp);

	GLboolean blend_was_on = glIsEnabled(GL_BLEND);
	if (alpha < 1.0f) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	glActiveTexture(GL_TEXTURE0);
	GLShader_Use(&R_WorldWaterShader);
	glUniformMatrix4fv(R_WorldWaterShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform1i       (R_WorldWaterShader_u_tex, 0);
	glUniform1f       (R_WorldWaterShader_u_time, (float)realtime);
	glUniform1f       (R_WorldWaterShader_u_alpha, alpha);

	DynamicVBO_Upload(&warp_vbo, warp_soup, (GLsizei)(n * sizeof(warp_vtx_t)));
	DynamicVBO_Bind(&warp_vbo);
	glDrawArrays(GL_TRIANGLES, 0, n);

	if (!blend_was_on && alpha < 1.0f) glDisable(GL_BLEND);

	glBindVertexArray(0);
	glUseProgram(0);
}

/*
=============
EmitCausticsPolys

Overlaid on underwater opaque surfaces: ZERO,SRC_COLOR multiplies the frame
buffer by the caustic texture, brightening the pattern without washing out
the original texture.
=============
*/
void EmitCausticsPolys (msurface_t *s)
{
	glpoly_t	*p;
	float		*v;
	int			i, tn;

	if (!R_EnsureSpriteShader()) return;
	Warp_EnsureVBO();

	static warp_vtx_t fan[256];
	int n = 0;

	for (p = s->polys; p; p = p->next)
	{
		int nv = p->numverts;
		if (nv > 256) nv = 256;

		v = p->verts[0];
		for (i = 0; i < nv; ++i, v += VERTEXSIZE)
		{
			fan[i].x = v[0]; fan[i].y = v[1]; fan[i].z = v[2];
			fan[i].s = v[3]; fan[i].t = v[4];
		}

		for (int k = 1; k < nv - 1; ++k)
		{
			if (n + 3 > WARP_MAX_VERTS) goto flush;
			warp_soup[n++] = fan[0];
			warp_soup[n++] = fan[k];
			warp_soup[n++] = fan[k + 1];
		}
	}

flush:
	if (n == 0) return;

	tn = (int)(host_time * 20) % 31;

	float mvp[16];
	R_CurrentMVP(mvp);

	GLboolean blend_was_on = glIsEnabled(GL_BLEND);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ZERO, GL_SRC_COLOR);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, causticstexture[tn]);

	GLShader_Use(&R_SpriteShader);
	glUniformMatrix4fv(R_SpriteShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform4f       (R_SpriteShader_u_color, 1.0f, 1.0f, 1.0f, 0.3f);
	glUniform1i       (R_SpriteShader_u_tex, 0);

	DynamicVBO_Upload(&warp_vbo, warp_soup, (GLsizei)(n * sizeof(warp_vtx_t)));
	DynamicVBO_Bind(&warp_vbo);
	glDrawArrays(GL_TRIANGLES, 0, n);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	if (!blend_was_on) glDisable(GL_BLEND);

	glBindVertexArray(0);
	glUseProgram(0);
}