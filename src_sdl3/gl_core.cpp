/*
gl_core.cpp -- implementation of GLShader + DynamicVBO helpers.
*/

#include "gl_core.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// GLShader
// ---------------------------------------------------------------------------
static GLuint compile_stage(GLenum kind, const char *src, char *errbuf, size_t errlen)
{
	GLuint sh = glCreateShader(kind);
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);

	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		if (errbuf && errlen) {
			GLsizei n = 0;
			glGetShaderInfoLog(sh, (GLsizei)errlen - 1, &n, errbuf);
			errbuf[n < (GLsizei)errlen ? n : (GLsizei)errlen - 1] = 0;
		}
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

int GLShader_Build(GLShader *s, const char *vert_src, const char *frag_src, char *errbuf, size_t errlen)
{
	s->program = 0;

	GLuint vs = compile_stage(GL_VERTEX_SHADER, vert_src, errbuf, errlen);
	if (!vs) return 0;

	GLuint fs = compile_stage(GL_FRAGMENT_SHADER, frag_src, errbuf, errlen);
	if (!fs) {
		glDeleteShader(vs);
		return 0;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);

	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		if (errbuf && errlen) {
			GLsizei n = 0;
			glGetProgramInfoLog(prog, (GLsizei)errlen - 1, &n, errbuf);
			errbuf[n < (GLsizei)errlen ? n : (GLsizei)errlen - 1] = 0;
		}
		glDeleteProgram(prog);
		return 0;
	}

	s->program = prog;
	return 1;
}

void GLShader_Free(GLShader *s)
{
	if (s->program) {
		glDeleteProgram(s->program);
		s->program = 0;
	}
}

void GLShader_Use(const GLShader *s)
{
	glUseProgram(s->program);
}

GLint GLShader_Uniform(const GLShader *s, const char *name)
{
	return glGetUniformLocation(s->program, name);
}

// ---------------------------------------------------------------------------
// DynamicVBO
// ---------------------------------------------------------------------------
static void dvbo_zero_persistent (DynamicVBO *d)
{
	d->persistent = 0;
	d->mapped = NULL;
	d->segment_bytes = 0;
	d->current_segment = 0;
	d->segment_head = 0;
	for (int i = 0; i < DYNAMIC_VBO_PERSIST_SEGMENTS; ++i) d->fences[i] = 0;
}

void DynamicVBO_Init(DynamicVBO *d, GLsizei capacity_bytes)
{
	glGenVertexArrays(1, &d->vao);
	glGenBuffers(1, &d->vbo);
	d->capacity = capacity_bytes;
	d->head = 0;
	dvbo_zero_persistent(d);

	glBindVertexArray(d->vao);
	glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
	glBufferData(GL_ARRAY_BUFFER, capacity_bytes, NULL, GL_STREAM_DRAW);
	glBindVertexArray(0);
}

void DynamicVBO_InitPersistent(DynamicVBO *d, GLsizei capacity_bytes)
{
	glGenVertexArrays(1, &d->vao);
	glGenBuffers(1, &d->vbo);
	d->capacity = capacity_bytes;
	d->head = 0;
	dvbo_zero_persistent(d);

	const GLbitfield store_flags = GL_MAP_WRITE_BIT
	                             | GL_MAP_PERSISTENT_BIT
	                             | GL_MAP_COHERENT_BIT;

	glBindVertexArray(d->vao);
	glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
	glBufferStorage(GL_ARRAY_BUFFER, capacity_bytes, NULL, store_flags);

	d->mapped = glMapBufferRange(GL_ARRAY_BUFFER, 0, capacity_bytes, store_flags);
	if (!d->mapped) {
		// Shouldn't happen on a healthy GL 4.4 driver. Leave d->persistent = 0
		// so DynamicVBO_UploadStream falls back to the orphan path.
		// glBufferStorage made the buffer immutable, so we can't glBufferData
		// it anymore -- recreate.
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glDeleteBuffers(1, &d->vbo);
		glGenBuffers(1, &d->vbo);
		glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
		glBufferData(GL_ARRAY_BUFFER, capacity_bytes, NULL, GL_STREAM_DRAW);
		glBindVertexArray(0);
		return;
	}

	d->persistent = 1;
	d->segment_bytes = capacity_bytes / DYNAMIC_VBO_PERSIST_SEGMENTS;
	d->current_segment = 0;
	d->segment_head = 0;
	glBindVertexArray(0);
}

void DynamicVBO_Free(DynamicVBO *d)
{
	if (d->persistent && d->vbo) {
		glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
		glUnmapBuffer(GL_ARRAY_BUFFER);
		for (int i = 0; i < DYNAMIC_VBO_PERSIST_SEGMENTS; ++i) {
			if (d->fences[i]) { glDeleteSync(d->fences[i]); d->fences[i] = 0; }
		}
	}
	if (d->vbo) { glDeleteBuffers(1, &d->vbo); d->vbo = 0; }
	if (d->vao) { glDeleteVertexArrays(1, &d->vao); d->vao = 0; }
	d->capacity = 0;
	dvbo_zero_persistent(d);
}

void DynamicVBO_SetAttrib(DynamicVBO *d, GLuint index, GLint components, GLenum type,
                          GLboolean normalized, GLsizei stride, size_t offset)
{
	glBindVertexArray(d->vao);
	glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
	glEnableVertexAttribArray(index);
	glVertexAttribPointer(index, components, type, normalized, stride, (const void *)offset);
	glBindVertexArray(0);
}

void DynamicVBO_Upload(DynamicVBO *d, const void *data, GLsizei nbytes)
{
	glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
	// Orphan: discard old contents so the driver can give us a fresh block
	// without stalling on in-flight draws.
	glBufferData(GL_ARRAY_BUFFER, d->capacity, NULL, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, nbytes, data);
	d->head = 0; // streaming head irrelevant for the orphan path
}

// Ring-streaming upload. Two paths:
//   - Persistent (GL 4.4 + buffer_storage): writes straight into a permanently
//     mapped CPU pointer. Zero GL calls per upload. Uses a ring of N segments
//     with a fence per segment to avoid writing where the GPU is still reading.
//   - Orphan fallback (used when InitPersistent failed or caller used plain
//     Init): glMapBufferRange with UNSYNCHRONIZED + INVALIDATE_RANGE, wrapping
//     to 0 and orphaning the whole buffer when the ring fills.
GLsizei DynamicVBO_UploadStream(DynamicVBO *d, const void *data, GLsizei nbytes)
{
	if (nbytes <= 0) return 0;

	if (d->persistent) {
		// If this frame's segment filled up, just keep writing past its
		// boundary into the next segment's space. That's fine: the fence at
		// the end of this frame (placed by DynamicVBO_NextFrame) will cover
		// everything we wrote during the frame, whether we crossed a
		// boundary or not. What we can't do is spill past `capacity`.
		GLsizei seg_base = (GLsizei)d->current_segment * d->segment_bytes;
		GLsizei abs_pos  = seg_base + d->segment_head;

		if (abs_pos + nbytes > d->capacity) {
			// Extremely rare: one frame needed more than `capacity` bytes.
			// Fall back to orphaning.
			glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
			// Can't glBufferData on an immutable store; just wrap and hope
			// the GPU is done with the beginning. Wait on every fence.
			for (int i = 0; i < DYNAMIC_VBO_PERSIST_SEGMENTS; ++i) {
				if (d->fences[i]) {
					glClientWaitSync(d->fences[i], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
					glDeleteSync(d->fences[i]);
					d->fences[i] = 0;
				}
			}
			d->current_segment = 0;
			d->segment_head = 0;
			seg_base = 0;
			abs_pos = 0;
		}

		memcpy((char *)d->mapped + abs_pos, data, nbytes);
		d->segment_head += nbytes;
		return abs_pos;
	}

	// ---- orphan fallback ----
	glBindBuffer(GL_ARRAY_BUFFER, d->vbo);

	if (d->head + nbytes > d->capacity) {
		glBufferData(GL_ARRAY_BUFFER, d->capacity, NULL, GL_STREAM_DRAW);
		d->head = 0;
	}

	GLbitfield flags = GL_MAP_WRITE_BIT
	                 | GL_MAP_UNSYNCHRONIZED_BIT
	                 | GL_MAP_INVALIDATE_RANGE_BIT;
	void *p = glMapBufferRange(GL_ARRAY_BUFFER, d->head, nbytes, flags);
	GLsizei offset = d->head;
	if (p) {
		memcpy(p, data, nbytes);
		glUnmapBuffer(GL_ARRAY_BUFFER);
	} else {
		glBufferSubData(GL_ARRAY_BUFFER, d->head, nbytes, data);
	}
	d->head += nbytes;
	return offset;
}

void DynamicVBO_NextFrame(DynamicVBO *d)
{
	if (!d->persistent) return;

	// Place a fence covering everything written this frame. The fence will
	// be signaled once the GPU has consumed all draw commands submitted so
	// far -- which includes every glDrawArrays that read from this segment.
	if (d->fences[d->current_segment])
		glDeleteSync(d->fences[d->current_segment]);
	d->fences[d->current_segment] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

	// Advance to next segment and wait on its (older) fence if present.
	d->current_segment = (d->current_segment + 1) % DYNAMIC_VBO_PERSIST_SEGMENTS;
	d->segment_head = 0;

	GLsync f = d->fences[d->current_segment];
	if (f) {
		// Fast poll first: if already signaled, no wait.
		GLenum w = glClientWaitSync(f, 0, 0);
		if (w != GL_ALREADY_SIGNALED && w != GL_CONDITION_SATISFIED) {
			// Block until signaled; flush in case the driver hasn't submitted yet.
			glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
		}
		glDeleteSync(f);
		d->fences[d->current_segment] = 0;
	}
}

void DynamicVBO_Bind(const DynamicVBO *d)
{
	glBindVertexArray(d->vao);
}
