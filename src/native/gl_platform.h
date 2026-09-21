#ifndef MR_GL_PLATFORM_H
#define MR_GL_PLATFORM_H

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE

#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>

#else

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#define glBindFramebuffer glBindFramebufferEXT
#define glBindRenderbuffer glBindRenderbufferEXT
#define glGenFramebuffers glGenFramebuffersEXT
#define glGenRenderbuffers glGenRenderbuffersEXT
#define glDeleteFramebuffers glDeleteFramebuffersEXT
#define glDeleteRenderbuffers glDeleteRenderbuffersEXT
#define glFramebufferRenderbuffer glFramebufferRenderbufferEXT
#define glCheckFramebufferStatus glCheckFramebufferStatusEXT
#define glRenderbufferStorage glRenderbufferStorageEXT
#define glRenderbufferStorageMultisample glRenderbufferStorageMultisampleEXT
#define glGenerateMipmap glGenerateMipmapEXT
#define glFramebufferTexture2D glFramebufferTexture2DEXT
#define glBlitFramebuffer glBlitFramebufferEXT
#define glIsFramebuffer glIsFramebufferEXT
#define glIsRenderbuffer glIsRenderbufferEXT
#define glGetRenderbufferParameteriv glGetRenderbufferParameterivEXT
#define glGetFramebufferAttachmentParameteriv glGetFramebufferAttachmentParameterivEXT

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER GL_FRAMEBUFFER_EXT
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER GL_RENDERBUFFER_EXT
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER GL_READ_FRAMEBUFFER_EXT
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER GL_DRAW_FRAMEBUFFER_EXT
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING GL_READ_FRAMEBUFFER_BINDING_EXT
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0_EXT
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT GL_DEPTH_ATTACHMENT_EXT
#endif
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT GL_STENCIL_ATTACHMENT_EXT
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE GL_FRAMEBUFFER_COMPLETE_EXT
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 GL_DEPTH24_STENCIL8_EXT
#endif
#define MR_GL_TRANSLATE_GLSL 1

#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES GL_MAX_SAMPLES_EXT
#endif

static inline void glClearDepthf(GLfloat depth) {
    glClearDepth((GLdouble)depth);
}
static inline void glDepthRangef(GLfloat near_plane, GLfloat far_plane) {
    glDepthRange((GLdouble)near_plane, (GLdouble)far_plane);
}

// GLES3 maps PBO writes with glMapBufferRange. Desktop GL uses glMapBuffer, so
// this adapter implements the narrower operation required by the shim.
#define GL_MAP_WRITE_BIT 0x0002
static inline void *glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length,
                                     GLbitfield access) {
    (void)offset;
    (void)length;
    (void)access;
    return glMapBuffer(target, GL_WRITE_ONLY);
}

#define MR_GL_NO_INVALIDATE 1
static inline void glInvalidateFramebuffer(GLenum target, GLsizei count,
                                           const GLenum *attachments) {
    (void)target;
    (void)count;
    (void)attachments;
}

#ifndef GL_MAX_VARYING_VECTORS
#define MR_GL_VARYING_UNIT_IS_FLOAT 1
#endif

#endif

#endif
