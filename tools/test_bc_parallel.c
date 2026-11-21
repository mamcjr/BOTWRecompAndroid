// Host/Bionic tests for parallel CPU BC1-BC7 & BC1->ASTC decode policy, dynamic chunking, bit-exactness, and speedup.
#if defined(__ANDROID__) || defined(__linux__)
#define _GNU_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#if defined(__ANDROID__) || defined(__linux__)
#include <sched.h>
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "../runtime/nvn/bcdec.h"
#include "../runtime/nvn/bc1_astc.c"
#include "../runtime/nvn/vulkan/bc_parallel.h"

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void fill_prng(uint8_t *dst, size_t n, uint64_t seed) {
    uint64_t s = seed ? seed : 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < n; i++) {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        dst[i] = (uint8_t)((s * 0x2545F4914F6CDD1Dull) >> 56);
    }
}

static void ref_bc_rows(VkFormat bc, const uint8_t *blocks, uint32_t wb, uint32_t y0, uint32_t y1, uint32_t bsize,
                        uint8_t *out, uint32_t bpp) {
    int pitch = (int)(wb * 4 * bpp);
    for (uint32_t y = y0; y < y1; y++)
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
            case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: {
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

typedef struct {
    VkFormat bc;
    const uint8_t *blocks;
    uint32_t wb, bsize, bpp;
    uint8_t *out;
    _Atomic uint64_t *cpu_chunks;
} TestBcCtx;

static void test_bc_task(void *ctx, uint32_t start, uint32_t count) {
    const TestBcCtx *c = (const TestBcCtx *)ctx;
#if defined(__ANDROID__) || defined(__linux__)
    if (c->cpu_chunks) {
        int cpu = sched_getcpu();
        if (cpu >= 0 && cpu < 64) atomic_fetch_add_explicit(&c->cpu_chunks[cpu], 1, memory_order_relaxed);
    }
#endif
    ref_bc_rows(c->bc, c->blocks, c->wb, start, start + count, c->bsize, c->out, c->bpp);
}

typedef struct {
    const uint8_t *bc1;
    uint8_t *astc;
    _Atomic uint64_t *cpu_chunks;
} TestAstcCtx;

static void test_astc_task(void *ctx, uint32_t start, uint32_t count) {
    const TestAstcCtx *c = (const TestAstcCtx *)ctx;
#if defined(__ANDROID__) || defined(__linux__)
    if (c->cpu_chunks) {
        int cpu = sched_getcpu();
        if (cpu >= 0 && cpu < 64) atomic_fetch_add_explicit(&c->cpu_chunks[cpu], 1, memory_order_relaxed);
    }
#endif
    bc1_to_astc(c->bc1 + (uint64_t)start * 8, c->astc + (uint64_t)start * 16, count);
}

static void cover_task(void *ctx, uint32_t start, uint32_t count) {
    _Atomic uint32_t *hits = (_Atomic uint32_t *)ctx;
    for (uint32_t i = 0; i < count; i++) atomic_fetch_add_explicit(&hits[start + i], 1, memory_order_relaxed);
}

static void print_cpu_chunks(const _Atomic uint64_t cpu_chunks[64]) {
    printf(" cpus[");
    bool first = true;
    for (int cpu = 0; cpu < 64; cpu++) {
        uint64_t n = atomic_load_explicit(&cpu_chunks[cpu], memory_order_relaxed);
        if (!n) continue;
        printf("%s%d:%llu", first ? "" : " ", cpu, (unsigned long long)n);
        first = false;
    }
    printf("]");
}

int main(void) {
    // Thread count policy & env validation
    bool from_env = true;
    assert(vk_bc_parallel_threads(NULL, false, true, &from_env) == 4 && !from_env);
    assert(vk_bc_parallel_threads(NULL, false, false, &from_env) == 1 && !from_env);
    assert(vk_bc_parallel_threads("4", true, true, &from_env) == 1 && !from_env);
    assert(vk_bc_parallel_threads("", false, true, &from_env) == 4 && !from_env);
    assert(vk_bc_parallel_threads("   ", false, true, &from_env) == 4 && !from_env);
    assert(vk_bc_parallel_threads("0", false, true, &from_env) == 1 && from_env);
    assert(vk_bc_parallel_threads("-5", false, true, &from_env) == 1 && from_env);
    assert(vk_bc_parallel_threads("1", false, true, &from_env) == 1 && from_env);
    assert(vk_bc_parallel_threads("4", false, false, &from_env) == 4 && from_env);
    assert(vk_bc_parallel_threads("4 ", false, true, &from_env) == 4 && from_env);
    assert(vk_bc_parallel_threads("8", false, true, &from_env) == 8 && from_env);
    assert(vk_bc_parallel_threads("99", false, true, &from_env) == 8 && from_env);
    assert(vk_bc_parallel_threads("9999999999999999999999", false, true, &from_env) == 8 && from_env);
    assert(vk_bc_parallel_threads("-9999999999999999999999", false, true, &from_env) == 1 && from_env);
    assert(vk_bc_parallel_threads("abc", false, true, &from_env) == 4 && !from_env);
    assert(vk_bc_parallel_threads("4px", false, true, &from_env) == 4 && !from_env);

    // Split thresholds
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC7_UNORM_BLOCK, 64, 64, false, 1) == false);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC7_UNORM_BLOCK, 1, 1, false, 4) == false);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC7_UNORM_BLOCK, 16, 16, false, 4) == false);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC7_UNORM_BLOCK, 32, 16, false, 4) == true);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC7_SRGB_BLOCK, 32, 16, false, 4) == true);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC6H_UFLOAT_BLOCK, 32, 16, false, 4) == true);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC6H_SFLOAT_BLOCK, 32, 16, false, 4) == true);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC7_UNORM_BLOCK, 2048, 2, false, 4) == false);

    assert(vk_bc_parallel_should_split(VK_FORMAT_BC3_UNORM_BLOCK, 32, 16, false, 4) == false);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC3_UNORM_BLOCK, 64, 32, false, 4) == true);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC4_SNORM_BLOCK, 64, 32, false, 4) == true);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC5_UNORM_BLOCK, 2048, 2, false, 4) == false);

    assert(vk_bc_parallel_should_split(VK_FORMAT_BC1_RGB_UNORM_BLOCK, 64, 32, true, 4) == false);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC1_RGB_UNORM_BLOCK, 64, 64, true, 4) == true);
    assert(vk_bc_parallel_should_split(VK_FORMAT_BC1_RGB_UNORM_BLOCK, 0, 64, true, 4) == false);

    // Dynamic chunk sizing unit tests
    assert(vk_bc_chunk_size(0, 4, 2) == 0);
    assert(vk_bc_chunk_size(100, 1, 2) == 100);
    assert(vk_bc_chunk_size(100, 0, 2) == 100);
    assert(vk_bc_chunk_size(10, 4, 0) == 1);
    assert(vk_bc_chunk_size(1, 4, 2) == 1);
    assert(vk_bc_chunk_size(128, 4, 2) == 2);
    assert(vk_bc_chunk_size(256, 4, 2) == 4);
    assert(vk_bc_chunk_size(65536, 4, 256) == 1024);
    assert(vk_bc_chunk_size(UINT32_MAX, 4, 2) == 67108864u);

    // Non-little worker CPU affinity selector unit tests
    {
        static const long d9400[8] = {2400, 2400, 2400, 2400, 3300, 3300, 3300, 3730};
        assert(vk_bc_select_fast_cpus(d9400, 8, 0xff, 4) == 0xf0);
        assert(vk_bc_select_fast_cpus(d9400, 8, 0xff, 6) == 0);
        assert(vk_bc_select_fast_cpus(d9400, 8, 0xff, 8) == 0);
        assert(vk_bc_select_fast_cpus(d9400, 8, 0x0f, 4) == 0);
        assert(vk_bc_select_fast_cpus(d9400, 8, 0x70, 4) == 0);
        assert(vk_bc_select_fast_cpus(d9400, 8, 0xf0, 4) == 0);
        assert(vk_bc_select_fast_cpus(d9400, 8, 0x80, 4) == 0);

        static const long bl44[8] = {1800, 1800, 1800, 1800, 2800, 2800, 2800, 2800};
        assert(vk_bc_select_fast_cpus(bl44, 8, 0xff, 4) == 0xf0);
        assert(vk_bc_select_fast_cpus(bl44, 8, 0xff, 6) == 0);
        assert(vk_bc_select_fast_cpus(bl44, 8, 0xff, 8) == 0);

        static const long budget26[8] = {1800, 1800, 1800, 1800, 1800, 1800, 2600, 2600};
        assert(vk_bc_select_fast_cpus(budget26, 8, 0xff, 4) == 0);
        assert(vk_bc_select_fast_cpus(budget26, 8, 0xff, 2) == 0);

        static const long hom[8] = {2400, 2400, 2400, 2400, 2400, 2400, 2400, 2400};
        assert(vk_bc_select_fast_cpus(hom, 8, 0xff, 4) == 0);

        assert(vk_bc_select_fast_cpus(NULL, 8, 0xff, 4) == 0);
        assert(vk_bc_select_fast_cpus(d9400, 0, 0xff, 4) == 0);
    }

    VkBcPool pool = {0};

    // Destroy -> reuse stress test (verifies clean state reset & per-worker start_gen)
    for (int r = 0; r < 10; r++) {
        uint32_t total = 65;
        _Atomic uint32_t *hits = calloc(total, sizeof(*hits));
        assert(hits);
        vk_bc_pool_run(&pool, cover_task, hits, total, 2, 4);
        for (uint32_t i = 0; i < total; i++) assert(atomic_load_explicit(&hits[i], memory_order_relaxed) == 1);
        free(hits);
        vk_bc_pool_destroy(&pool);
    }

    // Concurrent exact-coverage check across total = {1, 7, 13, 65, 257} and threads = {1, 2, 4, 8} + 8->2 shrink
    static const uint32_t totals[] = {1, 7, 13, 65, 257};
    static const int ths[] = {1, 2, 4, 8, 2};
    for (size_t ti = 0; ti < sizeof ths / sizeof ths[0]; ti++) {
        for (size_t ni = 0; ni < sizeof totals / sizeof totals[0]; ni++) {
            uint32_t total = totals[ni];
            _Atomic uint32_t *hits = calloc(total, sizeof(*hits));
            assert(hits);
            vk_bc_pool_run(&pool, cover_task, hits, total, 2, ths[ti]);
            for (uint32_t i = 0; i < total; i++) assert(atomic_load_explicit(&hits[i], memory_order_relaxed) == 1);
            free(hits);
        }
    }

    // Bit-exact 1T vs 4T across all BC formats + BC1->ASTC across multiple odd & power-of-2 block dims
    static const struct { VkFormat fmt; uint32_t bsize, bpp; } fmts[] = {
        {VK_FORMAT_BC1_RGB_UNORM_BLOCK, 8, 4},
        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4},
        {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4},
        {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4},
        {VK_FORMAT_BC4_UNORM_BLOCK, 8, 1},
        {VK_FORMAT_BC4_SNORM_BLOCK, 8, 1},
        {VK_FORMAT_BC5_UNORM_BLOCK, 16, 2},
        {VK_FORMAT_BC5_SNORM_BLOCK, 16, 2},
        {VK_FORMAT_BC6H_UFLOAT_BLOCK, 16, 8},
        {VK_FORMAT_BC6H_SFLOAT_BLOCK, 16, 8},
        {VK_FORMAT_BC7_UNORM_BLOCK, 16, 4},
        {VK_FORMAT_BC7_SRGB_BLOCK, 16, 4},
    };
    static const uint32_t dims[][2] = {{13, 17}, {64, 65}, {128, 128}};

    for (size_t di = 0; di < sizeof dims / sizeof dims[0]; di++) {
        uint32_t wb = dims[di][0], hb = dims[di][1];
        size_t nblocks = (size_t)wb * hb;
        uint8_t *blocks = malloc(nblocks * 16);
        assert(blocks);
        fill_prng(blocks, nblocks * 16, 0x123456789abcdef0ull ^ di);

        for (size_t fi = 0; fi < sizeof fmts / sizeof fmts[0]; fi++) {
            size_t out_sz = nblocks * 16 * fmts[fi].bpp;
            uint8_t *out1 = malloc(out_sz);
            uint8_t *out4 = malloc(out_sz);
            assert(out1 && out4);
            memset(out1, 0xA5, out_sz);
            memset(out4, 0x5A, out_sz);

            TestBcCtx c1 = {fmts[fi].fmt, blocks, wb, fmts[fi].bsize, fmts[fi].bpp, out1, NULL};
            TestBcCtx c4 = {fmts[fi].fmt, blocks, wb, fmts[fi].bsize, fmts[fi].bpp, out4, NULL};
            vk_bc_pool_run(NULL, test_bc_task, &c1, hb, 2, 1);
            vk_bc_pool_run(&pool, test_bc_task, &c4, hb, 2, 4);
            assert(memcmp(out1, out4, out_sz) == 0);

            // Also verify shrinking active thread count on the same persistent pool
            memset(out4, 0x3C, out_sz);
            vk_bc_pool_run(&pool, test_bc_task, &c4, hb, 2, 2);
            assert(memcmp(out1, out4, out_sz) == 0);

            free(out1);
            free(out4);
        }

        // BC1 -> ASTC 4x4 bit-exact check
        size_t astc_sz = nblocks * 16;
        uint8_t *astc1 = malloc(astc_sz);
        uint8_t *astc4 = malloc(astc_sz);
        assert(astc1 && astc4);
        TestAstcCtx a1 = {blocks, astc1, NULL};
        TestAstcCtx a4 = {blocks, astc4, NULL};
        vk_bc_pool_run(NULL, test_astc_task, &a1, (uint32_t)nblocks, 256, 1);
        vk_bc_pool_run(&pool, test_astc_task, &a4, (uint32_t)nblocks, 256, 4);
        assert(memcmp(astc1, astc4, astc_sz) == 0);
        free(astc1);
        free(astc4);
        free(blocks);
    }

    // Wall-clock BC7 1T vs 4T benchmark on 512x512 (128x128 blocks) and 1024x1024 (256x256 blocks)
    double spd512 = 0.0, spd1024 = 0.0, spd_astc = 0.0;
    const uint32_t bench_dims[2] = {128, 256};
    for (int bi = 0; bi < 2; bi++) {
        uint32_t wb = bench_dims[bi], hb = bench_dims[bi];
        size_t nblocks = (size_t)wb * hb;
        uint8_t *blocks = malloc(nblocks * 16);
        uint8_t *out1 = malloc(nblocks * 64);
        uint8_t *out4 = malloc(nblocks * 64);
        assert(blocks && out1 && out4);
        fill_prng(blocks, nblocks * 16, 0xfeedfacecafebeefull ^ (uint64_t)bi);

        _Atomic uint64_t cpu_chunks[64];
        for (int c = 0; c < 64; c++) atomic_init(&cpu_chunks[c], 0);

        for (int w = 0; w < 2; w++) ref_bc_rows(VK_FORMAT_BC7_UNORM_BLOCK, blocks, wb, 0, hb, 16, out1, 4);
        uint64_t t1_best = UINT64_MAX;
        for (int it = 0; it < 5; it++) {
            uint64_t t0 = now_ns();
            ref_bc_rows(VK_FORMAT_BC7_UNORM_BLOCK, blocks, wb, 0, hb, 16, out1, 4);
            uint64_t dt1 = now_ns() - t0;
            if (dt1 < t1_best) t1_best = dt1;
        }

        TestBcCtx c4 = {VK_FORMAT_BC7_UNORM_BLOCK, blocks, wb, 16, 4, out4, NULL};
        for (int w = 0; w < 2; w++) vk_bc_pool_run(&pool, test_bc_task, &c4, hb, 2, 4);
        c4.cpu_chunks = cpu_chunks;
        uint64_t t4_best = UINT64_MAX;
        for (int it = 0; it < 5; it++) {
            uint64_t t0 = now_ns();
            vk_bc_pool_run(&pool, test_bc_task, &c4, hb, 2, 4);
            uint64_t dt4 = now_ns() - t0;
            if (dt4 < t4_best) t4_best = dt4;
        }
        assert(memcmp(out1, out4, nblocks * 64) == 0);
        double ms1 = t1_best / 1e6, ms4 = t4_best / 1e6, spd = ms1 / ms4;
        if (wb == 128) spd512 = spd;
        else spd1024 = spd;
        printf("[bench] BC7 %ux%u (%ux%u blocks): 1T %.2f ms vs 4T %.2f ms (%.2fx)",
               wb * 4, hb * 4, wb, hb, ms1, ms4, spd);
        print_cpu_chunks(cpu_chunks);
        printf("\n");
        free(blocks);
        free(out1);
        free(out4);
    }

    // Wall-clock BC1 -> ASTC 1T vs 4T benchmark on 1024x1024 (256x256 blocks = 65536 blocks)
    {
        uint32_t wb = 256, hb = 256;
        size_t nblocks = (size_t)wb * hb;
        uint8_t *bc1 = malloc(nblocks * 8);
        uint8_t *astc1 = malloc(nblocks * 16);
        uint8_t *astc4 = malloc(nblocks * 16);
        assert(bc1 && astc1 && astc4);
        fill_prng(bc1, nblocks * 8, 0x0badc0de12345678ull);

        _Atomic uint64_t cpu_chunks[64];
        for (int c = 0; c < 64; c++) atomic_init(&cpu_chunks[c], 0);

        for (int w = 0; w < 2; w++) bc1_to_astc(bc1, astc1, nblocks);
        uint64_t t1_best = UINT64_MAX;
        for (int it = 0; it < 5; it++) {
            uint64_t t0 = now_ns();
            bc1_to_astc(bc1, astc1, nblocks);
            uint64_t dt1 = now_ns() - t0;
            if (dt1 < t1_best) t1_best = dt1;
        }

        TestAstcCtx a4 = {bc1, astc4, NULL};
        for (int w = 0; w < 2; w++) vk_bc_pool_run(&pool, test_astc_task, &a4, (uint32_t)nblocks, 256, 4);
        a4.cpu_chunks = cpu_chunks;
        uint64_t t4_best = UINT64_MAX;
        for (int it = 0; it < 5; it++) {
            uint64_t t0 = now_ns();
            vk_bc_pool_run(&pool, test_astc_task, &a4, (uint32_t)nblocks, 256, 4);
            uint64_t dt4 = now_ns() - t0;
            if (dt4 < t4_best) t4_best = dt4;
        }
        assert(memcmp(astc1, astc4, nblocks * 16) == 0);
        double ms1 = t1_best / 1e6, ms4 = t4_best / 1e6;
        spd_astc = ms1 / ms4;
        printf("[bench] BC1->ASTC 1024x1024 (256x256 blocks): 1T %.2f ms vs 4T %.2f ms (%.2fx)", ms1, ms4, spd_astc);
        print_cpu_chunks(cpu_chunks);
        printf("\n");
        free(bc1);
        free(astc1);
        free(astc4);
    }

    vk_bc_pool_destroy(&pool);
    printf("PASS: test_bc_parallel (policy, chunking, coverage, bit-exact 12 BC + ASTC, 512x512 %.2fx / 1024x1024 %.2fx / ASTC %.2fx)\n",
           spd512, spd1024, spd_astc);
    return 0;
}
