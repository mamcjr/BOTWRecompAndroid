// Unit tests for 5th-gen Mali VkRenderPass optimization policy, deferred clear state machine, and storeOp rules.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include "nvn/vulkan/mali_rp_opt.h"

static void test_gating_policies(void) {
    const char *why = NULL;
    const uint32_t arm_id = VK_DRIVER_ID_ARM_PROPRIETARY;
    const char *r49 = "v1.r49p1-03bet0";
    const char *r48 = "v1.r48p0-01eac0";

    // Adreno/Turnip and desktop GPUs (has_bc == true) or missing ASTC must stay off even with env overrides.
    assert(!vk_mali_rp_merge_opt_want(true, true, arm_id, r49, true, "1", &why));
    assert(!vk_mali_rp_merge_opt_want(false, false, arm_id, r49, true, "1", &why));
    assert(!vk_mali_rp_store_opt_want(true, true, VK_API_VERSION_1_3, arm_id, r49, true, "1", &why));
    assert(!vk_mali_rp_store_opt_want(false, false, VK_API_VERSION_1_3, arm_id, r49, true, "1", &why));

    // Phase 2 requires Vulkan 1.3+ for core VK_ATTACHMENT_STORE_OP_NONE.
    assert(vk_mali_rp_merge_opt_want(false, true, arm_id, r49, true, NULL, &why));
    assert(!vk_mali_rp_store_opt_want(false, true, VK_API_VERSION_1_2, arm_id, r49, true, "1", &why));

    // Non-ARM drivers (PanVK, PowerVR, Maleoon) and older ARM drivers (< r49) stay OFF by default,
    // while honoring explicit env opt-in for isolated testing.
    assert(!vk_mali_rp_merge_opt_want(false, true, arm_id, r49, false, NULL, &why));
    assert(!vk_mali_rp_merge_opt_want(false, true, VK_DRIVER_ID_MESA_PANVK, "PanVK", true, NULL, &why));
    assert(!vk_mali_rp_merge_opt_want(false, true, VK_DRIVER_ID_IMAGINATION_PROPRIETARY, "PowerVR", true, NULL, &why));
    assert(!vk_mali_rp_merge_opt_want(false, true, arm_id, r48, true, NULL, &why));
    assert(vk_mali_rp_merge_opt_want(false, true, VK_DRIVER_ID_MESA_PANVK, "PanVK", true, "1", &why));

    assert(!vk_mali_rp_store_opt_want(false, true, VK_API_VERSION_1_3, arm_id, r49, false, NULL, &why));
    assert(!vk_mali_rp_store_opt_want(false, true, VK_API_VERSION_1_3, VK_DRIVER_ID_MESA_PANVK, "PanVK", true, NULL, &why));
    assert(!vk_mali_rp_store_opt_want(false, true, VK_API_VERSION_1_3, arm_id, r48, true, NULL, &why));
    assert(vk_mali_rp_store_opt_want(false, true, VK_API_VERSION_1_3, arm_id, r48, true, "1", &why));

    // ARM Mali r49+ defaults ON and honors env overrides.
    assert(vk_mali_rp_merge_opt_want(false, true, arm_id, r49, true, NULL, &why));
    assert(!vk_mali_rp_merge_opt_want(false, true, arm_id, r49, true, "0", &why));
    assert(vk_mali_rp_merge_opt_want(false, true, arm_id, r49, true, "1", &why));
    assert(vk_mali_rp_merge_opt_want(false, true, arm_id, r49, true, "garbage", &why));

    assert(vk_mali_rp_store_opt_want(false, true, VK_API_VERSION_1_3, arm_id, r49, true, NULL, &why));
    assert(!vk_mali_rp_store_opt_want(false, true, VK_API_VERSION_1_3, arm_id, r49, true, "0", &why));
    assert(vk_mali_rp_store_opt_want(false, true, VK_API_VERSION_1_3, arm_id, r49, true, "1", &why));

    // Mid-pass split policy: suppress 3ms timer on Mali unless explicit env override, keep 8192 draw ceiling.
    assert(!vk_mali_should_split_pass(true, false, 1500, true));
    assert(!vk_mali_should_split_pass(true, false, MALI_RP_MAX_DRAWS_PER_PASS - 1u, true));
    assert(vk_mali_should_split_pass(true, false, MALI_RP_MAX_DRAWS_PER_PASS, false));
    assert(vk_mali_should_split_pass(true, true, 100, true));
    assert(vk_mali_should_split_pass(false, false, 100, true));
    assert(!vk_mali_should_split_pass(false, false, 9000, false));
    assert(vk_mali_rp_store_opt_want(false, true, VK_API_VERSION_1_3, arm_id, r49, true, "garbage", &why));
    assert(vk_mali_rp_store_opt_want(false, true, VK_API_VERSION_1_3, arm_id, r49, true, "", &why));
}

static void test_deferred_clear_gbuffer_merge(void) {
    MaliDeferredClearTable t;
    mali_dclear_reset(&t);

    VkImage img_c0 = (VkImage)(uintptr_t)0x1001;
    VkImage img_c1 = (VkImage)(uintptr_t)0x1002;
    VkImage img_c3 = (VkImage)(uintptr_t)0x1003;
    VkImage img_c0_new = (VkImage)(uintptr_t)0x1004;
    VkImage img_d0 = (VkImage)(uintptr_t)0x1005;

    VkClearColorValue cc0 = {.float32 = {0.1f, 0.2f, 0.3f, 1.0f}};
    VkClearColorValue cc1 = {.float32 = {0.5f, 0.5f, 0.5f, 1.0f}};
    VkClearColorValue cc3 = {.float32 = {0.0f, 0.0f, 1.0f, 0.0f}};
    VkClearColorValue cc0n = {.float32 = {0.9f, 0.8f, 0.7f, 1.0f}};

    // Simulate BOTW p18..p21 incremental G-buffer clears.
    assert(mali_dclear_upsert(&t, img_c0, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                              VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1920, 1080, VK_SAMPLE_COUNT_1_BIT,
                              true, &cc0, false, 0.0f, false, 0) == MALI_DCLEAR_OK);
    assert(mali_dclear_upsert(&t, img_d0, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT,
                              VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1920, 1080, VK_SAMPLE_COUNT_1_BIT,
                              false, NULL, true, 1.0f, false, 0) == MALI_DCLEAR_OK);
    assert(mali_dclear_upsert(&t, img_c1, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1920, 1080, VK_SAMPLE_COUNT_1_BIT,
                              true, &cc1, false, 0.0f, false, 0) == MALI_DCLEAR_OK);
    assert(mali_dclear_upsert(&t, img_c3, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1920, 1080, VK_SAMPLE_COUNT_1_BIT,
                              true, &cc3, false, 0.0f, false, 0) == MALI_DCLEAR_OK);
    assert(mali_dclear_upsert(&t, img_c0_new, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1920, 1080, VK_SAMPLE_COUNT_1_BIT,
                              true, &cc0n, false, 0.0f, false, 0) == MALI_DCLEAR_OK);
    assert(t.count == 5);

    // Consume at p22 begin_pass.
    VkClearColorValue out_cc = {0};
    bool need_mat = false;
    assert(mali_dclear_consume_color(&t, img_c0_new, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, 1920, 1080, &out_cc, &need_mat));
    assert(!need_mat && memcmp(&out_cc, &cc0n, sizeof out_cc) == 0);
    assert(mali_dclear_consume_color(&t, img_c1, 0, 0, VK_FORMAT_R8G8B8A8_SRGB, 1920, 1080, &out_cc, &need_mat));
    assert(!need_mat && memcmp(&out_cc, &cc1, sizeof out_cc) == 0);
    assert(mali_dclear_consume_color(&t, img_c3, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, 1920, 1080, &out_cc, &need_mat));
    assert(!need_mat && memcmp(&out_cc, &cc3, sizeof out_cc) == 0);

    bool got_d = false, got_s = false;
    float dval = 0.0f;
    uint32_t sval = 0;
    mali_dclear_consume_ds(&t, img_d0, 0, 0, VK_FORMAT_D32_SFLOAT, 1920, 1080, true, false,
                           &got_d, &dval, &got_s, &sval, &need_mat);
    assert(got_d && !got_s && !need_mat && dval == 1.0f);

    // Only orphaned img_c0 remains to materialize before read/submit.
    assert(t.count == 1 && mali_dclear_has_image(&t, img_c0));
    MaliDeferredClear popped = {0};
    assert(mali_dclear_pop_any(&t, &popped) && popped.image == img_c0);
    assert(t.count == 0);
}

static void test_depth_stencil_and_conflicts(void) {
    MaliDeferredClearTable t;
    mali_dclear_reset(&t);

    VkImage img_ds = (VkImage)(uintptr_t)0x3001;

    // Independent depth then stencil clear on combined D24_S8 merges cleanly.
    assert(mali_dclear_upsert(&t, img_ds, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
                              VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 960, 540,
                              VK_SAMPLE_COUNT_1_BIT, false, NULL, true, 0.0f, false, 0) == MALI_DCLEAR_OK);
    assert(mali_dclear_upsert(&t, img_ds, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
                              VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 960, 540,
                              VK_SAMPLE_COUNT_1_BIT, false, NULL, false, 0.0f, true, 0x5A) == MALI_DCLEAR_OK);
    assert(t.count == 1 && t.entries[0].has_depth && t.entries[0].has_stencil);

    // Consuming depth only must keep stencil pending until materialized or consumed.
    bool got_d = false, got_s = false, need_mat = false;
    float dval = -1.0f;
    uint32_t sval = 0;
    mali_dclear_consume_ds(&t, img_ds, 0, 0, VK_FORMAT_D24_UNORM_S8_UINT, 960, 540, true, false,
                           &got_d, &dval, &got_s, &sval, &need_mat);
    assert(got_d && !got_s && !need_mat && dval == 0.0f);
    assert(t.count == 1 && !t.entries[0].has_depth && t.entries[0].has_stencil);

    // Extent mismatch forces materialization instead of wrong-area fold.
    mali_dclear_consume_ds(&t, img_ds, 0, 0, VK_FORMAT_D24_UNORM_S8_UINT, 480, 270, false, true,
                           &got_d, &dval, &got_s, &sval, &need_mat);
    assert(!got_s && need_mat && t.count == 1);
    MaliDeferredClear mat = {0};
    assert(mali_dclear_pop_for_image(&t, img_ds, &mat) && mat.has_stencil && mat.stencil_val == 0x5A);
    assert(t.count == 0);

    // Integer RT bit-exact preservation & view format mismatch guard.
    VkImage img_u = (VkImage)(uintptr_t)0x3002;
    VkClearColorValue cu = {.uint32 = {0x80000001u, 0xFFFFFFFFu, 0x12345678u, 7u}};
    assert(mali_dclear_upsert(&t, img_u, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT,
                              VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 256, 256, VK_SAMPLE_COUNT_1_BIT,
                              true, &cu, false, 0.0f, false, 0) == MALI_DCLEAR_OK);
    // Re-clearing same subresource in a different format must report MALI_DCLEAR_CONFLICT.
    assert(mali_dclear_upsert(&t, img_u, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT,
                              VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 256, 256, VK_SAMPLE_COUNT_1_BIT,
                              true, &cu, false, 0.0f, false, 0) == MALI_DCLEAR_CONFLICT);
    VkClearColorValue out_u = {0};
    assert(!mali_dclear_consume_color(&t, img_u, 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 256, 256, &out_u, &need_mat));
    assert(need_mat);
    assert(mali_dclear_consume_color(&t, img_u, 0, 0, VK_FORMAT_R32G32B32A32_UINT, 256, 256, &out_u, &need_mat));
    assert(!need_mat && out_u.uint32[0] == 0x80000001u && out_u.uint32[1] == 0xFFFFFFFFu);

    // Table capacity & reset / forget cleanup.
    for (int i = 0; i < MALI_DEFERRED_CLEAR_MAX; i++) {
        assert(mali_dclear_upsert(&t, (VkImage)(uintptr_t)(0x5000 + i),
                                  VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                  0, 0, 64, 64, VK_SAMPLE_COUNT_1_BIT, true, &cu, false, 0.0f, false, 0) == MALI_DCLEAR_OK);
    }
    assert(mali_dclear_upsert(&t, (VkImage)(uintptr_t)0x5999,
                              VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                              0, 0, 64, 64, VK_SAMPLE_COUNT_1_BIT, true, &cu, false, 0.0f, false, 0) == MALI_DCLEAR_FULL);
    mali_dclear_forget_image(&t, (VkImage)(uintptr_t)0x5000);
    assert(t.count == MALI_DEFERRED_CLEAR_MAX - 1);
    mali_dclear_reset(&t);
    assert(t.count == 0);
}

static void test_active_pass_conflicts(void) {
    MaliActivePassImgs ap;
    VkImage a0 = (VkImage)(uintptr_t)0x7001;
    VkImage a1 = (VkImage)(uintptr_t)0x7002;
    VkImage r0 = (VkImage)(uintptr_t)0x7003;
    VkImage unrelated = (VkImage)(uintptr_t)0x7004;
    VkImage atts[2] = {a0, a1};

    mali_active_pass_begin(&ap, atts, 2);
    mali_active_pass_note_read(&ap, r0);
    assert(mali_active_pass_conflicts(&ap, a0));
    assert(mali_active_pass_conflicts(&ap, r0));
    assert(!mali_active_pass_conflicts(&ap, unrelated));

    // Read overflow and bloom-filter dedup checks.
    assert(ap.last_read == r0 && (ap.read_bloom & mali_dclear_img_bit(r0)) != 0);
    mali_active_pass_note_read(&ap, r0);
    assert(ap.n_read == 1);
    for (int i = 0; i < MALI_ACTIVE_PASS_READ_MAX + 2; i++)
        mali_active_pass_note_read(&ap, (VkImage)(uintptr_t)(0x8000 + i));
    assert(ap.read_overflow);
    assert(mali_active_pass_conflicts(&ap, unrelated));
    mali_active_pass_reset(&ap);
    assert(ap.n_read == 0 && !ap.read_overflow && ap.read_bloom == 0 && ap.last_read == VK_NULL_HANDLE);
}

static void test_store_op_and_vuid_rules(void) {
    // Unwritten LOAD_OP_LOAD -> STORE_OP_NONE; LOAD_OP_CLEAR or written -> STORE_OP_STORE.
    assert(vk_mali_choose_store_op(VK_ATTACHMENT_LOAD_OP_LOAD, false) == VK_ATTACHMENT_STORE_OP_NONE);
    assert(vk_mali_choose_store_op(VK_ATTACHMENT_LOAD_OP_CLEAR, false) == VK_ATTACHMENT_STORE_OP_STORE);
    assert(vk_mali_choose_store_op(VK_ATTACHMENT_LOAD_OP_LOAD, true) == VK_ATTACHMENT_STORE_OP_STORE);
    assert(vk_mali_choose_store_op(VK_ATTACHMENT_LOAD_OP_CLEAR, true) == VK_ATTACHMENT_STORE_OP_STORE);

    // Depth write checks (gl_FragDepth does not write the attachment when depthWriteEnable or depthTestEnable is false).
    assert(!vk_mali_depth_state_writes(false, true));
    assert(!vk_mali_depth_state_writes(true, false));
    assert(!vk_mali_depth_state_writes(false, false));
    assert(vk_mali_depth_state_writes(true, true));

    // Stencil write checks (independent from depth).
    uint32_t keep_all = 0x111111u;  // NVN 1 = KEEP on all 6 ops
    uint32_t front_pass_replace = 0x111311u;  // front passOp = 3 (REPLACE)
    uint32_t back_dfail_wrap = 0x171111u;     // back depthFailOp = 7 (INCR_WRAP)
    assert(!vk_mali_stencil_state_writes(false, front_pass_replace, 0xFF, 0xFF));
    assert(!vk_mali_stencil_state_writes(true, keep_all, 0xFF, 0xFF));
    assert(!vk_mali_stencil_state_writes(true, front_pass_replace, 0x00, 0xFF));
    assert(vk_mali_stencil_state_writes(true, front_pass_replace, 0x01, 0x00));
    assert(!vk_mali_stencil_state_writes(true, back_dfail_wrap, 0xFF, 0x00));
    assert(vk_mali_stencil_state_writes(true, back_dfail_wrap, 0x00, 0x01));
}

static void test_deferred_clear_bloom_filter(void) {
    MaliDeferredClearTable t;
    mali_dclear_reset(&t);
    assert(t.image_bloom == 0);

    VkImage imgs[4] = {
        (VkImage)(uintptr_t)0x9001,
        (VkImage)(uintptr_t)0x9002,
        (VkImage)(uintptr_t)0x9003,
        (VkImage)(uintptr_t)0x9004,
    };
    VkClearColorValue cc = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    for (int i = 0; i < 3; i++) {
        assert(mali_dclear_upsert(&t, imgs[i], VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 128, 128, VK_SAMPLE_COUNT_1_BIT,
                                  true, &cc, false, 0.0f, false, 0) == MALI_DCLEAR_OK);
        assert((t.image_bloom & mali_dclear_img_bit(imgs[i])) != 0);
        assert(mali_dclear_has_image(&t, imgs[i]));
    }
    assert(!mali_dclear_has_image(&t, imgs[3]));

    // Remove middle entry (swaps tail) and ensure remaining entries keep valid bloom bits.
    mali_dclear_remove_at(&t, 0);
    assert(!mali_dclear_has_image(&t, imgs[0]));
    assert(mali_dclear_has_image(&t, imgs[1]));
    assert(mali_dclear_has_image(&t, imgs[2]));
    assert((t.image_bloom & mali_dclear_img_bit(imgs[1])) != 0);
    assert((t.image_bloom & mali_dclear_img_bit(imgs[2])) != 0);

    // Forget remaining images one by one until bloom clears back to 0.
    mali_dclear_forget_image(&t, imgs[1]);
    assert(!mali_dclear_has_image(&t, imgs[1]) && mali_dclear_has_image(&t, imgs[2]));
    mali_dclear_forget_image(&t, imgs[2]);
    assert(t.count == 0 && t.image_bloom == 0);

    assert(mali_dclear_upsert(&t, imgs[3], VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 128, 128, VK_SAMPLE_COUNT_1_BIT,
                              true, &cc, false, 0.0f, false, 0) == MALI_DCLEAR_OK);
    mali_dclear_reset(&t);
    assert(t.count == 0 && t.image_bloom == 0 && !mali_dclear_has_image(&t, imgs[3]));
}

int main(void) {
    test_gating_policies();
    test_deferred_clear_gbuffer_merge();
    test_depth_stencil_and_conflicts();
    test_active_pass_conflicts();
    test_store_op_and_vuid_rules();
    test_deferred_clear_bloom_filter();
    printf("test_mali_rp_opt: all checks passed\n");
    return 0;
}
