// gl_exposure.cpp -- adaptive exposure via lightstyle 0 delta.
//
// Each frame, measure the ambient light at the player's eye position using
// R_LightPoint, compute a target exposure factor (low brightness -> brighten,
// high brightness -> darken), smooth it with an exponential low-pass, and
// apply an additive OFFSET to d_lightstylevalue[0]. The offset is relative
// to whatever value R_AnimateLight wrote this frame, so any animation the
// engine itself runs on style 'a' is preserved.
//
// The existing lightmap pipeline picks up the change automatically because
// R_BuildLightMap re-runs for any surface whose cached_light[style] differs
// from d_lightstylevalue[style].

#include "quakedef.h"
#include <math.h>

extern vec3_t lightcolor;    // set by R_LightPoint
extern void   R_LightPoint (vec3_t p);

static cvar_t r_auto_exposure       = {"r_auto_exposure",       "0"};
static cvar_t r_auto_exposure_ref   = {"r_auto_exposure_ref",   "0.25"};
static cvar_t r_auto_exposure_min   = {"r_auto_exposure_min",   "0.5"};
static cvar_t r_auto_exposure_max   = {"r_auto_exposure_max",   "2.0"};
static cvar_t r_auto_exposure_rate  = {"r_auto_exposure_rate",  "1.0"};
static cvar_t r_auto_exposure_debug = {"r_auto_exposure_debug", "0"};

static float g_current_factor = 1.0f;
static float g_debug_accum    = 0.0f;
static int   g_last_applied_offset = 0;

void Exposure_Init (void)
{
	Cvar_RegisterVariable(&r_auto_exposure);
	Cvar_RegisterVariable(&r_auto_exposure_ref);
	Cvar_RegisterVariable(&r_auto_exposure_min);
	Cvar_RegisterVariable(&r_auto_exposure_max);
	Cvar_RegisterVariable(&r_auto_exposure_rate);
	Cvar_RegisterVariable(&r_auto_exposure_debug);
}

void Exposure_Reset (void)
{
	g_current_factor = 1.0f;
	g_last_applied_offset = 0;
	g_debug_accum = 0.0f;
}

// Called once per frame AFTER R_AnimateLight so d_lightstylevalue[0]
// already holds this frame's natural value for style 'a'.
void Exposure_Update (float frametime)
{
	if (r_auto_exposure.value == 0.0f) {
		// If the user disables the feature, make sure we leave no residual
		// offset applied to style 0.
		g_current_factor      = 1.0f;
		g_last_applied_offset = 0;
		return;
	}

	if (!cl.worldmodel) return;

	// --- measure local ambient brightness ---
	vec3_t probe;
	VectorCopy(r_refdef.vieworg, probe);
	probe[2] += 4.0f;    // lift off the floor a touch
	R_LightPoint(probe);
	float brightness =
	    (lightcolor[0] + lightcolor[1] + lightcolor[2]) / (3.0f * 255.0f);

	// --- target factor ---
	float ref = r_auto_exposure_ref.value;
	if (ref < 0.05f) ref = 0.05f;
	float raw_target = ref / (brightness > 0.05f ? brightness : 0.05f);

	float mn = r_auto_exposure_min.value;
	float mx = r_auto_exposure_max.value;
	if (mn < 0.1f) mn = 0.1f;
	if (mx < mn)   mx = mn + 0.01f;
	float target = raw_target;
	if (target < mn) target = mn;
	if (target > mx) target = mx;

	// --- exponential smoothing ---
	float rate = r_auto_exposure_rate.value;
	if (rate < 0.05f) rate = 0.05f;
	float k = 1.0f - expf(-frametime / rate);
	g_current_factor += (target - g_current_factor) * k;

	// --- apply as an additive OFFSET to the natural value ---
	int natural = d_lightstylevalue[0];
	int desired = (int)(natural * g_current_factor);
	int offset  = desired - natural;

	// Clamp the final style value. Upper bound generous so brightening
	// actually takes effect even when the samples are already high
	// (R_BuildLightMap store clamps the per-channel result to 255 anyway;
	// wider headroom here means dark rooms that need a big scale can get it).
	int final_v = natural + offset;
	if (final_v < 0)    final_v = 0;
	if (final_v > 2048) final_v = 2048;
	d_lightstylevalue[0] = final_v;
	g_last_applied_offset = offset;

	// --- debug print every 0.5s ---
	if (r_auto_exposure_debug.value != 0.0f) {
		g_debug_accum += frametime;
		if (g_debug_accum >= 0.5f) {
			g_debug_accum = 0.0f;
			extern int R_BuildLightMap_Counter;
			static int last_counter = 0;
			int rebuilds = R_BuildLightMap_Counter - last_counter;
			last_counter = R_BuildLightMap_Counter;
			extern cvar_t r_dynamic;
			char sign = (offset >= 0) ? '+' : '-';
			int abs_off = offset < 0 ? -offset : offset;
			Con_Printf("[exposure] bright=%.2f target=%.2f current=%.2f natural=%d offset=%c%d -> %d  rebuilds=%d r_dynamic=%g\n",
			           brightness, target, g_current_factor,
			           natural, sign, abs_off, final_v,
			           rebuilds, r_dynamic.value);
		}
	}
}
