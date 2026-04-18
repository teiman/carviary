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
void DynamicVBO_Init(DynamicVBO *d, GLsizei capacity_bytes)
{
	glGenVertexArrays(1, &d->vao);
	glGenBuffers(1, &d->vbo);
	d->capacity = capacity_bytes;
	d->head = 0;

	glBindVertexArray(d->vao);
	glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
	glBufferData(GL_ARRAY_BUFFER, capacity_bytes, NULL, GL_STREAM_DRAW);
	glBindVertexArray(0);
}

void DynamicVBO_Free(DynamicVBO *d)
{
	if (d->vbo) { glDeleteBuffers(1, &d->vbo); d->vbo = 0; }
	if (d->vao) { glDeleteVertexArrays(1, &d->vao); d->vao = 0; }
	d->capacity = 0;
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

// Ring-streaming upload. Writes `nbytes` at the current head, using
// GL_MAP_INVALIDATE_RANGE_BIT + GL_MAP_UNSYNCHRONIZED_BIT so the driver does
// NOT wait for in-flight draws that use earlier regions of the same buffer.
// On wrap, we invalidate the whole buffer (rare -- happens when the ring is
// full, which means we've consumed `capacity` bytes of vertex data in one
// frame, very unusual).
GLsizei DynamicVBO_UploadStream(DynamicVBO *d, const void *data, GLsizei nbytes)
{
	if (nbytes <= 0) return 0;
	glBindBuffer(GL_ARRAY_BUFFER, d->vbo);

	// If this write wouldn't fit before the end, wrap to 0 and orphan.
	if (d->head + nbytes > d->capacity) {
		glBufferData(GL_ARRAY_BUFFER, d->capacity, NULL, GL_STREAM_DRAW);
		d->head = 0;
	}

	// Map unsynchronized + invalidate-range: the driver gives us a writable
	// pointer without waiting. We only overwrite bytes the GPU isn't reading
	// because we always write past the previous write's end.
	GLbitfield flags = GL_MAP_WRITE_BIT
	                 | GL_MAP_UNSYNCHRONIZED_BIT
	                 | GL_MAP_INVALIDATE_RANGE_BIT;
	void *p = glMapBufferRange(GL_ARRAY_BUFFER, d->head, nbytes, flags);
	GLsizei offset = d->head;
	if (p) {
		memcpy(p, data, nbytes);
		glUnmapBuffer(GL_ARRAY_BUFFER);
	} else {
		// Shouldn't happen on a healthy driver; fall back to subdata.
		glBufferSubData(GL_ARRAY_BUFFER, d->head, nbytes, data);
	}
	d->head += nbytes;
	return offset;
}

void DynamicVBO_Bind(const DynamicVBO *d)
{
	glBindVertexArray(d->vao);
}
