/*
gl_core.h -- OpenGL 3.3 core helpers for the GL 1.x -> 3.3 migration.

Provides:
  - GLShader: compile/link of vertex+fragment GLSL, uniform location cache.
  - DynamicVBO: streaming vertex buffer (orphan-and-map pattern) for
    replacing immediate-mode glBegin/glEnd draws.

Portable. No Windows-specific types.
*/

#ifndef GL_CORE_H
#define GL_CORE_H

#include <glad/glad.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// GLShader
// ---------------------------------------------------------------------------
typedef struct GLShader_s {
	GLuint program;
} GLShader;

// Compile vertex+fragment shader sources and link. Returns 1 on success, 0 on failure.
// On failure, writes an error message (up to errlen-1 chars) into errbuf and program stays 0.
int  GLShader_Build(GLShader *s, const char *vert_src, const char *frag_src, char *errbuf, size_t errlen);
void GLShader_Free(GLShader *s);
void GLShader_Use(const GLShader *s);
GLint GLShader_Uniform(const GLShader *s, const char *name);

// ---------------------------------------------------------------------------
// DynamicVBO -- streaming VBO with orphan-on-fill.
//   Typical use: per-frame transient geometry that replaces glBegin/glEnd.
//   Allocate once, re-fill each frame with DynamicVBO_Upload(), then draw.
// ---------------------------------------------------------------------------
typedef struct DynamicVBO_s {
	GLuint vao;
	GLuint vbo;
	GLsizei capacity;   // bytes
	GLsizei head;       // next write offset (ring-streaming path)
} DynamicVBO;

// Create a VAO+VBO pair sized for `capacity_bytes` of vertex data.
// Vertex attribute layout must be configured separately via DynamicVBO_SetAttrib().
void DynamicVBO_Init(DynamicVBO *d, GLsizei capacity_bytes);
void DynamicVBO_Free(DynamicVBO *d);

// Describe one vertex attribute. Call once per attribute after Init, before drawing.
void DynamicVBO_SetAttrib(DynamicVBO *d, GLuint index, GLint components, GLenum type,
                          GLboolean normalized, GLsizei stride, size_t offset);

// Upload `nbytes` of vertex data, orphaning the old buffer first (streaming).
// Simple path: small buffers where orphan-per-upload cost is negligible.
void DynamicVBO_Upload(DynamicVBO *d, const void *data, GLsizei nbytes);

// Ring-streaming upload: writes `nbytes` into the buffer at a fresh offset
// without orphaning the whole buffer each call. The driver only stalls when
// the ring wraps back and the GPU hasn't finished that region yet -- which
// for a 2MB buffer holding ~100us of vertex data basically never happens
// at normal framerates. Returns the byte offset the data was written at,
// for use as `glDrawArrays(GL_TRIANGLES, first/vertex_stride, count)`.
// Use this for per-frame large world/warp uploads where the naive orphan
// path serializes the CPU thread behind driver uploads.
GLsizei DynamicVBO_UploadStream(DynamicVBO *d, const void *data, GLsizei nbytes);

// Bind VAO so a subsequent glDrawArrays/Elements uses this buffer.
void DynamicVBO_Bind(const DynamicVBO *d);

#endif // GL_CORE_H
