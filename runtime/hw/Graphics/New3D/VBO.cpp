#include "VBO.h"
#include <cstring>
#include <cstdlib>

VBO::VBO()
{
	m_id		= 0;
	m_target	= 0;
	m_capacity	= 0;
	m_size		= 0;
}

void VBO::Create(GLenum target, GLenum usage, GLsizeiptr size, const void* data)
{
	glGenBuffers(1, &m_id);							// create a vbo
	glBindBuffer(target, m_id);						// activate vbo id to use
	if (!data) {
		void* z = calloc((size_t)size, 1);
		glBufferData(target, size, z, usage);
		free(z);
	}
	else if (!data) { void* z = calloc((size_t)size, 1); glBufferData(target, size, z, usage); free(z); } else glBufferData(target, size, data, usage);

	m_target	= target;
	m_capacity	= (int)size;
	m_size		= 0;

	Bind(false);		// unbind
}

void VBO::BufferSubData(GLintptr offset, GLsizeiptr size, const GLvoid* data)
{
	if (size <= 0) return;
	static GLuint stage = 0;
	if (!stage) glGenBuffers(1, &stage);
	glBindBuffer(GL_COPY_READ_BUFFER, stage);
	glBufferData(GL_COPY_READ_BUFFER, size, data, GL_STREAM_DRAW);
	glCopyBufferSubData(GL_COPY_READ_BUFFER, m_target, 0, offset, size);
	glBindBuffer(GL_COPY_READ_BUFFER, 0);
}

bool VBO::AppendData(GLsizeiptr size, const GLvoid* data)
{
	if (size == 0 || !data) {
		return true;	// nothing to do
	}

	if (m_size + size >= m_capacity) {
		return false;
	}

	BufferSubData(m_size, size, data);

	m_size += (int)size;

	return true;
}

void VBO::Reset()
{
	m_size = 0;
}

void VBO::Destroy()
{
	if (m_id) {
		glDeleteBuffers(1, &m_id);
		m_id		= 0;
		m_target	= 0;
		m_capacity	= 0;
		m_size		= 0;
	}
}

void VBO::Bind(bool enable)
{
	if (enable) {
		glBindBuffer(m_target, m_id);
	}
	else {
		glBindBuffer(m_target, 0);
	}
}

int VBO::GetSize() const
{
	return m_size;
}

int VBO::GetCapacity() const
{
	return m_capacity;
}
