// BC textures drawn into as blocks without BC on the GPU (runtime/nvn/vulkan/textures.c: shadow images, decoded after
// the draws): what was drawn into a shadow is decoded before the shadow goes. Includes the implementation, the Vulkan
// work stubbed and counted. TEXTURES_C: another textures.c to test (the same checks on an older version).
#include <stdio.h>
#ifndef TEXTURES_C
#define TEXTURES_C "../runtime/nvn/vulkan/textures.c"
#endif
#include TEXTURES_C

// ---- the rest of the runtime, as far as the shadows reach it
VkG g_vk;
void rt_fatal_trace(void) { abort(); }
static uint8_t staging[1 << 20];
VkSpan vk_transient(uint64_t size, uint64_t align) { (void)size, (void)align; return (VkSpan){VK_NULL_HANDLE, 0, staging}; }
VkSpan vk_readback(uint64_t size) { (void)size; return (VkSpan){VK_NULL_HANDLE, 0, staging}; }
void vk_readback_ready(uint64_t size) { (void)size; }
VkCommandBuffer vk_cmd(void) { return VK_NULL_HANDLE; }
VkCommandBuffer vk_upload_cmd(void) { return VK_NULL_HANDLE; }
void vk_full_barrier(VkCommandBuffer cb) { (void)cb; }
void renderer_flush(void) {}
uint64_t vk_pending_seq(void) { return 1; }
uint64_t vk_frame_counter(void) { return 1; }
void vk_rp_forget_view(VkImageView v) { (void)v; }
void bc1_to_astc(const uint8_t *bc1, uint8_t *astc, uint64_t n) { (void)bc1, (void)astc, (void)n; }
static const FormatInfo bc4 = {.bw = 4, .bh = 4, .bpb = 8}, texel8 = {.bw = 1, .bh = 1, .bpb = 8};
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { (void)img, (void)w, (void)h, (void)lv; }
bool vk_driver_is_qualcomm(void) { return false; }
uint64_t SDL_GetTicksNS(void) { return 0; }
uint64_t prof_views_created;
const FormatInfo *nvn_format_info(uint32_t f) { return f == 0x49 ? &bc4 : &texel8; }

static int images, readbacks, readback_regions, uploads;
static uint64_t image_size;
VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                        VmaAllocation *mem, VmaAllocationInfo *info) {
    (void)a, (void)ai, (void)info;
    images++;
    image_size = (uint64_t)ci->extent.width * ci->extent.height * ci->extent.depth * ci->arrayLayers;
    *img = (VkImage)(uintptr_t)(0x100 + images);
    *mem = (VmaAllocation)(uintptr_t)(0x100 + images);
    return VK_SUCCESS;
}
void vmaDestroyImage(VmaAllocator a, VkImage img, VmaAllocation mem) { (void)a, (void)img, (void)mem; }
static VKAPI_ATTR void VKAPI_CALL copy_i2b(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) {
    (void)cb, (void)i, (void)l, (void)b, (void)r;
    readbacks++, readback_regions += (int)n;
}
static VKAPI_ATTR void VKAPI_CALL copy_b2i(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
    (void)cb, (void)i, (void)l, (void)b, (void)r;
    uploads += (int)n;
}
static VKAPI_ATTR void VKAPI_CALL barrier2(VkCommandBuffer cb, const VkDependencyInfo *d) { (void)cb, (void)d; }
static VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *v) {
    (void)d, (void)ci, (void)a;
    static uintptr_t n;
    *v = (VkImageView)(0x10000 + ++n);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_view(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d, (void)v, (void)a; }
PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = copy_i2b;
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = copy_b2i;
PFN_vkCmdPipelineBarrier2 vkCmdPipelineBarrier2 = barrier2;
PFN_vkCreateImageView vkCreateImageView = create_view;
PFN_vkDestroyImageView vkDestroyImageView = destroy_view;
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;

static int failures;
static void check(const char *name, bool ok) {
    printf("%s %s (shadows %d, readbacks %d of %d regions, uploads %d)\n", ok ? "ok  " : "FAIL", name, images, readbacks,
           readback_regions, uploads);
    failures += !ok;
}

// A 32x32x32 BC4 volume (6 levels), decoded into R8 (bc_vf), as BOTW's dissolve noise.
static NvnTexture tex;
static TexBackend be;
static void setup(void) {
    memset(&tex, 0, sizeof tex), memset(&be, 0, sizeof be);
    tex.width = tex.height = tex.depth = 32, tex.format = 0x49, tex.layout.fmt = &bc4, tex.backend = &be;
    be.bc_vf = VK_FORMAT_BC4_UNORM_BLOCK, be.format = VK_FORMAT_R8_UNORM, be.base_type = VK_IMAGE_VIEW_TYPE_3D;
    be.width = be.height = be.depth = 32, be.layers = 1, be.levels = 6, be.samples = 1, be.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    images = readbacks = readback_regions = uploads = 0;
}

static VkImageView draw(uint16_t fmt, uint8_t level) {  // a pass drawing into `level` through a view of nvn format fmt
    NvnTexture *targets[1] = {&tex};
    NvnTextureView v = {.format = fmt, .setmask = 4 | 1, .base_level = level};
#ifdef OLD_RESOLVE_API
    vk_bc_shadows_resolve(targets, 1);
#else
    uint8_t has = 1;
    vk_bc_shadows_resolve(targets, &v, &has, 1);
#endif
    VkFormat f;
    uint32_t w, h;
    return vk_bc_shadow_view(&tex, &v, &f, &w, &h);
}

static void outside(void) {  // anything outside a pass
#ifdef OLD_RESOLVE_API
    vk_bc_shadows_resolve(NULL, 0);
#else
    vk_bc_shadows_resolve(NULL, NULL, NULL, 0);
#endif
}

int main(void) {
    setenv("LARECOMP_3D_RT", "2", 1);  // (3D targets at every level: the dissolve volume's mips are drawn)
    // P1: level 0 drawn as RGBA16_UINT, then level 1 as RG32_UINT (another 8-byte format): level 0's blocks are decoded
    // before its shadow goes, and level 1's afterwards
    setup();
    draw(0x2C, 0);
    draw(0x17, 1);
    check("P1: format change after a draw: level 0 decoded first", readbacks == 1);
    outside();
    check("P1: then level 1 decoded", readbacks == 2);
    // control: the same format in both passes - one shadow, both levels decoded together outside the passes
    setup();
    draw(0x2C, 0);
    draw(0x2C, 1);
    check("control: same format - nothing decoded between the passes", readbacks == 0 && images == 1);
    outside();
    check("control: both levels decoded together", readbacks == 1 && readback_regions == 2);  // (a slice each)
    if (failures) { printf("test_bc_shadow: %d check(s) failed\n", failures); return 1; }
    printf("test_bc_shadow: all passed\n");
    return 0;
}
