/*
gl_mat4.cpp -- implementation of mat4 helpers.
*/

#include "gl_mat4.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float deg2rad(float d) { return d * (float)(M_PI / 180.0); }

void mat4_identity(mat4_t *o)
{
	memset(o, 0, sizeof(*o));
	o->m[0] = o->m[5] = o->m[10] = o->m[15] = 1.0f;
}

void mat4_mul(mat4_t *o, const mat4_t *a, const mat4_t *b)
{
	mat4_t r;
	for (int c = 0; c < 4; ++c) {
		for (int rr = 0; rr < 4; ++rr) {
			float s = 0.0f;
			for (int k = 0; k < 4; ++k)
				s += a->m[k * 4 + rr] * b->m[c * 4 + k];
			r.m[c * 4 + rr] = s;
		}
	}
	*o = r;
}

void mat4_ortho(mat4_t *o, float l, float r, float b, float t, float n, float f)
{
	memset(o, 0, sizeof(*o));
	o->m[0]  =  2.0f / (r - l);
	o->m[5]  =  2.0f / (t - b);
	o->m[10] = -2.0f / (f - n);
	o->m[12] = -(r + l) / (r - l);
	o->m[13] = -(t + b) / (t - b);
	o->m[14] = -(f + n) / (f - n);
	o->m[15] =  1.0f;
}

void mat4_perspective(mat4_t *o, float fovy_deg, float aspect, float n, float f)
{
	float t = tanf(deg2rad(fovy_deg) * 0.5f);
	memset(o, 0, sizeof(*o));
	o->m[0]  = 1.0f / (aspect * t);
	o->m[5]  = 1.0f / t;
	o->m[10] = -(f + n) / (f - n);
	o->m[11] = -1.0f;
	o->m[14] = -(2.0f * f * n) / (f - n);
}

void mat4_translate(mat4_t *o, float x, float y, float z)
{
	mat4_identity(o);
	o->m[12] = x;
	o->m[13] = y;
	o->m[14] = z;
}

void mat4_scale(mat4_t *o, float x, float y, float z)
{
	mat4_identity(o);
	o->m[0] = x;
	o->m[5] = y;
	o->m[10] = z;
}

void mat4_rotate_x(mat4_t *o, float deg)
{
	float r = deg2rad(deg);
	float c = cosf(r), s = sinf(r);
	mat4_identity(o);
	o->m[5] = c;  o->m[6] = s;
	o->m[9] = -s; o->m[10] = c;
}

void mat4_rotate_y(mat4_t *o, float deg)
{
	float r = deg2rad(deg);
	float c = cosf(r), s = sinf(r);
	mat4_identity(o);
	o->m[0] = c;  o->m[2] = -s;
	o->m[8] = s;  o->m[10] = c;
}

void mat4_rotate_z(mat4_t *o, float deg)
{
	float r = deg2rad(deg);
	float c = cosf(r), s = sinf(r);
	mat4_identity(o);
	o->m[0] = c;  o->m[1] = s;
	o->m[4] = -s; o->m[5] = c;
}
