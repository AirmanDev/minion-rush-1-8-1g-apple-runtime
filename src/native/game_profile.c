// Runtime-only loading of host-compiled, user-specific ARM64 metadata.
// No instruction translation or guest-code generation occurs on the device.
#include "game_profile.h"
#include "game_profile_limits.h"

#include <CommonCrypto/CommonDigest.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROFILE_HEADER_BYTES 76u
#define MAX_PROFILE_BYTES (24u * 1024u * 1024u)
#define MAX_BLOCK_COUNT MR_AOT_MAX_BLOCKS
#define MAX_BLOCKED_COUNT MR_AOT_MAX_BLOCKED
#define MAX_HOOK_COUNT MR_AOT_MAX_HOOKS
#define MAX_CODE_SIZE MR_AOT_CODE_SLOT_BYTES

uint32_t mr_profile_bindings[MR_NATIVE_PROFILE_SYMBOL_COUNT];
uint32_t *mr_profile_blocked;
uint32_t *mr_profile_hooks;
mr_profile_block *mr_profile_blocks;
uint32_t mr_profile_blocked_count;
uint32_t mr_profile_hook_count;
uint32_t mr_profile_block_count;
uint32_t mr_profile_exact_blocked_count;
uint32_t mr_profile_code_size;
uint64_t mr_profile_fingerprint;

_Static_assert(sizeof(mr_profile_block) == 12, "invalid binary AOT block ABI");

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8u | (uint32_t)p[2] << 16u | (uint32_t)p[3] << 24u;
}

static uint64_t u64(const uint8_t *p) {
    return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32u;
}

static int ordered(uint32_t value, uint32_t *last, int *set) {
    if (*set && value <= *last) return 0;
    *last = value;
    *set = 1;
    return 1;
}

int mr_game_profile_load(const char *path) {
    if (!path || mr_profile_blocks || mr_profile_fingerprint) return -1;
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    int result = -1;
    uint8_t header[PROFILE_HEADER_BYTES];
    uint8_t *payload = NULL;
    mr_profile_block *blocks = NULL;
    uint32_t *blocked = NULL;
    uint32_t *hooks = NULL;
    do {
        if (fread(header, 1, sizeof header, file) != sizeof header ||
            memcmp(header, "MRPROF3\0", 8) || u32(header + 8) != 3u)
            break;
        uint64_t fingerprint = u64(header + 12);
        uint32_t code_size = u32(header + 20);
        uint32_t bindings_n = u32(header + 24);
        uint32_t block_n = u32(header + 28);
        uint32_t blocked_n = u32(header + 32);
        uint32_t hook_n = u32(header + 36);
        uint32_t exact_n = u32(header + 40);
        if (!fingerprint || bindings_n != MR_NATIVE_PROFILE_SYMBOL_COUNT || code_size < 64u ||
            code_size > MAX_CODE_SIZE || (code_size & 3u) || !block_n ||
            block_n > MAX_BLOCK_COUNT || !blocked_n || blocked_n > MAX_BLOCKED_COUNT ||
            hook_n <= blocked_n || hook_n > MAX_HOOK_COUNT || exact_n >= blocked_n)
            break;
        size_t size =
            (size_t)bindings_n * 4u + (size_t)block_n * 12u + ((size_t)blocked_n + hook_n) * 4u;
        if (size > MAX_PROFILE_BYTES) break;
        payload = malloc(size);
        blocks = calloc(block_n, sizeof *blocks);
        blocked = calloc(blocked_n, sizeof *blocked);
        hooks = calloc(hook_n, sizeof *hooks);
        if (!payload || !blocks || !blocked || !hooks || fread(payload, 1, size, file) != size ||
            fgetc(file) != EOF)
            break;
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(payload, (CC_LONG)size, digest);
        if (memcmp(digest, header + 44, CC_SHA256_DIGEST_LENGTH)) break;
        const uint8_t *cursor = payload;
        uint32_t binding_values[MR_NATIVE_PROFILE_SYMBOL_COUNT];
        for (uint32_t i = 0; i < bindings_n; ++i, cursor += 4)
            binding_values[i] = u32(cursor);
        uint32_t last_pc[2] = {0, 0};
        int set_pc[2] = {0, 0};
        for (uint32_t i = 0; i < block_n; ++i, cursor += 12) {
            uint32_t pc = u32(cursor);
            uint32_t offset = u32(cursor + 4);
            uint8_t thumb = cursor[8];
            if (thumb > 1u || cursor[9] || cursor[10] || cursor[11] || (offset & 3u) ||
                offset >= code_size || !ordered(pc, &last_pc[thumb], &set_pc[thumb]))
                goto done;
            blocks[i].pc = pc;
            blocks[i].offset = offset;
            blocks[i].thumb = thumb;
        }
        uint32_t last = 0;
        int seen = 0;
        for (uint32_t i = 0; i < blocked_n; ++i, cursor += 4) {
            uint32_t v = u32(cursor);
            if (!ordered(v, &last, &seen)) goto done;
            blocked[i] = v;
        }
        last = 0;
        seen = 0;
        for (uint32_t i = 0; i < hook_n; ++i, cursor += 4) {
            uint32_t v = u32(cursor);
            if (!ordered(v, &last, &seen)) goto done;
            hooks[i] = v;
        }
        memcpy(mr_profile_bindings, binding_values, sizeof binding_values);
        mr_profile_blocks = blocks;
        mr_profile_blocked = blocked;
        mr_profile_hooks = hooks;
        mr_profile_block_count = block_n;
        mr_profile_blocked_count = blocked_n;
        mr_profile_hook_count = hook_n;
        mr_profile_exact_blocked_count = exact_n;
        mr_profile_code_size = code_size;
        mr_profile_fingerprint = fingerprint;
        blocks = NULL;
        blocked = NULL;
        hooks = NULL;
        result = 0;
    } while (0);
done:
    free(payload);
    free(blocks);
    free(blocked);
    free(hooks);
    fclose(file);
    return result;
}
