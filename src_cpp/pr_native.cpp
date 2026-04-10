/*
Native C replacements for QuakeC functions.
These are swapped in at progs load time if the bytecode hash matches.
*/

#include "quakedef.h"
#include <direct.h>

// field offsets resolved at swap time
static int ofs_invisible_finished;
static int ofs_invisible_sound;
static int ofs_invisible_time;
static int ofs_invincible_finished;
static int ofs_invincible_time;
static int ofs_super_damage_finished;
static int ofs_super_time;
static int ofs_radsuit_finished;
static int ofs_rad_time;
static int ofs_air_finished;

static int ofs_modelindex_eyes;   // QC global
static int ofs_modelindex_player; // QC global
extern void SV_StartSound (edict_t *entity, int channel, char *sample, int volume, float attenuation);
extern void SV_ClientPrintf (char *fmt, ...);
extern void Host_ClientCommands (char *fmt, ...);

#define EDICT_FIELD(ed, ofs) (*(float *)((byte *)&(ed)->v + (ofs)))

/*
================
PF_CheckPowerups_Native

Online version: full network calls (sound, sprint, stuffcmd)
================
*/
static void PF_CheckPowerups_Native (void)
{
	edict_t *self = PROG_TO_EDICT(pr_global_struct->self);
	float	t = sv.time;

	if (self->v.health <= 0)
		return;

	// ---- invisibility ----
	float inv_finished = EDICT_FIELD(self, ofs_invisible_finished);
	if (inv_finished)
	{
		float *inv_sound = &EDICT_FIELD(self, ofs_invisible_sound);
		float *inv_time = &EDICT_FIELD(self, ofs_invisible_time);

		if (*inv_sound < t)
		{
			SV_StartSound(self, 0, (char *)"items/inv3.wav", 128, 2.0f);
			*inv_sound = t + ((rand() / (float)RAND_MAX) * 3.0f) + 1.0f;
		}
		if (inv_finished < t + 3)
		{
			if (*inv_time == 1)
			{
				SV_ClientPrintf((char *)"Ring of Shadows magic is fading\n");
				Host_ClientCommands((char *)"bf\n");
				SV_StartSound(self, 0, (char *)"items/inv2.wav", 255, 1.0f);
				*inv_time = t + 1;
			}
			else if (*inv_time < t)
			{
				*inv_time = t + 1;
				Host_ClientCommands((char *)"bf\n");
			}
		}
		if (inv_finished < t)
		{
			self->v.items = (float)((int)self->v.items & ~IT_INVISIBILITY);
			EDICT_FIELD(self, ofs_invisible_finished) = 0;
			*inv_time = 0;
		}
		self->v.frame = 0;
		self->v.modelindex = pr_globals[ofs_modelindex_eyes];
	}
	else
		self->v.modelindex = pr_globals[ofs_modelindex_player];

	// ---- invincibility ----
	float pent_finished = EDICT_FIELD(self, ofs_invincible_finished);
	if (pent_finished)
	{
		float *pent_time = &EDICT_FIELD(self, ofs_invincible_time);
		if (pent_finished < t + 3)
		{
			if (*pent_time == 1)
			{
				SV_ClientPrintf((char *)"Protection is almost burned out\n");
				Host_ClientCommands((char *)"bf\n");
				SV_StartSound(self, 0, (char *)"items/protect2.wav", 255, 1.0f);
				*pent_time = t + 1;
			}
			else if (*pent_time < t)
			{
				*pent_time = t + 1;
				Host_ClientCommands((char *)"bf\n");
			}
		}
		if (pent_finished < t)
		{
			self->v.items = (float)((int)self->v.items & ~IT_INVULNERABILITY);
			*pent_time = 0;
			EDICT_FIELD(self, ofs_invincible_finished) = 0;
		}
		if (pent_finished > t)
			self->v.effects = (float)((int)self->v.effects | EF_DIMLIGHT);
		else
			self->v.effects = (float)((int)self->v.effects & ~EF_DIMLIGHT);
	}

	// ---- quad damage ----
	float quad_finished = EDICT_FIELD(self, ofs_super_damage_finished);
	if (quad_finished)
	{
		float *quad_time = &EDICT_FIELD(self, ofs_super_time);
		if (quad_finished < t + 3)
		{
			if (*quad_time == 1)
			{
				SV_ClientPrintf((char *)"Quad Damage is wearing off\n");
				Host_ClientCommands((char *)"bf\n");
				SV_StartSound(self, 0, (char *)"items/damage2.wav", 255, 1.0f);
				*quad_time = t + 1;
			}
			else if (*quad_time < t)
			{
				*quad_time = t + 1;
				Host_ClientCommands((char *)"bf\n");
			}
		}
		if (quad_finished < t)
		{
			self->v.items = (float)((int)self->v.items & ~IT_QUAD);
			EDICT_FIELD(self, ofs_super_damage_finished) = 0;
			*quad_time = 0;
		}
		if (quad_finished > t)
			self->v.effects = (float)((int)self->v.effects | EF_DIMLIGHT);
		else
			self->v.effects = (float)((int)self->v.effects & ~EF_DIMLIGHT);
	}

	// ---- biosuit ----
	float suit_finished = EDICT_FIELD(self, ofs_radsuit_finished);
	if (suit_finished)
	{
		EDICT_FIELD(self, ofs_air_finished) = t + 12;
		float *rad_time = &EDICT_FIELD(self, ofs_rad_time);
		if (suit_finished < t + 3)
		{
			if (*rad_time == 1)
			{
				SV_ClientPrintf((char *)"Air supply in Biosuit expiring\n");
				Host_ClientCommands((char *)"bf\n");
				SV_StartSound(self, 0, (char *)"items/suit2.wav", 255, 1.0f);
				*rad_time = t + 1;
			}
			else if (*rad_time < t)
			{
				*rad_time = t + 1;
				Host_ClientCommands((char *)"bf\n");
			}
		}
		if (suit_finished < t)
		{
			self->v.items = (float)((int)self->v.items & ~IT_SUIT);
			*rad_time = 0;
			EDICT_FIELD(self, ofs_radsuit_finished) = 0;
		}
	}
}

/*
================
PF_CheckPowerups_Offline

Offline version: skips all network calls (sound, sprint, stuffcmd).
Only updates entity state (items, effects, timers, modelindex).
================
*/
static void PF_CheckPowerups_Offline (void)
{
	edict_t *self = PROG_TO_EDICT(pr_global_struct->self);
	float	t = sv.time;

	if (self->v.health <= 0)
		return;

	// ---- invisibility ----
	float inv_finished = EDICT_FIELD(self, ofs_invisible_finished);
	if (inv_finished)
	{
		float *inv_sound = &EDICT_FIELD(self, ofs_invisible_sound);
		float *inv_time = &EDICT_FIELD(self, ofs_invisible_time);

		if (*inv_sound < t)
			*inv_sound = t + ((rand() / (float)RAND_MAX) * 3.0f) + 1.0f;
		if (inv_finished < t + 3)
		{
			if (*inv_time == 1)
				*inv_time = t + 1;
			else if (*inv_time < t)
				*inv_time = t + 1;
		}
		if (inv_finished < t)
		{
			self->v.items = (float)((int)self->v.items & ~IT_INVISIBILITY);
			EDICT_FIELD(self, ofs_invisible_finished) = 0;
			*inv_time = 0;
		}
		self->v.frame = 0;
		self->v.modelindex = pr_globals[ofs_modelindex_eyes];
	}
	else
		self->v.modelindex = pr_globals[ofs_modelindex_player];

	// ---- invincibility ----
	float pent_finished = EDICT_FIELD(self, ofs_invincible_finished);
	if (pent_finished)
	{
		float *pent_time = &EDICT_FIELD(self, ofs_invincible_time);
		if (pent_finished < t + 3)
		{
			if (*pent_time == 1)
				*pent_time = t + 1;
			else if (*pent_time < t)
				*pent_time = t + 1;
		}
		if (pent_finished < t)
		{
			self->v.items = (float)((int)self->v.items & ~IT_INVULNERABILITY);
			*pent_time = 0;
			EDICT_FIELD(self, ofs_invincible_finished) = 0;
		}
		if (pent_finished > t)
			self->v.effects = (float)((int)self->v.effects | EF_DIMLIGHT);
		else
			self->v.effects = (float)((int)self->v.effects & ~EF_DIMLIGHT);
	}

	// ---- quad damage ----
	float quad_finished = EDICT_FIELD(self, ofs_super_damage_finished);
	if (quad_finished)
	{
		float *quad_time = &EDICT_FIELD(self, ofs_super_time);
		if (quad_finished < t + 3)
		{
			if (*quad_time == 1)
				*quad_time = t + 1;
			else if (*quad_time < t)
				*quad_time = t + 1;
		}
		if (quad_finished < t)
		{
			self->v.items = (float)((int)self->v.items & ~IT_QUAD);
			EDICT_FIELD(self, ofs_super_damage_finished) = 0;
			*quad_time = 0;
		}
		if (quad_finished > t)
			self->v.effects = (float)((int)self->v.effects | EF_DIMLIGHT);
		else
			self->v.effects = (float)((int)self->v.effects & ~EF_DIMLIGHT);
	}

	// ---- biosuit ----
	float suit_finished = EDICT_FIELD(self, ofs_radsuit_finished);
	if (suit_finished)
	{
		EDICT_FIELD(self, ofs_air_finished) = t + 12;
		float *rad_time = &EDICT_FIELD(self, ofs_rad_time);
		if (suit_finished < t + 3)
		{
			if (*rad_time == 1)
				*rad_time = t + 1;
			else if (*rad_time < t)
				*rad_time = t + 1;
		}
		if (suit_finished < t)
		{
			self->v.items = (float)((int)self->v.items & ~IT_SUIT);
			*rad_time = 0;
			EDICT_FIELD(self, ofs_radsuit_finished) = 0;
		}
	}
}

/*
================
PR_FindFieldOfs

Helper: find a field offset, returns -1 if not found
================
*/
static int PR_FindFieldOfs (const char *name)
{
	ddef_t *d = ED_FindField((char *)name);
	if (!d)
	{
		Con_Printf("PR_Native: field '%s' not found\n", name);
		return -1;
	}
	return d->ofs * 4;
}

/*
================
PR_SwapNatives

Called after PR_LoadProgs. Checks bytecode hashes and swaps
matching QC functions for native C implementations.
================
*/

#define NATIVE_BUILTIN_BASE 200 // builtin IDs for native replacements

extern builtin_t *pr_builtins;
extern int pr_numbuiltins;

// expanded builtin table
static builtin_t pr_builtin_native[256];
static int pr_num_native = 0;

void PR_SwapNatives (void)
{
	int i, j;
	dfunction_t *f;

	// CheckPowerups hash (from id1 progs.dat)
	const unsigned long long hash_CheckPowerups = 0xB2FE49D01F606203ULL;

	// find CheckPowerups
	for (i = 1; i < progs->numfunctions; i++)
	{
		f = &pr_functions[i];
		const char *name = pr_strings + f->s_name;

		if (strcmp(name, "CheckPowerups") != 0)
			continue;
		if (f->first_statement < 0)
			continue; // already a builtin

		// compute hash
		int end_stmt = progs->numstatements;
		for (j = i + 1; j < progs->numfunctions; j++)
		{
			if (pr_functions[j].first_statement > f->first_statement)
			{
				end_stmt = pr_functions[j].first_statement;
				break;
			}
		}
		int num_stmts = end_stmt - f->first_statement;

		unsigned long long hash = 0xcbf29ce484222325ULL;
		if (num_stmts > 0)
		{
			byte *data = (byte *)&pr_statements[f->first_statement];
			int size = num_stmts * sizeof(dstatement_t);
			for (int k = 0; k < size; k++)
			{
				hash ^= data[k];
				hash *= 0x100000001b3ULL;
			}
		}

		if (hash != hash_CheckPowerups)
		{
			Con_Printf("PR_Native: CheckPowerups hash mismatch (%016llX != %016llX), skipping swap\n",
				hash, hash_CheckPowerups);
			break;
		}

		// resolve field offsets
		ofs_invisible_finished = PR_FindFieldOfs("invisible_finished");
		ofs_invisible_sound = PR_FindFieldOfs("invisible_sound");
		ofs_invisible_time = PR_FindFieldOfs("invisible_time");
		ofs_invincible_finished = PR_FindFieldOfs("invincible_finished");
		ofs_invincible_time = PR_FindFieldOfs("invincible_time");
		ofs_super_damage_finished = PR_FindFieldOfs("super_damage_finished");
		ofs_super_time = PR_FindFieldOfs("super_time");
		ofs_radsuit_finished = PR_FindFieldOfs("radsuit_finished");
		ofs_rad_time = PR_FindFieldOfs("rad_time");
		ofs_air_finished = PR_FindFieldOfs("air_finished");

		// resolve QC globals
		ddef_t *g;
		g = ED_FindGlobal((char *)"modelindex_eyes");
		ofs_modelindex_eyes = g ? g->ofs : -1;
		g = ED_FindGlobal((char *)"modelindex_player");
		ofs_modelindex_player = g ? g->ofs : -1;

		if (ofs_invisible_finished < 0 || ofs_invincible_finished < 0 ||
			ofs_super_damage_finished < 0 || ofs_radsuit_finished < 0 ||
			ofs_modelindex_eyes < 0 || ofs_modelindex_player < 0)
		{
			Con_Printf("PR_Native: CheckPowerups field offsets missing, skipping swap\n");
			break;
		}

		// swap: set first_statement to negative builtin number
		f->first_statement = -NATIVE_BUILTIN_BASE;

		// choose version based on singleplayer vs multiplayer
		if (svs.maxclients == 1)
		{
			pr_builtins[NATIVE_BUILTIN_BASE] = PF_CheckPowerups_Offline;
			Con_Printf("PR_Native: CheckPowerups swapped to native C OFFLINE (%d statements replaced)\n", num_stmts);
		}
		else
		{
			pr_builtins[NATIVE_BUILTIN_BASE] = PF_CheckPowerups_Native;
			Con_Printf("PR_Native: CheckPowerups swapped to native C ONLINE (%d statements replaced)\n", num_stmts);
		}
		if (NATIVE_BUILTIN_BASE >= pr_numbuiltins)
			pr_numbuiltins = NATIVE_BUILTIN_BASE + 1;
		break;
	}
}
