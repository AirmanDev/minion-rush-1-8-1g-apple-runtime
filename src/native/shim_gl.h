#ifndef MR_SHIM_GL_H
#define MR_SHIM_GL_H

#include "cpu.h"
#include "elf_loader.h"
#include "shim_libc.h"

mr_thunk_fn mr_gl_lookup(const char *name);

typedef struct {
    unsigned clear;        // glClear
    unsigned draw;         // glDrawArrays + glDrawElements
    unsigned vertices;     // Vertices or indices submitted to draw calls.
    unsigned textures;     // New texture images.
    uint64_t upload_bytes; // Pixel data submitted during this frame.
    double upload_ms;      // Host CPU time spent in upload calls.
    unsigned shader_jobs;  // Shader compilations and program links.
    double shader_ms;
    unsigned state_skips;
    unsigned storage_reuses; // Existing storage updated instead of glTexImage.
    unsigned pbo_fallbacks;  // Direct uploads because all PBOs were busy.
} mr_gl_stats;

typedef struct {
    uint64_t state_skips;
    uint64_t storage_reuses;
    uint64_t pbo_fallbacks;
} mr_gl_totals;

const mr_gl_stats *mr_gl_get_stats(void);
const mr_gl_totals *mr_gl_get_totals(void);
void mr_gl_reset_stats(void);

int mr_gl_target_ok(void);
int mr_gl_save_png(const char *path); // Saves the target as PNG; zero is success.
void mr_gl_set_startup_capture(const char *path);
int mr_gl_startup_capture_enabled(void);
void mr_gl_request_startup_capture(void);
int mr_gl_startup_capture_result(void);

void mr_gl_blit_to_window(uint32_t w, uint32_t h);

typedef struct {
    double present_hz;
    double engine_hz; // Rate at which the engine produces new frames.
    double frame_ms;  // Median frame interval.
    double frame_p99_ms;
    double work_ms;
    double work_p99_ms;
    double display_hz; // Rate requested from the display link.
    double judder_per_s;
    uint32_t step_ms;
    uint64_t drops;
} mr_gl_hud;

void mr_gl_set_hud(const mr_gl_hud *hud);

void mr_gl_restore_target(void);

int mr_gl_init(uint32_t width, uint32_t height);
void mr_gl_shutdown(void);
const char *mr_gl_renderer(void);

// Target-framebuffer sample count; zero disables multisampling.
int mr_gl_samples(void);
float mr_gl_anisotropy(void);
int mr_gl_pbo_enabled(void);

void mr_gl_set_string_area(uint32_t base, uint32_t size);

#endif
