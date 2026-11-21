// Dynamic rendering local read (VK_KHR_dynamic_rendering_local_read) gating and slot helpers.
// Included from runtime/nvn/vulkan/device.c, runtime/nvn/vulkan/exec.c, and tools/test_local_read.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "dyn_vi.h"

// Enable VK_KHR_dynamic_rendering_local_read by default only on verified drivers (ARM proprietary Mali r49+):
// replaces G-buffer feedback copies and render-pass breaks with tile-local subpassInput loads and BY_REGION barriers.
static inline bool vk_local_read_driver_default(uint32_t driver_id, const char *driver_info) {
    if (driver_id == VK_DRIVER_ID_ARM_PROPRIETARY) return vk_arm_driver_major(driver_info) >= 49;
    return false;
}

static inline bool vk_local_read_want(uint32_t driver_id, const char *driver_info, bool have_driver_props,
                                      const char *env_val, bool no_env) {
    if (no_env) return false;
    if (env_val && *env_val) return atoi(env_val) != 0;
    return have_driver_props && vk_local_read_driver_default(driver_id, driver_info);
}

// Look up the color attachment index (0..7) mapped to FS texture slot `tex_slot`, or -1 if none.
static inline int vk_local_read_slot_att(const uint8_t slots[8], const uint8_t atts[8], int tex_slot) {
    if (tex_slot < 0 || tex_slot >= 64) return -1;
    uint8_t want = (uint8_t)(tex_slot + 1);
    for (int i = 0; i < 8 && slots[i]; i++) {
        if (slots[i] == want) return (int)atts[i];
    }
    return -1;
}
