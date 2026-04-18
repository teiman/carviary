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
#include <time.h>
#include <sys/stat.h>
#include <direct.h>
#include <float.h>

// profile stats accumulated per frame
static double	profile_frame_min = 999.0;
static double	profile_frame_max = 0.0;
static double	profile_frame_last = 0.0;
static int		profile_total_wpoly = 0;
static int		profile_total_epoly = 0;

extern int c_brush_polys, c_alias_polys;
extern int glwidth, glheight;

void CL_FinishTimeDemo (void);

/*
==============================================================================

DEMO CODE

When a demo is playing back, all NET_SendMessages are skipped, and
NET_GetMessages are read from the demo file.

Whenever cl.time gets past the last received message, another message is
read from the demo file.
==============================================================================
*/

/*
==============
CL_StopPlayback

Called when a demo file runs out, or the user starts a game
==============
*/
void CL_StopPlayback (void)
{
	if (!cls.demoplayback)
		return;

	fclose (cls.demofile);
	cls.demoplayback = false;
	cls.demofile = NULL;
	cls.state = ca_disconnected;

	if (cls.timedemo)
		CL_FinishTimeDemo ();

	cls.playprofile = false;
	cls.pp_framecount = 0;
	cls.pp_shotcount = 0;
}

/*
====================
CL_WriteDemoMessage

Dumps the current net message, prefixed by the length and view angles
====================
*/
void CL_WriteDemoMessage (void)
{
	int		len;
	float	f[3];

	len = LittleLong (net_message.cursize);
	fwrite (&len, 4, 1, cls.demofile);

	f[0] = LittleFloat (cl.viewangles[0]);
	f[1] = LittleFloat (cl.viewangles[1]);
	f[2] = LittleFloat (cl.viewangles[2]);

	fwrite (f, 12, 1, cls.demofile);

	fwrite (net_message.data, net_message.cursize, 1, cls.demofile);
	fflush (cls.demofile);
}

/*
====================
CL_GetMessage

Handles recording and playback of demos, on top of NET_ code
====================
*/
int CL_GetMessage (void)
{
	int		r, i;
	
	if	(cls.demoplayback)
	{
	// decide if it is time to grab the next message		
		if (cls.signon == SIGNONS)	// allways grab until fully connected
		{
			if (cls.timedemo)
			{
				if (host_framecount == cls.td_lastframe)
					return 0;		// allready read this frame's message
				cls.td_lastframe = host_framecount;

				if (cls.playprofile)
					cls.pp_framecount++;
			// if this is the second frame, grab the real td_starttime
			// so the bogus time on the first frame doesn't count
				if (host_framecount == cls.td_startframe + 1)
					cls.td_starttime = realtime;

				// accumulate per-frame stats for profile
				if (cls.profile && profile_frame_last > 0)
				{
					double ft = realtime - profile_frame_last;
					if (ft < profile_frame_min) profile_frame_min = ft;
					if (ft > profile_frame_max) profile_frame_max = ft;
					profile_total_wpoly += c_brush_polys;
					profile_total_epoly += c_alias_polys;
				}
				profile_frame_last = realtime;
			}
			else if ( /* cl.time > 0 && */ cl.time <= cl.mtime[0])
			{
					return 0;		// don't need another message yet
			}
		}
		
	// get the next message
		fread (&net_message.cursize, 4, 1, cls.demofile);
		VectorCopy (cl.mviewangles[0], cl.mviewangles[1]);
		r = fread (cl.mviewangles[0], 12, 1, cls.demofile);

		for (i=0 ; i<3 ; i++)
			cl.mviewangles[0][i] = LittleFloat (cl.mviewangles[0][i]);
		
		net_message.cursize = LittleLong (net_message.cursize);
		if (net_message.cursize > MAX_MSGLEN)
			Sys_Error ("Demo message > MAX_MSGLEN");
		r = fread (net_message.data, net_message.cursize, 1, cls.demofile);
		if (r != 1)
		{
			CL_StopPlayback ();
			return 0;
		}
	
		return 1;
	}

	while (1)
	{
		r = NET_GetMessage (cls.netcon);
		
		if (r != 1 && r != 2)
			return r;
	
	// discard nop keepalive message
		if (net_message.cursize == 1 && net_message.data[0] == svc_nop)
			Con_Printf ("<-- server to client keepalive\n");
		else
			break;
	}

	if (cls.demorecording)
		CL_WriteDemoMessage ();
	
	return r;
}


/*
====================
CL_Stop_f

stop recording a demo
====================
*/
void CL_Stop_f (void)
{
	if (cmd_source != src_command)
		return;

	if (!cls.demorecording)
	{
		Con_Printf ("Not recording a demo.\n");
		return;
	}

// write a disconnect message to the demo file
	SZ_Clear (&net_message);
	MSG_WriteByte (&net_message, svc_disconnect);
	CL_WriteDemoMessage ();

// finish up
	fclose (cls.demofile);
	cls.demofile = NULL;
	cls.demorecording = false;
	Con_Printf ("Completed demo\n");
}

/*
====================
CL_Record_f

record <demoname> <map> [cd track]
====================
*/
void CL_Record_f (void)
{
	int		c;
	char	name[MAX_OSPATH];
	int		track;

	if (cmd_source != src_command)
		return;

	c = Cmd_Argc();
	if (c != 2 && c != 3 && c != 4)
	{
		Con_Printf ("record <demoname> [<map> [cd track]]\n");
		return;
	}

	if (strstr(Cmd_Argv(1), ".."))
	{
		Con_Printf ("Relative pathnames are not allowed.\n");
		return;
	}

	if (c == 2 && cls.state == ca_connected)
	{
		Con_Printf("Can not record - already connected to server\nClient demo recording must be started before connecting\n");
		return;
	}

// write the forced cd track number, or -1
	if (c == 4)
	{
		track = atoi(Cmd_Argv(3));
		Con_Printf ("Forcing CD track to %i\n", cls.forcetrack);
	}
	else
		track = -1;	

	_snprintf (name, sizeof(name), "%s/%s", com_gamedir, Cmd_Argv(1));
	
//
// start the map up
//
	if (c > 2)
		Cmd_ExecuteString ( va("map %s", Cmd_Argv(2)), src_command);
	
//
// open the demo file
//
	COM_DefaultExtension (name, ".dem");

	Con_Printf ("recording to %s.\n", name);
	cls.demofile = fopen (name, "wb");
	if (!cls.demofile)
	{
		Con_Printf ("ERROR: couldn't open.\n");
		return;
	}

	cls.forcetrack = track;
	fprintf (cls.demofile, "%i\n", cls.forcetrack);
	
	cls.demorecording = true;
}


/*
====================
CL_PlayDemo_f

play [demoname]
====================
*/
void CL_PlayDemo_f (void)
{
	char	name[256];
	int c;
	qboolean neg = false;

	if (cmd_source != src_command)
		return;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("playdemo <demoname> : plays a demo\n");
		return;
	}

//
// disconnect from server
//
	CL_Disconnect ();
	
//
// open the demo file
//
	strcpy (name, Cmd_Argv(1));
	COM_DefaultExtension (name, ".dem");

	Con_Printf ("Playing demo from %s.\n", name);
	COM_FOpenFile (name, &cls.demofile);
	if (!cls.demofile)
	{
		Con_Printf ("ERROR: couldn't open.\n");
		cls.demonum = -1;		// stop demo loop
		return;
	}

	cls.demoplayback = true;
	cls.state = ca_connected;
	cls.forcetrack = 0;

	while ((c = getc(cls.demofile)) != '\n')
		if (c == '-')
			neg = true;
		else
			cls.forcetrack = cls.forcetrack * 10 + (c - '0');

	if (neg)
		cls.forcetrack = -cls.forcetrack;
// ZOID, fscanf is evil
//	fscanf (cls.demofile, "%i\n", &cls.forcetrack);
}

/*
====================
CL_FinishTimeDemo

====================
*/
static void CL_WriteProfileJSON (int frames, float time, float fps)
{
	char	path[MAX_OSPATH];
	FILE	*f;
	float	frame_min_ms, frame_max_ms, frame_avg_ms;
	int		avg_wpoly, avg_epoly;

	_snprintf (path, sizeof(path), "%s\\carviary\\profile.json", host_parms.basedir);
	f = fopen (path, "w");
	if (!f)
	{
		Con_Printf ("profile: ERROR could not write %s\n", path);
		return;
	}

	frame_avg_ms = (frames > 0) ? (time / frames) * 1000.0f : 0;
	frame_min_ms = (float)(profile_frame_min * 1000.0);
	frame_max_ms = (float)(profile_frame_max * 1000.0);
	avg_wpoly = (frames > 0) ? profile_total_wpoly / frames : 0;
	avg_epoly = (frames > 0) ? profile_total_epoly / frames : 0;

	fprintf (f,
		"{\n"
		"  \"success\": true,\n"
		"  \"frames\": %d,\n"
		"  \"seconds\": %.3f,\n"
		"  \"fps\": %.3f,\n"
		"  \"frame_ms_avg\": %.2f,\n"
		"  \"frame_ms_min\": %.2f,\n"
		"  \"frame_ms_max\": %.2f,\n"
		"  \"avg_wpoly\": %d,\n"
		"  \"avg_epoly\": %d,\n"
		"  \"resolution\": \"%dx%d\"\n"
		"}\n",
		frames, time, fps,
		frame_avg_ms, frame_min_ms, frame_max_ms,
		avg_wpoly, avg_epoly,
		glwidth, glheight);

	fclose (f);
	Con_Printf ("profile: saved to %s\n", path);
}

void CL_FinishTimeDemo (void)
{
	int		frames;
	float	time;
	float	fps;

	cls.timedemo = false;

// the first frame didn't count
	frames = (host_framecount - cls.td_startframe) - 1;
	time = realtime - cls.td_starttime;
	if (!time)
		time = 1;
	fps = frames / time;
	Con_Printf ("%i frames\n%5.3f seconds\n%5.3f fps\n", frames, time, fps);

	if (cls.playprofile)
	{
		Con_Printf ("playprofile: %i screenshots saved to screenies/playprofile/\n", cls.pp_shotcount);
		Con_Printf ("playprofile: %i net frames, %5.3f seconds, %5.3f fps\n", frames, time, fps);
		cls.playprofile = false;
		cls.pp_framecount = 0;
		cls.pp_shotcount = 0;
		Sys_Quit ();
	}

	if (cls.profile)
	{
		CL_WriteProfileJSON (frames, time, fps);
		cls.profile = false;
		Sys_Quit ();
	}
}

/*
====================
CL_TimeDemo_f

timedemo [demoname]
====================
*/
void CL_TimeDemo_f (void)
{
	if (cmd_source != src_command)
		return;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("timedemo <demoname> : gets demo speeds\n");
		return;
	}

	CL_PlayDemo_f ();
	
// cls.td_starttime will be grabbed at the second frame of the demo, so
// all the loading time doesn't get counted
	
	cls.timedemo = true;
	cls.td_startframe = host_framecount;
	cls.td_lastframe = -1;		// get a new message this frame
}

/*
====================
CL_Profile_f

profile [demoname]
Like timedemo but writes results to JSON and auto-quits
====================
*/
void CL_Profile_f (void)
{
	if (cmd_source != src_command)
		return;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("profile <demoname> : benchmark and save results to JSON\n");
		return;
	}

	CL_PlayDemo_f ();

	cls.timedemo = true;
	cls.profile = true;
	cls.td_startframe = host_framecount;
	cls.td_lastframe = -1;

	// reset profile stats
	profile_frame_min = 999.0;
	profile_frame_max = 0.0;
	profile_frame_last = 0.0;
	profile_total_wpoly = 0;
	profile_total_epoly = 0;
}

/*
====================
CL_PlayProfile_f

playprofile [demoname]
Like timedemo but takes a screenshot every 100 net frames
====================
*/
void CL_PlayProfile_f (void)
{
	if (cmd_source != src_command)
		return;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("playprofile <demoname> : plays demo fast, screenshots every 100 frames\n");
		return;
	}

	CL_PlayDemo_f ();

	cls.timedemo = true;
	cls.playprofile = true;
	cls.pp_framecount = 0;
	cls.pp_shotcount = 0;
	cls.td_startframe = host_framecount;
	cls.td_lastframe = -1;
}

