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
    VK_WARM_INLINE_DEFAULT_US = 10000,
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

static inline uint64_t vk_warm_inline_budget_ns(const char *env_val, bool enabled) {
    if (!enabled) return 0;
    if (env_val && *env_val) {
        char *end = NULL;
        errno = 0;
        long us = strtol(env_val, &end, 10);
        if (end == env_val) return (uint64_t)VK_WARM_INLINE_DEFAULT_US * 1000ull;
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
        if (*end != '\0') return (uint64_t)VK_WARM_INLINE_DEFAULT_US * 1000ull;
        if (errno == ERANGE) return us == LONG_MIN ? 0 : (uint64_t)VK_WARM_INLINE_MAX_US * 1000ull;
        if (us <= 0) return 0;
        if (us > VK_WARM_INLINE_MAX_US) us = VK_WARM_INLINE_MAX_US;
        return (uint64_t)us * 1000ull;
    }
    return (uint64_t)VK_WARM_INLINE_DEFAULT_US * 1000ull;
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
