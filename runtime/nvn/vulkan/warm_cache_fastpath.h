// Policy and bounded per-frame budget helpers for inline warm shader/pipeline cache hits.
// Included from runtime/nvn/vulkan/device.c, runtime/nvn/vulkan/exec.c, and tools/test_warm_cache_fastpath.c.
#pragma once

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#if !defined(VK_VERSION_1_0)
#include <vulkan/vulkan_core.h>
#endif

#include "dyn_vi.h"

enum {
    VK_WARM_INLINE_DEFAULT_30_US = 4000,
    VK_WARM_INLINE_DEFAULT_45_US = 3000,
    VK_WARM_INLINE_DEFAULT_60_US = 2000,
    VK_WARM_INLINE_MAX_US = 20000,
};

// Default to enabled only on verified proprietary Mali r49+ drivers where Vulkan 1.3 cache-control queries succeed.
static inline bool vk_pipe_cache_ctrl_driver_default(uint32_t driver_id, const char *driver_info) {
    if (driver_id == VK_DRIVER_ID_ARM_PROPRIETARY) return vk_arm_driver_major(driver_info) >= 49;
    return false;
}

static inline bool vk_pipe_cache_ctrl_want(uint32_t driver_id, const char *driver_info, bool have_driver_props,
                                           const char *env_val, bool no_env) {
    if (no_env) return false;
    if (env_val && *env_val) return atoi(env_val) != 0;
    return have_driver_props && vk_pipe_cache_ctrl_driver_default(driver_id, driver_info);
}

static inline bool vk_pipe_cache_ctrl_select(uint32_t api_version, VkBool32 have_v13_feature, uint32_t driver_id,
                                             const char *driver_info, bool have_driver_props, const char *env_val,
                                             bool no_env, bool no_pcache, VkBool32 *out_v13_enable,
                                             const char **out_why) {
    bool have_pcc = api_version >= VK_API_VERSION_1_3 && have_v13_feature != 0;
    bool want_pcc = vk_pipe_cache_ctrl_want(driver_id, driver_info, have_driver_props, env_val, no_env);
    bool enabled = want_pcc && have_pcc && !no_pcache;
    if (out_v13_enable) *out_v13_enable = (api_version >= VK_API_VERSION_1_3 && enabled) ? VK_TRUE : VK_FALSE;
    if (out_why) {
        *out_why = no_env ? "LARECOMP_VK_NO_CACHE_CTRL"
            : (env_val && *env_val && !want_pcc) ? "LARECOMP_VK_CACHE_CTRL=0"
            : !want_pcc ? "off by default"
            : no_pcache ? "pipeline cache disabled"
            : !have_pcc ? "unsupported"
            : (env_val && *env_val) ? "LARECOMP_VK_CACHE_CTRL" : "driver default";
    }
    return enabled;
}

static inline int vk_warm_inline_default_us(int target_fps) {
    if (target_fps > 0 && target_fps <= 30) return VK_WARM_INLINE_DEFAULT_30_US;
    if (target_fps > 30 && target_fps <= 45) return VK_WARM_INLINE_DEFAULT_45_US;
    return VK_WARM_INLINE_DEFAULT_60_US;
}

static inline bool vk_warm_inline_parse_env_ns(const char *env_val, uint64_t *out_ns) {
    if (!env_val || !*env_val) return false;
    char *end = NULL;
    errno = 0;
    long us = strtol(env_val, &end, 10);
    if (end == env_val) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n' || *end == '\v' || *end == '\f') end++;
    if (*end != '\0') return false;
    uint64_t ns;
    if (errno == ERANGE) ns = us == LONG_MIN ? 0 : (uint64_t)VK_WARM_INLINE_MAX_US * 1000ull;
    else if (us <= 0) ns = 0;
    else {
        if (us > VK_WARM_INLINE_MAX_US) us = VK_WARM_INLINE_MAX_US;
        ns = (uint64_t)us * 1000ull;
    }
    if (out_ns) *out_ns = ns;
    return true;
}

static inline uint64_t vk_warm_inline_budget_ns(const char *env_val, bool enabled, int target_fps) {
    if (!enabled) return 0;
    uint64_t ns = 0;
    if (vk_warm_inline_parse_env_ns(env_val, &ns)) return ns;
    return (uint64_t)vk_warm_inline_default_us(target_fps) * 1000ull;
}

typedef struct {
    uint64_t frame;
    uint64_t spent_ns;
    uint64_t limit_ns;
} VkWarmInlineBudget;

static inline bool vk_warm_budget_allow(VkWarmInlineBudget *b, uint64_t frame) {
    if (!b || b->limit_ns == 0) return false;
    if (b->frame != frame) {
        b->frame = frame;
        b->spent_ns = 0;
    }
    return b->spent_ns < b->limit_ns;
}

static inline void vk_warm_budget_account(VkWarmInlineBudget *b, uint64_t frame, uint64_t dt_ns) {
    if (!b) return;
    if (b->frame != frame) {
        b->frame = frame;
        b->spent_ns = 0;
    }
    if (dt_ns > UINT64_MAX - b->spent_ns) b->spent_ns = UINT64_MAX;
    else b->spent_ns += dt_ns;
}

static inline void vk_warm_budget_reset(VkWarmInlineBudget *b) {
    if (!b) return;
    b->frame = ~0ull;
    b->spent_ns = 0;
}
