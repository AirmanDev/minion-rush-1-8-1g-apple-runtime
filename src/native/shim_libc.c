
#include "shim_libc.h"
#include "guest_runtime.h"
#include "host_time.h"
#include "localization.h"

#include "guest_threads.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

#define MR_FILE_SZ 152u
#define MAX_FILES 64
static uint32_t STDIO_BASE, FILEBUF_BASE, ERRNO_SLOT;

#define HDR 8u
#define PREV_FREE 1u
#define MIN_BLOCK 16u // Fits the header, two free-list links, and footer.
#define HEAP_BINS 32u

static uint32_t heap_base, heap_end, heap_brk;
static uint32_t free_bins[HEAP_BINS];

void mr_heap_init(uint32_t base, uint32_t size) {
    heap_base = (base + 7u) & ~7u;
    heap_end = base + size;
    heap_brk = heap_base;
    memset(free_bins, 0, sizeof free_bins);
}

// Eight-byte class followed by power-of-two size classes.
static unsigned heap_bin(uint32_t size) {
    uint32_t units = (size + 7u) >> 3;
    unsigned bin = 0;
    while (units > 1 && bin + 1 < HEAP_BINS) {
        units = (units + 1) >> 1;
        bin++;
    }
    return bin;
}

static uint32_t blk_size(mr_cpu *c, uint32_t b) {
    return mr_ld32(c, b) & ~7u;
}
static int blk_used(mr_cpu *c, uint32_t b) {
    return mr_ld32(c, b + 4) == 1u;
}
static uint32_t blk_flags(mr_cpu *c, uint32_t b) {
    return mr_ld32(c, b) & 7u;
}

static void blk_mark_next(mr_cpu *c, uint32_t b, uint32_t size, int prev_free) {
    uint32_t next = b + size;
    if (next >= heap_brk) return;
    uint32_t head = mr_ld32(c, next);
    mr_st32(c, next, prev_free ? (head | PREV_FREE) : (head & ~PREV_FREE));
}

static void free_unlink(mr_cpu *c, uint32_t b) {
    uint32_t prev = mr_ld32(c, b + 4), next = mr_ld32(c, b + 8);
    if (prev)
        mr_st32(c, prev + 8, next);
    else
        free_bins[heap_bin(blk_size(c, b))] = next;
    if (next) mr_st32(c, next + 4, prev);
}

static void free_push(mr_cpu *c, uint32_t b, uint32_t size, uint32_t flags) {
    mr_st32(c, b, size | flags);
    unsigned bin = heap_bin(size);
    uint32_t head = free_bins[bin];
    mr_st32(c, b + 4, 0);
    mr_st32(c, b + 8, head);
    if (head) mr_st32(c, head + 4, b);
    free_bins[bin] = b;
    mr_st32(c, b + size - 4, size);
    blk_mark_next(c, b, size, 1);
}

static uint32_t take_block(mr_cpu *c, uint32_t b, uint32_t size, uint32_t need) {
    uint32_t flags = blk_flags(c, b) & PREV_FREE;
    uint32_t remain = size - need;
    if (remain >= MIN_BLOCK) {
        size = need;
        free_push(c, b + need, remain, 0);
    }
    mr_st32(c, b, size | flags);
    mr_st32(c, b + 4, 1);
    blk_mark_next(c, b, size, 0);
    return b + HDR;
}

static uint32_t heap_alloc(mr_cpu *c, uint32_t size) {
    if (!size) size = 1;
    if (size > UINT32_MAX - HDR - 7u) return 0;
    uint32_t need = (size + HDR + 7u) & ~7u;
    if (need < MIN_BLOCK) need = MIN_BLOCK;

    for (unsigned bin = heap_bin(need); bin < HEAP_BINS; bin++) {
        for (uint32_t p = free_bins[bin]; p; p = mr_ld32(c, p + 8)) {
            uint32_t bsz = blk_size(c, p);
            if (bsz < need) continue;
            free_unlink(c, p);
            return take_block(c, p, bsz, need);
        }
    }

    if (need > heap_end - heap_brk) return 0; // NULL.
    uint32_t p = heap_brk;
    heap_brk += need;
    mr_st32(c, p, need);
    mr_st32(c, p + 4, 1);
    return p + HDR;
}

static uint32_t heap_block_size(mr_cpu *c, uint32_t ptr) {
    if (!ptr || ptr < HDR) return 0;
    uint32_t block = ptr - HDR;
    if (block < heap_base || block > heap_brk || heap_brk - block < HDR ||
        ((block - heap_base) & 7u)) {
        c->fault = "invalid heap pointer";
        c->fault_addr = ptr;
        c->halted = 1;
        return 0;
    }
    uint32_t size = blk_size(c, block);
    if (size < MIN_BLOCK || size > heap_brk - block || !blk_used(c, block)) {
        c->fault = "corrupt or already freed heap block";
        c->fault_addr = ptr;
        c->halted = 1;
        return 0;
    }
    return size;
}

static void heap_free(mr_cpu *c, uint32_t ptr) {
    if (!ptr) return;
    uint32_t size = heap_block_size(c, ptr);
    if (!size) return;

    uint32_t b = ptr - HDR;
    uint32_t flags = blk_flags(c, b) & PREV_FREE;

    uint32_t next = b + size;
    if (next < heap_brk && !blk_used(c, next)) {
        free_unlink(c, next);
        size += blk_size(c, next);
    }
    if (flags & PREV_FREE) {
        uint32_t prev_size = mr_ld32(c, b - 4);
        if (prev_size >= MIN_BLOCK && !(prev_size & 7u) && b - prev_size >= heap_base) {
            uint32_t prev = b - prev_size;
            free_unlink(c, prev);
            size += prev_size;
            b = prev;
            flags = blk_flags(c, b) & PREV_FREE;
        }
    }
    // Return a free block adjacent to the heap top directly.
    if (b + size == heap_brk) {
        heap_brk = b;
        return;
    }
    free_push(c, b, size, flags);
}

uint32_t mr_guest_alloc(mr_cpu *c, uint32_t size) {
    return heap_alloc(c, size);
}

void mr_guest_free(mr_cpu *c, uint32_t ptr) {
    heap_free(c, ptr);
}

static uint32_t heap_size_of(mr_cpu *c, uint32_t ptr) {
    if (!ptr) return 0;
    uint32_t size = heap_block_size(c, ptr);
    return size ? size - HDR : 0;
}

// Helpers.

#define A0 (c->r[0])
#define A1 (c->r[1])
#define A2 (c->r[2])
#define A3 (c->r[3])
#define RET(x) (c->r[0] = (uint32_t)(x))

static uint8_t ZERO_PAGE[4096];

static void *GN(mr_cpu *c, uint32_t addr, uint32_t len) {
    if (!mr_mem_ok(c, addr, len)) {
        if (!c->fault) {
            c->fault = "invalid guest range in shim";
            c->fault_addr = addr;
        }
        c->halted = 1;
        memset(ZERO_PAGE, 0, sizeof(ZERO_PAGE));
        return ZERO_PAGE;
    }
    return mr_mem(c, addr);
}

static void *GW(mr_cpu *c, uint32_t addr, uint32_t len) {
    if (!mr_mem_ok(c, addr, len) || mr_ro_hit(c, addr, len)) {
        if (!c->fault) {
            c->fault = addr < 0x1000 ? "shim write near NULL" : "shim write to read-only region";
            c->fault_addr = addr;
        }
        c->halted = 1;
        memset(ZERO_PAGE, 0, sizeof(ZERO_PAGE));
        return ZERO_PAGE;
    }
    return mr_mem(c, addr);
}

static char *GSN(mr_cpu *c, uint32_t addr, size_t limit) {
    return (char *)mr_guest_cstrn(c, addr, limit);
}

static char *GS(mr_cpu *c, uint32_t addr) {
    return (char *)mr_guest_cstr(c, addr);
}

static void s_malloc(mr_cpu *c) {
    RET(heap_alloc(c, A0));
}
static void s_free(mr_cpu *c) {
    heap_free(c, A0);
    RET(0);
}

static void s_calloc(mr_cpu *c) {
    uint64_t total = (uint64_t)A0 * (uint64_t)A1;
    if (total > 0xFFFFFFFFull) {
        RET(0);
        return;
    }
    uint32_t n = (uint32_t)total;
    uint32_t p = heap_alloc(c, n);
    if (p) memset(GN(c, p, n), 0, n);
    RET(p);
}

static void s_realloc(mr_cpu *c) {
    uint32_t old = A0, want = A1;
    if (!old) {
        RET(heap_alloc(c, want));
        return;
    }
    if (!want) {
        heap_free(c, old);
        RET(0);
        return;
    }
    uint32_t have = heap_size_of(c, old);
    if (c->halted) {
        RET(0);
        return;
    }
    if (have >= want) {
        RET(old);
        return;
    }
    uint32_t p = heap_alloc(c, want);
    if (p) {
        void *dst = GN(c, p, have), *src = GN(c, old, have);
        if (!c->halted) memcpy(dst, src, have);
        heap_free(c, old);
    }
    RET(p);
}

// C++ operator new and delete.

#define IF_EMPTY(n)                                                                                \
    do {                                                                                           \
        if (!(n)) {                                                                                \
            RET(A0);                                                                               \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void s_memcpy(mr_cpu *c) {
    IF_EMPTY(A2);
    uint32_t n = A2;
    void *d = GW(c, A0, n);
    void *v = GN(c, A1, n);
    if (!c->halted) memcpy(d, v, n);
    RET(A0);
}
static void s_memmove(mr_cpu *c) {
    IF_EMPTY(A2);
    uint32_t n = A2;
    void *d = GW(c, A0, n);
    void *v = GN(c, A1, n);
    if (!c->halted) memmove(d, v, n);
    RET(A0);
}
static void s_memset(mr_cpu *c) {
    IF_EMPTY(A2);
    uint32_t n = A2;
    void *d = GW(c, A0, n);
    if (!c->halted) memset(d, (int)A1, n);
    RET(A0);
}
static void s_memcmp(mr_cpu *c) {
    if (!A2) {
        RET(0);
        return;
    }
    uint32_t n = A2;
    void *a = GN(c, A0, n), *b = GN(c, A1, n);
    RET(c->halted ? 0 : (uint32_t)memcmp(a, b, n));
}

static void s_strlen(mr_cpu *c) {
    RET((uint32_t)strlen(GS(c, A0)));
}
static void s_strcmp(mr_cpu *c) {
    RET((uint32_t)strcmp(GS(c, A0), GS(c, A1)));
}
static void s_strncmp(mr_cpu *c) {
    if (!A2) {
        RET(0);
        return;
    }
    RET((uint32_t)strncmp(GSN(c, A0, A2), GSN(c, A1, A2), A2));
}
static void s_strcpy(mr_cpu *c) {
    const char *v = GS(c, A1);
    size_t n = strlen(v) + 1;
    char *d = GW(c, A0, (uint32_t)n);
    if (!c->halted) memcpy(d, v, n);
    RET(A0);
}
static void s_strncpy(mr_cpu *c) {
    uint32_t n = A2;
    if (!n) {
        RET(A0);
        return;
    }
    char *d = GW(c, A0, n);
    const char *v = GSN(c, A1, n);
    if (!c->halted) strncpy(d, v, n);
    RET(A0);
}
static void s_strcat(mr_cpu *c) {
    char *old = GS(c, A0);
    const char *v = GS(c, A1);
    size_t have = strlen(old), add = strlen(v);
    if (have > UINT32_MAX - add - 1u) {
        c->fault = "guest string size overflow";
        c->fault_addr = A0;
        c->halted = 1;
        RET(A0);
        return;
    }
    char *d = GW(c, A0, (uint32_t)(have + add + 1));
    if (!c->halted) memmove(d + have, v, add + 1);
    RET(A0);
}

static void s_strchr(mr_cpu *c) {
    char *s = GS(c, A0);
    char *p = strchr(s, (int)A1);
    RET(p ? A0 + (uint32_t)(p - s) : 0);
}
static void s_strrchr(mr_cpu *c) {
    char *s = GS(c, A0);
    char *p = strrchr(s, (int)A1);
    RET(p ? A0 + (uint32_t)(p - s) : 0);
}
static void s_strstr(mr_cpu *c) {
    char *h = GS(c, A0);
    char *p = strstr(h, GS(c, A1));
    RET(p ? A0 + (uint32_t)(p - h) : 0);
}
static void s_strdup(mr_cpu *c) {
    const char *s = GS(c, A0);
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = heap_alloc(c, n);
    if (p) {
        void *dst = GW(c, p, n);
        if (!c->halted) memcpy(dst, s, n);
    }
    RET(p);
}

static void s_strcasecmp(mr_cpu *c) {
    RET((uint32_t)strcasecmp(GS(c, A0), GS(c, A1)));
}
static void s_strncasecmp(mr_cpu *c) {
    if (!A2) {
        RET(0);
        return;
    }
    RET((uint32_t)strncasecmp(GSN(c, A0, A2), GSN(c, A1, A2), A2));
}

// The C locale uses bytewise collation, so strcoll is equivalent to strcmp.
static void s_strcoll(mr_cpu *c) {
    RET((uint32_t)strcmp(GS(c, A0), GS(c, A1)));
}
static void s_strcspn(mr_cpu *c) {
    RET((uint32_t)strcspn(GS(c, A0), GS(c, A1)));
}

static void s_strncat(mr_cpu *c) {
    const char *src = GSN(c, A1, A2);
    size_t have = strlen(GS(c, A0)), add = strnlen(src, A2);
    if (have > UINT32_MAX - add - 1u) {
        c->fault = "guest string size overflow";
        c->fault_addr = A0;
        c->halted = 1;
        RET(A0);
        return;
    }
    char *dst = GW(c, A0, (uint32_t)(have + add + 1));
    if (!c->halted) {
        memcpy(dst + have, src, add);
        dst[have + add] = '\0';
    }
    RET(A0);
}

static void s_strlcat(mr_cpu *c) {
    const char *src = GS(c, A1);
    uint32_t cap = A2;
    const char *old = cap ? GSN(c, A0, cap) : (const char *)ZERO_PAGE;
    size_t have = strnlen(old, cap), want = strlen(src);
    if (have < cap) {
        size_t room = cap - have - 1, add = want < room ? want : room;
        char *dst = GW(c, A0, (uint32_t)(have + add + 1));
        if (!c->halted) {
            memcpy(dst + have, src, add);
            dst[have + add] = '\0';
        }
    }
    RET((uint32_t)(have + want));
}

// strtok_r state lives in the caller-provided guest slot.
static void s_strtok_r(mr_cpu *c) {
    uint32_t s = A0 ? A0 : (A2 && mr_mem_ok(c, A2, 4) ? mr_ld32(c, A2) : 0);
    if (!s) {
        RET(0);
        return;
    }

    const char *delim = GS(c, A1);
    const char *p = GS(c, s);
    if (c->halted) return;

    while (*p && strchr(delim, *p)) {
        p++;
        s++;
    }
    if (!*p) {
        if (A2) mr_st32(c, A2, 0);
        RET(0);
        return;
    }

    uint32_t start = s;
    while (*p && !strchr(delim, *p)) {
        p++;
        s++;
    }
    if (*p) {
        char *w = GW(c, s, 1);
        if (c->halted) return;
        *w = '\0';
        s++;
    }
    if (A2) mr_st32(c, A2, *p ? s : 0);
    RET(start);
}

static const unsigned char *find_bytes(const unsigned char *haystack, size_t haystack_size,
                                       const unsigned char *needle, size_t needle_size) {
    if (!needle_size) return haystack;
    if (needle_size > haystack_size) return NULL;

    size_t remaining = haystack_size - needle_size + 1;
    const unsigned char *cursor = haystack;
    while (remaining) {
        const unsigned char *candidate = memchr(cursor, needle[0], remaining);
        if (!candidate) return NULL;
        if (needle_size == 1 || memcmp(candidate, needle, needle_size) == 0) return candidate;
        size_t consumed = (size_t)(candidate - cursor) + 1;
        cursor += consumed;
        remaining -= consumed;
    }
    return NULL;
}

static void s_memmem(mr_cpu *c) {
    uint32_t hay = A0, hay_n = A1, need_n = A3;
    if (!need_n) {
        RET(hay);
        return;
    }
    if (need_n > hay_n) {
        RET(0);
        return;
    }
    const unsigned char *h = GN(c, hay, hay_n);
    const unsigned char *n = GN(c, A2, need_n);
    if (c->halted) {
        RET(0);
        return;
    }
    const unsigned char *p = find_bytes(h, hay_n, n, need_n);
    RET(p ? hay + (uint32_t)(p - h) : 0);
}

// POSIX strerror_r returns zero on success and an error code otherwise.
static void s_strerror_r(mr_cpu *c) {
    uint32_t cap = A2;
    if (!A1 || !cap) {
        RET(EINVAL);
        return;
    }
    char *dst = GW(c, A1, cap);
    if (c->halted) {
        RET(EFAULT);
        return;
    }
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    char *message = strerror_r((int)A0, dst, cap);
    if (message != dst) {
        snprintf(dst, cap, "%s", message);
    }
    RET(0);
#else
    RET((uint32_t)strerror_r((int)A0, dst, cap));
#endif
}

static uint32_t STRTOK_SAVE;

static void s_strtok(mr_cpu *c) {
    uint32_t s = A0 ? A0 : STRTOK_SAVE;
    if (!s) {
        RET(0);
        return;
    }

    const char *delim = GS(c, A1);
    const char *p = GS(c, s);
    if (c->halted) return;

    while (*p && strchr(delim, *p)) {
        p++;
        s++;
    } // Skip leading delimiters.
    if (!*p) {
        STRTOK_SAVE = 0;
        RET(0);
        return;
    }

    uint32_t start = s;
    while (*p && !strchr(delim, *p)) {
        p++;
        s++;
    } // Find the token end.

    if (*p) {
        char *w = GW(c, s, 1);
        if (c->halted) return;
        *w = '\0';
        STRTOK_SAVE = s + 1;
    } else {
        STRTOK_SAVE = 0;
    }
    RET(start);
}

static void s_atexit(mr_cpu *c) {
    RET(0);
}

static void s_abort(mr_cpu *c) {
    c->fault = "guest called abort()";
    c->halted = 1;
}

static int guest_log_format(mr_cpu *c, char *out, size_t cap, const char *fmt, int first_arg);

static void s_android_log(mr_cpu *c) {
    const char *tag = A1 ? GS(c, A1) : "?";
    const char *fmt = A2 ? GS(c, A2) : "";

    static int show_all = -1;
    if (show_all < 0) show_all = getenv("MR_GUEST_LOG") ? 1 : 0;
    if (!show_all && strcmp(tag, "HEI") == 0) {
        RET(0);
        return;
    }

    char buf[4096];
    guest_log_format(c, buf, sizeof(buf), fmt, 3);
    printf("[guest %s] %s\n", tag, buf);
    RET(0);
}

static void s_puts(mr_cpu *c) {
    printf("[guest] %s\n", GS(c, A0));
    RET(0);
}

typedef struct {
    const char *name;
    uint32_t addr;
} data_sym;
static data_sym DATA_SYMS[16];
static int DATA_COUNT;

static void data_add(const char *name, uint32_t addr) {
    if (DATA_COUNT < 16) DATA_SYMS[DATA_COUNT++] = (data_sym){name, addr};
}

// BSD/bionic ctype flags.
#define _CT_U 0x01 // Uppercase.
#define _CT_L 0x02 // Lowercase.
#define _CT_D 0x04 // Digit.
#define _CT_S 0x08 // Whitespace.
#define _CT_P 0x10 // Punctuation.
#define _CT_C 0x20 // Control character.
#define _CT_X 0x40 // Hexadecimal digit.
#define _CT_B 0x80 // Space.

uint32_t mr_shim_data_init(mr_cpu *c, uint32_t base) {
    uint32_t p = (base + 7u) & ~7u;
    DATA_COUNT = 0;

    uint32_t ctype_tab = p;
    for (int i = 0; i < 257; i++) {
        int ch = i - 1;
        uint8_t f = 0;
        if (ch >= 0 && ch < 256) {
            if (isupper(ch)) f |= _CT_U;
            if (islower(ch)) f |= _CT_L;
            if (isdigit(ch)) f |= _CT_D;
            if (isspace(ch)) f |= _CT_S;
            if (ispunct(ch)) f |= _CT_P;
            if (iscntrl(ch)) f |= _CT_C;
            if (isxdigit(ch)) f |= _CT_X;
            if (ch == ' ') f |= _CT_B;
        }
        mr_st8(c, p + (uint32_t)i, f);
    }
    p += 257;
    p = (p + 3u) & ~3u;

    uint32_t lower_tab = p;
    for (int i = 0; i < 257; i++) {
        int ch = i - 1;
        int16_t v = (int16_t)((ch >= 0 && ch < 256) ? tolower(ch) : ch);
        mr_st16(c, p + (uint32_t)i * 2, (uint16_t)v);
    }
    p += 257 * 2;
    p = (p + 3u) & ~3u;

    uint32_t upper_tab = p;
    for (int i = 0; i < 257; i++) {
        int ch = i - 1;
        int16_t v = (int16_t)((ch >= 0 && ch < 256) ? toupper(ch) : ch);
        mr_st16(c, p + (uint32_t)i * 2, (uint16_t)v);
    }
    p += 257 * 2;
    p = (p + 7u) & ~7u;

    uint32_t iid_engine = p;
    p += 16;
    uint32_t iid_bufq = p;
    p += 16;
    uint32_t iid_play = p;
    p += 16;

    uint32_t sF = p;
    STDIO_BASE = sF;
    p += 3 * MR_FILE_SZ;
    for (int i = 0; i < 3; i++)
        mr_st16(c, sF + (uint32_t)i * MR_FILE_SZ + 14, (uint16_t)i); // _file

    FILEBUF_BASE = p;
    p += MAX_FILES * MR_FILE_SZ;

    ERRNO_SLOT = p;
    mr_st32(c, p, 0);
    p += 8;
    p = (p + 7u) & ~7u;

// Symbols occupy four-byte guest slots.
#define SLOT(name, value)                                                                          \
    do {                                                                                           \
        mr_st32(c, p, (value));                                                                    \
        data_add((name), p);                                                                       \
        p += 4;                                                                                    \
    } while (0)

    SLOT("_ctype_", ctype_tab);
    SLOT("_tolower_tab_", lower_tab);
    SLOT("_toupper_tab_", upper_tab);
    SLOT("SL_IID_ENGINE", iid_engine);
    SLOT("SL_IID_BUFFERQUEUE", iid_bufq);
    SLOT("SL_IID_PLAY", iid_play);
    SLOT("__stack_chk_guard", 0xDEADC0DEu);
#undef SLOT

    data_add("__sF", sF);

    return (p + 7u) & ~7u;
}

uint32_t mr_shim_data_lookup(const char *name) {
    for (int i = 0; i < DATA_COUNT; i++)
        if (strcmp(DATA_SYMS[i].name, name) == 0) return DATA_SYMS[i].addr;
    return 0;
}

// Locale.

static void s_wctob(mr_cpu *c) {
    RET((A0 < 128) ? A0 : 0xFFFFFFFFu);
} // EOF.
static void s_btowc(mr_cpu *c) {
    RET((A0 < 128) ? A0 : 0xFFFFFFFFu);
}
static void s_setlocale(mr_cpu *c) {
    RET(0);
}

static uint64_t rng_state = 0x1234ABCD330Eull;

static uint32_t rng_next(void) {
    rng_state = (0x5DEECE66Dull * rng_state + 0xB) & 0xFFFFFFFFFFFFull;
    return (uint32_t)(rng_state >> 16);
}

static void s_lrand48(mr_cpu *c) {
    RET(rng_next() & 0x7FFFFFFF);
}
static void s_srand(mr_cpu *c) {
    rng_state = ((uint64_t)A0 << 16) | 0x330E;
    RET(0);
}

static uint64_t guest_monotonic_ns(const mr_cpu *c) {
    uint64_t ns;
    return mr_guest_clock_monotonic(c, &ns) ? ns : mr_mono_ns();
}

static uint64_t guest_realtime_ns(const mr_cpu *c) {
    uint64_t ns;
    return mr_guest_clock_realtime(c, &ns) ? ns : mr_real_ns();
}

static void s_time(mr_cpu *c) {
    uint32_t t = (uint32_t)(guest_realtime_ns(c) / 1000000000ull);
    if (A0) mr_st32(c, A0, t);
    RET(t);
}

static void s_clock_gettime(mr_cpu *c) {
    // bionic uses CLOCK_REALTIME=0 and CLOCK_MONOTONIC=1.
    uint64_t ns = A0 == 0 ? guest_realtime_ns(c) : guest_monotonic_ns(c);
    if (A1) {
        mr_st32(c, A1, (uint32_t)(ns / 1000000000ull));
        mr_st32(c, A1 + 4, (uint32_t)(ns % 1000000000ull));
    }
    RET(0);
}

static void s_gettimeofday(mr_cpu *c) {
    uint64_t ns = guest_realtime_ns(c);
    if (A0) {
        mr_st32(c, A0, (uint32_t)(ns / 1000000000ull));
        mr_st32(c, A0 + 4, (uint32_t)(ns % 1000000000ull / 1000ull));
    }
    RET(0);
}

static void s_clock(mr_cpu *c) {
    RET((uint32_t)(guest_monotonic_ns(c) / 1000ull));
}

// Locale classes, system calls, and file handling.

enum {
    WCT_ALNUM = 1,
    WCT_ALPHA,
    WCT_BLANK,
    WCT_CNTRL,
    WCT_DIGIT,
    WCT_GRAPH,
    WCT_LOWER,
    WCT_PRINT,
    WCT_PUNCT,
    WCT_SPACE,
    WCT_UPPER,
    WCT_XDIGIT
};

static void s_wctype(mr_cpu *c) {
    const char *n = GS(c, A0);
    static const struct {
        const char *name;
        int id;
    } T[] = {
        {"alnum", WCT_ALNUM}, {"alpha", WCT_ALPHA}, {"blank", WCT_BLANK}, {"cntrl", WCT_CNTRL},
        {"digit", WCT_DIGIT}, {"graph", WCT_GRAPH}, {"lower", WCT_LOWER}, {"print", WCT_PRINT},
        {"punct", WCT_PUNCT}, {"space", WCT_SPACE}, {"upper", WCT_UPPER}, {"xdigit", WCT_XDIGIT},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (strcmp(n, T[i].name) == 0) {
            RET(T[i].id);
            return;
        }
    RET(0);
}

static void s_iswctype(mr_cpu *c) {
    wint_t ch = (wint_t)A0;
    switch (A1) {
    case WCT_ALNUM:
        RET(iswalnum(ch));
        return;
    case WCT_ALPHA:
        RET(iswalpha(ch));
        return;
    case WCT_BLANK:
        RET(ch == ' ' || ch == '\t');
        return;
    case WCT_CNTRL:
        RET(iswcntrl(ch));
        return;
    case WCT_DIGIT:
        RET(iswdigit(ch));
        return;
    case WCT_GRAPH:
        RET(iswgraph(ch));
        return;
    case WCT_LOWER:
        RET(iswlower(ch));
        return;
    case WCT_PRINT:
        RET(iswprint(ch));
        return;
    case WCT_PUNCT:
        RET(iswpunct(ch));
        return;
    case WCT_SPACE:
        RET(iswspace(ch));
        return;
    case WCT_UPPER:
        RET(iswupper(ch));
        return;
    case WCT_XDIGIT:
        RET(iswxdigit(ch));
        return;
    default:
        RET(0);
        return;
    }
}

static void s_towlower(mr_cpu *c) {
    RET((uint32_t)towlower((wint_t)A0));
}
static void s_towupper(mr_cpu *c) {
    RET((uint32_t)towupper((wint_t)A0));
}

// Process identity.
static void s_getpid(mr_cpu *c) {
    RET(1);
}

static FILE *FILES[MAX_FILES];
static char DATA_ROOT[1024];
static char DATA_BASE[1024];
static int KEEP_GUEST_LOGS;

void mr_set_data_root(const char *path) {
    if (!path || snprintf(DATA_ROOT, sizeof(DATA_ROOT), "%s", path) >= (int)sizeof(DATA_ROOT))
        DATA_ROOT[0] = '\0';
    KEEP_GUEST_LOGS = getenv("MR_GUEST_LOG") != NULL;
}

void mr_set_data_base(const char *path) {
    if (!path || snprintf(DATA_BASE, sizeof(DATA_BASE), "%s", path) >= (int)sizeof(DATA_BASE))
        DATA_BASE[0] = '\0';
}

const char *mr_data_base(void) {
    return DATA_BASE;
}

static int copy_from_base(const char *from, const char *to) {
    char dir[1024];
    if (snprintf(dir, sizeof dir, "%s", to) >= (int)sizeof dir) return -1;
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(dir, 0777);
            *p = '/';
        }

    int in = open(from, O_RDONLY);
    if (in < 0) return -1;
    char temporary[1024];
    if (snprintf(temporary, sizeof temporary, "%s.copy.XXXXXX", to) >= (int)sizeof temporary) {
        close(in);
        return -1;
    }
    int out = mkstemp(temporary);
    if (out < 0) {
        close(in);
        return -1;
    }
    unsigned char buffer[64 * 1024];
    int rc = 0;
    for (;;) {
        ssize_t got = read(in, buffer, sizeof buffer);
        if (got == 0) break;
        if (got < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        size_t written = 0;
        while (written < (size_t)got) {
            ssize_t n = write(out, buffer + written, (size_t)got - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                rc = -1;
                break;
            }
            written += (size_t)n;
        }
        if (rc != 0) break;
    }
    if (close(in) != 0) rc = -1;
    if (rc == 0 && fsync(out) != 0) rc = -1;
    if (close(out) != 0) rc = -1;
    if (rc == 0 && rename(temporary, to) != 0) rc = -1;
    if (rc != 0) unlink(temporary);
    return rc;
}

#define PATH_CACHE_SLOTS 256

typedef struct {
    char guest[256];
    char host[1024];
    int for_write;
    int used;
} path_entry;

static path_entry PATH_CACHE[PATH_CACHE_SLOTS];
static uint64_t PATH_HITS, PATH_MISSES;

static size_t path_hash(const char *s, int for_write) {
    size_t h = (size_t)for_write * 1099511628211u ^ 14695981039346656037u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211u;
    return h % PATH_CACHE_SLOTS;
}

static void path_cache_flush(void) {
    for (int i = 0; i < PATH_CACHE_SLOTS; i++)
        PATH_CACHE[i].used = 0;
}

void mr_shim_path_cache_flush(void) {
    path_cache_flush();
}

void mr_shim_path_cache_stats(uint64_t *hits, uint64_t *misses) {
    if (hits) *hits = PATH_HITS;
    if (misses) *misses = PATH_MISSES;
}

static void map_path_uncached(const char *guest, char *out, size_t n, int for_write);

static void map_write_target(const char *guest, char *out, size_t n);

static int map_allowed_host_path(const char *guest, char *out, size_t n, int for_write) {
    static const struct {
        const char *path;
        int writable;
    } ALLOWED[] = {
        {"/dev/null", 1},
        {"/dev/urandom", 0},
        {"/dev/random", 0},
        {"/dev/srandom", 0},
        {"/dev/egd-pool", 0},
        {"/dev/tty", 1},
        {"/proc/cpuinfo", 0},
        {"/sys/devices/system/cpu/present", 0},
        {"/sys/devices/system/cpu/possible", 0},
    };
    for (size_t i = 0; i < sizeof(ALLOWED) / sizeof(ALLOWED[0]); i++) {
        if (strcmp(guest, ALLOWED[i].path) != 0 || (for_write && !ALLOWED[i].writable)) continue;
        int written = snprintf(out, n, "%s", guest);
        if (written < 0 || (size_t)written >= n) out[0] = '\0';
        return 1;
    }
    return 0;
}

static int safe_relative_path(const char *path) {
    if (strcmp(path, "..") == 0 || strncmp(path, "../", 3) == 0) return 0;
    for (const char *p = strstr(path, "/.."); p; p = strstr(p + 3, "/.."))
        if (p[3] == '/' || p[3] == '\0') return 0;
    return 1;
}

static void join_guest_root(const char *root, const char *relative, char *out, size_t n) {
    if (!n) return;
    if (!root[0] || !safe_relative_path(relative)) {
        out[0] = '\0';
        return;
    }
    int written = snprintf(out, n, "%s/%s", root, relative);
    if (written < 0 || (size_t)written >= n) out[0] = '\0';
}

static const char *guest_relative_path(const char *guest) {
    if (strcmp(guest, MR_GUEST_ROOT) == 0) return "";

    static const char *PREFIXES[] = {
        MR_GUEST_ROOT "/",
        "/storage/emulated/0/Android/data/com.gameloft.android.ANMP.GloftDMHM/files/",
        "/sdcard/Android/data/com.gameloft.android.ANMP.GloftDMHM/files/",
        "/mnt/sdcard/Android/data/com.gameloft.android.ANMP.GloftDMHM/files/",
        "/data/data/com.gameloft.android.ANMP.GloftDMHM/files/",
        "/sdcard/gameloft/games/GloftDMHM/",
    };
    for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++) {
        size_t len = strlen(PREFIXES[i]);
        if (strncmp(guest, PREFIXES[i], len) == 0) return guest + len;
    }
    return guest[0] == '/' ? guest + 1 : guest;
}

static void map_path_mode(const char *guest, char *out, size_t n, int for_write) {
    size_t slot = path_hash(guest, for_write);
    path_entry *e = &PATH_CACHE[slot];
    if (!for_write && e->used && !e->for_write && strcmp(e->guest, guest) == 0) {
        snprintf(out, n, "%s", e->host);
        PATH_HITS++;
        return;
    }

    map_path_uncached(guest, out, n, for_write);
    PATH_MISSES++;

    if (for_write) {
        path_cache_flush();
        return;
    }
    if (strlen(guest) < sizeof e->guest && strlen(out) < sizeof e->host) {
        snprintf(e->guest, sizeof e->guest, "%s", guest);
        snprintf(e->host, sizeof e->host, "%s", out);
        e->for_write = 0;
        e->used = 1;
    }
}

static void map_path_uncached(const char *guest, char *out, size_t n, int for_write) {
    if (!n) return;
    const char *base = strrchr(guest, '/');
    base = base ? base + 1 : guest;
    if (!KEEP_GUEST_LOGS && strcmp(base, "manhattan.log") == 0) {
        snprintf(out, n, "/dev/null");
        return;
    }

    if (map_allowed_host_path(guest, out, n, for_write)) return;

    const char *relative = guest_relative_path(guest);
    if (!for_write && mr_localization_override_path(relative, out, n)) {
        if (getenv("MR_DIAGNOSTICS") &&
            (strstr(relative, ".texts") || strstr(relative, "gui_fonts_info") ||
             strstr(relative, "langs.json")))
            printf("[Localization] %s -> %s\n", relative, out);
        return;
    }
    join_guest_root(DATA_ROOT, relative, out, n);
    if (!out[0] || !DATA_BASE[0] || access(out, F_OK) == 0) return;

    char from_base[1024];
    join_guest_root(DATA_BASE, relative, from_base, sizeof from_base);
    if (!from_base[0] || access(from_base, F_OK) != 0) return;

    if (!for_write) {
        snprintf(out, n, "%s", from_base);
        if (getenv("MR_DIAGNOSTICS") &&
            (strstr(relative, ".texts") || strstr(relative, "gui_fonts_info") ||
             strstr(relative, "langs.json")))
            printf("[Localization] %s -> %s\n", relative, out);
        return;
    }

    if (copy_from_base(from_base, out) != 0) out[0] = '\0';
}

static void map_path(const char *guest, char *out, size_t n) {
    map_path_mode(guest, out, n, 0);
}

static void map_write_target(const char *guest, char *out, size_t n) {
    if (!n) return;
    if (map_allowed_host_path(guest, out, n, 1)) return;
    join_guest_root(DATA_ROOT, guest_relative_path(guest), out, n);
}

static void map_path_base_only(const char *guest, char *out, size_t n) {
    if (!DATA_BASE[0]) {
        out[0] = 0;
        return;
    }
    if (map_allowed_host_path(guest, out, n, 0)) return;
    join_guest_root(DATA_BASE, guest_relative_path(guest), out, n);
}

static uint64_t OPEN_CALLS;
static double OPEN_MS;

static int read_only_mode(const char *mode) {
    return mode && mode[0] == 'r' && !strchr(mode, '+');
}

static int file_slot(void) {
    for (int i = 1; i < MAX_FILES; i++)
        if (!FILES[i]) return i;
    return 0;
}

static FILE *file_of(uint32_t h) {
    if (STDIO_BASE && h >= STDIO_BASE && h < STDIO_BASE + 3 * MR_FILE_SZ) {
        uint32_t i = (h - STDIO_BASE) / MR_FILE_SZ;
        return i == 0 ? stdin : i == 1 ? stdout : stderr;
    }
    if (FILEBUF_BASE && h >= FILEBUF_BASE && h < FILEBUF_BASE + MAX_FILES * MR_FILE_SZ)
        return FILES[(h - FILEBUF_BASE) / MR_FILE_SZ];
    return NULL;
}

static uint32_t file_handle(int slot) {
    return FILEBUF_BASE + (uint32_t)slot * MR_FILE_SZ;
}

static int is_std_out(uint32_t h) {
    return STDIO_BASE && h >= STDIO_BASE + MR_FILE_SZ && h < STDIO_BASE + 3 * MR_FILE_SZ;
}

static void s_fopen(mr_cpu *c) {
    double open_t0 = mr_monotonic_ms();
    char path[2048];
    const char *mode = A1 ? GS(c, A1) : "rb";
    map_path_mode(GS(c, A0), path, sizeof(path), !read_only_mode(mode));
    int slot = file_slot();
    if (!slot) {
        RET(0);
        return;
    }
    FILE *f = fopen(path, mode);
    OPEN_MS += mr_monotonic_ms() - open_t0;
    OPEN_CALLS++;
    if (!f) {
        RET(0);
        return;
    }
    FILES[slot] = f;
    uint32_t h = file_handle(slot);
    // Guest C++ streams read the descriptor from the FILE _file field.
    memset(mr_mem(c, h), 0, MR_FILE_SZ);
    mr_st16(c, h + 14, (uint16_t)fileno(f));
    RET(h);
}

static void s_fclose(mr_cpu *c) {
    FILE *f = file_of(A0);
    if (f) {
        fclose(f);
        if (A0 >= FILEBUF_BASE) FILES[(A0 - FILEBUF_BASE) / MR_FILE_SZ] = NULL;
    }
    RET(0);
}

static uint64_t READ_CALLS, READ_BYTES;
static double READ_MS;

void mr_shim_io_stats(uint64_t *calls, uint64_t *bytes, double *ms) {
    if (calls) *calls = READ_CALLS;
    if (bytes) *bytes = READ_BYTES;
    if (ms) *ms = READ_MS;
}

void mr_shim_open_stats(uint64_t *calls, double *ms) {
    if (calls) *calls = OPEN_CALLS;
    if (ms) *ms = OPEN_MS;
}

static void s_fread(mr_cpu *c) {
    FILE *f = file_of(A3);
    uint64_t bytes = (uint64_t)A1 * A2;
    if (bytes > UINT32_MAX) {
        mr_sched_set_errno(c, EOVERFLOW);
        RET(0);
        return;
    }
    uint32_t n = (uint32_t)bytes;
    if (!f || !n) {
        RET(0);
        return;
    }
    void *dst = GW(c, A0, n);
    if (c->halted) {
        RET(0);
        return;
    }
    double t0 = mr_monotonic_ms();
    size_t got = fread(dst, A1, A2, f);
    READ_MS += mr_monotonic_ms() - t0;
    READ_CALLS++;
    READ_BYTES += (uint64_t)got * A1;
    RET((uint32_t)got);
}

static void s_fwrite_file(mr_cpu *c) {
    FILE *f = file_of(A3);
    uint64_t bytes = (uint64_t)A1 * A2;
    if (bytes > UINT32_MAX) {
        mr_sched_set_errno(c, EOVERFLOW);
        RET(0);
        return;
    }
    uint32_t n = (uint32_t)bytes;
    if (!f || !n) {
        RET(0);
        return;
    }
    void *src = GN(c, A0, n);
    if (c->halted) {
        RET(0);
        return;
    }
    RET((uint32_t)fwrite(src, A1, A2, f));
}

static void s_fseek(mr_cpu *c) {
    FILE *f = file_of(A0);
    RET(f ? (uint32_t)fseek(f, (long)(int32_t)A1, (int)A2) : 0xFFFFFFFFu);
}
static void s_ftell(mr_cpu *c) {
    FILE *f = file_of(A0);
    RET(f ? (uint32_t)ftell(f) : 0xFFFFFFFFu);
}
static void s_fflush(mr_cpu *c) {
    FILE *f = file_of(A0);
    if (f) fflush(f);
    RET(0);
}

static void s_fgetc(mr_cpu *c) {
    FILE *f = file_of(A0);
    RET(f ? (uint32_t)fgetc(f) : 0xFFFFFFFFu);
}

static void s_fgets(mr_cpu *c) {
    FILE *f = file_of(A2);
    uint32_t n = A1;
    if (!f || !n || n > INT32_MAX) {
        RET(0);
        return;
    }
    char *dst = GW(c, A0, n);
    if (c->halted) {
        RET(0);
        return;
    }
    RET(fgets(dst, (int)n, f) ? A0 : 0);
}

static void s_remove(mr_cpu *c) {
    char path[2048];
    map_path_mode(GS(c, A0), path, sizeof(path), 1);
    path_cache_flush();
    RET((uint32_t)remove(path));
}

static void s_rename(mr_cpu *c) {
    char old_path[2048], new_path[2048];
    map_path_mode(GS(c, A0), old_path, sizeof(old_path), 1);
    map_write_target(GS(c, A1), new_path, sizeof(new_path));
    path_cache_flush();
    RET((uint32_t)rename(old_path, new_path));
}

static void s_chmod(mr_cpu *c) {
    char path[2048];
    map_path_mode(GS(c, A0), path, sizeof(path), 1);
    RET((uint32_t)chmod(path, (mode_t)(A1 & 07777u)));
}

static void s_mkdir(mr_cpu *c) {
    char path[2048];
    map_write_target(GS(c, A0), path, sizeof(path));
    path_cache_flush();
    RET((uint32_t)mkdir(path, (mode_t)(A1 & 07777u)));
}

// Formatted output, mathematics, and low-level file handling.

typedef struct {
    mr_cpu *c;
    int idx;
    uint32_t ap;
} va_walk;

static uint32_t va_next(va_walk *v) {
    if (v->ap) {
        uint32_t x = mr_ld32(v->c, v->ap);
        v->ap += 4;
        return x;
    }
    uint32_t x = (v->idx < 4) ? v->c->r[v->idx]
                              : mr_ld32(v->c, v->c->r[MR_R_SP] + (uint32_t)(v->idx - 4) * 4);
    v->idx++;
    return x;
}

static uint64_t va_next64(va_walk *v) {
    if (v->ap)
        v->ap = (v->ap + 7u) & ~7u; // Eight-byte alignment.
    else if (v->idx & 1)
        v->idx++;
    uint32_t lo = va_next(v);
    uint32_t hi = va_next(v);
    return ((uint64_t)hi << 32) | lo;
}

// Formatter covering the conversion forms used by the engine.
static int guest_vsnprintf(mr_cpu *c, char *out, size_t cap, const char *fmt, va_walk *v) {
    size_t w = 0;
#define PUT(ch)                                                                                    \
    do {                                                                                           \
        if (w + 1 < cap) out[w] = (ch);                                                            \
        w++;                                                                                       \
    } while (0)

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            PUT(*p);
            continue;
        }
        p++;
        if (*p == '%') {
            PUT('%');
            continue;
        }

        char sub[64];
        size_t sw = 0;
        int sub_ok = 1;
#define SUB(ch)                                                                                    \
    do {                                                                                           \
        if (sw + 1 < sizeof(sub))                                                                  \
            sub[sw++] = (ch);                                                                      \
        else                                                                                       \
            sub_ok = 0;                                                                            \
    } while (0)
#define SUB_UINT(value)                                                                            \
    do {                                                                                           \
        char digits[16];                                                                           \
        snprintf(digits, sizeof(digits), "%u", (unsigned)(value));                                 \
        for (const char *d = digits; *d; d++)                                                      \
            SUB(*d);                                                                               \
    } while (0)

        SUB('%');
        int left = 0;
        while (*p && strchr("-+ #0", *p)) {
            if (*p == '-') left = 1;
            SUB(*p++);
        }

        if (*p == '*') {
            int32_t width = (int32_t)va_next(v);
            p++;
            if (width < 0) {
                if (!left) SUB('-');
                SUB_UINT(0u - (uint32_t)width);
            } else {
                SUB_UINT((uint32_t)width);
            }
        } else {
            while (*p >= '0' && *p <= '9')
                SUB(*p++);
        }

        if (*p == '.') {
            p++;
            if (*p == '*') {
                int32_t precision = (int32_t)va_next(v);
                p++;
                // A negative star precision behaves as if precision were omitted.
                if (precision >= 0) {
                    SUB('.');
                    SUB_UINT((uint32_t)precision);
                }
            } else {
                SUB('.');
                while (*p >= '0' && *p <= '9')
                    SUB(*p++);
            }
        }

        int is64 = 0;
        while (*p && strchr("hlLzjt", *p)) {
            if ((*p == 'l' && p[1] == 'l') || *p == 'j') is64 = 1;
            p++;
        }
        char conv = *p;
        if (!conv) {
            PUT('%');
            break;
        }
        int integer = strchr("diuxXo", conv) != NULL;
        if (integer && is64) {
            SUB('l');
            SUB('l');
        }
        SUB(conv);
        if (!sub_ok) {
            sw = 0;
            sub[sw++] = '%';
            if (integer && is64) {
                sub[sw++] = 'l';
                sub[sw++] = 'l';
            }
            sub[sw++] = conv;
        }
        sub[sw] = 0;
#undef SUB_UINT
#undef SUB

        char tmp[512];
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            if (is64) {
                uint64_t value = va_next64(v);
                if (conv == 'd' || conv == 'i')
                    snprintf(tmp, sizeof(tmp), sub, (long long)value);
                else
                    snprintf(tmp, sizeof(tmp), sub, (unsigned long long)value);
            } else {
                uint32_t value = va_next(v);
                if (conv == 'd' || conv == 'i')
                    snprintf(tmp, sizeof(tmp), sub, (int32_t)value);
                else
                    snprintf(tmp, sizeof(tmp), sub, value);
            }
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            uint64_t bits = va_next64(v);
            double d;
            memcpy(&d, &bits, 8);
            snprintf(tmp, sizeof(tmp), sub, d);
            break;
        }
        case 'c':
            snprintf(tmp, sizeof(tmp), sub, (int)va_next(v));
            break;
        case 's': {
            uint32_t sp = va_next(v);
            const char *s = sp ? GS(c, sp) : "(null)";
            snprintf(tmp, sizeof(tmp), sub, s);
            break;
        }
        case 'p':
            snprintf(tmp, sizeof(tmp), "0x%08x", va_next(v));
            break;
        default:
            snprintf(tmp, sizeof(tmp), "%s", sub);
            break;
        }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
        for (const char *q = tmp; *q; q++)
            PUT(*q);
    }
    if (cap) out[w < cap ? w : cap - 1] = 0;
#undef PUT
    return (int)w;
}

// Shared formatter for the logging shim.
static int guest_log_format(mr_cpu *c, char *out, size_t cap, const char *fmt, int first_arg) {
    va_walk v = {c, first_arg, 0};
    return guest_vsnprintf(c, out, cap, fmt, &v);
}

static int guest_vformat_to_memory(mr_cpu *c, uint32_t dst, uint32_t cap, const char *fmt,
                                   va_walk *v) {
    char buf[4096];
    int n = guest_vsnprintf(c, buf, sizeof(buf), fmt, v);
    size_t len = strlen(buf) + 1;
    if (len > cap) len = cap;
    if (cap) {
        void *out = GW(c, dst, (uint32_t)len);
        if (!c->halted) {
            memcpy(out, buf, len);
            ((char *)out)[len - 1] = 0;
        }
    }
    return n;
}

static int guest_vformat_to_file(mr_cpu *c, FILE *f, int prefix, const char *fmt, va_walk *v) {
    char buf[4096];
    int n = guest_vsnprintf(c, buf, sizeof(buf), fmt, v);
    if (f) {
        if (prefix) fputs("[guest] ", f);
        fputs(buf, f);
    }
    return n;
}

static void s_sprintf(mr_cpu *c) {
    va_walk v = {c, 2, 0};
    RET(guest_vformat_to_memory(c, A0, UINT32_MAX, GS(c, A1), &v));
}

static void s_snprintf(mr_cpu *c) {
    va_walk v = {c, 3, 0};
    RET(guest_vformat_to_memory(c, A0, A1, GS(c, A2), &v));
}

static uint64_t MESH_LINES;

static int chatty_guest_line(const char *s) {
    static int show_all = -1;
    if (show_all < 0) show_all = getenv("MR_GUEST_LOG") ? 1 : 0;
    if (show_all) return 0;
    return strstr(s, "Mesh CreateShape") != NULL;
}

static void s_printf(mr_cpu *c) {
    va_walk v = {c, 1, 0};
    const char *fmt = GS(c, A0);
    if (chatty_guest_line(fmt)) {
        MESH_LINES++;
        char scratch[4096];
        RET(guest_vsnprintf(c, scratch, sizeof scratch, fmt, &v));
        return;
    }
    RET(guest_vformat_to_file(c, stdout, 0, fmt, &v));
}

static void s_vsnprintf(mr_cpu *c) {
    va_walk v = {c, 0, A3};
    RET(guest_vformat_to_memory(c, A0, A1, GS(c, A2), &v));
}

static void s_vsprintf(mr_cpu *c) {
    va_walk v = {c, 0, A2};
    RET(guest_vformat_to_memory(c, A0, UINT32_MAX, GS(c, A1), &v));
}

static void s_android_log_v(mr_cpu *c) {
    const char *tag = A1 ? GS(c, A1) : "?";
    const char *fmt = A2 ? GS(c, A2) : "";
    char buf[4096];
    va_walk v = {c, 0, A3};
    guest_vsnprintf(c, buf, sizeof(buf), fmt, &v);
    printf("[guest %s] %s\n", tag, buf);
    RET(0);
}

// Mathematics with softfp doubles in two words.
#define MATH1(nm, fn)                                                                              \
    static void nm(mr_cpu *c) {                                                                    \
        uint64_t b = ((uint64_t)A1 << 32) | A0;                                                    \
        double d;                                                                                  \
        memcpy(&d, &b, 8);                                                                         \
        double r = fn(d);                                                                          \
        memcpy(&b, &r, 8);                                                                         \
        c->r[0] = (uint32_t)b;                                                                     \
        c->r[1] = (uint32_t)(b >> 32);                                                             \
    }
// clang-format off
MATH1(s_floor, floor)
MATH1(s_ceil, ceil)
MATH1(s_acos, acos)
MATH1(s_asin, asin)
MATH1(s_atan, atan)
MATH1(s_sinh, sinh)
MATH1(s_cosh, cosh)
MATH1(s_tanh, tanh)
MATH1(s_log10, log10)
// clang-format on

static double guest_double_arg(mr_cpu *c) {
    uint64_t bits = ((uint64_t)A1 << 32) | A0;
    double value;
    memcpy(&value, &bits, 8);
    return value;
}

static void guest_double_result(mr_cpu *c, double value) {
    uint64_t bits;
    memcpy(&bits, &value, 8);
    c->r[0] = (uint32_t)bits;
    c->r[1] = (uint32_t)(bits >> 32);
}

static void s_frexp(mr_cpu *c) {
    int exponent = 0;
    double fraction = frexp(guest_double_arg(c), &exponent);
    if (A2 && mr_mem_ok(c, A2, 4)) mr_st32(c, A2, (uint32_t)exponent);
    guest_double_result(c, fraction);
}

static void s_ldexp(mr_cpu *c) {
    guest_double_result(c, ldexp(guest_double_arg(c), (int)(int32_t)A2));
}

static void s_modf(mr_cpu *c) {
    double integral = 0.0;
    double fraction = modf(guest_double_arg(c), &integral);
    if (A2 && mr_mem_ok(c, A2, 8)) {
        uint64_t bits;
        memcpy(&bits, &integral, 8);
        mr_st32(c, A2, (uint32_t)bits);
        mr_st32(c, A2 + 4, (uint32_t)(bits >> 32));
    }
    guest_double_result(c, fraction);
}

// Low-level file handling.

static void io_track(int fd);

static void s_open(mr_cpu *c) {
    double open_t0 = mr_monotonic_ms();
    char path[2048];
    const int writing = (A1 & 3u) != 0u || (A1 & 0x40u) || (A1 & 0x200u);
    map_path_mode(GS(c, A0), path, sizeof(path), writing);

    const uint32_t guest = A1;
    int flags;
    switch (guest & 3u) {
    case 0:
        flags = O_RDONLY;
        break;
    case 1:
        flags = O_WRONLY;
        break;
    case 2:
        flags = O_RDWR;
        break;
    default:
        errno = EINVAL;
        mr_sched_set_errno(c, EINVAL);
        RET(0xFFFFFFFFu);
        return;
    }

    if (guest & 0x00000040u) flags |= O_CREAT;
    if (guest & 0x00000080u) flags |= O_EXCL;
    if (guest & 0x00000200u) flags |= O_TRUNC;
    if (guest & 0x00000400u) flags |= O_APPEND;
    if (guest & 0x00000800u) flags |= O_NONBLOCK;
    if (guest & 0x00101000u) flags |= O_SYNC;
    if (guest & 0x00010000u) flags |= O_DIRECTORY;
    if (guest & 0x00020000u) flags |= O_NOFOLLOW;
    if (guest & 0x00080000u) flags |= O_CLOEXEC;

    int fd = (flags & O_CREAT) ? open(path, flags, (mode_t)(A2 & 07777u)) : open(path, flags);
    if (fd < 0) {
        mr_sched_set_errno(c, errno);
    } else if ((guest & 3u) == 0) {
        io_track(fd);
    }
    OPEN_MS += mr_monotonic_ms() - open_t0;
    OPEN_CALLS++;
    RET(fd < 0 ? 0xFFFFFFFFu : (uint32_t)fd);
}

#define IO_WINDOW 32768
#define IO_SLOTS 16

typedef struct {
    int fd; // -1 means unused.
    off_t pos;
    off_t start;
    uint32_t len;
    uint64_t used; // Last-use counter for LRU replacement.
    unsigned char data[IO_WINDOW];
} io_window;

static io_window IO[IO_SLOTS];
static uint64_t IO_CLOCK;
static int IO_READY;

static io_window *io_find(int fd) {
    for (int i = 0; i < IO_SLOTS; i++)
        if (IO[i].fd == fd) {
            IO[i].used = ++IO_CLOCK;
            return &IO[i];
        }
    return NULL;
}

static void io_track(int fd) {
    if (!IO_READY) {
        for (int i = 0; i < IO_SLOTS; i++)
            IO[i].fd = -1;
        IO_READY = 1;
    }
    io_window *slot = &IO[0];
    for (int i = 0; i < IO_SLOTS; i++) {
        if (IO[i].fd < 0) {
            slot = &IO[i];
            break;
        }
        if (IO[i].used < slot->used) slot = &IO[i];
    }
    slot->fd = fd;
    slot->pos = slot->start = 0;
    slot->len = 0;
    slot->used = ++IO_CLOCK;
}

static void io_drop(int fd) {
    io_window *w = io_find(fd);
    if (w) {
        w->fd = -1;
        w->len = 0;
    }
}

// Read the requested range and return the number of copied bytes.
static ssize_t io_read(io_window *w, void *dst, uint32_t n) {
    unsigned char *out = (unsigned char *)dst;
    uint32_t done = 0;
    while (done < n) {
        // Refill the window from the current position on a miss.
        if (w->pos < w->start || w->pos >= w->start + (off_t)w->len) {
            double t0 = mr_monotonic_ms();
            ssize_t got = pread(w->fd, w->data, IO_WINDOW, w->pos);
            READ_MS += mr_monotonic_ms() - t0;
            READ_CALLS++;
            if (got <= 0) break;
            READ_BYTES += (uint64_t)got;
            w->start = w->pos;
            w->len = (uint32_t)got;
        }
        uint32_t offset = (uint32_t)(w->pos - w->start);
        uint32_t avail = w->len - offset;
        uint32_t take = n - done < avail ? n - done : avail;
        memcpy(out + done, w->data + offset, take);
        w->pos += (off_t)take;
        done += take;
    }
    return (ssize_t)done;
}

static void s_read(mr_cpu *c) {
    uint32_t n = A2;
    void *dst = GW(c, A1, n);
    if (c->halted) {
        RET(0xFFFFFFFFu);
        return;
    }

    io_window *w = io_find((int)A0);
    if (w) {
        RET((uint32_t)io_read(w, dst, n));
        return;
    }

    double t0 = mr_monotonic_ms();
    ssize_t got = read((int)A0, dst, n);
    READ_MS += mr_monotonic_ms() - t0;
    READ_CALLS++;
    if (got > 0) READ_BYTES += (uint64_t)got;
    RET((uint32_t)got);
}

static void s_close(mr_cpu *c) {
    io_drop((int)A0);
    RET((uint32_t)close((int)A0));
}

static off_t io_seek(io_window *w, off_t offset, int whence) {
    off_t base = 0;
    if (whence == SEEK_CUR) {
        base = w->pos;
    } else if (whence == SEEK_END) {
        struct stat st;
        if (fstat(w->fd, &st) != 0) return (off_t)-1;
        base = st.st_size;
    } else if (whence != SEEK_SET) {
        errno = EINVAL;
        return (off_t)-1;
    }
    off_t target = base + offset;
    if (target < 0) {
        errno = EINVAL;
        return (off_t)-1;
    }
    w->pos = target;
    return target;
}

static void s_lseek(mr_cpu *c) {
    io_window *w = io_find((int)A0);
    if (w) {
        RET((uint32_t)io_seek(w, (off_t)(int32_t)A1, (int)A2));
        return;
    }
    RET((uint32_t)lseek((int)A0, (off_t)(int32_t)A1, (int)A2));
}

static void s_lseek64(mr_cpu *c) {
    // off64_t occupies words two and three; word four holds whence.
    int64_t off = (int64_t)(((uint64_t)A3 << 32) | A2);
    int whence = (int)mr_ld32(c, c->r[MR_R_SP]);
    io_window *w = io_find((int)A0);
    off_t r = w ? io_seek(w, (off_t)off, whence) : lseek((int)A0, (off_t)off, whence);
    c->r[0] = (uint32_t)r;
    c->r[1] = (uint32_t)((uint64_t)r >> 32);
}

#define MR_STAT_SZ 104u

static void store_stat(mr_cpu *c, uint32_t p, const struct stat *st) {
    if (!p || !mr_mem_ok(c, p, MR_STAT_SZ)) return;
    memset(mr_mem(c, p), 0, MR_STAT_SZ);
    mr_st32(c, p + 0, (uint32_t)st->st_dev);
    mr_st32(c, p + 12, (uint32_t)st->st_ino);
    mr_st32(c, p + 16, (uint32_t)st->st_mode);
    mr_st32(c, p + 20, (uint32_t)st->st_nlink);
    mr_st32(c, p + 24, (uint32_t)st->st_uid);
    mr_st32(c, p + 28, (uint32_t)st->st_gid);
    mr_st32(c, p + 48, (uint32_t)st->st_size);
    mr_st32(c, p + 52, (uint32_t)((uint64_t)st->st_size >> 32));
    mr_st32(c, p + 56, (uint32_t)st->st_blksize);
    mr_st32(c, p + 64, (uint32_t)st->st_blocks);
    mr_st32(c, p + 72, (uint32_t)st->st_atime);
    mr_st32(c, p + 80, (uint32_t)st->st_mtime);
    mr_st32(c, p + 88, (uint32_t)st->st_ctime);
    mr_st32(c, p + 96, (uint32_t)st->st_ino);
}

static void s_stat(mr_cpu *c) {
    char path[2048];
    map_path(GS(c, A0), path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) {
        RET(0xFFFFFFFFu);
        return;
    }
    store_stat(c, A1, &st);
    RET(0);
}

static void s_chdir(mr_cpu *c) {
    RET(0);
}

static void s_write(mr_cpu *c) {
    uint32_t n = A2;
    const void *src = GN(c, A1, n);
    if (c->halted) {
        RET(0xFFFFFFFFu);
        return;
    }
    RET((uint32_t)write((int)A0, src, n));
}

// Guest iovec layout: { void *base; size_t len }, eight bytes per entry.
static void s_writev(mr_cpu *c) {
    uint32_t vec = A1, cnt = A2;
    if (cnt > UINT32_MAX / 8u || (cnt && !mr_mem_ok(c, vec, cnt * 8u))) {
        mr_sched_set_errno(c, EFAULT);
        RET(0xFFFFFFFFu);
        return;
    }
    long total = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t base = mr_ld32(c, vec + i * 8);
        uint32_t len = mr_ld32(c, vec + i * 8 + 4);
        if (!len) continue;
        const void *src = GN(c, base, len);
        if (c->halted) {
            RET(0xFFFFFFFFu);
            return;
        }
        ssize_t w = write((int)A0, src, len);
        if (w < 0) {
            RET(0xFFFFFFFFu);
            return;
        }
        total += w;
        if ((uint32_t)w < len) break;
    }
    RET((uint32_t)total);
}

static void s_fstat(mr_cpu *c) {
    struct stat st;
    if (fstat((int)A0, &st) != 0) {
        RET(0xFFFFFFFFu);
        return;
    }
    store_stat(c, A1, &st);
    RET(0);
}

static void s_fsync(mr_cpu *c) {
    RET((uint32_t)fsync((int)A0));
}

static void s_pread(mr_cpu *c) {
    uint32_t len = A2;
    void *dst = GW(c, A1, len);
    if (c->halted) {
        RET(0xFFFFFFFFu);
        return;
    }
    ssize_t got = pread((int)A0, dst, len, (off_t)(int32_t)A3);
    if (got < 0) mr_sched_set_errno(c, errno);
    RET((uint32_t)got);
}

static void s_getcwd(mr_cpu *c) {
    uint32_t cap = A1;
    if (!A0 || !cap) {
        mr_sched_set_errno(c, EINVAL);
        RET(0);
        return;
    }
    char *dst = GW(c, A0, cap);
    if (c->halted) {
        RET(0);
        return;
    }
    if ((size_t)cap <= strlen(MR_GUEST_ROOT)) {
        mr_sched_set_errno(c, ERANGE);
        RET(0);
        return;
    }
    memcpy(dst, MR_GUEST_ROOT, strlen(MR_GUEST_ROOT) + 1);
    RET(A0);
}

static void s_rmdir(mr_cpu *c) {
    char path[2048];
    map_write_target(GS(c, A0), path, sizeof(path));
    path_cache_flush();
    RET((uint32_t)rmdir(path));
}

static void s_getuid(mr_cpu *c) {
    RET(1000);
}

static void s_exit(mr_cpu *c) {
    c->fault = "guest called exit()";
    c->fault_addr = A0;
    c->halted = 1;
}

static void s_stack_chk_fail(mr_cpu *c) {
    c->fault = "guest stack overflow (__stack_chk_fail)";
    c->fault_addr = c->r[MR_R_LR];
    c->halted = 1;
}

static void s_strtol(mr_cpu *c) {
    const char *s = GS(c, A0);
    char *end;
    long v = strtol(s, &end, (int)A2);
    if (A1 && mr_mem_ok(c, A1, 4)) mr_st32(c, A1, A0 + (uint32_t)(end - s));
    RET((uint32_t)v);
}

static void s_strtoul(mr_cpu *c) {
    const char *s = GS(c, A0);
    char *end;
    unsigned long v = strtoul(s, &end, (int)A2);
    if (A1 && mr_mem_ok(c, A1, 4)) mr_st32(c, A1, A0 + (uint32_t)(end - s));
    RET((uint32_t)v);
}

static void s_strtod(mr_cpu *c) {
    const char *s = GS(c, A0);
    char *end;
    double v = strtod(s, &end);
    if (A1 && mr_mem_ok(c, A1, 4)) mr_st32(c, A1, A0 + (uint32_t)(end - s));
    uint64_t b;
    memcpy(&b, &v, 8);
    c->r[0] = (uint32_t)b;
    c->r[1] = (uint32_t)(b >> 32);
}

static void s_difftime(mr_cpu *c) {
    double v = (double)(int32_t)A0 - (double)(int32_t)A1;
    uint64_t b;
    memcpy(&b, &v, 8);
    c->r[0] = (uint32_t)b;
    c->r[1] = (uint32_t)(b >> 32);
}

static void s_getenv(mr_cpu *c) {
    RET(0);
}

static uint32_t libc_call_guest(mr_cpu *c, uint32_t fn, uint32_t a0, uint32_t a1) {
    uint32_t saved_r[16];
    memcpy(saved_r, c->r, sizeof saved_r);
    mr_flags saved_f = c->f;
    int saved_thumb = c->thumb;
    uint32_t saved_it = c->itstate;

    uint32_t argv[2] = {a0, a1};
    uint32_t r = mr_guest_call(c, fn, 2, argv);

    memcpy(c->r, saved_r, sizeof saved_r);
    c->f = saved_f;
    c->thumb = saved_thumb;
    c->itstate = saved_it;
    return r;
}

static void s_qsort(mr_cpu *c) {
    uint32_t base = A0, n = A1, sz = A2, cmp = A3;
    if (!base || !cmp || n < 2 || !sz || sz > 4096) {
        RET(0);
        return;
    }
    uint64_t bytes = (uint64_t)n * sz;
    if (bytes > UINT32_MAX || !mr_mem_ok(c, base, (uint32_t)bytes)) {
        RET(0);
        return;
    }

    unsigned char tmp[4096];
    for (uint32_t i = 1; i < n; i++) {
        uint32_t cur = base + i * sz;
        memcpy(tmp, mr_mem(c, cur), sz);
        uint32_t j = i;
        while (j > 0) {
            uint32_t prev = base + (j - 1) * sz;
            memcpy(mr_mem(c, cur), tmp, sz);
            if ((int32_t)libc_call_guest(c, cmp, prev, cur) <= 0) break;
            memmove(mr_mem(c, base + j * sz), mr_mem(c, prev), sz);
            j--;
        }
        memcpy(mr_mem(c, base + j * sz), tmp, sz);
    }
    RET(0);
}

// bsearch(key, base, nmemb, size, compar)
static void s_bsearch(mr_cpu *c) {
    uint32_t key = A0, base = A1, n = A2, sz = A3;
    uint32_t cmp = mr_ld32(c, c->r[MR_R_SP]);
    if (!key || !base || !cmp || !n || !sz) {
        RET(0);
        return;
    }
    uint64_t bytes = (uint64_t)n * sz;
    if (bytes > UINT32_MAX || !mr_mem_ok(c, base, (uint32_t)bytes)) {
        RET(0);
        return;
    }

    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t at = base + mid * sz;
        int32_t r = (int32_t)libc_call_guest(c, cmp, key, at);
        if (r == 0) {
            RET(at);
            return;
        }
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    RET(0);
}

static void s_atol(mr_cpu *c) {
    RET((uint32_t)strtol(GS(c, A0), NULL, 10));
}

static void s_atoll(mr_cpu *c) {
    long long v = strtoll(GS(c, A0), NULL, 10);
    c->r[0] = (uint32_t)v;
    c->r[1] = (uint32_t)((uint64_t)v >> 32);
}

static void s_strtoll(mr_cpu *c) {
    const char *s = GS(c, A0);
    char *end;
    long long v = strtoll(s, &end, (int)A2);
    if (A1 && mr_mem_ok(c, A1, 4)) mr_st32(c, A1, A0 + (uint32_t)(end - s));
    c->r[0] = (uint32_t)v;
    c->r[1] = (uint32_t)((uint64_t)v >> 32);
}

static void s_fcntl(mr_cpu *c) {
    RET(0);
}
static void s_ioctl(mr_cpu *c) {
    RET(0xFFFFFFFFu);
}
static void s_atoi(mr_cpu *c) {
    RET((uint32_t)atoi(GS(c, A0)));
}

static void s_strpbrk(mr_cpu *c) {
    const char *s = GS(c, A0);
    const char *p = strpbrk(s, GS(c, A1));
    RET(p ? A0 + (uint32_t)(p - s) : 0);
}

static void s_statfs(mr_cpu *c) {
    // Report finite but ample free space to the engine.
    if (A1 && mr_mem_ok(c, A1, 64)) {
        memset(mr_mem(c, A1), 0, 64);
        mr_st32(c, A1 + 4, 4096);         // f_bsize
        mr_st32(c, A1 + 8, 1024u * 1024); // f_blocks
        mr_st32(c, A1 + 16, 512u * 1024); // f_bfree
        mr_st32(c, A1 + 20, 512u * 1024); // f_bavail
    }
    RET(0);
}

#define JB_WORDS 12

static void s_setjmp(mr_cpu *c) {
    uint32_t b = A0;
    if (!b || !mr_mem_ok(c, b, JB_WORDS * 4)) {
        RET(0);
        return;
    }
    for (int i = 0; i < 8; i++)
        mr_st32(c, b + (uint32_t)i * 4, c->r[4 + i]);
    mr_st32(c, b + 32, c->r[MR_R_SP]);
    mr_st32(c, b + 36, c->r[MR_R_LR]);
    RET(0);
}

static void s_longjmp(mr_cpu *c) {
    uint32_t b = A0, val = A1 ? A1 : 1;
    if (!b || !mr_mem_ok(c, b, JB_WORDS * 4)) {
        RET(val);
        return;
    }
    for (int i = 0; i < 8; i++)
        c->r[4 + i] = mr_ld32(c, b + (uint32_t)i * 4);
    c->r[MR_R_SP] = mr_ld32(c, b + 32);
    uint32_t ret = mr_ld32(c, b + 36);
    c->r[0] = val;
    c->thunk_redirect = ret ? ret : c->r[MR_R_LR];
}

static uint32_t STRERR_BUF;
static void s_strerror(mr_cpu *c) {
    if (!STRERR_BUF) STRERR_BUF = heap_alloc(c, 128);
    if (!STRERR_BUF) {
        RET(0);
        return;
    }
    void *d = GW(c, STRERR_BUF, 128);
    if (!c->halted) snprintf((char *)d, 128, "%s", strerror((int)A0));
    RET(STRERR_BUF);
}

static void s_memchr(mr_cpu *c) {
    uint32_t n = A2;
    if (!n) {
        RET(0);
        return;
    }
    const unsigned char *s = GN(c, A0, n);
    if (c->halted) {
        RET(0);
        return;
    }
    const unsigned char *p = memchr(s, (int)(A1 & 0xFF), n);
    RET(p ? A0 + (uint32_t)(p - s) : 0);
}

static void fail_errno(mr_cpu *c, int e) {
    errno = e;
    mr_sched_set_errno(c, e);
    RET(0xFFFFFFFFu);
}
static void s_pipe_offline(mr_cpu *c) {
    fail_errno(c, ENOSYS);
}
static uint64_t BLOCKED_NETWORK_CALLS;

uint64_t mr_blocked_network_calls(void) {
    return BLOCKED_NETWORK_CALLS;
}

static void s_network_offline(mr_cpu *c) {
    BLOCKED_NETWORK_CALLS++;
    if (getenv("MR_OFFLINE_LOG"))
        fprintf(stderr, "[offline] network import blocked at 0x%08x\n", c->r[MR_R_LR]);
    fail_errno(c, ENETDOWN);
}
static void s_network_pointer_offline(mr_cpu *c) {
    BLOCKED_NETWORK_CALLS++;
    errno = ENETDOWN;
    mr_sched_set_errno(c, ENETDOWN);
    RET(0);
}
static void s_inet_addr_offline(mr_cpu *c) {
    BLOCKED_NETWORK_CALLS++;
    errno = ENETDOWN;
    mr_sched_set_errno(c, ENETDOWN);
    RET(UINT32_MAX);
}
static void s_epoll_offline(mr_cpu *c) {
    fail_errno(c, ENOSYS);
}

#define EPOLL_RETRY_MS 2u

static void s_epoll_wait(mr_cpu *c) {
    if (mr_sched_sleep(c, EPOLL_RETRY_MS * 1000000ull)) return;
    fail_errno(c, ENOSYS);
}

static void s_getaddrinfo_offline(mr_cpu *c) {
    BLOCKED_NETWORK_CALLS++;
    if (getenv("MR_OFFLINE_LOG"))
        fprintf(stderr, "[offline] resolver import blocked at 0x%08x\n", c->r[MR_R_LR]);
    if (A3 && mr_mem_ok(c, A3, 4)) mr_st32(c, A3, 0);
    RET(8u); // EAI_NONAME is permanent and must not be retried.
}

static void s_freeaddrinfo_offline(mr_cpu *c) {
    RET(0);
}

static void s_errno(mr_cpu *c) {
    uint32_t slot = mr_sched_errno_addr(c);
    if (!slot) slot = ERRNO_SLOT;
    if (!c->scheduler && slot) mr_st32(c, slot, (uint32_t)errno);
    RET(slot);
}

static void s_sscanf(mr_cpu *c) {
    const char *in = GS(c, A0);
    const char *fmt = GS(c, A1);
    va_walk v = {c, 2, 0};
    int filled = 0;
    const char *p = in;

    for (const char *f = fmt; *f; f++) {
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*p))
                p++;
            continue;
        }
        if (*f != '%') {
            if (*p != *f) break;
            p++;
            continue;
        }
        f++;
        int suppress = 0, width = 0;
        if (*f == '*') {
            suppress = 1;
            f++;
        }
        while (isdigit((unsigned char)*f))
            width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'h' || *f == 'L')
            f++;

        if (*f != 'c')
            while (isspace((unsigned char)*p))
                p++;
        if (!*p) break;

        char buf[256];
        int n = 0;
        int cap = (width > 0 && width < (int)sizeof(buf) - 1) ? width : (int)sizeof(buf) - 1;

        switch (*f) {
        case 'd':
        case 'u':
        case 'x':
        case 'i': {
            char *end;
            long val = strtol(p, &end, *f == 'x' ? 16 : (*f == 'i' ? 0 : 10));
            if (end == p) {
                f = "";
                break;
            }
            p = end;
            if (!suppress) {
                mr_st32(c, va_next(&v), (uint32_t)val);
                filled++;
            }
            break;
        }
        case 'f':
        case 'e':
        case 'g': {
            char *end;
            float val = strtof(p, &end);
            if (end == p) {
                f = "";
                break;
            }
            p = end;
            if (!suppress) {
                uint32_t bits;
                memcpy(&bits, &val, 4);
                mr_st32(c, va_next(&v), bits);
                filled++;
            }
            break;
        }
        case 's': {
            while (*p && !isspace((unsigned char)*p) && n < cap)
                buf[n++] = *p++;
            buf[n] = 0;
            if (!suppress) {
                uint32_t dst = va_next(&v);
                void *d = GW(c, dst, (uint32_t)n + 1);
                if (!c->halted) memcpy(d, buf, (size_t)n + 1);
                filled++;
            }
            break;
        }
        case 'c': {
            int cnt = width > 0 ? width : 1;
            if (!suppress) {
                uint32_t dst = va_next(&v);
                void *d = GW(c, dst, (uint32_t)cnt);
                if (!c->halted) memcpy(d, p, (size_t)cnt);
                filled++;
            }
            p += cnt;
            break;
        }
        case '%':
            if (*p == '%')
                p++;
            else
                f = "";
            break;
        default:
            f = "";
            break;
        }
        if (!*f) break;
    }
    RET((uint32_t)filled);
}

#define MATHF1(nm, fn)                                                                             \
    static void nm(mr_cpu *c) {                                                                    \
        float f;                                                                                   \
        memcpy(&f, &c->r[0], 4);                                                                   \
        float r = fn(f);                                                                           \
        memcpy(&c->r[0], &r, 4);                                                                   \
    }
// clang-format off
MATHF1(s_sinf, sinf)
MATHF1(s_cosf, cosf)
MATHF1(s_tanf, tanf)
MATHF1(s_asinf, asinf)
MATHF1(s_acosf, acosf)
MATHF1(s_floorf, floorf)
MATHF1(s_ceilf, ceilf)
// clang-format on

#define MATHF2(nm, fn)                                                                             \
    static void nm(mr_cpu *c) {                                                                    \
        float a, b;                                                                                \
        memcpy(&a, &c->r[0], 4);                                                                   \
        memcpy(&b, &c->r[1], 4);                                                                   \
        float r = fn(a, b);                                                                        \
        memcpy(&c->r[0], &r, 4);                                                                   \
    }
// clang-format off
MATHF2(s_atan2f, atan2f)
MATHF2(s_powf, powf)
MATHF2(s_fmodf, fmodf)
MATHF1(s_atanf, atanf)
// clang-format on

#define MATHD2(nm, fn)                                                                             \
    static void nm(mr_cpu *c) {                                                                    \
        uint64_t x = ((uint64_t)c->r[1] << 32) | c->r[0];                                          \
        uint64_t y = ((uint64_t)c->r[3] << 32) | c->r[2];                                          \
        double a, b;                                                                               \
        memcpy(&a, &x, 8);                                                                         \
        memcpy(&b, &y, 8);                                                                         \
        double r = fn(a, b);                                                                       \
        uint64_t o;                                                                                \
        memcpy(&o, &r, 8);                                                                         \
        c->r[0] = (uint32_t)o;                                                                     \
        c->r[1] = (uint32_t)(o >> 32);                                                             \
    }
// clang-format off
MATHD2(s_pow, pow)
MATHD2(s_fmod, fmod)
MATHD2(s_atan2, atan2)
MATH1(s_sin, sin)
MATH1(s_cos, cos)
MATH1(s_tan, tan)
MATH1(s_exp, exp)
MATH1(s_log, log)
// clang-format on

static void s_fputs(mr_cpu *c) {
    FILE *f = file_of(A1);
    const char *s = GS(c, A0);
    if (!f) {
        RET(0xFFFFFFFFu);
        return;
    }
    if (is_std_out(A1))
        fprintf(f, "[guest] %s", s);
    else
        fputs(s, f);
    RET(0);
}

static void s_fputc(mr_cpu *c) {
    FILE *f = file_of(A1);
    if (f) fputc((int)A0, f);
    RET(A0);
}

static void s_fprintf(mr_cpu *c) {
    FILE *f = file_of(A0);
    va_walk v = {c, 2, 0};
    RET(guest_vformat_to_file(c, f, is_std_out(A0), GS(c, A1), &v));
}

static void s_vfprintf(mr_cpu *c) {
    FILE *f = file_of(A0);
    va_walk v = {c, 0, A2};
    RET(guest_vformat_to_file(c, f, is_std_out(A0), GS(c, A1), &v));
}

static void s_putchar(mr_cpu *c) {
    putchar((int)A0);
    RET(A0);
}
static void s_ungetc(mr_cpu *c) {
    FILE *f = file_of(A1);
    RET(f ? (uint32_t)ungetc((int)A0, f) : 0xFFFFFFFFu);
}

static void s_setvbuf(mr_cpu *c) {
    RET(0);
}

static void s_perror(mr_cpu *c) {
    const char *prefix = A0 ? GS(c, A0) : NULL;
    fprintf(stderr, "[guest] %s%s%s\n", prefix ? prefix : "", prefix && *prefix ? ": " : "",
            strerror(errno));
    RET(0);
}

#define WCHAR_SZ 4u

static void s_wcslen(mr_cpu *c) {
    uint32_t n = 0;
    while (mr_mem_ok(c, A0 + n * WCHAR_SZ, WCHAR_SZ) && mr_ld32(c, A0 + n * WCHAR_SZ))
        n++;
    RET(n);
}

static void s_wmemcpy(mr_cpu *c) {
    if (A2 > UINT32_MAX / WCHAR_SZ) {
        RET(A0);
        return;
    }
    uint32_t n = A2 * WCHAR_SZ;
    IF_EMPTY(n);
    void *d = GW(c, A0, n);
    void *v = GN(c, A1, n);
    if (!c->halted) memcpy(d, v, n);
    RET(A0);
}
static void s_wmemmove(mr_cpu *c) {
    if (A2 > UINT32_MAX / WCHAR_SZ) {
        RET(A0);
        return;
    }
    uint32_t n = A2 * WCHAR_SZ;
    IF_EMPTY(n);
    void *d = GW(c, A0, n);
    void *v = GN(c, A1, n);
    if (!c->halted) memmove(d, v, n);
    RET(A0);
}
static void s_wmemcmp(mr_cpu *c) {
    if (A2 > UINT32_MAX / WCHAR_SZ ||
        (A2 && (!mr_mem_ok(c, A0, A2 * WCHAR_SZ) || !mr_mem_ok(c, A1, A2 * WCHAR_SZ)))) {
        RET(0);
        return;
    }
    for (uint32_t i = 0; i < A2; i++) {
        uint32_t a = mr_ld32(c, A0 + i * WCHAR_SZ);
        uint32_t b = mr_ld32(c, A1 + i * WCHAR_SZ);
        if (c->halted) {
            RET(0);
            return;
        }
        if (a != b) {
            RET(a < b ? (uint32_t)-1 : 1u);
            return;
        }
    }
    RET(0);
}
static void s_wmemset(mr_cpu *c) {
    if (A2 > UINT32_MAX / WCHAR_SZ || (A2 && !mr_mem_ok(c, A0, A2 * WCHAR_SZ))) {
        RET(A0);
        return;
    }
    for (uint32_t i = 0; i < A2; i++)
        mr_st32(c, A0 + i * WCHAR_SZ, A1);
    RET(A0);
}
static void s_wmemchr(mr_cpu *c) {
    if (A2 > UINT32_MAX / WCHAR_SZ || (A2 && !mr_mem_ok(c, A0, A2 * WCHAR_SZ))) {
        RET(0);
        return;
    }
    for (uint32_t i = 0; i < A2; i++) {
        uint32_t value = mr_ld32(c, A0 + i * WCHAR_SZ);
        if (c->halted) {
            RET(0);
            return;
        }
        if (value == A1) {
            RET(A0 + i * WCHAR_SZ);
            return;
        }
    }
    RET(0);
}

#define MAX_DIRS 16
#define DIRENT_SZ 280u
static DIR *DIRS[MAX_DIRS];
static DIR *DIRS_BASE[MAX_DIRS];
#define DIR_SEEN_MAX 512u
static char *DIRS_SEEN[MAX_DIRS][DIR_SEEN_MAX];
static unsigned DIRS_SEEN_COUNT[MAX_DIRS];

static void dir_forget(int slot) {
    for (unsigned i = 0; i < DIRS_SEEN_COUNT[slot]; i++)
        free(DIRS_SEEN[slot][i]);
    DIRS_SEEN_COUNT[slot] = 0;
}

static int dir_already_seen(int slot, const char *name) {
    for (unsigned i = 0; i < DIRS_SEEN_COUNT[slot]; i++)
        if (strcmp(DIRS_SEEN[slot][i], name) == 0) return 1;
    return 0;
}

static void dir_remember(int slot, const char *name) {
    if (DIRS_SEEN_COUNT[slot] >= DIR_SEEN_MAX) return;
    char *copy = strdup(name);
    if (copy) DIRS_SEEN[slot][DIRS_SEEN_COUNT[slot]++] = copy;
}
static uint32_t DIRENT_BUF[MAX_DIRS]; // Guest address of the returned record.

static void s_opendir(mr_cpu *c) {
    char path[2048];
    map_path(GS(c, A0), path, sizeof(path));
    int slot = 0;
    for (int i = 1; i < MAX_DIRS; i++)
        if (!DIRS[i]) {
            slot = i;
            break;
        }
    if (!slot) {
        RET(0);
        return;
    }

    char base_path[2048];
    map_path_base_only(GS(c, A0), base_path, sizeof base_path);

    DIR *d = opendir(path);
    DIR *b = base_path[0] && strcmp(base_path, path) != 0 ? opendir(base_path) : NULL;
    if (!d && !b) {
        RET(0);
        return;
    }
    DIRS[slot] = d;
    DIRS_BASE[slot] = b;
    dir_forget(slot);
    if (!DIRENT_BUF[slot]) DIRENT_BUF[slot] = heap_alloc(c, DIRENT_SZ);
    RET((uint32_t)slot);
}

static void s_readdir(mr_cpu *c) {
    uint32_t h = A0;
    if (!h || h >= MAX_DIRS || (!DIRS[h] && !DIRS_BASE[h])) {
        RET(0);
        return;
    }

    struct dirent *e = DIRS[h] ? readdir(DIRS[h]) : NULL;
    if (e) {
        dir_remember((int)h, e->d_name);
    } else {
        while (DIRS_BASE[h] && (e = readdir(DIRS_BASE[h])) != NULL)
            if (!dir_already_seen((int)h, e->d_name)) break;
    }
    uint32_t buf = DIRENT_BUF[h];
    if (!e || !buf) {
        RET(0);
        return;
    }
    void *dst = GW(c, buf, DIRENT_SZ);
    if (c->halted) {
        RET(0);
        return;
    }
    memset(dst, 0, DIRENT_SZ);
    mr_st16(c, buf + 16, (uint16_t)DIRENT_SZ);        // d_reclen
    mr_st8(c, buf + 18, e->d_type);                   // d_type
    snprintf((char *)dst + 19, 256, "%s", e->d_name); // d_name
    RET(buf);
}

static void s_closedir(mr_cpu *c) {
    uint32_t h = A0;
    if (h && h < MAX_DIRS) {
        if (DIRS[h]) {
            closedir(DIRS[h]);
            DIRS[h] = NULL;
        }
        if (DIRS_BASE[h]) {
            closedir(DIRS_BASE[h]);
            DIRS_BASE[h] = NULL;
        }
        dir_forget((int)h);
    }
    RET(0);
}

#define TM_SZ 44u
static uint32_t TM_BUF, ASC_BUF;

static uint32_t tm_area(mr_cpu *c) {
    if (!TM_BUF) TM_BUF = heap_alloc(c, TM_SZ);
    return TM_BUF;
}

static void store_tm(mr_cpu *c, uint32_t p, const struct tm *t) {
    const int32_t v[9] = {t->tm_sec,  t->tm_min,  t->tm_hour, t->tm_mday, t->tm_mon,
                          t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst};
    for (int i = 0; i < 9; i++)
        mr_st32(c, p + (uint32_t)i * 4, (uint32_t)v[i]);
    mr_st32(c, p + 36, 0); // tm_gmtoff
    mr_st32(c, p + 40, 0);
}

static void s_localtime_r(mr_cpu *c) {
    time_t t = A0 ? (time_t)(int32_t)mr_ld32(c, A0) : 0;
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (A1) store_tm(c, A1, &tmv);
    RET(A1);
}

static void s_gmtime_r(mr_cpu *c) {
    time_t t = A0 ? (time_t)(int32_t)mr_ld32(c, A0) : 0;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    if (A1) store_tm(c, A1, &tmv);
    RET(A1);
}

static void s_localtime(mr_cpu *c) {
    time_t t = A0 ? (time_t)(int32_t)mr_ld32(c, A0) : 0;
    struct tm tmv;
    localtime_r(&t, &tmv);
    uint32_t p = tm_area(c);
    if (p) store_tm(c, p, &tmv);
    RET(p);
}

static void s_gmtime(mr_cpu *c) {
    time_t t = A0 ? (time_t)(int32_t)mr_ld32(c, A0) : 0;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    uint32_t p = tm_area(c);
    if (p) store_tm(c, p, &tmv);
    RET(p);
}

static void load_tm(mr_cpu *c, uint32_t p, struct tm *out) {
    memset(out, 0, sizeof(*out));
    if (!p) return;
    int *f = &out->tm_sec;
    for (int i = 0; i < 9; i++)
        f[i] = (int32_t)mr_ld32(c, p + (uint32_t)i * 4);
}

static void s_mktime(mr_cpu *c) {
    struct tm tmv;
    load_tm(c, A0, &tmv);
    RET((uint32_t)mktime(&tmv));
}

static void s_strftime(mr_cpu *c) {
    uint32_t dst = A0, cap = A1;
    if (!dst || !cap) {
        RET(0);
        return;
    }
    struct tm tmv;
    load_tm(c, A3, &tmv);

    char buf[512];
    size_t n = strftime(buf, sizeof(buf), GS(c, A2), &tmv);
    if (!n || n + 1 > cap) {
        RET(0);
        return;
    }
    void *d = GW(c, dst, (uint32_t)n + 1);
    if (!c->halted) memcpy(d, buf, n + 1);
    RET((uint32_t)n);
}

static void s_strptime(mr_cpu *c) {
    const char *text = GS(c, A0);
    const char *format = A1 ? GS(c, A1) : "";
    if (c->halted) {
        RET(0);
        return;
    }

    struct tm value;
    load_tm(c, A2, &value);
    if (strcmp(format, "%Y-%m-%d %H:%M:%SZ") == 0) value.tm_isdst = -1;
    char *end = strptime(text, format, &value);
    if (!end) {
        RET(0);
        return;
    }
    if (A2) store_tm(c, A2, &value);
    RET(A0 + (uint32_t)(end - text));
}

static void s_asctime(mr_cpu *c) {
    struct tm tmv;
    load_tm(c, A0, &tmv);
    char buf[32];
    if (!asctime_r(&tmv, buf)) snprintf(buf, sizeof(buf), "\n");
    if (!ASC_BUF) ASC_BUF = heap_alloc(c, 32);
    if (ASC_BUF) {
        void *d = GW(c, ASC_BUF, 32);
        if (!c->halted) snprintf((char *)d, 32, "%s", buf);
    }
    RET(ASC_BUF);
}

// System properties.

// bionic values: _SC_PAGESIZE 0x27, _SC_NPROCESSORS_CONF 0x60,
// _SC_NPROCESSORS_ONLN 0x61, _SC_PHYS_PAGES 0x62, _SC_AVPHYS_PAGES 0x63.
static void s_sysconf(mr_cpu *c) {
    switch (A0) {
    case 0x27:
        RET(4096);
        return; // Page size.
    case 0x60:
    case 0x61:
        RET(4);
        return; // Report four processors.
    case 0x62:
        RET(512u * 1024 * 1024 / 4096);
        return;
    case 0x63:
        RET(256u * 1024 * 1024 / 4096);
        return;
    case 0x0b:
        RET(1024);
        return; // _SC_OPEN_MAX
    default:
        RET(0);
        return;
    }
}

static void s_setrlimit(mr_cpu *c) {
    RET(0);
}

static uint32_t EXIDX_ADDR, EXIDX_COUNT;

void mr_shim_set_exidx(uint32_t addr, uint32_t count) {
    EXIDX_ADDR = addr;
    EXIDX_COUNT = count;
}

static void s_find_exidx(mr_cpu *c) {
    if (A1 && mr_mem_ok(c, A1, 4)) mr_st32(c, A1, EXIDX_COUNT);
    RET(EXIDX_ADDR);
}

static void s_mmap(mr_cpu *c) {
    uint32_t len = A1, flags = A3;
    uint32_t fd = mr_ld32(c, c->r[MR_R_SP]);
    uint32_t off = mr_ld32(c, c->r[MR_R_SP] + 4);
    if (!len) {
        RET(0xFFFFFFFFu);
        return;
    }

    uint32_t p = heap_alloc(c, len);
    if (!p) {
        RET(0xFFFFFFFFu);
        return;
    }
    void *dst = GW(c, p, len);
    if (c->halted) {
        RET(0xFFFFFFFFu);
        return;
    }
    memset(dst, 0, len);

    if (!(flags & 0x20)) {
        size_t done = 0;
        while (done < len) {
            ssize_t got =
                pread((int)fd, (unsigned char *)dst + done, len - done, (off_t)off + (off_t)done);
            if (got > 0) {
                done += (size_t)got;
                continue;
            }
            if (got == 0) break; // The zero-filled remainder is valid at EOF.
            if (errno == EINTR) continue;
            mr_sched_set_errno(c, errno);
            heap_free(c, p);
            RET(0xFFFFFFFFu);
            return;
        }
    }
    RET(p);
}

static void s_munmap(mr_cpu *c) {
    heap_free(c, A0);
    RET(0);
}

static const mr_shim SHIMS[] = {
    {"malloc", s_malloc},
    {"free", s_free},
    {"calloc", s_calloc},
    {"realloc", s_realloc},
    {"mmap", s_mmap},
    {"munmap", s_munmap},

    {"memcpy", s_memcpy},
    {"memmove", s_memmove},
    {"memset", s_memset},
    {"memcmp", s_memcmp},
    {"memchr", s_memchr},
    {"strlen", s_strlen},
    {"strcmp", s_strcmp},
    {"strncmp", s_strncmp},
    {"strcpy", s_strcpy},
    {"strncpy", s_strncpy},
    {"strcat", s_strcat},
    {"strchr", s_strchr},
    {"strrchr", s_strrchr},
    {"strstr", s_strstr},
    {"strdup", s_strdup},
    {"strpbrk", s_strpbrk},
    {"strcasecmp", s_strcasecmp},
    {"strncasecmp", s_strncasecmp},
    {"strtok", s_strtok},
    {"strtok_r", s_strtok_r},
    {"strerror", s_strerror},
    {"strerror_r", s_strerror_r},
    {"strncat", s_strncat},
    {"strlcat", s_strlcat},
    {"strcspn", s_strcspn},
    {"strcoll", s_strcoll},
    {"memmem", s_memmem},
    {"qsort", s_qsort},
    {"bsearch", s_bsearch},

    // Numeric conversions.
    {"atoi", s_atoi},
    {"atol", s_atol},
    {"atoll", s_atoll},
    {"strtol", s_strtol},
    {"strtoul", s_strtoul},
    {"strtod", s_strtod},
    {"strtoll", s_strtoll},

    // Formatting.
    {"printf", s_printf},
    {"sprintf", s_sprintf},
    {"snprintf", s_snprintf},
    {"vsprintf", s_vsprintf},
    {"vsnprintf", s_vsnprintf},
    {"sscanf", s_sscanf},
    {"puts", s_puts},
    {"vfprintf", s_vfprintf},
    {"putchar", s_putchar},
    {"__android_log_print", s_android_log},
    {"__android_log_vprint", s_android_log_v},

    {"__aeabi_atexit", s_atexit},
    {"abort", s_abort},
    {"__gnu_Unwind_Find_exidx", s_find_exidx},
    {"setjmp", s_setjmp},
    {"longjmp", s_longjmp},

    {"setlocale", s_setlocale},
    {"wctob", s_wctob},
    {"btowc", s_btowc},
    {"wctype", s_wctype},
    {"iswctype", s_iswctype},
    {"towlower", s_towlower},
    {"towupper", s_towupper},

    // Mathematics with softfp doubles in two core registers.
    {"sin", s_sin},
    {"cos", s_cos},
    {"tan", s_tan},
    {"exp", s_exp},
    {"log", s_log},
    {"pow", s_pow},
    {"fmod", s_fmod},
    {"atan2", s_atan2},
    {"floor", s_floor},
    {"ceil", s_ceil},
    {"acos", s_acos},
    {"asin", s_asin},
    {"atan", s_atan},
    {"sinh", s_sinh},
    {"cosh", s_cosh},
    {"tanh", s_tanh},
    {"log10", s_log10},
    {"frexp", s_frexp},
    {"ldexp", s_ldexp},
    {"modf", s_modf},
    {"sinf", s_sinf},
    {"cosf", s_cosf},
    {"tanf", s_tanf},
    {"asinf", s_asinf},
    {"acosf", s_acosf},
    {"atanf", s_atanf},
    {"atan2f", s_atan2f},
    {"powf", s_powf},
    {"fmodf", s_fmodf},
    {"floorf", s_floorf},
    {"ceilf", s_ceilf},

    {"lrand48", s_lrand48},
    {"srand48", s_srand},

    {"time", s_time},
    {"clock", s_clock},
    {"times", s_clock},
    {"clock_gettime", s_clock_gettime},
    {"gettimeofday", s_gettimeofday},
    {"difftime", s_difftime},
    {"localtime", s_localtime},
    {"localtime_r", s_localtime_r},
    {"gmtime", s_gmtime},
    {"gmtime_r", s_gmtime_r},
    {"mktime", s_mktime},
    {"asctime", s_asctime},
    {"strftime", s_strftime},
    {"strptime", s_strptime},

    // File streams.
    {"fopen", s_fopen},
    {"fclose", s_fclose},
    {"fread", s_fread},
    {"fwrite", s_fwrite_file},
    {"fseek", s_fseek},
    {"ftell", s_ftell},
    {"fflush", s_fflush},
    {"fgets", s_fgets},
    {"getc", s_fgetc},
    {"ungetc", s_ungetc},
    {"fputs", s_fputs},
    {"fputc", s_fputc},
    {"putc", s_fputc},
    {"fprintf", s_fprintf},
    {"setvbuf", s_setvbuf},
    {"perror", s_perror},

    {"open", s_open},
    {"read", s_read},
    {"write", s_write},
    {"close", s_close},
    {"writev", s_writev},
    {"pread", s_pread},
    {"lseek", s_lseek},
    {"lseek64", s_lseek64},
    {"stat", s_stat},
    {"fstat", s_fstat},
    {"remove", s_remove},
    {"unlink", s_remove},
    {"rename", s_rename},
    {"mkdir", s_mkdir},
    {"chmod", s_chmod},
    {"rmdir", s_rmdir},
    {"chdir", s_chdir},
    {"getcwd", s_getcwd},
    {"fsync", s_fsync},
    {"fcntl", s_fcntl},
    {"ioctl", s_ioctl},
    {"statfs", s_statfs},
    {"opendir", s_opendir},
    {"readdir", s_readdir},
    {"closedir", s_closedir},

    {"wcslen", s_wcslen},
    {"wmemchr", s_wmemchr},
    {"wmemcmp", s_wmemcmp},
    {"wmemcpy", s_wmemcpy},
    {"wmemmove", s_wmemmove},
    {"wmemset", s_wmemset},

    {"getpid", s_getpid},
    {"getenv", s_getenv},
    {"sysconf", s_sysconf},
    {"__errno", s_errno},
    {"setrlimit", s_setrlimit},
    {"getuid", s_getuid},
    {"geteuid", s_getuid},
    {"getgid", s_getuid},
    {"getegid", s_getuid},
    {"exit", s_exit},
    {"__stack_chk_fail", s_stack_chk_fail},

    {"socket", s_network_offline},
    {"accept", s_network_offline},
    {"bind", s_network_offline},
    {"connect", s_network_offline},
    {"gethostbyaddr", s_network_pointer_offline},
    {"gethostbyname", s_network_pointer_offline},
    {"gethostname", s_network_offline},
    {"getpeername", s_network_offline},
    {"getsockname", s_network_offline},
    {"getsockopt", s_network_offline},
    {"inet_addr", s_inet_addr_offline},
    {"inet_ntoa", s_network_pointer_offline},
    {"listen", s_network_offline},
    {"poll", s_network_offline},
    {"recv", s_network_offline},
    {"recvfrom", s_network_offline},
    {"recvmsg", s_network_offline},
    {"select", s_network_offline},
    {"send", s_network_offline},
    {"sendmsg", s_network_offline},
    {"sendto", s_network_offline},
    {"setsockopt", s_network_offline},
    {"shutdown", s_network_offline},
    {"pipe", s_pipe_offline},
    {"epoll_create", s_epoll_offline},
    {"epoll_ctl", s_epoll_offline},
    {"epoll_wait", s_epoll_wait},
    {"getaddrinfo", s_getaddrinfo_offline},
    {"freeaddrinfo", s_freeaddrinfo_offline},
};

mr_thunk_fn mr_shim_lookup(const char *name) {
    mr_thunk_fn fn = mr_thread_shim_lookup(name);
    if (fn) return fn;
    for (size_t i = 0; i < sizeof(SHIMS) / sizeof(SHIMS[0]); i++)
        if (strcmp(SHIMS[i].name, name) == 0) return SHIMS[i].fn;
    return NULL;
}
