
#define GL_SILENCE_DEPRECATION 1

#include "shim_gl.h"
#include "host_time.h"
#include "graphics_config.h"

#include "gl_context.h"
#include "gl_platform.h"

#include <TargetConditionals.h>
#include "platform_window.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif

// Arguments.

// cpu.h provides the shared ARM32 argument reader.
static float argf(mr_cpu *c, int i) {
    uint32_t bits = mr_guest_arg32(c, (unsigned)i);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Convert guest pointers to host pointers while preserving NULL.
static void *argp(mr_cpu *c, int i) {
    uint32_t a = mr_guest_arg32(c, (unsigned)i);
    if (!a) return NULL;
    if (!mr_mem_ok(c, a, 1)) {
        if (!c->fault) {
            c->fault = "invalid GL pointer";
            c->fault_addr = a;
        }
        c->halted = 1;
        return NULL;
    }
    return mr_mem(c, a);
}

static const char *args(mr_cpu *c, int i) {
    uint32_t a = mr_guest_arg32(c, (unsigned)i);
    return a ? mr_guest_cstr(c, a) : NULL;
}

#define RET(x) (c->r[0] = (uint32_t)(x))
#define A(i) mr_guest_arg32(c, (unsigned)i)
#define F(i) argf(c, i)
#define P(i) argp(c, i)

// GL context.

static void *GL_CTX;
static int GL_CTX_OWNED;
static int GL_READY;
static char STARTUP_CAPTURE_PATH[1024];
static int STARTUP_CAPTURE_REQUESTED;
static int STARTUP_CAPTURE_RESULT = -1;
static void capture_startup_frame(GLuint framebuffer, uint32_t width, uint32_t height);

void mr_gl_set_startup_capture(const char *path) {
    snprintf(STARTUP_CAPTURE_PATH, sizeof STARTUP_CAPTURE_PATH, "%s", path ? path : "");
}

int mr_gl_startup_capture_enabled(void) {
    return getenv("MR_DIAGNOSTICS") != NULL;
}

void mr_gl_request_startup_capture(void) {
    STARTUP_CAPTURE_REQUESTED = 1;
    STARTUP_CAPTURE_RESULT = -1;
}

int mr_gl_startup_capture_result(void) {
    return STARTUP_CAPTURE_RESULT;
}

static mr_gl_stats STAT;
static mr_gl_totals TOTAL;

const mr_gl_stats *mr_gl_get_stats(void) {
    return &STAT;
}
const mr_gl_totals *mr_gl_get_totals(void) {
    return &TOTAL;
}
void mr_gl_reset_stats(void) {
    memset(&STAT, 0, sizeof STAT);
}

static void count_state_skip(void) {
    STAT.state_skips++;
    TOTAL.state_skips++;
}

static void count_storage_reuse(void) {
    STAT.storage_reuses++;
    TOTAL.storage_reuses++;
}

static void count_pbo_fallback(void) {
    STAT.pbo_fallbacks++;
    TOTAL.pbo_fallbacks++;
}

static GLuint DEF_FBO, DEF_COLOR, DEF_DEPTH;
static GLuint RESOLVE_FBO, RESOLVE_COLOR;
static GLsizei SAMPLES;
static uint32_t DEF_W, DEF_H;
static uint32_t EGL_W, EGL_H;

#define UPLOAD_PBO_COUNT 3u
#define PBO_THRESHOLD (64u * 1024u)
#define GUEST_TEXTURE_UNITS 32u
#define CAP_CACHE_SLOTS 32u
#define TEXTURE_META_SLOTS 32768u

typedef struct {
    GLuint texture;
    GLenum target;
    GLint level;
    GLint internal_format;
    GLsizei width, height;
    GLenum format, type;
    GLsizei image_size;
    unsigned char state; // 0 empty, 1 occupied, 2 deleted.
    unsigned char compressed;
} texture_meta;

typedef struct {
    GLenum cap;
    unsigned char known;
    unsigned char enabled;
} cap_cache_entry;

static GLuint UPLOAD_PBOS[UPLOAD_PBO_COUNT];
static GLsync UPLOAD_PBO_FENCES[UPLOAD_PBO_COUNT];
static size_t UPLOAD_PBO_CAPACITY[UPLOAD_PBO_COUNT];
static unsigned UPLOAD_PBO_NEXT;
static GLuint ARRAY_BUFFER_BINDING;
static GLuint ELEMENT_ARRAY_BUFFER_BINDING;
static GLenum ACTIVE_TEXTURE = GL_TEXTURE0;
static GLuint TEXTURE_2D_BINDING[GUEST_TEXTURE_UNITS];
static GLuint TEXTURE_CUBE_BINDING[GUEST_TEXTURE_UNITS];
static GLuint CURRENT_PROGRAM;
static int CURRENT_PROGRAM_KNOWN;
static cap_cache_entry CAP_CACHE[CAP_CACHE_SLOTS];
static texture_meta TEXTURE_META[TEXTURE_META_SLOTS];
static GLint UNPACK_ALIGNMENT = 4;
static float ANISOTROPY = 1.0f;
static int USE_PBO;
static int USE_PBO_SYNC;
static int TRANSFER_READY;

static void clear_gl_errors(void) {
    for (unsigned i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {
    }
}

static int has_gl_extension(const char *wanted) {
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!extensions || !wanted || !*wanted || strchr(wanted, ' ')) return 0;
    size_t length = strlen(wanted);
    for (const char *p = extensions; (p = strstr(p, wanted)) != NULL; p += length) {
        if ((p == extensions || p[-1] == ' ') && (p[length] == '\0' || p[length] == ' ')) return 1;
    }
    return 0;
}

static unsigned texture_unit_index(void) {
    if (ACTIVE_TEXTURE < GL_TEXTURE0) return GUEST_TEXTURE_UNITS;
    unsigned unit = (unsigned)(ACTIVE_TEXTURE - GL_TEXTURE0);
    return unit < GUEST_TEXTURE_UNITS ? unit : GUEST_TEXTURE_UNITS;
}

static GLuint bound_texture(GLenum target) {
    unsigned unit = texture_unit_index();
    if (unit >= GUEST_TEXTURE_UNITS) return 0;
    if (target == GL_TEXTURE_2D) return TEXTURE_2D_BINDING[unit];
    if (target == GL_TEXTURE_CUBE_MAP ||
        (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z))
        return TEXTURE_CUBE_BINDING[unit];
    return 0;
}

static uint32_t texture_meta_hash(GLuint texture, GLenum target, GLint level) {
    uint32_t x = texture * 2654435761u;
    x ^= (uint32_t)target * 2246822519u;
    x ^= (uint32_t)level * 3266489917u;
    return x & (TEXTURE_META_SLOTS - 1u);
}

static texture_meta *texture_meta_slot(GLuint texture, GLenum target, GLint level, int create) {
    if (!texture) return NULL;
    uint32_t i = texture_meta_hash(texture, target, level);
    texture_meta *deleted = NULL;
    for (uint32_t n = 0; n < TEXTURE_META_SLOTS; n++, i = (i + 1u) & (TEXTURE_META_SLOTS - 1u)) {
        texture_meta *m = &TEXTURE_META[i];
        if (m->state == 1 && m->texture == texture && m->target == target && m->level == level)
            return m;
        if (m->state == 2 && !deleted) deleted = m;
        if (m->state == 0) {
            if (!create) return NULL;
            m = deleted ? deleted : m;
            memset(m, 0, sizeof *m);
            m->texture = texture;
            m->target = target;
            m->level = level;
            m->state = 1;
            return m;
        }
    }
    if (create && deleted) {
        memset(deleted, 0, sizeof *deleted);
        deleted->texture = texture;
        deleted->target = target;
        deleted->level = level;
        deleted->state = 1;
        return deleted;
    }
    return NULL;
}

static void texture_meta_remove(GLuint texture) {
    if (!texture) return;
    for (uint32_t i = 0; i < TEXTURE_META_SLOTS; i++)
        if (TEXTURE_META[i].state == 1 && TEXTURE_META[i].texture == texture)
            TEXTURE_META[i].state = 2;
}

static void texture_meta_store(GLuint texture, GLenum target, GLint level, GLint internal_format,
                               GLsizei width, GLsizei height, GLenum format, GLenum type,
                               GLsizei image_size, int compressed) {
    texture_meta *m = texture_meta_slot(texture, target, level, 1);
    if (!m) return;
    m->internal_format = internal_format;
    m->width = width;
    m->height = height;
    m->format = format;
    m->type = type;
    m->image_size = image_size;
    m->compressed = compressed != 0;
}

static int texture_meta_matches(GLuint texture, GLenum target, GLint level, GLint internal_format,
                                GLsizei width, GLsizei height, GLenum format, GLenum type,
                                GLsizei image_size, int compressed) {
    texture_meta *m = texture_meta_slot(texture, target, level, 0);
    return m && m->compressed == (compressed != 0) && m->internal_format == internal_format &&
           m->width == width && m->height == height && m->format == format && m->type == type &&
           (!compressed || m->image_size == image_size);
}

static cap_cache_entry *cap_cache_slot(GLenum cap) {
    uint32_t i = ((uint32_t)cap * 2654435761u) & (CAP_CACHE_SLOTS - 1u);
    for (uint32_t n = 0; n < CAP_CACHE_SLOTS; n++, i = (i + 1u) & (CAP_CACHE_SLOTS - 1u)) {
        cap_cache_entry *entry = &CAP_CACHE[i];
        if (!entry->known || entry->cap == cap) return entry;
    }
    return NULL;
}

static void discard_multisample(void) {
    static const GLenum all[] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
    glInvalidateFramebuffer(GL_READ_FRAMEBUFFER, 3, all);
}

static void init_transfer_features(void) {
    if (TRANSFER_READY) return;
    TRANSFER_READY = 1;

    USE_PBO = MR_GRAPHICS_USE_PBO;
    USE_PBO_SYNC = USE_PBO && has_gl_extension("GL_ARB_sync");
    if (USE_PBO) {
        clear_gl_errors();
        glGenBuffers((GLsizei)UPLOAD_PBO_COUNT, UPLOAD_PBOS);
        GLenum error = glGetError();
        for (unsigned i = 0; i < UPLOAD_PBO_COUNT; i++) {
            if (error == GL_NO_ERROR && UPLOAD_PBOS[i]) continue;
            glDeleteBuffers((GLsizei)UPLOAD_PBO_COUNT, UPLOAD_PBOS);
            memset(UPLOAD_PBOS, 0, sizeof UPLOAD_PBOS);
            USE_PBO = 0;
            break;
        }
    }

    ANISOTROPY = 1.0f;
    if (has_gl_extension("GL_EXT_texture_filter_anisotropic")) {
        GLfloat max = 1.0f;
        clear_gl_errors();
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max);
        if (glGetError() == GL_NO_ERROR && max > 1.0f)
            ANISOTROPY = MR_GRAPHICS_ANISOTROPY < max ? MR_GRAPHICS_ANISOTROPY : max;
    }
}

// Clamp the single configured sample count to the hardware limit.
static GLsizei choose_samples(void) {
    const GLint want = MR_GRAPHICS_MSAA_SAMPLES;
    if (want < 2) return 0;

    GLint max = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max);
    glGetError();
    if (max < 2) return 0;
    return (GLsizei)(want < max ? want : max);
}

static void rb_storage(GLenum format, uint32_t w, uint32_t h) {
    if (SAMPLES)
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, SAMPLES, format, (GLsizei)w, (GLsizei)h);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, format, (GLsizei)w, (GLsizei)h);
}

static GLenum build_target(uint32_t w, uint32_t h) {
    glBindRenderbuffer(GL_RENDERBUFFER, DEF_COLOR);
    rb_storage(GL_RGBA8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, DEF_DEPTH);
    rb_storage(GL_DEPTH24_STENCIL8, w, h);

    glBindFramebuffer(GL_FRAMEBUFFER, DEF_FBO);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, DEF_COLOR);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, DEF_DEPTH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, DEF_DEPTH);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (!SAMPLES || status != GL_FRAMEBUFFER_COMPLETE) return status;

    if (!RESOLVE_FBO) {
        glGenFramebuffers(1, &RESOLVE_FBO);
        glGenRenderbuffers(1, &RESOLVE_COLOR);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, RESOLVE_COLOR);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)w, (GLsizei)h);
    glBindFramebuffer(GL_FRAMEBUFFER, RESOLVE_FBO);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, RESOLVE_COLOR);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, DEF_FBO);
    return status;
}

static void ensure_target(uint32_t w, uint32_t h) {
    if (!GL_READY || !w || !h) return;
    if (DEF_FBO && DEF_W == w && DEF_H == h) return;

    if (!DEF_FBO) {
        glGenFramebuffers(1, &DEF_FBO);
        glGenRenderbuffers(1, &DEF_COLOR);
        glGenRenderbuffers(1, &DEF_DEPTH);
        SAMPLES = choose_samples();
    }
    if (build_target(w, h) != GL_FRAMEBUFFER_COMPLETE && SAMPLES) {
        fprintf(stderr, "[GL] %dx multisampling is unavailable; disabled\n", (int)SAMPLES);
        SAMPLES = 0;
        build_target(w, h);
    }
    DEF_W = w;
    DEF_H = h;
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
}

static GLuint resolved_target(void) {
    static const GLenum transient[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
    if (!SAMPLES) {
        glBindFramebuffer(GL_FRAMEBUFFER, DEF_FBO);
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, transient);
        return DEF_FBO;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, DEF_FBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, RESOLVE_FBO);
    glBlitFramebuffer(0, 0, (GLint)DEF_W, (GLint)DEF_H, 0, 0, (GLint)DEF_W, (GLint)DEF_H,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    static const GLenum resolved[] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT,
                                      GL_STENCIL_ATTACHMENT};
    glInvalidateFramebuffer(GL_READ_FRAMEBUFFER, 3, resolved);
    return RESOLVE_FBO;
}

static mr_gl_hud HUD;
static int SHOW_FPS;
static int DIAGNOSTICS;

void mr_gl_set_hud(const mr_gl_hud *hud) {
    if (!SHOW_FPS || !hud) {
        HUD = (mr_gl_hud){0};
        return;
    }
    HUD = *hud;
}

static void glyph_rows(char ch, unsigned char row[7]) {
    static const unsigned char digit[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},  {14, 17, 1, 2, 4, 8, 31},
        {30, 1, 1, 14, 1, 1, 30},     {2, 6, 10, 18, 31, 2, 2}, {31, 16, 16, 30, 1, 1, 30},
        {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},   {14, 17, 17, 14, 17, 17, 14},
        {14, 17, 17, 15, 1, 1, 14},
    };
    static const unsigned char upper[26][7] = {
        {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
        {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
        {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
        {7, 2, 2, 2, 2, 18, 12},      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
        {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
        {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
        {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
        {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 27, 17}, {17, 17, 10, 4, 10, 17, 17},
        {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},
    };
    static const struct {
        char ch;
        unsigned char rows[7];
    } SYMBOL[] = {
        {'/', {1, 2, 2, 4, 8, 8, 16}}, {'.', {0, 0, 0, 0, 0, 0, 4}},  {':', {0, 4, 0, 0, 0, 4, 0}},
        {'-', {0, 0, 0, 14, 0, 0, 0}}, {'+', {0, 4, 4, 31, 4, 4, 0}},
    };
    memset(row, 0, 7);
    if (ch >= '0' && ch <= '9') {
        memcpy(row, digit[ch - '0'], 7);
        return;
    }
    if (ch >= 'A' && ch <= 'Z') {
        memcpy(row, upper[ch - 'A'], 7);
        return;
    }
    for (size_t i = 0; i < sizeof SYMBOL / sizeof SYMBOL[0]; i++)
        if (SYMBOL[i].ch == ch) {
            memcpy(row, SYMBOL[i].rows, 7);
            return;
        }
}

#define HUD_LINES 5
#define HUD_COLS 24

static int hud_text(char line[HUD_LINES][HUD_COLS]) {
    double present = HUD.present_hz > 999.9 ? 999.9 : HUD.present_hz;
    double engine = HUD.engine_hz > 999.9 ? 999.9 : HUD.engine_hz;
    snprintf(line[0], HUD_COLS, "%.1f/%.1f FPS", present, engine);
    snprintf(line[1], HUD_COLS, "DISP %.0f HZ STEP %u MS", HUD.display_hz, HUD.step_ms);
    snprintf(line[2], HUD_COLS, "FRAME %.1f P99 %.1f", HUD.frame_ms, HUD.frame_p99_ms);
    snprintf(line[3], HUD_COLS, "WORK %.1f P99 %.1f", HUD.work_ms, HUD.work_p99_ms);
    snprintf(line[4], HUD_COLS, "JUDDER %.2f DROP %llu", HUD.judder_per_s,
             (unsigned long long)HUD.drops);
    return HUD_LINES;
}

static void draw_hud(uint32_t w, uint32_t h) {
    if (HUD.present_hz <= 0.0) return;

    char line[HUD_LINES][HUD_COLS];
    int lines = hud_text(line);
    int cols = 0;
    for (int i = 0; i < lines; i++) {
        int len = (int)strlen(line[i]);
        if (len > cols) cols = len;
    }
    int scale = w >= 1600 ? 3 : 2;
    int text_w = (cols * 6 - 1) * scale;
    int line_h = 8 * scale;
    int text_h = lines * line_h - scale;
    int pad = 2 * scale;
    int x0 = (int)w - text_w - pad * 2 - 3 * scale;
    int y0 = (int)h - text_h - pad * 2 - 3 * scale;
    if (x0 < 0 || y0 < 0) return;

    GLboolean old_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint old_box[4];
    GLfloat old_clear[4];
    GLboolean old_mask[4];
    glGetIntegerv(GL_SCISSOR_BOX, old_box);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
    glGetBooleanv(GL_COLOR_WRITEMASK, old_mask);

    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.02f, 0.02f, 0.02f, 1.0f);
    glScissor(x0, y0, text_w + pad * 2, text_h + pad * 2);
    glClear(GL_COLOR_BUFFER_BIT);

    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    for (int i = 0; i < lines; i++) {
        // GL starts at the bottom-left, while image rows start at the top.
        int base_y = y0 + pad + (lines - 1 - i) * line_h;
        for (int c = 0; line[i][c]; c++) {
            unsigned char rows[7];
            glyph_rows(line[i][c], rows);
            for (int y = 0; y < 7; y++) {
                for (int x = 0; x < 5; x++) {
                    if (!(rows[y] & (1u << (4 - x)))) continue;
                    glScissor(x0 + pad + (c * 6 + x) * scale, base_y + (6 - y) * scale, scale,
                              scale);
                    glClear(GL_COLOR_BUFFER_BIT);
                }
            }
        }
    }

    glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    glColorMask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    glScissor(old_box[0], old_box[1], old_box[2], old_box[3]);
    if (!old_scissor) glDisable(GL_SCISSOR_TEST);
}

void mr_gl_blit_to_window(uint32_t w, uint32_t h) {
    if (!GL_READY || !DEF_FBO || !w || !h) return;

    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean dither = glIsEnabled(GL_DITHER);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    if (dither) glDisable(GL_DITHER);

    const GLuint window_fbo = mr_gl_context_window_framebuffer();

    mr_win_fit fit = mr_win_fit_surface((double)w, (double)h, (double)DEF_W, (double)DEF_H);
    GLint dst_x = (GLint)lround(fit.x), dst_y = (GLint)lround(fit.y);
    GLint dst_w = (GLint)lround(fit.w), dst_h = (GLint)lround(fit.h);

    if (dst_w < (GLint)w || dst_h < (GLint)h) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, window_fbo);
        GLfloat old_clear[4];
        GLboolean old_mask[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
        glGetBooleanv(GL_COLOR_WRITEMASK, old_mask);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
        glColorMask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    }

    const int one_to_one =
        dst_x == 0 && dst_y == 0 && dst_w == (GLint)DEF_W && dst_h == (GLint)DEF_H;
    if (SAMPLES && one_to_one && !STARTUP_CAPTURE_REQUESTED) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, DEF_FBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, window_fbo);
        // GLES3 multisample resolves require equal sizes and GL_NEAREST.
        glBlitFramebuffer(0, 0, (GLint)DEF_W, (GLint)DEF_H, 0, 0, (GLint)DEF_W, (GLint)DEF_H,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        discard_multisample();
    } else {
        GLuint source = resolved_target();
        if (STARTUP_CAPTURE_REQUESTED) capture_startup_frame(source, DEF_W, DEF_H);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, window_fbo);
        glBlitFramebuffer(0, 0, (GLint)DEF_W, (GLint)DEF_H, dst_x, dst_y, dst_x + dst_w,
                          dst_y + dst_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    draw_hud(w, h);

    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (dither) glEnable(GL_DITHER);
}

void mr_gl_restore_target(void) {
    if (!GL_READY || !DEF_FBO) return;
    glBindFramebuffer(GL_FRAMEBUFFER, DEF_FBO);
}

int mr_gl_target_ok(void) {
    if (!GL_READY || !DEF_FBO) return 0;
    glBindFramebuffer(GL_FRAMEBUFFER, DEF_FBO);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void put32be(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static int png_chunk(FILE *f, const char *tag, const unsigned char *data, uint32_t n) {
    unsigned char hdr[8];
    put32be(hdr, n);
    memcpy(hdr + 4, tag, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (n && fwrite(data, 1, n, f) != n) return -1;
    uLong crc = crc32(0, (const Bytef *)tag, 4);
    if (n) crc = crc32(crc, (const Bytef *)data, n);
    unsigned char c[4];
    put32be(c, (uint32_t)crc);
    return fwrite(c, 1, 4, f) == 4 ? 0 : -1;
}

static int write_rgb_png(const char *path, const unsigned char *px, uint32_t w, uint32_t h) {
    size_t stride = (size_t)w * 3;
    size_t raw_n = (stride + 1) * h;
    unsigned char *raw = malloc(raw_n);
    if (!raw) return -1;
    for (uint32_t y = 0; y < h; y++) {
        unsigned char *dst = raw + (size_t)y * (stride + 1);
        *dst = 0;
        memcpy(dst + 1, px + (size_t)(h - 1 - y) * stride, stride);
    }

    uLongf comp_n = compressBound((uLong)raw_n);
    unsigned char *comp = malloc(comp_n);
    if (!comp || compress2(comp, &comp_n, raw, (uLong)raw_n, 6) != Z_OK) {
        free(raw);
        free(comp);
        return -1;
    }
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(comp);
        return -1;
    }
    static const unsigned char SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    int rc = fwrite(SIG, 1, 8, f) == 8 ? 0 : -1;
    unsigned char ihdr[13];
    put32be(ihdr, w);
    put32be(ihdr + 4, h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0; // 8-bit RGB.
    if (!rc) rc = png_chunk(f, "IHDR", ihdr, sizeof ihdr);
    if (!rc) rc = png_chunk(f, "IDAT", comp, (uint32_t)comp_n);
    if (!rc) rc = png_chunk(f, "IEND", NULL, 0);
    if (fclose(f) != 0 && !rc) rc = -1;
    free(comp);
    return rc;
}

static unsigned char *read_framebuffer_rgb(GLuint framebuffer, uint32_t w, uint32_t h) {
    size_t pixels = (size_t)w * h;
    if (!pixels || pixels > SIZE_MAX / 4u) return NULL;
    unsigned char *data = calloc(pixels, 4u);
    if (!data) return NULL;

    GLint pack_alignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);

    for (size_t i = 0; i < pixels; i++) {
        data[i * 3u] = data[i * 4u];
        data[i * 3u + 1u] = data[i * 4u + 1u];
        data[i * 3u + 2u] = data[i * 4u + 2u];
    }
    return data;
}

static void capture_startup_frame(GLuint framebuffer, uint32_t w, uint32_t h) {
    STARTUP_CAPTURE_REQUESTED = 0;
    size_t pixels = (size_t)w * h;
    if (!pixels || pixels > SIZE_MAX / 3u) {
        STARTUP_CAPTURE_RESULT = -2;
        return;
    }

    unsigned char *rgb = read_framebuffer_rgb(framebuffer, w, h);
    if (!rgb) {
        STARTUP_CAPTURE_RESULT = -2;
        return;
    }

    size_t visible = 0;
    for (size_t i = 0; i < pixels; i++) {
        const unsigned char *color = rgb + i * 3u;
        if (color[0] > 4u || color[1] > 4u || color[2] > 4u) visible++;
    }
    size_t minimum = pixels / 1000u;
    if (!minimum) minimum = 1;
    int saved = !STARTUP_CAPTURE_PATH[0] || write_rgb_png(STARTUP_CAPTURE_PATH, rgb, w, h) == 0;
    STARTUP_CAPTURE_RESULT = !saved ? -2 : visible >= minimum;
    printf("  *** startup frame check: %zu/%zu visible pixels, PNG %s ***\n", visible, pixels,
           STARTUP_CAPTURE_PATH[0] ? (saved ? "saved" : "FAILED") : "not requested");
    free(rgb);
}

int mr_gl_save_png(const char *path) {
    if (!GL_READY || !DEF_FBO || !path) return -1;
    uint32_t w = DEF_W, h = DEF_H;
    if (!w || !h) return -1;

    GLuint framebuffer = resolved_target();
    unsigned char *rgb = read_framebuffer_rgb(framebuffer, w, h);
    if (!rgb) return -1;
    glBindFramebuffer(GL_FRAMEBUFFER, DEF_FBO);
    int result = write_rgb_png(path, rgb, w, h);
    free(rgb);
    return result;
}

static int finish_gl_init(void) {
    memset(TEXTURE_2D_BINDING, 0, sizeof TEXTURE_2D_BINDING);
    memset(TEXTURE_CUBE_BINDING, 0, sizeof TEXTURE_CUBE_BINDING);
    memset(CAP_CACHE, 0, sizeof CAP_CACHE);
    memset(TEXTURE_META, 0, sizeof TEXTURE_META);
    memset(&TOTAL, 0, sizeof TOTAL);
    ARRAY_BUFFER_BINDING = ELEMENT_ARRAY_BUFFER_BINDING = 0;
    ACTIVE_TEXTURE = GL_TEXTURE0;
    CURRENT_PROGRAM = 0;
    CURRENT_PROGRAM_KNOWN = 0;
    UNPACK_ALIGNMENT = 4;
    STARTUP_CAPTURE_REQUESTED = 0;
    STARTUP_CAPTURE_RESULT = -1;
    GL_READY = 1;
    init_transfer_features();
    ensure_target(EGL_W, EGL_H);
    return 0;
}

int mr_gl_init(uint32_t w, uint32_t h) {
    if (!w || !h) return -1;
    const char *show_fps = getenv("MR_SHOW_FPS");
    SHOW_FPS = show_fps && strcmp(show_fps, "0") != 0;
    DIAGNOSTICS = getenv("MR_DIAGNOSTICS") != NULL;
    EGL_W = w;
    EGL_H = h;
    if (GL_READY) {
        ensure_target(w, h);
        return 0;
    }

    if ((GL_CTX = mr_gl_context_current()) != NULL) {
        GL_CTX_OWNED = 0;
        return finish_gl_init();
    }

    if ((GL_CTX = mr_gl_context_create()) == NULL) return -1;
    GL_CTX_OWNED = 1;
    mr_gl_context_make_current(GL_CTX);
    return finish_gl_init();
}

void mr_gl_shutdown(void) {
    if (!GL_READY) return;
    if (GL_CTX) mr_gl_context_make_current(GL_CTX);
    for (unsigned i = 0; i < UPLOAD_PBO_COUNT; i++) {
        if (UPLOAD_PBO_FENCES[i]) {
            glDeleteSync(UPLOAD_PBO_FENCES[i]);
            UPLOAD_PBO_FENCES[i] = NULL;
        }
    }
    if (UPLOAD_PBOS[0] || UPLOAD_PBOS[1] || UPLOAD_PBOS[2])
        glDeleteBuffers((GLsizei)UPLOAD_PBO_COUNT, UPLOAD_PBOS);
    if (DEF_FBO) glDeleteFramebuffers(1, &DEF_FBO);
    if (RESOLVE_FBO) glDeleteFramebuffers(1, &RESOLVE_FBO);
    if (DEF_COLOR) glDeleteRenderbuffers(1, &DEF_COLOR);
    if (DEF_DEPTH) glDeleteRenderbuffers(1, &DEF_DEPTH);
    if (RESOLVE_COLOR) glDeleteRenderbuffers(1, &RESOLVE_COLOR);

    memset(UPLOAD_PBOS, 0, sizeof UPLOAD_PBOS);
    memset(UPLOAD_PBO_CAPACITY, 0, sizeof UPLOAD_PBO_CAPACITY);
    memset(TEXTURE_META, 0, sizeof TEXTURE_META);
    UPLOAD_PBO_NEXT = 0;
    DEF_FBO = DEF_COLOR = DEF_DEPTH = RESOLVE_FBO = RESOLVE_COLOR = 0;
    DEF_W = DEF_H = 0;
    SAMPLES = 0;
    GL_READY = TRANSFER_READY = USE_PBO = USE_PBO_SYNC = 0;
    if (GL_CTX_OWNED && GL_CTX) {
        mr_gl_context_make_current(NULL);
        mr_gl_context_destroy(GL_CTX);
    }
    GL_CTX = NULL;
    GL_CTX_OWNED = 0;
}

const char *mr_gl_renderer(void) {
    if (!GL_READY) return "";
    return (const char *)glGetString(GL_RENDERER);
}

int mr_gl_samples(void) {
    return (int)SAMPLES;
}
float mr_gl_anisotropy(void) {
    return ANISOTROPY;
}
int mr_gl_pbo_enabled(void) {
    return USE_PBO;
}

// Shader translation.

static char *translate_shader(const char *src, int with_prolog) {
#ifndef MR_GL_TRANSLATE_GLSL
    (void)with_prolog;
    size_t len = strlen(src);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, src, len + 1);
    return copy;
#else
    static const char PROLOG[] = "#version 120\n"
                                 "#define lowp\n#define mediump\n#define highp\n";

    size_t n = strlen(src);
    char *out = malloc(sizeof(PROLOG) + n + 1);
    if (!out) return NULL;
    out[0] = 0;
    if (with_prolog) strcpy(out, PROLOG);
    char *w = out + strlen(out);

    const char *p = src;
    while (*p) {
        // Drop guest #version directives because the host supplies one.
        if (strncmp(p, "#version", 8) == 0) {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        // Desktop GLSL does not accept precision declarations.
        if (strncmp(p, "precision", 9) == 0 && (p[9] == ' ' || p[9] == '\t')) {
            while (*p && *p != ';')
                p++;
            if (*p == ';') p++;
            continue;
        }
        *w++ = *p++;
    }
    *w = 0;
    return out;
#endif
}

#define EGL_OK_HANDLE 0x1000
static void t_eglGetDisplay(mr_cpu *c) {
    RET(EGL_OK_HANDLE);
}
static void t_eglInitialize(mr_cpu *c) {
    if (mr_gl_init(EGL_W, EGL_H) != 0) {
        RET(0);
        return;
    }
    // Major and minor output parameters.
    uint32_t maj = A(1), min = A(2);
    if (maj) mr_st32(c, maj, 1);
    if (min) mr_st32(c, min, 4);
    RET(1);
}
static void t_eglTerminate(mr_cpu *c) {
    RET(1);
}
static void t_eglGetError(mr_cpu *c) {
    RET(0x3000);
} // EGL_SUCCESS
static void t_eglChooseConfig(mr_cpu *c) {
    uint32_t configs = A(2), size = A(3), num = A(4);
    if (configs && size) mr_st32(c, configs, EGL_OK_HANDLE + 1);
    if (num) mr_st32(c, num, 1);
    RET(1);
}
static void t_eglGetConfigAttrib(mr_cpu *c) {
    uint32_t value = A(3);
    if (value) mr_st32(c, value, 0);
    RET(1);
}
static void t_eglCreateWindowSurface(mr_cpu *c) {
    RET(EGL_OK_HANDLE + 2);
}
static void t_eglCreateContext(mr_cpu *c) {
    RET(EGL_OK_HANDLE + 3);
}
static void t_eglDestroySurface(mr_cpu *c) {
    RET(1);
}
static void t_eglDestroyContext(mr_cpu *c) {
    RET(1);
}
static void t_eglMakeCurrent(mr_cpu *c) {
    if (GL_CTX) mr_gl_context_make_current(GL_CTX);
    RET(1);
}
static void t_eglQuerySurface(mr_cpu *c) {
    uint32_t attr = A(2), value = A(3);
    if (value)
        mr_st32(c, value,
                (attr == 0x3057) ? EGL_W : // EGL_WIDTH
                    (attr == 0x3056) ? EGL_H
                                     : 0); // EGL_HEIGHT
    RET(1);
}
static void t_eglSwapBuffers(mr_cpu *c) {
    RET(1);
}
static void t_eglGetProcAddress(mr_cpu *c) {
    RET(0);
}

// GL thunks.

#define GL0(nm)                                                                                    \
    static void t_##nm(mr_cpu *c) {                                                                \
        (void)c;                                                                                   \
        nm();                                                                                      \
    }
#define GL1i(nm)                                                                                   \
    static void t_##nm(mr_cpu *c) {                                                                \
        nm(A(0));                                                                                  \
    }
#define GL2i(nm)                                                                                   \
    static void t_##nm(mr_cpu *c) {                                                                \
        nm(A(0), A(1));                                                                            \
    }
#define GL3i(nm)                                                                                   \
    static void t_##nm(mr_cpu *c) {                                                                \
        nm(A(0), A(1), A(2));                                                                      \
    }
#define GL4i(nm)                                                                                   \
    static void t_##nm(mr_cpu *c) {                                                                \
        nm(A(0), A(1), A(2), A(3));                                                                \
    }
#define GL1f(nm)                                                                                   \
    static void t_##nm(mr_cpu *c) {                                                                \
        nm(F(0));                                                                                  \
    }
#define GL2f(nm)                                                                                   \
    static void t_##nm(mr_cpu *c) {                                                                \
        nm(F(0), F(1));                                                                            \
    }
#define GL4f(nm)                                                                                   \
    static void t_##nm(mr_cpu *c) {                                                                \
        nm(F(0), F(1), F(2), F(3));                                                                \
    }
#define GL1i_R(nm)                                                                                 \
    static void t_##nm(mr_cpu *c) {                                                                \
        RET(nm(A(0)));                                                                             \
    }
#define GL0_R(nm)                                                                                  \
    static void t_##nm(mr_cpu *c) {                                                                \
        (void)c;                                                                                   \
        RET(nm());                                                                                 \
    }

static int cacheable_cap(GLenum cap) {
    switch (cap) {
    case GL_BLEND:
    case GL_CULL_FACE:
    case GL_DEPTH_TEST:
    case GL_DITHER:
    case GL_POLYGON_OFFSET_FILL:
    case GL_SAMPLE_ALPHA_TO_COVERAGE:
    case GL_SAMPLE_COVERAGE:
    case GL_SCISSOR_TEST:
    case GL_STENCIL_TEST:
        return 1;
    default:
        return 0;
    }
}

static void set_cap(mr_cpu *c, int enabled) {
    GLenum cap = A(0);
    cap_cache_entry *entry = cacheable_cap(cap) ? cap_cache_slot(cap) : NULL;
    if (entry && entry->known && entry->enabled == (enabled != 0)) {
        count_state_skip();
        return;
    }
    if (enabled)
        glEnable(cap);
    else
        glDisable(cap);
    if (entry) {
        entry->cap = cap;
        entry->known = 1;
        entry->enabled = enabled != 0;
    }
}
static void t_glEnable(mr_cpu *c) {
    set_cap(c, 1);
}
static void t_glDisable(mr_cpu *c) {
    set_cap(c, 0);
}
static void t_glActiveTexture(mr_cpu *c) {
    GLenum texture = A(0);
    int cacheable = texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + GUEST_TEXTURE_UNITS;
    if (cacheable && texture == ACTIVE_TEXTURE) {
        count_state_skip();
        return;
    }
    glActiveTexture(texture);
    if (cacheable) ACTIVE_TEXTURE = texture;
}
GL1i(glCullFace) GL1i(glFrontFace) GL1i(glDepthFunc) GL1i(glDepthMask) GL1i(glStencilMask)
    GL1i(glBlendEquation) GL2i(glBlendFunc) GL2i(glHint) GL2i(glStencilMaskSeparate)
        GL2i(glBlendEquationSeparate) GL3i(glStencilFunc) GL3i(glStencilOp)

            static void t_glPixelStorei(mr_cpu *c) {
    GLenum pname = A(0);
    GLint value = (GLint)A(1);
    if (pname == GL_UNPACK_ALIGNMENT && value == UNPACK_ALIGNMENT) {
        count_state_skip();
        return;
    }
    glPixelStorei(pname, value);
    if (pname == GL_UNPACK_ALIGNMENT && (value == 1 || value == 2 || value == 4 || value == 8))
        UNPACK_ALIGNMENT = value;
}

static void t_glClear(mr_cpu *c) {
    STAT.clear++;
    glClear(A(0));
}
static void t_glDrawArrays(mr_cpu *c) {
    STAT.draw++;
    STAT.vertices += A(2);
    glDrawArrays(A(0), A(1), A(2));
}
GL4i(glColorMask) GL4i(glViewport) GL4i(glScissor) GL4i(glBlendFuncSeparate)
    GL4i(glStencilFuncSeparate) GL4i(glStencilOpSeparate) GL4f(glClearColor) GL4f(glBlendColor)
        GL1f(glLineWidth) GL2f(glPolygonOffset) GL2f(glSampleCoverage) GL0(glFinish) GL0(glFlush)
            GL0_R(glGetError) GL1i(glClearStencil) GL1i_R(glIsEnabled) GL1i_R(glIsBuffer)
                GL1i_R(glIsTexture) GL1i_R(glIsProgram) GL1i_R(glIsShader)

    // Desktop GL represents depth values as double.
    static void t_glClearDepthf(mr_cpu *c) {
    glClearDepthf((GLfloat)F(0));
}
static void t_glDepthRangef(mr_cpu *c) {
    glDepthRangef((GLfloat)F(0), (GLfloat)F(1));
}

// Buffers. Pointer arguments depend on the current bindings, which are cached
// locally to avoid synchronizing with the driver on the draw path.
static void t_glBindBuffer(mr_cpu *c) {
    GLenum target = A(0);
    GLuint buffer = A(1);
    GLuint *binding = target == GL_ARRAY_BUFFER           ? &ARRAY_BUFFER_BINDING
                      : target == GL_ELEMENT_ARRAY_BUFFER ? &ELEMENT_ARRAY_BUFFER_BINDING
                                                          : NULL;
    if (binding && *binding == buffer) {
        count_state_skip();
        return;
    }
    glBindBuffer(target, buffer);
    if (binding) *binding = buffer;
}

GL1i(glGenerateMipmap) static void t_glGenBuffers(mr_cpu *c) {
    glGenBuffers(A(0), P(1));
}
static void t_glDeleteBuffers(mr_cpu *c) {
    GLsizei count = (GLsizei)A(0);
    const GLuint *buffers = (const GLuint *)P(1);
    if (count > 0 && !buffers) return;
    for (GLsizei i = 0; i < count; i++) {
        if (buffers[i] == ARRAY_BUFFER_BINDING) ARRAY_BUFFER_BINDING = 0;
        if (buffers[i] == ELEMENT_ARRAY_BUFFER_BINDING) ELEMENT_ARRAY_BUFFER_BINDING = 0;
    }
    glDeleteBuffers(count, buffers);
}
static void t_glBufferData(mr_cpu *c) {
    GLsizeiptr size = (GLsizeiptr)A(1);
    const void *source = P(2);
    if (c->halted) return;
    glBufferData(A(0), size, source, A(3));
}
static void t_glBufferSubData(mr_cpu *c) {
    glBufferSubData(A(0), A(1), A(2), P(3));
}
static void t_glGetBufferParameteriv(mr_cpu *c) {
    glGetBufferParameteriv(A(0), A(1), P(2));
}

// Textures.
static void t_glBindTexture(mr_cpu *c) {
    GLenum target = A(0);
    GLuint texture = A(1);
    unsigned unit = texture_unit_index();
    GLuint *binding = NULL;
    if (unit < GUEST_TEXTURE_UNITS) {
        if (target == GL_TEXTURE_2D)
            binding = &TEXTURE_2D_BINDING[unit];
        else if (target == GL_TEXTURE_CUBE_MAP)
            binding = &TEXTURE_CUBE_BINDING[unit];
    }
    if (binding && *binding == texture) {
        count_state_skip();
        return;
    }
    glBindTexture(target, texture);
    if (binding) *binding = texture;
}
static void t_glGenTextures(mr_cpu *c) {
    glGenTextures(A(0), P(1));
}
static void t_glDeleteTextures(mr_cpu *c) {
    GLsizei count = (GLsizei)A(0);
    const GLuint *textures = (const GLuint *)P(1);
    if (count > 0 && !textures) return;
    for (GLsizei i = 0; i < count; i++) {
        GLuint texture = textures[i];
        texture_meta_remove(texture);
        for (unsigned unit = 0; unit < GUEST_TEXTURE_UNITS; unit++) {
            if (TEXTURE_2D_BINDING[unit] == texture) TEXTURE_2D_BINDING[unit] = 0;
            if (TEXTURE_CUBE_BINDING[unit] == texture) TEXTURE_CUBE_BINDING[unit] = 0;
        }
    }
    glDeleteTextures(count, textures);
}
static int mipmapped_filter(GLint value) {
    return value == GL_NEAREST_MIPMAP_NEAREST || value == GL_LINEAR_MIPMAP_NEAREST ||
           value == GL_NEAREST_MIPMAP_LINEAR || value == GL_LINEAR_MIPMAP_LINEAR;
}

static void apply_anisotropy(GLenum target, GLenum pname, GLint value) {
    if (ANISOTROPY > 1.0f && target == GL_TEXTURE_2D && pname == GL_TEXTURE_MIN_FILTER &&
        mipmapped_filter(value))
        glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, ANISOTROPY);
}

static void t_glTexParameteri(mr_cpu *c) {
    GLenum target = A(0), pname = A(1);
    GLint value = (GLint)A(2);
    glTexParameteri(target, pname, value);
    apply_anisotropy(target, pname, value);
}
static void t_glTexParameterf(mr_cpu *c) {
    GLenum target = A(0), pname = A(1);
    GLfloat value = F(2);
    glTexParameterf(target, pname, value);
    apply_anisotropy(target, pname, (GLint)value);
}
static void t_glTexParameteriv(mr_cpu *c) {
    GLenum target = A(0), pname = A(1);
    uint32_t guest = A(2);
    if (!guest || !mr_mem_ok(c, guest, sizeof(GLint))) return;
    const GLint *value = mr_mem(c, guest);
    glTexParameteriv(target, pname, value);
    GLint first;
    memcpy(&first, value, sizeof first);
    apply_anisotropy(target, pname, first);
}
static void t_glTexParameterfv(mr_cpu *c) {
    GLenum target = A(0), pname = A(1);
    uint32_t guest = A(2);
    if (!guest || !mr_mem_ok(c, guest, sizeof(GLfloat))) return;
    const GLfloat *value = mr_mem(c, guest);
    glTexParameterfv(target, pname, value);
    GLfloat first;
    memcpy(&first, value, sizeof first);
    apply_anisotropy(target, pname, (GLint)first);
}
static void t_glGetTexParameteriv(mr_cpu *c) {
    glGetTexParameteriv(A(0), A(1), P(2));
}
static void t_glGetTexParameterfv(mr_cpu *c) {
    glGetTexParameterfv(A(0), A(1), P(2));
}
static size_t pixel_size(GLenum format, GLenum type, GLsizei width, GLsizei height) {
    if (width <= 0 || height <= 0) return 0;

    size_t bytes_per_pixel = 0;
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        bytes_per_pixel = 2;
        break;
#ifdef GL_UNSIGNED_INT_8_8_8_8_REV
    case GL_UNSIGNED_INT_8_8_8_8_REV:
        bytes_per_pixel = 4;
        break;
#endif
    case GL_UNSIGNED_BYTE:
    case GL_UNSIGNED_SHORT:
    case GL_FLOAT: {
        size_t components = format == GL_ALPHA || format == GL_LUMINANCE ? 1u
                            : format == GL_LUMINANCE_ALPHA               ? 2u
                            : format == GL_RGB                           ? 3u
                            : (format == GL_RGBA || format == GL_BGRA)   ? 4u
                                                                         : 0u;
        size_t component_size = type == GL_UNSIGNED_BYTE ? 1u : type == GL_UNSIGNED_SHORT ? 2u : 4u;
        bytes_per_pixel = components * component_size;
        break;
    }
    default:
        return 0;
    }
    if (!bytes_per_pixel || (size_t)width > SIZE_MAX / bytes_per_pixel) return 0;

    size_t row = (size_t)width * bytes_per_pixel;
    size_t alignment = (size_t)UNPACK_ALIGNMENT;
    size_t stride = (row + alignment - 1u) & ~(alignment - 1u);
    if ((size_t)(height - 1) > (SIZE_MAX - row) / stride) return 0;
    return stride * (size_t)(height - 1) + row;
}

static size_t pbo_capacity(size_t bytes) {
    const size_t granularity = 1024u * 1024u;
    if (bytes > SIZE_MAX - (granularity - 1u)) return bytes;
    return (bytes + granularity - 1u) & ~(granularity - 1u);
}

static int pbo_slot_ready(unsigned slot) {
    if (!USE_PBO_SYNC || !UPLOAD_PBO_FENCES[slot]) return 1;
    GLenum status = glClientWaitSync(UPLOAD_PBO_FENCES[slot], 0, 0);
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) return 0;
    glDeleteSync(UPLOAD_PBO_FENCES[slot]);
    UPLOAD_PBO_FENCES[slot] = NULL;
    return 1;
}

static const void *upload_source(mr_cpu *c, int argument, size_t bytes, int *using_pbo) {
    *using_pbo = 0;
    uint32_t guest = A(argument);
    if (!guest) return NULL;
    if (!bytes || !mr_mem_ok(c, guest, bytes)) {
        if (bytes && !c->fault) {
            c->fault = "invalid texture source";
            c->fault_addr = guest;
            c->halted = 1;
            return NULL;
        }
        return P(argument);
    }

    const void *source = mr_mem(c, guest);
    if (!USE_PBO || bytes < PBO_THRESHOLD) return source;

    int selected = -1;
    for (unsigned n = 0; n < UPLOAD_PBO_COUNT; n++) {
        unsigned slot = (UPLOAD_PBO_NEXT + n) % UPLOAD_PBO_COUNT;
        if (pbo_slot_ready(slot)) {
            selected = (int)slot;
            break;
        }
    }
    if (selected < 0) {
        count_pbo_fallback();
        return source;
    }
    unsigned slot = (unsigned)selected;
    UPLOAD_PBO_NEXT = (slot + 1u) % UPLOAD_PBO_COUNT;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, UPLOAD_PBOS[slot]);

    size_t capacity = pbo_capacity(bytes);
    if (!USE_PBO_SYNC || UPLOAD_PBO_CAPACITY[slot] < bytes) {
        glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)capacity, NULL, GL_STREAM_DRAW);
        UPLOAD_PBO_CAPACITY[slot] = capacity;
    }
    void *mapped =
        glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)capacity, GL_MAP_WRITE_BIT);
    if (!mapped) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        count_pbo_fallback();
        return source;
    }
    memcpy(mapped, source, bytes);
    if (!glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER)) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        count_pbo_fallback();
        return source;
    }
    *using_pbo = (int)slot + 1;
    return NULL;
}

static void upload_end(int using_pbo) {
    if (!using_pbo) return;
    unsigned slot = (unsigned)(using_pbo - 1);
    if (USE_PBO_SYNC) {
        UPLOAD_PBO_FENCES[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!UPLOAD_PBO_FENCES[slot]) USE_PBO_SYNC = 0;
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

static double diagnostic_start(void) {
    return DIAGNOSTICS ? mr_monotonic_ms() : 0.0;
}

static void count_upload(size_t bytes, double started) {
    if (!DIAGNOSTICS) return;
    STAT.upload_bytes += bytes;
    STAT.upload_ms += mr_monotonic_ms() - started;
}

static void t_glTexImage2D(mr_cpu *c) {
    GLenum target = A(0);
    GLint level = (GLint)A(1);
    GLint internal_format = (GLint)A(2);
    GLsizei width = (GLsizei)A(3), height = (GLsizei)A(4);
    GLint border = (GLint)A(5);
    GLenum format = A(6), type = A(7);
    size_t bytes = pixel_size(format, type, width, height);
    int pbo = 0;
    double started = diagnostic_start();
    const void *source = upload_source(c, 8, bytes, &pbo);
    GLuint texture = bound_texture(target);
    int reuse = border == 0 && (source || pbo) &&
                texture_meta_matches(texture, target, level, internal_format, width, height, format,
                                     type, 0, 0);
    if (!c->halted) {
        if (reuse) {
            glTexSubImage2D(target, level, 0, 0, width, height, format, type, source);
            count_storage_reuse();
        } else {
            glTexImage2D(target, level, internal_format, width, height, border, format, type,
                         source);
            texture_meta_store(texture, target, level, internal_format, width, height, format, type,
                               0, 0);
            STAT.textures++;
        }
    }
    upload_end(pbo);
    count_upload(source || pbo ? bytes : 0, started);
}
static void t_glTexSubImage2D(mr_cpu *c) {
    GLsizei width = (GLsizei)A(4), height = (GLsizei)A(5);
    size_t bytes = pixel_size(A(6), A(7), width, height);
    int pbo = 0;
    double started = diagnostic_start();
    const void *source = upload_source(c, 8, bytes, &pbo);
    if (!c->halted) glTexSubImage2D(A(0), A(1), A(2), A(3), width, height, A(6), A(7), source);
    upload_end(pbo);
    count_upload(source || pbo ? bytes : 0, started);
}
static void t_glCompressedTexImage2D(mr_cpu *c) {
    GLenum target = A(0);
    GLint level = (GLint)A(1);
    GLenum internal_format = A(2);
    GLsizei width = (GLsizei)A(3), height = (GLsizei)A(4);
    GLint border = (GLint)A(5);
    GLsizei image_size = (GLsizei)A(6);
    size_t bytes = image_size > 0 ? (size_t)image_size : 0;
    int pbo = 0;
    double started = diagnostic_start();
    const void *source = upload_source(c, 7, bytes, &pbo);
    GLuint texture = bound_texture(target);
    int reuse = border == 0 && (source || pbo) &&
                texture_meta_matches(texture, target, level, (GLint)internal_format, width, height,
                                     internal_format, 0, image_size, 1);
    if (!c->halted) {
        if (reuse) {
            glCompressedTexSubImage2D(target, level, 0, 0, width, height, internal_format,
                                      image_size, source);
            count_storage_reuse();
        } else {
            glCompressedTexImage2D(target, level, internal_format, width, height, border,
                                   image_size, source);
            texture_meta_store(texture, target, level, (GLint)internal_format, width, height,
                               internal_format, 0, image_size, 1);
            STAT.textures++;
        }
    }
    upload_end(pbo);
    count_upload(source || pbo ? bytes : 0, started);
}
static void t_glCompressedTexSubImage2D(mr_cpu *c) {
    size_t bytes = (size_t)A(7);
    int pbo = 0;
    double started = diagnostic_start();
    const void *source = upload_source(c, 8, bytes, &pbo);
    if (!c->halted)
        glCompressedTexSubImage2D(A(0), A(1), A(2), A(3), A(4), A(5), A(6), (GLsizei)bytes, source);
    upload_end(pbo);
    count_upload(source || pbo ? bytes : 0, started);
}
static GLuint read_source_begin(void) {
    GLint bound = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &bound);
    if (SAMPLES && (GLuint)bound == DEF_FBO)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, resolved_target());
    return (GLuint)bound;
}

static void read_source_end(GLuint bound) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, bound);
}

static void t_glCopyTexImage2D(mr_cpu *c) {
    GLenum target = A(0);
    GLint level = (GLint)A(1);
    GLint internal_format = (GLint)A(2);
    GLsizei width = (GLsizei)A(5), height = (GLsizei)A(6);
    GLuint bound = read_source_begin();
    glCopyTexImage2D(target, level, internal_format, A(3), A(4), width, height, A(7));
    read_source_end(bound);
    texture_meta_store(bound_texture(target), target, level, internal_format, width, height, 0, 0,
                       0, 0);
}
static void t_glCopyTexSubImage2D(mr_cpu *c) {
    GLuint bound = read_source_begin();
    glCopyTexSubImage2D(A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7));
    read_source_end(bound);
}
static void t_glReadPixels(mr_cpu *c) {
    GLuint bound = read_source_begin();
    glReadPixels(A(0), A(1), A(2), A(3), A(4), A(5), P(6));
    read_source_end(bound);
}

static void t_glBindFramebuffer(mr_cpu *c) {
    uint32_t fb = A(1);
    glBindFramebuffer(A(0), fb ? fb : DEF_FBO);
}
static void t_glBindRenderbuffer(mr_cpu *c) {
    glBindRenderbuffer(A(0), A(1));
}
static void t_glGenFramebuffers(mr_cpu *c) {
    glGenFramebuffers(A(0), P(1));
}
static void t_glGenRenderbuffers(mr_cpu *c) {
    glGenRenderbuffers(A(0), P(1));
}
static void t_glDeleteFramebuffers(mr_cpu *c) {
    glDeleteFramebuffers(A(0), P(1));
}
static void t_glDeleteRenderbuffers(mr_cpu *c) {
    glDeleteRenderbuffers(A(0), P(1));
}
static void t_glFramebufferTexture2D(mr_cpu *c) {
    glFramebufferTexture2D(A(0), A(1), A(2), A(3), A(4));
}
static void t_glFramebufferRenderbuffer(mr_cpu *c) {
    glFramebufferRenderbuffer(A(0), A(1), A(2), A(3));
}
static void t_glRenderbufferStorage(mr_cpu *c) {
    glRenderbufferStorage(A(0), A(1), A(2), A(3));
}
static void t_glCheckFramebufferStatus(mr_cpu *c) {
    RET(glCheckFramebufferStatus(A(0)));
}
static void t_glIsFramebuffer(mr_cpu *c) {
    RET(glIsFramebuffer(A(0)));
}
static void t_glIsRenderbuffer(mr_cpu *c) {
    RET(glIsRenderbuffer(A(0)));
}
static void t_glGetRenderbufferParameteriv(mr_cpu *c) {
    glGetRenderbufferParameteriv(A(0), A(1), P(2));
}
static void t_glGetFramebufferAttachmentParameteriv(mr_cpu *c) {
    glGetFramebufferAttachmentParameteriv(A(0), A(1), A(2), P(3));
}

GL1i_R(glCreateShader) GL0_R(glCreateProgram) static void t_glCompileShader(mr_cpu *c) {
    GLuint shader = A(0);
    double started = diagnostic_start();
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (DIAGNOSTICS) {
        STAT.shader_jobs++;
        STAT.shader_ms += mr_monotonic_ms() - started;
    }
    if (ok) return;

    char log[2048] = {0};
    glGetShaderInfoLog(shader, (GLsizei)sizeof log - 1, NULL, log);
    fprintf(stderr, "[GL] shader compilation FAILED (#%u): %s\n", shader, log);
}

static void t_glLinkProgram(mr_cpu *c) {
    GLuint program = A(0);
    double started = diagnostic_start();
    glLinkProgram(program);
    if (DIAGNOSTICS) {
        STAT.shader_jobs++;
        STAT.shader_ms += mr_monotonic_ms() - started;
    }
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetProgramInfoLog(program, (GLsizei)sizeof log - 1, NULL, log);
        fprintf(stderr, "[GL] program linking FAILED (#%u): %s\n", program, log);
    }
}
static void t_glUseProgram(mr_cpu *c) {
    GLuint program = A(0);
    if (CURRENT_PROGRAM_KNOWN && CURRENT_PROGRAM == program) {
        count_state_skip();
        return;
    }
    glUseProgram(program);
    CURRENT_PROGRAM = program;
    CURRENT_PROGRAM_KNOWN = 1;
}
GL1i(glDeleteShader) static void t_glDeleteProgram(mr_cpu *c) {
    GLuint program = A(0);
    glDeleteProgram(program);
    if (CURRENT_PROGRAM_KNOWN && CURRENT_PROGRAM == program) CURRENT_PROGRAM_KNOWN = 0;
}
GL1i(glValidateProgram) GL2i(glAttachShader) GL2i(glDetachShader)

    static void t_glShaderSource(mr_cpu *c) {
    GLuint shader = A(0);
    GLsizei count = (GLsizei)A(1);
    uint32_t strings = A(2), lengths = A(3);
    if (count < 0 || (uint32_t)count > UINT32_MAX / 4u ||
        (count && !mr_mem_ok(c, strings, (uint32_t)count * 4u)) ||
        (lengths && count && !mr_mem_ok(c, lengths, (uint32_t)count * 4u))) {
        c->fault = "invalid shader-source array";
        c->fault_addr = strings;
        c->halted = 1;
        return;
    }

    char **src = calloc((size_t)count, sizeof(char *));
    if (count && !src) return;
    for (GLsizei i = 0; i < count; i++) {
        uint32_t sp = mr_ld32(c, strings + (uint32_t)i * 4);
        const char *raw = "";
        char *tmp = NULL;
        int32_t len = -1;
        if (lengths) {
            len = (int32_t)mr_ld32(c, lengths + (uint32_t)i * 4);
            if (len >= 0) {
                if (!sp || !mr_mem_ok(c, sp, (uint32_t)len)) {
                    c->fault = "invalid shader source";
                    c->fault_addr = sp;
                    c->halted = 1;
                    break;
                }
                tmp = malloc((size_t)len + 1);
                if (!tmp) break;
                memcpy(tmp, mr_mem(c, sp), (size_t)len);
                tmp[len] = 0;
                raw = tmp;
            }
        }
        if (len < 0 && sp) raw = mr_guest_cstr(c, sp);
        if (c->halted) {
            free(tmp);
            break;
        }
        src[i] = translate_shader(raw, i == 0);
        free(tmp);
        if (!src[i]) break;
    }
    GLsizei ready = 0;
    while (ready < count && src[ready])
        ready++;
    if (ready == count && !c->halted)
        glShaderSource(shader, count, (const GLchar *const *)src, NULL);
    for (GLsizei i = 0; i < count; i++)
        free(src[i]);
    free(src);
}

static void t_glGetShaderiv(mr_cpu *c) {
    glGetShaderiv(A(0), A(1), P(2));
}
static void t_glGetProgramiv(mr_cpu *c) {
    glGetProgramiv(A(0), A(1), P(2));
}
static void t_glGetShaderInfoLog(mr_cpu *c) {
    glGetShaderInfoLog(A(0), A(1), P(2), P(3));
}
static void t_glGetProgramInfoLog(mr_cpu *c) {
    glGetProgramInfoLog(A(0), A(1), P(2), P(3));
}
static void t_glGetShaderSource(mr_cpu *c) {
    glGetShaderSource(A(0), A(1), P(2), P(3));
}
static void t_glGetAttachedShaders(mr_cpu *c) {
    glGetAttachedShaders(A(0), A(1), P(2), P(3));
}
static void t_glBindAttribLocation(mr_cpu *c) {
    const char *name = args(c, 2);
    if (!c->halted) glBindAttribLocation(A(0), A(1), name);
}
static void t_glGetAttribLocation(mr_cpu *c) {
    const char *name = args(c, 1);
    RET(c->halted ? (uint32_t)-1 : (uint32_t)glGetAttribLocation(A(0), name));
}
static void t_glGetUniformLocation(mr_cpu *c) {
    const char *name = args(c, 1);
    RET(c->halted ? (uint32_t)-1 : (uint32_t)glGetUniformLocation(A(0), name));
}
static void t_glGetActiveAttrib(mr_cpu *c) {
    glGetActiveAttrib(A(0), A(1), A(2), P(3), P(4), P(5), P(6));
}
static void t_glGetActiveUniform(mr_cpu *c) {
    glGetActiveUniform(A(0), A(1), A(2), P(3), P(4), P(5), P(6));
}
static void t_glGetUniformfv(mr_cpu *c) {
    glGetUniformfv(A(0), A(1), P(2));
}
static void t_glGetUniformiv(mr_cpu *c) {
    glGetUniformiv(A(0), A(1), P(2));
}

// GLES2 shader binaries have no desktop GL equivalent.
static void t_glReleaseShaderCompiler(mr_cpu *c) {
    (void)c;
}
static void t_glShaderBinary(mr_cpu *c) {
    (void)c;
}
static void t_glGetShaderPrecisionFormat(mr_cpu *c) {
    uint32_t range = A(2), precision = A(3);
    if (range) {
        mr_st32(c, range, 127);
        mr_st32(c, range + 4, 127);
    }
    if (precision) mr_st32(c, precision, 23);
}

// Uniforms.
static void t_glUniform1i(mr_cpu *c) {
    glUniform1i(A(0), (GLint)A(1));
}
static void t_glUniform2i(mr_cpu *c) {
    glUniform2i(A(0), A(1), A(2));
}
static void t_glUniform3i(mr_cpu *c) {
    glUniform3i(A(0), A(1), A(2), A(3));
}
static void t_glUniform4i(mr_cpu *c) {
    glUniform4i(A(0), A(1), A(2), A(3), A(4));
}
static void t_glUniform1f(mr_cpu *c) {
    glUniform1f(A(0), F(1));
}
static void t_glUniform2f(mr_cpu *c) {
    glUniform2f(A(0), F(1), F(2));
}
static void t_glUniform3f(mr_cpu *c) {
    glUniform3f(A(0), F(1), F(2), F(3));
}
static void t_glUniform4f(mr_cpu *c) {
    glUniform4f(A(0), F(1), F(2), F(3), F(4));
}
static void t_glUniform1iv(mr_cpu *c) {
    glUniform1iv(A(0), A(1), P(2));
}
static void t_glUniform2iv(mr_cpu *c) {
    glUniform2iv(A(0), A(1), P(2));
}
static void t_glUniform3iv(mr_cpu *c) {
    glUniform3iv(A(0), A(1), P(2));
}
static void t_glUniform4iv(mr_cpu *c) {
    glUniform4iv(A(0), A(1), P(2));
}
static void t_glUniform1fv(mr_cpu *c) {
    glUniform1fv(A(0), A(1), P(2));
}
static void t_glUniform2fv(mr_cpu *c) {
    glUniform2fv(A(0), A(1), P(2));
}
static void t_glUniform3fv(mr_cpu *c) {
    glUniform3fv(A(0), A(1), P(2));
}
static void t_glUniform4fv(mr_cpu *c) {
    glUniform4fv(A(0), A(1), P(2));
}
static void t_glUniformMatrix2fv(mr_cpu *c) {
    glUniformMatrix2fv(A(0), A(1), A(2), P(3));
}
static void t_glUniformMatrix3fv(mr_cpu *c) {
    glUniformMatrix3fv(A(0), A(1), A(2), P(3));
}
static void t_glUniformMatrix4fv(mr_cpu *c) {
    glUniformMatrix4fv(A(0), A(1), A(2), P(3));
}

// Vertex attributes.
GL1i(glEnableVertexAttribArray)
    GL1i(glDisableVertexAttribArray) static void t_glVertexAttrib1f(mr_cpu *c) {
    glVertexAttrib1f(A(0), F(1));
}
static void t_glVertexAttrib2f(mr_cpu *c) {
    glVertexAttrib2f(A(0), F(1), F(2));
}
static void t_glVertexAttrib3f(mr_cpu *c) {
    glVertexAttrib3f(A(0), F(1), F(2), F(3));
}
static void t_glVertexAttrib4f(mr_cpu *c) {
    glVertexAttrib4f(A(0), F(1), F(2), F(3), F(4));
}
static void t_glVertexAttrib1fv(mr_cpu *c) {
    glVertexAttrib1fv(A(0), P(1));
}
static void t_glVertexAttrib2fv(mr_cpu *c) {
    glVertexAttrib2fv(A(0), P(1));
}
static void t_glVertexAttrib3fv(mr_cpu *c) {
    glVertexAttrib3fv(A(0), P(1));
}
static void t_glVertexAttrib4fv(mr_cpu *c) {
    glVertexAttrib4fv(A(0), P(1));
}
static void t_glGetVertexAttribfv(mr_cpu *c) {
    glGetVertexAttribfv(A(0), A(1), P(2));
}
static void t_glGetVertexAttribiv(mr_cpu *c) {
    glGetVertexAttribiv(A(0), A(1), P(2));
}
static void t_glGetVertexAttribPointerv(mr_cpu *c) {
    uint32_t out = A(2);
    if (out) mr_st32(c, out, 0);
}

static void t_glVertexAttribPointer(mr_cpu *c) {
    uint32_t ptr = A(5);
    const void *p = ARRAY_BUFFER_BINDING ? (const void *)(uintptr_t)ptr : P(5);
    if (c->halted) return;
    glVertexAttribPointer(A(0), A(1), A(2), (GLboolean)A(3), A(4), p);
}

static void t_glDrawElements(mr_cpu *c) {
    uint32_t idx = A(3);
    const void *p = ELEMENT_ARRAY_BUFFER_BINDING ? (const void *)(uintptr_t)idx : P(3);
    if (c->halted) return;
    STAT.draw++;
    STAT.vertices += A(1);
    glDrawElements(A(0), A(1), A(2), p);
}

static int gles2_limit(GLenum pname, GLint *out) {
    GLint v = 0;
    switch (pname) {
    case 0x8DFB: // GL_MAX_VERTEX_UNIFORM_VECTORS
        glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, &v);
        *out = v / 4;
        return 1;
    case 0x8DFD: // GL_MAX_FRAGMENT_UNIFORM_VECTORS
        glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, &v);
        *out = v / 4;
        return 1;
    case 0x8DFC: // GL_MAX_VARYING_VECTORS
#ifdef MR_GL_VARYING_UNIT_IS_FLOAT
        glGetIntegerv(GL_MAX_VARYING_FLOATS, &v);
        *out = v / 4;
        return 1;
#else
        glGetIntegerv(GL_MAX_VARYING_VECTORS, &v);
        *out = v;
        return 1;
#endif
    case 0x8DFA:
        *out = GL_TRUE;
        return 1; // GL_SHADER_COMPILER
    case 0x8DF9:
        *out = 0;
        return 1; // GL_NUM_SHADER_BINARY_FORMATS
    case 0x8DF8:
        *out = 0;
        return 1; // GL_SHADER_BINARY_FORMATS
    case 0x8B9B:
        *out = GL_RGBA;
        return 1; // GL_IMPLEMENTATION_COLOR_READ_FORMAT
    case 0x8B9A:
        *out = GL_UNSIGNED_BYTE;
        return 1; // ..._TYPE
    default:
        return 0;
    }
}

static void t_glGetIntegerv(mr_cpu *c) {
    GLint v;
    if (gles2_limit(A(0), &v)) {
        GLint *p = (GLint *)P(1);
        if (p) *p = v;
        return;
    }
    glGetIntegerv(A(0), P(1));
}
static void t_glGetFloatv(mr_cpu *c) {
    glGetFloatv(A(0), P(1));
}
static void t_glGetBooleanv(mr_cpu *c) {
    glGetBooleanv(A(0), P(1));
}

#define STRING_CACHE_CAP 8u
typedef struct {
    GLenum name;
    uint32_t addr;
} gl_string_cache;

static uint32_t STR_AREA, STR_AREA_END, STR_NEXT;
static gl_string_cache STR_CACHE[STRING_CACHE_CAP];
static unsigned STR_CACHE_COUNT;

void mr_gl_set_string_area(uint32_t base, uint32_t size) {
    STR_AREA = base;
    STR_AREA_END = base + size;
    STR_NEXT = base;
    STR_CACHE_COUNT = 0;
}

static void t_glGetString(mr_cpu *c) {
    GLenum name = A(0);
    for (unsigned i = 0; i < STR_CACHE_COUNT; i++) {
        if (STR_CACHE[i].name == name) {
            RET(STR_CACHE[i].addr);
            return;
        }
    }
    const char *s = GL_READY ? (const char *)glGetString(name) : "";
    // The GLES2 engine parses this version string.
    if (name == GL_VERSION)
        s = "OpenGL ES 2.0 (macOS nativ port)";
    else if (name == GL_SHADING_LANGUAGE_VERSION)
        s = "OpenGL ES GLSL ES 1.00";
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    if (!STR_AREA || STR_NEXT + n > STR_AREA_END) {
        RET(0);
        return;
    }
    uint32_t addr = STR_NEXT;
    memcpy(mr_mem(c, addr), s, n);
    STR_NEXT += (uint32_t)((n + 3) & ~3u);
    if (STR_CACHE_COUNT < STRING_CACHE_CAP) {
        STR_CACHE[STR_CACHE_COUNT].name = name;
        STR_CACHE[STR_CACHE_COUNT].addr = addr;
        STR_CACHE_COUNT++;
    }
    RET(addr);
}

// Buffer mapping through the OES extension.
static void t_glMapBufferOES(mr_cpu *c) {
    RET(0);
}
static void t_glUnmapBufferOES(mr_cpu *c) {
    RET(1);
}

static const mr_shim GL_SHIMS[] = {
    // EGL
    {"eglGetDisplay", t_eglGetDisplay},
    {"eglInitialize", t_eglInitialize},
    {"eglTerminate", t_eglTerminate},
    {"eglGetError", t_eglGetError},
    {"eglChooseConfig", t_eglChooseConfig},
    {"eglGetConfigAttrib", t_eglGetConfigAttrib},
    {"eglCreateWindowSurface", t_eglCreateWindowSurface},
    {"eglCreateContext", t_eglCreateContext},
    {"eglDestroySurface", t_eglDestroySurface},
    {"eglDestroyContext", t_eglDestroyContext},
    {"eglMakeCurrent", t_eglMakeCurrent},
    {"eglQuerySurface", t_eglQuerySurface},
    {"eglSwapBuffers", t_eglSwapBuffers},
    {"eglGetProcAddress", t_eglGetProcAddress},

    {"glEnable", t_glEnable},
    {"glDisable", t_glDisable},
    {"glClear", t_glClear},
    {"glClearColor", t_glClearColor},
    {"glClearDepthf", t_glClearDepthf},
    {"glClearStencil", t_glClearStencil},
    {"glDepthRangef", t_glDepthRangef},
    {"glDepthFunc", t_glDepthFunc},
    {"glDepthMask", t_glDepthMask},
    {"glColorMask", t_glColorMask},
    {"glCullFace", t_glCullFace},
    {"glFrontFace", t_glFrontFace},
    {"glViewport", t_glViewport},
    {"glScissor", t_glScissor},
    {"glBlendFunc", t_glBlendFunc},
    {"glBlendFuncSeparate", t_glBlendFuncSeparate},
    {"glBlendEquation", t_glBlendEquation},
    {"glBlendEquationSeparate", t_glBlendEquationSeparate},
    {"glBlendColor", t_glBlendColor},
    {"glStencilFunc", t_glStencilFunc},
    {"glStencilFuncSeparate", t_glStencilFuncSeparate},
    {"glStencilOp", t_glStencilOp},
    {"glStencilOpSeparate", t_glStencilOpSeparate},
    {"glStencilMask", t_glStencilMask},
    {"glStencilMaskSeparate", t_glStencilMaskSeparate},
    {"glPolygonOffset", t_glPolygonOffset},
    {"glSampleCoverage", t_glSampleCoverage},
    {"glLineWidth", t_glLineWidth},
    {"glHint", t_glHint},
    {"glPixelStorei", t_glPixelStorei},
    {"glActiveTexture", t_glActiveTexture},
    {"glFinish", t_glFinish},
    {"glFlush", t_glFlush},
    {"glGetError", t_glGetError},
    {"glIsEnabled", t_glIsEnabled},

    // Buffers.
    {"glGenBuffers", t_glGenBuffers},
    {"glDeleteBuffers", t_glDeleteBuffers},
    {"glBindBuffer", t_glBindBuffer},
    {"glBufferData", t_glBufferData},
    {"glBufferSubData", t_glBufferSubData},
    {"glIsBuffer", t_glIsBuffer},
    {"glGetBufferParameteriv", t_glGetBufferParameteriv},
    {"glMapBufferOES", t_glMapBufferOES},
    {"glUnmapBufferOES", t_glUnmapBufferOES},

    // Textures.
    {"glGenTextures", t_glGenTextures},
    {"glDeleteTextures", t_glDeleteTextures},
    {"glBindTexture", t_glBindTexture},
    {"glIsTexture", t_glIsTexture},
    {"glTexImage2D", t_glTexImage2D},
    {"glTexSubImage2D", t_glTexSubImage2D},
    {"glCompressedTexImage2D", t_glCompressedTexImage2D},
    {"glCompressedTexSubImage2D", t_glCompressedTexSubImage2D},
    {"glCopyTexImage2D", t_glCopyTexImage2D},
    {"glCopyTexSubImage2D", t_glCopyTexSubImage2D},
    {"glTexParameteri", t_glTexParameteri},
    {"glTexParameterf", t_glTexParameterf},
    {"glTexParameteriv", t_glTexParameteriv},
    {"glTexParameterfv", t_glTexParameterfv},
    {"glGetTexParameteriv", t_glGetTexParameteriv},
    {"glGetTexParameterfv", t_glGetTexParameterfv},
    {"glGenerateMipmap", t_glGenerateMipmap},
    {"glReadPixels", t_glReadPixels},

    // Framebuffers.
    {"glBindFramebuffer", t_glBindFramebuffer},
    {"glBindRenderbuffer", t_glBindRenderbuffer},
    {"glGenFramebuffers", t_glGenFramebuffers},
    {"glGenRenderbuffers", t_glGenRenderbuffers},
    {"glDeleteFramebuffers", t_glDeleteFramebuffers},
    {"glDeleteRenderbuffers", t_glDeleteRenderbuffers},
    {"glFramebufferTexture2D", t_glFramebufferTexture2D},
    {"glFramebufferRenderbuffer", t_glFramebufferRenderbuffer},
    {"glRenderbufferStorage", t_glRenderbufferStorage},
    {"glCheckFramebufferStatus", t_glCheckFramebufferStatus},
    {"glIsFramebuffer", t_glIsFramebuffer},
    {"glIsRenderbuffer", t_glIsRenderbuffer},
    {"glGetRenderbufferParameteriv", t_glGetRenderbufferParameteriv},
    {"glGetFramebufferAttachmentParameteriv", t_glGetFramebufferAttachmentParameteriv},

    // Shaders.
    {"glCreateShader", t_glCreateShader},
    {"glDeleteShader", t_glDeleteShader},
    {"glShaderSource", t_glShaderSource},
    {"glCompileShader", t_glCompileShader},
    {"glCreateProgram", t_glCreateProgram},
    {"glDeleteProgram", t_glDeleteProgram},
    {"glAttachShader", t_glAttachShader},
    {"glDetachShader", t_glDetachShader},
    {"glLinkProgram", t_glLinkProgram},
    {"glUseProgram", t_glUseProgram},
    {"glValidateProgram", t_glValidateProgram},
    {"glIsProgram", t_glIsProgram},
    {"glIsShader", t_glIsShader},
    {"glGetShaderiv", t_glGetShaderiv},
    {"glGetProgramiv", t_glGetProgramiv},
    {"glGetShaderInfoLog", t_glGetShaderInfoLog},
    {"glGetProgramInfoLog", t_glGetProgramInfoLog},
    {"glGetShaderSource", t_glGetShaderSource},
    {"glGetAttachedShaders", t_glGetAttachedShaders},
    {"glBindAttribLocation", t_glBindAttribLocation},
    {"glGetAttribLocation", t_glGetAttribLocation},
    {"glGetUniformLocation", t_glGetUniformLocation},
    {"glGetActiveAttrib", t_glGetActiveAttrib},
    {"glGetActiveUniform", t_glGetActiveUniform},
    {"glGetUniformfv", t_glGetUniformfv},
    {"glGetUniformiv", t_glGetUniformiv},
    {"glReleaseShaderCompiler", t_glReleaseShaderCompiler},
    {"glShaderBinary", t_glShaderBinary},
    {"glGetShaderPrecisionFormat", t_glGetShaderPrecisionFormat},

    // Uniforms.
    {"glUniform1i", t_glUniform1i},
    {"glUniform2i", t_glUniform2i},
    {"glUniform3i", t_glUniform3i},
    {"glUniform4i", t_glUniform4i},
    {"glUniform1f", t_glUniform1f},
    {"glUniform2f", t_glUniform2f},
    {"glUniform3f", t_glUniform3f},
    {"glUniform4f", t_glUniform4f},
    {"glUniform1iv", t_glUniform1iv},
    {"glUniform2iv", t_glUniform2iv},
    {"glUniform3iv", t_glUniform3iv},
    {"glUniform4iv", t_glUniform4iv},
    {"glUniform1fv", t_glUniform1fv},
    {"glUniform2fv", t_glUniform2fv},
    {"glUniform3fv", t_glUniform3fv},
    {"glUniform4fv", t_glUniform4fv},
    {"glUniformMatrix2fv", t_glUniformMatrix2fv},
    {"glUniformMatrix3fv", t_glUniformMatrix3fv},
    {"glUniformMatrix4fv", t_glUniformMatrix4fv},

    {"glEnableVertexAttribArray", t_glEnableVertexAttribArray},
    {"glDisableVertexAttribArray", t_glDisableVertexAttribArray},
    {"glVertexAttribPointer", t_glVertexAttribPointer},
    {"glVertexAttrib1f", t_glVertexAttrib1f},
    {"glVertexAttrib2f", t_glVertexAttrib2f},
    {"glVertexAttrib3f", t_glVertexAttrib3f},
    {"glVertexAttrib4f", t_glVertexAttrib4f},
    {"glVertexAttrib1fv", t_glVertexAttrib1fv},
    {"glVertexAttrib2fv", t_glVertexAttrib2fv},
    {"glVertexAttrib3fv", t_glVertexAttrib3fv},
    {"glVertexAttrib4fv", t_glVertexAttrib4fv},
    {"glGetVertexAttribfv", t_glGetVertexAttribfv},
    {"glGetVertexAttribiv", t_glGetVertexAttribiv},
    {"glGetVertexAttribPointerv", t_glGetVertexAttribPointerv},
    {"glDrawArrays", t_glDrawArrays},
    {"glDrawElements", t_glDrawElements},

    // Queries.
    {"glGetIntegerv", t_glGetIntegerv},
    {"glGetFloatv", t_glGetFloatv},
    {"glGetBooleanv", t_glGetBooleanv},
    {"glGetString", t_glGetString},
};

mr_thunk_fn mr_gl_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(GL_SHIMS) / sizeof(GL_SHIMS[0]); i++)
        if (strcmp(GL_SHIMS[i].name, name) == 0) return GL_SHIMS[i].fn;
    return NULL;
}
