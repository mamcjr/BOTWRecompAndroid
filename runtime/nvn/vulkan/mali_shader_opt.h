// Policy helpers for Mali shader and SPIR-V cleanup optimizations (Phase 1).
// Included from device.c and unit tests.
#pragma once

#include <stdbool.h>

#include "device_compat.h"

static inline bool vk_mali_spirv_cleanup_want(bool has_bc, bool has_astc_ldr, const char *env, const char **why_out) {
    if (has_bc || !has_astc_ldr) {
        if (why_out) *why_out = has_bc ? "BC supported (!BC && ASTC gate)" : "no ASTC_LDR (!BC && ASTC gate)";
        return false;
    }
    bool env_val = false;
    if (vk_compat_parse_env_bool(env, &env_val)) {
        if (why_out) *why_out = env_val ? "LARECOMP_MALI_SPIRV_CLEANUP=1" : "LARECOMP_MALI_SPIRV_CLEANUP=0";
        return env_val;
    }
    if (why_out) *why_out = "!BC && ASTC default";
    return true;
}

static inline bool vk_mali_discard_opt_want(bool has_bc, bool has_astc_ldr, const char *env, const char **why_out) {
    if (has_bc || !has_astc_ldr) {
        if (why_out) *why_out = has_bc ? "BC supported (!BC && ASTC gate)" : "no ASTC_LDR (!BC && ASTC gate)";
        return false;
    }
    bool env_val = false;
    if (vk_compat_parse_env_bool(env, &env_val)) {
        if (why_out) *why_out = env_val ? "LARECOMP_MALI_DISCARD_OPT=1" : "LARECOMP_MALI_DISCARD_OPT=0";
        return env_val;
    }
    if (why_out) *why_out = "!BC && ASTC default";
    return true;
}
