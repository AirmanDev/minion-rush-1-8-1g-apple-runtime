#ifndef MR_GUEST_THREADS_H
#define MR_GUEST_THREADS_H

#include "cpu.h"

int mr_sched_init(mr_cpu *main_cpu, uint32_t stack_pool_bottom, uint32_t stack_pool_top);
void mr_sched_destroy(mr_cpu *main_cpu);
int mr_sched_run(mr_cpu *main_cpu, uint32_t main_stop_addr, uint64_t max_insn);

int mr_sched_service_idle(mr_cpu *main_cpu, uint64_t max_insn, uint64_t max_ns,
                          uint64_t *executed_out);

int mr_sched_busy(const mr_cpu *cpu);

int mr_sched_post_callback(mr_cpu *main_cpu, uint32_t *thread_id, uint32_t entry, int argc,
                           const uint32_t *argv);
int mr_sched_cancel_callback(mr_cpu *cpu, uint32_t thread_id);

void mr_sched_set_audio_pump(mr_cpu *main_cpu, void (*pump)(mr_cpu *));

int mr_sched_sleep(mr_cpu *cpu, uint64_t ns);

int mr_sched_live_threads(const mr_cpu *cpu);
uint32_t mr_sched_total_created(const mr_cpu *cpu);
int mr_sched_runnable(const mr_cpu *cpu);
void mr_sched_dump(const mr_cpu *cpu); // Reports each thread's wait state.
uint32_t mr_sched_current_tid(const mr_cpu *cpu);
uint32_t mr_sched_errno_addr(mr_cpu *cpu);
uint32_t mr_sched_tls_addr(mr_cpu *cpu);
void mr_sched_set_errno(mr_cpu *cpu, int value);

mr_thunk_fn mr_thread_shim_lookup(const char *name);

#endif
