// LARECOMP_MEMFD_PROBE=1 (a test, Android): can this driver give the guest heap as GPU memory through an exported file
// (VK_KHR_external_memory_fd: a dma-buf mapped at the heap's own address, exec.c vk_heap_place - after
// legendszaonandroid's VULKAN_BACKEND.md, MIT), where it has neither placed maps nor the host import (Mali, Xclipse)?
// For each host-visible memory type: 64 MB exported, its file mapped, the CPU's write and read speed through that map
// (the game runs on it: an uncached map crawls, as Mali's coherent type did in 0.2.3), and whether the GPU sees what the
// CPU wrote and the CPU what the GPU wrote. Once, at the device's start, before the game; the log says it all.
#ifdef __ANDROID__
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "vk.h"

#define PROBE_SIZE (64u << 20)

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// The GPU copies src[0, 4 KB) to dst and fills dst[4 KB, 8 KB) with 0x5a: true once done
static bool gpu_round(VkBuffer src, VkBuffer dst) {
    VkCommandPool pool;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_vk.qfam};
    if (vkCreateCommandPool(g_vk.dev, &pi, NULL, &pool) != VK_SUCCESS) return false;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    bool ok = vkAllocateCommandBuffers(g_vk.dev, &ai, &cb) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        VkBufferCopy cp = {0, 0, 4096};
        vkCmdCopyBuffer(cb, src, dst, 1, &cp);
        vkCmdFillBuffer(cb, dst, 4096, 4096, 0x5a5a5a5au);
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkEndCommandBuffer(cb);
        VkFence fence;
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        ok = vkCreateFence(g_vk.dev, &fi, NULL, &fence) == VK_SUCCESS;
        if (ok) {
            VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
            ok = vkQueueSubmit(g_vk.queue, 1, &si, fence) == VK_SUCCESS &&
                 vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
            vkDestroyFence(g_vk.dev, fence, NULL);
        }
    }
    vkDestroyCommandPool(g_vk.dev, pool, NULL);
    return ok;
}

static void probe_type(uint32_t type, VkMemoryPropertyFlags flags, VkExternalMemoryHandleTypeFlagBits ht) {
    const char *kind = ht == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ? "dma-buf" : "opaque";
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = ht};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = PROBE_SIZE,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buf = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    int fd = -1;
    uint8_t *p = MAP_FAILED;
    const char *why = NULL;
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &buf) != VK_SUCCESS) { why = "buffer"; goto done; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, buf, &mr);
    if (!(mr.memoryTypeBits & (1u << type))) { why = "the buffer can't use this type"; goto done; }
    VkExportMemoryAllocateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                     .handleTypes = ht};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ex, .allocationSize = mr.size,
                                .memoryTypeIndex = type};
    VkResult r = vkAllocateMemory(g_vk.dev, &mai, NULL, &mem);
    if (r != VK_SUCCESS) { why = "allocation"; goto done; }
    if (vkBindBufferMemory(g_vk.dev, buf, mem, 0) != VK_SUCCESS) { why = "bind"; goto done; }
    VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem,
                               .handleType = ht};
    if (vkGetMemoryFdKHR(g_vk.dev, &gi, &fd) != VK_SUCCESS || fd < 0) { why = "no file for it"; goto done; }
    p = mmap(NULL, PROBE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { why = "mmap of the file"; goto done; }
    // the CPU through the file's map: writes, then reads (what the game's code does all frame long)
    uint8_t *tmp = malloc(PROBE_SIZE);
    if (!tmp) { why = "memory"; goto done; }
    memset(tmp, 0x11, PROBE_SIZE);
    double t0 = now_s();
    for (int i = 0; i < 4; i++) memcpy(p, tmp, PROBE_SIZE);
    double t1 = now_s();
    volatile uint64_t sum = 0;
    for (int i = 0; i < 4; i++) {
        memcpy(tmp, p, PROBE_SIZE);
        sum += tmp[i * 4096];
    }
    double t2 = now_s();
    free(tmp);
    double wr = 4.0 * PROBE_SIZE / (t1 - t0) / 1e9, rd = 4.0 * PROBE_SIZE / (t2 - t1) / 1e9;
    // the GPU: sees the CPU's pattern (copied back out), and its own fill reaches the CPU's map
    // (the file's map isn't a vkMapMemory one, so nothing is flushed by hand: the result shows what the GPU gets as the
    // game would leave it. The copy and the fill land in the same memory: [0, 4 KB) to [8 MB, +4 KB), the fill after)
    for (int i = 0; i < 4096; i++) p[i] = (uint8_t)(i * 7 + 3);
    bool gpu_ok = false, cpu_sees = false, gpu_sees = false;
    {
        VkBufferCreateInfo vbi = bci;  // (a second buffer on the same memory at 8 MB: the copy's target)
        vbi.size = 1u << 20;
        if (vkCreateBuffer(g_vk.dev, &vbi, NULL, &out) == VK_SUCCESS && vkBindBufferMemory(g_vk.dev, out, mem, 8u << 20) == VK_SUCCESS)
            gpu_ok = gpu_round(buf, out);
    }
    if (gpu_ok) {
        gpu_sees = true;
        for (int i = 0; i < 4096 && gpu_sees; i++) gpu_sees = p[(8u << 20) + i] == (uint8_t)(i * 7 + 3);
        cpu_sees = true;
        for (int i = 0; i < 4096 && cpu_sees; i++) cpu_sees = p[(8u << 20) + 4096 + i] == 0x5a;
    }
    LOG("[memfd] %s, type %u (flags 0x%x%s%s): exported and mapped; CPU writes %.1f GB/s, reads %.1f GB/s; the GPU %s the CPU's "
        "bytes, the CPU %s the GPU's\n", kind, type, (unsigned)flags, flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " cached" : "",
        flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " coherent" : "", wr, rd,
        !gpu_ok ? "(submission failed)" : gpu_sees ? "sees" : "does NOT see", !gpu_ok ? "-" : cpu_sees ? "sees" : "does NOT see");
done:
    if (why) LOG("[memfd] %s, type %u (flags 0x%x): failed at %s\n", kind, type, (unsigned)flags, why);
    if (p != MAP_FAILED) munmap(p, PROBE_SIZE);
    if (fd >= 0) close(fd);
    if (out) vkDestroyBuffer(g_vk.dev, out, NULL);
    if (buf) vkDestroyBuffer(g_vk.dev, buf, NULL);
    if (mem) vkFreeMemory(g_vk.dev, mem, NULL);
}

void vk_memfd_probe(void) {
    if (!getenv("LARECOMP_MEMFD_PROBE")) return;
    if (!g_vk.mem_fd) { LOG("[memfd] no VK_KHR_external_memory_fd on this driver\n"); return; }
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        probe_type(i, f, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
        if (g_vk.mem_dmabuf) probe_type(i, f, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);  // (the other file kind, #234)
    }
}
#endif
