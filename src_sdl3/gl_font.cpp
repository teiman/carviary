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
// gl_font.c this is where our font engine is located
//

#include "quakedef.h"
#include "gl_render.h"

float		scr_centertime_start;	// for slow victory printing
float		scr_centertime_off;

int			char_texture;
int			scr_center_lines;
int			scr_erase_lines;
int			scr_erase_center;
int			font_initialised = true;

cvar_t		scr_centertime	= {"scr_centertime",	"2"};
cvar_t		scr_printspeed	= {"scr_printspeed",	"8"};

qboolean	scr_drawdialog;

char		scr_centerstring[1024];
char		*scr_notifystring;

/*
================
Font_Init

This is the main font function, where the char is loaded...
================
*/
void Font_Init(void)
{
	byte		*draw_chars;// 8*8 graphic characters
	int			i;

	Cvar_RegisterVariable (&scr_centertime);
	Cvar_RegisterVariable (&scr_printspeed);

	char_texture = loadtextureimage ("gfx/wad/conchars", false, false); 

	if (!char_texture)
	{
		draw_chars = (byte *)W_GetLumpName ("conchars");
		for (i=0 ; i<256*64 ; i++)
			if (draw_chars[i] == 0)
				draw_chars[i] = 255;

		char_texture = GL_LoadTexture ("charset", 128, 128, draw_chars, false, true, 1); // default conchars as font 0	
	}
}

/*
================
Draw_Character

Draws one 8*8 graphics character with 0 being transparent.
It can be clipped to the top of the screen to allow the console to be
smoothly scrolled off.
================
*/
// Current color applied by Draw_Character (set via glColor-equivalent in the
// string parser; kept here so the batched path can respect it).
float char_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static qboolean char_batching = false;

// External setter used by console / menu (replaces glColor3f calls).
void Draw_SetCharColor (float r, float g, float b, float a)
{
	char_color[0] = r;
	char_color[1] = g;
	char_color[2] = b;
	char_color[3] = a;
}

void Draw_BeginCharBatch (void)
{
	if (char_batching)
		return;
	char_batching = true;
	R_HudBeginCharBatch(char_texture);
}

void Draw_EndCharBatch (void)
{
	if (!char_batching)
		return;
	R_HudEndCharBatch();
	char_batching = false;
}

void Draw_Character (int x, int y, int num)
{
	qboolean self_batch;

	if (num == 32)
		return;

	if (y <= -8)
		return;

	self_batch = !char_batching;
	if (self_batch)
		Draw_BeginCharBatch();

	R_HudAddChar(x, y, num, char_color[0], char_color[1], char_color[2], char_color[3]);

	if (self_batch)
		Draw_EndCharBatch();
}

/*
================
Draw_String
================
*/
void Draw_Alt_String (int x, int y, char *str, int maxlen)
{
	float	r, g, b;
	int		len;
	int		num;

	if (y <= -8)
		return;

	len = (maxlen) ? maxlen : strlen(str);

	R_HudBeginCharBatch(char_texture);
	float cr = 1.0f, cg = 1.0f, cb = 1.0f;

	while (*str && len)
	{
		if (*str == '&')
		{
			++str;
			len--;

			if (!*str) { R_HudEndCharBatch(); return; }

			switch(*str)
			{
			case 'f':
				{
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					r = (float)(*str - '0') * 0.11111111f;
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					g = (float)(*str - '0') * 0.11111111f;
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					b = (float)(*str - '0') * 0.11111111f;
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }

					cr = r; cg = g; cb = b;
					break;
				}
			case 'r':
				{
					cr = 1.0f; cg = 1.0f; cb = 1.0f;
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
				}

			default:
				{
					--str; len++;
				}
			}
		}

		if (*str != 32)
		{
			num = (int)(*str | 128);
			R_HudAddChar(x, y, num, cr, cg, cb, 1.0f);
		}

		++str;
		len--;

		x += 8;

		if (x > (signed)vid.width - 16)
			y += 8;
	}

	R_HudEndCharBatch();
}

/*
================
Draw_String
================
*/
void Draw_String (int x, int y, char *str, int maxlen)
{
	float	r, g, b;
	int		len;
	int		num;

	if (y <= -8)
		return;

	len = (maxlen) ? maxlen : strlen(str);

	R_HudBeginCharBatch(char_texture);
	float cr = 1.0f, cg = 1.0f, cb = 1.0f;

	while (*str && len)
	{
		if (*str == '&')
		{
			++str;
			len--;

			if (!*str) { R_HudEndCharBatch(); return; }

			switch(*str)
			{
			case 'f':
				{
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					r = (float)(*str - '0') * 0.11111111f;
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					g = (float)(*str - '0') * 0.11111111f;
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					b = (float)(*str - '0') * 0.11111111f;
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }

					cr = r; cg = g; cb = b;
					break;
				}
			case 'r':
				{
					cr = 1.0f; cg = 1.0f; cb = 1.0f;
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
					++str; len--; if(!*str) { R_HudEndCharBatch(); return; }
				}

			default:
				{
					--str; len++;
				}
			}
		}

		if (*str != 32)
		{
			num = (int)*str;
			R_HudAddChar(x, y, num, cr, cg, cb, 1.0f);
		}

		++str;
		len--;

		x += 8;

		if (x > (signed)vid.width - 16)
			y += 8;
	}

	R_HudEndCharBatch();
}


/*
===============================================================================

CENTER PRINTING

===============================================================================
*/


/*
==============
CPrintf

prints a font to centered to the screen, is removed after it is stopped being called
==============
*/
void CPrintf(char *str)
{
	int	x,y,l,j;
	int		font = 0;

	y = vid.height*0.35;

	do	
	{
	// scan the width of the line
		for (l=0 ; l<40 ; l++)
		{
			if (str[l] == '\n' || !str[l])
				break;
		}

		x = (vid.width - l*20)/2;
		
		for (j=0 ; j<l ; j++, x+=20)
		{
			Draw_Character (x, y, str[j]);
		}
			
		y += 30;

		while (*str && *str != '\n')
			str++;

		if (!*str)
			break;
		str++;		// skip the \n
	} while (1);	
}
/*
==============
SCR_CenterPrint

Called for important messages that should stay in the center of the screen
for a few moments
==============
*/
void SCR_CenterPrint (char *str)
{
	strncpy (scr_centerstring, str, sizeof(scr_centerstring)-1);
	scr_centertime_off = scr_centertime.value;
	scr_centertime_start = cl.time;

	if(print_center_to_console.value)
		Con_Printf("%s\n", str);

// count the number of lines for centering
	scr_center_lines = 1;
	while (*str)
	{
		if (*str == '\n')
			scr_center_lines++;
		str++;
	}
}


void SCR_DrawCenterString (void)
{
	char	*start;
	int		l;
	int		j;
	int		x, y;
	int		remaining;
	float	fade;

// the finale prints the characters one at a time
	if (cl.intermission)
		remaining = scr_printspeed.value * (cl.time - scr_centertime_start);
	else
		remaining = 9999;

	scr_erase_center = 0;
	start = scr_centerstring;

	if (scr_center_lines <= 4)
		y = vid.height*0.35;
	else
		y = 48;

	do
	{
	// scan the width of the line
		for (l=0 ; l<40 ; l++)
			if (start[l] == '\n' || !start[l])
				break;
		x = (vid.width - l*8)/2;

		if (!cl.intermission && centerfade.value)
		{
			if (cl.time - scr_centertime_start < 1)
				fade =  (cl.time - scr_centertime_start);
			else
				fade = -(cl.time - scr_centertime_start) + 2;
		}
		else
			fade = 1;

		for (j=0 ; j<l ; j++, x+=8)
		{
			if (!cl.intermission && centerfade.value)
				char_color[3] = fade;

			Draw_Character (x, y, start[j]);

			if (!remaining--)
			{
				char_color[0] = char_color[1] = char_color[2] = char_color[3] = 1.0f;
				return;
			}
		}

		y += 8;

		while (*start && *start != '\n')
			start++;

		if (!*start)
			break;
		start++;		// skip the \n
	} while (1);

	char_color[0] = char_color[1] = char_color[2] = char_color[3] = 1.0f;
}

void SCR_CheckDrawCenterString (void)
{
	scr_copytop = 1;
	if (scr_center_lines > scr_erase_lines)
		scr_erase_lines = scr_center_lines;

	scr_centertime_off -= host_frametime;
	
	if (scr_centertime_off <= 0 && !cl.intermission)
		return;
	if (key_dest != key_game)
		return;

	SCR_DrawCenterString ();
}

void SCR_DrawNotifyString (void)
{
	char	*start;
	int		l;
	int		x, y;

	start = scr_notifystring;

	y = vid.height*0.35;

	do	
	{
	// scan the width of the line
		for (l=0 ; l<40 ; l++)
			if (start[l] == '\n' || !start[l])
				break;
		x = (vid.width - l*8)/2;
//		for (j=0 ; j<l ; j++, x+=8)
//			Draw_Character (x, y, start[j]);	
		Draw_String (x, y, start, l);
			
		y += 8;

		while (*start && *start != '\n')
			start++;

		if (!*start)
			break;
		start++;		// skip the \n
	} while (1);
}

/*
==================
SCR_ModalMessage

Displays a text string in the center of the screen and waits for a Y or N
keypress.  
==================
*/
int SCR_ModalMessage (char *text)
{
	if (cls.state == ca_dedicated)
		return true;

	scr_notifystring = text;
 
// draw a fresh screen
	scr_fullupdate = 0;
	scr_drawdialog = true;
	SCR_UpdateScreen ();
	scr_drawdialog = false;
	
	S_ClearBuffer ();		// so dma doesn't loop current sound

	do
	{
		key_count = -1;		// wait for a key down and up
		Sys_SendKeyEvents ();
	} while (key_lastpress != 'y' && key_lastpress != 'n' && key_lastpress != K_ESCAPE);

	scr_fullupdate = 0;
	SCR_UpdateScreen ();

	return key_lastpress == 'y';
}