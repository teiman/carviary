/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2000      Tomazquake / Nelson Fernandez
Copyright (C) 2026      Carviary SDL3 port

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
// sys_sdl.cpp -- SDL3 system interface, replaces sys_win.cpp

// windows.h must be included before GLAD (pulled in via quakedef.h/sdlquake.h)
// to avoid APIENTRY/tagPOINT collisions.
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "quakedef.h"
#include "sdlquake.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>

// ===========================================================================
// File handle table
// ===========================================================================

#define MAX_HANDLES 32

static FILE *sys_handles[MAX_HANDLES];

static int Sys_FindHandle(void)
{
	for (int i = 1; i < MAX_HANDLES; i++)
	{
		if (!sys_handles[i])
			return i;
	}
	Sys_Error("out of handles");
	return -1;
}

// ===========================================================================
// Log file
// ===========================================================================

static FILE *sys_logfile = NULL;

void Sys_InitLog(char *basedir)
{
	char logpath[MAX_OSPATH];

	if (sys_logfile)
	{
		fclose(sys_logfile);
		sys_logfile = NULL;
	}

	snprintf(logpath, sizeof(logpath), "%s/qconsole.log", basedir);
	sys_logfile = fopen(logpath, "w");
}

void Sys_Log(char *msg)
{
	if (sys_logfile)
	{
		fprintf(sys_logfile, "%s", msg);
		fflush(sys_logfile);
	}
}

// ===========================================================================
// File I/O
// ===========================================================================

int Sys_FileOpenRead(char *path, int *hndl)
{
	int h = Sys_FindHandle();

	FILE *f = fopen(path, "rb");
	if (!f)
	{
		*hndl = -1;
		return -1;
	}

	sys_handles[h] = f;
	*hndl = h;

	fseek(f, 0, SEEK_END);
	int len = (int)ftell(f);
	fseek(f, 0, SEEK_SET);

	return len;
}

int Sys_FileOpenWrite(char *path)
{
	int h = Sys_FindHandle();

	FILE *f = fopen(path, "wb");
	if (!f)
		Sys_Error("Error opening %s: %s", path, strerror(errno));

	sys_handles[h] = f;
	return h;
}

void Sys_FileClose(int handle)
{
	if (handle >= 0 && handle < MAX_HANDLES && sys_handles[handle])
	{
		fclose(sys_handles[handle]);
		sys_handles[handle] = NULL;
	}
}

void Sys_FileSeek(int handle, int position)
{
	if (handle >= 0 && handle < MAX_HANDLES && sys_handles[handle])
		fseek(sys_handles[handle], position, SEEK_SET);
}

int Sys_FileRead(int handle, void *dest, int count)
{
	if (handle >= 0 && handle < MAX_HANDLES && sys_handles[handle])
		return (int)fread(dest, 1, count, sys_handles[handle]);
	return 0;
}

int Sys_FileWrite(int handle, void *data, int count)
{
	if (handle >= 0 && handle < MAX_HANDLES && sys_handles[handle])
		return (int)fwrite(data, 1, count, sys_handles[handle]);
	return 0;
}

int Sys_FileTime(char *path)
{
	struct stat buf;
	if (stat(path, &buf) == -1)
		return -1;
	return (int)buf.st_mtime;
}

void Sys_mkdir(char *path)
{
#ifdef _WIN32
	_mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

// ===========================================================================
// Memory protection (stub -- not needed on modern systems)
// ===========================================================================

void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length)
{
	// No-op on modern systems: we don't use self-modifying code.
}

// ===========================================================================
// System I/O
// ===========================================================================

void Sys_Error(char *error, ...)
{
	va_list argptr;
	char    text[1024];

	va_start(argptr, error);
	vsnprintf(text, sizeof(text), error, argptr);
	va_end(argptr);

	Host_Shutdown();

	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Quake Error", text, NULL);

	fprintf(stderr, "\n*** Sys_Error: %s\n\n", text);

	if (sys_logfile)
	{
		fprintf(sys_logfile, "Sys_Error: %s\n", text);
		fclose(sys_logfile);
		sys_logfile = NULL;
	}

	exit(1);
}

void Sys_Printf(char *fmt, ...)
{
	va_list argptr;
	char    text[1024];

	va_start(argptr, fmt);
	vsnprintf(text, sizeof(text), fmt, argptr);
	va_end(argptr);

	fprintf(stdout, "%s", text);
	fflush(stdout);

	Sys_Log(text);
}

void Sys_Quit(void)
{
	Host_Shutdown();

	if (sys_logfile)
	{
		fclose(sys_logfile);
		sys_logfile = NULL;
	}

	SDL_Quit();
	exit(0);
}

// ===========================================================================
// Timing
// ===========================================================================

double Sys_FloatTime(void)
{
	static Uint64 freq  = 0;
	static Uint64 base  = 0;

	if (freq == 0)
	{
		freq = SDL_GetPerformanceFrequency();
		base = SDL_GetPerformanceCounter();
	}

	Uint64 now = SDL_GetPerformanceCounter();
	return (double)(now - base) / (double)freq;
}

// ===========================================================================
// Console input (dedicated server)
// ===========================================================================

char *Sys_ConsoleInput(void)
{
	// Not implemented for the SDL windowed client.
	// A dedicated server build would read from stdin here.
	return NULL;
}

// ===========================================================================
// Sleep / yield
// ===========================================================================

void Sys_Sleep(void)
{
	SDL_Delay(1);
}

// ===========================================================================
// Input event pump
// ===========================================================================

// Defined in in_sdl.cpp -- processes each SDL event for keyboard/mouse input
extern void IN_ProcessSDLEvent(SDL_Event *event);

void Sys_SendKeyEvents(void)
{
	SDL_Event event;

	while (SDL_PollEvent(&event))
	{
		IN_ProcessSDLEvent(&event);
	}
}

// ===========================================================================
// FP precision stubs
// ===========================================================================

void Sys_LowFPPrecision(void)
{
	// No-op: modern x86-64 / SSE does not need FP precision toggling.
}

void Sys_HighFPPrecision(void)
{
	// No-op.
}

// ===========================================================================
// Debug log
// ===========================================================================

void Sys_DebugLog(char *file, char *fmt, ...)
{
	va_list argptr;
	char    text[1024];

	va_start(argptr, fmt);
	vsnprintf(text, sizeof(text), fmt, argptr);
	va_end(argptr);

	FILE *fd = fopen(file, "at");
	if (fd)
	{
		fprintf(fd, "%s", text);
		fclose(fd);
	}
}

// ===========================================================================
// main() -- replaces WinMain
// ===========================================================================

qboolean isDedicated;

int main(int argc, char *argv[])
{
	quakeparms_t parms;
	double       time, oldtime, newtime;

	memset(&parms, 0, sizeof(parms));

	// -- Crash handler to log segfaults with stack trace -----------------------
#ifdef _WIN32
	SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *ep) -> LONG {
		FILE *d = fopen("sdl3_crash.log", "w");
		if (d) {
			fprintf(d, "CRASH: exception 0x%08lX at address 0x%p\n",
				ep->ExceptionRecord->ExceptionCode,
				ep->ExceptionRecord->ExceptionAddress);

			HANDLE process = GetCurrentProcess();
			HANDLE thread = GetCurrentThread();
			SymInitialize(process, NULL, TRUE);

			STACKFRAME64 sf = {};
			CONTEXT ctx = *ep->ContextRecord;
#ifdef _M_IX86
			DWORD machineType = IMAGE_FILE_MACHINE_I386;
			sf.AddrPC.Offset    = ctx.Eip;
			sf.AddrFrame.Offset = ctx.Ebp;
			sf.AddrStack.Offset = ctx.Esp;
#else
			DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
			sf.AddrPC.Offset    = ctx.Rip;
			sf.AddrFrame.Offset = ctx.Rbp;
			sf.AddrStack.Offset = ctx.Rsp;
#endif
			sf.AddrPC.Mode    = AddrModeFlat;
			sf.AddrFrame.Mode = AddrModeFlat;
			sf.AddrStack.Mode = AddrModeFlat;

			fprintf(d, "\nStack trace:\n");
			for (int i = 0; i < 32; i++) {
				if (!StackWalk64(machineType, process, thread,
					&sf, &ctx, NULL, SymFunctionTableAccess64,
					SymGetModuleBase64, NULL))
					break;

				char symBuf[sizeof(SYMBOL_INFO) + 256];
				SYMBOL_INFO *sym = (SYMBOL_INFO *)symBuf;
				sym->SizeOfStruct = sizeof(SYMBOL_INFO);
				sym->MaxNameLen = 255;

				DWORD64 disp = 0;
				if (SymFromAddr(process, sf.AddrPC.Offset, &disp, sym))
					fprintf(d, "  [%d] %s + 0x%llx\n", i, sym->Name, disp);
				else
					fprintf(d, "  [%d] 0x%llx\n", i, sf.AddrPC.Offset);
			}
			SymCleanup(process);
			fflush(d); fclose(d);
		}
		return EXCEPTION_EXECUTE_HANDLER;
	});
#endif

	// -- Debug log (early, before anything else) --------------------------------
	{
	}

	// -- Initialize SDL --------------------------------------------------------
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
	{
		return 1;
	}
	{
	}

	// -- Init math constants (anglemod etc.) -----------------------------------
	Math_Init();

	// -- Parse -external flag (enable TGA/PCX texture replacements) ------------
	{
		extern qboolean external_textures;
		external_textures = false;
		for (int i = 1; i < argc; i++)
			if (!strcmp(argv[i], "-external"))
				external_textures = true;
	}

	// -- Parse -dedicated flag early -------------------------------------------
	isDedicated = false;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-dedicated"))
		{
			isDedicated = true;
			break;
		}
	}

	// -- Command line args -----------------------------------------------------
	COM_InitArgv(argc, argv);

	parms.argc = com_argc;
	parms.argv = com_argv;
	parms.basedir = (char *)".";

	// -- Memory allocation -----------------------------------------------------
	parms.memsize = 0x10000000; // 256 MB default (enough for large mods like AD)
	if (COM_CheckParm("-heapsize"))
	{
		int t = COM_CheckParm("-heapsize") + 1;
		if (t < com_argc)
			parms.memsize = Q_atoi(com_argv[t]) * 1024;
	}
	else if (COM_CheckParm("-mem"))
	{
		int t = COM_CheckParm("-mem") + 1;
		if (t < com_argc)
			parms.memsize = Q_atoi(com_argv[t]) * 1024 * 1024;
	}

	if (parms.memsize < 0x0880000)
		parms.memsize = 0x0880000; // minimum ~8.5 MB

	parms.membase = malloc(parms.memsize);
	if (!parms.membase)
		Sys_Error("Not enough memory free; check disk space");

	// -- Initialize the log ----------------------------------------------------
	Sys_InitLog(parms.basedir);

	Sys_Printf("Carviary / TomazQuake SDL3 -- %4.2f\n", (float)TOMAZQUAKE_VERSION);

	// -- Init the host ---------------------------------------------------------
	{
	}
	Host_Init(&parms);
	{
	}

	// -- Main loop -------------------------------------------------------------
	oldtime = Sys_FloatTime();

	while (1)
	{
		newtime = Sys_FloatTime();
		time    = newtime - oldtime;

		if (isDedicated)
		{
			// Dedicated server: run at a fixed tick rate
			while (time < sys_ticrate.value)
			{
				Sys_Sleep();
				newtime = Sys_FloatTime();
				time    = newtime - oldtime;
			}
		}
		else
		{
			// Client: no minimum frame time (uncapped, vsync controls it)
			if (time < 0.001)
			{
				// Don't spin at absurd framerates
				Sys_Sleep();
				continue;
			}
		}

		oldtime = newtime;

		Host_Frame((float)time);
	}

	// Never reached
	return 0;
}
