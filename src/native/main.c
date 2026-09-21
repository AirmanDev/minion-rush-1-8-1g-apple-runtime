
#include "a64_runtime.h"
#include "engine.h"
#include "host_time.h"
#include "guest_runtime.h"
#include "graphics_config.h"
#include "game_bindings.h"
#include "offline_mode.h"
#include "offline_events.h"
#include "cpu.h"
#include "elf_loader.h"
#include "shim_gl.h"
#include "guest_threads.h"
#include "jni_bridge.h"
#include "language_ui.h"
#include "shim_audio.h"
#include "shim_libc.h"
#include "platform_window.h"
#include "localization.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#define GAME_VERSION "1.8.1g"
#define WINDOW_W 640u
#define WINDOW_H 960u

#define GUEST_EXTRA (3072u * 1024u * 1024u)
#define STACK_POOL (512u * 1024u * 1024u)

#define JNI_ARENA (4u * 1024u * 1024u)
#define GL_STRINGS (64u * 1024u)
#define MAX_THUNKS 1024

#define INSN_LIMIT 8000000000ull

#define IDLE_GUEST_SLICE_INSNS 200000u
#define IDLE_GUEST_SLICE_MS 0.5

#define PRESENT_RESERVE_MS 1.0

#define DEFAULT_HEADLESS_FPS 120

static mr_elf_image IMG;
static mr_cpu CPU;
static uint32_t STACK_TOP;
static uint32_t LOCALIZATION_OBJECT;

// Imports.

static mr_thunk THUNKS[MAX_THUNKS];
static char *NAMES[MAX_THUNKS];
static uint32_t STUB_CALLS[MAX_THUNKS];
static int THUNK_COUNT;
static int IMPORT_COUNT, SHIM_COUNT, DATA_IMPORT_COUNT, STUB_IMPORT_COUNT;

static void stub_thunk(mr_cpu *c) {
    uint32_t idx = MR_THUNK_INDEX(c->r[MR_R_PC]);
    if (idx < MAX_THUNKS) STUB_CALLS[idx]++;
    c->r[0] = 0;
}

static uint32_t register_thunk(const char *name, mr_thunk_fn fn, void *user) {
    (void)user;
    if (THUNK_COUNT >= MAX_THUNKS) return 0;
    NAMES[THUNK_COUNT] = strdup(name);
    if (!NAMES[THUNK_COUNT]) return 0;
    THUNKS[THUNK_COUNT].name = NAMES[THUNK_COUNT];
    THUNKS[THUNK_COUNT].fn = fn;
    return MR_THUNK_ADDR(THUNK_COUNT++);
}

static int resolve_game_import(const char *name, uint32_t *out, void *user) {
    (void)user;
    IMPORT_COUNT++;

    uint32_t data = mr_shim_data_lookup(name);
    if (data) {
        DATA_IMPORT_COUNT++;
        *out = data;
        return 1;
    }

    mr_thunk_fn fn = mr_audio_lookup(name);
    if (!fn) fn = mr_shim_lookup(name);
    if (!fn) fn = mr_gl_lookup(name);
    if (!fn) fn = mr_zlib_lookup(name);
    if (fn)
        SHIM_COUNT++;
    else
        STUB_IMPORT_COUNT++;

    *out = register_thunk(name, fn ? fn : stub_thunk, NULL);
    return *out != 0;
}

// Guest-function calls.

static void print_call_trace(void) {
    uint32_t n = CPU.call_ring_pos < MR_CALL_RING ? CPU.call_ring_pos : MR_CALL_RING;
    if (n > 24) n = 24;
    for (uint32_t i = 1; i <= n; i++) {
        uint32_t a = CPU.call_ring[(CPU.call_ring_pos - i) & (MR_CALL_RING - 1)];
        uint32_t off = 0;
        const char *name = mr_elf_nearest(&IMG, a, &off);
        if (a >= MR_THUNK_BASE) {
            uint32_t idx = MR_THUNK_INDEX(a);
            printf("      0x%08x  [shim] %s\n", a, idx < (uint32_t)THUNK_COUNT ? NAMES[idx] : "?");
        } else if (name) {
            printf("      0x%08x  %s+0x%x\n", a, name, off);
        } else {
            printf("      0x%08x  ?\n", a);
        }
    }
}

static void reset_cpu(void) {
    memset(CPU.r, 0, sizeof CPU.r);
    memset(&CPU.f, 0, sizeof CPU.f);
    CPU.halted = 0;
    CPU.fault = NULL;
    CPU.fault_addr = 0;
    CPU.a64_slow_pc = 0;
    CPU.a64_slow_addr = 0;
    CPU.thumb = 0;
    CPU.itstate = 0;
    CPU.r[MR_R_SP] = STACK_TOP;
}

static int engine_call_guest_result(const char *label, uint32_t fn, int argc, const uint32_t *argv,
                                    uint32_t *result) {
    if (!fn) {
        printf("  %-24s NO SUCH SYMBOL\n", label);
        return -1;
    }

    reset_cpu();
    uint32_t value = mr_guest_call(&CPU, fn, argc, argv);
    if (CPU.fault) {
        printf("  %-24s STOPPED: %s (data 0x%08x, instruction 0x%08x)\n", label, CPU.fault,
               CPU.fault_addr, CPU.r[MR_R_PC]);
        printf("      r0=%08x r1=%08x r2=%08x r3=%08x "
               "r9=%08x sp=%08x lr=%08x\n",
               CPU.r[0], CPU.r[1], CPU.r[2], CPU.r[3], CPU.r[9], CPU.r[MR_R_SP], CPU.r[MR_R_LR]);
        print_call_trace();
        return -1;
    }
    if (result) *result = value;
    return 0;
}

static int engine_call_guest(const char *label, uint32_t fn, int argc, const uint32_t *argv) {
    return engine_call_guest_result(label, fn, argc, argv, NULL);
}

static uint32_t engine_language_value(const char *code) {
    if (!code || strlen(code) != 2) return 0;
    return (uint32_t)(uint8_t)code[0] << 8 | (uint32_t)(uint8_t)code[1];
}

static int load_engine_fonts(void) {
    uint32_t manager = mr_ld32(&CPU, MR_GAME_FONT_INFO_SINGLETON);
    uint32_t game = mr_ld32(&CPU, MR_GAME_GAME_SINGLETON);
    if (!manager || !game) {
        fprintf(stderr, "ERROR: engine font manager is unavailable\n");
        return -1;
    }

    const uint32_t can_load_arguments[2] = {manager, LOCALIZATION_OBJECT};
    uint32_t can_load = 0;
    if (engine_call_guest_result("FontInfo.CanLoad", MR_GAME_FONT_INFO_CAN_LOAD, 2,
                                 can_load_arguments, &can_load) != 0)
        return -1;
    if (!can_load) {
        fprintf(stderr, "ERROR: engine has no font configuration for localization '%s'\n",
                mr_localization_engine_code());
        return -1;
    }

    const uint32_t load_arguments[2] = {game, LOCALIZATION_OBJECT};
    return engine_call_guest("Game.LoadFonts", MR_GAME_GAME_LOAD_FONTS, 2, load_arguments);
}

static int select_engine_language(const char *code) {
    uint32_t babel = mr_ld32(&CPU, MR_GAME_BABEL_SINGLETON);
    uint32_t language = engine_language_value(code);
    if (!babel || !LOCALIZATION_OBJECT || !language) {
        fprintf(stderr, "ERROR: engine localization is unavailable\n");
        return -1;
    }
    mr_st32(&CPU, LOCALIZATION_OBJECT, language);
    mr_st32(&CPU, LOCALIZATION_OBJECT + 4u, 0x2d2d2d2du);
    mr_st32(&CPU, LOCALIZATION_OBJECT + 8u, 0x00002d2du);
    int custom = mr_localization_current() >= 0 && strcmp(code, mr_localization_engine_code()) == 0;
    if (custom && load_engine_fonts() != 0) return -1;
    const uint32_t arguments[3] = {babel, LOCALIZATION_OBJECT, 1u};
    uint32_t selected = 0;
    if (engine_call_guest_result("Babel.SelectLanguage", MR_GAME_BABEL_SELECT_LOCALIZATION, 3,
                                 arguments, &selected) != 0)
        return -1;
    if (!selected) {
        fprintf(stderr, "ERROR: engine could not load localization '%s'\n", code);
        return -1;
    }
    return 0;
}

static int apply_language_request(void) {
    int requested = mr_localization_take_request();
    int previous = mr_localization_current();
    if (requested == MR_LOCALIZATION_NO_REQUEST || requested == previous) return 0;
    if (previous >= 0 && requested >= 0 && select_engine_language("en") != 0) return -1;
    if (!mr_localization_activate(requested)) return 0;
    mr_shim_path_cache_flush();
    return select_engine_language(mr_localization_engine_code());
}

static int run_static_constructors(void) {
    uint32_t n = IMG.init_array_sz / 4, ok = 0, failed = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t fn = mr_ld32(&CPU, IMG.init_array + i * 4);
        if (!fn) continue;
        reset_cpu();
        mr_guest_call(&CPU, fn, 0, NULL);
        if (CPU.fault) {
            if (failed++ < 3)
                printf("  constructor #%u (0x%08x) STOPPED: %s @ 0x%08x\n", i, fn, CPU.fault,
                       CPU.fault_addr);
        } else {
            ok++;
        }
    }
    printf("static constructors: %u completed, %u stopped\n", ok, failed);
    return failed == 0;
}

static int start_engine(uint32_t env, uint32_t version_str, uint32_t render_w, uint32_t render_h) {
    printf("\nstartup:\n");

    const uint32_t onload_args[2] = {mr_jni_vm(), 0u};
    if (engine_call_guest("JNI_OnLoad", MR_GAME_JNI_ON_LOAD, 2, onload_args) != 0) return -1;

    const uint32_t jni_args[2] = {env, 0u};
    if (engine_call_guest("nativeInitMethods", MR_GAME_NATIVE_INIT_METHODS, 2, jni_args) != 0)
        return -1;
    if (engine_call_guest("Game.nativeInit", MR_GAME_GAME_NATIVE_INIT, 2, jni_args) != 0) return -1;

    const uint32_t renderer_args[6] = {env, 0u, 0u, render_w, render_h, version_str};
    if (engine_call_guest("Renderer.nativeInit", MR_GAME_RENDERER_NATIVE_INIT, 6, renderer_args) !=
        0)
        return -1;

    if (engine_call_guest("SurfaceView.nativeResume", MR_GAME_SURFACE_NATIVE_RESUME, 2, jni_args) !=
        0)
        return -1;

    return 0;
}

static int apply_surface_orientation(uint32_t env, uint32_t *width, uint32_t *height) {
    int landscape = mr_win_take_surface_orientation();
    if (landscape < 0) return 0;

    uint32_t short_side = *width < *height ? *width : *height;
    uint32_t long_side = *width < *height ? *height : *width;
    uint32_t wanted_width = landscape ? long_side : short_side;
    uint32_t wanted_height = landscape ? short_side : long_side;
    if (*width == wanted_width && *height == wanted_height) return 0;

    if (mr_gl_init(wanted_width, wanted_height) != 0) {
        fprintf(stderr, "ERROR: cannot resize the game render surface\n");
        return -1;
    }
    mr_win_set_surface_size(wanted_width, wanted_height);
    const uint32_t arguments[4] = {env, 0u, wanted_width, wanted_height};
    if (engine_call_guest("Renderer.nativeResize", MR_GAME_RENDERER_NATIVE_RESIZE, 4, arguments) !=
        0)
        return -1;
    *width = wanted_width;
    *height = wanted_height;
    printf("[Orientation] %s surface: %ux%u\n", landscape ? "landscape" : "portrait", wanted_width,
           wanted_height);
    return 0;
}

#define GRAVITY_ACCELERATION 9.80665f
#define KEYBOARD_TILT_ACCELERATION 5.0f

static int deliver_input(const char *label, uint32_t fn, int argc, const uint32_t *argv) {
    return engine_call_guest(label, fn, argc, argv);
}

static int deliver_touch(uint32_t env, uint32_t on_touch, int action, int x, int y) {
    uint32_t argv[6] = {env, 0u, (uint32_t)action, (uint32_t)x, (uint32_t)y, 0u};
    return deliver_input("touch", on_touch, 6, argv);
}

static FILE *TRACE;
static double TRACE_T0;

static void trace_open(void) {
    const char *path = getenv("MR_TRACE");
    if (!path || !*path) return;
    TRACE = fopen(path, "w");
    if (!TRACE) {
        fprintf(stderr, "note: %s is not writable; tracing is disabled\n", path);
        return;
    }
    setvbuf(TRACE, NULL, _IOLBF, 0);
    TRACE_T0 = mr_monotonic_ms();
    fprintf(TRACE, "# K time frame frame_ms refresh_ms work_ms "
                   "present_ms cpu_ms draws vertices upload_b upload_ms "
                   "shader_count shader_ms state_skips storage_reuses pbo_fallbacks "
                   "audio_ms underruns events dropped runnable idle_ms "
                   "new_content engine_hz\n"
                   "# E time action x y\n"
                   "# D time tilt\n");
}

static void trace_close(void) {
    if (!TRACE) return;
    fclose(TRACE);
    TRACE = NULL;
}

static void trace_event(const char *kind, const char *fmt, ...) {
    if (!TRACE) return;
    fprintf(TRACE, "%s %.2f ", kind, mr_monotonic_ms() - TRACE_T0);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(TRACE, fmt, ap);
    va_end(ap);
    fputc('\n', TRACE);
}

static int deliver_touches(uint32_t env, uint32_t on_touch) {
    mr_touch touch;
    int delivered = 0;
    while (mr_win_next_touch(&touch)) {
        trace_event("E", "%d %d %d", touch.action, touch.x, touch.y);
        delivered++;
        if (deliver_touch(env, on_touch, touch.action, touch.x, touch.y) != 0) return -1;
    }
    return delivered;
}

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static int deliver_tilt(uint32_t env, uint32_t on_accelerometer, const mr_accel *a) {
    uint32_t argv[5] = {env, 0u, float_bits(a->x), float_bits(a->y), float_bits(a->z)};
    return deliver_input("accelerometer", on_accelerometer, 5, argv);
}

#define ACCEL_EPSILON 0.02f

static int accel_changed(const mr_accel *a, const mr_accel *b) {
    return fabsf(a->x - b->x) > ACCEL_EPSILON || fabsf(a->y - b->y) > ACCEL_EPSILON ||
           fabsf(a->z - b->z) > ACCEL_EPSILON;
}

// Frame loop.

static int parse_nonnegative_int(const char *s, int default_value, const char *name) {
    if (!s) return default_value;
    char *end = NULL;
    errno = 0;
    long value = strtol(s, &end, 10);
    if (errno || end == s || *end || value < 0 || value > INT_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, s);
        return -1;
    }
    return (int)value;
}

static uint32_t scaled_dimension(uint32_t base, double scale) {
    uint32_t pixels = (uint32_t)((double)base * scale + 0.5);
    return (pixels + 7u) & ~7u;
}

static void sleep_ms(double ms) {
    if (ms <= 0.0) return;
    time_t seconds = (time_t)(ms / 1000.0);
    struct timespec remaining = {seconds, (long)((ms - (double)seconds * 1000.0) * 1000000.0)};
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
}

static const char *THERMAL_NAME[] = {"nominal", "fair", "serious", "critical"};

typedef struct {
    uint32_t frames;    // Frames in the current window.
    uint32_t late;      // Engine work exceeded one display period.
    uint32_t dropped;   // Frame interval exceeded one display period.
    double worst_work;  // Longest engine workload.
    double worst_frame; // Longest frame interval.
} stall_window;

static void stall_note(stall_window *w, double work_ms, double frame_ms, double refresh_ms) {
    w->frames++;
    if (work_ms > w->worst_work) w->worst_work = work_ms;
    if (frame_ms > w->worst_frame) w->worst_frame = frame_ms;
    if (refresh_ms > 0.0) {
        if (work_ms > refresh_ms) w->late++;
        if (frame_ms > refresh_ms * 1.5) w->dropped++;
    }
}

static void frame_status(int frame, double frame_ms, double work_ms, double present_ms,
                         double cpu_ms, double fps, double engine_hz, double idle_ms,
                         const stall_window *w) {
    const mr_gl_stats *g = mr_gl_get_stats();
    mr_audio_stats a = mr_audio_stats_get();
    printf("  #%-6d %5.1f ms (present %5.1f / engine %5.1f fps) | CPU %4.1f ms "
           "(engine %4.1f + present %4.1f) | GL: %u clears, "
           "%u draws (%u vertices), %u textures",
           frame, frame_ms, fps, engine_hz, cpu_ms, work_ms, present_ms, g->clear, g->draw,
           g->vertices, g->textures);
    if (g->upload_bytes)
        printf(", %.2f MB / %.2f ms upload", (double)g->upload_bytes / (1024.0 * 1024.0),
               g->upload_ms);
    if (g->shader_jobs) printf(", %u shader / %.2f ms", g->shader_jobs, g->shader_ms);
    if (g->state_skips || g->storage_reuses || g->pbo_fallbacks)
        printf(", cache %u/%u, PBO fallbacks %u", g->state_skips, g->storage_reuses,
               g->pbo_fallbacks);
    double copy_ms = 0.0, swap_ms = 0.0;
    mr_win_present_split(&copy_ms, &swap_ms);
    if (copy_ms > 0.0 || swap_ms > 0.0)
        printf(" | present: %.1f copy + %.1f swap", copy_ms, swap_ms);
    printf(" | audio: %u ms, silence %u ms", a.latency_ms, a.silence_ms);
    if (w && w->frames)
        printf(" || window %u frames: %u late, %u dropped, "
               "longest work %.1f ms, interval %.1f ms",
               w->frames, w->late, w->dropped, w->worst_work, w->worst_frame);
    printf(" | idle %.1f ms", idle_ms);
    int thermal = mr_win_thermal_state();
    if (thermal > 0 && thermal < (int)(sizeof THERMAL_NAME / sizeof *THERMAL_NAME))
        printf(" | thermal: %s", THERMAL_NAME[thermal]);
    printf("\n");
}

#define PACING_BUCKETS 16u

typedef struct {
    uint64_t seen_steps;
    uint64_t presents;
    uint64_t content_frames;
    uint64_t double_steps;
    double refresh_ms;
    uint32_t since_change;
    uint64_t interval[PACING_BUCKETS]; // Bucket n means n periods late.
    uint64_t interval_over;            // Longer than the last bucket.
    double smooth_hz;
} content_pacing;

static uint32_t pacing_typical(const content_pacing *p) {
    uint32_t best = 0;
    for (uint32_t i = 1; i < PACING_BUCKETS; i++)
        if (p->interval[i] > p->interval[best]) best = i;
    return best;
}

static uint64_t pacing_irregular(const content_pacing *p) {
    uint32_t typical = pacing_typical(p);
    uint64_t total = p->interval_over;
    for (uint32_t i = 1; i < PACING_BUCKETS; i++)
        if (i != typical) total += p->interval[i];
    return total;
}

#define ROLL_SAMPLES 256u
#define ROLL_PERIOD 32u

typedef struct {
    double value[ROLL_SAMPLES];
    uint32_t count, write, since;
    double p50, p99;
} roll_stat;

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void roll_push(roll_stat *r, double sample) {
    if (!(sample >= 0.0)) return;
    r->value[r->write] = sample;
    r->write = (r->write + 1u) % ROLL_SAMPLES;
    if (r->count < ROLL_SAMPLES) r->count++;
    if (++r->since < ROLL_PERIOD) return;
    r->since = 0;

    double sorted[ROLL_SAMPLES];
    memcpy(sorted, r->value, r->count * sizeof sorted[0]);
    qsort(sorted, r->count, sizeof sorted[0], compare_double);
    r->p50 = sorted[r->count / 2u];
    r->p99 = sorted[(r->count * 99u) / 100u];
}

static int pacing_observe(content_pacing *p, uint64_t steps, double refresh_ms) {
    p->presents++;
    p->since_change++;
    if (refresh_ms > 0.0) p->refresh_ms = refresh_ms;
    uint64_t taken = steps - p->seen_steps;
    if (!taken) return 0;
    p->seen_steps = steps;
    if (taken > 1) p->double_steps += taken - 1;

    uint32_t periods = p->since_change;
    p->since_change = 0;
    if (!p->content_frames++) return 1;

    if (periods < PACING_BUCKETS)
        p->interval[periods]++;
    else
        p->interval_over++;
    if (refresh_ms > 0.0) {
        double hz = 1000.0 / ((double)periods * refresh_ms);
        p->smooth_hz = p->smooth_hz > 0.0 ? p->smooth_hz * 0.85 + hz * 0.15 : hz;
    }
    return 1;
}

static void report_pacing(const content_pacing *p, double refresh_ms) {
    if (!p->content_frames) return;
    mr_guest_step_stats step = mr_guest_step_get_stats();
    if (step.steps) {
        printf("engine time step: %llu steps, average dt %.2f ms "
               "(min %u, max %u)\n",
               (unsigned long long)step.steps, (double)step.dt_sum / (double)step.steps,
               step.dt_min, step.dt_max);
        double period = p->refresh_ms;
        double average_step = (double)step.dt_sum / (double)step.steps;
        if (period > 0.0 && !step.scaled_steps) {
            long frames = lround(average_step / period);
            if (frames < 1) frames = 1;
            double locked = average_step / (double)frames;
            printf("  timeline lock: %.3f ms/frame instead of display %.3f ms "
                   "(world time %+.2f%%)\n",
                   locked, period, 100.0 * (locked / period - 1.0));
        } else if (period > 0.0) {
            printf("  timeline lock: time step changed during the session "
                   "(%u..%u ms) with the requested display rate; "
                   "one world-time ratio is not meaningful\n",
                   step.dt_min, step.dt_max);
        }
        if (step.scaled_steps)
            printf("  time scale: %llu scaled steps (%.3f..%.3f), "
                   "truncation corrected on %llu\n",
                   (unsigned long long)step.scaled_steps, (double)step.scale_min,
                   (double)step.scale_max, (unsigned long long)step.scale_fixed);
        else if (step.scale_rejected > step.scale_one)
            printf("  time scale: SUSPICIOUS - invalid values were read from "
                   "Game+0x1ec on %llu steps (1.0 only %llu times); the field "
                   "address is probably WRONG and compensation is ineffective\n",
                   (unsigned long long)step.scale_rejected, (unsigned long long)step.scale_one);
        else
            printf("  time scale: 1.0 for %llu steps, invalid for %llu; "
                   "fractional base step active\n",
                   (unsigned long long)step.scale_one, (unsigned long long)step.scale_rejected);
    }
    uint32_t typical = pacing_typical(p);
    uint64_t irregular = pacing_irregular(p);
    uint64_t rated = p->content_frames - 1;
    double seconds = (double)p->presents * refresh_ms / 1000.0;

    printf("engine content rate: %llu new frames / %llu presents",
           (unsigned long long)p->content_frames, (unsigned long long)p->presents);
    if (seconds > 0.0)
        printf(" = %.1f Hz (present %.1f Hz)", (double)p->content_frames / seconds,
               (double)p->presents / seconds);
    printf("\n");
    if (typical && refresh_ms > 0.0)
        printf("  typical interval: %u display periods (%.1f ms = %.1f Hz)\n", typical,
               (double)typical * refresh_ms, 1000.0 / ((double)typical * refresh_ms));
    if (rated) {
        printf("  irregular intervals: %llu / %llu (%.2f%%", (unsigned long long)irregular,
               (unsigned long long)rated, 100.0 * (double)irregular / (double)rated);
        if (seconds > 0.0) printf(", %.2f/s", (double)irregular / seconds);
        printf(")\n");
    }
    if (p->double_steps)
        printf("  catch-up double steps: %llu%s\n", (unsigned long long)p->double_steps,
               seconds > 0.0 ? "" : "");
    printf("  interval histogram:");
    for (uint32_t i = 1; i < PACING_BUCKETS; i++)
        if (p->interval[i]) printf(" %u:%llu", i, (unsigned long long)p->interval[i]);
    if (p->interval_over) printf(" %u+:%llu", PACING_BUCKETS, (unsigned long long)p->interval_over);
    printf("\n");
}

typedef struct {
    mr_cpu *cpu;
    uint64_t calls;
    uint64_t insns;
    double frame_ms;
} idle_service_state;

static int service_idle_guest(void *opaque, double budget_ms) {
    idle_service_state *state = (idle_service_state *)opaque;
    if (budget_ms > IDLE_GUEST_SLICE_MS) budget_ms = IDLE_GUEST_SLICE_MS;
    if (budget_ms <= 0.0) return 0;

    uint64_t ran = 0;
    double started = mr_monotonic_ms();
    int rc = mr_sched_service_idle(state->cpu, IDLE_GUEST_SLICE_INSNS,
                                   (uint64_t)(budget_ms * 1000000.0), &ran);
    if (rc != 0) return -1;
    if (!ran) return 0;
    state->calls++;
    state->insns += ran;
    state->frame_ms += mr_monotonic_ms() - started;
    return 1;
}

static int run_frames(uint32_t env, int frames, int windowed, uint32_t render_w,
                      uint32_t render_h) {
    int headless_fps = DEFAULT_HEADLESS_FPS;
    if (!windowed) {
        headless_fps = parse_nonnegative_int(getenv("MR_HEADLESS_FPS"), DEFAULT_HEADLESS_FPS,
                                             "MR_HEADLESS_FPS");
        if (headless_fps < 0) return -1;
    }
    double period_ms = !windowed && headless_fps > 0 ? 1000.0 / (double)headless_fps : 0.0;
    trace_open();

    const uint32_t render = MR_GAME_NATIVE_RENDER;
    const uint32_t render_args[2] = {env, 0u};
    const uint32_t on_touch = MR_GAME_NATIVE_TOUCH;
    uint32_t on_accelerometer = MR_GAME_NATIVE_ACCELEROMETER;
    mr_accel last_tilt = {0.0f, 0.0f, 0.0f};
    int have_tilt = 0;
    uint64_t frame_insns = mr_a64_native_insns();
    stall_window stalls = {0};

    double smooth_fps = 0.0;
    int first_draw = 0;
    const char *log_frames_env = getenv("MR_LOG_FRAMES");
    int log_frames = log_frames_env && strcmp(log_frames_env, "0") != 0;
    int startup_test_ms = 0;
    const char *startup_test = getenv("MR_STARTUP_TEST_MS");
    if (startup_test) {
        startup_test_ms = parse_nonnegative_int(startup_test, 0, "MR_STARTUP_TEST_MS");
        if (!windowed || startup_test_ms <= 0 || !getenv("MR_DIAGNOSTICS")) {
            fprintf(stderr, "ERROR: startup testing requires windowed mode, diagnostics, "
                            "and a positive timeout\n");
            return -1;
        }
    }

    printf("\nframes: ");
    if (windowed)
        printf("CADisplayLink, initial rate %.0f Hz (panel max %.0f Hz)", mr_win_refresh_hz(),
               mr_win_max_refresh_hz());
    else if (period_ms > 0.0)
        printf("headless target %d fps (%.2f ms)", headless_fps, period_ms);
    else
        printf("headless, unpaced");
    printf("\n");

    double deadline = mr_monotonic_ms();
    double display_sum = 0.0, display_min = DBL_MAX, display_max = 0.0;
    uint64_t display_samples = 0;
    uint64_t startup_callback_gaps = 0, steady_callback_gaps = 0;
    idle_service_state idle_state = {&CPU, 0, 0, 0.0};
    content_pacing pacing = {0};
    roll_stat frame_roll = {0}, work_roll = {0};
    double test_started_ms = mr_monotonic_ms();
    int startup_ready = 0;
    uint64_t startup_signal_callbacks = 0;
    int localization_pending = mr_localization_current() >= 0;
    int startup_capture = windowed && mr_gl_startup_capture_enabled();
    if (!localization_pending) mr_localization_finish_engine_bootstrap();

    mr_guest_clock_reset();
    mr_guest_set_target_fps(MR_GRAPHICS_ENGINE_HZ);

    for (int f = 0; frames == 0 || f < frames; f++) {
        mr_frame_timing timing = {0};
        idle_state.frame_ms = 0.0;
        if (windowed) {
            int wait_rc = mr_win_wait_frame(&timing, service_idle_guest, &idle_state);
            if (wait_rc < 0) return -1;
            if (wait_rc == 0) {
                printf("  (window closed)\n");
                break;
            }
            if (apply_surface_orientation(env, &render_w, &render_h) != 0) return -1;
        }

        double frame_ms = windowed ? timing.frame_ms : 0.0;
        double fps = frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0;
        if (windowed) {
            smooth_fps = smooth_fps > 0.0 ? smooth_fps * 0.85 + fps * 0.15 : fps;
            double seconds = (double)pacing.presents * pacing.refresh_ms / 1000.0;
            mr_gl_hud hud = {
                .present_hz = smooth_fps,
                .engine_hz = pacing.smooth_hz,
                .frame_ms = frame_roll.p50,
                .frame_p99_ms = frame_roll.p99,
                .work_ms = work_roll.p50,
                .work_p99_ms = work_roll.p99,
                .display_hz = mr_win_refresh_hz(),
                .judder_per_s = seconds > 0.0 ? (double)pacing_irregular(&pacing) / seconds : 0.0,
                .step_ms = mr_guest_step_get_stats().last_dt,
                .drops = mr_win_cadence_drops(),
            };
            mr_gl_set_hud(&hud);

            if (timing.refresh_ms > 0.0) {
                long periods = lround(frame_ms / timing.refresh_ms);
                if (periods < 1) periods = 1;
                mr_guest_clock_step(timing.refresh_ms, (uint32_t)periods);
            }
        }

        double t0 = mr_monotonic_ms();
        int events = 0;
        if (windowed) {
            if (apply_language_request() != 0) return -1;
            if (on_touch && (events = deliver_touches(env, on_touch)) < 0) return -1;
            mr_accel accel;
            if (on_accelerometer && mr_win_accel(&accel) &&
                (!have_tilt || accel_changed(&accel, &last_tilt))) {
                if (deliver_tilt(env, on_accelerometer, &accel) != 0) {
                    on_accelerometer = 0;
                } else {
                    last_tilt = accel;
                    have_tilt = 1;
                }
                trace_event("D", "%.3f %.3f %.3f", accel.x, accel.y, accel.z);
            }
        }

        mr_gl_reset_stats();
        mr_guest_clock_begin_render();
        int rc = engine_call_guest("nativeRender", render, 2, render_args);
        mr_guest_clock_end_render();
        if (rc != 0) return -1;
        if (mr_language_ui_apply(&CPU, STACK_TOP) != 0) return -1;
        if (localization_pending && mr_ld32(&CPU, MR_GAME_BABEL_SINGLETON)) {
            if (select_engine_language(mr_localization_engine_code()) != 0) return -1;
            mr_localization_finish_engine_bootstrap();
            localization_pending = 0;
        }
        int new_content = pacing_observe(&pacing, mr_guest_steps_taken(), timing.refresh_ms);
        int menu_signal =
            !startup_ready && mr_offline_main_menu_ready() && mr_offline_events_ready();
        if (menu_signal && startup_capture) mr_gl_request_startup_capture();

        double work_ms = mr_monotonic_ms() - t0;
        if (work_ms >= 1000.0) {
            uint64_t insns = mr_a64_native_insns() - frame_insns;
            printf("  *** long frame #%d: %.1f s, %.2f billion guest instructions "
                   "(%.2f billion/s) ***\n",
                   f + 1, work_ms / 1000.0, (double)insns / 1e9, (double)insns / 1e6 / work_ms);
        }
        frame_insns = mr_a64_native_insns();
        roll_push(&work_roll, work_ms);
        if (windowed) roll_push(&frame_roll, frame_ms);
        stall_note(&stalls, work_ms, frame_ms, timing.refresh_ms);
        double present_ms = 0.0;
        if (windowed) {
            double slack = mr_win_time_to_tick_ms() - PRESENT_RESERVE_MS;
            while (slack > 0.0) {
                if (service_idle_guest(&idle_state, slack) <= 0) break;
                slack = mr_win_time_to_tick_ms() - PRESENT_RESERVE_MS;
            }

            double present_start = mr_monotonic_ms();
            mr_win_present();
            present_ms = mr_monotonic_ms() - present_start;
        } else if (period_ms > 0.0) {
            deadline += period_ms;
            double now = mr_monotonic_ms();
            if (now < deadline) {
                sleep_ms(deadline - now);
            } else {
                deadline = now;
            }
        }

        double cpu_ms = mr_monotonic_ms() - t0;
        if (!windowed) {
            frame_ms = cpu_ms;
            fps = frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0;
            smooth_fps = smooth_fps > 0.0 ? smooth_fps * 0.85 + fps * 0.15 : fps;
        } else if (frame_ms > 0.0) {
            display_sum += frame_ms;
            if (frame_ms < display_min) display_min = frame_ms;
            if (frame_ms > display_max) display_max = frame_ms;
            display_samples++;
            if (timing.refresh_ms > 0.0) {
                long periods = lround(frame_ms / timing.refresh_ms);
                if (periods > 1) {
                    uint64_t gaps = (uint64_t)(periods - 1);
                    if (f < 60)
                        startup_callback_gaps += gaps;
                    else
                        steady_callback_gaps += gaps;
                }
            }
        }

        if (TRACE) {
            const mr_gl_stats *g = mr_gl_get_stats();
            mr_audio_stats a = mr_audio_stats_get();
            trace_event("K",
                        "%d %.3f %.3f %.3f %.3f %.3f %u %u %llu %.3f %u %.3f "
                        "%u %u %u %u %u %d %u %d %.3f %d %.2f",
                        f + 1, frame_ms, timing.refresh_ms, work_ms, present_ms, cpu_ms, g->draw,
                        g->vertices, (unsigned long long)g->upload_bytes, g->upload_ms,
                        g->shader_jobs, g->shader_ms, g->state_skips, g->storage_reuses,
                        g->pbo_fallbacks, a.latency_ms, a.underruns, events,
                        mr_win_dropped_touches(), mr_sched_runnable(&CPU), idle_state.frame_ms,
                        new_content, pacing.smooth_hz);
        }

        if (!first_draw && mr_gl_get_stats()->draw) {
            first_draw = 1;
            if (windowed) mr_win_enable_input();
            printf("  *** first frame: %u draw calls on frame %d ***\n", mr_gl_get_stats()->draw,
                   f + 1);
        }
        if (f == 0 || f + 1 == frames || (log_frames && (f + 1) % 100 == 0)) {
            frame_status(f + 1, frame_ms, work_ms, present_ms, cpu_ms, fps, pacing.smooth_hz,
                         idle_state.frame_ms, &stalls);
            stalls = (stall_window){0};
        }

        if (menu_signal) {
            int capture_result = startup_capture ? mr_gl_startup_capture_result() : 1;
            if (capture_result == -2) {
                fprintf(stderr, "ERROR: startup presentation buffer cannot be saved\n");
                return -1;
            }
            if (capture_result == 1) {
                startup_ready = 1;
                startup_signal_callbacks = mr_audio_stats_get().signal_callbacks;
                printf("  *** startup services ready: %.1f s, frame %d ***\n",
                       (mr_monotonic_ms() - test_started_ms) / 1000.0, f + 1);
            }
        }

        if (startup_test_ms) {
            if (mr_blocked_network_calls()) {
                fprintf(stderr, "ERROR: guest attempted a network import during startup\n");
                return -1;
            }
            mr_audio_stats audio = mr_audio_stats_get();
            if (startup_ready && audio.signal_callbacks > startup_signal_callbacks) {
                printf("  *** post-menu PCM signal arrived on frame %d ***\n", f + 1);
                break;
            }
            if (mr_monotonic_ms() - test_started_ms >= (double)startup_test_ms) {
                fprintf(stderr,
                        "ERROR: startup services, local event, and audio signal did not arrive "
                        "within %d ms\n",
                        startup_test_ms);
                return -1;
            }
        }
    }

    if (windowed && display_samples) {
        uint64_t presents = mr_win_present_count();
        printf("\ndisplay link: %llu engine updates, %.3f / %.3f / %.3f ms "
               "(min/average/max)\n",
               (unsigned long long)display_samples, display_min,
               display_sum / (double)display_samples, display_max);
        printf("display-link callback gaps: startup %llu, steady %llu"
               " | longest gap %.0f ms\n",
               (unsigned long long)startup_callback_gaps, (unsigned long long)steady_callback_gaps,
               display_max);
        printf("engine update rate: %.0f Hz, %llu reductions, "
               "%llu recoveries\n",
               mr_win_refresh_hz(), (unsigned long long)mr_win_cadence_drops(),
               (unsigned long long)mr_win_cadence_recoveries());
        printf("host presents: %llu / %llu engine updates\n", (unsigned long long)presents,
               (unsigned long long)display_samples);
        printf("idle guest work: %llu slices, %llu instructions\n",
               (unsigned long long)idle_state.calls, (unsigned long long)idle_state.insns);
        report_pacing(&pacing, display_sum / (double)display_samples);
        if (presents != display_samples) {
            fprintf(stderr, "ERROR: host-present count differs from engine-update count\n");
            return -1;
        }
    }
    return 0;
}

// Final report.

static void report_io(void) {
    uint64_t calls = 0, bytes = 0;
    double ms = 0.0;
    mr_shim_io_stats(&calls, &bytes, &ms);
    if (!calls) return;
    printf("file reads: %llu calls, %.1f MB, %.0f ms (average %.1f KB/call)\n",
           (unsigned long long)calls, (double)bytes / (1024.0 * 1024.0), ms,
           (double)bytes / 1024.0 / (double)calls);

    uint64_t opens = 0;
    double open_ms = 0.0;
    mr_shim_open_stats(&opens, &open_ms);
    if (opens)
        printf("file opens: %llu calls, %.0f ms (average %.2f ms/call)\n",
               (unsigned long long)opens, open_ms, open_ms / (double)opens);

    uint64_t hits = 0, misses = 0;
    mr_shim_path_cache_stats(&hits, &misses);
    if (hits + misses)
        printf("path cache: %llu hits, %llu misses (%.0f%%)\n", (unsigned long long)hits,
               (unsigned long long)misses, 100.0 * (double)hits / (double)(hits + misses));
}

static void report_cpu_load(double wall_ms) {
    struct rusage usage;
    if (wall_ms <= 0.0 || getrusage(RUSAGE_SELF, &usage) != 0) return;
    double user = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6;
    double sys = usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
    printf("CPU time: %.1f s user + %.1f s system = %.2f cores "
           "on average over %.1f s\n",
           user, sys, (user + sys) / (wall_ms / 1000.0), wall_ms / 1000.0);
}

static int report(void) {
    int failures = 0;
    int stubs = 0;
    mr_audio_stats a = mr_audio_stats_get();
    printf("\naudio: %u buffers played, %u enqueues, %u underruns, "
           "%u ms silence inserted, %u ms queued, %u callbacks behind\n",
           a.buffers_played, a.enqueues, a.underruns, a.silence_ms, a.latency_ms,
           a.callback_backlog);
    if (getenv("MR_DIAGNOSTICS"))
        printf("audio signal: %llu/%llu non-silent samples, %llu/%llu non-silent callbacks, "
               "peak %u\n",
               (unsigned long long)a.signal_samples, (unsigned long long)a.rendered_samples,
               (unsigned long long)a.signal_callbacks, (unsigned long long)a.render_callbacks,
               a.peak_amplitude);
    printf("render target: %s\n", mr_gl_target_ok() ? "complete" : "INCOMPLETE");
    const mr_gl_totals *gl_total = mr_gl_get_totals();
    printf("GL optimization: %llu redundant states skipped, "
           "%llu texture stores reused, %llu PBO fallbacks\n",
           (unsigned long long)gl_total->state_skips, (unsigned long long)gl_total->storage_reuses,
           (unsigned long long)gl_total->pbo_fallbacks);

    const char *shot = getenv("MR_SHOT");
    if (shot) printf("screenshot %s: %s\n", shot, mr_gl_save_png(shot) == 0 ? "saved" : "FAILED");

    mr_jni_report();
    failures |= mr_a64_report();
    mr_offline_report();
    uint64_t network_calls = mr_blocked_network_calls();
    printf("blocked network calls: %llu\n", (unsigned long long)network_calls);
    if (network_calls) failures++;
    if (CPU.insn_count) {
        printf("guest code: %llu ARM32/Thumb instructions in static ARM64 blocks\n",
               (unsigned long long)mr_a64_native_insns());
        printf("ABI boundary: %llu shim calls, %llu kuser calls\n",
               (unsigned long long)mr_guest_host_calls(),
               (unsigned long long)mr_guest_kuser_calls());
    }

    mr_guest_gameplay_flush();
    const mr_guest_gameplay_stat *gp = mr_guest_gameplay_get_stats();
    int gp_seen = 0;
    for (int i = 0; i < MR_GUEST_GAMEPLAY_SLOTS; i++) {
        if (!gp[i].sessions) continue;
        if (!gp_seen++) printf("\ngameplay subsystems (session = one start):\n");
        double hz = gp[i].active_ms > 0.0 ? (double)gp[i].calls * 1000.0 / gp[i].active_ms : 0.0;
        const char *kind = "timed?";
        if (gp[i].sessions >= 2) {
            double spread = gp[i].max_ms > 0.0 ? (gp[i].max_ms - gp[i].min_ms) / gp[i].max_ms : 0.0;
            kind = spread > 0.15 ? "PLAYER-DEPENDENT" : "timed";
        }
        printf("  %-21s %2u sessions | %7llu updates | %8.2f s active | "
               "%6.1f updates/s | session length %.2f..%.2f s | %s\n",
               gp[i].name, gp[i].sessions, (unsigned long long)gp[i].calls,
               gp[i].active_ms / 1000.0, hz, gp[i].sessions ? gp[i].min_ms / 1000.0 : 0.0,
               gp[i].max_ms / 1000.0, kind);
    }
    if (!gp_seen) printf("\ngameplay subsystems: no minigame started\n");

    uint64_t fluffy = mr_guest_fluffy_fixes();
    if (fluffy)
        printf("  Fluffy mixed integration: %llu corrections "
               "(gravity made time-proportional)\n",
               (unsigned long long)fluffy);

    mr_guest_clock_stats clock = mr_guest_clock_get_stats();
    printf("guest clock: during render %llu monotonic + %llu real | "
           "outside render %llu + %llu | reanchors %llu\n",
           (unsigned long long)clock.monotonic_render, (unsigned long long)clock.realtime_render,
           (unsigned long long)clock.monotonic_outside, (unsigned long long)clock.realtime_outside,
           (unsigned long long)clock.reanchors);

    for (int i = 0; i < THUNK_COUNT; i++) {
        if (!STUB_CALLS[i]) continue;
        if (!stubs++) printf("\nunimplemented but CALLED imports:\n");
        printf("  %8u x  %s\n", STUB_CALLS[i], NAMES[i]);
    }

    printf("\nguest threads: %d active, %u created\n", mr_sched_live_threads(&CPU),
           mr_sched_total_created(&CPU));
    mr_sched_dump(&CPU);
    return failures || stubs;
}

// Entry point.

int mr_engine_main(int argc, char **argv) {
    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <libdespicablemefree.so> [game-data] [frames]\n"
                "  frame count 0 or omitted: run until the window closes\n",
                argv[0]);
        return 2;
    }
    const char *data_root = argc > 2 ? argv[2] : ".";
    int frames = parse_nonnegative_int(argc > 3 ? argv[3] : NULL, 0, "frame count");
    if (frames < 0) return 2;
    mr_set_data_root(data_root);
    const char *data_base = getenv("MR_DATA_BASE");
    if (data_base) mr_set_data_base(data_base);
    char system_language[64];
    mr_win_preferred_language(system_language, sizeof system_language);
    mr_localization_init(getenv("MR_LOCALIZATION_ROOT"), data_root, system_language,
                         getenv("MR_LANGUAGE"));

    if (mr_elf_load(argv[1], &IMG, GUEST_EXTRA) != 0) {
        fprintf(stderr, "ERROR: %s\n", IMG.error);
        return 1;
    }
    int result = 1;
    int windowed = 0;
    int window_open = 0;
    int gl_ready = 0;
    int audio_ready = 0;
    int scheduler_ready = 0;
    int a64_ready = 0;
    int offline_ready = 0;
    const uint64_t engine_hash = mr_elf_fingerprint(&IMG);
    if (engine_hash != MR_GAME_BINDING_HASH) {
        fprintf(stderr, "ERROR: build-time game bindings do not match this engine\n");
        goto cleanup;
    }
    CPU.mem_host = (uint8_t *)IMG.host_base;
    CPU.mem_guest_base = IMG.guest_base;
    CPU.mem_size = (uint32_t)IMG.span;
    CPU.ro_start = IMG.ro_start;
    CPU.ro_end = IMG.ro_end;
    CPU.exec_start = IMG.exec_start;
    CPU.exec_end = IMG.exec_end;
    CPU.insn_limit = INSN_LIMIT;
    mr_shim_set_exidx(IMG.exidx, IMG.exidx_count);

    // Guest regions immediately above the loaded image.
    uint32_t p = IMG.guest_base + (uint32_t)IMG.image_span;
    p = mr_shim_data_init(&CPU, p);
    mr_gl_set_string_area(p, GL_STRINGS);
    p += GL_STRINGS;
    mr_zlib_set_area(&CPU, p);
    p += 64;
    uint32_t version_str = p;
    p += 16;
    memcpy(mr_mem(&CPU, version_str), GAME_VERSION, sizeof GAME_VERSION);

    const double scale = MR_GRAPHICS_RENDER_SCALE;
    uint32_t window_h = WINDOW_H;
    double aspect = mr_win_display_aspect();
    if (aspect > 0.0) {
        long fitted = lround((double)WINDOW_W / aspect);
        if (fitted > 0) window_h = (uint32_t)fitted;
    }
    uint32_t render_w = scaled_dimension(WINDOW_W, scale);
    uint32_t render_h = scaled_dimension(window_h, scale);

    windowed = parse_nonnegative_int(getenv("MR_WINDOW"), 1, "MR_WINDOW");
    if (windowed < 0 || windowed > 1) {
        fprintf(stderr, "invalid MR_WINDOW: expected 0 or 1\n");
        result = 2;
        goto cleanup;
    }
    window_open = windowed;
    if (windowed && mr_win_open(WINDOW_W, WINDOW_H, render_w, render_h, "Minion Rush") != 0) {
        fprintf(stderr, "ERROR: window or display link cannot start\n");
        goto cleanup;
    }
    if (windowed) mr_win_request_content_rate(MR_GRAPHICS_ENGINE_HZ);

    if (mr_gl_init(render_w, render_h) != 0) {
        fprintf(stderr, "ERROR: cannot open GL context\n");
        goto cleanup;
    }
    gl_ready = 1;

    if (mr_elf_resolve_imports(&IMG, resolve_game_import, NULL) != 0) {
        fprintf(stderr, "ERROR: engine imports cannot be resolved\n");
        goto cleanup;
    }

    uint32_t env = mr_jni_init(&CPU, p, JNI_ARENA, register_thunk, NULL);
    mr_jni_set_gpu_name(mr_gl_renderer());
    p = mr_jni_end();
    p = mr_audio_init(&CPU, p, register_thunk, NULL) + 4096;
    audio_ready = 1;

    mr_elf_relocate(&IMG);
    CPU.thunks = THUNKS;
    CPU.thunk_count = (uint32_t)THUNK_COUNT;

    uint32_t heap_size = GUEST_EXTRA - STACK_POOL;
    mr_heap_init(p, heap_size);
    LOCALIZATION_OBJECT = mr_guest_alloc(&CPU, 12u);
    if (!LOCALIZATION_OBJECT) {
        fprintf(stderr, "ERROR: engine localization storage cannot be allocated\n");
        goto cleanup;
    }
    STACK_TOP = (IMG.guest_base + (uint32_t)IMG.span - 64) & ~7u;
    if (mr_sched_init(&CPU, p + heap_size, STACK_TOP + 64u) != 0) {
        fprintf(stderr, "ERROR: guest-thread scheduler cannot start\n");
        goto cleanup;
    }
    scheduler_ready = 1;
    mr_sched_set_audio_pump(&CPU, mr_audio_pump);

    if (mr_a64_init(&CPU, IMG.exec_end, engine_hash) != 0) {
        fprintf(stderr, "ERROR: static ARM64 code does not match this engine\n");
        goto cleanup;
    }
    a64_ready = 1;
    mr_guest_runtime_reset_stats();
    if (mr_offline_init(&CPU) != 0) {
        fprintf(stderr, "ERROR: offline system cannot start\n");
        goto cleanup;
    }
    offline_ready = 1;

    printf("engine: %.1f MB | ELF imports: %d (%d shim, %d data, %d stubs) | GPU: %s",
           (double)IMG.image_span / (1024 * 1024), IMPORT_COUNT, SHIM_COUNT, DATA_IMPORT_COUNT,
           STUB_IMPORT_COUNT, mr_gl_renderer());
    if (mr_gl_samples())
        printf(" | antialiasing: %dx", mr_gl_samples());
    else
        printf(" | antialiasing: none");
    printf(" | AF: %.0fx | PBO: %s | surface: %ux%u (%.2fx)\n", mr_gl_anisotropy(),
           mr_gl_pbo_enabled() ? "on" : "off", render_w, render_h, scale);
    printf("game data: %s\n", data_root);
    if (mr_data_base()[0]) printf("read base: %s\n", mr_data_base());
    printf("language: %s", mr_localization_code());
    if (mr_localization_current() >= 0) printf(" (engine code %s)", mr_localization_engine_code());
    printf("\n");
    printf("\n");

    double run_started_ms = mr_monotonic_ms();
    if (run_static_constructors() && start_engine(env, version_str, render_w, render_h) == 0) {
        result = run_frames(env, frames, windowed, render_w, render_h) == 0 ? 0 : 1;
    }
    double run_wall_ms = mr_monotonic_ms() - run_started_ms;
    if (report()) result = 1;
    report_io();
    report_cpu_load(run_wall_ms);
    trace_close();

cleanup:
    if (audio_ready) mr_audio_shutdown();
    mr_jni_shutdown();
    if (offline_ready) mr_offline_shutdown();
    if (a64_ready) mr_a64_shutdown();
    if (scheduler_ready) mr_sched_destroy(&CPU);
    if (gl_ready) mr_gl_shutdown();
    if (window_open) mr_win_close();
    for (int i = 0; i < THUNK_COUNT; i++)
        free(NAMES[i]);
    mr_elf_free(&IMG);
    return result;
}
