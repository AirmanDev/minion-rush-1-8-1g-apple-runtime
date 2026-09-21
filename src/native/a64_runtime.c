// Runtime for the ARM64 blocks generated at build time.
//
// Guest instructions have exactly one execution path: a matching compiled
// block. Missing blocks, unsupported block exits and mid-IT entries are fatal.
// Host imports and kuser helpers are ABI boundaries handled by guest_runtime.c.

#include "a64_runtime.h"

#include "elf_loader.h"
#include "game_bindings.h"
#include "guest_runtime.h"
#include "offline_mode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THUMB_SLOTS 32768u

typedef struct {
    uint32_t pc;
    uint32_t offset;
    uint8_t thumb;
} block_desc;

#define A32_BLOCK(pc, offset) {(pc), (offset), 0},
#define THUMB_BLOCK(pc, offset) {(pc), (offset), 1},
static const block_desc GAME_BLOCKS[] = {
#include "minion_map.inc"
};
#undef A32_BLOCK
#undef THUMB_BLOCK

#define BLOCK_COUNT (sizeof(GAME_BLOCKS) / sizeof(GAME_BLOCKS[0]))

#define ENTRY_HOOK_BIT ((uintptr_t)1)

typedef struct {
    uint32_t pc;
    uintptr_t entry; // Block address plus ENTRY_HOOK_BIT.
    uint8_t used;
} thumb_slot;

extern const uint8_t mr_game_code[];
extern const uint8_t mr_game_code_end[];

static uintptr_t *A32;
static uint32_t A32_COUNT;
static thumb_slot *THUMB;
static uint64_t NATIVE_INSNS;
static uint64_t DISPATCHES;
static uint64_t MISSING_BLOCKS;
static uint64_t UNSUPPORTED_BLOCKS;
static uint64_t MID_IT_ENTRIES;
static uint64_t SLOW_MEMORY_FAULTS;
static int READY;

_Static_assert(sizeof(mr_a64_block_fn) == sizeof(uintptr_t),
               "code and function pointers must have equal size");

static mr_a64_block_fn block_from_entry(uintptr_t entry) {
    mr_a64_block_fn function = NULL;
    memcpy(&function, &entry, sizeof function);
    return function;
}

static thumb_slot *runtime_thumb_slot(uint32_t pc);

static uintptr_t *reserve_entry(uint32_t pc, int thumb) {
    if (!thumb) {
        uint32_t index = pc >> 2;
        return index < A32_COUNT ? &A32[index] : NULL;
    }
    thumb_slot *slot = runtime_thumb_slot(pc);
    if (!slot) return NULL;
    slot->pc = pc;
    slot->used = 1;
    return &slot->entry;
}

static thumb_slot *runtime_thumb_slot(uint32_t pc) {
    uint32_t i = ((pc >> 1) * 2654435761u) & (THUMB_SLOTS - 1u);
    for (uint32_t n = 0; n < THUMB_SLOTS; n++, i = (i + 1u) & (THUMB_SLOTS - 1u)) {
        if (!THUMB[i].used || THUMB[i].pc == pc) return &THUMB[i];
    }
    return NULL;
}

static int flag_layout_ok(void) {
    mr_flags f;
    memset(&f, 0, sizeof f);
    f.n = 1;
    if (f.nzcv != 0x80000000u) return 0;
    memset(&f, 0, sizeof f);
    f.z = 1;
    if (f.nzcv != 0x40000000u) return 0;
    memset(&f, 0, sizeof f);
    f.c = 1;
    if (f.nzcv != 0x20000000u) return 0;
    memset(&f, 0, sizeof f);
    f.v = 1;
    if (f.nzcv != 0x10000000u) return 0;
    return 1;
}

static void fail_at(mr_cpu *cpu, const char *message, uint64_t *counter, uint32_t address) {
    (*counter)++;
    cpu->fault = message;
    cpu->fault_addr = address;
    cpu->halted = 1;
}

static void fail(mr_cpu *cpu, const char *message, uint64_t *counter) {
    fail_at(cpu, message, counter, cpu->r[MR_R_PC]);
}

int mr_a64_init(mr_cpu *cpu, uint32_t code_end, uint64_t fingerprint) {
    if (!cpu || cpu->mem_guest_base != 0 || !cpu->mem_host || fingerprint != MR_GAME_BLOCK_HASH)
        return -1;
    if (!flag_layout_ok()) {
        fprintf(stderr, "unexpected flag-bit layout\n");
        return -1;
    }

    NATIVE_INSNS = DISPATCHES = 0;
    MISSING_BLOCKS = UNSUPPORTED_BLOCKS = MID_IT_ENTRIES = 0;
    SLOW_MEMORY_FAULTS = 0;
    A32_COUNT = (code_end + 3u) / 4u;
    A32 = calloc(A32_COUNT, sizeof *A32);
    THUMB = calloc(THUMB_SLOTS, sizeof *THUMB);
    if (!A32 || !THUMB) {
        mr_a64_shutdown();
        return -1;
    }

    size_t code_size = (size_t)(mr_game_code_end - mr_game_code);
    for (size_t n = 0; n < BLOCK_COUNT; n++) {
        const block_desc *block = &GAME_BLOCKS[n];
        if (block->offset >= code_size) {
            mr_a64_shutdown();
            return -1;
        }
        uintptr_t code = (uintptr_t)(const void *)(mr_game_code + block->offset);
        if (code & ENTRY_HOOK_BIT) { // Generated blocks are word-aligned.
            mr_a64_shutdown();
            return -1;
        }
        uintptr_t *entry = reserve_entry(block->pc, block->thumb);
        if (!entry) {
            // Out-of-range A32 addresses are skipped. A full Thumb hash table is fatal.
            if (block->thumb) {
                mr_a64_shutdown();
                return -1;
            }
            continue;
        }
        *entry = code | (*entry & ENTRY_HOOK_BIT);
    }

    for (size_t i = 0; i < MR_GAME_OFFLINE_HOOK_FUNCTION_COUNT; i++) {
        uint32_t address = MR_GAME_OFFLINE_HOOK_FUNCTIONS[i];
        uintptr_t *entry = reserve_entry(address & ~1u, (int)(address & 1u));
        if (!entry) {
            mr_a64_shutdown();
            return -1;
        }
        *entry |= ENTRY_HOOK_BIT;
    }

    READY = 1;
    return 0;
}

void mr_a64_shutdown(void) {
    free(A32);
    free(THUMB);
    A32 = NULL;
    THUMB = NULL;
    A32_COUNT = 0;
    NATIVE_INSNS = DISPATCHES = 0;
    MISSING_BLOCKS = UNSUPPORTED_BLOCKS = MID_IT_ENTRIES = 0;
    SLOW_MEMORY_FAULTS = 0;
    READY = 0;
}

static uint32_t DYNAMIC_HOOK_PC;
static int DYNAMIC_HOOK_ACTIVE;

void mr_a64_set_dynamic_hook(uint32_t pc, int on) {
    if (!READY) return;
    if (DYNAMIC_HOOK_ACTIVE) {
        uintptr_t *entry = reserve_entry(DYNAMIC_HOOK_PC & ~1u, (int)(DYNAMIC_HOOK_PC & 1u));
        if (entry) *entry &= ~ENTRY_HOOK_BIT;
        DYNAMIC_HOOK_ACTIVE = 0;
    }
    if (!on) return;

    uintptr_t *entry = reserve_entry(pc & ~1u, (int)(pc & 1u));
    if (!entry || (*entry & ENTRY_HOOK_BIT)) return;
    *entry |= ENTRY_HOOK_BIT;
    DYNAMIC_HOOK_PC = pc;
    DYNAMIC_HOOK_ACTIVE = 1;
}

uint32_t mr_a64_run(mr_cpu *cpu, uint32_t budget) {
    if (!READY || !cpu || cpu->halted) return 0;
    uint32_t total = 0;
    uint32_t limit = budget ? budget : MR_A64_DEFAULT_BATCH;

    while (total < limit) {
        uint32_t pc = cpu->r[MR_R_PC];
        if (pc >= MR_THUNK_BASE) break;

        uintptr_t entry = 0;
        if (cpu->thumb) {
            if (cpu->itstate) {
                fail(cpu, "no translated entry in the middle of a Thumb IT block", &MID_IT_ENTRIES);
                break;
            }
            const thumb_slot *slot = runtime_thumb_slot(pc);
            if (slot && slot->used && slot->pc == pc) entry = slot->entry;
        } else {
            uint32_t index = pc >> 2;
            if (index < A32_COUNT) entry = A32[index];
        }

        if (entry & ENTRY_HOOK_BIT) {
            if (mr_guest_block_entry(cpu)) {
                total++;
                if (cpu->halted) break;
                continue;
            }
        }

        mr_a64_block_fn fn = block_from_entry(entry & ~ENTRY_HOOK_BIT);
        if (!fn) {
            fail(cpu, "statically translated ARM64 block is missing", &MISSING_BLOCKS);
            break;
        }

        // A generated block accumulates its own and any directly chained
        // predecessor counts in w3. A runtime entry has no predecessor.
        DISPATCHES++;
        uint32_t remaining = limit - total;
        cpu->a64_slow_pc = 0;
        cpu->a64_slow_addr = 0;
        uint32_t ran = fn(cpu, cpu->mem_host, cpu->mem_size, 0u, remaining);
        if (cpu->a64_slow_pc) {
            cpu->r[MR_R_PC] = cpu->a64_slow_pc;
            fail_at(cpu, "invalid address in static memory operation", &SLOW_MEMORY_FAULTS,
                    cpu->a64_slow_addr);
            break;
        }
        if (!ran) {
            fail(cpu, "static ARM64 block reported an unsupported exit", &UNSUPPORTED_BLOCKS);
            break;
        }
        total += ran;
    }

    NATIVE_INSNS += total;
    return total;
}

uint64_t mr_a64_native_insns(void) {
    return NATIVE_INSNS;
}

int mr_a64_report(void) {
    if (!READY) {
        printf("ARM64 code: not linked\n");
        return 1;
    }
    printf("ARM64 code: %zu blocks, %llu instructions executed natively\n", BLOCK_COUNT,
           (unsigned long long)NATIVE_INSNS);
    printf("dispatcher: %llu entries, average %.1f guest instructions per entry\n",
           (unsigned long long)DISPATCHES,
           DISPATCHES ? (double)NATIVE_INSNS / (double)DISPATCHES : 0.0);
    printf("ARM64 block faults: %llu missing blocks, %llu unsupported "
           "block exits, %llu invalid memory addresses, %llu Thumb IT entries\n",
           (unsigned long long)MISSING_BLOCKS, (unsigned long long)UNSUPPORTED_BLOCKS,
           (unsigned long long)SLOW_MEMORY_FAULTS, (unsigned long long)MID_IT_ENTRIES);
    return (MISSING_BLOCKS + UNSUPPORTED_BLOCKS + SLOW_MEMORY_FAULTS + MID_IT_ENTRIES) != 0;
}
