#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Cache generation 4: keep experimental optimization results separate from normal compilation.
static inline uint64_t vk_shader_source_hash_ex(const char *source, int stage, bool optimized, uint32_t post_flags) {
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)stage ^ 0x0400000000000000ull;
    h ^= optimized ? 0x9e3779b97f4a7c15ull : 0;
    if (post_flags) h ^= (uint64_t)post_flags * 0xbf58476d1ce4e5b9ull;
    for (const unsigned char *p = (const unsigned char *)source; *p; p++) h = (h ^ *p) * 0x100000001B3ull;
    return h;
}

static inline uint64_t vk_shader_source_hash(const char *source, int stage, bool optimized) {
    return vk_shader_source_hash_ex(source, stage, optimized, 0);
}

// Framing checks only, not a semantic SPIR-V validator. Never walk beyond a cached instruction's bounds.
static inline bool vk_shader_cache_framed(const uint32_t *words, size_t count) {
    if (!words || count <= 5 || count > 16u * 1024 * 1024 / 4) return false;
    if (words[0] != 0x07230203 || !words[3] || words[4]) return false;
    if (words[1] < 0x00010000 || words[1] > 0x00010600 || (words[1] & 0xff0000ffu)) return false;
    for (size_t i = 5; i < count;) {
        uint32_t length = words[i] >> 16;
        if (!length || length > count - i) return false;
        i += length;
    }
    return true;
}
