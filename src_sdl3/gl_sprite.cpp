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
// All sprite drawing
//

#include "quakedef.h"
#include "gl_render.h"
#include "gl_profiler.h"

typedef struct {
	float x, y, z;
	float s, t;
} sprite_vertex_t;

static DynamicVBO sprite_vbo;
static qboolean   sprite_vbo_ready = false;

static void Sprite_EnsureVBO(void)
{
	if (sprite_vbo_ready) return;
	if (!R_EnsureSpriteShader()) return;

	DynamicVBO_Init(&sprite_vbo, 6 * sizeof(sprite_vertex_t));
	DynamicVBO_SetAttrib(&sprite_vbo, 0, 3, GL_FLOAT, GL_FALSE, sizeof(sprite_vertex_t), offsetof(sprite_vertex_t, x));
	DynamicVBO_SetAttrib(&sprite_vbo, 1, 2, GL_FLOAT, GL_FALSE, sizeof(sprite_vertex_t), offsetof(sprite_vertex_t, s));
	sprite_vbo_ready = true;
}

typedef struct mspriteframe_s
{
	int		width;
	int		height;
	float	up, down, left, right;
	int		gl_texturenum;
} mspriteframe_t;

typedef struct
{
	int				numframes;
	float			*intervals;
	mspriteframe_t	*frames[1];
} mspritegroup_t;

typedef struct
{
	spriteframetype_t	type;
	mspriteframe_t		*frameptr;
} mspriteframedesc_t;

typedef struct
{
	int					type;
	int					maxwidth;
	int					maxheight;
	int					numframes;
	float				beamlength;		// remove?
	void				*cachespot;		// remove?
	mspriteframedesc_t	frames[1];
} msprite_t;

extern char		loadname[32];

extern model_t	*loadmodel;

/*
===================
Mod_LoadSpriteFrame
===================
*/
void * Mod_LoadSpriteFrame (void * pin, mspriteframe_t **ppframe, int framenum)
{
	dspriteframe_t		*pinframe;
	mspriteframe_t		*pspriteframe;
	int					width, height, size, origin[2];
	char				name[64], sprite[64];	// Tomaz - TGA

	pinframe = (dspriteframe_t *)pin;

	width = LittleLong (pinframe->width);
	height = LittleLong (pinframe->height);
	size = width * height;

	pspriteframe = (mspriteframe_t *)Hunk_AllocName (sizeof (mspriteframe_t),loadname);

	Q_memset (pspriteframe, 0, sizeof (mspriteframe_t));

	*ppframe = pspriteframe;

	pspriteframe->width = width;
	pspriteframe->height = height;
	origin[0] = LittleLong (pinframe->origin[0]);
	origin[1] = LittleLong (pinframe->origin[1]);

	pspriteframe->up = origin[1];
	pspriteframe->down = origin[1] - height;
	pspriteframe->left = origin[0];
	pspriteframe->right = width + origin[0];

	// Tomaz - TGA Begin
	COM_StripExtension(loadmodel->name, sprite); 
	_snprintf (name, sizeof(name),"%s_%i", sprite, framenum);

	pspriteframe->gl_texturenum = loadtextureimage (name, false, true);
	if (pspriteframe->gl_texturenum == 0)// did not find a matching TGA...		
	{
		_snprintf (name, sizeof(name),"%s_%i", loadmodel->name, framenum);	
		pspriteframe->gl_texturenum = GL_LoadTexture (name, width, height, (byte *)(pinframe + 1), true, true, 1);
	}
	// Tomaz - TGA End

	return (void *)((byte *)pinframe + sizeof (dspriteframe_t) + size);
}


/*
=================
Mod_LoadSpriteGroup
=================
*/
void * Mod_LoadSpriteGroup (void * pin, mspriteframe_t **ppframe, int framenum)
{
	dspritegroup_t		*pingroup;
	mspritegroup_t		*pspritegroup;
	int					i, numframes;
	dspriteinterval_t	*pin_intervals;
	float				*poutintervals;
	void				*ptemp;

	pingroup = (dspritegroup_t *)pin;

	numframes = LittleLong (pingroup->numframes);

	pspritegroup = (mspritegroup_t *)Hunk_AllocName (sizeof (mspritegroup_t) +
				(numframes - 1) * sizeof (pspritegroup->frames[0]), loadname);

	pspritegroup->numframes = numframes;

	*ppframe = (mspriteframe_t *)pspritegroup;

	pin_intervals = (dspriteinterval_t *)(pingroup + 1);

	poutintervals = (float *)Hunk_AllocName (numframes * sizeof (float), loadname);

	pspritegroup->intervals = poutintervals;

	for (i=0 ; i<numframes ; i++)
	{
		*poutintervals = LittleFloat (pin_intervals->interval);
		if (*poutintervals <= 0.0)
			Host_Error ("Mod_LoadSpriteGroup: interval<=0");

		poutintervals++;
		pin_intervals++;
	}

	ptemp = (void *)pin_intervals;

	for (i=0 ; i<numframes ; i++)
	{
		ptemp = Mod_LoadSpriteFrame (ptemp, &pspritegroup->frames[i], framenum * 100 + i);
	}

	return ptemp;
}


/*
=================
Mod_LoadSpriteModel
=================
*/
void Mod_LoadSpriteModel (model_t *mod, void *buffer)
{
	int					i;
	int					version;
	dsprite_t			*pin;
	msprite_t			*psprite;
	int					numframes;
	int					size;
	dspriteframetype_t	*pframetype;
	
	pin = (dsprite_t *)buffer;

	version = LittleLong (pin->version);
	if (version != SPRITE_VERSION)
		Sys_Error ("%s has wrong version number "
				 "(%i should be %i)", mod->name, version, SPRITE_VERSION);

	numframes = LittleLong (pin->numframes);

	size = sizeof (msprite_t) +	(numframes - 1) * sizeof (psprite->frames);

	psprite = (msprite_t *)Hunk_AllocName (size, loadname);

	mod->cache.data = psprite;

	psprite->type = LittleLong (pin->type);
	psprite->maxwidth = LittleLong (pin->width);
	psprite->maxheight = LittleLong (pin->height);
	psprite->beamlength = LittleFloat (pin->beamlength);
	mod->synctype = (synctype_t)LittleLong (pin->synctype);
	psprite->numframes = numframes;

	mod->mins[0] = mod->mins[1] = -psprite->maxwidth/2;
	mod->maxs[0] = mod->maxs[1] = psprite->maxwidth/2;
	mod->mins[2] = -psprite->maxheight/2;
	mod->maxs[2] = psprite->maxheight/2;
	
//
// load the frames
//
	if (numframes < 1)
		Host_Error ("Mod_LoadSpriteModel: Invalid # of frames: %d\n", numframes);

	mod->numframes = numframes;

	pframetype = (dspriteframetype_t *)(pin + 1);

	for (i=0 ; i<numframes ; i++)
	{
		spriteframetype_t	frametype;

		frametype = (spriteframetype_t)LittleLong (pframetype->type);
		psprite->frames[i].type = frametype;

		if (frametype == SPR_SINGLE)
		{
			pframetype = (dspriteframetype_t *)	Mod_LoadSpriteFrame (pframetype + 1, &psprite->frames[i].frameptr, i);
		}
		else
		{
			pframetype = (dspriteframetype_t *)	Mod_LoadSpriteGroup (pframetype + 1, &psprite->frames[i].frameptr, i);
		}
	}

	mod->type = mod_sprite;
}

/*
================
R_GetSpriteFrame
================
*/
mspriteframe_t *R_GetSpriteFrame (entity_t *e)
{
	msprite_t		*psprite;
	mspritegroup_t	*pspritegroup;
	mspriteframe_t	*pspriteframe;
	int				i, numframes, frame;
	float			*pintervals, fullinterval, targettime, time;

	psprite	= (msprite_t *)e->model->cache.data;
	frame	= e->frame;

	if ((frame >= psprite->numframes) || (frame < 0))
	{
		Con_Printf ("R_DrawSprite: no such frame %d\n", frame);
		frame = 0;
	}

	if (psprite->frames[frame].type == SPR_SINGLE)
	{
		pspriteframe = psprite->frames[frame].frameptr;
	}
	else
	{
		pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		pintervals = pspritegroup->intervals;
		numframes = pspritegroup->numframes;
		fullinterval = pintervals[numframes-1];

		time = cl.time + e->syncbase;

	// when loading in Mod_LoadSpriteGroup, we guaranteed all interval values
	// are positive, so we don't have to worry about division by 0
		targettime = time - ((int)(time / fullinterval)) * fullinterval;

		for (i=0 ; i<(numframes-1) ; i++)
		{
			if (pintervals[i] > targettime)
				break;
		}

		pspriteframe = pspritegroup->frames[i];
	}

	return pspriteframe;
}

/*
=================
R_DrawSpriteModel
=================
*/
void R_DrawSpriteModel (entity_t *e)
{
	mspriteframe_t	*frame;
	vec3_t			right, up;
	msprite_t		*psprite;

	Sprite_EnsureVBO();
	if (!sprite_vbo_ready)
		return;

	// don't even bother culling, because it's just a single
	// polygon without a surface cache
	frame = R_GetSpriteFrame (e);
	psprite = (msprite_t *)e->model->cache.data;

	if (psprite->type == SPR_ORIENTED)
	{	// bullet marks on walls
		vec3_t forward;
		AngleVectors (e->angles, forward, right, up);
	}
	else
	{	// normal sprite
		VectorCopy(vup, up);
		VectorCopy(vright, right);
	}

	// Pre-scale the billboard offsets so the shader MVP stays the scene's
	// plain view-projection (no per-sprite glScalef needed).
	float s   = e->scale;
	float l   = frame->left  * s;
	float r   = frame->right * s;
	float dn  = frame->down  * s;
	float upv = frame->up    * s;

	vec3_t p_dl, p_ul, p_ur, p_dr;
	VectorMA (e->origin, dn,  up,    p_dl); VectorMA (p_dl, l, right, p_dl);
	VectorMA (e->origin, upv, up,    p_ul); VectorMA (p_ul, l, right, p_ul);
	VectorMA (e->origin, upv, up,    p_ur); VectorMA (p_ur, r, right, p_ur);
	VectorMA (e->origin, dn,  up,    p_dr); VectorMA (p_dr, r, right, p_dr);

	sprite_vertex_t verts[6] = {
		{p_dl[0], p_dl[1], p_dl[2], 0, 1},
		{p_ul[0], p_ul[1], p_ul[2], 0, 0},
		{p_ur[0], p_ur[1], p_ur[2], 1, 0},
		{p_dl[0], p_dl[1], p_dl[2], 0, 1},
		{p_ur[0], p_ur[1], p_ur[2], 1, 0},
		{p_dr[0], p_dr[1], p_dr[2], 1, 1},
	};

	float mvp[16];
	R_CurrentMVP(mvp);

	glDepthMask(GL_FALSE);

	glBindTexture (GL_TEXTURE_2D, frame->gl_texturenum);

	GLShader_Use(&R_SpriteShader);
	glUniformMatrix4fv(R_SpriteShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform4f       (R_SpriteShader_u_color, 1.0f, 1.0f, 1.0f, e->alpha);
	glUniform1i       (R_SpriteShader_u_tex, 0);

	DynamicVBO_Upload(&sprite_vbo, verts, sizeof(verts));
	DynamicVBO_Bind(&sprite_vbo);
	Prof_CountDraw(6);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	glBindVertexArray(0);
	glUseProgram(0);
	glDepthMask(GL_TRUE);
}
