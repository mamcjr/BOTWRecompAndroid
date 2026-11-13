#ifndef VK_OBJECT_CACHE_H
#define VK_OBJECT_CACHE_H

#include <stdint.h>
#include <stdlib.h>

// Render-thread lookup only. Objects live separately: growing the index must not move
// objects referenced by compile workers, pipeline keys or the previous draw.
typedef struct { uint64_t key; void *object; } VkObjectEntry;
typedef struct { VkObjectEntry *entries; size_t capacity, count; } VkObjectCache;

static inline size_t vk_object_bucket(uint64_t key, size_t capacity) {
    return (size_t)(key ^ (key >> 32)) & (capacity - 1);
}

static inline VkObjectEntry *vk_object_slot(VkObjectCache *cache, uint64_t key) {
    if (!cache->capacity || cache->count >= cache->capacity * 3 / 4) {
        size_t capacity = cache->capacity ? cache->capacity * 2 : 1024;
        if (capacity < cache->capacity || capacity > SIZE_MAX / sizeof(VkObjectEntry)) abort();
        VkObjectEntry *entries = calloc(capacity, sizeof *entries);
        if (!entries) abort();
        for (size_t i = 0; i < cache->capacity; i++) {
            VkObjectEntry entry = cache->entries[i];
            if (!entry.key) continue;
            size_t slot = vk_object_bucket(entry.key, capacity);
            while (entries[slot].key) slot = (slot + 1) & (capacity - 1);
            entries[slot] = entry;
        }
        free(cache->entries);
        cache->entries = entries;
        cache->capacity = capacity;
    }
    size_t slot = vk_object_bucket(key, cache->capacity);
    while (cache->entries[slot].key && cache->entries[slot].key != key)
        slot = (slot + 1) & (cache->capacity - 1);
    if (!cache->entries[slot].key) {
        cache->entries[slot].key = key;
        cache->count++;
    }
    return &cache->entries[slot];
}
#endif
