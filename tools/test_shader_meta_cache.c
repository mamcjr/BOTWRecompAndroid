// Host/Bionic unit tests for binary Maxwell stage metadata cache (.smeta) key, flags, negative controls, & persistence.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define mw_getpid() _getpid()
#define mw_mkdir(p) _mkdir(p)
#define mw_rmdir(p) _rmdir(p)
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <unistd.h>
#define mw_getpid() getpid()
#define mw_mkdir(p) mkdir((p), 0700)
#define mw_rmdir(p) rmdir(p)
#endif

#include "nvn/vulkan/shader_meta_cache.h"

static bool dir_exists(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void make_test_dir(char *out, size_t out_sz) {
    const char *base = getenv("TMPDIR");
    if (!dir_exists(base)) base = getenv("TMP");
    if (!dir_exists(base)) base = getenv("TEMP");
    if (!dir_exists(base)) base = dir_exists("/data/local/tmp") ? "/data/local/tmp" : dir_exists("/tmp") ? "/tmp" : ".";
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        int n = snprintf(out, out_sz, "%s/mw_smeta_test_%llu_%u", base, (unsigned long long)mw_getpid(), attempt);
        assert(n > 0 && (size_t)n < out_sz);
        if (mw_mkdir(out) == 0) return;
    }
    assert(0 && "mw_mkdir failed");
}

static void put_u64(uint8_t *dst, uint64_t v) { memcpy(dst, &v, sizeof v); }
static void put_u32(uint8_t *dst, uint32_t v) { memcpy(dst, &v, sizeof v); }

static void build_fake_maxwell_shader(uint8_t *buf, size_t buf_sz, size_t *out_cmax, size_t *out_const_sz,
                                      bool include_brx) {
    memset(buf, 0, buf_sz);
    for (size_t i = 0; i < 0x50; i++) buf[0x30 + i] = (uint8_t)(i + 1);
    uint8_t *code = buf + 0x80;
    put_u64(code + 0 * 8, 0x001fc00000000000ull);
    put_u64(code + 1 * 8, 0x5c98078000070001ull);
    put_u64(code + 2 * 8, include_brx ? 0xe250000000000000ull : 0x5c98078000070002ull);
    put_u64(code + 3 * 8, 0xe30000000007000full);
    put_u64(code + 4 * 8, 0x001fc00000000000ull);
    put_u64(code + 5 * 8, 0xe240000000000000ull | (0xFFFFF8ull << 20));  // bra -8
    put_u64(code + 6 * 8, 0xdeadbeefcafebabeull);                        // past bra self
    *out_cmax = 0x80 + 16 * 8;
    uint8_t *cb1 = buf + *out_cmax;
    put_u32(cb1 + 0, 0x10);
    put_u32(cb1 + 4, 0x20);
    *out_const_sz = 16;
}

#define TEST_BUILD_ID "abc1234:1122334455667788"

static void assert_key_differs_vs(const uint8_t *blob, size_t cmax, size_t const_sz, int stage,
                                  const ShaderKey *k, uint32_t tf, uint64_t gbase, uint64_t gsize,
                                  const char *bid, uint64_t ref1, uint64_t ref2) {
    uint64_t h1 = 0, h2 = 0;
    assert(mw_stage_key(blob, cmax, const_sz, stage, k, tf, gbase, gsize, bid, &h1, &h2));
    assert(h1 != ref1 || h2 != ref2);
}

int main(void) {
    uint8_t blob_a[1024], blob_b[1024];
    size_t cmax = 0, const_sz = 0;
    build_fake_maxwell_shader(blob_a, sizeof blob_a, &cmax, &const_sz, false);
    memcpy(blob_b, blob_a, sizeof blob_a);
    put_u64(blob_b + 0x80 + 6 * 8, 0x123456789abcdef0ull);  // dead padding past bra self

    ShaderKey vkey = {.glsl = 1, .flip_y = true, .depth_minus_one = true};
    vkey.attr_kind[0] = 0;
    vkey.attr_kind[1] = 2;

    uint64_t h1a = 0, h2a = 0, h1b = 0, h2b = 0;
    uint32_t tf = MW_TF_BRX_WINDOW | MW_TF_STATIC_STACKS | MW_TF_STRUCTURED;
    assert(mw_stage_key(blob_a, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1a, &h2a));
    assert(mw_stage_key(blob_b, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(h1a == h1b && h2a == h2b);

    // Build / translator ID invalidation
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x100000000ull, 0x80000000ull,
                          "abc1234:deadbeefdeadbeef", h1a, h2a);
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x100000000ull, 0x80000000ull,
                          "def5678:1122334455667788", h1a, h2a);
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x100000000ull, 0x80000000ull,
                          NULL, h1a, h2a);

    // Explicit pad_ bytes and opposite-stage fields must not perturb STAGE_VERTEX hash
    ShaderKey vk_pad = vkey;
    memset(vk_pad.pad_, 0xA5, sizeof vk_pad.pad_);
    vk_pad.rt_kind[0] = 2;
    vk_pad.no_ds_write = true;
    vk_pad.vs_out_mask = 0x12345678u;
    assert(mw_stage_key(blob_a, cmax, const_sz, STAGE_VERTEX, &vk_pad, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(h1a == h1b && h2a == h2b);

    // Non-BRX shader must ignore cb1 changes even with MW_TF_BRX_WINDOW set
    put_u32(blob_b + cmax + 4, 0x99);
    assert(mw_stage_key(blob_b, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(h1a == h1b && h2a == h2b);

    // Unaligned blob pointer must hash identically
    uint8_t blob_unaligned[1025];
    memcpy(blob_unaligned + 1, blob_a, sizeof blob_a);
    assert(mw_stage_key(blob_unaligned + 1, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(h1a == h1b && h2a == h2b);

    // Instruction, SPH, gmem bounds, and every ShaderKey / trans_flags field must perturb the hash
    memcpy(blob_b, blob_a, sizeof blob_a);
    put_u64(blob_b + 0x80 + 1 * 8, 0x5c98078000070001ull ^ 1ull);
    assert_key_differs_vs(blob_b, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);

    memcpy(blob_b, blob_a, sizeof blob_a);
    blob_b[0x35] ^= 0x80u;
    assert_key_differs_vs(blob_b, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);

    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x200000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x100000000ull, 0x40000000ull, TEST_BUILD_ID, h1a, h2a);

    ShaderKey vk_var = vkey; vk_var.flip_x = true;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    vk_var = vkey; vk_var.flip_y = false;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    vk_var = vkey; vk_var.depth_minus_one = false;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    vk_var = vkey; vk_var.attr_kind[1] = 1;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    vk_var = vkey; vk_var.gmem_linear_lookup = true;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    vk_var = vkey; vk_var.glsl = 0;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    vk_var = vkey; vk_var.soft_linear = 1ull;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    vk_var = vkey; vk_var.soft_linear_mip = 1ull;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    vk_var = vkey; vk_var.soft_linear_rep_u = 1ull;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    vk_var = vkey; vk_var.soft_linear_rep_v = 1ull;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);

    for (unsigned bit = 0; bit <= 22; bit++)
        assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vkey, tf ^ (1u << bit), 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vkey, tf ^ MW_TF_MALI_CLEANUP, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vkey, tf ^ MW_TF_MALI_DISCARD_OPT, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_VERTEX, &vkey, tf ^ MW_TF_SHADER_TERMINATE, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, h1a, h2a);

    // Fragment ShaderKey fields
    ShaderKey fkey = {.glsl = 1, .flip_y = true, .vs_out_mask = 0x3fu};
    fkey.rt_kind[0] = 0;
    uint64_t fh1 = 0, fh2 = 0;
    assert(mw_stage_key(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &fh1, &fh2));
    ShaderKey fk_pad = fkey;
    memset(fk_pad.pad_, 0x5A, sizeof fk_pad.pad_);
    fk_pad.attr_kind[3] = 2;
    fk_pad.depth_minus_one = true;
    assert(mw_stage_key(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_pad, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(fh1 == h1b && fh2 == h2b);

    ShaderKey fk_var = fkey; fk_var.vs_out_mask = 0x7fu;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, fh1, fh2);
    fk_var = fkey; fk_var.rt_kind[0] = 2;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, fh1, fh2);
    fk_var = fkey; fk_var.flip_x = true;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, fh1, fh2);
    fk_var = fkey; fk_var.flip_y = false;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, fh1, fh2);
    fk_var = fkey; fk_var.gmem_linear_lookup = true;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, fh1, fh2);
    fk_var = fkey; fk_var.glsl = 0;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, fh1, fh2);
    fk_var = fkey; fk_var.no_ds_write = true;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, fh1, fh2);
    fk_var = fkey; fk_var.soft_linear = 2ull;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, fh1, fh2);
    fk_var = fkey; fk_var.soft_linear_mip = 2ull;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, fh1, fh2);
    fk_var = fkey; fk_var.soft_linear_rep_u = 2ull;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, fh1, fh2);
    fk_var = fkey; fk_var.soft_linear_rep_v = 2ull;
    assert_key_differs_vs(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_var, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, fh1, fh2);

    // w == 0 && i > 4 stop + BRX in dead padding past w == 0 ignored
    uint8_t zero_pad_a[1024], zero_pad_b[1024];
    memset(zero_pad_a, 0, sizeof zero_pad_a);
    for (size_t i = 0; i < 0x50; i++) zero_pad_a[0x30 + i] = (uint8_t)(i + 1);
    put_u64(zero_pad_a + 0x80 + 0 * 8, 0x001fc00000000000ull);
    put_u64(zero_pad_a + 0x80 + 1 * 8, 0x5c98078000070001ull);
    put_u64(zero_pad_a + 0x80 + 5 * 8, 0ull);                    // stop scan at w == 0 && i > 4
    put_u64(zero_pad_a + 0x80 + 6 * 8, 0xe250000000000000ull);   // BRX past w == 0 (dead padding)
    memcpy(zero_pad_b, zero_pad_a, sizeof zero_pad_a);
    put_u32(zero_pad_b + cmax + 4, 0xABCDu);
    assert(mw_stage_key(zero_pad_a, cmax, const_sz, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1a, &h2a));
    assert(mw_stage_key(zero_pad_b, cmax, const_sz, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(h1a == h1b && h2a == h2b);

    // BRX table sensitivity, >4096 const_bytes clamping, & negative controls
    build_fake_maxwell_shader(blob_a, sizeof blob_a, &cmax, &const_sz, true);
    memcpy(blob_b, blob_a, sizeof blob_a);
    put_u32(blob_b + cmax + 4, 0x38);
    assert(mw_stage_key(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1a, &h2a));
    assert(mw_stage_key(blob_b, cmax, const_sz, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(h1a != h1b && h2a != h2b);

    static uint8_t clamp_a[8192], clamp_b[8192];
    memcpy(clamp_a, blob_a, sizeof blob_a);
    memcpy(clamp_b, blob_a, sizeof blob_a);
    assert(mw_stage_key(clamp_a, cmax, 4200, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1a, &h2a));
    put_u32(clamp_b + cmax + 4096, 0x12345678u);  // beyond 1024-word clamp
    assert(mw_stage_key(clamp_b, cmax, 4200, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(h1a == h1b && h2a == h2b);
    put_u32(clamp_b + cmax + 4092, 0x87654321u);  // inside 1024-word window
    assert(mw_stage_key(clamp_b, cmax, 4200, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(h1a != h1b || h2a != h2b);

    // Negative controls: without MW_TF_BRX_WINDOW, without glsl, const_bytes < 4, or cmax >= 0x10000 -> cb1 ignored
    assert(mw_stage_key(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fkey, tf & ~MW_TF_BRX_WINDOW, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(mw_stage_key(blob_b, cmax, const_sz, STAGE_FRAGMENT, &fkey, tf & ~MW_TF_BRX_WINDOW, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1a, &h2a));
    assert(h1a == h1b && h2a == h2b);

    ShaderKey fk_msl = fkey; fk_msl.glsl = 0;
    assert(mw_stage_key(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fk_msl, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(mw_stage_key(blob_b, cmax, const_sz, STAGE_FRAGMENT, &fk_msl, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1a, &h2a));
    assert(h1a == h1b && h2a == h2b);

    assert(mw_stage_key(blob_a, cmax, 2, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    assert(mw_stage_key(blob_b, cmax, 2, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1a, &h2a));
    assert(h1a == h1b && h2a == h2b);

    static uint8_t big_a[0x10040], big_b[0x10040];
    memcpy(big_a, blob_a, sizeof blob_a);
    memcpy(big_b, blob_b, sizeof blob_b);
    assert(mw_stage_key(big_a, 0x10000, 16, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1b, &h2b));
    big_b[0x10004] = 0x77;
    assert(mw_stage_key(big_b, 0x10000, 16, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1a, &h2a));
    assert(h1a == h1b && h2a == h2b);

    assert(!mw_stage_key(NULL, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0, 0, TEST_BUILD_ID, &h1a, &h2a));
    assert(!mw_stage_key(blob_a, 0x80, 0, STAGE_VERTEX, &vkey, tf, 0, 0, TEST_BUILD_ID, &h1a, &h2a));
    assert(!mw_stage_key(blob_a, cmax, const_sz, STAGE_COMPUTE, &vkey, tf, 0, 0, TEST_BUILD_ID, &h1a, &h2a));

    // Binary .smeta round-trip and validation
    char dir[512];
    make_test_dir(dir, sizeof dir);

    assert(mw_stage_key(blob_a, cmax, const_sz, STAGE_FRAGMENT, &fkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &h1a, &h2a));
    assert(!mw_meta_exists(dir, h1a));
    assert(!mw_meta_exists(NULL, h1a));
    assert(!mw_meta_exists(dir, 0));
    ShaderInfo src = {0};
    src.stage = STAGE_FRAGMENT;
    src.cbuf_mask = 0x15u;
    src.cbuf_size[0] = 64;
    src.cbuf_size[2] = 512;
    src.cbuf_dynamic = 0x4u;
    src.cbwin_mask = 0x2u;
    src.ntex = 2;
    src.tex[0] = (ShaderTexture){.handle_offset = 0x28, .type = TEXTYPE_2D_ARRAY, .shadow = true, .is_array = true, .sampled = true};
    src.tex[1] = (ShaderTexture){.handle_offset = 0x40, .type = TEXTYPE_3D, .is_image = true, .bindless = true,
                                 .bindless_cbuf = 3, .bindless_offset = 0x80};
    src.attr_mask = 0x1fu;
    src.out_mask = 0x0f0fu;
    src.writes_depth = true;
    src.uses_discard = true;
    src.uses_gmem = true;
    src.uses_gmem_map = true;
    src.uses_tint = true;
    src.uses_rscale = true;
    src.local_mem = 128;
    src.num_insts = 42;

    // Invalid store/load arguments and overlong directory path rejection
    assert(!mw_meta_store(NULL, h1a, h2a, STAGE_FRAGMENT, &src, 0x1234ull, false, 1));
    assert(!mw_meta_store("", h1a, h2a, STAGE_FRAGMENT, &src, 0x1234ull, false, 1));
    assert(!mw_meta_store(dir, 0, h2a, STAGE_FRAGMENT, &src, 0x1234ull, false, 1));
    assert(!mw_meta_store(dir, h1a, 0, STAGE_FRAGMENT, &src, 0x1234ull, false, 1));
    assert(!mw_meta_store(dir, h1a, h2a, STAGE_FRAGMENT, NULL, 0x1234ull, false, 1));
    assert(!mw_meta_store(dir, h1a, h2a, STAGE_COMPUTE, &src, 0x1234ull, false, 1));
    assert(!mw_meta_store(dir, h1a, h2a, STAGE_FRAGMENT, &src, 0ull, false, 1));
    src.ntex = -1;
    assert(!mw_meta_store(dir, h1a, h2a, STAGE_FRAGMENT, &src, 0x1234ull, false, 1));
    src.ntex = 65;
    assert(!mw_meta_store(dir, h1a, h2a, STAGE_FRAGMENT, &src, 0x1234ull, false, 1));
    src.ntex = 2;
    char long_dir[300];
    memset(long_dir, 'a', sizeof long_dir - 1);
    long_dir[sizeof long_dir - 1] = 0;
    assert(!mw_meta_store(long_dir, h1a, h2a, STAGE_FRAGMENT, &src, 0x1234ull, false, 1));
    uint64_t got_spv = 0;
    assert(mw_meta_load(NULL, h1a, h2a, STAGE_FRAGMENT, &got_spv, NULL, NULL) == NULL);
    assert(mw_meta_load("", h1a, h2a, STAGE_FRAGMENT, &got_spv, NULL, NULL) == NULL);
    assert(mw_meta_load(dir, 0, h2a, STAGE_FRAGMENT, &got_spv, NULL, NULL) == NULL);
    assert(mw_meta_load(dir, h1a, 0, STAGE_FRAGMENT, &got_spv, NULL, NULL) == NULL);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_COMPUTE, &got_spv, NULL, NULL) == NULL);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_FRAGMENT, NULL, NULL, NULL) == NULL);
    assert(mw_meta_load(long_dir, h1a, h2a, STAGE_FRAGMENT, &got_spv, NULL, NULL) == NULL);

    // STAGE_FRAGMENT store/load
    assert(mw_meta_store(dir, h1a, h2a, STAGE_FRAGMENT, &src, 0xfeedface12345678ull, false, 3));
    assert(mw_meta_exists(dir, h1a));
    bool got_vid = true;
    uint8_t got_gather = 0;
    ShaderInfo *loaded = mw_meta_load(dir, h1a, h2a, STAGE_FRAGMENT, &got_spv, &got_vid, &got_gather);
    assert(loaded && loaded->msl == NULL && got_spv == 0xfeedface12345678ull && !got_vid && got_gather == 3);
    assert(loaded->cbuf_mask == src.cbuf_mask && loaded->cbuf_size[0] == 64 && loaded->cbuf_size[2] == 512);
    assert(loaded->cbuf_dynamic == src.cbuf_dynamic && loaded->cbwin_mask == src.cbwin_mask && loaded->ntex == 2);
    assert(loaded->attr_mask == src.attr_mask && loaded->out_mask == src.out_mask);
    assert(loaded->tex[0].handle_offset == 0x28 && loaded->tex[0].type == TEXTYPE_2D_ARRAY &&
           loaded->tex[0].shadow && loaded->tex[0].is_array && loaded->tex[0].sampled);
    assert(loaded->tex[1].handle_offset == 0x40 && loaded->tex[1].type == TEXTYPE_3D &&
           loaded->tex[1].is_image && loaded->tex[1].bindless && !loaded->tex[1].sampled &&
           loaded->tex[1].bindless_cbuf == 3 && loaded->tex[1].bindless_offset == 0x80);
    assert(loaded->writes_depth && loaded->uses_discard && loaded->uses_gmem && loaded->uses_gmem_map);
    assert(loaded->uses_tint && loaded->uses_rscale && loaded->local_mem == 128 && loaded->num_insts == 42);
    free(loaded);

    // STAGE_VERTEX roundtrip + overwrite with ntex = 0 and ntex = 64
    uint64_t vh1 = 0, vh2 = 0;
    assert(mw_stage_key(blob_a, cmax, const_sz, STAGE_VERTEX, &vkey, tf, 0x100000000ull, 0x80000000ull, TEST_BUILD_ID, &vh1, &vh2));
    assert(!mw_meta_exists(dir, vh1));
    ShaderInfo vsrc = {0};
    vsrc.stage = STAGE_VERTEX;
    vsrc.ntex = 0;
    vsrc.writes_point_size = true;
    vsrc.uses_gmem = true;
    vsrc.uses_gmem_store = true;
    assert(mw_meta_store(dir, vh1, vh2, STAGE_VERTEX, &vsrc, 0xabcdef0123456789ull, true, 0));
    loaded = mw_meta_load(dir, vh1, vh2, STAGE_VERTEX, &got_spv, &got_vid, NULL);
    assert(loaded && got_vid && loaded->writes_point_size && loaded->uses_gmem_store && loaded->ntex == 0);
    free(loaded);

    vsrc.ntex = 64;
    for (int i = 0; i < 64; i++) vsrc.tex[i] = (ShaderTexture){.handle_offset = (uint32_t)(i * 8), .type = TEXTYPE_2D};
    assert(mw_meta_store(dir, vh1, vh2, STAGE_VERTEX, &vsrc, 0xabcdef0123456789ull, true, 0));
    loaded = mw_meta_load(dir, vh1, vh2, STAGE_VERTEX, &got_spv, &got_vid, NULL);
    assert(loaded && loaded->ntex == 64 && loaded->tex[63].handle_offset == 63u * 8u);
    free(loaded);

    // n >= MW_BUF_CAP rejection (ntex = 64 + 1 trailing byte)
    char vpath[600];
    snprintf(vpath, sizeof vpath, "%s/%016llx.smeta", dir, (unsigned long long)vh1);
    FILE *vf = fopen(vpath, "ab");
    assert(vf && fputc(0, vf) != EOF);
    fclose(vf);
    assert(mw_meta_load(dir, vh1, vh2, STAGE_VERTEX, &got_spv, &got_vid, NULL) == NULL);
    remove(vpath);

    // Rejection checks on STAGE_FRAGMENT file: h1, h2, stage, bit flip, magic, version, spv_hash=0, ntex=65, trailing, truncs
    assert(mw_meta_load(dir, h1a, h2a ^ 1ull, STAGE_FRAGMENT, &got_spv, &got_vid, &got_gather) == NULL);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_VERTEX, &got_spv, &got_vid, &got_gather) == NULL);

    char path[600];
    snprintf(path, sizeof path, "%s/%016llx.smeta", dir, (unsigned long long)h1a);
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    uint8_t raw[sizeof(MwMetaHdr) + 64 * sizeof(MwMetaTex)];
    size_t raw_n = fread(raw, 1, sizeof raw, f);
    fclose(f);
    assert(raw_n == sizeof(MwMetaHdr) + 2 * sizeof(MwMetaTex));

    raw[64] ^= 0x5au;
    f = fopen(path, "wb"); assert(f && fwrite(raw, 1, raw_n, f) == raw_n); fclose(f);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_FRAGMENT, &got_spv, &got_vid, &got_gather) == NULL);
    raw[64] ^= 0x5au;

    raw[0] = 'X';
    f = fopen(path, "wb"); assert(f && fwrite(raw, 1, raw_n, f) == raw_n); fclose(f);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_FRAGMENT, &got_spv, &got_vid, &got_gather) == NULL);
    raw[0] = 'M';

    MwMetaHdr bad_hdr;
    MwMetaTex bad_tex[2];
    memcpy(&bad_hdr, raw, sizeof bad_hdr);
    memcpy(bad_tex, raw + sizeof bad_hdr, sizeof bad_tex);
    bad_hdr.h1 = h1a ^ 1ull;
    bad_hdr.payload_hash = mw_meta_payload_hash(&bad_hdr, bad_tex, 2);
    f = fopen(path, "wb");
    assert(f && fwrite(&bad_hdr, sizeof bad_hdr, 1, f) == 1 && fwrite(bad_tex, sizeof bad_tex, 1, f) == 1);
    fclose(f);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_FRAGMENT, &got_spv, &got_vid, &got_gather) == NULL);

    bad_hdr.h1 = h1a;
    bad_hdr.version = MW_META_VERSION + 1;
    bad_hdr.payload_hash = mw_meta_payload_hash(&bad_hdr, bad_tex, 2);
    f = fopen(path, "wb");
    assert(f && fwrite(&bad_hdr, sizeof bad_hdr, 1, f) == 1 && fwrite(bad_tex, sizeof bad_tex, 1, f) == 1);
    fclose(f);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_FRAGMENT, &got_spv, &got_vid, &got_gather) == NULL);

    bad_hdr.version = MW_META_VERSION;
    bad_hdr.spv_hash = 0;
    bad_hdr.payload_hash = mw_meta_payload_hash(&bad_hdr, bad_tex, 2);
    f = fopen(path, "wb");
    assert(f && fwrite(&bad_hdr, sizeof bad_hdr, 1, f) == 1 && fwrite(bad_tex, sizeof bad_tex, 1, f) == 1);
    fclose(f);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_FRAGMENT, &got_spv, &got_vid, &got_gather) == NULL);

    bad_hdr.spv_hash = 0xfeedface12345678ull;
    bad_hdr.ntex = 65;
    bad_hdr.payload_hash = mw_meta_payload_hash(&bad_hdr, bad_tex, 2);
    f = fopen(path, "wb");
    assert(f && fwrite(&bad_hdr, sizeof bad_hdr, 1, f) == 1 && fwrite(bad_tex, sizeof bad_tex, 1, f) == 1);
    fclose(f);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_FRAGMENT, &got_spv, &got_vid, &got_gather) == NULL);

    f = fopen(path, "wb");
    assert(f && fwrite(raw, 1, raw_n, f) == raw_n && fputc(0, f) != EOF);
    fclose(f);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_FRAGMENT, &got_spv, &got_vid, &got_gather) == NULL);

    f = fopen(path, "wb");
    assert(f && fwrite(raw, 1, sizeof(MwMetaHdr), f) == sizeof(MwMetaHdr));
    fclose(f);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_FRAGMENT, &got_spv, &got_vid, &got_gather) == NULL);

    f = fopen(path, "wb");
    assert(f && fwrite(raw, 1, 32, f) == 32);
    fclose(f);
    assert(mw_meta_load(dir, h1a, h2a, STAGE_FRAGMENT, &got_spv, &got_vid, &got_gather) == NULL);

    remove(path);
    assert(!mw_meta_exists(dir, h1a));
    mw_rmdir(dir);
    puts("PASS: test_shader_meta_cache (.smeta key/store/load/corruption)");
    return 0;
}
