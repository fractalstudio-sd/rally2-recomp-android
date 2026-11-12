// GLES 3.2 stand-in for <GL/glew.h> (used when building with -DM3_GLES=ON, e.g. Android).
// The renderer is written against desktop GL 4.x; this header maps the few differences:
//   * shader sources: "#version 410/450 core" -> "#version 320 es" + default precisions (m3_glShaderSource)
//   * glClearDepth / glDrawBuffer, anisotropy token, glew init stubs
#pragma once
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <string>
#include <vector>
#include <cstring>

#define GLEW_OK 0
#define GLEW_STATIC
static bool glewExperimental = true;
static inline int glewInit() { return GLEW_OK; }

#define glClearDepth glClearDepthf
static inline void m3_glDrawBuffer(GLenum b) { glDrawBuffers(1, &b); }
#define glDrawBuffer m3_glDrawBuffer
#ifndef GL_BACK_LEFT
#define GL_BACK_LEFT GL_BACK
#endif

static inline void m3_glShaderSource(GLuint sh, GLsizei n, const GLchar *const *src, const GLint *len)
{
	std::string all;
	for (GLsizei i = 0; i < n; i++) {
		if (!src[i]) continue;
		all.append(src[i], (len && len[i] >= 0) ? (size_t)len[i] : strlen(src[i]));
	}
	size_t v = all.find("#version");
	std::string out;
	if (v != std::string::npos) {
		size_t e = all.find('\n', v);
		if (e == std::string::npos) e = all.size();
		// nothing may precede #version (Mali: "P0005 #version must be on the first line"), so drop everything before it
		out = std::string("#version 320 es\n"
			"precision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp usampler2D;\n"
			"precision highp isampler2D;\nprecision highp sampler2DArray;\n") + all.substr(e);
	} else out = all;
	const GLchar *p = out.c_str(); GLint l = (GLint)out.size();
	glShaderSource(sh, 1, &p, &l);
}
#define glShaderSource m3_glShaderSource
