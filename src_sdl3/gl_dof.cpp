/*
	gl_dof.cpp -- Depth of Field post-process effect

	Uses the fixed-function GL pipeline:
	1. Read the rendered scene + depth buffer
	2. Downsample and blur on CPU
	3. Composite sharp + blurred based on depth
	4. Write result back with glDrawPixels
*/

#include "quakedef.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// CVars
cvar_t	r_dof			= {"r_dof", "0", true};				// 0=off, 1=on, 3=debug depth view
cvar_t	r_dof_focal		= {"r_dof_focal", "0.04", true};	// focal depth (linear 0=near, 1=far)
cvar_t	r_dof_range		= {"r_dof_range", "0.03", true};	// range around focal point that stays sharp
cvar_t	r_dof_strength	= {"r_dof_strength", "0.5", true};	// blur strength multiplier
cvar_t	r_dof_blur		= {"r_dof_blur", "0", true};		// blur radius at quarter res

// Buffers (allocated once, resized as needed)
static byte		*dof_scene_pixels;		// full-res scene (RGB)
static float	*dof_depth_pixels;		// full-res depth (raw z-buffer)
static float	*dof_linear_depth;		// full-res depth (linearized 0-1)
static byte		*dof_small_pixels;		// quarter-res scene
static byte		*dof_blur_pixels;		// quarter-res blurred
static byte		*dof_result_pixels;		// full-res composited result
static int		dof_buf_w, dof_buf_h;

static void DOF_AllocBuffers(int w, int h)
{
	if (dof_buf_w == w && dof_buf_h == h)
		return;

	if (dof_scene_pixels)	free(dof_scene_pixels);
	if (dof_depth_pixels)	free(dof_depth_pixels);
	if (dof_linear_depth)	free(dof_linear_depth);
	if (dof_small_pixels)	free(dof_small_pixels);
	if (dof_blur_pixels)	free(dof_blur_pixels);
	if (dof_result_pixels)	free(dof_result_pixels);

	dof_buf_w = w;
	dof_buf_h = h;

	int sw = w / 4;
	int sh = h / 4;

	dof_scene_pixels	= (byte *)malloc(w * h * 3);
	dof_depth_pixels	= (float *)malloc(w * h * sizeof(float));
	dof_linear_depth	= (float *)malloc(w * h * sizeof(float));
	dof_small_pixels	= (byte *)malloc(sw * sh * 3);
	dof_blur_pixels		= (byte *)malloc(sw * sh * 3);
	dof_result_pixels	= (byte *)malloc(w * h * 3);
}

void DOF_Init(void)
{
	Cvar_RegisterVariable(&r_dof);
	Cvar_RegisterVariable(&r_dof_focal);
	Cvar_RegisterVariable(&r_dof_range);
	Cvar_RegisterVariable(&r_dof_strength);
	Cvar_RegisterVariable(&r_dof_blur);
}

// Downsample full-res to quarter-res using box filter
static void DOF_Downsample(byte *src, int sw, int sh, byte *dst, int dw, int dh)
{
	for (int y = 0; y < dh; y++)
	{
		int sy = y * sh / dh;
		for (int x = 0; x < dw; x++)
		{
			int sx = x * sw / dw;

			// 2x2 box filter
			int r = 0, g = 0, b = 0;
			int count = 0;
			for (int dy = 0; dy < 2 && (sy + dy) < sh; dy++)
			{
				for (int dx = 0; dx < 2 && (sx + dx) < sw; dx++)
				{
					int si = ((sy + dy) * sw + (sx + dx)) * 3;
					r += src[si + 0];
					g += src[si + 1];
					b += src[si + 2];
					count++;
				}
			}
			int di = (y * dw + x) * 3;
			dst[di + 0] = r / count;
			dst[di + 1] = g / count;
			dst[di + 2] = b / count;
		}
	}
}

// Simple box blur at quarter resolution
static void DOF_Blur(byte *src, byte *dst, int w, int h, int radius)
{
	// Horizontal pass into dst
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			int r = 0, g = 0, b = 0, count = 0;
			for (int dx = -radius; dx <= radius; dx++)
			{
				int sx = x + dx;
				if (sx < 0) sx = 0;
				if (sx >= w) sx = w - 1;
				int si = (y * w + sx) * 3;
				r += src[si + 0];
				g += src[si + 1];
				b += src[si + 2];
				count++;
			}
			int di = (y * w + x) * 3;
			dst[di + 0] = r / count;
			dst[di + 1] = g / count;
			dst[di + 2] = b / count;
		}
	}

	// Vertical pass in-place (use src as temp, then copy back)
	memcpy(src, dst, w * h * 3);
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			int r = 0, g = 0, b = 0, count = 0;
			for (int dy = -radius; dy <= radius; dy++)
			{
				int sy = y + dy;
				if (sy < 0) sy = 0;
				if (sy >= h) sy = h - 1;
				int si = (sy * w + x) * 3;
				r += src[si + 0];
				g += src[si + 1];
				b += src[si + 2];
				count++;
			}
			int di = (y * w + x) * 3;
			dst[di + 0] = r / count;
			dst[di + 1] = g / count;
			dst[di + 2] = b / count;
		}
	}
}

// Composite: blend sharp scene with blurred version based on depth
static void DOF_Composite(byte *sharp, byte *blur_small, float *depth,
						  byte *result, int w, int h, int bw, int bh,
						  float focal, float range, float strength)
{
	for (int y = 0; y < h; y++)
	{
		int by = y * bh / h;
		if (by >= bh) by = bh - 1;

		for (int x = 0; x < w; x++)
		{
			int pi = y * w + x;
			int si = pi * 3;

			// Depth-based blur factor
			float d = depth[pi];
			float dist = (float)fabs(d - focal);
			float factor = (dist - range) / (range + 0.001f);
			if (factor < 0.0f) factor = 0.0f;
			if (factor > 1.0f) factor = 1.0f;
			factor *= strength;
			if (factor > 1.0f) factor = 1.0f;

			// Lookup blurred pixel (nearest from quarter-res)
			int bx = x * bw / w;
			if (bx >= bw) bx = bw - 1;
			int bi = (by * bw + bx) * 3;

			// Blend
			result[si + 0] = (byte)(sharp[si + 0] * (1.0f - factor) + blur_small[bi + 0] * factor);
			result[si + 1] = (byte)(sharp[si + 1] * (1.0f - factor) + blur_small[bi + 1] * factor);
			result[si + 2] = (byte)(sharp[si + 2] * (1.0f - factor) + blur_small[bi + 2] * factor);
		}
	}
}

/*
================
DOF_Apply

Called after V_RenderView(), before GL_Set2D().
Reads the 3D scene and depth, applies DoF blur, writes result back.
================
*/
void DOF_Apply(void)
{
	if (!r_dof.value)
		return;

	int w = glwidth;
	int h = glheight;

	if (w < 16 || h < 16)
		return;

	DOF_AllocBuffers(w, h);

	int sw = w / 4;
	int sh = h / 4;
	if (sw < 4 || sh < 4)
		return;

	// Read the rendered scene and depth buffer
	glReadPixels(glx, gly, w, h, GL_RGB, GL_UNSIGNED_BYTE, dof_scene_pixels);
	glReadPixels(glx, gly, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, dof_depth_pixels);

	// Linearize the depth buffer
	// Engine uses znear ~4.0, zfar = 8193 (from MYgluPerspective in gl_rmain.cpp)
	float znear = 4.0f;
	float zfar = 8193.0f;
	for (int i = 0; i < w * h; i++)
	{
		float z = dof_depth_pixels[i];
		// Convert hyperbolic z-buffer to linear [0..1] where 0=near, 1=far
		float lin = (znear * zfar) / (zfar - z * (zfar - znear));
		dof_linear_depth[i] = (lin - znear) / (zfar - znear);
	}

	// r_dof 3 = visualize linearized depth buffer
	if (r_dof.value >= 3)
	{
		for (int i = 0; i < w * h; i++)
		{
			byte v = (byte)(dof_linear_depth[i] * 255.0f);
			dof_result_pixels[i * 3 + 0] = v;
			dof_result_pixels[i * 3 + 1] = v;
			dof_result_pixels[i * 3 + 2] = v;
		}
	}
	else
	{
		// Downsample scene to quarter resolution
		DOF_Downsample(dof_scene_pixels, w, h, dof_small_pixels, sw, sh);

		// Blur the downsampled image
		int blur_radius = (int)r_dof_blur.value;
		if (blur_radius < 1) blur_radius = 1;
		if (blur_radius > 20) blur_radius = 20;
		DOF_Blur(dof_small_pixels, dof_blur_pixels, sw, sh, blur_radius);

		// Composite based on linearized depth
		float focal = r_dof_focal.value;
		float range = r_dof_range.value;
		float strength = r_dof_strength.value;
		if (strength < 0.0f) strength = 0.0f;

		DOF_Composite(dof_scene_pixels, dof_blur_pixels, dof_linear_depth,
					  dof_result_pixels, w, h, sw, sh, focal, range, strength);
	}

	// Write result directly to framebuffer
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0, w, 0, h, -1, 1);

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);

	glRasterPos2i(glx, gly);
	glDrawPixels(w, h, GL_RGB, GL_UNSIGNED_BYTE, dof_result_pixels);

	// Restore GL state
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_DEPTH_TEST);

	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
}
