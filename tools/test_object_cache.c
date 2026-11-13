#include <assert.h>
#include <stdio.h>
#include "nvn/vulkan/object_cache.h"

int main(void) {
    VkObjectCache cache = {0};
    enum { N = 20000 };
    static int objects[N];
    // More distinct entries than both former fixed limits, including colliding buckets.
    for (int i = 0; i < N; i++) {
        uint64_t key = (uint64_t)(i + 1) << 16;
        VkObjectEntry *entry = vk_object_slot(&cache, key);
        assert(!entry->object);
        objects[i] = i;
        entry->object = &objects[i];
    }
    assert(cache.count == N);
    for (int i = N - 1; i >= 0; i--) {
        VkObjectEntry *entry = vk_object_slot(&cache, (uint64_t)(i + 1) << 16);
        assert(entry->object == &objects[i]);
        assert(*(int *)entry->object == i);
    }
    assert(cache.count == N);
    free(cache.entries);
    puts("PASS: 20000 cache entries, collisions, growth and stable object pointers");
    return 0;
}
