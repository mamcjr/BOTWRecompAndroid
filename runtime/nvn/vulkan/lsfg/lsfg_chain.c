// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// Ported to C for Banana Native from Eden's src/video_core/renderer_vulkan/present/lsfg_{common,chain,mipmaps,alpha,beta,gamma,delta,generate}.cpp: the compute passes of
// Lossless Scaling's frame generation (performance set) over two input frames. Per game frame: mipmaps of the luminance-like flow input (7 levels), alpha (4 stages per
// level, history of 3 frames), beta (5 stages); per generated frame: gamma at each level and delta at the 3 finest, then generate into the output image. The images,
// descriptor sets and pass order are Eden's. Differences: every chain image is put in GENERAL layout once when the chain is made, so the barriers between passes are
// global memory barriers (Eden's per-image barriers keep the layout GENERAL as well); the pipelines of one pass are made once and shared by the levels that run it; the
// outputs are the frame generation's own images (vka_framegen.c) instead of the swapchain frames, and their descriptor sets are written when the chain is made
#if defined(__ANDROID__)
#include "lsfg_host.h"
#include "lsfg.h"
#include <stdlib.h>
#include <string.h>

#define LSFG_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define LSFG_FLOW_FORMAT VK_FORMAT_R8_UNORM
#define LSFG_MOTION_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
enum { HIST = 3, MIPS = 7, ALPHA_STAGES = 4, BETA_STAGES = 5, BETA_OUTPUTS = 6, GAMMA_STAGES = 5, DELTA_STAGES = 10, DELTA_N = 3, FIRST_DELTA_LEVEL = 4,
       MAX_IMAGES = 192, MAX_UBO = 48, DESCRIPTORS_PER_TYPE = 4096, FIXED_SETS = 64, SETS_PER_SLOT = 112 };

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkExtent2D ext; VkFormat fmt; } limg;
typedef struct { VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe; } lpass;
typedef struct { struct { float ts; int fi, fis; } key; VkDeviceSize off; } lubo;

struct lsfg_chain {
    int bad;
    VkDescriptorPool pool; VkCommandPool cpool;
    VkSampler samp, samp_border, samp_edge;
    VkBuffer ubuf; VkDeviceMemory umem; uint8_t *umap; VkDeviceSize ustride; lubo ubo[MAX_UBO]; int nubo;
    limg imgs[MAX_IMAGES]; int nimg;
    limg *frames[2];
    // mipmaps
    lpass mip_pass; VkDescriptorSet mip_sets[2]; VkExtent2D flow_ext; limg *mip[MIPS];
    // alpha
    lpass alpha_pass[ALPHA_STAGES];
    struct { limg *input, *t1, *t2, *t3[2], *out[HIST][2]; VkDescriptorSet sets[ALPHA_STAGES - 1], last[HIST]; } alpha[MIPS];
    // beta
    lpass beta_pass[BETA_STAGES];
    VkDescriptorSet beta_first[HIST], beta_sets[BETA_STAGES - 1]; limg *beta_t1[2], *beta_t2[2], *beta_out[BETA_OUTPUTS];
    // gamma
    lpass gamma_pass[GAMMA_STAGES];
    struct { limg *t1[3], *t2[2], *out; struct { VkDescriptorSet first[HIST], sets[GAMMA_STAGES - 1]; } g[LSFG_GENERATION_SLOTS]; } gamma[MIPS];
    // delta
    lpass delta_pass[DELTA_STAGES];
    struct { limg *t1[3], *t2[2], *out1, *out2; struct { VkDescriptorSet first[HIST], sixth[HIST], sets[DELTA_STAGES - 2]; } g[LSFG_GENERATION_SLOTS]; } delta[DELTA_N];
    // generate
    lpass gen_pass; VkDescriptorSet gen_sets[LSFG_GENERATION_SLOTS][LSFG_MAX_GENERATIONS][2]; VkExtent2D out_ext;
};

// Entry points the replay's table (vka.h) does not carry
#define LSFG_FNS(X) X(vkDestroyPipeline) X(vkDestroyPipelineLayout) X(vkDestroyDescriptorSetLayout) X(vkDestroyDescriptorPool) X(vkDestroyCommandPool)
static struct { LSFG_FNS(VKA_DECL) } lk;
static int lk_load(void) {
    static int state;
    if (!state) {
        state = 1;
#define LK_LOAD(f) lk.f = (PFN_##f)vkGetDeviceProcAddr(g_vk.dev, #f); if (!lk.f) { VRED("lsfg: no %s", #f); state = -1; }
        LSFG_FNS(LK_LOAD)
#undef LK_LOAD
    }
    return state > 0;
}

static size_t gen_slot(size_t count, size_t gen) { return (count - 1) * count / 2 + gen; }
static float slot_timestamp(size_t slot) {
    size_t count = 1; while (gen_slot(count + 1, 0) <= slot) count++;
    return (float)(slot - gen_slot(count, 0) + 1) / (float)(count + 1);
}
static uint32_t groups(uint32_t size, uint32_t shift) { return (size + (1u << shift) - 1) >> shift; }

// ---------- Resources ----------
static limg *img_new(lsfg_chain *c, VkExtent2D ext, VkFormat fmt) {
    if (c->bad) return NULL;
    if (c->nimg >= MAX_IMAGES) { VRED("lsfg: more than %d images", MAX_IMAGES); c->bad = 1; return NULL; }
    limg *im = &c->imgs[c->nimg++];
    im->ext = (VkExtent2D){ext.width ? ext.width : 1, ext.height ? ext.height : 1}; im->fmt = fmt;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {im->ext.width, im->ext.height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkResult r = vkCreateImage(g_vk.dev, &ci, NULL, &im->img);
    VkMemoryRequirements mr = {0}; if (!r) vkGetImageMemoryRequirements(g_vk.dev, im->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vkAllocateMemory(g_vk.dev, &ai, NULL, &im->mem);
    if (!r) r = vkBindImageMemory(g_vk.dev, im->img, im->mem, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!r) r = vkCreateImageView(g_vk.dev, &vi, NULL, &im->view);
    if (r) { VRED("lsfg: image %ux%u format %d: %d", im->ext.width, im->ext.height, fmt, r); c->bad = 1; return NULL; }
    return im;
}

static VkSampler sampler_new(lsfg_chain *c, VkSamplerAddressMode mode, VkCompareOp op, int white) {
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = mode, .addressModeV = mode, .addressModeW = mode, .compareOp = op, .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(g_vk.dev, &si, NULL, &s)) { VRED("lsfg: sampler"); c->bad = 1; }
    return s;
}

// The constants of a pass (Eden's LsfgConstants, 48 bytes) at their offset in the chain's uniform buffer; one entry per distinct (timestamp, first_iter, first_iter_s)
typedef struct { uint32_t input_offset[2], first_iter, first_iter_s, advanced_color_kind, hdr_support; float resolution_inv_scale, timestamp, ui_threshold; uint32_t pad[3]; } lconst;
_Static_assert(sizeof(lconst) == 48, "LsfgConstants");
static VkDeviceSize ubo_get(lsfg_chain *c, float flow_scale, float ts, int fi, int fis) {
    for (int i = 0; i < c->nubo; i++) if (c->ubo[i].key.ts == ts && c->ubo[i].key.fi == fi && c->ubo[i].key.fis == fis) return c->ubo[i].off;
    if (c->nubo >= MAX_UBO) { VRED("lsfg: more than %d constant blocks", MAX_UBO); c->bad = 1; return 0; }
    lubo *u = &c->ubo[c->nubo]; u->key.ts = ts; u->key.fi = fi; u->key.fis = fis; u->off = (VkDeviceSize)c->nubo * c->ustride; c->nubo++;
    lconst k = {{0, 0}, fi ? 1u : 0u, fis ? 1u : 0u, 0, 0, 1.0f / flow_scale, ts, 0.5f, {0, 0, 0}};
    if (c->umap) memcpy(c->umap + u->off, &k, sizeof k);
    return u->off;
}

// A pass: the compute pipeline of shader id with its set layout, one descriptor per binding in the order of types (the bindings lsfg_dll.c renumbered)
typedef struct { int n; VkDescriptorType t; } lbind;
static void pass_new(lsfg_chain *c, lpass *p, const lsfg_modules *mods, uint32_t id, const lbind *b, int nb) {
    if (c->bad) return;
    VkDescriptorSetLayoutBinding bs[32]; uint32_t n = 0;
    for (int i = 0; i < nb; i++) for (int j = 0; j < b[i].n && n < 32; j++, n++) bs[n] = (VkDescriptorSetLayoutBinding){n, b[i].t, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = bs};
    VkResult r = vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &p->dsl);
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &p->dsl};
    if (!r) r = vkCreatePipelineLayout(g_vk.dev, &li, NULL, &p->pl);
    const lsfg_module *m = lsfg_module_get(mods, id);
    VkShaderModule sm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = m ? m->nwords * 4u : 0, .pCode = m ? m->words : NULL};
    if (!r) r = m ? vkCreateShaderModule(g_vk.dev, &mi, NULL, &sm) : VK_ERROR_INITIALIZATION_FAILED;
    VkComputePipelineCreateInfo pi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}, .layout = p->pl};
    if (!r) r = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &p->pipe);
    if (sm) vkDestroyShaderModule(g_vk.dev, sm, NULL);
    if (r) { VRED("lsfg: pipeline of shader %u: %d", id, r); c->bad = 1; }
}
static VkDescriptorSet set_new(lsfg_chain *c, const lpass *p) {
    if (c->bad) return VK_NULL_HANDLE;
    VkDescriptorSet s = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = c->pool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl};
    VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &s);
    if (r) { VRED("lsfg: descriptor set: %d", r); c->bad = 1; }
    return s;
}

// The descriptor writer (Eden's LsfgDescriptorWriter): bindings 0, 1, 2, ... in the order they are added; a NULL image is a null descriptor
typedef struct { lsfg_chain *c; VkDescriptorSet set; uint32_t n; VkWriteDescriptorSet w[32]; VkDescriptorImageInfo ii[32]; VkDescriptorBufferInfo bi[32]; } lwriter;
static void w_begin(lwriter *w, lsfg_chain *c, VkDescriptorSet s) { w->c = c; w->set = s; w->n = 0; }
static void w_image(lwriter *w, VkDescriptorType t, VkSampler s, VkImageView v) {
    if (w->n >= 32) return;
    w->ii[w->n] = (VkDescriptorImageInfo){s, v, v ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED};
    w->w[w->n] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = w->set, .dstBinding = w->n, .descriptorCount = 1, .descriptorType = t, .pImageInfo = &w->ii[w->n]};
    w->n++;
}
static void w_samp(lwriter *w, VkSampler s) { w_image(w, VK_DESCRIPTOR_TYPE_SAMPLER, s, VK_NULL_HANDLE); }
static void w_tex(lwriter *w, const limg *im) { w_image(w, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_NULL_HANDLE, im ? im->view : VK_NULL_HANDLE); }
static void w_texs(lwriter *w, limg *const *ims, int n) { for (int i = 0; i < n; i++) w_tex(w, ims[i]); }
static void w_store(lwriter *w, const limg *im) { w_image(w, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE, im ? im->view : VK_NULL_HANDLE); }
static void w_stores(lwriter *w, limg *const *ims, int n) { for (int i = 0; i < n; i++) w_store(w, ims[i]); }
static void w_view(lwriter *w, VkImageView v) { w_image(w, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE, v); }
static void w_ubo(lwriter *w, VkDeviceSize off) {
    if (w->n >= 32) return;
    w->bi[w->n] = (VkDescriptorBufferInfo){w->c->ubuf, off, sizeof(lconst)};
    w->w[w->n] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = w->set, .dstBinding = w->n, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &w->bi[w->n]};
    w->n++;
}
static void w_end(lwriter *w) { if (!w->c->bad && w->n) vkUpdateDescriptorSets(g_vk.dev, w->n, w->w, 0, NULL); }

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst, VkAccessFlags da) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da};
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void cbar(VkCommandBuffer cb) {   // one compute pass's writes before the next pass reads or overwrites them
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}
static void run(VkCommandBuffer cb, const lpass *p, VkDescriptorSet s, uint32_t gx, uint32_t gy) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pl, 0, 1, &s, 0, NULL);
    vkCmdDispatch(cb, gx, gy, 1);
}

#define S_UBO VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
#define S_SMP VK_DESCRIPTOR_TYPE_SAMPLER
#define S_TEX VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
#define S_STO VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
#define PASS(p, id, ...) do { const lbind b_[] = {__VA_ARGS__}; pass_new(c, p, mods, id, b_, (int)(sizeof b_ / sizeof b_[0])); } while (0)

static VkExtent2D halve(VkExtent2D e) { return (VkExtent2D){(e.width + 1) >> 1, (e.height + 1) >> 1}; }

// ---------- The chain ----------
lsfg_chain *lsfg_chain_new(const lsfg_modules *mods, VkExtent2D ext, float flow_scale, VkImage outs[LSFG_MAX_GENERATIONS], VkImageView out_views[LSFG_MAX_GENERATIONS]) {
    if (!lk_load()) return NULL;
    lsfg_chain *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    lwriter w;
    // Descriptor pool, samplers, uniform buffer
    VkDescriptorPoolSize ps[4] = {{S_UBO, DESCRIPTORS_PER_TYPE}, {S_SMP, DESCRIPTORS_PER_TYPE}, {S_TEX, DESCRIPTORS_PER_TYPE}, {S_STO, DESCRIPTORS_PER_TYPE}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = FIXED_SETS + SETS_PER_SLOT * LSFG_GENERATION_SLOTS, .poolSizeCount = 4, .pPoolSizes = ps};
    if (vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &c->pool)) { VRED("lsfg: descriptor pool"); c->bad = 1; }
    c->samp = sampler_new(c, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, 0);
    c->samp_border = sampler_new(c, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, 1);
    c->samp_edge = sampler_new(c, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS, 0);
    VkDeviceSize al = g_vk.props.limits.minUniformBufferOffsetAlignment ? g_vk.props.limits.minUniformBufferOffsetAlignment : 256; c->ustride = (sizeof(lconst) + al - 1) / al * al;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = c->ustride * MAX_UBO, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkResult r = vkCreateBuffer(g_vk.dev, &bi, NULL, &c->ubuf);
    VkMemoryRequirements mr = {0}; if (!r) vkGetBufferMemoryRequirements(g_vk.dev, c->ubuf, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vkAllocateMemory(g_vk.dev, &ai, NULL, &c->umem);
    if (!r) r = vkBindBufferMemory(g_vk.dev, c->ubuf, c->umem, 0);
    void *map = NULL; if (!r) r = vkMapMemory(g_vk.dev, c->umem, 0, VK_WHOLE_SIZE, 0, &map);
    if (r) { VRED("lsfg: uniform buffer: %d", r); c->bad = 1; } else c->umap = map;

    // Inputs
    for (int i = 0; i < 2; i++) c->frames[i] = img_new(c, ext, LSFG_FORMAT);

    // Mipmaps (LsfgMipmaps)
    PASS(&c->mip_pass, LSFG_SH_MIPMAPS, {1, S_UBO}, {1, S_SMP}, {1, S_TEX}, {MIPS, S_STO});
    c->flow_ext = (VkExtent2D){(uint32_t)(ext.width * flow_scale), (uint32_t)(ext.height * flow_scale)};
    if (!c->flow_ext.width) c->flow_ext.width = 1;
    if (!c->flow_ext.height) c->flow_ext.height = 1;
    for (int i = 0; i < MIPS; i++) c->mip[i] = img_new(c, (VkExtent2D){c->flow_ext.width >> i, c->flow_ext.height >> i}, LSFG_FLOW_FORMAT);
    for (int i = 0; i < 2; i++) {
        c->mip_sets[i] = set_new(c, &c->mip_pass);
        w_begin(&w, c, c->mip_sets[i]); w_ubo(&w, ubo_get(c, flow_scale, 0, 0, 0)); w_samp(&w, c->samp); w_tex(&w, c->frames[i]); w_stores(&w, c->mip, MIPS); w_end(&w);
    }

    // Alpha (LsfgAlphaPasses, LsfgAlpha per level)
    PASS(&c->alpha_pass[0], 290, {1, S_SMP}, {1, S_TEX}, {1, S_STO});
    PASS(&c->alpha_pass[1], 291, {1, S_SMP}, {1, S_TEX}, {1, S_STO});
    PASS(&c->alpha_pass[2], 292, {1, S_SMP}, {1, S_TEX}, {2, S_STO});
    PASS(&c->alpha_pass[3], 293, {1, S_SMP}, {2, S_TEX}, {2, S_STO});
    for (int l = 0; l < MIPS && !c->bad; l++) {
        __typeof__(c->alpha[0]) *a = &c->alpha[l];
        a->input = c->mip[l];
        VkExtent2D half = halve(a->input->ext), quarter = halve(half);
        a->t1 = img_new(c, half, LSFG_FORMAT); a->t2 = img_new(c, half, LSFG_FORMAT);
        for (int i = 0; i < 2; i++) { a->t3[i] = img_new(c, quarter, LSFG_FORMAT); for (int j = 0; j < HIST; j++) a->out[j][i] = img_new(c, quarter, LSFG_FORMAT); }
        for (int s = 0; s < ALPHA_STAGES - 1; s++) a->sets[s] = set_new(c, &c->alpha_pass[s]);
        for (int h = 0; h < HIST; h++) a->last[h] = set_new(c, &c->alpha_pass[3]);
        if (c->bad) break;
        w_begin(&w, c, a->sets[0]); w_samp(&w, c->samp); w_tex(&w, a->input); w_store(&w, a->t1); w_end(&w);
        w_begin(&w, c, a->sets[1]); w_samp(&w, c->samp); w_tex(&w, a->t1); w_store(&w, a->t2); w_end(&w);
        w_begin(&w, c, a->sets[2]); w_samp(&w, c->samp); w_tex(&w, a->t2); w_stores(&w, a->t3, 2); w_end(&w);
        for (int h = 0; h < HIST; h++) { w_begin(&w, c, a->last[h]); w_samp(&w, c->samp); w_texs(&w, a->t3, 2); w_stores(&w, a->out[h], 2); w_end(&w); }
    }

    // Beta (LsfgBeta, on alpha level 0's outputs)
    PASS(&c->beta_pass[0], 298, {1, S_SMP}, {6, S_TEX}, {2, S_STO});
    for (int i = 1; i < BETA_STAGES - 1; i++) PASS(&c->beta_pass[i], 298 + (uint32_t)i, {1, S_SMP}, {2, S_TEX}, {2, S_STO});
    PASS(&c->beta_pass[4], 302, {1, S_UBO}, {1, S_SMP}, {2, S_TEX}, {6, S_STO});
    if (!c->bad) {
        limg *(*in)[2] = c->alpha[0].out;
        VkExtent2D e = in[0][0]->ext;
        for (int i = 0; i < 2; i++) { c->beta_t1[i] = img_new(c, e, LSFG_FORMAT); c->beta_t2[i] = img_new(c, e, LSFG_FORMAT); }
        for (int i = 0; i < BETA_OUTPUTS; i++) c->beta_out[i] = img_new(c, (VkExtent2D){e.width >> i, e.height >> i}, LSFG_FLOW_FORMAT);
        for (int h = 0; h < HIST; h++) c->beta_first[h] = set_new(c, &c->beta_pass[0]);
        for (int s = 0; s < BETA_STAGES - 1; s++) c->beta_sets[s] = set_new(c, &c->beta_pass[s + 1]);
        for (int h = 0; h < HIST && !c->bad; h++) {
            w_begin(&w, c, c->beta_first[h]); w_samp(&w, c->samp_border);
            w_texs(&w, in[(h + 1) % HIST], 2); w_texs(&w, in[(h + 2) % HIST], 2); w_texs(&w, in[h % HIST], 2); w_stores(&w, c->beta_t1, 2); w_end(&w);
        }
        if (!c->bad) {
            w_begin(&w, c, c->beta_sets[0]); w_samp(&w, c->samp); w_texs(&w, c->beta_t1, 2); w_stores(&w, c->beta_t2, 2); w_end(&w);
            w_begin(&w, c, c->beta_sets[1]); w_samp(&w, c->samp); w_texs(&w, c->beta_t2, 2); w_stores(&w, c->beta_t1, 2); w_end(&w);
            w_begin(&w, c, c->beta_sets[2]); w_samp(&w, c->samp); w_texs(&w, c->beta_t1, 2); w_stores(&w, c->beta_t2, 2); w_end(&w);
            w_begin(&w, c, c->beta_sets[3]); w_ubo(&w, ubo_get(c, flow_scale, 0.5f, 0, 0)); w_samp(&w, c->samp); w_texs(&w, c->beta_t2, 2); w_stores(&w, c->beta_out, BETA_OUTPUTS); w_end(&w);
        }
    }

    // Gamma (LsfgGamma, coarsest level first) and delta (LsfgDelta, the three finest)
    PASS(&c->gamma_pass[0], 280, {1, S_UBO}, {2, S_SMP}, {5, S_TEX}, {3, S_STO});
    PASS(&c->gamma_pass[1], 282, {1, S_SMP}, {3, S_TEX}, {2, S_STO});
    PASS(&c->gamma_pass[2], 283, {1, S_SMP}, {2, S_TEX}, {2, S_STO});
    PASS(&c->gamma_pass[3], 284, {1, S_SMP}, {2, S_TEX}, {2, S_STO});
    PASS(&c->gamma_pass[4], 285, {1, S_UBO}, {2, S_SMP}, {4, S_TEX}, {1, S_STO});
    PASS(&c->delta_pass[0], 280, {1, S_UBO}, {2, S_SMP}, {5, S_TEX}, {3, S_STO});
    PASS(&c->delta_pass[1], 286, {1, S_SMP}, {3, S_TEX}, {2, S_STO});
    PASS(&c->delta_pass[2], 287, {1, S_SMP}, {2, S_TEX}, {2, S_STO});
    PASS(&c->delta_pass[3], 288, {1, S_SMP}, {2, S_TEX}, {2, S_STO});
    PASS(&c->delta_pass[4], 289, {1, S_UBO}, {2, S_SMP}, {4, S_TEX}, {1, S_STO});
    PASS(&c->delta_pass[5], 281, {1, S_UBO}, {2, S_SMP}, {6, S_TEX}, {1, S_STO});
    for (int i = 6; i < DELTA_STAGES - 1; i++) PASS(&c->delta_pass[i], 294 + (uint32_t)(i - 6), {1, S_SMP}, {1, S_TEX}, {1, S_STO});
    PASS(&c->delta_pass[9], 297, {1, S_UBO}, {2, S_SMP}, {2, S_TEX}, {1, S_STO});
    for (int i = 0; i < MIPS && !c->bad; i++) {
        int level = MIPS - 1 - i;
        limg *(*in)[2] = c->alpha[level].out;
        limg *flow = c->beta_out[level < BETA_OUTPUTS - 1 ? level : BETA_OUTPUTS - 1];
        limg *prev = i == 0 ? NULL : c->gamma[i - 1].out;
        __typeof__(c->gamma[0]) *g = &c->gamma[i];
        VkExtent2D e = in[0][0]->ext;
        for (int k = 0; k < 3; k++) g->t1[k] = img_new(c, e, LSFG_FORMAT);
        for (int k = 0; k < 2; k++) g->t2[k] = img_new(c, e, LSFG_FORMAT);
        g->out = img_new(c, e, LSFG_MOTION_FORMAT);
        for (int s = 0; s < LSFG_GENERATION_SLOTS && !c->bad; s++) {
            VkDeviceSize ub = ubo_get(c, flow_scale, slot_timestamp((size_t)s), prev == NULL, 0);
            for (int h = 0; h < HIST; h++) g->g[s].first[h] = set_new(c, &c->gamma_pass[0]);
            for (int k = 0; k < GAMMA_STAGES - 1; k++) g->g[s].sets[k] = set_new(c, &c->gamma_pass[k + 1]);
            if (c->bad) break;
            for (int h = 0; h < HIST; h++) {
                w_begin(&w, c, g->g[s].first[h]); w_ubo(&w, ub); w_samp(&w, c->samp_border); w_samp(&w, c->samp_edge);
                w_texs(&w, in[(h + 2) % HIST], 2); w_texs(&w, in[h % HIST], 2); w_tex(&w, prev); w_stores(&w, g->t1, 3); w_end(&w);
            }
            w_begin(&w, c, g->g[s].sets[0]); w_samp(&w, c->samp); w_texs(&w, g->t1, 3); w_stores(&w, g->t2, 2); w_end(&w);
            w_begin(&w, c, g->g[s].sets[1]); w_samp(&w, c->samp); w_texs(&w, g->t2, 2); w_store(&w, g->t1[0]); w_store(&w, g->t1[1]); w_end(&w);
            w_begin(&w, c, g->g[s].sets[2]); w_samp(&w, c->samp); w_tex(&w, g->t1[0]); w_tex(&w, g->t1[1]); w_stores(&w, g->t2, 2); w_end(&w);
            w_begin(&w, c, g->g[s].sets[3]); w_ubo(&w, ub); w_samp(&w, c->samp); w_samp(&w, c->samp_edge); w_texs(&w, g->t2, 2); w_tex(&w, prev); w_tex(&w, flow); w_store(&w, g->out); w_end(&w);
        }
        if (i < FIRST_DELTA_LEVEL || c->bad) continue;
        int di = i - FIRST_DELTA_LEVEL;
        __typeof__(c->delta[0]) *d = &c->delta[di];
        limg *pg = i == FIRST_DELTA_LEVEL ? NULL : c->gamma[i - 1].out;
        limg *p1 = i == FIRST_DELTA_LEVEL ? NULL : c->delta[di - 1].out1, *p2 = i == FIRST_DELTA_LEVEL ? NULL : c->delta[di - 1].out2;
        limg *dflow = c->beta_out[level];
        for (int k = 0; k < 3; k++) d->t1[k] = img_new(c, e, LSFG_FORMAT);
        for (int k = 0; k < 2; k++) d->t2[k] = img_new(c, e, LSFG_FORMAT);
        d->out1 = img_new(c, e, LSFG_MOTION_FORMAT); d->out2 = img_new(c, e, LSFG_MOTION_FORMAT);
        for (int s = 0; s < LSFG_GENERATION_SLOTS && !c->bad; s++) {
            VkDeviceSize ub = ubo_get(c, flow_scale, slot_timestamp((size_t)s), 0, pg == NULL);
            for (int h = 0; h < HIST; h++) { d->g[s].first[h] = set_new(c, &c->delta_pass[0]); d->g[s].sixth[h] = set_new(c, &c->delta_pass[5]); }
            for (int k = 0; k < 4; k++) d->g[s].sets[k] = set_new(c, &c->delta_pass[k + 1]);
            for (int k = 4; k < DELTA_STAGES - 2; k++) d->g[s].sets[k] = set_new(c, &c->delta_pass[k + 2]);
            if (c->bad) break;
            for (int h = 0; h < HIST; h++) {
                w_begin(&w, c, d->g[s].first[h]); w_ubo(&w, ub); w_samp(&w, c->samp_border); w_samp(&w, c->samp_edge);
                w_texs(&w, in[(h + 2) % HIST], 2); w_texs(&w, in[h % HIST], 2); w_tex(&w, pg); w_stores(&w, d->t1, 3); w_end(&w);
                w_begin(&w, c, d->g[s].sixth[h]); w_ubo(&w, ub); w_samp(&w, c->samp_border); w_samp(&w, c->samp_edge);
                w_texs(&w, in[(h + 2) % HIST], 2); w_texs(&w, in[h % HIST], 2); w_tex(&w, pg); w_tex(&w, p1); w_store(&w, d->t2[0]); w_end(&w);
            }
            w_begin(&w, c, d->g[s].sets[0]); w_samp(&w, c->samp); w_texs(&w, d->t1, 3); w_stores(&w, d->t2, 2); w_end(&w);
            w_begin(&w, c, d->g[s].sets[1]); w_samp(&w, c->samp); w_texs(&w, d->t2, 2); w_store(&w, d->t1[0]); w_store(&w, d->t1[1]); w_end(&w);
            w_begin(&w, c, d->g[s].sets[2]); w_samp(&w, c->samp); w_tex(&w, d->t1[0]); w_tex(&w, d->t1[1]); w_stores(&w, d->t2, 2); w_end(&w);
            w_begin(&w, c, d->g[s].sets[3]); w_ubo(&w, ub); w_samp(&w, c->samp); w_samp(&w, c->samp_edge); w_texs(&w, d->t2, 2); w_tex(&w, pg); w_tex(&w, dflow); w_store(&w, d->out1); w_end(&w);
            w_begin(&w, c, d->g[s].sets[4]); w_samp(&w, c->samp); w_tex(&w, d->t2[0]); w_store(&w, d->t1[0]); w_end(&w);
            w_begin(&w, c, d->g[s].sets[5]); w_samp(&w, c->samp); w_tex(&w, d->t1[0]); w_store(&w, d->t2[0]); w_end(&w);
            w_begin(&w, c, d->g[s].sets[6]); w_samp(&w, c->samp); w_tex(&w, d->t2[0]); w_store(&w, d->t1[0]); w_end(&w);
            w_begin(&w, c, d->g[s].sets[7]); w_ubo(&w, ub); w_samp(&w, c->samp); w_samp(&w, c->samp_edge); w_tex(&w, d->t1[0]); w_tex(&w, p2); w_store(&w, d->out2); w_end(&w);
        }
    }

    // Generate (LsfgGenerate): for each slot and output, two sets (which input is the previous frame)
    PASS(&c->gen_pass, LSFG_SH_GENERATE, {1, S_UBO}, {2, S_SMP}, {5, S_TEX}, {1, S_STO});
    c->out_ext = ext;
    for (int s = 0; s < LSFG_GENERATION_SLOTS && !c->bad; s++) {
        VkDeviceSize ub = ubo_get(c, flow_scale, slot_timestamp((size_t)s), 0, 0);
        for (int t = 0; t < LSFG_MAX_GENERATIONS; t++) for (int k = 0; k < 2 && !c->bad; k++) {
            c->gen_sets[s][t][k] = set_new(c, &c->gen_pass);
            if (c->bad) break;
            w_begin(&w, c, c->gen_sets[s][t][k]); w_ubo(&w, ub); w_samp(&w, c->samp); w_samp(&w, c->samp_edge);
            w_tex(&w, c->frames[1 - k]); w_tex(&w, c->frames[k]); w_tex(&w, c->gamma[MIPS - 1].out); w_tex(&w, c->delta[DELTA_N - 1].out1); w_tex(&w, c->delta[DELTA_N - 1].out2);
            w_view(&w, out_views[t]); w_end(&w);
        }
    }

    // Every image in GENERAL layout, once (the outputs too)
    if (!c->bad) {
        VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_vk.qfam};
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        r = vkCreateCommandPool(g_vk.dev, &cpi, NULL, &c->cpool);
        cai.commandPool = c->cpool;
        if (!r) r = vkAllocateCommandBuffers(g_vk.dev, &cai, &cb);
        VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (!r) r = vkBeginCommandBuffer(cb, &cbi);
        if (!r) {
            for (int i = 0; i < c->nimg; i++) vka_img_barrier(cb, c->imgs[i].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            for (int t = 0; t < LSFG_MAX_GENERATIONS; t++) vka_img_barrier(cb, outs[t], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            r = vkEndCommandBuffer(cb);
        }
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        if (!r) { lsfg_queue_lock(); r = vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE); if (!r) r = vkQueueWaitIdle(g_vk.queue); lsfg_queue_unlock(); }
        if (r) { VRED("lsfg: layout setup: %d", r); c->bad = 1; }
    }
    if (c->bad) { lsfg_chain_free(c); return NULL; }
    VLOG("lsfg: chain %ux%u, flow %ux%u (scale %.2f): %d images, %d constant blocks", ext.width, ext.height, c->flow_ext.width, c->flow_ext.height, flow_scale, c->nimg, c->nubo);
    return c;
}

static void pass_free(lpass *p) {
    if (p->pipe) lk.vkDestroyPipeline(g_vk.dev, p->pipe, NULL);
    if (p->pl) lk.vkDestroyPipelineLayout(g_vk.dev, p->pl, NULL);
    if (p->dsl) lk.vkDestroyDescriptorSetLayout(g_vk.dev, p->dsl, NULL);
}
void lsfg_chain_free(lsfg_chain *c) {
    if (!c) return;
    for (int i = 0; i < c->nimg; i++) {
        if (c->imgs[i].view) vkDestroyImageView(g_vk.dev, c->imgs[i].view, NULL);
        if (c->imgs[i].img) vkDestroyImage(g_vk.dev, c->imgs[i].img, NULL);
        if (c->imgs[i].mem) vkFreeMemory(g_vk.dev, c->imgs[i].mem, NULL);
    }
    pass_free(&c->mip_pass); pass_free(&c->gen_pass);
    for (int i = 0; i < ALPHA_STAGES; i++) pass_free(&c->alpha_pass[i]);
    for (int i = 0; i < BETA_STAGES; i++) pass_free(&c->beta_pass[i]);
    for (int i = 0; i < GAMMA_STAGES; i++) pass_free(&c->gamma_pass[i]);
    for (int i = 0; i < DELTA_STAGES; i++) pass_free(&c->delta_pass[i]);
    if (c->pool) lk.vkDestroyDescriptorPool(g_vk.dev, c->pool, NULL);
    if (c->cpool) lk.vkDestroyCommandPool(g_vk.dev, c->cpool, NULL);
    if (c->samp) vkDestroySampler(g_vk.dev, c->samp, NULL);
    if (c->samp_border) vkDestroySampler(g_vk.dev, c->samp_border, NULL);
    if (c->samp_edge) vkDestroySampler(g_vk.dev, c->samp_edge, NULL);
    if (c->ubuf) vkDestroyBuffer(g_vk.dev, c->ubuf, NULL);
    if (c->umem) vkFreeMemory(g_vk.dev, c->umem, NULL);
    free(c);
}

VkImage lsfg_chain_input(lsfg_chain *c, uint64_t count) { return c->frames[count % 2]->img; }

// LsfgChain::DispatchShared: mipmaps of the new frame, the 4 alpha stages at every level, beta
void lsfg_chain_shared(lsfg_chain *c, VkCommandBuffer cb, uint64_t count) {
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    run(cb, &c->mip_pass, c->mip_sets[count % 2], groups(c->flow_ext.width, 6), groups(c->flow_ext.height, 6));
    for (int s = 0; s < ALPHA_STAGES; s++) {
        cbar(cb);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->alpha_pass[s].pipe);
        for (int l = 0; l < MIPS; l++) {
            VkExtent2D e = s < 2 ? c->alpha[l].t1->ext : c->alpha[l].t3[0]->ext;
            VkDescriptorSet set = s < ALPHA_STAGES - 1 ? c->alpha[l].sets[s] : c->alpha[l].last[count % HIST];
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->alpha_pass[s].pl, 0, 1, &set, 0, NULL);
            vkCmdDispatch(cb, groups(e.width, 3), groups(e.height, 3), 1);
        }
    }
    VkExtent2D e = c->beta_t1[0]->ext; uint32_t gx = groups(e.width, 3), gy = groups(e.height, 3);
    cbar(cb); run(cb, &c->beta_pass[0], c->beta_first[count % HIST], gx, gy);
    for (int s = 1; s < BETA_STAGES - 1; s++) { cbar(cb); run(cb, &c->beta_pass[s], c->beta_sets[s - 1], gx, gy); }
    cbar(cb); run(cb, &c->beta_pass[4], c->beta_sets[3], groups(e.width, 5), groups(e.height, 5));
}

// LsfgChain::DispatchGeneration: gamma at each level (delta at the finest three) for the generation's slot, then generate into its output
void lsfg_chain_generate(lsfg_chain *c, VkCommandBuffer cb, uint64_t count, size_t ngen, size_t gen) {
    size_t slot = gen_slot(ngen, gen); int h = (int)(count % HIST);
    for (int i = 0; i < MIPS; i++) {
        __typeof__(c->gamma[0]) *g = &c->gamma[i];
        VkExtent2D e = g->t1[0]->ext; uint32_t gx = groups(e.width, 3), gy = groups(e.height, 3);
        cbar(cb); run(cb, &c->gamma_pass[0], g->g[slot].first[h], gx, gy);
        for (int k = 1; k < GAMMA_STAGES; k++) { cbar(cb); run(cb, &c->gamma_pass[k], g->g[slot].sets[k - 1], gx, gy); }
        if (i < FIRST_DELTA_LEVEL) continue;
        __typeof__(c->delta[0]) *d = &c->delta[i - FIRST_DELTA_LEVEL];
        cbar(cb); run(cb, &c->delta_pass[0], d->g[slot].first[h], gx, gy);
        for (int k = 1; k <= 4; k++) { cbar(cb); run(cb, &c->delta_pass[k], d->g[slot].sets[k - 1], gx, gy); }
        cbar(cb); run(cb, &c->delta_pass[5], d->g[slot].sixth[h], gx, gy);
        for (int k = 6; k < DELTA_STAGES; k++) { cbar(cb); run(cb, &c->delta_pass[k], d->g[slot].sets[k - 2], gx, gy); }
    }
    cbar(cb);
    run(cb, &c->gen_pass, c->gen_sets[slot][gen][count % 2], groups(c->out_ext.width, 4), groups(c->out_ext.height, 4));
}
#endif
