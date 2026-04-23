// gl_true_trails.cpp -- polyline-per-entity ribbons for projectile trails.
//
// Replaces the puff-based R_RocketTrail / spike trails with camera-facing
// ribbons. Each projectile entity owns a polyline of timestamped points
// (head-to-tail). At draw time the whole polyline is tesselated as a
// continuous strip using bisectors at interior vertices -- no seams, because
// adjacent segments literally share the same vertex.
//
// Near-camera behavior (cvar r_trail_stipple):
//   1 = Bayer 4x4 dither. Far = solid, near = sparse/transparent. The
//       pixel pattern stays coherent per-pixel regardless of the segment
//       a fragment comes from, so the whole ribbon reads as one dithered
//       object.
//   0 = smooth alpha fade, far = nominal alpha, near = transparent.
//
// Underwater: spawn bubble particles along the sub-segment instead of
// extending the ribbon. Matches the project rule "all effects underwater
// must use bubbles".

#include "quakedef.h"
#include "gl_render.h"
#include <stdint.h>
#include <math.h>

extern vec3_t vpn, vright, vup;
extern void R_SpawnBubbleParticle (const vec3_t pos, float scale);
extern void R_SpawnEmberParticle  (const vec3_t pos, const vec3_t vel, float scale, float life);

// ---------------------------------------------------------------------------
// Trail kinds.
// ---------------------------------------------------------------------------
typedef enum {
    TRAIL_SPIKE = 0,
    TRAIL_WIZSPIKE,
    TRAIL_MISSILE,
    TRAIL_KIND_COUNT
} ttrail_kind_t;

typedef struct {
    float width;
    float life;            // point lifetime in seconds
    float r, g, b;         // base "cold" color (end of life)
    float alpha;
    int   additive;
    int   cools;           // 1 = start hot orange, cool to (r,g,b) quickly
    float distort_pos;     // lateral jitter magnitude in world units
    float distort_width;   // width wobble amplitude (0..1 of base width)
    int   rainbow;         // 1 = cycle HSV by age, overrides cools
} trail_spec_t;

static cvar_t r_trail_near      = {"r_trail_near",      "20"};
static cvar_t r_trail_far       = {"r_trail_far",       "40"};
static cvar_t r_rainbow_trails  = {"r_rainbow_trails",  "0"};   // fun experiment

static const trail_spec_t g_specs[TRAIL_KIND_COUNT] = {
    /* SPIKE    */ { 0.1f, 0.30f, 0.144f, 0.144f, 0.144f, 0.85f, 0, 0, 0.0f, 0.0f, 0 },
    /* WIZSPIKE */ { 0.6f, 0.30f, 0.25f,  1.00f,  0.35f,  0.95f, 1, 0, 0.0f, 0.0f, 0 },
    /* MISSILE  */ { 2.8f, 0.91f, 0.30f,  0.30f,  0.30f,  0.55f, 0, 1, 2.2f, 0.5f, 0 },
};

// Resolve the effective spec for a given trail kind, applying the
// r_rainbow_trails cvar when relevant. Returns a pointer to either the
// static base spec or a modified static copy. Not thread-safe (single GL
// thread, used within one draw frame).
static const trail_spec_t *Trail_ResolveSpec (ttrail_kind_t kind)
{
    const trail_spec_t *base = &g_specs[kind];
    if (r_rainbow_trails.value != 0.0f && kind == TRAIL_MISSILE) {
        static trail_spec_t override_spec;
        override_spec = *base;
        override_spec.life     = 10.0f;   // much longer rainbow tail
        override_spec.cools    = 0;        // don't override with orange
        override_spec.rainbow  = 1;
        override_spec.alpha    = 1.0f;     // fully saturated
        return &override_spec;
    }
    return base;
}

// ---------------------------------------------------------------------------
// Per-entity polyline. Each entity (indexed by cl_entities offset) holds a
// ring of points. When a new oldorg->origin update arrives we push `origin`
// as a new point (`oldorg` matches the previous top if the entity was live
// last frame; otherwise we push both to seed the trail).
// ---------------------------------------------------------------------------
#define TTRAIL_POINTS_PER_ENT 128  // headroom for long r_rainbow_trails tails

typedef struct {
    vec3_t pos;
    float  spawn_time;
} ttrail_pt_t;

typedef struct {
    ttrail_pt_t pts[TTRAIL_POINTS_PER_ENT];
    int         head;       // next write slot
    int         count;      // live points
    ttrail_kind_t kind;
    float       last_touch; // cl.time of last push, for GC
    int         active;
} ttrail_ent_t;

#define TTRAIL_MAX_ENTS 4096   // matches MAX_EDICTS
static ttrail_ent_t g_trails[TTRAIL_MAX_ENTS];

// ---------------------------------------------------------------------------
// GL resources.
// ---------------------------------------------------------------------------
static GLShader R_TTrailShader;
static GLint    R_TTrailShader_u_mvp       = -1;
static GLint    R_TTrailShader_u_eye       = -1;
static GLint    R_TTrailShader_u_near      = -1;
static GLint    R_TTrailShader_u_far       = -1;
static GLint    R_TTrailShader_u_additive  = -1;
static qboolean ttrail_ok = false;

typedef struct {
    float x, y, z;
    unsigned char r, g, b, a;
} ttrail_vtx_t;

#define TTRAIL_MAX_VERTS 32768
static ttrail_vtx_t ttrail_verts[TTRAIL_MAX_VERTS];

static DynamicVBO ttrail_vbo;
static int        ttrail_vbo_inited = 0;

// Cvars.
//   r_trail_stipple : 1 = Bayer dither, 0 = alpha fade
//   r_trail_near    : distance at which the trail is fully transparent
//                     (near the camera = see-through)
//   r_trail_far     : distance at which the trail reaches nominal opacity
// ---------------------------------------------------------------------------
// Shader.
// ---------------------------------------------------------------------------
static qboolean R_EnsureTTrailShader (void)
{
    if (ttrail_ok) return true;
    const char *vs =
        "#version 330 core\n"
        "layout(location = 0) in vec3  a_pos;\n"
        "layout(location = 1) in vec4  a_color;\n"
        "uniform mat4 u_mvp;\n"
        "out vec4 v_color;\n"
        "out vec3 v_world;\n"
        "void main() {\n"
        "    v_color = a_color;\n"
        "    v_world = a_pos;\n"
        "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
        "}\n";
    const char *fs =
        "#version 330 core\n"
        "in vec4 v_color;\n"
        "in vec3 v_world;\n"
        "uniform vec3  u_eye;\n"
        "uniform float u_near;\n"
        "uniform float u_far;\n"
        "uniform int   u_additive;\n"
        "layout(location = 0) out vec4 frag_color;\n"
        "layout(location = 1) out vec4 frag_fbmask;\n"
        "void main() {\n"
        // 0 at/under u_near (transparent), 1 at/over u_far (solid).
        "    float d = length(v_world - u_eye);\n"
        "    float dist_solid = clamp((d - u_near) / max(u_far - u_near, 1.0), 0.0, 1.0);\n"
        "    vec4 c = v_color;\n"
        "    if (u_additive == 1) c.rgb *= dist_solid;\n"
        "    else                 c.a   *= dist_solid;\n"
        "    frag_color  = c;\n"
        "    frag_fbmask = vec4(0.0);\n"
        "}\n";
    char err[512];
    if (!GLShader_Build(&R_TTrailShader, vs, fs, err, sizeof(err))) {
        Con_Printf("true-trail shader failed: %s\n", err);
        return false;
    }
    R_TTrailShader_u_mvp      = GLShader_Uniform(&R_TTrailShader, "u_mvp");
    R_TTrailShader_u_eye      = GLShader_Uniform(&R_TTrailShader, "u_eye");
    R_TTrailShader_u_near     = GLShader_Uniform(&R_TTrailShader, "u_near");
    R_TTrailShader_u_far      = GLShader_Uniform(&R_TTrailShader, "u_far");
    R_TTrailShader_u_additive = GLShader_Uniform(&R_TTrailShader, "u_additive");
    ttrail_ok = true;
    return true;
}

static void R_EnsureTTrailVBO (void)
{
    if (ttrail_vbo_inited) return;
    DynamicVBO_Init(&ttrail_vbo, (GLsizei)sizeof(ttrail_verts));
    DynamicVBO_SetAttrib(&ttrail_vbo, 0, 3, GL_FLOAT,         GL_FALSE, sizeof(ttrail_vtx_t), offsetof(ttrail_vtx_t, x));
    DynamicVBO_SetAttrib(&ttrail_vbo, 1, 4, GL_UNSIGNED_BYTE, GL_TRUE,  sizeof(ttrail_vtx_t), offsetof(ttrail_vtx_t, r));
    ttrail_vbo_inited = 1;
}

// ---------------------------------------------------------------------------
// Entity index lookup. Returns -1 if ent is out of the cl_entities range.
// ---------------------------------------------------------------------------
extern entity_t cl_entities[];
static int TTrail_EntIndex (entity_t *ent)
{
    ptrdiff_t idx = ent - cl_entities;
    if (idx < 0 || idx >= TTRAIL_MAX_ENTS) return -1;
    return (int)idx;
}

// ---------------------------------------------------------------------------
// Push a point onto the entity's polyline. If the trail is new or there
// was a big time gap since the last push, reset the polyline.
// ---------------------------------------------------------------------------
static void TTrail_Push (int idx, ttrail_kind_t kind, const vec3_t pos, const vec3_t oldorg)
{
    ttrail_ent_t *tr = &g_trails[idx];
    float now = (float)cl.time;

    // Reset window: if rainbow missile (life=10s) is active, the "gap before
    // reset" has to be wider than the decimation interval, otherwise every
    // decimated skip would trigger a reset.
    float reset_gap = 0.25f;
    const trail_spec_t *sp = Trail_ResolveSpec(kind);
    float decimate_dt = 0.0f;
    if (sp->rainbow) {
        // Space points out so the ring covers spec->life with some slack.
        decimate_dt = sp->life / (float)(TTRAIL_POINTS_PER_ENT - 2);
        reset_gap   = decimate_dt * 3.0f + 0.1f;
    }

    // Reset if kind changed, never used, or gap in updates.
    if (!tr->active || tr->kind != kind || (now - tr->last_touch) > reset_gap) {
        tr->head = 0;
        tr->count = 0;
        tr->kind = kind;
        tr->active = 1;
        // Seed with the previous position so the first segment has length.
        ttrail_pt_t *p0 = &tr->pts[tr->head];
        VectorCopy(oldorg, p0->pos);
        p0->spawn_time = now;
        tr->head = (tr->head + 1) % TTRAIL_POINTS_PER_ENT;
        tr->count = 1;
        tr->last_touch = now;
        // Continue to append pos on first frame, below.
    } else if (decimate_dt > 0.0f && (now - tr->last_touch) < decimate_dt) {
        // Skip this push: ring budget says we sample at a lower rate so the
        // 10s tail actually fits in TTRAIL_POINTS_PER_ENT slots.
        return;
    }

    ttrail_pt_t *p = &tr->pts[tr->head];
    VectorCopy(pos, p->pos);
    p->spawn_time = now;
    tr->head = (tr->head + 1) % TTRAIL_POINTS_PER_ENT;
    if (tr->count < TTRAIL_POINTS_PER_ENT) tr->count++;
    tr->last_touch = now;
}

// ---------------------------------------------------------------------------
// Per-kind emission. Underwater -> bubbles, otherwise push a polyline point.
// ---------------------------------------------------------------------------
static void TTrail_Emit (ttrail_kind_t kind, const vec3_t start, const vec3_t end, entity_t *ent)
{
    int idx = TTrail_EntIndex(ent);
    if (idx < 0) return;

    vec3_t delta;
    VectorSubtract(end, start, delta);
    float total = (float)sqrt(DotProduct(delta, delta));
    if (total < 0.01f) return;

    int contents = Mod_PointInLeaf((float *)start, cl.worldmodel)->contents;
    if (contents == CONTENTS_SKY || contents == CONTENTS_LAVA)
        return;

    if (contents == CONTENTS_WATER || contents == CONTENTS_SLIME) {
        // Underwater: bubbles. Also reset the polyline so we don't draw a
        // stale segment from the last above-water push.
        g_trails[idx].active = 0;
        g_trails[idx].count  = 0;
        float bubble_step = 6.0f;
        float bubble_scale = g_specs[kind].width * 0.6f + 0.5f;
        for (float t = 0.0f; t < total; t += bubble_step) {
            float u = t / total;
            vec3_t pos = {
                start[0] + delta[0] * u,
                start[1] + delta[1] * u,
                start[2] + delta[2] * u,
            };
            R_SpawnBubbleParticle(pos, bubble_scale);
        }
        return;
    }

    TTrail_Push(idx, kind, end, start);

    // Rocket embers: spray a handful backwards each frame. The exhaust feels
    // alive when small glowing specks scatter behind the missile while the
    // main ribbon drifts in the air.
    if (kind == TRAIL_MISSILE) {
        vec3_t dir = { delta[0]/total, delta[1]/total, delta[2]/total };
        int n = 3 + (rand() & 3);   // 3..6 embers per frame
        for (int i = 0; i < n; ++i) {
            // Spawn near the back of the rocket (slightly behind `end`).
            float back = 2.0f + (rand() & 3);
            vec3_t pos = {
                end[0] - dir[0] * back,
                end[1] - dir[1] * back,
                end[2] - dir[2] * back,
            };
            // Base velocity: opposite direction of motion, with jitter on
            // the two axes perpendicular to travel (cone spray).
            float speed = 40.0f + (rand() & 31);
            vec3_t up_guess = { 0, 0, 1 };
            if (fabsf(dir[2]) > 0.9f) { up_guess[0] = 1; up_guess[2] = 0; }
            vec3_t ax1 = {
                dir[1]*up_guess[2] - dir[2]*up_guess[1],
                dir[2]*up_guess[0] - dir[0]*up_guess[2],
                dir[0]*up_guess[1] - dir[1]*up_guess[0],
            };
            float a1l = (float)sqrt(DotProduct(ax1, ax1));
            if (a1l > 1e-4f) { ax1[0]/=a1l; ax1[1]/=a1l; ax1[2]/=a1l; }
            vec3_t ax2 = {
                dir[1]*ax1[2] - dir[2]*ax1[1],
                dir[2]*ax1[0] - dir[0]*ax1[2],
                dir[0]*ax1[1] - dir[1]*ax1[0],
            };
            float j1 = (((rand() & 255) / 127.5f) - 1.0f) * 35.0f;
            float j2 = (((rand() & 255) / 127.5f) - 1.0f) * 35.0f;
            vec3_t vel = {
                -dir[0] * speed + ax1[0] * j1 + ax2[0] * j2,
                -dir[1] * speed + ax1[1] * j1 + ax2[1] * j2,
                -dir[2] * speed + ax1[2] * j1 + ax2[2] * j2,
            };
            float life  = 0.45f + (rand() & 31) * 0.01f;  // 0.45..0.76s
            float scale = 0.8f + (rand() & 15) * 0.06f;   // 0.8..1.7
            R_SpawnEmberParticle(pos, vel, scale, life);
        }
    }
}

// ---------------------------------------------------------------------------
// Public trail entry points.
// ---------------------------------------------------------------------------
void R_TrueTrail_Spike    (vec3_t start, vec3_t end, entity_t *ent)
{
    // Only ~1 in 5 spikes leaves a trail. Key the decision on the entity
    // index so the same spike is consistently with-or-without trail for
    // its whole flight (no flicker).
    int idx = (int)(ent - cl_entities);
    unsigned h = (unsigned)idx * 2654435761u;   // Knuth hash
    if ((h % 5u) != 0u) return;
    TTrail_Emit(TRAIL_SPIKE, start, end, ent);
}
void R_TrueTrail_WizSpike (vec3_t start, vec3_t end, entity_t *ent) { TTrail_Emit(TRAIL_WIZSPIKE, start, end, ent); }
void R_TrueTrail_Missile  (vec3_t start, vec3_t end, entity_t *ent) { TTrail_Emit(TRAIL_MISSILE,  start, end, ent); }

// ---------------------------------------------------------------------------
// Init / Clear.
// ---------------------------------------------------------------------------
void R_TrueTrail_Init (void)
{
    memset(g_trails, 0, sizeof(g_trails));
    Cvar_RegisterVariable(&r_trail_near);
    Cvar_RegisterVariable(&r_trail_far);
    Cvar_RegisterVariable(&r_rainbow_trails);
}

void R_TrueTrail_Clear (void)
{
    memset(g_trails, 0, sizeof(g_trails));
}

// ---------------------------------------------------------------------------
// Screen-perpendicular offset (unit). out = normalize(cross(dir, view_ray)).
// ---------------------------------------------------------------------------
static void TTrail_ScreenPerp (const vec3_t p, const vec3_t dir, vec3_t out)
{
    vec3_t view_ray = {
        p[0] - r_refdef.vieworg[0],
        p[1] - r_refdef.vieworg[1],
        p[2] - r_refdef.vieworg[2],
    };
    vec3_t perp = {
        dir[1] * view_ray[2] - dir[2] * view_ray[1],
        dir[2] * view_ray[0] - dir[0] * view_ray[2],
        dir[0] * view_ray[1] - dir[1] * view_ray[0],
    };
    float len = (float)sqrt(DotProduct(perp, perp));
    if (len < 1e-4f) { VectorCopy(vup, out); return; }
    out[0] = perp[0] / len;
    out[1] = perp[1] / len;
    out[2] = perp[2] / len;
}

// ---------------------------------------------------------------------------
// Tesselate one polyline. Writes up to (count-1)*24 vertices (8 tris per
// segment = 4 cross-stripes with alpha ramp 0/.7/1/.7/0).
//
// Continuity: for each interior vertex we compute the perpendicular using
// the bisector of the two adjacent segment directions. The shared vertex
// therefore has ONE offset vector, shared by both segments -> no gap.
// ---------------------------------------------------------------------------
// Map a hue in [0,1] to fully saturated RGB in [0,1].
static void Trail_HueToRGB (float h, float *r, float *g, float *b)
{
    h -= floorf(h);
    float hp = h * 6.0f;
    float x = 1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f);
    if      (hp < 1.0f) { *r = 1.0f; *g = x;    *b = 0.0f; }
    else if (hp < 2.0f) { *r = x;    *g = 1.0f; *b = 0.0f; }
    else if (hp < 3.0f) { *r = 0.0f; *g = 1.0f; *b = x;    }
    else if (hp < 4.0f) { *r = 0.0f; *g = x;    *b = 1.0f; }
    else if (hp < 5.0f) { *r = x;    *g = 0.0f; *b = 1.0f; }
    else                { *r = 1.0f; *g = 0.0f; *b = x;    }
}

static int TTrail_TesselateTrail (const ttrail_ent_t *tr, float now,
                                  int want_additive, int start_nverts)
{
    const trail_spec_t *spec = Trail_ResolveSpec(tr->kind);
    if (spec->additive != want_additive) return start_nverts;
    if (tr->count < 2) return start_nverts;

    // Collect points in chronological order into a local array. The ring's
    // oldest entry sits at (head - count + N) for N in [0..count).
    vec3_t pts[TTRAIL_POINTS_PER_ENT];
    float  ages01[TTRAIL_POINTS_PER_ENT];
    float  spawns[TTRAIL_POINTS_PER_ENT];   // seed for per-point noise
    int    nkeep = 0;
    for (int i = 0; i < tr->count; ++i) {
        int slot = (tr->head - tr->count + i + TTRAIL_POINTS_PER_ENT) % TTRAIL_POINTS_PER_ENT;
        const ttrail_pt_t *pt = &tr->pts[slot];
        float age = now - pt->spawn_time;
        if (age < 0.0f || age >= spec->life) continue;
        VectorCopy(pt->pos, pts[nkeep]);
        ages01[nkeep] = age / spec->life;
        spawns[nkeep] = pt->spawn_time;
        nkeep++;
    }
    if (nkeep < 2) return start_nverts;

    // Apply lateral distortion to each point. The displacement grows
    // linearly with age (0 at spawn, full at end of life) so the head of
    // the trail stays clean on the projectile. Noise is seeded from the
    // point's spawn_time, so a given point keeps the same base offset
    // across frames -- the ribbon does not jitter frame-to-frame; it
    // drifts smoothly via `now` modulating the sines.
    if (spec->distort_pos > 0.0f) {
        for (int i = 0; i < nkeep; ++i) {
            // Raw direction between neighbors (approx. tangent at this point)
            // for building two lateral axes.
            vec3_t t = {0,0,0};
            if (i > 0 && i < nkeep - 1) {
                t[0] = pts[i+1][0] - pts[i-1][0];
                t[1] = pts[i+1][1] - pts[i-1][1];
                t[2] = pts[i+1][2] - pts[i-1][2];
            } else if (i == 0) {
                t[0] = pts[1][0] - pts[0][0];
                t[1] = pts[1][1] - pts[0][1];
                t[2] = pts[1][2] - pts[0][2];
            } else {
                t[0] = pts[i][0] - pts[i-1][0];
                t[1] = pts[i][1] - pts[i-1][1];
                t[2] = pts[i][2] - pts[i-1][2];
            }
            float tl = (float)sqrt(DotProduct(t, t));
            if (tl < 1e-4f) continue;
            t[0]/=tl; t[1]/=tl; t[2]/=tl;

            // Pick any vector not parallel to t, cross for axis1, axis2.
            vec3_t up_guess = { 0, 0, 1 };
            if (fabsf(t[2]) > 0.9f) { up_guess[0]=1; up_guess[2]=0; }
            vec3_t ax1 = {
                t[1]*up_guess[2] - t[2]*up_guess[1],
                t[2]*up_guess[0] - t[0]*up_guess[2],
                t[0]*up_guess[1] - t[1]*up_guess[0],
            };
            float a1l = (float)sqrt(DotProduct(ax1, ax1));
            if (a1l < 1e-4f) continue;
            ax1[0]/=a1l; ax1[1]/=a1l; ax1[2]/=a1l;
            vec3_t ax2 = {
                t[1]*ax1[2] - t[2]*ax1[1],
                t[2]*ax1[0] - t[0]*ax1[2],
                t[0]*ax1[1] - t[1]*ax1[0],
            };

            // Per-point seed: fractional part of spawn_time * large prime
            // gives a stable offset in [0, 2pi) for this point.
            float seed = spawns[i] * 12.9898f;
            float phase1 = seed;
            float phase2 = seed * 1.7f + 2.3f;
            // Time-varying component (drift). Slow enough to read as smoke
            // pushed by air, not jitter.
            float amp = spec->distort_pos * ages01[i];
            float d1 = sinf(phase1 + now * 1.8f) * amp;
            float d2 = sinf(phase2 + now * 1.3f) * amp;
            // Vertical sag/lift (upward drift is a stronger signature).
            float upd = sinf(seed * 2.1f + now * 1.1f) * amp * 0.6f + amp * 0.4f;

            pts[i][0] += ax1[0]*d1 + ax2[0]*d2;
            pts[i][1] += ax1[1]*d1 + ax2[1]*d2;
            pts[i][2] += ax1[2]*d1 + ax2[2]*d2 + upd;
        }
    }

    // Per-point width modulation. Stored so every cross-section sample uses
    // its local width.
    float widths[TTRAIL_POINTS_PER_ENT];
    for (int i = 0; i < nkeep; ++i) {
        float base = spec->width;
        if (spec->distort_width > 0.0f) {
            float seed = spawns[i] * 7.321f;
            float w = 1.0f + spec->distort_width * sinf(seed + now * 2.4f);
            // Also let the tail swell as it ages (smoke billowing out).
            w *= (1.0f + ages01[i] * 0.7f);
            base *= w;
        }
        widths[i] = base;
    }

    // Per-vertex direction (tangent) used for the perpendicular offset.
    // Boundaries: tangent of the adjacent segment. Interior: bisector of
    // the two adjacent segment directions. Computed AFTER displacement so
    // perpendiculars follow the distorted curve.
    vec3_t tan[TTRAIL_POINTS_PER_ENT];
    for (int i = 0; i < nkeep; ++i) {
        vec3_t din = {0,0,0}, dout = {0,0,0};
        if (i > 0) {
            din[0] = pts[i][0] - pts[i-1][0];
            din[1] = pts[i][1] - pts[i-1][1];
            din[2] = pts[i][2] - pts[i-1][2];
            float l = (float)sqrt(DotProduct(din, din));
            if (l > 1e-4f) { din[0]/=l; din[1]/=l; din[2]/=l; }
        }
        if (i < nkeep - 1) {
            dout[0] = pts[i+1][0] - pts[i][0];
            dout[1] = pts[i+1][1] - pts[i][1];
            dout[2] = pts[i+1][2] - pts[i][2];
            float l = (float)sqrt(DotProduct(dout, dout));
            if (l > 1e-4f) { dout[0]/=l; dout[1]/=l; dout[2]/=l; }
        }
        vec3_t sum = { din[0]+dout[0], din[1]+dout[1], din[2]+dout[2] };
        float sl = (float)sqrt(DotProduct(sum, sum));
        if (sl < 1e-4f) {
            // Pick whichever has length.
            if (i < nkeep - 1) VectorCopy(dout, tan[i]);
            else               VectorCopy(din,  tan[i]);
        } else {
            tan[i][0] = sum[0]/sl; tan[i][1] = sum[1]/sl; tan[i][2] = sum[2]/sl;
        }
    }

    // Perpendicular offset per vertex (unit).
    vec3_t perp[TTRAIL_POINTS_PER_ENT];
    for (int i = 0; i < nkeep; ++i) TTrail_ScreenPerp(pts[i], tan[i], perp[i]);

    // Tesselate each segment [i, i+1] as 4 cross-stripes.
    const float offs[5]   = { -1.0f, -0.333f, 0.0f, 0.333f, 1.0f };
    // Rainbow wants fully opaque stripes so the outer hues (red/magenta)
    // show up instead of fading to transparent. Other modes keep the soft
    // transparent feathered edge.
    const float alphas_soft[5]    = { 0.0f, 0.7f, 1.0f, 0.7f, 0.0f };
    const float alphas_rainbow[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    const float *alphas = spec->rainbow ? alphas_rainbow : alphas_soft;

    int nverts = start_nverts;

    // 7-band ROYGBIV rainbow. Each band drawn as its own quad with both
    // vertices sharing one color -> hard bands, no interpolation.
    static const float rainbow_rgb[7][3] = {
        { 1.00f, 0.00f, 0.00f },   // red
        { 1.00f, 0.50f, 0.00f },   // orange
        { 1.00f, 1.00f, 0.00f },   // yellow
        { 0.00f, 0.78f, 0.00f },   // green
        { 0.25f, 0.65f, 1.00f },   // light blue
        { 0.00f, 0.00f, 0.80f },   // deep blue
        { 0.55f, 0.00f, 0.80f },   // violet
    };

    for (int i = 0; i < nkeep - 1; ++i) {
        int need = spec->rainbow ? 42 : 24;
        if (nverts + need > TTRAIL_MAX_VERTS) break;

        // Age envelope averaged across both endpoints (prevents flicker).
        float age01 = 0.5f * (ages01[i] + ages01[i+1]);
        float env;
        if (age01 < 0.1f) env = age01 / 0.1f;
        else { float u = (age01 - 0.1f) / 0.9f; env = 1.0f - u * u; }
        if (env < 0.0f) env = 0.0f;

        float a_center = spec->alpha * env;

        if (spec->rainbow) {
            // 7 solid-color bands spanning the ribbon width. Band k occupies
            // the interval [-1 + 2k/7, -1 + 2(k+1)/7] in offs space.
            ttrail_vtx_t *v = &ttrail_verts[nverts];
            int vi = 0;
            for (int k = 0; k < 7; ++k) {
                float o_lo = -1.0f + 2.0f * (float)k       / 7.0f;
                float o_hi = -1.0f + 2.0f * (float)(k + 1) / 7.0f;

                float w0_lo = o_lo * widths[i];
                float w0_hi = o_hi * widths[i];
                float w1_lo = o_lo * widths[i+1];
                float w1_hi = o_hi * widths[i+1];

                vec3_t P0_lo = {
                    pts[i][0] + perp[i][0]*w0_lo,
                    pts[i][1] + perp[i][1]*w0_lo,
                    pts[i][2] + perp[i][2]*w0_lo };
                vec3_t P0_hi = {
                    pts[i][0] + perp[i][0]*w0_hi,
                    pts[i][1] + perp[i][1]*w0_hi,
                    pts[i][2] + perp[i][2]*w0_hi };
                vec3_t P1_lo = {
                    pts[i+1][0] + perp[i+1][0]*w1_lo,
                    pts[i+1][1] + perp[i+1][1]*w1_lo,
                    pts[i+1][2] + perp[i+1][2]*w1_lo };
                vec3_t P1_hi = {
                    pts[i+1][0] + perp[i+1][0]*w1_hi,
                    pts[i+1][1] + perp[i+1][1]*w1_hi,
                    pts[i+1][2] + perp[i+1][2]*w1_hi };

                float cr = rainbow_rgb[k][0];
                float cg = rainbow_rgb[k][1];
                float cb = rainbow_rgb[k][2];
                unsigned char rr, gg, bb, aa;
                if (want_additive) {
                    rr = (unsigned char)(cr * a_center * 255.0f);
                    gg = (unsigned char)(cg * a_center * 255.0f);
                    bb = (unsigned char)(cb * a_center * 255.0f);
                    aa = 255;
                } else {
                    rr = (unsigned char)(cr * 255.0f);
                    gg = (unsigned char)(cg * 255.0f);
                    bb = (unsigned char)(cb * 255.0f);
                    aa = (unsigned char)(a_center * 255.0f);
                }
                v[vi].x=P0_lo[0]; v[vi].y=P0_lo[1]; v[vi].z=P0_lo[2]; v[vi].r=rr; v[vi].g=gg; v[vi].b=bb; v[vi].a=aa; ++vi;
                v[vi].x=P0_hi[0]; v[vi].y=P0_hi[1]; v[vi].z=P0_hi[2]; v[vi].r=rr; v[vi].g=gg; v[vi].b=bb; v[vi].a=aa; ++vi;
                v[vi].x=P1_hi[0]; v[vi].y=P1_hi[1]; v[vi].z=P1_hi[2]; v[vi].r=rr; v[vi].g=gg; v[vi].b=bb; v[vi].a=aa; ++vi;
                v[vi].x=P0_lo[0]; v[vi].y=P0_lo[1]; v[vi].z=P0_lo[2]; v[vi].r=rr; v[vi].g=gg; v[vi].b=bb; v[vi].a=aa; ++vi;
                v[vi].x=P1_hi[0]; v[vi].y=P1_hi[1]; v[vi].z=P1_hi[2]; v[vi].r=rr; v[vi].g=gg; v[vi].b=bb; v[vi].a=aa; ++vi;
                v[vi].x=P1_lo[0]; v[vi].y=P1_lo[1]; v[vi].z=P1_lo[2]; v[vi].r=rr; v[vi].g=gg; v[vi].b=bb; v[vi].a=aa; ++vi;
            }
            nverts += 42;
            continue;
        }

        // Default: single color per segment (spike/wiz/missile base).
        float seg_r = spec->r, seg_g = spec->g, seg_b = spec->b;
        if (spec->cools) {
            // Hot -> cold ramp over the first 25% of life.
            float kc = age01 / 0.25f;
            if (kc > 1.0f) kc = 1.0f;
            float hr = 1.00f, hg = 0.55f, hb = 0.15f;
            seg_r = hr + (spec->r - hr) * kc;
            seg_g = hg + (spec->g - hg) * kc;
            seg_b = hb + (spec->b - hb) * kc;
        }

        float sr[5], sg[5], sb[5];
        for (int k = 0; k < 5; ++k) { sr[k]=seg_r; sg[k]=seg_g; sb[k]=seg_b; }

        // Build 5 samples at each endpoint (each endpoint uses its own
        // width so the ribbon breathes).
        vec3_t P0[5], P1[5];
        for (int k = 0; k < 5; ++k) {
            float w0 = offs[k] * widths[i];
            float w1 = offs[k] * widths[i+1];
            P0[k][0] = pts[i  ][0] + perp[i  ][0] * w0;
            P0[k][1] = pts[i  ][1] + perp[i  ][1] * w0;
            P0[k][2] = pts[i  ][2] + perp[i  ][2] * w0;
            P1[k][0] = pts[i+1][0] + perp[i+1][0] * w1;
            P1[k][1] = pts[i+1][1] + perp[i+1][1] * w1;
            P1[k][2] = pts[i+1][2] + perp[i+1][2] * w1;
        }

        ttrail_vtx_t *v = &ttrail_verts[nverts];
        int vi = 0;
        for (int k = 0; k < 4; ++k) {
            float ak_lo = alphas[k];
            float ak_hi = alphas[k + 1];
            float cr_lo = sr[k],   cg_lo = sg[k],   cb_lo = sb[k];
            float cr_hi = sr[k+1], cg_hi = sg[k+1], cb_hi = sb[k+1];
            unsigned char rr_lo, gg_lo, bb_lo, aa_lo;
            unsigned char rr_hi, gg_hi, bb_hi, aa_hi;
            if (want_additive) {
                rr_lo = (unsigned char)(cr_lo * a_center * ak_lo * 255.0f);
                gg_lo = (unsigned char)(cg_lo * a_center * ak_lo * 255.0f);
                bb_lo = (unsigned char)(cb_lo * a_center * ak_lo * 255.0f);
                aa_lo = 255;
                rr_hi = (unsigned char)(cr_hi * a_center * ak_hi * 255.0f);
                gg_hi = (unsigned char)(cg_hi * a_center * ak_hi * 255.0f);
                bb_hi = (unsigned char)(cb_hi * a_center * ak_hi * 255.0f);
                aa_hi = 255;
            } else {
                rr_lo = (unsigned char)(cr_lo * 255.0f);
                gg_lo = (unsigned char)(cg_lo * 255.0f);
                bb_lo = (unsigned char)(cb_lo * 255.0f);
                aa_lo = (unsigned char)(a_center * ak_lo * 255.0f);
                rr_hi = (unsigned char)(cr_hi * 255.0f);
                gg_hi = (unsigned char)(cg_hi * 255.0f);
                bb_hi = (unsigned char)(cb_hi * 255.0f);
                aa_hi = (unsigned char)(a_center * ak_hi * 255.0f);
            }
            v[vi].x=P0[k][0];   v[vi].y=P0[k][1];   v[vi].z=P0[k][2];   v[vi].r=rr_lo; v[vi].g=gg_lo; v[vi].b=bb_lo; v[vi].a=aa_lo; ++vi;
            v[vi].x=P0[k+1][0]; v[vi].y=P0[k+1][1]; v[vi].z=P0[k+1][2]; v[vi].r=rr_hi; v[vi].g=gg_hi; v[vi].b=bb_hi; v[vi].a=aa_hi; ++vi;
            v[vi].x=P1[k+1][0]; v[vi].y=P1[k+1][1]; v[vi].z=P1[k+1][2]; v[vi].r=rr_hi; v[vi].g=gg_hi; v[vi].b=bb_hi; v[vi].a=aa_hi; ++vi;
            v[vi].x=P0[k][0];   v[vi].y=P0[k][1];   v[vi].z=P0[k][2];   v[vi].r=rr_lo; v[vi].g=gg_lo; v[vi].b=bb_lo; v[vi].a=aa_lo; ++vi;
            v[vi].x=P1[k+1][0]; v[vi].y=P1[k+1][1]; v[vi].z=P1[k+1][2]; v[vi].r=rr_hi; v[vi].g=gg_hi; v[vi].b=bb_hi; v[vi].a=aa_hi; ++vi;
            v[vi].x=P1[k][0];   v[vi].y=P1[k][1];   v[vi].z=P1[k][2];   v[vi].r=rr_lo; v[vi].g=gg_lo; v[vi].b=bb_lo; v[vi].a=aa_lo; ++vi;
        }
        nverts += 24;
    }

    return nverts;
}

// ---------------------------------------------------------------------------
// Draw. Two passes (additive + alpha-over).
// ---------------------------------------------------------------------------
void R_TrueTrail_Draw (void)
{
    if (!cl.worldmodel) return;
    if (!R_EnsureTTrailShader()) return;
    R_EnsureTTrailVBO();

    float now = (float)cl.time;
    float mvp[16];
    R_CurrentMVP(mvp);

    // Garbage-collect trails the spec'd lifetime has passed over entirely.
    for (int i = 0; i < TTRAIL_MAX_ENTS; ++i) {
        ttrail_ent_t *tr = &g_trails[i];
        if (!tr->active) continue;
        const trail_spec_t *spec = Trail_ResolveSpec(tr->kind);
        if (now - tr->last_touch > spec->life + 0.1f) {
            tr->active = 0;
            tr->count = 0;
        }
    }

    GLboolean blend_was      = glIsEnabled(GL_BLEND);
    GLboolean cull_was       = glIsEnabled(GL_CULL_FACE);
    GLboolean depth_test_was = glIsEnabled(GL_DEPTH_TEST);
    GLboolean dmask_was; glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask_was);
    GLint src_rgb, dst_rgb, src_a, dst_a;
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);

    if (!blend_was) glEnable(GL_BLEND);
    if (!depth_test_was) glEnable(GL_DEPTH_TEST);
    if (cull_was) glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    GLShader_Use(&R_TTrailShader);
    glUniformMatrix4fv(R_TTrailShader_u_mvp, 1, GL_FALSE, mvp);
    glUniform3f(R_TTrailShader_u_eye, r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2]);
    glUniform1f(R_TTrailShader_u_near, r_trail_near.value);
    glUniform1f(R_TTrailShader_u_far,  r_trail_far.value);

    for (int pass = 0; pass < 2; ++pass) {
        int want_additive = (pass == 0);
        glUniform1i(R_TTrailShader_u_additive, want_additive);

        int nverts = 0;
        for (int i = 0; i < TTRAIL_MAX_ENTS; ++i) {
            const ttrail_ent_t *tr = &g_trails[i];
            if (!tr->active || tr->count < 2) continue;
            nverts = TTrail_TesselateTrail(tr, now, want_additive, nverts);
            if (nverts + 24 > TTRAIL_MAX_VERTS) break;
        }
        if (nverts == 0) continue;

        if (want_additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        else               glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        DynamicVBO_Upload(&ttrail_vbo, ttrail_verts, (GLsizei)(nverts * sizeof(ttrail_vtx_t)));
        DynamicVBO_Bind(&ttrail_vbo);
        glDrawArrays(GL_TRIANGLES, 0, nverts);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBlendFuncSeparate(src_rgb, dst_rgb, src_a, dst_a);
    if (!blend_was) glDisable(GL_BLEND);
    if (cull_was) glEnable(GL_CULL_FACE);
    if (!depth_test_was) glDisable(GL_DEPTH_TEST);
    glDepthMask(dmask_was ? GL_TRUE : GL_FALSE);
}
