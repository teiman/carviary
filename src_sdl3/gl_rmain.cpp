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
// r_main.c

#include "quakedef.h"
#include "gl_render.h"
#include "gl_mat4.h"
#include "gl_profiler.h"

// precalculated dot products for quantized angles
extern float	r_avertexnormal_dots_mdl[16][256];

extern float	*shadedots_mdl, *shadedots2_mdl;

float   lightlerpoffset;

extern int	lastposenum;
extern int	lastposenum0;
void	R_LightPoint (vec3_t p);
GLuint	playertextures[16];		// up to 16 color translated skins
int		c_brush_polys, c_alias_polys;
int		r_visframecount;	// bumped when going to a new PVS
int		r_framecount;		// used for dlight push checking
int		d_lightstylevalue[256];	// 8.8 fraction of base light value

extern	vec3_t	lightcolor;
extern	vec3_t	lightspot;

entity_t	r_worldentity;
entity_t	*currententity;

// view origin
vec3_t	vup;
vec3_t	vpn;
vec3_t	vright;
vec3_t	r_origin;
vec3_t	modelorg;

// screen size info
refdef_t	r_refdef;
mleaf_t		*r_viewleaf, *r_oldviewleaf;
mplane_t	frustum[4];
texture_t	*r_notexture_mip;

// these are the only functions which are used in this file, rest is in gl_md2 and gl_mdl

void	GL_DrawAliasBlendedFrame (int frame, aliashdr_t *paliashdr, entity_t* e);
void	GL_DrawAliasBlendedFrame2 (int frame, aliashdr_t *paliashdr, entity_t* e);
void	R_SetupBrushPolys (entity_t *e);
void	R_AnimateLight (void);
void	V_CalcBlend (void);
void	R_DrawSpriteModel (entity_t *e);
void	R_DrawWorld (void);
void	R_DrawWaterSurfaces (void);
void	R_DrawParticles (qboolean waterpart);
void	R_MarkLeaves (void);
void	R_DrawGlows(entity_t *e);


cvar_t	r_norefresh			= {"r_norefresh","0"};
cvar_t	r_drawviewmodel		= {"r_drawviewmodel","1"};
cvar_t	r_speeds			= {"r_speeds","0"};
cvar_t	r_wateralpha		= {"r_wateralpha","1", true};
cvar_t	r_dynamic			= {"r_dynamic","1"};
cvar_t	r_novis				= {"r_novis","0"};
cvar_t	gl_finish			= {"gl_finish","0", true};
cvar_t	gl_clear			= {"gl_clear","0", true};
cvar_t	gl_nocolors			= {"gl_nocolors","0"};
cvar_t	gl_keeptjunctions	= {"gl_keeptjunctions","1"};
cvar_t	gl_doubleeyes		= {"gl_doubleeys", "1"};
cvar_t	centerfade			= {"centerfade", "0", true};	// Tomaz - Fading CenterPrints
cvar_t	sbar_alpha			= {"sbar_alpha", "1", true};	// Tomaz - Sbar Alpha
cvar_t	con_alpha			= {"con_alpha", "0.5", true};	// Tomaz - Console Alpha
cvar_t	gl_glows			= {"gl_glows", "1", true};		// Tomaz - Glow
cvar_t	r_bobbing			= {"r_bobbing", "1", true};		// Tomaz - Bobbing Items
cvar_t	gl_caustics			= {"gl_caustics", "0", true};	// Tomaz - Underwater Caustics
cvar_t	gl_fbr				= {"gl_fbr", "1", true};		// Tomaz - Fullbrights
cvar_t	impaim				= {"impaim", "1", true};		// Tomaz - Improved Aiming
cvar_t	skybox_spin			= {"skybox_spin", "0", true};	// Tomaz - Spinning Skyboxes
cvar_t	mapshots			= {"mapshots", "1", true};		// Tomaz - MapShots
cvar_t	gl_particles		= {"gl_particles", "1", true};	// Tomaz - Particles
cvar_t	print_center_to_console		= {"print_center_to_console", "0", true};		// Tomaz - Prints CenerString's to the console
cvar_t	gl_particle_fire	= {"gl_particle_fire", "0", true};		// Tomaz - Fire Particles (disabled: requires TomazQuake assets)


/*
=================
R_CullBox

Returns true if the box is completely outside the frustom
=================
*/
qboolean R_CullBox (vec3_t emins, vec3_t emaxs)
{
	int i;
	mplane_t *p;
	byte signbits;
	float vec[3];
	for (i = 0; i < 4; i++)
	{
		p = frustum + i;
		signbits = p->signbits;
		vec[0] = (signbits & 1) ? emins[0] : emaxs[0];
		vec[1] = (signbits & 2) ? emins[1] : emaxs[1];
		vec[2] = (signbits & 4) ? emins[2] : emaxs[2];
		if (p->normal[0]*vec[0] + p->normal[1]*vec[1] + p->normal[2]*vec[2] < p->dist)
			return true;
	}
	return false;
}

/*
=============
R_BlendedRotateForEntity
=============
*/
void R_BlendedRotateForEntity (entity_t *e, int shadow)	// Tomaz - New Shadow
{
	float timepassed;
	float blend;
	vec3_t d;
	int i;

	// positional interpolation

	timepassed = realtime - e->translate_start_time; 

	if (e->translate_start_time == 0 || timepassed > 1)
	{
		e->translate_start_time = realtime;
		VectorCopy (e->origin, e->origin1);
		VectorCopy (e->origin, e->origin2);
	}

	if (!VectorCompare (e->origin, e->origin2))
	{
		e->translate_start_time = realtime;
		VectorCopy (e->origin2, e->origin1);
		VectorCopy (e->origin,  e->origin2);
		blend = 0;
	}
	else
	{
		blend =  (timepassed * 10) * slowmo.value; 	// Tomaz - Speed

		if (cl.paused || blend > 1) blend = 1;
	}

	VectorSubtract (e->origin2, e->origin1, d);

	MatStack_MulTranslate(&r_modelview,
		e->origin1[0] + (blend * d[0]),
		e->origin1[1] + (blend * d[1]),
		e->origin1[2] + (blend * d[2]));

	// orientation interpolation (Euler angles, yuck!)

	timepassed = realtime - e->rotate_start_time; 

	if (e->rotate_start_time == 0 || timepassed > 1)
	{
		e->rotate_start_time = realtime;
		VectorCopy (e->angles, e->angles1);
		VectorCopy (e->angles, e->angles2);
	}

	if (!VectorCompare (e->angles, e->angles2))
	{
		e->rotate_start_time = realtime;
		VectorCopy (e->angles2, e->angles1);
		VectorCopy (e->angles,  e->angles2);
		blend = 0;
	}
	else
	{
		blend = (timepassed * 10) * slowmo.value;	// Tomaz - Speed
 
		if (cl.paused || blend > 1) blend = 1;
	}

	VectorSubtract (e->angles2, e->angles1, d);

	// always interpolate along the shortest path
	for (i = 0; i < 3; i++) 
	{
		if (d[i] > 180)
		{
			d[i] -= 360;
		}
		else if (d[i] < -180)
		{
			d[i] += 360;
		}
	}

	MatStack_MulRotate(&r_modelview, e->angles1[1] + blend * d[1], 0, 0, 1);

	if (shadow == 0)
	{
		MatStack_MulRotate(&r_modelview, -e->angles1[0] - blend * d[0], 0, 1, 0);
		MatStack_MulRotate(&r_modelview,  e->angles1[2] + blend * d[2], 1, 0, 0);
	}

	MatStack_MulScale(&r_modelview, e->scale, e->scale, e->scale);
}
// Tomaz - Model Transform Interpolation End
/*
=================
R_RotateForEntity
functions used to draw the weapon models, so we don't have that nasty effect
=================
*/
void R_RotateForEntity (entity_t *e, int shadow)
{
	MatStack_MulTranslate(&r_modelview, e->origin[0], e->origin[1], e->origin[2]);

	MatStack_MulRotate(&r_modelview, e->angles[1], 0, 0, 1);

	if (shadow == 0)
	{
		MatStack_MulRotate(&r_modelview, -e->angles[0], 0, 1, 0);
		MatStack_MulRotate(&r_modelview,  e->angles[2], 1, 0, 0);
	}

	MatStack_MulScale(&r_modelview, e->scale, e->scale, e->scale);
}

qboolean weaponmodel = false;

/*
=================
R_DrawAliasModel
Common function used both by mdl and md2, so we put it in gl_rmain
=================
*/
void R_DrawAliasModel (entity_t *e, int cull)
{
	int			i, lnum, anim;
	float		add;
	vec3_t		dist, mins, maxs;
	model_t		*clmodel;
	aliashdr_t	*paliashdr;

	clmodel = currententity->model;

	VectorAdd (currententity->origin, clmodel->mins, mins);
	VectorAdd (currententity->origin, clmodel->maxs, maxs);

	if (!strcmp (currententity->model->name, "progs/fire.mdl"))
	{
		R_Fire(currententity, false);
	}

	if (!strcmp (currententity->model->name, "progs/fire2.mdl"))
	{
		R_Fire(currententity, true);
		return;
	}

	if (cull && R_CullBox (mins, maxs))
		return;

	VectorSubtract (r_origin, currententity->origin, modelorg);

	// HACK HACK HACK -- no fullbright colors, so make torches full light
	if (clmodel->fullbright)
	{
		lightcolor[0] = lightcolor[1] = lightcolor[2] = 256 / 200;	// Tomaz - Lit Support
	}
	else
	{
		float ang_ceil, ang_floor;

		R_LightPoint(currententity->origin); // Tomaz - Lit Support

		for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
		{
			if (cl_dlights[lnum].die >= cl.time)
			{
				VectorSubtract (currententity->origin,
								cl_dlights[lnum].origin,
								dist);
				add = cl_dlights[lnum].radius - Length(dist);

				if (add > 0)
				{
					lightcolor[0] += add * cl_dlights[lnum].color[0];
					lightcolor[1] += add * cl_dlights[lnum].color[1];
					lightcolor[2] += add * cl_dlights[lnum].color[2];
				}
			}
		}

		// add pitch angle so lighting changes when looking up/down (mainly for viewmodel)
		lightlerpoffset = e->angles[1] * (16 / 360.0);
		ang_ceil = ceil(lightlerpoffset);
		ang_floor = floor(lightlerpoffset);

		lightlerpoffset = ang_ceil - lightlerpoffset;

		shadedots_mdl	= r_avertexnormal_dots_mdl[(int)ang_ceil & (16 - 1)];
		shadedots2_mdl	= r_avertexnormal_dots_mdl[(int)ang_floor & (16 - 1)];

		lightcolor[0] = lightcolor[0] * 0.005;
		lightcolor[1] = lightcolor[1] * 0.005;
		lightcolor[2] = lightcolor[2] * 0.005;

		if (!lightcolor[0] && !lightcolor[1] && !lightcolor[2])
		{
			VectorCopy  (lightcolor, currententity->last_light);
		}
		else
		{
			VectorAdd   (lightcolor, currententity->last_light, lightcolor);
			VectorScale (lightcolor, 0.5f, lightcolor);
			VectorCopy  (lightcolor, currententity->last_light);
		}
	}

	paliashdr = (aliashdr_t *)Mod_Extradata (currententity->model);
	c_alias_polys += paliashdr->numtris;

	MatStack_Push(&r_modelview);

	// prevent viewmodels from messing up
	if (!weaponmodel)
		R_BlendedRotateForEntity (e, 0);
	else
		R_RotateForEntity (e, 0);

	if (!strcmp (clmodel->name, "progs/eyes.mdl") && gl_doubleeyes.value)
	{
		MatStack_MulTranslate(&r_modelview, paliashdr->scale_origin[0], paliashdr->scale_origin[1], paliashdr->scale_origin[2] - (22 + 8));
		// double size of eyes, since they are really hard to see in gl
		MatStack_MulScale(&r_modelview, paliashdr->scale[0]*2, paliashdr->scale[1]*2, paliashdr->scale[2]*2);
	}
	else
	{
		MatStack_MulTranslate(&r_modelview, paliashdr->scale_origin[0], paliashdr->scale_origin[1], paliashdr->scale_origin[2]);
		MatStack_MulScale(&r_modelview, paliashdr->scale[0], paliashdr->scale[1], paliashdr->scale[2]);
	}

	anim = (int)(cl.time*10) & 3;

	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, paliashdr->gl_texturenum[currententity->skinnum][anim]);

	// we can't dynamically colormap textures, so they are cached
	// seperately for the players.  Heads are just uncolored.
	if (currententity->colormap != vid.colormap && !gl_nocolors.value)
	{
		i = currententity - cl_entities;
		if (i >= 1 && i<=cl.maxclients)
			glBindTexture (GL_TEXTURE_2D, playertextures[i - 1]);
	}

	GL_DrawAliasBlendedFrame (currententity->frame, paliashdr, currententity);

	if (paliashdr->fb_texturenum[currententity->skinnum][anim] && gl_fbr.value)
	{
		glBindTexture (GL_TEXTURE_2D, paliashdr->fb_texturenum[currententity->skinnum][anim]);
		GL_DrawAliasBlendedFrame (currententity->frame, paliashdr, currententity);
	}

	MatStack_Pop(&r_modelview);

	// Glow flare begin - see gl_flares.c for more info
	if (gl_glows.value)
	{
		if (clmodel->glow_radius)
			R_DrawGlows(currententity);
	}
}
//==================================================================================

/*
=============
R_SetupTransEntities
=============
*/
void R_SetupTransEntities (void)
{
	int		i;

	for (i=0 ; i<cl_numvisedicts ; i++)
	{
		currententity = cl_visedicts[i];
	
		currententity->contents = Mod_PointInLeaf(currententity->origin, cl.worldmodel)->contents;

		if ((currententity->model->type == mod_alias) &&
			(currententity == &cl_entities[cl.viewentity]))
			 currententity->angles[0] *= 0.3f;

		if ((currententity->alpha > 1) ||
			(currententity->alpha <= 0))
			 currententity->alpha = 1;

		if ((currententity->scale > 4) ||
			(currententity->scale <= 0))
			 currententity->scale = 1;

		currententity->solid = false;
	}
}

/*
=============
R_SortTransEntities
=============
*/
void R_SortTransEntities (qboolean inwater)
{
	int			i;
	float		bestdist, dist;
	entity_t	*bestent;
	vec3_t		start, test;
	
	VectorCopy(r_refdef.vieworg, start);

transgetent:
	bestdist = 0;
	for (i=0 ; i<cl_numvisedicts ; i++)
	{
		currententity = cl_visedicts[i];

		if (currententity->solid)
			continue;

		if(!inwater && currententity->contents != CONTENTS_EMPTY)
			continue;
		
		if(inwater && currententity->contents == CONTENTS_EMPTY)
			continue;

		VectorCopy(currententity->origin, test);

		dist = (((test[0] - start[0]) * (test[0] - start[0])) +
				((test[1] - start[1]) * (test[1] - start[1])) +
				((test[2] - start[2]) * (test[2] - start[2])));

		if (dist > bestdist)
		{
			bestdist	= dist;
			bestent		= currententity;
		}
	}

	if (bestdist == 0)
		return;

	bestent->solid = true;

	currententity = bestent;
	switch (currententity->model->type)
	{
	case mod_alias:
		Prof_BeginSection (PROF_CPU_ALIAS);
		R_DrawAliasModel  (currententity, true);
		Magic_DrawForEntity (currententity);
		Prof_EndSection (PROF_CPU_ALIAS);
		break;
	case mod_brush:
		Prof_BeginSection (PROF_CPU_BRUSH_TRANS);
		R_SetupBrushPolys (currententity);
		Prof_EndSection (PROF_CPU_BRUSH_TRANS);
		break;
	case mod_sprite:
		Prof_BeginSection (PROF_CPU_SPRITE);
		R_DrawSpriteModel (currententity);
		Prof_EndSection (PROF_CPU_SPRITE);
		break;
	default:
		break;
	}

	goto transgetent;
}

/*
=============
R_DrawViewModel
=============
*/
void R_DrawViewModel (void)
{

	if (!r_drawviewmodel.value)
		return;

	if (chase_active.value)
		return;

	if (cl.items & IT_INVISIBILITY)
		return;

	if (cl.stats[STAT_HEALTH] <= 0)
		return;

	currententity = &cl.viewent;
	if (!currententity->model)
		return;

	if (currententity->model->type != mod_alias)
		return;

	// Tomaz - QC Alpha Scale Begin
	currententity->alpha = cl_entities[cl.viewentity].alpha;
	currententity->scale = cl_entities[cl.viewentity].scale;

	if ((currententity->alpha > 1) ||
		(currententity->alpha <= 0))
		currententity->alpha = 1;

	if ((currententity->scale > 4) ||
		(currententity->scale <= 0))
		currententity->scale = 1;
	// Tomaz - QC Alpha Scale End

	// hack the depth range to prevent view model from poking into walls
	glDepthRange (gldepthmin, gldepthmin + 0.3*(gldepthmax-gldepthmin));
	weaponmodel = true;
	R_DrawAliasModel (currententity, false);
	weaponmodel = false;
	glDepthRange (gldepthmin, gldepthmax);
}

extern cvar_t brightness; // muff
// idea originally nicked from LordHavoc
// re-worked + extended - muff 5 Feb 2001
// called inside polyblend

/*
============
R_PolyBlend

Screen-space overlays: damage/pickup tint (v_blend) and brightness gamma.
Both are full-viewport quads blended over the already-rendered scene.
Uses a clip-space [-1,+1] quad directly so we never depend on the caller's
view matrix (R_HudFill would pull in the 3D MVP via R_CurrentMVP).
============
*/
static GLuint polyblend_vao = 0;
static GLuint polyblend_vbo = 0;

static void R_InitPolyBlendVBO(void)
{
	if (polyblend_vao) return;

	// Two triangles covering clip space [-1,+1].
	const float verts[12] = {
		-1, -1,
		 1, -1,
		 1,  1,
		-1, -1,
		 1,  1,
		-1,  1,
	};

	glGenVertexArrays(1, &polyblend_vao);
	glGenBuffers(1, &polyblend_vbo);
	glBindVertexArray(polyblend_vao);
	glBindBuffer(GL_ARRAY_BUFFER, polyblend_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
	glBindVertexArray(0);
}

void R_PolyBlend (void)
{
	if (brightness.value == 1 && !v_blend[3])
		return;

	if (brightness.value < 0.2f) brightness.value = 0.2f;
	if (brightness.value > 1.0f) brightness.value = 1.0f;

	if (!R_EnsureFullscreenShader()) return;
	R_InitPolyBlendVBO();

	// Identity MVP: input positions are already clip-space.
	float identity[16] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1,
	};

	GLboolean depth_was_on = glIsEnabled(GL_DEPTH_TEST);
	GLboolean blend_was_on = glIsEnabled(GL_BLEND);
	GLboolean cull_was_on  = glIsEnabled(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);  // R_SetupGL flipped to GL_FRONT culling
	glEnable(GL_BLEND);
	glDepthRange(0.0, 1.0);

	GLShader_Use(&R_FullscreenShader);
	glUniformMatrix4fv(R_FullscreenShader_u_mvp, 1, GL_FALSE, identity);
	glBindVertexArray(polyblend_vao);

	// v_blend: per-frame damage flash / pickup tint.
	if (v_blend[3])
	{
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glUniform4f(R_FullscreenShader_u_color, v_blend[0], v_blend[1], v_blend[2], v_blend[3]);
		Prof_CountDraw(6);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	// Brightness gamma: multiply framebuffer by (1+brightness). Classic path
	// drew two passes of a DST_COLOR-blended quad to widen the range.
	if (brightness.value < 1.0f)
	{
		glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
		glUniform4f(R_FullscreenShader_u_color, 1.0f, 1.0f, 1.0f, brightness.value);
		Prof_CountDraw(6);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		Prof_CountDraw(6);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	glBindVertexArray(0);
	glUseProgram(0);
	if (!blend_was_on) glDisable(GL_BLEND);
	if (depth_was_on)  glEnable(GL_DEPTH_TEST);
	if (cull_was_on)   glEnable(GL_CULL_FACE);
}



int SignbitsForPlane (mplane_t *out)
{
	int	bits, j;

	// for fast box on planeside test

	bits = 0;
	for (j=0 ; j<3 ; j++)
	{
		if (out->normal[j] < 0)
			bits |= 1<<j;
	}
	return bits;
}

void R_SetFrustum (void)
{
	int		i;

	if (r_refdef.fov_x == 90) 
	{
		// front side is visible

		VectorAdd (vpn, vright, frustum[0].normal);
		VectorSubtract (vpn, vright, frustum[1].normal);

		VectorAdd (vpn, vup, frustum[2].normal);
		VectorSubtract (vpn, vup, frustum[3].normal);
	}
	else
	{

		RotatePointAroundVector( frustum[0].normal, vup, vpn, -(90-r_refdef.fov_x * 0.5 ) );		// Tomaz - Speed
		RotatePointAroundVector( frustum[1].normal, vup, vpn, 90-r_refdef.fov_x * 0.5 );			// Tomaz - Speed
		RotatePointAroundVector( frustum[2].normal, vright, vpn, 90-r_refdef.fov_y * 0.5 );			// Tomaz - Speed
		RotatePointAroundVector( frustum[3].normal, vright, vpn, -( 90 - r_refdef.fov_y * 0.5 ) );	// Tomaz - Speed
	}

	for (i=0 ; i<4 ; i++)
	{
		frustum[i].type = PLANE_ANYZ;
		frustum[i].dist = DotProduct (r_origin, frustum[i].normal);
		frustum[i].signbits = SignbitsForPlane (&frustum[i]);
	}
}

/*
===============
R_SetupFrame
===============
*/
void R_SetupFrame (void)
{
	R_AnimateLight ();

	r_framecount++;

// build the transformation matrix for the given view angles
	VectorCopy (r_refdef.vieworg, r_origin);

	AngleVectors (r_refdef.viewangles, vpn, vright, vup);

// current viewleaf
	r_oldviewleaf = r_viewleaf;
	r_viewleaf = Mod_PointInLeaf (r_origin, cl.worldmodel);

	V_SetContentsColor (r_viewleaf->contents);
	V_CalcBlend ();

	c_brush_polys = 0;
	c_alias_polys = 0;

}


void MYgluPerspective( GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar )
{
	// Compose frustum onto the current projection stack top.
	float ymax = (float)(zNear * tan(fovy * 0.008726646259971));
	float ymin = -ymax;
	float xmin = ymin * (float)aspect;
	float xmax = ymax * (float)aspect;

	mat4_t f;
	mat4_frustum(&f, xmin, xmax, ymin, ymax, (float)zNear, (float)zFar);
	mat4_t cur = *MatStack_Top(&r_projection);
	mat4_mul(MatStack_Top(&r_projection), &cur, &f);
}

/*
=============
R_SetupGL
=============
*/
void R_SetupGL (void)
{

	float	screenaspect;
	extern	int glwidth, glheight;
	int		x, x2, y2, y, w, h;

	//
	// set up viewpoint
	//
	MatStack_LoadIdentity(&r_projection);
	x = r_refdef.vrect.x * glwidth/vid.width;
	x2 = (r_refdef.vrect.x + r_refdef.vrect.width) * glwidth/vid.width;
	y = (vid.height-r_refdef.vrect.y) * glheight/vid.height;
	y2 = (vid.height - (r_refdef.vrect.y + r_refdef.vrect.height)) * glheight/vid.height;

	glCullFace(GL_FRONT);

	// fudge around because of frac screen scale
	if (x > 0)
		x--;
	if (x2 < glwidth)
		x2++;
	if (y2 < 0)
		y2--;
	if (y < glheight)
		y++;

	w = x2 - x;
	h = y - y2;

	glViewport (glx + x, gly + y2, w, h);
    screenaspect = (float)r_refdef.vrect.width/r_refdef.vrect.height;

	// adaptive near clip: reduce at high FOV to avoid seeing through walls
	{
		float fw = 1.0f / tan(r_refdef.fov_x * M_PI / 360.0f);
		float fh = 1.0f / tan(r_refdef.fov_y * M_PI / 360.0f);
		float znear = 12.0f * (fw < fh ? fw : fh);
		if (znear < 0.5f) znear = 0.5f;
		if (znear > 4.0f) znear = 4.0f;
		MYgluPerspective (r_refdef.fov_y, screenaspect, znear, 8193);
	}


	MatStack_LoadIdentity(&r_modelview);
	MatStack_MulRotate(&r_modelview, -90,                       1, 0, 0);  // put Z going up
	MatStack_MulRotate(&r_modelview,  90,                       0, 0, 1);  // put Z going up
	MatStack_MulRotate(&r_modelview, -r_refdef.viewangles[2],   1, 0, 0);
	MatStack_MulRotate(&r_modelview, -r_refdef.viewangles[0],   0, 1, 0);
	MatStack_MulRotate(&r_modelview, -r_refdef.viewangles[1],   0, 0, 1);
	MatStack_MulTranslate(&r_modelview, -r_refdef.vieworg[0], -r_refdef.vieworg[1], -r_refdef.vieworg[2]);

	// Keep r_world_matrix in sync: some paths (water) reload it onto the modelview.
	memcpy(r_world_matrix, MatStack_Top(&r_modelview)->m, sizeof(r_world_matrix));

	//
	// set drawing parms
	//

	glEnable(GL_DEPTH_TEST);
}

/*
=============
R_Clear
=============
*/
void R_Clear (void)
{
	Prof_BeginSection (PROF_CLEAR);
	if (gl_clear.value)
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	else
		glClear (GL_DEPTH_BUFFER_BIT);

	gldepthmin = 0;
	gldepthmax = 1;
	glDepthFunc (GL_LEQUAL);

	glDepthRange (gldepthmin, gldepthmax);
	Prof_EndSection (PROF_CLEAR);
}

/*
=============
R_RenderScene
renders the current screen, without mirrors, thus can be used in mirror code
=============
*/

void R_MoveParticles ();

void R_RenderScene (void)
{
	double	time1, time2;

	if (r_norefresh.value)
		return;

	Prof_BeginSection (PROF_SCENE);

	if (r_speeds.value)
	{
		glFinish ();
		time1 = Sys_FloatTime ();
		c_brush_polys = 0;
		c_alias_polys = 0;
	}

	R_SetupFrame ();

	extern void Lamp_Update (void);
	Lamp_Update ();

	R_PushDlights ();

	R_SetFrustum ();

	R_SetupGL ();

	Prof_BeginSection (PROF_CPU_MARK_LEAVES);
	R_MarkLeaves ();	// done here so we know if we're in water
	Prof_EndSection (PROF_CPU_MARK_LEAVES);

	R_DrawWorld ();		// adds static entities to the list

	S_ExtraUpdate ();	// don't let sound get messed up if going slow

	Prof_BeginSection (PROF_CPU_TRANS_SETUP);
	R_SetupTransEntities ();
	Prof_EndSection (PROF_CPU_TRANS_SETUP);

	Prof_BeginSection (PROF_CPU_PARTICLES);
	R_MoveParticles ();
	Prof_EndSection (PROF_CPU_PARTICLES);

	if (cl.cshifts[CSHIFT_CONTENTS].percent == 0)
	{
		R_SortTransEntities (true);
		if (gl_particles.value) {
			Prof_BeginSection (PROF_CPU_PARTICLES);
			R_DrawParticles (true);	// Tomaz - fixing particle / water bug
			Prof_EndSection (PROF_CPU_PARTICLES);
		}
		Prof_BeginSection (PROF_CPU_WATER);
		R_DrawWaterSurfaces (); // Tomaz - fixing particle / water bug
		Flames_Draw();          // procedural flame billboards over lava
		Prof_EndSection (PROF_CPU_WATER);
		R_SortTransEntities (false);
		if (gl_particles.value) {
			Prof_BeginSection (PROF_CPU_PARTICLES);
			R_DrawParticles (false);// Tomaz - fixing particle / water bug
			Prof_EndSection (PROF_CPU_PARTICLES);
		}
	}
	else
	{
		R_SortTransEntities (false);
		if (gl_particles.value) {
			Prof_BeginSection (PROF_CPU_PARTICLES);
			R_DrawParticles (false);// Tomaz - fixing particle / water bug
			Prof_EndSection (PROF_CPU_PARTICLES);
		}
		Prof_BeginSection (PROF_CPU_WATER);
		R_DrawWaterSurfaces ();	// Tomaz - fixing particle / water bug
		Flames_Draw();          // procedural flame billboards over lava
		Prof_EndSection (PROF_CPU_WATER);
		R_SortTransEntities (true);
		if (gl_particles.value) {
			Prof_BeginSection (PROF_CPU_PARTICLES);
			R_DrawParticles (true);	// Tomaz - fixing particle / water bug
			Prof_EndSection (PROF_CPU_PARTICLES);
		}
	}

	// Gunshot impact glow + smoke. Drawn AFTER every trans-entity pass so
	// monsters, pickups and other alias models (which all route through
	// R_SortTransEntities, including the alpha=1 ones) are already on
	// screen with correct depth -- the smoke just composites on top.
	{ extern void Gunshot_Draw(void); Gunshot_Draw(); }

	if (r_speeds.value)
	{
		time2 = Sys_FloatTime ();
		Con_Printf ("%3i ms  %4i wpoly %4i epoly\n", (int)((time2-time1)*1000), c_brush_polys, c_alias_polys);
	}

	Prof_EndSection (PROF_SCENE);
}

/*
================
R_RenderView
================
*/
void R_RenderView (void)
{
	if (!r_worldentity.model || !cl.worldmodel)
	{
		Host_Error ("R_RenderView: NULL worldmodel");
	}

	R_Clear ();
	R_RenderScene();

	Prof_BeginSection (PROF_VIEWMODEL);
	R_DrawViewModel ();
	Prof_EndSection (PROF_VIEWMODEL);

	Prof_BeginSection (PROF_POLYBLEND);
	R_PolyBlend ();
	Prof_EndSection (PROF_POLYBLEND);
}
