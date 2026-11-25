// Test of runtime/nvn/bc1_astc.c (BC1, BC3, BC5_UNORM -> ASTC 4x4) and runtime/nvn/vulkan/bc_astc_policy.h
// against bcdec and runtime/nvn/astc.c (bit-exact with ARM's decoder).
// Build (Android NDK clang, run on device):
//   $NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang --target=aarch64-linux-android30 -O2 \
//     -I runtime/nvn -I android/deps/out/include -DVK_NO_PROTOTYPES \
//     tools/test_bc1_astc.c runtime/nvn/bc1_astc.c runtime/nvn/astc.c -o android/out/test_bc1_astc
#include "../runtime/nvn/astc.h"
#include "../runtime/nvn/bc1_astc.h"
#include "../runtime/nvn/vulkan/bc_astc_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "../runtime/nvn/bcdec.h"

static uint64_t g_rng = 1;
static uint64_t rnd(void) {  // splitmix64
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void fill_bc4_indices(uint8_t blk[8], const uint8_t idx[16]) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) v |= ((uint64_t)(idx[i] & 7u)) << (3 * i);
    for (int i = 0; i < 6; i++) blk[2 + i] = (uint8_t)(v >> (8 * i));
}

static int test_bc1(long n) {
    const char *kind[3] = {"4 colours", "3 colours", "3 colours + transparent"};
    int worst[2][3] = {{0}}, alpha_bad[2][3] = {{0}}, illegal = 0;
    long count[3] = {0};
    for (long i = 0; i < n; i++) {
        uint8_t bc[8], astc[16], ref[64], out[64];
        uint64_t r = rnd();
        memcpy(bc, &r, 8);
        if (i % 4 == 1) bc[2] = bc[0], bc[3] = bc[1];
        if (i % 4 == 2) bc[2] = bc[0] ^ (uint8_t)(r >> 60), bc[3] = bc[1];
        uint32_t c0 = bc[0] | bc[1] << 8, c1 = bc[2] | bc[3] << 8, idx;
        memcpy(&idx, bc + 4, 4);
        int k = c0 > c1 ? 0 : 1;
        if (k == 1)
            for (int t = 0; t < 16; t++) if ((idx >> 2 * t & 3) == 3) k = 2;
        count[k]++;
        bcdec_bc1(bc, ref, 16);
        bc1_to_astc(bc, astc, 1);
        for (int s = 0; s < 2; s++) {
            if (!astc_decode_block(astc, 4, 4, s, out)) {
                if (illegal++ < 5) printf("BC1 illegal block (kind %d)\n", k);
                continue;
            }
            for (int t = 0; t < 16; t++) {
                if (ref[4 * t + 3] != out[4 * t + 3]) alpha_bad[s][k]++;
                if (ref[4 * t + 3] == 0) continue;
                for (int c = 0; c < 3; c++) {
                    int d = abs(ref[4 * t + c] - out[4 * t + c]);
                    if (d > worst[s][k]) worst[s][k] = d;
                }
            }
        }
    }
    int fail = illegal > 0;
    for (int k = 0; k < 3; k++)
        for (int s = 0; s < 2; s++) {
            printf("[BC1] %-24s %s: %ld blocks, max rgb err %d, alpha wrong %d\n",
                   kind[k], s ? "sRGB" : "LDR ", count[k], worst[s][k], alpha_bad[s][k]);
            if (worst[s][k] > 3 || alpha_bad[s][k]) fail = 1;
        }
    return fail;
}

static int test_bc3(long n) {
    enum { K_CONST255 = 0, K_CONST_LT, K_CONSTRGB, K_CUTOUT, K_GENERAL, K_NUM };
    const char *kname[K_NUM] = {
        "const alpha 255", "const alpha <255", "const RGB + alpha", "a0<=a1 (0/255 cutout)", "general RGB+alpha"
    };
    int worst_rgb[2][K_NUM] = {{0}}, worst_a[2][K_NUM] = {{0}}, cutout_bad[2] = {0}, illegal = 0;
    long count[K_NUM] = {0};

    for (long i = 0; i < n; i++) {
        uint8_t bc[16], astc[16], ref[64], out[64];
        uint64_t r0 = rnd(), r1 = rnd();
        memcpy(bc, &r0, 8);
        memcpy(bc + 8, &r1, 8);

        int k = (int)(i % K_NUM);
        if (k == K_CONST255) {
            bc[0] = bc[1] = 255;
            memset(bc + 2, 0, 6);
        } else if (k == K_CONST_LT) {
            bc[0] = bc[1] = (uint8_t)(r0 & 0xFE);
            memset(bc + 2, 0, 6);
        } else if (k == K_CONSTRGB) {
            if (i & 1) { bc[10] = bc[8]; bc[11] = bc[9]; }
            else { uint32_t pat = (uint32_t)(r1 >> 62); pat *= 0x55555555u; memcpy(bc + 12, &pat, 4); }
        } else if (k == K_CUTOUT) {
            if ((i & 3) == 0) {
                bc[0] = bc[1] = 0;
            } else if ((i & 3) == 1) {
                bc[0] = bc[1] = 255;
            } else {
                uint8_t a0 = (uint8_t)(r0 & 0x7Fu) + 20, a1 = (uint8_t)(a0 + (r0 >> 8 & 0x3Fu));
                if (a1 < a0) a1 = a0;
                bc[0] = a0; bc[1] = a1;
            }
            if ((i & 3) == 2) { bc[10] = bc[8]; bc[11] = bc[9]; }  // cutout + constant RGB
            uint8_t idx[16];
            for (int t = 0; t < 16; t++) {
                uint64_t sel = (r0 >> (16 + 3 * t)) & 7u;
                if ((i & 3) == 0) idx[t] = (t == 0) ? 7 : (uint8_t)(sel == 6 ? 0 : sel);
                else if ((i & 3) == 1) idx[t] = (t == 0) ? 6 : (uint8_t)(sel == 7 ? 0 : sel);
                else idx[t] = (t % 3 == 0) ? 6 : (t % 3 == 1) ? 7 : (uint8_t)sel;
            }
            fill_bc4_indices(bc, idx);
        } else {
            // Explicitly force c0 <= c1 half the time to verify 4-color RGB mode in BC3.
            uint16_t c0 = (uint16_t)(bc[8] | bc[9] << 8), c1 = (uint16_t)(bc[10] | bc[11] << 8);
            if ((i & 1) && c0 > c1) {
                bc[8] = (uint8_t)c1; bc[9] = (uint8_t)(c1 >> 8);
                bc[10] = (uint8_t)c0; bc[11] = (uint8_t)(c0 >> 8);
            }
        }
        count[k]++;
        bcdec_bc3(bc, ref, 16);
        bc3_to_astc(bc, astc, 1);

        for (int s = 0; s < 2; s++) {
            if (!astc_decode_block(astc, 4, 4, s, out)) {
                if (illegal++ < 5) printf("BC3 illegal block (kind %d)\n", k);
                continue;
            }
            for (int t = 0; t < 16; t++) {
                for (int c = 0; c < 3; c++) {
                    int d = abs((int)ref[4 * t + c] - (int)out[4 * t + c]);
                    if (d > worst_rgb[s][k]) worst_rgb[s][k] = d;
                }
                int da = abs((int)ref[4 * t + 3] - (int)out[4 * t + 3]);
                if (da > worst_a[s][k]) worst_a[s][k] = da;
                if (k == K_CUTOUT && (ref[4 * t + 3] == 0 || ref[4 * t + 3] == 255) && da != 0)
                    cutout_bad[s]++;
            }
        }
    }
    int fail = illegal > 0 || cutout_bad[0] > 0 || cutout_bad[1] > 0;
    for (int k = 0; k < K_NUM; k++)
        for (int s = 0; s < 2; s++) {
            printf("[BC3] %-24s %s: %ld blocks, max rgb err %2d, max alpha err %2d\n",
                   kname[k], s ? "sRGB" : "LDR ", count[k], worst_rgb[s][k], worst_a[s][k]);
            if (worst_rgb[s][k] > 5 || worst_a[s][k] > 45) fail = 1;
            if ((k == K_CONST255 || k == K_CONST_LT) && (worst_rgb[s][k] > 2 || worst_a[s][k] > 0)) fail = 1;
            if (k == K_CONSTRGB && (worst_rgb[s][k] > 2 || worst_a[s][k] > 20)) fail = 1;
        }
    printf("[BC3] cutout 0/255 exactness mismatches: LDR=%d sRGB=%d\n", cutout_bad[0], cutout_bad[1]);
    return fail;
}

static int test_bc5(long n) {
    enum { K_CONST = 0, K_ONECHAN, K_SMOOTH, K_CUTOUT, K_STEEP, K_NUM };
    const char *kname[K_NUM] = {
        "constant", "single-channel", "smooth (delta<=48)", "a0<=a1 (0/255 extremes)", "steep (delta>48)"
    };
    int worst_r[K_NUM] = {0}, worst_g[K_NUM] = {0}, la_bad = 0, cutout_bad = 0, smooth_ep_bad = 0, illegal = 0;
    long count[K_NUM] = {0};

    for (long i = 0; i < n; i++) {
        uint8_t bc[16], astc[16], ref[32], out[64];
        uint64_t r0 = rnd(), r1 = rnd();
        memcpy(bc, &r0, 8);
        memcpy(bc + 8, &r1, 8);

        int k = (int)(i % K_NUM);
        if (k == K_CONST) {
            bc[0] = bc[1] = (uint8_t)(r0 & 0xFF);
            bc[8] = bc[9] = (uint8_t)(r1 & 0xFF);
            memset(bc + 2, 0, 6);
            memset(bc + 10, 0, 6);
        } else if (k == K_ONECHAN) {
            if (((i / K_NUM) & 1) == 0) {
                bc[8] = bc[9] = (uint8_t)(r1 & 0xFF);
                memset(bc + 10, 0, 6);
            } else {
                bc[0] = bc[1] = (uint8_t)(r0 & 0xFF);
                memset(bc + 2, 0, 6);
            }
        } else if (k == K_SMOOTH) {
            uint8_t base0 = (uint8_t)(r0 % 207), base1 = (uint8_t)(r1 % 207);
            uint8_t d0 = (uint8_t)(1 + (r0 >> 8) % 48), d1 = (uint8_t)(1 + (r1 >> 8) % 48);  // includes delta == 48
            if ((i & 3) == 0) {
                // a0 <= a1 smooth block (no indices 6/7)
                bc[0] = base0; bc[1] = (uint8_t)(base0 + d0);
                bc[8] = base1; bc[9] = (uint8_t)(base1 + d1);
                uint8_t idx0[16], idx1[16];
                for (int t = 0; t < 16; t++) {
                    idx0[t] = (uint8_t)(((r0 >> (16 + 3 * t)) & 7u) % 6u);
                    idx1[t] = (uint8_t)(((r1 >> (16 + 3 * t)) & 7u) % 6u);
                }
                fill_bc4_indices(bc, idx0);
                fill_bc4_indices(bc + 8, idx1);
            } else {
                bc[0] = (uint8_t)(base0 + d0); bc[1] = base0;
                bc[8] = (uint8_t)(base1 + d1); bc[9] = base1;
            }
        } else if (k == K_CUTOUT) {
            if ((i & 3) == 0) {
                bc[0] = bc[1] = 0; bc[8] = bc[9] = 255;
            } else if ((i & 3) == 1) {
                bc[0] = bc[1] = 255; bc[8] = bc[9] = 0;
            } else {
                uint8_t x0 = (uint8_t)(r0 & 0x7Fu) + 20, x1 = (uint8_t)(x0 + ((r0 >> 8) & 0x3Fu));
                uint8_t y0 = (uint8_t)(r1 & 0x7Fu) + 20, y1 = (uint8_t)(y0 + ((r1 >> 8) & 0x3Fu));
                bc[0] = x0; bc[1] = x1;
                bc[8] = y0; bc[9] = y1;
            }
            uint8_t idx0[16], idx1[16];
            for (int t = 0; t < 16; t++) {
                uint64_t s0 = (r0 >> (16 + 3 * t)) & 7u, s1 = (r1 >> (16 + 3 * t)) & 7u;
                if ((i & 3) == 0) {
                    idx0[t] = (t == 0) ? 7 : (uint8_t)(s0 == 6 ? 0 : s0);
                    idx1[t] = (t == 0) ? 6 : (uint8_t)(s1 == 7 ? 0 : s1);
                } else if ((i & 3) == 1) {
                    idx0[t] = (t == 0) ? 6 : (uint8_t)(s0 == 7 ? 0 : s0);
                    idx1[t] = (t == 0) ? 7 : (uint8_t)(s1 == 6 ? 0 : s1);
                } else {
                    idx0[t] = (t % 3 == 0) ? 6 : (t % 3 == 1) ? 7 : (uint8_t)s0;
                    idx1[t] = (t % 4 == 0) ? 6 : (t % 4 == 1) ? 7 : (uint8_t)s1;
                }
            }
            fill_bc4_indices(bc, idx0);
            fill_bc4_indices(bc + 8, idx1);
        } else {
            // Ensure delta > 48 on at least one channel (a0 > a1)
            bc[0] = 240; bc[1] = 10;
        }
        count[k]++;
        bcdec_bc5(bc, ref, 8, 0);
        bc5_unorm_to_astc_la(bc, astc, 1);

        if (!astc_decode_block(astc, 4, 4, false, out)) {
            if (illegal++ < 5) printf("BC5 illegal block (kind %d)\n", k);
            continue;
        }
        uint8_t xmin = 255, xmax = 0, ymin = 255, ymax = 0;
        for (int t = 0; t < 16; t++) {
            uint8_t rx = ref[2 * t + 0], ry = ref[2 * t + 1];
            if (rx < xmin) xmin = rx;
            if (rx > xmax) xmax = rx;
            if (ry < ymin) ymin = ry;
            if (ry > ymax) ymax = ry;
            if (out[4 * t + 0] != out[4 * t + 1] || out[4 * t + 0] != out[4 * t + 2]) la_bad++;
            int dr = abs((int)rx - (int)out[4 * t + 0]);
            int dg = abs((int)ry - (int)out[4 * t + 3]);
            if (dr > worst_r[k]) worst_r[k] = dr;
            if (dg > worst_g[k]) worst_g[k] = dg;
            if (k == K_CUTOUT) {
                if ((rx == 0 || rx == 255) && dr != 0) cutout_bad++;
                if ((ry == 0 || ry == 255) && dg != 0) cutout_bad++;
            }
        }
        if (k == K_SMOOTH) {
            for (int t = 0; t < 16; t++) {
                if (ref[2 * t + 0] == xmin || ref[2 * t + 0] == xmax)
                    if (out[4 * t + 0] != ref[2 * t + 0]) smooth_ep_bad++;
                if (ref[2 * t + 1] == ymin || ref[2 * t + 1] == ymax)
                    if (out[4 * t + 3] != ref[2 * t + 1]) smooth_ep_bad++;
            }
        }
    }
    int fail = illegal > 0 || la_bad > 0 || cutout_bad > 0 || smooth_ep_bad > 0;
    for (int k = 0; k < K_NUM; k++) {
        printf("[BC5] %-24s LDR : %ld blocks, max R err %2d, max G(A) err %2d\n",
               kname[k], count[k], worst_r[k], worst_g[k]);
        if (k == K_CONST && (worst_r[k] > 0 || worst_g[k] > 0)) fail = 1;
        if (k == K_ONECHAN && (worst_r[k] > 12 || worst_g[k] > 12)) fail = 1;
        if (k == K_SMOOTH && (worst_r[k] > 8 || worst_g[k] > 8)) fail = 1;
        if (k == K_CUTOUT && (worst_r[k] > 28 || worst_g[k] > 28)) fail = 1;
        if (k == K_STEEP && (worst_r[k] > 28 || worst_g[k] > 28)) fail = 1;
    }
    printf("[BC5] LA-invariant=%d, cutout 0/255 bad=%d, smooth endpoint bad=%d\n", la_bad, cutout_bad, smooth_ep_bad);
    return fail;
}

static int test_odd_dims(void) {
    static const uint32_t dims[][2] = {{1, 1}, {2, 2}, {3, 3}, {5, 7}, {13, 9}};
    for (size_t di = 0; di < sizeof dims / sizeof dims[0]; di++) {
        uint32_t w = dims[di][0], h = dims[di][1];
        uint32_t wb = (w + 3) / 4, hb = (h + 3) / 4;
        uint8_t bc1[16 * 8], bc3[16 * 16], bc5[16 * 16], astc[16 * 16], dec[16 * 16 * 4];
        for (uint32_t i = 0; i < wb * hb; i++) {
            uint64_t a = rnd(), b = rnd();
            memcpy(bc1 + i * 8, &a, 8);
            memcpy(bc3 + i * 16, &a, 8);
            memcpy(bc3 + i * 16 + 8, &b, 8);
            bc3[i * 16] = 40; bc3[i * 16 + 1] = 180;  // a0 <= a1 with explicit 0/255 cutout texels
            uint8_t idx[16];
            for (int t = 0; t < 16; t++) idx[t] = (t & 1) ? 6 : 7;
            fill_bc4_indices(bc3 + i * 16, idx);

            memcpy(bc5 + i * 16, &a, 8);
            memcpy(bc5 + i * 16 + 8, &b, 8);
            if (i == 0) {
                bc5[0] = bc5[1] = 123; bc5[8] = bc5[9] = 45;  // Void-Extent block
                memset(bc5 + 2, 0, 6);
                memset(bc5 + 10, 0, 6);
            } else if (i & 1) {
                bc5[i * 16] = 60; bc5[i * 16 + 1] = 200;
                fill_bc4_indices(bc5 + i * 16, idx);
            } else {
                bc5[i * 16] = 120; bc5[i * 16 + 1] = 90;
                bc5[i * 16 + 8] = 140; bc5[i * 16 + 9] = 110;
            }
        }

        bc1_to_astc(bc1, astc, (uint64_t)wb * hb);
        astc_decode_ldr(astc, w, h, 4, 4, false, dec);
        for (uint32_t by = 0; by < hb; by++)
            for (uint32_t bx = 0; bx < wb; bx++) {
                uint8_t ref[64];
                bcdec_bc1(bc1 + (by * wb + bx) * 8, ref, 16);
                uint32_t cw = w - bx * 4 < 4 ? w - bx * 4 : 4;
                uint32_t ch = h - by * 4 < 4 ? h - by * 4 : 4;
                for (uint32_t py = 0; py < ch; py++)
                    for (uint32_t px = 0; px < cw; px++) {
                        uint8_t *o = dec + ((by * 4 + py) * w + (bx * 4 + px)) * 4;
                        uint8_t *r = ref + (py * 4 + px) * 4;
                        if (o[3] != r[3]) return 1;
                        if (r[3] == 0) continue;
                        for (int c = 0; c < 3; c++)
                            if (abs((int)o[c] - (int)r[c]) > 3) return 1;
                    }
            }

        bc3_to_astc(bc3, astc, (uint64_t)wb * hb);
        astc_decode_ldr(astc, w, h, 4, 4, false, dec);
        for (uint32_t by = 0; by < hb; by++)
            for (uint32_t bx = 0; bx < wb; bx++) {
                uint8_t ref[64];
                bcdec_bc3(bc3 + (by * wb + bx) * 16, ref, 16);
                uint32_t cw = w - bx * 4 < 4 ? w - bx * 4 : 4;
                uint32_t ch = h - by * 4 < 4 ? h - by * 4 : 4;
                for (uint32_t py = 0; py < ch; py++)
                    for (uint32_t px = 0; px < cw; px++) {
                        uint8_t *o = dec + ((by * 4 + py) * w + (bx * 4 + px)) * 4;
                        uint8_t *r = ref + (py * 4 + px) * 4;
                        if (o[3] != r[3]) {
                            printf("odd dims BC3 %ux%u alpha cutout mismatch at (%u,%u)\n", w, h, bx * 4 + px, by * 4 + py);
                            return 1;
                        }
                        for (int c = 0; c < 3; c++)
                            if (abs((int)o[c] - (int)r[c]) > 5) {
                                printf("odd dims BC3 %ux%u rgb mismatch at (%u,%u)\n", w, h, bx * 4 + px, by * 4 + py);
                                return 1;
                            }
                    }
            }

        bc5_unorm_to_astc_la(bc5, astc, (uint64_t)wb * hb);
        astc_decode_ldr(astc, w, h, 4, 4, false, dec);
        for (uint32_t by = 0; by < hb; by++)
            for (uint32_t bx = 0; bx < wb; bx++) {
                uint8_t ref[32];
                bcdec_bc5(bc5 + (by * wb + bx) * 16, ref, 8, 0);
                uint32_t cw = w - bx * 4 < 4 ? w - bx * 4 : 4;
                uint32_t ch = h - by * 4 < 4 ? h - by * 4 : 4;
                for (uint32_t py = 0; py < ch; py++)
                    for (uint32_t px = 0; px < cw; px++) {
                        uint8_t *o = dec + ((by * 4 + py) * w + (bx * 4 + px)) * 4;
                        uint8_t *r = ref + (py * 4 + px) * 2;
                        if (o[0] != o[1] || o[0] != o[2]) return 1;
                        if (abs((int)o[0] - (int)r[0]) > 28 || abs((int)o[3] - (int)r[1]) > 28) {
                            printf("odd dims BC5 %ux%u mismatch at (%u,%u)\n", w, h, bx * 4 + px, by * 4 + py);
                            return 1;
                        }
                    }
            }
    }
    printf("[GRID] tiny/odd mip dimensions (BC1/BC3/BC5 1x1..13x9): PASS\n");
    return 0;
}

static int test_policy_helpers(void) {
    struct { VkComponentMapping in, exp; } sw_cases[] = {
        {{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
         {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE}},
        {{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE},
         {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE}},
        {{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G},
         {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A}},
        {{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_G},
         {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_A}},
        {{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE},
         {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE}},
        {{VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A},
         {VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE}},
        {{VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
         {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE}},
    };
    for (size_t i = 0; i < sizeof sw_cases / sizeof sw_cases[0]; i++) {
        VkComponentMapping got = vk_bc5_la_remap_swizzle(sw_cases[i].in);
        if (memcmp(&got, &sw_cases[i].exp, sizeof got) != 0) {
            printf("swizzle remap case %zu FAILED: got (%d,%d,%d,%d) exp (%d,%d,%d,%d)\n",
                   i, got.r, got.g, got.b, got.a, sw_cases[i].exp.r, sw_cases[i].exp.g, sw_cases[i].exp.b, sw_cases[i].exp.a);
            return 1;
        }
    }
    if (!vk_format_is_astc_ldr(VK_FORMAT_ASTC_4x4_UNORM_BLOCK) ||
        !vk_format_is_astc_ldr(VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ||
        vk_format_is_astc_ldr(VK_FORMAT_BC3_UNORM_BLOCK) ||
        vk_format_is_astc_ldr(VK_FORMAT_R8G8B8A8_UNORM)) {
        printf("vk_format_is_astc_ldr check FAILED\n");
        return 1;
    }
    printf("[POLICY] swizzle remap + ASTC LDR format helper: PASS\n");
    return 0;
}

static void bench_throughput(void) {
    const uint64_t n = 262144;  // 4 MiB of 16-byte blocks (2 MiB BC1)
    uint8_t *src = malloc(n * 16), *dst = malloc(n * 16);
    if (!src || !dst) {
        free(src);
        free(dst);
        return;
    }
    for (uint64_t i = 0; i < n * 16 / 8; i++) {
        uint64_t r = rnd();
        memcpy(src + i * 8, &r, 8);
    }
    uint64_t t0 = now_ns();
    bc1_to_astc(src, dst, n);
    uint64_t t1 = now_ns();
    bc3_to_astc(src, dst, n);
    uint64_t t2 = now_ns();
    bc5_unorm_to_astc_la(src, dst, n);
    uint64_t t3 = now_ns();
    printf("[PERF 1T] BC1: %.1f ns/blk (%.0f MB/s) | BC3: %.1f ns/blk (%.0f MB/s) | BC5u: %.1f ns/blk (%.0f MB/s)\n",
           (t1 - t0) / (double)n, (n * 8.0) / ((t1 - t0) / 1e3),
           (t2 - t1) / (double)n, (n * 16.0) / ((t2 - t1) / 1e3),
           (t3 - t2) / (double)n, (n * 16.0) / ((t3 - t2) / 1e3));
    free(src);
    free(dst);
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    int fail = 0;
    fail |= test_policy_helpers();
    fail |= test_odd_dims();
    fail |= test_bc1(n);
    fail |= test_bc3(n);
    fail |= test_bc5(n);
    bench_throughput();
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
