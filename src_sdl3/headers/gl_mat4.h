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

// Rotate by `deg` around axis (x,y,z). Matches glRotatef semantics.
void mat4_rotate_axis(mat4_t *o, float deg, float x, float y, float z);

// Frustum matrix, matches glFrustum.
void mat4_frustum(mat4_t *o, float l, float r, float b, float t, float n, float f);

// ---------------------------------------------------------------------------
// Matrix stack (replacement for glPushMatrix/glPopMatrix on the modelview)
//
// Usage mirrors the fixed-function pipeline: `Top()` is the current transform
// that will be fed to shaders as modelview. The `Mul*` helpers compose a new
// transform on top (equivalent to glRotate/Translate/Scale on the current
// matrix).
// ---------------------------------------------------------------------------
#define MAT_STACK_DEPTH 32

typedef struct {
	mat4_t m[MAT_STACK_DEPTH];
	int    top;
} MatStack;

void MatStack_Init(MatStack *s);   // top = 0, m[0] = identity
void MatStack_Push(MatStack *s);   // duplicates top
void MatStack_Pop(MatStack *s);    // discards top (underflow ignored)
mat4_t *MatStack_Top(MatStack *s); // pointer to current matrix

// Overwrite current matrix.
void MatStack_LoadIdentity(MatStack *s);
void MatStack_Load(MatStack *s, const mat4_t *m);

// Compose onto the current matrix (post-multiply, like fixed-function GL).
void MatStack_MulTranslate(MatStack *s, float x, float y, float z);
void MatStack_MulScale    (MatStack *s, float x, float y, float z);
void MatStack_MulRotate   (MatStack *s, float deg, float x, float y, float z);

// ---------------------------------------------------------------------------
// Global modelview and projection stacks used by the renderer.
// ---------------------------------------------------------------------------
extern MatStack r_modelview;
extern MatStack r_projection;

// Combined MVP = projection.top * modelview.top. Fills `out` in column-major.
void R_MVP(float out[16]);

#endif // GL_MAT4_H
