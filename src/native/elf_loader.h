#ifndef MR_ELF_LOADER_H
#define MR_ELF_LOADER_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "elf32.h"

#define MR_THUNK_BASE 0xF0000000u
#define MR_THUNK_SHIFT 2
#define MR_THUNK_ADDR(i) (MR_THUNK_BASE + ((uint32_t)(i) << MR_THUNK_SHIFT))
#define MR_THUNK_INDEX(a) (((a) - MR_THUNK_BASE) >> MR_THUNK_SHIFT)

typedef struct {
    void *host_base; // Host mapping of the guest image.
    size_t span;
    size_t image_span; // Portion occupied by the loaded image.
    uint32_t guest_base;

    const char *strtab;
    uint32_t strsz;
    const Elf32_Sym *symtab;
    uint32_t symcount;
    uint32_t *resolved_imports; // build once, indexed by dynsym

    const Elf32_Rel *rel;
    uint32_t rel_count;
    const Elf32_Rel *jmprel;
    uint32_t jmprel_count;

    uint32_t ro_start, ro_end;
    uint32_t exec_start, exec_end;

    // PT_ARM_EXIDX exception-unwind table with 8-byte entries.
    uint32_t exidx, exidx_count;

    // C++ static-constructor table.
    uint32_t init_array;
    uint32_t init_array_sz;

    char error[256];
} mr_elf_image;

// Convert a guest address to a host pointer.
static inline void *mr_g2h(const mr_elf_image *img, uint32_t addr) {
    return (char *)img->host_base + (addr - img->guest_base);
}

// Returns nonzero when the symbol resolves; out receives a guest address.
typedef int (*mr_resolver)(const char *name, uint32_t *out, void *user);

int mr_elf_load(const char *path, mr_elf_image *img, uint32_t extra_bytes);
int mr_elf_resolve_imports(mr_elf_image *img, mr_resolver resolve, void *user);
void mr_elf_relocate(mr_elf_image *img);
void mr_elf_free(mr_elf_image *img);

#ifdef MR_ELF_BUILD_TOOLS
uint32_t mr_elf_lookup(const mr_elf_image *img, const char *name);

typedef void (*mr_elf_named_func_cb)(const char *name, uint32_t address, void *user);
void mr_elf_each_named_function(const mr_elf_image *img, mr_elf_named_func_cb callback, void *user);
#endif

uint64_t mr_elf_fingerprint(const mr_elf_image *img);

#ifdef MR_ELF_BUILD_TOOLS
typedef void (*mr_elf_func_cb)(uint32_t addr, uint32_t size, void *user);
void mr_elf_each_function(const mr_elf_image *img, mr_elf_func_cb cb, void *user);

void mr_elf_each_exidx_start(const mr_elf_image *img, mr_elf_func_cb cb, void *user);

void mr_elf_each_code_pointer(const mr_elf_image *img, mr_elf_func_cb cb, void *user);

#endif

const char *mr_elf_nearest(const mr_elf_image *img, uint32_t addr, uint32_t *off);

#endif
