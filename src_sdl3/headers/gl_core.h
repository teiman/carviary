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
// Persistent-mapped streaming ring.
// The caller-visible capacity is split into PROF_PERSIST_SEGMENTS segments.
// Each frame writes into one segment, places a GPU fence at the end, and
// advances to the next segment. Before writing, if the segment's fence from
// its previous use isn't signaled, glClientWaitSync blocks until it is.
#define DYNAMIC_VBO_PERSIST_SEGMENTS 3

typedef struct DynamicVBO_s {
	GLuint   vao;
	GLuint   vbo;
	GLsizei  capacity;   // bytes
	GLsizei  head;       // next write offset (ring-streaming path, orphan fallback)

	// Persistent-mapped path (GL_ARB_buffer_storage, 4.4 core).
	int      persistent;        // 1 if initialized with DynamicVBO_InitPersistent
	void    *mapped;            // persistent CPU pointer; valid the entire buffer lifetime
	GLsizei  segment_bytes;     // capacity / DYNAMIC_VBO_PERSIST_SEGMENTS
	int      current_segment;   // which segment this frame writes into
	GLsizei  segment_head;      // write offset within the current segment
	GLsync   fences[DYNAMIC_VBO_PERSIST_SEGMENTS];
} DynamicVBO;

// Create a VAO+VBO pair sized for `capacity_bytes` of vertex data.
// Simple orphan-streaming path; good for small per-frame buffers.
// Vertex attribute layout must be configured separately via DynamicVBO_SetAttrib().
void DynamicVBO_Init(DynamicVBO *d, GLsizei capacity_bytes);

// Like DynamicVBO_Init but allocates an immutable buffer store with
// glBufferStorage and a persistent, coherent CPU mapping. The buffer lives
// as a ring of DYNAMIC_VBO_PERSIST_SEGMENTS segments; DynamicVBO_UploadStream
// writes with pure memcpy (no GL calls per upload) and places a fence at the
// end of each frame to ensure the CPU never overwrites memory the GPU is
// still reading. Use this for per-frame large streaming buffers.
// `capacity_bytes` is the TOTAL size; the usable per-frame budget is
// capacity_bytes / DYNAMIC_VBO_PERSIST_SEGMENTS.
void DynamicVBO_InitPersistent(DynamicVBO *d, GLsizei capacity_bytes);

void DynamicVBO_Free(DynamicVBO *d);

// Advance to the next segment of the persistent ring. Call once per frame
// (at the point where the previous frame's draws have been issued). No-op
// for non-persistent buffers. Places a fence at the end of the segment
// just finished.
void DynamicVBO_NextFrame(DynamicVBO *d);

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
