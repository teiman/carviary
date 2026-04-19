// gl_lamp.cpp -- flashlight / "lamp" command.
//
// Refreshes a single spotlight-cone dlight at the player's eye each frame.
// The cone is handled in R_AddDynamicLights (per-surface gating) and its
// radius is ample enough to reach across most interior spaces while the
// cone angle keeps it from lighting up everything behind the player.
//
// Only one lamp exists at a time -- there's no `lamp 2 on` etc. `lamp on`
// enables it, `lamp off` disables it. `lamp` without args prints state.

#include "quakedef.h"

static qboolean lamp_active = false;

// Pick a fixed dlight slot high in the array so it doesn't collide with
// engine-allocated lights (muzzleflash, explosions) which start probing
// from index 0. MAX_DLIGHTS is 32; we reserve the last.
#define LAMP_DLIGHT_SLOT  (MAX_DLIGHTS - 1)

// Tuning. Values chosen to look like a real flashlight in Quake's
// interior scale (player height ~56 units):
//   radius       -- how far the cone reaches
//   inner/outer  -- cos of half-angles; inner = full bright, outer = zero
//   color        -- slightly warm white, stops it looking sterile
//   forward      -- push the origin a little in front of the eye so the
//                   near walls aren't back-lit by the light being behind
//                   their plane.
#define LAMP_RADIUS          600.0f
#define LAMP_CONE_INNER_DEG  16.0f    // full brightness within this half-angle
#define LAMP_CONE_OUTER_DEG  28.0f    // zero brightness beyond this half-angle
#define LAMP_FORWARD_OFFSET  4.0f
#define LAMP_COLOR_R         1.00f
#define LAMP_COLOR_G         0.95f
#define LAMP_COLOR_B         0.85f

static void Lamp_f (void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf ("lamp is %s\n", lamp_active ? "on" : "off");
		Con_Printf ("usage: lamp on | lamp off\n");
		return;
	}
	char *arg = Cmd_Argv(1);
	if (!Q_strcasecmp(arg, (char *)"on")) {
		lamp_active = true;
		Con_Printf ("lamp on\n");
	} else if (!Q_strcasecmp(arg, (char *)"off")) {
		lamp_active = false;
		// Kill the dlight immediately so it doesn't linger for one frame.
		cl_dlights[LAMP_DLIGHT_SLOT].die = 0.0f;
		cl_dlights[LAMP_DLIGHT_SLOT].radius = 0.0f;
		Con_Printf ("lamp off\n");
	} else {
		Con_Printf ("lamp: expected 'on' or 'off'\n");
	}
}

void Lamp_Init (void)
{
	Cmd_AddCommand ("lamp", Lamp_f);
}

// Called every frame from R_RenderScene, just before R_PushDlights so the
// cone dlight is picked up by the standard lightmap update path.
void Lamp_Update (void)
{
	if (!lamp_active) return;
	if (cls.state != ca_connected) return;

	dlight_t *dl = &cl_dlights[LAMP_DLIGHT_SLOT];
	memset (dl, 0, sizeof(*dl));

	dl->origin[0] = r_refdef.vieworg[0] + vpn[0] * LAMP_FORWARD_OFFSET;
	dl->origin[1] = r_refdef.vieworg[1] + vpn[1] * LAMP_FORWARD_OFFSET;
	dl->origin[2] = r_refdef.vieworg[2] + vpn[2] * LAMP_FORWARD_OFFSET;
	dl->radius    = LAMP_RADIUS;
	dl->die       = cl.time + 0.1f; // refreshed each frame; short TTL in case we stop updating
	dl->decay     = 0.0f;
	dl->minlight  = 0.0f;
	dl->color[0]  = LAMP_COLOR_R;
	dl->color[1]  = LAMP_COLOR_G;
	dl->color[2]  = LAMP_COLOR_B;

	dl->cone_dir[0] = vpn[0];
	dl->cone_dir[1] = vpn[1];
	dl->cone_dir[2] = vpn[2];
	// Cache cosines of the cone half-angles.
	float inner_rad = LAMP_CONE_INNER_DEG * (float)M_PI / 180.0f;
	float outer_rad = LAMP_CONE_OUTER_DEG * (float)M_PI / 180.0f;
	dl->cone_inner  = cosf(inner_rad);
	dl->cone_outer  = cosf(outer_rad);
}
