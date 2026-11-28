// Policy helpers for BC3 / BC5_UNORM -> ASTC 4x4 transcoding on BC-less GPUs.
// Included from runtime/nvn/vulkan/textures.c, runtime/nvn/vulkan/exec.c, and unit tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#if !defined(VK_VERSION_1_0)
#include <vulkan/vulkan_core.h>
#endif

enum {
    VK_BC_ASTC_NONE = 0,
    VK_BC_ASTC_RGBA = 1,
    VK_BC_ASTC_LA = 2,
};

static inline bool vk_format_is_astc_ldr(VkFormat f) {
    return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
}

// BC5_UNORM is encoded in ASTC CEM 4 (L, L, L, A) with logical R in ASTC L (R) and logical G in ASTC A.
// Expand VK_COMPONENT_SWIZZLE_IDENTITY by slot position before remapping logical (R,G,B,A) -> (R,A,ZERO,ONE).
static inline VkComponentSwizzle vk_bc5_la_remap_one(VkComponentSwizzle s, int slot) {
    if (s == VK_COMPONENT_SWIZZLE_IDENTITY) s = (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + slot);
    switch (s) {
    case VK_COMPONENT_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case VK_COMPONENT_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_A;
    case VK_COMPONENT_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_ZERO;
    case VK_COMPONENT_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_ONE;
    default: return s;
    }
}

static inline VkComponentMapping vk_bc5_la_remap_swizzle(VkComponentMapping sw) {
    return (VkComponentMapping){
        vk_bc5_la_remap_one(sw.r, 0),
        vk_bc5_la_remap_one(sw.g, 1),
        vk_bc5_la_remap_one(sw.b, 2),
        vk_bc5_la_remap_one(sw.a, 3),
    };
}
