// No game files: classification, frame identity across the render queue, missing feedback and window isolation.
#include <assert.h>
#include <stdarg.h>
#include "../runtime/hle/benchmark_trace.c"
bool g_benchmark;
static int clock_reads;
uint64_t plat_now_ns(void) { clock_reads++; return 123; }
void rt_fatal_message(const char *format, ...) { (void)format; abort(); }
int main(void) {
    int evaluated = 0;
    bench_add(BP_BUILDS, ++evaluated);
    bench_add(BP_BUILD_NS, plat_now_ns());
    assert(!evaluated && !clock_reads && !atomic_load(&g_bench_perf[BP_BUILD_NS]));
    bench_add(BP_BUILDS, 9); assert(!atomic_load(&g_bench_perf[BP_BUILDS])); assert(!bench_clock());
    assert(bench_cooling_check(1, 0, 0, 10) == 1);
    assert(bench_cooling_check(1, 2, 9.9, 10) == 0);
    assert(bench_cooling_check(1, 2, 10, 10) == -2);
    assert(bench_cooling_check(1, -1, 0, 10) == -1);
    assert(!clock_reads);
    g_benchmark = true;
    bench_add(BP_BUILDS, ++evaluated);
    assert(evaluated == 1 && atomic_load(&g_bench_perf[BP_BUILDS]) == 1);
    TraceFrame f = {.ms = {50}};
    assert(cause(&f, 0) == 7);
    f.c[BP_GPU_WAIT_NS] = 12000000; assert(cause(&f, 10) == 7); assert(cause(&f, 0) == 1);
    f.c[BP_BUILD_WAIT_NS] = 15000000; assert(cause(&f, 0) == 0);
    f.c[BP_BC_NS] = 20000000; f.c[BP_TEX_NS] = 22000000; assert(cause(&f, 0) == 2);
    f = (TraceFrame){.ms = {50}, .c = {[BP_BUILDS] = 2}}; assert(cause(&f, 0) == 6);
    f.c[BP_EVICT_NS] = 10000000; assert(cause(&f, 0) == 5);
    f.ms[0] = 1000.0/30; assert(cause(&f, 0) == -1);
    bench_trace_begin();
    bench_handoff(100); bench_add(BP_FILE_BYTES, 999); bench_trace_frame("warmup", false, 50, 10, 10, 8, 25);
    bench_handoff(200); bench_add(BP_BUILDS, 2); bench_trace_frame("window", true, 50, 36, 36, 10, 35);
    bench_handoff(300); bench_trace_frame("window", true, 40, 10, 10, 8, -1);
    bench_handoff(400); bench_trace_frame("window", true, 40, 10, 10, 8, 25);
    bench_present_queue(11, 200); bench_present_queue(12, 300); bench_present_queue(14, 400);
    bench_present_actual(12, 1050000000); bench_present_actual(11, 1000000000); bench_present_actual(14, 1100000000);
    assert(trace[1].actual == 1000000000 && trace[2].actual == 1050000000);
    assert(trace[1].headroom < 0 && isnan(trace[2].headroom));
    char dir[] = "/data/local/tmp/bench-trace-XXXXXX"; assert(mkdtemp(dir));
    BenchStats s; bench_trace_finish(dir, "test", 0, &s);
    assert(s.long_frames == 3 && s.causes[6] == 1 && s.causes[7] == 2);
    assert(s.totals[BP_BUILDS] == 2 && s.totals[BP_FILE_BYTES] == 0);
    assert(s.display.samples == 1 && fabs(s.display.p50 - 50) < 1e-9);
    assert(s.headroom.samples == 2 && s.missing_display == 0 && isnan(trace[3].display));
    char path[256]; snprintf(path, sizeof path, "%s/frames.csv", dir); unlink(path); rmdir(dir);
    // A new boot must discard previous presentation IDs and never reuse stale actual timestamps.
    bench_trace_begin(); bench_handoff(500); bench_trace_frame("window", true, 40, 10, 10, 8, 25);
    bench_present_actual(11, 1200000000); assert(!trace[0].actual);
    // Synchronous rendering can deliver feedback before the game records its frame.
    bench_trace_begin();
    bench_present_queue(21, 600); bench_present_actual(21, 2000000000);
    bench_handoff(600); bench_trace_frame("window", true, 40, 10, 10, 8, 25);
    assert(trace[0].actual == 2000000000 && trace[0].present_id == 21);
    // Reused ring slots must not inherit an old presentation timestamp.
    bench_present_queue(277, 700);
    bench_handoff(700); bench_trace_frame("window", true, 40, 10, 10, 8, 25);
    assert(!trace[1].actual);
    bench_present_actual(277, 2040000000); assert(trace[1].actual == 2040000000);
    puts("benchmark trace PASS");
    return 0;
}
