// zlib ABI bridge between 32-bit guest streams and the host library.

#include "shim_libc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define A0 (c->r[0])
#define A1 (c->r[1])
#define A2 (c->r[2])
#define A3 (c->r[3])
#define RET(x) (c->r[0] = (uint32_t)(x))

// Offsets in the 32-bit guest z_stream.
enum {
    Z_NEXT_IN = 0,
    Z_AVAIL_IN = 4,
    Z_TOTAL_IN = 8,
    Z_NEXT_OUT = 12,
    Z_AVAIL_OUT = 16,
    Z_TOTAL_OUT = 20,
    Z_MSG = 24,
    Z_STATE = 28,
    Z_DATA_TYPE = 44,
    Z_ADLER = 48,
};

#define MAX_STREAMS 16
#define GUEST_Z_STREAM_SIZE 56u
typedef struct {
    uint32_t guest;
    z_stream host;
    int inflate;
} zslot;
static zslot SLOTS[MAX_STREAMS];

static zslot *slot_find(uint32_t guest) {
    if (!guest) return NULL;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (SLOTS[i].guest == guest) return &SLOTS[i];
    return NULL;
}

static zslot *slot_new(uint32_t guest) {
    if (!guest) return NULL;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (!SLOTS[i].guest) {
            memset(&SLOTS[i], 0, sizeof(SLOTS[i]));
            SLOTS[i].guest = guest;
            return &SLOTS[i];
        }
    return NULL;
}

static zslot *slot_begin(uint32_t guest, int inflate) {
    zslot *s = slot_new(guest);
    if (!s) return NULL;
    memset(&s->host, 0, sizeof(s->host));
    s->inflate = inflate;
    return s;
}

static int load_stream(mr_cpu *c, zslot *s) {
    uint32_t g = s->guest;
    uint32_t in = mr_ld32(c, g + Z_NEXT_IN), in_n = mr_ld32(c, g + Z_AVAIL_IN);
    uint32_t out = mr_ld32(c, g + Z_NEXT_OUT), out_n = mr_ld32(c, g + Z_AVAIL_OUT);

    if (in_n && !mr_mem_ok(c, in, in_n)) return 0;
    if (out_n && !mr_mem_ok(c, out, out_n)) return 0;

    s->host.next_in = in_n ? (Bytef *)mr_mem(c, in) : NULL;
    s->host.avail_in = in_n;
    s->host.next_out = out_n ? (Bytef *)mr_mem(c, out) : NULL;
    s->host.avail_out = out_n;
    s->host.total_in = mr_ld32(c, g + Z_TOTAL_IN);
    s->host.total_out = mr_ld32(c, g + Z_TOTAL_OUT);
    return 1;
}

// Store host state while converting pointers back to guest addresses.
static void store_stream(mr_cpu *c, zslot *s) {
    uint32_t g = s->guest;
    uint32_t in = mr_ld32(c, g + Z_NEXT_IN);
    uint32_t in_n = mr_ld32(c, g + Z_AVAIL_IN);
    uint32_t out = mr_ld32(c, g + Z_NEXT_OUT);
    uint32_t out_n = mr_ld32(c, g + Z_AVAIL_OUT);

    mr_st32(c, g + Z_NEXT_IN, in + (in_n - s->host.avail_in));
    mr_st32(c, g + Z_AVAIL_IN, s->host.avail_in);
    mr_st32(c, g + Z_NEXT_OUT, out + (out_n - s->host.avail_out));
    mr_st32(c, g + Z_AVAIL_OUT, s->host.avail_out);
    mr_st32(c, g + Z_TOTAL_IN, (uint32_t)s->host.total_in);
    mr_st32(c, g + Z_TOTAL_OUT, (uint32_t)s->host.total_out);
    mr_st32(c, g + Z_ADLER, (uint32_t)s->host.adler);
    mr_st32(c, g + Z_DATA_TYPE, (uint32_t)s->host.data_type);
    // A host message pointer cannot enter guest memory.
    mr_st32(c, g + Z_MSG, 0);
    mr_st32(c, g + Z_STATE, g);
}

static void s_deflateInit(mr_cpu *c) {
    if (!mr_mem_ok(c, A0, GUEST_Z_STREAM_SIZE) || slot_find(A0)) {
        RET(Z_STREAM_ERROR);
        return;
    }
    zslot *s = slot_begin(A0, 0);
    if (!s) {
        RET(Z_MEM_ERROR);
        return;
    }
    int rc = deflateInit(&s->host, (int)A1);
    if (rc == Z_OK)
        store_stream(c, s);
    else
        s->guest = 0;
    RET(rc);
}

static void s_inflateInit(mr_cpu *c) {
    if (!mr_mem_ok(c, A0, GUEST_Z_STREAM_SIZE) || slot_find(A0)) {
        RET(Z_STREAM_ERROR);
        return;
    }
    zslot *s = slot_begin(A0, 1);
    if (!s) {
        RET(Z_MEM_ERROR);
        return;
    }
    int rc = inflateInit(&s->host);
    if (rc == Z_OK)
        store_stream(c, s);
    else
        s->guest = 0;
    RET(rc);
}

// inflateInit2_(strm, windowBits, version, stream_size)
static void s_inflateInit2(mr_cpu *c) {
    if (!mr_mem_ok(c, A0, GUEST_Z_STREAM_SIZE) || slot_find(A0)) {
        RET(Z_STREAM_ERROR);
        return;
    }
    zslot *s = slot_begin(A0, 1);
    if (!s) {
        RET(Z_MEM_ERROR);
        return;
    }
    int rc = inflateInit2(&s->host, (int)A1);
    if (rc == Z_OK)
        store_stream(c, s);
    else
        s->guest = 0;
    RET(rc);
}

static void s_deflate(mr_cpu *c) {
    zslot *s = slot_find(A0);
    if (!s) {
        RET(Z_STREAM_ERROR);
        return;
    }
    if (!load_stream(c, s)) {
        RET(Z_BUF_ERROR);
        return;
    }
    int rc = deflate(&s->host, (int)A1);
    store_stream(c, s);
    RET(rc);
}

static void s_inflate(mr_cpu *c) {
    zslot *s = slot_find(A0);
    if (!s) {
        RET(Z_STREAM_ERROR);
        return;
    }
    if (!load_stream(c, s)) {
        RET(Z_BUF_ERROR);
        return;
    }
    int rc = inflate(&s->host, (int)A1);
    store_stream(c, s);
    RET(rc);
}

static void s_deflateEnd(mr_cpu *c) {
    zslot *s = slot_find(A0);
    if (!s) {
        RET(Z_STREAM_ERROR);
        return;
    }
    int rc = deflateEnd(&s->host);
    s->guest = 0;
    RET(rc);
}

static void s_inflateEnd(mr_cpu *c) {
    zslot *s = slot_find(A0);
    if (!s) {
        RET(Z_STREAM_ERROR);
        return;
    }
    int rc = inflateEnd(&s->host);
    s->guest = 0;
    RET(rc);
}

// One-shot operations.

// compress(dest, destLen*, source, sourceLen)
static void s_compress(mr_cpu *c) {
    uint32_t dlen_p = A1, src = A2, slen = A3;
    if (!mr_mem_ok(c, dlen_p, 4)) {
        RET(Z_BUF_ERROR);
        return;
    }
    uLongf dlen = mr_ld32(c, dlen_p);
    if (!mr_mem_ok(c, A0, (uint32_t)dlen) || (slen && !mr_mem_ok(c, src, slen))) {
        RET(Z_BUF_ERROR);
        return;
    }
    int rc =
        compress((Bytef *)mr_mem(c, A0), &dlen, slen ? (const Bytef *)mr_mem(c, src) : NULL, slen);
    if (rc == Z_OK) mr_st32(c, dlen_p, (uint32_t)dlen);
    RET(rc);
}

static void s_uncompress(mr_cpu *c) {
    uint32_t dlen_p = A1, src = A2, slen = A3;
    if (!mr_mem_ok(c, dlen_p, 4)) {
        RET(Z_BUF_ERROR);
        return;
    }
    uLongf dlen = mr_ld32(c, dlen_p);
    if (!mr_mem_ok(c, A0, (uint32_t)dlen) || (slen && !mr_mem_ok(c, src, slen))) {
        RET(Z_BUF_ERROR);
        return;
    }
    int rc = uncompress((Bytef *)mr_mem(c, A0), &dlen, slen ? (const Bytef *)mr_mem(c, src) : NULL,
                        slen);
    if (rc == Z_OK) mr_st32(c, dlen_p, (uint32_t)dlen);
    RET(rc);
}

static void s_compressBound(mr_cpu *c) {
    RET((uint32_t)compressBound(A0));
}

static void s_crc32(mr_cpu *c) {
    uint32_t buf = A1, len = A2;
    if (!len || !buf) {
        RET((uint32_t)crc32(A0, NULL, 0));
        return;
    }
    if (!mr_mem_ok(c, buf, len)) {
        RET(A0);
        return;
    }
    RET((uint32_t)crc32(A0, (const Bytef *)mr_mem(c, buf), len));
}

static uint32_t VERSION_STR;
void mr_zlib_set_area(mr_cpu *c, uint32_t base) {
    VERSION_STR = base;
    const char *v = zlibVersion();
    memcpy(mr_mem(c, base), v, strlen(v) + 1);
}

static void s_zlibVersion(mr_cpu *c) {
    RET(VERSION_STR);
}

#define ZLIB_ERROR_TEXT 32u
static void s_zError(mr_cpu *c) {
    if (!VERSION_STR) {
        RET(0);
        return;
    }
    uint32_t slot = VERSION_STR + ZLIB_ERROR_TEXT;
    if (!mr_mem_ok(c, slot, ZLIB_ERROR_TEXT)) {
        RET(0);
        return;
    }
    snprintf((char *)mr_mem(c, slot), ZLIB_ERROR_TEXT, "%s", zError((int)(int32_t)A0));
    RET(slot);
}

static const mr_shim SHIMS[] = {
    {"deflateInit_", s_deflateInit},
    {"deflate", s_deflate},
    {"deflateEnd", s_deflateEnd},
    {"inflateInit_", s_inflateInit},
    {"inflate", s_inflate},
    {"inflateEnd", s_inflateEnd},
    {"inflateInit2_", s_inflateInit2},
    {"uncompress", s_uncompress},
    {"compressBound", s_compressBound},
    {"compress", s_compress},
    {"crc32", s_crc32},
    {"zlibVersion", s_zlibVersion},
    {"zError", s_zError},
};

mr_thunk_fn mr_zlib_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(SHIMS) / sizeof(SHIMS[0]); i++)
        if (strcmp(SHIMS[i].name, name) == 0) return SHIMS[i].fn;
    return NULL;
}
