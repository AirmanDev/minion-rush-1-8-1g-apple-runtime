#ifndef MR_SHIM_LIBC_H
#define MR_SHIM_LIBC_H

#include "cpu.h"

typedef struct {
    const char *name;
    mr_thunk_fn fn;
} mr_shim;

// Initializes the heap using guest addresses.
void mr_heap_init(uint32_t base, uint32_t size);
uint32_t mr_guest_alloc(mr_cpu *cpu, uint32_t size);
void mr_guest_free(mr_cpu *cpu, uint32_t ptr);

uint64_t mr_blocked_network_calls(void);

mr_thunk_fn mr_shim_lookup(const char *name);

uint32_t mr_shim_data_init(mr_cpu *cpu, uint32_t base);
uint32_t mr_shim_data_lookup(const char *name);

void mr_set_data_root(const char *path);

void mr_set_data_base(const char *path);

void mr_shim_io_stats(uint64_t *calls, uint64_t *bytes, double *ms);
void mr_shim_open_stats(uint64_t *calls, double *ms);
void mr_shim_path_cache_stats(uint64_t *hits, uint64_t *misses);
void mr_shim_path_cache_flush(void);
const char *mr_data_base(void);

#define MR_GUEST_ROOT "/gd"

// zlib bridge to the host library.
mr_thunk_fn mr_zlib_lookup(const char *name);
void mr_zlib_set_area(mr_cpu *cpu, uint32_t base);

void mr_shim_set_exidx(uint32_t addr, uint32_t count);

#endif
