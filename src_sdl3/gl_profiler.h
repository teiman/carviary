// gl_profiler.h -- per-section GPU/CPU profiler with deterministic demo sampling.
// See docs/profiling.md for the design.

#ifndef GL_PROFILER_H
#define GL_PROFILER_H

// Section IDs. Order must match g_section_names / g_section_kind in gl_profiler.cpp.
//
// Sections come in three kinds:
//   - LEAF (GPU + CPU): a single Begin/End per frame around a block that
//     issues GL draws. Gets timestamps, primitives, fragments, draw-call count.
//   - CONTAINER (GPU + CPU): wraps other LEAF sections. Uses GL timestamps
//     (they nest) but skips PRIMITIVES_GENERATED / SAMPLES_PASSED (those
//     don't nest). Example: scene, frame_total.
//   - CPU_ONLY: no GL queries at all, just Sys_FloatTime. Can be called
//     multiple times per frame -- each interval is accumulated into the
//     sample. This is how we measure pieces of the scene that happen
//     multiple times intercalated (water, particles) or nested deep inside
//     an already-GL-timed section.
typedef enum {
    PROF_CLEAR,
    PROF_WORLD_OPAQUE,       // R_DrawWorld (includes sky rendered inside it)
    PROF_VIEWMODEL,
    PROF_POLYBLEND,
    PROF_HUD_2D,
    PROF_SWAP,
    PROF_SCENE,              // container: all of R_RenderScene
    PROF_FRAME_TOTAL,        // container: all of SCR_UpdateScreen

    // CPU-only sub-sections inside scene. These accumulate: multiple
    // Begin/End pairs in one frame add up.
    PROF_CPU_MARK_LEAVES,
    PROF_CPU_TRANS_SETUP,
    PROF_CPU_WATER,
    PROF_CPU_PARTICLES,
    PROF_CPU_ALIAS,          // sum of R_DrawAliasModel per entity
    PROF_CPU_SPRITE,         // sum of R_DrawSpriteModel per entity
    PROF_CPU_BRUSH_TRANS,    // sum of R_DrawBrushMTexTrans per surface
    PROF_CPU_WATER_UPLOAD,   // DynamicVBO_Upload of the water soup (suspected driver stall)

    PROF_NUM_SECTIONS
} prof_section_t;

// Lifecycle.
void Prof_Init        (void);              // call once at renderer init; registers cvars
void Prof_Start       (const char *demo);  // arm profiler for the next demo playback (deterministic net-frame sampling)
void Prof_StartLive   (void);              // start capturing in live gameplay: sample every Nth render frame until stopped or 100 samples reached
void Prof_StopLive    (void);              // stop live capture and write JSON
void Prof_BeginFrame  (void);              // called from SCR_UpdateScreen at the top
void Prof_EndFrame    (void);              // called after swap
void Prof_Finish      (void);              // flushes JSON to disk; safe no-op if not running

// Section hooks (no-op when not sampling this frame).
void Prof_BeginSection (prof_section_t s);
void Prof_EndSection   (prof_section_t s);

// Draw-call counter. Call with the vertex count right before each glDrawArrays.
// No-op when the current frame is not being sampled.
void Prof_CountDraw    (int vertices);

// Query if the current frame is being sampled.
int  Prof_IsSamplingFrame (void);

// True while the profiler is armed or capturing. CL_Profile_f uses this.
int  Prof_IsRunning (void);

#endif // GL_PROFILER_H
