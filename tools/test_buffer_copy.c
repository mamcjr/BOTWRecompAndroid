// Empty regions leave a buffer copy; the others stay in order, and none at all means no copy.
#include <stdio.h>
#include <vulkan/vulkan_core.h>
#include "vulkan/buffer_copy.h"

static int failed;
#define CHECK(c) do { if (c) printf("ok: %s\n", #c); else { printf("FAIL: %s\n", #c); failed = 1; } } while (0)

int main(void) {
    VkBufferCopy in[4] = {{0, 0, 0}, {16, 32, 256}, {1, 2, 0}, {64, 128, 512}}, out[4];
    uint32_t n = vk_copies_nonempty(in, 4, out);
    CHECK(n == 2);
    CHECK(out[0].srcOffset == 16 && out[0].dstOffset == 32 && out[0].size == 256);
    CHECK(out[1].srcOffset == 64 && out[1].dstOffset == 128 && out[1].size == 512);
    VkBufferCopy none[1] = {{4, 8, 0}};
    CHECK(vk_copies_nonempty(none, 1, out) == 0);
    VkBufferCopy full[2] = {{0, 0, 4}, {8, 8, 4}};
    CHECK(vk_copies_nonempty(full, 2, out) == 2);
    if (!failed) printf("test_buffer_copy: all passed\n");
    return failed;
}
