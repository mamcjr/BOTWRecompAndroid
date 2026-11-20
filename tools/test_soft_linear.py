#!/usr/bin/env python3
"""Verify soft_linear production C helpers (wasm32 + Node), GLSL/SPIR-V compilation (NDK glslc), and 1/256 bilinear math."""
import math
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'soft-linear-tests'
OUT.mkdir(parents=True, exist_ok=True)
BIN_DIR = Path(sys.argv[1]) if len(sys.argv) > 1 else None
if BIN_DIR is None:
    raise SystemExit('usage: test_soft_linear.py <NDK llvm bin directory>')


def find_glslc(bin_dir: Path) -> Path:
    # <ndk>/toolchains/llvm/prebuilt/<host>/bin -> <ndk>/shader-tools/<host>/glslc.exe
    host = bin_dir.parent.name
    ndk_root = bin_dir.parents[4]
    cand = ndk_root / 'shader-tools' / host / ('glslc.exe' if sys.platform == 'win32' else 'glslc')
    if cand.is_file():
        return cand
    raise FileNotFoundError(f'glslc not found at {cand}')


def extract(text: str, start_marker: str, end_marker: str) -> str:
    s = text.index(start_marker)
    e = text.index(end_marker, s)
    return text[s:e]


def run_wasm_c_checks():
    textures_src = (ROOT / 'runtime/nvn/vulkan/textures.c').read_text()
    exec_src = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text()

    resolve_fn = extract(textures_src, 'static VkFormat resolve_tex_format(', '\nstatic void create_image(')
    view_fn = extract(textures_src, 'static VkFormat view_format(', '\nVkImageView vk_view(')
    is_int_fn = extract(textures_src, 'bool vk_format_is_int(', '\nstatic _Atomic int g_any_no_linear')
    any_fn = extract(textures_src, 'static _Atomic int g_any_no_linear;', '\n// A format the GPU')
    mask_fn = extract(exec_src, 'static void stage_fill_soft_linear(', '\nstatic void texture_info(')
    tex_info_fn = extract(exec_src, 'static void texture_info(', '\nstatic bool is_ubo(')
    assert 'static _Atomic int g_any_no_linear;' in textures_src
    assert 'VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT' in textures_src
    assert 'if (!vk_format_is_int(f)) atomic_fetch_or_explicit(&g_any_no_linear, 3, memory_order_relaxed);' in textures_src
    assert 'memo[ms].tfmt = tfmt;' in tex_info_fn and 'vk_sampled_format(te->tex' not in tex_info_fn
    assert 'if (sl_state == 1) { vkey = vk2; fkey = fk2; pr = sl; }' in exec_src
    assert 'if (sl_state == 1) cp = sl;' in exec_src
    assert '(!pr || atomic_load(&pr->state) == 2)' in exec_src

    c_source = r'''
#define __ANDROID__ 1
#define VK_NO_PROTOTYPES 1
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#include "maxwell/shader.h"

void *memset(void *p, int c, __SIZE_TYPE__ n) {
    unsigned char *b = p;
    for (__SIZE_TYPE__ i = 0; i < n; i++) b[i] = (unsigned char)c;
    return p;
}

typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;
typedef struct { uint32_t bpb, bw, bh; } FormatInfo;
typedef struct { const FormatInfo *fmt; } TexLayout;
typedef struct {
    TexLayout layout;
    uint32_t format, width, height;
    void *backend;
} NvnTexture;
typedef struct {
    uint16_t min_layer, num_layers, format;
    uint8_t setmask, target, ds_mode, base_level, num_levels;
} NvnTextureView;
typedef struct {
    NvnTexture *tex;
    NvnTextureView view;
    bool has_view;
} NvnTexEntry;
typedef struct {
    uint32_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
} NvnSamplerDesc;
typedef struct {
    int count;
    void *entries;
} NvnDescPool;
typedef struct {
    VkFormat format;
    VkFormat bc_vf;
    VkImageAspectFlags aspect;
    bool fixed_fmt;
    VkFormat view_alt;
} TexBackend;

static struct { NvnDescPool *tpool, *spool; } st;
static bool mock_filters_linear_91 = false;
static bool mock_bc_supported = true;
static bool mock_any_no_linear = true;
static uint64_t mock_handles[64];

static const char *getenv(const char *k) { (void)k; return 0; }
#define LOG(...) ((void)0)

static VkFmt fmt_of(uint32_t nvn_fmt) {
    switch (nvn_fmt) {
    case 0x25: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 0};
    case 0x2a: return (VkFmt){VK_FORMAT_R16G16B16A16_UNORM, 0, 0};
    case 0x2c: return (VkFmt){VK_FORMAT_R16G16B16A16_UINT, 0, 0};
    case 0x42: return (VkFmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0, 0};
    default:   return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 0};
    }
}
static VkFormat vk_format(uint32_t nvn_fmt) { return fmt_of(nvn_fmt).vf; }
static bool is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
static VkFormat bc_target(VkFormat bc, uint32_t *bpp) { *bpp = 4; (void)bc; return VK_FORMAT_R8G8B8A8_UNORM; }
static bool renderer_format_is_depth(uint32_t f) { (void)f; return false; }
static VkImageAspectFlags aspect_of(uint32_t f) { (void)f; return VK_IMAGE_ASPECT_COLOR_BIT; }
static int naliases = 0;
static NvnTexture *alias_read(NvnTexture *t) { return t; }
static bool format_supports(VkFormat f, VkFormatFeatureFlags want) {
    if ((want & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && f == VK_FORMAT_R16G16B16A16_UNORM)
        return mock_filters_linear_91;
    if (is_bc(f)) return mock_bc_supported;
    return true;
}
#define vk_any_format_no_linear vk_any_format_no_linear_prod
''' + '\n' + any_fn + r'''
#undef vk_any_format_no_linear
bool vk_any_format_no_linear(void) { return mock_any_no_linear; }
bool vk_format_filters_linear(VkFormat f) {
    if (f == VK_FORMAT_R16G16B16A16_UNORM) return mock_filters_linear_91;
    if (f == VK_FORMAT_R32_SFLOAT) return false;
    if (f == VK_FORMAT_R16G16B16A16_UINT || f == VK_FORMAT_R32_SINT || f == VK_FORMAT_S8_UINT) return false;
    return f != VK_FORMAT_UNDEFINED;
}
static uint64_t shader_texture_handle(int stage, const ShaderTexture *t_) {
    (void)stage;
    uint32_t idx = (t_->handle_offset - 0x20u) / 8u;
    return idx < 64u ? mock_handles[idx] : 0;
}
''' + '\n' + resolve_fn + '\n' + view_fn + '\n' + is_int_fn + '\n' + mask_fn + r'''

_Static_assert(sizeof(ShaderKey) == 104, "ShaderKey must be 104 bytes with explicit padding");
_Static_assert(offsetof(ShaderKey, pad_) == 50, "ShaderKey pad_ offset");
_Static_assert(offsetof(ShaderKey, soft_linear) == 56, "ShaderKey soft_linear offset");
_Static_assert(offsetof(ShaderKey, soft_linear_mip) == 64, "ShaderKey soft_linear_mip offset");
_Static_assert(offsetof(ShaderKey, soft_linear_rep_u) == 72, "ShaderKey soft_linear_rep_u offset");
_Static_assert(offsetof(ShaderKey, soft_linear_rep_v) == 80, "ShaderKey soft_linear_rep_v offset");
_Static_assert(offsetof(ShaderKey, local_read_slot) == 88, "ShaderKey local_read_slot offset");
_Static_assert(offsetof(ShaderKey, local_read_att) == 96, "ShaderKey local_read_att offset");

static int cases;
#define CHECK(cond) do { cases++; if (!(cond)) return -cases; } while (0)

int check(void) {
    cases = 0;

    // 1. ShaderKey explicit zero-padding guarantees deterministic memcmp/hashing
    ShaderKey k0 = {0}, k1 = {0};
    k1.soft_linear = 1ULL;
    k1.soft_linear_mip = 1ULL;
    k1.soft_linear_rep_u = 1ULL;
    k1.soft_linear_rep_v = 1ULL;
    const uint8_t *b0 = (const uint8_t *)&k0;
    for (int i = 0; i < 6; i++) CHECK(k0.pad_[i] == 0 && b0[50 + i] == 0);
    CHECK(sizeof(ShaderKey) == 104);

    // 2. vk_sampled_format: backend present vs absent, views, fixed_fmt, BC fallback
    FormatInfo bc_fi = {.bpb = 8, .bw = 4, .bh = 4};
    FormatInfo r16_fi = {.bpb = 8, .bw = 1, .bh = 1};
    NvnTexture t_r16 = {.layout = {&r16_fi}, .format = 0x2a, .width = 64, .height = 64, .backend = 0};
    CHECK(vk_sampled_format(&t_r16, 0, false) == VK_FORMAT_R16G16B16A16_UNORM);

    // Non-backend texture with a format view override
    NvnTexture t_rgba8 = {.layout = {&r16_fi}, .format = 0x25, .width = 64, .height = 64, .backend = 0};
    NvnTextureView v_r16 = {.format = 0x2a, .setmask = 4};
    CHECK(vk_sampled_format(&t_rgba8, &v_r16, true) == VK_FORMAT_R16G16B16A16_UNORM);
    CHECK(vk_sampled_format(&t_rgba8, &v_r16, false) == VK_FORMAT_R8G8B8A8_UNORM);

    // Backend present with fixed_fmt blocking view format override
    TexBackend b_fixed = {.format = VK_FORMAT_R8G8B8A8_UNORM, .bc_vf = 0, .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                          .fixed_fmt = true, .view_alt = VK_FORMAT_R8G8B8A8_SRGB};
    t_rgba8.backend = &b_fixed;
    CHECK(vk_sampled_format(&t_rgba8, &v_r16, true) == VK_FORMAT_R8G8B8A8_UNORM);

    // CPU-decoded BC fallback (!backend) maps BC view to RGBA8
    mock_bc_supported = false;
    NvnTexture t_bc = {.layout = {&bc_fi}, .format = 0x42, .width = 64, .height = 64, .backend = 0};
    NvnTextureView v_bc = {.format = 0x42, .setmask = 4};
    CHECK(vk_sampled_format(&t_bc, &v_bc, true) == VK_FORMAT_R8G8B8A8_UNORM);
    mock_bc_supported = true;

    // 3. vk_format_is_int checks
    CHECK(vk_format_is_int(VK_FORMAT_R16G16B16A16_UINT));
    CHECK(vk_format_is_int(VK_FORMAT_R32_SINT));
    CHECK(vk_format_is_int(VK_FORMAT_S8_UINT));
    CHECK(!vk_format_is_int(VK_FORMAT_R16G16B16A16_UNORM));
    CHECK(!vk_format_is_int(VK_FORMAT_R8G8B8A8_UNORM));

    // 4. stage_soft_linear_mask checks across slots, filter modes, wrap modes, mip flags, high slots >= 32
    NvnTexEntry tex_entries[8] = {0};
    tex_entries[0].tex = &t_r16;
    TexBackend b_r8 = {.format = VK_FORMAT_R8G8B8A8_UNORM, .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
    NvnTexture t_r8_b = {.layout = {&r16_fi}, .format = 0x25, .backend = &b_r8};
    tex_entries[1].tex = &t_r8_b;
    TexBackend b_int = {.format = VK_FORMAT_R16G16B16A16_UINT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
    NvnTexture t_int = {.layout = {&r16_fi}, .format = 0x2c, .backend = &b_int};
    tex_entries[2].tex = &t_int;
    TexBackend b_f32 = {.format = VK_FORMAT_R32_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
    NvnTexture t_f32 = {.layout = {&r16_fi}, .format = 0x2a, .backend = &b_f32};
    tex_entries[3].tex = &t_f32;
    // t_r8 with a format-override view to 0x2a (UNORM16, unsupported linear filter)
    TexBackend b_r8_mut = {.format = VK_FORMAT_R8G8B8A8_UNORM, .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .fixed_fmt = false};
    NvnTexture t_r8_mut = {.layout = {&r16_fi}, .format = 0x25, .backend = &b_r8_mut};
    tex_entries[4].tex = &t_r8_mut;
    tex_entries[4].view = v_r16;
    tex_entries[4].has_view = true;

    NvnSamplerDesc smp_entries[10] = {0};
    smp_entries[0] = (NvnSamplerDesc){.min_filter = 3, .mag_filter = 1, .wrap = {7, 7, 0}};  // CLAMP_TO_EDGE, mip
    smp_entries[1] = (NvnSamplerDesc){.min_filter = 0, .mag_filter = 0, .wrap = {7, 7, 0}};  // pure nearest
    smp_entries[2] = (NvnSamplerDesc){.min_filter = 4, .mag_filter = 0, .wrap = {7, 7, 0}};  // nearest spatial, linear mip
    smp_entries[3] = (NvnSamplerDesc){.min_filter = 5, .mag_filter = 0, .wrap = {0, 7, 0}};  // CLAMP/CLAMP_TO_EDGE, mip
    smp_entries[4] = (NvnSamplerDesc){.min_filter = 0, .mag_filter = 1, .wrap = {7, 7, 0}};  // mag linear only, NO mip
    smp_entries[5] = (NvnSamplerDesc){.min_filter = 3, .mag_filter = 1, .wrap = {1, 7, 0}};  // REPEAT U, CLAMP_TO_EDGE V
    smp_entries[6] = (NvnSamplerDesc){.min_filter = 3, .mag_filter = 1, .wrap = {1, 1, 0}};  // REPEAT U & V (toroidal clipmap)
    smp_entries[7] = (NvnSamplerDesc){.min_filter = 3, .mag_filter = 1, .wrap = {7, 6, 0}};  // MIRRORED_REPEAT V -> OFF
    smp_entries[8] = (NvnSamplerDesc){.min_filter = 3, .mag_filter = 1, .wrap = {5, 7, 0}};  // CLAMP_TO_BORDER U -> OFF

    NvnDescPool tp = {.count = 8, .entries = tex_entries};
    NvnDescPool sp = {.count = 10, .entries = smp_entries};
    st.tpool = &tp;
    st.spool = &sp;

    ShaderInfo si = {0};
    #define MKH(tic, tsc) ((1ULL << 32) | ((uint64_t)(tsc) << 20) | (uint64_t)(tic))
    // Slot 0: 2D f0x2a + linear mip + CLAMP_TO_EDGE -> ENABLED + mip
    si.tex[0] = (ShaderTexture){.handle_offset = 0x20, .type = TEXTYPE_2D, .sampled = true};
    mock_handles[0] = MKH(0, 0);
    // Slot 1: 2D RGBA8 + linear sampler -> OFF (filters natively)
    si.tex[1] = (ShaderTexture){.handle_offset = 0x28, .type = TEXTYPE_2D, .sampled = true};
    mock_handles[1] = MKH(1, 0);
    // Slot 2: 2D UINT + linear sampler -> OFF (int format)
    si.tex[2] = (ShaderTexture){.handle_offset = 0x30, .type = TEXTYPE_2D, .sampled = true};
    mock_handles[2] = MKH(2, 0);
    // Slot 3: 2D f0x2a, NOT sampled (texelFetch-only) -> OFF
    si.tex[3] = (ShaderTexture){.handle_offset = 0x38, .type = TEXTYPE_2D, .sampled = false};
    mock_handles[3] = MKH(0, 0);
    // Slot 4: 2D f0x2a, shadow=true -> OFF
    si.tex[4] = (ShaderTexture){.handle_offset = 0x40, .type = TEXTYPE_2D, .shadow = true, .sampled = true};
    mock_handles[4] = MKH(0, 0);
    // Slot 5: 2D f0x2a, is_image=true -> OFF
    si.tex[5] = (ShaderTexture){.handle_offset = 0x48, .type = TEXTYPE_2D, .is_image = true, .sampled = true};
    mock_handles[5] = MKH(0, 0);
    // Slot 6: 2D f0x2a + pure nearest -> OFF
    si.tex[6] = (ShaderTexture){.handle_offset = 0x50, .type = TEXTYPE_2D, .sampled = true};
    mock_handles[6] = MKH(0, 1);
    // Slot 7: 3D R32F + linear sampler -> OFF (only 2D & 2D_ARRAY supported)
    si.tex[7] = (ShaderTexture){.handle_offset = 0x58, .type = TEXTYPE_3D, .sampled = true};
    mock_handles[7] = MKH(3, 0);
    // Slot 8: 2D_ARRAY R32F + mag=1/min=0 -> ENABLED, non-mip (mip bit 0!)
    si.tex[8] = (ShaderTexture){.handle_offset = 0x60, .type = TEXTYPE_2D_ARRAY, .sampled = true};
    mock_handles[8] = MKH(3, 4);
    // Slot 9: 2D RGBA8-view-R16 + min=5 -> ENABLED (via view override!) + mip
    si.tex[9] = (ShaderTexture){.handle_offset = 0x68, .type = TEXTYPE_2D, .sampled = true};
    mock_handles[9] = MKH(4, 3);
    // Slot 10: 2D f0x2a + mag=0/min=4 (nearest spatial, linear mip) -> OFF
    si.tex[10] = (ShaderTexture){.handle_offset = 0x70, .type = TEXTYPE_2D, .sampled = true};
    mock_handles[10] = MKH(0, 2);
    // Slot 11: 2D f0x2a + REPEAT U / CLAMP_TO_EDGE V -> ENABLED + mip + rep_u
    si.tex[11] = (ShaderTexture){.handle_offset = 0x78, .type = TEXTYPE_2D, .sampled = true};
    mock_handles[11] = MKH(0, 5);
    // Slot 12: 2D f0x2a + REPEAT U & V -> ENABLED + mip + rep_u + rep_v
    si.tex[12] = (ShaderTexture){.handle_offset = 0x80, .type = TEXTYPE_2D, .sampled = true};
    mock_handles[12] = MKH(0, 6);
    // Slot 13: 2D f0x2a + MIRRORED_REPEAT V wrap -> OFF
    si.tex[13] = (ShaderTexture){.handle_offset = 0x88, .type = TEXTYPE_2D, .sampled = true};
    mock_handles[13] = MKH(0, 7);
    // Slot 14: 2D f0x2a + CLAMP_TO_BORDER U wrap -> OFF
    si.tex[14] = (ShaderTexture){.handle_offset = 0x90, .type = TEXTYPE_2D, .sampled = true};
    mock_handles[14] = MKH(0, 8);
    // Slot 33: high bit >= 32 -> ENABLED + mip
    si.tex[33] = (ShaderTexture){.handle_offset = 0x20 + 33 * 8, .type = TEXTYPE_2D_ARRAY, .sampled = true};
    mock_handles[33] = MKH(3, 0);
    si.ntex = 34;

    ShaderKey sk = {.soft_linear = ~0ULL, .soft_linear_mip = ~0ULL, .soft_linear_rep_u = ~0ULL, .soft_linear_rep_v = ~0ULL};
    mock_any_no_linear = false;
    stage_fill_soft_linear(1, &si, &sk);
    CHECK(!sk.soft_linear && !sk.soft_linear_mip && !sk.soft_linear_rep_u && !sk.soft_linear_rep_v);
    mock_any_no_linear = true;

    stage_fill_soft_linear(1, &si, &sk);
    uint64_t want = (1ULL << 0) | (1ULL << 8) | (1ULL << 9) | (1ULL << 11) | (1ULL << 12) | (1ULL << 33);
    uint64_t want_mip = (1ULL << 0) | (1ULL << 9) | (1ULL << 11) | (1ULL << 12) | (1ULL << 33);
    CHECK(sk.soft_linear == want && sk.soft_linear_mip == want_mip &&
          sk.soft_linear_rep_u == ((1ULL << 11) | (1ULL << 12)) && sk.soft_linear_rep_v == (1ULL << 12));

    // Adreno simulation: vk_format_filters_linear(R16G16B16A16_UNORM) == true -> slots 0, 9, 11, 12 drop out
    mock_filters_linear_91 = true;
    stage_fill_soft_linear(1, &si, &sk);
    CHECK(sk.soft_linear == ((1ULL << 8) | (1ULL << 33)) && sk.soft_linear_mip == (1ULL << 33) &&
          !sk.soft_linear_rep_u && !sk.soft_linear_rep_v);

    // 5. vk_any_format_no_linear atomic_fetch_or race safety
    atomic_store_explicit(&g_any_no_linear, 0, memory_order_relaxed);
    CHECK(!vk_any_format_no_linear_prod() && atomic_load_explicit(&g_any_no_linear, memory_order_relaxed) == 1);
    atomic_fetch_or_explicit(&g_any_no_linear, 3, memory_order_relaxed);
    atomic_fetch_or_explicit(&g_any_no_linear, 1, memory_order_relaxed);
    CHECK(vk_any_format_no_linear_prod() && atomic_load_explicit(&g_any_no_linear, memory_order_relaxed) == 3);
    atomic_store_explicit(&g_any_no_linear, 0, memory_order_relaxed);
    mock_filters_linear_91 = false;
    CHECK(vk_any_format_no_linear_prod() && atomic_load_explicit(&g_any_no_linear, memory_order_relaxed) == 3);

    return cases;
}
'''
    c_path = OUT / 'test_c.c'
    o_path = OUT / 'test_c.o'
    wasm_path = OUT / 'test_c.wasm'
    js_path = OUT / 'run_c.cjs'
    c_path.write_text(c_source)

    clang = str(BIN_DIR / ('clang.exe' if sys.platform == 'win32' else 'clang'))
    lld = str(BIN_DIR / ('ld.lld.exe' if sys.platform == 'win32' else 'ld.lld'))
    subprocess.run([clang, '--target=wasm32', '-ffreestanding', '-O2', '-nostdlib',
                    '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'runtime/nvn'),
                    '-I', str(ROOT / 'android/deps/out/include'),
                    '-c', str(c_path), '-o', str(o_path)], check=True)
    subprocess.run([lld, '-flavor', 'wasm', '--no-entry', '--export=check',
                    str(o_path), '-o', str(wasm_path)], check=True)
    js_path.write_text(r'''
const fs = require('fs');
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({instance}) => {
  const cases = instance.exports.check();
  if (cases < 0) throw new Error(`soft_linear C check ${-cases} failed`);
  console.log(`${cases} soft_linear C checks passed (production C in wasm32)`);
}).catch(e => { console.error(e); process.exit(1); });
''')
    subprocess.run(['node', str(js_path), str(wasm_path)], check=True)


def run_glslc_checks():
    glslc = find_glslc(BIN_DIR)
    translate_src = (ROOT / 'runtime/nvn/maxwell/translate.c').read_text()
    for needle in (
        'int ml = max(textureQueryLevels(t%d) - 1, 0);',
        'int il = int(clamp(floor(clamp(lod, 0.0f, float(ml)) + 0.5f), 0.0f, float(ml)));',
        '    int il = 0;',
        'vec2 sz = max(vec2(tsz.xy), vec2(1.0f));',
        'vec2 tc = floor((wuv * sz - 0.5f) * 256.0f + 0.5f) / 256.0f;',
        'ivec2 p1 = clamp((p0 + 1) %% isz, ivec2(0), isz - 1);',
        'ivec2 p1 = clamp(p0 + 1, ivec2(0), isz - 1);',
        'textureQueryLod(t%d, uv).x',
        'vec4 slin%d_i(vec2 uv%s) { return slin%d(uv%s, 0.0f); }',
    ):
        if needle not in translate_src:
            raise AssertionError(f'Missing expected slin emission in translate.c: {needle}')
    if 'textureQueryLod(t%d, uv).y' in translate_src:
        raise AssertionError('Found unclamped textureQueryLod().y instead of clamped .x in translate.c')

    def slin_helper(slot: int, arr: bool, is_frag: bool, mip: bool = True, rep_u: bool = False, rep_v: bool = False) -> str:
        tsize = f'ivec3 tsize{slot}(int l) {{ return textureSize(t{slot}, l); }}' if arr else f'ivec3 tsize{slot}(int l) {{ return ivec3(textureSize(t{slot}, l), 1); }}'
        la = ', int lyr' if arr else ''
        lp = ', lyr' if arr else ''
        if arr:
            fetch = (
                f'    int cl = clamp(lyr, 0, max(tsz.z - 1, 0));\n'
                f'    vec4 s00 = texelFetch(t{slot}, ivec3(p0, cl), il);\n'
                f'    vec4 s10 = texelFetch(t{slot}, ivec3(p1.x, p0.y, cl), il);\n'
                f'    vec4 s01 = texelFetch(t{slot}, ivec3(p0.x, p1.y, cl), il);\n'
                f'    vec4 s11 = texelFetch(t{slot}, ivec3(p1, cl), il);\n'
            )
        else:
            fetch = (
                f'    vec4 s00 = texelFetch(t{slot}, p0, il);\n'
                f'    vec4 s10 = texelFetch(t{slot}, ivec2(p1.x, p0.y), il);\n'
                f'    vec4 s01 = texelFetch(t{slot}, ivec2(p0.x, p1.y), il);\n'
                f'    vec4 s11 = texelFetch(t{slot}, p1, il);\n'
            )
        if mip:
            lod_sel = (
                f'    int ml = max(textureQueryLevels(t{slot}) - 1, 0);\n'
                f'    int il = int(clamp(floor(clamp(lod, 0.0f, float(ml)) + 0.5f), 0.0f, float(ml)));\n'
            )
        else:
            lod_sel = '    int il = 0;\n'
        wuv = 'fract(uv)' if (rep_u and rep_v) else ('vec2(fract(uv.x), uv.y)' if rep_u else ('vec2(uv.x, fract(uv.y))' if rep_v else 'uv'))
        if rep_u and rep_v:
            wrap_idx = '    ivec2 p1 = clamp((p0 + 1) % isz, ivec2(0), isz - 1);\n    p0 = clamp((p0 + isz) % isz, ivec2(0), isz - 1);\n'
        elif rep_u:
            wrap_idx = '    ivec2 p1 = clamp(ivec2((p0.x + 1) % isz.x, p0.y + 1), ivec2(0), isz - 1);\n    p0 = clamp(ivec2((p0.x + isz.x) % isz.x, p0.y), ivec2(0), isz - 1);\n'
        elif rep_v:
            wrap_idx = '    ivec2 p1 = clamp(ivec2(p0.x + 1, (p0.y + 1) % isz.y), ivec2(0), isz - 1);\n    p0 = clamp(ivec2(p0.x, (p0.y + isz.y) % isz.y), ivec2(0), isz - 1);\n'
        else:
            wrap_idx = '    ivec2 p1 = clamp(p0 + 1, ivec2(0), isz - 1);\n    p0 = clamp(p0, ivec2(0), isz - 1);\n'
        extra = ''
        if is_frag:
            if mip:
                extra = (
                    f'vec4 slin{slot}_i(vec2 uv{la}) {{ return slin{slot}(uv{lp}, textureQueryLod(t{slot}, uv).x); }}\n'
                    f'vec4 slin{slot}_b(vec2 uv{la}, float bias) {{ return slin{slot}(uv{lp}, textureQueryLod(t{slot}, uv).x + bias); }}\n'
                )
            else:
                extra = (
                    f'vec4 slin{slot}_i(vec2 uv{la}) {{ return slin{slot}(uv{lp}, 0.0f); }}\n'
                    f'vec4 slin{slot}_b(vec2 uv{la}, float bias) {{ return slin{slot}(uv{lp}, 0.0f); }}\n'
                )
        return (
            f'layout(set = {1 if is_frag else 0}, binding = {32 + slot}) uniform sampler2D{"Array" if arr else ""} t{slot};\n'
            f'{tsize}\n'
            f'vec4 slin{slot}(vec2 uv{la}, float lod) {{\n'
            f'{lod_sel}'
            f'    ivec3 tsz = tsize{slot}(il);\n'
            f'    vec2 sz = max(vec2(tsz.xy), vec2(1.0f));\n'
            f'    ivec2 isz = ivec2(sz);\n'
            f'    vec2 wuv = {wuv};\n'
            f'    vec2 tc = floor((wuv * sz - 0.5f) * 256.0f + 0.5f) / 256.0f;\n'
            f'    ivec2 p0 = ivec2(floor(tc));\n'
            f'    vec2 f = tc - vec2(p0);\n'
            f'{wrap_idx}'
            f'{fetch}'
            f'    return mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);\n'
            f'}}\n'
            f'{extra}'
        )

    shaders = {
        'vert_2d_and_arr.vert': (
            '#version 460\n'
            + slin_helper(0, False, False, True, False, False)
            + slin_helper(1, True, False, False, True, True)
            + 'layout(location = 0) out vec4 o0;\n'
            'void main() {\n'
            '    o0 = slin0(vec2(0.25f, 0.75f), 0.0f) + slin1(vec2(0.5f), 2, -1e38f);\n'
            '    gl_Position = o0;\n'
            '}\n'
        ),
        'frag_all_modes.frag': (
            '#version 460\n'
            + slin_helper(0, False, True, True, False, False)
            + slin_helper(1, True, True, True, True, True)
            + slin_helper(2, False, True, False, True, False)
            + slin_helper(3, True, True, False, False, True)
            + 'layout(location = 0) in vec2 uv;\n'
            'layout(location = 0) out vec4 outc;\n'
            'void main() {\n'
            '    outc = slin0(uv, 0.0f) + slin0_i(uv) + slin0_b(uv, -0.5f)\n'
            '         + slin1(uv, 1, 99.0f) + slin1_i(uv, 0) + slin1_b(uv, 3, 1.25f)\n'
            '         + slin2(uv, 0.0f) + slin2_i(uv) + slin2_b(uv, 0.5f)\n'
            '         + slin3(uv, 1, 2.0f) + slin3_i(uv, 0) + slin3_b(uv, 0, -0.25f);\n'
            '}\n'
        ),
        'comp_2d_and_arr.comp': (
            '#version 460\n'
            'layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;\n'
            + slin_helper(0, False, False, True, True, False)
            + slin_helper(1, True, False, False, False, True)
            + 'layout(std430, set = 0, binding = 0) buffer Out { vec4 res[]; };\n'
            'void main() {\n'
            '    res[gl_GlobalInvocationID.x] = slin0(vec2(0.5f), 0.0f) + slin1(vec2(0.125f, 0.875f), 0, 1.0f);\n'
            '}\n'
        ),
    }

    compiled = 0
    for name, src in shaders.items():
        sh_path = OUT / name
        spv_path = OUT / (name + '.spv')
        sh_path.write_text(src)
        r = subprocess.run([str(glslc), '--target-env=vulkan1.1', str(sh_path), '-o', str(spv_path)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f'glslc failed on {name}:\n{r.stderr}')
        compiled += 1
    print(f'{compiled} soft_linear Vulkan 1.1 GLSL->SPIR-V checks passed (glslc VS/FS/CS 2D & 2DArray)')


def slin_ref(grid, w: int, h: int, u: float, v: float, lod: float = 0.0, levels: int = 1,
             rep_u: bool = False, rep_v: bool = False):
    ml = max(levels - 1, 0)
    clamped_lod = min(max(lod, 0.0), float(ml))
    il = int(min(max(math.floor(clamped_lod + 0.5), 0.0), float(ml)))
    wu = (u - math.floor(u)) if rep_u else u
    wv = (v - math.floor(v)) if rep_v else v
    tcx = math.floor((wu * w - 0.5) * 256.0 + 0.5) / 256.0
    tcy = math.floor((wv * h - 0.5) * 256.0 + 0.5) / 256.0
    p0x = int(math.floor(tcx))
    p0y = int(math.floor(tcy))
    fx = tcx - p0x
    fy = tcy - p0y
    p1x = ((p0x + 1) % w) if rep_u else min(max(p0x + 1, 0), w - 1)
    p1y = ((p0y + 1) % h) if rep_v else min(max(p0y + 1, 0), h - 1)
    p0x = ((p0x + w) % w) if rep_u else min(max(p0x, 0), w - 1)
    p0y = ((p0y + h) % h) if rep_v else min(max(p0y, 0), h - 1)
    s00 = grid[p0y * w + p0x]
    s10 = grid[p0y * w + p1x]
    s01 = grid[p1y * w + p0x]
    s11 = grid[p1y * w + p1x]
    r0 = s00 * (1.0 - fx) + s10 * fx
    r1 = s01 * (1.0 - fx) + s11 * fx
    return il, p0x, p0y, p1x, p1y, fx, fy, r0 * (1.0 - fy) + r1 * fy


def run_math_checks():
    n = 0
    # 1. Exact texel centers (issue #21 parity): u=(k+0.5)/64 +- 1e-7 -> zero neighbor bleed
    grid = [i / 4096.0 for i in range(64 * 64)]
    for k in range(64):
        for eps in (-1e-7, 0.0, 1e-7):
            u = (k + 0.5) / 64.0 + eps
            _, p0x, p0y, _, _, fx, fy, val = slin_ref(grid, 64, 64, u, u)
            assert p0x == k and p0y == k and fx == 0.0 and fy == 0.0
            assert val == grid[k * 64 + k]
            n += 1

    # 2. Out-of-bounds clamping & extreme LOD clamping
    for u, v, exp_xy in [(-1.0, -0.5, (0, 0)), (0.0, 0.0, (0, 0)), (1.0, 1.0, (63, 63)), (2.5, 3.0, (63, 63))]:
        il, p0x, p0y, p1x, p1y, _, _, _ = slin_ref(grid, 64, 64, u, v, lod=-1e38, levels=1)
        assert il == 0 and (p0x, p0y) == exp_xy and (p1x, p1y) == exp_xy
        n += 1

    # 3. Shoreline clipmap quad from Mali vs Adreno divergence: (0x24e2, 0x24e2, 0x2530, 0x1eeb)
    # 2x2 patch: top-left=0x24e2 (river water), top-right=0x24e2, bottom-left=0x2530, bottom-right=0x1eeb (dry bank)
    patch = [0x24e2 / 65535.0, 0x24e2 / 65535.0, 0x2530 / 65535.0, 0x1eeb / 65535.0]
    # Center between all 4 texels: u=0.5, v=0.5 -> tc=(0.5, 0.5), f=(0.5, 0.5)
    _, p0x, p0y, p1x, p1y, fx, fy, interp = slin_ref(patch, 2, 2, 0.5, 0.5)
    assert (p0x, p0y, p1x, p1y) == (0, 0, 1, 1) and fx == 0.5 and fy == 0.5
    expected = (patch[0] + patch[1] + patch[2] + patch[3]) * 0.25
    assert abs(interp - expected) < 1e-12
    # Quantize back to R16_UNORM (nearest unorm16 step): confirms shoreline drops below water plateau instead of staying 0x24e2/0x2530
    q16 = round(interp * 65535.0)
    assert 0x1eeb < q16 < 0x24e2, f'Unexpected quantized shoreline height {q16:#x}'
    # Also verify exact 1/256 quantization steps around u=(0.5+0.25/2)/2 -> 1/8 texel step
    _, _, _, _, _, fx2, _, val2 = slin_ref(patch, 2, 2, (0.5 + 32.0 / 256.0 + 0.1 / 256.0) / 2.0, 0.25)
    assert fx2 == 32.0 / 256.0
    n += 2
    # 4. REPEAT toroidal wrap seam parity (u=0.0, u=1.0, u=-1e-9 blend 0.5*texel[w-1] + 0.5*texel[0])
    ring = [float(i) for i in range(64 * 64)]
    for u in (-1e-9, 0.0, 1.0, 2.0):
        _, p0x, p0y, p1x, p1y, fx, fy, val = slin_ref(ring, 64, 64, u, 0.5 / 64.0, rep_u=True, rep_v=False)
        assert (p0x, p1x) == (63, 0) and fx == 0.5 and abs(val - 31.5) < 1e-12
        n += 1
    _, p0x, p0y, p1x, p1y, fx, fy, val = slin_ref(ring, 64, 64, 1.5 / 64.0, 0.0, rep_u=True, rep_v=True)
    assert (p0x, p0y, p1x, p1y) == (1, 63, 2, 0) and fx == 0.0 and fy == 0.5
    assert abs(val - 0.5 * (ring[63 * 64 + 1] + ring[1])) < 1e-12
    n += 1
    print(f'{n} numerical 1/256 bilinear math checks passed (texel-center #21 + clamp + REPEAT seam + shoreline quad {q16:#x})')


if __name__ == '__main__':
    run_wasm_c_checks()
    run_glslc_checks()
    run_math_checks()
