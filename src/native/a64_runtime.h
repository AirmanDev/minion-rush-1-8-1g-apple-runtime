#ifndef MR_A64_RUNTIME_H
#define MR_A64_RUNTIME_H

#include "cpu.h"

#include <stdint.h>

// Maximum static guest-block size and default runtime batch size.
#define MR_A64_MAX_BLOCK 128u
#define MR_A64_DEFAULT_BATCH 8192u

// ABI of every generated block. The fourth argument is the number of guest
// instructions already executed on the current direct chain; the fifth is the
// per-entry instruction budget. Runtime entry passes zero and the remaining
// budget. Every linked edge checks the budget before entering its successor.
typedef uint32_t (*mr_a64_block_fn)(mr_cpu *, uint8_t *, uint32_t, uint32_t, uint32_t);

int mr_a64_init(mr_cpu *cpu, uint32_t code_end, uint64_t fingerprint);
void mr_a64_shutdown(void);

uint32_t mr_a64_run(mr_cpu *cpu, uint32_t budget);

void mr_a64_set_dynamic_hook(uint32_t pc, int on);

uint64_t mr_a64_native_insns(void);
int mr_a64_report(void);

#endif
