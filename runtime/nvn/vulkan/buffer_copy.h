// VkBufferCopy regions must not be empty (VUID-VkBufferCopy-size-01988). Copies the non-empty ones of src[n] to out,
// returns how many.
#ifndef BUFFER_COPY_H
#define BUFFER_COPY_H
#include <stdint.h>

static inline uint32_t vk_copies_nonempty(const VkBufferCopy *src, uint32_t n, VkBufferCopy *out) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (src[i].size) out[k++] = src[i];
    return k;
}
#endif
