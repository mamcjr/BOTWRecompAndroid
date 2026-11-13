// SPDX-License-Identifier: GPL-3.0-or-later
// What the frame generation files (ported from Swap2-Native's dm/gfx/vk_android/lsfg, itself from Eden and lsfg-vk) took
// from Swap2-Native's Vulkan replay (vka.h), on BOTWRecompAndroid's device (../vk.h): the device and queue, a memory type,
// an image barrier and the log. The calls are volk's, as the rest of the renderer's.
#pragma once
#include <pthread.h>
#include <stdio.h>
#include "../vk.h"
#include "../../../rt.h"
#include "../../../platform.h"

#define VKA_DECL(f) PFN_##f f;
#define VRED(...) do { fprintf(stderr, "[lsfg] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define VLOG(...) VRED(__VA_ARGS__)

static inline void lsfg_queue_lock(void) {}    // (the queue: only the render thread submits, frame generation included)
static inline void lsfg_queue_unlock(void) {}

static inline uint32_t vka_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want && !(mp.memoryTypes[i].propertyFlags & avoid)) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static inline void vka_img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from,
                              .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                              .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
