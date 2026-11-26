// Policy and state-machine helpers for 5th-gen Mali VkRenderPass optimization:
// - Deferred full-surface clears (avoiding empty clear passes and false mid-pass breaks)
// - Mid-pass split timer suppression (avoiding tile store + reload churn on tilers)
// - Bounded lookahead STORE_OP_NONE for unwritten LOAD_OP_LOAD attachments
// Included from device.c, exec.c, and standalone host / Bionic unit tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "device_compat.h"

#define MALI_RP_MAX_DRAWS_PER_PASS 8192u
#define MALI_DEFERRED_CLEAR_MAX 64
#define MALI_ACTIVE_PASS_ATT_MAX 9
#define MALI_ACTIVE_PASS_READ_MAX 64

enum {
    MALI_DCLEAR_OK = 0,
    MALI_DCLEAR_CONFLICT = 1,
    MALI_DCLEAR_FULL = 2,
};

typedef struct {
    VkImage image;
    VkFormat view_format;
    VkFormat image_format;
    VkImageAspectFlags image_aspect;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t extent_w;
    uint32_t extent_h;
    VkSampleCountFlagBits samples;
    bool has_color;
    bool has_depth;
    bool has_stencil;
    VkImageAspectFlags aspect_mask;
    VkClearColorValue color;
    float depth_val;
    uint32_t stencil_val;
} MaliDeferredClear;

typedef struct {
    MaliDeferredClear entries[MALI_DEFERRED_CLEAR_MAX];
    int count;
    uint64_t image_bloom;
} MaliDeferredClearTable;

typedef struct {
    VkImage att_imgs[MALI_ACTIVE_PASS_ATT_MAX];
    int n_att;
    VkImage read_imgs[MALI_ACTIVE_PASS_READ_MAX];
    int n_read;
    VkImage last_read;
    uint64_t read_bloom;
    bool read_overflow;
} MaliActivePassImgs;

// ---- Capability + env gating

// Defer full-surface clears across passes and suppress the 3 ms mid-pass split timer on ARM Mali r49+ (!BC && ASTC).
static inline bool vk_mali_rp_merge_opt_want(bool has_bc, bool has_astc_ldr, uint32_t driver_id,
                                             const char *driver_info, bool have_driver_props,
                                             const char *env, const char **why_out) {
    if (has_bc || !has_astc_ldr) {
        if (why_out) *why_out = has_bc ? "BC supported (!BC && ASTC gate)" : "no ASTC_LDR (!BC && ASTC gate)";
        return false;
    }
    bool env_val = false;
    if (vk_compat_parse_env_bool(env, &env_val)) {
        if (why_out) *why_out = env_val ? "LARECOMP_MALI_RP_MERGE_OPT=1" : "LARECOMP_MALI_RP_MERGE_OPT=0";
        return env_val;
    }
    if (!have_driver_props) {
        if (why_out) *why_out = "no driver properties";
        return false;
    }
    if (driver_id != VK_DRIVER_ID_ARM_PROPRIETARY) {
        if (why_out) *why_out = "not ARM Mali proprietary";
        return false;
    }
    if (vk_arm_driver_major(driver_info) < 49) {
        if (why_out) *why_out = "ARM Mali driver < r49";
        return false;
    }
    if (why_out) *why_out = "ARM Mali r49+ default";
    return true;
}

// Emit STORE_OP_NONE for unwritten LOAD_OP_LOAD attachments via bounded lookahead (ARM Mali r49+ && !BC && ASTC && VK 1.3+).
static inline bool vk_mali_rp_store_opt_want(bool has_bc, bool has_astc_ldr, uint32_t api, uint32_t driver_id,
                                             const char *driver_info, bool have_driver_props,
                                             const char *env, const char **why_out) {
    if (has_bc || !has_astc_ldr) {
        if (why_out) *why_out = has_bc ? "BC supported (!BC && ASTC gate)" : "no ASTC_LDR (!BC && ASTC gate)";
        return false;
    }
    if (api < VK_API_VERSION_1_3) {
        if (why_out) *why_out = "Vulkan API < 1.3 (needs core STORE_OP_NONE)";
        return false;
    }
    bool env_val = false;
    if (vk_compat_parse_env_bool(env, &env_val)) {
        if (why_out) *why_out = env_val ? "LARECOMP_MALI_RP_STORE_OPT=1" : "LARECOMP_MALI_RP_STORE_OPT=0";
        return env_val;
    }
    if (!have_driver_props) {
        if (why_out) *why_out = "no driver properties";
        return false;
    }
    if (driver_id != VK_DRIVER_ID_ARM_PROPRIETARY) {
        if (why_out) *why_out = "not ARM Mali proprietary";
        return false;
    }
    if (vk_arm_driver_major(driver_info) < 49) {
        if (why_out) *why_out = "ARM Mali driver < r49";
        return false;
    }
    if (why_out) *why_out = "ARM Mali r49+ default";
    return true;
}

// Suppress the 3 ms mid-pass split timer on Mali unless LARECOMP_VK_SPLIT_US was explicitly set,
// while retaining a high per-pass draw safety ceiling (MALI_RP_MAX_DRAWS_PER_PASS).
static inline bool vk_mali_should_split_pass(bool mali_merge_opt, bool split_env_explicit, uint32_t pass_draws,
                                             bool timer_expired) {
    if (mali_merge_opt && !split_env_explicit) return pass_draws >= MALI_RP_MAX_DRAWS_PER_PASS;
    return timer_expired;
}

// ---- Phase 1 deferred full-surface clear table

static inline uint64_t mali_dclear_img_bit(VkImage img) {
    uint64_t h = (uint64_t)(uintptr_t)img * 0x9E3779B97F4A7C15ull;
    return 1ull << (h >> 58);
}

static inline void mali_dclear_reset(MaliDeferredClearTable *t) {
    if (!t) return;
    t->count = 0;
    t->image_bloom = 0;
}

static inline int mali_dclear_find(const MaliDeferredClearTable *t, VkImage img, uint32_t mip_level,
                                   uint32_t array_layer) {
    if (!t || !img || t->count <= 0) return -1;
    if (!(t->image_bloom & mali_dclear_img_bit(img))) return -1;
    for (int i = 0; i < t->count; i++) {
        const MaliDeferredClear *e = &t->entries[i];
        if (e->image == img && e->mip_level == mip_level && e->array_layer == array_layer) return i;
    }
    return -1;
}

static inline bool mali_dclear_has_image(const MaliDeferredClearTable *t, VkImage img) {
    if (!t || !img || t->count <= 0) return false;
    if (!(t->image_bloom & mali_dclear_img_bit(img))) return false;
    for (int i = 0; i < t->count; i++)
        if (t->entries[i].image == img) return true;
    return false;
}

static inline void mali_dclear_remove_at(MaliDeferredClearTable *t, int idx) {
    if (!t || idx < 0 || idx >= t->count) return;
    t->entries[idx] = t->entries[--t->count];
    uint64_t bm = 0;
    for (int i = 0; i < t->count; i++) bm |= mali_dclear_img_bit(t->entries[i].image);
    t->image_bloom = bm;
}

static inline void mali_dclear_forget_image(MaliDeferredClearTable *t, VkImage img) {
    if (!t || !img || t->count <= 0) return;
    if (!(t->image_bloom & mali_dclear_img_bit(img))) return;
    for (int i = t->count - 1; i >= 0; i--)
        if (t->entries[i].image == img) mali_dclear_remove_at(t, i);
}

static inline int mali_dclear_upsert(MaliDeferredClearTable *t, VkImage img, VkFormat view_format,
                                     VkFormat image_format, VkImageAspectFlags image_aspect, uint32_t mip_level,
                                     uint32_t array_layer, uint32_t extent_w, uint32_t extent_h,
                                     VkSampleCountFlagBits samples, bool set_color, const VkClearColorValue *color,
                                     bool set_depth, float depth_val, bool set_stencil, uint32_t stencil_val) {
    if (!t || !img || !extent_w || !extent_h) return MALI_DCLEAR_CONFLICT;
    int idx = mali_dclear_find(t, img, mip_level, array_layer);
    if (idx >= 0) {
        MaliDeferredClear *e = &t->entries[idx];
        if (e->view_format != view_format || e->extent_w != extent_w || e->extent_h != extent_h ||
            e->samples != samples)
            return MALI_DCLEAR_CONFLICT;
        if (set_color && color) {
            e->has_color = true;
            e->color = *color;
            e->aspect_mask |= VK_IMAGE_ASPECT_COLOR_BIT;
        }
        if (set_depth) {
            e->has_depth = true;
            e->depth_val = depth_val;
            e->aspect_mask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if (set_stencil) {
            e->has_stencil = true;
            e->stencil_val = stencil_val;
            e->aspect_mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        return MALI_DCLEAR_OK;
    }
    if (t->count >= MALI_DEFERRED_CLEAR_MAX) return MALI_DCLEAR_FULL;
    MaliDeferredClear e = {0};
    e.image = img;
    e.view_format = view_format;
    e.image_format = image_format;
    e.image_aspect = image_aspect;
    e.mip_level = mip_level;
    e.array_layer = array_layer;
    e.extent_w = extent_w;
    e.extent_h = extent_h;
    e.samples = samples ? samples : VK_SAMPLE_COUNT_1_BIT;
    if (set_color && color) {
        e.has_color = true;
        e.color = *color;
        e.aspect_mask |= VK_IMAGE_ASPECT_COLOR_BIT;
    }
    if (set_depth) {
        e.has_depth = true;
        e.depth_val = depth_val;
        e.aspect_mask |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (set_stencil) {
        e.has_stencil = true;
        e.stencil_val = stencil_val;
        e.aspect_mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    t->entries[t->count++] = e;
    t->image_bloom |= mali_dclear_img_bit(img);
    return MALI_DCLEAR_OK;
}

static inline bool mali_dclear_consume_color(MaliDeferredClearTable *t, VkImage img, uint32_t mip_level,
                                             uint32_t array_layer, VkFormat pass_fmt, uint32_t pass_w, uint32_t pass_h,
                                             VkClearColorValue *out_color, bool *needs_materialize) {
    if (needs_materialize) *needs_materialize = false;
    int idx = mali_dclear_find(t, img, mip_level, array_layer);
    if (idx < 0) return false;
    MaliDeferredClear *e = &t->entries[idx];
    if (!e->has_color) return false;
    if (e->view_format != pass_fmt || e->extent_w != pass_w || e->extent_h != pass_h) {
        if (needs_materialize) *needs_materialize = true;
        return false;
    }
    if (out_color) *out_color = e->color;
    e->has_color = false;
    e->aspect_mask &= ~VK_IMAGE_ASPECT_COLOR_BIT;
    if (!e->has_color && !e->has_depth && !e->has_stencil) mali_dclear_remove_at(t, idx);
    return true;
}

static inline void mali_dclear_consume_ds(MaliDeferredClearTable *t, VkImage img, uint32_t mip_level,
                                          uint32_t array_layer, VkFormat pass_ds_fmt, uint32_t pass_w, uint32_t pass_h,
                                          bool want_d, bool want_s, bool *got_d, float *out_d, bool *got_s,
                                          uint32_t *out_s, bool *needs_materialize) {
    if (got_d) *got_d = false;
    if (got_s) *got_s = false;
    if (needs_materialize) *needs_materialize = false;
    int idx = mali_dclear_find(t, img, mip_level, array_layer);
    if (idx < 0) return;
    MaliDeferredClear *e = &t->entries[idx];
    if (!e->has_depth && !e->has_stencil) return;
    if (e->image_format != pass_ds_fmt || e->extent_w != pass_w || e->extent_h != pass_h) {
        if (needs_materialize) *needs_materialize = true;
        return;
    }
    if (want_d && e->has_depth) {
        if (got_d) *got_d = true;
        if (out_d) *out_d = e->depth_val;
        e->has_depth = false;
        e->aspect_mask &= ~VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (want_s && e->has_stencil) {
        if (got_s) *got_s = true;
        if (out_s) *out_s = e->stencil_val;
        e->has_stencil = false;
        e->aspect_mask &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (!e->has_color && !e->has_depth && !e->has_stencil) mali_dclear_remove_at(t, idx);
}

static inline bool mali_dclear_pop_for_image(MaliDeferredClearTable *t, VkImage img, MaliDeferredClear *out) {
    if (!t || !img || t->count <= 0) return false;
    if (!(t->image_bloom & mali_dclear_img_bit(img))) return false;
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].image == img) {
            if (out) *out = t->entries[i];
            mali_dclear_remove_at(t, i);
            return true;
        }
    }
    return false;
}

static inline bool mali_dclear_pop_any(MaliDeferredClearTable *t, MaliDeferredClear *out) {
    if (!t || t->count <= 0) return false;
    if (out) *out = t->entries[0];
    mali_dclear_remove_at(t, 0);
    return true;
}

// ---- Active-pass conflict tracking for unrelated ClearTexture deferral

static inline void mali_active_pass_reset(MaliActivePassImgs *p) {
    if (!p) return;
    p->n_att = 0;
    p->n_read = 0;
    p->last_read = VK_NULL_HANDLE;
    p->read_bloom = 0;
    p->read_overflow = false;
}

static inline void mali_active_pass_begin(MaliActivePassImgs *p, const VkImage *atts, int n_att) {
    mali_active_pass_reset(p);
    if (!p || !atts) return;
    for (int i = 0; i < n_att && p->n_att < MALI_ACTIVE_PASS_ATT_MAX; i++) {
        if (!atts[i]) continue;
        bool dup = false;
        for (int j = 0; j < p->n_att; j++)
            if (p->att_imgs[j] == atts[i]) { dup = true; break; }
        if (!dup) p->att_imgs[p->n_att++] = atts[i];
    }
}

static inline void mali_active_pass_note_read(MaliActivePassImgs *p, VkImage img) {
    if (!p || !img || p->read_overflow || img == p->last_read) return;
    uint64_t bit = mali_dclear_img_bit(img);
    if (p->read_bloom & bit) {
        for (int i = 0; i < p->n_read; i++) {
            if (p->read_imgs[i] == img) {
                p->last_read = img;
                return;
            }
        }
    }
    if (p->n_read >= MALI_ACTIVE_PASS_READ_MAX) {
        p->read_overflow = true;
        return;
    }
    p->read_imgs[p->n_read++] = img;
    p->read_bloom |= bit;
    p->last_read = img;
}

static inline bool mali_active_pass_conflicts(const MaliActivePassImgs *p, VkImage img) {
    if (!p || !img) return true;
    for (int i = 0; i < p->n_att; i++)
        if (p->att_imgs[i] == img) return true;
    if (p->read_overflow) return true;
    if (!(p->read_bloom & mali_dclear_img_bit(img))) return false;
    for (int i = 0; i < p->n_read; i++)
        if (p->read_imgs[i] == img) return true;
    return false;
}

// ---- Strict Vulkan 1.3 storeOp selection helpers

// Maps NVN stencil op code (exec.c stencil_op()) to whether it can modify stencil bits (anything != KEEP).
static inline bool vk_mali_nvn_stencil_op_writes(uint8_t o) {
    return o >= 2u && o <= 8u;
}

// Checks whether a draw can mutate the stencil buffer under Vulkan 1.3 rules for stencilStoreOp = NONE.
// ds_ops_24bit matches exec.c's (st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16):
// front fail=[3:0], depthFail=[7:4], pass=[11:8]; back fail=[15:12], depthFail=[19:16], pass=[23:20].
static inline bool vk_mali_stencil_state_writes(bool stencil_test_enable, uint32_t ds_ops_24bit,
                                                uint32_t front_wmask, uint32_t back_wmask) {
    if (!stencil_test_enable) return false;
    uint32_t wmasks[2] = {front_wmask & 0xFFu, back_wmask & 0xFFu};
    for (int f = 0; f < 2; f++) {
        if (!wmasks[f]) continue;
        uint32_t o = f ? (ds_ops_24bit >> 12) : ds_ops_24bit;
        if (vk_mali_nvn_stencil_op_writes((uint8_t)(o & 0xFu)) ||
            vk_mali_nvn_stencil_op_writes((uint8_t)((o >> 4) & 0xFu)) ||
            vk_mali_nvn_stencil_op_writes((uint8_t)((o >> 8) & 0xFu)))
            return true;
    }
    return false;
}

// Checks whether a draw can mutate the depth buffer under Vulkan depth-test / depth-write rules.
static inline bool vk_mali_depth_state_writes(bool depth_test_enable, bool depth_write_enable) {
    return depth_test_enable && depth_write_enable;
}

// Emit VK_ATTACHMENT_STORE_OP_NONE strictly for unwritten LOAD_OP_LOAD attachments (preserving cleared contents).
static inline VkAttachmentStoreOp vk_mali_choose_store_op(VkAttachmentLoadOp load_op, bool written_in_pass) {
    if (load_op == VK_ATTACHMENT_LOAD_OP_LOAD && !written_in_pass) return VK_ATTACHMENT_STORE_OP_NONE;
    return VK_ATTACHMENT_STORE_OP_STORE;
}
