// SPDX-License-Identifier: GPL-3.0-or-later
// Frame generation in BOTWRecompAndroid's present (Lossless Scaling's LSFG: lsfg_chain.c, lsfg_dll.c, lsfg_pacer.c, ported
// from Swap2-Native's dm/gfx/vk_android/lsfg, itself a C port of Eden's, which builds on lsfg-vk; after Swap2-Native's
// vka_framegen.c). Each game frame (vulkan/device.c vk_present_now) is copied into the chain's input, the shared passes
// run, and the frames between the previous game frame and this one are written to images of their own; the present
// shows them before the game frame, each for an equal share of the game's measured frame interval, through the same
// upscaler and overlay as the game's. The work is recorded into the frame's own command buffer, ahead of the presents
// that read it. Off by default; the user's own Lossless.dll (Lossless Scaling for Windows) is read once, never bundled.
// Settings: Video > Frame generation (settings.h lsfg_*); LARECOMP_LSFG=0/1, LARECOMP_LSFG_MULT=2..4,
// LARECOMP_LSFG_FLOW=0 (the game's resolution) or 25..100 (%), LARECOMP_LSFG_DLL=<path> override them.
#if defined(__ANDROID__)
#include "lsfg_host.h"
#include "lsfg.h"
#include "../../../settings.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static struct {
    int state;  // 0 not tried, 1 ready (modules loaded), -1 unavailable
    char reason[256], dll[512];
    lsfg_modules mods;
    lsfg_chain *chain;
    VkExtent2D ext;
    int flow;
    VkImage out[LSFG_MAX_GENERATIONS];
    VmaAllocation out_mem[LSFG_MAX_GENERATIONS];
    VkImageView out_view[LSFG_MAX_GENERATIONS];
    TexBackend out_tex[LSFG_MAX_GENERATIONS];  // (the outputs as the present draws textures: vk_present_draw)
    lsfg_pacer pacer;
    uint64_t count;
    uint32_t warm_streak;
    int generating;  // the last frame's: 1 generated, 0 not
} F;

static double mono_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int env_int(const char *name, int def) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

static void unavailable(const char *why) {
    F.state = -1;
    snprintf(F.reason, sizeof F.reason, "%s", why);
    LOG("[lsfg] frame generation unavailable: %s\n", why);
}

// Where the user's Lossless.dll may be: LARECOMP_LSFG_DLL, the app's own files, then the places Swap2-Native looks
const char *vk_fg_dll_path(void) {
    static char found[512];
    static int looked;
    if (looked) return found[0] ? found : NULL;
    looked = 1;
    const char *e = getenv("LARECOMP_LSFG_DLL");
    if (e && *e) { snprintf(found, sizeof found, "%s", e); return found; }
    static const char *const places[] = {
        "/storage/emulated/0/Android/data/com.botwrecomp.android/files/Lossless.dll",
        "/storage/emulated/0/Documents/Emulation/bios/Lossless.dll",
        "/storage/emulated/0/Switch2/Lossless.dll",
        "/storage/emulated/0/LSFG/Lossless.dll",
        "/storage/emulated/0/Download/Lossless.dll",
        "/storage/emulated/0/Lossless.dll",
    };
    struct stat sb;
    for (size_t i = 0; i < sizeof places / sizeof *places; i++)
        if (stat(places[i], &sb) == 0 && sb.st_size > 0) { snprintf(found, sizeof found, "%s", places[i]); return found; }
    return NULL;
}

// The device must have what the shaders use (Float16, storage images of any format, unformatted writes), null
// descriptors, and storage images of the chain's formats, linearly filterable
static int device_ok(void) {
    if (!g_vk.lsfg_features) { unavailable("the GPU lacks shaderFloat16 or storage images of any format (Vulkan 1.2 features)"); return 0; }
    if (!g_vk.null_descriptors) { unavailable("the GPU lacks null descriptors (VK_EXT_robustness2)"); return 0; }
    VkFormat fmts[3] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    for (int i = 0; i < 3; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, fmts[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
            !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
            unavailable("the GPU lacks storage images of R8, RGBA8 or RGBA16F");
            return 0;
        }
    }
    return 1;
}

static int load(void) {
    if (F.state) return F.state > 0;
    if (!device_ok()) return 0;
    const char *path = vk_fg_dll_path();
    if (!path) {
        unavailable("no Lossless.dll (put your own copy in Download/, LSFG/, Documents/Emulation/bios/ or the app's files)");
        return 0;
    }
    uint64_t t0 = plat_now_ns();
    int r = lsfg_dll_load(path, &F.mods);
    if (r) {
        char b[600];
        snprintf(b, sizeof b, "%s: %s", path, lsfg_status_name(r));
        unavailable(b);
        return 0;
    }
    snprintf(F.dll, sizeof F.dll, "%s", path);
    LOG("[lsfg] %d shaders from %s (%.1f ms)\n", F.mods.n, path, (plat_now_ns() - t0) / 1e6);
    F.state = 1;
    return 1;
}

static void teardown(void) {
    if (!F.chain && !F.out[0]) return;
    vkQueueWaitIdle(g_vk.queue);
    lsfg_chain_free(F.chain);
    F.chain = NULL;
    for (int i = 0; i < LSFG_MAX_GENERATIONS; i++) {
        extern void vk_texture_views_drop(TexBackend *b);  // (textures.c: the present's views of it)
        if (F.out[i]) vk_texture_views_drop(&F.out_tex[i]);
        if (F.out_view[i]) vkDestroyImageView(g_vk.dev, F.out_view[i], NULL);
        if (F.out[i]) vmaDestroyImage(g_vk.vma, F.out[i], F.out_mem[i]);
        F.out[i] = VK_NULL_HANDLE, F.out_view[i] = VK_NULL_HANDLE, F.out_mem[i] = NULL;
        memset(&F.out_tex[i], 0, sizeof F.out_tex[i]);
    }
    lsfg_pacer_reset(&F.pacer);
    F.count = 0, F.warm_streak = 0;
}

static int out_new(int i, VkExtent2D e) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {e.width, e.height, 1}, .mipLevels = 1,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkResult r = vmaCreateImage(g_vk.vma, &ci, &ai, &F.out[i], &F.out_mem[i], NULL);
    if (r) { LOG("[lsfg] output image %ux%u: %d\n", e.width, e.height, r); return 0; }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = F.out[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if ((r = vkCreateImageView(g_vk.dev, &vi, NULL, &F.out_view[i]))) { LOG("[lsfg] output view: %d\n", r); return 0; }
    TexBackend *t = &F.out_tex[i];
    memset(t, 0, sizeof *t);
    t->image = F.out[i];
    t->format = VK_FORMAT_R8G8B8A8_UNORM;
    t->base_type = VK_IMAGE_VIEW_TYPE_2D;
    t->width = e.width, t->height = e.height, t->depth = 1, t->layers = 1, t->levels = 1, t->samples = 1;
    t->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    t->gpu_owned = true, t->uploaded = true;
    return 1;
}

static int rebuild(VkExtent2D e, int flow) {
    teardown();
    for (int i = 0; i < LSFG_MAX_GENERATIONS; i++)
        if (!out_new(i, e)) { teardown(); return 0; }
    uint64_t t0 = plat_now_ns();
    F.chain = lsfg_chain_new(&F.mods, e, flow ? flow / 100.0f : 1.0f, F.out, F.out_view);
    if (!F.chain) { teardown(); return 0; }
    F.ext = e, F.flow = flow;
    LOG("[lsfg] chain for %ux%u made in %.1f ms (flow %d%%)\n", e.width, e.height, (plat_now_ns() - t0) / 1e6, flow ? flow : 100);
    return 1;
}

// The present's state, for the menu: 1 generating, 0 off, 2 on and not generating now, -1 unavailable (reason in *why)
int vk_fg_status(const char **why) {
    bool on = env_int("LARECOMP_LSFG", g_settings.lsfg) != 0;
    if (why) *why = F.state < 0 ? F.reason : !on ? "off" : F.generating ? "generating" : "on, waiting for a steady frame rate";
    return F.state < 0 ? -1 : !on ? 0 : F.generating ? 1 : 2;
}

// For the game frame in `game` (its present, before the window image is taken): the frames to show before it (gen[],
// up to 3) and how long each is held (*hold_ns); 0 when none. Records into the frame's command buffer (vk_cmd).
int vk_fg_frame(TexBackend *game, TexBackend **gen, uint64_t *hold_ns, uint64_t vsync_ns) {
    *hold_ns = 0;
    bool on = env_int("LARECOMP_LSFG", g_settings.lsfg) != 0;
    int mult = env_int("LARECOMP_LSFG_MULT", g_settings.lsfg_mult), flow = env_int("LARECOMP_LSFG_FLOW", g_settings.lsfg_flow);
    if (mult < 2) mult = 2;
    if (mult > 4) mult = 4;
    if (flow && flow < 25) flow = 25;
    if (flow > 100) flow = 100;
    if (!on) {
        if (F.chain) { teardown(); LOG("[lsfg] frame generation off\n"); }
        F.generating = 0;
        return 0;
    }
    if (!load()) return 0;
    if (!game || !game->image || game->aspect != VK_IMAGE_ASPECT_COLOR_BIT || game->samples > 1 || !game->width || !game->height) {
        F.generating = 0;
        return 0;
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, game->format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
        static int told;
        if (!told++) LOG("[lsfg] the game's image (format %d) can't be copied into the chain: not generating\n", game->format);
        F.generating = 0;
        return 0;
    }
    VkExtent2D e = {game->width, game->height};
    if (!F.chain || F.ext.width != e.width || F.ext.height != e.height || F.flow != flow) {
        if (!rebuild(e, flow)) { unavailable("the frame generation chain could not be made (see the [lsfg] lines)"); return 0; }
    }
    lsfg_plan plan = lsfg_pacer_plan(&F.pacer, LSFG_MAX_GENERATIONS, mult, 0, mono_s());
    uint64_t count = F.count++;
    int warm = plan.warm && count + 1 >= 2;
    F.warm_streak = warm ? F.warm_streak + 1 : 0;
    size_t ng = warm && F.warm_streak >= 2 ? plan.generations : 0;
    if (ng > LSFG_MAX_GENERATIONS) ng = LSFG_MAX_GENERATIONS;
    double interval = F.pacer.smoothed_interval > 0 ? F.pacer.smoothed_interval : 1.0 / 30;
    // The panel shows one image a refresh: no more frames than the refreshes in the game's frame leave room for (30 fps
    // on 60 Hz: one; on 120 Hz: three). LARECOMP_LSFG_REFRESH_CAP=0: no cap.
    if (ng && vsync_ns && env_int("LARECOMP_LSFG_REFRESH_CAP", 1)) {
        double n = interval * 1e9 / (double)vsync_ns;
        size_t fit = n < 1.9 ? 0 : (size_t)(n + 0.1) - 1;
        if (ng > fit) ng = fit, F.pacer.issued = ng;
    }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // (the game's writes to its image; the presents still reading the outputs)
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)e.width, (int32_t)e.height, 1}},
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)e.width, (int32_t)e.height, 1}}};
    vkCmdBlitImage(cb, game->image, VK_IMAGE_LAYOUT_GENERAL, lsfg_chain_input(F.chain, count), VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                   VK_FILTER_NEAREST);
    if (warm) lsfg_chain_shared(F.chain, cb, count);
    for (size_t g = 0; g < ng; g++) lsfg_chain_generate(F.chain, cb, count, ng, g);
    vk_full_barrier(cb);  // (the outputs, for the presents after this)
    for (size_t g = 0; g < ng; g++) gen[g] = &F.out_tex[g];
    if (ng) *hold_ns = (uint64_t)(interval * 1e9 / (double)(ng + 1));
    if (!!ng != F.generating) {
        static int nlog;
        if (nlog++ < 20) LOG("[lsfg] %s (game frame interval %.1f ms, %zu generated)\n", ng ? "generating" : "not generating",
                             interval * 1e3, ng);
    }
    F.generating = ng ? 1 : 0;
    return (int)ng;
}
#endif
