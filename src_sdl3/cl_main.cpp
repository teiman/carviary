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
// cl_main.c  -- client main loop

#include "quakedef.h"

// we need to declare some mouse variables here, because the menu system
// references them even when on a unix system.

// these two are not intended to be set directly
cvar_t	cl_name = {"_cl_name", "player", true};
cvar_t	cl_color = {"_cl_color", "0", true};

cvar_t	cl_shownet = {"cl_shownet","0"};	// can be 0, 1, or 2
cvar_t	cl_nolerp = {"cl_nolerp","0"};

cvar_t	lookspring = {"lookspring","0", true};
cvar_t	lookstrafe = {"lookstrafe","0", true};
cvar_t	sensitivity = {"sensitivity","3", true};

cvar_t	m_pitch = {"m_pitch","0.022", true};
cvar_t	m_yaw = {"m_yaw","0.022", true};
cvar_t	m_forward = {"m_forward","1", true};
cvar_t	m_side = {"m_side","0.8", true};


client_static_t	cls;
client_state_t	cl;
// FIXME: put these on hunk?
efrag_t			cl_efrags[MAX_EFRAGS];
entity_t		cl_entities[MAX_EDICTS];
entity_t		cl_static_entities[MAX_STATIC_ENTITIES];
lightstyle_t	cl_lightstyle[MAX_LIGHTSTYLES];
dlight_t		cl_dlights[MAX_DLIGHTS];

int				cl_numvisedicts;
entity_t		*cl_visedicts[MAX_VISEDICTS];

/*
=====================
CL_ClearState

=====================
*/
void CL_ClearState (void)
{
	int			i;

	if (!sv.active)
		Host_ClearMemory ();

// wipe the entire cl structure
	memset (&cl, 0, sizeof(cl));

	SZ_Clear (&cls.message);

	// reset decoupled-camera state so the new level snaps to the player's angles
	{
		extern qboolean cam_viewangles_init;
		cam_viewangles_init = false;
	}

// clear other arrays	
	memset (cl_efrags,			0, sizeof(cl_efrags));
	memset (cl_entities,		0, sizeof(cl_entities));
	memset (cl_dlights,			0, sizeof(cl_dlights));
	memset (cl_lightstyle,		0, sizeof(cl_lightstyle));
	memset (cl_temp_entities,	0, sizeof(cl_temp_entities));
	memset (cl_beams,			0, sizeof(cl_beams));

//
// allocate the efrags and chain together into a free list
//
	cl.free_efrags = cl_efrags;
	for (i=0 ; i<MAX_EFRAGS-1 ; i++)
		cl.free_efrags[i].entnext = &cl.free_efrags[i+1];
	cl.free_efrags[i].entnext = NULL;
}

/*
=====================
CL_Disconnect

Sends a disconnect message to the server
This is also called on Host_Error, so it shouldn't cause any errors
=====================
*/
void CL_Disconnect (void)
{
// stop sounds (especially looping!)
	S_StopAllSounds (true);

// bring the console down and fade the colors back to normal
//	SCR_BringDownConsole ();

// if running a local server, shut it down
	if (cls.demoplayback)
		CL_StopPlayback ();
	else if (cls.state == ca_connected)
	{
		if (cls.demorecording)
			CL_Stop_f ();

		Con_DPrintf ("Sending clc_disconnect\n");
		SZ_Clear (&cls.message);
		MSG_WriteByte (&cls.message, clc_disconnect);
		NET_SendUnreliableMessage (cls.netcon, &cls.message);
		SZ_Clear (&cls.message);
		NET_Close (cls.netcon);

		cls.state = ca_disconnected;
		if (sv.active)
			Host_ShutdownServer(false);
	}

	cls.demoplayback = cls.timedemo = false;
	cls.signon = 0;
}

void CL_Disconnect_f (void)
{
	CL_Disconnect ();
	if (sv.active)
		Host_ShutdownServer (false);
}




/*
=====================
CL_EstablishConnection

Host should be either "local" or a net address to be passed on
=====================
*/
void CL_EstablishConnection (char *host)
{
	if (cls.state == ca_dedicated)
		return;

	if (cls.demoplayback)
		return;

	CL_Disconnect ();

	cls.netcon = NET_Connect (host);
	if (!cls.netcon)
		Host_Error ("CL_Connect: connect failed\n");
	Con_DPrintf ("CL_EstablishConnection: connected to %s\n", host);
	
	cls.demonum = -1;			// not in the demo loop now
	cls.state = ca_connected;
	cls.signon = 0;				// need all the signon messages before playing
}

/*
=====================
CL_SignonReply

An svc_signonnum has been received, perform a client side setup
=====================
*/
void CL_SignonReply (void)
{
	char 	str[8192];

Con_DPrintf ("CL_SignonReply: %i\n", cls.signon);

	switch (cls.signon)
	{
	case 1:
		MSG_WriteByte (&cls.message, clc_stringcmd);
		MSG_WriteString (&cls.message, "prespawn");
		break;
		
	case 2:		
		MSG_WriteByte (&cls.message, clc_stringcmd);
		MSG_WriteString (&cls.message, va("name \"%s\"\n", cl_name.string));
	
		MSG_WriteByte (&cls.message, clc_stringcmd);
		MSG_WriteString (&cls.message, va("color %i %i\n", ((int)cl_color.value)>>4, ((int)cl_color.value)&15));
	
		MSG_WriteByte (&cls.message, clc_stringcmd);
		_snprintf (str, sizeof(str), "spawn %s", cls.spawnparms);
		MSG_WriteString (&cls.message, str);
		break;
		
	case 3:	
		MSG_WriteByte (&cls.message, clc_stringcmd);
		MSG_WriteString (&cls.message, "begin");
		Cache_Report ();		// print remaining memory
		break;
		
	case 4:
		SCR_EndLoadingPlaque ();		// allow normal screen updates
		break;
	}
}

/*
=====================
CL_NextDemo

Called to play the next demo in the demo loop
=====================
*/
void CL_NextDemo (void)
{
	char	str[1024];

	if (cls.demonum == -1)
		return;		// don't play demos

	SCR_BeginLoadingPlaque ();

	if (!cls.demos[cls.demonum][0] || cls.demonum == MAX_DEMOS)
	{
		cls.demonum = 0;
		if (!cls.demos[cls.demonum][0])
		{
			Con_Printf ("No demos listed with startdemos\n");
			cls.demonum = -1;
			CL_Disconnect ();
			return;
		}
	}

	_snprintf (str,sizeof(str),"playdemo %s\n", cls.demos[cls.demonum]);
	Cbuf_InsertText (str);
	cls.demonum++;
}

/*
==============
CL_Lightstyle_f

Inspect / override lightstyle channels locally (client only). The server
will overwrite the channel on the next svc_lightstyle update, so this is
purely a debug/visualization tool.

  lightstyle                -> print usage
  lightstyle show           -> list every non-empty style
  lightstyle <n> <string>   -> set style <n> to <string> (letters 'a'..'z')
==============
*/
void CL_Lightstyle_f (void)
{
	int argc = Cmd_Argc();
	if (argc < 2) {
		Con_Printf ("usage:\n");
		Con_Printf ("  lightstyle show            list current lightstyles\n");
		Con_Printf ("  lightstyle <n> <string>    set style <n> (0..%d) to <string>\n", MAX_LIGHTSTYLES - 1);
		Con_Printf ("examples:\n");
		Con_Printf ("  lightstyle 0 z             channel 0 full bright\n");
		Con_Printf ("  lightstyle 0 a             channel 0 pitch black\n");
		Con_Printf ("  lightstyle 1 mamamamabama  flicker\n");
		Con_Printf ("  lightstyle 2 abcdefghijklmnopqrrqponmlkjihgfedcba  slow pulse\n");
		return;
	}

	if (!Q_strcasecmp(Cmd_Argv(1), "show")) {
		int shown = 0;
		for (int i = 0; i < MAX_LIGHTSTYLES; ++i) {
			if (cl_lightstyle[i].length <= 0) continue;
			Con_Printf ("  %2d: \"%s\"\n", i, cl_lightstyle[i].map);
			shown++;
		}
		if (shown == 0) Con_Printf ("(no lightstyles set)\n");
		else            Con_Printf ("%d style(s)\n", shown);
		return;
	}

	if (argc != 3) {
		Con_Printf ("usage: lightstyle <n> <string>  (run `lightstyle` alone for help)\n");
		return;
	}

	int n = Q_atoi (Cmd_Argv(1));
	if (n < 0 || n >= MAX_LIGHTSTYLES) {
		Con_Printf ("lightstyle: channel %d out of range (0..%d)\n", n, MAX_LIGHTSTYLES - 1);
		return;
	}

	const char *src = Cmd_Argv(2);
	int len = (int)strlen(src);
	if (len <= 0) {
		Con_Printf ("lightstyle: empty string; use e.g. \"m\" for normal\n");
		return;
	}
	if (len >= MAX_STYLESTRING) {
		Con_Printf ("lightstyle: string too long (max %d)\n", MAX_STYLESTRING - 1);
		return;
	}
	for (int k = 0; k < len; ++k) {
		char c = src[k];
		if (c < 'a' || c > 'z') {
			Con_Printf ("lightstyle: only lowercase 'a'..'z' accepted (got '%c')\n", c);
			return;
		}
	}

	Q_strcpy (cl_lightstyle[n].map, (char *)src);
	cl_lightstyle[n].length = len;
	Con_Printf ("lightstyle %d = \"%s\"\n", n, src);
}

/*
==============
CL_PrintEntities_f
==============
*/
void CL_PrintEntities_f (void)
{
	entity_t	*ent;
	int			i;
	
	for (i=0,ent=cl_entities ; i<cl.num_entities ; i++,ent++)
	{
		Con_Printf ("%3i:",i);
		if (!ent->model)
		{
			Con_Printf ("EMPTY\n");
			continue;
		}
		Con_Printf ("%s:%2i  (%5.1f,%5.1f,%5.1f) [%5.1f %5.1f %5.1f]\n"
		,ent->model->name,ent->frame, ent->origin[0], ent->origin[1], ent->origin[2], ent->angles[0], ent->angles[1], ent->angles[2]);
	}
}

/*
===============
CL_AllocDlight
===============
*/
dlight_t *CL_AllocDlight (int key)
{
	int			i;
	dlight_t	*dl;

// Default color is white. Callers that want tinted dlights (lava, torches,
// tarbaby explosion, etc.) overwrite dl->color[] after the Alloc. Leaving
// the default at (1, 0.5, 0.25) meant every caller that forgot -- muzzleflash,
// rocket explosion, EXPLOSION2 -- painted the walls orange.
// Initialize color white and cone in omnidirectional mode (cone_outer > 1
// means every direction is inside the cone, so the angular falloff in
// R_AddDynamicLights collapses to 1.0 everywhere).
#define DLIGHT_DEFAULT_COLOR() do { \
	dl->color[0] = 1; dl->color[1] = 1; dl->color[2] = 1; \
	dl->cone_outer = 2.0f; dl->cone_inner = 2.0f; \
	dl->cone_dir[0] = 0; dl->cone_dir[1] = 0; dl->cone_dir[2] = 1; \
} while (0)

// first look for an exact key match
	if (key)
	{
		dl = cl_dlights;
		for (i=0 ; i<MAX_DLIGHTS ; i++, dl++)
		{
			if (dl->key == key)
			{
				memset (dl, 0, sizeof(*dl));
				dl->key = key;
				DLIGHT_DEFAULT_COLOR();
				return dl;
			}
		}
	}

// then look for anything else
	dl = cl_dlights;
	for (i=0 ; i<MAX_DLIGHTS ; i++, dl++)
	{
		if (dl->die < cl.time)
		{
			memset (dl, 0, sizeof(*dl));
			dl->key = key;
			DLIGHT_DEFAULT_COLOR();
			return dl;
		}
	}

	dl = &cl_dlights[0];
	memset (dl, 0, sizeof(*dl));
	dl->key = key;
	DLIGHT_DEFAULT_COLOR();
	return dl;
}


/*
===============
CL_DecayLights
===============
*/
void CL_DecayLights (void)
{
	int			i;
	dlight_t	*dl;
	float		time;
	
	time = cl.time - cl.oldtime;

	dl = cl_dlights;
	for (i=0 ; i<MAX_DLIGHTS ; i++, dl++)
	{
		if (dl->die < cl.time || !dl->radius)
			continue;
		
		dl->radius -= time*dl->decay;
		if (dl->radius < 0)
			dl->radius = 0;
	}
}

/*
===============
CL_LerpPoint

Determines the fraction between the last two messages that the objects
should be put at.
===============
*/
float	CL_LerpPoint (void)
{
	float	f, frac;

	f = cl.mtime[0] - cl.mtime[1];

	if (!f || cls.timedemo || sv.active)
	{
		cl.time = cl.mtime[0];
		return 1;
	}

	if (f > 0.1)
	{	// dropped packet, or start of demo
		cl.mtime[1] = cl.mtime[0] - 0.1;
		f = 0.1f;
	}
	frac = (cl.time - cl.mtime[1]) / f;

	if (cl_nolerp.value)
		return 1;
//Con_Printf ("frac: %f\n",frac);
	if (frac < 0)
	{
		if (frac < -0.01)
		{
			cl.time = cl.mtime[1];
//				Con_Printf ("low frac\n");
		}
		frac = 0;
	}
	else if (frac > 1)
	{
		if (frac > 1.01)
		{
			cl.time = cl.mtime[0];
//				Con_Printf ("high frac\n");
		}
		frac = 1;
	}
	return frac;
}


/*
===============
CL_RelinkEntities
===============
*/
void CL_RelinkEntities (void)
{
	entity_t	*ent;
	int			i, j;
	float		frac, f, d;
	vec3_t		delta;
	float		bobjrotate;
	vec3_t		oldorg;
	dlight_t	*dl;

// determine partial update time	
	frac = CL_LerpPoint ();

	cl_numvisedicts = 0;

//
// interpolate player info
//
	cl.velocity[0] = cl.mvelocity[1][0] + frac * (cl.mvelocity[0][0] - cl.mvelocity[1][0]);
	cl.velocity[1] = cl.mvelocity[1][1] + frac * (cl.mvelocity[0][1] - cl.mvelocity[1][1]);
	cl.velocity[2] = cl.mvelocity[1][2] + frac * (cl.mvelocity[0][2] - cl.mvelocity[1][2]);

	if (cls.demoplayback)
	{
	// interpolate the angles	
		for (j=0 ; j<3 ; j++)
		{
			d = cl.mviewangles[0][j] - cl.mviewangles[1][j];
			if (d > 180)
				d -= 360;
			else if (d < -180)
				d += 360;
			cl.viewangles[j] = cl.mviewangles[1][j] + frac*d;
		}
	}
	
	bobjrotate = anglemod(100*cl.time);
	
// start on the entity after the world
	for (i=1,ent=cl_entities+1 ; i<cl.num_entities ; i++,ent++)
	{
		if (!ent->model)
		{	// empty slot
			continue;
		}

// if the object wasn't included in the last packet, remove it
		if (ent->msgtime != cl.mtime[0])
		{
			ent->model = NULL;
			// Tomaz - Model Transform Interpolation Begin
			ent->translate_start_time = 0;
			ent->rotate_start_time    = 0;
			VectorClear (ent->last_light);
			// Tomaz - Model Transform Interpolation End
			continue;
		}

		VectorCopy (ent->origin, oldorg);

		if (ent->forcelink)
		{	// the entity was not updated in the last message
			// so move to the final spot
			VectorCopy (ent->msg_origins[0], ent->origin);
			VectorCopy (ent->msg_angles[0], ent->angles);
		}
		else
		{	// if the delta is large, assume a teleport and don't lerp
			f = frac;
			for (j=0 ; j<3 ; j++)
			{
				delta[j] = ent->msg_origins[0][j] - ent->msg_origins[1][j];
				if (delta[j] > 100 || delta[j] < -100)
					f = 1;		// assume a teleportation, not a motion
			}

			// Tomaz - Model Transform Interpolation Begin
			if (f >= 1)
			{
				ent->translate_start_time = 0;
				ent->rotate_start_time    = 0;
				VectorClear (ent->last_light);
			}
			// Tomaz - Model Transform Interpolation End
		// interpolate the origin and angles
			for (j=0 ; j<3 ; j++)
			{
				ent->origin[j] = ent->msg_origins[1][j] + f*delta[j];

				d = ent->msg_angles[0][j] - ent->msg_angles[1][j];
				if (d > 180)
					d -= 360;
				else if (d < -180)
					d += 360;
				ent->angles[j] = ent->msg_angles[1][j] + f*d;
			}
			
		}

		if (ent->effects)
		{
			if (ent->effects & EF_MUZZLEFLASH)
			{
				vec3_t		fv, rv, uv;

				dl = CL_AllocDlight (i);
				VectorCopy (ent->origin,  dl->origin);
				dl->origin[2] += 16;
				AngleVectors (ent->angles, fv, rv, uv);
				 
				VectorMA (dl->origin, 18, fv, dl->origin);
				dl->radius = 250;
				dl->minlight = 32;
				dl->die = cl.time + 0.1;
			}
			
			if (ent->effects & EF_BRIGHTLIGHT)
			{			
				dl = CL_AllocDlight (i);
				VectorCopy (ent->origin,  dl->origin);
				dl->origin[2] += 16;
				dl->radius = 350;
				dl->die = cl.time + 0.001;
				dl->color[0] = 1.0f;
				dl->color[1] = 1.0f;
				dl->color[2] = 1.0f;
			}
			
			if (ent->effects & EF_DIMLIGHT)
			{			
				dl = CL_AllocDlight (i);
				VectorCopy (ent->origin,  dl->origin);
				dl->radius = 250 + (rand()&31);
				dl->die = cl.time + 0.001;
				dl->color[0] = 1.0f;
				dl->color[1] = 1.0f;
				dl->color[2] = 1.0f;
			}

			if (ent->effects & EF_BRIGHTFIELD)
				R_EntityParticles (ent);
		}

		if (ent->model->flags)
		{
			// rotate binary objects locally
			// Tomaz - Bobbing Items
			if (ent->model->flags & EF_ROTATE)
			{	
				ent->angles[1] = bobjrotate;
				if (r_bobbing.value)
					ent->origin[2] += (( sin(bobjrotate/90*M_PI) * 5) + 5 );
			}
			// Tomaz - Bobbing Items

			if (ent->model->flags & EF_ROCKET)
			{
				R_TrueTrail_Missile (oldorg, ent->origin, ent);
				dl = CL_AllocDlight (i);
				VectorCopy (ent->origin, dl->origin);
				dl->radius = 120;
				dl->die = cl.time + 0.01;
			}
			else if (ent->model->flags & EF_GRENADE)
				R_TrueTrail_Missile (oldorg, ent->origin, ent);
			
			else if (ent->model->flags & EF_GIB)
				R_BloodTrail (oldorg, ent->origin, ent);
			
			else if (ent->model->flags & EF_ZOMGIB)
				R_BloodTrail (oldorg, ent->origin, ent);
			
			else if (ent->model->flags & EF_TRACER)
			{
				R_TracerTrail (oldorg, ent->origin, ent, 63);
				dl = CL_AllocDlight (i);
				VectorCopy (ent->origin, dl->origin);
				dl->radius = 250;
				dl->die = cl.time + 0.01;
				dl->color[0] = 0.42f;
				dl->color[1] = 0.42f;
				dl->color[2] = 0.06f;
			}
			
			else if (ent->model->flags & EF_TRACER2)
			{
				R_TracerTrail (oldorg, ent->origin, ent, 236);
				dl = CL_AllocDlight (i);
				VectorCopy (ent->origin, dl->origin);
				dl->radius = 250;
				dl->die = cl.time + 0.01;
				dl->color[0] = 0.88f;
				dl->color[1] = 0.58f;
				dl->color[2] = 0.31f;
			}
			
			else if (ent->model->flags & EF_TRACER3)
			{
				R_VoorTrail (oldorg, ent->origin, ent);
				dl = CL_AllocDlight (i);
				VectorCopy (ent->origin, dl->origin);
				dl->radius = 250;
				dl->die = cl.time + 0.01;
				dl->color[0] = 0.73f;
				dl->color[1] = 0.45f;
				dl->color[2] = 0.62f;
			}
		}

		// True-trail for spikes / wizard spikes. Stock Quake has no EF_* flag
		// for these, so they're identified by model name. Must run outside
		// the `ent->model->flags` branch above because the spike models
		// carry no effect flags.
		if (ent->model->name[0] == 'p')
		{
			if (!strcmp(ent->model->name, "progs/spike.mdl") ||
			    !strcmp(ent->model->name, "progs/s_spike.mdl"))
			{
				R_TrueTrail_Spike (oldorg, ent->origin, ent);
			}
			else if (!strcmp(ent->model->name, "progs/w_spike.mdl"))
			{
				R_TrueTrail_WizSpike (oldorg, ent->origin, ent);
			}
		}

		// Tomaz - QC Glow Begin
		if (ent->glow_size)
		{
			dl = CL_AllocDlight (i);
			VectorCopy (ent->origin, dl->origin);
			dl->radius = ent->glow_size;
			dl->die = cl.time + 0.01;
			dl->color[0] = ent->glow_red;
			dl->color[1] = ent->glow_green;
			dl->color[2] = ent->glow_blue;
		}
		// Tomaz - QC Glow End

		ent->forcelink = false;

		if (i == cl.viewentity && !chase_active.value)
			continue;

		if (cl_numvisedicts < MAX_VISEDICTS)
		{
			cl_visedicts[cl_numvisedicts] = ent;
			cl_numvisedicts++;
		}
	}
}

/*
===============
CL_ReadFromServer

Read all incoming data from the server
===============
*/
int CL_ReadFromServer (void)
{
	int		ret;

	cl.oldtime = cl.time;
	cl.time += host_frametime;
	
	do
	{
		ret = CL_GetMessage ();
		if (ret == -1)
			Host_Error ("CL_ReadFromServer: lost server connection");
		if (!ret)
			break;
		
		cl.last_received_message = realtime;
		CL_ParseServerMessage ();
	} while (ret && cls.state == ca_connected);
	
	if (cl_shownet.value)
		Con_Printf ("\n");

	CL_RelinkEntities ();
	CL_UpdateTEnts ();

//
// bring the links up to date
//
	return 0;
}

/*
=================
CL_SendCmd
=================
*/
void CL_SendCmd (void)
{
	usercmd_t		cmd;

	if (cls.state != ca_connected)
		return;

	if (cls.signon == SIGNONS)
	{
	// get basic movement from keyboard
		CL_BaseMove (&cmd);
	
	// allow mice or other external controllers to add to the move
		IN_Move (&cmd);
	
	// send the unreliable message
		CL_SendMove (&cmd);
	
	}

	if (cls.demoplayback)
	{
		SZ_Clear (&cls.message);
		return;
	}
	
// send the reliable message
	if (!cls.message.cursize)
		return;		// no message at all
	
	if (!NET_CanSendMessage (cls.netcon))
	{
		Con_DPrintf ("CL_WriteToServer: can't send\n");
		return;
	}

	if (NET_SendMessage (cls.netcon, &cls.message) == -1)
		Host_Error ("CL_WriteToServer: lost server connection");

	SZ_Clear (&cls.message);
}

/*
=================
CL_Init
=================
*/
void CL_Init (void)
{
	SZ_Alloc (&cls.message, 1024);

	CL_InitInput ();
	CL_InitTEnts ();
	
//
// register our commands
//
	Cvar_RegisterVariable (&cl_name);
	Cvar_RegisterVariable (&cl_color);
	Cvar_RegisterVariable (&cl_upspeed);
	Cvar_RegisterVariable (&cl_forwardspeed);
	Cvar_RegisterVariable (&cl_backspeed);
	Cvar_RegisterVariable (&cl_sidespeed);
	Cvar_RegisterVariable (&cl_movespeedkey);
	Cvar_RegisterVariable (&cl_yawspeed);
	Cvar_RegisterVariable (&cl_pitchspeed);
	Cvar_RegisterVariable (&cl_anglespeedkey);
	Cvar_RegisterVariable (&cl_shownet);
	Cvar_RegisterVariable (&cl_nolerp);
	Cvar_RegisterVariable (&lookspring);
	Cvar_RegisterVariable (&lookstrafe);
	Cvar_RegisterVariable (&sensitivity);
	Cvar_RegisterVariable (&in_mlook); // Tomaz - MouseLook
	Cvar_RegisterVariable (&m_pitch);
	Cvar_RegisterVariable (&m_yaw);
	Cvar_RegisterVariable (&m_forward);
	Cvar_RegisterVariable (&m_side);

//	Cvar_RegisterVariable (&cl_autofire);
	
	Cmd_AddCommand ("entities", CL_PrintEntities_f);
	Cmd_AddCommand ("disconnect", CL_Disconnect_f);
	Cmd_AddCommand ("record", CL_Record_f);
	Cmd_AddCommand ("stop", CL_Stop_f);
	Cmd_AddCommand ("playdemo", CL_PlayDemo_f);
	Cmd_AddCommand ("timedemo", CL_TimeDemo_f);
	Cmd_AddCommand ("profile", CL_Profile_f);
	Cmd_AddCommand ("playprofile", CL_PlayProfile_f);
	Cmd_AddCommand ("profile_live_start", CL_ProfileLiveStart_f);
	Cmd_AddCommand ("profile_live_stop", CL_ProfileLiveStop_f);
	Cmd_AddCommand ("lightstyle", CL_Lightstyle_f);
}

