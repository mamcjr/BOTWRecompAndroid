// Vulkan textures: NVN formats, images, change-detected uploads from guest memory, views and samplers
// (counterpart of the texture half of metal.m and the view helpers of renderer.m).
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "../astc.h"
#include "../renderer.h"
#include "../texpatch.h"

// ================================================================== formats
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;

// DEPTH24_STENCIL8 as itself where the GPU has it for everything the game does with depth (NVIDIA, Intel): half the
// bytes of D32_SFLOAT_S8_UINT (4 instead of 8 per pixel) for every depth test, clear and copy - the game copies its
// full-size depth twice a frame. AMD has no D24S8 attachments: D32S8 there (and on Metal). LARECOMP_VK_D24=0: D32S8.
static VkFormat depth24_stencil8(void) {
    static VkFormat f;
    if (!f) {
        const char *e = getenv("LARECOMP_VK_D24");
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_D24_UNORM_S8_UINT, &p);
        VkFormatFeatureFlags want = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        bool ok = (p.optimalTilingFeatures & want) == want && !(e && !atoi(e));
        f = ok ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        LOG("[vk] depth24 stencil8: %s\n", ok ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
    }
    return f;
}

static VkFmt fmt_of(uint32_t f) {
    switch (f) {
    case 0x01: return (VkFmt){VK_FORMAT_R8_UNORM};
    case 0x02: return (VkFmt){VK_FORMAT_R8_SNORM};
    case 0x03: return (VkFmt){VK_FORMAT_R8_UINT};
    case 0x04: return (VkFmt){VK_FORMAT_R8_SINT};
    case 0x05: return (VkFmt){VK_FORMAT_R16_SFLOAT};
    case 0x06: return (VkFmt){VK_FORMAT_R16_UNORM};
    case 0x07: return (VkFmt){VK_FORMAT_R16_SNORM};
    case 0x08: return (VkFmt){VK_FORMAT_R16_UINT};
    case 0x09: return (VkFmt){VK_FORMAT_R16_SINT};
    case 0x0A: return (VkFmt){VK_FORMAT_R32_SFLOAT};
    case 0x0B: return (VkFmt){VK_FORMAT_R32_UINT};
    case 0x0C: return (VkFmt){VK_FORMAT_R32_SINT};
    case 0x0D: return (VkFmt){VK_FORMAT_R8G8_UNORM};
    case 0x0E: return (VkFmt){VK_FORMAT_R8G8_SNORM};
    case 0x0F: return (VkFmt){VK_FORMAT_R8G8_UINT};
    case 0x10: return (VkFmt){VK_FORMAT_R8G8_SINT};
    case 0x11: return (VkFmt){VK_FORMAT_R16G16_SFLOAT};
    case 0x12: return (VkFmt){VK_FORMAT_R16G16_UNORM};
    case 0x13: return (VkFmt){VK_FORMAT_R16G16_SNORM};
    case 0x14: return (VkFmt){VK_FORMAT_R16G16_UINT};
    case 0x15: return (VkFmt){VK_FORMAT_R16G16_SINT};
    case 0x16: return (VkFmt){VK_FORMAT_R32G32_SFLOAT};
    case 0x17: return (VkFmt){VK_FORMAT_R32G32_UINT};
    case 0x18: return (VkFmt){VK_FORMAT_R32G32_SINT};
    case 0x19: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 1, 1};  // RGB8: expanded on upload
    case 0x25: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    case 0x26: return (VkFmt){VK_FORMAT_R8G8B8A8_SNORM};
    case 0x27: return (VkFmt){VK_FORMAT_R8G8B8A8_UINT};
    case 0x28: return (VkFmt){VK_FORMAT_R8G8B8A8_SINT};
    case 0x29: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT};
    case 0x2A: return (VkFmt){VK_FORMAT_R16G16B16A16_UNORM};
    case 0x2B: return (VkFmt){VK_FORMAT_R16G16B16A16_SNORM};
    case 0x2C: return (VkFmt){VK_FORMAT_R16G16B16A16_UINT};
    case 0x2D: return (VkFmt){VK_FORMAT_R16G16B16A16_SINT};
    case 0x2E: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT};
    case 0x2F: return (VkFmt){VK_FORMAT_R32G32B32A32_UINT};
    case 0x30: return (VkFmt){VK_FORMAT_R32G32B32A32_SINT};
    case 0x31: return (VkFmt){VK_FORMAT_S8_UINT};
    case 0x32: return (VkFmt){VK_FORMAT_D16_UNORM};
    case 0x33: return (VkFmt){VK_FORMAT_D32_SFLOAT};  // DEPTH24 (as on Metal: no 24-bit depth there)
    case 0x34: return (VkFmt){VK_FORMAT_D32_SFLOAT};
    case 0x35: return (VkFmt){depth24_stencil8()};
    case 0x36: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x37: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB, 0, 1};
    case 0x38: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB};
    case 0x39: return (VkFmt){VK_FORMAT_R4G4B4A4_UNORM_PACK16};   // Metal ABGR4Unorm
    case 0x3A: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};     // Metal B5G6R5Unorm
    case 0x3B: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};   // Metal A1BGR5Unorm
    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};
    case 0x3D: return (VkFmt){VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    case 0x3E: return (VkFmt){VK_FORMAT_A2B10G10R10_UINT_PACK32};
    case 0x3F: return (VkFmt){VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    case 0x40: return (VkFmt){VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    case 0x41: case 0x42: return (VkFmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    case 0x43: return (VkFmt){VK_FORMAT_BC2_UNORM_BLOCK};
    case 0x44: return (VkFmt){VK_FORMAT_BC3_UNORM_BLOCK};
    case 0x45: case 0x46: return (VkFmt){VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
    case 0x47: return (VkFmt){VK_FORMAT_BC2_SRGB_BLOCK};
    case 0x48: return (VkFmt){VK_FORMAT_BC3_SRGB_BLOCK};
    case 0x49: return (VkFmt){VK_FORMAT_BC4_UNORM_BLOCK};
    case 0x4A: return (VkFmt){VK_FORMAT_BC4_SNORM_BLOCK};
    case 0x4B: return (VkFmt){VK_FORMAT_BC5_UNORM_BLOCK};
    // 0x4C = signed RGTC2; LARECOMP_BC5_UNORM=1 decodes it unsigned (see metal.m / notes/PLAN.md)
    case 0x4C: return (VkFmt){getenv("LARECOMP_BC5_UNORM") ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK};
    case 0x4D: return (VkFmt){VK_FORMAT_BC7_UNORM_BLOCK};
    case 0x4E: return (VkFmt){VK_FORMAT_BC7_SRGB_BLOCK};
    case 0x4F: return (VkFmt){VK_FORMAT_BC6H_SFLOAT_BLOCK};
    case 0x50: return (VkFmt){VK_FORMAT_BC6H_UFLOAT_BLOCK};
    case 0x6D: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 1};
    case 0x71: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1};
    case 0x76: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT, 0, 1};
    case 0x79: return (VkFmt){VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    case 0x7A: return (VkFmt){VK_FORMAT_ASTC_5x4_UNORM_BLOCK};
    case 0x7B: return (VkFmt){VK_FORMAT_ASTC_5x5_UNORM_BLOCK};
    case 0x7C: return (VkFmt){VK_FORMAT_ASTC_6x5_UNORM_BLOCK};
    case 0x7D: return (VkFmt){VK_FORMAT_ASTC_6x6_UNORM_BLOCK};
    case 0x7E: return (VkFmt){VK_FORMAT_ASTC_8x5_UNORM_BLOCK};
    case 0x7F: return (VkFmt){VK_FORMAT_ASTC_8x6_UNORM_BLOCK};
    case 0x80: return (VkFmt){VK_FORMAT_ASTC_8x8_UNORM_BLOCK};
    case 0x81: return (VkFmt){VK_FORMAT_ASTC_10x5_UNORM_BLOCK};
    case 0x82: return (VkFmt){VK_FORMAT_ASTC_10x6_UNORM_BLOCK};
    case 0x83: return (VkFmt){VK_FORMAT_ASTC_10x8_UNORM_BLOCK};
    case 0x84: return (VkFmt){VK_FORMAT_ASTC_10x10_UNORM_BLOCK};
    case 0x85: return (VkFmt){VK_FORMAT_ASTC_12x10_UNORM_BLOCK};
    case 0x86: return (VkFmt){VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
    case 0x87: return (VkFmt){VK_FORMAT_ASTC_4x4_SRGB_BLOCK};
    case 0x88: return (VkFmt){VK_FORMAT_ASTC_5x4_SRGB_BLOCK};
    case 0x89: return (VkFmt){VK_FORMAT_ASTC_5x5_SRGB_BLOCK};
    case 0x8A: return (VkFmt){VK_FORMAT_ASTC_6x5_SRGB_BLOCK};
    case 0x8B: return (VkFmt){VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    case 0x8C: return (VkFmt){VK_FORMAT_ASTC_8x5_SRGB_BLOCK};
    case 0x8D: return (VkFmt){VK_FORMAT_ASTC_8x6_SRGB_BLOCK};
    case 0x8E: return (VkFmt){VK_FORMAT_ASTC_8x8_SRGB_BLOCK};
    case 0x8F: return (VkFmt){VK_FORMAT_ASTC_10x5_SRGB_BLOCK};
    case 0x90: return (VkFmt){VK_FORMAT_ASTC_10x6_SRGB_BLOCK};
    case 0x91: return (VkFmt){VK_FORMAT_ASTC_10x8_SRGB_BLOCK};
    case 0x92: return (VkFmt){VK_FORMAT_ASTC_10x10_SRGB_BLOCK};
    case 0x93: return (VkFmt){VK_FORMAT_ASTC_12x10_SRGB_BLOCK};
    case 0x94: return (VkFmt){VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
    case 0x95: case 0x96: case 0x97: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    case 0x98: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM, 0, 1};
    case 0x99: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM};
    case 0x9A: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB, 0, 1};
    case 0x9B: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB};
    default: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    }
}

bool renderer_format_is_depth(uint32_t f) { return f >= 0x31 && f <= 0x36; }
bool renderer_format_has_stencil(uint32_t f) { return f == 0x31 || f == 0x35 || f == 0x36; }
VkFormat vk_format(uint32_t f) { return fmt_of(f).vf; }
bool vk_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }
bool renderer_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }

static VkImageAspectFlags aspect_of(uint32_t f) {
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (renderer_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool format_supports(VkFormat f, VkFormatFeatureFlags want) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

#ifdef __ANDROID__
// BC textures on a GPU without BC (Mali, and others outside Adreno): decoded on the CPU at upload (bcdec, MIT /
// Unlicense) into the smallest plain format that holds them - BC4 R8, BC5 RG8 (2x the size), BC2/3/7 RGBA8 (4x),
// BC6H RGBA16F - and BC1 into ASTC 4x4 where the GPU has it (bc1_astc), else RGBA8 (8x). They used to become RGBA8
// images left unwritten: colored 4x4 garbage on every BC texture (a Mali-G615, 2026-10-17).
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE  // (signed BC4 / BC5: the game's normal maps)
#include "../bcdec.h"
#include "../bc1_astc.h"

// BC1 (most of BOTW's textures: ~150 MB of them around a village, ~1.1 GB as RGBA8 - the Mali phones' "low memory"
// kills and 1-5 fps after a few minutes, #143 #155 #162) into ASTC 4x4 instead where the GPU samples it (every Mali):
// the same block, 2x BC1's size, the colours within a step or two (bc1_astc.c). LARECOMP_BC1_ASTC=0: RGBA8 as before.
static bool bc1_astc(void) {
    static int ok = -1;
    if (ok < 0) {
        const char *e = getenv("LARECOMP_BC1_ASTC");
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        ok = (!e || atoi(e)) && format_supports(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, need) &&
             format_supports(VK_FORMAT_ASTC_4x4_SRGB_BLOCK, need);
        if (ok) LOG("[vk] BC1 textures as ASTC 4x4\n");
    }
    return ok;
}

// The format a BC texture is decoded into without BC; *bpp: its bytes per texel (0: ASTC 4x4 blocks - bc1_to_astc)
static VkFormat bc_target(VkFormat bc, uint32_t *bpp) {
    switch (bc) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        if (bc1_astc()) { *bpp = 0; return VK_FORMAT_ASTC_4x4_UNORM_BLOCK; }
        *bpp = 4; return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        if (bc1_astc()) { *bpp = 0; return VK_FORMAT_ASTC_4x4_SRGB_BLOCK; }
        *bpp = 4; return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC4_UNORM_BLOCK: *bpp = 1; return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_BC4_SNORM_BLOCK: *bpp = 1; return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_BC5_UNORM_BLOCK: *bpp = 2; return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_BC5_SNORM_BLOCK: *bpp = 2; return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: *bpp = 8; return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: *bpp = 4; return VK_FORMAT_R8G8B8A8_SRGB;
    default: *bpp = 4; return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

static bool is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }

// wb x hb blocks (bsize bytes each, rows of blocks) -> texels, rows of wb * 4 texels of bpp bytes
static void bc_decode(VkFormat bc, const uint8_t *blocks, uint32_t wb, uint32_t hb, uint32_t bsize, uint8_t *out, uint32_t bpp) {
    int pitch = (int)(wb * 4 * bpp);
    for (uint32_t y = 0; y < hb; y++)
        for (uint32_t x = 0; x < wb; x++) {
            const uint8_t *b = blocks + ((uint64_t)y * wb + x) * bsize;
            uint8_t *o = out + (uint64_t)y * 4 * (uint64_t)pitch + (uint64_t)x * 4 * bpp;
            switch (bc) {
            case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: bcdec_bc1(b, o, pitch); break;
            case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: bcdec_bc2(b, o, pitch); break;
            case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: bcdec_bc3(b, o, pitch); break;
            case VK_FORMAT_BC4_UNORM_BLOCK: bcdec_bc4(b, o, pitch, 0); break;
            case VK_FORMAT_BC4_SNORM_BLOCK: bcdec_bc4(b, o, pitch, 1); break;
            case VK_FORMAT_BC5_UNORM_BLOCK: bcdec_bc5(b, o, pitch, 0); break;
            case VK_FORMAT_BC5_SNORM_BLOCK: bcdec_bc5(b, o, pitch, 1); break;
            case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: {  // (RGB halves -> RGBA, alpha 1)
                uint16_t rgb[16 * 3];
                bcdec_bc6h_half(b, rgb, 4 * 3, bc == VK_FORMAT_BC6H_SFLOAT_BLOCK);
                for (int py = 0; py < 4; py++) {
                    uint16_t *d = (uint16_t *)(o + (uint64_t)py * (uint64_t)pitch);
                    for (int px = 0; px < 4; px++) {
                        d[px * 4 + 0] = rgb[(py * 4 + px) * 3 + 0];
                        d[px * 4 + 1] = rgb[(py * 4 + px) * 3 + 1];
                        d[px * 4 + 2] = rgb[(py * 4 + px) * 3 + 2];
                        d[px * 4 + 3] = 0x3C00;
                    }
                }
                break;
            }
            default: bcdec_bc7(b, o, pitch); break;
            }
        }
}
#endif

// ================================================================== output resolution / per-frame clears
// Render targets are allocated at one of two scales (runtime/nvn/output_scale.c sets both every presented frame;
// the rules come from SavageWindRecomp, which scales BOTW 1.8.2 the same way):
//   RS_OUT    the game's final image (1920x1080, its halves) and the display images: the screen's resolution, so the
//             HUD and menus are drawn sharp
//   RS_SCENE  the 3D scene's targets (1600x900 docked, the game's dynamic sizes down to 1280x720, their fractions):
//             the internal resolution (Video > Internal resolution)
static float g_rs[3][2] = {{1, 1}, {1, 1}, {1, 1}};
void renderer_get_scale(float *sx, float *sy) { *sx = g_rs[RS_OUT][0]; *sy = g_rs[RS_OUT][1]; }
void renderer_set_scale(float sx, float sy) { g_rs[RS_OUT][0] = sx; g_rs[RS_OUT][1] = sy; }
void renderer_get_scene_scale(float *sx, float *sy) { *sx = g_rs[RS_SCENE][0]; *sy = g_rs[RS_SCENE][1]; }
void renderer_set_scene_scale(float sx, float sy) { g_rs[RS_SCENE][0] = sx; g_rs[RS_SCENE][1] = sy; }
uint64_t renderer_present_count(void);
static bool rs_stale(const TexBackend *b) {
    return b->rs_group && (b->sx != g_rs[b->rs_group][0] || b->sy != g_rs[b->rs_group][1]);
}

// Which scale a texture takes (RS_*): the game's screen-sized targets are 16:9 2D images or 2D arrays (BOTW renders
// some half-size passes into layers of 800x450x2 arrays) with the compressible flag (render targets) or the display
// flag. Everything else keeps its guest size: shadow maps, LUTs, cubemaps, the small odd-sized steps of the bloom and
// exposure chains (sampled with normalized coordinates, they work at any size). LARECOMP_NO_SCALE=1: none scaled.
static uint8_t tex_scale_group(const NvnTexture *t) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_NO_SCALE") == NULL;
    if (!on) return RS_NONE;
    if ((t->target != 1 && t->target != 4) || t->samples > 1 || !(t->flags & 9) || t->width < 32 || t->height < 16) return RS_NONE;
    float a = (float)t->width / (float)t->height;
    if (a < 1.74f || a > 1.81f) return RS_NONE;
    if (t->flags & 1) return RS_OUT;  // NVN_TEXTURE_FLAGS_DISPLAY: the images the game presents
    for (int k = 0; k < 4; k++)       // the final image's 1920x1080 and its halves (rounded either way)
        if ((t->width == 1920u >> k || t->width == (1920u + (1u << k) - 1) >> k) &&
            (t->height == 1080u >> k || t->height == (1080u + (1u << k) - 1) >> k))
            return RS_OUT;
    return RS_SCENE;
}

bool renderer_texture_first_write(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!b || !b->presented || (b->sx == b->sy) || b->touched_frame == total_frames + 1) return false;
    b->touched_frame = total_frames + 1;
    return true;
}

static uint32_t g_level_gen = 1;
void vk_level_change_check(void) {
    extern const char *game_level_name(void);
    static char last[64];
    const char *lv = game_level_name();
    if (strncmp(lv, last, sizeof last - 1) != 0) {
        strlcpy(last, lv, sizeof last);
        if (!strncmp(lv, "TelephoneBox", 12) || getenv("LARECOMP_CLEAR_ALL_LEVELS")) g_level_gen++;
    }
}

// (Link's Awakening cleared its scaled scene targets at their first use in each frame. BOTW keeps data in them from
// frame to frame, as on the Switch, where nothing clears them: opt-in, LARECOMP_FRAME_CLEAR=1.)
bool renderer_texture_frame_fresh(NvnTexture *t) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_FRAME_CLEAR") != NULL;
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!on || !b || !b->scalable || b->frame_cleared == total_frames + 1) return false;
    b->frame_cleared = total_frames + 1;
    return true;
}

bool renderer_texture_level_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    static int keep = -1;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_TARGETS") != NULL;
    if (!b || keep || b->level_gen == g_level_gen) return false;
    if (!b->scalable) { b->level_gen = g_level_gen; return false; }
    bool fresh = b->level_gen != 0;
    b->level_gen = g_level_gen;
    return fresh;
}

void renderer_texture_mark_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b) b->presented = true;
}

bool renderer_texture_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && b->presented;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && b->scalable) { *sx = b->sx; *sy = b->sy; }
    else { *sx = 1; *sy = 1; }
}

// ================================================================== images
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// Images destroyed by the guest are kept until the submissions that may use them have completed.
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; uint64_t seq; } Dead;
static Dead *dead;
static int ndead, cap_dead;
static void views_forget(TexBackend *b);
#ifdef __ANDROID__
// (the render thread (vulkan/async.c) collects and defers too, the game thread meanwhile: the list grew under a
// collection - a crash in free() after a level change)
static pthread_mutex_t dead_lock = PTHREAD_MUTEX_INITIALIZER;
#define DEAD_LOCK() pthread_mutex_lock(&dead_lock)
#define DEAD_UNLOCK() pthread_mutex_unlock(&dead_lock)
#else
#define DEAD_LOCK() ((void)0)
#define DEAD_UNLOCK() ((void)0)
#endif

// Views destroyed and not handed out again since (issue #176: Mali's driver crashed in vkCmdBeginRendering after
// ~30 min, twice at the same place - a destroyed attachment view is the suspect): exec.c begin_pass logs a pass that
// begins with one ("DEAD VIEW"). On unless LARECOMP_CHECK_VIEWS=0; a lock and a short probe per view, per pass.
#define DV_N 65536
static uint64_t dv_tab[DV_N];
static pthread_mutex_t dv_lock = PTHREAD_MUTEX_INITIALIZER;
static int dv_on = -1;
static void dv_set(VkImageView v, bool dead) {
    if (dv_on < 0) { const char *e = getenv("LARECOMP_CHECK_VIEWS"); dv_on = !e || atoi(e) != 0; }
    if (!dv_on || !v) return;
    uint64_t k = (uint64_t)v, h = (k * 0x9E3779B97F4A7C15ull) >> 48;
    pthread_mutex_lock(&dv_lock);
    for (int p = 0; p < 64; p++, h = (h + 1) & (DV_N - 1)) {
        if (dead && dv_tab[h] <= 1) { dv_tab[h] = k; break; }  // (an empty slot or a tombstone)
        if (dv_tab[h] == k) { if (!dead) dv_tab[h] = 1; break; }  // (1: a tombstone)
    }
    pthread_mutex_unlock(&dv_lock);
}
bool vk_view_is_dead(VkImageView v) {
    if (dv_on <= 0 || !v) return false;
    uint64_t k = (uint64_t)v, h = (k * 0x9E3779B97F4A7C15ull) >> 48;
    bool r = false;
    pthread_mutex_lock(&dv_lock);
    for (int p = 0; p < 64 && dv_tab[h]; p++, h = (h + 1) & (DV_N - 1))
        if (dv_tab[h] == k) { r = true; break; }
    pthread_mutex_unlock(&dv_lock);
    return r;
}

void vk_collect_dead(void) {
    uint64_t done = vk_completed_seq();
    DEAD_LOCK();
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].seq > done) { dead[k++] = dead[i]; continue; }
        if (dead[i].view) dv_set(dead[i].view, true);  // (#176: the check above)
        if (dead[i].view) vk_rp_forget_view(dead[i].view), vkDestroyImageView(g_vk.dev, dead[i].view, NULL);
        else vmaDestroyImage(g_vk.vma, dead[i].img, dead[i].mem);
    }
    ndead = k;
    DEAD_UNLOCK();
    vk_rp_collect(done);
}

static void create_image(NvnTexture *t, TexBackend *b) {
    VkFmt mf = fmt_of(t->format);
    b->scalable = b->rs_group != RS_NONE;
    b->sx = g_rs[b->rs_group][0];
    b->sy = g_rs[b->rs_group][1];
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (b->scalable) {
        tw = (uint32_t)lroundf(tw * b->sx); th = (uint32_t)lroundf(th * b->sy);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t layers = 1, depth = 1;
    switch (t->target) {
    case 0: type = VK_IMAGE_TYPE_1D; th = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D; depth = t->depth ? t->depth : 1; break;
    case 3: type = VK_IMAGE_TYPE_1D; layers = t->height ? t->height : 1; th = 1; break;
    case 4: case 6: layers = t->depth ? t->depth : 1; break;
    case 8: layers = 6; break;
    case 9: layers = t->depth ? t->depth : 6; break;
    default: break;
    }
    uint32_t levels = t->levels ? t->levels : 1;
    {   // never more levels than the size allows (scaled targets, odd guest values)
        uint32_t m = 1, sz = tw > th ? tw : th;
        if (depth > sz) sz = depth;
        while (sz > 1) { sz >>= 1; m++; }
        if (levels > m) levels = m;
    }
    uint32_t samples = t->samples > 1 ? t->samples : 1;
    if (samples > 1) levels = 1;
    VkFormat vf = mf.vf;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    b->astc_bw = b->astc_bh = 0;
#ifdef __ANDROID__
    b->bc_vf = VK_FORMAT_UNDEFINED;
    static int no_bc = -1;  // LARECOMP_NO_BC=1 (a test): as a GPU without BC - the CPU decode on an Adreno
    if (no_bc < 0) no_bc = getenv("LARECOMP_NO_BC") != NULL;
    if (!format_supports(vf, need) || (no_bc && is_bc(vf))) {
#else
    if (!format_supports(vf, need)) {
#endif
        bool astc = vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        const FormatInfo *fi = t->layout.fmt;
#ifdef __ANDROID__
        uint32_t bpp;
        if (is_bc(vf) && fi && (fi->bpb == 8 || fi->bpb == 16) && fi->bw == 4 && fi->bh == 4 &&
            format_supports(bc_target(vf, &bpp), need)) {
            static bool told;
            if (!told) { told = true; LOG("[vk] no BC textures on this GPU: decoded on the CPU at upload\n"); }
            b->bc_vf = vf;
            vf = bc_target(vf, &bpp);
        } else
#endif
        if (astc && fi && fi->bpb == 16) {  // desktop GPUs: decoded on the CPU into an RGBA8 image of the same colour space
            b->astc_bw = fi->bw;
            b->astc_bh = fi->bh;
            b->astc_srgb = t->format >= 0x87;
            vf = b->astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            static int nlog;
            if (nlog++ < 20) LOG("[vk] format %d (nvn %#x) not supported: RGBA8 instead\n", vf, t->format);
            vf = VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
    bool depthfmt = renderer_format_is_depth(t->format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depthfmt) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format_supports(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateFlags flags = 0;
    // Render targets (NVN's COMPRESSIBLE flag) without MUTABLE_FORMAT, but for their sRGB / UNORM twin named in a format
    // list: a Mali driver doesn't compress (AFBC) an image whose views may take any format - each pass's loads and
    // stores move it whole (from p5r-android's notes). BOTW views none in another format (title, world: logged) but the
    // window's sRGB; a view that asks for one gets the image's own (vk_view). On Turnip it made no difference (UBWC,
    // docs/BOTW-PORT.md), and it once broke a compressed image there (exec.c post_image): Mali only.
    // LARECOMP_VK_RT_FORMAT_LIST=1 / 0 (LARECOMP_VK_NO_MUTABLE=1 as 1).
    static int rt_list = -1;
    if (rt_list < 0) {
        const char *e = getenv("LARECOMP_VK_RT_FORMAT_LIST");
        rt_list = e ? atoi(e) != 0 : getenv("LARECOMP_VK_NO_MUTABLE") != NULL || g_vk.props.vendorID == 0x13B5;
        if (rt_list) LOG("[vk] render targets without mutable formats (their sRGB twin in a format list)\n");
    }
    VkFormat twin = vf == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB : vf == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
                  : vf == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_SRGB : vf == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM
                  : vf == VK_FORMAT_A8B8G8R8_UNORM_PACK32 ? VK_FORMAT_A8B8G8R8_SRGB_PACK32
                  : vf == VK_FORMAT_A8B8G8R8_SRGB_PACK32 ? VK_FORMAT_A8B8G8R8_UNORM_PACK32 : VK_FORMAT_UNDEFINED;
    VkFormat listed[2] = {vf, twin};
    VkImageFormatListCreateInfo fl = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2,
                                      .pViewFormats = listed};
    bool fixed = rt_list && !depthfmt && (t->flags & 8) && (!twin || g_vk.api >= VK_API_VERSION_1_2);
    b->fixed_fmt = fixed;
    b->view_alt = fixed ? twin : VK_FORMAT_UNDEFINED;
    if (!depthfmt && (!fixed || twin))
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // format views (NVN texture views, raw sRGB views)
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && tw == th && samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;  // (its slices rendered to through 2D views: vk_view)
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = fixed && twin ? &fl : NULL,
                            .flags = flags, .imageType = type, .format = vf,
                            .extent = {tw, th, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkResult r = vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL);
    if (r != VK_SUCCESS) {
        LOG("[vk] image creation failed (%d): nvn fmt %#x %ux%ux%u target %u\n", r, t->format, t->width, t->height, t->depth, t->target);
        ci.format = vf = VK_FORMAT_R8G8B8A8_UNORM;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, ci.pNext = NULL, b->fixed_fmt = false;
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
        depthfmt = false;
    }
    b->format = vf;
    b->width = tw; b->height = th; b->depth = depth; b->layers = layers; b->levels = levels; b->samples = samples;
    b->aspect = depthfmt ? aspect_of(t->format) : VK_IMAGE_ASPECT_COLOR_BIT;
    b->base_type = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : t->target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || t->target == 4 || t->target == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vk_image_init_layout(vk_upload_cmd(), b->image, b->aspect);
    if (b->scalable && !depthfmt) {  // (made again at a new scale: no stale memory in what frames keep)
        VkClearColorValue zero = {{0}};
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(vk_upload_cmd(), b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
    }
    if (depthfmt) {
        // New depth / stencil images start zeroed (as fresh memory reads on Apple GPUs). The game builds its depth
        // pyramid with depth-only copies, so the stencil of those images was never written: whatever the memory
        // held, different per image and per run, then failed or passed the depth-of-field passes' stencil test in
        // patches (flicker that came and went between runs, every other frame, below 1x internal resolution).
        VkCommandBuffer cb = vk_upload_cmd();
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
    }
    if (depthfmt || (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        // Render-target-capable color images start zeroed too: BOTW draws its UI layers over what the target held
        // without clearing it (on the Switch: the memory's old contents, zeros at first), and a new target's
        // VkImage held whatever memory the driver recycled - colored noise behind the save list (Turnip).
        VkCommandBuffer cb = vk_upload_cmd();
        if (!depthfmt) {
            VkClearColorValue zero = {{0}};
            VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        }
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
    }
}

static void defer(Dead d) {
    d.seq = vk_pending_seq();
    DEAD_LOCK();
    if (ndead == cap_dead) { cap_dead = cap_dead ? cap_dead * 2 : 256; dead = realloc(dead, cap_dead * sizeof *dead); }
    dead[ndead++] = d;
    DEAD_UNLOCK();
}

void vk_defer_image(VkImage img, VmaAllocation mem) { defer((Dead){.img = img, .mem = mem}); }
void vk_defer_view(VkImageView v) { defer((Dead){.view = v}); }

void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv);  // (exec.c: LARECOMP_VK_DEVLOST_TRACE)

static uint32_t level_w(const TexBackend *b, uint32_t lv) { uint32_t v = b->width >> lv; return v ? v : 1; }
static uint32_t level_h(const TexBackend *b, uint32_t lv) { uint32_t v = b->height >> lv; return v ? v : 1; }

#ifdef __ANDROID__
static void bc_shadow_destroy(TexBackend *b);

// wb x hb blocks of a BC texture the GPU lacks (bsize bytes each, rows of blocks) decoded into level lv of its image,
// at layer (3D: slice z); the decoded size. *decode_ns (when given): the decode's time.
static uint64_t bc_upload_blocks(VkCommandBuffer cb, TexBackend *b, const uint8_t *blocks, uint32_t wb, uint32_t hb,
                                 uint32_t bsize, uint32_t lv, uint32_t layer, uint32_t z, uint64_t *decode_ns) {
    uint32_t bpp;
    bc_target(b->bc_vf, &bpp);
    uint64_t out_size = bpp ? (uint64_t)wb * 4 * hb * 4 * bpp : (uint64_t)wb * hb * 16;
    VkSpan s = vk_transient(out_size, 16);
    uint64_t decode_start = decode_ns ? SDL_GetTicksNS() : 0;
    if (bpp) bc_decode(b->bc_vf, blocks, wb, hb, bsize, s.ptr, bpp);
    else bc1_to_astc(blocks, s.ptr, (uint64_t)wb * hb);
    if (decode_ns) *decode_ns = SDL_GetTicksNS() - decode_start;
    uint32_t iw = level_w(b, lv), ih = level_h(b, lv);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = wb * 4, .bufferImageHeight = hb * 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1}, .imageOffset = {0, 0, (int32_t)z},
                            .imageExtent = {iw < wb * 4 ? iw : wb * 4, ih < hb * 4 ? ih : hb * 4, 1}};
    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, lv);
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    return out_size;
}
#endif

static void destroy_image(TexBackend *b) {
#ifdef __ANDROID__
    bc_shadow_destroy(b);
#endif
    views_forget(b);
    if (b->image || b->mem) defer((Dead){.img = b->image, .mem = b->mem});  // (never the same allocation twice)
    b->image = VK_NULL_HANDLE;
    b->mem = VK_NULL_HANDLE;
}

// Live game textures: an in-place reboot (vk_textures_reset) destroys them all.
static NvnTexture **g_live;
static int g_nlive, g_cap_live;
int vk_texture_count(void) {
    static int log_diag = -1;
    if (log_diag < 0) log_diag = getenv("LARECOMP_LOG_TEXDIAG") ? 1 : 0;
    if (log_diag) {
        extern uint64_t g_tex_stat_init, g_tex_stat_reinit, g_tex_stat_reused, g_tex_stat_fin_owner, g_tex_stat_fin_copy, g_tex_stat_reg;
        int fin_held = 0, dup_addr = 0, gpu_own = 0, upl = 0;
        for (int i = 0; i < g_nlive; i++) {
            NvnTexture *t = g_live[i];
            TexBackend *b = t->backend;
            if (t->finalized) fin_held++;
            if (b && b->gpu_owned) gpu_own++;
            if (b && b->uploaded) upl++;
            for (int j = 0; j < i; j++) {
                if (g_live[j]->addr && g_live[j]->addr == t->addr) { dup_addr++; break; }
            }
        }
        LOG("[tex-diag] live_vk=%d (fin_held=%d dup_addr=%d gpu_own=%d upl=%d) init=%llu reused=%llu reinit=%llu fin_own=%llu fin_copy=%llu reg=%llu\n",
            g_nlive, fin_held, dup_addr, gpu_own, upl,
            (unsigned long long)g_tex_stat_init, (unsigned long long)g_tex_stat_reused, (unsigned long long)g_tex_stat_reinit,
            (unsigned long long)g_tex_stat_fin_owner, (unsigned long long)g_tex_stat_fin_copy,
            (unsigned long long)g_tex_stat_reg);
        extern uint64_t g_tex_drain_ns, g_tex_drains;
        LOG("[tex-diag] slot replacements waited for the render thread: %llu, %.1f ms in all\n",
            (unsigned long long)__atomic_exchange_n(&g_tex_drains, 0, __ATOMIC_RELAXED),
            __atomic_exchange_n(&g_tex_drain_ns, 0, __ATOMIC_RELAXED) / 1e6);
        for (int i = g_nlive >= 3 ? g_nlive - 3 : 0; i < g_nlive; i++) {
            NvnTexture *t = g_live[i];
            LOG("[tex-sample #%d] addr=%llx %ux%ux%u fmt=%#x tgt=%u flags=%#x refs=%u fin=%d label=%s\n",
                i, (unsigned long long)t->addr, t->width, t->height, t->depth, t->format, t->target, t->flags, t->refs, t->finalized, t->label);
        }
    }
    return g_nlive;
}
// (diagnosis, exec.c vk_dump_watch_frame) the live texture over guest address addr with the most levels x layers
NvnTexture *vk_texture_at(uint64_t addr) {
    NvnTexture *best = NULL;
    for (int i = 0; i < g_nlive; i++) {
        NvnTexture *t = g_live[i];
        if (t->addr != addr || !t->backend) continue;
        if (!best || t->levels * t->layout.layers > best->levels * best->layout.layers) best = t;
    }
    return best;
}

static void live_add(NvnTexture *t, TexBackend *b) {
    if (g_nlive == g_cap_live) { g_cap_live = g_cap_live ? g_cap_live * 2 : 1024; g_live = realloc(g_live, g_cap_live * sizeof *g_live); }
    g_live[g_nlive++] = t;
    b->reg = g_nlive;
}
static void live_remove(TexBackend *b) {
    if (!b->reg) return;
    NvnTexture *last = g_live[--g_nlive];
    g_live[b->reg - 1] = last;
    if (last->backend) ((TexBackend *)last->backend)->reg = b->reg;
    b->reg = 0;
}

static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        if (rs_stale(b)) {  // output resolution changed: reallocate
            destroy_image(b);
            create_image(t, b);
        }
        return b;
    }
    vk_collect_dead();
    b = calloc(1, sizeof *b);
    b->rs_group = tex_scale_group(t);
    create_image(t, b);
    t->backend = b;
    live_add(t, b);
    return b;
}

static void texture_snapshot_forget(TexBackend *b);

void backend_texture_destroyed(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (!b) return;
#ifdef __ANDROID__
    extern void vk_async_drain(void);
    vk_async_drain();  // (the render thread idle: vulkan/async.c)
#endif
    extern void vk_alias_texture_destroyed(NvnTexture *t);  // (exec.c copy aliases: a source's contents handed over)
    vk_alias_texture_destroyed(t);
    live_remove(b);
    destroy_image(b);
    texture_snapshot_forget(b);
    free(b->pending_rgba);
    free(b);
    t->backend = NULL;
}

// Reboot (device.c backend_reset; GPU idle): every game texture and its views go.
void vk_textures_reset(void) {
    int n = g_nlive;
    extern void vk_alias_reset(void);
    vk_alias_reset();
    while (g_nlive > 0) backend_texture_destroyed(g_live[g_nlive - 1]);
    vk_collect_dead();
    LOG("[vk] reboot: %d textures released\n", n);
}

// ================================================================== uploads (same change detection as metal.m)
static uint64_t full_hash_budget, full_hash_budget_frame;

// Four independent lanes (one serial multiply chain ran at ~1.5 bytes/cycle; dynamic textures are hashed every
// frame). Values are only compared within a run.
static uint64_t hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

// (1024 bytes spread over the texture: without page write tracking (Android) every check samples, and 4096 a texture -
// cache misses mostly - were ~10% of the render thread in the overworld; the periodic full hash stays the safety net)
static uint64_t fnv_sample(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    uint64_t step = n > 1024 ? n / 1024 : 1;
    for (uint64_t i = 0; i < n; i += step) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

// Guest texels for a scaled image (the CPU filled a render target: the title screen's save pictures): they go into a
// guest-size image of their own, then a filtered blit brings them to the allocation's size (SavageWindRecomp's).
static void upload_scaled(VkCommandBuffer cb, TexBackend *b, VkSpan s, VkBufferImageCopy rg, uint32_t gw, uint32_t gh) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                            .extent = {gw, gh, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img;
    VmaAllocation mem;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS) return;
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    uint32_t lv = rg.imageSubresource.mipLevel, layer = rg.imageSubresource.baseArrayLayer;
    rg.imageSubresource.mipLevel = 0;
    rg.imageSubresource.baseArrayLayer = 0;
    rg.imageExtent = (VkExtent3D){gw, gh, 1};
    vkCmdCopyBufferToImage(cb, s.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    vk_full_barrier(cb);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)gw, (int32_t)gh, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                      .dstOffsets = {{0, 0, 0}, {(int32_t)level_w(b, lv), (int32_t)level_h(b, lv), 1}}};
    bool lin = format_supports(b->format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    vkCmdBlitImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    vk_full_barrier(cb);
    vk_defer_image(img, mem);
}

static void upload_pending(TexBackend *b) {
    pthread_mutex_lock(&pending_lock);
    void *px = b->pending_rgba;
    uint32_t w = b->pending_w, h = b->pending_h, stride = b->pending_stride;
    b->pending_rgba = NULL;
    pthread_mutex_unlock(&pending_lock);
    if (!px) return;
    bool scaled = b->scalable && (w != b->width || h != b->height);
    if (!scaled && w > b->width) w = b->width;
    if (!scaled && h > b->height) h = b->height;
    VkSpan s = vk_transient_copy(px, (uint64_t)stride * h, 16);
    free(px);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = stride / 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_cmd();
    if (scaled) { upload_scaled(cb, b, s, rg, w, h); return; }
    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

// Upload conversion runs on the render thread. Keep one scratch allocation instead of one per mip/layer.
static uint8_t *upload_scratch;
static size_t upload_scratch_size;
static uint8_t *texture_scratch(size_t size) {
    if (size > upload_scratch_size) {
        uint8_t *next = realloc(upload_scratch, size);
        if (!next) abort();
        upload_scratch = next;
        upload_scratch_size = size;
    }
    return upload_scratch;
}

// Exact comparisons replace repeated hashing for stable dynamic textures without relying on page tracking.
// At most eight 1 MB snapshots. The hash tag prevents reuse after an upload bypassed this cache.
typedef struct { TexBackend *owner; uint64_t hash, size; uint8_t *bytes; } TextureSnapshot;
static TextureSnapshot texture_snapshots[8];
static void texture_snapshot_forget(TexBackend *b) {
    for (int i = 0; i < 8; i++) if (texture_snapshots[i].owner == b) texture_snapshots[i].owner = NULL;
}
static bool texture_unchanged(TexBackend *b, const uint8_t *mem, uint64_t size) {
    static int enabled = -1;
    if (enabled < 0) { const char *e = getenv("LARECOMP_TEX_SNAPSHOTS"); enabled = !e || atoi(e) != 0; }
    if (!enabled || !size || size > (1u << 20)) return false;
    TextureSnapshot *e = &texture_snapshots[((uintptr_t)b >> 6) & 7];
    if (e->owner == b && e->hash == b->data_hash && e->size == size && !memcmp(e->bytes, mem, size)) return true;
    if (e->size != size) {
        uint8_t *next = realloc(e->bytes, size ? size : 1);
        if (!next) return false;
        e->bytes = next;
    }
    memcpy(e->bytes, mem, size);
    e->owner = b; e->hash = b->data_hash; e->size = size;
    return false;
}

uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn, prof_tex_watch_skips;
// Opt-in CPU measurements only: no GPU queries, flushes or waits. Called on the texture upload path.
static bool texture_trace_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_TEXTURE_TRACE"); on = e && atoi(e) != 0; }
    return on;
}
enum { TEX_TRACE_NATIVE, TEX_TRACE_BC, TEX_TRACE_ASTC };
static void texture_trace_record(int kind, uint64_t source, uint64_t upload, uint64_t decode_ns) {
    if (!texture_trace_on()) return;
    static uint64_t since, levels[3], source_bytes[3], upload_bytes[3], decode[3], max_decode[3];
    uint64_t now = SDL_GetTicksNS();
    if (!since) since = now;
    levels[kind]++; source_bytes[kind] += source; upload_bytes[kind] += upload; decode[kind] += decode_ns;
    if (decode_ns > max_decode[kind]) max_decode[kind] = decode_ns;
    if (now - since < 2000000000ull) return;
    static const char *const names[] = {"native", "bc-cpu", "astc-cpu"};
    for (int i = 0; i < 3; i++) {
        LOG("[texture-trace] window %.2fs frame %llu kind %s levels %llu source %.2fMiB staging %.2fMiB cpu-decode %.3fms max-level %.3fms\n",
            (now - since) / 1e9, (unsigned long long)vk_frame_counter(), names[i], (unsigned long long)levels[i],
            source_bytes[i] / 1048576.0, upload_bytes[i] / 1048576.0, decode[i] / 1e6, max_decode[i] / 1e6);
        levels[i] = source_bytes[i] = upload_bytes[i] = decode[i] = max_decode[i] = 0;
    }
    since = now;
}
static void upload_texture(NvnTexture *t, TexBackend *b) {
    if (!t->addr || t->target == 10) return;
    uint64_t frame = vk_frame_counter();
    if (b->uploaded && b->checked_frame == frame) return;
    // A texture that never changed since its upload is checked every 8th frame - but a linear one (flag 0x40 / 0x10)
    // every frame: BOTW fills its terrain height tiles (256x256 R16, 64x64, linear) on the CPU and draws them into the
    // terrain's arrays (512x512x8 RGBA8) right away, once. Read up to 7 frames late, an array layer was built from the
    // tile's old contents and kept them: the stretched "curtains" after a fast travel or a tower, until the save was
    // loaded again (2026-11-01, POCO, 1.9.0: in two sessions of fast travels none with every frame, and the first ones
    // back at once with every 8th; a bisection over the GPU-drawn textures had pointed at that array). There are few
    // linear textures, and page tracking makes most checks free. LARECOMP_LIN_STAGGER=1: every 8th frame, as before.
    static int lin_every = -1;
    if (lin_every < 0) lin_every = !getenv("LARECOMP_LIN_STAGGER");
    if (b->uploaded && !b->dynamic && !(lin_every && (t->flags & 0x50)) && ((frame + ((uintptr_t)b >> 6)) & 7)) return;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb) return;
    VkFmt mf = fmt_of(t->format);
    uint64_t total = tegra_total_size(&t->layout);
    b->checked_frame = frame;
    prof_tex_checks++;
    if (t->flags & 0x10) b->dynamic = true;
    if (full_hash_budget_frame != frame) { full_hash_budget_frame = frame; full_hash_budget = 4 << 20; }
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    static int always_full = -1;
    if (always_full < 0) always_full = getenv("LARECOMP_TEX_FULLHASH") ? 1 : 0;
    bool full = b->dynamic || !b->uploaded || always_full;
    // (safety net: a full hash now and then - one larger than the frame's budget too, as the frame's first: a texture
    // over 4 MB never fit it, and a change its samples missed stayed unseen - TOTKRecompAndroid's 26 MB terrain atlas, 20 s)
    bool periodic = frame - b->full_frame >= 120 && (full_hash_budget >= total || full_hash_budget == (4u << 20));
    if (!full && periodic) { full = true; full_hash_budget = full_hash_budget > total ? full_hash_budget - total : 0; }
    if (!b->tp) b->tp = texpatch_identify(t) + 1;  // (button prompts / Hero Mode icon: re-uploaded when their look changes)
    bool restyle = b->tp > 0 && b->uploaded && b->tp_gen != texpatch_generation();
    // The OS's page write tracking (exec.c vk_pages_changed, shared with the vertex cache): texture memory the CPU
    // hasn't written since the last check is not hashed at all (dynamic textures were fully hashed every frame,
    // ~2.4 MB a frame in the overworld). The periodic full hash stays as a safety net. LARECOMP_VK_TEX_WATCH=0: off.
    static int watch = -1;
    if (watch < 0) { const char *e = getenv("LARECOMP_VK_TEX_WATCH"); watch = !e || atoi(e) != 0; }
    if (watch && !always_full) {
        extern int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now);
        uint32_t now = 0;
        int ch = vk_pages_changed(t->addr, total, b->wgen, &now);
        if (ch >= 0) {
            b->wgen = now;
            if (!ch && b->uploaded && !restyle) {
                if (!periodic) { prof_tex_watch_skips++; return; }
                if (b->dynamic) full_hash_budget = full_hash_budget > total ? full_hash_budget - total : 0;  // (the others took theirs above)
            }
        }
    }
    if (b->uploaded && b->dynamic && !always_full && !restyle && !periodic && texture_unchanged(b, mem, total)) return;
    if (!full && !restyle) {
        prof_tex_samples++;
        uint64_t sh = fnv_sample(mem, total);
        if (sh == b->sample_hash) return;
    }
    prof_tex_full++; prof_tex_full_bytes += total; if (b->dynamic) prof_tex_dyn++;
    uint64_t h = hash_full(mem, total), last_full = b->full_frame, old_sample = b->sample_hash;
    b->full_frame = frame;
    b->sample_hash = fnv_sample(mem, total);
    if (b->uploaded && h == b->data_hash && !restyle) return;
    if (b->uploaded && h != b->data_hash && !b->dynamic && !always_full) {
        // LARECOMP_LOG_TEX_MISS=1 (diagnosis): a change the sampled check didn't see, found by the periodic full hash -
        // the texture showed its old contents meanwhile (the stretched-terrain curtains after a tower: gone with
        // LARECOMP_TEX_FULLHASH=1, 2026-10-26)
        static int on = -1, n;
        if (on < 0) on = getenv("LARECOMP_LOG_TEX_MISS") != NULL;
        if (on && n++ < 200)
            LOG("[texmiss] frame %llu: %ux%ux%u fmt %#x flags %#x target %u levels %u at %llx+%llx changed, found %llu frames "
                "after the last full hash; the samples %s\n", (unsigned long long)frame, t->width, t->height, t->depth,
                t->format, t->flags, t->target, t->levels, (unsigned long long)t->addr, (unsigned long long)total,
                (unsigned long long)(frame - last_full), b->sample_hash == old_sample ? "unchanged (missed)" : "changed");
    }
    if (b->uploaded && h != b->data_hash) b->dynamic = true;
    if (b->tp > 0) b->tp_gen = texpatch_generation();
    {   // LARECOMP_LOG_STALE_UPLOAD=1 (diagnosis): an upload from guest memory that a render target the GPU drew into
        // covers - its object's own image, where the drawing never reaches memory: the texture reads what was there
        // before (MacNativeGames PR #511: BOTW's terrain height map, drawn through one object and sampled through
        // another, kept zeros - Link's feet under the ground). Each pair once, 60 at most.
        static int on = -1, n;
        if (on < 0) on = getenv("LARECOMP_LOG_STALE_UPLOAD") != NULL;
        for (int i = 0; on && n < 60 && i < g_nlive; i++) {
            NvnTexture *e = g_live[i];
            if (e == t || !e->addr || !e->backend || !((TexBackend *)e->backend)->gpu_owned) continue;
            uint64_t ee = e->addr + tegra_total_size(&e->layout);
            if (e->addr >= t->addr + total || ee <= t->addr) continue;
            n++;
            LOG("[stale] frame %llu: %ux%u fmt %#x flags %#x at %llx+%llx uploaded from memory under the drawn target "
                "%ux%u fmt %#x flags %#x at %llx+%llx\n", (unsigned long long)frame, t->width, t->height, t->format, t->flags,
                (unsigned long long)t->addr, (unsigned long long)total, e->width, e->height, e->format, e->flags,
                (unsigned long long)e->addr, (unsigned long long)(ee - e->addr));
        }
    }
    b->data_hash = h;
    b->uploaded = true;
#ifdef __ANDROID__
    if (b->format != mf.vf && !b->astc_bw && !b->bc_vf) return;  // unsupported format: nothing sensible to copy
#else
    if (b->format != mf.vf && !b->astc_bw) return;  // unsupported format: nothing sensible to copy
#endif
    VkCommandBuffer cb = vk_upload_cmd();
    uint32_t layers = t->layout.layers ? t->layout.layers : 1;
    if (layers > b->layers && b->depth == 1) layers = b->layers;
    uint64_t layer_size = tegra_layer_size(&t->layout);
    // Linear textures: flag 0x10, rows t->stride apart; flag 0x40 (BOTW's terrain height tiles, 256x256 R16 filled by the
    // CPU) too, rows packed at their width - their stride field doesn't fit their storage. Read as block-linear, those
    // tiles gave the terrain's heights in stripes: walls stretched into vertical "curtains", spikes in the shrine.
    bool linear = (t->flags & 0x50) != 0;
#define LIN_STRIDE(row_bytes) ((t->flags & 0x10) ? t->stride : (uint64_t)(row_bytes))
    uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            if (b->astc_bw) {  // blocks -> linear block rows -> RGBA8 texels
                const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
                uint8_t *blocks = texture_scratch((uint64_t)l.wb * l.hb * 16);
                if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * 16, src + (uint64_t)y * LIN_STRIDE(l.wb * 16), l.wb * 16);
                else tegra_deswizzle(src, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
                if (b->tp > 0 && layer == 0) texpatch_apply(b->tp - 1, lv, blocks, l.w, l.h, l.wb * 16);
                VkSpan s = vk_transient((uint64_t)l.w * l.h * 4, 16);
                uint64_t decode_start = texture_trace_on() ? SDL_GetTicksNS() : 0;
                astc_decode_ldr(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                uint64_t decode_ns = decode_start ? SDL_GetTicksNS() - decode_start : 0;
                VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                        .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv), l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
                vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                texture_trace_record(TEX_TRACE_ASTC, (uint64_t)l.wb * l.hb * 16, (uint64_t)l.w * l.h * 4, decode_ns);
                continue;
            }
#ifdef __ANDROID__
            if (b->bc_vf) {  // blocks -> linear block rows -> texels (bc_upload_blocks)
                uint32_t bsize = fi->bpb;
                const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
                uint8_t *blocks = texture_scratch((uint64_t)l.wb * l.hb * bsize);
                if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * bsize, src + (uint64_t)y * LIN_STRIDE(l.wb * bsize), l.wb * bsize);
                else tegra_deswizzle(src, blocks, l.wb, l.hb, 1, bsize, l.block_height_log2, l.block_depth_log2);
                if (b->tp > 0 && layer == 0) texpatch_apply(b->tp - 1, lv, blocks, l.w, l.h, l.wb * bsize);
                uint64_t decode_ns = 0;
                uint64_t out_size = bc_upload_blocks(cb, b, blocks, l.wb, l.hb, bsize, lv, layer, 0, texture_trace_on() ? &decode_ns : NULL);
                texture_trace_record(TEX_TRACE_BC, (uint64_t)l.wb * l.hb * bsize, out_size, decode_ns);
                continue;
            }
#endif
            uint32_t row = l.wb * fi->bpb;
            uint64_t sz = (uint64_t)row * l.hb * l.d;
            bool expand = mf.expand_rgb && fi->bpb == 3;
            uint32_t out_row = expand ? l.wb * 4 : row;
            VkSpan s = vk_transient((uint64_t)out_row * l.hb * l.d, 16);
            uint8_t *lin = expand ? texture_scratch(sz) : s.ptr;
            const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
            if (linear) {
                for (uint32_t y = 0; y < l.hb; y++) memcpy(lin + (uint64_t)y * row, src + (uint64_t)y * LIN_STRIDE(row), row);
            } else {
                tegra_deswizzle(src, lin, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2);
            }
            {   // LARECOMP_DUMP_TEX=<w>x<h>x<d>,<path prefix>: level 0 of each upload of that shape, guest bytes (.raw)
                // and deswizzled (.lin), the first 8 (diagnosis)
                static int dw = -1, dh, dd, ndump;
                static char dpre[256];
                if (dw < 0) {
                    dw = 0;
                    const char *e = getenv("LARECOMP_DUMP_TEX");
                    if (e && sscanf(e, "%dx%dx%d,%255s", &dw, &dh, &dd, dpre) != 4) dw = 0;
                }
                if (dw && lv == 0 && layer == 0 && (int)t->width == dw && (int)t->height == dh && (int)t->depth == dd &&
                    ndump < 8) {
                    char p[300];
                    snprintf(p, sizeof p, "%s%d.lin", dpre, ndump);
                    FILE *f = fopen(p, "wb");
                    if (f) { fwrite(lin, 1, sz, f); fclose(f); }
                    snprintf(p, sizeof p, "%s%d.raw", dpre, ndump);
                    f = fopen(p, "wb");
                    if (f) { fwrite(src, 1, tegra_level(&t->layout, 0).offset + tegra_layer_size(&t->layout), f); fclose(f); }
                    LOG("[tex] dump %d: %ux%ux%u fmt %#x bh %u bd %u linear %d at %llx -> %s\n", ndump, t->width, t->height,
                        t->depth, t->format, l.block_height_log2, l.block_depth_log2, linear, (unsigned long long)t->addr, p);
                    ndump++;
                }
            }
            if (b->tp > 0 && layer == 0) texpatch_apply(b->tp - 1, lv, lin, l.w, l.h, row);
            if (expand) {  // RGB8 -> RGBA8
                uint8_t *o = s.ptr;
                uint64_t n = (uint64_t)l.wb * l.hb * l.d;
                for (uint64_t i = 0; i < n; i++) { o[i * 4] = lin[i * 3]; o[i * 4 + 1] = lin[i * 3 + 1]; o[i * 4 + 2] = lin[i * 3 + 2]; o[i * 4 + 3] = 255; }
            }
            uint32_t w = l.w, hh = l.h, d = l.d;
            // the copy stays inside the image's level: a texture allocated at the internal resolution (16:9, render
            // target sized) can still be filled from the CPU - the save slots' screenshots on the title screen. Its
            // full-size data overran the scaled image (only the width was clamped): GPU writes past the allocation,
            // a device loss below 1x after a seed switch.
            if (w > level_w(b, lv)) w = level_w(b, lv);
            if (hh > level_h(b, lv)) hh = level_h(b, lv);
            VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.wb * fi->bw, .bufferImageHeight = l.hb * fi->bh,
                                    .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect, lv,
                                                         b->depth > 1 ? 0 : layer, 1},
                                    .imageExtent = {w, b->base_type == VK_IMAGE_VIEW_TYPE_1D || b->base_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 1 : hh,
                                                    b->depth > 1 ? d : 1}};
            if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) continue;  // packed D/S: no CPU layout
            if (b->scalable && b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && b->depth == 1 &&
                (l.w != level_w(b, lv) || l.h != level_h(b, lv))) {  // (guest-size texels into a scaled allocation)
                upload_scaled(cb, b, s, rg, l.w, l.h);
                continue;
            }
            vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            texture_trace_record(TEX_TRACE_NATIVE, sz, (uint64_t)out_row * l.hb * l.d, 0);
        }
    }
}

TexBackend *vk_tex(NvnTexture *t, bool for_render) {
    TexBackend *b = tex_backend(t);
    if (b->pending_rgba) upload_pending(b);
    if (for_render && !b->gpu_owned && t->addr) {  // (LARECOMP_LOG_STALE_UPLOAD: textures uploaded before, over it)
        static int on = -1, n;
        if (on < 0) on = getenv("LARECOMP_LOG_STALE_UPLOAD") != NULL;
        uint64_t te = t->addr + tegra_total_size(&t->layout);
        for (int i = 0; on && n < 60 && i < g_nlive; i++) {
            NvnTexture *e = g_live[i];
            TexBackend *eb = e->backend;
            if (e == t || !e->addr || !eb || eb->gpu_owned || !eb->uploaded) continue;
            uint64_t ee = e->addr + tegra_total_size(&e->layout);
            if (e->addr >= te || ee <= t->addr) continue;
            n++;
            LOG("[stale] frame %llu: target %ux%u fmt %#x flags %#x at %llx+%llx first drawn over %ux%u fmt %#x flags %#x "
                "at %llx+%llx, uploaded from memory\n", (unsigned long long)vk_frame_counter(), t->width, t->height, t->format,
                t->flags, (unsigned long long)t->addr, (unsigned long long)(te - t->addr), e->width, e->height, e->format,
                e->flags, (unsigned long long)e->addr, (unsigned long long)(ee - e->addr));
        }
    }
    if (for_render) b->gpu_owned = true;
    else if (!b->gpu_owned) {
        extern uint64_t prof_up_ns;
        uint64_t t0 = prof_up_ns != ~0ull ? SDL_GetTicksNS() : 0;
        upload_texture(t, b);
        if (prof_up_ns != ~0ull) {
            extern uint64_t prof_up_cum;  // (exec.c: per-frame in LARECOMP_LOG_PACE)
            uint64_t d = SDL_GetTicksNS() - t0;
            prof_up_ns += d, prof_up_cum += d;
        }
    }
    return b;
}

void renderer_mark_gpu_owned(NvnTexture *t) { tex_backend(t)->gpu_owned = true; }
bool renderer_is_gpu_owned(NvnTexture *t) { return tex_backend(t)->gpu_owned; }

bool renderer_texture_scalable(NvnTexture *t) { return tex_backend(t)->scalable; }
uint8_t vk_texture_scale_group(NvnTexture *t) { return tex_backend(t)->rs_group; }

// Render at the scale of `like` from now on (another attachment of the same pass): a target drawn into together with
// scaled ones (its contents so far, kept at the guest size, are dropped: the pass about to run writes it)
void vk_make_scalable_like(NvnTexture *t, NvnTexture *like) {
    TexBackend *b = tex_backend(t);
    uint8_t g = tex_backend(like)->rs_group;
    if (b->rs_group || !g || (t->target != 1 && t->target != 4) || t->samples > 1) return;  // (a group stays)
    b->rs_group = g;
    destroy_image(b);
    create_image(t, b);
}
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// Host-produced texel data (decoded video, any thread): applied on the render thread at the texture's next use.
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
#ifdef __ANDROID__
    extern void vk_async_drain(void);
    vk_async_drain();  // (the render thread idle: vulkan/async.c)
#endif
    TexBackend *b = t->backend;
    if (!b) return;  // the game binds the texture before decoding into it; first frames may arrive earlier
    void *copy = malloc((size_t)stride * h);
    memcpy(copy, rgba, (size_t)stride * h);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->gpu_owned = true;
    pthread_mutex_unlock(&pending_lock);
}

// ================================================================== views
#define VIEW_TAB 16384
typedef struct { TexBackend *b; uint64_t k1, k2; VkImageView view; } ViewEnt;
static ViewEnt view_tab[VIEW_TAB];

// Views may still be referenced by submitted or pending work: destroyed once that has completed.
void vk_views_forget(TexBackend *b) { views_forget(b); }
uint32_t vk_view_gen;  // bumped whenever a cached view may be destroyed (exec.c's per-frame texture memo)
static void views_forget(TexBackend *b) {
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
}

VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) return e->view;
        if (!e->b) break;
    }
    extern uint64_t prof_views_created;
    prof_views_created++;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    VkImageView v;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    dv_set(v, false);  // (#176: a handle the driver hands out again is alive)
    ViewEnt *e = &view_tab[slot];
    if (e->b) { defer((Dead){.view = e->view}); vk_view_gen++; }  // evict (16 probes taken)
    *e = (ViewEnt){b, k1, k2, v};
    return v;
}

static VkImageViewType view_type_for(int textype) {
    switch (textype) {
    case TEXTYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TEXTYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TEXTYPE_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TEXTYPE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TEXTYPE_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case TEXTYPE_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static const VkComponentMapping IDENTITY = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

// Draws into 3D textures (vk_view): LARECOMP_3D_RT=0 none / 1 level 0 only / 2 every level (default: 2, 0 on
// Qualcomm's driver, which lost the device on them)
static int rt3d_mode(void) {
    static int rt3d = -1;
    if (rt3d < 0) {
        extern bool vk_driver_is_qualcomm(void);
        const char *e = getenv("LARECOMP_3D_RT");
        rt3d = e ? atoi(e) : vk_driver_is_qualcomm() ? 0 : 2;
    }
    return rt3d;
}

// render: attachment view (one level, one layer). Otherwise a sampled view of type want_textype (TEXTYPE_*),
// with the NVN component swizzle, converting between compatible types the way renderer.m does.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype) {
    TexBackend *b = vk_tex(t, render);
    if (!has_view) v = NULL;
    VkFormat fmt = b->format;
    if (v && (v->setmask & 4) && !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == b->format) fmt = vf;  // (fallback images keep their own format)
#ifdef __ANDROID__
        else if (b->bc_vf && is_bc(vf)) { uint32_t bpp; fmt = bc_target(vf, &bpp); }  // (decoded BC: its sRGB / UNORM view)
#endif
        if (b->fixed_fmt && fmt != b->format && fmt != b->view_alt) {  // (not in its format list: create_image)
            static int nlog;
            if (nlog++ < 8) LOG("[vk] a view of render target %ux%u (nvn %#x) as nvn %#x: in its own format\n", t->width, t->height,
                                t->format, v->format);
            fmt = b->format;
        }
    }
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : b->levels - (lv0 < b->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    if (lv0 + nlv > b->levels) nlv = b->levels - lv0;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    if (render) {
        if (b->base_type == VK_IMAGE_VIEW_TYPE_3D) {
            // A 3D target: one slice (the view's layer is its depth index at that level) as a 2D view - the image is
            // made 2D_ARRAY_COMPATIBLE. BOTW draws its colour LUT so (8x8x8, the 8 slices as the 8 targets of one draw:
            // the pause menu's background and its panels go through it - empty, they were black) and the mips of a
            // 32x32x32 volume, slice by slice.
            // Qualcomm's own driver lost the device on these draws (at the title screen, 2026-10-25: bisected to
            // bde9d0e): there, as before it, no view - those draws are skipped and the menu's background stays black.
            int rt3d = rt3d_mode();
            static int n3d;  // (which games draw so: TOTK's first frames on Qualcomm's driver lost the device too)
            if (n3d++ < 4) LOG("[vk] 3D target %ux%ux%u level %u: %s\n", t->width, t->height, b->depth, lv0,
                               rt3d == 0 || (rt3d == 1 && lv0 > 0) ? "skipped (LARECOMP_3D_RT)" : "a slice as 2D");
            if (rt3d == 0 || (rt3d == 1 && lv0 > 0)) return VK_NULL_HANDLE;
            uint32_t d = b->depth >> lv0, z = v && (v->setmask & 2) ? v->min_layer : 0;
            if (!d) d = 1;
            if (z >= d) z = d - 1;
            return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, fmt, lv0, 1, z, 1, IDENTITY, b->aspect);
        }
        return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, fmt, lv0, 1, ly0, 1, IDENTITY, b->aspect);
    }
    VkImageAspectFlags aspect = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    {   // sampled formats the GPU can't filter (Maxwell filters them all): a linear sampler on them is undefined in Vulkan
        static uint8_t told[256];
        VkFormatProperties fp;
        if (fmt < 256 && !told[fmt]) {
            vkGetPhysicalDeviceFormatProperties(g_vk.phys, fmt, &fp);
            told[fmt] = 1;
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
                LOG("[vk] sampled format %d (nvn %#x, %ux%u) has no linear filtering here\n", fmt, t->format, t->width, t->height);
        }
    }
    VkComponentMapping sw = IDENTITY;
    if (!(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && b->samples == 1) {
        const uint8_t *s = v && (v->setmask & 8) ? v->swizzle : t->swizzle;
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
        if (c[0] || c[1] || c[2] || c[3]) {  // never set: identity
            if (fmt_of(t->format).alpha_one)
                for (int i = 0; i < 4; i++) if (c[i] == 5) c[i] = 1;
            // NVN swizzle 0 zero, 1 one, 2..5 r,g,b,a == VkComponentSwizzle - 1
            sw = (VkComponentMapping){(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1), (VkComponentSwizzle)(c[2] + 1),
                                      (VkComponentSwizzle)(c[3] + 1)};
        } else if (fmt_of(t->format).alpha_one) {
            sw.a = VK_COMPONENT_SWIZZLE_ONE;
        }
    }
    VkImageViewType want = view_type_for(want_textype);
    VkImageViewType have = b->base_type;
    if (b->samples > 1) return vk_dummy_view(want_textype);  // multisampled images can't be sampled as 2D
    if (want == have) {
        if (have == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
        else if (have == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) nly = nly >= 6 ? nly - (nly % 6) : 6;
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_2D &&
        (have == VK_IMAGE_VIEW_TYPE_2D_ARRAY || have == VK_IMAGE_VIEW_TYPE_CUBE || have == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY &&
        (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE || have == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, have == VK_IMAGE_VIEW_TYPE_2D ? 1 : nly, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_CUBE && (have == VK_IMAGE_VIEW_TYPE_2D_ARRAY || have == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
        b->layers >= 6 && b->width == b->height)
        return vk_image_view(b, want, fmt, lv0, nlv, ly0 + 6 <= b->layers ? ly0 : 0, 6, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && (have == VK_IMAGE_VIEW_TYPE_CUBE || have == VK_IMAGE_VIEW_TYPE_2D_ARRAY) &&
        b->layers >= 6 && b->width == b->height) {
        uint32_t cly = nly >= 6 ? nly - (nly % 6) : 6;
        return vk_image_view(b, want, fmt, lv0, nlv, ly0 + cly <= b->layers ? ly0 : 0, cly, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_1D_ARRAY && have == VK_IMAGE_VIEW_TYPE_1D)
        return vk_image_view(b, want, fmt, lv0, nlv, 0, 1, sw, aspect);
    static int nlog;
    if (nlog++ < 30) LOG("[tex] type mismatch: want %d have %d (%ux%ux%u target %u fmt %#x layers %u)\n", want, have, t->width,
                         t->height, t->depth, t->target, t->format, b->layers);
    return vk_dummy_view(want_textype);
}

#ifdef __ANDROID__
// ---- BC textures the GPU compresses into, on a GPU without BC. BOTW makes its 32x32x32 BC4 noise volume so, once
// after loading: each slice and mip drawn through a view in a format of the block's size (8 bytes: a texel a block).
// Its image here holds the decoded texels (R8), and those block texels went into it as colours - the volume read
// zero, and the fast-travel dissolve lost its noise: a flat dark-blue Link that vanished all at once, never breaking
// up (Mali-G925, 2026-11-03). Such draws go to a shadow image in the view's format at the block grid instead; before
// the texture is next read (a pass that doesn't draw into it, a dispatch, a copy, the present) the blocks drawn come
// back to the CPU (one wait on the GPU each time) and are decoded into the image.
typedef struct BcShadow {
    TexBackend img;      // level lv holds bc_blocks() of the texture's level lv (wider when that isn't a power of two)
    uint32_t bsize;      // bytes a block
    uint32_t first[16];  // level lv's slices (3D) / layers: drawn[first[lv] + z]
    uint8_t *drawn;      // drawn into since the last decode - only those decoded (the rest of the shadow is undefined)
    bool dirty;
} BcShadow;
static TexBackend *bc_dirty[16];  // (the textures with a dirty shadow: a pass's targets at most)
static int nbc_dirty;

static uint32_t bc_blocks(uint32_t v, uint32_t lv) { v >>= lv; return ((v ? v : 1) + 3) / 4; }  // (a level's extent in blocks)
static uint32_t level_slices(const TexBackend *b, uint32_t lv) {
    if (b->base_type != VK_IMAGE_VIEW_TYPE_3D) return b->layers;
    uint32_t v = b->depth >> lv;
    return v ? v : 1;
}

static void bc_shadow_destroy(TexBackend *b) {
    BcShadow *s = b->bc_shadow;
    for (int i = 0; i < nbc_dirty; i++)
        if (bc_dirty[i] == b) bc_dirty[i--] = bc_dirty[--nbc_dirty];
    if (!s) return;
    views_forget(&s->img);
    defer((Dead){.img = s->img.image, .mem = s->img.mem});
    free(s->drawn);
    free(s);
    b->bc_shadow = NULL;
}

// A new shadow holds the texture's blocks before anything is drawn into it: a draw may cover only part of a slice,
// and the whole slice is decoded. From guest memory (the CPU's blocks; tegra layout as upload_texture reads it), or
// from the shadow in another format it replaces (`from`: the blocks drawn since, same size).
static void bc_shadow_fill(NvnTexture *t, TexBackend *b, BcShadow *s, const BcShadow *from) {
    bool is3d = b->base_type == VK_IMAGE_VIEW_TYPE_3D;
    uint32_t nlv = s->img.levels;
    if (from) {
        VkImageCopy c[16];
        for (uint32_t lv = 0; lv < nlv; lv++)
            c[lv] = (VkImageCopy){.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, b->layers},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, b->layers},
                                  .extent = {bc_blocks(b->width, lv), bc_blocks(b->height, lv), is3d ? level_slices(b, lv) : 1}};
        VkCommandBuffer cb = vk_cmd();
        vkCmdCopyImage(cb, from->img.image, VK_IMAGE_LAYOUT_GENERAL, s->img.image, VK_IMAGE_LAYOUT_GENERAL, nlv, c);
        vk_full_barrier(cb);
        return;
    }
    if (!t->addr) return;
    VkCommandBuffer cb = vk_upload_cmd();
    uint32_t layers = t->layout.layers ? t->layout.layers : 1, levels = t->layout.levels < nlv ? t->layout.levels : nlv;
    if (layers > b->layers) layers = b->layers;
    uint64_t layer_size = tegra_layer_size(&t->layout);
    for (uint32_t layer = 0; layer < layers; layer++)
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            uint32_t d = is3d ? l.d : 1, row = l.wb * s->bsize;
            VkSpan sp = vk_transient((uint64_t)row * l.hb * d, 16);
            const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
            if (t->flags & 0x50)  // (linear: rows t->stride apart with flag 0x10, packed with 0x40 - upload_texture)
                for (uint32_t y = 0; y < l.hb * d; y++)
                    memcpy((uint8_t *)sp.ptr + (uint64_t)y * row, src + (uint64_t)y * ((t->flags & 0x10) ? t->stride : row), row);
            else tegra_deswizzle(src, sp.ptr, l.wb, l.hb, d, s->bsize, l.block_height_log2, l.block_depth_log2);
            VkBufferImageCopy r = {.bufferOffset = sp.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                   .imageExtent = {l.wb, l.hb, d}};
            vkCmdCopyBufferToImage(cb, sp.buf, s->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
}

static BcShadow *bc_shadow_create(NvnTexture *t, TexBackend *b, VkFormat vf, uint32_t bsize, BcShadow *from) {
    bool is3d = b->base_type == VK_IMAGE_VIEW_TYPE_3D;
    uint32_t nlv = b->levels < 16 ? b->levels : 16, w = 1, h = 1, nslices = 0;
    BcShadow *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    for (uint32_t lv = 0; lv < nlv; lv++) {  // (level 0 wide enough that each level's blocks fit: 20 texels - 5, 3, 2, 1, 1;
        uint32_t bw = bc_blocks(b->width, lv), bh = bc_blocks(b->height, lv);  // a level of one block fits any: its extent is
        if (bw > 1 && bw << lv > w) w = bw << lv;                               // at least 1)
        if (bh > 1 && bh << lv > h) h = bh << lv;
        s->first[lv] = nslices;
        nslices += level_slices(b, lv);
    }
    {   // (as many levels as the image's largest side allows: that side at least 2^(levels - 1))
        uint32_t big = w > h ? w : h;
        if (is3d && b->depth > big) big = b->depth;
        if (big < 1u << (nlv - 1)) *(w >= h ? &w : &h) = 1u << (nlv - 1);
    }
    TexBackend *i = &s->img;
    i->width = w, i->height = h, i->depth = b->depth, i->layers = b->layers, i->levels = nlv, i->samples = 1;
    i->format = vf, i->aspect = VK_IMAGE_ASPECT_COLOR_BIT, i->base_type = b->base_type;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .flags = is3d ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0,  // (slices drawn through 2D views)
                            .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = vf,
                            .extent = {w, h, b->depth}, .mipLevels = nlv, .arrayLayers = b->layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT};  // (bc_shadow_fill)
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!(s->drawn = calloc(nslices, 1)) || vmaCreateImage(g_vk.vma, &ci, &ai, &i->image, &i->mem, NULL) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 4) LOG("[vk] BC %ux%ux%u drawn into as %d: no shadow image\n", t->width, t->height, t->depth, vf);
        free(s->drawn);
        free(s);
        return NULL;
    }
    vk_image_init_layout(vk_upload_cmd(), i->image, VK_IMAGE_ASPECT_COLOR_BIT);
    s->bsize = bsize;
    bc_shadow_fill(t, b, s, from);
    if (from) {  // (replaced: not dirty - vk_bc_shadows_resolve decoded it)
        views_forget(&from->img);
        defer((Dead){.img = from->img.image, .mem = from->img.mem});
        free(from->drawn);
        free(from);
    }
    b->bc_shadow = s;
    static int nlog;
    if (nlog++ < 8) LOG("[vk] BC texture %ux%ux%u (nvn %#x) drawn into as %d: blocks decoded after the draws\n",
                        t->width, t->height, t->depth, t->format, vf);
    return s;
}

// The format of the shadow a render target view of t draws into, VK_FORMAT_UNDEFINED when it draws without one
static VkFormat bc_shadow_format(const NvnTexture *t, const NvnTextureView *v) {
    const TexBackend *b = t->backend;
    if (!b || !b->bc_vf || !v || !(v->setmask & 4) || b->samples > 1) return VK_FORMAT_UNDEFINED;
    const FormatInfo *bf = t->layout.fmt, *vfi = nvn_format_info(v->format);
    if (!bf || !vfi || bf->bw != 4 || bf->bh != 4 || vfi->bw != 1 || vfi->bh != 1 || vfi->bpb != bf->bpb) return VK_FORMAT_UNDEFINED;
    uint32_t lv = v->setmask & 1 ? v->base_level : 0;
    int rt3d = rt3d_mode();
    if (lv >= b->levels || lv >= 16 || (b->base_type == VK_IMAGE_VIEW_TYPE_3D && (rt3d == 0 || (rt3d == 1 && lv > 0))))
        return VK_FORMAT_UNDEFINED;  // (LARECOMP_3D_RT: as vk_view)
    return vk_format(v->format);
}

VkImageView vk_bc_shadow_view(NvnTexture *t, const NvnTextureView *v, VkFormat *fmt, uint32_t *w, uint32_t *h) {
    TexBackend *b = t->backend;
    VkFormat vf = bc_shadow_format(t, v);
    if (!vf) return VK_NULL_HANDLE;
    const FormatInfo *bf = t->layout.fmt;
    uint32_t lv = v->setmask & 1 ? v->base_level : 0, z = v->setmask & 2 ? v->min_layer : 0;
    BcShadow *s = b->bc_shadow, *from = NULL;
    if (s && s->img.format != vf) {  // (one drawn into in another format: vk_bc_shadows_resolve decoded it, unless it is
        if (s->dirty) return VK_NULL_HANDLE;  // a target of this pass too; a new one takes its blocks)
        from = s, s = NULL;
    }
    if (!s && nbc_dirty < (int)(sizeof bc_dirty / sizeof *bc_dirty)) s = bc_shadow_create(t, b, vf, bf->bpb, from);
    if (!s || (!s->dirty && nbc_dirty == (int)(sizeof bc_dirty / sizeof *bc_dirty))) return VK_NULL_HANDLE;
    uint32_t n = level_slices(b, lv);
    if (z >= n) z = n - 1;
    VkImageView view = vk_image_view(&s->img, VK_IMAGE_VIEW_TYPE_2D, vf, lv, 1, z, 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!view) return VK_NULL_HANDLE;
    *fmt = vf;
    *w = bc_blocks(b->width, lv), *h = bc_blocks(b->height, lv);
    s->drawn[s->first[lv] + z] = 1;
    if (!s->dirty) s->dirty = true, bc_dirty[nbc_dirty++] = b;
    return view;
}

// The blocks drawn into b's shadow, back on the CPU and decoded into its image
static void bc_shadow_resolve(TexBackend *b) {
    BcShadow *s = b->bc_shadow;
    bool is3d = b->base_type == VK_IMAGE_VIEW_TYPE_3D;
    uint32_t nlv = s->img.levels, nslices = s->first[nlv - 1] + level_slices(b, nlv - 1), n = 0;
    VkBufferImageCopy *rg = malloc(nslices * sizeof *rg);
    if (!rg) {
        LOG("[vk] BC %ux%ux%u: no memory to decode its shadow\n", b->width, b->height, b->depth);
        memset(s->drawn, 0, nslices);  // (what was drawn is lost; the shadow stays usable)
        s->dirty = false;
        return;
    }
    uint64_t total = 0;
    for (uint32_t lv = 0; lv < nlv; lv++)
        for (uint32_t z = 0; z < level_slices(b, lv); z++) {
            if (!s->drawn[s->first[lv] + z]) continue;
            uint32_t wb = bc_blocks(b->width, lv), hb = bc_blocks(b->height, lv);
            rg[n++] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, is3d ? 0 : z, 1},
                                          .imageOffset = {0, 0, is3d ? (int32_t)z : 0}, .imageExtent = {wb, hb, 1}};
            total += (uint64_t)wb * hb * s->bsize;
        }
    if (n) {
        VkSpan rb = vk_readback(total);
        for (uint32_t i = 0; i < n; i++) rg[i].bufferOffset += rb.off;
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        vkCmdCopyImageToBuffer(cb, s->img.image, VK_IMAGE_LAYOUT_GENERAL, rb.buf, n, rg);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                               .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
        uint64_t t0 = SDL_GetTicksNS();
        renderer_flush();  // (the blocks on the CPU right after)
        vk_readback_ready(total);
        uint64_t t1 = SDL_GetTicksNS();
        cb = vk_cmd();
        for (uint32_t i = 0; i < n; i++) {
            const VkBufferImageCopy *r = &rg[i];
            bc_upload_blocks(cb, b, (const uint8_t *)rb.ptr + (r->bufferOffset - rb.off), r->imageExtent.width,
                             r->imageExtent.height, s->bsize, r->imageSubresource.mipLevel,
                             r->imageSubresource.baseArrayLayer, (uint32_t)r->imageOffset.z, NULL);
        }
        vk_full_barrier(cb);
        // (each decode: the GPU waited for - the render thread blocked - and the CPU's decode; the first 8, then every 64th)
        static uint64_t nres;
        if (nres++ < 8 || nres % 64 == 0) {
            uint64_t nonzero = 0, t2 = SDL_GetTicksNS();
            for (uint64_t i = 0; i < total; i++) nonzero += ((const uint8_t *)rb.ptr)[i] != 0;
            LOG("[vk] BC %ux%ux%u: decode %llu at frame %llu: %u slices (%llu of %llu bytes nonzero), GPU wait %.2f ms, decode %.2f ms\n",
                b->width, b->height, b->depth, (unsigned long long)nres, (unsigned long long)vk_frame_counter(), n,
                (unsigned long long)nonzero, (unsigned long long)total, (t1 - t0) / 1e6, (t2 - t1) / 1e6);
        }
    }
    free(rg);
    memset(s->drawn, 0, nslices);
    s->dirty = false;
}

void vk_bc_shadows_resolve(NvnTexture *const *targets, const NvnTextureView *views, const uint8_t *has_view, int ntargets) {
    for (int k = 0; k < nbc_dirty; k++) {
        TexBackend *b = bc_dirty[k];
        bool drawn = false;  // (still being drawn into: the next pass's target, through a view in the shadow's format -
                             // another one replaces the shadow, vk_bc_shadow_view)
        for (int i = 0; i < ntargets; i++)
            drawn |= targets[i] && targets[i]->backend == b && has_view[i] &&
                     bc_shadow_format(targets[i], &views[i]) == b->bc_shadow->img.format;
        if (drawn) continue;
        bc_shadow_resolve(b);
        bc_dirty[k--] = bc_dirty[--nbc_dirty];
    }
}
#endif

// A texture first drawn into at a level past 0, its mips made from the level the CPU filled: that level uploaded
// before the texture becomes the GPU's. BOTW fills its 32x32x32 noise volume on the CPU and draws the mips from it (then
// compresses it to BC4); never uploaded, it stayed zero - the fast-travel dissolve without its noise.
void vk_tex_upload_for_mips(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b && (b->gpu_owned || b->uploaded)) return;
    static int nlog;
    if (nlog++ < 8) LOG("[vk] %ux%ux%u (nvn %#x) first drawn into at a level past 0: its CPU contents uploaded first\n",
                        t->width, t->height, t->depth, t->format);
    vk_tex(t, false);
}

// 1x1 black textures for unbound slots, per type
VkImageView vk_dummy_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    TexBackend *b = &dummies[textype];
    VkImageViewType vt = view_type_for(textype);
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_R8G8B8A8_UNORM;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = vk_image_view(b, vt, b->format, 0, 1, 0, cube ? 6 : 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    return views[textype];
}

// ================================================================== samplers
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkSampler make_sampler(const NvnSamplerDesc *d, bool compare);

VkSampler vk_sampler(NvnSamplerDesc *d) {
    if (!d->backend) d->backend = (void *)make_sampler(d, true);
    return (VkSampler)d->backend;
}

// The sampler for a lookup without depth comparison (a plain texture() in the shader): a comparison sampler there is
// undefined in Vulkan. Turnip ignores the comparison; Qualcomm's driver applies it - BOTW's linear-depth pass samples
// its depth buffer through a comparison sampler and got 0 everywhere: no lighting, Link a black silhouette. Kept per
// comparison sampler (the slot's VkSampler), so the descriptor struct the game sees stays as it is.
VkSampler vk_sampler_plain(NvnSamplerDesc *d) {
    VkSampler s = vk_sampler(d);
    if (!d->compare_mode) return s;
    static struct { VkSampler cmp, plain; } tab[256];
    static int n, next;
    for (int i = 0; i < n; i++) if (tab[i].cmp == s) return tab[i].plain;
    VkSampler p = make_sampler(d, false);
    int i = n < 256 ? n++ : next++ % 256;  // (full: the oldest pair goes; its sampler stays alive, as the slots' do)
    tab[i].cmp = s; tab[i].plain = p;
    return p;
}

static VkSampler make_sampler(const NvnSamplerDesc *d, bool compare) {
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->min_filter;
    si.minFilter = (mf == 1 || mf == 3 || mf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    bool border = false;
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        // NVN wrap modes: 0 clamp, 1 repeat, 2 mirror clamp, 3 mirror clamp to edge, 4 mirror clamp to border, 5 clamp to
        // border, 6 mirrored repeat, 7 clamp to edge. BOTW's agl table (main + 0x1c21ccc, agl wrap -> NVN: [1, 6, 7, 5, 3,
        // 0, 3, 4]) is in agl's own order, not GX2's: read as GX2 it swapped 3 and 5, and the sun's shadow map (wrap 5,
        // clamp to border) turned mirror-once - sunlight through the shrine's walls.
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; border = true; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0]; si.addressModeV = modes[1]; si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= 1) si.maxLod = si.minLod + 0.25f;  // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias;
    if (d->max_aniso > 1) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = fminf(d->max_aniso, 16); }
    static float lodmin = -1;
    if (lodmin < 0) lodmin = getenv("LARECOMP_LOD_MIN") ? (float)atof(getenv("LARECOMP_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) si.minLod = fmaxf(si.minLod, lodmin);
    static int stest = -1;
    if (stest < 0) stest = getenv("LARECOMP_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { si.minLod = 0; si.maxLod = VK_LOD_CLAMP_NONE; si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 16; }
    if (d->compare_mode && compare) {
        si.compareEnable = VK_TRUE;
        si.compareOp = getenv("LARECOMP_NO_SHADOW") ? VK_COMPARE_OP_ALWAYS : compare_op(d->compare_func);
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border) {
        const float *c = d->border;
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (g_vk.custom_border) {
            memcpy(cb.customBorderColor.float32, c, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            si.pNext = &cb;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    VkSampler s;
    VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s));
    return s;
}

VkSampler vk_default_sampler(bool linear) {
    static VkSampler s[2];
    if (!s[linear]) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                  .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s[linear]));
    }
    return s[linear];
}
