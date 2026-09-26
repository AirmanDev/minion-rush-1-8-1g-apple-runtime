#ifndef MR_PLATFORM_WINDOW_H
#define MR_PLATFORM_WINDOW_H

#include <math.h>
#include <stdint.h>
#include <stddef.h>

int mr_win_open(uint32_t window_w, uint32_t window_h, uint32_t surface_w, uint32_t surface_h,
                const char *title); // Returns zero on success.

double mr_win_display_aspect(void);
typedef struct {
    // Difference between consecutive display-link target timestamps. A missed
    // refresh makes this an integer multiple of the physical display period.
    double frame_ms;
    double refresh_ms;
} mr_frame_timing;

typedef int (*mr_idle_service_fn)(void *ctx, double budget_ms);

int mr_win_wait_frame(mr_frame_timing *out, mr_idle_service_fn idle, void *idle_ctx);
void mr_win_present(void);
void mr_win_close(void);

double mr_win_time_to_tick_ms(void);

static inline double mr_win_tick_remaining_ms(double target_timestamp, double now_seconds) {
    if (!(target_timestamp > 0.0)) return 0.0;
    return (target_timestamp - now_seconds) * 1000.0;
}

void mr_win_present_split(double *copy_ms, double *swap_ms);

double mr_win_refresh_hz(void);     // Current requested engine update rate.
double mr_win_max_refresh_hz(void); // Maximum rate advertised by the display.

void mr_win_request_content_rate(double content_hz);

#define MR_WIN_MAX_PRESENT_HZ 60.0

static inline void mr_win_apply_content_rate(double content_hz, double max_hz, double *start_hz,
                                             void (*apply)(double)) {
    if (!(content_hz > 0.0) || !(max_hz > 0.0) || !start_hz || !apply) return;
    double wanted = content_hz < MR_WIN_MAX_PRESENT_HZ ? MR_WIN_MAX_PRESENT_HZ : content_hz;
    if (wanted > MR_WIN_MAX_PRESENT_HZ) wanted = MR_WIN_MAX_PRESENT_HZ;
    if (wanted > max_hz) wanted = max_hz;
    long divisor = lround(max_hz / wanted);
    if (divisor < 1) divisor = 1;
    *start_hz = max_hz / (double)divisor;
    apply(*start_hz);
}
int mr_win_thermal_state(void);

uint64_t mr_win_present_count(void); // Host presents excluding calibration.
uint64_t mr_win_cadence_drops(void);
uint64_t mr_win_cadence_recoveries(void);

typedef struct {
    double x, y; // Image origin inside the drawable.
    double w, h;
} mr_win_fit;

static inline mr_win_fit mr_win_fit_surface(double target_w, double target_h, double surface_w,
                                            double surface_h) {
    mr_win_fit fit = {0.0, 0.0, target_w, target_h};
    if (!(target_w > 0.0) || !(target_h > 0.0) || !(surface_w > 0.0) || !(surface_h > 0.0))
        return fit;
    double scale = fmin(target_w / surface_w, target_h / surface_h);
    fit.w = surface_w * scale;
    fit.h = surface_h * scale;
    fit.x = (target_w - fit.w) * 0.5;
    fit.y = (target_h - fit.h) * 0.5;
    return fit;
}

static inline mr_win_fit mr_win_fit_surface_near_fill(double target_w, double target_h,
                                                      double surface_w, double surface_h,
                                                      double max_crop_pixels) {
    mr_win_fit fit = mr_win_fit_surface(target_w, target_h, surface_w, surface_h);
    if (!(target_w > 0.0) || !(target_h > 0.0) || !(surface_w > 0.0) || !(surface_h > 0.0) ||
        !(max_crop_pixels >= 0.0))
        return fit;
    double scale = fmax(target_w / surface_w, target_h / surface_h);
    double crop_w = surface_w - target_w / scale;
    double crop_h = surface_h - target_h / scale;
    if (crop_w > max_crop_pixels || crop_h > max_crop_pixels) return fit;
    fit.w = surface_w * scale;
    fit.h = surface_h * scale;
    fit.x = (target_w - fit.w) * 0.5;
    fit.y = (target_h - fit.h) * 0.5;
    return fit;
}

static inline uint32_t mr_win_surface_long_side(double target_w, double target_h,
                                                uint32_t short_side) {
    if (!(target_w > 0.0) || !(target_h > 0.0) || !short_side) return 0;
    double aspect = fmin(target_w, target_h) / fmax(target_w, target_h);
    double exact = fmin((double)short_side / aspect, (double)short_side * 4.0);
    return ((uint32_t)lround(exact) + 7u) & ~7u;
}

enum { MR_TOUCH_RELEASE = 0, MR_TOUCH_PRESS = 1, MR_TOUCH_MOVE = 2 };

typedef struct {
    int action;
    int x, y; // Engine-surface coordinates from the top-left corner.
} mr_touch;

int mr_win_next_touch(mr_touch *out);

void mr_win_enable_input(void);

unsigned mr_win_dropped_touches(void);
typedef struct {
    float x, y, z;
} mr_accel;

int mr_win_accel(mr_accel *out);

void mr_win_preferred_language(char *output, size_t capacity);

typedef struct {
    // Insets of the unobscured content area, normalized to the game view height.
    double top;
    double bottom;
} mr_win_insets;

mr_win_insets mr_win_safe_area(void);
void mr_win_surface_size(uint32_t *width, uint32_t *height);

// Movie playback is landscape on iOS and leaves other screens portrait.
void mr_win_set_movie_orientation(int landscape);
int mr_win_take_surface_orientation(void);    // -1 unchanged, 0 portrait, 1 landscape.
uint32_t mr_win_take_surface_long_side(void); // Zero when the scene aspect is unchanged.
void mr_win_set_surface_size(uint32_t width, uint32_t height);

#endif
