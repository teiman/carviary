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

void mat4_rotate_axis(mat4_t *o, float deg, float x, float y, float z)
{
	// Axis-angle rotation, matches glRotatef. Axis must be non-zero; we
	// normalize defensively.
	float len = sqrtf(x*x + y*y + z*z);
	if (len < 1e-6f) { mat4_identity(o); return; }
	x /= len; y /= len; z /= len;

	float r = deg2rad(deg);
	float c = cosf(r), s = sinf(r);
	float omc = 1.0f - c;

	// Column-major.
	o->m[0]  = x*x*omc + c;     o->m[1]  = y*x*omc + z*s;   o->m[2]  = z*x*omc - y*s;   o->m[3]  = 0;
	o->m[4]  = x*y*omc - z*s;   o->m[5]  = y*y*omc + c;     o->m[6]  = z*y*omc + x*s;   o->m[7]  = 0;
	o->m[8]  = x*z*omc + y*s;   o->m[9]  = y*z*omc - x*s;   o->m[10] = z*z*omc + c;     o->m[11] = 0;
	o->m[12] = 0;               o->m[13] = 0;               o->m[14] = 0;               o->m[15] = 1;
}

void mat4_frustum(mat4_t *o, float l, float r, float b, float t, float n, float f)
{
	memset(o, 0, sizeof(*o));
	o->m[0]  =  (2.0f * n) / (r - l);
	o->m[5]  =  (2.0f * n) / (t - b);
	o->m[8]  =  (r + l) / (r - l);
	o->m[9]  =  (t + b) / (t - b);
	o->m[10] = -(f + n) / (f - n);
	o->m[11] = -1.0f;
	o->m[14] = -(2.0f * f * n) / (f - n);
}

// ---------------------------------------------------------------------------
// Stacks
// ---------------------------------------------------------------------------
MatStack r_modelview;
MatStack r_projection;

void MatStack_Init(MatStack *s)
{
	s->top = 0;
	mat4_identity(&s->m[0]);
}

void MatStack_Push(MatStack *s)
{
	if (s->top + 1 >= MAT_STACK_DEPTH) return;
	s->m[s->top + 1] = s->m[s->top];
	s->top++;
}

void MatStack_Pop(MatStack *s)
{
	if (s->top > 0) s->top--;
}

mat4_t *MatStack_Top(MatStack *s)
{
	return &s->m[s->top];
}

void MatStack_LoadIdentity(MatStack *s)
{
	mat4_identity(&s->m[s->top]);
}

void MatStack_Load(MatStack *s, const mat4_t *m)
{
	s->m[s->top] = *m;
}

// Post-multiply helpers: top = top * transform. This matches glRotatef etc.
// which post-multiply the current matrix by the new transform.
static void stack_postmul(MatStack *s, const mat4_t *rhs)
{
	mat4_t cur = s->m[s->top];
	mat4_mul(&s->m[s->top], &cur, rhs);
}

void MatStack_MulTranslate(MatStack *s, float x, float y, float z)
{
	mat4_t t; mat4_translate(&t, x, y, z);
	stack_postmul(s, &t);
}

void MatStack_MulScale(MatStack *s, float x, float y, float z)
{
	mat4_t t; mat4_scale(&t, x, y, z);
	stack_postmul(s, &t);
}

void MatStack_MulRotate(MatStack *s, float deg, float x, float y, float z)
{
	mat4_t t; mat4_rotate_axis(&t, deg, x, y, z);
	stack_postmul(s, &t);
}

// MVP = projection * modelview (both read from their respective stack tops).
void R_MVP(float out[16])
{
	mat4_t mvp;
	mat4_mul(&mvp, &r_projection.m[r_projection.top], &r_modelview.m[r_modelview.top]);
	memcpy(out, mvp.m, sizeof(float) * 16);
}
