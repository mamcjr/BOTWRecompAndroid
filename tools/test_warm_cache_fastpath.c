// Host/Bionic unit tests for warm-cache pipeline fast-path policy and per-frame budget helpers.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include "nvn/vulkan/warm_cache_fastpath.h"

int main(void) {
    // Default gating: strictly proprietary ARM Mali r49+; excluded elsewhere until verified.
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-03bet0") == true);
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r52p0-01eac0") == true);
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r48p1-01eac0") == false);
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, NULL) == false);
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_MESA_TURNIP, "Turnip Adreno (TM) 830") == false);
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_QUALCOMM_PROPRIETARY, "v1.r50p0") == false);
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_MESA_PANVK, "Mesa 25.0") == false);
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_SAMSUNG_PROPRIETARY, "Xclipse 940") == false);
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_MESA_RADV, "RADV") == false);
    assert(vk_pipe_cache_ctrl_driver_default(0, "v1.r50p0") == false);
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, "") == false);
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49x1") == false);
    assert(vk_pipe_cache_ctrl_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, "foor49p1") == false);

    // Override semantics: no_env beats everything; explicit env overrides driver default; empty string falls back.
    assert(vk_pipe_cache_ctrl_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1", true, NULL, false) == true);
    assert(vk_pipe_cache_ctrl_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1", true, "", false) == true);
    assert(vk_pipe_cache_ctrl_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1", true, "1", true) == false);
    assert(vk_pipe_cache_ctrl_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1", true, "0", false) == false);
    assert(vk_pipe_cache_ctrl_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1", true, "abc", false) == false);
    assert(vk_pipe_cache_ctrl_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1", false, NULL, false) == false);
    assert(vk_pipe_cache_ctrl_want(VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, NULL, false) == false);
    assert(vk_pipe_cache_ctrl_want(VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "", false) == false);
    assert(vk_pipe_cache_ctrl_want(VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "1", false) == true);
    assert(vk_pipe_cache_ctrl_want(VK_DRIVER_ID_QUALCOMM_PROPRIETARY, "", false, "1", false) == true);

    // Budget env parsing and clamping (microseconds -> nanoseconds).
    assert(vk_warm_inline_budget_ns(NULL, false) == 0);
    assert(vk_warm_inline_budget_ns("5000", false) == 0);
    assert(vk_warm_inline_budget_ns(NULL, true) == 10000000ull);
    assert(vk_warm_inline_budget_ns("", true) == 10000000ull);
    assert(vk_warm_inline_budget_ns("   ", true) == 10000000ull);
    assert(vk_warm_inline_budget_ns("0", true) == 0);
    assert(vk_warm_inline_budget_ns("0 ", true) == 0);
    assert(vk_warm_inline_budget_ns("-50", true) == 0);
    assert(vk_warm_inline_budget_ns("-999999999999999999999999", true) == 0);
    assert(vk_warm_inline_budget_ns("1", true) == 1000ull);
    assert(vk_warm_inline_budget_ns("1500", true) == 1500000ull);
    assert(vk_warm_inline_budget_ns(" 1500", true) == 1500000ull);
    assert(vk_warm_inline_budget_ns("+500", true) == 500000ull);
    assert(vk_warm_inline_budget_ns("5000\n", true) == 5000000ull);
    assert(vk_warm_inline_budget_ns("20000", true) == 20000000ull);
    assert(vk_warm_inline_budget_ns("20001", true) == 20000000ull);
    assert(vk_warm_inline_budget_ns("50000", true) == 20000000ull);
    assert(vk_warm_inline_budget_ns("999999999999999999999999", true) == 20000000ull);
    assert(vk_warm_inline_budget_ns("abc", true) == 10000000ull);
    assert(vk_warm_inline_budget_ns("500px", true) == 10000000ull);

    // Per-frame budget exhaustion (including exact limit_ns boundary), saturation, frame/reboot reset, and NULL safety.
    VkWarmInlineBudget b = {.frame = ~0ull, .limit_ns = 10000000ull};
    assert(vk_warm_budget_allow(&b, 10) == true);
    vk_warm_budget_account(&b, 10, 6000000ull);
    assert(vk_warm_budget_allow(&b, 10) == true);
    vk_warm_budget_account(&b, 10, 4000000ull);
    assert(b.spent_ns == 10000000ull);
    assert(vk_warm_budget_allow(&b, 10) == false);
    vk_warm_budget_account(&b, 10, 500000ull);
    assert(b.spent_ns == 10500000ull);
    assert(vk_warm_budget_allow(&b, 10) == false);

    // Saturation prevents uint64 wraparound back below limit_ns.
    vk_warm_budget_account(&b, 10, UINT64_MAX - 10ull);
    assert(b.spent_ns == UINT64_MAX);
    assert(vk_warm_budget_allow(&b, 10) == false);

    // Advancing frame resets spent_ns cleanly.
    assert(vk_warm_budget_allow(&b, 11) == true);
    assert(b.frame == 11 && b.spent_ns == 0);
    vk_warm_budget_account(&b, 11, 500000ull);

    // Explicitly accounting directly onto a new frame also resets previous frame residue.
    vk_warm_budget_account(&b, 12, 300000ull);
    assert(b.frame == 12 && b.spent_ns == 300000ull);

    // Reboot reset clears frame and spent_ns while keeping configured limit_ns.
    vk_warm_budget_reset(&b);
    assert(b.frame == ~0ull && b.spent_ns == 0 && b.limit_ns == 10000000ull);
    assert(vk_warm_budget_allow(&b, 0) == true);

    b.limit_ns = 0;
    assert(vk_warm_budget_allow(&b, 1) == false);
    assert(vk_warm_budget_allow(NULL, 1) == false);
    vk_warm_budget_account(NULL, 1, 100ull);
    vk_warm_budget_reset(NULL);
    printf("PASS: test_warm_cache_fastpath (gating, env overrides, budget clamping, frame/reboot reset)\n");
    return 0;
}
