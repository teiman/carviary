/*
gl_render.cpp -- shared GL 3.3 shader programs.
Each R_Ensure* is idempotent; the first caller builds, the rest reuse.
*/

#include "quakedef.h"
#include "gl_render.h"
#include "gl_mat4.h"
#include "gl_profiler.h"

GLShader R_ParticleShader;
GLint    R_ParticleShader_u_mvp = -1;

GLShader R_Hud2dShader;
GLint    R_Hud2dShader_u_ortho = -1;
GLint    R_Hud2dShader_u_tex   = -1;

GLShader R_SpriteShader;
GLint    R_SpriteShader_u_mvp   = -1;
GLint    R_SpriteShader_u_color = -1;
GLint    R_SpriteShader_u_tex   = -1;

GLShader R_FullscreenShader;
GLint    R_FullscreenShader_u_mvp   = -1;
GLint    R_FullscreenShader_u_color = -1;

GLShader R_AliasShader;
GLint    R_AliasShader_u_mvp              = -1;
GLint    R_AliasShader_u_tex              = -1;
GLint    R_AliasShader_u_pose_a           = -1;
GLint    R_AliasShader_u_pose_b           = -1;
GLint    R_AliasShader_u_blend            = -1;
GLint    R_AliasShader_u_shade_color      = -1;
GLint    R_AliasShader_u_lightlerpoffset  = -1;
GLint    R_AliasShader_u_alpha            = -1;
GLint    R_AliasShader_u_fullbright       = -1;
GLint    R_AliasShader_u_dot_row_ceil     = -1;
GLint    R_AliasShader_u_dot_row_floor    = -1;

GLShader R_WorldOpaqueShader;
GLint    R_WorldOpaqueShader_u_mvp        = -1;
GLint    R_WorldOpaqueShader_u_tex        = -1;
GLint    R_WorldOpaqueShader_u_lightmap   = -1;
GLint    R_WorldOpaqueShader_u_alpha      = -1;
GLint    R_WorldOpaqueShader_u_dream_amp  = -1;
GLint    R_WorldOpaqueShader_u_dream_time = -1;

GLShader R_WorldFenceShader;
GLint    R_WorldFenceShader_u_mvp        = -1;
GLint    R_WorldFenceShader_u_tex        = -1;
GLint    R_WorldFenceShader_u_lightmap   = -1;
GLint    R_WorldFenceShader_u_alpha      = -1;
GLint    R_WorldFenceShader_u_dream_amp  = -1;
GLint    R_WorldFenceShader_u_dream_time = -1;

GLShader R_WorldFullbrightShader;
GLint    R_WorldFullbrightShader_u_mvp = -1;
GLint    R_WorldFullbrightShader_u_tex = -1;

GLShader R_WorldWaterShader;
GLint    R_WorldWaterShader_u_mvp   = -1;
GLint    R_WorldWaterShader_u_tex   = -1;
GLint    R_WorldWaterShader_u_time  = -1;
GLint    R_WorldWaterShader_u_alpha = -1;

static qboolean particle_ok         = false;
static qboolean hud2d_ok            = false;
static qboolean sprite_ok           = false;
static qboolean fullscreen_ok       = false;
static qboolean alias_ok            = false;
static qboolean world_opaque_ok     = false;
static qboolean world_fence_ok      = false;
static qboolean world_fullbright_ok = false;
static qboolean world_water_ok      = false;

// ---------------------------------------------------------------------------
qboolean R_EnsureParticleShader(void)
{
	if (particle_ok) return true;

	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in vec3 a_pos;\n"
		"layout(location = 1) in vec4 a_color;\n"
		"uniform mat4 u_mvp;\n"
		"out vec4 v_color;\n"
		"void main() {\n"
		"    v_color = a_color;\n"
		"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
		"}\n";

	const char *fs =
		"#version 330 core\n"
		"in vec4 v_color;\n"
		"out vec4 frag_color;\n"
		"void main() { frag_color = v_color; }\n";

	char err[512];
	if (!GLShader_Build(&R_ParticleShader, vs, fs, err, sizeof(err))) {
		Con_Printf("particle shader failed: %s\n", err);
		return false;
	}
	R_ParticleShader_u_mvp = GLShader_Uniform(&R_ParticleShader, "u_mvp");
	particle_ok = true;
	return true;
}

// ---------------------------------------------------------------------------
qboolean R_EnsureHud2dShader(void)
{
	if (hud2d_ok) return true;

	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in vec2 a_pos;\n"
		"layout(location = 1) in vec2 a_tc;\n"
		"layout(location = 2) in vec4 a_color;\n"
		"uniform mat4 u_ortho;\n"
		"out vec2 v_tc;\n"
		"out vec4 v_color;\n"
		"void main() {\n"
		"    v_tc = a_tc;\n"
		"    v_color = a_color;\n"
		"    gl_Position = u_ortho * vec4(a_pos, 0.0, 1.0);\n"
		"}\n";

	const char *fs =
		"#version 330 core\n"
		"in vec2 v_tc;\n"
		"in vec4 v_color;\n"
		"uniform sampler2D u_tex;\n"
		"out vec4 frag_color;\n"
		"void main() { frag_color = texture(u_tex, v_tc) * v_color; }\n";

	char err[512];
	if (!GLShader_Build(&R_Hud2dShader, vs, fs, err, sizeof(err))) {
		Con_Printf("hud_2d shader failed: %s\n", err);
		return false;
	}
	R_Hud2dShader_u_ortho = GLShader_Uniform(&R_Hud2dShader, "u_ortho");
	R_Hud2dShader_u_tex   = GLShader_Uniform(&R_Hud2dShader, "u_tex");
	hud2d_ok = true;
	return true;
}

// ---------------------------------------------------------------------------
qboolean R_EnsureSpriteShader(void)
{
	if (sprite_ok) return true;

	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in vec3 a_pos;\n"
		"layout(location = 1) in vec2 a_tc;\n"
		"uniform mat4 u_mvp;\n"
		"out vec2 v_tc;\n"
		"void main() {\n"
		"    v_tc = a_tc;\n"
		"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
		"}\n";

	const char *fs =
		"#version 330 core\n"
		"in vec2 v_tc;\n"
		"uniform sampler2D u_tex;\n"
		"uniform vec4 u_color;\n"
		"out vec4 frag_color;\n"
		"void main() { frag_color = texture(u_tex, v_tc) * u_color; }\n";

	char err[512];
	if (!GLShader_Build(&R_SpriteShader, vs, fs, err, sizeof(err))) {
		Con_Printf("sprite shader failed: %s\n", err);
		return false;
	}
	R_SpriteShader_u_mvp   = GLShader_Uniform(&R_SpriteShader, "u_mvp");
	R_SpriteShader_u_color = GLShader_Uniform(&R_SpriteShader, "u_color");
	R_SpriteShader_u_tex   = GLShader_Uniform(&R_SpriteShader, "u_tex");
	sprite_ok = true;
	return true;
}

// ---------------------------------------------------------------------------
qboolean R_EnsureFullscreenShader(void)
{
	if (fullscreen_ok) return true;

	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in vec2 a_pos;\n"
		"uniform mat4 u_mvp;\n"
		"void main() { gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0); }\n";

	const char *fs =
		"#version 330 core\n"
		"uniform vec4 u_color;\n"
		"out vec4 frag_color;\n"
		"void main() { frag_color = u_color; }\n";

	char err[512];
	if (!GLShader_Build(&R_FullscreenShader, vs, fs, err, sizeof(err))) {
		Con_Printf("fullscreen shader failed: %s\n", err);
		return false;
	}
	R_FullscreenShader_u_mvp   = GLShader_Uniform(&R_FullscreenShader, "u_mvp");
	R_FullscreenShader_u_color = GLShader_Uniform(&R_FullscreenShader, "u_color");
	fullscreen_ok = true;
	return true;
}

// ---------------------------------------------------------------------------
// Alias shader. Reads two poses from an SSBO bound at binding 0, interpolates
// position and normal, does per-vertex Quake-style shading with a 16-row
// brightness table (UBO binding 1), and emits a vec4 color for the fragment
// shader. Only attribute is the texture coordinate; positions come from the
// SSBO indexed by gl_VertexID.
// ---------------------------------------------------------------------------
qboolean R_EnsureAliasShader(void)
{
	if (alias_ok) return true;

	// GLSL 4.30 for std430 SSBOs; we're on a 4.4 context.
	// Shading mirrors the CPU path exactly: two precomputed per-normal
	// brightness arrays (one for the ceil() light-angle bucket, one for
	// floor()), blended with u_lightlerpoffset. Then multiply by the
	// per-entity light color and clamp per channel. The u_normals array
	// isn't needed here because positions are dequantized directly and
	// shading uses only the normal index.
	const char *vs =
		"#version 430 core\n"
		"layout(location = 0) in vec2 a_tc;\n"
		"\n"
		"layout(std430, binding = 0) readonly buffer PoseBuf { uint poses[]; };\n"
		"\n"
		"uniform mat4  u_mvp;\n"
		"uniform int   u_pose_a;\n"
		"uniform int   u_pose_b;\n"
		"uniform float u_blend;\n"
		"uniform vec3  u_shade_color;\n"
		"uniform float u_lightlerpoffset;\n"
		"uniform float u_alpha;\n"
		"uniform int   u_fullbright;\n"
		"uniform int   u_dot_row_ceil;\n"
		"uniform int   u_dot_row_floor;\n"
		"// 16 yaw-angle brightness rows of 256 floats each, packed as vec4s\n"
		"// so the whole table fits in one 16 KB std140 uniform block. We pack\n"
		"// 4 scalars per vec4 and index by (i/4).xyzw[i%4] in the shader.\n"
		"// Uploaded once at shader build time; per-draw we only send two\n"
		"// row indices.\n"
		"layout(std140) uniform AnormDotsBlock { vec4 u_anorm_dots[16 * 64]; };\n"
		"\n"
		"float dot_row (int row, int n) {\n"
		"    int idx = row * 256 + n;\n"
		"    return u_anorm_dots[idx >> 2][idx & 3];\n"
		"}\n"
		"\n"
		"out vec2 v_tc;\n"
		"out vec4 v_color;\n"
		"\n"
		"void unpack(in uint w, out vec3 pos, out int nidx) {\n"
		"    pos = vec3(float(w & 0xFFu),\n"
		"               float((w >> 8) & 0xFFu),\n"
		"               float((w >> 16) & 0xFFu));\n"
		"    nidx = int((w >> 24) & 0xFFu);\n"
		"}\n"
		"\n"
		"void main() {\n"
		"    int idx = gl_VertexID;\n"
		"    uint wa = poses[u_pose_a + idx];\n"
		"    uint wb = poses[u_pose_b + idx];\n"
		"    vec3 pa; int na;\n"
		"    vec3 pb; int nb;\n"
		"    unpack(wa, pa, na);\n"
		"    unpack(wb, pb, nb);\n"
		"\n"
		"    // Positions stay in model's quantized byte space; scale + origin\n"
		"    // are already baked into the modelview stack by R_DrawAliasModel\n"
		"    // (MulTranslate + MulScale before drawing). This matches the CPU\n"
		"    // path, which also passes the raw byte values straight to the\n"
		"    // shader via a_pos.\n"
		"    vec3 pos = mix(pa, pb, u_blend);\n"
		"\n"
		"    vec3 lit;\n"
		"    if (u_fullbright != 0) {\n"
		"        lit = vec3(1.0);\n"
		"    } else {\n"
		"        // Interpolate brightness between the two poses using u_blend,\n"
		"        // and between the two light-angle rows using u_lightlerpoffset.\n"
		"        // Matches the CPU fallback in gl_mdl.cpp.\n"
		"        float ca = dot_row(u_dot_row_ceil,  na);\n"
		"        float cb = dot_row(u_dot_row_ceil,  nb);\n"
		"        float fa = dot_row(u_dot_row_floor, na);\n"
		"        float fb = dot_row(u_dot_row_floor, nb);\n"
		"        float lc = mix(ca, cb, u_blend);\n"
		"        float lf = mix(fa, fb, u_blend);\n"
		"        float l;\n"
		"        if (lc > lf)      l = lc - (lc - lf) * u_lightlerpoffset;\n"
		"        else if (lf > lc) l = lc + (lf - lc) * u_lightlerpoffset;\n"
		"        else              l = lc;\n"
		"        lit = clamp(u_shade_color * l, 0.0, 1.0);\n"
		"    }\n"
		"\n"
		"    v_tc = a_tc;\n"
		"    v_color = vec4(lit, u_alpha);\n"
		"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
		"}\n";

	const char *fs =
		"#version 430 core\n"
		"in vec2 v_tc;\n"
		"in vec4 v_color;\n"
		"uniform sampler2D u_tex;\n"
		"out vec4 frag_color;\n"
		"void main() { frag_color = texture(u_tex, v_tc) * v_color; }\n";

	char err[1024];
	if (!GLShader_Build(&R_AliasShader, vs, fs, err, sizeof(err))) {
		Con_Printf("alias shader failed: %s\n", err);
		return false;
	}

	R_AliasShader_u_mvp             = GLShader_Uniform(&R_AliasShader, "u_mvp");
	R_AliasShader_u_tex             = GLShader_Uniform(&R_AliasShader, "u_tex");
	R_AliasShader_u_pose_a          = GLShader_Uniform(&R_AliasShader, "u_pose_a");
	R_AliasShader_u_pose_b          = GLShader_Uniform(&R_AliasShader, "u_pose_b");
	R_AliasShader_u_blend           = GLShader_Uniform(&R_AliasShader, "u_blend");
	R_AliasShader_u_shade_color     = GLShader_Uniform(&R_AliasShader, "u_shade_color");
	R_AliasShader_u_lightlerpoffset = GLShader_Uniform(&R_AliasShader, "u_lightlerpoffset");
	R_AliasShader_u_alpha           = GLShader_Uniform(&R_AliasShader, "u_alpha");
	R_AliasShader_u_fullbright      = GLShader_Uniform(&R_AliasShader, "u_fullbright");
	R_AliasShader_u_dot_row_ceil    = GLShader_Uniform(&R_AliasShader, "u_dot_row_ceil");
	R_AliasShader_u_dot_row_floor   = GLShader_Uniform(&R_AliasShader, "u_dot_row_floor");

	// Upload the full 16x256 anorm-dots table once, as a std140 UBO. Using
	// a plain uniform float[4096] blew past NVIDIA's per-stage uniform
	// location limits. UBOs have a minimum guaranteed size of 16 KB in GL
	// 4.4 core, which fits 4096 floats packed as 1024 vec4s exactly.
	{
		extern float r_avertexnormal_dots_mdl[16][256];
		static GLuint anorm_dots_ubo = 0;
		glGenBuffers(1, &anorm_dots_ubo);
		glBindBuffer(GL_UNIFORM_BUFFER, anorm_dots_ubo);

		// Repack 4096 floats into 1024 vec4s (16 KB std140-compatible).
		float packed[1024 * 4];
		memcpy(packed, &r_avertexnormal_dots_mdl[0][0], sizeof(packed));
		glBufferData(GL_UNIFORM_BUFFER, sizeof(packed), packed, GL_STATIC_DRAW);
		glBindBuffer(GL_UNIFORM_BUFFER, 0);

		GLuint block_idx = glGetUniformBlockIndex(R_AliasShader.program, "AnormDotsBlock");
		if (block_idx != GL_INVALID_INDEX) {
			glUniformBlockBinding(R_AliasShader.program, block_idx, 1);
			glBindBufferBase(GL_UNIFORM_BUFFER, 1, anorm_dots_ubo);
		}
	}

	alias_ok = true;
	return true;
}

// ---------------------------------------------------------------------------
// World shaders share the same vertex layout, so build from a common source.
//
// Dream warp: when u_dream_amp > 0, every vertex is displaced by a 3D value
// noise sampled at its world position (optionally time-animated via
// u_dream_time). The offset is a pure function of (a_pos, u_dream_time), so
// vertices shared between adjacent surfaces get identical displacements and
// the seams stay closed -- Quake BSP vertex positions are integer-valued in
// world units, so there is no float divergence to worry about.
static const char *world_vs_src =
	"#version 330 core\n"
	"layout(location = 0) in vec3 a_pos;\n"
	"layout(location = 1) in vec2 a_tc;\n"
	"layout(location = 2) in vec2 a_lmtc;\n"
	"uniform mat4  u_mvp;\n"
	"uniform float u_dream_amp;\n"    // 0 = no warp; units of world displacement
	"uniform float u_dream_time;\n"   // seconds; adds a slow breathing motion
	"out vec2 v_tc;\n"
	"out vec2 v_lmtc;\n"
	"\n"
	"float dream_hash31(vec3 p) {\n"
	"    p = fract(p * vec3(0.1031, 0.1030, 0.0973));\n"
	"    p += dot(p, p.yxz + 33.33);\n"
	"    return fract((p.x + p.y) * p.z);\n"
	"}\n"
	"float dream_vnoise3(vec3 p) {\n"
	"    vec3 i = floor(p);\n"
	"    vec3 f = fract(p);\n"
	"    f = f * f * (3.0 - 2.0 * f);\n"
	"    float a = dream_hash31(i + vec3(0,0,0));\n"
	"    float b = dream_hash31(i + vec3(1,0,0));\n"
	"    float c = dream_hash31(i + vec3(0,1,0));\n"
	"    float d = dream_hash31(i + vec3(1,1,0));\n"
	"    float e = dream_hash31(i + vec3(0,0,1));\n"
	"    float g = dream_hash31(i + vec3(1,0,1));\n"
	"    float h = dream_hash31(i + vec3(0,1,1));\n"
	"    float k = dream_hash31(i + vec3(1,1,1));\n"
	"    return mix(mix(mix(a,b,f.x), mix(c,d,f.x), f.y),\n"
	"               mix(mix(e,g,f.x), mix(h,k,f.x), f.y), f.z);\n"
	"}\n"
	"\n"
	"vec3 dream_offset(vec3 wp, float t) {\n"
	"    // Spatial frequency ~0.015/unit -> one wavelength per ~66 units,\n"
	"    // comfortable at Quake interior scale (corridor widths 128-256).\n"
	"    vec3 q = wp * 0.015 + vec3(t * 0.07, -t * 0.05, t * 0.04);\n"
	"    // Three independent samples (offset in lattice) -> a smooth 3D\n"
	"    // vector field. -0.5 so the noise centers around 0 instead of 0.5.\n"
	"    float nx = dream_vnoise3(q + vec3(  0,   0,   0)) - 0.5;\n"
	"    float ny = dream_vnoise3(q + vec3(31,  17,   5)) - 0.5;\n"
	"    float nz = dream_vnoise3(q + vec3(73,  41, 101)) - 0.5;\n"
	"    return vec3(nx, ny, nz) * 2.0;\n"    // rescale to ~[-1, 1]
	"}\n"
	"\n"
	"void main() {\n"
	"    vec3 pos = a_pos;\n"
	"    if (u_dream_amp > 0.0) {\n"
	"        pos += dream_offset(a_pos, u_dream_time) * u_dream_amp;\n"
	"    }\n"
	"    v_tc = a_tc;\n"
	"    v_lmtc = a_lmtc;\n"
	"    gl_Position = u_mvp * vec4(pos, 1.0);\n"
	"}\n";

qboolean R_EnsureWorldOpaqueShader(void)
{
	if (world_opaque_ok) return true;

	const char *fs =
		"#version 330 core\n"
		"in vec2 v_tc;\n"
		"in vec2 v_lmtc;\n"
		"uniform sampler2D u_tex;\n"
		"uniform sampler2D u_lightmap;\n"
		"uniform float u_alpha;\n"
		"out vec4 frag_color;\n"
		"void main() {\n"
		"    vec4 diffuse = texture(u_tex, v_tc);\n"
		"    vec4 lm = texture(u_lightmap, v_lmtc);\n"
		"    frag_color = vec4(diffuse.rgb * lm.rgb, diffuse.a * u_alpha);\n"
		"}\n";

	char err[512];
	if (!GLShader_Build(&R_WorldOpaqueShader, world_vs_src, fs, err, sizeof(err))) {
		Con_Printf("world_opaque shader failed: %s\n", err);
		return false;
	}
	R_WorldOpaqueShader_u_mvp        = GLShader_Uniform(&R_WorldOpaqueShader, "u_mvp");
	R_WorldOpaqueShader_u_tex        = GLShader_Uniform(&R_WorldOpaqueShader, "u_tex");
	R_WorldOpaqueShader_u_lightmap   = GLShader_Uniform(&R_WorldOpaqueShader, "u_lightmap");
	R_WorldOpaqueShader_u_alpha      = GLShader_Uniform(&R_WorldOpaqueShader, "u_alpha");
	R_WorldOpaqueShader_u_dream_amp  = GLShader_Uniform(&R_WorldOpaqueShader, "u_dream_amp");
	R_WorldOpaqueShader_u_dream_time = GLShader_Uniform(&R_WorldOpaqueShader, "u_dream_time");
	world_opaque_ok = true;
	return true;
}

qboolean R_EnsureWorldFenceShader(void)
{
	if (world_fence_ok) return true;

	const char *fs =
		"#version 330 core\n"
		"in vec2 v_tc;\n"
		"in vec2 v_lmtc;\n"
		"uniform sampler2D u_tex;\n"
		"uniform sampler2D u_lightmap;\n"
		"uniform float u_alpha;\n"
		"out vec4 frag_color;\n"
		"void main() {\n"
		"    vec4 diffuse = texture(u_tex, v_tc);\n"
		"    if (diffuse.a < 0.666) discard;\n"
		"    vec4 lm = texture(u_lightmap, v_lmtc);\n"
		"    frag_color = vec4(diffuse.rgb * lm.rgb, diffuse.a * u_alpha);\n"
		"}\n";

	char err[512];
	if (!GLShader_Build(&R_WorldFenceShader, world_vs_src, fs, err, sizeof(err))) {
		Con_Printf("world_fence shader failed: %s\n", err);
		return false;
	}
	R_WorldFenceShader_u_mvp        = GLShader_Uniform(&R_WorldFenceShader, "u_mvp");
	R_WorldFenceShader_u_tex        = GLShader_Uniform(&R_WorldFenceShader, "u_tex");
	R_WorldFenceShader_u_lightmap   = GLShader_Uniform(&R_WorldFenceShader, "u_lightmap");
	R_WorldFenceShader_u_alpha      = GLShader_Uniform(&R_WorldFenceShader, "u_alpha");
	R_WorldFenceShader_u_dream_amp  = GLShader_Uniform(&R_WorldFenceShader, "u_dream_amp");
	R_WorldFenceShader_u_dream_time = GLShader_Uniform(&R_WorldFenceShader, "u_dream_time");
	world_fence_ok = true;
	return true;
}

qboolean R_EnsureWorldWaterShader(void)
{
	if (world_water_ok) return true;

	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in vec3 a_pos;\n"
		"layout(location = 1) in vec2 a_tc;\n"
		"uniform mat4 u_mvp;\n"
		"out vec2 v_tc;\n"
		"void main() {\n"
		"    v_tc = a_tc;\n"
		"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
		"}\n";

	// Per-fragment warp. Reproduces the legacy turbsin math continuously so
	// the motion stays smooth regardless of the surface tesselation. The
	// legacy path wrote `(os + turbsin) * 0.015` as the final TC, which
	// means the raw BSP coords `os,ot` are scaled by 0.015 too. Keeping
	// that same scaling so we don't have to rebuild the BSP with different
	// coords.
	// Classic Quake water warp. The legacy LUT was indexed by
	// `(int)((x * 0.125 + t) * 256/(2pi)) & 255`, i.e. sampling sin() at
	// phase = x*0.125 + t (no extra 2pi). LUT amplitude was 8, so UV
	// displacement after the *0.015 scale was 8*0.015 = 0.12 in texture
	// space. We reproduce that exactly, pre-scaled into 0-1 texture UVs
	// by dividing the raw BSP coords by 64.
	// Exact reproduction of the legacy fixed-function turbsin warp:
	//   r = (os + turbsin(ot*0.125 + realtime)) * 0.015
	// where turbsin(x) = 8 * sin(x) (amplitude 8), evaluated in world-space
	// before the 0.015 scale to UV. Spatial period ~50 world units.
	// `os,ot` are raw BSP texcoords from DotProduct(v, texinfo->vecs).
	// Water warp. Texture sampled at the legacy 1/64 scale (mosaic unchanged),
	// but the sine's spatial frequency is 1/4 of the legacy 0.125: a longer
	// warp cycle covers more area, which reads as less tiling. Amplitude 8
	// and the CPU-side turbsin math are preserved.
	const char *fs =
		"#version 330 core\n"
		"in vec2 v_tc;\n"
		"uniform sampler2D u_tex;\n"
		"uniform float u_time;\n"
		"uniform float u_alpha;\n"
		"out vec4 frag_color;\n"
		"void main() {\n"
		"    float os = v_tc.x;\n"
		"    float ot = v_tc.y;\n"
		"    float r_s = (os + 8.0 * sin(ot * 0.03125 + u_time)) * 0.015625;\n"
		"    float r_t = (ot + 8.0 * sin(os * 0.03125 + u_time)) * 0.015625;\n"
		"    vec4 c = texture(u_tex, vec2(r_s, r_t));\n"
		"    frag_color = vec4(c.rgb, c.a * u_alpha);\n"
		"}\n";

	char err[512];
	if (!GLShader_Build(&R_WorldWaterShader, vs, fs, err, sizeof(err))) {
		Con_Printf("world_water shader failed: %s\n", err);
		return false;
	}
	R_WorldWaterShader_u_mvp   = GLShader_Uniform(&R_WorldWaterShader, "u_mvp");
	R_WorldWaterShader_u_tex   = GLShader_Uniform(&R_WorldWaterShader, "u_tex");
	R_WorldWaterShader_u_time  = GLShader_Uniform(&R_WorldWaterShader, "u_time");
	R_WorldWaterShader_u_alpha = GLShader_Uniform(&R_WorldWaterShader, "u_alpha");
	world_water_ok = true;
	return true;
}

qboolean R_EnsureWorldFullbrightShader(void)
{
	if (world_fullbright_ok) return true;

	const char *vs =
		"#version 330 core\n"
		"layout(location = 0) in vec3 a_pos;\n"
		"layout(location = 1) in vec2 a_tc;\n"
		"uniform mat4 u_mvp;\n"
		"out vec2 v_tc;\n"
		"void main() {\n"
		"    v_tc = a_tc;\n"
		"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
		"}\n";

	const char *fs =
		"#version 330 core\n"
		"in vec2 v_tc;\n"
		"uniform sampler2D u_tex;\n"
		"out vec4 frag_color;\n"
		"void main() {\n"
		"    vec4 t = texture(u_tex, v_tc);\n"
		"    // Fullbright masks use alpha=0 for non-glowing pixels. With\n"
		"    // additive blending (ONE,ONE) we must premultiply by alpha or\n"
		"    // the transparent RGB leaks into the result as extra brightness.\n"
		"    frag_color = vec4(t.rgb * t.a, t.a);\n"
		"}\n";

	char err[512];
	if (!GLShader_Build(&R_WorldFullbrightShader, vs, fs, err, sizeof(err))) {
		Con_Printf("world_fullbright shader failed: %s\n", err);
		return false;
	}
	R_WorldFullbrightShader_u_mvp = GLShader_Uniform(&R_WorldFullbrightShader, "u_mvp");
	R_WorldFullbrightShader_u_tex = GLShader_Uniform(&R_WorldFullbrightShader, "u_tex");
	world_fullbright_ok = true;
	return true;
}

// ---------------------------------------------------------------------------
// Reads the current MVP from the CPU matrix stacks (r_projection, r_modelview).
void R_CurrentMVP(float out[16])
{
	R_MVP(out);
}

// ---------------------------------------------------------------------------
// HUD helpers
// ---------------------------------------------------------------------------

typedef struct {
	float x, y;
	float s, t;
	unsigned char r, g, b, a;
} hud_vtx_t;

static DynamicVBO hud_quad_vbo;
static qboolean   hud_quad_ready = false;

static void EnsureHudQuadVBO(void)
{
	if (hud_quad_ready) return;
	DynamicVBO_Init(&hud_quad_vbo, 6 * sizeof(hud_vtx_t));
	DynamicVBO_SetAttrib(&hud_quad_vbo, 0, 2, GL_FLOAT,         GL_FALSE, sizeof(hud_vtx_t), offsetof(hud_vtx_t, x));
	DynamicVBO_SetAttrib(&hud_quad_vbo, 1, 2, GL_FLOAT,         GL_FALSE, sizeof(hud_vtx_t), offsetof(hud_vtx_t, s));
	DynamicVBO_SetAttrib(&hud_quad_vbo, 2, 4, GL_UNSIGNED_BYTE, GL_TRUE,  sizeof(hud_vtx_t), offsetof(hud_vtx_t, r));
	hud_quad_ready = true;
}

void R_HudTexQuad(int x, int y, int w, int h, int texnum,
                  float sl, float tl, float sh, float th,
                  float r, float g, float b, float a)
{
	if (!R_EnsureHud2dShader()) return;
	EnsureHudQuadVBO();

	unsigned char cr = (unsigned char)(r * 255.0f + 0.5f);
	unsigned char cg = (unsigned char)(g * 255.0f + 0.5f);
	unsigned char cb = (unsigned char)(b * 255.0f + 0.5f);
	unsigned char ca = (unsigned char)(a * 255.0f + 0.5f);

	float x0 = (float)x,     y0 = (float)y;
	float x1 = (float)(x+w), y1 = (float)(y+h);

	hud_vtx_t verts[6] = {
		{x0, y0, sl, tl, cr, cg, cb, ca},
		{x1, y0, sh, tl, cr, cg, cb, ca},
		{x1, y1, sh, th, cr, cg, cb, ca},
		{x0, y0, sl, tl, cr, cg, cb, ca},
		{x1, y1, sh, th, cr, cg, cb, ca},
		{x0, y1, sl, th, cr, cg, cb, ca},
	};

	float mvp[16];
	R_CurrentMVP(mvp);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texnum);
	GLShader_Use(&R_Hud2dShader);
	glUniformMatrix4fv(R_Hud2dShader_u_ortho, 1, GL_FALSE, mvp);
	glUniform1i(R_Hud2dShader_u_tex, 0);

	DynamicVBO_Upload(&hud_quad_vbo, verts, sizeof(verts));
	DynamicVBO_Bind(&hud_quad_vbo);
	Prof_CountDraw(6);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	glBindVertexArray(0);
	glUseProgram(0);
}

// ---------------------------------------------------------------------------

typedef struct { float x, y; } hud_pos_t;
static DynamicVBO hud_fill_vbo;
static qboolean   hud_fill_ready = false;

static void EnsureHudFillVBO(void)
{
	if (hud_fill_ready) return;
	DynamicVBO_Init(&hud_fill_vbo, 6 * sizeof(hud_pos_t));
	DynamicVBO_SetAttrib(&hud_fill_vbo, 0, 2, GL_FLOAT, GL_FALSE, sizeof(hud_pos_t), offsetof(hud_pos_t, x));
	hud_fill_ready = true;
}

void R_HudFill(int x, int y, int w, int h, float r, float g, float b, float a)
{
	if (!R_EnsureFullscreenShader()) return;
	EnsureHudFillVBO();

	float x0 = (float)x,     y0 = (float)y;
	float x1 = (float)(x+w), y1 = (float)(y+h);

	hud_pos_t verts[6] = {
		{x0, y0}, {x1, y0}, {x1, y1},
		{x0, y0}, {x1, y1}, {x0, y1},
	};

	float mvp[16];
	R_CurrentMVP(mvp);

	GLShader_Use(&R_FullscreenShader);
	glUniformMatrix4fv(R_FullscreenShader_u_mvp, 1, GL_FALSE, mvp);
	glUniform4f(R_FullscreenShader_u_color, r, g, b, a);

	DynamicVBO_Upload(&hud_fill_vbo, verts, sizeof(verts));
	DynamicVBO_Bind(&hud_fill_vbo);
	Prof_CountDraw(6);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	glBindVertexArray(0);
	glUseProgram(0);
}

// ---------------------------------------------------------------------------
// Character batching
// ---------------------------------------------------------------------------

#define HUD_CHAR_BATCH_MAX 4096
static hud_vtx_t  char_batch_verts[HUD_CHAR_BATCH_MAX * 6];
static int        char_batch_count = 0;
static int        char_batch_texture = 0;
static qboolean   char_batch_active = false;
static DynamicVBO char_batch_vbo;
static qboolean   char_batch_vbo_ready = false;

static void EnsureCharBatchVBO(void)
{
	if (char_batch_vbo_ready) return;
	DynamicVBO_Init(&char_batch_vbo, sizeof(char_batch_verts));
	DynamicVBO_SetAttrib(&char_batch_vbo, 0, 2, GL_FLOAT,         GL_FALSE, sizeof(hud_vtx_t), offsetof(hud_vtx_t, x));
	DynamicVBO_SetAttrib(&char_batch_vbo, 1, 2, GL_FLOAT,         GL_FALSE, sizeof(hud_vtx_t), offsetof(hud_vtx_t, s));
	DynamicVBO_SetAttrib(&char_batch_vbo, 2, 4, GL_UNSIGNED_BYTE, GL_TRUE,  sizeof(hud_vtx_t), offsetof(hud_vtx_t, r));
	char_batch_vbo_ready = true;
}

void R_HudBeginCharBatch(int char_texture)
{
	if (char_batch_active) return;
	if (!R_EnsureHud2dShader()) return;
	EnsureCharBatchVBO();
	char_batch_active  = true;
	char_batch_texture = char_texture;
	char_batch_count   = 0;
}

void R_HudAddChar(int x, int y, int ch, float r, float g, float b, float a)
{
	if (!char_batch_active) return;
	if (char_batch_count >= HUD_CHAR_BATCH_MAX) return;

	ch &= 255;
	int row = ch >> 4;
	int col = ch & 15;
	float frow = row * 0.0625f;
	float fcol = col * 0.0625f;

	unsigned char cr = (unsigned char)(r * 255.0f + 0.5f);
	unsigned char cg = (unsigned char)(g * 255.0f + 0.5f);
	unsigned char cb = (unsigned char)(b * 255.0f + 0.5f);
	unsigned char ca = (unsigned char)(a * 255.0f + 0.5f);

	float x0 = (float)x,       y0 = (float)y;
	float x1 = (float)(x + 8), y1 = (float)(y + 8);
	float s0 = fcol,           t0 = frow;
	float s1 = fcol + 0.0625f, t1 = frow + 0.0625f;

	hud_vtx_t *v = &char_batch_verts[char_batch_count * 6];
	v[0] = {x0, y0, s0, t0, cr, cg, cb, ca};
	v[1] = {x1, y0, s1, t0, cr, cg, cb, ca};
	v[2] = {x1, y1, s1, t1, cr, cg, cb, ca};
	v[3] = {x0, y0, s0, t0, cr, cg, cb, ca};
	v[4] = {x1, y1, s1, t1, cr, cg, cb, ca};
	v[5] = {x0, y1, s0, t1, cr, cg, cb, ca};
	char_batch_count++;
}

void R_HudEndCharBatch(void)
{
	if (!char_batch_active) return;
	if (char_batch_count == 0) {
		char_batch_active = false;
		return;
	}

	float mvp[16];
	R_CurrentMVP(mvp);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, char_batch_texture);
	GLShader_Use(&R_Hud2dShader);
	glUniformMatrix4fv(R_Hud2dShader_u_ortho, 1, GL_FALSE, mvp);
	glUniform1i(R_Hud2dShader_u_tex, 0);

	DynamicVBO_Upload(&char_batch_vbo, char_batch_verts, (GLsizei)(char_batch_count * 6 * sizeof(hud_vtx_t)));
	DynamicVBO_Bind(&char_batch_vbo);
	Prof_CountDraw(char_batch_count * 6);
	glDrawArrays(GL_TRIANGLES, 0, char_batch_count * 6);

	glBindVertexArray(0);
	glUseProgram(0);

	char_batch_count  = 0;
	char_batch_active = false;
}
