#ifndef MR_GUEST_RUNTIME_H
#define MR_GUEST_RUNTIME_H

#include "cpu.h"

#include <stddef.h>
#include <stdint.h>

int mr_guest_step_boundary(mr_cpu *cpu);

int mr_guest_block_entry(mr_cpu *cpu);

// Completes the current guest call without executing the original function.
int mr_guest_synthetic_return(mr_cpu *cpu, uint32_t value);

// Runs guest code until stop_addr or failure. max_insn == 0 uses the CPU limit.
int mr_guest_run(mr_cpu *cpu, uint32_t stop_addr, uint64_t max_insn);

// AAPCS guest function call. The return value is r0.
uint32_t mr_guest_call(mr_cpu *cpu, uint32_t func, int argc, const uint32_t *argv);

int mr_guest_read_jet_string(mr_cpu *cpu, uint32_t object, char *out, size_t capacity);

int mr_guest_read_std_string(mr_cpu *cpu, uint32_t object, char *out, size_t capacity);

void mr_guest_runtime_reset_stats(void);
uint64_t mr_guest_host_calls(void);
uint64_t mr_guest_kuser_calls(void);

void mr_guest_clock_reset(void);

void mr_guest_clock_step(double refresh_ms, uint32_t periods);

void mr_guest_clock_begin_render(void);
void mr_guest_clock_end_render(void);

int mr_guest_clock_monotonic(const mr_cpu *cpu, uint64_t *ns);
int mr_guest_clock_realtime(const mr_cpu *cpu, uint64_t *ns);

typedef struct {
    uint64_t monotonic_render, monotonic_outside;
    uint64_t realtime_render, realtime_outside;
    uint64_t reanchors;
} mr_guest_clock_stats;
mr_guest_clock_stats mr_guest_clock_get_stats(void);

typedef struct {
    uint64_t steps;
    uint32_t last_dt;
    uint64_t dt_sum;
    uint32_t dt_min, dt_max;

    uint64_t scaled_steps;
    uint64_t scale_fixed; // Steps whose delta time was adjusted.
    float scale_min, scale_max;
    uint64_t scale_one;
    uint64_t scale_rejected;
} mr_guest_step_stats;

void mr_guest_set_target_fps(uint32_t fps);

typedef struct {
    const char *name;
    uint32_t sessions; // Number of sessions started.
    uint64_t calls;    // Total update calls.
    double active_ms;  // Total measured active time.
    uint64_t last_calls;
    double last_ms; // Measured duration of the last session.

    double min_ms, max_ms;
} mr_guest_gameplay_stat;

#define MR_GUEST_GAMEPLAY_SLOTS 8

const mr_guest_gameplay_stat *mr_guest_gameplay_get_stats(void);

uint64_t mr_guest_fluffy_fixes(void);

void mr_guest_gameplay_flush(void);

uint64_t mr_guest_steps_taken(void);

mr_guest_step_stats mr_guest_step_get_stats(void);

#endif
