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
	EmitSkyPolys_Pass(s, 8.0f);

	// Pass 2: alpha sky, double-speed scroll, blended over pass 1.
	GLboolean blend_was_on = glIsEnabled(GL_BLEND);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glBindTexture(GL_TEXTURE_2D, alphaskytexture);
	EmitSkyPolys_Pass(s, 16.0f);

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
void R_DrawSky (msurface_t *s)
{
	for ( ; s ; s=s->texturechain)
		EmitSkyPolys (s);
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
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);


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
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

