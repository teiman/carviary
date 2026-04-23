// gl_magic.cpp -- "dark magic" aura around alias monsters.
//
// For every visible alias entity (except the view model), we draw a single
// view-aligned quad centered on the entity's origin. A fragment shader
// paints the quad with layered fbm noise and emits TWO outputs used by
// dual-source blending:
//
//   outColor  (loc 0, index 0) -- light added to the framebuffer (core glow)
//   outFactor (loc 0, index 1) -- per-channel factor; framebuffer is
//                                 multiplied by (1 - factor) before the add
//
// Blend setup:
//   glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC1_COLOR,
//                       GL_ONE, GL_ONE_MINUS_SRC1_ALPHA);
//   -> final = outColor + framebuffer * (1 - outFactor)
//
// So per-pixel we can BOTH darken (outFactor > 0) AND brighten
// (outColor > 0) in a single pass. Edges of the cloud emit outFactor>0 with
// outColor=0 (pure darken). The core emits outColor>0 with outFactor small
// (brighten).
//
// Shader noise: 3 octaves of 2D value noise scrolled over time, plus a
// radial falloff so the quad has soft edges.

#include "quakedef.h"
#include "gl_render.h"
#include "gl_core.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Tuning.
// ---------------------------------------------------------------------------
#define MAGIC_QUAD_RADIUS_K   32.0f   // quad half-size in world units (billboard)
#define MAGIC_DARK_STRENGTH    0.70f  // peak darkening
#define MAGIC_LIGHT_STRENGTH   0.60f  // peak brightening at core

static GLShader R_MagicShader;
static GLint    R_MagicShader_u_mvp       = -1;
static GLint    R_MagicShader_u_center    = -1;
static GLint    R_MagicShader_u_cam_right = -1;
static GLint    R_MagicShader_u_cam_up    = -1;
static GLint    R_MagicShader_u_radius    = -1;
static GLint    R_MagicShader_u_time      = -1;
static GLint    R_MagicShader_u_seed      = -1;
static GLint    R_MagicShader_u_dark_k    = -1;
static GLint    R_MagicShader_u_light_k   = -1;
static qboolean magic_shader_ok          = false;

// Minimal VAO: one attrib, corner index 0..5.
static GLuint magic_vao = 0;
static GLuint magic_vbo = 0;

static qboolean R_EnsureMagicShader (void)
{
	if (magic_shader_ok) return true;

	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in float a_corner;\n"  // 0..5 (two triangles)
		"uniform mat4  u_mvp;\n"
		"uniform vec3  u_center;\n"
		"uniform vec3  u_cam_right;\n"
		"uniform vec3  u_cam_up;\n"
		"uniform float u_radius;\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    int c = int(a_corner);\n"
		"    // Triangle 1: 0,1,2  Triangle 2: 3,4,5 covering the quad\n"
		"    // as (BL, BR, TR) + (BL, TR, TL). Build UV from the index.\n"
		"    vec2 uv;\n"
		"    if      (c == 0 || c == 3) uv = vec2(0.0, 0.0);\n"
		"    else if (c == 1)           uv = vec2(1.0, 0.0);\n"
		"    else if (c == 2 || c == 4) uv = vec2(1.0, 1.0);\n"
		"    else                       uv = vec2(0.0, 1.0);\n"
		"    v_uv = uv;\n"
		"    vec2 o = (uv - 0.5) * 2.0 * u_radius;\n"  // [-r, +r]
		"    vec3 pos = u_center + u_cam_right * o.x + u_cam_up * o.y;\n"
		"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
		"}\n";

	const char *fs =
		"#version 330 core\n"
		"in vec2 v_uv;\n"
		"uniform float u_time;\n"
		"uniform float u_seed;\n"
		"uniform float u_dark_k;\n"
		"uniform float u_light_k;\n"
		"\n"
		"// Two outputs, same location index, distinct indices. Declared with\n"
		"// layout(index = 0/1) so the linker routes them to SRC0/SRC1 for\n"
		"// dual-source blending.\n"
		"layout(location = 0, index = 0) out vec4 frag_color;\n"   // SRC0
		"layout(location = 0, index = 1) out vec4 frag_factor;\n"  // SRC1
		"// NOTE: no location=1 here. NVIDIA rejects mixing dual-source\n"
		"// (two outputs at location 0 with distinct index) and a second\n"
		"// MRT attachment at location 1. The magic aura therefore does\n"
		"// not feed the bloom mask; its dual-source composite is the\n"
		"// intended effect by itself.\n"
		"\n"
		"float hash21(vec2 p) {\n"
		"    p = fract(p * vec2(123.34, 456.21));\n"
		"    p += dot(p, p + 45.32);\n"
		"    return fract(p.x * p.y);\n"
		"}\n"
		"float vnoise(vec2 p) {\n"
		"    vec2 i = floor(p);\n"
		"    vec2 f = fract(p);\n"
		"    vec2 u = f * f * (3.0 - 2.0 * f);\n"
		"    float a = hash21(i);\n"
		"    float b = hash21(i + vec2(1.0, 0.0));\n"
		"    float c = hash21(i + vec2(0.0, 1.0));\n"
		"    float d = hash21(i + vec2(1.0, 1.0));\n"
		"    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);\n"
		"}\n"
		"float fbm(vec2 p) {\n"
		"    float s = 0.0;\n"
		"    float a = 0.55;\n"
		"    for (int i = 0; i < 3; ++i) {\n"
		"        s += vnoise(p) * a;\n"
		"        p = p * 2.07 + 11.3;\n"
		"        a *= 0.55;\n"
		"    }\n"
		"    return s;\n"
		"}\n"
		"\n"
		"void main() {\n"
		"    vec2 p = v_uv - 0.5;\n"
		"    float r = length(p) * 2.0;\n"            // 0 at center, 1 at quad edge
		"    if (r >= 1.0) discard;\n"
		"\n"
		"    // Radial falloff. Softer than smoothstep so the cloud has body.\n"
		"    float falloff = 1.0 - r;\n"
		"    falloff = falloff * falloff;\n"
		"\n"
		"    // Animated fbm in [0,1). Seed per-entity, plus two time streams\n"
		"    // at different speeds so the turbulence doesn't just translate.\n"
		"    vec2 q = v_uv * 2.8 + vec2(u_seed * 13.1, -u_seed * 7.7);\n"
		"    float n = fbm(q + vec2(u_time * 0.30,  u_time * 0.17));\n"
		"    float m = fbm(q * 1.7 + vec2(-u_time * 0.21, u_time * 0.11));\n"
		"    float turb = n * 0.65 + m * 0.35;\n"     // ~[0,1)
		"\n"
		"    // Core vs. edge split. Core mask peaks toward the center and is\n"
		"    // modulated by the turbulence (so the bright blob writhes).\n"
		"    float core_mask = smoothstep(0.75, 0.15, r);\n"
		"    core_mask *= smoothstep(0.35, 0.70, turb);\n"
		"\n"
		"    // Edge mask: rest of the cloud, darkening. Stronger near the\n"
		"    // bright-noise ridges so the shadow has structure.\n"
		"    float edge_mask = falloff * turb;\n"
		"    edge_mask *= (1.0 - core_mask);\n"       // don't darken the core
		"\n"
		"    // Outputs.\n"
		"    // SRC0 = additive brighten contribution (core glow). Warm\n"
		"    // yellow-orange so the core reads as burning heat.\n"
		"    vec3 core_color = vec3(1.00, 0.55, 0.12);\n"
		"    frag_color = vec4(core_color * core_mask * u_light_k, 0.0);\n"
		"\n"
		"    // SRC1 = per-channel darkening factor. Framebuffer is multiplied\n"
		"    // by (1 - frag_factor). We bias the factor blue-heavy so the\n"
		"    // shadow eats the blue channel most and preserves red/orange,\n"
		"    // leaving a smoky red halo instead of a flat black cloud.\n"
		"    vec3 shadow_tint = vec3(0.70, 0.90, 1.00);\n"
		"    frag_factor = vec4(shadow_tint * edge_mask * u_dark_k, 0.0);\n"
		"}\n";

	char err[512];
	if (!GLShader_Build(&R_MagicShader, vs, fs, err, sizeof(err))) {
		Con_Printf("magic shader failed: %s\n", err);
		return false;
	}
	R_MagicShader_u_mvp       = GLShader_Uniform(&R_MagicShader, "u_mvp");
	R_MagicShader_u_center    = GLShader_Uniform(&R_MagicShader, "u_center");
	R_MagicShader_u_cam_right = GLShader_Uniform(&R_MagicShader, "u_cam_right");
	R_MagicShader_u_cam_up    = GLShader_Uniform(&R_MagicShader, "u_cam_up");
	R_MagicShader_u_radius    = GLShader_Uniform(&R_MagicShader, "u_radius");
	R_MagicShader_u_time      = GLShader_Uniform(&R_MagicShader, "u_time");
	R_MagicShader_u_seed      = GLShader_Uniform(&R_MagicShader, "u_seed");
	R_MagicShader_u_dark_k    = GLShader_Uniform(&R_MagicShader, "u_dark_k");
	R_MagicShader_u_light_k   = GLShader_Uniform(&R_MagicShader, "u_light_k");
	magic_shader_ok = true;
	return true;
}

static void Magic_EnsureVAO (void)
{
	if (magic_vao) return;
	float corners[6] = { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
	glGenVertexArrays(1, &magic_vao);
	glGenBuffers(1, &magic_vbo);
	glBindVertexArray(magic_vao);
	glBindBuffer(GL_ARRAY_BUFFER, magic_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 1, GL_FLOAT, GL_FALSE, sizeof(float), (void*)0);
	glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Public API. Call after drawing each visible alias entity, passing the
// same entity pointer. Skips view model automatically via the caller; we
// don't test `weaponmodel` here because the caller already knows.
// ---------------------------------------------------------------------------
extern vec3_t vright, vup;

void Magic_DrawForEntity (entity_t *e)
{
	// Classification done once per model at load time; we just check the
	// cached flag here. See Mod_DeriveNameFlags in gl_model.cpp.
	if (!e || !e->model) return;
	if (!e->model->magic_aura) return;

	if (!R_EnsureMagicShader()) return;
	Magic_EnsureVAO();

	// Use the entity's model radius for the billboard size. Fallback to a
	// reasonable default if radius looks wrong.
	float radius = e->model->radius;
	if (radius <= 1.0f) radius = MAGIC_QUAD_RADIUS_K;
	else                radius *= 1.4f;

	// Center the quad on the entity's bbox midpoint so the cloud doesn't
	// sit at the feet (origin is usually at the feet for Quake monsters).
	float center[3] = {
		e->origin[0],
		e->origin[1],
		e->origin[2] + (e->model->maxs[2] + e->model->mins[2]) * 0.5f,
	};

	float mvp[16];
	R_CurrentMVP(mvp);

	// Per-entity stable seed so the turbulence is different per monster
	// but doesn't jitter frame-to-frame.
	float seed = (float)(((uintptr_t)e * 2654435761u) & 0xFFFFu) / 65535.0f;

	GLShader_Use(&R_MagicShader);
	glUniformMatrix4fv(R_MagicShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform3f(R_MagicShader_u_center,    center[0], center[1], center[2]);
	glUniform3f(R_MagicShader_u_cam_right, vright[0], vright[1], vright[2]);
	glUniform3f(R_MagicShader_u_cam_up,    vup[0],    vup[1],    vup[2]);
	glUniform1f(R_MagicShader_u_radius,    radius);
	glUniform1f(R_MagicShader_u_time,      (float)realtime);
	glUniform1f(R_MagicShader_u_seed,      seed);
	glUniform1f(R_MagicShader_u_dark_k,    MAGIC_DARK_STRENGTH);
	glUniform1f(R_MagicShader_u_light_k,   MAGIC_LIGHT_STRENGTH);

	// Save state.
	GLboolean blend_was  = glIsEnabled(GL_BLEND);
	GLboolean cull_was   = glIsEnabled(GL_CULL_FACE);
	GLboolean dmask_was;
	glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask_was);
	GLint src_rgb, dst_rgb, src_a, dst_a;
	glGetIntegerv(GL_BLEND_SRC_RGB,   &src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB,   &dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);

	// Dual-source blend: final = outColor + framebuffer * (1 - outFactor).
	// GL_ONE_MINUS_SRC1_COLOR uses the second shader output as the factor
	// multiplied into the destination per-channel.
	if (!blend_was) glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC1_COLOR,
	                    GL_ONE, GL_ONE_MINUS_SRC1_ALPHA);
	if (cull_was) glDisable(GL_CULL_FACE);
	glDepthMask(GL_FALSE);

	glBindVertexArray(magic_vao);
	PostFX_BeginNoMaskWrite();
	glDrawArrays(GL_TRIANGLES, 0, 6);
	PostFX_EndNoMaskWrite();

	// Restore.
	glDepthMask(dmask_was);
	if (cull_was) glEnable(GL_CULL_FACE);
	glBlendFuncSeparate(src_rgb, dst_rgb, src_a, dst_a);
	if (!blend_was) glDisable(GL_BLEND);
	glBindVertexArray(0);
	glUseProgram(0);
}

void Magic_Init (void)
{
	// Nothing to register; shaders are lazy, VAO is lazy.
}
