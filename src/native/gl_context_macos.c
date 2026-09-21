
#include "gl_context.h"

#include <OpenGL/OpenGL.h>

#include <stddef.h>

void *mr_gl_context_current(void) {
    return CGLGetCurrentContext();
}

void *mr_gl_context_create(void) {
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated,          kCGLPFAColorSize,
                                       (CGLPixelFormatAttribute)24, kCGLPFAAlphaSize,
                                       (CGLPixelFormatAttribute)8,  kCGLPFADepthSize,
                                       (CGLPixelFormatAttribute)24, kCGLPFAStencilSize,
                                       (CGLPixelFormatAttribute)8,  kCGLPFADoubleBuffer,
                                       (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pixel_format;
    GLint count;
    if (CGLChoosePixelFormat(attrs, &pixel_format, &count) != kCGLNoError) return NULL;

    CGLContextObj context = NULL;
    CGLError rc = CGLCreateContext(pixel_format, NULL, &context);
    CGLDestroyPixelFormat(pixel_format);
    return rc == kCGLNoError ? context : NULL;
}

void mr_gl_context_make_current(void *context) {
    CGLSetCurrentContext((CGLContextObj)context);
}

void mr_gl_context_destroy(void *context) {
    if (context) CGLDestroyContext((CGLContextObj)context);
}

unsigned mr_gl_context_window_framebuffer(void) {
    return 0;
}
