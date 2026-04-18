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
#include "gl_mat4.h"

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
cvar_t	vid_vsync = {"vid_vsync", "1", true}; // 1=on (wait for vblank), 0=off (uncapped)

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

	// CPU-side matrix stacks used by the renderer.
	MatStack_Init(&r_modelview);
	MatStack_Init(&r_projection);

	// ------ bind texture function pointers ------
	// In the SDL3 port these are directly available from GL headers.
	bindTexFunc      = (BINDTEXFUNCPTR)glBindTexture;
	delTexFunc       = (DELTEXFUNCPTR)glDeleteTextures;
	TexSubImage2DFunc = (TEXSUBIMAGEPTR)glTexSubImage2D;

	// ------ default GL state ------
	glClearColor(0, 0, 0, 0);
	glCullFace(GL_FRONT);

	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glEnable(GL_CULL_FACE);
	glEnable(GL_BLEND);

	// ------ depth ------
	glEnable(GL_DEPTH_TEST);

	gldepthmin = 0.0f;
	gldepthmax = 1.0f;
	glDepthRange(gldepthmin, gldepthmax);

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
	Cvar_RegisterVariable(&vid_vsync);

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
	// OpenGL 4.4 Core. Needed for GL_ARB_buffer_storage (persistent mapped
	// buffers) and for MultiDrawArrays without extensions.
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
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

	// Load GL function pointers via GLAD
	if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
		Sys_Error("Failed to load OpenGL via GLAD");

	if (!GLAD_GL_VERSION_4_4)
		Sys_Error("OpenGL 4.4 not available on this system");

	// vsync: driven by vid_vsync cvar, can be toggled at runtime.
	// We read the cvar rather than hardcoding 1 so a saved config with
	// vid_vsync=0 takes effect on startup.
	SDL_GL_SetSwapInterval(vid_vsync.value ? 1 : 0);

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
// Called once per frame to let each streaming VBO place an end-of-frame fence
// and advance to the next segment of its persistent ring. Implemented in each
// module that owns a persistent DynamicVBO.
extern "C" void World_StreamFrameEnd (void);
extern "C" void Warp_StreamFrameEnd  (void);

void GL_EndRendering (void)
{
	// Advance persistent-ring VBOs BEFORE the swap. At this point every draw
	// that reads from the current segment has been submitted; the fence will
	// only be signaled after the GPU consumes them.
	World_StreamFrameEnd();
	Warp_StreamFrameEnd();

	// Re-apply vsync only when the cvar changes. SetSwapInterval is a
	// cheap call but we still avoid it every frame for cleanliness.
	static int last_vsync = -1;
	int want = vid_vsync.value ? 1 : 0;
	if (want != last_vsync)
	{
		SDL_GL_SetSwapInterval(want);
		last_vsync = want;
	}

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
