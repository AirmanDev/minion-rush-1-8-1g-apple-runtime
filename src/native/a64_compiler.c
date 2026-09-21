// Build-time, game-specific A32/Thumb -> ARM64 block compiler.
//
// Every executable guest entry used at runtime must have a generated block.
// Unsupported static candidates are reported during discovery. The runtime
// executes generated blocks only.

#include "a64_runtime.h"
#include "elf_loader.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CODE_CAPACITY (256u * 1024u * 1024u)
#define BLOCK_ROOM 16384u
#define MAX_BLOCK MR_A64_MAX_BLOCK
#define THUMB_SLOTS 32768u
#define VENEER_POOL_TRIGGER (64u * 1024u * 1024u)
#define VENEER_CAPACITY 32768u
#define VENEER_WORDS 3u

#define BIT(x, n) (((x) >> (n)) & 1u)
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

typedef struct {
    uint32_t pc;
    mr_a64_block_fn fn;
} thumb_slot;

typedef struct {
    uint8_t *code;
    size_t used;
    size_t capacity;
    mr_a64_block_fn *blocks;
    uint32_t block_count;
    thumb_slot *thumb_blocks;
    mr_cpu *cpu;
} a64_state;

typedef struct {
    size_t branch_offset;
    uint32_t target_pc;
    uint8_t target_thumb;
} direct_link;

typedef struct {
    uint32_t *start;
    uint32_t *out;
    uint32_t *end;
    mr_cpu *cpu;
    uint32_t count;
    int ended;
    int ended_uncond;
    int has_direct_target;
    uint32_t direct_target;
    int direct_target_thumb;
    int is_call;
    int chained;
    int failed;
    size_t direct_link_mark;
} emitter;

static a64_state J;
static direct_link *DIRECT_LINKS;
static size_t DIRECT_LINK_COUNT;
static size_t DIRECT_LINK_CAPACITY;
static size_t DIRECT_LINKED;
static size_t DIRECT_LINKED_LONG;
static size_t DIRECT_LINK_SKIPPED_HOOK;
static size_t DIRECT_LINK_SKIPPED_RANGE;
static size_t VENEER_POOL_OFFSET = SIZE_MAX;
static size_t VENEER_USED;
static int RECORD_DIRECT_LINKS;

_Static_assert(sizeof(mr_a64_block_fn) == sizeof(uint8_t *),
               "code and function pointers must have equal size");

static mr_a64_block_fn block_from_code(uint8_t *code) {
    mr_a64_block_fn function = NULL;
    memcpy(&function, &code, sizeof function);
    return function;
}

static uint8_t *code_from_block(mr_a64_block_fn function) {
    uint8_t *code = NULL;
    memcpy(&code, &function, sizeof code);
    return code;
}

// The runtime is tied to the validated Minion Rush 1.8.1g engine, but the
// host platform is not.  Game entry points are therefore resolved once at
// build time from the supplied ELF and emitted as a generated header.  The
// runtime never scans symbol names for game logic.
enum {
    GAME_REQUIRED_SYMBOL_COUNT = 0
#define MR_GAME_SYMBOL(id, name) +1
#define MR_GAME_HOOK_SYMBOL(id, name) +1
#define MR_BLOCKED_SYMBOL(name)
#define MR_BLOCKED_PREFIX(prefix)
#include "game_symbols.def"
#undef MR_GAME_SYMBOL
#undef MR_GAME_HOOK_SYMBOL
#undef MR_BLOCKED_SYMBOL
#undef MR_BLOCKED_PREFIX
};

static const char *const GAME_BLOCKED_SYMBOLS[] = {
#define MR_GAME_SYMBOL(id, name)
#define MR_GAME_HOOK_SYMBOL(id, name)
#define MR_BLOCKED_SYMBOL(name) name,
#define MR_BLOCKED_PREFIX(prefix)
#include "game_symbols.def"
#undef MR_GAME_SYMBOL
#undef MR_GAME_HOOK_SYMBOL
#undef MR_BLOCKED_SYMBOL
#undef MR_BLOCKED_PREFIX
};

static const char *const GAME_BLOCKED_PREFIXES[] = {
#define MR_GAME_SYMBOL(id, name)
#define MR_GAME_HOOK_SYMBOL(id, name)
#define MR_BLOCKED_SYMBOL(name)
#define MR_BLOCKED_PREFIX(prefix) prefix,
#include "game_symbols.def"
#undef MR_GAME_SYMBOL
#undef MR_GAME_HOOK_SYMBOL
#undef MR_BLOCKED_SYMBOL
#undef MR_BLOCKED_PREFIX
};

static const char *const GAME_HOOK_SYMBOLS[] = {
#define MR_GAME_SYMBOL(id, name)
#define MR_GAME_HOOK_SYMBOL(id, name) name,
#define MR_BLOCKED_SYMBOL(name)
#define MR_BLOCKED_PREFIX(prefix)
#include "game_symbols.def"
#undef MR_GAME_SYMBOL
#undef MR_GAME_HOOK_SYMBOL
#undef MR_BLOCKED_SYMBOL
#undef MR_BLOCKED_PREFIX
};

enum {
    X9 = 0,  // x0: mr_cpu *
    X10 = 1, // x1: mem_host
    X8 = 2,  // w2: mem_size
    X11 = 11,
    X12,
    X13,
    X14,
    X15,
    X16,
    XZR = 31
};

static void emit(emitter *e, uint32_t x) {
    if (e->out >= e->end) {
        e->failed = 1;
        return;
    }
    *e->out++ = x;
}

static void patch_b19(uint32_t *at, uint32_t *to) {
    int64_t d = to - at;
    *at = (*at & ~0x00FFFFE0u) | (((uint32_t)d & 0x7FFFFu) << 5);
}

static int patch_b26(uint32_t *at, uint32_t *to) {
    int64_t words = to - at;
    if (words < -(1ll << 25) || words >= (1ll << 25)) return 0;
    *at = 0x14000000u | ((uint32_t)words & 0x03FFFFFFu);
    return 1;
}

static int patch_long_branch(uint32_t *at, uint32_t *to) {
    uintptr_t from = (uintptr_t)at;
    uintptr_t target = (uintptr_t)to;
    int64_t pages =
        ((int64_t)(target & ~(uintptr_t)0xFFFu) - (int64_t)(from & ~(uintptr_t)0xFFFu)) >> 12;
    if (pages < -(1ll << 20) || pages >= (1ll << 20)) return 0;

    uint32_t immlo = (uint32_t)pages & 3u;
    uint32_t immhi = ((uint32_t)pages >> 2) & 0x7FFFFu;
    uint32_t pageoff = (uint32_t)(target & 0xFFFu);
    at[0] = 0x90000000u | (immlo << 29) | (immhi << 5) | (uint32_t)X16;
    at[1] = 0x91000000u | (pageoff << 10) | ((uint32_t)X16 << 5) | (uint32_t)X16;
    at[2] = 0xD61F0000u | ((uint32_t)X16 << 5);
    return 1;
}

static void reserve_veneer_pool(void) {
    if (VENEER_POOL_OFFSET != SIZE_MAX) return;
    size_t bytes = (size_t)VENEER_CAPACITY * VENEER_WORDS * sizeof(uint32_t);
    size_t aligned = (J.used + 15u) & ~(size_t)15u;
    if (aligned + bytes > J.capacity) {
        fprintf(stderr, "no space for far-branch veneers\n");
        exit(1);
    }
    memset(J.code + J.used, 0, aligned - J.used);
    VENEER_POOL_OFFSET = aligned;
    uint32_t *pool = (uint32_t *)(void *)(J.code + aligned);
    for (size_t i = 0; i < (size_t)VENEER_CAPACITY * VENEER_WORDS; i++)
        pool[i] = 0xD4200000u;
    J.used = aligned + bytes;
}

static uint32_t *allocate_veneer(uint32_t *target) {
    if (VENEER_POOL_OFFSET == SIZE_MAX || VENEER_USED >= VENEER_CAPACITY) return NULL;
    uint32_t *pool = (uint32_t *)(void *)(J.code + VENEER_POOL_OFFSET);
    uint32_t *veneer = pool + VENEER_USED * VENEER_WORDS;
    if (!patch_long_branch(veneer, target)) return NULL;
    VENEER_USED++;
    return veneer;
}

static void mov_w_imm(emitter *e, int rd, uint32_t v) {
    emit(e, 0x52800000u | ((v & 0xFFFFu) << 5) | (uint32_t)rd);
    if (v >> 16) emit(e, 0x72A00000u | ((v >> 16) << 5) | (uint32_t)rd);
}

static void ldr_w(emitter *e, int rt, int rn, size_t off) {
    emit(e, 0xB9400000u | ((uint32_t)(off / 4) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}

static void str_w(emitter *e, int rt, int rn, size_t off) {
    emit(e, 0xB9000000u | ((uint32_t)(off / 4) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}

static void guest_load(emitter *e, int dst, uint32_t r, uint32_t pc) {
    if (r == 15)
        mov_w_imm(e, dst, pc + 8);
    else
        ldr_w(e, dst, X9, offsetof(mr_cpu, r) + r * 4u);
}

static void guest_store(emitter *e, int src, uint32_t r) {
    str_w(e, src, X9, offsetof(mr_cpu, r) + r * 4u);
}

static void set_pc_imm(emitter *e, uint32_t pc) {
    mov_w_imm(e, X11, pc);
    guest_store(e, X11, MR_R_PC);
}

static void add_count(emitter *e, uint32_t n) {
    emit(e, 0x11000000u | (n << 10) | (3u << 5) | 3u); // add w3, w3, #n
}

static void return_count(emitter *e, uint32_t n) {
    emit(e, 0x11000000u | (n << 10) | (3u << 5) | 0u); // add w0, w3, #n
    emit(e, 0xD65F03C0u);                              // ret
}

static void return_accumulated(emitter *e) {
    emit(e, 0x2A0303E0u); // mov w0, w3
    emit(e, 0xD65F03C0u); // ret
}

static void remember_direct_link(uint32_t *branch, uint32_t target_pc, int target_thumb) {
    if (!RECORD_DIRECT_LINKS) return;
    if (DIRECT_LINK_COUNT == DIRECT_LINK_CAPACITY) {
        size_t next = DIRECT_LINK_CAPACITY ? DIRECT_LINK_CAPACITY * 2u : (1u << 18);
        direct_link *grown = realloc(DIRECT_LINKS, next * sizeof *grown);
        if (!grown) {
            perror("realloc");
            exit(1);
        }
        DIRECT_LINKS = grown;
        DIRECT_LINK_CAPACITY = next;
    }
    DIRECT_LINKS[DIRECT_LINK_COUNT++] = (direct_link){
        .branch_offset = (size_t)((uint8_t *)branch - J.code),
        .target_pc = target_pc,
        .target_thumb = target_thumb != 0,
    };
}

static void direct_exit(emitter *e, uint32_t target_pc, int target_thumb, uint32_t count) {
    add_count(e, count);
    emit(e, 0x6B04007Fu); // cmp w3, w4
    uint32_t *over_budget = e->out;
    emit(e, 0x54000002u); // b.hs fallback
    uint32_t *branch = e->out;
    emit(e, 0x14000000u); // b fallback; patched to the target or veneer later
    uint32_t *fallback = e->out;
    set_pc_imm(e, target_pc);
    return_accumulated(e);
    patch_b19(over_budget, fallback);
    (void)patch_b26(branch, fallback);
    remember_direct_link(branch, target_pc, target_thumb);
}

static void chain_forward(emitter *e, uint32_t target_pc, uint32_t count) {
    add_count(e, count);
    emit(e, 0x6B04007Fu); // cmp w3, w4
    uint32_t *continue_chain = e->out;
    emit(e, 0x54000003u); // b.lo continue
    set_pc_imm(e, target_pc);
    return_accumulated(e);
    patch_b19(continue_chain, e->out);
}

static void shift_imm(emitter *e, int rd, int rn, uint32_t type, uint32_t n) {
    n &= 31u;
    switch (type) {
    case 0: // LSL
        if (!n) {
            emit(e, 0x2A0003E0u | ((uint32_t)rn << 16) | (uint32_t)rd);
        } else {
            emit(e, 0x53000000u | (((32u - n) & 31u) << 16) | ((31u - n) << 10) |
                        ((uint32_t)rn << 5) | (uint32_t)rd);
        }
        break;
    case 1: // LSR
        emit(e, 0x53000000u | (n << 16) | (31u << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
        break;
    case 2: // ASR
        emit(e, 0x13000000u | (n << 16) | (31u << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
        break;
    default: // ROR
        emit(e,
             0x13800000u | ((uint32_t)rn << 16) | (n << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
        break;
    }
}

static void set_nz(emitter *e, int reg);

// Flags use the ARM64 NZCV layout everywhere.
#define F_OFF offsetof(mr_cpu, f.nzcv)

// Callers consume the C flag as zero or one.
static void carry_to_reg(emitter *e, int reg) {
    ldr_w(e, reg, X9, F_OFF);
    emit(e, 0x53000000u | (29u << 16) | (29u << 10) // ubfx reg, reg, #29, #1
                | ((uint32_t)reg << 5) | (uint32_t)reg);
}

// Insert a zero-or-one value into C without changing the other flags.
static void carry_from_reg(emitter *e, int src, int scratch) {
    ldr_w(e, scratch, X9, F_OFF);
    emit(e, 0x33000000u | (3u << 16) | (0u << 10) // bfi scratch, src, #29, #1
                | ((uint32_t)src << 5) | (uint32_t)scratch);
    str_w(e, scratch, X9, F_OFF);
}

static int operand2_ok(uint32_t insn, int need_carry) {
    if (BIT(insn, 25)) return 1;
    if (BIT(insn, 4)) return 1;
    if (BITS(insn, 6, 5) == 3 && !BITS(insn, 11, 7)) return !need_carry;
    return 1;
}

static void apply_shift_imm(emitter *e, int dst, uint32_t type, uint32_t n) {
    if ((type == 1 || type == 2) && !n) n = 32;
    if (type == 3 && !n) {
        carry_to_reg(e, X14);
        shift_imm(e, dst, dst, 1, 1);
        shift_imm(e, X14, X14, 0, 31);
        emit(e, 0x2A000000u | ((uint32_t)X14 << 16) | ((uint32_t)dst << 5) | (uint32_t)dst);
    } else if (n) {
        shift_imm(e, dst, dst, type, n);
    }
}

static void operand2(emitter *e, uint32_t insn, uint32_t pc, int dst) {
    if (BIT(insn, 25)) {
        uint32_t imm = BITS(insn, 7, 0);
        uint32_t rot = BITS(insn, 11, 8) * 2u;
        uint32_t v = rot ? (imm >> rot) | (imm << (32u - rot)) : imm;
        mov_w_imm(e, dst, v);
        return;
    }

    uint32_t rm = BITS(insn, 3, 0);
    uint32_t type = BITS(insn, 6, 5);
    guest_load(e, dst, rm, pc);
    if (BIT(insn, 4)) {
        guest_load(e, X14, BITS(insn, 11, 8), pc);
        emit(e, 0x12001C00u | ((uint32_t)X14 << 5) | X14); // and w14,w14,#255
        if (type == 2) shift_imm(e, X15, dst, 2, 31);      // asr w15,dst,#31
        static const uint32_t SHIFTV[] = {0x1AC02000u, 0x1AC02400u, 0x1AC02800u, 0x1AC02C00u};
        emit(e, SHIFTV[type] | ((uint32_t)X14 << 16) | ((uint32_t)dst << 5) | (uint32_t)dst);
        if (type != 3) {
            uint32_t big = type == 2 ? (uint32_t)X15 : 31u;            // ASR sign, otherwise zero
            emit(e, 0x7100001Fu | (32u << 10) | ((uint32_t)X14 << 5)); // cmp w14,#32
            emit(e, 0x1A800000u | (big << 16) | (3u << 12)             // csel lo
                        | ((uint32_t)dst << 5) | (uint32_t)dst);
        }
    } else {
        apply_shift_imm(e, dst, type, BITS(insn, 11, 7));
    }
}

static void ldst_offset(emitter *e, uint32_t insn, uint32_t pc, int dst) {
    uint32_t rm = BITS(insn, 3, 0);
    uint32_t type = BITS(insn, 6, 5);
    guest_load(e, dst, rm, pc);
    apply_shift_imm(e, dst, type, BITS(insn, 11, 7));
}

static void save_reg_shift_carry(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t type = BITS(insn, 6, 5);
    guest_load(e, X15, BITS(insn, 3, 0), pc);          // Original Rm.
    guest_load(e, X14, BITS(insn, 11, 8), pc);         // Rs
    emit(e, 0x12001C00u | ((uint32_t)X14 << 5) | X14); // and w14,w14,#255

    if (type == 0) { // LSL
        mov_w_imm(e, X12, 32);
        emit(e, 0x4B000000u | ((uint32_t)X14 << 16) | ((uint32_t)X12 << 5) | X12); // 32-n
        emit(e, 0x1AC02400u | ((uint32_t)X12 << 16) | ((uint32_t)X15 << 5) | X12); // lsr Rm,(32-n)
        emit(e, 0x7100001Fu | (32u << 10) | ((uint32_t)X14 << 5));
        emit(e, 0x1A800000u | ((uint32_t)XZR << 16) | (9u << 12) | ((uint32_t)X12 << 5) |
                    X12);                                  // n<=32 ? bit : 0
    } else if (type == 1) {                                // LSR
        emit(e, 0x51000400u | ((uint32_t)X14 << 5) | X12); // n-1
        emit(e, 0x1AC02400u | ((uint32_t)X12 << 16) | ((uint32_t)X15 << 5) | X12);
        emit(e, 0x7100001Fu | (32u << 10) | ((uint32_t)X14 << 5));
        emit(e, 0x1A800000u | ((uint32_t)XZR << 16) | (9u << 12) | ((uint32_t)X12 << 5) | X12);
    } else if (type == 2) {                                // ASR
        emit(e, 0x51000400u | ((uint32_t)X14 << 5) | X12); // n-1
        emit(e, 0x1AC02400u | ((uint32_t)X12 << 16) | ((uint32_t)X15 << 5) | X12);
        shift_imm(e, X11, X15, 1, 31); // Sign bit.
        emit(e, 0x7100001Fu | (32u << 10) | ((uint32_t)X14 << 5));
        emit(e, 0x1A800000u | ((uint32_t)X11 << 16) | (3u << 12) | ((uint32_t)X12 << 5) |
                    X12); // n<32 ? bit : sign
    } else {              // ROR
        mov_w_imm(e, X11, 31);
        emit(e, 0x0A000000u | ((uint32_t)X11 << 16) | ((uint32_t)X14 << 5) | X11); // rot=n&31
        emit(e, 0x51000400u | ((uint32_t)X11 << 5) | X12);                         // rot-1
        emit(e, 0x1AC02400u | ((uint32_t)X12 << 16) | ((uint32_t)X15 << 5) | X12);
        shift_imm(e, X15, X15, 1, 31);
        emit(e, 0x7100001Fu | ((uint32_t)X11 << 5)); // cmp rot,#0
        emit(e, 0x1A800000u | ((uint32_t)X12 << 16) | ((uint32_t)X15 << 5) |
                    X12); // rot==0 ? bit31 : bit
    }

    // A zero register-shift amount preserves C.
    carry_to_reg(e, X11);
    emit(e, 0x7100001Fu | ((uint32_t)X14 << 5));
    emit(e, 0x1A800000u | ((uint32_t)X11 << 16) | (1u << 12) | ((uint32_t)X12 << 5) |
                X12); // Select the new value only when n is nonzero.
    carry_from_reg(e, X12, X11);
}

static void save_logic_flags(emitter *e, uint32_t insn, uint32_t pc, int shifted, int result) {
    set_nz(e, result);
    if (!BIT(insn, 25) && BIT(insn, 4)) {
        save_reg_shift_carry(e, insn, pc);
        return;
    }
    if (BIT(insn, 25)) {
        uint32_t rot = BITS(insn, 11, 8) * 2u;
        if (!rot) return; // C is unchanged.
        shift_imm(e, X14, shifted, 1, 31);
    } else {
        uint32_t type = BITS(insn, 6, 5);
        uint32_t n = BITS(insn, 11, 7);
        if (!n && type == 0) return;
        if (!n && type == 3) return;
        guest_load(e, X14, BITS(insn, 3, 0), pc);
        uint32_t bit = type == 0 ? 32u - n : n ? n - 1u : 31u;
        shift_imm(e, X14, X14, 1, bit);
    }
    carry_from_reg(e, X14, X15);
}

static void restore_carry(emitter *e) {
    ldr_w(e, X14, X9, F_OFF);
    emit(e, 0xD51B420Eu);
}

static void save_nzcv(emitter *e) {
    emit(e, 0xD53B420Eu); // mrs x14, nzcv
    str_w(e, X14, X9, F_OFF);
}

static void save_nz(emitter *e) {
    emit(e, 0xD53B420Eu); // mrs x14, nzcv
    ldr_w(e, X15, X9, F_OFF);
    shift_imm(e, X14, X14, 1, 30);                // lsr w14, w14, #30 gives N:Z in bits 1:0
    emit(e, 0x33000000u | (2u << 16) | (1u << 10) // bfi w15, w14, #30, #2
                | ((uint32_t)X14 << 5) | X15);
    str_w(e, X15, X9, F_OFF);
}

static void set_nz(emitter *e, int reg) {
    emit(e, 0x7100001Fu | ((uint32_t)reg << 5)); // cmp wReg,#0
    save_nz(e);
}

static void thumb_guest_load(emitter *e, int dst, uint32_t r, uint32_t pc) {
    if (r == 15)
        mov_w_imm(e, dst, pc + 4);
    else
        ldr_w(e, dst, X9, offsetof(mr_cpu, r) + r * 4u);
}

static void set_thumb_pc_reg_and_return(emitter *e, int reg, uint32_t count) {
    shift_imm(e, X12, reg, 1, 1);
    shift_imm(e, X12, X12, 0, 1);
    guest_store(e, X12, MR_R_PC);
    return_count(e, count);
    e->ended = 1;
}

static uint32_t *guard_condition(emitter *e, uint32_t cond) {
    if (cond >= 14) return NULL;
    ldr_w(e, X11, X9, F_OFF);
    emit(e, 0xD51B420Bu);
    uint32_t *at = e->out;
    emit(e, 0x54000000u | (cond ^ 1u));
    return at;
}

static void set_pc_reg_and_return(emitter *e, int reg, uint32_t count) {
    // thumb = target & 1
    emit(e, 0x53000000u | (0u << 16) | (0u << 10) | ((uint32_t)reg << 5) | X12);
    str_w(e, X12, X9, offsetof(mr_cpu, thumb));
    // pc = target & ~1
    shift_imm(e, X12, reg, 1, 1);
    shift_imm(e, X12, X12, 0, 1);
    guest_store(e, X12, MR_R_PC);
    return_count(e, count);
    e->ended = 1;
}

static void slow_memory_exit(emitter *e, int addr, uint32_t pc, uint32_t ran) {
    // w8 holds the guest-memory size.
    emit(e, 0x6B00001Fu | ((uint32_t)X8 << 16) | ((uint32_t)addr << 5)); // cmp wAddr, wSize
    uint32_t *safe = e->out;
    emit(e, 0x54000003u); // b.lo safe
    str_w(e, addr, X9, offsetof(mr_cpu, a64_slow_addr));
    mov_w_imm(e, X11, pc);
    guest_store(e, X11, MR_R_PC);
    str_w(e, X11, X9, offsetof(mr_cpu, a64_slow_pc));
    return_count(e, ran);
    patch_b19(safe, e->out);
}

static int compile_data(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t op = BITS(insn, 24, 21);
    uint32_t s = BIT(insn, 20);
    uint32_t rn = BITS(insn, 19, 16);
    uint32_t rd = BITS(insn, 15, 12);

    if (!s && op >= 8 && op <= 11) {
        if (BIT(insn, 25) && (op == 8 || op == 10)) {
            uint32_t imm = (BITS(insn, 19, 16) << 12) | BITS(insn, 11, 0);
            if (op == 8)
                mov_w_imm(e, X13, imm);
            else {
                guest_load(e, X13, rd, pc);
                mov_w_imm(e, X12, 0xFFFFu);
                emit(e, 0x0A000000u | ((uint32_t)X12 << 16) | ((uint32_t)X13 << 5) | X13);
                mov_w_imm(e, X12, imm << 16);
                emit(e, 0x2A000000u | ((uint32_t)X12 << 16) | ((uint32_t)X13 << 5) | X13);
            }
            if (rd == 15) {
                set_pc_reg_and_return(e, X13, e->count + 1);
            } else
                guest_store(e, X13, rd);
            return 1;
        }
        if (BIT(insn, 25) && (op == 9 || op == 11) && !BITS(insn, 19, 16)) return 1; // hint
        return 0;
    }

    int logical =
        op == 0 || op == 1 || op == 8 || op == 9 || op == 12 || op == 13 || op == 14 || op == 15;
    if (!operand2_ok(insn, s && logical)) return 0;

    guest_load(e, X11, rn, pc);
    operand2(e, insn, pc, X12);
    uint32_t base = 0;
    int writes = 1;
    switch (op) {
    case 0:
        base = 0x0A000000u;
        break;
    case 1:
        base = 0x4A000000u;
        break;
    case 2:
        base = s ? 0x6B000000u : 0x4B000000u;
        break;
    case 3:
        base = s ? 0x6B000000u : 0x4B000000u;
        break;
    case 4:
        base = s ? 0x2B000000u : 0x0B000000u;
        break;
    case 5:
        base = s ? 0x3A000000u : 0x1A000000u;
        break;
    case 6:
        base = s ? 0x7A000000u : 0x5A000000u;
        break;
    case 7:
        base = s ? 0x7A000000u : 0x5A000000u;
        break;
    case 8:
        base = 0x0A000000u;
        writes = 0;
        break;
    case 9:
        base = 0x4A000000u;
        writes = 0;
        break;
    case 10:
        base = 0x6B000000u;
        writes = 0;
        break;
    case 11:
        base = 0x2B000000u;
        writes = 0;
        break;
    case 12:
        base = 0x2A000000u;
        break;
    case 13:
        base = 0x2A000000u;
        break;
    case 14:
        base = 0x0A200000u;
        break;
    case 15:
        base = 0x2A200000u;
        break;
    default:
        return 0;
    }

    int left = X11, right = X12;
    if (op == 3 || op == 7) {
        left = X12;
        right = X11;
    }
    int out = (writes || logical) ? X13 : XZR;
    if (op >= 5 && op <= 7) restore_carry(e);
    if (op == 13 || op == 15)
        emit(e, base | ((uint32_t)right << 16) | ((uint32_t)XZR << 5) | out);
    else
        emit(e, base | ((uint32_t)right << 16) | ((uint32_t)left << 5) | out);

    if (s) {
        if (logical)
            save_logic_flags(e, insn, pc, X12, X13);
        else
            save_nzcv(e);
    }
    if (writes) {
        if (rd == 15)
            set_pc_reg_and_return(e, X13, e->count + 1);
        else
            guest_store(e, X13, rd);
    }
    return 1;
}

static int compile_mul(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t op = BITS(insn, 23, 21);
    uint32_t s = BIT(insn, 20);
    if (s && op == 3) return 0;       // MLS has no S form.
    uint32_t hi = BITS(insn, 19, 16); // MUL/MLA: Rd; long form: RdHi.
    uint32_t lo = BITS(insn, 15, 12); // MLA: Ra; long form: RdLo.
    guest_load(e, X11, BITS(insn, 3, 0), pc);
    guest_load(e, X12, BITS(insn, 11, 8), pc);

    if (op <= 1) { // MUL / MLA
        emit(e, 0x1B007C00u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
        if (op == 1) {
            guest_load(e, X14, lo, pc);
            emit(e, 0x0B000000u | ((uint32_t)X14 << 16) | ((uint32_t)X13 << 5) | X13);
        }
        if (s) set_nz(e, X13);
        guest_store(e, X13, hi);
        return 1;
    }
    if (op == 3) { // MLS: Ra - Rm * Rs
        guest_load(e, X14, lo, pc);
        emit(e, 0x1B007C00u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
        emit(e, 0x4B000000u | ((uint32_t)X13 << 16) | ((uint32_t)X14 << 5) | X13);
        guest_store(e, X13, hi);
        return 1;
    }
    if (op == 2) return 0; // UMAAL is not used by the validated engine.

    int is_signed = op >= 6, accumulate = (op & 1u) != 0;
    emit(e, (is_signed ? 0x9B207C00u : 0x9BA07C00u) | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) |
                X13);
    if (accumulate) {
        ldr_w(e, X14, X9, offsetof(mr_cpu, r) + lo * 4u);
        ldr_w(e, X15, X9, offsetof(mr_cpu, r) + hi * 4u);
        emit(e, 0xAA000000u | ((uint32_t)X15 << 16) | (32u << 10) | ((uint32_t)X14 << 5) | X14);
        emit(e, 0x8B000000u | ((uint32_t)X14 << 16) | ((uint32_t)X13 << 5) | X13);
    }
    if (s) {
        emit(e, 0xF100001Fu | ((uint32_t)X13 << 5)); // cmp x13, #0
        save_nz(e);
    }
    guest_store(e, X13, lo);
    emit(e, 0xD360FC00u | ((uint32_t)X13 << 5) | X14); // lsr x14, x13, #32
    guest_store(e, X14, hi);
    return 1;
}

static void load_signed_half(emitter *e, int dst, uint32_t reg, int upper, uint32_t pc) {
    guest_load(e, dst, reg, pc);
    if (upper)
        shift_imm(e, dst, dst, 2, 16); // Sign-extend the upper halfword.
    else
        emit(e, 0x13003C00u | ((uint32_t)dst << 5) | (uint32_t)dst); // sxth
}

static int compile_halfword_mul(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t op = BITS(insn, 22, 21);
    if (op != 0 && op != 3) return 0;

    load_signed_half(e, X11, BITS(insn, 3, 0), BIT(insn, 5), pc);
    load_signed_half(e, X12, BITS(insn, 11, 8), BIT(insn, 6), pc);
    emit(e, 0x1B007C00u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13); // mul w13,w11,w12

    if (op == 0) { // SMLA<x><y>: 32-bit overflow sets the sticky Q flag.
        guest_load(e, X14, BITS(insn, 15, 12), pc);
        emit(e,
             0x2B000000u | ((uint32_t)X14 << 16) | ((uint32_t)X13 << 5) | X13); // adds w13,w13,w14
        emit(e, 0xD53B420Eu);                                                   // mrs x14,nzcv
        shift_imm(e, X15, X14, 1, 28);
        emit(e, 0x120001EFu); // and w15,w15,#1
        ldr_w(e, X14, X9, offsetof(mr_cpu, f.q));
        emit(e,
             0x2A000000u | ((uint32_t)X15 << 16) | ((uint32_t)X14 << 5) | X14); // orr w14,w14,w15
        str_w(e, X14, X9, offsetof(mr_cpu, f.q));
    }

    guest_store(e, X13, BITS(insn, 19, 16));
    return 1;
}

static int compile_ldst(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t i = BIT(insn, 25), p = BIT(insn, 24), u = BIT(insn, 23);
    uint32_t byte = BIT(insn, 22), w = BIT(insn, 21), load = BIT(insn, 20);
    uint32_t rn = BITS(insn, 19, 16), rt = BITS(insn, 15, 12);

    guest_load(e, X11, rn, pc);
    if (i) {
        ldst_offset(e, insn, pc, X12);
    } else {
        mov_w_imm(e, X12, BITS(insn, 11, 0));
    }
    if (p)
        emit(e,
             (u ? 0x0B000000u : 0x4B000000u) | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
    else
        emit(e, 0x2A0003EDu | ((uint32_t)X11 << 16)); // mov w13,w11

    slow_memory_exit(e, X13, pc, e->count);
    emit(e, 0x8B200000u | ((uint32_t)X13 << 16) | (2u << 13) | ((uint32_t)X10 << 5) |
                X14); // add x14,x10,w13,uxtw
    if (load) {
        emit(e, (byte ? 0x39400000u : 0xB9400000u) | ((uint32_t)X14 << 5) | X15);
        if (rt != 15) {
            guest_store(e, X15, rt);
        }
    } else {
        guest_load(e, X15, rt, pc + 4);
        emit(e, (byte ? 0x39000000u : 0xB9000000u) | ((uint32_t)X14 << 5) | X15);
    }

    if (!p)
        emit(e,
             (u ? 0x0B000000u : 0x4B000000u) | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
    if ((!p || w) && rn != 15 && !(load && rn == rt)) guest_store(e, X13, rn);
    if (load && rt == 15) set_pc_reg_and_return(e, X15, e->count + 1);
    return 1;
}

static int compile_ldm(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t p = BIT(insn, 24), u = BIT(insn, 23);
    uint32_t w = BIT(insn, 21), load = BIT(insn, 20);
    uint32_t rn = BITS(insn, 19, 16), list = BITS(insn, 15, 0);
    uint32_t n = (uint32_t)__builtin_popcount(list);
    if (!n) return 0;

    guest_load(e, X11, rn, pc);
    uint32_t delta = n * 4u;
    mov_w_imm(e, X12, delta);
    if (u)
        emit(e, 0x2A0003EDu | ((uint32_t)X11 << 16)); // addr=base
    else
        emit(e, 0x4B000000u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
    if (u ? p : !p) emit(e, 0x110011ADu); // add w13,w13,#4

    slow_memory_exit(e, X13, pc, e->count);
    emit(e, 0x8B200000u | ((uint32_t)X13 << 16) | (2u << 13) | ((uint32_t)X10 << 5) | X14);
    uint32_t off = 0;
    int loads_pc = load && (list & 0x8000u);
    for (uint32_t r = 0; r < 16; r++) {
        if (!(list & (1u << r))) continue;
        if (load) {
            emit(e, 0xB9400000u | ((off / 4u) << 10) | ((uint32_t)X14 << 5) | X15);
            if (r != 15) guest_store(e, X15, r);
        } else {
            guest_load(e, X15, r, pc + 4);
            emit(e, 0xB9000000u | ((off / 4u) << 10) | ((uint32_t)X14 << 5) | X15);
        }
        off += 4;
    }
    if (w) {
        emit(e,
             (u ? 0x0B000000u : 0x4B000000u) | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
        guest_store(e, X13, rn);
    }
    if (loads_pc) set_pc_reg_and_return(e, X15, e->count + 1);
    return 1;
}

static void fp_load(emitter *e, int vt, uint32_t reg, int dbl) {
    size_t off = offsetof(mr_cpu, v) + reg * (dbl ? 8u : 4u);
    uint32_t op = dbl ? 0xFD400000u : 0xBD400000u;
    uint32_t scale = dbl ? 8u : 4u;
    emit(e, op | ((uint32_t)(off / scale) << 10) | ((uint32_t)X9 << 5) | (uint32_t)vt);
}

static void fp_store(emitter *e, int vt, uint32_t reg, int dbl) {
    size_t off = offsetof(mr_cpu, v) + reg * (dbl ? 8u : 4u);
    uint32_t op = dbl ? 0xFD000000u : 0xBD000000u;
    uint32_t scale = dbl ? 8u : 4u;
    emit(e, op | ((uint32_t)(off / scale) << 10) | ((uint32_t)X9 << 5) | (uint32_t)vt);
}

static void fp_binary(emitter *e, uint32_t op, int dbl, int vd, int vn, int vm) {
    emit(e,
         op | (dbl ? 0x00400000u : 0) | ((uint32_t)vm << 16) | ((uint32_t)vn << 5) | (uint32_t)vd);
}

static void fp_unary(emitter *e, uint32_t op, int dbl, int vd, int vn) {
    emit(e, op | (dbl ? 0x00400000u : 0) | ((uint32_t)vn << 5) | (uint32_t)vd);
}

static void slow_memory_exit_size(emitter *e, int addr, uint32_t pc, uint32_t ran, uint32_t size);
static void thumb_add_imm(emitter *e, int rd, int rn, uint32_t imm, int sub);

static void vword_load(emitter *e, int dst, uint32_t word) {
    ldr_w(e, dst, X9, offsetof(mr_cpu, v) + word * 4u);
}

static void vword_store(emitter *e, int src, uint32_t word) {
    str_w(e, src, X9, offsetof(mr_cpu, v) + word * 4u);
}

static int compile_vfp_data(emitter *e, uint32_t insn) {
    uint32_t cp = BITS(insn, 11, 8);
    if ((cp != 10 && cp != 11) || BIT(insn, 4)) return 0;
    int dbl = cp == 11;
    uint32_t p = BIT(insn, 23), q = BIT(insn, 21);
    uint32_t r = BIT(insn, 20), s = BIT(insn, 6);
    uint32_t sel = (p << 2) | (q << 1) | r;
    uint32_t vd =
        dbl ? (BIT(insn, 22) << 4) | BITS(insn, 15, 12) : (BITS(insn, 15, 12) << 1) | BIT(insn, 22);
    uint32_t vn =
        dbl ? (BIT(insn, 7) << 4) | BITS(insn, 19, 16) : (BITS(insn, 19, 16) << 1) | BIT(insn, 7);
    uint32_t vm =
        dbl ? (BIT(insn, 5) << 4) | BITS(insn, 3, 0) : (BITS(insn, 3, 0) << 1) | BIT(insn, 5);

    if (sel != 7) {
        if (sel > 4) return 0;
        fp_load(e, 0, vn, dbl);
        fp_load(e, 1, vm, dbl);
        if (sel <= 1) {
            fp_load(e, 2, vd, dbl);
            uint32_t op = sel == 0 ? (s ? 0x1F008000u : 0x1F000000u)  // FMSUB / FMADD
                                   : (s ? 0x1F200000u : 0x1F208000u); // FNMADD / FNMSUB
            emit(e, op | (dbl ? 0x00400000u : 0) | (1u << 16) | (2u << 10) | 2u);
        } else if (sel == 2) {
            fp_binary(e, 0x1E200800u, dbl, 2, 0, 1);
            if (s) fp_unary(e, 0x1E214000u, dbl, 2, 2);
        } else if (sel == 3) {
            fp_binary(e, s ? 0x1E203800u : 0x1E202800u, dbl, 2, 0, 1);
        } else {
            fp_binary(e, 0x1E201800u, dbl, 2, 0, 1);
        }
        fp_store(e, 2, vd, dbl);
        return 1;
    }

    if (!BIT(insn, 6)) {
        uint32_t imm8 = (BITS(insn, 19, 16) << 4) | BITS(insn, 3, 0);
        if (dbl) {
            uint64_t bits = ((uint64_t)BIT(imm8, 7) << 63) |
                            ((uint64_t)(BIT(imm8, 6) ? 0u : 1u) << 62) |
                            ((uint64_t)(BIT(imm8, 6) ? 0xFFu : 0u) << 54) |
                            ((uint64_t)BITS(imm8, 5, 4) << 52) | ((uint64_t)BITS(imm8, 3, 0) << 48);
            mov_w_imm(e, X11, (uint32_t)bits);
            mov_w_imm(e, X12, (uint32_t)(bits >> 32));
            vword_store(e, X11, vd * 2u);
            vword_store(e, X12, vd * 2u + 1u);
        } else {
            uint32_t bits = (BIT(imm8, 7) << 31) | ((BIT(imm8, 6) ? 0u : 1u) << 30) |
                            ((BIT(imm8, 6) ? 0x1Fu : 0u) << 25) | (BITS(imm8, 5, 4) << 23) |
                            (BITS(imm8, 3, 0) << 19);
            mov_w_imm(e, X11, bits);
            vword_store(e, X11, vd);
        }
        return 1;
    }
    uint32_t op2 = BITS(insn, 19, 16);
    uint32_t o3 = BITS(insn, 7, 6);
    fp_load(e, 0, vm, dbl);

    if (op2 == 4 || op2 == 5) {
        fp_load(e, 1, vd, dbl);
        uint32_t fcmp = op2 == 5 ? 0x1E202008u : 0x1E202000u; // #0.0 / Vm
        emit(e, fcmp | (dbl ? 0x00400000u : 0u) | (op2 == 5 ? 0u : (0u << 16)) | (1u << 5));
        emit(e, 0xD53B420Eu); // mrs x14, nzcv
        shift_imm(e, X14, X14, 1, 28);
        shift_imm(e, X14, X14, 0, 28);
        ldr_w(e, X15, X9, offsetof(mr_cpu, fpscr));
        shift_imm(e, X15, X15, 0, 4);
        shift_imm(e, X15, X15, 1, 4);
        emit(e, 0x2A000000u | ((uint32_t)X14 << 16) | ((uint32_t)X15 << 5) | X15);
        str_w(e, X15, X9, offsetof(mr_cpu, fpscr));
        return 1;
    }

    if (op2 == 7) { // VCVT single/double precision.
        if (dbl) {
            uint32_t sd = (BITS(insn, 15, 12) << 1) | BIT(insn, 22);
            emit(e, 0x1E624002u); // fcvt s2,d0
            fp_store(e, 2, sd, 0);
        } else {
            uint32_t dd = (BIT(insn, 22) << 4) | BITS(insn, 15, 12);
            emit(e, 0x1E22C002u); // fcvt d2,s0
            fp_store(e, 2, dd, 1);
        }
        return 1;
    }
    if (op2 == 8) { // VCVT integer to floating point.
        uint32_t sm = (BITS(insn, 3, 0) << 1) | BIT(insn, 5);
        vword_load(e, X11, sm);
        uint32_t base = BIT(insn, 7) ? 0x1E220000u : 0x1E230000u;
        emit(e, base | (dbl ? 0x00400000u : 0u) | ((uint32_t)X11 << 5) | 2u);
        fp_store(e, 2, vd, dbl);
        return 1;
    }
    if (op2 == 12 || op2 == 13) { // VCVT floating point to integer, toward zero.
        uint32_t sd = (BITS(insn, 15, 12) << 1) | BIT(insn, 22);
        uint32_t base = op2 == 13 ? 0x1E380000u : 0x1E390000u;
        emit(e, base | (dbl ? 0x00400000u : 0u) | (0u << 5) | X11);
        vword_store(e, X11, sd);
        return 1;
    }

    if ((op2 == 10 || op2 == 11 || op2 == 14 || op2 == 15) && BIT(insn, 7)) {
        uint32_t imm = (BITS(insn, 3, 0) << 1) | BIT(insn, 5);
        if (imm >= 32u) return 0; // frac == 0 is not encodable in A64.
        uint32_t scale = 64u - (32u - imm);
        uint32_t wide = dbl ? 0x00400000u : 0u;
        uint32_t word = dbl ? vd * 2u : vd;
        int to_fixed = BIT(insn, 18);
        int is_signed = !BIT(insn, 16);
        if (to_fixed) {
            fp_load(e, 0, vd, dbl);
            emit(e, (is_signed ? 0x1E180000u : 0x1E190000u) | wide // FCVTZS/U
                        | (scale << 10) | (0u << 5) | X11);
            if (dbl) {
                emit(e, 0x93407C00u | ((uint32_t)X11 << 5) | X11); // sxtw
                vword_store(e, X11, word);
                emit(e, 0xD360FC00u | ((uint32_t)X11 << 5) | X12); // lsr #32
                vword_store(e, X12, word + 1u);
            } else {
                vword_store(e, X11, word);
            }
        } else {
            vword_load(e, X11, word); // Low word of the fixed-point value.
            emit(e, (is_signed ? 0x1E020000u : 0x1E030000u) | wide // SCVTF/UCVTF
                        | (scale << 10) | ((uint32_t)X11 << 5) | 2u);
            fp_store(e, 2, vd, dbl);
        }
        return 1;
    }
    if (op2 == 0) {
        fp_unary(e, o3 == 1 ? 0x1E204000u : 0x1E20C000u, dbl, 2, 0); // VMOV / VABS
    } else if (op2 == 1) {
        fp_unary(e, o3 == 1 ? 0x1E214000u : 0x1E21C000u, dbl, 2, 0); // VNEG / VSQRT
    } else {
        return 0;
    }
    fp_store(e, 2, vd, dbl);
    return 1;
}

static int compile_vfp_move(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t cp = BITS(insn, 11, 8);
    if ((cp != 10 && cp != 11) || !BIT(insn, 4)) return 0;
    uint32_t load = BIT(insn, 20), rt = BITS(insn, 15, 12);

    if (BITS(insn, 27, 24) == 14 && BITS(insn, 23, 21) == 7 && cp == 10) {
        if (load) {
            ldr_w(e, X11, X9, offsetof(mr_cpu, fpscr));
            if (rt == 15) {
                shift_imm(e, X12, X11, 1, 28); // Upper four bits only.
                shift_imm(e, X12, X12, 0, 28);
                str_w(e, X12, X9, F_OFF);
            } else {
                guest_store(e, X11, rt);
            }
        } else {
            guest_load(e, X11, rt, pc);
            str_w(e, X11, X9, offsetof(mr_cpu, fpscr));
        }
        return 1;
    }

    uint32_t sn = (BITS(insn, 19, 16) << 1) | BIT(insn, 7);
    if (load) {
        vword_load(e, X11, sn);
        guest_store(e, X11, rt);
    } else {
        guest_load(e, X11, rt, pc);
        vword_store(e, X11, sn);
    }
    return 1;
}

static int compile_vfp_move2(emitter *e, uint32_t insn, uint32_t pc) {
    if (BITS(insn, 27, 21) != 0x62) return 0;
    uint32_t cp = BITS(insn, 11, 8);
    if (cp != 10 && cp != 11) return 0;
    uint32_t load = BIT(insn, 20);
    uint32_t rt = BITS(insn, 15, 12), rt2 = BITS(insn, 19, 16);
    uint32_t word = cp == 11 ? (((BIT(insn, 5) << 4) | BITS(insn, 3, 0)) * 2u)
                             : ((BITS(insn, 3, 0) << 1) | BIT(insn, 5));
    if (load) {
        vword_load(e, X11, word);
        vword_load(e, X12, word + 1);
        guest_store(e, X11, rt);
        guest_store(e, X12, rt2);
    } else {
        guest_load(e, X11, rt, pc);
        guest_load(e, X12, rt2, pc);
        vword_store(e, X11, word);
        vword_store(e, X12, word + 1);
    }
    return 1;
}

static int compile_vfp_ldst(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t cp = BITS(insn, 11, 8);
    if (cp != 10 && cp != 11) return 0;
    int dbl = cp == 11;
    uint32_t p = BIT(insn, 24), u = BIT(insn, 23), w = BIT(insn, 21);
    uint32_t load = BIT(insn, 20), rn = BITS(insn, 19, 16);
    uint32_t imm8 = BITS(insn, 7, 0);
    uint32_t vd =
        dbl ? (BIT(insn, 22) << 4) | BITS(insn, 15, 12) : (BITS(insn, 15, 12) << 1) | BIT(insn, 22);
    uint32_t count = p && !w ? 1u : (dbl ? imm8 / 2u : imm8);
    uint32_t bytes = count * (dbl ? 8u : 4u);
    if (!count || !bytes) return 0;

    guest_load(e, X11, rn, pc);
    uint32_t delta = p && !w ? imm8 * 4u : bytes;
    thumb_add_imm(e, X13, X11, delta, !u);
    if (!p || w) {
        if (u) emit(e, 0x2A0003EDu | ((uint32_t)X11 << 16));
        // A decrementing multiple transfer starts at the range bottom.
    }
    slow_memory_exit_size(e, X13, pc, e->count, bytes);
    emit(e, 0x8B200000u | ((uint32_t)X13 << 16) | (2u << 13) | ((uint32_t)X10 << 5) | X14);
    uint32_t word = dbl ? vd * 2u : vd;
    uint32_t words = bytes / 4u;
    for (uint32_t i = 0; i < words; i++) {
        if (load) {
            emit(e, 0xB9400000u | (i << 10) | ((uint32_t)X14 << 5) | X15);
            vword_store(e, X15, word + i);
        } else {
            vword_load(e, X15, word + i);
            emit(e, 0xB9000000u | (i << 10) | ((uint32_t)X14 << 5) | X15);
        }
    }
    if (w) {
        thumb_add_imm(e, X13, X11, bytes, !u);
        guest_store(e, X13, rn);
    }
    return 1;
}

static int compile_media(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t op = BITS(insn, 27, 20), rd = BITS(insn, 15, 12);
    uint32_t rn = BITS(insn, 3, 0), op2 = BITS(insn, 7, 4);
    uint32_t op2b = BITS(insn, 6, 4);

    if ((op >= 0x7A && op <= 0x7F) && op2b == 5) {
        uint32_t width = BITS(insn, 20, 16) + 1u;
        uint32_t lsb = BITS(insn, 11, 7);
        if (width > 32u - lsb) return 0;
        guest_load(e, X11, rn, pc);
        uint32_t base = op <= 0x7B ? 0x13000000u : 0x53000000u;
        emit(e, base | (lsb << 16) | ((lsb + width - 1u) << 10) | ((uint32_t)X11 << 5) | X13);
        guest_store(e, X13, rd);
        return 1;
    }

    if (op >= 0x7C && op <= 0x7D && op2b == 1) {
        uint32_t msb = BITS(insn, 20, 16), lsb = BITS(insn, 11, 7);
        if (msb < lsb) return 0;
        uint32_t width = msb - lsb + 1u;
        uint32_t mask = width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1u) << lsb;
        guest_load(e, X11, rd, pc);
        if (rn == 15)
            mov_w_imm(e, X12, 0);
        else
            guest_load(e, X12, rn, pc);
        if (lsb) shift_imm(e, X12, X12, 0, lsb);
        mov_w_imm(e, X14, mask);
        emit(e, 0x0A200000u | ((uint32_t)X14 << 16) | ((uint32_t)X11 << 5) | X13); // bic old,mask
        emit(e, 0x0A000000u | ((uint32_t)X14 << 16) | ((uint32_t)X12 << 5) | X12);
        emit(e, 0x2A000000u | ((uint32_t)X12 << 16) | ((uint32_t)X13 << 5) | X13);
        guest_store(e, X13, rd);
        return 1;
    }

    if ((op == 0x6B || op == 0x6F) && (op2 == 3 || op2 == 11)) {
        guest_load(e, X11, rn, pc);
        if (op == 0x6B && op2 == 3) {
            emit(e, 0x5AC00800u | ((uint32_t)X11 << 5) | X13); // REV
        } else if (op == 0x6B) {
            emit(e, 0x5AC00400u | ((uint32_t)X11 << 5) | X13); // REV16
        } else if (op2 == 3) {
            emit(e, 0x5AC00000u | ((uint32_t)X11 << 5) | X13); // RBIT
        } else {
            emit(e, 0x5AC00400u | ((uint32_t)X11 << 5) | X13);
            emit(e, 0x13003C00u | ((uint32_t)X13 << 5) | X13); // SXTH
        }
        guest_store(e, X13, rd);
        return 1;
    }

    if (op2 == 7 && (op == 0x6A || op == 0x6B || op == 0x6E || op == 0x6F)) {
        uint32_t rot = BITS(insn, 11, 10) * 8u;
        guest_load(e, X11, rn, pc);
        if (rot) shift_imm(e, X11, X11, 3, rot);
        uint32_t base = (op == 0x6A || op == 0x6B) ? 0x13000000u : 0x53000000u;
        uint32_t top = (op == 0x6A || op == 0x6E) ? 7u : 15u;
        emit(e, base | (top << 10) | ((uint32_t)X11 << 5) | X13);
        uint32_t acc = BITS(insn, 19, 16);
        if (acc != 15) {
            guest_load(e, X12, acc, pc);
            emit(e, 0x0B000000u | ((uint32_t)X12 << 16) | ((uint32_t)X13 << 5) | X13);
        }
        guest_store(e, X13, rd);
        return 1;
    }
    return 0;
}

static int compile_ldst_extra(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t p = BIT(insn, 24), u = BIT(insn, 23), imm = BIT(insn, 22);
    uint32_t w = BIT(insn, 21), load = BIT(insn, 20);
    uint32_t rn = BITS(insn, 19, 16), rt = BITS(insn, 15, 12);
    uint32_t op = BITS(insn, 6, 5);
    if (!op) return 0;

    guest_load(e, X11, rn, pc);
    if (imm)
        mov_w_imm(e, X12, (BITS(insn, 11, 8) << 4) | BITS(insn, 3, 0));
    else
        guest_load(e, X12, BITS(insn, 3, 0), pc);
    if (p) {
        emit(e,
             (u ? 0x0B000000u : 0x4B000000u) | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
    } else {
        emit(e, 0x2A0003EDu | ((uint32_t)X11 << 16));
    }

    uint32_t bytes = (!load && op >= 2) ? 8u : op == 2 ? 1u : 2u;
    slow_memory_exit_size(e, X13, pc, e->count, bytes);
    emit(e, 0x8B200000u | ((uint32_t)X13 << 16) | (2u << 13) | ((uint32_t)X10 << 5) | X14);
    if (load) {
        uint32_t memop = op == 1 ? 0x79400000u : op == 2 ? 0x39C00000u : 0x79C00000u;
        emit(e, memop | ((uint32_t)X14 << 5) | X15);
        guest_store(e, X15, rt);
    } else if (op == 1) {
        guest_load(e, X15, rt, pc);
        emit(e, 0x79000000u | ((uint32_t)X14 << 5) | X15);
    } else if (op == 2) {
        emit(e, 0xB9400000u | ((uint32_t)X14 << 5) | X15);
        guest_store(e, X15, rt);
        emit(e, 0xB9400000u | (1u << 10) | ((uint32_t)X14 << 5) | X15);
        guest_store(e, X15, rt + 1u);
    } else {
        guest_load(e, X15, rt, pc);
        emit(e, 0xB9000000u | ((uint32_t)X14 << 5) | X15);
        guest_load(e, X15, rt + 1u, pc);
        emit(e, 0xB9000000u | (1u << 10) | ((uint32_t)X14 << 5) | X15);
    }

    if (!p)
        emit(e,
             (u ? 0x0B000000u : 0x4B000000u) | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
    if ((!p || w) && rn != 15) guest_store(e, X13, rn);
    return 1;
}

static int compile_sync(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t op = BITS(insn, 27, 20);
    if (op != 0x18 && op != 0x19) return 0;
    uint32_t rn = BITS(insn, 19, 16);
    uint32_t rd = BITS(insn, 15, 12);
    uint32_t rt = BITS(insn, 3, 0);

    guest_load(e, X13, rn, pc);
    slow_memory_exit_size(e, X13, pc, e->count, 4);
    emit(e, 0x8B200000u | ((uint32_t)X13 << 16) | (2u << 13) | ((uint32_t)X10 << 5) | X14);
    if (op == 0x19) {
        emit(e, 0xB9400000u | ((uint32_t)X14 << 5) | X15);
        guest_store(e, X15, rd);
    } else {
        guest_load(e, X15, rt, pc);
        emit(e, 0xB9000000u | ((uint32_t)X14 << 5) | X15);
        mov_w_imm(e, X15, 0);
        guest_store(e, X15, rd);
    }
    return 1;
}

static int compile_one(emitter *e, uint32_t insn, uint32_t pc) {
    uint32_t cond = BITS(insn, 31, 28);
    if (cond == 15) {
        if (BITS(insn, 27, 26) == 1 && BIT(insn, 20) && BITS(insn, 15, 12) == 15) return 1;
        if (BITS(insn, 27, 25) == 5) {
            int32_t off = ((int32_t)(insn << 8)) >> 6;
            off |= (int32_t)(BIT(insn, 24) << 1);
            uint32_t target = (uint32_t)((int32_t)pc + 8 + off);
            mov_w_imm(e, X11, pc + 4);
            guest_store(e, X11, MR_R_LR);
            mov_w_imm(e, X11, 1);
            str_w(e, X11, X9, offsetof(mr_cpu, thumb));
            direct_exit(e, target, 1, e->count + 1);
            e->has_direct_target = 1;
            e->direct_target = target;
            e->direct_target_thumb = 1;
            e->is_call = 1;
            e->ended = 1;
            e->ended_uncond = 1;
            return 1;
        }
        return BITS(insn, 27, 24) == 5;
    }
    uint32_t op1 = BITS(insn, 27, 25);

    uint32_t *guard = guard_condition(e, cond);
    int ok = 0;

    if (op1 == 5) {
        int32_t off = ((int32_t)(insn << 8)) >> 6;
        uint32_t target = (uint32_t)((int32_t)pc + 8 + off);
        e->has_direct_target = 1;
        e->direct_target = target;
        if (BIT(insn, 24)) {
            mov_w_imm(e, X11, pc + 4);
            guest_store(e, X11, MR_R_LR);
            e->is_call = 1;
        }
        direct_exit(e, target, 0, e->count + 1);
        if (guard) {
            patch_b19(guard, e->out);
            direct_exit(e, pc + 4, 0, e->count + 1);
        }
        e->ended = 1;
        e->ended_uncond = !guard;
        return 1;
    }

    if (op1 == 0 && BITS(insn, 24, 23) == 2 && !BIT(insn, 20)) {
        uint32_t sub = BITS(insn, 7, 4), op = BITS(insn, 22, 21);
        if (BIT(insn, 7) && !BIT(insn, 4)) ok = compile_halfword_mul(e, insn, pc);
        if ((sub == 1 || sub == 3) && op == 1) { // BX / BLX reg
            guest_load(e, X11, BITS(insn, 3, 0), pc);
            if (sub == 3) {
                mov_w_imm(e, X12, pc + 4);
                guest_store(e, X12, MR_R_LR);
                e->is_call = 1;
            }
            set_pc_reg_and_return(e, X11, e->count + 1);
            ok = 1;
        }
        if (!ok && sub == 1 && op == 3) { // CLZ
            guest_load(e, X11, BITS(insn, 3, 0), pc);
            emit(e, 0x5AC01000u | ((uint32_t)X11 << 5) | X13);
            guest_store(e, X13, BITS(insn, 15, 12));
            ok = 1;
        }
    }
    if (ok) {
        if (guard) {
            patch_b19(guard, e->out);
            if (e->ended) {
                set_pc_imm(e, pc + 4);
                return_count(e, e->count + 1);
            }
        }
        if (e->ended && !guard) e->ended_uncond = 1;
        return 1;
    }

    if (op1 == 7) {
        ok = BIT(insn, 4) ? compile_vfp_move(e, insn, pc) : compile_vfp_data(e, insn);
    } else if (op1 == 6) {
        ok = BITS(insn, 27, 21) == 0x62 ? compile_vfp_move2(e, insn, pc)
                                        : compile_vfp_ldst(e, insn, pc);
    } else if (op1 == 3 && BIT(insn, 4)) {
        ok = compile_media(e, insn, pc);
    } else if (op1 == 0 && BITS(insn, 7, 4) == 9) {
        ok = BIT(insn, 24) ? compile_sync(e, insn, pc) : compile_mul(e, insn, pc);
    } else if (op1 == 0 && BIT(insn, 7) && BIT(insn, 4)) {
        ok = compile_ldst_extra(e, insn, pc);
    } else if (op1 <= 1)
        ok = compile_data(e, insn, pc);
    else if (op1 == 2 || (op1 == 3 && !BIT(insn, 4)))
        ok = compile_ldst(e, insn, pc);
    else if (op1 == 4)
        ok = compile_ldm(e, insn, pc);

    if (!ok) return 0;
    if (guard) {
        patch_b19(guard, e->out);
        if (e->ended) {
            set_pc_imm(e, pc + 4);
            return_count(e, e->count + 1);
        }
    }
    if (e->ended && !guard) e->ended_uncond = 1;
    return 1;
}

static void slow_memory_exit_size(emitter *e, int addr, uint32_t pc, uint32_t ran, uint32_t size) {
    if (size == 1) {
        emit(e, 0x6B00001Fu | ((uint32_t)X8 << 16) | ((uint32_t)addr << 5)); // cmp wAddr,wSize
    } else {
        emit(e, 0x5100000Fu | (size << 10) | ((uint32_t)X8 << 5)); // sub w15,w8,#size
        emit(e, 0x6B0F001Fu | ((uint32_t)addr << 5));              // cmp wAddr,w15
    }
    uint32_t *safe = e->out;
    emit(e, 0x54000000u | (size == 1 ? 3u : 9u)); // b.lo / b.ls
    str_w(e, addr, X9, offsetof(mr_cpu, a64_slow_addr));
    mov_w_imm(e, X11, pc);
    guest_store(e, X11, MR_R_PC);
    str_w(e, X11, X9, offsetof(mr_cpu, a64_slow_pc));
    return_count(e, ran);
    patch_b19(safe, e->out);
}

static void thumb_memory(emitter *e, int addr, uint32_t rt, uint32_t size, int load, int sign,
                         uint32_t pc) {
    slow_memory_exit_size(e, addr, pc, e->count, size);
    emit(e, 0x8B200000u | ((uint32_t)addr << 16) | (2u << 13) | ((uint32_t)X10 << 5) |
                X14); // host + uxtw(addr)
    if (load) {
        uint32_t op;
        if (size == 1)
            op = sign ? 0x39C00000u : 0x39400000u;
        else if (size == 2)
            op = sign ? 0x79C00000u : 0x79400000u;
        else
            op = 0xB9400000u;
        emit(e, op | ((uint32_t)X14 << 5) | X15);
        if (rt == 15)
            set_pc_reg_and_return(e, X15, e->count + 1);
        else
            guest_store(e, X15, rt);
    } else {
        thumb_guest_load(e, X15, rt, pc);
        uint32_t op = size == 1 ? 0x39000000u : size == 2 ? 0x79000000u : 0xB9000000u;
        emit(e, op | ((uint32_t)X14 << 5) | X15);
    }
}

static void thumb_add_imm(emitter *e, int rd, int rn, uint32_t imm, int sub) {
    emit(e, (sub ? 0x51000000u : 0x11000000u) | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static void thumb_addsub_flags(emitter *e, int rd, int rn, int rm, int sub) {
    emit(e, (sub ? 0x6B000000u : 0x2B000000u) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
                (uint32_t)rd);
    save_nzcv(e);
}

static void thumb_addsub_imm_flags(emitter *e, int rd, int rn, uint32_t imm, int sub) {
    emit(e, (sub ? 0x71000000u : 0x31000000u) | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
    save_nzcv(e);
}

static void thumb_shift(emitter *e, uint32_t rd, uint32_t rm, uint32_t type, uint32_t amount,
                        uint32_t pc) {
    thumb_guest_load(e, X11, rm, pc);
    if (type == 0 && amount == 0) {
        emit(e, 0x2A0003EDu | ((uint32_t)X11 << 16)); // mov w13,w11
    } else if (type == 1 && amount == 0) {
        mov_w_imm(e, X13, 0);
        shift_imm(e, X15, X11, 1, 31);
        carry_from_reg(e, X15, X12);
    } else if (type == 2 && amount == 0) {
        shift_imm(e, X13, X11, 2, 31);
        shift_imm(e, X15, X11, 1, 31);
        carry_from_reg(e, X15, X12);
    } else {
        uint32_t carry_bit = type == 0 ? 32u - amount : amount - 1u;
        shift_imm(e, X15, X11, 1, carry_bit);
        emit(e, 0x120001EFu);
        carry_from_reg(e, X15, X12);
        shift_imm(e, X13, X11, type, amount);
    }
    guest_store(e, X13, rd);
    set_nz(e, X13);
}

static void thumb_shift_reg(emitter *e, uint32_t rd, uint32_t rs, uint32_t type, uint32_t pc) {
    thumb_guest_load(e, X15, rd, pc);
    thumb_guest_load(e, X14, rs, pc);                  // Shift amount.
    emit(e, 0x12001C00u | ((uint32_t)X14 << 5) | X14); // and w14,w14,#255

    // Carry output.
    if (type == 0) { // LSL
        mov_w_imm(e, X12, 32);
        emit(e, 0x4B000000u | ((uint32_t)X14 << 16) | ((uint32_t)X12 << 5) | X12);
        emit(e, 0x1AC02400u | ((uint32_t)X12 << 16) | ((uint32_t)X15 << 5) | X12);
        emit(e, 0x7100001Fu | (32u << 10) | ((uint32_t)X14 << 5));
        emit(e, 0x1A800000u | ((uint32_t)XZR << 16) | (9u << 12) | ((uint32_t)X12 << 5) | X12);
    } else if (type == 1 || type == 2) {                   // LSR / ASR
        emit(e, 0x51000400u | ((uint32_t)X14 << 5) | X12); // n-1
        emit(e, 0x1AC02400u | ((uint32_t)X12 << 16) | ((uint32_t)X15 << 5) | X12);
        shift_imm(e, X11, X15, 1, 31); // Sign bit.
        emit(e, 0x7100001Fu | (32u << 10) | ((uint32_t)X14 << 5));
        emit(e, 0x1A800000u | ((uint32_t)(type == 2 ? X11 : XZR) << 16) | (3u << 12) |
                    ((uint32_t)X12 << 5) | X12);
    } else { // ROR
        mov_w_imm(e, X11, 31);
        emit(e, 0x0A000000u | ((uint32_t)X11 << 16) | ((uint32_t)X14 << 5) | X11);
        emit(e, 0x51000400u | ((uint32_t)X11 << 5) | X12);
        emit(e, 0x1AC02400u | ((uint32_t)X12 << 16) | ((uint32_t)X15 << 5) | X12);
        shift_imm(e, X11, X15, 1, 31);
        emit(e, 0x7100001Fu | ((uint32_t)X11 << 5));
        emit(e, 0x1A800000u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X12);
    }
    // A zero amount preserves C.
    carry_to_reg(e, X11);
    emit(e, 0x7100001Fu | ((uint32_t)X14 << 5));
    emit(e, 0x1A800000u | ((uint32_t)X11 << 16) | (1u << 12) | ((uint32_t)X12 << 5) | X12);
    carry_from_reg(e, X12, X13);

    static const uint32_t SHIFTV[] = {0x1AC02000u, 0x1AC02400u, 0x1AC02800u, 0x1AC02C00u};
    if (type == 2) shift_imm(e, X11, X15, 2, 31); // ASR sign fill.
    emit(e, SHIFTV[type] | ((uint32_t)X14 << 16) | ((uint32_t)X15 << 5) | X13);
    if (type != 3) {
        uint32_t big = type == 2 ? (uint32_t)X11 : 31u;
        emit(e, 0x7100001Fu | (32u << 10) | ((uint32_t)X14 << 5));
        emit(e, 0x1A800000u | (big << 16) | (3u << 12) | ((uint32_t)X13 << 5) | X13);
    }
    guest_store(e, X13, rd);
    set_nz(e, X13);
}

static int compile_thumb_stack(emitter *e, uint32_t list, int load, uint32_t pc, uint32_t base_reg,
                               int writeback) {
    uint32_t n = (uint32_t)__builtin_popcount(list);
    if (!n) return 0;
    thumb_guest_load(e, X11, base_reg, pc);
    uint32_t bytes = n * 4u;
    if (!load && base_reg == MR_R_SP)
        thumb_add_imm(e, X13, X11, bytes, 1);
    else
        emit(e, 0x2A0003EDu | ((uint32_t)X11 << 16)); // mov w13,w11

    slow_memory_exit_size(e, X13, pc, e->count, bytes);
    emit(e, 0x8B200000u | ((uint32_t)X13 << 16) | (2u << 13) | ((uint32_t)X10 << 5) | X14);
    uint32_t off = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!(list & (1u << r))) continue;
        if (load) {
            emit(e, 0xB9400000u | ((off / 4u) << 10) | ((uint32_t)X14 << 5) | X15);
            if (r != 15) guest_store(e, X15, r);
        } else {
            thumb_guest_load(e, X15, r, pc);
            emit(e, 0xB9000000u | ((off / 4u) << 10) | ((uint32_t)X14 << 5) | X15);
        }
        off += 4;
    }
    if (writeback) {
        if (base_reg == MR_R_SP) {
            thumb_add_imm(e, X13, X11, bytes, load ? 0 : 1);
        } else {
            thumb_add_imm(e, X13, X11, bytes, 0);
        }
        guest_store(e, X13, base_reg);
    }
    if (load && (list & (1u << 15))) {
        set_pc_reg_and_return(e, X15, e->count + 1);
        e->ended_uncond = 1;
    }
    return 1;
}

static int compile_thumb32(emitter *e, uint32_t hw1, uint32_t hw2, uint32_t pc) {
    if (BITS(hw1, 12, 11) != 2u || !BIT(hw2, 15)) return 0;

    uint32_t s = BIT(hw1, 10);
    uint32_t imm10 = BITS(hw1, 9, 0);
    uint32_t j1 = BIT(hw2, 13), j2 = BIT(hw2, 11), imm11 = BITS(hw2, 10, 0);

    if (BIT(hw2, 14)) { // BL / BLX
        uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
        uint32_t raw = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1);
        int32_t off = (raw & (1u << 24)) ? (int32_t)(raw | 0xFE000000u) : (int32_t)raw;
        uint32_t target = (uint32_t)((int32_t)pc + 4 + off);
        mov_w_imm(e, X11, (pc + 4u) | 1u);
        guest_store(e, X11, MR_R_LR);
        if (BIT(hw2, 12)) {
            e->direct_target_thumb = 1;
        } else { // BLX switches to ARM mode.
            target &= ~3u;
            mov_w_imm(e, X11, 0);
            str_w(e, X11, X9, offsetof(mr_cpu, thumb));
            e->direct_target_thumb = 0;
        }
        direct_exit(e, target, e->direct_target_thumb, e->count + 1);
        e->has_direct_target = 1;
        e->direct_target = target;
        e->is_call = 1;
        e->ended = 1;
        e->ended_uncond = 1;
        return 1;
    }

    uint32_t cond = BITS(hw1, 9, 6);
    if (cond >= 14u) return 0; // MSR / hint / DSB
    uint32_t raw = (s << 20) | (j2 << 19) | (j1 << 18) | (BITS(hw1, 5, 0) << 12) | (imm11 << 1);
    int32_t off = (raw & (1u << 20)) ? (int32_t)(raw | 0xFFE00000u) : (int32_t)raw;
    uint32_t *not_taken = guard_condition(e, cond);
    e->has_direct_target = 1;
    e->direct_target = (uint32_t)((int32_t)pc + 4 + off);
    e->direct_target_thumb = 1;
    direct_exit(e, e->direct_target, 1, e->count + 1);
    patch_b19(not_taken, e->out);
    direct_exit(e, pc + 4u, 1, e->count + 1);
    e->ended = 1;
    return 1;
}

static int compile_thumb16(emitter *e, uint16_t insn, uint32_t pc) {
    uint32_t op = BITS(insn, 15, 11);

    if (op <= 3) {
        uint32_t type = BITS(insn, 12, 11);
        uint32_t rd = BITS(insn, 2, 0), rm = BITS(insn, 5, 3);
        if (type != 3) {
            thumb_shift(e, rd, rm, type, BITS(insn, 10, 6), pc);
        } else {
            uint32_t sub = BIT(insn, 9), imm = BIT(insn, 10);
            thumb_guest_load(e, X11, rm, pc);
            if (imm) {
                thumb_addsub_imm_flags(e, X13, X11, BITS(insn, 8, 6), sub);
            } else {
                thumb_guest_load(e, X12, BITS(insn, 8, 6), pc);
                thumb_addsub_flags(e, X13, X11, X12, sub);
            }
            guest_store(e, X13, rd);
        }
        return 1;
    }

    if (op <= 7) {
        uint32_t kind = BITS(insn, 12, 11);
        uint32_t rd = BITS(insn, 10, 8), imm = BITS(insn, 7, 0);
        thumb_guest_load(e, X11, rd, pc);
        if (kind == 0) {
            mov_w_imm(e, X13, imm);
            guest_store(e, X13, rd);
            set_nz(e, X13);
        } else if (kind == 1) {
            thumb_addsub_imm_flags(e, XZR, X11, imm, 1);
        } else {
            thumb_addsub_imm_flags(e, X13, X11, imm, kind == 3);
            guest_store(e, X13, rd);
        }
        return 1;
    }

    if (BITS(insn, 15, 10) == 0x10) {
        uint32_t kind = BITS(insn, 9, 6);
        uint32_t rd = BITS(insn, 2, 0), rm = BITS(insn, 5, 3);
        if (kind >= 2 && kind <= 4) { // Register LSL, LSR, or ASR.
            thumb_shift_reg(e, rd, rm, kind - 2u, pc);
            return 1;
        }
        if (kind == 7) { // Register ROR.
            thumb_shift_reg(e, rd, rm, 3, pc);
            return 1;
        }
        if (kind == 5 || kind == 6) { // ADC or SBC.
            thumb_guest_load(e, X11, rd, pc);
            thumb_guest_load(e, X12, rm, pc);
            restore_carry(e);
            emit(e, (kind == 5 ? 0x3A000000u : 0x7A000000u) | ((uint32_t)X12 << 16) |
                        ((uint32_t)X11 << 5) | X13);
            save_nzcv(e);
            guest_store(e, X13, rd);
            return 1;
        }
        thumb_guest_load(e, X11, rd, pc);
        thumb_guest_load(e, X12, rm, pc);
        int writes = 1;
        switch (kind) {
        case 0:
            emit(e, 0x0A000000u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
            break;
        case 1:
            emit(e, 0x4A000000u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
            break;
        case 8:
            emit(e, 0x0A000000u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
            writes = 0;
            break;
        case 9:
            emit(e, 0x6B0003EDu | ((uint32_t)X12 << 16));
            save_nzcv(e);
            break;
        case 10:
            thumb_addsub_flags(e, XZR, X11, X12, 1);
            writes = 0;
            break;
        case 11:
            thumb_addsub_flags(e, XZR, X11, X12, 0);
            writes = 0;
            break;
        case 12:
            emit(e, 0x2A000000u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
            break;
        case 13:
            emit(e, 0x1B007C00u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
            break;
        case 14:
            emit(e, 0x0A200000u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
            break;
        case 15:
            emit(e, 0x2A2003EDu | ((uint32_t)X12 << 16));
            break;
        default:
            return 0;
        }
        if (writes) guest_store(e, X13, rd);
        if (kind != 9 && kind != 10 && kind != 11) set_nz(e, X13);
        return 1;
    }

    if (BITS(insn, 15, 10) == 0x11) {
        uint32_t kind = BITS(insn, 9, 8), rm = BITS(insn, 6, 3);
        uint32_t rd = (BIT(insn, 7) << 3) | BITS(insn, 2, 0);
        thumb_guest_load(e, X11, rm, pc);
        if (kind == 0) {
            thumb_guest_load(e, X12, rd, pc);
            emit(e, 0x0B000000u | ((uint32_t)X11 << 16) | ((uint32_t)X12 << 5) | X13);
            if (rd == 15) {
                set_thumb_pc_reg_and_return(e, X13, e->count + 1);
                e->ended_uncond = 1;
            } else
                guest_store(e, X13, rd);
        } else if (kind == 1) {
            thumb_guest_load(e, X12, rd, pc);
            thumb_addsub_flags(e, XZR, X12, X11, 1);
        } else if (kind == 2) {
            if (rd == 15) {
                set_pc_reg_and_return(e, X11, e->count + 1);
                e->ended_uncond = 1;
            } else
                guest_store(e, X11, rd);
        } else {
            if (BIT(insn, 7)) {
                mov_w_imm(e, X12, (pc + 2) | 1u);
                guest_store(e, X12, MR_R_LR);
                e->is_call = 1;
            }
            set_pc_reg_and_return(e, X11, e->count + 1);
            e->ended_uncond = 1;
        }
        return 1;
    }

    if (op == 9) {
        uint32_t addr = ((pc + 4) & ~3u) + BITS(insn, 7, 0) * 4u;
        mov_w_imm(e, X13, addr);
        thumb_memory(e, X13, BITS(insn, 10, 8), 4, 1, 0, pc);
        return 1;
    }

    if (BITS(insn, 15, 12) == 5) {
        uint32_t kind = BITS(insn, 11, 9);
        thumb_guest_load(e, X11, BITS(insn, 5, 3), pc);
        thumb_guest_load(e, X12, BITS(insn, 8, 6), pc);
        emit(e, 0x0B000000u | ((uint32_t)X12 << 16) | ((uint32_t)X11 << 5) | X13);
        const uint8_t sizes[] = {4, 2, 1, 1, 4, 2, 1, 2};
        thumb_memory(e, X13, BITS(insn, 2, 0), sizes[kind], kind >= 3, kind == 3 || kind == 7, pc);
        return 1;
    }

    if (BITS(insn, 15, 13) == 3 || BITS(insn, 15, 12) == 8) {
        uint32_t half = BITS(insn, 15, 12) == 8;
        uint32_t byte = !half && BIT(insn, 12);
        uint32_t load = BIT(insn, 11);
        uint32_t scale = half ? 2u : byte ? 1u : 4u;
        thumb_guest_load(e, X11, BITS(insn, 5, 3), pc);
        thumb_add_imm(e, X13, X11, BITS(insn, 10, 6) * scale, 0);
        thumb_memory(e, X13, BITS(insn, 2, 0), scale, load, 0, pc);
        return 1;
    }

    if (BITS(insn, 15, 12) == 9) {
        thumb_guest_load(e, X11, MR_R_SP, pc);
        thumb_add_imm(e, X13, X11, BITS(insn, 7, 0) * 4u, 0);
        thumb_memory(e, X13, BITS(insn, 10, 8), 4, BIT(insn, 11), 0, pc);
        return 1;
    }

    if (BITS(insn, 15, 12) == 10) {
        uint32_t base = BIT(insn, 11) ? 0 : ((pc + 4) & ~3u);
        if (BIT(insn, 11))
            thumb_guest_load(e, X11, MR_R_SP, pc);
        else
            mov_w_imm(e, X11, base);
        thumb_add_imm(e, X13, X11, BITS(insn, 7, 0) * 4u, 0);
        guest_store(e, X13, BITS(insn, 10, 8));
        return 1;
    }

    if (BITS(insn, 15, 12) == 11) {
        uint32_t sub = BITS(insn, 11, 8);
        if (sub == 1 || sub == 3 || sub == 9 || sub == 11) {
            uint32_t nz = BIT(insn, 11), rn = BITS(insn, 2, 0);
            uint32_t target = pc + 4 + (BIT(insn, 9) << 6) + (BITS(insn, 7, 3) << 1);
            e->has_direct_target = 1;
            e->direct_target = target;
            thumb_guest_load(e, X11, rn, pc);
            emit(e, 0x7100001Fu | ((uint32_t)X11 << 5));
            uint32_t *not_taken = e->out;
            emit(e, 0x54000000u | (nz ? 0u : 1u));
            direct_exit(e, target, 1, e->count + 1);
            patch_b19(not_taken, e->out);
            direct_exit(e, pc + 2, 1, e->count + 1);
            e->ended = 1;
            return 1;
        }
        if (sub == 0) {
            thumb_guest_load(e, X11, MR_R_SP, pc);
            thumb_add_imm(e, X13, X11, BITS(insn, 6, 0) * 4u, BIT(insn, 7));
            guest_store(e, X13, MR_R_SP);
            return 1;
        }
        if (sub == 2) {
            uint32_t rd = BITS(insn, 2, 0), rm = BITS(insn, 5, 3);
            thumb_guest_load(e, X11, rm, pc);
            uint32_t kind = BITS(insn, 7, 6);
            uint32_t base = kind < 2 ? 0x13000000u : 0x53000000u;
            uint32_t width = (kind & 1u) ? 7u : 15u;
            emit(e, base | (width << 10) | ((uint32_t)X11 << 5) | X13);
            guest_store(e, X13, rd);
            return 1;
        }
        if (sub == 4 || sub == 5) {
            uint32_t list = BITS(insn, 7, 0) | (BIT(insn, 8) << 14);
            return compile_thumb_stack(e, list, 0, pc, MR_R_SP, 1);
        }
        if (sub == 12 || sub == 13) {
            uint32_t list = BITS(insn, 7, 0) | (BIT(insn, 8) << 15);
            return compile_thumb_stack(e, list, 1, pc, MR_R_SP, 1);
        }
        if (sub == 15) return BITS(insn, 3, 0) == 0; // Hint; IT stays on the reference path.
        return 0;
    }

    if (BITS(insn, 15, 12) == 12) {
        uint32_t rn = BITS(insn, 10, 8), list = BITS(insn, 7, 0);
        int load = BIT(insn, 11);
        int writeback = !load || !(list & (1u << rn));
        return compile_thumb_stack(e, list, load, pc, rn, writeback);
    }

    if (BITS(insn, 15, 12) == 13) {
        uint32_t cond = BITS(insn, 11, 8);
        if (cond >= 14) return 0;
        uint32_t *not_taken = guard_condition(e, cond);
        int32_t off = (int32_t)(int8_t)BITS(insn, 7, 0) * 2;
        e->has_direct_target = 1;
        e->direct_target = (uint32_t)((int32_t)pc + 4 + off);
        direct_exit(e, e->direct_target, 1, e->count + 1);
        patch_b19(not_taken, e->out);
        direct_exit(e, pc + 2, 1, e->count + 1);
        e->ended = 1;
        return 1;
    }

    if (op == 28) {
        uint32_t raw = (insn & 0x7FFu) << 1;
        int32_t off = (raw & 0x800u) ? (int32_t)(raw | 0xFFFFF000u) : (int32_t)raw;
        e->has_direct_target = 1;
        e->direct_target = (uint32_t)((int32_t)pc + 4 + off);
        direct_exit(e, e->direct_target, 1, e->count + 1);
        e->ended = 1;
        e->ended_uncond = 1;
        return 1;
    }
    return 0;
}

// Block translation.

static emitter block_start(void) {
    emitter e = {
        .start = (uint32_t *)(J.code + J.used),
        .out = (uint32_t *)(J.code + J.used),
        .end = (uint32_t *)(J.code + J.used + BLOCK_ROOM),
        .cpu = J.cpu,
        .direct_link_mark = DIRECT_LINK_COUNT,
    };
    return e;
}

static mr_a64_block_fn block_finish(emitter *e, uint32_t end_pc, uint32_t chain_pc) {
    if (!e->ended && e->count) {
        if (chain_pc && end_pc == chain_pc) {
            chain_forward(e, end_pc, e->count);
            e->chained = 1;
        } else {
            set_pc_imm(e, end_pc);
            return_count(e, e->count);
        }
    }
    size_t bytes = (size_t)((uint8_t *)e->out - (J.code + J.used));
    if (e->failed || !e->count || bytes > BLOCK_ROOM) {
        DIRECT_LINK_COUNT = e->direct_link_mark;
        return NULL;
    }
    J.used += bytes;
    return block_from_code((uint8_t *)e->start);
}

static void emit_bailout(uint32_t pc) {
    if (J.used + 64u > J.capacity) return;
    emitter e = {.start = (uint32_t *)(J.code + J.used),
                 .out = (uint32_t *)(J.code + J.used),
                 .end = (uint32_t *)(J.code + J.used + 64u),
                 .cpu = J.cpu};
    set_pc_imm(&e, pc);
    return_count(&e, 0);
    if (!e.failed) J.used += (size_t)((uint8_t *)e.out - (J.code + J.used));
}

typedef enum { STOP_FALL, STOP_UNCOND, STOP_UNKNOWN } stop_kind;

typedef struct {
    uint32_t pc;
    stop_kind kind;
    uint32_t target;
    uint32_t unknown_size;
    uint32_t insns;
    int chained;
    int has_target;
    int unknown_falls_through;
    int ends_with_call;
    int target_thumb;
} block_stop;

static int a32_falls_through(uint32_t insn) {
    uint32_t op = (insn >> 25) & 7u;

    if (op == 5u) return 0; // B, BL
    if ((insn & 0x0FFFFFF0u) == 0x012FFF10u || (insn & 0x0FFFFFF0u) == 0x012FFF30u)
        return 0;                                      // BX, BLX
    if ((insn & 0x0F000000u) == 0x0F000000u) return 0; // SVC

    if (((insn >> 26) & 3u) == 0u && ((insn >> 12) & 15u) == 15u) return 0;

    // Single data load into PC.
    if (((insn >> 26) & 3u) == 1u && (insn & (1u << 20)) && ((insn >> 12) & 15u) == 15u) return 0;

    // LDM/POP that also loads PC.
    if (op == 4u && (insn & (1u << 20)) && (insn & (1u << 15))) return 0;

    if (((insn >> 25) & 7u) == 7u && !(insn & (1u << 24)) && ((insn >> 12) & 15u) == 15u) return 0;

    return 1;
}

static mr_a64_block_fn compile_a32_block(uint32_t pc, uint32_t max_insns, block_stop *stop,
                                         uint32_t chain_pc) {
    if (!max_insns) return NULL;
    if (J.used + BLOCK_ROOM > J.capacity) {
        fprintf(stderr, "generated code image is full (%.1f MB)\n",
                (double)J.capacity / (1024.0 * 1024.0));
        exit(1);
    }
    emitter e = block_start();

    int unknown = 0;
    uint32_t unknown_insn = 0;
    for (e.count = 0; e.count < max_insns && !e.ended; e.count++) {
        uint32_t at = pc + e.count * 4u;
        if (!mr_mem_ok(J.cpu, at, 4)) break;
        uint32_t insn;
        memcpy(&insn, mr_mem(J.cpu, at), 4);
        uint32_t *before = e.out;
        if (!compile_one(&e, insn, at)) {
            e.out = before;
            unknown = 1;
            unknown_insn = insn;
            break;
        }
    }

    uint32_t end = pc + e.count * 4u;
    if (stop) {
        *stop = (block_stop){
            .pc = end,
            .kind = e.ended_uncond ? STOP_UNCOND
                    : unknown      ? STOP_UNKNOWN
                                   : STOP_FALL,
            .target = e.direct_target,
            .unknown_size = unknown ? 4u : 0u,
            .has_target = e.has_direct_target,
            .unknown_falls_through = unknown && a32_falls_through(unknown_insn),
            .ends_with_call = e.is_call,
            .target_thumb = e.direct_target_thumb,
        };
    }
    mr_a64_block_fn fn = block_finish(&e, end, chain_pc);
    if (stop) {
        stop->chained = e.chained;
        stop->insns = e.count;
    }
    return fn;
}

static mr_a64_block_fn compile_thumb_block(uint32_t pc, uint32_t max_halfwords, block_stop *stop,
                                           uint32_t chain_pc) {
    if (!max_halfwords) return NULL;
    if (J.used + BLOCK_ROOM > J.capacity) {
        fprintf(stderr, "generated code image is full (%.1f MB)\n",
                (double)J.capacity / (1024.0 * 1024.0));
        exit(1);
    }
    emitter e = block_start();
    e.direct_target_thumb = 1; // Targets from a Thumb block remain in Thumb mode.

    uint32_t at = pc;
    uint32_t consumed = 0;
    uint32_t unknown_size = 0;
    while (e.count < MAX_BLOCK && consumed < max_halfwords && !e.ended && mr_mem_ok(J.cpu, at, 2)) {
        uint16_t insn;
        memcpy(&insn, mr_mem(J.cpu, at), 2);
        uint32_t top = insn >> 11;
        if (top == 29 || top == 30 || top == 31) {
            uint16_t second = 0;
            if (consumed + 2u > max_halfwords || !mr_mem_ok(J.cpu, at, 4)) break;
            memcpy(&second, mr_mem(J.cpu, at + 2u), 2);
            uint32_t *before = e.out;
            if (!compile_thumb32(&e, insn, second, at)) {
                e.out = before;
                unknown_size = 4u;
                break;
            }
            e.count++;
            at += 4u;
            consumed += 2u;
            continue;
        }
        uint32_t *before = e.out;
        if (!compile_thumb16(&e, insn, at)) {
            e.out = before;
            unknown_size = 2u;
            break;
        }
        e.count++;
        at += 2u;
        consumed++;
    }

    if (stop) {
        *stop = (block_stop){
            .pc = at,
            .kind = e.ended_uncond ? STOP_UNCOND
                    : unknown_size ? STOP_UNKNOWN
                                   : STOP_FALL,
            .target = e.direct_target,
            .unknown_size = unknown_size,
            .has_target = e.has_direct_target,
            .unknown_falls_through = 0,
            .ends_with_call = e.is_call,
            .target_thumb = e.direct_target_thumb,
        };
    }
    mr_a64_block_fn fn = block_finish(&e, at, chain_pc);
    if (stop) {
        stop->chained = e.chained;
        stop->insns = e.count;
    }
    return fn;
}

static thumb_slot *compiler_thumb_slot(uint32_t pc) {
    uint32_t i = ((pc >> 1) * 2654435761u) & (THUMB_SLOTS - 1u);
    for (uint32_t n = 0; n < THUMB_SLOTS; n++, i = (i + 1) & (THUMB_SLOTS - 1u))
        if (!J.thumb_blocks[i].fn || J.thumb_blocks[i].pc == pc) return &J.thumb_blocks[i];
    return NULL;
}

static uint64_t FINGERPRINT;

typedef struct {
    uint32_t pc;
    uint32_t opcode;
    uint8_t thumb;
    uint8_t width;
} rejected_entry;

static rejected_entry *REJECTED;
static size_t REJECTED_N, REJECTED_CAP;

static void record_rejected(uint32_t pc, int thumb, uint32_t opcode, uint32_t width) {
    if (REJECTED_N == REJECTED_CAP) {
        size_t next = REJECTED_CAP ? REJECTED_CAP * 2u : 1024u;
        rejected_entry *grown = realloc(REJECTED, next * sizeof *grown);
        if (!grown) {
            perror("realloc");
            exit(1);
        }
        REJECTED = grown;
        REJECTED_CAP = next;
    }
    REJECTED[REJECTED_N++] =
        (rejected_entry){.pc = pc, .opcode = opcode, .thumb = thumb != 0, .width = (uint8_t)width};
}

static int write_rejected_report(const char *path) {
    if (!path || !*path) return -1;

    size_t length = strlen(path) + 48u;
    char *tmp = malloc(length);
    if (!tmp) return -1;
    snprintf(tmp, length, "%s.tmp.%ld", path, (long)getpid());

    FILE *file = fopen(tmp, "w");
    if (!file) {
        free(tmp);
        return -1;
    }
    int failed =
        fprintf(file, "# minion-rejected-candidates-v1 0x%016llx\nmode\tpc\twidth\topcode\n",
                (unsigned long long)FINGERPRINT) < 0;
    for (size_t i = 0; i < REJECTED_N; i++) {
        const rejected_entry *entry = &REJECTED[i];
        if (fprintf(file, "%s\t0x%08x\t%u\t0x%0*x\n", entry->thumb ? "THUMB" : "A32", entry->pc,
                    entry->width, entry->width * 2, entry->opcode) < 0)
            failed = 1;
    }
    if (fclose(file) != 0) failed = 1;
    if (!failed && rename(tmp, path) == 0) {
        printf("   rejected static candidates: %zu, report: %s\n", REJECTED_N, path);
        free(tmp);
        return 0;
    }
    remove(tmp);
    free(tmp);
    return -1;
}

static const mr_elf_image *IMAGE;
static uint8_t *A32_SEEN;
static uint8_t *OFFLINE_HOOK;
static uint8_t *FUNC_START;
static uint8_t *THUMB_SEEN;
static uint8_t *THUMB_CODE;
static uint32_t *A32_WORK, *THUMB_WORK, *EXIDX_WORK;
static size_t A32_WORK_N, A32_WORK_CAP;
static size_t THUMB_WORK_N, THUMB_WORK_CAP;
static size_t EXIDX_WORK_N, EXIDX_WORK_CAP;
static size_t THUMB_COUNT;
static size_t EXIDX_THUMB_PROLOGUES;

static void append_work(uint32_t **items, size_t *count, size_t *capacity, uint32_t pc) {
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2u : (1u << 15);
        uint32_t *grown = realloc(*items, next * sizeof **items);
        if (!grown) {
            perror("realloc");
            exit(1);
        }
        *items = grown;
        *capacity = next;
    }
    (*items)[(*count)++] = pc;
}

static int bit_is_set(const uint8_t *bits, uint32_t bit) {
    return (bits[bit >> 3] >> (bit & 7u)) & 1u;
}

static int thumb_code(uint32_t pc) {
    return bit_is_set(THUMB_CODE, pc >> 1);
}

static void want_thumb(uint32_t pc) {
    pc &= ~1u;
    if (pc < IMAGE->exec_start || pc >= IMAGE->exec_end) return;
    uint32_t bit = pc >> 1;
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    if (THUMB_SEEN[bit >> 3] & mask) return;
    THUMB_SEEN[bit >> 3] |= mask;
    append_work(&THUMB_WORK, &THUMB_WORK_N, &THUMB_WORK_CAP, pc);
}

static void mark_func_start(uint32_t pc) {
    pc &= ~1u;
    if (pc < IMAGE->exec_start || pc >= IMAGE->exec_end) return;
    FUNC_START[(pc - IMAGE->exec_start) >> 2] = 1;
}

static int is_func_start(uint32_t pc) {
    if (pc < IMAGE->exec_start || pc >= IMAGE->exec_end) return 1; // Conservative boundary.
    return FUNC_START[(pc - IMAGE->exec_start) >> 2];
}

static int is_offline_hook(uint32_t pc) {
    uint32_t index = (pc & ~1u) >> 2;
    return index < J.block_count && OFFLINE_HOOK[index];
}

static void want_a32(uint32_t pc) {
    if ((pc & 3u) || pc < IMAGE->exec_start || pc >= IMAGE->exec_end) return;
    if (thumb_code(pc)) return;
    uint32_t index = pc >> 2;
    if (index >= J.block_count || A32_SEEN[index]) return;
    A32_SEEN[index] = 1;
    append_work(&A32_WORK, &A32_WORK_N, &A32_WORK_CAP, pc);
}

static uint32_t thumb_width(uint32_t pc) {
    if (!mr_mem_ok(J.cpu, pc, 2)) return 0;
    uint16_t half;
    memcpy(&half, mr_mem(J.cpu, pc), sizeof half);
    uint32_t top = half >> 11;
    return top == 29u || top == 30u || top == 31u ? 4u : 2u;
}

static void mark_thumb_code(uint32_t begin, uint32_t end) {
    if (begin < IMAGE->exec_start) begin = IMAGE->exec_start;
    if (end > IMAGE->exec_end) end = IMAGE->exec_end;
    for (uint32_t pc = begin & ~1u; pc < end; pc += 2u) {
        uint32_t bit = pc >> 1;
        THUMB_CODE[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
    }
}

static void scan_known_entry(uint32_t addr, uint32_t size, void *user) {
    (void)user;
    mark_func_start(addr);
    if (!(addr & 1u)) {
        want_a32(addr);
        return;
    }

    uint32_t begin = addr & ~1u;
    uint64_t raw_end = (uint64_t)begin + (size ? size : 2u);
    uint32_t end = raw_end > IMAGE->exec_end ? IMAGE->exec_end : (uint32_t)raw_end;
    mark_thumb_code(begin, end);

    for (uint32_t pc = begin; pc < end;) {
        want_thumb(pc);
        uint32_t width = thumb_width(pc);
        if (!width || width > end - pc) break;
        pc += width;
    }
}

static int compare_u32(const void *left, const void *right);

static void collect_exidx(uint32_t addr, uint32_t size, void *user) {
    (void)size;
    (void)user;
    append_work(&EXIDX_WORK, &EXIDX_WORK_N, &EXIDX_WORK_CAP, addr & ~1u);
}

static int looks_like_thumb_push(uint32_t addr) {
    if (!mr_mem_ok(J.cpu, addr, 4)) return 0;
    uint16_t first;
    uint32_t word;
    memcpy(&first, mr_mem(J.cpu, addr), sizeof first);
    memcpy(&word, mr_mem(J.cpu, addr), sizeof word);

    return (first & 0xFF00u) == 0xB500u && (word & 0x0E000000u) != 0x0A000000u;
}

static void add_exidx_entries(void) {
    qsort(EXIDX_WORK, EXIDX_WORK_N, sizeof *EXIDX_WORK, compare_u32);
    for (size_t i = 0; i < EXIDX_WORK_N; i++) {
        uint32_t addr = EXIDX_WORK[i];
        mark_func_start(addr);
        if (thumb_code(addr)) {
            want_thumb(addr);
        } else {
            want_a32(addr);
            if (looks_like_thumb_push(addr)) {
                want_thumb(addr);
                EXIDX_THUMB_PROLOGUES++;
            }
        }
    }
}

static int compare_u32(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static size_t JUMP_TABLE_TARGETS;
static size_t AFTER_RETURN_TARGETS;

static uint8_t *LITERAL;

static void mark_literal(uint32_t addr, uint32_t bytes) {
    if (addr < IMAGE->exec_start || addr >= IMAGE->exec_end) return;
    for (uint32_t a = addr & ~3u; a < addr + bytes && a < IMAGE->exec_end; a += 4u)
        LITERAL[(a - IMAGE->exec_start) >> 2] = 1;
}

static int is_literal(uint32_t addr) {
    if (addr < IMAGE->exec_start || addr >= IMAGE->exec_end) return 0;
    return LITERAL[(addr - IMAGE->exec_start) >> 2];
}

static void collect_literals(void) {
    size_t words = ((size_t)IMAGE->exec_end - IMAGE->exec_start) / 4u + 1u;
    LITERAL = calloc(words, 1);
    if (!LITERAL) {
        perror("calloc");
        exit(1);
    }

    for (uint32_t pc = IMAGE->exec_start; pc + 4u <= IMAGE->exec_end; pc += 4u) {
        if (!mr_mem_ok(J.cpu, pc, 4)) continue;
        uint32_t insn;
        memcpy(&insn, mr_mem(J.cpu, pc), 4);
        if (BITS(insn, 31, 28) == 15) continue;
        uint32_t base = pc + 8u;
        int up = BIT(insn, 23);

        // LDR / LDRB rX, [pc, #+-imm12]
        if (BITS(insn, 27, 25) == 2 && !BIT(insn, 25) && BIT(insn, 24) && !BIT(insn, 21) &&
            BIT(insn, 20) && BITS(insn, 19, 16) == 15) {
            uint32_t off = BITS(insn, 11, 0);
            mark_literal(up ? base + off : base - off, 4);
            continue;
        }
        // LDRH / LDRSB / LDRSH / LDRD rX, [pc, #+-imm8]
        if (BITS(insn, 27, 25) == 0 && BIT(insn, 7) && BIT(insn, 4) && BIT(insn, 24) &&
            BIT(insn, 22) && BITS(insn, 19, 16) == 15) {
            uint32_t off = (BITS(insn, 11, 8) << 4) | BITS(insn, 3, 0);
            uint32_t op = BITS(insn, 6, 5);
            mark_literal(up ? base + off : base - off, (!BIT(insn, 20) && op == 2) ? 8u : 4u);
            continue;
        }
        // VLDR sX/dX, [pc, #+-imm8*4]
        if (BITS(insn, 27, 25) == 6 && BIT(insn, 24) && !BIT(insn, 21) && BIT(insn, 20) &&
            BITS(insn, 19, 16) == 15) {
            uint32_t cp = BITS(insn, 11, 8);
            if (cp != 10 && cp != 11) continue;
            uint32_t off = BITS(insn, 7, 0) * 4u;
            mark_literal(up ? base + off : base - off, cp == 11 ? 8u : 4u);
        }
    }
}

static int is_return_insn(uint32_t insn) {
    if (BITS(insn, 31, 28) != 14u) return 0;
    if ((insn & 0x0FFFFFFFu) == 0x012FFF1Eu) return 1;                        // bx lr
    if ((insn & 0x0FFFFFFFu) == 0x01A0F00Eu) return 1;                        // mov pc, lr
    if (BITS(insn, 27, 25) == 4u && BIT(insn, 20) && BIT(insn, 15)) return 1; // ldm/pop {..., pc}
    if (BITS(insn, 27, 25) == 5u && !BIT(insn, 24)) return 1;
    return 0;
}

static void collect_jump_tables(void) {
    for (uint32_t pc = IMAGE->exec_start; pc + 4u <= IMAGE->exec_end; pc += 4u) {
        if (!mr_mem_ok(J.cpu, pc, 4)) continue;
        uint32_t insn;
        memcpy(&insn, mr_mem(J.cpu, pc), 4);
        if (BITS(insn, 31, 28) == 15) continue;

        // add pc, pc, rX, lsl #2
        if ((insn & 0x0FFFFFF0u) == 0x008FF100u) {
            for (uint32_t k = 0; k < 1024u; k++) {
                uint32_t at = pc + 8u + k * 4u;
                if (!mr_mem_ok(J.cpu, at, 4) || at >= IMAGE->exec_end) break;
                uint32_t word;
                memcpy(&word, mr_mem(J.cpu, at), 4);
                if ((word & 0xFF000000u) != 0xEA000000u) break;
                if (!A32_SEEN[at >> 2]) JUMP_TABLE_TARGETS++;
                want_a32(at);
            }
            continue;
        }

        if (is_return_insn(insn)) {
            uint32_t at = pc + 4u;
            for (uint32_t n = 0; n < 256u && is_literal(at); n++)
                at += 4u;
            if (at < IMAGE->exec_end && !is_literal(at) && (at >> 2) < J.block_count &&
                !A32_SEEN[at >> 2]) {
                AFTER_RETURN_TARGETS++;
                want_a32(at);
            }
            continue;
        }

        // ldr pc, [pc, rX, lsl #2]: the table contains addresses.
        if ((insn & 0x0FFFFFF0u) == 0x079FF100u) {
            for (uint32_t k = 0; k < 1024u; k++) {
                uint32_t at = pc + 8u + k * 4u;
                if (!mr_mem_ok(J.cpu, at, 4) || at >= IMAGE->exec_end) break;
                uint32_t target;
                memcpy(&target, mr_mem(J.cpu, at), 4);
                if ((target & 3u) || target < IMAGE->exec_start || target >= IMAGE->exec_end) break;
                mark_literal(at, 4);
                if (!A32_SEEN[target >> 2]) JUMP_TABLE_TARGETS++;
                want_a32(target);
            }
        }
    }
}

static void discover_entries(size_t a32_from, size_t thumb_from) {
    uint8_t *saved_code = J.code;
    size_t saved_capacity = J.capacity;
    size_t saved_used = J.used;
    uint8_t *scratch = malloc(BLOCK_ROOM);
    if (!scratch) {
        perror("malloc");
        exit(1);
    }

    J.code = scratch;
    J.capacity = BLOCK_ROOM;

    for (size_t i = a32_from; i < A32_WORK_N; i++) {
        block_stop stop = {0};
        J.used = 0;
        (void)compile_a32_block(A32_WORK[i], MAX_BLOCK, &stop, 0);
        if (stop.has_target) {
            if (stop.target_thumb)
                want_thumb(stop.target);
            else
                want_a32(stop.target);
        }
        if (stop.ends_with_call && stop.pc > A32_WORK[i]) want_a32(stop.pc);
        if (stop.kind == STOP_FALL && stop.pc > A32_WORK[i])
            want_a32(stop.pc);
        else if (stop.kind == STOP_UNKNOWN && stop.unknown_falls_through)
            want_a32(stop.pc + stop.unknown_size);
    }

    for (size_t i = thumb_from; i < THUMB_WORK_N; i++) {
        block_stop stop = {0};
        J.used = 0;
        (void)compile_thumb_block(THUMB_WORK[i], MAX_BLOCK, &stop, 0);
        if (stop.has_target) {
            if (stop.target_thumb)
                want_thumb(stop.target);
            else
                want_a32(stop.target);
        }
        if (stop.ends_with_call && stop.pc > THUMB_WORK[i]) want_thumb(stop.pc);
        if (stop.kind == STOP_FALL && stop.pc > THUMB_WORK[i]) want_thumb(stop.pc);
    }

    free(scratch);
    J.code = saved_code;
    J.capacity = saved_capacity;
    J.used = saved_used;
}

typedef struct {
    uint32_t *work;
    size_t count;
    uint32_t unit; // Instruction unit in bytes: A32 = 4, Thumb = 2.
    int thumb;
} entry_pass;

static mr_a64_block_fn pass_compile(const entry_pass *p, uint32_t pc, uint32_t max_units,
                                    block_stop *stop, uint32_t chain_pc) {
    return p->thumb ? compile_thumb_block(pc, max_units, stop, chain_pc)
                    : compile_a32_block(pc, max_units, stop, chain_pc);
}

static int pass_store(const entry_pass *p, uint32_t pc, mr_a64_block_fn fn) {
    if (!p->thumb) {
        if ((pc >> 2) >= J.block_count) return 0;
        J.blocks[pc >> 2] = fn;
        return 1;
    }
    thumb_slot *slot = compiler_thumb_slot(pc);
    if (!slot) return 0;
    slot->pc = pc;
    slot->fn = fn;
    return 1;
}

static void pass_record_failure(const entry_pass *p, uint32_t pc) {
    if (!p->thumb) {
        uint32_t opcode = 0;
        if (mr_mem_ok(J.cpu, pc, 4)) memcpy(&opcode, mr_mem(J.cpu, pc), sizeof opcode);
        record_rejected(pc, 0, opcode, 4);
        return;
    }
    uint16_t first = 0, second = 0;
    uint32_t width = thumb_width(pc);
    if (mr_mem_ok(J.cpu, pc, 2)) memcpy(&first, mr_mem(J.cpu, pc), sizeof first);
    if (width == 4 && mr_mem_ok(J.cpu, pc + 2u, 2))
        memcpy(&second, mr_mem(J.cpu, pc + 2u), sizeof second);
    record_rejected(pc, 1, (uint32_t)first | ((uint32_t)second << 16), width ? width : 2u);
}

static size_t CHAINED_BLOCKS, CHAIN_MAX;

static mr_a64_block_fn direct_link_target(uint32_t pc, int thumb) {
    if (!thumb) {
        uint32_t index = pc >> 2;
        return index < J.block_count ? J.blocks[index] : NULL;
    }
    thumb_slot *slot = compiler_thumb_slot(pc);
    return slot && slot->fn && slot->pc == pc ? slot->fn : NULL;
}

static void patch_direct_links(void) {
    for (size_t i = 0; i < DIRECT_LINK_COUNT; i++) {
        const direct_link *link = &DIRECT_LINKS[i];
        uint32_t *branch = (uint32_t *)(void *)(J.code + link->branch_offset);
        if (is_offline_hook(link->target_pc)) {
            DIRECT_LINK_SKIPPED_HOOK++;
            continue;
        }
        mr_a64_block_fn fn = direct_link_target(link->target_pc, link->target_thumb);
        if (!fn) continue;
        uint32_t *target = (uint32_t *)code_from_block(fn);
        if (patch_b26(branch, target)) {
            DIRECT_LINKED++;
            continue;
        }
        uint32_t *veneer = allocate_veneer(target);
        if (veneer && patch_b26(branch, veneer)) {
            DIRECT_LINKED++;
            DIRECT_LINKED_LONG++;
        } else {
            DIRECT_LINK_SKIPPED_RANGE++;
        }
    }
}

static size_t compile_entries(const entry_pass *p) {
    qsort(p->work, p->count, sizeof *p->work, compare_u32);

    size_t compiled = 0;
    uint32_t chain_pending = 0;
    uint32_t chain_insns = 0; // Guest instructions already in the chain.

    for (size_t i = 0; i < p->count; i++) {
        uint32_t pc = p->work[i];
        uint32_t next = i + 1u < p->count ? p->work[i + 1u] : 0;
        uint32_t max_units = MAX_BLOCK;
        if (next) {
            uint32_t distance = (next - pc) / p->unit;
            if (distance < max_units) max_units = distance;
        }

        uint32_t chain_pc = 0;
        if (next && !is_func_start(next)) chain_pc = next;

        block_stop stop = {0};
        mr_a64_block_fn fn = max_units ? pass_compile(p, pc, max_units, &stop, chain_pc) : NULL;

        if (!fn || !pass_store(p, pc, fn)) {
            // Fall-through control needs a normal exit when the next block is absent.
            if (chain_pending == pc) emit_bailout(pc);
            if (max_units) pass_record_failure(p, pc);
            chain_pending = 0;
            chain_insns = 0;
            continue;
        }

        compiled++;
        if (stop.chained) {
            chain_pending = stop.pc;
            chain_insns += stop.insns;
            CHAINED_BLOCKS++;
            if (chain_insns > CHAIN_MAX) CHAIN_MAX = chain_insns;
        } else {
            chain_pending = 0;
            chain_insns = 0;
        }
        if (VENEER_POOL_OFFSET == SIZE_MAX && J.used >= VENEER_POOL_TRIGGER && !chain_pending)
            reserve_veneer_pool();
    }
    if (chain_pending) emit_bailout(chain_pending);
    return compiled;
}

static size_t compile_a32_entries(void) {
    entry_pass pass = {A32_WORK, A32_WORK_N, 4u, 0};
    return compile_entries(&pass);
}

static size_t compile_thumb_entries(void) {
    entry_pass pass = {THUMB_WORK, THUMB_WORK_N, 2u, 1};
    THUMB_COUNT = compile_entries(&pass);
    return THUMB_COUNT;
}

// Game bindings.

typedef struct {
    uint32_t values[512];
    size_t count;
    int overflow;
} address_list;

static void add_address(address_list *list, uint32_t address) {
    address &= ~1u;
    if (!address) return;
    if (list->count >= sizeof list->values / sizeof list->values[0]) {
        list->overflow = 1;
        return;
    }
    list->values[list->count++] = address;
}

static void sort_unique_addresses(address_list *list) {
    qsort(list->values, list->count, sizeof list->values[0], compare_u32);
    size_t unique = 0;
    for (size_t i = 0; i < list->count; i++) {
        if (unique && list->values[unique - 1] == list->values[i]) continue;
        list->values[unique++] = list->values[i];
    }
    list->count = unique;
}

static int has_blocked_prefix(const char *name) {
    for (size_t i = 0; i < sizeof GAME_BLOCKED_PREFIXES / sizeof GAME_BLOCKED_PREFIXES[0]; i++) {
        size_t length = strlen(GAME_BLOCKED_PREFIXES[i]);
        if (strncmp(name, GAME_BLOCKED_PREFIXES[i], length) == 0) return 1;
    }
    return 0;
}

static void collect_prefixed_blocked(const char *name, uint32_t address, void *user) {
    if (has_blocked_prefix(name)) add_address((address_list *)user, address);
}

static int collect_blocked_addresses(const mr_elf_image *image, address_list *blocked) {
    for (size_t i = 0; i < sizeof GAME_BLOCKED_SYMBOLS / sizeof GAME_BLOCKED_SYMBOLS[0]; i++) {
        uint32_t address = mr_elf_lookup(image, GAME_BLOCKED_SYMBOLS[i]);
        if (!address) {
            fprintf(stderr, "game-specific offline symbol is missing: %s\n",
                    GAME_BLOCKED_SYMBOLS[i]);
            return -1;
        }
        add_address(blocked, address);
    }
    mr_elf_each_named_function(image, collect_prefixed_blocked, blocked);
    if (blocked->overflow) {
        fprintf(stderr, "too many blocked game functions for the binding table\n");
        return -1;
    }

    sort_unique_addresses(blocked);
    if (blocked->count <= sizeof GAME_BLOCKED_SYMBOLS / sizeof GAME_BLOCKED_SYMBOLS[0]) {
        fprintf(stderr, "no game function with the GameAPI prefix was found\n");
        return -1;
    }
    return 0;
}

static int collect_offline_hooks(const mr_elf_image *image, const address_list *blocked,
                                 address_list *hooks) {
    for (size_t i = 0; i < blocked->count; i++)
        add_address(hooks, blocked->values[i]);
    for (size_t i = 0; i < sizeof GAME_HOOK_SYMBOLS / sizeof GAME_HOOK_SYMBOLS[0]; i++) {
        uint32_t address = mr_elf_lookup(image, GAME_HOOK_SYMBOLS[i]);
        if (!address) {
            fprintf(stderr, "offline-hook symbol is missing: %s\n", GAME_HOOK_SYMBOLS[i]);
            return -1;
        }
        add_address(hooks, address);
    }
    if (hooks->overflow) {
        fprintf(stderr, "too many offline hooks for the binding table\n");
        return -1;
    }
    sort_unique_addresses(hooks);
    return 0;
}

static int mark_offline_hook(uint32_t address) {
    size_t index = (address & ~1u) >> 2;
    if (index >= J.block_count) {
        fprintf(stderr, "offline hook lies outside executable address space: 0x%08x\n", address);
        return -1;
    }
    OFFLINE_HOOK[index] = 1;
    return 0;
}

static int build_offline_hook_map(const address_list *hooks) {
    for (size_t i = 0; i < hooks->count; i++)
        if (mark_offline_hook(hooks->values[i]) != 0) return -1;
    return 0;
}

static int write_game_bindings(const mr_elf_image *image, const address_list *blocked,
                               const address_list *hooks, const char *path) {
    FILE *out = fopen(path, "w");
    if (!out) return -1;
    int failed = 0;
    fprintf(out, "// Generated by a64-compiler from the validated 1.8.1g engine. Do not edit.\n");
    fprintf(out, "#ifndef MR_GAME_BINDINGS_H\n#define MR_GAME_BINDINGS_H\n\n");
    fprintf(out, "#include <stdint.h>\n\n");
    fprintf(out, "#define MR_GAME_BINDING_HASH UINT64_C(0x%016llx)\n",
            (unsigned long long)FINGERPRINT);
    fprintf(out, "#define MR_GAME_SYMBOL_COUNT %du\n", GAME_REQUIRED_SYMBOL_COUNT);

#define MR_GAME_SYMBOL(id, name)                                                                   \
    do {                                                                                           \
        uint32_t address = mr_elf_lookup(image, name);                                             \
        if (!address) {                                                                            \
            fprintf(stderr, "game symbol is missing: %s\n", name);                                 \
            failed = 1;                                                                            \
        } else {                                                                                   \
            fprintf(out, "#define MR_GAME_%s 0x%08xu\n", #id, address);                            \
        }                                                                                          \
    } while (0);
#define MR_GAME_HOOK_SYMBOL(id, name) MR_GAME_SYMBOL(id, name)
#define MR_BLOCKED_SYMBOL(name)
#define MR_BLOCKED_PREFIX(prefix)
#include "game_symbols.def"
#undef MR_GAME_SYMBOL
#undef MR_GAME_HOOK_SYMBOL
#undef MR_BLOCKED_SYMBOL
#undef MR_BLOCKED_PREFIX

    fprintf(out, "\nstatic const uint32_t MR_GAME_BLOCKED_FUNCTIONS[] = {\n");
    for (size_t i = 0; i < blocked->count; i++)
        fprintf(out, "    0x%08xu,%s", blocked->values[i], (i + 1) % 4 == 0 ? "\n" : " ");
    if (blocked->count % 4) fputc('\n', out);
    fprintf(out, "};\n");
    fprintf(out, "#define MR_GAME_BLOCKED_FUNCTION_COUNT %zuu\n", blocked->count);
    fprintf(out, "#define MR_GAME_EXACT_BLOCKED_FUNCTION_COUNT %zuu\n",
            sizeof GAME_BLOCKED_SYMBOLS / sizeof GAME_BLOCKED_SYMBOLS[0]);

    fprintf(out, "\nstatic const uint32_t MR_GAME_OFFLINE_HOOK_FUNCTIONS[] = {\n");
    for (size_t i = 0; i < hooks->count; i++)
        fprintf(out, "    0x%08xu,%s", hooks->values[i], (i + 1) % 4 == 0 ? "\n" : " ");
    if (hooks->count % 4) fputc('\n', out);
    fprintf(out, "};\n");
    fprintf(out, "#define MR_GAME_OFFLINE_HOOK_FUNCTION_COUNT %zuu\n", hooks->count);
    fprintf(out, "\n#endif\n");

    if (fclose(out) != 0) failed = 1;
    if (failed) {
        remove(path);
    } else {
        printf("   game bindings: %d fixed entry points, %zu blocked services, "
               "%zu offline hooks\n",
               GAME_REQUIRED_SYMBOL_COUNT, blocked->count, hooks->count);
    }
    return failed ? -1 : 0;
}

// Output.

static int write_image(const char *code_path, const char *map_path) {
    FILE *code = fopen(code_path, "wb");
    if (!code) return -1;
    int failed = fwrite(J.code, 1, J.used, code) != J.used;
    if (fclose(code) != 0 || failed) return -1;

    size_t thumb_n = 0;
    for (uint32_t i = 0; i < THUMB_SLOTS; i++)
        if (J.thumb_blocks[i].fn) thumb_n++;
    thumb_slot *thumbs = malloc((thumb_n ? thumb_n : 1) * sizeof *thumbs);
    if (!thumbs) return -1;
    size_t k = 0;
    for (uint32_t i = 0; i < THUMB_SLOTS; i++)
        if (J.thumb_blocks[i].fn) thumbs[k++] = J.thumb_blocks[i];
    for (size_t i = 1; i < thumb_n; i++) { // Insertion sort.
        thumb_slot v = thumbs[i];
        size_t j = i;
        while (j && thumbs[j - 1].pc > v.pc) {
            thumbs[j] = thumbs[j - 1];
            j--;
        }
        thumbs[j] = v;
    }

    FILE *map = fopen(map_path, "w");
    if (!map) {
        free(thumbs);
        return -1;
    }
    fprintf(map, "// Generated by a64-compiler. Do not edit.\n");
    fprintf(map, "#define MR_GAME_BLOCK_HASH UINT64_C(0x%016llx)\n",
            (unsigned long long)FINGERPRINT);
    for (uint32_t i = 0; i < J.block_count; i++)
        if (J.blocks[i])
            fprintf(map, "A32_BLOCK(0x%08xu, %zuu)\n", i << 2,
                    (size_t)(code_from_block(J.blocks[i]) - J.code));
    for (size_t i = 0; i < thumb_n; i++)
        fprintf(map, "THUMB_BLOCK(0x%08xu, %zuu)\n", thumbs[i].pc,
                (size_t)(code_from_block(thumbs[i].fn) - J.code));
    free(thumbs);
    return fclose(map) == 0 ? 0 : -1;
}

// Entry point.

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr,
                "usage: %s <engine.so> <code.bin> <map.inc> <rejected.tsv> <game-bindings.h>\n",
                argv[0]);
        return 2;
    }

    mr_elf_image image;
    if (mr_elf_load(argv[1], &image, 0) != 0) {
        fprintf(stderr, "%s: %s\n", argv[1], image.error);
        return 1;
    }

    static mr_cpu cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.mem_host = image.host_base;
    cpu.mem_guest_base = image.guest_base;
    cpu.mem_size = (uint32_t)image.span;
    cpu.ro_start = image.ro_start;
    cpu.ro_end = image.ro_end;
    cpu.exec_start = image.exec_start;
    cpu.exec_end = image.exec_end;
    FINGERPRINT = mr_elf_fingerprint(&image);

    IMAGE = &image;
    J.cpu = &cpu;
    J.block_count = (image.exec_end + 3u) / 4u;
    J.blocks = calloc(J.block_count, sizeof *J.blocks);
    J.thumb_blocks = calloc(THUMB_SLOTS, sizeof *J.thumb_blocks);
    A32_SEEN = calloc(J.block_count, 1);
    OFFLINE_HOOK = calloc(J.block_count, 1);
    FUNC_START = calloc(((size_t)image.exec_end - image.exec_start) / 4u + 1u, 1);
    size_t thumb_seen_bytes = (((size_t)image.exec_end >> 1) + 7u) / 8u;
    THUMB_SEEN = calloc(thumb_seen_bytes ? thumb_seen_bytes : 1u, 1);
    THUMB_CODE = calloc(thumb_seen_bytes ? thumb_seen_bytes : 1u, 1);
    if (!J.blocks || !J.thumb_blocks || !A32_SEEN || !OFFLINE_HOOK || !THUMB_SEEN || !THUMB_CODE ||
        !FUNC_START) {
        fprintf(stderr, "cannot allocate compiler memory\n");
        mr_elf_free(&image);
        return 1;
    }

    mr_elf_each_function(&image, scan_known_entry, NULL);
    mr_elf_each_code_pointer(&image, scan_known_entry, NULL);
    mr_elf_each_exidx_start(&image, collect_exidx, NULL);
    add_exidx_entries();
    collect_literals();
    collect_jump_tables();
    size_t initial_a32 = A32_WORK_N;
    size_t initial_thumb = THUMB_WORK_N;
    discover_entries(0, 0);

    printf("   discovery: %zu -> %zu A32, %zu -> %zu Thumb entry points"
           " (exidx Thumb prologues: %zu, jump table: %zu, "
           "after return: %zu)\n",
           initial_a32, A32_WORK_N, initial_thumb, THUMB_WORK_N, EXIDX_THUMB_PROLOGUES,
           JUMP_TABLE_TARGETS, AFTER_RETURN_TARGETS);

    address_list blocked = {0}, hooks = {0};
    if (collect_blocked_addresses(&image, &blocked) != 0 ||
        collect_offline_hooks(&image, &blocked, &hooks) != 0 ||
        build_offline_hook_map(&hooks) != 0) {
        mr_elf_free(&image);
        return 1;
    }

    J.code = mmap(NULL, CODE_CAPACITY, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    J.capacity = CODE_CAPACITY;
    J.used = 0;
    if (J.code == MAP_FAILED) {
        J.code = NULL;
        fprintf(stderr, "cannot allocate code-image memory\n");
        mr_elf_free(&image);
        return 1;
    }

    RECORD_DIRECT_LINKS = 1;
    size_t a32 = compile_a32_entries();
    size_t thumb = compile_thumb_entries();
    RECORD_DIRECT_LINKS = 0;
    if (VENEER_POOL_OFFSET == SIZE_MAX) reserve_veneer_pool();
    patch_direct_links();
    int rc = write_image(argv[2], argv[3]);
    if (write_rejected_report(argv[4]) != 0) {
        fprintf(stderr, "failed to write the rejected-candidate report\n");
        rc = -1;
    }
    if (write_game_bindings(&image, &blocked, &hooks, argv[5]) != 0) {
        fprintf(stderr, "failed to generate the game-specific binding table\n");
        rc = -1;
    }
    if (rc != 0) {
        fprintf(stderr, "failed to write translated code\n");
    } else {
        printf("   %zu A32 + %zu Thumb blocks, %.1f MB machine code\n", a32, thumb,
               (double)J.used / (1024.0 * 1024.0));
        printf("   linear chains: %zu (%.1f%%), longest chain %zu instructions\n", CHAINED_BLOCKS,
               100.0 * (double)CHAINED_BLOCKS / (double)(a32 + thumb), CHAIN_MAX);
        printf("   direct branches: %zu/%zu linked (%zu long), "
               "%zu hook targets, %zu out of range\n",
               DIRECT_LINKED, DIRECT_LINK_COUNT, DIRECT_LINKED_LONG, DIRECT_LINK_SKIPPED_HOOK,
               DIRECT_LINK_SKIPPED_RANGE);
        printf("   far-branch veneers: %zu/%u used, %.1f KiB reserved\n", VENEER_USED,
               VENEER_CAPACITY,
               (double)(VENEER_CAPACITY * VENEER_WORDS * sizeof(uint32_t)) / 1024.0);
    }

    munmap(J.code, CODE_CAPACITY);
    free(J.blocks);
    free(J.thumb_blocks);
    free(A32_SEEN);
    free(OFFLINE_HOOK);
    free(FUNC_START);
    free(THUMB_SEEN);
    free(THUMB_CODE);
    free(LITERAL);
    free(A32_WORK);
    free(THUMB_WORK);
    free(EXIDX_WORK);
    free(REJECTED);
    free(DIRECT_LINKS);
    mr_elf_free(&image);
    return rc != 0;
}
