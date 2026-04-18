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
// sdlquake.h: SDL3 platform header, replaces winquake.h for the SDL3 port

#ifndef SDLQUAKE_H
#define SDLQUAKE_H

#include <SDL3/SDL.h>

#ifndef SERVERONLY
#include <glad/glad.h>
#endif

// ---------------------------------------------------------------------------
// SDL3 window and GL context globals (replace HWND mainwindow, HINSTANCE, etc.)
// ---------------------------------------------------------------------------
extern SDL_Window	*sdl_window;

#ifndef SERVERONLY
extern SDL_GLContext	sdl_glcontext;
#endif

// ---------------------------------------------------------------------------
// Video mode state -- kept identical to the original engine enum
// ---------------------------------------------------------------------------
typedef enum { MS_WINDOWED, MS_FULLSCREEN, MS_FULLDIB, MS_UNINIT } modestate_t;

extern modestate_t	modestate;

// ---------------------------------------------------------------------------
// Application / focus state
// ---------------------------------------------------------------------------
extern qboolean		ActiveApp, Minimized;

// ---------------------------------------------------------------------------
// Mouse input
// ---------------------------------------------------------------------------
void IN_ShowMouse (void);
void IN_DeactivateMouse (void);
void IN_HideMouse (void);
void IN_ActivateMouse (void);
void IN_RestoreOriginalMouseState (void);
void IN_SetQuakeMouseState (void);
void IN_MouseEvent (int mstate);
void IN_UpdateClipCursor (void);

extern qboolean		mouseinitialized;

extern cvar_t		_windowed_mouse;

// ---------------------------------------------------------------------------
// Window geometry -- replaces the Win32 RECT with a plain struct
// ---------------------------------------------------------------------------
typedef struct
{
	int left;
	int top;
	int right;
	int bottom;
} sdl_rect_t;

extern int		window_center_x, window_center_y;
extern sdl_rect_t	window_rect;

// ---------------------------------------------------------------------------
// POINT struct replacement (used by menu mouse code)
// ---------------------------------------------------------------------------
#ifndef _WINDEF_
typedef struct tagPOINT { int x; int y; } POINT;
#endif
extern POINT current_pos;

// ---------------------------------------------------------------------------
// Sound helpers
// ---------------------------------------------------------------------------
void S_BlockSound (void);
void S_UnblockSound (void);

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------
void VID_SetDefaultMode (void);

#endif // SDLQUAKE_H
