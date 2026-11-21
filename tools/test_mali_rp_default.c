// Unit tests for Mali r49+ classic render-pass default gating, settings/env precedence, and rpcompat cache/OOM.
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include "nvn/vulkan/device_compat.h"

#if defined(__ANDROID__) || (defined(__clang__) && (__has_include(<vma/vk_mem_alloc.h>) || __has_include(<vk_mem_alloc.h>)))
#include "nvn/vulkan/vk.h"

void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *fmt, ...) { (void)fmt; abort(); }
void vk_device_fault_report(void) {}

VkG g_vk = {.dev = (VkDevice)(uintptr_t)1, .dyn_render = false};
static uint64_t s_pend_seq = 1;
static int s_create_rp_calls;
static bool s_fail_create_rp;
static bool s_fail_calloc;
static bool s_fail_create_fb;
static uintptr_t s_next_rp = 0x100;
static uintptr_t s_next_fb = 0x200;
static int s_destroy_fb_calls;
static int s_begin_dr_calls;
static int s_end_dr_calls;
static int s_begin_rp_calls;
static int s_end_rp_calls;

uint64_t vk_pending_seq(void) { return s_pend_seq; }

static void *mock_calloc(size_t n, size_t sz) {
    if (s_fail_calloc) {
        errno = ENOMEM;
        return NULL;
    }
    return calloc(n, sz);
}

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
    if (s_fail_create_fb) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = (VkFramebuffer)s_next_fb++;
    return VK_SUCCESS;
}
static void VKAPI_CALL mock_destroy_fb(VkDevice dev, VkFramebuffer fb, const VkAllocationCallbacks *alloc) {
    (void)dev, (void)fb, (void)alloc;
    s_destroy_fb_calls++;
}
static void VKAPI_CALL mock_begin_dr(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    (void)cb, (void)ri;
    s_begin_dr_calls++;
}
static void VKAPI_CALL mock_end_dr(VkCommandBuffer cb) {
    (void)cb;
    s_end_dr_calls++;
}
static void VKAPI_CALL mock_begin_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c) {
    (void)cb, (void)bi, (void)c;
    s_begin_rp_calls++;
}
static void VKAPI_CALL mock_end_rp(VkCommandBuffer cb) {
    (void)cb;
    s_end_rp_calls++;
}

#define calloc mock_calloc
#define vkCreateRenderPass mock_create_rp
#define vkCreateFramebuffer mock_create_fb
#define vkDestroyFramebuffer mock_destroy_fb
#define vkCmdBeginRendering mock_begin_dr
#define vkCmdEndRendering mock_end_dr
#define vkCmdBeginRenderPass mock_begin_rp
#define vkCmdEndRenderPass mock_end_rp
#include "../runtime/nvn/vulkan/rpcompat.c"
#undef calloc
#undef vkCreateRenderPass
#undef vkCreateFramebuffer
#undef vkDestroyFramebuffer
#undef vkCmdBeginRendering
#undef vkCmdEndRendering
#undef vkCmdBeginRenderPass
#undef vkCmdEndRenderPass

static void test_rpcompat_cache(void) {
    // 1. Initial table allocation OOM must return VK_NULL_HANDLE without calling make_rp or leaking handles.
    s_fail_calloc = true;
    VkFormat oom_c = VK_FORMAT_R8G8B8A8_UNORM;
    assert(vk_rp_compat(1, &oom_c, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) == VK_NULL_HANDLE);
    assert(s_create_rp_calls == 0 && nrps == 0 && !rps);
    s_fail_calloc = false;

    // 2. Verify grow/rehash beyond 50% load (RP_INIT_CAP=1024 -> >512 keys) and zero duplicate make_rp calls.
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

    // 3. make_rp failure must return VK_NULL_HANDLE and not pollute the table.
    s_fail_create_rp = true;
    VkFormat fail_c = VK_FORMAT_R16G16B16A16_SFLOAT;
    assert(vk_rp_compat(1, &fail_c, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) == VK_NULL_HANDLE);
    assert(nrps == 700);
    s_fail_create_rp = false;
    assert(vk_rp_compat(1, &fail_c, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) != VK_NULL_HANDLE);
    assert(nrps == 701);

    // 4. Fill table up to rp_cap - 1 under calloc OOM: must refuse uncached insertion BEFORE calling make_rp (0 leak).
    s_fail_calloc = true;
    int extra = 0;
    while ((uint32_t)nrps + 1u < rp_cap) {
        int idx = 1000 + extra++;
        VkFormat c[2] = {(VkFormat)(100 + (idx % 50)), (VkFormat)(1 + (idx / 50))};
        assert(vk_rp_compat(2, c, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) != VK_NULL_HANDLE);
    }
    assert((uint32_t)nrps + 1u == rp_cap);
    int creates_before_full = s_create_rp_calls;
    VkFormat overflow_c[2] = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM};
    // Repeated consecutive OOM calls when full must stay VK_NULL_HANDLE with zero make_rp calls; existing hits still work.
    for (int rep = 0; rep < 3; rep++) {
        assert(vk_rp_compat(2, overflow_c, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) ==
               VK_NULL_HANDLE);
    }
    assert(s_create_rp_calls == creates_before_full);
    assert((uint32_t)nrps + 1u == rp_cap);
    for (int i = 0; i < 700; i++) {
        VkFormat c[2] = {(VkFormat)(1 + (i % 40)), (VkFormat)(1 + ((i / 40) % 20))};
        assert(vk_rp_compat(2, c, (i & 1) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                            VK_SAMPLE_COUNT_1_BIT) == seen[i]);
    }
    assert(s_create_rp_calls == creates_before_full);

    // Recover once calloc succeeds again (2048 -> 4096 growth) and re-verify earlier entries after rehash.
    s_fail_calloc = false;
    assert(vk_rp_compat(2, overflow_c, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) != VK_NULL_HANDLE);
    assert(rp_cap >= 4096u);
    int creates_after_grow = s_create_rp_calls;
    for (int i = 0; i < 700; i++) {
        VkFormat c[2] = {(VkFormat)(1 + (i % 40)), (VkFormat)(1 + ((i / 40) % 20))};
        assert(vk_rp_compat(2, c, (i & 1) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                            VK_SAMPLE_COUNT_1_BIT) == seen[i]);
    }
    assert(s_create_rp_calls == creates_after_grow);

    // 5. Verify vk_begin_rendering / vk_rendering_active / vk_end_rendering return values and OOM safety.
    VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)0x300;
    VkImageView col_view = (VkImageView)(uintptr_t)0x400;
    VkFormat col_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = col_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, {1280, 720}},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &ca};
    assert(!vk_rendering_active());
    assert(vk_begin_rendering(cb, &ri, &col_fmt, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) == true);
    assert(s_begin_rp_calls == 1 && vk_rendering_active());
    vk_end_rendering(cb);
    assert(s_end_rp_calls == 1 && !vk_rendering_active());

    // Framebuffer OOM must return false without calling vkCmdBeginRenderPass, and vk_end_rendering must not call EndRenderPass.
    s_fail_create_fb = true;
    VkImageView fb_oom_view = (VkImageView)(uintptr_t)0x401;
    ca.imageView = fb_oom_view;
    assert(vk_begin_rendering(cb, &ri, &col_fmt, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) == false);
    assert(s_begin_rp_calls == 1 && !vk_rendering_active());
    vk_end_rendering(cb);
    assert(s_end_rp_calls == 1);
    s_fail_create_fb = false;

    // Forget view + dynamic rendering path.
    vk_rp_forget_view(col_view);
    assert(s_destroy_fb_calls == 1);
    g_vk.dyn_render = true;
    assert(vk_begin_rendering(cb, &ri, &col_fmt, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) == true);
    assert(s_begin_dr_calls == 1 && vk_rendering_active());
    vk_end_rendering(cb);
    assert(s_end_dr_calls == 1);
    g_vk.dyn_render = false;
    assert(!vk_rendering_active());

    // 6. Deferred destruction queue: fill framebuffer table past FB_MAX * 3 / 4 to trigger drop_all()
    //    and deferred-queue growth, then verify vk_rp_collect() respects fence seq ordering.
    s_pend_seq = 5;
    int destroyed_before = s_destroy_fb_calls;
    for (int i = 0; i <= FB_MAX * 3 / 4; i++) {
        ca.imageView = (VkImageView)(uintptr_t)(0x10000 + i);
        assert(vk_begin_rendering(cb, &ri, &col_fmt, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT) == true);
        vk_end_rendering(cb);
    }
    int queued = atomic_load_explicit(&nlate, memory_order_relaxed);
    assert(queued >= FB_MAX * 3 / 4 - 1 && queued <= FB_MAX * 3 / 4);
    assert(s_destroy_fb_calls == destroyed_before);
    vk_rp_collect(4);
    assert(atomic_load_explicit(&nlate, memory_order_relaxed) == queued);
    assert(s_destroy_fb_calls == destroyed_before);
    vk_rp_collect(5);
    assert(atomic_load_explicit(&nlate, memory_order_relaxed) == 0);
    assert(s_destroy_fb_calls == destroyed_before + queued);
    vk_rp_collect(100);
}
#endif

int main(void) {
    const char *reason = NULL;

    // 1. ARM Mali proprietary r49+ defaults dynamic rendering OFF when env is unset, empty, or whitespace-only (setting=-1).
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, NULL, -1, &reason));
    assert(reason && strcmp(reason, "Mali r49+ default") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r54p1-00eac0", true, NULL, -1, &reason));
    assert(reason && strcmp(reason, "Mali r49+ default") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "r49p0", true, "", -1, &reason));
    assert(reason && strcmp(reason, "Mali r49+ default") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "r49p1", true, "   \t ", -1, &reason));
    assert(reason && strcmp(reason, "Mali r49+ default") == 0);

    // 2. Older Mali (< r49 including exact r48 boundary) and malformed/missing tokens stay ON by default.
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r48p1-00eac0", true, NULL, -1, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r38p1-00eac0", true, NULL, -1, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "", true, NULL, -1, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, NULL, true, NULL, -1, &reason));
    assert(reason && strcmp(reason, "default") == 0);

    // 3. Other GPU drivers (Turnip, RADV, PanVK, Qualcomm, Samsung/Xclipse) keep dynamic rendering ON by default.
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Mesa 25.0 Turnip", true, NULL, -1, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_MESA_RADV, "Mesa RADV", true, NULL, -1, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_MESA_PANVK, "v1.r49p1-panvk", true, NULL, -1, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_QUALCOMM_PROPRIETARY, "v1.r49p1", true, NULL, -1, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_SAMSUNG_PROPRIETARY, "v1.r49p1", true, NULL, -1, &reason));
    assert(reason && strcmp(reason, "default") == 0);

    // 4. Missing driver properties prevents Mali gating.
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", false, NULL, -1, &reason));
    assert(reason && strcmp(reason, "default") == 0);

    // 5. Unsupported hardware cannot be forced ON by env or setting.
    assert(!vk_dyn_render_want(false, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "0", -1, &reason));
    assert(reason && strcmp(reason, "the driver lacks it") == 0);
    assert(!vk_dyn_render_want(false, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1", true, NULL, 1, &reason));
    assert(reason && strcmp(reason, "the driver lacks it") == 0);

    // 6. Settings (-1 Auto, 1 On, 0 Off) and env precedence over settings.
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, NULL, 1, &reason));
    assert(reason && strcmp(reason, "settings") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, NULL, 0, &reason));
    assert(reason && strcmp(reason, "settings") == 0);
    // Env wins over explicit settings.
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "1", 1, &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=1") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, "0", 0, &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=0") == 0);

    // 7. Rollback env override and invalid/non-numeric fallthrough.
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, "0", -1, &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=0") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, "-0", -1, &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=0") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, "+000", -1, &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=0") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "1", -1, &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=1") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "-1", -1, &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=1") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "99999999999999999999", -1, &reason));
    assert(reason && strcmp(reason, "LARECOMP_NO_DYNAMIC_RENDERING=1") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "01", -1, NULL));
    // Non-numeric / trailing garbage must be ignored and fall through cleanly to setting/default.
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, "abc", -1, &reason));
    assert(reason && strcmp(reason, "default") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, "1x", -1, &reason));
    assert(reason && strcmp(reason, "Mali r49+ default") == 0);
    assert(!vk_dyn_render_want(true, VK_DRIVER_ID_MESA_TURNIP, "Turnip", true, " 1a", 0, &reason));
    assert(reason && strcmp(reason, "settings") == 0);
    assert(vk_dyn_render_want(true, VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-00eac0", true, "abc", 1, &reason));
    assert(reason && strcmp(reason, "settings") == 0);

#if defined(__ANDROID__) || (defined(__clang__) && (__has_include(<vma/vk_mem_alloc.h>) || __has_include(<vk_mem_alloc.h>)))
    test_rpcompat_cache();
#endif

    printf("test_mali_rp_default: all checks passed\n");
    return 0;
}
