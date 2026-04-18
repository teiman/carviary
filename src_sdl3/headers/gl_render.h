/*
gl_render.h -- registry of the shared GL 3.3 shader programs used across the
engine as the GL 1.x -> 3.3 migration progresses. Each program is compiled once
(lazily) and reused by multiple files.
*/

#ifndef GL_RENDER_H
#define GL_RENDER_H

#include "gl_core.h"

// ---------------------------------------------------------------------------
// Shared shader: `particle`
//   Attrs: vec3 pos (loc 0), vec4 color (loc 1, unsigned_byte normalized).
//   Uniform: mat4 u_mvp.
//   Used by: gl_part (particles), gl_flares (radial glows).
// ---------------------------------------------------------------------------
extern GLShader R_ParticleShader;
extern GLint    R_ParticleShader_u_mvp;

// ---------------------------------------------------------------------------
// Shared shader: `hud_2d`
//   Attrs: vec2 pos (loc 0), vec2 tc (loc 1), vec4 color (loc 2, ubyte normalized).
//   Uniform: mat4 u_ortho, sampler2D u_tex.
//   Used by: gl_crosshair (and later gl_font, gl_draw).
// ---------------------------------------------------------------------------
extern GLShader R_Hud2dShader;
extern GLint    R_Hud2dShader_u_ortho;
extern GLint    R_Hud2dShader_u_tex;

// ---------------------------------------------------------------------------
// Shared shader: `sprite`
//   Attrs: vec3 pos (loc 0), vec2 tc (loc 1).
//   Uniform: mat4 u_mvp, vec4 u_color, sampler2D u_tex.
//   Used by: gl_sprite (billboards).
// ---------------------------------------------------------------------------
extern GLShader R_SpriteShader;
extern GLint    R_SpriteShader_u_mvp;
extern GLint    R_SpriteShader_u_color;
extern GLint    R_SpriteShader_u_tex;

// ---------------------------------------------------------------------------
// Shared shader: `fullscreen`
//   Attrs: vec2 pos (loc 0).
//   Uniform: mat4 u_mvp, vec4 u_color.
//   Used by: solid-color fills (fade, brighten, Draw_Fill).
// ---------------------------------------------------------------------------
extern GLShader R_FullscreenShader;
extern GLint    R_FullscreenShader_u_mvp;
extern GLint    R_FullscreenShader_u_color;

// ---------------------------------------------------------------------------
// Shared shader: `alias_model`
//   Attrs: vec3 pos (loc 0), vec2 tc (loc 1), vec4 color (loc 2, ubyte normalized).
//   Uniform: mat4 u_mvp, sampler2D u_tex.
//   Used by: gl_mdl (animated model rendering with per-vertex light).
// Lerp + per-vertex lighting are computed on the CPU and baked into color.
// ---------------------------------------------------------------------------
extern GLShader R_AliasShader;
extern GLint    R_AliasShader_u_mvp;
extern GLint    R_AliasShader_u_tex;

// ---------------------------------------------------------------------------
// Shared shader: `world_opaque`
//   Attrs: vec3 pos (loc 0), vec2 tc (loc 1), vec2 lm_tc (loc 2).
//   Uniform: mat4 u_mvp, sampler2D u_tex (unit 0), sampler2D u_lightmap (unit 1).
//   Output: texture * lightmap * 2 (legacy MODULATE with lightmap_format scale).
//   Used by: gl_rsurf (BSP surfaces).
// ---------------------------------------------------------------------------
extern GLShader R_WorldOpaqueShader;
extern GLint    R_WorldOpaqueShader_u_mvp;
extern GLint    R_WorldOpaqueShader_u_tex;
extern GLint    R_WorldOpaqueShader_u_lightmap;
extern GLint    R_WorldOpaqueShader_u_alpha;

// ---------------------------------------------------------------------------
// Shared shader: `world_fence`
//   Same layout as `world_opaque`, plus alpha-discard for {fence textures.
// ---------------------------------------------------------------------------
extern GLShader R_WorldFenceShader;
extern GLint    R_WorldFenceShader_u_mvp;
extern GLint    R_WorldFenceShader_u_tex;
extern GLint    R_WorldFenceShader_u_lightmap;
extern GLint    R_WorldFenceShader_u_alpha;

// ---------------------------------------------------------------------------
// Shared shader: `world_fullbright`
//   Attrs: vec3 pos (loc 0), vec2 tc (loc 1).
//   Uniform: mat4 u_mvp, sampler2D u_tex.
//   Blend additive (GL_ONE, GL_ONE); drawn as a 2nd pass over world_opaque.
// ---------------------------------------------------------------------------
extern GLShader R_WorldFullbrightShader;
extern GLint    R_WorldFullbrightShader_u_mvp;
extern GLint    R_WorldFullbrightShader_u_tex;

// ---------------------------------------------------------------------------
// Shared shader: `world_water`
//   Attrs: vec3 pos (loc 0), vec2 tc (loc 1). Raw BSP texcoords, no CPU warp.
//   Uniform: mat4 u_mvp, sampler2D u_tex, float u_time, float u_alpha.
//   The warp is computed per-fragment in GLSL via sin(), so the result stays
//   smooth regardless of how coarsely the surface is subdivided.
// ---------------------------------------------------------------------------
extern GLShader R_WorldWaterShader;
extern GLint    R_WorldWaterShader_u_mvp;
extern GLint    R_WorldWaterShader_u_tex;
extern GLint    R_WorldWaterShader_u_time;
extern GLint    R_WorldWaterShader_u_alpha;

// Lazy initialization. Idempotent. Returns true if the shader is usable.
qboolean R_EnsureParticleShader(void);
qboolean R_EnsureHud2dShader(void);
qboolean R_EnsureSpriteShader(void);
qboolean R_EnsureFullscreenShader(void);
qboolean R_EnsureAliasShader(void);
qboolean R_EnsureWorldOpaqueShader(void);
qboolean R_EnsureWorldFenceShader(void);
qboolean R_EnsureWorldFullbrightShader(void);
qboolean R_EnsureWorldWaterShader(void);

// Helper: build MVP from the current fixed-function matrices. Valid during
// the transition while parts of the engine still push/pop the legacy stack.
void R_CurrentMVP(float out_mvp[16]);

// ---------------------------------------------------------------------------
// HUD helpers
//
// These use R_CurrentMVP() for their transform, so they expect the caller has
// already established a 2D ortho via GL_Set2D() (or equivalent).
// ---------------------------------------------------------------------------

// Textured quad with per-quad RGBA tint.
void R_HudTexQuad(int x, int y, int w, int h, int texnum,
                  float sl, float tl, float sh, float th,
                  float r, float g, float b, float a);

// Solid-color fill (no texture).
void R_HudFill(int x, int y, int w, int h, float r, float g, float b, float a);

// Batched character draw for the 16x16 font atlas. Between Begin and End,
// call AddChar repeatedly. Begin binds the shader and texture; End flushes.
void R_HudBeginCharBatch(int char_texture);
void R_HudAddChar(int x, int y, int ch, float r, float g, float b, float a);
void R_HudEndCharBatch(void);

#endif // GL_RENDER_H
