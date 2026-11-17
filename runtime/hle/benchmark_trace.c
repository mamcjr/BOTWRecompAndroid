// See benchmark_perf.h and docs/BENCHMARK.md for attribution and measurement limits.
#include "../benchmark_perf.h"
#include "../rt.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

// Cumulative snapshots and per-frame deltas follow SavageWindRecomp control_prof.c.
// See third_party/licenses/SavageWindRecomp.txt.
_Atomic uint64_t g_bench_perf[BP_COUNT];
#define BUDGET (1000.0 / 30)
typedef struct {
    uint64_t asked, actual, c[BP_COUNT];
    uint32_t present_id;
    double ms[5], headroom, display;
    const char *phase;
    bool window;
    int cause;
} TraceFrame;
static TraceFrame *trace;
static int count, capacity;
static uint64_t previous[BP_COUNT], handoff;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t id; uint64_t asked, actual; } pending[256];
static const char *reasons[BENCH_REASONS] = {"pipeline_wait", "gpu_wait", "texture_decode", "texture_upload", "file_read", "pipeline_evict", "pipeline_activity", "unknown"};
static const char *counters[BP_COUNT] = {"builds", "build_ns", "build_wait_ns", "gpu_wait_ns", "texture_upload_ns", "texture_bytes", "bc_decode_ns", "astc_decode_ns", "file_reads", "file_bytes", "file_ns", "pipeline_rebuilds", "pipeline_evict_ns", "pipelines_evicted"};

void bench_trace_begin(void) {
    pthread_mutex_lock(&lock);
    count = 0;
    handoff = 0;
    memset(pending, 0, sizeof pending);
    for (int k = 0; k < BP_COUNT; k++) previous[k] = atomic_load_explicit(&g_bench_perf[k], memory_order_relaxed);
    pthread_mutex_unlock(&lock);
}
void bench_handoff(uint64_t asked) { handoff = asked; } // game's presenting thread only
void bench_present_queue(uint32_t id, uint64_t asked) {
    pthread_mutex_lock(&lock);
    pending[id % 256].id = id;
    pending[id % 256].asked = asked;
    pending[id % 256].actual = 0;
    pthread_mutex_unlock(&lock);
}
void bench_present_actual(uint32_t id, uint64_t actual) {
    if (!actual) return;
    pthread_mutex_lock(&lock);
    uint64_t asked = pending[id % 256].id == id ? pending[id % 256].asked : 0;
    if (asked) pending[id % 256].actual = actual;
    // Match the game's handoff, not the render thread's later frame counter. The first frame is not sampled.
    int lo = 0, hi = count;
    while (lo < hi) { int m = (lo + hi) / 2; if (trace[m].asked < asked) lo = m + 1; else hi = m; }
    if (asked && lo < count && trace[lo].asked == asked) trace[lo].actual = actual, trace[lo].present_id = id;
    pthread_mutex_unlock(&lock);
}
void bench_trace_frame(const char *phase, bool window, double period, double cpu, double game, double render, double gpu) {
    pthread_mutex_lock(&lock);
    if (count == capacity) {
        int cap = capacity ? capacity * 2 : 4096;
        TraceFrame *p = realloc(trace, sizeof *trace * (size_t)cap);
        if (!p) FATAL("benchmark: no memory for frame diagnostics\n");
        trace = p, capacity = cap;
    }
    TraceFrame *f = &trace[count++];
    *f = (TraceFrame){.asked = handoff, .phase = phase, .window = window, .ms = {period, cpu, game, render, gpu},
                      .headroom = cpu >= 0 && gpu >= 0 ? BUDGET - fmax(cpu, gpu) : NAN, .display = NAN, .cause = -1};
    for (int i = 0; i < 256; i++) if (handoff && pending[i].asked == handoff && pending[i].actual) {
        f->actual = pending[i].actual; f->present_id = pending[i].id; break;
    }
    for (int k = 0; k < BP_COUNT; k++) {
        uint64_t n = atomic_load_explicit(&g_bench_perf[k], memory_order_relaxed);
        f->c[k] = n - previous[k]; previous[k] = n;
    }
    pthread_mutex_unlock(&lock);
}
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static BenchPct pct(double *v, int n) {
    if (!n) return (BenchPct){.p50 = NAN, .p95 = NAN, .p99 = NAN};
    qsort(v, (size_t)n, sizeof *v, cmp);
    return (BenchPct){v[(n - 1) * 50 / 100], v[(n - 1) * 95 / 100], v[(n - 1) * 99 / 100], n};
}
static double low_fps(const double *sorted, int n, double share) {
    if (!n) return NAN;
    int k = (int)ceil(n * share); double sum = 0;
    for (int i = n - k; i < n; i++) sum += sorted[i];
    return sum > 0 ? k * 1000 / sum : NAN;
}
// Largest-stall selection follows SavageWindRecomp tools/perf_routes.py.
// See third_party/licenses/SavageWindRecomp.txt.
static int cause(const TraceFrame *f, double baseline_wait) {
    if (f->ms[0] <= BUDGET) return -1;
    double stalls[] = {f->c[BP_BUILD_WAIT_NS] / 1e6, fmax(0, f->c[BP_GPU_WAIT_NS] / 1e6 - baseline_wait),
                      (f->c[BP_BC_NS] + f->c[BP_ASTC_NS]) / 1e6,
                      fmax(0, ((double)f->c[BP_TEX_NS] - f->c[BP_BC_NS] - f->c[BP_ASTC_NS]) / 1e6), f->c[BP_FILE_NS] / 1e6,
                      f->c[BP_EVICT_NS] / 1e6};
    int best = 0;
    for (int k = 1; k < (int)(sizeof stalls / sizeof stalls[0]); k++) if (stalls[k] > stalls[best]) best = k;
    // An absolute budget (not SW's 2x median). Ignore sub-ms jitter when selecting a likely cause.
    if (stalls[best] >= fmax(0.1, (f->ms[0] - BUDGET) * .25)) return best;
    return f->c[BP_BUILDS] ? 6 : 7; // background activity is correlation, not a measured stall
}
void bench_trace_finish(const char *dir, const char *scene, int repeat, BenchStats *stats) {
    pthread_mutex_lock(&lock);
    *stats = (BenchStats){0};
    double *v = malloc(sizeof *v * (size_t)(count + 1));
    if (!v) FATAL("benchmark: no memory for diagnostics summary\n");
    int n = 0;
    for (int i = 0; i < count; i++) if (trace[i].window) v[n++] = trace[i].c[BP_GPU_WAIT_NS] / 1e6;
    BenchPct waits = pct(v, n);
    double base_wait = n ? waits.p50 : 0;
    n = 0;
    for (int i = 0; i < count; i++) {
        TraceFrame *f = &trace[i];
        // Only successive game frames and successive present IDs: no invented interval across missing feedback.
        if (i && f->actual && trace[i-1].actual && f->present_id == trace[i-1].present_id + 1 && f->actual > trace[i-1].actual)
            f->display = (f->actual - trace[i-1].actual) / 1e6;
        f->cause = cause(f, base_wait);
        if (!f->window) continue;
        for (int k = 0; k < BP_COUNT; k++) stats->totals[k] += f->c[k];
        if (f->cause >= 0) stats->long_frames++, stats->causes[f->cause]++;
        if (isfinite(f->headroom)) v[n++] = f->headroom;
        if (f->actual) stats->display_frames++;
        else stats->missing_display++;
    }
    stats->headroom = pct(v, n);
    n = 0; double sum = 0;
    for (int i = 1; i < count; i++) if (trace[i].window && trace[i-1].window && isfinite(trace[i].display))
        v[n++] = trace[i].display, sum += trace[i].display;
    stats->display = pct(v, n);
    stats->display_fps = sum > 0 ? n * 1000 / sum : NAN;
    stats->display_low1 = low_fps(v, n, .01); stats->display_low01 = low_fps(v, n, .001);
    for (int i = 0; i < n; i++) { stats->display_over_budget += v[i] > BUDGET; stats->display_over50 += v[i] > 50; }
    free(v);
    char path[1200]; snprintf(path, sizeof path, "%s/frames.csv", dir);
    bool fresh = access(path, F_OK) != 0;
    FILE *out = fopen(path, "a");
    if (!out) FATAL("benchmark: cannot write frames.csv\n");
    if (fresh) {
        fprintf(out, "scene,repeat,frame,phase,window,handoff_ns,period_ms,cpu_ms,game_ms,render_ms,gpu_ms,headroom_ms,display_present_id,actual_present_ns,display_period_ms,over_budget,likely_cause");
        for (int k = 0; k < BP_COUNT; k++) fprintf(out, ",%s", counters[k]);
        fputc('\n', out);
    }
    for (int i = 0; i < count; i++) {
        TraceFrame *f = &trace[i];
        fprintf(out, "%s,%d,%d,%s,%d,%llu,%.6f,%.6f,%.6f,%.6f,%.6f,", scene, repeat, i, f->phase, f->window,
                (unsigned long long)f->asked, f->ms[0], f->ms[1], f->ms[2], f->ms[3], f->ms[4]);
        if (isfinite(f->headroom)) fprintf(out, "%.6f", f->headroom);
        fprintf(out, ",%u,%llu,", f->present_id, (unsigned long long)f->actual);
        if (isfinite(f->display)) fprintf(out, "%.6f", f->display);
        fprintf(out, ",%d,%s", f->cause >= 0, f->cause >= 0 ? reasons[f->cause] : "");
        for (int k = 0; k < BP_COUNT; k++) fprintf(out, ",%llu", (unsigned long long)f->c[k]);
        fputc('\n', out);
    }
    if (fclose(out)) FATAL("benchmark: frames.csv write failed\n");
    pthread_mutex_unlock(&lock);
}
static void number(FILE *f, double x) { if (isfinite(x)) fprintf(f, "%.6f", x); else fputs("null", f); }
static void json_pct(FILE *f, const char *key, BenchPct p) {
    fprintf(f, "\"%s\":{\"samples\":%d,\"p50\":", key, p.samples); number(f, p.p50);
    fputs(",\"p95\":", f); number(f, p.p95); fputs(",\"p99\":", f); number(f, p.p99); fputs("},", f);
}
void bench_trace_json(FILE *f, const BenchStats *s) {
    fprintf(f, "\"budget_ms\":%.6f,\"over_budget\":%d,", BUDGET, s->long_frames);
    json_pct(f, "headroom_ms", s->headroom);
    fputs("\"presentation\":{\"source\":\"VK_GOOGLE_display_timing\",", f);
    json_pct(f, "period_ms", s->display);
    fprintf(f, "\"frames\":%d,\"missing_feedback\":%d,\"fps\":", s->display_frames, s->missing_display);
    number(f, s->display_fps);
    fputs(",\"low_1pct_fps\":", f); number(f, s->display_low1);
    fputs(",\"low_01pct_fps\":", f); number(f, s->display_low01);
    fprintf(f, ",\"over_budget\":%d,\"over_50ms\":%d},\"long_frame_causes\":{", s->display_over_budget, s->display_over50);
    for (int k = 0; k < BENCH_REASONS; k++) fprintf(f, "%s\"%s\":%d", k ? "," : "", reasons[k], s->causes[k]);
    fputs("},\"window_counters\":{", f);
    for (int k = 0; k < BP_COUNT; k++) fprintf(f, "%s\"%s\":%llu", k ? "," : "", counters[k], (unsigned long long)s->totals[k]);
    fputs("},", f);
}
