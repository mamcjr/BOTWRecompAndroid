#!/usr/bin/env python3
"""Host unit tests for narrow-landscape Unlocked aspect ratio (output_scale.c, device.c, widescreen.c, exec.c)."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ENV(s) (getenv(s) != NULL)
#define LOG(...) ((void)(0 && printf(__VA_ARGS__)))
#define RT_ON_RESET(name) static void name(void)

enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1 };
enum { RES_MATCH = 0, RES_1X = 1, RES_Q75 = 9, RES_Q50 = 10, RES_Q25 = 11, RES_DYNAMIC = 12 };
enum { OUT_AUTO = 0 };

static struct { int aspect, res_mode, out_lines; bool center_world_map; } g_settings = {ASPECT_UNLOCKED, RES_MATCH, OUT_AUTO, false};
static int g_game_version = 190;
static bool title, menu, touch, low_ram_flag;
static uint64_t frame = 100, scene = 100;

bool framepace_on_title(void) { return title; }
bool framepace_in_menu(void) { return menu; }
bool framepace_on(void) { return true; }
int framepace_target(void) { return 30; }
int fps_mode_now(void) { return 1; }
uint64_t vk_scene_frame(void) { return scene; }
uint64_t renderer_present_count(void) { return frame; }
bool touch_hud_top(void) { return touch; }
bool vk_low_ram(void) { return low_ram_flag; }

static float from_bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static void eqf(float a, float b) {
    if (!(fabsf(a - b) < 0.0005f)) { fprintf(stderr, "eqf fail: %.6f != %.6f\n", a, b); assert(0); }
}
static void eqd(double a, double b) {
    if (!(fabs(a - b) < 0.0005)) { fprintf(stderr, "eqd fail: %.6f != %.6f\n", a, b); assert(0); }
}
'''

UI_STUBS = r'''
typedef struct { uint32_t width, height; int target; } NvnTexture;
typedef struct { uint64_t addr, size; } MockRange;
typedef struct { int ntex; int tex[4]; } MockFi;
typedef struct { int32_t x, y; } VkOffset2D;
typedef struct { uint32_t width, height; } VkExtent2D;
typedef struct { VkOffset2D offset; VkExtent2D extent; } VkRect2D;
enum { RS_OUT = 1 };
static NvnTexture target = {1920, 1080, 1};
static NvnTexture bg_tex = {1920, 1080, 1};
static MockFi mock_fi = {1, {0}};
static struct { struct { int ncolor; NvnTexture *color[1]; } rt; int scissor[1][4]; MockRange ubuf[2][1]; } st = {
    .rt = {1, {&target}},
    .scissor = {{0, 0, 1920, 1080}},
};
static struct { MockFi *fi; } mock_pr = {&mock_fi}, *pr = &mock_pr;
static NvnTexture *shader_texture_object(int s, int *t) { (void)s; (void)t; return &bg_tex; }
static float mock_tex_sx = 1.0f, mock_tex_sy = 0.9f;
static void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) { (void)t; *sx = mock_tex_sx; *sy = mock_tex_sy; }
static int vk_texture_scale_group(NvnTexture *t) { (void)t; return RS_OUT; }
static uint32_t pass_w = 1728, pass_h = 1080;
static float vp_sx = 0.9f, vp_sy = 1.0f, vp_ox = 0.0f, vp_oy = 0.0f;
static float mock_pass_sx = 1.0f, mock_pass_sy = 0.9f;
static void pass_size(int *pw, int *ph) { *pw = (int)pass_w; *ph = (int)pass_h; }
static uint64_t g_cur_fs_hash;
static bool g_wide_backdrop;
static bool g_off_layout;
static bool g_debug_frame;
static float copied[140];
typedef struct { void *ptr; } VkSpan;
typedef VkSpan VkDescriptorBufferInfo;
static VkSpan vk_transient_copy(const void *f, uint64_t size, int align) {
    assert(size <= sizeof copied && align == 256);
    memset(copied, 0, sizeof copied);
    memcpy(copied, f, size);
    return (VkSpan){copied};
}
static VkSpan ubo_span(VkSpan c, uint64_t size) {
    assert(c.ptr == copied && size <= sizeof copied);
    return c;
}
static void panefix_log(const char *why, const float *f) { (void)why; (void)f; }
'''

EXEC_WRAPPERS = r'''
static bool run_cbuf(const float *f, uint64_t size, NvnTexture *t0) {
    assert(size >= 64);
    copied[0] = NAN;
    (void)ubo_span;
'''

EXEC_WRAPPER_MID = r'''
    return false;
}
'''

BACKDROP_HEAD = r'''
static void eval_wide_backdrop(void) {
'''

VIEWPORT_HEAD = r'''
static void eval_tall_ui_viewport(bool composite, const float *f, uint64_t size) {
    st.ubuf[0][0] = (MockRange){(uint64_t)(uintptr_t)f, size};
    float pass_sx = mock_pass_sx, pass_sy = mock_pass_sy;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0.0f; vp_oy = 0.0f;
'''

SCISSOR_HEAD = r'''
static VkRect2D run_scissor_blocks(void) {
    int w = (int)pass_w, h = (int)pass_h;
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    float sx = vp_sx, sy = vp_sy, ox = vp_ox, oy = vp_oy;
'''

MAIN = r'''
static void boot(float shape, int aspect) {
    widescreen_reset();
    g_settings.aspect = aspect;
    widescreen_set_shape(shape);
    widescreen_set_factor(shape);
}

static void init_f_defaults(float *f, float px_aspect) {
    memset(f, 0, sizeof(float) * 140);
    f[0] = 1.0f;
    f[5] = px_aspect;
    f[14] = -1.0f;
    f[16] = 1.0f;
    f[21] = 1.0f;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--wide-test")) {
        setenv("LARECOMP_WIDE_TEST", argv[2], 1);
        OutScale os = {0};
        output_scale(2560, 1600, true, 1.0, 10.0, &os);
        double wt = atof(argv[2]);
        double want = (!isfinite(wt) || wt <= 0.0 || wt > 16.0) ? 0.9
                    : (wt > 1.005 || (wt < 0.995 && wt >= 9.0 / 16.0)) ? wt : 1.0;
        eqd(os.shape, want);
        eval_device_shape(2560, 1600);
        eqf(atomic_load_explicit(&g_shape, memory_order_relaxed), (float)want);
        return 0;
    }

    /* 1. output_scale.c & device.c shape guard tests */
    OutScale os = {0};
    /* 16:9 baseline */
    output_scale(1920, 1080, true, 1.0, 10.0, &os);
    eqd(os.shape, 1.0); eqd(os.vw, 1920.0); eqd(os.vh, 1080.0); eqd(os.vx, 0.0); eqd(os.vy, 0.0);
    /* 16:10 Auto (1080 lines) */
    output_scale(2560, 1600, true, 1.0, 10.0, &os);
    eqd(os.shape, 0.9);
    eqd(os.vw, 2560.0); eqd(os.vh, 1600.0); eqd(os.vx, 0.0); eqd(os.vy, 0.0);
    eqd(os.sx / os.sy, 0.9);
    eqd(os.scene_sx / os.scene_sy, 0.9);
    /* 16:10 Match display (out_lines = 1600 -> sy * 1080 = 1600) */
    g_settings.out_lines = 1600;
    output_scale(2560, 1600, true, 1.0, 10.0, &os);
    eqd(os.shape, 0.9); eqd(os.sy * 1080.0, 1600.0); eqd(os.sx * 1920.0, 2560.0);
    g_settings.out_lines = OUT_AUTO;
    /* 4:3 & exact 1:1 (9/16 = 0.5625) landscape */
    output_scale(2048, 1536, true, 1.0, 10.0, &os);
    eqd(os.shape, 0.75); eqd(os.vw, 2048.0); eqd(os.vh, 1536.0);
    output_scale(1440, 1440, true, 1.0, 10.0, &os);
    eqd(os.shape, 0.5625); eqd(os.vw, 1440.0); eqd(os.vh, 1440.0);
    /* Portrait rejected (letterboxed 16:9) */
    output_scale(1080, 2400, true, 1.0, 10.0, &os);
    eqd(os.shape, 1.0); eqd(os.vw, 1080.0); eqd(os.vh, 607.5);
    output_scale(1812, 2176, true, 1.0, 10.0, &os);
    eqd(os.shape, 1.0);
    output_scale(900, 1000, true, 1.0, 10.0, &os);
    eqd(os.shape, 1.0);
    /* Invalid extents (dh == 0, negative dw/dh, NaN) never produce NaN/Inf */
    output_scale(1920, 0, true, 1.0, 10.0, &os);
    assert(isfinite(os.sx) && isfinite(os.sy) && isfinite(os.scene_sx));
    eqd(os.shape, 1.0);
    output_scale(-1440, -1440, true, 1.0, 10.0, &os);
    eqd(os.shape, 1.0);
    output_scale(2560, 1600, true, NAN, NAN, &os);
    assert(isfinite(os.sx) && isfinite(os.sy) && isfinite(os.shape));
    /* 20:9 Ultrawide unchanged */
    output_scale(3200, 1440, true, 1.0, 10.0, &os);
    eqd(os.shape, 1.25); eqd(os.vw, 3200.0); eqd(os.vh, 1440.0);
    /* Original rejected */
    output_scale(2560, 1600, false, 1.0, 10.0, &os);
    eqd(os.shape, 1.0); eqd(os.vw, 2560.0); eqd(os.vh, 1440.0); eqd(os.vx, 0.0); eqd(os.vy, 80.0);
    /* device.c deadband [0.995, 1.005] and portrait guard */
    unsetenv("LARECOMP_WIDE_TEST");
    eval_device_shape(1916, 1080); eqf(atomic_load_explicit(&g_shape, memory_order_relaxed), 1.0f);  /* 0.9979 -> 1.0 */
    eval_device_shape(1928, 1080); eqf(atomic_load_explicit(&g_shape, memory_order_relaxed), 1.0f);  /* 1.0042 -> 1.0 */
    eval_device_shape(1080, 1920); eqf(atomic_load_explicit(&g_shape, memory_order_relaxed), 1.0f);  /* portrait -> 1.0 */
    eval_device_shape(1440, 1440); eqf(atomic_load_explicit(&g_shape, memory_order_relaxed), 0.5625f);
    eval_device_shape(2560, 1600); eqf(atomic_load_explicit(&g_shape, memory_order_relaxed), 0.9f);

    /* 2. widescreen.c camera Vert+, Hor-, LRU, resize restoration, rt_persp_aspect, reset */
    float proj[64] = {0};
    const int ni = 0x98 / 4, fi = 0x9C / 4, fvi = 0xA0 / 4, si = 0xA4 / 4, ci = 0xA8 / 4, ti = 0xAC / 4, ai = 0xB0 / 4;
    const float base = 16.0f / 9.0f;
    float orig_fovy = 50.0f * 0.0174532925f;

    /* 16:9 Original rejects widening/narrowing */
    boot(0.9f, ASPECT_ORIGINAL);
    proj[ni] = 1.0f; proj[fi] = 25000.0f; proj[fvi] = orig_fovy; proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    eqf(proj[ai], base); eqf(proj[fvi], orig_fovy);
    assert(!widescreen_ui_centred()); assert(!widescreen_narrow_vert_plus());

    /* 20:9 Ultrawide widens aspect without altering fovy */
    boot(1.25f, ASPECT_UNLOCKED);
    proj[ni] = 1.0f; proj[fi] = 25000.0f; proj[fvi] = orig_fovy; proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    eqf(proj[ai], base * 1.25f); eqf(proj[fvi], orig_fovy);
    assert(widescreen_ui_centred()); assert(!widescreen_narrow_vert_plus());
    assert(!atomic_load_explicit(&vert_used, memory_order_acquire));  /* zero mutex overhead in 20:9 */
    proj[ai] = base;
    eqf(from_bits(rt_persp_aspect((uint64_t)(uintptr_t)proj)), base * 1.25f);

    /* 16:10 Vert+ (mAngle / proj[fvi] preserved so fovy-derived blends cannot creep) */
    boot(0.9f, ASPECT_UNLOCKED);
    proj[ni] = 1.0f; proj[fi] = 25000.0f; proj[fvi] = orig_fovy; proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    float want_tan_09 = tanf(orig_fovy * 0.5f) / 0.9f;
    float want_half_09 = atanf(want_tan_09);
    eqf(proj[ai], base * 0.9f);
    eqf(proj[ti], want_tan_09);
    eqf(proj[si], sinf(want_half_09));
    eqf(proj[ci], cosf(want_half_09));
    eqf(proj[fvi], orig_fovy);
    assert(widescreen_narrow_vert_plus());
    assert(atomic_load_explicit(&vert_used, memory_order_acquire));

    /* Idempotency + dynamic near/far update without double-scaling */
    proj[ai] = base; proj[ni] = 0.25f; proj[fi] = 5000.0f;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    eqf(proj[ai], base * 0.9f);
    eqf(proj[ti], want_tan_09);
    eqf(proj[fvi], orig_fovy);

    /* LRU eviction (> 64 transient cameras) must NOT evict actively rebuilt main camera */
    float temps[300][64];
    for (int t = 0; t < 300; t++) {
        memset(temps[t], 0, sizeof temps[t]);
        temps[t][ni] = 1.0f; temps[t][fi] = 1000.0f; temps[t][fvi] = orig_fovy; temps[t][ai] = base;
        rt_persp_update((uint64_t)(uintptr_t)temps[t]);
        if (t % 50 == 0) {
            proj[ai] = base;
            rt_persp_update((uint64_t)(uintptr_t)proj);
            eqf(proj[ti], want_tan_09);
            eqf(proj[fvi], orig_fovy);
        }
    }
    proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    eqf(proj[ti], want_tan_09);
    eqf(proj[fvi], orig_fovy);

    /* Stack address reuse by native 1:1 (90 deg, near 0.1, far 300) camera after 16:9 (near 1.0, far 25000)
       on 1:1 display (want_aspect == 1.0f), AND unrelated aspect 0.8f reuse: neither mis-scales! */
    boot(0.5625f, ASPECT_UNLOCKED);
    float reused[64] = {0};
    reused[ni] = 1.0f; reused[fi] = 25000.0f; reused[fvi] = orig_fovy; reused[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)reused);
    reused[ni] = 0.1f; reused[fi] = 300.0f; reused[fvi] = 90.0f * 0.0174532925f;
    reused[si] = sinf(reused[fvi] * 0.5f); reused[ci] = cosf(reused[fvi] * 0.5f); reused[ti] = tanf(reused[fvi] * 0.5f);
    reused[ai] = 1.0f;  /* same numeric want_aspect=1.0 on 1:1 display, different near/far & trig */
    rt_persp_update((uint64_t)(uintptr_t)reused);
    eqf(reused[fvi], 90.0f * 0.0174532925f); eqf(reused[ti], 1.0f); eqf(reused[ai], 1.0f);
    reused[ni] = 0.1f; reused[fi] = 300.0f; reused[fvi] = orig_fovy; reused[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)reused);
    reused[fvi] = 90.0f * 0.0174532925f;
    reused[ai] = 0.8f;  /* unrelated aspect invalidates entry */
    rt_persp_update((uint64_t)(uintptr_t)reused);
    eqf(reused[fvi], 90.0f * 0.0174532925f); eqf(reused[ai], 0.8f);

    /* Dynamic FOV update while aspect stayed want_aspect -> scales cleanly */
    boot(0.9f, ASPECT_UNLOCKED);
    proj[ni] = 1.0f; proj[fi] = 25000.0f; proj[fvi] = orig_fovy; proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    float zoom_fovy = 20.0f * 0.0174532925f;
    proj[fvi] = zoom_fovy;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    float want_zoom_tan = tanf(zoom_fovy * 0.5f) / 0.9f;
    eqf(proj[ti], want_zoom_tan);
    eqf(proj[fvi], zoom_fovy);
    proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    eqf(proj[ti], want_zoom_tan);
    eqf(proj[fvi], zoom_fovy);

    /* 60-frame zoom/paraglider/cutscene lerp reading current proj[fvi]: 0.0000 creep with & without aspect reset */
    for (int reset_asp = 0; reset_asp < 2; reset_asp++) {
        float lerp_p[64] = {0};
        lerp_p[ni] = 1.0f; lerp_p[fi] = 25000.0f; lerp_p[fvi] = orig_fovy; lerp_p[ai] = base;
        float expected = orig_fovy;
        for (int fr = 0; fr < 60; fr++) {
            float target = fr < 30 ? zoom_fovy : orig_fovy;
            float cur = lerp_p[fvi];
            expected = expected + (target - expected) * 0.12f;
            float next = cur + (target - cur) * 0.12f;
            lerp_p[fvi] = next;
            lerp_p[si] = sinf(next * 0.5f); lerp_p[ci] = cosf(next * 0.5f); lerp_p[ti] = tanf(next * 0.5f);
            if (reset_asp || fr == 0) lerp_p[ai] = base;
            rt_persp_update((uint64_t)(uintptr_t)lerp_p);
            eqf(lerp_p[fvi], expected);
            eqf(lerp_p[ti], tanf(expected * 0.5f) / 0.9f);
        }
    }
    /* Copy projection into a fresh stack/object address -> zero double-scaling */
    float copy_p[64];
    memcpy(copy_p, proj, sizeof copy_p);
    copy_p[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)copy_p);
    eqf(copy_p[fvi], zoom_fovy);
    eqf(copy_p[ti], want_zoom_tan);

    /* Portrait shapes (< 9/16 = 0.5625) never reshape cameras; exact 0.5625 accepted */
    for (int pi = 0; pi < 3; pi++) {
        float ps = pi == 0 ? 0.561f : pi == 1 ? 0.54f : 0.506f;
        boot(ps, ASPECT_UNLOCKED);
        proj[fvi] = orig_fovy; proj[ai] = base;
        rt_persp_update((uint64_t)(uintptr_t)proj);
        eqf(proj[ai], base); eqf(proj[fvi], orig_fovy);
        eqf(from_bits(rt_persp_aspect((uint64_t)(uintptr_t)proj)), base);
    }
    boot(0.5625f, ASPECT_UNLOCKED);
    proj[fvi] = orig_fovy; proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    eqf(proj[ai], 1.0f);
    eqf(proj[ti], tanf(orig_fovy * 0.5f) / 0.5625f);
    eqf(proj[fvi], orig_fovy);

    /* Extreme fovy near 2.95 rad clamped safely <= 2.98 rad with consistent sin/cos/tan while preserving mAngle */
    float ext_proj[64] = {0};
    ext_proj[ni] = 1.0f; ext_proj[fi] = 1000.0f; ext_proj[fvi] = 2.95f; ext_proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)ext_proj);
    eqf(ext_proj[fvi], 2.95f);
    eqf(ext_proj[ti], tanf(1.49f));
    eqf(ext_proj[si], sinf(1.49f));
    eqf(ext_proj[ci], cosf(1.49f));

    /* Dynamic resize 0.90 -> 0.75 -> 1.00 restores trig/tan AND mAspect AND clears vert_used */
    boot(0.9f, ASPECT_UNLOCKED);
    proj[ni] = 1.0f; proj[fi] = 25000.0f; proj[fvi] = orig_fovy; proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    widescreen_set_shape(0.75f);
    eqf(from_bits(rt_persp_aspect((uint64_t)(uintptr_t)proj)), base * 0.75f);
    rt_persp_update((uint64_t)(uintptr_t)proj);
    eqf(proj[ai], base * 0.75f);
    eqf(proj[fvi], orig_fovy);
    eqf(proj[ti], tanf(orig_fovy * 0.5f) / 0.75f);
    widescreen_set_shape(1.0f);
    eqf(from_bits(rt_persp_aspect((uint64_t)(uintptr_t)proj)), base);
    rt_persp_update((uint64_t)(uintptr_t)proj);
    eqf(proj[ai], base);
    eqf(proj[fvi], orig_fovy);
    eqf(proj[ti], tanf(orig_fovy * 0.5f));
    eqf(proj[si], sinf(orig_fovy * 0.5f));
    eqf(proj[ci], cosf(orig_fovy * 0.5f));
    assert(!atomic_load_explicit(&vert_used, memory_order_acquire));

    /* Dynamic resize 0.75 -> 1.25 (ultrawide) restores trig/tan and widens mAspect to 2.2222 without guest 16:9 reset */
    boot(0.75f, ASPECT_UNLOCKED);
    proj[ni] = 1.0f; proj[fi] = 25000.0f; proj[fvi] = orig_fovy; proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    widescreen_set_shape(1.25f);
    rt_persp_update((uint64_t)(uintptr_t)proj);
    eqf(proj[ai], base * 1.25f);
    eqf(proj[fvi], orig_fovy);
    eqf(proj[ti], tanf(orig_fovy * 0.5f));
    assert(!atomic_load_explicit(&vert_used, memory_order_acquire));

    /* widescreen_reset() clears g_wide, menu_last, vert_used, and seen timestamps */
    frame = 500; scene = 400; menu = true;
    assert(widescreen_in_menu());
    widescreen_set_factor(0.9f);
    menu = false;
    widescreen_reset();
    eqf(widescreen_factor(), 1.0f);
    assert(!widescreen_in_menu());
    assert(!atomic_load_explicit(&vert_used, memory_order_acquire));

    /* LARECOMP_NARROW_HOR_MINUS=1 disables Vert+ fovy change */
    setenv("LARECOMP_NARROW_HOR_MINUS", "1", 1);
    boot(0.9f, ASPECT_UNLOCKED);
    proj[fvi] = orig_fovy; proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    assert(!widescreen_narrow_vert_plus());
    eqf(proj[ai], base * 0.9f);
    eqf(proj[fvi], orig_fovy);
    unsetenv("LARECOMP_NARROW_HOR_MINUS");

    /* 3. widescreen_hud_anchor, minimap_at, present_crop, ultrawide hud_shift */
    boot(0.9f, ASPECT_UNLOCKED);
    proj[fvi] = orig_fovy; proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    frame = 600; scene = 600; title = false; menu = false; touch = false;
    widescreen_minimap_seen();
    eqf(widescreen_hud_anchor(-574, 320), 40.0f);
    eqf(widescreen_hud_anchor(-180, 320), 40.0f);  /* 15-heart / yellow heart row stays intact */
    eqf(widescreen_hud_anchor(-554, 200), 40.0f);  /* far-left Champion/Rune icon moves up */
    eqf(widescreen_hud_anchor(-350, 200), 0.0f);   /* 3D enemy HP bar stays locked to world */
    eqf(widescreen_hud_anchor(500, 320), 40.0f);
    eqf(widescreen_hud_anchor(0, 320), 0.0f);
    eqf(widescreen_hud_anchor(0, -235), 0.0f);
    eqf(widescreen_hud_anchor(515, -245), -40.0f);
    eqf(widescreen_hud_anchor(-300, -330), -40.0f);
    eqf(widescreen_hud_anchor(-300, -300), 0.0f);
    eqf(widescreen_hud_anchor_text(-300, -300), -40.0f);
    touch = true;
    eqf(widescreen_hud_anchor(515, -245), 40.0f);
    float mx = 0, my = 0, mr = 0;
    assert(widescreen_minimap_at(2560, 1600, &mx, &my, &mr));
    eqf(mx, 2310.0f); eqf(my, 230.0f); eqf(mr, 210.0f);
    touch = false;

    /* Suppress anchors during scope, band, talk, menu */
    widescreen_scope_seen(); eqf(widescreen_hud_anchor(-574, 320), 0.0f); frame += 4;
    widescreen_band_seen();  eqf(widescreen_hud_anchor(-574, 320), 0.0f); frame += 4;
    widescreen_talk_seen();  eqf(widescreen_hud_anchor(-574, 320), 0.0f); frame += 4;
    menu = true; eqf(widescreen_hud_anchor(-574, 320), 0.0f); menu = false; frame += 31; scene = frame;

    /* LARECOMP_NO_SCOPE_BARS=1 keeps widescreen_cropped() false during scope */
    setenv("LARECOMP_NO_SCOPE_BARS", "1", 1);
    widescreen_scope_seen();
    assert(!widescreen_cropped());
    unsetenv("LARECOMP_NO_SCOPE_BARS");

    /* present_crop: vertical letterbox on Title/Scope in 16:10 & 4:3; horizontal pillarbox in 20:9 */
    float crop[4]; double vx, vy, vw, vh;
    vx = 0; vy = 0; vw = 2560; vh = 1600;
    assert(!widescreen_cropped());
    widescreen_present_crop(frame, 0.9, crop, &vx, &vy, &vw, &vh);
    eqd(vx, 0); eqd(vy, 0); eqd(vw, 2560); eqd(vh, 1600); eqf(crop[3], 1.0f);
    title = true;
    assert(widescreen_cropped());
    widescreen_present_crop(frame, 0.9, crop, &vx, &vy, &vw, &vh);
    eqf(crop[0], 0.0f); eqf(crop[1], 0.05f); eqf(crop[2], 1.0f); eqf(crop[3], 0.9f);
    eqd(vy, 80.0); eqd(vh, 1440.0);
    vx = 0; vy = 0; vw = 3200; vh = 1440;
    widescreen_present_crop(frame, 1.25, crop, &vx, &vy, &vw, &vh);
    eqf(crop[0], 0.1f); eqf(crop[2], 0.8f); eqf(crop[3], 1.0f); eqd(vx, 320.0); eqd(vw, 2560.0);
    title = false;
    menu = true; assert(!widescreen_cropped()); menu = false; frame += 31; scene = frame;
    widescreen_scope_seen();
    assert(widescreen_cropped());
    vx = 0; vy = 0; vw = 2048; vh = 1536;
    widescreen_present_crop(frame, 0.75, crop, &vx, &vy, &vw, &vh);
    eqf(crop[1], 0.125f); eqf(crop[3], 0.75f); eqd(vy, 192.0); eqd(vh, 1152.0);
    frame += 4; scene = frame;

    /* 4. exec.c extracted narrow / guard-page / map / backdrop / scissor / ultrawide logic */
    float f[140];
    /* 4a. Guard-page OOB test: 64-byte text UBO at page boundary before PROT_NONE page must NEVER fault */
    long pg = sysconf(_SC_PAGESIZE);
    assert(pg >= 4096);
    uint8_t *mem = mmap(NULL, (size_t)pg * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED);
    assert(mprotect(mem + pg, (size_t)pg, PROT_NONE) == 0);
    float *short_text = (float *)(mem + pg - 64);
    short_text[0] = 1.0f; short_text[5] = -1.6f; short_text[14] = -1.0f; short_text[15] = 4.0f;
    short_text[3] = 0.0f; short_text[7] = -300.0f * 4.0f * 0.9f / 360.0f;
    g_cur_fs_hash = 0x28d0b545bd03bd61ull;
    assert(run_cbuf(short_text, 64, &target));  /* would SIGSEGV if tall_full_cover reads past 64 bytes */
    eqf(copied[7], -340.0f * 4.0f * 0.9f / 360.0f);
    /* Non-uniformly scaled text (sx != sy => !adapted && !fixed) still anchors under Vert+ and fits under Hor- */
    short_text[5] = -1.6f * 1.15f;
    short_text[7] = -300.0f * 4.0f * (1.6f * 1.15f / base) / 360.0f;
    assert(run_cbuf(short_text, 64, &target));
    eqf(copied[7], short_text[7] - 40.0f * 0.9f / 360.0f * 4.0f);
    munmap(mem, (size_t)pg * 2);
    g_cur_fs_hash = 0;

    /* Normal fitted Vert+ adapted pane with dy=0 -> no UBO rewrite */
    init_f_defaults(f, 1.6f); f[19] = 0; f[23] = 0;
    assert(!run_cbuf(f, sizeof f, &target));

    /* 4b. Vert+ adapted HUD pane (hearts) -> dy = +40 pushed into m[23] */
    init_f_defaults(f, 1.6f); f[19] = -574.0f; f[23] = 320.0f;
    assert(run_cbuf(f, sizeof f, &target)); eqf(copied[23], 360.0f); eqf(copied[5], 1.6f);

    /* 4d. Vert+ fixed (16:9 cam) UI pane -> m[4..7] *= 0.9 */
    init_f_defaults(f, base); f[19] = -574.0f; f[23] = 320.0f;
    assert(run_cbuf(f, sizeof f, &target)); eqf(copied[5], 1.6f); eqf(copied[23], 360.0f);

    /* 4e. Interior thin full-width bar (height 80, y=240..320), off-screen bars (+400..+480 / -480..-400),
       and negative-scale band rejected -> stay fitted! */
    menu = true; g_cur_fs_hash = 0x1111;
    init_f_defaults(f, 1.6f); f[19] = 0; f[23] = 280; f[132] = 1280; f[133] = 80; f[134] = -640; f[135] = 40;
    assert(!run_cbuf(f, sizeof f, &target));
    /* Completely off-screen top bar (y_lo=400 >= 359.5) stays untouched and does NOT set band_frame */
    menu = false; frame += 31; scene = frame;
    init_f_defaults(f, 1.6f); f[19] = 0; f[23] = 440; f[132] = 1280; f[133] = 80; f[134] = -640; f[135] = 40;
    assert(!run_cbuf(f, sizeof f, &target));
    init_f_defaults(f, 1.6f); f[19] = 0; f[23] = -440; f[132] = 1280; f[133] = 80; f[134] = -640; f[135] = 40;
    assert(!run_cbuf(f, sizeof f, &target));
    /* Negative f[21] in band_cand stays untouched */
    init_f_defaults(f, 1.6f); f[21] = -1.0f; f[19] = 0; f[23] = 360; f[132] = 1280; f[133] = 275; f[134] = -640; f[135] = 275;
    assert(!run_cbuf(f, sizeof f, &target));
    assert(fabsf(widescreen_hud_anchor(-574, 320) - 40.0f) < 1e-4f);  /* off-screen band did NOT poison band_frame */

    /* 4f. Covers (1280x720, sliding 4542x758, negative-scale flipped cover, pause_layer) stretch AND must NOT poison band_frame when in menu */
    menu = true;
    init_f_defaults(f, 1.6f); f[132] = 1280; f[133] = 720; f[134] = -640; f[135] = 360;
    assert(run_cbuf(f, sizeof f, &target)); eqf(copied[5], base);
    init_f_defaults(f, 1.6f); f[19] = -1700.0f; f[23] = -19.0f; f[132] = 4542; f[133] = 758; f[134] = -2295; f[135] = 379;
    assert(run_cbuf(f, sizeof f, &target)); eqf(copied[5], base);
    init_f_defaults(f, 1.6f); f[21] = -1.0f; f[132] = 1280; f[133] = 720; f[134] = -640; f[135] = 360;
    assert(run_cbuf(f, sizeof f, &target)); eqf(copied[5], base);
    /* Close menu on the very next frame: world HUD anchor activates immediately without 3-frame lag */
    menu = false; frame += 1; scene = frame;
    eqf(widescreen_hud_anchor(-574, 320), 40.0f);

    /* Scaled-down 1280x720 card (f[16]=0.5, f[21]=0.5) & inner 545-tall frame at y=-19 must NOT stretch */
    menu = true;
    init_f_defaults(f, 1.6f); f[16] = 0.5f; f[21] = 0.5f; f[132] = 1280; f[133] = 720; f[134] = -640; f[135] = 360;
    assert(!run_cbuf(f, sizeof f, &target));
    init_f_defaults(f, 1.6f); f[19] = 0; f[23] = -19.0f; f[132] = 1280; f[133] = 545; f[134] = -640; f[135] = 272.5f;
    assert(!run_cbuf(f, sizeof f, &target));
    /* Off-center backdrop must NOT match is_backdrop; centered backdrop matches */
    g_wide_backdrop = true; g_cur_fs_hash = 0xc94630386244616eull;
    init_f_defaults(f, 1.6f); f[19] = 100.0f; f[132] = 1280; f[133] = 400; f[134] = -640; f[135] = 200;
    assert(!run_cbuf(f, sizeof f, &target));
    init_f_defaults(f, 1.6f); f[19] = 0.0f;
    assert(run_cbuf(f, sizeof f, &target)); eqf(copied[5], base);
    g_wide_backdrop = false; g_cur_fs_hash = 0;

    /* 4f2. Title & Scope crop protection + Scope menu==false/cropped==false check + Loading bands */
    menu = false; title = true; frame += 31; scene = frame;
    init_f_defaults(f, 1.6f); f[16] = 0.75f; f[21] = 0.75f;
    f[132] = 1920; f[133] = 1080; f[134] = -960; f[135] = 540; g_cur_fs_hash = 0xd4fa32dabc35066cull;
    assert(!run_cbuf(f, sizeof f, &target));
    title = false;

    /* Even with LARECOMP_NO_SCOPE_BARS (cropped==false), menu==false keeps 1280x608 0xe91b1fb6 out of map_open */
    map_frame = 0;
    init_f_defaults(f, 1.6f); f[132] = 1280; f[133] = 608; f[134] = -640; f[135] = 304; g_cur_fs_hash = 0xe91b1fb6e1eb920bull;
    assert(!run_cbuf(f, sizeof f, &target));
    assert(map_frame == 0);
    g_cur_fs_hash = 0;

    /* Loading screen cream top band (1280x275 at y=360, f[135]=0.0, fixed) at g=0.9 and g=0.75 */
    for (int ti_idx = 0; ti_idx < 2; ti_idx++) {
        float tg = ti_idx == 0 ? 0.9f : 0.75f;
        widescreen_set_shape(tg);
        widescreen_set_factor(tg);
        init_f_defaults(f, base); f[19] = 0; f[23] = 360.0f; f[132] = 1280; f[133] = 275; f[134] = -640; f[135] = 0.0f;
        g_cur_fs_hash = 0xc94630386244616eull;
        assert(run_cbuf(f, sizeof f, &target));
        float top_y = copied[23] + copied[21] * 0.0f;
        float seam_y = copied[23] + copied[21] * (-275.0f);
        eqf(top_y * (copied[5] / base) / 360.0f, 1.0f);
        eqf(seam_y * (copied[5] / base) / 360.0f, 85.0f * tg / 360.0f);
    }
    /* Bottom-edge full-width band (1280x100 at y=-360, f[135]=100, adapted) at g=0.9 */
    widescreen_set_shape(0.9f); widescreen_set_factor(0.9f);
    init_f_defaults(f, 1.6f); f[19] = 0; f[23] = -360.0f; f[132] = 1280; f[133] = 100; f[134] = -640; f[135] = 100.0f;
    g_cur_fs_hash = 0xd4fa32dabc35066cull;
    assert(run_cbuf(f, sizeof f, &target));
    eqf((copied[23] + copied[21] * 0.0f) * (copied[5] / base) / 360.0f, -1.0f);
    eqf((copied[23] + copied[21] * 100.0f) * (copied[5] / base) / 360.0f, -260.0f * 0.9f / 360.0f);
    menu = true; g_cur_fs_hash = 0; frame += 4; scene = frame;

    /* 4g. Map terrain + marker + text + scissor under Vert+ */
    init_f_defaults(f, 1.6f); f[132] = 1280; f[133] = 608; f[134] = -640; f[135] = 304;
    g_cur_fs_hash = 0xe91b1fb6e1eb920bull;
    assert(run_cbuf(f, sizeof f, &target)); eqf(copied[5], base);
    g_cur_fs_hash = 0x2222;
    init_f_defaults(f, 1.6f); f[19] = 100; f[23] = 180;
    assert(run_cbuf(f, sizeof f, &target)); eqf(copied[23], 200.0f); eqf(copied[21], 1.0f);
    init_f_defaults(f, base); f[19] = 100; f[23] = 180;
    assert(run_cbuf(f, sizeof f, &target)); eqf(copied[5], 1.6f); eqf(copied[23], 200.0f);
    memset(f, 0, sizeof f); f[0] = 1.0f; f[5] = 1.6f; f[14] = -1.0f; f[15] = 2.0f; f[7] = 0.9f;
    assert(run_cbuf(f, 64, &target)); eqf(copied[7], 1.0f); eqf(copied[5], 1.6f);
    memset(f, 0, sizeof f); f[0] = 1.0f; f[5] = base; f[14] = -1.0f; f[15] = 2.0f; f[7] = 1.0f;
    assert(run_cbuf(f, 64, &target)); eqf(copied[5], 1.6f); eqf(copied[7], 1.0f);
    st.scissor[0][0] = 100; st.scissor[0][1] = 90; st.scissor[0][2] = 1720; st.scissor[0][3] = 900;
    VkRect2D sr = run_scissor_blocks();
    assert(sr.offset.y == 89 && sr.extent.height == 901);
    /* Centered map is opt-in. Check the fitted terrain, marker, scissor and final viewport
       at both the HONOR Magic V5 inner-screen size and 4:3. */
    const int displays[][2] = {{2352, 2172}, {2048, 1536}};
    for (int i = 0; i < 2; ++i) {
        double w = displays[i][0], h = displays[i][1], shape = w / h / base;
        widescreen_set_shape(shape); widescreen_set_factor(shape);
        g_cur_fs_hash = 0xe91b1fb6e1eb920bull;
        init_f_defaults(f, base * shape);
        f[132] = 1280; f[133] = 608; f[134] = -640; f[135] = 304;
        g_settings.center_world_map = false;
        assert(run_cbuf(f, sizeof f, &target)); eqf(copied[5], base);
        assert(!widescreen_cropped());
        g_settings.center_world_map = true;
        assert(!run_cbuf(f, sizeof f, &target));  /* terrain already fits */
        assert(widescreen_cropped());
        vx = vy = 0; vw = w; vh = h;
        widescreen_present_crop(frame, shape, crop, &vx, &vy, &vw, &vh);
        eqf(vw, w); eqf(vx, 0); eqf(vw / vh, base); eqf(2 * vy + vh, h);
        g_cur_fs_hash = 0x2222;
        init_f_defaults(f, base); f[19] = 100; f[23] = 180;
        assert(run_cbuf(f, sizeof f, &target));
        eqf(copied[23], 180); eqf(copied[5], base * shape);
        pass_w = (uint32_t)w; pass_h = (uint32_t)h;
        vp_sx = (float)w / target.width; vp_sy = (float)h / target.height;
        sr = run_scissor_blocks();
        double scale = w / target.width;
        assert(fabs(sr.offset.y - ((h - target.height * scale) / 2 + 90 * scale)) <= 1);
        assert(fabs(sr.extent.height - 900 * scale) < 2);
        frame += 4; scene = frame;
        assert(!widescreen_cropped());
    }
    pass_w = 1728; pass_h = 1080; vp_sx = 0.9f; vp_sy = 1.0f;
    g_settings.center_world_map = false;
    widescreen_set_shape(0.9f); widescreen_set_factor(0.9f);
    menu = false; map_frame = 0; frame += 31; scene = frame;
    touch = true; widescreen_minimap_seen();
    st.scissor[0][0] = 1625; st.scissor[0][1] = 800; st.scissor[0][2] = 200; st.scissor[0][3] = 200;
    sr = run_scissor_blocks();
    assert(sr.offset.y == 58 && sr.extent.height == 181);
    /* Clamping y < 0 works cleanly */
    st.scissor[0][1] = -20; st.scissor[0][3] = 100;
    sr = run_scissor_blocks();
    assert(sr.offset.y == 0);

    /* 4h. Hor- (LARECOMP_NARROW_HOR_MINUS=1) layout fit: adapted, fixed, condensed text, map, cover, band */
    setenv("LARECOMP_NARROW_HOR_MINUS", "1", 1);
    touch = false; widescreen_minimap_seen();
    init_f_defaults(f, 1.6f); f[19] = -574.0f; f[23] = 320.0f;
    assert(run_cbuf(f, sizeof f, &target));
    eqf(copied[0], 0.9f); eqf(copied[5], 1.44f); eqf(copied[23], 360.0f);
    init_f_defaults(f, base); f[19] = -574.0f; f[23] = 320.0f;
    assert(run_cbuf(f, sizeof f, &target));
    eqf(copied[0], 1.0f); eqf(copied[5], 1.6f); eqf(copied[23], 360.0f);
    memset(f, 0, sizeof f); f[0] = 1.0f; f[5] = -1.6f * 1.15f; f[14] = -1.0f; f[15] = 2.0f;
    f[7] = -300.0f * 2.0f / 360.0f;
    assert(run_cbuf(f, 64, &target));
    eqf(copied[0], 0.9f); eqf(copied[5], -1.6f * 1.15f * 0.9f);
    menu = true;
    init_f_defaults(f, 1.6f); f[132] = 1280; f[133] = 720; f[134] = -640; f[135] = 360;
    assert(run_cbuf(f, sizeof f, &target)); eqf(copied[0], 0.9f); eqf(copied[5], 1.6f);
    unsetenv("LARECOMP_NARROW_HOR_MINUS");

    /* 4i. 20:9 Ultrawide regression checks (cbuf_info + scissor_rect horizontal map expansion) */
    boot(1.25f, ASPECT_UNLOCKED);
    proj[fvi] = orig_fovy; proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    frame += 31; scene = frame; menu = false; touch = false;
    eqf(widescreen_hud_anchor(-574, 320), 0.0f);
    assert(widescreen_hud_shift(-574, 320) < -150.0f);
    init_f_defaults(f, base * 1.25f); f[19] = -574.0f; f[23] = 320.0f;
    assert(run_cbuf(f, sizeof f, &target));
    eqf(copied[19], -574.0f - 160.0f);
    menu = true; widescreen_map_seen();
    g_settings.center_world_map = true; assert(!widescreen_cropped());
    target.width = 1920; target.height = 1080; pass_w = 2400; pass_h = 1080; vp_sx = 1.25f; vp_sy = 1.0f; vp_ox = 0; vp_oy = 0;
    st.scissor[0][0] = 160; st.scissor[0][1] = 100; st.scissor[0][2] = 1600; st.scissor[0][3] = 880;
    sr = run_scissor_blocks();
    assert(sr.offset.x == 200 && sr.extent.width == 2000);
    g_settings.center_world_map = false;

    /* 4j. draw_() narrow tall_ui viewport bypass & g_wide_backdrop scale-divergence check */
    boot(0.9f, ASPECT_UNLOCKED);
    proj[fvi] = orig_fovy; proj[ai] = base;
    rt_persp_update((uint64_t)(uintptr_t)proj);
    target.width = 1920; target.height = 1080; pass_w = 1920; pass_h = 1200;
    mock_pass_sx = 1.0f; mock_pass_sy = 1.0f / 0.9f;
    init_f_defaults(f, base * 0.9f);
    eval_tall_ui_viewport(false, f, sizeof f);
    eqf(vp_sx, 1.0f); eqf(vp_sy, 1.0f / 0.9f); eqf(vp_ox, 0.0f); eqf(vp_oy, 0.0f);
    f[14] = 0.0f;  /* non-layout draw still gets centered square-pixel viewport */
    eval_tall_ui_viewport(false, f, sizeof f);
    eqf(vp_sx, 1.0f); eqf(vp_sy, 1.0f); eqf(vp_ox, 0.0f); eqf(vp_oy, 60.0f);
    mock_tex_sx = 1.25f; mock_tex_sy = 1.0f;
    eval_wide_backdrop();
    assert(g_wide_backdrop);
    mock_tex_sx = 1.0f; mock_tex_sy = 1.0f / 0.9f;
    eval_wide_backdrop();
    assert(g_wide_backdrop);
    mock_tex_sx = 1.0f; mock_tex_sy = 1.0f;
    eval_wide_backdrop();
    assert(!g_wide_backdrop);
    g_wide_backdrop = false;

    puts("PASS: output_scale, device shape, widescreen Vert+/Hor-/20:9/16:9, PROT_NONE guard, map/band/cover/scissor tests");
    return 0;
}
'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wsl', action='store_true')
    parser.add_argument('--negative-control', choices=['camera', 'ui', 'anchor', 'map', 'center_map', 'crop', 'band', 'crop_title', 'oob_guard', 'tall_ui', 'backdrop'])
    args = parser.parse_args()

    out_scale_src = (ROOT / 'runtime/nvn/output_scale.c').read_text(encoding='utf-8')
    out_scale_src = re.sub(r'^#include "[^"]+"\s*$', '', out_scale_src, flags=re.M)
    out_scale_src = 'typedef struct { double sx, sy, scene_sx, scene_sy, shape, vx, vy, vw, vh; } OutScale;\n' + out_scale_src

    dev_src = (ROOT / 'runtime/nvn/vulkan/device.c').read_text(encoding='utf-8')
    d_start = dev_src.index('        static bool wt_init;')
    d_end = dev_src.index('        {\n            static double last_dw, last_dh;', d_start)
    dev_guard = dev_src[d_start:d_end].replace('static bool wt_init;', 'bool wt_init = false;')
    dev_func = f'static void eval_device_shape(double dw, double dh) {{\n{dev_guard}\n}}\n'

    ws_src = (ROOT / 'runtime/hle/widescreen.c').read_text(encoding='utf-8')
    ws_src = re.sub(r'^#include\s+[<"].+[>"]\s*$', '', ws_src, flags=re.M)

    ex = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text(encoding='utf-8')
    wh_start = ex.index('static float wide_half(void)')
    wh_end = ex.index('// LARECOMP_LOG_PANEFIX=1', wh_start)
    wide_helpers = ex[wh_start:wh_end]

    sc1_start = ex.index('    {   // The minimap group up top')
    sc1_end = ex.index('    if (sx != 1 || sy != 1 || ox != 0 || oy != 0) {', sc1_start)
    sc2_end = ex.index('    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};', sc1_end)
    scissor_block = ex[sc1_start:sc2_end]

    bd_start = ex.index('            extern bool widescreen_ui_centred(void);\n            g_wide_backdrop = false;')
    bd_end = ex.index('            // A menu layout\'s pane or text', bd_start)
    backdrop_block = ex[bd_start:bd_end]

    vp_start = ex.index('        extern bool widescreen_ui_centred(void);\n        extern float widescreen_factor(void);')
    vp_end = ex.index('    }\n    {   // camera-attached effects sized for 16:9', vp_start)
    viewport_block = ex[vp_start:vp_end]

    cb_start = ex.index('        if (t0 && vk_texture_scale_group(t0) == RS_OUT && f[12] == 0 && f[13] == 0 && f[14] == -1.0f && f[1] == 0 && f[2] == 0 &&')
    cb_end = ex.index('\n    }\n#ifdef __ANDROID__', cb_start)
    cb_block = ex[cb_start:cb_end]
    cb_block = cb_block.replace('return ubo_span(c, size);', '{ ubo_span(c, size); return true; }')

    source = (
        PRELUDE
        + out_scale_src + '\n'
        + ws_src + '\n'
        + dev_func + '\n'
        + UI_STUBS
        + wide_helpers
        + BACKDROP_HEAD + backdrop_block + '}\n'
        + VIEWPORT_HEAD + viewport_block + '}\n'
        + EXEC_WRAPPERS + cb_block + EXEC_WRAPPER_MID
        + SCISSOR_HEAD + scissor_block + '    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};\n}\n'
        + MAIN
    )

    if args.negative_control == 'camera':
        old = '((shape >= NARROW_MIN && shape < 0.999f) || shape > 1.001f)'
        assert old in source
        source = source.replace(old, '(shape > 1.001f)', 1)
    elif args.negative_control == 'ui':
        old = 'for (int i = 4; i < 8; i++) m[i] *= g;'
        assert old in source
        source = source.replace(old, '/* negative control */')
    elif args.negative_control == 'anchor':
        old = 'float e = 360.0f * (1.0f / wide - 1.0f);'
        assert old in source
        source = source.replace(old, 'float e = 0.0f;', 1)
    elif args.negative_control == 'map':
        old = 'if (vert_plus) { for (int i = 4; i < 8; i++) m[i] /= g; }'
        assert old in source
        source = source.replace(old, 'if (vert_plus) { /* negative control */ }', 1)
    elif args.negative_control == 'center_map':
        old = 'g_settings.center_world_map && wide < 0.999f'
        assert old in source
        source = source.replace(old, 'g_settings.center_world_map && wide < 0.5f', 1)
    elif args.negative_control == 'crop':
        old = 'crop[3] = (float)shape, crop[1] = (1.0f - crop[3]) * 0.5f;'
        assert old in source
        source = source.replace(old, '/* negative control */', 1)
    elif args.negative_control == 'band':
        old = 'bool top_band = band_cand && isfinite(y_lo) && isfinite(y_hi) && y_hi >= 359.5f &&'
        assert old in source
        source = source.replace(old, 'bool top_band = false &&', 1)
    elif args.negative_control == 'crop_title':
        old = 'scope || framepace_on_title()'
        assert old in source
        source = source.replace(old, 'scope', 1)
    elif args.negative_control == 'oob_guard':
        old = 'bool is_cover = !cropped && pane && tall_full_cover(f);'
        assert old in source
        source = source.replace(old, 'bool is_cover = !cropped && tall_full_cover(f);', 1)
    elif args.negative_control == 'tall_ui':
        old = 'if (!composite && !tall_ui) {'
        assert old in source
        source = source.replace(old, 'if (!composite && ((void)tall_ui, true)) {', 1)
    elif args.negative_control == 'backdrop':
        old = 'g_wide_backdrop = sx > sy * 1.01f || sx < sy * 0.99f;'
        assert old in source
        source = source.replace(old, 'g_wide_backdrop = sx > sy * 1.01f;', 1)

    with tempfile.TemporaryDirectory(prefix='fullscreen-narrow-') as directory:
        binary = Path(directory) / 'test'
        if args.wsl:
            target = '/mnt/' + binary.drive[0].lower() + binary.as_posix()[2:]
            compiler, cmd = ['wsl', 'gcc'], ['wsl', target]
        else:
            target = str(binary)
            compiler, cmd = shlex.split(os.environ.get('CC', 'cc')), [target]
        subprocess.run(
            compiler + ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-lm', '-lpthread', '-o', target],
            input=source.encode('utf-8'),
            check=True,
        )
        res = subprocess.run(cmd)
        if args.negative_control:
            assert res.returncode != 0, f'Negative control not detected: {args.negative_control}'
            print('PASS: detected negative control:', args.negative_control)
        else:
            res.check_returncode()
            for wt in ('-0.5', '0.5', '0.5625', '0.75', '0.996', '1.25', 'inf'):
                subprocess.run(cmd + ['--wide-test', wt], check=True)
            print('PASS: --wide-test -0.5, 0.5, 0.5625, 0.75, 0.996, 1.25, inf')

if __name__ == '__main__':
    main()
