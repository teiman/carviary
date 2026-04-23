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
// all MDL code goes into this file
//

#include "quakedef.h"
#include "gl_render.h"
#include "gl_profiler.h"

// Per-model VAO for the GPU path. Each model has its own (texcoord VBO, IBO)
// pair, so we allocate a VAO per model lazily on first draw. We cache inside
// the aliashdr_t -- we reuse gpu_vbo_texcoords layout so we do not store
// a separate field, but we DO need the VAO id. Using a GL object directly
// in aliashdr is simpler than adding yet another field.
static void Alias_EnsureGPU_VAO (aliashdr_t *hdr, GLuint *out_vao)
{
	// We stash the VAO in gpu_num_indices' high bits? No -- keep it simple,
	// use a small static map. For most scenes there's <256 alias models.
	// Key = ssbo handle (stable). Value = VAO handle.
	enum { MAP_SIZE = 256 };
	static GLuint keys[MAP_SIZE];
	static GLuint vaos[MAP_SIZE];
	static int    map_count = 0;

	for (int i = 0; i < map_count; ++i) {
		if (keys[i] == hdr->gpu_ssbo_poses) { *out_vao = vaos[i]; return; }
	}

	GLuint vao = 0;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	// Texcoord attribute (location 0 in alias_gpu shader).
	glBindBuffer(GL_ARRAY_BUFFER, hdr->gpu_vbo_texcoords);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (const void *)0);

	// Element buffer.
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, hdr->gpu_ibo);

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	if (map_count < MAP_SIZE) {
		keys[map_count] = hdr->gpu_ssbo_poses;
		vaos[map_count] = vao;
		map_count++;
	}
	*out_vao = vao;
}

// Shading state set up by R_DrawAliasModel before it calls this function:
//  - lightcolor  : RGB of the dynamic+static light sampled at the entity
//  - shadedots_mdl / shadedots2_mdl : the two anorm-dot rows being blended
//    between (ceil vs floor of the entity's yaw bucket)
//  - lightlerpoffset : 0..1 factor to blend those two rows
extern float   *shadedots_mdl;
extern float   *shadedots2_mdl;
extern vec3_t   lightcolor;
extern float    lightlerpoffset;
extern int      lastposenum;
extern int      lastposenum0;
extern float    r_avertexnormal_dots_mdl[16][256];

// Draw one alias-model frame using the shader-based path.
// The vertex shader reads two poses from an SSBO, interpolates position and
// lighting, and emits the final color. No triangulation or shading on CPU.
static void GL_DrawAliasBlendedFrame_Impl (int frame, aliashdr_t *hdr, entity_t *e)
{
	if (!R_EnsureAliasShader()) return;
	if (hdr->gpu_ssbo_poses == 0 || hdr->gpu_ibo == 0 || hdr->gpu_num_indices <= 0) return;

	// Resolve the two poses + blend factor exactly like the CPU path. This
	// logic also updates e->pose1 / e->pose2 / e->frame_start_time.
	if ((frame >= hdr->numframes) || (frame < 0)) frame = 0;
	int pose      = hdr->frames[frame].firstpose;
	int numposes  = hdr->frames[frame].numposes;
	if (numposes > 1) {
		e->frame_interval = hdr->frames[frame].interval;
		pose += (int)(cl.time / e->frame_interval) % numposes;
	} else {
		e->frame_interval = 0.1f;
	}

	float blend;
	if (e->pose2 != pose) {
		e->frame_start_time = realtime;
		e->pose1 = e->pose2;
		e->pose2 = pose;
		blend = 0;
	} else {
		blend = (float)((realtime - e->frame_start_time) * slowmo.value) / e->frame_interval;
		if (cl.paused || blend > 1) blend = 1;
	}

	lastposenum0 = e->pose1;
	lastposenum  = e->pose2;

	int pose_a_base = e->pose1 * hdr->poseverts;
	int pose_b_base = e->pose2 * hdr->poseverts;

	GLuint vao = 0;
	Alias_EnsureGPU_VAO(hdr, &vao);
	glBindVertexArray(vao);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, hdr->gpu_ssbo_poses);

	float mvp[16];
	R_CurrentMVP(mvp);

	GLShader_Use(&R_AliasShader);
	glUniformMatrix4fv(R_AliasShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform1i  (R_AliasShader_u_tex, 0);
	glUniform1i  (R_AliasShader_u_pose_a, pose_a_base);
	glUniform1i  (R_AliasShader_u_pose_b, pose_b_base);
	glUniform1f  (R_AliasShader_u_blend,  blend);
	glUniform1f  (R_AliasShader_u_alpha,  e->alpha);
	glUniform1i  (R_AliasShader_u_fullbright, e->model->fullbright ? 1 : 0);

	if (e->model->fullbright) {
		// fullbright ignores shade inputs, but set sane defaults.
		glUniform3f(R_AliasShader_u_shade_color, 1.0f, 1.0f, 1.0f);
		glUniform1f(R_AliasShader_u_lightlerpoffset, 0.0f);
	} else {
		glUniform3f(R_AliasShader_u_shade_color,
		            lightcolor[0], lightcolor[1], lightcolor[2]);
		glUniform1f(R_AliasShader_u_lightlerpoffset, lightlerpoffset);

		// Recover the yaw-angle row indices from the pointers the CPU path
		// sets up. r_avertexnormal_dots_mdl is a 16x256 table; the pointer
		// arithmetic gives us back the row number 0..15.
		int row_ceil  = (int)(shadedots_mdl  - &r_avertexnormal_dots_mdl[0][0]) / 256;
		int row_floor = (int)(shadedots2_mdl - &r_avertexnormal_dots_mdl[0][0]) / 256;
		glUniform1i(R_AliasShader_u_dot_row_ceil,  row_ceil  & 15);
		glUniform1i(R_AliasShader_u_dot_row_floor, row_floor & 15);
	}

	Prof_CountDraw(hdr->gpu_num_indices);
	glDrawElements(GL_TRIANGLES, hdr->gpu_num_indices, GL_UNSIGNED_SHORT, (const void *)0);

	glBindVertexArray(0);
	glUseProgram(0);
}

stvert_t	stverts[MAXALIASVERTS];
trivertx_t	*poseverts[MAXALIASFRAMES];
aliashdr_t	*pheader;
aliashdr_t	*paliashdr;
mtriangle_t	triangles[MAXALIASTRIS];
extern model_t	*loadmodel;
model_t		*aliasmodel;


// precalculated dot products for quantized angles
float	r_avertexnormal_dots_mdl[16][256] =
#include "anorm_dots.h"
;
float			*shadedots_mdl = r_avertexnormal_dots_mdl[0];
float			*shadedots2_mdl = r_avertexnormal_dots_mdl[0];
extern float	lightlerpoffset;

int		posenum;
int		lastposenum;
int		lastposenum0;
int		commands[65536];
int		numcommands;
int		vertexorder[8192];
int		numorder;
int		allverts, alltris;
int		stripverts[8192];	
int		striptris[8192];
int		stripcount;

extern	vec3_t  lightcolor;
extern	vec3_t	lightspot;
extern unsigned d_8to24table[];

extern char	loadname[32];	// for hunk tags

qboolean	used[8192];

// must be a power of 2
#define FLOODFILL_FIFO_SIZE 0x1000
#define FLOODFILL_FIFO_MASK (FLOODFILL_FIFO_SIZE - 1)
#define FLOODFILL_STEP( off, dx, dy ) \
{ \
	if (pos[off] == fillcolor) \
	{ \
		pos[off] = 255; \
		fifo[inpt].x = x + (dx), fifo[inpt].y = y + (dy); \
		inpt = (inpt + 1) & FLOODFILL_FIFO_MASK; \
	} \
	else if (pos[off] != 255) fdc = pos[off]; \
}
#define BOUNDI(VALUE,MIN,MAX) if (VALUE < MIN || VALUE >= MAX) Host_Error("model %s has an invalid VALUE (%d exceeds %d - %d)\n", mod->name, VALUE, MIN, MAX);
#define BOUNDF(VALUE,MIN,MAX) if (VALUE < MIN || VALUE >= MAX) Host_Error("model %s has an invalid VALUE (%f exceeds %f - %f)\n", mod->name, VALUE, MIN, MAX);

typedef struct
{
	short		x, y;
} floodfill_t;


/*
=================================================================

ALIAS MODEL DISPLAY LIST GENERATION

=================================================================
*/

/*
================
StripLength
================
*/
int	StripLength (int starttri, int startv)
{
	int			m1, m2;
	int			j;
	mtriangle_t	*last, *check;
	int			k;

	used[starttri] = 2;

	last = &triangles[starttri];

	stripverts[0] = last->vertindex[(startv)%3];
	stripverts[1] = last->vertindex[(startv+1)%3];
	stripverts[2] = last->vertindex[(startv+2)%3];

	striptris[0] = starttri;
	stripcount = 1;

	m1 = last->vertindex[(startv+2)%3];
	m2 = last->vertindex[(startv+1)%3];

	// look for a matching triangle
nexttri:
	for (j=starttri+1, check=&triangles[starttri+1] ; j<pheader->numtris ; j++, check++)
	{
		if (check->facesfront != last->facesfront)
			continue;
		for (k=0 ; k<3 ; k++)
		{
			if (check->vertindex[k] != m1)
				continue;
			if (check->vertindex[ (k+1)%3 ] != m2)
				continue;

			// this is the next part of the fan

			// if we can't use this triangle, this tristrip is done
			if (used[j])
				goto done;

			// the new edge
			if (stripcount & 1)
				m2 = check->vertindex[ (k+2)%3 ];
			else
				m1 = check->vertindex[ (k+2)%3 ];

			stripverts[stripcount+2] = check->vertindex[ (k+2)%3 ];
			striptris[stripcount] = j;
			stripcount++;

			used[j] = 2;
			goto nexttri;
		}
	}
done:

	// clear the temp used flags
	for (j=starttri+1 ; j<pheader->numtris ; j++)
		if (used[j] == 2)
			used[j] = 0;

	return stripcount;
}

/*
===========
FanLength
===========
*/
int	FanLength (int starttri, int startv)
{
	int		m1, m2;
	int		j;
	mtriangle_t	*last, *check;
	int		k;

	used[starttri] = 2;

	last = &triangles[starttri];

	stripverts[0] = last->vertindex[(startv)%3];
	stripverts[1] = last->vertindex[(startv+1)%3];
	stripverts[2] = last->vertindex[(startv+2)%3];

	striptris[0] = starttri;
	stripcount = 1;

	m1 = last->vertindex[(startv+0)%3];
	m2 = last->vertindex[(startv+2)%3];


	// look for a matching triangle
nexttri:
	for (j=starttri+1, check=&triangles[starttri+1] ; j<pheader->numtris ; j++, check++)
	{
		if (check->facesfront != last->facesfront)
			continue;
		for (k=0 ; k<3 ; k++)
		{
			if (check->vertindex[k] != m1)
				continue;
			if (check->vertindex[ (k+1)%3 ] != m2)
				continue;

			// this is the next part of the fan

			// if we can't use this triangle, this tristrip is done
			if (used[j])
				goto done;

			// the new edge
			m2 = check->vertindex[ (k+2)%3 ];

			stripverts[stripcount+2] = m2;
			striptris[stripcount] = j;
			stripcount++;

			used[j] = 2;
			goto nexttri;
		}
	}
done:

	// clear the temp used flags
	for (j=starttri+1 ; j<pheader->numtris ; j++)
		if (used[j] == 2)
			used[j] = 0;

	return stripcount;
}

/*
================
BuildTris

Generate a list of trifans or strips
for the model, which holds for all frames
================
*/
void BuildTris (void)
{
	int		i, j, k;
	int		startv;
	float	s, t;
	int		len, bestlen, besttype;
	int		bestverts[MAXALIASVERTS];	// Test
	int		besttris[MAXALIASVERTS];		// Test
	int		type;

	//
	// build tristrips
	//
	numorder = 0;
	numcommands = 0;
	memset (used, 0, sizeof(used));
	for (i=0 ; i<pheader->numtris ; i++)
	{
		// pick an unused triangle and start the trifan
		if (used[i])
			continue;

		bestlen = 0;
		for (type = 0 ; type < 2 ; type++)
//	type = 1;
		{
			for (startv =0 ; startv < 3 ; startv++)
			{
				if (type == 1)
					len = StripLength (i, startv);
				else
					len = FanLength (i, startv);
				if (len > bestlen)
				{
					besttype = type;
					bestlen = len;
					for (j=0 ; j<bestlen+2 ; j++)
						bestverts[j] = stripverts[j];
					for (j=0 ; j<bestlen ; j++)
						besttris[j] = striptris[j];
				}
			}
		}

		// mark the tris on the best strip as used
		for (j=0 ; j<bestlen ; j++)
			used[besttris[j]] = 1;

		if (besttype == 1)
			commands[numcommands++] = (bestlen+2);
		else
			commands[numcommands++] = -(bestlen+2);

		for (j=0 ; j<bestlen+2 ; j++)
		{
			// emit a vertex into the reorder buffer
			k = bestverts[j];
			vertexorder[numorder++] = k;

			// emit s/t coords into the commands stream
			s = stverts[k].s;
			t = stverts[k].t;
			if (!triangles[besttris[0]].facesfront && stverts[k].onseam)
				s += pheader->skinwidth * 0.5;	// Tomaz - Speed
			s = (s + 0.5) / pheader->skinwidth;
			t = (t + 0.5) / pheader->skinheight;

			*(float *)&commands[numcommands++] = s;
			*(float *)&commands[numcommands++] = t;
		}
	}

	commands[numcommands++] = 0;		// end of list marker

	Con_DPrintf ("%3i tri %3i vert %3i cmd\n", pheader->numtris, numorder, numcommands);

	allverts += numorder;
	alltris += pheader->numtris;
}

qboolean started_loading;

/*
================
Alias_BuildGPUData

Phase 1 of the GPU alias path. For each loaded MDL we create GL buffers the
shader path will use:

  - gpu_ssbo_poses: all `numposes` poses laid out contiguously. Each vertex
    is 4 bytes {byte v[3], byte lightnormalindex}. Indexed by the shader as
    (pose_index * poseverts + slot). Same layout as aliashdr->posedata.
  - gpu_vbo_texcoords: one {float s, t} per slot (poseverts entries).
    Extracted from the command list -- they are constant across poses.
  - gpu_ibo: triangle list (uint16 per index), built by pre-triangulating
    the command list's strips and fans into plain GL_TRIANGLES. No more
    per-frame retriangulation.

All buffers are GL_STATIC_DRAW / immutable once uploaded. They live for the
lifetime of the model.

This runs at the tail of GL_MakeAliasModelDisplayLists, after `commands[]`
and `posedata` are set up.
================
*/
static void Alias_BuildGPUData (aliashdr_t *hdr)
{
	const int poseverts = hdr->poseverts;
	const int numposes  = hdr->numposes;
	if (poseverts <= 0 || numposes <= 0) return;

	const int *cmds = (const int *)((byte *)hdr + hdr->commands);
	const trivertx_t *posedata = (const trivertx_t *)((byte *)hdr + hdr->posedata);

	// Pass 1: allocate per-slot texcoords + count triangles.
	// `poseverts` slots total (== numorder at build time), one (s,t) each.
	float *texcoords = (float *)malloc(poseverts * 2 * sizeof(float));
	if (!texcoords) return;
	// Default all slots to 0; only slots that appear in the command stream
	// get real coords. Unreferenced slots can't be indexed by the IBO, so
	// garbage there is harmless.
	memset(texcoords, 0, poseverts * 2 * sizeof(float));

	int num_triangles = 0;
	int slot = 0;
	const int *p = cmds;
	for (;;) {
		int count = *p++;
		if (count == 0) break;
		qboolean is_fan = (count < 0);
		int n = is_fan ? -count : count;
		if (slot + n > poseverts) break; // defensive

		for (int i = 0; i < n; ++i) {
			float s = ((const float *)p)[0];
			float t = ((const float *)p)[1];
			p += 2;
			texcoords[slot*2 + 0] = s;
			texcoords[slot*2 + 1] = t;
			slot++;
		}
		if (n >= 3) num_triangles += (n - 2);
	}

	if (num_triangles == 0) {
		free(texcoords);
		return;
	}

	// Pass 2: build the index list. We walk the command stream again, this
	// time tracking the slot numbers (which are just the running counter),
	// and emit (n-2) triangles per strip/fan.
	unsigned short *indices = (unsigned short *)malloc(num_triangles * 3 * sizeof(unsigned short));
	if (!indices) { free(texcoords); return; }

	int ibo_pos = 0;
	slot = 0;
	p = cmds;
	for (;;) {
		int count = *p++;
		if (count == 0) break;
		qboolean is_fan = (count < 0);
		int n = is_fan ? -count : count;
		p += n * 2; // skip the (s,t) pairs, we already extracted them

		int base = slot;
		if (is_fan) {
			for (int i = 1; i < n - 1; ++i) {
				indices[ibo_pos++] = (unsigned short)(base + 0);
				indices[ibo_pos++] = (unsigned short)(base + i);
				indices[ibo_pos++] = (unsigned short)(base + i + 1);
			}
		} else {
			// Triangle strip: winding flips every other tri.
			for (int i = 0; i < n - 2; ++i) {
				if (i & 1) {
					indices[ibo_pos++] = (unsigned short)(base + i + 1);
					indices[ibo_pos++] = (unsigned short)(base + i);
					indices[ibo_pos++] = (unsigned short)(base + i + 2);
				} else {
					indices[ibo_pos++] = (unsigned short)(base + i);
					indices[ibo_pos++] = (unsigned short)(base + i + 1);
					indices[ibo_pos++] = (unsigned short)(base + i + 2);
				}
			}
		}
		slot += n;
	}

	// Upload.
	GLuint ssbo = 0, vbo = 0, ibo = 0;
	glGenBuffers(1, &ssbo);
	glGenBuffers(1, &vbo);
	glGenBuffers(1, &ibo);

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
	glBufferData(GL_SHADER_STORAGE_BUFFER,
	             numposes * poseverts * sizeof(trivertx_t),
	             posedata, GL_STATIC_DRAW);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, poseverts * 2 * sizeof(float),
	             texcoords, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, ibo_pos * sizeof(unsigned short),
	             indices, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	hdr->gpu_ssbo_poses    = ssbo;
	hdr->gpu_vbo_texcoords = vbo;
	hdr->gpu_ibo           = ibo;
	hdr->gpu_num_indices   = ibo_pos;

	free(texcoords);
	free(indices);
}

/*
================
GL_MakeAliasModelDisplayLists
================
*/
void GL_MakeAliasModelDisplayLists (model_t *m, aliashdr_t *hdr)
{
	// Tomaz - Removed ms2 Begin
	int			i, j;
	int			*cmds;
	trivertx_t	*verts;

	aliasmodel	=	m;
	paliashdr	=	hdr;

	if (!started_loading)
	{
		started_loading = true;
//		Con_Printf ("Meshing models");
	}

//	Con_Printf (".");

	BuildTris ();		// trifans or lists

	// save the data out

	paliashdr->poseverts = numorder;

	cmds = (int *)Hunk_Alloc (numcommands * 4);
	paliashdr->commands = (byte *)cmds - (byte *)paliashdr;
	memcpy (cmds, commands, (numcommands * 4));

	verts = (trivertx_t *)Hunk_Alloc (paliashdr->numposes * paliashdr->poseverts * sizeof(trivertx_t));
	paliashdr->posedata = (byte *)verts - (byte *)paliashdr;
	for (i=0 ; i<paliashdr->numposes ; i++)
		for (j=0 ; j<numorder ; j++)
			*verts++ = poseverts[i][vertexorder[j]];
	// Tomaz - Removed ms2 End

	// Phase 1 of GPU alias path: build per-model static GL buffers.
	// Safe to call here -- no effect on the CPU render path, just adds data
	// for future shader use.
	Alias_BuildGPUData (hdr);
}

/*
=================================================================

ALIAS MODEL SKIN

=================================================================
*/

/*
=================
Mod_FloodFillSkin
=================
*/
void Mod_FloodFillSkin( byte *skin, int skinwidth, int skinheight )
{
	byte				fillcolor = *skin; // assume this is the pixel to fill
	floodfill_t			fifo[FLOODFILL_FIFO_SIZE];
	int					inpt = 0, outpt = 0;
	int					filledcolor = -1;
	int					i;

	if (filledcolor == -1)
	{
		filledcolor = 0;
		// attempt to find opaque black
		for (i = 0; i < 256; ++i)
			if (d_8to24table[i] == (255 << 0)) // alpha 1.0
			{
				filledcolor = i;
				break;
			}
	}

	// can't fill to filled color or to transparent color (used as visited marker)
	if ((fillcolor == filledcolor) || (fillcolor == 255))
	{
		//printf( "not filling skin from %d to %d\n", fillcolor, filledcolor );
		return;
	}

	fifo[inpt].x = 0, fifo[inpt].y = 0;
	inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;

	while (outpt != inpt)
	{
		int			x = fifo[outpt].x, y = fifo[outpt].y;
		int			fdc = filledcolor;
		byte		*pos = &skin[x + skinwidth * y];

		outpt = (outpt + 1) & FLOODFILL_FIFO_MASK;

		if (x > 0)				FLOODFILL_STEP( -1, -1, 0 );
		if (x < skinwidth - 1)	FLOODFILL_STEP( 1, 1, 0 );
		if (y > 0)				FLOODFILL_STEP( -skinwidth, 0, -1 );
		if (y < skinheight - 1)	FLOODFILL_STEP( skinwidth, 0, 1 );
		skin[x + skinwidth * y] = fdc;
	}
}
/*
===============
Mod_LoadAllSkins
===============
*/
void *Mod_LoadAllSkins (int numskins, daliasskintype_t *pskintype)
{
	int		i, j, k, size;
	char	name[64], model[64];
	int		s;
	byte	*skin, *data = NULL, *data2;
	byte	*texels;
	daliasskingroup_t		*pinskingroup;
	int		groupskins;
	daliasskininterval_t	*pinskinintervals;
//	int		modelname;
	
	skin = (byte *)(pskintype + 1);

	if (numskins < 1 || numskins > MAX_SKINS)
		Sys_Error ("Mod_LoadAliasModel: Invalid # of skins: %d\n", numskins);

	s = pheader->skinwidth * pheader->skinheight;

	for (i=0 ; i<numskins ; i++)
	{
		if (pskintype->type == ALIAS_SKIN_SINGLE)
		{
			Mod_FloodFillSkin( skin, pheader->skinwidth, pheader->skinheight );
			
			texels = (byte *)Hunk_AllocName(s, loadname);
			pheader->texels[i] = texels - (byte *)pheader;
			memcpy (texels, (byte *)(pskintype + 1), s);

			// TGA Begin
			// we check to see if a tga version of the skin exists, drawing happens elsewhere
			COM_StripExtension(loadmodel->name, model);
			_snprintf (name, sizeof(name),"%s_%i", model, i);

			pheader->transparent = true;

			pheader->gl_texturenum[i][0] =
			pheader->gl_texturenum[i][1] =
			pheader->gl_texturenum[i][2] =
			pheader->gl_texturenum[i][3] =

			loadtextureimage3 (name, false, true, data); // load texture "name"

			if (pheader->gl_texturenum[i][0] == 0)// did not find a matching TGA...		
			{
				data = (byte *)(pskintype + 1);
				pheader->transparent = false;
				pheader->gl_texturenum[i][0] =
				pheader->gl_texturenum[i][1] =
				pheader->gl_texturenum[i][2] =
				pheader->gl_texturenum[i][3] =

				GL_LoadTexture (name, pheader->skinwidth, pheader->skinheight, data, true, false, 1);

				size = pheader->skinwidth*pheader->skinheight;

				if (Has_Fullbrights (data, size))
				{
					data2 = (byte *)malloc (size);
					
					for (j = 0;j < size;j++)
					{
						if (data[j] > 223)
							data2[j] = data[j];
						else
							data2[j] = 255;
					}
					
					pheader->fb_texturenum[i][0] =
					pheader->fb_texturenum[i][1] =
					pheader->fb_texturenum[i][2] =
					pheader->fb_texturenum[i][3] =
						GL_LoadTexture (va("fbrm_%s",name), pheader->skinwidth, pheader->skinheight, data2, true, true, 1);

					free(data2);
				}
			}
			// TGA End

			pskintype = (daliasskintype_t *)((byte *)(pskintype+1) + s);
		}
		else
		{
			// animating skin group.  yuck.
			pskintype++;
			pinskingroup = (daliasskingroup_t *)pskintype;
			groupskins = LittleLong (pinskingroup->numskins);
			pinskinintervals = (daliasskininterval_t *)(pinskingroup + 1);

			pskintype = (daliasskintype_t *)(void *)(pinskinintervals + groupskins);

			for (j=0 ; j<groupskins ; j++)
			{
				Mod_FloodFillSkin( skin, pheader->skinwidth, pheader->skinheight );
				if (j == 0) {
					texels = (byte *)Hunk_AllocName(s, loadname);
					pheader->texels[i] = texels - (byte *)pheader;
					data = (byte *)(pskintype);
					memcpy (texels, data, s);
				}
				_snprintf (name, sizeof(name),"%s_%i_%i", loadmodel->name, i,j);
				pheader->gl_texturenum[i][j&3] = 
					GL_LoadTexture (name, pheader->skinwidth, pheader->skinheight, data, true, false, 1);
				
				size = pheader->skinwidth*pheader->skinheight;
				
				if (Has_Fullbrights (data, size))
				{
					data2 = (byte *)malloc (size);
					
					for (j = 0;j < size;j++)
					{
						if (data[j] > 223)
							data2[j] = data[j];
						else
							data2[j] = 255;
					}
					
					pheader->fb_texturenum[i][j&3] =
						GL_LoadTexture (va("fbrm_%s",name), pheader->skinwidth, pheader->skinheight, data2, true, true, 1);
					
					free(data2);
				}
				
				pskintype = (daliasskintype_t *)((byte *)(pskintype) + s);
			}
			k = j;
			for (/* */; j < 4; j++)
				pheader->gl_texturenum[i][j&3] = 
				pheader->gl_texturenum[i][j - k]; 
		}
	}


	return (void *)pskintype;
}

/*
=================================================================

ALIAS MODEL FRAMES

=================================================================
*/

int		aliasbboxmins[3], aliasbboxmaxs[3];

/*
=================
Mod_LoadAliasFrame
=================
*/
void * Mod_LoadAliasFrame (void * pin, maliasframedesc_t *frame)
{
	trivertx_t		*pinframe;
	int				i;
	daliasframe_t	*pdaliasframe;
	
	pdaliasframe = (daliasframe_t *)pin;

	strcpy (frame->name, pdaliasframe->name);
	frame->firstpose = posenum;
	frame->numposes = 1;

	for (i=0 ; i<3 ; i++)
	{
	// these are byte values, so we don't have to worry about
	// endianness
		frame->bboxmin.v[i] = pdaliasframe->bboxmin.v[i];
		frame->bboxmax.v[i] = pdaliasframe->bboxmax.v[i];

		aliasbboxmins[i] = min (frame->bboxmin.v[i], aliasbboxmins[i]);
		aliasbboxmaxs[i] = max (frame->bboxmax.v[i], aliasbboxmaxs[i]);
	}

	pinframe = (trivertx_t *)(pdaliasframe + 1);

	poseverts[posenum] = pinframe;
	posenum++;

	pinframe += pheader->numverts;

	return (void *)pinframe;
}

/*
=================
Mod_LoadAliasGroup
=================
*/
void *Mod_LoadAliasGroup (void * pin,  maliasframedesc_t *frame)
{
	daliasgroup_t		*pingroup;
	int					i, numframes;
	daliasinterval_t	*pin_intervals;
	void				*ptemp;
	
	pingroup = (daliasgroup_t *)pin;

	numframes = LittleLong (pingroup->numframes);

	frame->firstpose = posenum;
	frame->numposes = numframes;

	for (i=0 ; i<3 ; i++)
	{
	// these are byte values, so we don't have to worry about endianness
		frame->bboxmin.v[i] = pingroup->bboxmin.v[i];
		frame->bboxmax.v[i] = pingroup->bboxmax.v[i];

		aliasbboxmins[i] = min (frame->bboxmin.v[i], aliasbboxmins[i]);
		aliasbboxmaxs[i] = max (frame->bboxmax.v[i], aliasbboxmaxs[i]);
	}

	pin_intervals = (daliasinterval_t *)(pingroup + 1);

	frame->interval = LittleFloat (pin_intervals->interval);

	pin_intervals += numframes;

	ptemp = (void *)pin_intervals;

	for (i=0 ; i<numframes ; i++)
	{
		poseverts[posenum] = (trivertx_t *)((daliasframe_t *)ptemp + 1);
		posenum++;

		ptemp = (trivertx_t *)((daliasframe_t *)ptemp + 1) + pheader->numverts;
	}

	return ptemp;
}
/*
=============================================================

  ALIAS MODELS

=============================================================
*/
/*
=================
Mod_LoadAliasModel
=================
*/
void Mod_LoadAliasModel (model_t *mod, void *buffer)
{
	int					i, j;
	mdl_t				*pinmodel;
	stvert_t			*pinstverts;
	dtriangle_t			*pintriangles;
	int					version, numframes;
	int					size;
	daliasframetype_t	*pframetype;
	daliasskintype_t	*pskintype;
	int					start, end, total;
	
	start = Hunk_LowMark ();

	pinmodel = (mdl_t *)buffer;

	version = LittleLong (pinmodel->version);
	if (version != ALIAS_VERSION)
		Host_Error ("%s has wrong version number (%i should be %i)",
				 mod->name, version, ALIAS_VERSION);

//
// allocate space for a working header, plus all the data except the frames,
// skin and group info
//
	size = 	sizeof (aliashdr_t) + (LittleLong (pinmodel->numframes) - 1) * sizeof (pheader->frames[0]);
	pheader = (aliashdr_t *)Hunk_AllocName (size, loadname);
	
	mod->flags = LittleLong (pinmodel->flags);

//
// endian-adjust and copy the data, starting with the alias model header
//
	pheader->boundingradius = LittleFloat (pinmodel->boundingradius);
	pheader->numskins		= LittleLong  (pinmodel->numskins);
	pheader->skinwidth		= LittleLong  (pinmodel->skinwidth);
	pheader->skinheight		= LittleLong  (pinmodel->skinheight);
	pheader->numverts		= LittleLong  (pinmodel->numverts);
	pheader->numtris		= LittleLong  (pinmodel->numtris);
	pheader->numframes		= LittleLong  (pinmodel->numframes);

	BOUNDI(pheader->skinheight, 0, MAX_LBM_HEIGHT);
	BOUNDI(pheader->numverts  , 0, MAXALIASVERTS);
	BOUNDI(pheader->numtris   , 0, MAXALIASTRIS);
	BOUNDI(pheader->numframes , 1, MAXALIASFRAMES);

	numframes = pheader->numframes;

	pheader->size = LittleFloat (pinmodel->size) * ALIAS_BASE_SIZE_RATIO;
	mod->synctype = (synctype_t)LittleLong (pinmodel->synctype);
	mod->numframes = pheader->numframes;

	for (i=0 ; i<3 ; i++)
	{
		pheader->scale[i] = LittleFloat (pinmodel->scale[i]);
		pheader->scale_origin[i] = LittleFloat (pinmodel->scale_origin[i]);
		pheader->eyeposition[i] = LittleFloat (pinmodel->eyeposition[i]);
	}


//
// load the skins
//

	pskintype = (daliasskintype_t *)&pinmodel[1];
	pskintype = (daliasskintype_t *)Mod_LoadAllSkins (pheader->numskins, pskintype);		


//
// load base s and t vertices
//
	pinstverts = (stvert_t *)pskintype;

	for (i=0 ; i<pheader->numverts ; i++)
	{
		stverts[i].onseam = LittleLong (pinstverts[i].onseam);
		stverts[i].s = LittleLong (pinstverts[i].s);
		stverts[i].t = LittleLong (pinstverts[i].t);
	}

//
// load triangle lists
//
	pintriangles = (dtriangle_t *)&pinstverts[pheader->numverts];

	for (i=0 ; i<pheader->numtris ; i++)
	{
		triangles[i].facesfront = LittleLong (pintriangles[i].facesfront);

		for (j=0 ; j<3 ; j++)
			triangles[i].vertindex[j] =	LittleLong (pintriangles[i].vertindex[j]);
	}

//
// load the frames
//
	posenum = 0;
	pframetype = (daliasframetype_t *)&pintriangles[pheader->numtris];

	aliasbboxmins[0] = aliasbboxmins[1] = aliasbboxmins[2] = 255;
	aliasbboxmaxs[0] = aliasbboxmaxs[1] = aliasbboxmaxs[2] = -255;

	for (i=0 ; i<numframes ; i++)
	{
		if ((aliasframetype_t) LittleLong (pframetype->type) == ALIAS_SINGLE)
			pframetype = (daliasframetype_t *) Mod_LoadAliasFrame (pframetype + 1, &pheader->frames[i]);
		else
			pframetype = (daliasframetype_t *) Mod_LoadAliasGroup (pframetype + 1, &pheader->frames[i]);
	}

	pheader->numposes = posenum;

	mod->type = mod_alias;

	for (i = 0; i < 3; i++)
	{
		mod->mins[i] = aliasbboxmins[i] * pheader->scale[i] + pheader->scale_origin[i];
		mod->maxs[i] = aliasbboxmaxs[i] * pheader->scale[i] + pheader->scale_origin[i];
	}

	mod->glow_radius = 0.0f;
	VectorClear (mod->glow_color);

	if (!strcmp (mod->name, "progs/plasma.mdl"))
		mod->glow_radius = 6.0f;
	else if ((!strncmp (mod->name, "progs/glow_", 11)) ||
			(!strncmp (mod->name, "progs/bolt", 10))  ||
			(!strcmp (mod->name, "progs/laser.mdl")))
		mod->glow_radius = 24.0f;

	if (!strcmp (mod->name, "progs/missile.mdl"))
		VectorSet(mod->glow_color, 0.7f, 0.49f, 0.28f);
	else if (!strcmp (mod->name, "progs/plasma.mdl"))
		VectorSet(mod->glow_color, 0.0f, 0.7f, 0.0f);
	else if ((!strcmp (mod->name, "progs/bolt.mdl"))	||
			 (!strcmp (mod->name, "progs/laser.mdl")))
		VectorSet(mod->glow_color, 0.2f, 0.06f, 0.06f);
	else if ((!strcmp (mod->name, "progs/bolt2.mdl"))	||
			 (!strcmp (mod->name, "progs/bolt3.mdl")))
		VectorSet(mod->glow_color, 0.06f, 0.06f, 0.2f);

	mod->noshadow = false;

	if ((!strcmp (mod->name, "progs/lavaball.mdl"))	||
		(!strcmp (mod->name, "progs/laser.mdl"))	||
		(!strcmp (mod->name, "progs/boss.mdl"))		|| 
		(!strcmp (mod->name, "progs/oldone.mdl"))	|| 
		(!strcmp (mod->name, "progs/missile.mdl"))	||
		(!strcmp (mod->name, "progs/grenade.mdl"))	||
		(!strcmp (mod->name, "progs/spike.mdl"))	|| 
		(!strcmp (mod->name, "progs/s_spike.mdl"))	||
		(!strcmp (mod->name, "progs/zom_gib.mdl"))	||
		(!strncmp (mod->name, "progs/v_", 8))		||
		(!strncmp (mod->name, "progs/bolt", 10))	||
		(!strncmp (mod->name, "progs/gib", 9))		||
		(!strncmp (mod->name, "progs/h_", 8))		||
		(!strncmp (mod->name, "progs/flame", 11)))
		mod->noshadow = true;

	mod->fullbright = false;

	if (
		(!strcmp (mod->name, "progs/laser")) ||
		(!strcmp (mod->name, "progs/lavaball.mdl")) ||
		(!strncmp (mod->name, "progs/bolt", 10)) ||
		(!strncmp (mod->name, "progs/flame", 11)) 
		)
		mod->fullbright = true;

	//
	// build the draw lists
	//
	GL_MakeAliasModelDisplayLists (mod, pheader);

//
// move the complete, relocatable alias model to the cache
//	
	end = Hunk_LowMark ();
	total = end - start;
	
	Cache_Alloc (&mod->cache, total, loadname);
	if (!mod->cache.data)
		return;
	memcpy (mod->cache.data, pheader, total);

	Hunk_FreeToLowMark (start);
}


/*
=============
GL_DrawAliasBlendedFrame

Thin forwarder to the GPU path. Kept as a separate symbol because
R_DrawAliasModel still calls it (potentially twice: once for diffuse, once
for fullbright overlay). The actual work lives in
GL_DrawAliasBlendedFrame_GPU, which does pose interpolation + shading in
the vertex shader.
=============
*/
void GL_DrawAliasBlendedFrame (int frame, aliashdr_t *paliashdr, entity_t* e)
{
	if (paliashdr->gpu_ssbo_poses == 0)
		return; // Alias_BuildGPUData didn't run or failed for this model

	// When the whole model is fullbright (progs/flame*.mdl, lavaball,
	// etc.) every pixel it writes belongs in the glow mask. The caller
	// also enables this wrapper for the fb_texturenum second-pass overlay.
	qboolean route_to_mask = (e && e->model && e->model->fullbright);
	if (route_to_mask) PostFX_BeginFullbrightMask();
	GL_DrawAliasBlendedFrame_Impl(frame, paliashdr, e);
	if (route_to_mask) PostFX_EndFullbrightMask();
}
