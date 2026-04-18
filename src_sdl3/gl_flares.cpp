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
//
// Glow halos around light sources (torches etc.). Colored radial fan with the
// center at the torch and the outer ring faded to black, reusing the shared
// `particle` shader (pos + color, no texture).
//
#include "quakedef.h"
#include "gl_render.h"

typedef struct {
	float x, y, z;
	unsigned char r, g, b, a;
} flare_vertex_t;

static DynamicVBO flares_vbo;
static qboolean   flares_vbo_ready = false;

static void Flares_EnsureVBO(void)
{
	if (flares_vbo_ready) return;
	if (!R_EnsureParticleShader()) return;

	DynamicVBO_Init(&flares_vbo, 51 * sizeof(flare_vertex_t));  // 17 fan triangles
	DynamicVBO_SetAttrib(&flares_vbo, 0, 3, GL_FLOAT,         GL_FALSE, sizeof(flare_vertex_t), offsetof(flare_vertex_t, x));
	DynamicVBO_SetAttrib(&flares_vbo, 1, 4, GL_UNSIGNED_BYTE, GL_TRUE,  sizeof(flare_vertex_t), offsetof(flare_vertex_t, r));
	flares_vbo_ready = true;
}


float glowcos[17] = 
{
	 1.000000f,
	 0.923880f,
	 0.707105f,
	 0.382680f,
	 0.000000f,
	-0.382680f,
	-0.707105f,
	-0.923880f,
	-1.000000f,
	-0.923880f,
	-0.707105f,
	-0.382680f,
	 0.000000f,
	 0.382680f,
	 0.707105f,
	 0.923880f,
	 1.000000f
};


float glowsin[17] = 
{
	 0.000000f,
	 0.382680f,
	 0.707105f,
	 0.923880f,
	 1.000000f,
	 0.923880f,
	 0.707105f,
	 0.382680f,
	-0.000000f,
	-0.382680f,
	-0.707105f,
	-0.923880f,
	-1.000000f,
	-0.923880f,
	-0.707105f,
	-0.382680f,
	 0.000000f
};

void R_DrawGlows (entity_t *e)
{
	vec3_t	lightorigin;
	vec3_t	v;
	float	radius;
	float	distance;
	float	intensity;
	int			i;
	model_t		*clmodel;

	clmodel = e->model;

	VectorCopy(e->origin, lightorigin);
	radius = clmodel->glow_radius;

	VectorSubtract(lightorigin, r_origin, v);
	distance = Length(v);
	if (distance <= radius)
		return;

	Flares_EnsureVBO();
	if (!flares_vbo_ready)
		return;

	unsigned char cr, cg, cb;
	if (!strncmp (clmodel->name, "progs/glow_", 11))
	{
		lightorigin[2] += 4.0f;
		intensity = (1 - ((1024.0f - distance) * 0.0009765625)) * 0.75;
		cr = (unsigned char)fminf(255.0f, 1.0f * intensity * 255.0f);
		cg = (unsigned char)fminf(255.0f, 0.7f * intensity * 255.0f);
		cb = (unsigned char)fminf(255.0f, 0.4f * intensity * 255.0f);
	}
	else
	{
		cr = (unsigned char)(clmodel->glow_color[0] * 255.0f);
		cg = (unsigned char)(clmodel->glow_color[1] * 255.0f);
		cb = (unsigned char)(clmodel->glow_color[2] * 255.0f);
	}

	// Center vertex (slightly toward viewer).
	vec3_t center;
	VectorScale (vpn, -radius, v);
	VectorAdd   (v, lightorigin, center);

	// 17 ring vertices around the light origin.
	vec3_t ring[17];
	for (i = 0; i < 17; i++)
	{
		ring[i][0] = lightorigin[0] + radius * (vright[0] * glowcos[i] + vup[0] * glowsin[i]);
		ring[i][1] = lightorigin[1] + radius * (vright[1] * glowcos[i] + vup[1] * glowsin[i]);
		ring[i][2] = lightorigin[2] + radius * (vright[2] * glowcos[i] + vup[2] * glowsin[i]);
	}

	// Triangle soup: 16 triangles (ring[i] descending from 16 to 0 in the original fan).
	flare_vertex_t verts[16 * 3];
	int n = 0;
	for (i = 16; i > 0; i--)
	{
		int a = i, b = i - 1;
		verts[n].x = center[0]; verts[n].y = center[1]; verts[n].z = center[2]; verts[n].r = cr; verts[n].g = cg; verts[n].b = cb; verts[n].a = 255; n++;
		verts[n].x = ring[a][0]; verts[n].y = ring[a][1]; verts[n].z = ring[a][2]; verts[n].r = 0; verts[n].g = 0; verts[n].b = 0; verts[n].a = 255; n++;
		verts[n].x = ring[b][0]; verts[n].y = ring[b][1]; verts[n].z = ring[b][2]; verts[n].r = 0; verts[n].g = 0; verts[n].b = 0; verts[n].a = 255; n++;
	}

	float mvp[16];
	R_CurrentMVP(mvp);

	glDepthMask (GL_FALSE);
	glBlendFunc (GL_ONE, GL_ONE);
	glDisable (GL_TEXTURE_2D);
	if (gl_fogenable.value) glDisable(GL_FOG);

	GLShader_Use(&R_ParticleShader);
	glUniformMatrix4fv(R_ParticleShader_u_mvp, 1, GL_FALSE, mvp);

	DynamicVBO_Upload(&flares_vbo, verts, (GLsizei)(n * sizeof(flare_vertex_t)));
	DynamicVBO_Bind(&flares_vbo);
	glDrawArrays(GL_TRIANGLES, 0, n);

	glBindVertexArray(0);
	glUseProgram(0);
	glEnable (GL_TEXTURE_2D);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask (GL_TRUE);
	if (gl_fogenable.value) glEnable(GL_FOG);
	glColor3f (1,1,1);
}