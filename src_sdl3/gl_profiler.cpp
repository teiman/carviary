// gl_profiler.cpp -- see gl_profiler.h and docs/profiling.md.
//
// Design notes only for the non-obvious parts:
//   * GL_TIME_ELAPSED, GL_PRIMITIVES_GENERATED and GL_SAMPLES_PASSED target
//     different query "slots" in GL, so all three can be active simultaneously
//     around the same section. Only *same-target* begin/end pairs cannot nest.
//   * Query results are async. We read slot N three frames after issuing it.
//     A ring of 4 slots per section covers that latency with headroom.
//   * Sampling is driven by the network-frame index of the demo (cls.pp_framecount).
//     We sample only when that index hits one of g_target_frames[], so the set
//     of sampled frames is identical across builds and across machines.

#include "quakedef.h"
#include "gl_profiler.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PROF_MAX_SAMPLES    100
#define PROF_RING_SIZE      4   // frames of readback latency coverage

// network-frame indices at which we sample. Evenly spaced every 2 net
// frames, from frame 10 (skip load/startup noise) through frame 208.
// Short demos (~250 net frames like demo1) fill all 100 slots. For longer
// demos you get wider coverage at the cost of density. Keep this list
// literal and identical across builds -- diffing profiles relies on it.
static const int g_target_frames[PROF_MAX_SAMPLES] = {
      10,  12,  14,  16,  18,  20,  22,  24,  26,  28,
      30,  32,  34,  36,  38,  40,  42,  44,  46,  48,
      50,  52,  54,  56,  58,  60,  62,  64,  66,  68,
      70,  72,  74,  76,  78,  80,  82,  84,  86,  88,
      90,  92,  94,  96,  98, 100, 102, 104, 106, 108,
     110, 112, 114, 116, 118, 120, 122, 124, 126, 128,
     130, 132, 134, 136, 138, 140, 142, 144, 146, 148,
     150, 152, 154, 156, 158, 160, 162, 164, 166, 168,
     170, 172, 174, 176, 178, 180, 182, 184, 186, 188,
     190, 192, 194, 196, 198, 200, 202, 204, 206, 208,
};

#define SK_LEAF      0   // GL timestamps + prims + frags + draw counter
#define SK_CONTAINER 1   // GL timestamps only (no prims/frags; they don't nest)
#define SK_CPU_ONLY  2   // no GL queries; CPU time only; accumulates across calls

// Order must match the enum in gl_profiler.h.
static const char *g_section_names[PROF_NUM_SECTIONS] = {
    "clear",
    "world_opaque",
    "viewmodel",
    "polyblend",
    "hud_2d",
    "swap",
    "scene",
    "frame_total",
    "cpu_mark_leaves",
    "cpu_trans_setup",
    "cpu_water",
    "cpu_particles",
    "cpu_alias",
    "cpu_sprite",
    "cpu_brush_trans",
    "cpu_water_upload",
};

static const int g_section_kind[PROF_NUM_SECTIONS] = {
    SK_LEAF,       // clear
    SK_LEAF,       // world_opaque
    SK_LEAF,       // viewmodel
    SK_LEAF,       // polyblend
    SK_LEAF,       // hud_2d
    SK_LEAF,       // swap
    SK_CONTAINER,  // scene
    SK_CONTAINER,  // frame_total
    SK_CPU_ONLY,   // cpu_mark_leaves
    SK_CPU_ONLY,   // cpu_trans_setup
    SK_CPU_ONLY,   // cpu_water
    SK_CPU_ONLY,   // cpu_particles
    SK_CPU_ONLY,   // cpu_alias
    SK_CPU_ONLY,   // cpu_sprite
    SK_CPU_ONLY,   // cpu_brush_trans
    SK_CPU_ONLY,   // cpu_water_upload
};

// ---- state ----------------------------------------------------------------

// Per-frame intervals for CPU_ONLY sections. Particles / water / alias are
// invoked multiple times per frame (intercalated), so each Begin/End pair
// needs its own pair of GPU timestamps. We sum them at drain time.
#define PROF_MAX_INTERVALS 16

typedef struct {
    GLuint  q_t0;        // GL_TIMESTAMP (start); LEAF/CONTAINER only
    GLuint  q_t1;        // GL_TIMESTAMP (end);   LEAF/CONTAINER only
    GLuint  q_prims;     // GL_PRIMITIVES_GENERATED (LEAF only)
    GLuint  q_frags;     // GL_SAMPLES_PASSED       (LEAF only)
    // CPU_ONLY: a pool of timestamp pairs, one per Begin/End interval.
    GLuint  q_i0[PROF_MAX_INTERVALS];
    GLuint  q_i1[PROF_MAX_INTERVALS];
    int     n_intervals; // how many of the q_i0/q_i1 pairs were used this frame
    double  cpu_start;   // Sys_FloatTime at Begin of the current interval
    double  cpu_elapsed; // LEAF/CONTAINER: last interval; CPU_ONLY: accumulated this frame
    int     draw_calls;  // counter this frame
    int     active;      // LEAF/CONTAINER: query issued, pending readback.
                         // CPU_ONLY: slot is open this frame (accepts Begin/End).
    int     pending;     // CPU_ONLY: frame closed, intervals finalized, awaiting GPU readback.
    int     in_interval; // CPU_ONLY: currently inside a Begin/End pair
    int     sample_index;// which sample slot this slot's data will fill
} prof_slot_t;

typedef struct {
    int        gpu_us;
    int        cpu_us;
    int        draw_calls;
    int        tris;        // GL_PRIMITIVES_GENERATED
    int        frags;       // GL_SAMPLES_PASSED
} prof_sample_t;

typedef struct {
    prof_slot_t    ring[PROF_RING_SIZE];
    prof_sample_t  samples[PROF_MAX_SAMPLES];
    int            samples_filled;
} prof_section_state_t;

typedef enum {
    PS_IDLE,
    PS_ARMED,      // Prof_Start was called; waiting for the first sampled frame
    PS_RUNNING,    // actively sampling
    PS_FINISHING,  // all samples taken; draining rings then writing JSON
} prof_state_t;

// Two sampling modes:
//   - DEMO: fires on specific network frames from g_target_frames[] during
//     a timedemo. Deterministic, reproducible across runs of the same demo.
//   - LIVE: fires every `profile_live_every` render frames during normal
//     gameplay. Not deterministic, but necessary because pp_framecount only
//     advances in timedemo, so the demo mode can't see gameplay.
typedef enum {
    PM_DEMO,
    PM_LIVE,
} prof_mode_t;

static prof_state_t             g_state = PS_IDLE;
static prof_mode_t              g_mode = PM_DEMO;
static prof_section_state_t     g_sections[PROF_NUM_SECTIONS];
static int                      g_ring_head = 0;         // which ring slot index this frame uses
static int                      g_next_target_idx = 0;   // index into g_target_frames (DEMO mode)
static int                      g_live_render_frame = 0; // counter since StartLive (LIVE mode)
static int                      g_sampling_this_frame = 0;
static int                      g_current_sample = 0;    // 0..PROF_MAX_SAMPLES-1
static char                     g_demo_name[128] = "";
static char                     g_out_path[MAX_OSPATH] = "";

static cvar_t prof_out        = {"profile_out", "profile.json", true};
static cvar_t prof_live_every = {"profile_live_every", "15", true}; // sample every Nth render frame in live mode

// ---- helpers --------------------------------------------------------------

static int compare_ints (const void *a, const void *b) {
    int ia = *(const int*)a, ib = *(const int*)b;
    return (ia > ib) - (ia < ib);
}

static int percentile (int *sorted, int n, int p) {
    if (n <= 0) return 0;
    int idx = (p * (n - 1) + 50) / 100;
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

static int mean_of (int *a, int n) {
    if (n <= 0) return 0;
    long long s = 0;
    for (int i = 0; i < n; ++i) s += a[i];
    return (int)(s / n);
}

// Determine if network-frame `net_frame` is one we want to sample.
// Returns sample index (0..N-1) or -1 if not a target.
static int target_index_for (int net_frame) {
    // g_target_frames is sorted ascending. Linear scan is fine for 100 entries.
    if (g_next_target_idx >= PROF_MAX_SAMPLES) return -1;
    if (net_frame < g_target_frames[g_next_target_idx]) return -1;
    // Could be the current target or we skipped past it (e.g. variable net cadence).
    while (g_next_target_idx < PROF_MAX_SAMPLES &&
           net_frame > g_target_frames[g_next_target_idx]) {
        g_next_target_idx++;
    }
    if (g_next_target_idx >= PROF_MAX_SAMPLES) return -1;
    if (net_frame == g_target_frames[g_next_target_idx]) {
        return g_next_target_idx++;
    }
    return -1;
}

// ---- public API -----------------------------------------------------------

void Prof_Init (void) {
    memset(g_sections, 0, sizeof(g_sections));
    Cvar_RegisterVariable(&prof_out);
    Cvar_RegisterVariable(&prof_live_every);
    for (int s = 0; s < PROF_NUM_SECTIONS; ++s) {
        for (int r = 0; r < PROF_RING_SIZE; ++r) {
            prof_slot_t *slot = &g_sections[s].ring[r];
            if (g_section_kind[s] == SK_CPU_ONLY) {
                // Pool of timestamp pairs, one per Begin/End interval.
                for (int k = 0; k < PROF_MAX_INTERVALS; ++k) {
                    glGenQueries(1, &slot->q_i0[k]);
                    glGenQueries(1, &slot->q_i1[k]);
                }
                continue;
            }
            glGenQueries(1, &slot->q_t0);
            glGenQueries(1, &slot->q_t1);
            if (g_section_kind[s] == SK_LEAF) {
                glGenQueries(1, &slot->q_prims);
                glGenQueries(1, &slot->q_frags);
            }
        }
    }
}

static void reset_capture_state (void) {
    for (int s = 0; s < PROF_NUM_SECTIONS; ++s) {
        g_sections[s].samples_filled = 0;
        memset(g_sections[s].samples, 0, sizeof(g_sections[s].samples));
        for (int r = 0; r < PROF_RING_SIZE; ++r) {
            g_sections[s].ring[r].active = 0;
            g_sections[s].ring[r].pending = 0;
            g_sections[s].ring[r].n_intervals = 0;
            g_sections[s].ring[r].in_interval = 0;
        }
    }
    g_ring_head = 0;
    g_next_target_idx = 0;
    g_live_render_frame = 0;
    g_current_sample = 0;
    g_sampling_this_frame = 0;
    _snprintf(g_out_path, sizeof(g_out_path), "%s/carviary/%s",
              host_parms.basedir, prof_out.string);
}

void Prof_Start (const char *demo) {
    if (g_state != PS_IDLE) {
        Con_Printf("profile: already running\n");
        return;
    }
    if (demo && demo[0]) {
        strncpy(g_demo_name, demo, sizeof(g_demo_name)-1);
        g_demo_name[sizeof(g_demo_name)-1] = 0;
    } else {
        g_demo_name[0] = 0;
    }
    reset_capture_state();
    g_mode = PM_DEMO;
    g_state = PS_ARMED;
    Con_Printf("profile: armed for demo %s, output %s\n",
               g_demo_name[0] ? g_demo_name : "<current>", g_out_path);
}

void Prof_StartLive (void) {
    if (g_state != PS_IDLE) {
        Con_Printf("profile: already running (use profile_live_stop first)\n");
        return;
    }
    g_demo_name[0] = 0; // no demo in live mode
    reset_capture_state();
    g_mode = PM_LIVE;
    g_state = PS_ARMED;
    int every = (int)prof_live_every.value;
    if (every < 1) every = 1;
    Con_Printf("profile: live capture armed, every %d render frames, %d samples max, output %s\n",
               every, PROF_MAX_SAMPLES, g_out_path);
    Con_Printf("profile: it will stop automatically at %d samples, or run 'profile_live_stop' to stop early\n",
               PROF_MAX_SAMPLES);
}

void Prof_StopLive (void) {
    if (g_state == PS_IDLE) {
        Con_Printf("profile: not running\n");
        return;
    }
    if (g_mode != PM_LIVE) {
        Con_Printf("profile: not in live mode (use demo mode's own stop path)\n");
        return;
    }
    // Transition to FINISHING so pending GL queries drain on the next few
    // frames, then Prof_Finish runs and writes JSON.
    g_state = PS_FINISHING;
    Con_Printf("profile: live capture stopping, draining queries...\n");
}

int Prof_IsRunning (void) {
    return g_state != PS_IDLE;
}

int Prof_IsSamplingFrame (void) {
    return g_sampling_this_frame;
}

// Drain one ring slot: read GL query results into the samples array when ready.
// Works for all three kinds:
//   - LEAF/CONTAINER: async readback of q_t0/q_t1 (+ prims/frags for LEAF).
//   - CPU_ONLY: sums all recorded intervals' (t1-t0) and commits gpu_us.
// CPU time and draw_calls are already committed at Prof_EndFrame time for
// CPU_ONLY (synchronous values), so this pass only adds the GPU column.
static void drain_slot (int s, int slot_idx) {
    prof_slot_t *slot = &g_sections[s].ring[slot_idx];

    if (g_section_kind[s] == SK_CPU_ONLY) {
        if (!slot->pending) return;
        if (slot->n_intervals == 0) {
            slot->pending = 0; // nothing to drain
            return;
        }
        // Wait until the *last* interval's q_i1 is available. By GL ordering
        // all earlier queries are ready too once the last one is.
        GLuint available = 0;
        glGetQueryObjectuiv(slot->q_i1[slot->n_intervals - 1],
                            GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) return;

        GLuint64 total_ns = 0;
        for (int k = 0; k < slot->n_intervals; ++k) {
            GLuint64 t0 = 0, t1 = 0;
            glGetQueryObjectui64v(slot->q_i0[k], GL_QUERY_RESULT, &t0);
            glGetQueryObjectui64v(slot->q_i1[k], GL_QUERY_RESULT, &t1);
            if (t1 > t0) total_ns += (t1 - t0);
        }
        int idx = slot->sample_index;
        if (idx >= 0 && idx < PROF_MAX_SAMPLES) {
            g_sections[s].samples[idx].gpu_us = (int)(total_ns / 1000);
        }
        slot->pending = 0;
        return;
    }

    // LEAF / CONTAINER path
    if (!slot->active) return;

    GLuint available = 0;
    glGetQueryObjectuiv(slot->q_t1, GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available) return; // not ready yet; try next frame

    GLuint64 t0 = 0, t1 = 0, prims = 0, frags = 0;
    glGetQueryObjectui64v(slot->q_t0, GL_QUERY_RESULT, &t0);
    glGetQueryObjectui64v(slot->q_t1, GL_QUERY_RESULT, &t1);
    if (g_section_kind[s] == SK_LEAF) {
        glGetQueryObjectui64v(slot->q_prims, GL_QUERY_RESULT, &prims);
        glGetQueryObjectui64v(slot->q_frags, GL_QUERY_RESULT, &frags);
    }

    int idx = slot->sample_index;
    if (idx >= 0 && idx < PROF_MAX_SAMPLES) {
        prof_sample_t *sp = &g_sections[s].samples[idx];
        sp->gpu_us     = (int)((t1 - t0) / 1000);
        sp->cpu_us     = (int)(slot->cpu_elapsed * 1e6);
        sp->draw_calls = slot->draw_calls;
        sp->tris       = (int)prims;
        sp->frags      = (int)frags;
        if (idx + 1 > g_sections[s].samples_filled)
            g_sections[s].samples_filled = idx + 1;
    }
    slot->active = 0;
}

// CPU_ONLY commit: copies CPU time, draw calls, and marks the slot pending
// so drain_slot picks up the GPU timestamps on a later frame.
static void commit_cpu_only_sections (void) {
    if (!g_sampling_this_frame) return;
    for (int s = 0; s < PROF_NUM_SECTIONS; ++s) {
        if (g_section_kind[s] != SK_CPU_ONLY) continue;
        prof_slot_t *slot = &g_sections[s].ring[g_ring_head];
        if (!slot->active) continue;
        int idx = slot->sample_index;
        if (idx >= 0 && idx < PROF_MAX_SAMPLES) {
            prof_sample_t *sp = &g_sections[s].samples[idx];
            sp->gpu_us     = 0; // placeholder; filled in by drain_slot
            sp->cpu_us     = (int)(slot->cpu_elapsed * 1e6);
            sp->draw_calls = slot->draw_calls;
            sp->tris       = 0;
            sp->frags      = 0;
            if (idx + 1 > g_sections[s].samples_filled)
                g_sections[s].samples_filled = idx + 1;
        }
        slot->active = 0;
        slot->pending = (slot->n_intervals > 0) ? 1 : 0;
    }
}

static int any_slot_active (void) {
    for (int s = 0; s < PROF_NUM_SECTIONS; ++s)
        for (int r = 0; r < PROF_RING_SIZE; ++r) {
            prof_slot_t *slot = &g_sections[s].ring[r];
            if (slot->active || slot->pending) return 1;
        }
    return 0;
}

void Prof_BeginFrame (void) {
    // Always try to drain older ring slots -- cheap when profiler is idle
    // because all slots are !active.
    if (g_state == PS_IDLE) return;

    for (int s = 0; s < PROF_NUM_SECTIONS; ++s)
        for (int r = 0; r < PROF_RING_SIZE; ++r)
            drain_slot(s, r);

    if (g_state == PS_FINISHING) {
        if (!any_slot_active()) {
            Prof_Finish();
        }
        g_sampling_this_frame = 0;
        return;
    }

    // Is this frame a sampling target?
    //  - DEMO mode: match net-frame index (cls.pp_framecount) against g_target_frames.
    //  - LIVE mode: fire every Nth render frame since StartLive, up to PROF_MAX_SAMPLES.
    int sample_idx = -1;
    if (g_state == PS_ARMED || g_state == PS_RUNNING) {
        if (g_mode == PM_DEMO) {
            sample_idx = target_index_for(cls.pp_framecount);
        } else { // PM_LIVE
            int every = (int)prof_live_every.value;
            if (every < 1) every = 1;
            if (g_current_sample < PROF_MAX_SAMPLES &&
                (g_live_render_frame % every) == 0) {
                sample_idx = g_current_sample;
            }
            g_live_render_frame++;
        }
    }

    if (sample_idx >= 0) {
        g_state = PS_RUNNING;
        g_sampling_this_frame = 1;
        g_current_sample = sample_idx;
        g_ring_head = sample_idx % PROF_RING_SIZE;
    } else {
        g_sampling_this_frame = 0;
    }
}

void Prof_EndFrame (void) {
    if (g_state == PS_IDLE) return;
    // CPU-only sections are synchronous: copy accumulated cpu_elapsed
    // straight to the sample array now, while g_sampling_this_frame is still 1.
    commit_cpu_only_sections();
    // In LIVE mode we advance the sample counter here (one sampled frame = one
    // slot used). In DEMO mode target_index_for already advanced g_next_target_idx.
    if (g_sampling_this_frame && g_mode == PM_LIVE) {
        g_current_sample++;
    }
    g_sampling_this_frame = 0;
    // If we've emitted all samples, drop into FINISHING so we wait for the
    // last ones' GL queries to drain.
    if (g_state == PS_RUNNING) {
        int done = (g_mode == PM_DEMO)
            ? (g_next_target_idx >= PROF_MAX_SAMPLES)
            : (g_current_sample >= PROF_MAX_SAMPLES);
        if (done) g_state = PS_FINISHING;
    }
}

void Prof_BeginSection (prof_section_t s) {
    if (!g_sampling_this_frame) return;
    if ((unsigned)s >= PROF_NUM_SECTIONS) return;
    prof_slot_t *slot = &g_sections[s].ring[g_ring_head];

    if (g_section_kind[s] == SK_CPU_ONLY) {
        // First Begin of the frame opens the slot and clears accumulators.
        // Subsequent Begins just restart the interval timer; End will add
        // (now - cpu_start) to cpu_elapsed. This handles water / particles
        // being called twice per frame.
        if (!slot->active) {
            slot->active = 1;
            slot->in_interval = 0;
            slot->sample_index = g_current_sample;
            slot->draw_calls = 0;
            slot->cpu_elapsed = 0.0;
            slot->n_intervals = 0;
        }
        if (slot->in_interval) return; // nested Begin of same section: ignore
        slot->in_interval = 1;
        slot->cpu_start = Sys_FloatTime();
        // Also open a GPU timestamp pair, unless we've run out of pool.
        if (slot->n_intervals < PROF_MAX_INTERVALS) {
            glQueryCounter(slot->q_i0[slot->n_intervals], GL_TIMESTAMP);
        }
        return;
    }

    // LEAF / CONTAINER path: one Begin/End per frame, backed by GL queries.
    if (slot->active) return; // not drained yet; skip this sample
    slot->active = 1;
    slot->sample_index = g_current_sample;
    slot->draw_calls = 0;
    slot->cpu_start = Sys_FloatTime();
    glQueryCounter(slot->q_t0, GL_TIMESTAMP);
    if (g_section_kind[s] == SK_LEAF) {
        glBeginQuery(GL_PRIMITIVES_GENERATED, slot->q_prims);
        glBeginQuery(GL_SAMPLES_PASSED,       slot->q_frags);
    }
}

void Prof_EndSection (prof_section_t s) {
    if (!g_sampling_this_frame) return;
    if ((unsigned)s >= PROF_NUM_SECTIONS) return;
    prof_slot_t *slot = &g_sections[s].ring[g_ring_head];

    if (g_section_kind[s] == SK_CPU_ONLY) {
        if (!slot->active || !slot->in_interval) return;
        slot->cpu_elapsed += Sys_FloatTime() - slot->cpu_start;
        slot->in_interval = 0;
        // Close the matching GPU timestamp pair.
        if (slot->n_intervals < PROF_MAX_INTERVALS) {
            glQueryCounter(slot->q_i1[slot->n_intervals], GL_TIMESTAMP);
            slot->n_intervals++;
        }
        return;
    }

    if (!slot->active) return;
    if (g_section_kind[s] == SK_LEAF) {
        glEndQuery(GL_SAMPLES_PASSED);
        glEndQuery(GL_PRIMITIVES_GENERATED);
    }
    glQueryCounter(slot->q_t1, GL_TIMESTAMP);
    slot->cpu_elapsed = Sys_FloatTime() - slot->cpu_start;
}

void Prof_CountDraw (int vertices) {
    (void)vertices;
    if (!g_sampling_this_frame) return;
    // Increment the draw counter only for sections that are *currently inside*
    // a Begin/End pair. For LEAF/CONTAINER sections that means slot->active
    // (they have exactly one Begin/End per frame, so active==inside). For
    // CPU_ONLY sections we must use in_interval instead, because CPU_ONLY
    // slots stay active across multiple intervals within a frame -- otherwise
    // a CPU_ONLY section that closed early (like cpu_mark_leaves) would
    // keep accumulating every draw that happens afterwards.
    for (int s = 0; s < PROF_NUM_SECTIONS; ++s) {
        prof_slot_t *slot = &g_sections[s].ring[g_ring_head];
        if (!slot->active) continue;
        if (g_section_kind[s] == SK_CPU_ONLY && !slot->in_interval) continue;
        slot->draw_calls++;
    }
}

// ---- JSON output ----------------------------------------------------------

static void write_dist (FILE *f, const char *key, int *values, int n, int is_int_counter) {
    int sorted[PROF_MAX_SAMPLES];
    memcpy(sorted, values, n * sizeof(int));
    qsort(sorted, n, sizeof(int), compare_ints);
    fprintf(f,
        "      \"%s\": { \"mean\": %d, \"p50\": %d, \"p95\": %d, \"p99\": %d, \"min\": %d, \"max\": %d }",
        key,
        mean_of(values, n),
        percentile(sorted, n, 50),
        percentile(sorted, n, 95),
        percentile(sorted, n, 99),
        sorted[0],
        sorted[n-1]);
    (void)is_int_counter;
}

void Prof_Finish (void) {
    if (g_state == PS_IDLE) return;

    FILE *f = fopen(g_out_path, "w");
    if (!f) {
        Con_Printf("profile: ERROR could not write %s\n", g_out_path);
        g_state = PS_IDLE;
        return;
    }

    const char *gl_ver = (const char *)glGetString(GL_VERSION);
    if (!gl_ver) gl_ver = "unknown";

    fprintf(f, "{\n");
    fprintf(f, "  \"build\": \"carviary\",\n");
    fprintf(f, "  \"gl_version\": \"%s\",\n", gl_ver);
    fprintf(f, "  \"demo\": \"%s\",\n", g_demo_name);
    fprintf(f, "  \"map\": \"%s\",\n", cl.levelname);
    fprintf(f, "  \"resolution\": [%d, %d],\n", glwidth, glheight);

    int n_min = PROF_MAX_SAMPLES;
    for (int s = 0; s < PROF_NUM_SECTIONS; ++s)
        if (g_sections[s].samples_filled < n_min)
            n_min = g_sections[s].samples_filled;
    fprintf(f, "  \"samples\": %d,\n", n_min);
    fprintf(f, "  \"sections\": {\n");

    int first = 1;
    for (int s = 0; s < PROF_NUM_SECTIONS; ++s) {
        int n = g_sections[s].samples_filled;
        if (n <= 0) continue;
        int gpu[PROF_MAX_SAMPLES], cpu[PROF_MAX_SAMPLES], dcs[PROF_MAX_SAMPLES];
        int tris[PROF_MAX_SAMPLES], frags[PROF_MAX_SAMPLES];
        for (int i = 0; i < n; ++i) {
            gpu[i]  = g_sections[s].samples[i].gpu_us;
            cpu[i]  = g_sections[s].samples[i].cpu_us;
            dcs[i]  = g_sections[s].samples[i].draw_calls;
            tris[i] = g_sections[s].samples[i].tris;
            frags[i]= g_sections[s].samples[i].frags;
        }
        if (!first) fprintf(f, ",\n");
        first = 0;
        fprintf(f, "    \"%s\": {\n", g_section_names[s]);
        write_dist(f, "gpu_us",     gpu, n, 0); fprintf(f, ",\n");
        write_dist(f, "cpu_us",     cpu, n, 0); fprintf(f, ",\n");
        write_dist(f, "draw_calls", dcs, n, 1);
        if (g_section_kind[s] == SK_LEAF) {
            fprintf(f, ",\n");
            write_dist(f, "tris",          tris,  n, 1); fprintf(f, ",\n");
            write_dist(f, "frags_visible", frags, n, 1); fprintf(f, "\n");
        } else {
            fprintf(f, "\n");
        }
        fprintf(f, "    }");
    }
    fprintf(f, "\n  }\n}\n");
    fclose(f);
    Con_Printf("profile: wrote %s (%d samples)\n", g_out_path, n_min);
    g_state = PS_IDLE;
}
