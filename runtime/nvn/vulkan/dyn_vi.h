// Dynamic vertex input descriptor builder for VK_EXT_vertex_input_dynamic_state.
// Included from runtime/nvn/vulkan/exec.c and tools/test_dyn_vertex_input.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(VK_VERSION_1_0)
#include <vulkan/vulkan_core.h>
#endif

#include "device_compat.h"

#ifndef ZERO_BINDING
#define ZERO_BINDING 16
#endif

typedef struct {
    uint64_t gen;
    uint32_t attr_mask, used_streams;
    uint32_t stride[17], divisor[17];
    uint32_t na, nb;
    VkVertexInputAttributeDescription2EXT va[32];
    VkVertexInputBindingDescription2EXT vb[17];
} VkDynVi;

static inline void vk_build_dyn_vi(uint32_t prog_attr_mask, int nattrib, const uint32_t *attrib,
                                   int nstream, const uint32_t (*stream)[2], bool have_divisor,
                                   VkFormat (*fmt_fn)(uint32_t), VkDynVi *out, uint64_t gen) {
    out->gen = gen;
    out->attr_mask = 0;
    out->used_streams = 0;
    memset(out->stride, 0, sizeof out->stride);
    memset(out->divisor, 0, sizeof out->divisor);
    out->na = 0;
    out->nb = 0;

    for (int i = 0; i < 32; i++) {
        if (!(prog_attr_mask & (1u << i))) continue;
        out->attr_mask |= 1u << i;
        uint32_t a = i < nattrib ? attrib[i] : 0x2Eu;
        uint32_t fmt = a & 0xFFu, strm = (a >> 8) & 0x1Fu, off = a >> 13;
        VkFormat vkfmt;
        uint32_t bind_slot, vk_off;
        if (i >= nattrib || fmt == 0 || strm >= 16) {
            vkfmt = VK_FORMAT_R32G32B32A32_SFLOAT;
            bind_slot = ZERO_BINDING;
            vk_off = 0;
        } else {
            vkfmt = fmt_fn(fmt);
            bind_slot = strm;
            vk_off = off;
        }
        out->used_streams |= 1u << bind_slot;
        out->va[out->na++] = (VkVertexInputAttributeDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
            .pNext = NULL,
            .location = (uint32_t)i,
            .binding = bind_slot,
            .format = vkfmt,
            .offset = vk_off,
        };
    }
    for (uint32_t s = 0; s < 17; s++) {
        if (!(out->used_streams & (1u << s))) continue;
        if (s < 16) {
            out->stride[s] = (int)s < nstream ? stream[s][0] : 0;
            out->divisor[s] = ((int)s < nstream && out->stride[s]) ? stream[s][1] : 0;
            if (out->divisor[s] > 1 && !have_divisor) out->divisor[s] = 1;
        }
        bool inst = out->divisor[s] != 0;
        out->vb[out->nb++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .pNext = NULL,
            .binding = s,
            .stride = out->stride[s],
            .inputRate = inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            .divisor = inst ? out->divisor[s] : 1u,
        };
    }
}

// Enable dynamic vertex input by default only on drivers verified to handle vkCmdSetVertexInputEXT reliably:
// Mesa Turnip and ARM proprietary Mali r49+ (Qualcomm's proprietary driver crashes / stalls; older Mali is excluded).
static inline bool vk_dyn_vi_driver_default(uint32_t driver_id, const char *driver_info) {
    if (driver_id == VK_DRIVER_ID_MESA_TURNIP) return true;
    if (driver_id == VK_DRIVER_ID_ARM_PROPRIETARY) return vk_arm_driver_major(driver_info) >= 49;
    return false;
}

static inline bool vk_dyn_vi_want(uint32_t driver_id, const char *driver_info, bool have_driver_props,
                                  const char *env_val, bool no_env) {
    if (no_env) return false;
    if (env_val && *env_val) return atoi(env_val) != 0;
    return have_driver_props && vk_dyn_vi_driver_default(driver_id, driver_info);
}
