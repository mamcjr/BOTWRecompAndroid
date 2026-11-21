// Unit tests for Mali r49+ classic render-pass default gating and rpcompat hash/grow cache.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include "nvn/vulkan/device_compat.h"

#if defined(__ANDROID__) && defined(__aarch64__)
#include "nvn/vulkan/vk.h"

void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *fmt, ...) { (void)fmt; abort(); }
void vk_device_fault_report(void) {}

VkG g_vk = {.dev = (VkDevice)(uintptr_t)1, .dyn_render = false};
static uint64_t s_pend_seq = 1;
static int s_create_rp_calls;
static bool s_fail_create_rp;
static uintptr_t s_next_rp = 0x100;
static int s_destroy_fb_calls;

uint64_t vk_pending_seq(void) { return s_pend_seq; }

static VkResult VKAPI_CALL mock_create_rp(VkDevice dev, const VkRenderPassCreateInfo *ci,
                                          const VkAllocationCallbacks *alloc, VkRenderPass *out) {
    (void)dev, (void)ci, (void)alloc;
    s_create_rp_calls++;
    if (s_fail_create_rp) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = (VkRenderPass)s_next_rp++;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL mock_create_fb(VkDevice dev, const VkFramebufferCreateInfo *ci,
                                          const VkAllocationCallbacks *alloc, VkFramebuffer *out) {
    (void)dev, (void)ci, (void)alloc;
    *out = (VkFramebuffer)(uintptr_t)0x200;
    return VK_SUCCESS;
}
static void VKAPI_CALL mock_destroy_fb(VkDevice dev, VkFramebuffer fb, const VkAllocationCallbacks *alloc) {
    (void)dev, (void)fb, (void)alloc;
    s_destroy_fb_calls++;
}
static void VKAPI_CALL mock_begin_dr(VkCommandBuffer cb, const VkRenderingInfo *ri) { (void)cb, (void)ri; }
static void VKAPI_CALL mock_end_dr(VkCommandBuffer cb) { (void)cb; }
static void VKAPI_CALL mock_begin_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c) {
    (void)cb, (void)bi, (void)c;
}
static void VKAPI_CALL mock_end_rp(VkCommandBuffer cb) { (void)cb; }

#define vkCreateRenderPass mock_create_rp
#define vkCreateFramebuffer mock_create_fb
#define vkDestroyFramebuffer mock_destroy_fb
#define vkCmdBeginRendering mock_begin_dr
#define vkCmdEndRendering mock_end_dr
#define vkCmdBeginRenderPass mock_begin_rp
#define vkCmdEndRenderPass mock_end_rp
#include "../runtime/nvn/vulkan/rpcompat.c"

static void test_rpcompat_cache(void) {
    // Verify grow/rehash beyond initial 50% load (RP_INIT_CAP=1024 -> >512 keys) and zero duplicate make_rp calls.
    VkRenderPass seen[700];
    for (int i = 0; i < 700; i++) {
        VkFormat c[2] = {(VkFormat)(1 + (i % 40)), (VkFormat)(1 + ((i / 40) % 20))};
        seen[i] = vk_rp_compat(2, c, (i & 1) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED,
                               VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
        assert(seen[i] != VK_NULL_HANDLE);
    }
    assert(s_create_rp_calls == 700);
    assert(nrps == 700);
    assert(rp_cap >= 2048u);

    // Every previously inserted key must hit cleanly without invoking make_rp.
    for (int i = 0; i < 700; i++) {
        VkFormat c[2] = {(VkFormat)(1 + (i % 40)), (VkFormat)(1 + ((i / 40) % 20))};
        VkRenderPass hit = vk_rp_compat(2, c, (i & 1) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED,
                                        VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
        assert(hit == seen[i]);
    }
    assert(s_create_rp_calls == 700);

    // make_rp failure must return VK_NULL_HANDLE and not pollute the table.
    s_fail_create_rp = true;
    VkFormat fail_c = VK_FORMAT_R16G16B16A16_SFLOAT;
    assert(vk_rp_compat(1, &fail_c, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) == VK_NULL_HANDLE);
    assert(nrps == 700);
    s_fail_create_rp = false;
    assert(vk_rp_compat(1, &fail_c, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) != VK_NULL_HANDLE);
    assert(nrps == 701);
}
#endif

int main(void) {
    const char *reason = NULL;

    // 1. ARM Mali proprietary r49+ defaults dynamic rendering OFF when env is unset, empty, or whitespace-only.
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, NULL, &reason));
    assert(reason && strcmp(reason, "Mali r49+ default") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r54p1-00eac0", true, NULL, &reason));
    assert(reason && strcmp(reason, "Mali r49+ default") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "r49p0", true, "", &reason));
    assert(reason && strcmp(reason, "Mali r49+ default") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "r49p1", true, "   \t ", &reason));
    assert(reason && strcmp(reason, "Mali r49+ default") == 0);

    // 2. Older Mali (< r49 including exact r48 boundary) and malformed/missing tokens stay ON by default.
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r48p1-00eac0", true, NULL, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r38p1-00eac0", true, NULL, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "", true, NULL, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, NULL, true, NULL, &reason));
    assert(reason && strcmp(reason, "default") == 0);

    // 3. Other GPU drivers (Turnip, RADV, PanVK, Qualcomm, Samsung/Xclipse) keep dynamic rendering ON by default.
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Mesa 25.0 Turnip", true, NULL, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_MESA_RADV, "Mesa RADV", true, NULL, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_MESA_PANVK, "v1.r49p1-panvk", true, NULL, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_QUALCOMM_PROPRIETARY, "v1.r49p1", true, NULL, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_SAMSUNG_PROPRIETARY, "v1.r49p1", true, NULL, &reason));
    assert(reason && strcmp(reason, "default") == 0);

    // 4. Missing driver properties prevents Mali gating.
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", false, NULL, &reason));
    assert(reason && strcmp(reason, "default") == 0);

    // 5. Unsupported hardware cannot be forced ON by LARECOMP_NO_DYNAMIC_RENDERING=0.
    assert(!vk_dyn_render_want(false, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "0", &reason));
    assert(reason && strcmp(reason, "the driver lacks it") == 0);
    assert(!vk_dyn_render_want(false, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1", true, "0", NULL));

    // 6. Rollback env override and numeric edge cases.
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, "0", &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=0") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, "-0", &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=0") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, "+000", &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=0") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "1", &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=1") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "-1", &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=1") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, " 1a", &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=1") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "99999999999999999999", &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=1") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "01", NULL));

#if defined(__ANDROID__) && defined(__aarch64__)
    test_rpcompat_cache();
#endif

    printf("test_mali_rp_default: all checks passed\n");
    return 0;
}
