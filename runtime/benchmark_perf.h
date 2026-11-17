// Cumulative frame diagnostics, inspired by SavageWindRecomp perfcount.h / perf_routes.py (MIT).
// Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors
// Copyright (c) 2026 Peiikai (LAS Recomp). See third_party/licenses/SavageWindRecomp.txt.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#ifdef __ANDROID__
#include <stdatomic.h>
extern bool g_benchmark;
enum { BP_BUILDS, BP_BUILD_NS, BP_BUILD_WAIT_NS, BP_GPU_WAIT_NS, BP_TEX_NS, BP_TEX_BYTES,
       BP_BC_NS, BP_ASTC_NS, BP_FILE_READS, BP_FILE_BYTES, BP_FILE_NS,
       BP_REBUILDS, BP_EVICT_NS, BP_EVICTED, BP_COUNT };
extern _Atomic uint64_t g_bench_perf[BP_COUNT];
// Keep arguments (notably clock reads) behind the same flag as the update.
#define bench_add(k, n) do { if (g_benchmark) atomic_fetch_add_explicit(&g_bench_perf[(k)], (n), memory_order_relaxed); } while (0)
static inline bool bench_on(void) { return g_benchmark; }
static inline uint64_t bench_clock(void) { extern uint64_t plat_now_ns(void); return g_benchmark ? plat_now_ns() : 0; }
static inline void bench_record(int k, uint64_t n) {
    atomic_fetch_add_explicit(&g_bench_perf[k], n, memory_order_relaxed);
}
static inline void bench_elapsed(int k, uint64_t t) {
    if (t) { extern uint64_t plat_now_ns(void); bench_record(k, plat_now_ns() - t); }
}
static inline void bench_io_done(int time, int bytes, uint64_t start, uint64_t size) {
    if (!start) return;
    bench_elapsed(time, start);
    bench_record(bytes, size);
    if (time == BP_FILE_NS) bench_record(BP_FILE_READS, 1);
}
#define BENCH_REASONS 8
typedef struct { double p50, p95, p99; int samples; } BenchPct;
typedef struct {
    uint64_t totals[BP_COUNT];
    int long_frames, causes[BENCH_REASONS], display_frames, missing_display;
    double display_fps, display_low1, display_low01;
    int display_over_budget, display_over50;
    BenchPct headroom, display;
} BenchStats;
static inline void bench_build_done(uint64_t *t) { if (*t) { bench_elapsed(BP_BUILD_NS, *t); bench_record(BP_BUILDS, 1); } }
#define BENCH_BUILD() uint64_t bench_build_t __attribute__((cleanup(bench_build_done))) = bench_clock()
static inline void bench_texture_done(uint64_t *t) { bench_elapsed(BP_TEX_NS, *t); }
#define BENCH_TEXTURE() uint64_t bench_texture_t __attribute__((cleanup(bench_texture_done))) = bench_clock()
// 1 ready, 0 wait, -1 unavailable, -2 timed out; elapsed uses a monotonic clock.
static inline int bench_cooling_check(int limit, int status, double elapsed, int timeout) {
    if (status < 0) return -1;
    if (status <= limit) return 1;
    return elapsed >= timeout ? -2 : 0;
}
void bench_trace_begin(void);
void bench_trace_frame(const char *phase, bool window, double period, double cpu, double game, double render, double gpu);
void bench_trace_finish(const char *dir, const char *scene, int repeat, BenchStats *stats);
void bench_trace_json(FILE *f, const BenchStats *s);
void bench_handoff(uint64_t asked);
void bench_present_queue(uint32_t id, uint64_t asked);
void bench_present_actual(uint32_t id, uint64_t actual);
#else
#define BENCH_BUILD() ((void)0)
#define BENCH_TEXTURE() ((void)0)
#define bench_on() false
#define bench_clock() 0ull
#define bench_add(k, n) ((void)0)
#define bench_record(k, n) ((void)0)
#define bench_elapsed(k, t) ((void)0)
#define bench_io_done(time, bytes, start, size) ((void)0)
#endif
