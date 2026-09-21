#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES3/gl.h>

#include "gl_context.h"

static GLuint WINDOW_FRAMEBUFFER;

void mr_gl_context_set_window_framebuffer(unsigned framebuffer) {
    WINDOW_FRAMEBUFFER = (GLuint)framebuffer;
}

unsigned mr_gl_context_window_framebuffer(void) {
    return WINDOW_FRAMEBUFFER;
}

void *mr_gl_context_current(void) {
    return (__bridge void *)[EAGLContext currentContext];
}

void *mr_gl_context_create(void) {
    EAGLContext *context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    return (__bridge_retained void *)context;
}

void mr_gl_context_make_current(void *context) {
    [EAGLContext setCurrentContext:(__bridge EAGLContext *)context];
}

void mr_gl_context_destroy(void *context) {
    if (!context) return;
    EAGLContext *owned = (__bridge_transfer EAGLContext *)context;
    (void)owned;
}
