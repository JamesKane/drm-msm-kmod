/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 James Kane
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Offscreen GLES2 on the msm render node through GBM and EGL: print the
 * renderer, draw a triangle into a framebuffer object and check pixels.
 */
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>

#define	W	256
#define	H	256

static const char *vs =
    "attribute vec2 pos;\n"
    "attribute vec3 col;\n"
    "varying vec3 v;\n"
    "void main() { v = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs =
    "precision mediump float;\n"
    "varying vec3 v;\n"
    "void main() { gl_FragColor = vec4(v, 1.0); }\n";

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s;
	GLint ok;
	char log[512];

	s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		errx(1, "shader: %s", log);
	}
	return (s);
}

int
main(int argc, char **argv)
{
	static const GLfloat verts[] = {
		-0.8f, -0.8f,	1, 0, 0,
		 0.8f, -0.8f,	0, 1, 0,
		 0.0f,  0.8f,	0, 0, 1,
	};
	const EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	struct gbm_device *gbm;
	EGLDisplay dpy;
	EGLContext ctx;
	EGLint major, minor;
	GLuint prog, fbo, tex;
	GLubyte px[4];
	int fd;

	fd = open(argc > 1 ? argv[1] : "/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		err(1, "open");
	if ((gbm = gbm_create_device(fd)) == NULL)
		errx(1, "gbm_create_device");
	printf("gbm backend %s\n", gbm_device_get_backend_name(gbm));
	dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor))
		errx(1, "eglInitialize: %#x", eglGetError());
	printf("EGL %d.%d %s\n", major, minor,
	    eglQueryString(dpy, EGL_VENDOR));
	eglBindAPI(EGL_OPENGL_ES_API);
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
	if (ctx == EGL_NO_CONTEXT)
		errx(1, "eglCreateContext: %#x", eglGetError());
	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		errx(1, "eglMakeCurrent: %#x", eglGetError());
	printf("GL_RENDERER %s\nGL_VERSION %s\n", glGetString(GL_RENDERER),
	    glGetString(GL_VERSION));

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA,
	    GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
	    GL_FRAMEBUFFER_COMPLETE)
		errx(1, "framebuffer incomplete");

	prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "pos");
	glBindAttribLocation(prog, 1, "col");
	glLinkProgram(prog);
	glUseProgram(prog);

	glViewport(0, 0, W, H);
	glClearColor(0.25f, 0.25f, 0.25f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
	    verts);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
	    verts + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();

	glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("corner %d %d %d %d (expect about 64 64 64 255)\n",
	    px[0], px[1], px[2], px[3]);
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("centre %d %d %d %d (expect a mix of red, green, blue)\n",
	    px[0], px[1], px[2], px[3]);
	printf("glGetError %#x\n", glGetError());
	return (0);
}
