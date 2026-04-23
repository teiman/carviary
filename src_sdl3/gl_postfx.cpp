// gl_postfx.cpp -- offscreen scene FBO and post-processing pipeline.
//
// Stage 1 (this file at first): the 3D scene is rendered into an offscreen
// FBO (R_Clear .. R_PolyBlend), then blitted to the default framebuffer so
// the HUD/console can overlay as before. Zero visual change by itself.
//
// Future stages plug on top of this infrastructure:
//   - Second color attachment (fb_mask) that captures fullbright pixels,
//     then a blur + additive composite for a "fullbright glow" effect.
//   - Any other scene-wide post-process (motion blur, fog, etc.).

#include "quakedef.h"
#include "gl_render.h"
#include "gl_core.h"

// ---------------------------------------------------------------------------
// Scene FBO state. Lazily (re)allocated when viewport size changes.
// ---------------------------------------------------------------------------
static GLuint  scene_fbo          = 0;
static GLuint  scene_color_tex    = 0;   // attachment 0: main color
static GLuint  scene_fbmask_tex   = 0;   // attachment 1: fullbright mask
static GLuint  scene_depth_tex    = 0;   // depth texture (sampled in composite)
static int     scene_width        = 0;
static int     scene_height       = 0;

// Snapshot of the depth buffer at an arbitrary moment in the frame. Used
// by the water shader so it can sample the "pre-water" scene depth without
// tripping the feedback-loop rule (sampling an attachment of the currently
// bound FBO is UB). PostFX_SnapshotDepth() blits scene_depth_tex here.
static GLuint  depth_snapshot_fbo = 0;
static GLuint  depth_snapshot_tex = 0;

// Ping-pong FBOs used to blur the fb_mask in two separable passes
// (horizontal then vertical). Same size as the scene for now; can be
// downscaled later if we need to push the radius cheaply.
static GLuint  blur_fbo[2]       = { 0, 0 };
static GLuint  blur_tex[2]       = { 0, 0 };

static qboolean postfx_active = false;   // true between Begin/End

// Cached draw-buffer setups.
static const GLenum DRAWBUFS_COLOR_ONLY[] = { GL_COLOR_ATTACHMENT0, GL_NONE };
static const GLenum DRAWBUFS_COLOR_AND_MASK[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };

// Debug: if 1, final blit samples fb_mask (blurred) instead of the main
// scene so you can see the glow field itself.
static cvar_t r_fb_debug  = {"r_fb_debug", "0"};

// Glow strength + radius. r_fb_glow_radius = number of blur iterations
// (each pass is a 9-tap gaussian; radius stacks linearly). 0 disables.
static cvar_t r_fb_glow         = {"r_fb_glow",         "1"};
static cvar_t r_fb_glow_radius  = {"r_fb_glow_radius",  "12"};
static cvar_t r_fb_glow_strength= {"r_fb_glow_strength","1.0"};

// ---------------------------------------------------------------------------
// Blit shader: sample scene_color_tex and write to default framebuffer.
// Uses a clip-space triangle (no VBO needed, gl_VertexID drives positions).
// ---------------------------------------------------------------------------
static GLShader R_PostFXBlitShader;
static GLint    R_PostFXBlitShader_u_tex = -1;
static qboolean blit_shader_ok = false;
static GLuint   blit_vao       = 0;

// Separable bilateral gaussian blur. Same as a standard 9-tap gaussian
// except each tap's weight is also multiplied by a depth-similarity term
// so the mask does not bleed across a depth discontinuity (doors, walls
// covering an emitter). This is the key to keeping the glow from leaking
// through occluders without killing the halo around visible emitters.
static GLShader R_BlurShader;
static GLint    R_BlurShader_u_tex       = -1;
static GLint    R_BlurShader_u_depth     = -1;
static GLint    R_BlurShader_u_direction = -1;
static qboolean blur_shader_ok = false;

// Additive composite: sample blurred mask, multiply by strength, add
// to the current framebuffer. Used in the final blit when glow is on.
static GLShader R_CompositeShader;
static GLint    R_CompositeShader_u_scene    = -1;
static GLint    R_CompositeShader_u_glow     = -1;
static GLint    R_CompositeShader_u_depth    = -1;
static GLint    R_CompositeShader_u_strength = -1;
static qboolean composite_shader_ok = false;

static qboolean R_EnsurePostFXBlitShader (void)
{
	if (blit_shader_ok) return true;
	const char *vs =
		"#version 330 core\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    // Oversized triangle covering the viewport:\n"
		"    //   vid 0 -> (-1,-1), vid 1 -> ( 3,-1), vid 2 -> (-1, 3)\n"
		"    vec2 p = vec2(\n"
		"        (gl_VertexID == 1) ?  3.0 : -1.0,\n"
		"        (gl_VertexID == 2) ?  3.0 : -1.0);\n"
		"    v_uv = (p + 1.0) * 0.5;\n"
		"    gl_Position = vec4(p, 0.0, 1.0);\n"
		"}\n";
	const char *fs =
		"#version 330 core\n"
		"in vec2 v_uv;\n"
		"uniform sampler2D u_tex;\n"
		"out vec4 frag_color;\n"
		"void main() {\n"
		"    frag_color = texture(u_tex, v_uv);\n"
		"}\n";
	char err[512];
	if (!GLShader_Build(&R_PostFXBlitShader, vs, fs, err, sizeof(err))) {
		Con_Printf("postfx blit shader failed: %s\n", err);
		return false;
	}
	R_PostFXBlitShader_u_tex = GLShader_Uniform(&R_PostFXBlitShader, "u_tex");
	blit_shader_ok = true;
	return true;
}

static qboolean R_EnsureBlurShader (void)
{
	if (blur_shader_ok) return true;
	const char *vs =
		"#version 330 core\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    vec2 p = vec2(\n"
		"        (gl_VertexID == 1) ?  3.0 : -1.0,\n"
		"        (gl_VertexID == 2) ?  3.0 : -1.0);\n"
		"    v_uv = (p + 1.0) * 0.5;\n"
		"    gl_Position = vec4(p, 0.0, 1.0);\n"
		"}\n";
	// 9-tap bilateral gaussian. Depth-aware weighting avoids mixing the
	// mask across occluders: a tap farther from the camera than the
	// center (i.e. coming from BEHIND something that covers the center
	// in screen space) is down-weighted -- the glow of an emitter hidden
	// behind a door will not spill onto the door's pixels.
	// RGB: plain 9-tap gaussian. Alpha: MAX over the same taps, so the
	// resulting alpha channel carries the depth of the CLOSEST emitter
	// that reached this pixel. The composite later rejects pixels where
	// an occluder is drawn in front of that emitter.
	const char *fs =
		"#version 330 core\n"
		"in vec2 v_uv;\n"
		"uniform sampler2D u_tex;\n"
		"uniform vec2      u_direction;\n"
		"out vec4 frag_color;\n"
		"void main() {\n"
		"    const float w0 = 0.227027;\n"
		"    const float w1 = 0.194594;\n"
		"    const float w2 = 0.121622;\n"
		"    const float w3 = 0.054054;\n"
		"    const float w4 = 0.016216;\n"
		"    vec4 t0 = texture(u_tex, v_uv);\n"
		"    vec4 t1p = texture(u_tex, v_uv + u_direction * 1.0);\n"
		"    vec4 t1n = texture(u_tex, v_uv - u_direction * 1.0);\n"
		"    vec4 t2p = texture(u_tex, v_uv + u_direction * 2.0);\n"
		"    vec4 t2n = texture(u_tex, v_uv - u_direction * 2.0);\n"
		"    vec4 t3p = texture(u_tex, v_uv + u_direction * 3.0);\n"
		"    vec4 t3n = texture(u_tex, v_uv - u_direction * 3.0);\n"
		"    vec4 t4p = texture(u_tex, v_uv + u_direction * 4.0);\n"
		"    vec4 t4n = texture(u_tex, v_uv - u_direction * 4.0);\n"
		"    vec3 rgb = t0.rgb * w0\n"
		"             + (t1p.rgb + t1n.rgb) * w1\n"
		"             + (t2p.rgb + t2n.rgb) * w2\n"
		"             + (t3p.rgb + t3n.rgb) * w3\n"
		"             + (t4p.rgb + t4n.rgb) * w4;\n"
		"    // Alpha is the emitter depth (0 near, 1 far). We want the\n"
		"    // MIN of non-zero alphas across the taps (nearest emitter).\n"
		"    // A tap whose alpha is 0 means 'no contribution' and is\n"
		"    // ignored. We add 1.0 to zero taps so they don't win the min.\n"
		"    float BIG = 2.0;\n"
		"    float a0  = t0.a  > 0.0 ? t0.a  : BIG;\n"
		"    float a1p = t1p.a > 0.0 ? t1p.a : BIG;\n"
		"    float a1n = t1n.a > 0.0 ? t1n.a : BIG;\n"
		"    float a2p = t2p.a > 0.0 ? t2p.a : BIG;\n"
		"    float a2n = t2n.a > 0.0 ? t2n.a : BIG;\n"
		"    float a3p = t3p.a > 0.0 ? t3p.a : BIG;\n"
		"    float a3n = t3n.a > 0.0 ? t3n.a : BIG;\n"
		"    float a4p = t4p.a > 0.0 ? t4p.a : BIG;\n"
		"    float a4n = t4n.a > 0.0 ? t4n.a : BIG;\n"
		"    float mn = min(min(min(a0, min(a1p,a1n)), min(min(a2p,a2n), min(a3p,a3n))), min(a4p,a4n));\n"
		"    float alpha = (mn >= BIG) ? 0.0 : mn;\n"
		"    frag_color = vec4(rgb, alpha);\n"
		"}\n";
	char err[512];
	if (!GLShader_Build(&R_BlurShader, vs, fs, err, sizeof(err))) {
		Con_Printf("postfx blur shader failed: %s\n", err);
		return false;
	}
	R_BlurShader_u_tex       = GLShader_Uniform(&R_BlurShader, "u_tex");
	R_BlurShader_u_direction = GLShader_Uniform(&R_BlurShader, "u_direction");
	blur_shader_ok = true;
	return true;
}

static qboolean R_EnsureCompositeShader (void)
{
	if (composite_shader_ok) return true;
	const char *vs =
		"#version 330 core\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    vec2 p = vec2(\n"
		"        (gl_VertexID == 1) ?  3.0 : -1.0,\n"
		"        (gl_VertexID == 2) ?  3.0 : -1.0);\n"
		"    v_uv = (p + 1.0) * 0.5;\n"
		"    gl_Position = vec4(p, 0.0, 1.0);\n"
		"}\n";
	const char *fs =
		"#version 330 core\n"
		"in vec2 v_uv;\n"
		"uniform sampler2D u_scene;\n"
		"uniform sampler2D u_glow;\n"
		"uniform sampler2D u_depth;\n"
		"uniform float     u_strength;\n"
		"out vec4 frag_color;\n"
		"void main() {\n"
		"    vec3  s  = texture(u_scene, v_uv).rgb;\n"
		"    vec4  g4 = texture(u_glow,  v_uv);\n"
		"    vec3  g  = g4.rgb;\n"
		"    // Alpha of the blurred mask = depth of the NEAREST emitter that\n"
		"    // reached this pixel (propagated by min() through the blur).\n"
		"    // If an occluder now sits in front of that emitter in screen\n"
		"    // space, scene_depth < emitter_depth -> reject the glow.\n"
		"    float emitter_depth = g4.a;\n"
		"    float scene_depth   = texture(u_depth, v_uv).r;\n"
		"    float visible = (emitter_depth > 0.0) ?\n"
		"                    step(emitter_depth, scene_depth + 0.0005) : 0.0;\n"
		"    frag_color = vec4(s + g * u_strength * visible, 1.0);\n"
		"}\n";
	char err[512];
	if (!GLShader_Build(&R_CompositeShader, vs, fs, err, sizeof(err))) {
		Con_Printf("postfx composite shader failed: %s\n", err);
		return false;
	}
	R_CompositeShader_u_scene    = GLShader_Uniform(&R_CompositeShader, "u_scene");
	R_CompositeShader_u_glow     = GLShader_Uniform(&R_CompositeShader, "u_glow");
	R_CompositeShader_u_depth    = GLShader_Uniform(&R_CompositeShader, "u_depth");
	R_CompositeShader_u_strength = GLShader_Uniform(&R_CompositeShader, "u_strength");
	composite_shader_ok = true;
	return true;
}

static void R_DestroySceneFBO (void)
{
	if (scene_fbo)        { glDeleteFramebuffers(1, &scene_fbo);        scene_fbo = 0; }
	if (scene_color_tex)  { glDeleteTextures(1, &scene_color_tex);      scene_color_tex = 0; }
	if (scene_fbmask_tex) { glDeleteTextures(1, &scene_fbmask_tex);     scene_fbmask_tex = 0; }
	if (scene_depth_tex)  { glDeleteTextures(1, &scene_depth_tex);      scene_depth_tex = 0; }
	if (depth_snapshot_fbo) { glDeleteFramebuffers(1, &depth_snapshot_fbo); depth_snapshot_fbo = 0; }
	if (depth_snapshot_tex) { glDeleteTextures(1, &depth_snapshot_tex);     depth_snapshot_tex = 0; }
	for (int i = 0; i < 2; ++i) {
		if (blur_fbo[i]) { glDeleteFramebuffers(1, &blur_fbo[i]); blur_fbo[i] = 0; }
		if (blur_tex[i]) { glDeleteTextures(1, &blur_tex[i]);     blur_tex[i] = 0; }
	}
	scene_width = scene_height = 0;
}

static qboolean R_EnsureBlurFBOs (int w, int h)
{
	if (blur_fbo[0] && blur_tex[0]) return true;   // tied to scene size
	for (int i = 0; i < 2; ++i) {
		glGenTextures(1, &blur_tex[i]);
		glBindTexture(GL_TEXTURE_2D, blur_tex[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenFramebuffers(1, &blur_fbo[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, blur_fbo[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, blur_tex[i], 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			Con_Printf("postfx: blur FBO %d incomplete\n", i);
			return false;
		}
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

static qboolean R_EnsureSceneFBO (int w, int h)
{
	if (w <= 0 || h <= 0) return false;
	if (scene_fbo && scene_width == w && scene_height == h) return true;

	R_DestroySceneFBO();

	glGenTextures(1, &scene_color_tex);
	glBindTexture(GL_TEXTURE_2D, scene_color_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// fb_mask: receives contributions from fullbright passes only. HDR-ish
	// RGBA8 is fine for an additive mask; glow values rarely exceed 1.
	glGenTextures(1, &scene_fbmask_tex);
	glBindTexture(GL_TEXTURE_2D, scene_fbmask_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// Depth as a texture (not a renderbuffer) so the composite pass can
	// sample it. Needed for depth-aware glow that does not leak through
	// occluders like doors.
	glGenTextures(1, &scene_depth_tex);
	glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
	             GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenFramebuffers(1, &scene_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, scene_color_tex, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
	                       GL_TEXTURE_2D, scene_fbmask_tex, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
	                       GL_TEXTURE_2D, scene_depth_tex, 0);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		Con_Printf("postfx: scene FBO incomplete (0x%x)\n", status);
		R_DestroySceneFBO();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return false;
	}

	// Depth snapshot: a separate texture + FBO we can blit the depth into,
	// so the water shader samples an unrelated texture instead of the
	// currently-bound depth attachment (avoids GL feedback-loop UB).
	glGenTextures(1, &depth_snapshot_tex);
	glBindTexture(GL_TEXTURE_2D, depth_snapshot_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
	             GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenFramebuffers(1, &depth_snapshot_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, depth_snapshot_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
	                       GL_TEXTURE_2D, depth_snapshot_tex, 0);
	// Depth-only FBOs need draw buffers disabled.
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		Con_Printf("postfx: depth snapshot FBO incomplete\n");
		R_DestroySceneFBO();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return false;
	}

	scene_width  = w;
	scene_height = h;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (!R_EnsureBlurFBOs(w, h)) {
		R_DestroySceneFBO();
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Public API. Bracket the 3D scene draws with Begin/End. HUD/console draws
// AFTER End, to the default framebuffer.
// ---------------------------------------------------------------------------
void PostFX_BeginScene (void)
{
	int w = (int)vid.width;
	int h = (int)vid.height;
	if (!R_EnsureSceneFBO(w, h)) {
		postfx_active = false;
		return;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);

	// Both attachments always receive writes. Opaque shaders write 0 to
	// fb_mask (attachment 1) so that when geometry is drawn over a pixel
	// that was previously tagged as fullbright (e.g. a door closing over
	// a fullbright wall panel), the tag is cleared -- the bloom pass
	// won't leak through the occluder.
	glDrawBuffers(2, DRAWBUFS_COLOR_AND_MASK);
	GLfloat zero[4] = { 0, 0, 0, 0 };
	glClearBufferfv(GL_COLOR, 1, zero);           // only attachment 1

	postfx_active = true;
}

// Fullbright-mask bracketing is now a no-op: both draw buffers are always
// attached. Kept as exported symbols so existing call sites stay valid.
void PostFX_BeginFullbrightMask (void) {}
void PostFX_EndFullbrightMask   (void) {}

// Shaders that can't declare a `location=1` output (dual-source blending
// shaders already use locations 0 index 0 and 0 index 1 -- adding a third
// output exceeds the driver's budget) bracket their draw with this pair
// so the fb_mask attachment is disabled and previous tags at the written
// pixels stay untouched.
void PostFX_BeginNoMaskWrite (void)
{
	if (!postfx_active) return;
	glDrawBuffers(2, DRAWBUFS_COLOR_ONLY);
}
void PostFX_EndNoMaskWrite (void)
{
	if (!postfx_active) return;
	glDrawBuffers(2, DRAWBUFS_COLOR_AND_MASK);
}

// Getter so other effects (sky sun) can sample the scene depth.
GLuint PostFX_GetDepthTex (void) { return scene_depth_tex; }

// Getter for the snapshot. Call PostFX_SnapshotDepth first to refresh.
GLuint PostFX_GetDepthSnapshotTex (void) { return depth_snapshot_tex; }

// Blit the current scene depth into the snapshot texture. After this the
// snapshot holds the depth as of NOW; any subsequent writes to scene_depth_tex
// won't affect it. Call right before a pass that needs to sample depth.
void PostFX_SnapshotDepth (void)
{
	if (!postfx_active) return;
	if (!depth_snapshot_fbo || !scene_fbo) return;

	GLint prev_read, prev_draw;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, depth_snapshot_fbo);
	glBlitFramebuffer(0, 0, scene_width, scene_height,
	                  0, 0, scene_width, scene_height,
	                  GL_DEPTH_BUFFER_BIT, GL_NEAREST);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
}

void PostFX_EndScene (void)
{
	if (!postfx_active) return;
	postfx_active = false;

	if (!R_EnsurePostFXBlitShader()) { glBindFramebuffer(GL_FRAMEBUFFER, 0); return; }
	if (!blit_vao) glGenVertexArrays(1, &blit_vao);

	// Save state we touch.
	GLboolean blend_was      = glIsEnabled(GL_BLEND);
	GLboolean depth_test_was = glIsEnabled(GL_DEPTH_TEST);
	GLboolean cull_was       = glIsEnabled(GL_CULL_FACE);
	GLboolean scissor_was    = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean dmask_was; glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask_was);

	if (blend_was)       glDisable(GL_BLEND);
	if (depth_test_was)  glDisable(GL_DEPTH_TEST);
	if (cull_was)        glDisable(GL_CULL_FACE);
	if (scissor_was)     glDisable(GL_SCISSOR_TEST);
	glDepthMask(GL_FALSE);

	glBindVertexArray(blit_vao);

	// ---- Bloom pipeline: separable gaussian blur on fb_mask, N iterations ----
	int   iters    = (int)r_fb_glow_radius.value;
	if (iters < 0)  iters = 0;
	if (iters > 32) iters = 32;
	qboolean glow_on  = (r_fb_glow.value != 0.0f) && (iters > 0);
	GLuint   glow_tex = scene_fbmask_tex;   // starts as raw mask

	if (glow_on && R_EnsureBlurShader()) {
		GLuint src = scene_fbmask_tex;
		int    dst = 0;
		GLShader_Use(&R_BlurShader);
		glUniform1i(R_BlurShader_u_tex, 0);
		float invw = 1.0f / (float)scene_width;
		float invh = 1.0f / (float)scene_height;
		for (int it = 0; it < iters; ++it) {
			glBindFramebuffer(GL_FRAMEBUFFER, blur_fbo[dst]);
			glViewport(0, 0, scene_width, scene_height);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, src);
			glUniform2f(R_BlurShader_u_direction, invw, 0.0f);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glBindFramebuffer(GL_FRAMEBUFFER, blur_fbo[1 - dst]);
			glBindTexture(GL_TEXTURE_2D, blur_tex[dst]);
			glUniform2f(R_BlurShader_u_direction, 0.0f, invh);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			src = blur_tex[1 - dst];
			int tmp = dst; dst = 1 - dst; (void)tmp;
		}
		glow_tex = src;
	}

	// ---- Final composite to the default framebuffer ----
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, scene_width, scene_height);

	if (r_fb_debug.value) {
		// Raw mask view (blurred if glow is on, otherwise unblurred).
		GLShader_Use(&R_PostFXBlitShader);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, glow_tex);
		glUniform1i(R_PostFXBlitShader_u_tex, 0);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	} else if (glow_on && R_EnsureCompositeShader()) {
		GLShader_Use(&R_CompositeShader);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, scene_color_tex);
		glUniform1i(R_CompositeShader_u_scene, 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, glow_tex);
		glUniform1i(R_CompositeShader_u_glow, 1);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
		glUniform1i(R_CompositeShader_u_depth, 2);
		glUniform1f(R_CompositeShader_u_strength, r_fb_glow_strength.value);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glActiveTexture(GL_TEXTURE0);
	} else {
		// Glow off: plain blit.
		GLShader_Use(&R_PostFXBlitShader);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, scene_color_tex);
		glUniform1i(R_PostFXBlitShader_u_tex, 0);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}

	glDepthMask(dmask_was);
	if (cull_was)       glEnable(GL_CULL_FACE);
	if (depth_test_was) glEnable(GL_DEPTH_TEST);
	if (blend_was)      glEnable(GL_BLEND);
	if (scissor_was)    glEnable(GL_SCISSOR_TEST);

	glBindVertexArray(0);
	glUseProgram(0);
}

void PostFX_Shutdown (void)
{
	R_DestroySceneFBO();
	if (blit_vao) { glDeleteVertexArrays(1, &blit_vao); blit_vao = 0; }
}

void PostFX_Init (void)
{
	Cvar_RegisterVariable(&r_fb_debug);
	Cvar_RegisterVariable(&r_fb_glow);
	Cvar_RegisterVariable(&r_fb_glow_radius);
	Cvar_RegisterVariable(&r_fb_glow_strength);
}
