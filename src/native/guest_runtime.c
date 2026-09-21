// Guest execution control around the statically compiled ARM64 blocks.
//
// This module handles only ABI boundaries: imported host functions, ARM Linux
// kuser helpers, scheduling and AAPCS calls. It contains no A32/Thumb decoder
// and cannot execute an uncompiled guest instruction.

#include "guest_runtime.h"

#include "a64_runtime.h"
#include "elf_loader.h"
#include "game_bindings.h"
#include "guest_threads.h"
#include "host_time.h"
#include "language_ui.h"
#include "offline_mode.h"
#include "localization.h"
#include "platform_window.h"
#include "safe_area.h"
#include "shim_libc.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t HOST_CALLS;
static uint64_t KUSER_CALLS;

void mr_guest_runtime_reset_stats(void) {
    HOST_CALLS = 0;
    KUSER_CALLS = 0;
}

uint64_t mr_guest_host_calls(void) {
    return HOST_CALLS;
}
uint64_t mr_guest_kuser_calls(void) {
    return KUSER_CALLS;
}

int mr_guest_synthetic_return(mr_cpu *cpu, uint32_t value) {
    uint32_t target = cpu->r[MR_R_LR];
    cpu->r[0] = value;
    cpu->thumb = (int)(target & 1u);
    cpu->itstate = 0;
    cpu->r[MR_R_PC] = target & ~1u;
    return 1;
}

// Frame timeline.

#define CLOCK_GAP_PERIODS 8u

#define CLOCK_LOCK_TOLERANCE 0.05

static mr_guest_step_stats STEP_STATS = {0, 0, 0, UINT32_MAX, 0, 0, 0, INFINITY, 0.0f, 0, 0};

static uint64_t FRAME_NS; // Quantized monotonic timeline.
static uint64_t MONO_ANCHOR_NS;
static uint64_t REAL_ANCHOR_NS;  // Same instant on the real clock.
static uint64_t RENDER_START_NS; // Render-window start on the real clock.
static int CLOCK_ACTIVE;         // Nonzero while the display timeline is valid.
static int RENDER_DEPTH;
static mr_guest_clock_stats CLOCK_STATS;

void mr_guest_clock_reset(void) {
    FRAME_NS = MONO_ANCHOR_NS = RENDER_START_NS = mr_mono_ns();
    REAL_ANCHOR_NS = mr_real_ns();
    CLOCK_ACTIVE = 0;
    RENDER_DEPTH = 0;
    CLOCK_STATS = (mr_guest_clock_stats){0};
}

static uint64_t frame_advance_ns(double refresh_ms) {
    uint64_t refresh_ns = (uint64_t)(refresh_ms * 1e6);
    uint32_t step_ms = STEP_STATS.last_dt;
    if (!step_ms || !(refresh_ms > 0.0)) return refresh_ns;

    long frames = lround((double)step_ms / refresh_ms);
    if (frames < 1) return refresh_ns;
    double advance_ms = (double)step_ms / (double)frames;
    if (fabs(advance_ms - refresh_ms) > refresh_ms * CLOCK_LOCK_TOLERANCE) return refresh_ns;
    return (uint64_t)(advance_ms * 1e6);
}

static void derive_target_fps_byte(double refresh_ms);

void mr_guest_clock_step(double refresh_ms, uint32_t periods) {
    derive_target_fps_byte(refresh_ms);
    if (!CLOCK_ACTIVE || periods > CLOCK_GAP_PERIODS) {
        FRAME_NS = MONO_ANCHOR_NS = mr_mono_ns();
        REAL_ANCHOR_NS = mr_real_ns();
        if (CLOCK_ACTIVE) CLOCK_STATS.reanchors++;
        CLOCK_ACTIVE = 1;
        return;
    }
    FRAME_NS += frame_advance_ns(refresh_ms) * periods;
}

void mr_guest_clock_begin_render(void) {
    if (RENDER_DEPTH++ == 0) RENDER_START_NS = mr_mono_ns();
}

void mr_guest_clock_end_render(void) {
    if (RENDER_DEPTH > 0) RENDER_DEPTH--;
}

#define MR_MAIN_GUEST_TID 1u

static int frame_clock_applies(const mr_cpu *cpu) {
    return CLOCK_ACTIVE && RENDER_DEPTH > 0 && cpu && cpu->guest_tid == MR_MAIN_GUEST_TID;
}

static uint64_t frame_clock_ns(void) {
    uint64_t now = mr_mono_ns();
    return FRAME_NS + (now > RENDER_START_NS ? now - RENDER_START_NS : 0);
}

int mr_guest_clock_monotonic(const mr_cpu *cpu, uint64_t *ns) {
    if (!frame_clock_applies(cpu)) {
        CLOCK_STATS.monotonic_outside++;
        return 0;
    }
    CLOCK_STATS.monotonic_render++;
    *ns = frame_clock_ns();
    return 1;
}

int mr_guest_clock_realtime(const mr_cpu *cpu, uint64_t *ns) {
    if (!frame_clock_applies(cpu)) {
        CLOCK_STATS.realtime_outside++;
        return 0;
    }
    CLOCK_STATS.realtime_render++;
    *ns = REAL_ANCHOR_NS + (frame_clock_ns() - MONO_ANCHOR_NS);
    return 1;
}

mr_guest_clock_stats mr_guest_clock_get_stats(void) {
    return CLOCK_STATS;
}

// Block-entry hook.

uint64_t mr_guest_steps_taken(void) {
    return STEP_STATS.steps;
}
mr_guest_step_stats mr_guest_step_get_stats(void) {
    return STEP_STATS;
}

static void observe_frame_update(const mr_cpu *cpu) {
    uint32_t dt = cpu->r[1];
    STEP_STATS.steps++;
    STEP_STATS.last_dt = dt;
    STEP_STATS.dt_sum += dt;
    if (dt < STEP_STATS.dt_min) STEP_STATS.dt_min = dt;
    if (dt > STEP_STATS.dt_max) STEP_STATS.dt_max = dt;
}

#define GAME_TARGET_FPS_OFFSET 0x3e6u

static uint32_t TARGET_FPS;     // Requested simulation rate; zero leaves it unchanged.
static uint8_t TARGET_FPS_BYTE; // Engine value derived from display timing.
static double STEP_RESIDUAL;

void mr_guest_set_target_fps(uint32_t fps) {
    TARGET_FPS = fps;
    STEP_RESIDUAL = 0.0;
}

static void derive_target_fps_byte(double refresh_ms) {
    if (!TARGET_FPS || !(refresh_ms > 0.0)) return;
    long frames = lround(1000.0 / (double)TARGET_FPS / refresh_ms);
    if (frames < 1) frames = 1;
    long fps = lround(1000.0 / (refresh_ms * (double)frames));
    if (fps < 1) fps = 1;
    TARGET_FPS_BYTE = (uint8_t)(fps > 255 ? 255 : fps);
}

static void apply_target_fps(mr_cpu *cpu) {
    uint32_t game = cpu->r[0];
    if (!TARGET_FPS_BYTE || !game) return;
    uint32_t field = game + GAME_TARGET_FPS_OFFSET;
    if (!mr_mem_ok(cpu, field, 1)) return;
    if (mr_ld8(cpu, field) != TARGET_FPS_BYTE) mr_st8(cpu, field, TARGET_FPS_BYTE);
}

#define GAME_TIME_SCALE_OFFSET 0x1ecu

static void apply_time_step(mr_cpu *cpu) {
    uint32_t game = cpu->r[0];
    if (!TARGET_FPS_BYTE || !game) return;
    uint32_t field = game + GAME_TIME_SCALE_OFFSET;
    if (!mr_mem_ok(cpu, field, 4)) return;

    uint32_t bits = mr_ld32(cpu, field);
    float scale;
    memcpy(&scale, &bits, sizeof scale);
    if (!(scale > 0.0f) || !(scale < 4.0f)) {
        STEP_STATS.scale_rejected++;
        STEP_RESIDUAL = 0.0;
        return;
    }

    uint32_t original_dt = cpu->r[1];
    STEP_RESIDUAL += (1000.0 / (double)TARGET_FPS_BYTE) * (double)scale;
    uint32_t dt = (uint32_t)STEP_RESIDUAL;
    STEP_RESIDUAL -= (double)dt;
    cpu->r[1] = dt;

    if (scale == 1.0f) {
        STEP_STATS.scale_one++;
        return;
    }

    STEP_STATS.scaled_steps++;
    if (scale < STEP_STATS.scale_min) STEP_STATS.scale_min = scale;
    if (scale > STEP_STATS.scale_max) STEP_STATS.scale_max = scale;
    if (dt != original_dt) STEP_STATS.scale_fixed++;
}

#define FLUFFY_VELOCITY_OFFSET 0xf8u // Contiguous V.x, V.y, and V.z.
#define FLUFFY_ACCEL_OFFSET 0x12cu   // Contiguous A.x, A.y, and A.z.
#define FLUFFY_REFERENCE_DT_MS 33.0

static uint64_t FLUFFY_FIXES;

static float guest_read_float(mr_cpu *cpu, uint32_t addr) {
    uint32_t bits = mr_ld32(cpu, addr);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void guest_write_float(mr_cpu *cpu, uint32_t addr, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    mr_st32(cpu, addr, bits);
}

static void fluffy_fix_integration(mr_cpu *cpu) {
    uint32_t obj = cpu->r[0];
    uint32_t dt = STEP_STATS.last_dt;
    if (!obj || !dt) return;

    double ratio = (double)dt / FLUFFY_REFERENCE_DT_MS;
    if (ratio >= 1.0) return;
    double correction = 1.0 - ratio;

    for (uint32_t i = 0; i < 3u; i++) {
        uint32_t v = obj + FLUFFY_VELOCITY_OFFSET + i * 4u;
        uint32_t a = obj + FLUFFY_ACCEL_OFFSET + i * 4u;
        if (!mr_mem_ok(cpu, v, 4) || !mr_mem_ok(cpu, a, 4)) return;
        float accel = guest_read_float(cpu, a);
        if (!isfinite(accel) || accel == 0.0f) continue;
        float vel = guest_read_float(cpu, v);
        if (!isfinite(vel)) continue;
        guest_write_float(cpu, v, (float)((double)vel - (double)accel * correction));
    }
    FLUFFY_FIXES++;
}

uint64_t mr_guest_fluffy_fixes(void) {
    return FLUFFY_FIXES;
}

#define GAMEPLAY_SESSION_GAP_MS 500.0

static mr_guest_gameplay_stat GAMEPLAY[MR_GUEST_GAMEPLAY_SLOTS] = {
    {"CartGameplay", 0, 0, 0.0, 0, 0.0, INFINITY, 0.0},
    {"FluffyGameplay", 0, 0, 0.0, 0, 0.0, INFINITY, 0.0},
    {"MoonGameplay", 0, 0, 0.0, 0, 0.0, INFINITY, 0.0},
    {"RocketGameplay", 0, 0, 0.0, 0, 0.0, INFINITY, 0.0},
    {"BossGameplay", 0, 0, 0.0, 0, 0.0, INFINITY, 0.0},
    {"LargeMinionGameplay", 0, 0, 0.0, 0, 0.0, INFINITY, 0.0},
    {"PowerUpMgr", 0, 0, 0.0, 0, 0.0, INFINITY, 0.0},
    {"Bonus", 0, 0, 0.0, 0, 0.0, INFINITY, 0.0},
};
static double GAMEPLAY_START_MS[MR_GUEST_GAMEPLAY_SLOTS];
static double GAMEPLAY_LAST_MS[MR_GUEST_GAMEPLAY_SLOTS];
static uint64_t GAMEPLAY_SESSION_CALLS[MR_GUEST_GAMEPLAY_SLOTS];

static void gameplay_close(int slot) {
    if (!GAMEPLAY_SESSION_CALLS[slot]) return;
    double span = GAMEPLAY_LAST_MS[slot] - GAMEPLAY_START_MS[slot];
    if (span < 0.0) span = 0.0;
    GAMEPLAY[slot].active_ms += span;
    GAMEPLAY[slot].last_ms = span;
    GAMEPLAY[slot].last_calls = GAMEPLAY_SESSION_CALLS[slot];
    if (span < GAMEPLAY[slot].min_ms) GAMEPLAY[slot].min_ms = span;
    if (span > GAMEPLAY[slot].max_ms) GAMEPLAY[slot].max_ms = span;
    GAMEPLAY_SESSION_CALLS[slot] = 0;
}

static void observe_gameplay(int slot) {
    double now = mr_monotonic_ms();
    if (!GAMEPLAY_SESSION_CALLS[slot] || now - GAMEPLAY_LAST_MS[slot] > GAMEPLAY_SESSION_GAP_MS) {
        gameplay_close(slot);
        GAMEPLAY[slot].sessions++;
        GAMEPLAY_START_MS[slot] = now;
    }
    GAMEPLAY_LAST_MS[slot] = now;
    GAMEPLAY_SESSION_CALLS[slot]++;
    GAMEPLAY[slot].calls++;
}

void mr_guest_gameplay_flush(void) {
    for (int i = 0; i < MR_GUEST_GAMEPLAY_SLOTS; i++)
        gameplay_close(i);
}

const mr_guest_gameplay_stat *mr_guest_gameplay_get_stats(void) {
    return GAMEPLAY;
}

static int gameplay_slot(uint32_t pc) {
    static const uint32_t PC[MR_GUEST_GAMEPLAY_SLOTS] = {
        MR_GAME_GP_CART, MR_GAME_GP_FLUFFY,       MR_GAME_GP_MOON,    MR_GAME_GP_ROCKET,
        MR_GAME_GP_BOSS, MR_GAME_GP_LARGE_MINION, MR_GAME_GP_POWERUP, MR_GAME_GP_BONUS,
    };
    for (int i = 0; i < MR_GUEST_GAMEPLAY_SLOTS; i++)
        if (PC[i] == pc) return i;
    return -1;
}

int mr_guest_block_entry(mr_cpu *cpu) {
    uint32_t pc = cpu->r[MR_R_PC];
    if (pc == MR_GAME_FRAME_UPDATE) {
        apply_time_step(cpu);
        observe_frame_update(cpu);
    } else if (pc == MR_GAME_ON_UPDATE) {
        apply_target_fps(cpu);
    } else if (pc == MR_GAME_FLUFFY_MOVEMENT) {
        fluffy_fix_integration(cpu);
    } else if (pc == MR_GAME_OPTIONS_SETTINGS_ENTER) {
        mr_language_ui_enter(MR_LANGUAGE_PAGE_OPTIONS, cpu->r[0]);
    } else if (pc == MR_GAME_INITIAL_LANGUAGE_ENTER) {
        mr_language_ui_enter(MR_LANGUAGE_PAGE_INITIAL, cpu->r[0]);
    } else if (pc == MR_GAME_OPTIONS_SETTINGS_REFRESH &&
               mr_language_ui_defer_refresh(MR_LANGUAGE_PAGE_OPTIONS, cpu->r[0])) {
        uint32_t target = cpu->r[MR_R_LR];
        cpu->r[0] = 0;
        cpu->thumb = (int)(target & 1u);
        cpu->itstate = 0;
        cpu->r[MR_R_PC] = target & ~1u;
        return 1;
    } else if (pc == MR_GAME_INITIAL_LANGUAGE_REFRESH &&
               mr_language_ui_defer_refresh(MR_LANGUAGE_PAGE_INITIAL, cpu->r[0])) {
        uint32_t target = cpu->r[MR_R_LR];
        cpu->r[0] = 0;
        cpu->thumb = (int)(target & 1u);
        cpu->itstate = 0;
        cpu->r[MR_R_PC] = target & ~1u;
        return 1;
    } else if (pc == MR_GAME_OPTIONS_SETTINGS_LEAVE) {
        mr_language_ui_leave(MR_LANGUAGE_PAGE_OPTIONS, cpu->r[0], 0);
    } else if (pc == MR_GAME_OPTIONS_SETTINGS_DESTROY) {
        mr_language_ui_leave(MR_LANGUAGE_PAGE_OPTIONS, cpu->r[0], 1);
    } else if (pc == MR_GAME_INITIAL_LANGUAGE_LEAVE) {
        mr_language_ui_leave(MR_LANGUAGE_PAGE_INITIAL, cpu->r[0], 1);
    } else if (pc == MR_GAME_MOVIE_MENU_ENTER) {
        mr_win_set_movie_orientation(1);
    } else if (pc == MR_GAME_MOVIE_FINISHED || pc == MR_GAME_MOVIE_MENU_LEAVE) {
        mr_win_set_movie_orientation(0);
    } else if (pc == MR_GAME_OPTIONS_SETTINGS_LANGUAGE || pc == MR_GAME_INITIAL_LANGUAGE_LANGUAGE) {
        mr_language_page kind = pc == MR_GAME_OPTIONS_SETTINGS_LANGUAGE ? MR_LANGUAGE_PAGE_OPTIONS
                                                                        : MR_LANGUAGE_PAGE_INITIAL;
        int selected = mr_language_ui_selection(kind, cpu->r[0], cpu->r[1]);
        if (selected != mr_localization_current() && mr_localization_activate(selected)) {
            mr_shim_path_cache_flush();
            if (getenv("MR_DIAGNOSTICS")) {
                printf("[Localization UI] selected %s from %s\n", mr_localization_code(),
                       kind == MR_LANGUAGE_PAGE_OPTIONS ? "settings" : "initial-language");
            }
        }
    } else {
        int slot = gameplay_slot(pc);
        if (slot >= 0) observe_gameplay(slot);
    }
    if (mr_safe_area_before_block(cpu)) return 1;
    return mr_offline_before_block(cpu);
}

int mr_guest_read_jet_string(mr_cpu *cpu, uint32_t object, char *out, size_t capacity) {
    if (!cpu || !out || capacity < 2) return -1;
    out[0] = '\0';
    if (!object || !mr_mem_ok(cpu, object, 4)) return 0;

    uint32_t data = mr_ld32(cpu, object);
    if (!data || !mr_mem_ok(cpu, data + 8u, 4)) return 0;
    uint32_t chars = mr_ld32(cpu, data + 8u);
    if (!chars || !mr_mem_ok(cpu, chars, 1)) return 0;

    for (size_t i = 0; i < capacity; i++) {
        if (!mr_mem_ok(cpu, chars + (uint32_t)i, 1)) {
            out[0] = '\0';
            return -1;
        }
        out[i] = (char)mr_ld8(cpu, chars + (uint32_t)i);
        if (!out[i]) return 1;
    }
    out[capacity - 1] = '\0';
    return -1;
}

int mr_guest_read_std_string(mr_cpu *cpu, uint32_t object, char *out, size_t capacity) {
    if (!cpu || !out || capacity < 2) return -1;
    out[0] = '\0';
    if (!object || !mr_mem_ok(cpu, object, 4)) return 0;

    uint32_t chars = mr_ld32(cpu, object);
    if (!chars || chars < 12u || !mr_mem_ok(cpu, chars - 12u, 4)) return 0;
    uint32_t length = mr_ld32(cpu, chars - 12u);
    if ((size_t)length >= capacity || !mr_mem_ok(cpu, chars, length + 1u)) return -1;
    for (uint32_t i = 0; i < length; i++)
        out[i] = (char)mr_ld8(cpu, chars + i);
    if (mr_ld8(cpu, chars + length) != 0) {
        out[0] = '\0';
        return -1;
    }
    out[length] = '\0';
    return 1;
}

static void kuser_return(mr_cpu *c) {
    uint32_t ret = c->r[MR_R_LR];
    c->thumb = ret & 1u;
    c->r[MR_R_PC] = ret & ~1u;
}

static int kuser_load64(mr_cpu *c, uint32_t addr, uint64_t *out) {
    uint32_t lo = mr_ld32(c, addr);
    uint32_t hi = mr_ld32(c, addr + 4u);
    if (c->halted) return -1;
    *out = (uint64_t)lo | ((uint64_t)hi << 32);
    return 0;
}

// 1 = handled, 0 = not a supported kuser address, -1 = guest-memory failure.
static int kuser_step(mr_cpu *c) {
    switch (c->r[MR_R_PC]) {
    case MR_KUSER_MEMORY_BARRIER:
        kuser_return(c);
        return 1;

    case MR_KUSER_CMPXCHG: {
        uint32_t expected = c->r[0];
        uint32_t desired = c->r[1];
        uint32_t ptr = c->r[2];
        uint32_t current = mr_ld32(c, ptr);
        if (c->halted) return -1;
        int ok = current == expected;
        if (ok) {
            mr_st32(c, ptr, desired);
            if (c->halted) return -1;
        }
        c->r[0] = ok ? 0u : 1u;
        c->f.c = ok ? 1u : 0u;
        kuser_return(c);
        return 1;
    }

    case MR_KUSER_CMPXCHG64: {
        uint64_t expected, desired, current;
        if (kuser_load64(c, c->r[0], &expected) != 0 || kuser_load64(c, c->r[1], &desired) != 0 ||
            kuser_load64(c, c->r[2], &current) != 0)
            return -1;
        int ok = current == expected;
        if (ok) {
            mr_st32(c, c->r[2], (uint32_t)desired);
            mr_st32(c, c->r[2] + 4u, (uint32_t)(desired >> 32));
            if (c->halted) return -1;
        }
        c->r[0] = ok ? 0u : 1u;
        c->f.c = ok ? 1u : 0u;
        kuser_return(c);
        return 1;
    }

    case MR_KUSER_GET_TLS:
        c->r[0] = mr_sched_tls_addr(c);
        kuser_return(c);
        return 1;

    default:
        return 0;
    }
}

uint32_t mr_load_word_slow(mr_cpu *c, uint32_t addr) {
    if (addr == MR_KUSER_TLS_WORD) return mr_sched_tls_addr(c);
    if (addr == MR_KUSER_HELPER_VERSION) return MR_KUSER_VERSION_VALUE;
    c->fault = "invalid read";
    c->fault_addr = addr;
    c->halted = 1;
    return 0;
}

static int step_host_import(mr_cpu *c, uint32_t pc) {
    uint32_t idx = MR_THUNK_INDEX(pc);
    if (idx >= c->thunk_count || !c->thunks[idx].fn) {
        c->fault = "call to unresolved import";
        c->fault_addr = pc;
        c->halted = 1;
        return -1;
    }

    c->thunk_redirect = 0;
    c->thunk_blocked = 0;
    errno = 0;
    c->thunks[idx].fn(c);
    if (errno && c->scheduler) mr_sched_set_errno(c, errno);
    c->insn_count++;
    HOST_CALLS++;

    // A blocked pthread/cond/sleep thunk remains at its own address. The
    // scheduler switches threads and retries it after wake-up.
    if (c->thunk_blocked) return 0;

    uint32_t ret = c->thunk_redirect ? c->thunk_redirect : c->r[MR_R_LR];
    c->thumb = ret & 1u;
    c->r[MR_R_PC] = ret & ~1u;
    return 0;
}

int mr_guest_step_boundary(mr_cpu *c) {
    if (!c || c->halted) return -1;

    uint32_t pc = c->r[MR_R_PC];
    if (pc < MR_THUNK_BASE) {
        c->fault = "no statically translated ARM64 block";
        c->fault_addr = pc;
        c->halted = 1;
        return -1;
    }

    if (pc >= 0xFFFF0000u) {
        int rc = kuser_step(c);
        if (rc != 0) {
            c->insn_count++;
            if (rc > 0) KUSER_CALLS++;
            return rc < 0 ? -1 : 0;
        }
    }

    return step_host_import(c, pc);
}

int mr_guest_run(mr_cpu *c, uint32_t stop_addr, uint64_t max_insn) {
    if (c->scheduler && !mr_sched_busy(c)) return mr_sched_run(c, stop_addr, max_insn);

    uint64_t start = c->insn_count;
    uint64_t limit = max_insn ? max_insn : c->insn_limit;
    while (!c->halted) {
        if (c->r[MR_R_PC] == stop_addr) return 0;
        if (limit && c->insn_count - start >= limit) {
            c->fault = "instruction limit reached";
            c->fault_addr = c->r[MR_R_PC];
            c->halted = 1;
            return -1;
        }

        uint32_t ran = mr_a64_run(c, 0);
        if (ran) {
            c->insn_count += ran;
            continue;
        }
        if (c->halted || mr_guest_step_boundary(c) != 0) return -1;
        if (c->thunk_blocked) {
            c->fault = "nested guest call is blocked";
            c->fault_addr = c->r[MR_R_PC];
            c->halted = 1;
            return -1;
        }
    }
    return -1;
}

#define MR_RETURN_MAGIC 0xFFFF0000u

uint32_t mr_guest_call(mr_cpu *c, uint32_t func, int argc, const uint32_t *argv) {
    const int nested = c->scheduler && mr_sched_busy(c);
    const uint32_t saved_pc = c->r[MR_R_PC];
    const uint32_t saved_lr = c->r[MR_R_LR];
    const uint32_t saved_sp = c->r[MR_R_SP];
    const uint32_t saved_itstate = c->itstate;
    const uint32_t saved_redirect = c->thunk_redirect;
    const int saved_thumb = c->thumb;
    const int saved_blocked = c->thunk_blocked;

    for (int i = 0; i < argc && i < 4; i++)
        c->r[i] = argv[i];
    if (argc > 4) {
        uint32_t extra = (uint32_t)(argc - 4);
        c->r[MR_R_SP] -= extra * 4;
        c->r[MR_R_SP] &= ~7u;
        for (uint32_t i = 0; i < extra; i++)
            mr_st32(c, c->r[MR_R_SP] + i * 4, argv[4 + i]);
    }

    c->r[MR_R_LR] = MR_RETURN_MAGIC;
    c->thumb = func & 1u;
    c->r[MR_R_PC] = func & ~1u;
    mr_guest_run(c, MR_RETURN_MAGIC, 0);
    uint32_t result = c->r[0];
    c->r[MR_R_SP] = saved_sp;

    if (nested && !c->fault) {
        c->r[MR_R_PC] = saved_pc;
        c->r[MR_R_LR] = saved_lr;
        c->thumb = saved_thumb;
        c->itstate = saved_itstate;
        c->thunk_redirect = saved_redirect;
        c->thunk_blocked = saved_blocked;
        c->r[0] = result;
    }
    return result;
}
