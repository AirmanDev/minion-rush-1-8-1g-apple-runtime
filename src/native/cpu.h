// Guest ARM32 CPU state and memory access shared by the static compiler,
// generated ARM64 blocks, scheduler and host ABI bridges.

#ifndef MR_CPU_H
#define MR_CPU_H

#include <stdint.h>
#include <string.h>

// ARM Linux kuser helper page with stable ABI addresses.
#define MR_KUSER_CMPXCHG64 0xFFFF0F60u
#define MR_KUSER_MEMORY_BARRIER 0xFFFF0FA0u
#define MR_KUSER_CMPXCHG 0xFFFF0FC0u
#define MR_KUSER_GET_TLS 0xFFFF0FE0u
#define MR_KUSER_TLS_WORD 0xFFFF0FF0u
#define MR_KUSER_HELPER_VERSION 0xFFFF0FFCu
#define MR_KUSER_VERSION_VALUE 5u

enum { MR_R_SP = 13, MR_R_LR = 14, MR_R_PC = 15 };

typedef struct {
    union {
        struct {
            uint32_t pad : 28, v : 1, c : 1, z : 1, n : 1;
        };
        uint32_t nzcv;
    };
    uint32_t q; // Saturation bit.
} mr_flags;

typedef struct mr_cpu mr_cpu;
typedef struct mr_scheduler mr_scheduler;

typedef void (*mr_thunk_fn)(mr_cpu *cpu);

typedef struct {
    const char *name;
    mr_thunk_fn fn;
} mr_thunk;

struct mr_cpu {
    uint32_t r[16];
    mr_flags f;

    union {
        uint64_t d[32];
        uint32_t s[64];
        uint8_t b[256];
    } v;
    uint32_t fpscr;

    // Guest memory: host = mem_host + (guest - mem_guest_base).
    uint8_t *mem_host;
    uint32_t mem_guest_base;
    uint32_t mem_size;

    uint32_t ro_start, ro_end;
    uint32_t exec_start, exec_end;

    // Imported host functions.
    const mr_thunk *thunks;
    uint32_t thunk_count;

    uint32_t thunk_redirect;

    int thumb;
    uint32_t itstate;

    uint32_t call_ring[256];
    uint32_t call_ring_pos;

    mr_scheduler *scheduler;
    uint32_t guest_tid;
    int thunk_blocked;
    int yield_requested;

    int halted; // Nonzero after execution stops.
    const char *fault;
    uint32_t fault_addr; // Address of the invalid access.
    uint32_t a64_slow_pc;
    uint32_t a64_slow_addr;
    uint64_t insn_count;

    uint64_t insn_limit;
};

#define MR_CALL_RING 256

#define MR_NULL_PAGE 0x1000u

static inline int mr_mem_ok(const mr_cpu *c, uint32_t addr, uint32_t len) {
    if (addr < MR_NULL_PAGE) return 0;
    uint32_t off = addr - c->mem_guest_base;
    return off <= c->mem_size && len <= c->mem_size - off;
}

static inline void *mr_mem(mr_cpu *c, uint32_t addr) {
    return c->mem_host + (addr - c->mem_guest_base);
}

static inline const char *mr_guest_cstrn(mr_cpu *c, uint32_t addr, size_t limit) {
    static const char EMPTY[] = "";
    if (!limit) return EMPTY;
    if (!mr_mem_ok(c, addr, 1)) {
        if (!c->fault) {
            c->fault = "invalid guest string";
            c->fault_addr = addr;
        }
        c->halted = 1;
        return EMPTY;
    }

    const char *value = (const char *)mr_mem(c, addr);
    size_t available = c->mem_size - (size_t)(addr - c->mem_guest_base);
    size_t scan = limit < available ? limit : available;
    if (memchr(value, '\0', scan) || scan == limit) return value;

    if (!c->fault) {
        c->fault = "unterminated guest string";
        c->fault_addr = addr;
    }
    c->halted = 1;
    return EMPTY;
}

static inline const char *mr_guest_cstr(mr_cpu *c, uint32_t addr) {
    return mr_guest_cstrn(c, addr, SIZE_MAX);
}

uint32_t mr_load_word_slow(mr_cpu *cpu, uint32_t addr);

static inline uint32_t mr_ld32(mr_cpu *c, uint32_t a) {
    if (!mr_mem_ok(c, a, 4)) return mr_load_word_slow(c, a);
    uint32_t x;
    memcpy(&x, mr_mem(c, a), 4);
    return x;
}
static inline float mr_ldf32(mr_cpu *c, uint32_t a) {
    uint32_t bits = mr_ld32(c, a);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}
static inline uint32_t mr_guest_arg32(mr_cpu *c, unsigned index) {
    if (index < 4u) return c->r[index];
    return mr_ld32(c, c->r[MR_R_SP] + (index - 4u) * 4u);
}
static inline uint8_t mr_ld8(mr_cpu *c, uint32_t a) {
    if (!mr_mem_ok(c, a, 1)) {
        c->fault = "invalid read";
        c->fault_addr = a;
        c->halted = 1;
        return 0;
    }
    return *(uint8_t *)mr_mem(c, a);
}
static inline int mr_ro_hit(const mr_cpu *c, uint32_t a, uint32_t len) {
    return c->ro_end > c->ro_start && a < c->ro_end && a + len > c->ro_start;
}

static inline void mr_st32(mr_cpu *c, uint32_t a, uint32_t x) {
    if (!mr_mem_ok(c, a, 4)) {
        c->fault = "invalid write";
        c->fault_addr = a;
        c->halted = 1;
        return;
    }
    if (mr_ro_hit(c, a, 4)) {
        c->fault = "write to read-only region";
        c->fault_addr = a;
        c->halted = 1;
        return;
    }
    memcpy(mr_mem(c, a), &x, 4);
}
static inline void mr_stf32(mr_cpu *c, uint32_t a, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    mr_st32(c, a, bits);
}
static inline void mr_st16(mr_cpu *c, uint32_t a, uint16_t x) {
    if (!mr_mem_ok(c, a, 2)) {
        c->fault = "invalid write";
        c->fault_addr = a;
        c->halted = 1;
        return;
    }
    if (mr_ro_hit(c, a, 2)) {
        c->fault = "write to read-only region";
        c->fault_addr = a;
        c->halted = 1;
        return;
    }
    memcpy(mr_mem(c, a), &x, 2);
}
static inline void mr_st8(mr_cpu *c, uint32_t a, uint8_t x) {
    if (!mr_mem_ok(c, a, 1)) {
        c->fault = "invalid write";
        c->fault_addr = a;
        c->halted = 1;
        return;
    }
    if (mr_ro_hit(c, a, 1)) {
        c->fault = "write to read-only region";
        c->fault_addr = a;
        c->halted = 1;
        return;
    }
    *(uint8_t *)mr_mem(c, a) = x;
}

#endif
