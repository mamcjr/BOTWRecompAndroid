// Host/Bionic unit tests for warm-cache pipeline fast-path policy and per-frame budget helpers.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

    // FPS-scaled default budget tiers: <=30 -> 4000 us, 31..45 -> 3000 us, >45 or 0 -> 2000 us.
    assert(vk_warm_inline_default_us(20) == 4000);
    assert(vk_warm_inline_default_us(30) == 4000);
    assert(vk_warm_inline_default_us(31) == 3000);
    assert(vk_warm_inline_default_us(40) == 3000);
    assert(vk_warm_inline_default_us(45) == 3000);
    assert(vk_warm_inline_default_us(46) == 2000);
    assert(vk_warm_inline_default_us(60) == 2000);
    assert(vk_warm_inline_default_us(120) == 2000);
    assert(vk_warm_inline_default_us(0) == 2000);
    assert(vk_warm_inline_default_us(-1) == 2000);

    // Explicit env override parsing helper vs fallback to FPS-scaled default.
    uint64_t parsed_ns = 999ull;
    assert(vk_warm_inline_parse_env_ns(NULL, &parsed_ns) == false);
    assert(vk_warm_inline_parse_env_ns("", &parsed_ns) == false);
    assert(vk_warm_inline_parse_env_ns("   ", &parsed_ns) == false);
    assert(vk_warm_inline_parse_env_ns("abc", &parsed_ns) == false);
    assert(vk_warm_inline_parse_env_ns("500px", &parsed_ns) == false);
    assert(vk_warm_inline_parse_env_ns("100 200", &parsed_ns) == false);
    assert(vk_warm_inline_parse_env_ns("500\v\f", &parsed_ns) == true && parsed_ns == 500000ull);
    assert(vk_warm_inline_parse_env_ns("1500", &parsed_ns) == true && parsed_ns == 1500000ull);
    assert(vk_warm_inline_parse_env_ns("0", &parsed_ns) == true && parsed_ns == 0ull);
    assert(vk_warm_inline_parse_env_ns("-50", &parsed_ns) == true && parsed_ns == 0ull);
    assert(vk_warm_inline_parse_env_ns("999999999999999999999999", &parsed_ns) == true && parsed_ns == 20000000ull);
    assert(vk_warm_inline_parse_env_ns("5000", NULL) == true);

    // Budget env parsing, FPS defaults, and clamping (microseconds -> nanoseconds).
    assert(vk_warm_inline_budget_ns(NULL, false, 30) == 0);
    assert(vk_warm_inline_budget_ns("5000", false, 30) == 0);
    assert(vk_warm_inline_budget_ns(NULL, true, 30) == 4000000ull);
    assert(vk_warm_inline_budget_ns(NULL, true, 40) == 3000000ull);
    assert(vk_warm_inline_budget_ns(NULL, true, 60) == 2000000ull);
    assert(vk_warm_inline_budget_ns("", true, 30) == 4000000ull);
    assert(vk_warm_inline_budget_ns("   ", true, 45) == 3000000ull);
    assert(vk_warm_inline_budget_ns("0", true, 30) == 0);
    assert(vk_warm_inline_budget_ns("0 ", true, 30) == 0);
    assert(vk_warm_inline_budget_ns("-50", true, 30) == 0);
    assert(vk_warm_inline_budget_ns("-999999999999999999999999", true, 30) == 0);
    assert(vk_warm_inline_budget_ns("1", true, 60) == 1000ull);
    assert(vk_warm_inline_budget_ns("1500", true, 60) == 1500000ull);
    assert(vk_warm_inline_budget_ns(" 1500", true, 60) == 1500000ull);
    assert(vk_warm_inline_budget_ns("+500", true, 60) == 500000ull);
    assert(vk_warm_inline_budget_ns("5000\n", true, 60) == 5000000ull);
    assert(vk_warm_inline_budget_ns("20000", true, 30) == 20000000ull);
    assert(vk_warm_inline_budget_ns("20001", true, 30) == 20000000ull);
    assert(vk_warm_inline_budget_ns("50000", true, 30) == 20000000ull);
    assert(vk_warm_inline_budget_ns("999999999999999999999999", true, 30) == 20000000ull);
    assert(vk_warm_inline_budget_ns("abc", true, 30) == 4000000ull);
    assert(vk_warm_inline_budget_ns("500px", true, 60) == 2000000ull);

    // Device gating & Vulkan 1.3 pipelineCreationCacheControl selection (mirroring device.c).
    assert(!vk_pipe_cache_ctrl_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1", true, "   ", false));
    VkBool32 v13_en = VK_TRUE;
    const char *why = NULL;
    assert(vk_pipe_cache_ctrl_select(VK_API_VERSION_1_3, VK_TRUE, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1",
                                     true, NULL, false, false, &v13_en, &why) &&
           v13_en == VK_TRUE && strcmp(why, "driver default") == 0);
    assert(!vk_pipe_cache_ctrl_select(VK_API_VERSION_1_2, VK_TRUE, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1",
                                      true, NULL, false, false, &v13_en, &why) &&
           v13_en == VK_FALSE && strcmp(why, "unsupported") == 0);
    assert(!vk_pipe_cache_ctrl_select(VK_API_VERSION_1_3, VK_FALSE, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1",
                                      true, NULL, false, false, &v13_en, &why) &&
           v13_en == VK_FALSE && strcmp(why, "unsupported") == 0);
    assert(!vk_pipe_cache_ctrl_select(VK_API_VERSION_1_3, VK_TRUE, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1",
                                      true, NULL, false, true, &v13_en, &why) &&
           v13_en == VK_FALSE && strcmp(why, "pipeline cache disabled") == 0);
    assert(!vk_pipe_cache_ctrl_select(VK_API_VERSION_1_3, VK_TRUE, VK_DRIVER_ID_MESA_TURNIP, "Turnip",
                                      true, NULL, false, false, &v13_en, &why) &&
           v13_en == VK_FALSE && strcmp(why, "off by default") == 0);
    assert(vk_pipe_cache_ctrl_select(VK_API_VERSION_1_3, VK_TRUE, VK_DRIVER_ID_MESA_TURNIP, "Turnip",
                                     true, "1", false, false, &v13_en, &why) &&
           v13_en == VK_TRUE && strcmp(why, "LARECOMP_VK_CACHE_CTRL") == 0);
    assert(!vk_pipe_cache_ctrl_select(VK_API_VERSION_1_3, VK_TRUE, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1",
                                      true, "0", false, false, &v13_en, &why) &&
           v13_en == VK_FALSE && strcmp(why, "LARECOMP_VK_CACHE_CTRL=0") == 0);
    assert(!vk_pipe_cache_ctrl_select(VK_API_VERSION_1_3, VK_TRUE, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1",
                                      true, "1", true, false, &v13_en, &why) &&
           v13_en == VK_FALSE && strcmp(why, "LARECOMP_VK_NO_CACHE_CTRL") == 0);

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
