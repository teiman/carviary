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
// gl_vid_sdl.cpp -- SDL3 video driver for OpenGL
// Replaces gl_vidnt.cpp for the SDL3 port of TomazQuake/Carviary

#include "quakedef.h"
#include "sdlquake.h"

// ---------------------------------------------------------------------------
// SDL3 window / GL context
// ---------------------------------------------------------------------------
SDL_Window		*sdl_window		= NULL;
SDL_GLContext	sdl_glcontext	= NULL;

// ---------------------------------------------------------------------------
// Global video state
// ---------------------------------------------------------------------------
viddef_t	vid;				// global video state
int			console_enabled = 1;	// Tomaz - Console Toggle

// ---------------------------------------------------------------------------
// Video mode state
// ---------------------------------------------------------------------------
modestate_t		modestate = MS_UNINIT;

// ---------------------------------------------------------------------------
// Application / focus state
// ---------------------------------------------------------------------------
qboolean	ActiveApp	= true;
qboolean	Minimized	= false;

// ---------------------------------------------------------------------------
// Window geometry
// ---------------------------------------------------------------------------
int			window_center_x, window_center_y;
sdl_rect_t	window_rect;

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
unsigned int	d_8to24table[256];
unsigned short	d_8to16table[256];

// ---------------------------------------------------------------------------
// GL viewport (defined in gl_screen.cpp, declared extern here)
// ---------------------------------------------------------------------------
// int glx, gly, glwidth, glheight; -- already defined in gl_screen.cpp

// ---------------------------------------------------------------------------
// Texture management
// ---------------------------------------------------------------------------
int		texture_extension_number = 1;
int		texture_mode = GL_LINEAR;

BINDTEXFUNCPTR		bindTexFunc;
DELTEXFUNCPTR		delTexFunc;
TEXSUBIMAGEPTR		TexSubImage2DFunc;

// ---------------------------------------------------------------------------
// Depth range
// ---------------------------------------------------------------------------
float	gldepthmin, gldepthmax;

// ---------------------------------------------------------------------------
// GL strings
// ---------------------------------------------------------------------------
const char	*gl_vendor;
const char	*gl_renderer;
const char	*gl_version;
const char	*gl_extensions;

// ---------------------------------------------------------------------------
// Cvars
// ---------------------------------------------------------------------------
cvar_t	gl_ztrick = {"gl_ztrick", "0", true};

// ---------------------------------------------------------------------------
// Multitexture
// ---------------------------------------------------------------------------
qboolean		gl_mtexable = false;

GLenum	TEXTURE0_SGIS_ARB = TEXTURE0_ARB;
GLenum	TEXTURE1_SGIS_ARB = TEXTURE1_ARB;

// qglMTexCoord2fSGIS_ARB and qglSelectTextureSGIS_ARB defined in gl_rsurf.cpp

// ---------------------------------------------------------------------------
// Current window size tracking
// ---------------------------------------------------------------------------
static int	currentWidth  = 1024;
static int	currentHeight = 768;

// =========================================================================
// VID_SetPalette -- build 32-bit palette lookup table
// =========================================================================
void VID_SetPalette (unsigned char *palette)
{
	byte	*pal;
	unsigned int	r, g, b;
	unsigned int	v;
	unsigned short	i;
	unsigned int	*table;

	// 8-to-24 table
	pal = palette;
	table = d_8to24table;

	for (i = 0; i < 256; i++)
	{
		r = pal[0];
		g = pal[1];
		b = pal[2];
		pal += 3;

		v = (255 << 24) | (r << 0) | (g << 8) | (b << 16);
		*table++ = v;
	}

	// Index 255 is transparent (full-bright pink in the palette)
	d_8to24table[255] &= 0x00ffffff;	// alpha = 0
}

// =========================================================================
// VID_SetDefaultMode
// =========================================================================
void VID_SetDefaultMode (void)
{
	modestate = MS_WINDOWED;
}

// =========================================================================
// VID_UpdateWindowStatus -- update window_rect and center from SDL window
// =========================================================================
void VID_UpdateWindowStatus (void)
{
	int x, y, w, h;

	SDL_GetWindowPosition(sdl_window, &x, &y);
	SDL_GetWindowSize(sdl_window, &w, &h);

	window_rect.left   = x;
	window_rect.top    = y;
	window_rect.right  = x + w;
	window_rect.bottom = y + h;

	window_center_x = (window_rect.left + window_rect.right)  / 2;
	window_center_y = (window_rect.top  + window_rect.bottom) / 2;
}

// =========================================================================
// VID_SetMode -- set video mode (create / resize window)
// =========================================================================
int VID_SetMode (int modenum, unsigned char *palette)
{
	// For the SDL3 port we only support one "mode" -- the current window.
	// On a mode-set request we simply resize the existing window.

	if (!sdl_window)
		return false;

	SDL_SetWindowSize(sdl_window, currentWidth, currentHeight);

	modestate = MS_WINDOWED;

	VID_SetPalette(palette);

	vid.width	= currentWidth;
	vid.height	= currentHeight;
	vid.numpages = 2;
	vid.recalc_refdef = true;

	glwidth  = currentWidth;
	glheight = currentHeight;

	VID_UpdateWindowStatus();

	return true;
}

// =========================================================================
// GL_Init -- query GL info, set up default GL state, load extensions
// =========================================================================
static void GL_Init (void)
{
	// ------ GL info strings ------
	gl_vendor     = (const char *)glGetString(GL_VENDOR);
	gl_renderer   = (const char *)glGetString(GL_RENDERER);
	gl_version    = (const char *)glGetString(GL_VERSION);
	gl_extensions = (const char *)glGetString(GL_EXTENSIONS);

	Con_Printf("GL_VENDOR: %s\n",     gl_vendor);
	Con_Printf("GL_RENDERER: %s\n",   gl_renderer);
	Con_Printf("GL_VERSION: %s\n",    gl_version);

	// ------ bind texture function pointers ------
	// In the SDL3 port these are directly available from GL headers.
	bindTexFunc      = (BINDTEXFUNCPTR)glBindTexture;
	delTexFunc       = (DELTEXFUNCPTR)glDeleteTextures;
	TexSubImage2DFunc = (TEXSUBIMAGEPTR)glTexSubImage2D;

	// ------ default GL state ------
	glClearColor(0, 0, 0, 0);
	glCullFace(GL_FRONT);
	glEnable(GL_TEXTURE_2D);

	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.666f);

	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glShadeModel(GL_SMOOTH);

	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	glEnable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);

	// ------ depth ------
	glEnable(GL_DEPTH_TEST);

	gldepthmin = 0.0f;
	gldepthmax = 1.0f;
	glDepthRange(gldepthmin, gldepthmax);

	// ------ multitexture extension ------
	gl_mtexable = false;

	if (gl_extensions && strstr(gl_extensions, "GL_ARB_multitexture"))
	{
		qglMTexCoord2fSGIS_ARB = (lpMTexFUNC)SDL_GL_GetProcAddress("glMultiTexCoord2fARB");
		qglSelectTextureSGIS_ARB = (lpSelTexFUNC)SDL_GL_GetProcAddress("glActiveTextureARB");

		if (qglMTexCoord2fSGIS_ARB && qglSelectTextureSGIS_ARB)
		{
			TEXTURE0_SGIS_ARB = TEXTURE0_ARB;
			TEXTURE1_SGIS_ARB = TEXTURE1_ARB;
			gl_mtexable = true;
			Con_Printf("ARB multitexture extensions found.\n");
		}
	}
	else if (gl_extensions && strstr(gl_extensions, "GL_SGIS_multitexture"))
	{
		qglMTexCoord2fSGIS_ARB = (lpMTexFUNC)SDL_GL_GetProcAddress("glMTexCoord2fSGIS");
		qglSelectTextureSGIS_ARB = (lpSelTexFUNC)SDL_GL_GetProcAddress("glSelectTextureSGIS");

		if (qglMTexCoord2fSGIS_ARB && qglSelectTextureSGIS_ARB)
		{
			TEXTURE0_SGIS_ARB = TEXTURE0_SGIS;
			TEXTURE1_SGIS_ARB = TEXTURE1_SGIS;
			gl_mtexable = true;
			Con_Printf("SGIS multitexture extensions found.\n");
		}
	}

	if (!gl_mtexable)
		Con_Printf("Multitexture not supported.\n");
}

// =========================================================================
// VID_Init -- main video initialization
// =========================================================================
void VID_Init (unsigned char *palette)
{
	int		width, height;
	Uint32	flags  = SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS;
	int		p;

	Cvar_RegisterVariable(&gl_ztrick);

	// Default to desktop resolution (borderless fullscreen)
	const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
	if (dm)
	{
		width  = dm->w;
		height = dm->h;
	}
	else
	{
		width  = 1920;
		height = 1080;
	}

	// ----- parse command-line overrides -----
	p = COM_CheckParm((char *)"-width");
	if (p && p < com_argc - 1)
		width = Q_atoi(com_argv[p + 1]);

	p = COM_CheckParm((char *)"-height");
	if (p && p < com_argc - 1)
		height = Q_atoi(com_argv[p + 1]);

	p = COM_CheckParm((char *)"-window");
	if (p)
		flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

	p = COM_CheckParm((char *)"-fullscreen");
	if (p)
		flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN;

	currentWidth  = width;
	currentHeight = height;

	// ----- SDL GL attributes -----
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	// ----- create window -----
	sdl_window = SDL_CreateWindow("Carviary", width, height, flags);

	if (!sdl_window)
		Sys_Error("Couldn't create SDL window: %s", SDL_GetError());

	// Position borderless window at top-left to cover the screen
	if (flags & SDL_WINDOW_BORDERLESS)
		SDL_SetWindowPosition(sdl_window, 0, 0);

	// ----- create GL context -----
	sdl_glcontext = SDL_GL_CreateContext(sdl_window);

	if (!sdl_glcontext)
		Sys_Error("Couldn't create GL context: %s", SDL_GetError());

	SDL_GL_MakeCurrent(sdl_window, sdl_glcontext);

	// enable vsync (swap interval 1); ignore failure
	SDL_GL_SetSwapInterval(1);

	// ----- set up engine video globals -----
	vid.width	= width;
	vid.height	= height;
	vid.numpages = 2;
	vid.recalc_refdef = true;
	vid.colormap = host_colormap;

	glx = 0;
	gly = 0;
	glwidth  = width;
	glheight = height;

	modestate = (flags & SDL_WINDOW_FULLSCREEN) ? MS_FULLSCREEN : MS_WINDOWED;

	VID_UpdateWindowStatus();

	// ----- initialize OpenGL state and extensions -----
	GL_Init();

	// ----- build palette table -----
	VID_SetPalette(palette);

	// ----- print GL info to console -----
	Con_Printf("\nGL_VENDOR: %s\n",   gl_vendor);
	Con_Printf("GL_RENDERER: %s\n",   gl_renderer);
	Con_Printf("GL_VERSION: %s\n",    gl_version);
	Con_Printf("Video mode: %dx%d %s\n", width, height,
		(flags & SDL_WINDOW_FULLSCREEN) ? "fullscreen" : "windowed");
}

// =========================================================================
// GL_BeginRendering -- return viewport dimensions
// =========================================================================
void GL_BeginRendering (int *x, int *y, int *width, int *height)
{
	*x      = glx;
	*y      = gly;
	*width  = glwidth;
	*height = glheight;
}

// =========================================================================
// GL_EndRendering -- swap buffers
// =========================================================================
void GL_EndRendering (void)
{
	SDL_GL_SwapWindow(sdl_window);
}

// =========================================================================
// VID_Shutdown -- destroy GL context and window
// =========================================================================
void VID_Shutdown (void)
{
	if (sdl_glcontext)
	{
		SDL_GL_DestroyContext(sdl_glcontext);
		sdl_glcontext = NULL;
	}

	if (sdl_window)
	{
		SDL_DestroyWindow(sdl_window);
		sdl_window = NULL;
	}

	modestate = MS_UNINIT;
}

// =========================================================================
// Mode enumeration stubs -- simplified for SDL3 single-mode approach
// =========================================================================
int VID_NumModes (void)
{
	return 1;
}

void VID_DescribeCurrentMode_f (void)
{
	Con_Printf("Current mode: %dx%d %s\n",
		currentWidth, currentHeight,
		(modestate == MS_FULLSCREEN) ? "fullscreen" : "windowed");
}
