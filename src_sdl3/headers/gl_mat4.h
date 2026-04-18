/*
gl_mat4.h -- minimal 4x4 matrix helpers for the GL 3.3 migration.

Column-major layout to match GL's glUniformMatrix4fv(transpose=GL_FALSE) convention.
All functions are portable C, no dependencies beyond <math.h>.
*/

#ifndef GL_MAT4_H
#define GL_MAT4_H

// Column-major: m[col * 4 + row].
typedef struct {
	float m[16];
} mat4_t;

void mat4_identity(mat4_t *o);
void mat4_mul(mat4_t *o, const mat4_t *a, const mat4_t *b);

// Build common transforms. All write into `o`, overwriting it.
void mat4_ortho(mat4_t *o, float l, float r, float b, float t, float n, float f);
void mat4_perspective(mat4_t *o, float fovy_deg, float aspect, float n, float f);
void mat4_translate(mat4_t *o, float x, float y, float z);
void mat4_scale(mat4_t *o, float x, float y, float z);
void mat4_rotate_x(mat4_t *o, float deg);
void mat4_rotate_y(mat4_t *o, float deg);
void mat4_rotate_z(mat4_t *o, float deg);

#endif // GL_MAT4_H
