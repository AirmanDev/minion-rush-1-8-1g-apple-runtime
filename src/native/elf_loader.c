
#include "elf_loader.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAGE_DOWN(x, p) ((x) & ~((uint32_t)(p) - 1))
#define PAGE_UP(x, p) (((x) + (uint32_t)(p) - 1) & ~((uint32_t)(p) - 1))

static void err(mr_elf_image *img, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void err(mr_elf_image *img, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(img->error, sizeof(img->error), fmt, ap);
    va_end(ap);
}

static int range_ok(size_t total, uint64_t off, uint64_t size) {
    return off <= total && size <= total - off;
}

static int image_range_ok(const mr_elf_image *img, uint32_t addr, uint32_t size) {
    if (addr < img->guest_base) return 0;
    uint64_t off = (uint64_t)addr - img->guest_base;
    return off <= img->image_span && size <= img->image_span - off;
}

static const char *dyn_str(const mr_elf_image *img, uint32_t off) {
    if (!img->strtab || off >= img->strsz) return "";
    return img->strtab + off;
}

// Loading.

int mr_elf_load(const char *path, mr_elf_image *img, uint32_t extra_bytes) {
    memset(img, 0, sizeof(*img));

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        err(img, "cannot open: %s", path);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        err(img, "fstat failed");
        return -1;
    }
    if (st.st_size < (off_t)sizeof(Elf32_Ehdr)) {
        close(fd);
        err(img, "truncated ELF header");
        return -1;
    }

    size_t fsize = (size_t)st.st_size;
    void *file = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) {
        err(img, "file cannot be mapped");
        return -1;
    }

    int rc = -1;
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file;

    if (memcmp(eh->e_ident,
               "\x7f"
               "ELF",
               4) != 0 ||
        eh->e_ident[4] != 1 || eh->e_ident[5] != 1 || eh->e_ident[6] != 1) {
        err(img, "not a 32-bit little-endian ELF");
        goto done;
    }
    if (eh->e_machine != EM_ARM) {
        err(img, "not an ARM machine type: %u", eh->e_machine);
        goto done;
    }

    if (!eh->e_phnum || eh->e_phentsize != sizeof(Elf32_Phdr) ||
        !range_ok(fsize, eh->e_phoff, (uint64_t)eh->e_phnum * sizeof(Elf32_Phdr))) {
        err(img, "invalid program-header table");
        goto done;
    }

    // Determine the virtual range to load.
    const Elf32_Phdr *ph = (const Elf32_Phdr *)((const char *)file + eh->e_phoff);
    uint32_t vmin = 0xffffffffu, vmax = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t end = (uint64_t)ph[i].p_vaddr + ph[i].p_memsz;
        if (ph[i].p_filesz > ph[i].p_memsz || !range_ok(fsize, ph[i].p_offset, ph[i].p_filesz) ||
            end > UINT32_MAX) {
            err(img, "invalid PT_LOAD segment #%d", i);
            goto done;
        }
        if (ph[i].p_vaddr < vmin) vmin = ph[i].p_vaddr;
        if ((uint32_t)end > vmax) vmax = (uint32_t)end;
    }
    if (vmin > vmax) {
        err(img, "no PT_LOAD segment");
        goto done;
    }

    uint32_t section_exec_start = 0xFFFFFFFFu, section_exec_end = 0;
    int have_exec_sections = 0;
    if (eh->e_shnum && eh->e_shentsize == sizeof(Elf32_Shdr) &&
        range_ok(fsize, eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(Elf32_Shdr))) {
        const Elf32_Shdr *sh = (const Elf32_Shdr *)((const char *)file + eh->e_shoff);
        for (uint32_t i = 0; i < eh->e_shnum; i++) {
            if ((sh[i].sh_flags & (SHF_ALLOC | SHF_EXECINSTR)) != (SHF_ALLOC | SHF_EXECINSTR) ||
                !sh[i].sh_size) {
                continue;
            }
            uint64_t end = (uint64_t)sh[i].sh_addr + sh[i].sh_size;
            if (!range_ok(fsize, sh[i].sh_offset, sh[i].sh_size) || sh[i].sh_addr < vmin ||
                end > vmax || end > UINT32_MAX) {
                err(img, "invalid executable section #%u", i);
                goto done;
            }
            if (sh[i].sh_addr < section_exec_start) section_exec_start = sh[i].sh_addr;
            if ((uint32_t)end > section_exec_end) section_exec_end = (uint32_t)end;
            have_exec_sections = 1;
        }
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || (uint64_t)page_size > UINT32_MAX ||
        ((uint32_t)page_size & ((uint32_t)page_size - 1u)) != 0) {
        err(img, "invalid system page size: %ld", page_size);
        goto done;
    }
    uint32_t pagesz = (uint32_t)page_size;
    if (vmax > UINT32_MAX - (pagesz - 1u) || extra_bytes > UINT32_MAX - (pagesz - 1u)) {
        err(img, "guest address space is too large");
        goto done;
    }
    img->guest_base = PAGE_DOWN(vmin, pagesz);
    img->image_span = PAGE_UP(vmax, pagesz) - img->guest_base;
    size_t extra_span = PAGE_UP(extra_bytes, pagesz);
    if (img->image_span > UINT32_MAX - img->guest_base ||
        extra_span > UINT32_MAX - img->guest_base - img->image_span) {
        err(img, "guest address space does not fit in 32 bits");
        goto done;
    }
    img->span = img->image_span + extra_span;

    // Host mapping location does not affect guest addresses.
    void *base = mmap(NULL, img->span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        err(img, "cannot allocate guest region (%zu bytes)", img->span);
        goto done;
    }
    img->host_base = base;

    img->ro_start = 0xFFFFFFFFu;
    img->ro_end = 0;
    img->exec_start = 0xFFFFFFFFu;
    img->exec_end = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_ARM_EXIDX) {
            if (!image_range_ok(img, ph[i].p_vaddr, ph[i].p_memsz) || ph[i].p_memsz % 8u != 0) {
                err(img, "invalid PT_ARM_EXIDX");
                goto done;
            }
            img->exidx = ph[i].p_vaddr;
            img->exidx_count = ph[i].p_memsz / 8; // Eight-byte entries.
            continue;
        }
        if (ph[i].p_type != PT_LOAD) continue;
        memcpy(mr_g2h(img, ph[i].p_vaddr), (const char *)file + ph[i].p_offset, ph[i].p_filesz);
        if (!(ph[i].p_flags & PF_W)) {
            if (ph[i].p_vaddr < img->ro_start) img->ro_start = ph[i].p_vaddr;
            uint32_t e = ph[i].p_vaddr + ph[i].p_memsz;
            if (e > img->ro_end) img->ro_end = e;
        }
        if (!have_exec_sections && (ph[i].p_flags & PF_X)) {
            if (ph[i].p_vaddr < img->exec_start) img->exec_start = ph[i].p_vaddr;
            uint32_t e = ph[i].p_vaddr + ph[i].p_memsz;
            if (e > img->exec_end) img->exec_end = e;
        }
    }
    if (img->ro_start > img->ro_end) {
        img->ro_start = img->ro_end = 0;
    }
    if (have_exec_sections) {
        img->exec_start = section_exec_start;
        img->exec_end = section_exec_end;
    } else if (img->exec_start > img->exec_end) {
        img->exec_start = img->exec_end = 0;
    }

    // Parse .dynamic.
    const Elf32_Dyn *dyn = NULL;
    uint32_t dyn_count = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            if (!image_range_ok(img, ph[i].p_vaddr, ph[i].p_memsz) ||
                ph[i].p_memsz < sizeof(Elf32_Dyn)) {
                err(img, "invalid PT_DYNAMIC");
                goto done;
            }
            dyn = (const Elf32_Dyn *)mr_g2h(img, ph[i].p_vaddr);
            dyn_count = ph[i].p_memsz / sizeof(Elf32_Dyn);
            break;
        }
    }
    if (!dyn) {
        err(img, "no PT_DYNAMIC segment");
        goto done;
    }

    uint32_t rel_off = 0, rel_sz = 0, jmprel_off = 0, pltrel_sz = 0;
    uint32_t symtab_off = 0, strtab_off = 0;

    int dyn_terminated = 0;
    for (uint32_t i = 0; i < dyn_count; i++) {
        const Elf32_Dyn *d = &dyn[i];
        if (d->d_tag == DT_NULL) {
            dyn_terminated = 1;
            break;
        }
        switch (d->d_tag) {
        case DT_STRTAB:
            strtab_off = d->d_un.d_ptr;
            break;
        case DT_SYMTAB:
            symtab_off = d->d_un.d_ptr;
            break;
        case DT_STRSZ:
            img->strsz = d->d_un.d_val;
            break;
        case DT_REL:
            rel_off = d->d_un.d_ptr;
            break;
        case DT_RELSZ:
            rel_sz = d->d_un.d_val;
            break;
        case DT_JMPREL:
            jmprel_off = d->d_un.d_ptr;
            break;
        case DT_PLTRELSZ:
            pltrel_sz = d->d_un.d_val;
            break;
        case DT_INIT_ARRAY:
            img->init_array = d->d_un.d_ptr;
            break;
        case DT_INIT_ARRAYSZ:
            img->init_array_sz = d->d_un.d_val;
            break;
        default:
            break;
        }
    }
    if (!dyn_terminated) {
        err(img, "unterminated PT_DYNAMIC segment");
        goto done;
    }

    if (!strtab_off || !img->strsz || !image_range_ok(img, strtab_off, img->strsz) ||
        ((const char *)mr_g2h(img, strtab_off))[img->strsz - 1] != '\0') {
        err(img, "invalid dynamic string table");
        goto done;
    }
    img->strtab = (const char *)mr_g2h(img, strtab_off);

    if (!symtab_off || strtab_off <= symtab_off ||
        (strtab_off - symtab_off) % sizeof(Elf32_Sym) != 0 ||
        !image_range_ok(img, symtab_off, strtab_off - symtab_off)) {
        err(img, "invalid dynamic symbol table");
        goto done;
    }
    img->symtab = (const Elf32_Sym *)mr_g2h(img, symtab_off);
    img->symcount = (strtab_off - symtab_off) / sizeof(Elf32_Sym);

    if ((rel_sz % sizeof(Elf32_Rel)) || (pltrel_sz % sizeof(Elf32_Rel)) ||
        (rel_sz && !image_range_ok(img, rel_off, rel_sz)) ||
        (pltrel_sz && !image_range_ok(img, jmprel_off, pltrel_sz)) ||
        (img->init_array_sz && (!image_range_ok(img, img->init_array, img->init_array_sz) ||
                                img->init_array_sz % 4u != 0))) {
        err(img, "invalid dynamic table");
        goto done;
    }
    img->rel = rel_sz ? (const Elf32_Rel *)mr_g2h(img, rel_off) : NULL;
    img->rel_count = rel_sz / sizeof(Elf32_Rel);
    img->jmprel = pltrel_sz ? (const Elf32_Rel *)mr_g2h(img, jmprel_off) : NULL;
    img->jmprel_count = pltrel_sz / sizeof(Elf32_Rel);

    rc = 0;

done:
    munmap(file, fsize);
    if (rc != 0 && img->host_base) {
        munmap(img->host_base, img->span);
        img->host_base = NULL;
    }
    return rc;
}

// Relocations.

int mr_elf_resolve_imports(mr_elf_image *img, mr_resolver resolve, void *user) {
    if (!img || !img->symtab || !resolve) return -1;
    free(img->resolved_imports);
    img->resolved_imports = calloc(img->symcount, sizeof *img->resolved_imports);
    if (!img->resolved_imports) return -1;

    for (uint32_t i = 1; i < img->symcount; i++) {
        const Elf32_Sym *symbol = &img->symtab[i];
        if (symbol->st_shndx != 0) continue;
        const char *name = dyn_str(img, symbol->st_name);
        if (!*name) continue;
        uint32_t address = 0;
        if (!resolve(name, &address, user) || !address) {
            fprintf(stderr, "unresolved ELF import: %s\n", name);
            free(img->resolved_imports);
            img->resolved_imports = NULL;
            return -1;
        }
        img->resolved_imports[i] = address;
    }
    return 0;
}

static void apply_rels(mr_elf_image *img, const Elf32_Rel *rels, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type = ELF32_R_TYPE(rels[i].r_info);
        uint32_t sym = ELF32_R_SYM(rels[i].r_info);

        if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT &&
            type != R_ARM_REL32)
            continue;
        if (!sym || !img->symtab || sym >= img->symcount) continue;
        if (!image_range_ok(img, rels[i].r_offset, 4)) continue;

        const Elf32_Sym *symbol = &img->symtab[sym];
        uint32_t value = symbol->st_shndx != 0
                             ? symbol->st_value
                             : (img->resolved_imports ? img->resolved_imports[sym] : 0);
        if (!value) continue;

        uint32_t *slot = (uint32_t *)mr_g2h(img, rels[i].r_offset);
        *slot = (type == R_ARM_REL32) ? value - rels[i].r_offset : value;
    }
}

void mr_elf_relocate(mr_elf_image *img) {
    apply_rels(img, img->rel, img->rel_count);
    apply_rels(img, img->jmprel, img->jmprel_count);
}

// Queries.

#ifdef MR_ELF_BUILD_TOOLS
uint32_t mr_elf_lookup(const mr_elf_image *img, const char *name) {
    if (!img->symtab) return 0;
    for (uint32_t i = 0; i < img->symcount; i++) {
        const Elf32_Sym *s = &img->symtab[i];
        if (s->st_shndx == 0) continue;
        if (strcmp(dyn_str(img, s->st_name), name) == 0) return s->st_value;
    }
    return 0;
}

void mr_elf_each_named_function(const mr_elf_image *img, mr_elf_named_func_cb callback,
                                void *user) {
    if (!img || !img->symtab || !callback) return;
    for (uint32_t i = 0; i < img->symcount; i++) {
        const Elf32_Sym *symbol = &img->symtab[i];
        if (symbol->st_shndx == 0 || (symbol->st_info & 0xf) != 2) continue;
        const char *name = dyn_str(img, symbol->st_name);
        if (*name) callback(name, symbol->st_value, user);
    }
}

#endif

const char *mr_elf_nearest(const mr_elf_image *img, uint32_t addr, uint32_t *off) {
    if (!img->symtab) return NULL;
    const char *best = NULL;
    uint32_t best_val = 0;
    for (uint32_t i = 0; i < img->symcount; i++) {
        const Elf32_Sym *s = &img->symtab[i];
        if (s->st_shndx == 0 || (s->st_info & 0xf) != 2) continue; // STT_FUNC
        uint32_t v = s->st_value & ~1u;
        if (v > addr || v < best_val) continue;
        const char *n = dyn_str(img, s->st_name);
        if (!*n) continue;
        best = n;
        best_val = v;
    }
    if (best && off) *off = addr - best_val;
    return best;
}

uint64_t mr_elf_fingerprint(const mr_elf_image *img) {
    if (img->ro_end <= img->ro_start) return 0;
    const uint8_t *bytes = mr_g2h(img, img->ro_start);
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint32_t i = 0, n = img->ro_end - img->ro_start; i < n; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

#ifdef MR_ELF_BUILD_TOOLS
void mr_elf_each_function(const mr_elf_image *img, mr_elf_func_cb cb, void *user) {
    if (!img->symtab) return;
    for (uint32_t i = 0; i < img->symcount; i++) {
        const Elf32_Sym *s = &img->symtab[i];
        if (s->st_shndx == 0 || (s->st_info & 0xf) != 2) continue; // STT_FUNC
        if (s->st_value) cb(s->st_value, s->st_size, user);
    }
}

void mr_elf_each_exidx_start(const mr_elf_image *img, mr_elf_func_cb cb, void *user) {
    if (!img->exidx || !img->exidx_count) return;
    const uint32_t *tab = (const uint32_t *)mr_g2h(img, img->exidx);
    for (uint32_t i = 0; i < img->exidx_count; i++) {
        uint32_t w = tab[i * 2u];
        if (w & 0x80000000u) continue;
        // prel31 is a signed 31-bit offset relative to its own entry.
        int32_t off = (int32_t)(w << 1) >> 1;
        cb(img->exidx + i * 8u + (uint32_t)off, 0, user);
    }
}

static int executable_pointer(const mr_elf_image *img, uint32_t value) {
    uint32_t addr = value & ~1u;
    return addr >= img->exec_start && addr < img->exec_end;
}

static void each_relocated_code_pointer(const mr_elf_image *img, const Elf32_Rel *rels,
                                        uint32_t count, mr_elf_func_cb cb, void *user) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type = ELF32_R_TYPE(rels[i].r_info);
        uint32_t sym = ELF32_R_SYM(rels[i].r_info);
        uint32_t value = 0;

        if (!image_range_ok(img, rels[i].r_offset, 4)) continue;
        const uint32_t addend = *(const uint32_t *)mr_g2h(img, rels[i].r_offset);

        if (type == R_ARM_RELATIVE) {
            value = addend;
        } else if ((type == R_ARM_ABS32 || type == R_ARM_GLOB_DAT || type == R_ARM_JUMP_SLOT) &&
                   sym && img->symtab && sym < img->symcount) {
            const Elf32_Sym *symbol = &img->symtab[sym];
            if (symbol->st_shndx != 0)
                value = symbol->st_value + (type == R_ARM_ABS32 ? addend : 0u);
        }

        if (executable_pointer(img, value)) cb(value, 0, user);
    }
}

void mr_elf_each_code_pointer(const mr_elf_image *img, mr_elf_func_cb cb, void *user) {
    each_relocated_code_pointer(img, img->rel, img->rel_count, cb, user);
    each_relocated_code_pointer(img, img->jmprel, img->jmprel_count, cb, user);

    if (!img->init_array || !img->init_array_sz) return;
    const uint32_t *entries = (const uint32_t *)mr_g2h(img, img->init_array);
    for (uint32_t i = 0; i < img->init_array_sz / 4u; i++)
        if (executable_pointer(img, entries[i])) cb(entries[i], 0, user);
}

#endif

void mr_elf_free(mr_elf_image *img) {
    free(img->resolved_imports);
    if (img->host_base) munmap(img->host_base, img->span);
    memset(img, 0, sizeof(*img));
}
