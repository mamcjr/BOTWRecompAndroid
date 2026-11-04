// BC textures drawn into as blocks without BC on the GPU (runtime/nvn/vulkan/textures.c: shadow images, decoded after
// the draws): what was drawn into a shadow is decoded before the shadow goes, and a new shadow holds the texture's
// blocks before the first draw. Includes the implementation, the Vulkan work stubbed and counted. TEXTURES_C: another
// textures.c to test (the same checks on an older version).
#include <stdio.h>
#ifndef TEXTURES_C
#define TEXTURES_C "../runtime/nvn/vulkan/textures.c"
#endif
#include TEXTURES_C
#include "../runtime/nvn/tegra.c"

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

static NvnTexture tex;
static TexBackend be;
static int images, readbacks, readback_regions, uploads, fills, image_copies, image_copy_regions;
static uint64_t filled_blocks, image_extent[3];
VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                        VmaAllocation *mem, VmaAllocationInfo *info) {
    (void)a, (void)ai, (void)info;
    images++;
    image_extent[0] = ci->extent.width, image_extent[1] = ci->extent.height, image_extent[2] = ci->extent.depth;
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
    (void)cb, (void)l, (void)b;
    if (i == be.image) { uploads += (int)n; return; }  // (decoded blocks into the texture's own image)
    fills += (int)n;  // (blocks into a shadow)
    for (uint32_t k = 0; k < n; k++) filled_blocks += (uint64_t)r[k].imageExtent.width * r[k].imageExtent.height * r[k].imageExtent.depth;
}
static VKAPI_ATTR void VKAPI_CALL copy_i2i(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
                                           const VkImageCopy *r) {
    (void)cb, (void)s, (void)sl, (void)d, (void)dl, (void)r;
    image_copies++, image_copy_regions += (int)n;
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
PFN_vkCmdCopyImage vkCmdCopyImage = copy_i2i;
PFN_vkCmdPipelineBarrier2 vkCmdPipelineBarrier2 = barrier2;
PFN_vkCreateImageView vkCreateImageView = create_view;
PFN_vkDestroyImageView vkDestroyImageView = destroy_view;
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;

static int failures;
static void check(const char *name, bool ok) {
    printf("%s %s (shadows %d, last %llux%llux%llu, readbacks %d of %d regions, uploads %d, shadow fills %d of %llu blocks, shadow copies %d of %d levels)\n",
           ok ? "ok  " : "FAIL", name, images, (unsigned long long)image_extent[0], (unsigned long long)image_extent[1],
           (unsigned long long)image_extent[2], readbacks, readback_regions, uploads, fills, (unsigned long long)filled_blocks,
           image_copies, image_copy_regions);
    failures += !ok;
}

// A 32x32x32 BC4 volume (6 levels) in guest memory, decoded into R8 (bc_vf), as BOTW's dissolve noise.
static uint8_t guest[1 << 20];
static void outside(void);
static void setup(void) {
    if (tex.backend) outside();  // (the previous case's shadows decoded: off the dirty list)
    memset(&tex, 0, sizeof tex), memset(&be, 0, sizeof be);
    tex.width = tex.height = tex.depth = 32, tex.format = 0x49, tex.backend = &be, tex.addr = (uintptr_t)guest;
    tex.layout = (TexLayout){.width = 32, .height = 32, .depth = 32, .layers = 1, .levels = 6, .fmt = &bc4};
    tegra_layout_init(&tex.layout);
    be.bc_vf = VK_FORMAT_BC4_UNORM_BLOCK, be.format = VK_FORMAT_R8_UNORM, be.base_type = VK_IMAGE_VIEW_TYPE_3D;
    be.width = be.height = be.depth = 32, be.layers = 1, be.levels = 6, be.samples = 1, be.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    images = readbacks = readback_regions = uploads = fills = image_copies = image_copy_regions = 0;
    filled_blocks = 0;
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
    setvbuf(stdout, NULL, _IONBF, 0);  // (a crash keeps what was printed)
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
    // a pass in the shadow's format through a view that draws without a shadow (here: a level past the texture's):
    // the shadow is decoded first, the pass drawing into the decoded image itself
    setup();
    draw(0x2C, 0);
    draw(0x2C, 7);
    check("P1: same format, view without a shadow: decoded first", readbacks == 1);
    // P2: a new shadow holds the texture's blocks before the first draw (a draw may cover part of a slice; the whole
    // slice is decoded): every level's blocks from guest memory - 8x8x32 + 4x4x16 + 2x2x8 + 1x1x4 + 1x1x2 + 1x1x1
    setup();
    draw(0x2C, 1);
    check("P2: new shadow filled from guest memory, all 6 levels", fills == 6 && filled_blocks == 2048 + 256 + 32 + 4 + 2 + 1);
    outside();
    draw(0x17, 2);  // another format: the old shadow's blocks (level 1 drawn since) go into the new one
    check("P2: a shadow in another format takes the old one's blocks", image_copies == 1 && image_copy_regions == 6 && fills == 6);
    // P3: the shadow's level 0 as small as its levels allow - the 32x32x32 volume's 8x8 blocks (not 32x32); a 2048x16
    // texture with 12 levels: 512x4 blocks, widened to 2048 for the level count (not 2048x2048); 20x20 with 5 levels
    setup();
    draw(0x2C, 0);
    check("P3: 32x32x32, 6 levels: shadow 8x8x32", image_extent[0] == 8 && image_extent[1] == 8 && image_extent[2] == 32);
    setup();
    tex.width = be.width = 2048, tex.height = be.height = 16, tex.depth = be.depth = 1, be.levels = 12;
    be.base_type = VK_IMAGE_VIEW_TYPE_2D;
    tex.layout = (TexLayout){.width = 2048, .height = 16, .depth = 1, .layers = 1, .levels = 12, .fmt = &bc4};
    tegra_layout_init(&tex.layout);
    draw(0x2C, 0);
    check("P3: 2048x16, 12 levels: shadow 2048x4", image_extent[0] == 2048 && image_extent[1] == 4 && image_extent[2] == 1);
    setup();
    tex.width = be.width = 20, tex.height = be.height = 20, tex.depth = be.depth = 1, be.levels = 5;
    be.base_type = VK_IMAGE_VIEW_TYPE_2D;
    tex.layout = (TexLayout){.width = 20, .height = 20, .depth = 1, .layers = 1, .levels = 5, .fmt = &bc4};
    tegra_layout_init(&tex.layout);
    draw(0x2C, 0);
    check("P3: 20x20, 5 levels: shadow 16x8", image_extent[0] == 16 && image_extent[1] == 8 && image_extent[2] == 1);
    if (failures) { printf("test_bc_shadow: %d check(s) failed\n", failures); return 1; }
    printf("test_bc_shadow: all passed\n");
    return 0;
}
