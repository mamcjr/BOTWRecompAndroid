// Regression test for write-back render targets first drawn into with LOAD_OP_LOAD (#296 / #438).
// Replays recorded upload/main command buffers against byte-backed mock images and validates Vulkan hazards.
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../runtime/nvn/formats.c"
#include "../runtime/nvn/tegra.c"
#ifndef TEXTURES_C
#define TEXTURES_C "../runtime/nvn/vulkan/textures.c"
#endif
#ifndef EXEC_C
#define EXEC_C "../runtime/nvn/vulkan/exec.c"
#endif
#include TEXTURES_C
#include EXEC_C

// ---- external runtime stubs
VkG g_vk = {.sync2 = true, .dyn_render = true, .eds = true, .eds2 = true};
Settings g_settings;
bool g_benchmark = false, g_widescreen = false;
#ifdef __ANDROID__
_Atomic uint64_t g_bench_perf[BP_COUNT];
#endif
int g_present_interval = 1, g_debug_ab_pause = 0, g_pass_layer = 0;
uint32_t g_tex_active_gen = 0;
uint64_t g_tex_drain_ns = 0, g_tex_drains = 0, g_guest_lo = 0, g_guest_limit = 0, maxwell_gmem_base = 0;
uint64_t g_tex_stat_init = 0, g_tex_stat_reinit = 0, g_tex_stat_reused = 0, g_tex_stat_fin_owner = 0, g_tex_stat_fin_copy = 0, g_tex_stat_reg = 0;
__thread uint64_t g_blocked_ns = 0;
void *g_guest_base = NULL;
NvnTexture *g_pass_t0 = NULL;
TexBackend *g_dump_sampled = NULL;
static uint64_t s_frame_counter = 1;
static bool mock_turnip = false, mock_pt_enabled = true;
static uint8_t mock_dirty[4096];

void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *fmt, ...) { (void)fmt; abort(); }
void vk_device_fault_report(void) {}
WdMark vk_wd_enter(uint32_t stage, uint64_t arg) { (void)stage, (void)arg; return (WdMark){0}; }
void vk_wd_leave(WdMark m) { (void)m; }
bool vk_driver_is_qualcomm(void) { return false; }
bool vk_driver_is_radv(void) { return false; }
bool vk_driver_is_turnip(void) { return mock_turnip; }
static uint32_t mock_mali_calls = 0;
bool vk_driver_is_mali(void) { mock_mali_calls++; return g_vk.props.vendorID == 0x13B5; }
bool vk_turnip_path_on(int s) { (void)s; return mock_turnip; }
bool vk_turnip_heap_on(int s) { (void)s; return false; }
bool vk_async_on(void) { return false; }
void vk_async_drain(void) {}
uint64_t vk_async_frame_work_ns(void) { return 0; }
uint64_t vk_async_frame_cpu_ns(void) { return 0; }
void vk_timing_trace_counter(const VkTimingCounter *s) { (void)s; }
void vk_timing_trace_wait(int k, uint64_t ns) { (void)k, (void)ns; }
void vk_timing_trace_query_failure(VkResult r, uint32_t n, uint64_t seq) { (void)r, (void)n, (void)seq; }
bool vk_timing_trace_on(void) { return false; }
void vk_timing_trace_submission(uint64_t ns, bool p) { (void)ns, (void)p; }
void vk_rp_collect(uint64_t seq) { (void)seq; }
void vk_rp_forget_view(VkImageView v) { (void)v; }
uint64_t vk_frame_counter(void) { return s_frame_counter; }
uint64_t renderer_present_count(void) { return s_frame_counter; }
int vk_between_fps(void) { return 30; }
int fps_mode_now(void) { return 0; }
void benchmark_frame(double a, double b, double c, double d, double e) { (void)a, (void)b, (void)c, (void)d, (void)e; }
void vk_save_png(const char *p, const uint8_t *r, uint32_t w, uint32_t h) { (void)p, (void)r, (void)w, (void)h; }
void vk_memfd_probe(void) {}
const float *vk_shot_crop(void) { return NULL; }
uint64_t nvn_watch_texaddr(void) { return 0; }
bool widescreen_in_menu(void) { return false; }
bool widescreen_ui_centred(void) { return false; }
float widescreen_factor(void) { return 1.0f; }
float widescreen_hud_top(float x, float y) { (void)x, (void)y; return 0.0f; }
float widescreen_hud_anchor(float x, float y) { (void)x, (void)y; return 0.0f; }
float widescreen_hud_anchor_text(float x, float y) { (void)x, (void)y; return 0.0f; }
float widescreen_hud_shift(float x, float y) { (void)x, (void)y; return 0.0f; }
float widescreen_hud_shift_text(float x, float y) { (void)x, (void)y; return 0.0f; }
bool widescreen_narrow_vert_plus(void) { return false; }
void widescreen_map_seen(void) {}
bool widescreen_map_open(float wide) { (void)wide; return false; }
bool widescreen_cropped(void) { return false; }
void widescreen_band_seen(void) {}
void widescreen_backdrop_seen(uint64_t h) { (void)h; }
void widescreen_scene_ubo(const float *u, uint64_t s) { (void)u, (void)s; }
float widescreen_effect_stretch(uint64_t h) { (void)h; return 1.0f; }
bool framepace_in_menu(void) { return false; }
const char *game_level_name(void) { return "MainField"; }
int texpatch_identify(const NvnTexture *t) { (void)t; return -1; }
uint32_t texpatch_generation(void) { return 0; }
void texpatch_apply(int id, uint32_t lv, uint8_t *b, uint32_t w, uint32_t h, uint32_t r) { (void)id, (void)lv, (void)b, (void)w, (void)h, (void)r; }
void astc_decode_ldr(const uint8_t *b, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, bool sr, uint8_t *o) { (void)b, (void)w, (void)h, (void)bw, (void)bh, (void)sr, (void)o; }
void bc1_to_astc(const uint8_t *b, uint8_t *a, uint64_t n) { (void)b, (void)a, (void)n; }
const unsigned char *smaa_area_tex(void) { return NULL; }
const unsigned char *smaa_search_tex(void) { return NULL; }
uint64_t device_integer(int id) { (void)id; return 128; }
uint64_t nvn_linear_rt_pitch(uint64_t row_bytes) { return (row_bytes + 127ull) & ~127ull; }
uint32_t nvn_window_origin(void) { return 0; }
uint32_t nvn_depth_mode(void) { return 0; }
void nvn_sync_set_seq(uint64_t g, uint64_t s) { (void)g, (void)s; }
void plat_write_tracking_enable(uint64_t l, uint64_t h) { (void)l, (void)h; }
bool plat_reset_guest(uint64_t l, uint64_t h) { (void)l, (void)h; return true; }
uint64_t kernel_heap_size(void) { return 1ull << 30; }
void vk_frame_suspend(void) {}
void vk_frame_resume(void) {}
VkRenderPass vk_rp_compat(uint32_t n, const VkFormat *c, VkFormat d, VkFormat s, VkSampleCountFlagBits sm) { (void)n, (void)c, (void)d, (void)s, (void)sm; return VK_NULL_HANDLE; }
void vk_shaders_init(void) {}
uint32_t *vk_glsl_to_spirv(const char *s, int stg, const char *n, size_t *nw) { (void)s, (void)stg, (void)n, (void)nw; return NULL; }
uint32_t *vk_glsl_to_spirv_ex(const char *s, int stg, const char *n, size_t *nw, uint64_t *oh) { (void)s, (void)stg, (void)n, (void)nw, (void)oh; return NULL; }
uint32_t *vk_spirv_load_cached(uint64_t h, size_t *nw) { (void)h, (void)nw; return NULL; }
void vk_shader_cache_ensure_dirs(void) {}
const uint8_t *maxwell_dbg_code = NULL;
const uint8_t *maxwell_dbg_vs = NULL;
VkShaderModule vk_module(const uint32_t *spv, size_t n) { (void)spv, (void)n; return VK_NULL_HANDLE; }
int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *o, int m) { (void)spv, (void)n, (void)o, (void)m; return 0; }
VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t s, VkShaderStageFlags stg) { (void)b, (void)n, (void)s, (void)stg; return VK_NULL_HANDLE; }
void maxwell_free(ShaderInfo *s) { (void)s; }
ShaderInfo *maxwell_translate(const uint8_t *c, size_t m, int s, const ShaderKey *k) { (void)c, (void)m, (void)s, (void)k; return NULL; }
ShaderInfo *maxwell_translate_bounded(const uint8_t *c, size_t m, size_t cz, int s, const ShaderKey *k) { (void)c, (void)m, (void)cz, (void)s, (void)k; return NULL; }
uint64_t maxwell_gmem_size = 0;
unsigned maxwell_gmem_miss_stages = ~0u;
bool maxwell_no_int64 = false, maxwell_gmem_map_all = false, maxwell_gmem_miss_log = false, maxwell_brx_window = true;
bool maxwell_static_stacks = true, maxwell_structured = true, maxwell_struct_skips = true, maxwell_no_switch = false;
bool maxwell_no_local_struct = false, maxwell_native_half = true, maxwell_fast_minmax = true, maxwell_cb_window = false;
int g_game_version = 0x190;
volatile int g_debug_frame_request = 0;
VkShaderModule vk_builtin_shader(const char *name) { (void)name; return VK_NULL_HANDLE; }
uint64_t plat_now_ns(void) { return 0; }
uint64_t SDL_GetTicksNS(void) { return 0; }
void SDL_DelayPrecise(uint64_t ns) { (void)ns; }
int SDL_GetNumLogicalCPUCores(void) { return 4; }
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *pw, int *ph) { (void)w; if (pw) *pw = 1280; if (ph) *ph = 720; return true; }
const char *paths_cache_dir(void) { return "/tmp"; }
const char *rt_data_root(void) { return "/tmp"; }
bool framepace_on_title(void) { return false; }
void widescreen_minimap_seen(void) {}
void widescreen_talk_seen(void) {}
const Settings *settings_get(void) { return &g_settings; }
int64_t plat_avail_ram_mb(void) { return 8192; }
int64_t plat_total_ram_mb(void) { return 12288; }
void plat_frame_work_gpu(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { (void)a, (void)b, (void)c, (void)d; }

static void guest_mark_dirty(const void *p, size_t size) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    if (!size || a < HEAP_BASE) return;
    for (uint64_t pg = (a - HEAP_BASE) >> 12; pg <= (a - HEAP_BASE + size - 1) >> 12 && pg < sizeof mock_dirty; pg++) mock_dirty[pg] = 1;
}
static void guest_write(void *dst, const void *src, size_t n) { memcpy(dst, src, n); guest_mark_dirty(dst, n); }
int plat_written_pages(uint64_t lo, uint64_t size, uint64_t *pages, int max, uint64_t *gen) {
    if (gen) *gen = 4096;
    if (!mock_pt_enabled || lo < HEAP_BASE || !size) return -1;
    int n = 0;
    for (uint64_t pg = (lo - HEAP_BASE) >> 12; pg < (lo - HEAP_BASE + size + 4095ull) >> 12 && pg < sizeof mock_dirty; pg++)
        if (mock_dirty[pg]) { mock_dirty[pg] = 0; if (n < max) pages[n] = HEAP_BASE + (pg << 12); n++; }
    return n;
}

// ---- byte-backed mock Vulkan GPU & hazard validator
typedef struct {
    VkImage handle; uint32_t w, h, bpb; uint8_t *px; VkImageLayout layout;
    VkPipelineStageFlags2 pending_write_stage, visible_dst_stage, pending_read_stage;
    VkAccessFlags2 pending_write_access, visible_dst_access;
} MockImg;
typedef struct { VkBuffer handle; uint64_t size; uint8_t *ptr; } MockBuf;
typedef struct { VkImageView handle; MockImg *img; } MockView;
enum { MOP_CLR = 1, MOP_B2I, MOP_COPY, MOP_PASS, MOP_DRAW, MOP_SMP, MOP_END, MOP_I2B, MOP_BAR };
typedef struct {
    int op; MockImg *img, *dst_img, *att[8], *bar_img; MockBuf *buf; VkBufferImageCopy bic; VkImageCopy ic;
    uint32_t rw, rh, ncol, row0, nrows; VkAttachmentLoadOp lop[8]; uint16_t val16, *smp16;
    bool has_mem, has_img; VkImageLayout bo, bn;
    VkPipelineStageFlags2 mss, mds, iss, ids; VkAccessFlags2 msa, mda, isa, ida;
} MockOp;
typedef struct { VkCommandBuffer handle; MockOp ops[512]; int nops; } MockCb;

static MockImg m_imgs[128]; static MockBuf m_bufs[128]; static MockView m_views[256]; static MockCb m_cbs[64];
static int n_imgs, n_bufs, n_views, n_cbs, stat_b2i, stat_in_up, stat_in_main, stat_bar, stat_up_bar, stat_sc_bar, stat_dispatch, stat_cbuf, cbuf_bars[8], mock_hazards;
static MockCb *act_cb; static MockImg *act_att0; static VkAttachmentLoadOp last_lop[8];
static MockImg *find_img(VkImage im) { for (int i = 0; i < n_imgs; i++) if (m_imgs[i].handle == im) return &m_imgs[i]; return NULL; }
static MockBuf *find_buf(VkBuffer b) { for (int i = 0; i < n_bufs; i++) if (m_bufs[i].handle == b) return &m_bufs[i]; return NULL; }
static MockCb *find_cb(VkCommandBuffer c) { for (int i = 0; i < n_cbs; i++) if (m_cbs[i].handle == c) return &m_cbs[i]; return NULL; }

VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *im, VmaAllocation *m, VmaAllocationInfo *in) {
    (void)a, (void)ai, (void)in;
    MockImg *x = &m_imgs[n_imgs++]; memset(x, 0, sizeof *x);
    x->w = ci->extent.width; x->h = ci->extent.height; x->bpb = ci->format == VK_FORMAT_R16_UNORM ? 2 : 8;
    x->layout = ci->initialLayout; x->px = calloc((size_t)x->w * x->h, x->bpb); memset(x->px, 0xDD, (size_t)x->w * x->h * x->bpb);
    x->handle = (VkImage)(uintptr_t)(0x10000 + n_imgs); *im = x->handle; *m = (VmaAllocation)x; return VK_SUCCESS;
}
void vmaDestroyImage(VmaAllocator a, VkImage im, VmaAllocation m) { (void)a, (void)im, (void)m; }
VkResult vmaCreateBuffer(VmaAllocator a, const VkBufferCreateInfo *bi, const VmaAllocationCreateInfo *ai, VkBuffer *b, VmaAllocation *m, VmaAllocationInfo *in) {
    (void)a, (void)ai;
    MockBuf *x = &m_bufs[n_bufs++]; x->size = bi->size; x->ptr = calloc(1, (size_t)bi->size);
    x->handle = (VkBuffer)(uintptr_t)(0x20000 + n_bufs); *b = x->handle; *m = (VmaAllocation)x;
    if (in) { memset(in, 0, sizeof *in); in->pMappedData = x->ptr; }
    return VK_SUCCESS;
}
void vmaDestroyBuffer(VmaAllocator a, VkBuffer b, VmaAllocation m) { (void)a, (void)b, (void)m; }
VkResult vmaInvalidateAllocation(VmaAllocator a, VmaAllocation m, VkDeviceSize o, VkDeviceSize s) { (void)a, (void)m, (void)o, (void)s; return VK_SUCCESS; }
VkResult vmaFlushAllocations(VmaAllocator a, uint32_t n, const VmaAllocation *m, const VkDeviceSize *o, const VkDeviceSize *s) { (void)a, (void)n, (void)m, (void)o, (void)s; return VK_SUCCESS; }
void vmaGetHeapBudgets(VmaAllocator a, VmaBudget *b) { (void)a; memset(b, 0, sizeof *b); }
void vmaGetMemoryProperties(VmaAllocator a, const VkPhysicalDeviceMemoryProperties **p) { (void)a; static VkPhysicalDeviceMemoryProperties mp; *p = &mp; }
void vmaGetMemoryTypeProperties(VmaAllocator a, uint32_t i, VkMemoryPropertyFlags *f) { (void)a, (void)i; *f = 0; }

bool vk_begin_rendering(VkCommandBuffer cb, const VkRenderingInfo *ri, const VkFormat *c, VkFormat ds, VkSampleCountFlagBits sm) {
    (void)c, (void)ds, (void)sm; MockCb *mc = find_cb(cb);
    MockOp op = {.op = MOP_PASS, .rw = ri->renderArea.extent.width, .rh = ri->renderArea.extent.height, .ncol = ri->colorAttachmentCount};
    for (uint32_t i = 0; i < ri->colorAttachmentCount && i < 8; i++) {
        MockImg *im = NULL;
        for (int k = 0; k < n_views; k++) if (m_views[k].handle == ri->pColorAttachments[i].imageView) im = m_views[k].img;
        op.att[i] = im; op.lop[i] = last_lop[i] = ri->pColorAttachments[i].loadOp;
    }
    mc->ops[mc->nops++] = op; act_cb = mc; act_att0 = op.att[0] ? op.att[0] : op.att[1];
    return true;
}
void vk_end_rendering(VkCommandBuffer cb) { MockCb *mc = find_cb(cb); mc->ops[mc->nops++] = (MockOp){.op = MOP_END}; act_cb = NULL; act_att0 = NULL; }
bool vk_rendering_active(void) { return act_cb != NULL; }
static void sim_draw_rows(uint32_t r0, uint32_t n, uint16_t v) { act_cb->ops[act_cb->nops++] = (MockOp){.op = MOP_DRAW, .img = act_att0, .row0 = r0, .nrows = n, .val16 = v}; }
static void sim_sample(NvnTexture *t, uint32_t x, uint32_t y, uint16_t *out) {
    MockImg *im = t && t->backend ? find_img(((TexBackend *)t->backend)->image) : NULL; MockCb *cb = find_cb(vk_cmd());
    if (!im || !cb) { mock_hazards++; return; }
    cb->ops[cb->nops++] = (MockOp){.op = MOP_SMP, .img = im, .row0 = y, .nrows = x, .smp16 = out};
}

static VKAPI_ATTR VkResult VKAPI_CALL mc_cpool(VkDevice d, const VkCommandPoolCreateInfo *c, const VkAllocationCallbacks *a, VkCommandPool *p) { (void)d, (void)c, (void)a; *p = (VkCommandPool)1; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL mr_cpool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) { (void)d, (void)p, (void)f; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL ma_cbs(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *cbs) {
    (void)d; for (uint32_t i = 0; i < ai->commandBufferCount; i++) { MockCb *c = &m_cbs[n_cbs++]; memset(c, 0, sizeof *c); c->handle = (VkCommandBuffer)(uintptr_t)(0x40000 + n_cbs); cbs[i] = c->handle; }
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL mb_cb(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) { (void)bi; find_cb(cb)->nops = 0; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL me_cb(VkCommandBuffer cb) { (void)cb; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL mr_dpool(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f) { (void)d, (void)p, (void)f; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL mc_qpool(VkDevice d, const VkQueryPoolCreateInfo *q, const VkAllocationCallbacks *a, VkQueryPool *p) { (void)d, (void)q, (void)a; *p = VK_NULL_HANDLE; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL md_fence(VkDevice d, VkFence f, const VkAllocationCallbacks *a) { (void)d, (void)f, (void)a; }
static VKAPI_ATTR VkResult VKAPI_CALL mc_fence(VkDevice d, const VkFenceCreateInfo *f, const VkAllocationCallbacks *a, VkFence *o) { (void)d, (void)f, (void)a; *o = VK_NULL_HANDLE; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL m_rfences(VkDevice d, uint32_t n, const VkFence *f) { (void)d, (void)n, (void)f; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL m_wfences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 a, uint64_t t) { (void)d, (void)n, (void)f, (void)a, (void)t; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL m_wsems(VkDevice d, const VkSemaphoreWaitInfo *w, uint64_t t) { (void)d, (void)w, (void)t; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL m_gsem(VkDevice d, VkSemaphore s, uint64_t *v) { (void)d, (void)s; *v = submit_seq; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL m_fprops(VkPhysicalDevice pd, VkFormat f, VkFormatProperties *p) {
    (void)pd, (void)f; memset(p, 0, sizeof *p);
    p->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
}
static VKAPI_ATTR VkResult VKAPI_CALL mc_view(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *v) {
    (void)d, (void)a; MockView *mv = &m_views[n_views++]; mv->img = find_img(ci->image); mv->handle = (VkImageView)(uintptr_t)(0x50000 + n_views); *v = mv->handle; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL md_view(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d, (void)v, (void)a; }
static VKAPI_ATTR void VKAPI_CALL m_clr_col(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearColorValue *cv, uint32_t n, const VkImageSubresourceRange *r) {
    (void)cv, (void)n, (void)r; if (act_cb || l != VK_IMAGE_LAYOUT_GENERAL) mock_hazards++;
    MockCb *c = find_cb(cb); c->ops[c->nops++] = (MockOp){.op = MOP_CLR, .img = find_img(im)};
}
static VKAPI_ATTR void VKAPI_CALL m_clr_ds(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r) { (void)cb, (void)im, (void)l, (void)v, (void)n, (void)r; }
static VKAPI_ATTR void VKAPI_CALL m_b2i(VkCommandBuffer cb, VkBuffer b, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
    if (act_cb || l != VK_IMAGE_LAYOUT_GENERAL) mock_hazards++; MockCb *c = find_cb(cb);
    for (uint32_t k = 0; k < n; k++) {
        c->ops[c->nops++] = (MockOp){.op = MOP_B2I, .buf = find_buf(b), .img = find_img(im), .bic = r[k]};
        if (!r[k].imageSubresource.mipLevel) { stat_b2i++; if (cb == subs[cur_sub].upload) stat_in_up++; else stat_in_main++; }
    }
}
static VKAPI_ATTR void VKAPI_CALL m_i2b(VkCommandBuffer cb, VkImage im, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) {
    if (act_cb || l != VK_IMAGE_LAYOUT_GENERAL) mock_hazards++; MockCb *c = find_cb(cb);
    for (uint32_t k = 0; k < n; k++) c->ops[c->nops++] = (MockOp){.op = MOP_I2B, .img = find_img(im), .buf = find_buf(b), .bic = r[k]};
}
static VKAPI_ATTR void VKAPI_CALL m_copy_img(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    if (act_cb || sl != VK_IMAGE_LAYOUT_GENERAL || dl != VK_IMAGE_LAYOUT_GENERAL) mock_hazards++; MockCb *c = find_cb(cb);
    for (uint32_t k = 0; k < n; k++) c->ops[c->nops++] = (MockOp){.op = MOP_COPY, .img = find_img(s), .dst_img = find_img(d), .ic = r[k]};
}
static VKAPI_ATTR void VKAPI_CALL m_bar2(VkCommandBuffer cb, const VkDependencyInfo *di) {
    stat_bar++; if (cb == subs[cur_sub].upload) stat_up_bar++;
    MockCb *c = find_cb(cb); MockOp op = {.op = MOP_BAR};
    if (di && di->memoryBarrierCount) {
        const VkMemoryBarrier2 *mb = &di->pMemoryBarriers[0];
        op.has_mem = true; op.mss = mb->srcStageMask; op.mds = mb->dstStageMask; op.msa = mb->srcAccessMask; op.mda = mb->dstAccessMask;
    }
    if (di && di->imageMemoryBarrierCount) {
        const VkImageMemoryBarrier2 *ib = &di->pImageMemoryBarriers[0];
        op.has_img = true; op.bar_img = find_img(ib->image); op.bo = ib->oldLayout; op.bn = ib->newLayout;
        op.iss = ib->srcStageMask; op.ids = ib->dstStageMask; op.isa = ib->srcAccessMask; op.ida = ib->dstAccessMask;
        if (ib->oldLayout != VK_IMAGE_LAYOUT_UNDEFINED && cb == subs[cur_sub].main) stat_sc_bar++;
    }
    c->ops[c->nops++] = op;
}
static bool stg_cov(VkPipelineStageFlags2 m, VkPipelineStageFlags2 n) {
    if (m & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) return true;
    if ((m & VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT) && (n & (VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT))) return true;
    return (m & n) == n;
}
static bool acc_cov(VkAccessFlags2 m, VkAccessFlags2 n) {
    if (m & VK_ACCESS_2_MEMORY_READ_BIT) m |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    if (m & VK_ACCESS_2_MEMORY_WRITE_BIT) m |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    return (m & n) == n;
}
static void bar_apply(MockImg *im, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    if (im->pending_read_stage && stg_cov(ss, im->pending_read_stage)) im->pending_read_stage = 0;
    if (im->pending_write_access && stg_cov(ss, im->pending_write_stage) && acc_cov(sa, im->pending_write_access)) {
        im->pending_write_stage = im->pending_write_access = im->visible_dst_stage = im->visible_dst_access = 0;
    }
    im->visible_dst_stage |= ds; im->visible_dst_access |= da;
}
static void chk_r(const MockImg *im, VkPipelineStageFlags2 s, VkAccessFlags2 a) {
    if (im->layout != VK_IMAGE_LAYOUT_GENERAL || im->pending_write_access || !stg_cov(im->visible_dst_stage, s) || !acc_cov(im->visible_dst_access, a)) mock_hazards++;
}
static void chk_w(const MockImg *im, VkPipelineStageFlags2 s, VkAccessFlags2 a) {
    if (im->layout != VK_IMAGE_LAYOUT_GENERAL || im->pending_write_access || im->pending_read_stage || !stg_cov(im->visible_dst_stage, s) || !acc_cov(im->visible_dst_access, a)) mock_hazards++;
}
static VKAPI_ATTR VkResult VKAPI_CALL m_submit2(VkQueue q, uint32_t nsub, const VkSubmitInfo2 *subs2, VkFence f) {
    (void)q, (void)f;
    for (uint32_t s = 0; s < nsub; s++)
        for (uint32_t b = 0; b < subs2[s].commandBufferInfoCount; b++) {
            MockCb *cb = find_cb(subs2[s].pCommandBufferInfos[b].commandBuffer);
            for (int k = 0; cb && k < cb->nops; k++) {
                MockOp *o = &cb->ops[k];
                if (o->op == MOP_BAR) {
                    if (o->has_img && o->bar_img) {
                        if (o->bar_img->layout != o->bo) mock_hazards++;
                        o->bar_img->layout = o->bn;
                        if (!o->iss && o->ids) {
                            o->bar_img->pending_write_stage = o->bar_img->pending_write_access = o->bar_img->pending_read_stage = 0;
                            o->bar_img->visible_dst_stage = o->ids; o->bar_img->visible_dst_access = o->ida;
                        } else bar_apply(o->bar_img, o->iss, o->isa, o->ids, o->ida);
                    }
                    if (o->has_mem) for (int m = 0; m < n_imgs; m++) bar_apply(&m_imgs[m], o->mss, o->msa, o->mds, o->mda);
                } else if (o->op == MOP_CLR && o->img) {
                    chk_w(o->img, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                    memset(o->img->px, 0, (size_t)o->img->w * o->img->h * o->img->bpb);
                    o->img->pending_write_stage |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; o->img->pending_write_access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
                } else if (o->op == MOP_B2I && o->img && o->buf && !o->bic.imageSubresource.mipLevel) {
                    chk_w(o->img, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                    uint32_t w = o->bic.imageExtent.width, h = o->bic.imageExtent.height, bpb = o->img->bpb, rl = o->bic.bufferRowLength ? o->bic.bufferRowLength : w;
                    for (uint32_t y = 0; y < h; y++) memcpy(o->img->px + ((size_t)(o->bic.imageOffset.y + y) * o->img->w + o->bic.imageOffset.x) * bpb, o->buf->ptr + o->bic.bufferOffset + (size_t)y * rl * bpb, (size_t)w * bpb);
                    o->img->pending_write_stage |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; o->img->pending_write_access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
                } else if (o->op == MOP_COPY && o->img && o->dst_img && !o->ic.srcSubresource.mipLevel) {
                    chk_r(o->img, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                    chk_w(o->dst_img, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                    for (uint32_t y = 0; y < o->ic.extent.height; y++) memcpy(o->dst_img->px + ((size_t)(o->ic.dstOffset.y + y) * o->dst_img->w + o->ic.dstOffset.x) * o->img->bpb, o->img->px + ((size_t)(o->ic.srcOffset.y + y) * o->img->w + o->ic.srcOffset.x) * o->img->bpb, (size_t)o->ic.extent.width * o->img->bpb);
                    o->img->pending_read_stage |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; o->dst_img->pending_write_stage |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; o->dst_img->pending_write_access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
                } else if (o->op == MOP_SMP && o->img) {
                    chk_r(o->img, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
                    if (o->smp16) memcpy(o->smp16, o->img->px + ((size_t)o->row0 * o->img->w + o->nrows) * o->img->bpb, 2);
                    o->img->pending_read_stage |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                } else if (o->op == MOP_PASS) {
                    for (uint32_t i = 0; i < o->ncol && i < 8; i++) {
                        MockImg *im = o->att[i]; if (!im) continue;
                        if (o->lop[i] == VK_ATTACHMENT_LOAD_OP_LOAD) { chk_r(im, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT); im->pending_read_stage |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; continue; }
                        if (o->lop[i] != VK_ATTACHMENT_LOAD_OP_CLEAR) continue;
                        chk_w(im, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                        for (uint32_t y = 0; y < (o->rh < im->h ? o->rh : im->h); y++) memset(im->px + (size_t)y * im->w * im->bpb, 0, (size_t)(o->rw < im->w ? o->rw : im->w) * im->bpb);
                        im->pending_write_stage |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; im->pending_write_access |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                    }
                } else if (o->op == MOP_DRAW && o->img) {
                    for (uint32_t y = o->row0; y < o->row0 + o->nrows && y < o->img->h; y++)
                        for (uint32_t x = 0; x < o->img->w; x++) memcpy(o->img->px + ((size_t)y * o->img->w + x) * o->img->bpb, &o->val16, 2);
                    o->img->pending_write_stage |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; o->img->pending_write_access |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                } else if (o->op == MOP_I2B && o->img && o->buf) {
                    chk_r(o->img, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                    uint32_t w = o->bic.imageExtent.width, h = o->bic.imageExtent.height, bpb = o->img->bpb, rl = o->bic.bufferRowLength ? o->bic.bufferRowLength : w;
                    for (uint32_t y = 0; y < h; y++) memcpy(o->buf->ptr + o->bic.bufferOffset + (size_t)y * rl * bpb, o->img->px + ((size_t)(o->bic.imageOffset.y + y) * o->img->w + o->bic.imageOffset.x) * bpb, (size_t)w * bpb);
                    o->img->pending_read_stage |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                }
            }
        }
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL n_vp(VkCommandBuffer c, uint32_t f, uint32_t n, const VkViewport *v) { (void)c, (void)f, (void)n, (void)v; }
static VKAPI_ATTR void VKAPI_CALL n_sc(VkCommandBuffer c, uint32_t f, uint32_t n, const VkRect2D *r) { (void)c, (void)f, (void)n, (void)r; }
static VKAPI_ATTR void VKAPI_CALL n_cbuf(VkCommandBuffer c, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) { (void)c, (void)s, (void)d, (void)n, (void)r; cbuf_bars[stat_cbuf++ & 7] = stat_bar; }
static VKAPI_ATTR void VKAPI_CALL n_blit(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) { (void)c, (void)s, (void)sl, (void)d, (void)dl, (void)n, (void)r, (void)f; }
static VKAPI_ATTR void VKAPI_CALL n_res(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *r) { (void)c, (void)s, (void)sl, (void)d, (void)dl, (void)n, (void)r; }
static VKAPI_ATTR void VKAPI_CALL n_pipe(VkCommandBuffer c, VkPipelineBindPoint p, VkPipeline pi) { (void)c, (void)p, (void)pi; }
static VKAPI_ATTR void VKAPI_CALL n_sets(VkCommandBuffer c, VkPipelineBindPoint p, VkPipelineLayout l, uint32_t f, uint32_t nc, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) { (void)c, (void)p, (void)l, (void)f, (void)nc, (void)s, (void)nd, (void)d; }
static VKAPI_ATTR void VKAPI_CALL n_push(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags s, uint32_t o, uint32_t z, const void *v) { (void)c, (void)l, (void)s, (void)o, (void)z, (void)v; }
static VKAPI_ATTR void VKAPI_CALL n_draw(VkCommandBuffer c, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) { (void)c, (void)v, (void)i, (void)fv, (void)fi; }
static VKAPI_ATTR void VKAPI_CALL n_dispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) { (void)c, (void)x, (void)y, (void)z; stat_dispatch++; }
static VKAPI_ATTR void VKAPI_CALL n_cat(VkCommandBuffer c, uint32_t na, const VkClearAttachment *ca, uint32_t nr, const VkClearRect *cr) { (void)c, (void)na, (void)ca, (void)nr, (void)cr; }
static VKAPI_ATTR void VKAPI_CALL n_rqp(VkCommandBuffer c, VkQueryPool q, uint32_t f, uint32_t n) { (void)c, (void)q, (void)f, (void)n; }
static VKAPI_ATTR void VKAPI_CALL n_ts2(VkCommandBuffer c, VkPipelineStageFlags2 s, VkQueryPool q, uint32_t n) { (void)c, (void)s, (void)q, (void)n; }
static VKAPI_ATTR VkResult VKAPI_CALL n_gqp(VkDevice d, VkQueryPool q, uint32_t f, uint32_t n, size_t s, void *p, VkDeviceSize st2, VkQueryResultFlags fl) { (void)d, (void)q, (void)f, (void)n, (void)s, (void)p, (void)st2, (void)fl; return VK_NOT_READY; }
static VKAPI_ATTR VkResult VKAPI_CALL n_cdp(VkDevice d, const VkDescriptorPoolCreateInfo *c, const VkAllocationCallbacks *a, VkDescriptorPool *p) { (void)d, (void)c, (void)a; *p = VK_NULL_HANDLE; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL n_ads(VkDevice d, const VkDescriptorSetAllocateInfo *c, VkDescriptorSet *s) { (void)d, (void)c, (void)s; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL n_uds(VkDevice d, uint32_t wc, const VkWriteDescriptorSet *w, uint32_t cc, const VkCopyDescriptorSet *c) { (void)d, (void)wc, (void)w, (void)cc, (void)c; }
static VKAPI_ATTR VkResult VKAPI_CALL n_dsl(VkDevice d, const VkDescriptorSetLayoutCreateInfo *c, const VkAllocationCallbacks *a, VkDescriptorSetLayout *l) { (void)d, (void)c, (void)a; *l = VK_NULL_HANDLE; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL n_pl(VkDevice d, const VkPipelineLayoutCreateInfo *c, const VkAllocationCallbacks *a, VkPipelineLayout *l) { (void)d, (void)c, (void)a; *l = VK_NULL_HANDLE; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL n_gp(VkDevice d, VkPipelineCache pc, uint32_t n, const VkGraphicsPipelineCreateInfo *c, const VkAllocationCallbacks *a, VkPipeline *p) { (void)d, (void)pc, (void)n, (void)c, (void)a, (void)p; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL n_dp(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) { (void)d, (void)p, (void)a; }
static VKAPI_ATTR void VKAPI_CALL n_dsm(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) { (void)d, (void)m, (void)a; }
static VKAPI_ATTR void VKAPI_CALL n_dpl(VkDevice d, VkPipelineLayout l, const VkAllocationCallbacks *a) { (void)d, (void)l, (void)a; }
static VKAPI_ATTR void VKAPI_CALL n_ddsl(VkDevice d, VkDescriptorSetLayout l, const VkAllocationCallbacks *a) { (void)d, (void)l, (void)a; }
static VKAPI_ATTR VkResult VKAPI_CALL n_csmp(VkDevice d, const VkSamplerCreateInfo *c, const VkAllocationCallbacks *a, VkSampler *s) { (void)d, (void)c, (void)a; *s = VK_NULL_HANDLE; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL n_mp(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties *m) { (void)p; memset(m, 0, sizeof *m); }
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL n_gpa(VkDevice d, const char *n) { (void)d, (void)n; return NULL; }
static VKAPI_ATTR VkResult VKAPI_CALL n_am(VkDevice d, const VkMemoryAllocateInfo *a, const VkAllocationCallbacks *c, VkDeviceMemory *m) { (void)d, (void)a, (void)c; *m = VK_NULL_HANDLE; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL n_fm(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *c) { (void)d, (void)m, (void)c; }
static VKAPI_ATTR VkResult VKAPI_CALL n_bbm(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) { (void)d, (void)b, (void)m, (void)o; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL n_crbuf(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *c, VkBuffer *b) { (void)d, (void)ci, (void)c; *b = VK_NULL_HANDLE; return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL n_dbuf(VkDevice d, VkBuffer b, const VkAllocationCallbacks *c) { (void)d, (void)b, (void)c; }
static VKAPI_ATTR void VKAPI_CALL n_db(VkCommandBuffer c, float k, float cl, float s) { (void)c, (void)k, (void)cl, (void)s; }
static VKAPI_ATTR void VKAPI_CALL n_bc(VkCommandBuffer c, const float b[4]) { (void)c, (void)b; }
static VKAPI_ATTR void VKAPI_CALL n_scm(VkCommandBuffer c, VkStencilFaceFlags f, uint32_t m) { (void)c, (void)f, (void)m; }
static VKAPI_ATTR void VKAPI_CALL n_bib(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkIndexType t) { (void)c, (void)b, (void)o, (void)t; }
static VKAPI_ATTR void VKAPI_CALL n_bvb(VkCommandBuffer c, uint32_t f, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) { (void)c, (void)f, (void)n, (void)b, (void)o; }
static VKAPI_ATTR void VKAPI_CALL n_di(VkCommandBuffer c, uint32_t ic, uint32_t nc, uint32_t fi, int32_t vo, uint32_t fn) { (void)c, (void)ic, (void)nc, (void)fi, (void)vo, (void)fn; }
static VKAPI_ATTR void VKAPI_CALL n_cm(VkCommandBuffer c, VkCullModeFlags m) { (void)c, (void)m; }
static VKAPI_ATTR void VKAPI_CALL n_ff(VkCommandBuffer c, VkFrontFace f) { (void)c, (void)f; }
static VKAPI_ATTR void VKAPI_CALL n_pt(VkCommandBuffer c, VkPrimitiveTopology t) { (void)c, (void)t; }
static VKAPI_ATTR void VKAPI_CALL n_b32(VkCommandBuffer c, VkBool32 e) { (void)c, (void)e; }
static VKAPI_ATTR void VKAPI_CALL n_dco(VkCommandBuffer c, VkCompareOp o) { (void)c, (void)o; }
static VKAPI_ATTR void VKAPI_CALL n_sop(VkCommandBuffer c, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b, VkStencilOp d, VkCompareOp o) { (void)c, (void)f, (void)a, (void)b, (void)d, (void)o; }
static VKAPI_ATTR void VKAPI_CALL n_svi(VkCommandBuffer c, uint32_t nb, const VkVertexInputBindingDescription2EXT *b, uint32_t na, const VkVertexInputAttributeDescription2EXT *a) { (void)c, (void)nb, (void)b, (void)na, (void)a; }
static VKAPI_ATTR VkResult VKAPI_CALL n_pep(VkDevice d, const VkPipelineInfoKHR *p, uint32_t *n, VkPipelineExecutablePropertiesKHR *o) { (void)d, (void)p, (void)o; if (n) *n = 0; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL n_pes(VkDevice d, const VkPipelineExecutableInfoKHR *p, uint32_t *n, VkPipelineExecutableStatisticKHR *o) { (void)d, (void)p, (void)o; if (n) *n = 0; return VK_SUCCESS; }

PFN_vkCreateDescriptorPool vkCreateDescriptorPool = n_cdp; PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = n_ads;
PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = n_uds; PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = n_dsl;
PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = n_ddsl; PFN_vkDestroyShaderModule vkDestroyShaderModule = n_dsm;
PFN_vkCreatePipelineLayout vkCreatePipelineLayout = n_pl; PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = n_dpl;
PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = n_gp;
PFN_vkDestroyPipeline vkDestroyPipeline = n_dp; PFN_vkCreateSampler vkCreateSampler = n_csmp;
PFN_vkCreateCommandPool vkCreateCommandPool = mc_cpool; PFN_vkResetCommandPool vkResetCommandPool = mr_cpool;
PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = ma_cbs; PFN_vkBeginCommandBuffer vkBeginCommandBuffer = mb_cb;
PFN_vkEndCommandBuffer vkEndCommandBuffer = me_cb; PFN_vkResetDescriptorPool vkResetDescriptorPool = mr_dpool;
PFN_vkCreateQueryPool vkCreateQueryPool = mc_qpool; PFN_vkDestroyFence vkDestroyFence = md_fence;
PFN_vkCreateFence vkCreateFence = mc_fence; PFN_vkResetFences vkResetFences = m_rfences;
PFN_vkWaitForFences vkWaitForFences = m_wfences; PFN_vkWaitSemaphores vkWaitSemaphores = m_wsems;
PFN_vkGetSemaphoreCounterValue vkGetSemaphoreCounterValue = m_gsem; PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = m_fprops;
PFN_vkCreateImageView vkCreateImageView = mc_view; PFN_vkDestroyImageView vkDestroyImageView = md_view;
PFN_vkCmdClearColorImage vkCmdClearColorImage = m_clr_col; PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage = m_clr_ds;
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = m_b2i; PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = m_i2b;
PFN_vkCmdPipelineBarrier2 vkCmdPipelineBarrier2 = m_bar2; PFN_vkQueueSubmit2 vkQueueSubmit2 = m_submit2;
PFN_vkCmdSetViewport vkCmdSetViewport = n_vp; PFN_vkCmdSetScissor vkCmdSetScissor = n_sc;
PFN_vkCmdCopyBuffer vkCmdCopyBuffer = n_cbuf; PFN_vkCmdCopyImage vkCmdCopyImage = m_copy_img;
PFN_vkCmdBlitImage vkCmdBlitImage = n_blit; PFN_vkCmdResolveImage vkCmdResolveImage = n_res;
PFN_vkCmdBindPipeline vkCmdBindPipeline = n_pipe; PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = n_sets;
PFN_vkCmdPushConstants vkCmdPushConstants = n_push; PFN_vkCmdDraw vkCmdDraw = n_draw;
PFN_vkCmdDispatch vkCmdDispatch = n_dispatch;
PFN_vkCmdClearAttachments vkCmdClearAttachments = n_cat; PFN_vkCmdResetQueryPool vkCmdResetQueryPool = n_rqp;
PFN_vkCmdWriteTimestamp2 vkCmdWriteTimestamp2 = n_ts2; PFN_vkGetQueryPoolResults vkGetQueryPoolResults = n_gqp;
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = n_mp;
PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = n_gpa; PFN_vkAllocateMemory vkAllocateMemory = n_am;
PFN_vkFreeMemory vkFreeMemory = n_fm; PFN_vkBindBufferMemory vkBindBufferMemory = n_bbm;
PFN_vkCreateBuffer vkCreateBuffer = n_crbuf; PFN_vkDestroyBuffer vkDestroyBuffer = n_dbuf;
PFN_vkCmdSetDepthBias vkCmdSetDepthBias = n_db; PFN_vkCmdSetBlendConstants vkCmdSetBlendConstants = n_bc;
PFN_vkCmdSetStencilCompareMask vkCmdSetStencilCompareMask = n_scm;
PFN_vkCmdSetStencilWriteMask vkCmdSetStencilWriteMask = n_scm;
PFN_vkCmdSetStencilReference vkCmdSetStencilReference = n_scm;
PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer = n_bib; PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers = n_bvb;
PFN_vkCmdDrawIndexed vkCmdDrawIndexed = n_di; PFN_vkCmdSetCullMode vkCmdSetCullMode = n_cm;
PFN_vkCmdSetFrontFace vkCmdSetFrontFace = n_ff; PFN_vkCmdSetPrimitiveTopology vkCmdSetPrimitiveTopology = n_pt;
PFN_vkCmdSetDepthTestEnable vkCmdSetDepthTestEnable = n_b32; PFN_vkCmdSetDepthWriteEnable vkCmdSetDepthWriteEnable = n_b32;
PFN_vkCmdSetDepthCompareOp vkCmdSetDepthCompareOp = n_dco; PFN_vkCmdSetStencilTestEnable vkCmdSetStencilTestEnable = n_b32;
PFN_vkCmdSetStencilOp vkCmdSetStencilOp = n_sop; PFN_vkCmdSetDepthBiasEnable vkCmdSetDepthBiasEnable = n_b32;
PFN_vkCmdSetPrimitiveRestartEnable vkCmdSetPrimitiveRestartEnable = n_b32;
PFN_vkCmdSetVertexInputEXT vkCmdSetVertexInputEXT = n_svi;
PFN_vkGetPipelineExecutablePropertiesKHR vkGetPipelineExecutablePropertiesKHR = n_pep;
PFN_vkGetPipelineExecutableStatisticsKHR vkGetPipelineExecutableStatisticsKHR = n_pes;

// ---- guest texture & test helpers
static uint8_t *guest_mem; static size_t guest_off; static NvnPool g_pool;
static uint8_t *alloc_guest(size_t n) {
    size_t al = (n + 4095ull) & ~4095ull; assert(guest_off + al <= (8u << 20));
    uint8_t *p = guest_mem + guest_off; guest_off += al; memset(p, 0, al); return p;
}
static NvnTexture make_tex(uint32_t fmt, uint32_t w, uint32_t h, uint32_t flags, uint16_t fill16) {
    const FormatInfo *fi = nvn_format_info(fmt);
    NvnTexture t = {.target = 1, .format = fmt, .flags = flags, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1,
                    .layout = {.width = w, .height = h, .depth = 1, .layers = 1, .levels = 1, .fmt = fi}};
    tegra_layout_init(&t.layout);
    size_t sz = tegra_total_size(&t.layout); uint8_t *mem = alloc_guest(sz);
    t.pool = &g_pool; t.pool_offset = (uint64_t)(size_t)(mem - guest_mem); t.addr = (uint64_t)(uintptr_t)mem;
    LevelLayout l = tegra_level(&t.layout, 0);
    if (flags & 0x40) {
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) memcpy(mem + ((size_t)y * l.wb + x) * fi->bpb, &fill16, 2);
    } else {
        uint8_t *lin = calloc((size_t)w * h, fi->bpb);
        for (uint32_t i = 0; i < w * h; i++) memcpy(lin + (size_t)i * fi->bpb, &fill16, 2);
        tegra_swizzle(lin, mem, l.wb, l.hb, 1, fi->bpb, l.block_height_log2, 0); free(lin);
    }
    guest_mark_dirty(mem, sz); return t;
}
static void write_texel(const NvnTexture *t, uint32_t x, uint32_t y, uint16_t val) {
    const FormatInfo *fi = t->layout.fmt; uint8_t *mem = (uint8_t *)(uintptr_t)t->addr; LevelLayout l = tegra_level(&t->layout, 0);
    if (t->flags & 0x40) { guest_write(mem + ((size_t)y * l.wb + x) * fi->bpb, &val, 2); return; }
    uint8_t *lin = calloc((size_t)l.wb * l.hb, fi->bpb), *swz = calloc(1, tegra_total_size(&t->layout));
    tegra_deswizzle(mem, lin, l.wb, l.hb, 1, fi->bpb, l.block_height_log2, 0);
    memcpy(lin + ((size_t)y * l.wb + x) * fi->bpb, &val, 2);
    tegra_swizzle(lin, swz, l.wb, l.hb, 1, fi->bpb, l.block_height_log2, 0);
    for (size_t i = 0; i < tegra_total_size(&t->layout); i++) if (swz[i] != mem[i]) guest_write(mem + i, &swz[i], 1);
    free(swz); free(lin);
}
static uint16_t read_texel0(const NvnTexture *t, uint32_t x, uint32_t y) {
    const FormatInfo *fi = t->layout.fmt; const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr; LevelLayout l = tegra_level(&t->layout, 0);
    uint16_t v = 0;
    if (t->flags & 0x40) { memcpy(&v, mem + ((size_t)y * l.wb + x) * fi->bpb, 2); return v; }
    uint8_t *lin = calloc((size_t)t->width * t->height, fi->bpb);
    tegra_deswizzle(mem, lin, l.wb, l.hb, 1, fi->bpb, l.block_height_log2, 0);
    memcpy(&v, lin + ((size_t)y * t->width + x) * fi->bpb, 2); free(lin); return v;
}
static void reset_pass(NvnTexture *c0, NvnTexture *c1, uint32_t chmask, const CmdRec *rec) {
    memset(&st, 0, sizeof st); st.rt.ncolor = c1 ? 2 : 1; st.rt.color[0] = c0; st.rt.color[1] = c1; st.channel_mask = chmask;
    st.scissor[0][2] = c0->width; st.scissor[0][3] = c0->height; st.rt_dirty = true; g_cur_rec = rec; memset(last_lop, 0, sizeof last_lop);
}
static int failures;
static void check(const char *name, bool ok) { printf("%s %s\n", ok ? "ok  " : "FAIL", name); failures += !ok; }

typedef struct {
    uint8_t buf[64];
    _Atomic bool stop;
} SeqStressCtx;

static void *seqlock_writer_fn(void *arg) {
    SeqStressCtx *ctx = (SeqStressCtx *)arg;
    for (int i = 0; i < 4000; i++) {
        cbwin_write_begin();
        memset(ctx->buf, (i & 1) ? 0xA1 : 0xB2, sizeof ctx->buf);
        cbwin_write_end();
    }
    atomic_store_explicit(&ctx->stop, true, memory_order_release);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("LARECOMP_TEX_DISCARD", "1", 1); setenv("LARECOMP_WRITEBACK_MAX", "262144", 1);
#ifdef MAP_FIXED_NOREPLACE
    guest_mem = mmap((void *)(uintptr_t)(HEAP_BASE + (1u << 20)), 8u << 20, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#else
    guest_mem = MAP_FAILED;
#endif
    if (guest_mem == MAP_FAILED)
        guest_mem = mmap((void *)(uintptr_t)(HEAP_BASE + (1u << 20)), 8u << 20, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(guest_mem != MAP_FAILED && (uintptr_t)guest_mem >= HEAP_BASE &&
           (uintptr_t)guest_mem - HEAP_BASE + (8u << 20) <= HEAP_MAX &&
           (((uintptr_t)guest_mem - HEAP_BASE + (8u << 20)) >> 12) <= sizeof mock_dirty);
    maxwell_gmem_base = HEAP_BASE; g_pool = (NvnPool){.storage = (uint64_t)(uintptr_t)guest_mem, .size = 8u << 20};

    // 1. Core #296: 64x64 linear R16G16B16A16_UNORM write-back RT drawn along 3 rows with LOAD_OP_LOAD.
    stat_b2i = 0; NvnTexture t1 = make_tex(0x2a, 64, 64, 0x40, 0x24cb);
    reset_pass(&t1, NULL, 0xF, NULL); begin_pass(); sim_draw_rows(0, 3, 0x24cd); vk_end_pass(); renderer_flush();
    int nz = 0; for (uint32_t y = 0; y < 64; y++) for (uint32_t x = 0; x < 64; x++) if (read_texel0(&t1, x, y)) nz++;
    check("#296 partial LOAD_OP_LOAD writeback preserves untouched CPU texels",
          last_lop[0] == VK_ATTACHMENT_LOAD_OP_LOAD && stat_b2i == 1 && read_texel0(&t1, 10, 1) == 0x24cd && read_texel0(&t1, 10, 40) == 0x24cb && nz == 4096);

    // 2. Whole-target CMD_CLEAR_COLOR -> LOAD_OP_CLEAR skips initial upload; partial MRT clear uploads larger RT.
    _Alignas(CmdRec) uint8_t cbuf[sizeof(CmdRec) + 16 + sizeof(CmdRec)] = {0};
    CmdRec *clr = (CmdRec *)cbuf; clr->op = CMD_CLEAR_COLOR; clr->size = (uint32_t)(sizeof(CmdRec) + 16); clr->x[2] = 0xF;
    stat_b2i = 0; NvnTexture t2 = make_tex(0x2a, 64, 64, 0x40, 0x24cb);
    reset_pass(&t2, NULL, 0xF, clr); begin_pass(); sim_draw_rows(0, 3, 0x24cd); vk_end_pass(); renderer_flush();
    check("whole-target LOAD_OP_CLEAR skips upload", last_lop[0] == VK_ATTACHMENT_LOAD_OP_CLEAR && !stat_b2i && read_texel0(&t2, 10, 40) == 0);
    NvnTexture t3a = make_tex(0x2a, 128, 128, 0x40, 0x24cb), t3b = make_tex(0x2a, 64, 64, 0x40, 0x24cb); renderer_mark_gpu_owned(&t3b);
    stat_b2i = 0; reset_pass(&t3a, &t3b, 0xFF, clr); begin_pass(); vk_end_pass(); renderer_flush();
    check("partial-area MRT clear still uploads larger write-back attachment", last_lop[0] == VK_ATTACHMENT_LOAD_OP_CLEAR && stat_b2i == 1 && read_texel0(&t3a, 100, 100) == 0x24cb);

    // 3. Already gpu_owned target adds 0 uploads and 0 extra pass barriers; duplicate slot + tex_discard skip on RT.
    stat_b2i = stat_bar = stat_up_bar = 0; write_texel(&t1, 10, 40, 0x1111);
    reset_pass(&t1, NULL, 0xF, NULL); begin_pass(); vk_end_pass(); int pb = stat_bar, ub = stat_up_bar; renderer_flush();
    check("gpu_owned target skips re-upload and extra barriers", !stat_b2i && !ub && pb == 3 && read_texel0(&t1, 10, 40) == 0x24cb);
    stat_b2i = 0; NvnTexture t5d = make_tex(0x2a, 64, 64, 0x40, 0x24cb), t5b = make_tex(0x2a, 128, 128, 0x0, 0x24cb);
    reset_pass(&t5d, &t5d, 0xF0, NULL); begin_pass(); sim_draw_rows(0, 2, 0x24cd); vk_end_pass(); renderer_flush();
    reset_pass(&t5b, NULL, 0xF, NULL); begin_pass(); sim_draw_rows(0, 2, 0x24cd); vk_end_pass(); renderer_flush();
    check("duplicate slot uploads once and tex_discard skips RT guest pages",
          stat_b2i == 2 && !((TexBackend *)t5b.backend)->discarded && read_texel0(&t5d, 5, 40) == 0x24cb && read_texel0(&t5b, 64, 64) == 0x24cb);

    // 4. Intra-submission uploaded=1 && !gpu_owned CPU write -> s->main upload preserves V1 for prior draws and V2 for pass & open-pass resolve_feedback.
    for (int pt = 1; pt >= 0; pt--) {
        mock_pt_enabled = pt != 0; memset(mock_dirty, 0, sizeof mock_dirty); s_frame_counter = 300 + (uint64_t)pt * 10;
        NvnTexture t = make_tex(0x2a, 64, 64, 0x40, 0x1111); uint16_t v1_seen = 0;
        stat_b2i = stat_in_up = stat_in_main = stat_sc_bar = 0;
        vk_tex(&t, false); sim_sample(&t, 10, 40, &v1_seen); write_texel(&t, 10, 40, 0x2222);
        reset_pass(&t, NULL, 0xF, NULL); begin_pass(); sim_draw_rows(0, 2, 0x3333); vk_end_pass(); renderer_flush();
        check(pt ? "intra-submission s->main upload (page tracking on)" : "intra-submission s->main upload (page tracking off)",
              v1_seen == 0x1111 && stat_in_up == 1 && stat_in_main == 1 && stat_sc_bar == 2 && read_texel0(&t, 10, 40) == 0x2222);
    }
    mock_pt_enabled = true;

    // 5. Stagger-skipped frame bypass + unchanged uploaded=1 -> gpu_owned zero-overhead.
    s_frame_counter = 400;
    NvnTexture ts = make_tex(0x06, 64, 64, 0x0, 0x1111); vk_tex(&ts, false); renderer_flush();
    while (((++s_frame_counter + ((uintptr_t)ts.backend >> 6)) & 7) == 0) {}
    write_texel(&ts, 1, 0, 0x5555); reset_pass(&ts, NULL, 0xF, NULL); begin_pass(); sim_draw_rows(63, 1, 0x6666); vk_end_pass(); renderer_flush();
    check("stagger-skipped CPU mutation syncs before gpu_owned", read_texel0(&ts, 1, 0) == 0x5555 && read_texel0(&ts, 0, 0) == 0x1111 && read_texel0(&ts, 0, 63) == 0x6666);
    NvnTexture tu = make_tex(0x2a, 64, 64, 0x40, 0x24cb); vk_tex(&tu, false); renderer_flush();
    stat_b2i = stat_up_bar = stat_sc_bar = 0; reset_pass(&tu, NULL, 0xF, NULL); begin_pass(); vk_end_pass(); renderer_flush();
    check("unchanged uploaded=1 -> gpu_owned adds 0 uploads and 0 barriers", !stat_b2i && !stat_up_bar && !stat_sc_bar && ((TexBackend *)tu.backend)->gpu_owned);

    // 6. Open-pass resolve_feedback + Turnip in-place feedback + destroy pointer cleanup.
    s_frame_counter = 500;
    NvnTexture tp = make_tex(0x2a, 64, 64, 0x40, 0x7777), tr = make_tex(0x2a, 64, 64, 0x40, 0x1111);
    vk_tex(&tr, false); renderer_flush();
    reset_pass(&tp, NULL, 0xF, NULL); begin_pass(); sim_draw_rows(0, 1, 0x7778); assert(in_pass);
    write_texel(&tr, 10, 40, 0x2222);
    NvnTexEntry ents[2] = {[1] = {.tex = &tr}}; NvnDescPool dpool = {.count = 2, .entries = ents};
    ShaderInfo vs = {.stage = STAGE_VERTEX}, fs = {.stage = STAGE_FRAGMENT, .ntex = 1, .tex = {[0] = {.handle_offset = 0x20, .type = TEXTYPE_2D}}};
    Prog pr = {.vi = &vs, .fi = &fs, .fs_out_mask = 0xF};
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull; nfeedback = stat_in_main = stat_sc_bar = 0;
    resolve_feedback(&pr, 1); begin_pass(); sim_draw_rows(0, 2, 0x3333); vk_end_pass(); renderer_flush();
    MockImg *sn0 = nfeedback == 1 && feedback_snap[0] ? find_img(feedback_snap[0]->image) : NULL; uint16_t s_texel = 0;
    if (sn0) memcpy(&s_texel, sn0->px + ((size_t)40 * sn0->w + 10) * sn0->bpb, 2);
    check("open-pass resolve_feedback ends P0 before s->main copy and snapshots V2", stat_in_main == 1 && stat_sc_bar == 2 && s_texel == 0x2222);

    backend_texture_destroyed(&tr); tr = make_tex(0x2a, 64, 64, 0x40, 0x1111); vk_tex(&tr, false); renderer_flush();
    mock_turnip = true; g_inplace_copy = -1; stat_inplace_breaks = 0; write_texel(&tr, 10, 40, 0x4444);
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); snapshots_mark_fb_dirty(&tr); begin_pass(); ninplace = 0; sim_draw_rows(0, 2, 0x3333);
    pass_written |= 1; st.rt_dirty = false; resolve_feedback(&pr, 1); vk_end_pass(); renderer_flush();
    bool ok_ip = !stat_inplace_breaks && !st.rt_dirty && read_texel0(&tr, 10, 40) == 0x4444;
    mock_turnip = false; g_inplace_copy = -1; backend_texture_destroyed(&tr); tr = make_tex(0x2a, 64, 64, 0x40, 0x5555);
    vk_tex(&tr, false); renderer_flush(); uint64_t fb0 = stat_feedback_copies;
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); begin_pass(); vk_end_pass(); renderer_flush();
    check("Turnip in-place feedback keeps snap_ok & destroy clears dead snapshot pointers", ok_ip && stat_feedback_copies == fb0 + 1);

    // 6b. Mali (vendorID 0x13B5) in-place feedback on Automatic (-1, #549 / 9f46f29) and on "On",
    // mid-pass break tracking, empty/non-empty env precedence, and non-Turnip/non-Mali negative control.
    g_vk.props.vendorID = 0x13B5; mock_turnip = false; g_settings.inplace_reads = -1; g_inplace_copy = -1;
    unsetenv("LARECOMP_FEEDBACK_INPLACE"); unsetenv("LARECOMP_FEEDBACK_COPY");
    backend_texture_destroyed(&tr); tr = make_tex(0x2a, 64, 64, 0x40, 0x1111); vk_tex(&tr, false); renderer_flush();
    stat_inplace_breaks = 0; uint64_t fb_mali0 = stat_feedback_copies;

    // Mali Automatic (-1) reads in place since #549 (exec.c: vk_driver_is_mali && setting != 0).
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); vk_end_pass(); renderer_flush();
    bool ok_mali_auto_copy = g_inplace_copy == 0 && stat_feedback_copies == fb_mali0;

    // Explicit Settings "On" (inplace_reads = 1) enables in-place reads on Mali + mid-pass break on barrier.
    snapshots_invalidate(&tr);
    g_settings.inplace_reads = 1; g_inplace_copy = -1;
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); snapshots_mark_fb_dirty(&tr); begin_pass(); ninplace = 0; sim_draw_rows(0, 2, 0x3333);
    pass_written |= 1; st.rt_dirty = false; resolve_feedback(&pr, 1);
    bool ok_same_pass = !g_inplace_copy && !stat_inplace_breaks && !st.rt_dirty;
    snapshots_flush_fb_dirty("barrier");
    resolve_feedback(&pr, 1);
    bool ok_mali_break = stat_inplace_breaks == 1 && st.rt_dirty && stat_feedback_copies == fb_mali0;
    vk_end_pass(); renderer_flush();

    snapshots_invalidate(&tr);
    g_settings.inplace_reads = 0; g_inplace_copy = -1;  // explicit setting=0 forces copies on Mali
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); vk_end_pass(); renderer_flush();
    bool ok_set_off = g_inplace_copy == 1 && stat_feedback_copies == fb_mali0 + 1;

    setenv("LARECOMP_FEEDBACK_INPLACE", "1", 1); g_inplace_copy = -1;  // LARECOMP_FEEDBACK_INPLACE=1 overrides setting=0
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); vk_end_pass(); renderer_flush();
    bool ok_fi_on = g_inplace_copy == 0 && stat_feedback_copies == fb_mali0 + 1;

    snapshots_invalidate(&tr);
    setenv("LARECOMP_FEEDBACK_INPLACE", "0", 1); g_settings.inplace_reads = 1; g_inplace_copy = -1;
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); vk_end_pass(); renderer_flush();
    bool ok_fi_off = g_inplace_copy == 1 && stat_feedback_copies == fb_mali0 + 2;

    // LARECOMP_FEEDBACK_COPY=1 takes precedence over LARECOMP_FEEDBACK_INPLACE=1 and settings=1.
    snapshots_invalidate(&tr);
    setenv("LARECOMP_FEEDBACK_COPY", "1", 1); setenv("LARECOMP_FEEDBACK_INPLACE", "1", 1);
    g_settings.inplace_reads = 1; g_inplace_copy = -1;
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); vk_end_pass(); renderer_flush();
    bool ok_fb_copy_prec = g_inplace_copy == 1 && stat_feedback_copies == fb_mali0 + 3;
    unsetenv("LARECOMP_FEEDBACK_COPY"); unsetenv("LARECOMP_FEEDBACK_INPLACE");

    // Non-Turnip, non-Mali GPU (vendorID 0x5143) even with settings=1 keeps feedback copies.
    snapshots_invalidate(&tr);
    g_vk.props.vendorID = 0x5143; g_settings.inplace_reads = 1; g_inplace_copy = -1;
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); vk_end_pass(); renderer_flush();
    bool ok_non_mali_off = g_inplace_copy == 1 && stat_feedback_copies == fb_mali0 + 4;

    // Empty LARECOMP_FEEDBACK_INPLACE="" does not block settings (-1 and 1 both in place on Mali).
    snapshots_invalidate(&tr);
    g_vk.props.vendorID = 0x13B5; setenv("LARECOMP_FEEDBACK_INPLACE", "", 1);
    g_settings.inplace_reads = -1; g_inplace_copy = -1;
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); vk_end_pass(); renderer_flush();
    bool ok_empty_auto = g_inplace_copy == 0 && stat_feedback_copies == fb_mali0 + 4;
    g_settings.inplace_reads = 1; g_inplace_copy = -1;
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); vk_end_pass(); renderer_flush();
    bool ok_empty_on = g_inplace_copy == 0 && stat_feedback_copies == fb_mali0 + 4;
    unsetenv("LARECOMP_FEEDBACK_INPLACE");

    // AB("copy") == 1 forces feedback copy on Mali even when g_inplace_copy == 0.
    snapshots_invalidate(&tr);
    g_vk.props.vendorID = 0x13B5; g_settings.inplace_reads = 1; g_inplace_copy = -1;
    setenv("LARECOMP_AB", "1:copy", 1); ab_n = -1; s_frame_counter = 1;
    reset_pass(&tr, NULL, 0xF, NULL); st.tpool = &dpool; st.tex[1][0] = 0x100001ull;
    resolve_feedback(&pr, 1); vk_end_pass(); renderer_flush();
    bool ok_ab_copy = g_inplace_copy == 0 && stat_feedback_copies == fb_mali0 + 5;
    unsetenv("LARECOMP_AB"); ab_n = -1; s_frame_counter = 500;

    check("Mali (0x13B5) in-place feedback on Automatic and On, pass-break, and env/setting matrix",
          ok_mali_auto_copy && ok_same_pass && ok_mali_break && ok_set_off && ok_fi_on && ok_fi_off &&
          ok_fb_copy_prec && ok_non_mali_off && ok_empty_auto && ok_empty_on && ok_ab_copy);
    g_vk.props.vendorID = 0; g_settings.inplace_reads = 0; g_inplace_copy = -1;

    // 7. tex_discarded_bytes accounting stays accurate across re-upload and destroy_image.
    tex_discarded_bytes = 0; uint64_t disc = 0;
    NvnTexture td1 = make_tex(0x2a, 128, 128, 0x0, 0x1234), td2 = make_tex(0x2a, 128, 128, 0x0, 0x5678);
    vk_tex(&td1, false); renderer_flush(); vk_texture_guest_bytes(&disc);
    uint64_t d_after_up = disc;
    while (((++s_frame_counter + ((uintptr_t)td1.backend >> 6)) & 7) != 0) {}
    write_texel(&td1, 4, 4, 0x4242); vk_tex(&td1, false); renderer_flush(); vk_texture_guest_bytes(&disc);
    uint64_t d_after_reup = disc;
    vk_tex(&td2, false); renderer_flush(); uint64_t d_before_del = 0; vk_texture_guest_bytes(&d_before_del);
    backend_texture_destroyed(&td2); vk_texture_guest_bytes(&disc);
    backend_texture_destroyed(&td1);
    check("undiscard lowers tex_discarded_bytes on re-upload and destroy_image (0 drift)",
          d_after_up == 131072 && d_after_reup == 0 && d_before_del == 131072 && disc == 0);

    {
        // 8. Integration: compute dispatch chaining in gcopy_mode, cs_writes_image barrier deferral, and vk_submit_extra flush.
    gcopy_mode = false; dchain = false; dchain_after = false; dchain_env = -1;
    unsetenv("LARECOMP_VK_DISPATCH_CHAIN");
    renderer_flush();

    ShaderInfo si_plain = {.stage = STAGE_COMPUTE};
    ShaderInfo si_img = {.stage = STAGE_COMPUTE, .ntex = 1, .tex = {[0] = {.type = TEXTYPE_2D, .is_image = true}}};
    ShaderInfo si_gstore = {.stage = STAGE_COMPUTE, .uses_gmem_map = true, .uses_gmem_store = true};
    CsProg *cp_plain = calloc(1, sizeof *cp_plain);
    CsProg *cp_img = calloc(1, sizeof *cp_img);
    CsProg *cp_gstore = calloc(1, sizeof *cp_gstore);
    assert(cp_plain && cp_img && cp_gstore);
    cp_plain->si = cp_plain->p.vi = &si_plain;
    atomic_store(&cp_plain->p.state, 1);
    cp_img->si = cp_img->p.vi = &si_img;
    atomic_store(&cp_img->p.state, 1);
    cp_gstore->si = cp_gstore->p.vi = &si_gstore;
    atomic_store(&cp_gstore->p.state, 1);

    NvnProgram prog_plain = {.stages = {[5] = {.code_addr = 0x90000100ull}}};
    NvnProgram prog_img = {.stages = {[5] = {.code_addr = 0x90000200ull}}};
    NvnProgram prog_gstore = {.stages = {[5] = {.code_addr = 0x90000300ull}}};
    VkObjectEntry *e_plain = vk_object_slot(&compute_cache, prog_plain.stages[5].code_addr);
    VkObjectEntry *e_img = vk_object_slot(&compute_cache, prog_img.stages[5].code_addr);
    VkObjectEntry *e_gstore = vk_object_slot(&compute_cache, prog_gstore.stages[5].code_addr);
    e_plain->object = cp_plain;
    e_img->object = cp_img;
    e_gstore->object = cp_gstore;

    int bar0 = stat_bar, disp0 = stat_dispatch; uint64_t dc0 = stat_dchained;
    _Alignas(CmdRec) CmdRec dcmds[8];
    memset(dcmds, 0, sizeof dcmds);
    dcmds[0] = (CmdRec){.op = CMD_BIND_PROGRAM, .size = sizeof(CmdRec), .x = {(uint64_t)(uintptr_t)&prog_img}};
    dcmds[1] = (CmdRec){.op = CMD_DISPATCH, .size = sizeof(CmdRec), .x = {1, 1, 1}};
    dcmds[2] = (CmdRec){.op = CMD_UPDATE_UNIFORM_BUFFER, .size = sizeof(CmdRec)};  // dchain_keeps, sz=0
    dcmds[3] = (CmdRec){.op = CMD_BIND_PROGRAM, .size = sizeof(CmdRec), .x = {(uint64_t)(uintptr_t)&prog_plain}};
    dcmds[4] = (CmdRec){.op = CMD_DISPATCH, .size = sizeof(CmdRec), .x = {1, 1, 1}};
    dcmds[5] = (CmdRec){.op = CMD_BARRIER, .size = sizeof(CmdRec)};  // non-dchain_keeps: flushes dchain_after
    dcmds[6] = (CmdRec){.op = CMD_END, .size = sizeof(CmdRec)};
    exec_list((uint64_t)(uintptr_t)dcmds, 0);
    bool ok_dchain_seq = (stat_dispatch - disp0 == 2) && (stat_dchained - dc0 == 1) &&
                         (stat_bar - bar0 == 2) && !dchain && !dchain_after;

    // gcopy_mode storing dispatch chains with subsequent dispatch and arms dchain_after.
    gcopy_mode = true;
    int barg0 = stat_bar, dispg0 = stat_dispatch; uint64_t dcg0 = stat_dchained;
    dcmds[0].x[0] = (uint64_t)(uintptr_t)&prog_gstore;
    exec_list((uint64_t)(uintptr_t)dcmds, 0);
    bool ok_gcopy_chain = (stat_dispatch - dispg0 == 2) && (stat_dchained - dcg0 == 1) &&
                          (stat_bar - barg0 == 2) && !dchain && !dchain_after;
    gcopy_mode = false;

    // Non-dchain_keeps op when dchain == false && dchain_after == true (e.g. gfast_n > 0) must still flush barrier.
    _Alignas(CmdRec) CmdRec bar_end[2] = {
        {.op = CMD_BARRIER, .size = sizeof(CmdRec)},
        {.op = CMD_END, .size = sizeof(CmdRec)},
    };
    int bar1 = stat_bar;
    dchain = false; dchain_after = true;
    exec_list((uint64_t)(uintptr_t)bar_end, 0);
    bool ok_dchain_after_only = (stat_bar - bar1 == 1) && !dchain && !dchain_after;

    // vk_submit_extra flushes owed dchain_after before vkEndCommandBuffer.
    _Alignas(CmdRec) CmdRec tail_cmds[3] = {
        {.op = CMD_BIND_PROGRAM, .size = sizeof(CmdRec), .x = {(uint64_t)(uintptr_t)&prog_img}},
        {.op = CMD_DISPATCH, .size = sizeof(CmdRec), .x = {1, 1, 1}},
        {.op = CMD_END, .size = sizeof(CmdRec)},
    };
    int bar2 = stat_bar;
    exec_list((uint64_t)(uintptr_t)tail_cmds, 0);
    bool armed_before_sub = dchain && dchain_after && (stat_bar - bar2 == 1);
    vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    bool ok_submit_flush = armed_before_sub && (stat_bar - bar2 == 2) && !dchain && !dchain_after;

    // LARECOMP_VK_DISPATCH_CHAIN=0 negative control: barriers before every dispatch and immediately after storing dispatch.
    setenv("LARECOMP_VK_DISPATCH_CHAIN", "0", 1); dchain_env = -1;
    int bar_off0 = stat_bar, disp_off0 = stat_dispatch; uint64_t dc_off0 = stat_dchained;
    dcmds[0].x[0] = (uint64_t)(uintptr_t)&prog_img;
    exec_list((uint64_t)(uintptr_t)dcmds, 0);
    bool ok_chain_off = (stat_dispatch - disp_off0 == 2) && (stat_dchained == dc_off0) &&
                        (stat_bar - bar_off0 == 3) && !dchain && !dchain_after;
    unsetenv("LARECOMP_VK_DISPATCH_CHAIN"); dchain_env = -1;

    // Plain read-only dispatches chained together + CMD_BARRIER -> 1 pre-dispatch barrier total, 0 barriers at CMD_BARRIER.
    _Alignas(CmdRec) CmdRec plain_cmds[6] = {
        {.op = CMD_BIND_PROGRAM, .size = sizeof(CmdRec), .x = {(uint64_t)(uintptr_t)&prog_plain}},
        {.op = CMD_DISPATCH, .size = sizeof(CmdRec), .x = {1, 1, 1}},
        {.op = CMD_DISPATCH, .size = sizeof(CmdRec), .x = {1, 1, 1}},
        {.op = CMD_BARRIER, .size = sizeof(CmdRec)},
        {.op = CMD_END, .size = sizeof(CmdRec)},
    };
    int bar_pl0 = stat_bar, disp_pl0 = stat_dispatch; uint64_t dc_pl0 = stat_dchained;
    exec_list((uint64_t)(uintptr_t)plain_cmds, 0);
    bool ok_plain_zero_bar = (stat_dispatch - disp_pl0 == 2) && (stat_dchained - dc_pl0 == 1) &&
                             (stat_bar - bar_pl0 == 1) && !dchain && !dchain_after;

    // !gcopy_mode fallback (m==0 -> gfast_handled==false) MUST arm dchain_after, and chained gfast_n>0 MUST flush
    // dchain_after BEFORE copy_buffer(gmem_buf -> gfast_buf), then copy back + barrier.
    gcopy_mode = false;
    g_vk.host_import = true;
    uint8_t *gf_guest = alloc_guest(4096);
    gmem_buf = (VkBuffer)(uintptr_t)0xCAFE0002ull;
    uint64_t need_gmem = (uint64_t)((uintptr_t)guest_mem - HEAP_BASE) + (8u << 20);
    gmem_size = need_gmem > kernel_heap_size() ? need_gmem : kernel_heap_size();
    memset(st.sbuf, 0, sizeof st.sbuf);
    _Alignas(CmdRec) CmdRec gstore_one[3] = {
        {.op = CMD_BIND_PROGRAM, .size = sizeof(CmdRec), .x = {(uint64_t)(uintptr_t)&prog_gstore}},
        {.op = CMD_DISPATCH, .size = sizeof(CmdRec), .x = {1, 1, 1}},
        {.op = CMD_END, .size = sizeof(CmdRec)},
    };
    int bar_gf0 = stat_bar; stat_cbuf = 0; uint64_t dc_gf0 = stat_dchained;
    exec_list((uint64_t)(uintptr_t)gstore_one, 0);
    bool ok_gstore_fallback = dchain && dchain_after && (stat_bar - bar_gf0 == 1);
    st.sbuf[5][0].addr = (uint64_t)(uintptr_t)gf_guest;
    st.sbuf[5][0].size = 4096;
    exec_list((uint64_t)(uintptr_t)gstore_one, 0);
    bool ok_gfast_pre_flush = (stat_dchained - dc_gf0 == 1) && stat_cbuf == 2 &&
                              cbuf_bars[0] == bar_gf0 + 2 && cbuf_bars[1] == bar_gf0 + 4 &&
                              stat_bar == bar_gf0 + 5 && !dchain_after;
    memset(st.sbuf, 0, sizeof st.sbuf);

    e_plain->object = NULL; e_img->object = NULL; e_gstore->object = NULL;
    free(cp_plain); free(cp_img); free(cp_gstore);
    st.program = NULL;

    // vk_exec_reset() clears dchain*, gwin_mode, and gfast_win across reboots.
    dchain = true; dchain_after = true; dchain_cb = vk_cmd(); dchain_env = 1;
    gwin_mode = true; gfast_win = (VkDescriptorBufferInfo){gmem_buf, 0, 4096};
    gmem_buf = VK_NULL_HANDLE; gmem_size = 0; g_vk.host_import = false;
    vk_exec_reset();
    bool ok_reset = !dchain && !dchain_after && dchain_cb == VK_NULL_HANDLE && dchain_env == -1 &&
                    !gwin_mode && gfast_win.buffer == VK_NULL_HANDLE;

    check("dchain integration: dispatch chaining, gcopy_mode, chain=0, plain 0-bar, gfast pre-flush, and reset",
          ok_dchain_seq && ok_gcopy_chain && ok_dchain_after_only && ok_submit_flush &&
          ok_chain_off && ok_plain_zero_bar && ok_gstore_fallback && ok_gfast_pre_flush && ok_reset);
    }
    {
        // 9. Integration: cbwin_bind, g_cbwin_cache, cbpatch dedupe, #453 snapshot=0 paths, isolated seqlock, and flushes.
    g_vk.props.vendorID = 0x13B5;
    g_vk.props.limits.maxUniformBufferRange = 65536;
    g_vk.props.limits.minUniformBufferOffsetAlignment = 256;
    gcopy_mode = true; gmem_placed = false;
    atomic_store_explicit(&cbwin_env, -2, memory_order_relaxed);
    atomic_store_explicit(&cbwin_snap_env, -1, memory_order_relaxed);
    unsetenv("LARECOMP_CB_WINDOW");
    unsetenv("LARECOMP_CBWIN_SNAPSHOT");
    cbwin_base_reset(&g_cbwin_cache);
    cbpatch_dedupe_reset(&g_cbpatch_dedupe);

    uint8_t *ubo = alloc_guest(4096), *src_cpy = alloc_guest(256);
    uint64_t ubo_addr = (uint64_t)(uintptr_t)ubo;
    memset(ubo, 0x11, 4096);
    memset(src_cpy, 0x44, 256);
    vk_ubo_inline_mark(ubo_addr + 0x40, 16);

    VkDescriptorBufferInfo cbi1 = {0}, cbi2 = {0}, cbi3 = {0};
    assert(!cbwin_bind(0, 2, ubo_addr + 4, 256, &cbi1));  // unaligned address rejected
    memset(&cbw_patch, 0, sizeof cbw_patch);
    stat_cbwin = 0; uint64_t cbc0 = stat_cb_copied;
    bool ok_cb1 = cbwin_bind(0, 2, ubo_addr, 4096, &cbi1) && cbw_patch[0].m[0] == 3u && cbw_patch[0].m[1] == 4u &&
                  cbw_patch[0].m[2] == 1u && stat_cbwin == 1 && stat_cb_copied == cbc0 + 4096;
    // Simulate draw_() up_fix retry: zero m[0..3] with non-zero stale rows[], check 16B active prefix dedupe.
    cbw_patch[0].rows[10][0] = 0xDEADBEEFu;
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    uint32_t act_0 = cbpatch_active_bytes(&cbw_patch[0]);
    VkSpan sp_zero = {0};
    assert(act_0 == 16u && !cbpatch_lookup(&g_cbpatch_dedupe, 0, vk_pending_seq(), &cbw_patch[0], &act_0, &sp_zero));
    VkSpan sp_commit = vk_transient_copy(&cbw_patch[0], sizeof cbw_patch[0], 256);
    cbpatch_commit(&g_cbpatch_dedupe, 0, vk_pending_seq(), &cbw_patch[0], act_0, sp_commit);
    cbw_patch[0].rows[10][0] = 0xCAFEBABEu;
    assert(cbpatch_lookup(&g_cbpatch_dedupe, 0, vk_pending_seq(), &cbw_patch[0], &act_0, &sp_zero) && sp_zero.off == sp_commit.off);

    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_cb_hit = cbwin_bind(0, 2, ubo_addr, 4096, &cbi2) && cbi2.buffer == cbi1.buffer &&
                     cbi2.offset == cbi1.offset && stat_cb_copied == cbc0 + 4096;

    // CMD_UPDATE_UNIFORM_BUFFER inside [ubo_addr+0x40, ubo_addr+0x50) keeps base snapshot; outside invalidates it.
    _Alignas(CmdRec) uint8_t ubo_cmds[256];
    memset(ubo_cmds, 0, sizeof ubo_cmds);
    CmdRec *u_in = (CmdRec *)ubo_cmds;
    u_in->op = CMD_UPDATE_UNIFORM_BUFFER;
    u_in->size = (uint32_t)((sizeof(CmdRec) + 16u + 7u) & ~7u);
    u_in->x[0] = ubo_addr; u_in->x[2] = 0x40; u_in->x[3] = 16;
    memset(u_in->extra, 0x22, 16);
    CmdRec *u_end = (CmdRec *)(ubo_cmds + u_in->size);
    u_end->op = CMD_END; u_end->size = sizeof(CmdRec);
    exec_list((uint64_t)(uintptr_t)ubo_cmds, 0);

    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_cb_in = cbwin_bind(0, 2, ubo_addr, 4096, &cbi3) && cbi3.buffer == cbi1.buffer &&
                    cbi3.offset == cbi1.offset && stat_cb_copied == cbc0 + 4096 && cbw_patch[0].rows[0][0] == 0x22222222u;

    memset(ubo_cmds, 0, sizeof ubo_cmds);
    CmdRec *u_out = (CmdRec *)ubo_cmds;
    u_out->op = CMD_UPDATE_UNIFORM_BUFFER;
    u_out->size = (uint32_t)((sizeof(CmdRec) + 16u + 7u) & ~7u);
    u_out->x[0] = ubo_addr; u_out->x[2] = 0x100; u_out->x[3] = 16;
    memset(u_out->extra, 0x33, 16);
    u_end = (CmdRec *)(ubo_cmds + u_out->size);
    u_end->op = CMD_END; u_end->size = sizeof(CmdRec);
    exec_list((uint64_t)(uintptr_t)ubo_cmds, 0);

    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_cb_out = cbwin_bind(0, 2, ubo_addr, 4096, &cbi3) && stat_cb_copied == cbc0 + 8192;

    // CMD_COPY_BUFFER_TO_BUFFER flushes g_cbwin_cache via guest_copy_flush(); seqlock write window bypasses & drops it.
    memset(ubo_cmds, 0, sizeof ubo_cmds);
    CmdRec *b2b = (CmdRec *)ubo_cmds;
    b2b->op = CMD_COPY_BUFFER_TO_BUFFER;
    b2b->size = sizeof(CmdRec);
    b2b->x[0] = (uint64_t)(uintptr_t)src_cpy; b2b->x[1] = ubo_addr + 0x40; b2b->x[2] = 64;
    u_end = (CmdRec *)(ubo_cmds + b2b->size);
    u_end->op = CMD_END; u_end->size = sizeof(CmdRec);
    exec_list((uint64_t)(uintptr_t)ubo_cmds, 0);

    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_b2b = memcmp(ubo + 0x40, src_cpy, 64) == 0 && cbwin_bind(0, 2, ubo_addr, 4096, &cbi3) &&
                  stat_cb_copied == cbc0 + 12288;
    // Active cbwin_write_begin/end seqlock window: reader sees !can_cache (no stale cache insert), then end bumps cbwin_retire_gen.
    cbwin_write_begin();
    bool seqlock_can_cache = true;
    (void)cbwin_read_begin(&seqlock_can_cache);
    memset(ubo + 0x100, 0x55, 64);
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    VkDescriptorBufferInfo cbi_mid = {0}, cbi_post = {0};
    bool ok_mid_write = !seqlock_can_cache && cbwin_bind(0, 2, ubo_addr, 4096, &cbi_mid) &&
                        stat_cb_copied == cbc0 + 16384;
    cbwin_write_end();
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_post_copy = ok_mid_write && cbwin_bind(0, 2, ubo_addr, 4096, &cbi_post) &&
                        stat_cb_copied == cbc0 + 20480;
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_async_drop = ok_post_copy && cbwin_bind(0, 2, ubo_addr, 4096, &cbi3) &&
                         cbi3.offset == cbi_post.offset && stat_cb_copied == cbc0 + 20480;

    // Upstream #453 compatibility: LARECOMP_CBWIN_SNAPSHOT=0 and AB("cbsnap")==1 fallback/placed behavior.
    setenv("LARECOMP_CBWIN_SNAPSHOT", "0", 1);
    atomic_store_explicit(&cbwin_snap_env, -1, memory_order_relaxed);
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_nosnap_gcopy = !cbwin_bind(0, 2, ubo_addr, 4096, &cbi3);
    VkBuffer saved_gbuf = gmem_buf;
    uint64_t saved_gsize = gmem_size;
    setenv("LARECOMP_CB_WINDOW", "1", 1);
    atomic_store_explicit(&cbwin_env, -2, memory_order_relaxed);
    gmem_placed = true;
    gmem_buf = (VkBuffer)(uintptr_t)0xCAFE0001ull;
    gmem_size = (uint64_t)(ubo_addr - HEAP_BASE) + 8192ull;
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_nosnap_placed = cbwin_bind(0, 2, ubo_addr, 4096, &cbi3) &&
                            cbi3.buffer == gmem_buf && cbi3.offset == ubo_addr - HEAP_BASE && cbi3.range == 4096;
    unsetenv("LARECOMP_CBWIN_SNAPSHOT");
    atomic_store_explicit(&cbwin_snap_env, -1, memory_order_relaxed);

    // AB("cbsnap") == 1 disables snapshots just like LARECOMP_CBWIN_SNAPSHOT=0.
    setenv("LARECOMP_AB", "1:cbsnap", 1); ab_n = -1;
    uint64_t saved_frame = s_frame_counter;
    s_frame_counter = 1;
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_ab_placed = cbwin_bind(0, 2, ubo_addr, 4096, &cbi3) && cbi3.buffer == gmem_buf;
    gmem_placed = false; gmem_buf = saved_gbuf; gmem_size = saved_gsize;
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_ab_gcopy = !cbwin_bind(0, 2, ubo_addr, 4096, &cbi3);
    unsetenv("LARECOMP_AB"); ab_n = -1; s_frame_counter = saved_frame;

    // Non-Mali negative control (vendorID 0x5143) with LARECOMP_CB_WINDOW unset stays off.
    unsetenv("LARECOMP_CB_WINDOW");
    atomic_store_explicit(&cbwin_env, -2, memory_order_relaxed);
    g_vk.props.vendorID = 0x5143;
    bool ok_non_mali_off = !cbwin_wanted() && !cbwin_bind(0, 2, ubo_addr, 4096, &cbi3);

    // Turnip placed-heap short-circuit negative control (gmem_placed=true, gcopy_mode=false, unset LARECOMP_CB_WINDOW):
    // cbwin_wanted() must return false on !gmem_placed with 0 invocations of vk_driver_is_mali().
    g_vk.props.vendorID = 0x13B5;
    gmem_placed = true;
    atomic_store_explicit(&gcopy_mode, false, memory_order_relaxed);
    atomic_store_explicit(&cbwin_env, -2, memory_order_relaxed);
    uint32_t mali_calls0 = mock_mali_calls;
    bool ok_placed_shortcircuit = !cbwin_wanted() && (mock_mali_calls == mali_calls0);
    gmem_placed = false;
    atomic_store_explicit(&gcopy_mode, true, memory_order_relaxed);
    atomic_store_explicit(&cbwin_env, -2, memory_order_relaxed);

    // Real gcopy_retire() + wb_apply() integration: when cbwin_retire_armed is true, retiring writebacks copy into guest UBO,
    // bump cbwin_retire_gen (not gcopy_gen), and drop g_cbwin_cache so the next cbwin_bind() re-copies.
    uint8_t retire_src[64], mock_wb_buf[128];
    uint8_t *saved_wb_ptr = wb_ptr;
    memset(retire_src, 0x77, sizeof retire_src);
    memset(mock_wb_buf, 0x88, sizeof mock_wb_buf);
    memset(ubo + 0x180, 0x11, sizeof retire_src);
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    uint64_t ggen_before_retire = gcopy_gen;
    bool ok_retire_prime = cbwin_bind(0, 2, ubo_addr, 4096, &cbi3);
    uint64_t cb_pre_retire = stat_cb_copied;
    pthread_mutex_lock(&gc_lock);
    gc_back[0] = (GcBack){.src = retire_src, .addr = ubo_addr + 0x180, .len = sizeof retire_src, .seq = vk_pending_seq(), .shadow = NULL};
    gc_nback = 1;
    pthread_mutex_unlock(&gc_lock);
    gcopy_retire(vk_pending_seq());
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_gc_drop = ok_retire_prime && gcopy_gen == ggen_before_retire &&
                      memcmp(ubo + 0x180, retire_src, sizeof retire_src) == 0 &&
                      cbwin_bind(0, 2, ubo_addr, 4096, &cbi3) && stat_cb_copied == cb_pre_retire + 4096;
    wb_ptr = mock_wb_buf;
    pthread_mutex_lock(&wb_lock);
    wb_ent[0] = (WbEntry){.seq = vk_pending_seq(), .addr = ubo_addr + 0x1C0, .wb = 16, .hb = 1, .bpb = 4, .pitch = 64, .off = 0, .size = 64, .order = ++wb_order, .busy = true};
    pthread_mutex_unlock(&wb_lock);
    wb_apply(vk_pending_seq());
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_retire_drop = ok_gc_drop && gcopy_gen == ggen_before_retire &&
                          memcmp(ubo + 0x1C0, mock_wb_buf, 64) == 0 &&
                          cbwin_bind(0, 2, ubo_addr, 4096, &cbi3) && stat_cb_copied == cb_pre_retire + 8192;
    wb_ptr = saved_wb_ptr;

    // 2-thread pthread seqlock stress test: reader never observes torn bytes when cbwin_read_end_ok() returns true.
    SeqStressCtx sctx;
    memset(sctx.buf, 0xA1, sizeof sctx.buf);
    atomic_init(&sctx.stop, false);
    pthread_t sth;
    bool ok_th = pthread_create(&sth, NULL, seqlock_writer_fn, &sctx) == 0;
    uint32_t valid_reads = 0;
    bool ok_no_tear = ok_th;
    while (ok_th && (!atomic_load_explicit(&sctx.stop, memory_order_acquire) || valid_reads < 10)) {
        bool sc = false;
        uint64_t sg0 = cbwin_read_begin(&sc);
        uint8_t snap[64];
        memcpy(snap, sctx.buf, sizeof snap);
        if (cbwin_read_end_ok(sg0, sc)) {
            valid_reads++;
            uint8_t first = snap[0];
            if (first != 0xA1 && first != 0xB2) ok_no_tear = false;
            for (size_t b = 1; b < sizeof snap; b++)
                if (snap[b] != first) ok_no_tear = false;
        }
    }
    if (ok_th) pthread_join(sth, NULL);
    bool ok_seqlock_mt = ok_no_tear && valid_reads >= 10;

    // LARECOMP_CB_WINDOW=0 disables Mali default.
    setenv("LARECOMP_CB_WINDOW", "0", 1);
    atomic_store_explicit(&cbwin_env, -2, memory_order_relaxed);
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    bool ok_cbwin_off = !cbwin_wanted() && !cbwin_bind(0, 2, ubo_addr, 4096, &cbi3);
    unsetenv("LARECOMP_CB_WINDOW");

    // vk_exec_reset() clears cbwin_env, cbwin_snap_env, cbwin_retire_armed, gmem_placed, and g_cbpatch_dedupe across reboots.
    atomic_store_explicit(&cbwin_env, 1, memory_order_relaxed);
    atomic_store_explicit(&cbwin_snap_env, 0, memory_order_relaxed);
    atomic_store_explicit(&cbwin_retire_armed, true, memory_order_relaxed);
    gmem_placed = true;
    memset(cbw_patch[0].m, 0, sizeof cbw_patch[0].m);
    uint32_t act_pre = cbpatch_active_bytes(&cbw_patch[0]);
    cbpatch_commit(&g_cbpatch_dedupe, 0, vk_pending_seq(), &cbw_patch[0], act_pre, sp_commit);
    vk_exec_reset();
    uint32_t act_post = 0; VkSpan sp_post = {0};
    bool ok_reset = atomic_load_explicit(&cbwin_env, memory_order_relaxed) == -2 &&
                    atomic_load_explicit(&cbwin_snap_env, memory_order_relaxed) == -1 &&
                    !atomic_load_explicit(&cbwin_retire_armed, memory_order_relaxed) &&
                    !gmem_placed &&
                    !cbpatch_lookup(&g_cbpatch_dedupe, 0, vk_pending_seq(), &cbw_patch[0], &act_post, &sp_post);

    // Non-cbwin copy-mode negative control: after reset (cbwin disarmed) on Qualcomm/unmatched vendor, gcopy_retire/wb_apply
    // leave gcopy_gen & cbwin_retire_gen unchanged and guest_copy() keeps 100% cache hits (0 extra bytes copied).
    g_vk.props.vendorID = 0x5143;
    gcopy_mode = true; gmem_placed = false;
    uint8_t *noncb_buf = alloc_guest(256);
    uint64_t noncb_addr = (uint64_t)(uintptr_t)noncb_buf;
    memset(noncb_buf, 0x3C, 256);
    uint64_t rgen0 = atomic_load_explicit(&cbwin_retire_gen, memory_order_relaxed);
    uint64_t ggen0 = gcopy_gen;
    VkSpan gc1 = guest_copy(noncb_addr, 256);
    uint64_t cb_noncb = stat_cb_copied;
    pthread_mutex_lock(&gc_lock);
    gc_back[0] = (GcBack){.src = retire_src, .addr = ubo_addr + 0x180, .len = sizeof retire_src, .seq = vk_pending_seq(), .shadow = NULL};
    gc_nback = 1;
    pthread_mutex_unlock(&gc_lock);
    gcopy_retire(vk_pending_seq());
    wb_ptr = mock_wb_buf;
    pthread_mutex_lock(&wb_lock);
    wb_ent[0] = (WbEntry){.seq = vk_pending_seq(), .addr = ubo_addr + 0x1C0, .wb = 16, .hb = 1, .bpb = 4, .pitch = 64, .off = 0, .size = 64, .order = ++wb_order, .busy = true};
    pthread_mutex_unlock(&wb_lock);
    wb_apply(vk_pending_seq());
    wb_ptr = saved_wb_ptr;
    VkSpan gc2 = guest_copy(noncb_addr, 256);
    bool ok_non_cbwin_zero_churn = gc1.off == gc2.off && stat_cb_copied == cb_noncb && gcopy_gen == ggen0 &&
                                   atomic_load_explicit(&cbwin_retire_gen, memory_order_relaxed) == rgen0;

    g_vk.props.vendorID = 0;
    bool ok_cb_all = ok_cb1 && ok_cb_hit && ok_cb_in && ok_cb_out && ok_b2b && ok_async_drop &&
                     ok_nosnap_gcopy && ok_nosnap_placed && ok_ab_placed && ok_ab_gcopy &&
                     ok_non_mali_off && ok_placed_shortcircuit && ok_retire_drop && ok_seqlock_mt &&
                     ok_cbwin_off && ok_reset && ok_non_cbwin_zero_churn;
    if (!ok_cb_all)
        printf("  cbwin fail breakdown: cb1=%d hit=%d in=%d out=%d b2b=%d seqlock=%d(mid=%d) nosnap=(%d,%d) ab=(%d,%d) nonmali=%d short=%d retire=%d mt=%d off=%d rst=%d nochurn=%d\n",
               ok_cb1, ok_cb_hit, ok_cb_in, ok_cb_out, ok_b2b, ok_async_drop, ok_mid_write,
               ok_nosnap_gcopy, ok_nosnap_placed, ok_ab_placed, ok_ab_gcopy, ok_non_mali_off,
               ok_placed_shortcircuit, ok_retire_drop, ok_seqlock_mt, ok_cbwin_off, ok_reset, ok_non_cbwin_zero_churn);
    check("cbwin integration: base reuse, in/out updates, b2b/seqlock, retire, mt stress, #453, short-circuit, non-Mali off, zero non-cbwin churn, and reset", ok_cb_all);
    }

    check("Vulkan mock synchronization validator recorded 0 hazards", !mock_hazards);
    printf("test_rt_load_upload: %s (%d failure(s))\n", failures ? "FAILED" : "all passed", failures);
    return failures != 0;
}
