#ifndef MR_GL_CONTEXT_H
#define MR_GL_CONTEXT_H

void *mr_gl_context_current(void);
void *mr_gl_context_create(void);
void mr_gl_context_make_current(void *context);
void mr_gl_context_destroy(void *context);

unsigned mr_gl_context_window_framebuffer(void);

#endif
