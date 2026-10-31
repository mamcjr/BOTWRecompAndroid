// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../hle.h"
#include "../../kernel.h"
#include "shaders.h"
#include "vk.h"
#include "timing_trace.h"
#include "../renderer.h"
#include "../../settings.h"
#include "../../paths.h"

// LARECOMP_AB (vk_ab, below): AB() costs no call while it is not set - it is asked per constant buffer and per pass
static int ab_n = -1;  // (LARECOMP_AB's frame count; 0: no A/B)
int vk_ab(const char *what);
#define AB(what) (ab_n == 0 ? -1 : vk_ab(what))
static uint64_t hash_words(uint64_t h, const void *p, size_t n);

static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
static uint64_t stat_feedback_reads;  // (attachments sampled as they are: only read, resolve_feedback)
static uint64_t stat_inplace_breaks;  // (passes ended so a draw reads a target the pass wrote: resolve_feedback)
static uint64_t stat_upload_bytes;
static uint64_t stat_cb_bytes[2], stat_cb_n[2], stat_cb_copied, stat_cbwin;  // (LARECOMP_LOG_CBUF_BYTES: game constant buffers bound,
                                                                 // fixed [0] / dynamically indexed [1], and bytes copied)
static uint64_t stat_pipe_binds, stat_set_binds;  // (graphics draws: pipelines / descriptor sets bound)
// LARECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_dsw, prof_cbuf, prof_set_hits, prof_set_miss;  // (sets: driver writes, cbuf copies, set cache)
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
static VkQueryPool prof_qp;
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
#ifdef __ANDROID__
uint64_t vk_cum_wait_ns(void) { return cum_wait; }  // (the performance overlay: time spent waiting on the GPU)
#endif
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
uint64_t prof_up_cum;  // (the same, never reset)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
static uint64_t op_counts[CMD_COUNT];
bool g_debug_frame;
static char stats_text[2048];
void vk_collect_dead(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#ifdef __ANDROID__
#define SUBS 16  // (LARECOMP_VK_ASYNC submits a frame in chunks: more submissions in flight)
#else
#define SUBS 8
#endif
typedef struct {
    VkCommandBuffer main, upload;
    VkCommandPool cpool;  // (their own pool, reset whole when the slot is reused - begin_cb)
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    uint64_t seq;
    VkDescriptorPool pools[16];
    int npools, cur_pool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static void ss_forget(void);
static uint64_t submit_seq;  // last submitted
#ifdef __ANDROID__
// The last submission with the game's commands in it (backend_execute's), and whether some are not submitted yet.
// The game's fences (nvnQueueFenceSync) right after its submission went to the next one - the window's present,
// which waits for a window image: its waits followed the compositor (SyncWait 8-17 ms a frame) rather than its own
// GPU work, all of which is ahead of the present in the queue. LARECOMP_FENCE_PRESENT=1: as before.
static uint64_t work_seq;
static bool game_pending;
static uint64_t last_submit_ns;  // (begin_pass: LARECOMP_VK_CHUNK_US)
#endif
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;
static uint32_t pass_written;  // (the attachments this pass's draws wrote so far: draw_writes)
static uint32_t pass_dup_null;  // (color slots left unbound: their image is bound in another slot - begin_pass)

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

// (LARECOMP_VK_PROF) waits on the GPU by cause: the caller names it in wait_why just before waiting
static __thread const char *wait_why;
static struct { const char *why; uint64_t ns, n; } wait_tab[12];
static pthread_mutex_t wait_tab_lock = PTHREAD_MUTEX_INITIALIZER;
static void wait_account(uint64_t ns) {
    const char *why = wait_why ? wait_why : "other";
    wait_why = NULL;
    pthread_mutex_lock(&wait_tab_lock);
    for (int i = 0; i < 12; i++)
        if (!wait_tab[i].why || wait_tab[i].why == why) { wait_tab[i].why = why; wait_tab[i].ns += ns; wait_tab[i].n++; break; }
    pthread_mutex_unlock(&wait_tab_lock);
}

static void cq_resolve_done(uint64_t done);
static void wb_apply(uint64_t done);

#ifdef __ANDROID__
static bool gcopy_mode;   // (no heap import: global memory copied by the CPU, gcopy_prepare)
#ifdef __ANDROID__
static bool gwin_mode;    // (heap placed, larger than a storage buffer reaches: draws get a window of it, gwin_prepare)
#endif
static VkSpan gc_span;    // this draw's / dispatch's copy, gc_size bytes (0: none)
static uint64_t gc_size;
static void gcopy_retire(uint64_t done);
static void ga_flush_dirty(void);
static void ga_invalidate(const void *p, uint64_t len);
#endif

void vk_wait_seq(uint64_t seq) {
    if (!seq) { wait_why = NULL; return; }
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    stat_wait_ns += SDL_GetTicksNS() - t0;
    cum_wait += SDL_GetTicksNS() - t0;
    wait_account(SDL_GetTicksNS() - t0);
#ifdef __ANDROID__
    if (vk_timing_trace_on()) vk_timing_trace_wait(VK_TIMING_TIMELINE, SDL_GetTicksNS() - t0);
#endif
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r == VK_SUCCESS) cq_resolve_done(seq);  // (the game reads its timing counters after waiting)
#ifdef __ANDROID__
    if (r == VK_SUCCESS) gcopy_retire(seq);
#endif
    else {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }
// The submission holding the last draw or dispatch that stores to global memory - the game's own memory on the GPU
// (vk_heap_place): what the CPU may read after a fence (nvnSyncWait's sync lag waits for it). Render thread writes.
static _Atomic uint64_t g_gmem_store_seq;
uint64_t vk_last_gmem_store_seq(void) { return atomic_load(&g_gmem_store_seq); }
static void note_gmem_store(int stage);  // (below: it reads the bound storage buffers)

// ---- the game's GPU timing counters (nvnCommandBufferReportCounter, two per frame)
// The game times its GPU work with these and drops to 30 fps when frames look slow. They used to get the host clock
// at translation time, so our command translation counted as GPU time: busy scenes (outdoors, ~400 draws) looked slow
// and the game locked itself at 30 even on fast GPUs, at any resolution. Now each counter also records a GPU
// timestamp at its place in the command stream; once that submission is done (the game waits on its fence before
// reading - on the Switch the GPU writes them too) the GPU time replaces the provisional host value, in the host
// clock's terms (offset calibrated at each resolve: a timestamp can't be later than the moment its submission is seen
// done). LARECOMP_VK_COUNTER=cpu: host clock only; =frozen: one value per presented frame (test); =legacy: the host
// clock in ns instead of GPU ticks, as before (test: reproduces the locked 30).
#define CQ_MAX 64
#ifdef __ANDROID__
// (Android) The game's pair of counters spans several of our submissions, and the GPU idles between them while the
// CPU translates the next: that idle counted as GPU time (copy mode outdoors: ~15 ms a pair, the game's 30 fps
// mode). So each submission's start and end are timestamped too (queries CQ_MAX, CQ_MAX + 1), and a pair's end
// counter gets its start's value plus the GPU's busy time in between - the time the Switch's GPU would show. Our
// own slowness is the pacer's business (vk_pace_frame: busy time per presented frame). LARECOMP_COUNTER_IDLE=1: as
// before.
#define CQ_QUERIES (CQ_MAX + 2)
static _Atomic uint64_t gpu_busy_total;  // (GPU ns working, all submissions so far: ADPF's GPU time per frame)
static double g_pace_gpu_ms;  // (the last presented frame's GPU busy time: LARECOMP_LOG_PACE)
#else
#define CQ_QUERIES CQ_MAX
#endif
typedef struct {
    VkQueryPool pool; uint32_t n; uint64_t seq; uint64_t *out[CQ_MAX]; bool ts;
#ifdef __ANDROID__
    uint64_t frame[CQ_MAX], host_ns[CQ_MAX];  // opt-in diagnostics: identity at command execution, not query resolution
    bool presentation;
#endif
} CounterQ;
static CounterQ cq[SUBS];
static pthread_mutex_t cq_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t cq_offset;  // host ns - GPU ns (the smallest seen)
// Counter timestamps are in the Switch GPU's ticks (614.4 MHz): the game converts the difference to ns itself
// (x 625 / 384). Writing ns made every GPU time look 1.63x as long.
static uint64_t ns_to_gpu_ticks(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * 384 / 625); }
static bool cq_calibrated;

#ifdef __ANDROID__
// ---- LARECOMP_FRAME_TIMELINE=<frame>: four frames from that one, every step in time order - the game's lists, fences,
// waits and presents, the render thread's translation and submissions, the GPU's start and end of each submission (its
// clock moved onto the CPU's: cq_offset) - in ms from the first. Where a frame's time goes between the three.
uint64_t renderer_present_count(void);
typedef struct { int64_t t; const char *what; uint64_t a, b; } TlEv;
static TlEv tl_ev[8192];
static int tl_n;
static long tl_from = -2;
static pthread_mutex_t tl_lock = PTHREAD_MUTEX_INITIALIZER;
static int tl_cmp(const void *x, const void *y) {
    const TlEv *p = x, *q = y;
    return p->t < q->t ? -1 : p->t > q->t;
}
static void tl_add(int64_t t, const char *what, uint64_t a, uint64_t b) {
    if (tl_from == -2) tl_from = getenv("LARECOMP_FRAME_TIMELINE") ? atol(getenv("LARECOMP_FRAME_TIMELINE")) : -1;
    if (tl_from < 0) return;
    uint64_t f = renderer_present_count();
    if ((long)f < tl_from) return;
    pthread_mutex_lock(&tl_lock);
    if ((long)f >= tl_from + 6 && tl_n) {  // (two frames' margin for the GPU's late timestamps)
        qsort(tl_ev, tl_n, sizeof *tl_ev, tl_cmp);
        LOG("[timeline] frames %ld-%ld, %d events (ms from the first)\n", tl_from, tl_from + 3, tl_n);
        for (int i = 0; i < tl_n; i++)
            LOG("[timeline] %8.3f %s %llu %llu\n", (tl_ev[i].t - tl_ev[0].t) / 1e6, tl_ev[i].what,
                (unsigned long long)tl_ev[i].a, (unsigned long long)tl_ev[i].b);
        tl_n = 0;
        tl_from = -1;
    } else if ((long)f < tl_from + 4 && tl_n < 8192) {
        tl_ev[tl_n++] = (TlEv){t, what, a, b};
    }
    pthread_mutex_unlock(&tl_lock);
}
void vk_tl(const char *what, uint64_t a, uint64_t b) { tl_add((int64_t)plat_now_ns(), what, a, b); }
#endif
static int counter_mode = -1;  // 0 cpu, 1 frozen, 2 gpu
static uint64_t stat_counters_gpu;

static int vk_counter_mode(void) {
    if (counter_mode < 0) {
        const char *e = getenv("LARECOMP_VK_COUNTER");
        counter_mode = e && !strcmp(e, "cpu") ? 0 : e && !strcmp(e, "frozen") ? 1 : e && !strcmp(e, "legacy") ? 3 : 2;
        if (counter_mode == 2 && !g_vk.props.limits.timestampComputeAndGraphics) counter_mode = 0;
    }
    return counter_mode;
}

// ---- frame pacing: the game's own 30 fps mode when we can't hold 60
// The game advances its world one fixed step per frame in its 60 fps mode, and two steps per frame in its 30 fps mode
// (measured: Link walks twice as far per frame there) - so a 60-mode frame that takes 20 ms plays in slow motion, while
// the 30 mode plays at full speed. The game picks its mode from these counters (GPU time per frame), as on the Switch;
// but our frames can be slow for reasons its GPU time doesn't show (command translation, a slower CPU). So each
// presented frame's busy time is measured (present to present, minus the time the presenting thread was blocked in
// kernel waits and sleeps: idle - the game waits for vsync through its own threads); when
// frames don't fit 60 (12 of the last 60 over 16.2 ms), the counters report a frame of at least PACE_SLOW_NS: the game
// takes its 30 fps mode, at full speed. Back to the real times after 2 s with headroom (busy under 13 ms on average,
// at most 2 of the last 60 frames over). The game itself also tries 60 again now and then.
// LARECOMP_PACE=0: off.
#define PACE_SLOW_NS 20000000ull
extern __thread uint64_t g_blocked_ns;  // kernel.c: this thread's time blocked in waits / sleeps
// Written by the game's presenting thread, read while resolving GPU counters on the render thread.
static _Atomic bool pace_slow;
static uint64_t stat_pace_over;
static _Atomic int pace_on = -1;

// The last presented frame's GPU busy time in ms, -1 unknown (Video > Internal resolution > Dynamic: output_scale.c)
double vk_frame_gpu_ms(void) {
#ifdef __ANDROID__
    return g_pace_gpu_ms > 0 ? g_pace_gpu_ms : -1;
#else
    return -1;
#endif
}

void vk_pace_frame(void) {
    {   // (write-backs of passes the GPU has finished since: the game may read them this frame)
        uint64_t v = 0;
        if (vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v) == VK_SUCCESS) wb_apply(v);
    }
    if (pace_on < 0) { const char *e = getenv("LARECOMP_PACE"); pace_on = !e || atoi(e) != 0; }
    static uint64_t last, last_wait;
    static uint8_t over[60];
    static int idx, nover, calm;
    static double avg;  // busy ms, moving average (~20 frames)
#ifdef __ANDROID__
    static uint64_t slow_since, probe_at, probe_wait = 10000000000ull;
    static int last_mode = -1, last_resolution = -1;
    // A failed trial describes the old workload. Reconsider Auto immediately when the user changes resolution
    // or returns from a fixed FPS mode; otherwise its old exponential backoff can keep the new workload at 30
    // for up to 80 seconds. Keep ordinary hitches from restarting trials (and causing repeated oscillation).
    bool changed = last_mode != g_settings.fps_mode || last_resolution != g_settings.res_mode;
    last_mode = g_settings.fps_mode;
    last_resolution = g_settings.res_mode;
    if (changed) {
        memset(over, 0, sizeof over);
        idx = nover = calm = 0;
        avg = 0;
        slow_since = probe_at = 0;
        probe_wait = 10000000000ull;
        pace_slow = pace_on && g_settings.fps_mode == 1;
        last = 0;  // discard the transition frame's timing, including menu time and resource reallocation
        LOG("[pace] settings changed: fresh %s pacing history\n", g_settings.fps_mode ? "fixed" : "auto");
    }
#endif
    uint64_t now = plat_now_ns(), w = g_blocked_ns;
    uint64_t p = now - last, wt = w - last_wait;
    bool first = !last;
    last = now, last_wait = w;
    if (first || p > 100000000ull) return;  // (loading, a hitch: not a pace)
    double busy = (p > wt ? p - wt : 0) / 1e6;
#ifdef __ANDROID__
    // LARECOMP_VK_ASYNC: the frame is two threads' work - the game's (busy) and the render thread's (its last frame)
    extern bool vk_async_on(void);
    extern uint64_t vk_async_frame_work_ns(void);
    double render_busy = vk_async_on() ? vk_async_frame_work_ns() / 1e6 : 0;
    double game_busy = busy;
    if (render_busy > busy) busy = render_busy;
#endif
#ifdef __ANDROID__
    // (ADPF: clocks for 60 fps even in the 30 fps mode - with that mode's 33 ms as the target, a 21 ms frame looked
    // comfortable, the clocks stayed low and so did the mode)
    static int follow = -1;  // LARECOMP_ADPF_FOLLOW=1: the old target (the mode's own)
    if (follow < 0) follow = getenv("LARECOMP_ADPF_FOLLOW") != NULL;
    {   // LARECOMP_ADPF_GPU=1 (a test): the GPU's time too (API 35+), so its governor would raise its clock for 60 fps
        // as well. Accepted on the Snapdragon 8 Elite (2026-10-16) but nothing measurable changed: off by default.
        static int no_gpu = -1;
        if (no_gpu < 0) no_gpu = getenv("LARECOMP_ADPF_GPU") == NULL;
        static uint64_t last_busy;
        uint64_t total = gpu_busy_total, gpu = total - last_busy;
        last_busy = total;
        g_pace_gpu_ms = gpu / 1e6;
        // (per presented frame, against 60 fps's budget in either mode: a 30 fps frame renders one image too - its GPU
        // time is what a 60 fps frame would take, and halved it looked comfortable: the clock stayed low)
        // The render hint defaults to the established wall-time signal; CPU-only timing is opt-in.
        extern uint64_t vk_async_frame_cpu_ns(void);
        double hint_busy = game_busy;
        double render_cpu = vk_async_on() ? vk_async_frame_cpu_ns() / 1e6 : 0;
        if (render_cpu > hint_busy) hint_busy = render_cpu;
        plat_frame_work_gpu((uint64_t)(hint_busy * 1e6), no_gpu ? 0 : gpu, follow && pace_slow ? 33333333ull : 16666667ull,
                            (uint64_t)(game_busy * 1e6));
    }
#endif
#ifdef __ANDROID__
    // Per world step: the game picks its present interval frame by frame (1: one step, 60 fps; 2: two steps, 30 fps),
    // and a 2-step frame's ~19 ms counted as a 60 fps frame over its budget - the pacer forced the 30 fps mode, which
    // made the game choose more 2-step frames, which kept it forced (2026-10-16, standing still in Mabe Village: left
    // alone the game kept 60 in 96% of its frames, forced 32% were at 30). Only a 1-step frame over 16.2 ms is slow
    // motion; the average is per step. LARECOMP_PACE_PER_FRAME=1: as before.
    extern int g_present_interval;
    static int per_frame = -1;
    if (per_frame < 0) per_frame = getenv("LARECOMP_PACE_PER_FRAME") != NULL;
    int steps = per_frame || g_present_interval < 1 ? 1 : g_present_interval;
    double step_busy = busy / steps;
    avg = avg ? avg + (step_busy - avg) * 0.05 : step_busy;
#else
    avg = avg ? avg + (busy - avg) * 0.05 : busy;
#endif
    static int log = -1;  // LARECOMP_LOG_PACE=1: each frame's period, vsync wait and busy time
    if (log < 0) log = getenv("LARECOMP_LOG_PACE") != NULL;
    if (log) {  // (with LARECOMP_VK_PROF also the frame's command translation and, of it, texture checks / uploads)
        static uint64_t le, lu;
#ifdef __ANDROID__
        extern int g_present_interval;
        extern int vk_ab(const char *what);
        LOG("[pace] period %.2f blocked %.2f busy %.2f ms exec %.2f tex %.2f interval %d slow %d game %.2f render %.2f gpu %.2f ab %d\n",
            p / 1e6, wt / 1e6, busy, (cum_exec - le) / 1e6, (prof_up_cum - lu) / 1e6, g_present_interval, pace_slow,
            game_busy, render_busy, g_pace_gpu_ms, vk_ab(NULL));
#else
        LOG("[pace] period %.2f blocked %.2f busy %.2f ms exec %.2f tex %.2f\n", p / 1e6, wt / 1e6, busy,
            (cum_exec - le) / 1e6, (prof_up_cum - lu) / 1e6);
#endif
        le = cum_exec, lu = prof_up_cum;
    }
#ifdef __ANDROID__
    // (or one that missed its vsync, whatever held it: with the render thread both halves can fit and the frame still
    // take two vsyncs - slow motion all the same)
    int o = steps == 1 && (busy > 16.2 || p > 20000000ull);
    // Low-volume diagnostics: distinguish real work from a late presentation without per-frame logcat traffic.
    // Deliberately independent of ADPF and of the decision below.
    static int trace = -1;
    if (trace < 0) { const char *e = getenv("LARECOMP_PACE_TRACE"); trace = e && atoi(e) != 0; }
    if (trace) {
        static uint64_t since;
        static int n, one, late_only, work_over, slow_n;
        static double game_sum, render_sum, period_sum, blocked_sum, busy_max;
        if (!since) since = now;
        n++; one += steps == 1; slow_n += pace_slow;
        late_only += steps == 1 && p > 20000000ull && busy <= 16.2;
        work_over += steps == 1 && busy > 16.2;
        game_sum += game_busy; render_sum += render_busy;
        period_sum += p / 1e6; blocked_sum += wt / 1e6;
        if (busy > busy_max) busy_max = busy;
        if (now - since >= 2000000000ull) {
            LOG("[pace-trace] n %d one %d late-only %d work-over %d forced30 %d period %.2f blocked %.2f game %.2f render %.2f max %.2f avg-step %.2f over %d retry %.0f\n",
                n, one, late_only, work_over, slow_n, period_sum / n, blocked_sum / n, game_sum / n,
                render_sum / n, busy_max, avg, nover, probe_wait / 1e9);
            since = now;
            n = one = late_only = work_over = slow_n = 0;
            game_sum = render_sum = period_sum = blocked_sum = busy_max = 0;
        }
    }
#else
    int o = busy > 16.2;
#endif
    stat_pace_over += o;
    nover += o - over[idx];
    over[idx] = (uint8_t)o;
    idx = (idx + 1) % 60;
    if (!pace_on) return;  // (measured and counted all the same: LARECOMP_LOG_PACE, the stats)
#ifdef __ANDROID__
    if (g_settings.fps_mode) {  // (Video: Frame rate 30 / 60) fixed: no measuring, no trials
        bool slow = g_settings.fps_mode == 1;
        if (slow != pace_slow) LOG("[pace] frame rate set to %d fps\n", slow ? 30 : 60);
        pace_slow = slow;
        return;
    }
#endif
#ifdef __ANDROID__
    // Trials: a 30 fps frame carries more of the game's CPU work than a 60 fps one, so the headroom test below rarely
    // passes there - one hitch (a pipeline compile) kept the overworld at 30 for good. Every probe_wait seconds at
    // 30 the game's own choice comes back for a trial; frames that don't fit within 3 s bring the 30 back (each
    // failed trial doubles the wait, up to 80 s; a trial that holds resets it).
    if (pace_slow && !slow_since) slow_since = now;
    if (probe_at && now - probe_at < 1000000000ull) nover = 0, memset(over, 0, sizeof over);  // (the game switching)
    if (probe_at && !pace_slow && now - probe_at >= 3000000000ull) probe_at = 0, probe_wait = 10000000000ull;
#endif
    if (!pace_slow && nover >= 12) {
        pace_slow = true, calm = 0;
        LOG("[pace] frames don't fit 60 fps (%d of the last 60 over 16.2 ms busy): the game's 30 fps mode\n", nover);
#ifdef __ANDROID__
        if (probe_at && now - probe_at < 3000000000ull && probe_wait < 80000000000ull) probe_wait *= 2;
        probe_at = 0;
        slow_since = now;
#endif
    } else if (pace_slow) {
        // headroom: busy well under a 60 fps frame on average, hardly a frame over (single spikes are normal play)
        calm = avg < 13.0 && nover <= 2 ? calm + 1 : 0;
#ifdef __ANDROID__
        // (headroom at 30 doesn't mean 60 fits: in the overworld this went back and forth every few seconds, slow
        // motion each time - it is a trial too, so a quick failure doubles the wait, and it waits a quarter of it)
        if (calm >= 120 && now - slow_since >= probe_wait / 4) {
            pace_slow = false, nover = 0;
            memset(over, 0, sizeof over);
            probe_at = now;
            LOG("[pace] 120 frames with headroom: back to the game's own choice\n");
        }
#else
        if (calm >= 120) {
            pace_slow = false, nover = 0;
            memset(over, 0, sizeof over);
            LOG("[pace] 2 s with headroom: back to the game's own choice\n");
        }
#endif
#ifdef __ANDROID__
        else if (now - slow_since >= probe_wait) {
            pace_slow = false, nover = 0;
            memset(over, 0, sizeof over);
            probe_at = now;
            LOG("[pace] trial: the game's own choice for a while (after %.0f s at 30 fps)\n", probe_wait / 1e9);
        }
        if (!pace_slow) slow_since = 0;
#endif
    }
}

// a pair's end counter (the 16 bytes after its start) is at least PACE_SLOW_NS after it while pace_slow
#ifdef __ANDROID__
// LARECOMP_FAST_COUNTERS=1 (a test): at most PACE_FAST_NS after it otherwise, so the game always tries its 60 fps
// mode and the pacer alone brings the 30 back. In the overworld its 60 fps frames averaged 10.5 ms busy, but 4.5%
// went over 16 ms in bursts: slow motion (the game steps its world once per frame), which the game's own threshold
// (it keeps 30 there at ~13-14 ms of GPU a frame) avoids. So by default the game chooses from the real times.
#define PACE_FAST_NS 8000000ull
#endif
static uint64_t pace_end_counter(const uint64_t *start_entry, const uint64_t *entry, uint64_t value) {
    if (!start_entry || (const uint8_t *)entry - (const uint8_t *)start_entry != 16) return value;
#ifdef __ANDROID__
    static int fast = -1;
    if (fast < 0) fast = getenv("LARECOMP_FAST_COUNTERS") ? atoi(getenv("LARECOMP_FAST_COUNTERS")) : 0;
    if ((!pace_slow && pace_on > 0 && (fast || g_settings.fps_mode == 2)) || fast == 2) {  // (2: with LARECOMP_PACE=0 too, a test)
        uint64_t max = start_entry[1] + ns_to_gpu_ticks(PACE_FAST_NS);
        return value > max ? max : value;
    }
#endif
    if (!pace_slow) return value;
    uint64_t min = start_entry[1] + ns_to_gpu_ticks(PACE_SLOW_NS);
    return value < min ? min : value;
}

static void cq_resolve(int i) {  // (cq_lock held) submission slot i is done: its counters get their GPU times
    CounterQ *q = &cq[i];
#ifdef __ANDROID__
    static int keep_idle = -1;
    if (keep_idle < 0) keep_idle = getenv("LARECOMP_COUNTER_IDLE") != NULL;
    static int64_t last_end = -1, idle;  // (GPU ns: the previous submission's end; the idle between submissions so far)
    if (q->ts) {
        q->ts = false;
        uint64_t se[2];
        VkResult result = vkGetQueryPoolResults(g_vk.dev, q->pool, CQ_MAX, 2, sizeof se, se, 8, VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS) {
            double per = g_vk.props.limits.timestampPeriod;
            int64_t s0 = (int64_t)((double)se[0] * per), s1 = (int64_t)((double)se[1] * per);
            if (last_end >= 0 && s0 > last_end) idle += s0 - last_end;
            if (cq_calibrated) {
                tl_add(s0 + cq_offset, "gpu: start of submission", q->seq, q->presentation);
                tl_add(s1 + cq_offset, "gpu: end of submission", q->seq, q->presentation);
            }
            {   // LARECOMP_LOG_GPU_IDLE=1: the GPU's idle between submissions, every ~2 s - after a frame's present (the
                // frame boundary) and inside a frame (the render thread's next chunk late), per frame and the largest
                static int on = -1;
                static bool prev_present;
                static int64_t gi_after, gi_within, gi_max, gi_t0;
                static int gi_frames, gi_big;
                if (on < 0) on = getenv("LARECOMP_LOG_GPU_IDLE") != NULL;
                if (on && last_end >= 0) {
                    int64_t gap = s0 > last_end ? s0 - last_end : 0;
                    if (prev_present) gi_after += gap, gi_frames++;
                    else gi_within += gap;
                    if (gap > gi_max) gi_max = gap;
                    gi_big += gap > 1000000;
                    if (!gi_t0) gi_t0 = s0;
                    if (s0 - gi_t0 > 2000000000 && gi_frames) {
                        LOG("[gpu-idle] per frame: %.2f ms after the present, %.2f ms within the frame (%d frames, %d gaps "
                            "over 1 ms, largest %.2f ms)\n", gi_after / 1e6 / gi_frames, gi_within / 1e6 / gi_frames, gi_frames,
                            gi_big, gi_max / 1e6);
                        gi_after = gi_within = gi_max = 0, gi_frames = gi_big = 0, gi_t0 = s0;
                    }
                }
                prev_present = q->presentation;
            }
            if (s1 > s0) {
                uint64_t ns = (uint64_t)(s1 - (s0 > last_end ? s0 : last_end > s1 ? s1 : last_end));
                gpu_busy_total += ns;
                vk_timing_trace_submission(ns, q->presentation);
            }
            if (s1 > last_end) last_end = s1;
        } else vk_timing_trace_query_failure(result, 0, q->seq);
    }
#endif
    if (!q->n) return;
    uint64_t t[CQ_MAX];
    VkResult result = vkGetQueryPoolResults(g_vk.dev, q->pool, 0, q->n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT);
    if (result == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod;
        int64_t cand = (int64_t)plat_now_ns() - (int64_t)((double)t[q->n - 1] * per);
        if (!cq_calibrated || cand < cq_offset) cq_offset = cand, cq_calibrated = true;
        static int log = -1;  // LARECOMP_LOG_COUNTER=1: the host (provisional) and GPU times of each counter
        if (log < 0) log = getenv("LARECOMP_LOG_COUNTER") != NULL;
        for (uint32_t k = 0; k < q->n; k++) {
            uint64_t gpu = ns_to_gpu_ticks((uint64_t)((int64_t)((double)t[k] * per) + cq_offset));
            const uint64_t *start = k ? q->out[k - 1] : NULL;
#ifdef __ANDROID__
            bool idle_matched = false;
            {   // a pair's end (16 bytes after a start seen before): the start's value + the busy time in between
                static struct { uint64_t *out; uint64_t val; int64_t raw, idle; } seen[4];
                static int nseen;
                int64_t raw = (int64_t)((double)t[k] * per);
                for (int j = 0; j < 4 && !keep_idle; j++)
                    if (seen[j].out && seen[j].out + 2 == q->out[k]) {
                        int64_t busy = raw - seen[j].raw - (idle - seen[j].idle);
                        uint64_t v = seen[j].val + ns_to_gpu_ticks((uint64_t)(busy > 0 ? busy : 0));
                        if (v < gpu) gpu = v;
                        start = seen[j].out;
                        idle_matched = true;
                        break;
                    }
                seen[nseen & 3].out = q->out[k];
                seen[nseen & 3].val = gpu;
                seen[nseen & 3].raw = raw;
                seen[nseen & 3].idle = idle;
                nseen++;
            }
#endif
            if (log) LOG("[counter] seq %llu #%u at %llx host %.3f gpu %.3f ms\n", (unsigned long long)q->seq, k,
                         (unsigned long long)(uintptr_t)q->out[k], q->out[k][1] * 625.0 / 384 / 1e6, gpu * 625.0 / 384 / 1e6);
            uint64_t delivered = pace_end_counter(start, q->out[k], gpu);
#ifdef __ANDROID__
            if (vk_timing_trace_on()) {
                VkTimingCounter sample = {.address = (uint64_t)(uintptr_t)q->out[k], .frame = q->frame[k],
                    .seq = q->seq, .host_ns = q->host_ns[k], .raw_ns = (int64_t)((double)t[k] * per), .idle_ns = idle,
                    .gpu_ticks = gpu, .delivered_ticks = delivered, .idle_matched = idle_matched};
                vk_timing_trace_counter(&sample);
            }
#endif
            q->out[k][1] = delivered;
        }
        stat_counters_gpu += q->n;
    }
#ifdef __ANDROID__
    else vk_timing_trace_query_failure(result, q->n, q->seq);
#endif
    q->n = 0;
}

static void cq_resolve_done(uint64_t done) {  // every slot whose submission is done
    wb_apply(done);
    pthread_mutex_lock(&cq_lock);
#ifdef __ANDROID__
    for (;;) {  // oldest first
        int best = -1;
        for (int i = 0; i < SUBS; i++)
            if ((cq[i].n || cq[i].ts) && cq[i].seq <= done && (best < 0 || cq[i].seq < cq[best].seq)) best = i;
        if (best < 0) break;
        cq_resolve(best);
        cq[best].ts = false;
    }
#else
    for (int i = 0; i < SUBS; i++) if (cq[i].n && cq[i].seq <= done) cq_resolve(i);
#endif
    pthread_mutex_unlock(&cq_lock);
}

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        wait_why = "ring slot";
        vk_wait_seq(s->seq);
        cq_resolve_done(s->seq);
        if (s->ts) {  // (profiling) GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
                prof_gpu += (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
            s->ts = false;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        if (s->cpool) vkResetCommandPool(g_vk.dev, s->cpool, 0);
        s->cur_pool = 0;
        s->seq = submit_seq + 1;
        s->main_begun = s->upload_begun = false;
    }
    return s;
}

// A submission slot's command buffers come from its own pool, reset whole (keeping its memory) when the slot is reused
// (sub). Begun again instead, each buffer reset itself implicitly: Qualcomm's driver then gave its memory back and
// allocated it again while recording (vkBeginCommandBuffer ~6% of the render thread, plus malloc / munmap).
static void begin_cb(Sub *s, VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!s->cpool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_vk.qfam};
        VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &s->cpool));
    }
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->cpool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    *begun = true;
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
static void upload_begin_barrier(VkCommandBuffer cb) {
#ifdef __ANDROID__
    static int narrow = -1;
    if (narrow < 0) {
        const char *e = getenv("LARECOMP_VK_UPLOAD_BARRIER");
        narrow = e && atoi(e) != 0;  // experimental: no repeatable frame-time benefit established yet
    }
    if (narrow) {
        // This buffer only records copies, clears and initial image-layout transitions. Prior submissions may
        // still read or write the upload destinations: retain ALL_COMMANDS on the source side (including WAR
        // execution ordering), but only the following transfer reads/writes need to wait. Image initialization
        // supplies its own image barrier; the end-of-upload publication barrier remains conservative.
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
        return;
    }
#endif
    vk_full_barrier(cb);
}
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(s, &s->main, &s->main_begun);
        ss_forget();  // (a new command buffer starts with no state)
        if (vk_counter_mode() == 2) {  // this slot's timing counter queries (reset outside any render pass)
            CounterQ *q = &cq[cur_sub];
            if (!q->pool) {
                VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                            .queryCount = CQ_QUERIES};
                if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &q->pool) != VK_SUCCESS) q->pool = VK_NULL_HANDLE;
            }
            pthread_mutex_lock(&cq_lock);
            q->n = 0;  // (resolved when the slot was freed: sub())
            q->seq = s->seq;
            pthread_mutex_unlock(&cq_lock);
            if (q->pool) vkCmdResetQueryPool(s->main, q->pool, 0, CQ_QUERIES);
#ifdef __ANDROID__
            if (q->pool) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, q->pool, CQ_MAX);  // (the start)
#endif
        }
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if (prof && prof_qp) {
            vkCmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 2, 2);
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 2);
            s->ts = true;
        }
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    if (!s->upload_begun) {
        begin_cb(s, &s->upload, &s->upload_begun);
        // after everything submitted before it: a submission's commands may start while the previous one's still run,
        // and these copies overwrite what those may read (textures being replaced, vertex cache pages)
        upload_begin_barrier(s->upload);
    }
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);
void vc_flush(void);

// ---- LARECOMP_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
static VkQueryPool gp_qp;
static int gp_n;
#ifdef __ANDROID__
#define GP_MAX 8192  // (LARECOMP_GPUPROF_DRAWS=1: a mark after every draw too - its shaders, size and GPU time; ~2,700 a 1.8.2 frame)
#else
#define GP_MAX 512
#endif
static char gp_label[GP_MAX][112];
static uint32_t gp_draws[GP_MAX];
// LARECOMP_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with LARECOMP_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = getenv("LARECOMP_VK_DEVLOST_TRACE") ? atoi(getenv("LARECOMP_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= GP_MAX - 1) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_MAX};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) vkCmdResetQueryPool(cb, gp_qp, 0, GP_MAX);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
void vk_gp_note(VkCommandBuffer cb, const char *what) { gp_mark(cb, "%s", what); }  // (device.c: the present's steps)
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    static uint64_t t[GP_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (rows: the work after each mark, until the next)\n", gp_n, total);
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
        }
        LOG("[gpuprof] (last mark: %s)\n", gp_label[gp_n - 1]);
    }
    gp_n = 0;
}

// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    vc_flush();
#ifdef __ANDROID__
    ga_flush_dirty();  // (the arena's CPU writes, before the GPU reads them)
#endif
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
#ifdef __ANDROID__
    if (vk_counter_mode() == 2 && cq[cur_sub].pool) {  // the submission's end (cq_resolve: the idle between submissions)
        vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, cq[cur_sub].pool, CQ_MAX + 1);
        pthread_mutex_lock(&cq_lock);
        cq[cur_sub].ts = true;
        cq[cur_sub].presentation = wait != VK_NULL_HANDLE || signal != VK_NULL_HANDLE;
        pthread_mutex_unlock(&cq_lock);
    }
#endif
    if (s->ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                     .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbs,
                        .signalSemaphoreInfoCount = signal ? 2 : 1, .pSignalSemaphoreInfos = sig,
                        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
#ifdef __ANDROID__
        uint64_t trace_start = vk_timing_trace_on() ? SDL_GetTicksNS() : 0;
#endif
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
#ifdef __ANDROID__
        tl_add((int64_t)plat_now_ns(), wait || signal ? "render: submitted (present)" : "render: submitted", submit_seq + 1, 0);
        if (trace_start) vk_timing_trace_wait(VK_TIMING_SUBMIT, SDL_GetTicksNS() - trace_start);
#endif
        if (sr == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
        VK_CHECK(sr);
        PROF_ADD(prof_submit);
    }
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
#ifdef __ANDROID__
    last_submit_ns = SDL_GetTicksNS();
    if (game_pending) work_seq = submit_seq;  // (cleared when backend_execute is done: flushes inside it come first)
#endif
    pthread_mutex_unlock(&seq_lock);
    {
        static int log = -1;  // (LARECOMP_LOG_SYNCSEQ, renderer_wait_seq)
        if (log < 0) log = getenv("LARECOMP_LOG_SYNCSEQ") != NULL;
        if (log) LOG("[syncseq] t %.2f submitted seq %llu%s\n", SDL_GetTicksNS() / 1e6, (unsigned long long)submit_seq, signal ? " (present)" : "");
    }
    cur_sub = (cur_sub + 1) % SUBS;
    stat_flushes++;
    cum_submits++;
    ring_retire();
    vk_collect_dead();
}

void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE); }

void renderer_flush(void) {
    renderer_submit();
    if (!wait_why) wait_why = "flush";
    vk_wait_seq(submit_seq);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = "QueueFinish / sync all";
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t q = submit_seq + (subs[cur_sub].main_begun || subs[cur_sub].upload_begun ? 1 : 0);
#ifdef __ANDROID__
    static int fence_present = -1;
    if (fence_present < 0) fence_present = getenv("LARECOMP_FENCE_PRESENT") != NULL;
    if (!game_pending && work_seq && !fence_present) q = work_seq;
    static int log = -1;
    if (log < 0) log = getenv("LARECOMP_LOG_SYNCSEQ") != NULL;
    if (log) LOG("[syncseq] t %.2f fence -> seq %llu (submitted %llu, work %llu, begun %d/%d, tid %d)\n", SDL_GetTicksNS() / 1e6,
                 (unsigned long long)q, (unsigned long long)submit_seq, (unsigned long long)work_seq, subs[cur_sub].main_begun,
                 subs[cur_sub].upload_begun, (int)pthread_gettid_np(pthread_self()));
#endif
    pthread_mutex_unlock(&seq_lock);
    return q;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = seq > s ? "SyncWait (fence not submitted yet)" : "SyncWait";
#ifdef __ANDROID__
    vk_timing_trace_fence(seq, s);
#endif
    static int log = -1;  // LARECOMP_LOG_SYNCSEQ=1: which submission each SyncWait waits for (vs the last one) and how long
    if (log < 0) log = getenv("LARECOMP_LOG_SYNCSEQ") != NULL;
    uint64_t t0 = log ? SDL_GetTicksNS() : 0;
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
    if (log) LOG("[syncseq] t %.2f wait seq %llu (submitted %llu): %.2f ms, frame %llu\n", t0 / 1e6, (unsigned long long)seq,
                 (unsigned long long)s, (SDL_GetTicksNS() - t0) / 1e6, (unsigned long long)vk_frame_counter());
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 16) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32768}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = 6, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        s->cur_pool++;
    }
}

// ================================================================== transient memory ring
// 64 MB host-visible chunks (device-local when the GPU exposes resizable BAR); a chunk is reused once every
// submission that allocated from it has completed.
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;

static int new_chunk(uint64_t size) {
    chunks = realloc(chunks, (nchunks + 1) * sizeof *chunks);
    Chunk *c = &chunks[nchunks];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    if (getenv("LARECOMP_VK_RING_SYSMEM")) ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;  // (test: a GPU without resizable BAR)
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    c->ptr = info.pMappedData;
    c->size = size;
    if (!nchunks) {  // where the ring lives: device-local (resizable BAR) or system memory read across PCIe
        VkMemoryPropertyFlags f;
        vmaGetMemoryTypeProperties(g_vk.vma, info.memoryType, &f);
        LOG("[vk] transient ring: memory type %u (%s%s%s)\n", info.memoryType, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local " : "system memory ",
            f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "host-visible " : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "cached" : "");
    }
    return nchunks++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in vk_transient

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
#ifdef __ANDROID__
    gcopy_retire(done);  // (copies back out of chunks about to be reused)
#endif
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(size > CHUNK_SIZE ? size : CHUNK_SIZE);
    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
// Cached, and on Mali not coherent (types 0x7 and 0xb: none both): the GPU's data is read after vk_readback_ready.
// (0.2.3 asked for coherent memory instead - on Mali the uncached type, where the CPU's reads crawled.)
static VmaAllocation rb_mem;
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, rb_mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &rb_mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

// The readback's first size bytes as the GPU wrote them, once its submission is done (no-op on coherent memory)
void vk_readback_ready(uint64_t size) {
    if (rb_mem) vmaInvalidateAllocation(g_vk.vma, rb_mem, 0, size);
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_CBP_V, DEDUPE_CBP_F, DEDUPE_SLOTS };
#ifdef __ANDROID__
// This draw's gmap table (gmap_tab) in transient memory, shared with the draws of the submission that have the same
// one (the last 8 kept): the copy mode gives every draw that reaches global memory a table, and a new copy each time
// made each of their descriptor sets new too - the overworld's lit draws all have the same table, and Qualcomm's
// driver is slow at writing descriptor sets (~4 ms a frame on them)
static uint32_t gmap_tab[(GMAP_MAX + 1) * 4];
static VkSpan gmap_copy(void) {
    static struct { uint64_t seq; uint32_t tab[(GMAP_MAX + 1) * 4]; VkSpan span; } c[8];
    static int next;
    uint64_t seq = vk_pending_seq();
    for (int i = 0; i < 8; i++)
        if (c[i].seq == seq && !memcmp(c[i].tab, gmap_tab, sizeof c[i].tab)) return c[i].span;
    VkSpan sp = vk_transient_copy(gmap_tab, sizeof c[0].tab, 256);
    c[next].seq = seq;
    memcpy(c[next].tab, gmap_tab, sizeof c[next].tab);
    c[next].span = sp;
    next = (next + 1) % 8;
    return sp;
}
#endif

static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
static struct { uint64_t addr, size, seq; VkSpan span; } gcopy[GCOPY_TAB];
static uint16_t gcopy_active[GCOPY_TAB];
static int gcopy_nactive;
static uint64_t gcopy_active_seq;
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; gcopy_nactive = 0; }
static void guest_copy_invalidate(uint64_t lo, uint64_t hi) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40);
    if (gcopy_active_seq != seq) return;
    for (int i = 0; i < gcopy_nactive; i++) {
        uint16_t s = gcopy_active[i];
        if (gcopy[s].seq == seq && gcopy[s].addr < hi && lo < gcopy[s].addr + gcopy[s].size) gcopy[s].seq = 0;
    }
}
static VkSpan guest_copy(uint64_t addr, uint64_t size) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    if (gcopy_active_seq != seq) { gcopy_active_seq = seq; gcopy_nactive = 0; }
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    if (gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size) return gcopy[slot].span;
    if (gcopy[slot].seq != seq && gcopy_nactive < GCOPY_TAB) gcopy_active[gcopy_nactive++] = (uint16_t)slot;
    VkSpan s = vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    stat_cb_copied += size;
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s;
    return s;
}

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// LARECOMP_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
static Job *jobs_head, *jobs_tail;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;

static void *worker(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&jobs_lock);
        while (!jobs_head) pthread_cond_wait(&jobs_cond, &jobs_lock);
        Job *j = jobs_head;
        jobs_head = j->next;
        if (!jobs_head) jobs_tail = NULL;
        pthread_mutex_unlock(&jobs_lock);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}

static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_SYNC_SHADERS") != NULL;
    return v;
}

// Shader translation: one dedicated thread (the translator isn't re-entrant - translate.c's RSC2 string ring - and
// translating on the game thread cost up to ~90 ms per frame on entering an area). Compiles go on to the workers.
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;  // graphics and compute share translator globals
static Job *tjobs_head, *tjobs_tail;
static pthread_mutex_t tjobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tjobs_cond = PTHREAD_COND_INITIALIZER;
static void *translator(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&tjobs_lock);
        while (!tjobs_head) pthread_cond_wait(&tjobs_cond, &tjobs_lock);
        Job *j = tjobs_head;
        tjobs_head = j->next;
        if (!tjobs_head) tjobs_tail = NULL;
        pthread_mutex_unlock(&tjobs_lock);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}
static bool sync_shaders(void);
static void run_translate(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) { started = true; pthread_t t; pthread_create(&t, NULL, translator, NULL); }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&tjobs_lock);
    if (tjobs_tail) tjobs_tail->next = j; else tjobs_head = j;
    tjobs_tail = j;
    pthread_cond_signal(&tjobs_cond);
    pthread_mutex_unlock(&tjobs_lock);
}

static void run_job(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) {
        started = true;
        // first entry into an area builds dozens of pipelines at once: use most of the CPU (draws wait for them)
        int cores = SDL_GetNumLogicalCPUCores();
        int n = cores - 4;
#ifdef __ANDROID__
        n = cores / 2;
        if (n > 4) n = 4;  // leave CPU capacity for the game and Vulkan translation
#endif
        const char *workers = getenv("LARECOMP_SHADER_WORKERS");
        if (workers) n = atoi(workers);
        if (n < 1) n = 1;
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); }
    }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&jobs_lock);
    if (jobs_tail) jobs_tail->next = j; else jobs_head = j;
    jobs_tail = j;
    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_lock);
}

// A skipped draw is lost for good when its target is built once and kept: BOTW's terrain height array (1024x1024x8,
// each layer drawn once from the one before - the first draw of that shader skipped left layer 3 and all after it
// empty: no ground on the plateau). Draws into arrays, cubemaps, 3D textures and mipmapped targets wait for their
// shader and pipeline instead (LARECOMP_WAIT_PERSISTENT=0: skip them like the rest).
static uint64_t stat_build_wait_ns;
static void wait_built(_Atomic int *state) {
    if (atomic_load(state)) return;
    uint64_t t0 = plat_now_ns();
    while (!atomic_load(state)) usleep(200);
    stat_build_wait_ns += plat_now_ns() - t0;
}
static bool rt_is_persistent(NvnTexture *t) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_WAIT_PERSISTENT") || atoi(getenv("LARECOMP_WAIT_PERSISTENT"));
    return on && t && (t->levels > 1 || (t->target != 1 && t->target != 5 && t->target != 7));  // (not 2D / 2D MS / rect)
}
// So is a target drawn into now and then - not in the frame before: the pause menu's background, the world captured
// and blurred once as the menu opens (1280x720 -> 640x360 blur -> colour LUT -> 320x180), its shaders used for the
// first time in that very frame. Skipped, their draws left it black for as long as the menu stayed open. A target drawn
// into every frame redoes a skipped draw the next frame and keeps skipping (no hitch while shaders build). Only passes
// without a depth target wait so (draw_): the 3D scene's (G-buffer, with depth) are new on the world's first frame after
// a loading screen too, and all of their draws waiting there added ~0.4 s of shaders the first time - the menu's chain,
// the LUT and post-processing have no depth. LARECOMP_WAIT_ONE_OFF=0: skip there too (as before).
static bool rt_one_off(NvnTexture *t, uint64_t frame) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_WAIT_ONE_OFF") || atoi(getenv("LARECOMP_WAIT_ONE_OFF"));
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (!on || !t) return false;
    if (!b) return true;  // (never drawn into yet)
    if (b->rt_frame != frame + 1) {  // its first draw this frame: was it drawn into the frame before?
        b->rt_oneoff = b->rt_frame != frame;
        b->rt_frame = frame + 1;
    }
    return b->rt_oneoff;
}
// And every draw without a depth target, drawn into each frame or not: post-processing, the final composite and the HUD
// are a few full-screen passes, and one skipped there while its shader built showed the whole frame black - TOTK's
// flashes on new ground (2026-10-25; the first use of each such shader waits once, then the caches have it).
// LARECOMP_WAIT_NO_DEPTH=0: only the one-off ones, as before.
static bool wait_no_depth(void) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_WAIT_NO_DEPTH") || atoi(getenv("LARECOMP_WAIT_NO_DEPTH"));
    return on;
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    bool depth_clamp;  // nvnCommandBufferSetDepthClamp: shadow casters in front of the near plane still cast (pancaking)
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
} st;

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (LARECOMP_VK_PROF) called once per presented frame: frames over 50 ms are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs;
    uint64_t now = SDL_GetTicksNS();
    if (last_t && now - last_t > 50000000) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        LOG("[hitch] frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f) + present %.1f + other %.1f; %llu draws, "
            "%llu submits, compiles %d pending\n", (unsigned long long)frame, dt / 1e6, ex / 1e6, wt / 1e6, present_ns / 1e6,
            ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws), (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles));
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits;
}

const char *renderer_stats(void) {
    static int dumps;
    if (dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d (waited %.0fms), fbcopy %llu (read %llu, breaks %llu), upload %.1fMB, binds %llu pipe %llu sets",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), (double)stat_build_wait_ns / 1e6, (unsigned long long)stat_feedback_copies, (unsigned long long)stat_feedback_reads, (unsigned long long)stat_inplace_breaks, (double)stat_upload_bytes / 1048576.0,
             (unsigned long long)stat_pipe_binds, (unsigned long long)stat_set_binds);
    stat_feedback_copies = stat_feedback_reads = stat_inplace_breaks = stat_upload_bytes = stat_build_wait_ns = stat_pipe_binds = stat_set_binds = 0;
    if (getenv("LARECOMP_LOG_CBUF_BYTES")) {
        LOG("[cbuf] fixed %llu x %.1f KB, dynamic %llu x %.1f KB; copied %.1f MB; in place, patched %llu\n",
            (unsigned long long)stat_cb_n[0], stat_cb_n[0] ? stat_cb_bytes[0] / 1024.0 / stat_cb_n[0] : 0,
            (unsigned long long)stat_cb_n[1], stat_cb_n[1] ? stat_cb_bytes[1] / 1024.0 / stat_cb_n[1] : 0,
            stat_cb_copied / 1048576.0, (unsigned long long)stat_cbwin);
        stat_cb_bytes[0] = stat_cb_bytes[1] = stat_cb_n[0] = stat_cb_n[1] = stat_cb_copied = stat_cbwin = 0;
    }
    {   // (GPU memory in use, every heap, and the game's textures with an image)
        VmaBudget bud[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(g_vk.vma, bud);
        const VkPhysicalDeviceMemoryProperties *mp;
        vmaGetMemoryProperties(g_vk.vma, &mp);
        uint64_t used = 0;
        for (uint32_t i = 0; i < mp->memoryHeapCount; i++) used += bud[i].usage;
        extern int vk_texture_count(void);
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", gpu mem %.0fMB, %d textures", (double)used / 1048576.0, vk_texture_count());
    }
    {
        extern void vk_alias_stats(void);
        vk_alias_stats();
    }
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | sets: driver %.0f cbuf %.0f ms, cache %llu hits %llu misses",
                 prof_dsw / 1e6, prof_cbuf / 1e6, (unsigned long long)prof_set_hits, (unsigned long long)prof_set_miss);
        prof_dsw = prof_cbuf = prof_set_hits = prof_set_miss = 0;
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu | counters gpu %llu | pace %s, %llu frames over", (unsigned long long)prof_views_created,
                 (unsigned long long)stat_counters_gpu, pace_slow ? "30" : "60", (unsigned long long)stat_pace_over);
        stat_counters_gpu = stat_pace_over = 0;
        {
            extern uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n,  " | vcache %llu hit %llu in place %.1f MB copied, %llu queries %.2f ms, %llu pages written",
                     (unsigned long long)prof_vc_hits, (unsigned long long)prof_vc_bypass, prof_vc_copied / 1048576.0,
                     (unsigned long long)prof_vc_queries, prof_vc_scan_ns / 1e6, (unsigned long long)prof_vc_scan_pages);
            prof_vc_hits = prof_vc_bypass = prof_vc_copied = prof_vc_queries = prof_vc_scan_pages = prof_vc_scan_ns = 0;
            extern uint64_t prof_tex_watch_skips;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | tex unwritten %llu", (unsigned long long)prof_tex_watch_skips);
            prof_tex_watch_skips = 0;
        }
        pthread_mutex_lock(&wait_tab_lock);
        for (int i = 0; i < 12 && wait_tab[i].why; i++) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, "%s%s %llux %.0f ms", i ? "," : " | waits:", wait_tab[i].why,
                     (unsigned long long)wait_tab[i].n, wait_tab[i].ns / 1e6);
        }
        memset(wait_tab, 0, sizeof wait_tab);
        pthread_mutex_unlock(&wait_tab_lock);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    return stats_text;
}

// Baked state changes and pass restarts invalidate the last pipeline; dynamic state does not.
static bool pipeline_dirty = true;

static void st_defaults(void) {
    pipeline_dirty = true;
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void vk_exec_init(void) {
    prof = getenv("LARECOMP_VK_PROF") != NULL;
#ifdef __ANDROID__
    gcopy_mode = !g_vk.host_import && !getenv("LARECOMP_VK_NO_GCOPY");
    if (gcopy_mode) LOG("[vk] no heap import: shader global memory through CPU copies\n");
    // (for good, without placed memory: the vertex cache and the texture checks fed by write tracking - vc_flush)
    if (gcopy_mode && !g_vk.map_placed) plat_write_tracking_enable(HEAP_BASE, HEAP_BASE + HEAP_MAX);
#endif
    if (prof) {
        prof_up_ns = 0;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 2};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    st_defaults();
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];
static uint32_t tex_wh[2][64][2];

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static int debug_pass_no;

// Redundant-state filter: what draw() last set in the current command buffer, so unchanged pipeline binds, dynamic
// state and vertex / index buffer binds are not recorded again (a draw used to issue ~20 vkCmd* calls; most repeat the
// previous draw's). Forgotten (ss_forget) whenever something else may have recorded state: a new command buffer, a new
// render pass (present, ImGui, post-processing and copies all run between game passes) and the clear pipeline.
// Descriptor sets are not filtered: every draw writes new ones (sharing identical sets within a submission was tried:
// a third matched, but comparing them cost more than allocating and writing them on NVIDIA).
// LARECOMP_VK_STATE_FILTER=0: every draw sets everything, as before.
enum { SS_PIPE = 1 << 0, SS_DT = 1 << 1, SS_DW = 1 << 2, SS_DOP = 1 << 3, SS_STEN = 1 << 4, SS_SFACE0 = 1 << 5, SS_SFACE1 = 1 << 6,
       SS_BC = 1 << 7, SS_CULL = 1 << 8, SS_FF = 1 << 9, SS_TOPO = 1 << 10, SS_PRE = 1 << 11, SS_DBE = 1 << 12, SS_DB = 1 << 13,
       SS_VP = 1 << 14, SS_SC = 1 << 15, SS_IB = 1 << 16, SS_SETS = 1 << 17 };
static struct {
    uint32_t known;  // SS_* bits: the value below is what the command buffer has
    uint32_t vb_known;  // per vertex binding
    VkPipeline pipe;
    VkBool32 dt, dw, sten, pre, dbe;
    VkCompareOp dop;
    uint32_t sface[2][7];  // fail, pass, depth fail, compare, compare mask, write mask, reference
    float bc[4], db[3];
    VkCullModeFlags cull;
    VkFrontFace ff;
    VkPrimitiveTopology topo;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ib; VkDeviceSize io; VkIndexType it;
    VkBuffer vb[17]; VkDeviceSize vo[17];
    VkPipelineLayout layout; VkDescriptorSet sets[2];  // (SS_SETS: the graphics sets bound and their layout,
    uint32_t ndyn, dyn[64];                             // and their dynamic uniform buffer offsets)
} ss;
static int ss_on = -1;
static void ss_forget(void) { ss.known = 0; ss.vb_known = 0; }
static bool ss_enabled(void) {
    if (ss_on < 0) { const char *e = getenv("LARECOMP_VK_STATE_FILTER"); ss_on = !e || atoi(e) != 0; }
    return ss_on;
}
// true: the state must be recorded (and is remembered as known)
static inline bool ss_need(uint32_t bit, bool same) {
    if (ss_on && (ss.known & bit) && same) return false;
    ss.known |= bit;
    return true;
}

// ---- write-backs: GPU results the game's CPU reads from the texture's own memory
// On the Switch a render target's texels land in its memory, and BOTW reads some back on the CPU: the depth bounds
// it reduces on the GPU down to 3x3 (RGBA16F, view-space depth per screen region, read by f_10120fdc4 - shadow
// cascade fitting, most likely). We render into Vulkan images only, so the CPU read stale memory. Tiny color
// targets (one level, unscaled, up to WB_MAX bytes) are copied out at the end of their pass and swizzled into guest
// memory once the GPU is done (cq_resolve_done / each present). LARECOMP_NO_WRITEBACK=1: off;
// LARECOMP_LOG_WRITEBACK=1 (or =<width>): the targets.
static uint64_t wb_max(void) {  // (LARECOMP_WRITEBACK_MAX=<bytes>: a larger limit, for finding other read-backs)
    static uint64_t m;
    if (!m) { const char *e = getenv("LARECOMP_WRITEBACK_MAX"); m = e ? strtoull(e, NULL, 0) : 4096; }
    return m;
}
#define WB_MAX wb_max()
// Linear render targets (NVN flag 0x40, rows nvn_linear_rt_pitch apart; or 0x10, at the texture's stride) are
// written back whatever their size up to a slot: BOTW's terrain height tiles (8 levels of 256x256 R16) are drawn on
// the GPU and read by the CPU's height queries (f_100f841cc) - the feet's placement on the terrain (f_100d1a080) put
// Link and the NPCs into the ground without them. LARECOMP_WB_LINEAR=0: off.
#define WB_LINEAR_MAX (512u * 1024)
#define WB_SLOT (WB_MAX > WB_LINEAR_MAX ? WB_MAX : WB_LINEAR_MAX)
// Two pools of slots: WB_SMALL of WB_MAX bytes for the small targets, WB_BIG of WB_SLOT for the linear ones (the terrain
// heights). A 1.9.0 frame queues ~30 write-backs (1x1 to 24x24: exposure, depth bounds, the feet's 1x1 targets) and 32
// slots of 512 KB each - all of them sized for a terrain tile - were full every frame once its previous one was still
// on the GPU: the CPU waited for the GPU ~12 times a second, and the game stuttered (2026-10-27, 8,700 waits in 20
// minutes). Now four frames of small ones fit, in about as much memory as before.
#define WB_SMALL 128
#define WB_BIG 32
#define WB_SLOTS (WB_SMALL + WB_BIG)
typedef struct { uint64_t seq, addr, pitch, off, order; uint32_t wb, hb, bpb, bh, fmt, size; bool busy; } WbEntry;  // (pitch: linear rows, 0: block-linear; off, size: its slot in wb_buf; order: when queued)
static WbEntry wb_ent[WB_SLOTS];
static int wb_next_small, wb_next_big;
static uint64_t wb_order;
static VkBuffer wb_buf;
static VmaAllocation wb_mem;
static uint8_t *wb_ptr;
static pthread_mutex_t wb_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnTexture *wb_pass[8];
static int wb_npass;
static uint64_t stat_wb, stat_wb_dropped;
// The submission of the latest write-back queued: the sync lag's wait covers it (objects.c nvnSyncWait) - the CPU reads
// these right after that wait (the terrain heights the feet stand on, the water's colour), so they must have landed
static _Atomic uint64_t g_wb_seq;
uint64_t vk_last_writeback_seq(void) { return atomic_load(&g_wb_seq); }

static bool wb_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_NO_WRITEBACK") == NULL;
    return on;
}

static bool wb_eligible(NvnTexture *t, TexBackend *b, uint32_t lv) {
    if (!wb_on() || !t || !b || lv || t->depth > 1 || t->levels > 1 || t->layout.layers > 1 || b->samples > 1) return false;
    if (!(b->aspect & VK_IMAGE_ASPECT_COLOR_BIT) || b->width != t->width || b->height != t->height) return false;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb || fi->bw != 1 || fi->bh != 1 || fi->bpb == 3) return false;
    static int only = -2;  // LARECOMP_WRITEBACK_FMT=<hex>: only targets of that format (bisecting)
    if (only == -2) only = getenv("LARECOMP_WRITEBACK_FMT") ? (int)strtol(getenv("LARECOMP_WRITEBACK_FMT"), NULL, 16) : -1;
    if (only >= 0 && t->format != (uint32_t)only && tegra_total_size(&t->layout) > 4096) return false;
    static int lin = -1;  // (linear targets too, up to a slot; LARECOMP_WB_LINEAR=0: not)
    if (lin < 0) lin = !getenv("LARECOMP_WB_LINEAR") || atoi(getenv("LARECOMP_WB_LINEAR"));
    return t->addr >= HEAP_BASE && tegra_total_size(&t->layout) <= ((lin && (t->flags & 0x50)) ? WB_SLOT : WB_MAX);
}

static void wb_queue(NvnTexture *t) {
    TexBackend *b = t->backend;
    pthread_mutex_lock(&wb_lock);
    if (!wb_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = (VkDeviceSize)WB_MAX * WB_SMALL + (VkDeviceSize)WB_SLOT * WB_BIG,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &wb_buf, &wb_mem, &info));
        wb_ptr = info.pMappedData;
    }
    // (the slot by the copy's size: a small one when it fits WB_MAX, a big one else)
    const FormatInfo *qfi = t->layout.fmt;
    LevelLayout ql = tegra_level(&t->layout, 0);  // (wb_apply reads the level's padded blocks: the larger of the two)
    uint64_t qbpb = qfi ? qfi->bpb : 16, need = (uint64_t)t->width * t->height * qbpb, padded = (uint64_t)ql.wb * ql.hb * qbpb;
    bool small = (need > padded ? need : padded) <= WB_MAX;
    int *next = small ? &wb_next_small : &wb_next_big;
    int base = small ? 0 : WB_SMALL, count = small ? WB_SMALL : WB_BIG;
    WbEntry *e = &wb_ent[base + *next];
    if (e->busy) {
        // Every slot still waits for the GPU. Dropped, the target's texels never reached memory: a tower's cutscene
        // draws the region's terrain height tiles (8 levels of 256x256 a tile) in a burst, the heights stayed stale and
        // the terrain sampled from that memory stood up in "curtains" until the area loaded again (2026-10-26). Now the
        // GPU is waited for up to the slot's submission and the slot's write-back lands first. LARECOMP_WB_DROP=1: dropped,
        // as before.
        static int drop = -1;
        if (drop < 0) drop = getenv("LARECOMP_WB_DROP") != NULL;
        stat_wb_dropped++;
        static int nlog;
        if (nlog++ < 40 || !(stat_wb_dropped & 255))
            LOG("[vk] write-back slots full (%llu so far): %s the GPU (submission %llu)\n", (unsigned long long)stat_wb_dropped,
                drop ? "dropped, not waiting for" : "waiting for", (unsigned long long)e->seq);
        if (drop) { pthread_mutex_unlock(&wb_lock); return; }
        uint64_t seq = e->seq;
        pthread_mutex_unlock(&wb_lock);
        if (seq >= vk_pending_seq()) renderer_submit();  // (its copy is in the work being recorded: send it)
        wait_why = "write-back slots full";
        vk_wait_seq(seq);  // (lands the finished write-backs: cq_resolve_done -> wb_apply)
        wb_apply(seq);
        pthread_mutex_lock(&wb_lock);
        e = &wb_ent[base + *next];
        if (e->busy) { pthread_mutex_unlock(&wb_lock); return; }  // (should not happen: still busy after the wait)
    }
    int slot = *next;
    *next = (*next + 1) % count;
    uint64_t off = small ? (uint64_t)slot * WB_MAX : (uint64_t)WB_MAX * WB_SMALL + (uint64_t)slot * WB_SLOT;
    const FormatInfo *fi = t->layout.fmt;
    LevelLayout l = tegra_level(&t->layout, 0);
    extern uint64_t nvn_linear_rt_pitch(uint64_t row_bytes);
    *e = (WbEntry){.seq = submit_seq + 1, .addr = t->addr, .wb = l.wb, .hb = l.hb, .bpb = fi->bpb,
                   .pitch = (t->flags & 0x10) ? t->stride : (t->flags & 0x40) ? nvn_linear_rt_pitch((uint64_t)l.wb * fi->bpb) : 0,
                   .bh = l.block_height_log2, .fmt = t->format, .off = off, .size = small ? WB_MAX : WB_SLOT,
                   .order = ++wb_order, .busy = true};
    atomic_store(&g_wb_seq, e->seq);
    pthread_mutex_unlock(&wb_lock);
    {   // LARECOMP_LOG_WB_FRAME=<n>: every write-back of every n-th frame - size, format, flags, submission (the frame's
        // submissions: nvnSyncWait's [synclag-frame] line), to tell which ones the CPU needs before the next frame
        static int every = -1;
        if (every < 0) every = getenv("LARECOMP_LOG_WB_FRAME") ? atoi(getenv("LARECOMP_LOG_WB_FRAME")) : 0;
        if (every > 0 && renderer_present_count() % (uint64_t)every == 0)
            LOG("[wb-frame] %llu: %ux%u f%#x flags %#x at %llx, submission %llu\n", (unsigned long long)renderer_present_count(),
                t->width, t->height, t->format, t->flags, (unsigned long long)t->addr, (unsigned long long)e->seq);
    }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = (VkDeviceSize)off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {t->width, t->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, wb_buf, 1, &c);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    stat_wb++;
    static int nlog;  // LARECOMP_LOG_WRITEBACK=1: the targets written back
    static int lw = -2;  // (=<w> > 1: only targets that wide)
    if (lw == -2) lw = getenv("LARECOMP_LOG_WRITEBACK") ? atoi(getenv("LARECOMP_LOG_WRITEBACK")) : -1;
    static uint64_t seen[256];  // (each target once: address and size)
    uint64_t key = t->addr ^ ((uint64_t)t->width << 48) ^ ((uint64_t)t->height << 36) ^ ((uint64_t)t->format << 58);
    bool fresh = true;
    for (int i = 0; i < nlog && i < 256; i++) if (seen[i] == key) { fresh = false; break; }
    if (lw >= 0 && fresh && (lw <= 1 || (int)t->width == lw) && nlog < 256 && (seen[nlog++] = key, 1))
        LOG("[vk] write-back %ux%u f%#x at %llx (submission %llu)\n", t->width, t->height, t->format,
            (unsigned long long)t->addr, (unsigned long long)e->seq);
}

static void wb_apply(uint64_t done) {
    if (!wb_ptr) return;
    pthread_mutex_lock(&wb_lock);
    // In the order they were queued, not the slots': once the 128 small slots wrapped, a target written back twice
    // (a frame's and the next one's, both landed) took the older texels last - the water turned white and Link's feet
    // sank, with the sync lag off too (2026-10-27, 0.2.1 / issue #43: the 32-slot ring was full every frame, and its
    // wait kept them in order)
    int ready[WB_SLOTS], nready = 0;
    for (int i = 0; i < WB_SLOTS; i++) {
        if (!wb_ent[i].busy || wb_ent[i].seq > done) continue;
        int k = nready++;
        for (; k > 0 && wb_ent[ready[k - 1]].order > wb_ent[i].order; k--) ready[k] = ready[k - 1];
        ready[k] = i;
    }
    for (int r = 0; r < nready; r++) {
        WbEntry *e = &wb_ent[ready[r]];
        vmaInvalidateAllocation(g_vk.vma, wb_mem, (VkDeviceSize)e->off, e->size);
        if (e->pitch)
            for (uint32_t y = 0; y < e->hb; y++)
                memcpy((uint8_t *)(uintptr_t)(e->addr + y * e->pitch), wb_ptr + (size_t)e->off + (size_t)y * e->wb * e->bpb, (size_t)e->wb * e->bpb);
        else tegra_swizzle(wb_ptr + (size_t)e->off, (uint8_t *)(uintptr_t)e->addr, e->wb, e->hb, 1, e->bpb, e->bh, 0);
        {   // LARECOMP_LOG_WB_NAN=1: the auto exposure's small targets - inf / NaN in them, and a black frame (the
            // 2x2 luminance of the previous image near 0) with the 12x1 parameters and the 1x1 average at that point
            static int on = -1;
            if (on < 0) on = getenv("LARECOMP_LOG_WB_NAN") != NULL;
            static float p12[12][3], p1[3];
            const uint8_t *px = wb_ptr + (size_t)e->off;
            if (on && e->fmt == 0x3f && e->hb == 1 && (e->wb == 12 || e->wb == 1)) {  // R11G11B10F
                for (uint32_t k = 0; k < e->wb; k++) {
                    uint32_t v = ((const uint32_t *)px)[k];
                    uint32_t m[3] = {v & 63, (v >> 11) & 63, (v >> 22) & 31}, x[3] = {(v >> 6) & 31, (v >> 17) & 31, (v >> 27) & 31};
                    for (int ch = 0; ch < 3; ch++) {
                        float mb = ch == 2 ? 32.0f : 64.0f;
                        float f = x[ch] == 31 ? (m[ch] ? NAN : INFINITY) : x[ch] == 0 ? m[ch] / mb * 6.1035156e-05f
                                                                                   : ldexpf(1.0f + m[ch] / mb, (int)x[ch] - 15);
                        if (e->wb == 12) p12[k][ch] = f; else p1[ch] = f;
                        if (!isfinite(f)) {
                            static uint64_t last;
                            if (renderer_present_count() != last) {
                                last = renderer_present_count();
                                LOG("[wb] %ux1 texel %u ch %d not finite, frame %llu\n", e->wb, k, ch, (unsigned long long)last);
                            }
                        }
                    }
                }
            }
            if (on && e->fmt == 0x5 && e->wb == 2 && e->hb == 2) {  // R16F: the previous image's luminance
                const _Float16 *h = (const _Float16 *)px;
                float avg = ((float)h[0] + (float)h[1] + (float)h[2] + (float)h[3]) / 4;
                static uint64_t last;
                uint64_t fr = renderer_present_count();
                if (fr > 1500 && (avg < 0.02f || !isfinite(avg)) && fr - last > 30) {
                    last = fr;
                    LOG("[wb] dark frame %llu: lum2x2 %g %g %g %g | 1x1 %g %g %g\n", (unsigned long long)fr, (double)h[0], (double)h[1],
                        (double)h[2], (double)h[3], p1[0], p1[1], p1[2]);
                    for (int k = 0; k < 12; k++) LOG("[wb]   p12[%d] %g %g %g\n", k, p12[k][0], p12[k][1], p12[k][2]);
                }
            }
        }
        e->busy = false;
    }
    pthread_mutex_unlock(&wb_lock);
}

TexBackend *g_dump_sampled;  // (LARECOMP_DUMP_SAMPLED: the texture to dump after the pass)
NvnTexture *g_pass_t0;  // (the open pass's first color target and its layer: LARECOMP_DUMP_AFTER_LAYER)
int g_pass_layer;
static void dump_layers_raw(TexBackend *b) {  // every layer, level 0, as stored (R8 / R16 / RGBA8 / RGBA16F...)
    uint32_t bpp = b->format == VK_FORMAT_R16_UNORM || b->format == VK_FORMAT_R16_SFLOAT ? 2 : b->format == VK_FORMAT_R8_UNORM ? 1
                 : b->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    uint32_t nl = b->layers ? b->layers : 1;
    uint32_t lvl = getenv("LARECOMP_DUMP_SAMPLED_LEVEL") ? (uint32_t)atoi(getenv("LARECOMP_DUMP_SAMPLED_LEVEL")) : 0;
    if (lvl >= b->levels) lvl = 0;
    uint32_t w = b->width >> lvl ? b->width >> lvl : 1, h = b->height >> lvl ? b->height >> lvl : 1;
    uint64_t one = (uint64_t)w * h * bpp;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback(one * nl);
    for (uint32_t l = 0; l < nl; l++) {
        VkBufferImageCopy c = {.bufferOffset = out.off + one * l, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lvl, l, 1},
                               .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    }
    renderer_flush();
    vk_readback_ready(one * nl);
    for (uint32_t l = 0; l < nl; l++) {
        char p[128];
        snprintf(p, sizeof p, "build/sampled_%ux%u_L%u.raw", b->width, b->height, l);
        FILE *f = fopen(p, "wb");
        if (f) { fwrite((const uint8_t *)out.ptr + one * l, 1, one, f); fclose(f); }
    }
    LOG("[vk] dumped %u layers of %ux%u vkformat %d (%u bytes a texel)\n", nl, b->width, b->height, (int)b->format, bpp);
}
// (diagnosis) LARECOMP_WATCH_TEXADDR's texture, every layer of every level, as stored: build/watch_L<layer>_lv<level>.raw,
// when the file dump_watch appears next to the data folder (adb can write there: touch files/dump_watch) - its contents
// at the moment something looks wrong on screen (the terrain height atlas after a tower's cutscene). Checked every 30
// frames; the file's change time, so each touch dumps once.
void vk_dump_watch_frame(void) {
    extern uint64_t nvn_watch_texaddr(void);
    extern NvnTexture *vk_texture_at(uint64_t addr);
    static uint64_t n;
    static time_t last;
    uint64_t wa = nvn_watch_texaddr();
    if (!wa || ++n % 30) return;
    struct stat sb;
    if (stat("../dump_watch", &sb) || sb.st_mtime == last) return;
    last = sb.st_mtime;
    NvnTexture *t = vk_texture_at(wa);
    TexBackend *b = t ? t->backend : NULL;
    if (!b || !b->image) { LOG("[vk] dump_watch: no texture at %llx\n", (unsigned long long)wa); return; }
    uint32_t bpp = b->format == VK_FORMAT_R16_UNORM || b->format == VK_FORMAT_R16_SFLOAT ? 2 : b->format == VK_FORMAT_R8_UNORM ? 1
                 : b->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    uint32_t nl = b->layers ? b->layers : 1;
    for (uint32_t lv = 0; lv < b->levels; lv++) {
        uint32_t w = b->width >> lv ? b->width >> lv : 1, h = b->height >> lv ? b->height >> lv : 1;
        uint64_t one = (uint64_t)w * h * bpp;
        VkCommandBuffer cb = vk_cmd();
        vk_end_pass();
        vk_full_barrier(cb);
        VkSpan out = vk_readback(one * nl);
        for (uint32_t l = 0; l < nl; l++) {
            VkBufferImageCopy c = {.bufferOffset = out.off + one * l, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, l, 1},
                                   .imageExtent = {w, h, 1}};
            vkCmdCopyImageToBuffer(vk_cmd(), b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
        }
        renderer_flush();
        vk_readback_ready(one * nl);
        for (uint32_t l = 0; l < nl; l++) {
            char p[128];
            snprintf(p, sizeof p, "build/watch_L%u_lv%u.raw", l, lv);
            FILE *f = fopen(p, "wb");
            if (f) { fwrite((const uint8_t *)out.ptr + one * l, 1, one, f); fclose(f); }
        }
    }
    LOG("[vk] dump_watch frame %llu: %ux%u, %u layers, %u levels, vkformat %d (%u bytes a texel)\n",
        (unsigned long long)renderer_present_count(), b->width, b->height, nl, b->levels, (int)b->format, bpp);
}
static void vk_end_pass_at(int line);
static void save_texture_raw(TexBackend *b, const char *path);
static TexBackend *pass_dump_b;  // (debug frame) the color target the open pass draws into
static uint32_t pass_dump_layer = ~0u;  // (a depth-only pass: its depth target's layer, dumped as red)
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    vk_end_rendering(vk_cmd());
    in_pass = false;
    {   extern TexBackend *g_dump_sampled;
        if (g_dump_sampled) { TexBackend *db = g_dump_sampled; g_dump_sampled = NULL; dump_layers_raw(db); }
    }
    {   // LARECOMP_DUMP_AFTER_LAYER=<n>: the watched texture's layers (level 0) right after the first pass into layer n
        extern NvnTexture *g_pass_t0; extern int g_pass_layer;
        extern uint64_t nvn_watch_texaddr(void);
        static int want = -2;
        if (want == -2) want = getenv("LARECOMP_DUMP_AFTER_LAYER") ? atoi(getenv("LARECOMP_DUMP_AFTER_LAYER")) : -1;
        uint64_t wa = want >= 0 ? nvn_watch_texaddr() : 0;
        if (wa && g_pass_t0 && g_pass_t0->addr == wa && g_pass_layer == want && g_pass_t0->backend) {
            LOG("[vk] after the first pass into layer %d (frame %llu):\n", want, (unsigned long long)renderer_present_count());
            want = -1;
            dump_layers_raw((TexBackend *)g_pass_t0->backend);
        }
    }
    for (int i = 0; i < wb_npass; i++) wb_queue(wb_pass[i]);
    wb_npass = 0;
    {   // (diagnosis) LARECOMP_DEBUG_WAIT_PASSES=<first>-<last>: in debug frames, the GPU finishes the work so far
        // after each of those passes (numbered from 0 each frame) - what the pass dumps do, without the dumps
        static int wa = -2, wz;
        if (wa == -2) {
            wa = -1;
            const char *e = getenv("LARECOMP_DEBUG_WAIT_PASSES");
            if (e && sscanf(e, "%d-%d", &wa, &wz) != 2) wa = -1;
        }
        static uint64_t wframe = ~0ull;
        static int wno;
        if (g_debug_frame && wa >= 0) {
            if (wframe != renderer_present_count()) wframe = renderer_present_count(), wno = 0;
            if (wno >= wa && wno <= wz) renderer_flush();
            wno++;
        }
    }
    // (the pass's own first color target: by now st.rt may already name the next pass's)
    static int nodump = -1;  // LARECOMP_DEBUG_NO_PASSES=1: the draw log alone (each dump waits for the GPU: timing changes)
    if (nodump < 0) nodump = getenv("LARECOMP_DEBUG_NO_PASSES") != NULL;
    if (g_debug_frame && pass_dump_b && !nodump) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("LARECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        TexBackend *b = pass_dump_b;
        pass_dump_b = NULL;
        extern uint32_t vk_dump_depth_layer;
        vk_dump_depth_layer = pass_dump_layer;
        renderer_save_texture_tex(b, path);
        vk_dump_depth_layer = ~0u;
        static int raw = -1;
        if (raw < 0) raw = getenv("LARECOMP_DEBUG_RAW_PASSES") != NULL;
        if (raw && pass_dump_layer == ~0u) save_texture_raw(b, path);
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }

// The frame the 3D scene's G-buffer was last drawn in (a pass of 3+ scene-sized targets with depth): widescreen.c
// keeps the title screen and loading screens - no scene - at 16:9
static volatile uint64_t g_scene_frame;
uint64_t vk_scene_frame(void) { return g_scene_frame; }

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    pipeline_dirty = true;
    vk_end_pass();
    if (st.rt.depth && st.rt.ncolor >= 3 && st.rt.color[0] && vk_texture_scale_group(st.rt.color[0]) == RS_SCENE) {
        g_scene_frame = renderer_present_count();
        static float last_w, last_h;  // (LARECOMP_LOG_SCENE=1: the game's dynamic resolution, the G-buffer's size)
        static int log_scene = -1;
        if (log_scene < 0) log_scene = getenv("LARECOMP_LOG_SCENE") != NULL;
        if (log_scene && (st.viewport[0][2] != last_w || st.viewport[0][3] != last_h))
            LOG("[scene] frame %llu: viewport %.0f x %.0f of %u x %u\n", (unsigned long long)g_scene_frame, st.viewport[0][2],
                st.viewport[0][3], st.rt.color[0]->width, st.rt.color[0]->height);
        last_w = st.viewport[0][2], last_h = st.viewport[0][3];
    }
#ifdef __ANDROID__
    {   // LARECOMP_VK_ASYNC: the GPU starts on a frame while the render thread still translates the rest of it - a
        // submission at a pass boundary (no pass split: on a tiler that would store and reload its tiles) once
        // LARECOMP_VK_CHUNK_US of translation has gone by. Translating a whole frame and then running it on the GPU
        // (~6 + ~11 ms) was the frame's critical path: the game waits on the previous frame's fence.
        extern bool vk_async_on(void);
        static long chunk = -1;
        if (chunk < 0) {
            // (not on Qualcomm's driver: its render thread is slow - ~12 ms a frame in the copy mode - so the GPU sat
            // idle between the chunks, and the game, timing its frames on the GPU from start to end, saw the gaps as
            // GPU time and stayed at 30 even indoors; a whole frame at once, it chose 60 there)
            extern bool vk_driver_is_qualcomm(void);
            const char *e = getenv("LARECOMP_VK_CHUNK_US");
            chunk = e ? atol(e) : vk_driver_is_qualcomm() ? 0 : 2000;
        }
        extern int vk_ab(const char *what);
        long ch = chunk > 0 && AB("chunk") == 1 ? 500 : chunk;
        if (ch > 0 && vk_async_on() && last_submit_ns && SDL_GetTicksNS() - last_submit_ns > (uint64_t)ch * 1000)
            renderer_submit();
    }
#endif
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each pass its own submission)
    st.rt_dirty = false;
    VkCommandBuffer cb = vk_cmd();
    static int no_barrier = -1;  // (LARECOMP_VK_NO_PASS_BARRIER=1: none - unsafe, a measurement of what it costs)
    if (no_barrier < 0) no_barrier = getenv("LARECOMP_VK_NO_PASS_BARRIER") != NULL;
    extern int vk_ab(const char *what);
    if (!no_barrier && AB("nobarrier") != 1) vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        gp_mark(cb, "pass: %d color %ux%u fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    {   // one scale for the pass: attachments the size of a scaled one are scaled too (BOTW's G-buffer mixes
        // targets the scaling rule takes and ones it does not; the unscaled ones got only part of the pass)
        NvnTexture *all[9];
        int n = 0;
        for (int i = 0; i < st.rt.ncolor && i < 8; i++) if (st.rt.color[i]) all[n++] = st.rt.color[i];
        if (st.rt.depth) all[n++] = st.rt.depth;
        for (int i = 0; i < n; i++) {
            if (!renderer_texture_scalable(all[i])) continue;
            for (int k = 0; k < n; k++)
                if (all[k]->width == all[i]->width && all[k]->height == all[i]->height) vk_make_scalable_like(all[k], all[i]);
            break;
        }
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    {   extern NvnTexture *g_pass_t0; extern int g_pass_layer;
        g_pass_t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        g_pass_layer = st.rt.ncolor && st.rt.has_cview[0] && (st.rt.cview[0].setmask & 2) ? st.rt.cview[0].min_layer : 0;
        if (st.rt.ncolor && st.rt.has_cview[0] && (st.rt.cview[0].setmask & 1) && st.rt.cview[0].base_level) g_pass_layer = -1;  // (mips: not these)
    }
    wb_npass = 0;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (wb_eligible(t, b, lv) && !(st.rt.has_cview[i] && ((st.rt.cview[i].setmask & ~3u) || st.rt.cview[i].min_layer)))
            wb_pass[wb_npass++] = t;  // (a view of its level 0 / layer 0 only)
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    // The same image in two color slots: only the one the draws write stays bound. BOTW's G-buffer binds its albedo and
    // normals targets twice each (slots 1/2, 3/4), writing one slot of each pair (channel mask f0f0f0ff); a tiler keeps
    // a copy per slot and stores both at the end of the pass - on Qualcomm's driver the masked slot's stale copy landed
    // last: no albedo, Link a black silhouette, the scene only fog. (A later draw writing a dropped slot: a new pass.)
    pass_dup_null = 0;
    for (int i = 1; i < st.rt.ncolor; i++)
        for (int j = 0; j < i && ca[i].imageView; j++) {
            if (ca[j].imageView != ca[i].imageView) continue;
            bool wi = (st.channel_mask >> (i * 4)) & 0xF, wj = (st.channel_mask >> (j * 4)) & 0xF;
            int drop = wj || !wi ? i : j;
            ca[drop].imageView = VK_NULL_HANDLE;
            pass_color[drop] = VK_FORMAT_UNDEFINED;
            pass_dup_null |= 1u << drop;
        }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    pass_w = minw; pass_h = minh;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    vk_begin_rendering(cb, &ri, pass_color, pass_depth != VK_FORMAT_UNDEFINED ? pass_depth : pass_stencil,
                       (VkSampleCountFlagBits)pass_samples);
    if (g_debug_frame) pass_dump_b = st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->backend : NULL;
    pass_dump_layer = ~0u;
    if (g_debug_frame && !pass_dump_b && st.rt.depth && st.rt.depth->backend) {  // depth only (shadow maps)
        pass_dump_b = st.rt.depth->backend;
        pass_dump_layer = st.rt.has_dview && (st.rt.dview.setmask & 2) ? st.rt.dview.min_layer : 0;
    }
    {   extern uint64_t nvn_watch_texaddr(void);
        uint64_t wa = nvn_watch_texaddr();
        for (int i = 0; wa && i < st.rt.ncolor; i++)
            if (st.rt.color[i] && st.rt.color[i]->addr <= wa && wa < st.rt.color[i]->addr + tegra_total_size(&st.rt.color[i]->layout))
                LOG("[watch] frame %llu: render target %d %ux%ux%u fmt %#x target %u at %llx (%p) view %d setmask %#x layer %u+%u level %u\n",
                    (unsigned long long)renderer_present_count(), i, st.rt.color[i]->width, st.rt.color[i]->height,
                    st.rt.color[i]->depth, st.rt.color[i]->format, st.rt.color[i]->target, (unsigned long long)st.rt.color[i]->addr,
                    (void *)st.rt.color[i], st.rt.has_cview[i], st.rt.cview[i].setmask, st.rt.cview[i].min_layer,
                    st.rt.cview[i].num_layers, st.rt.cview[i].base_level);
    }
    ss_forget();  // (whatever ran between the passes may have set its own state)
    in_pass = true;
    pass_written = 0;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static bool g_wide_backdrop;  // (this draw is a layout pane showing a target widened to the window: cbuf_info, scissor_rect)
static bool g_off_layout;  // (this draw is a menu layout's pane placed past the 16:9 screen's sides: scissor_rect)

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    float sx = vp_sx, sy = vp_sy, ox = vp_ox, oy = vp_oy;
    {   // Widescreen (1.8.2): the HUD's camera widens with the scene's (widescreen.c rt_persp_update), which puts the 16:9
        // layout in the centred 16:9 part of the wide final image - a scissor of part of that image (the minimap's) goes
        // there too: scaled straight across, it cut the minimap's left half off
        extern bool widescreen_ui_centred(void);
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        if (t0 && !ox && !oy && fabsf(sx - sy) > 0.01f * sy && !(x <= 0 && y <= 0 && x + sw >= (int)t0->width && y + sh >= (int)t0->height) &&
            widescreen_ui_centred() && vk_texture_scale_group(t0) == RS_OUT) {
            // (and moves with the HUD's panes out to the edge: widescreen_hud_shift, in 1280-wide layout units)
            extern float widescreen_hud_shift(float x);
            float k = t0->width / 1280.0f;
            x += (int)lroundf(widescreen_hud_shift(((float)x + sw * 0.5f - t0->width * 0.5f) / k) * k);
            float u = fminf(sx, sy);
            ox = ((float)w - t0->width * u) * 0.5f, oy = ((float)h - t0->height * u) * 0.5f;
            sx = sy = u;
        }
    }
    if (sx != 1 || sy != 1 || ox != 0 || oy != 0) {
        double x0 = floor(x * (double)sx + ox), y0 = floor(y * (double)sy + oy);
        double x1 = ceil(((double)x + sw) * sx + ox), y1 = ceil(((double)y + sh) * sy + oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    {   // ... and in the menus (pause, map) their layouts' panes and text stay in the 16:9 screen's part: pieces it
        // never shows (a black box right of the inventory's item description, the pages sliding in and out). The panes
        // as wide as the layout (the bars across the top and bottom) and the pictures of the world reach past it
        // (g_off_layout, draw_)
        extern bool widescreen_ui_centred(void);
        extern bool widescreen_in_menu(void);
        extern float widescreen_factor(void);
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        float g = widescreen_factor();
        if (t0 && g_off_layout && g > 1.001f && widescreen_ui_centred() && widescreen_in_menu() &&
            vk_texture_scale_group(t0) == RS_OUT) {
            int b0 = (int)floorf(w * (1 - 1 / g) * 0.5f), b1 = w - b0;
            if (x < b0) { sw -= b0 - x; x = b0; }
            if (x + sw > b1) sw = b1 - x;
        }
    }
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    if (!ss_need(SS_SC, !memcmp(&r, &ss.sc, sizeof r))) return;
    ss.sc = r;
    vkCmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    if (!ss_need(SS_VP, !memcmp(&vp, &ss.vp, sizeof vp))) return;
    ss.vp = vp;
    vkCmdSetViewport(vk_cmd(), 0, 1, &vp);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
// (the last two are extended dynamic state 2's: without it, pipeline state - PipeKey.prim_restart, depth bias always on;
// without extended dynamic state at all, the eight before them too - PipeKey.fx, clear_pipeline)
#define NDYN_CORE 7  // (Vulkan 1.0's, the first ones above)
#define NDYN (!g_vk.eds ? NDYN_CORE : sizeof DYN / sizeof DYN[0] - (g_vk.eds2 ? 0 : 2))

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    vkCmdSetDepthBias(cb, 0, 0, 0);
    if (g_vk.eds2) vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    if (g_vk.eds2) vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    if (!g_vk.eds) {  // (no extended dynamic state: as clear() sets them)
        ds.depthTestEnable = ds.depthWriteEnable = depth;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        ds.stencilTestEnable = stencil;
        ds.front = ds.back = (VkStencilOpState){VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS};
    }
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    {   // (no dynamic rendering: against a compatible render pass, rpcompat.c)
        VkRenderPass crp = vk_rp_compat(pass_ncolor, pass_color, pass_depth, pass_stencil, ms.rasterizationSamples);
        if (crp) pi.pNext = rinfo.pNext, pi.renderPass = crp;
    }
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    ss_forget();  // (its own pipeline and state: the next draw sets everything again)
    dyn_defaults(cb);
    if (depth) { vkCmdSetDepthTestEnable(cb, VK_TRUE); vkCmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    ss.known &= ~SS_SETS;  // (another layout: the draws' sets are bound again)
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 96
#ifdef __ANDROID__
// Reflection is immutable once a program is published. Partition its bindings once, rather than scanning both
// stages twice per draw. Only metadata is reused: live buffers, texture views and UBO contents are still resolved.
typedef struct { uint8_t regular[MAX_BIND], deferred[MAX_BIND]; int nregular, ndeferred; } DescriptorPlan;
static void descriptor_plan(const VkBindingInfo *binds, int count, uint32_t set, DescriptorPlan *plan) {
    plan->nregular = plan->ndeferred = 0;
    for (int i = 0; i < count; i++) {
        const VkBindingInfo *b = &binds[i];
        if (b->set != set) continue;
        if ((b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) ||
            (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) || b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            plan->regular[plan->nregular++] = (uint8_t)i;
        else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                 (b->binding == 18 || b->binding == 19 || b->binding == GMAP_BUFFER_INDEX || b->binding == CBPATCH_BUFFER_INDEX))
            plan->deferred[plan->ndeferred++] = (uint8_t)i;
    }
}
#endif
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // guest addresses retained for translation and diagnostics
    size_t const_bytes[2];
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
#ifdef __ANDROID__
    DescriptorPlan descriptor_plans[2];
#endif
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    bool vs_uses_vid;
    uint32_t fs_out_mask;  // fi->out_mask: the render targets the fragment shader writes (read by every draw; in
                           // ShaderInfo it sits 1.4 KB in, past the 64 textures - a cache miss each draw, ~7% of the
                           // render thread on the POCO F7 Ultra: draw_writes)
    uint8_t fs_gather;  // (up_fix) the fragment stage's first gathered texture: 0 not looked yet, 1 none, 2 + its slot
    _Atomic int state;  // 0 building, 1 ready, 2 failed
} Prog;

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
#ifdef __ANDROID__
    for (int set = 0; set < nsets; set++) descriptor_plan(p->binds, p->nbinds, (uint32_t)set, &p->descriptor_plans[set]);
#endif
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("LARECOMP_SAVE_MSL")) return;
#ifdef __ANDROID__
    char path[1100];  // (in the data folder, where adb reads it: <external files>/build/glsl)
    snprintf(path, sizeof path, "%s/build", rt_data_root());
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/build/glsl", rt_data_root());
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/build/glsl/%llx_%s.glsl", rt_data_root(), (unsigned long long)addr, suffix);
#else
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
#endif
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

#ifdef __ANDROID__
// LARECOMP_SHADER_SUBST=<folder>: a translated shader replaced by <folder>/<code address>_<vs|fs>.glsl when there is
// one (the names build/glsl uses), for measuring a hand-edited variant in the game (GPU times, LARECOMP_SHADER_STATS)
static void subst_glsl(ShaderInfo *si, uint64_t addr, const char *suffix) {
    const char *dir = getenv("LARECOMP_SHADER_SUBST");
    if (!dir || !si || !si->msl) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%llx_%s.glsl", dir, (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (s && fread(s, 1, (size_t)n, f) == (size_t)n) {
        s[n] = 0;
        free(si->msl);
        si->msl = s;
        LOG("[shader] %s substituted\n", path);
    } else free(s);
    fclose(f);
}
#endif

static void compile_program(void *arg) {
    Prog *p = arg;
    size_t nv = 0, nf = 0;
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv);
    uint32_t *sf = sv ? vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &nf) : NULL;
    if (!sv || !sf) { free(sv); free(sf); atomic_store(&p->state, 2); return; }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    free(sv);
    free(sf);
    if (!p->vs || !p->fs) {
        if (p->vs) vkDestroyShaderModule(g_vk.dev, p->vs, NULL);
        if (p->fs) vkDestroyShaderModule(g_vk.dev, p->fs, NULL);
        p->vs = p->fs = VK_NULL_HANDLE;
        atomic_store(&p->state, 2);
        return;
    }
#ifdef __ANDROID__
    {   // Dynamic uniform buffers: every draw copies its constant buffers to a new place in transient memory, and with
        // the offsets inside the sets hardly any set matched the previous draw's - one allocated and written per stage
        // per draw (Qualcomm's driver: ~18% of the render thread). With the offsets given at bind time (stage_set),
        // draws of the same buffers and textures share their sets. The game's constant buffers first, then the
        // runtime's tables, up to the device's limit. LARECOMP_NO_DYN_UBO=1: off.
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_DYN_UBO") != NULL;
        uint32_t lim = off ? 0 : g_vk.props.limits.maxDescriptorSetUniformBuffersDynamic, used = 0;
        if (lim > 64) lim = 64;  // (draw_'s offset arrays)
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < p->nbinds && used < lim; i++) {
                VkBindingInfo *b = &p->binds[i];
                bool game = b->binding >= 1 && b->binding < 17;
                if (b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || game != (pass == 0)) continue;
                b->type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                used++;
            }
    }
#endif
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&p->state, p->vs && p->fs ? 1 : 2);
}

#ifdef __ANDROID__
static uint64_t hmix(uint64_t h, const void *p, size_t n);  // (below; LARECOMP_DUMP_FS)
#endif

#ifdef __ANDROID__
// The structured control flow (maxwell_structured) unless LARECOMP_NO_STRUCTURE - and not on Qualcomm's own driver,
// whose shader compiler crashed on it inside vkCreateGraphicsPipelines (libllvm-qgl, 2026-10-16: the overworld) -
// LARECOMP_STRUCTURE=1 there anyway
static bool structure_on(void) {
    static int on = -1;
    if (on < 0) {
        extern bool vk_driver_is_qualcomm(void);
        on = !getenv("LARECOMP_NO_STRUCTURE") && (!vk_driver_is_qualcomm() || getenv("LARECOMP_STRUCTURE"));
        if (!on && vk_driver_is_qualcomm()) LOG("[vk] Qualcomm's driver: shaders keep their branch state machine (its compiler)\n");
    }
    return on;
}
#endif

// Qualcomm's driver with the structured control flow (LARECOMP_STRUCTURE=1): its compiler crashed on a shader whose
// structured form has a GLSL switch (a jump table) - those keep their state machine there. LARECOMP_QCOM_SWITCH=1: not.
static bool qcom_no_switch(void) {
    static int on = -1;
    if (on < 0) {
        extern bool vk_driver_is_qualcomm(void);
        on = vk_driver_is_qualcomm() && !getenv("LARECOMP_QCOM_SWITCH");
    }
    return on;
}

#ifdef __ANDROID__
static bool cbwin_wanted(void);  // (below gmem_placed)
#endif

static void translate_program(void *arg) {
    Prog *p = arg;
    pthread_mutex_lock(&translate_lock);
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
#ifdef __ANDROID__
    maxwell_no_int64 = !g_vk.shader_int64 || g_settings.shader_int32 || getenv("LARECOMP_VK_INT32") != NULL;
    maxwell_gmem_map_all = gcopy_mode || gwin_mode;
    maxwell_gmem_miss_log = gcopy_mode && !getenv("LARECOMP_NO_GMISS");
    if (getenv("LARECOMP_GMISS_STAGES"))  // (a test: which stages log misses - 1 vertex, 2 fragment, 32 compute)
        maxwell_gmem_miss_stages = (unsigned)strtoul(getenv("LARECOMP_GMISS_STAGES"), NULL, 0);
    maxwell_brx_window = !getenv("LARECOMP_BRX_ALL_LABELS");
    maxwell_static_stacks = !getenv("LARECOMP_DYN_STACKS");
    maxwell_structured = structure_on();
    maxwell_no_switch = qcom_no_switch();
    maxwell_no_local_struct = qcom_no_switch() && !getenv("LARECOMP_QCOM_LOCAL");
    maxwell_native_half = !getenv("LARECOMP_BITWISE_HALF");
    maxwell_fast_minmax = !getenv("LARECOMP_SAFE_MATH");
    maxwell_cb_window = cbwin_wanted();
#endif
    p->vi = maxwell_translate_bounded((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], p->const_bytes[0], STAGE_VERTEX, &p->vkey);
    if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        p->fi = maxwell_translate_bounded((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], p->const_bytes[1], STAGE_FRAGMENT, &p->fkey);
        p->fs_out_mask = p->fi ? p->fi->out_mask : 0;
    }
    pthread_mutex_unlock(&translate_lock);
    if (!p->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        atomic_store(&p->state, 2);
        return;
    }
    if (!p->fi->msl) {
        LOG("[shader] FS translate failed: %s\n", p->fi->error);
        atomic_store(&p->state, 2);
        return;
    }
#ifdef __ANDROID__
    {   // LARECOMP_DUMP_FS=<fs hash, as [gpuprof] / [fshash] print it>: that fragment shader's GLSL (and its vertex
        // shader's) to build/fs_<hash>.glsl, for looking at a slow one
        const char *want = getenv("LARECOMP_DUMP_FS");
        uint64_t h = hmix(1469598103934665603ull, (const void *)(uintptr_t)(p->code[1] + 0x30), 0x200);
        bool hit = false;  // (several: "+"-separated)
        for (const char *e = want; e && *e && !hit;) {
            hit = strtoull(e, (char **)&e, 16) == h;
            while (*e == '+' || *e == ',') e++;
        }
        if (hit) {
            char path[96];  // (one file per vertex shader: an effect's fragment shader is shared by many)
            snprintf(path, sizeof path, "build/fs_%016llx_vs%llx.glsl", (unsigned long long)h, (unsigned long long)p->code[0]);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "// fragment shader %016llx (code %llx)\n%s\n// ---- vertex shader (code %llx)\n%s\n",
                        (unsigned long long)h, (unsigned long long)p->code[1], p->fi->msl, (unsigned long long)p->code[0], p->vi->msl);
                fclose(f);
                LOG("[shader] dumped %s\n", path);
            }
        }
    }
    subst_glsl(p->vi, p->code[0], "vs");
    subst_glsl(p->fi, p->code[1], "fs");
    {   // LARECOMP_SHADER_PATCH=<file> (diagnosis): lines "<fs hash> <vs|fs> <old text>\t<new text>" - every first
        // occurrence of the old text replaced in that stage of the programs with that fragment shader. The effects
        // that share one (BOTW's particles) each have their own vertex shader, at addresses that change from frame to
        // frame: LARECOMP_SHADER_SUBST's files can't name them.
        const char *pf = getenv("LARECOMP_SHADER_PATCH");
        FILE *f = pf ? fopen(pf, "rb") : NULL;
        uint64_t h = f ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(p->code[1] + 0x30), 0x200) : 0;
        char line[1024];
        while (f && fgets(line, sizeof line, f)) {
            unsigned long long lh;
            char st[4];
            int at = 0;
            if (sscanf(line, "%llx %3s %n", &lh, st, &at) != 2 || lh != h || !at) continue;
            char *from = line + at, *tab = strchr(from, '\t'), *to;
            if (!tab) continue;
            *tab = 0, to = tab + 1;
            to[strcspn(to, "\r\n")] = 0;
            ShaderInfo *si = !strcmp(st, "vs") ? p->vi : p->fi;
            char *hit = strstr(si->msl, from);
            if (!hit) { LOG("[shader] patch %llx %s: \"%s\" not found\n", lh, st, from); continue; }
            size_t nf = strlen(from), nt = strlen(to), n = strlen(si->msl);
            char *s = malloc(n - nf + nt + 1);
            memcpy(s, si->msl, (size_t)(hit - si->msl));
            memcpy(s + (hit - si->msl), to, nt);
            strcpy(s + (hit - si->msl) + nt, hit + nf);
            free(si->msl);
            si->msl = s;
            LOG("[shader] patch %llx %s (code %llx): \"%s\" -> \"%s\"\n", lh, st, (unsigned long long)p->code[0], from, to);
        }
        if (f) fclose(f);
    }
#endif
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    run_job(compile_program, p);
}

#define PROG_TAB 4096
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    if (!(np->stage_mask & 1) || !(np->stage_mask & 2)) return NULL;
    // (the previous draw's program and keys again - most draws: no hashing; the code addresses are part of it, as
    // the program object can be reused for other code)
    static NvnProgram *last_np;
    static uint64_t last_code[2];
    static ShaderKey last_vkey, last_fkey;
    static Prog *last_prog;
    if (last_prog && np == last_np && last_code[0] == np->stages[0].code_addr && last_code[1] == np->stages[1].code_addr &&
        !memcmp(vkey, &last_vkey, sizeof *vkey) && !memcmp(fkey, &last_fkey, sizeof *fkey))
        return last_prog;
    last_np = np, last_code[0] = np->stages[0].code_addr, last_code[1] = np->stages[1].code_addr;
    last_vkey = *vkey, last_fkey = *fkey;
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = np->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    h = hash_words(h, vkey, sizeof *vkey);  // (8 bytes a step: the table is in memory only)
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
#ifdef __ANDROID__
    if (fkey->no_ds_write) { h ^= 0xE1; h *= 0x100000001B3ull; }  // (keys without it hash as before)
#endif
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) return last_prog = prog_tab[slot].prog;
    Prog *p = calloc(1, sizeof *p);
    last_prog = p;
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
        p->const_bytes[i] = np->stages[i].const_offset ? np->stages[i].const_size : 0;
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    run_translate(translate_program, p);
    return p;
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    // integer-to-float (NVN *_UI2F / *_I2F: the integer's value as a float, the shader reads floats) - Vulkan's
    // USCALED / SSCALED; none at 32 bits there: those are read as the integer's bits (none seen in BOTW)
    case 0x51: return VK_FORMAT_R8_USCALED;
    case 0x52: return VK_FORMAT_R8_SSCALED;
    case 0x53: return VK_FORMAT_R16_USCALED;
    case 0x54: return VK_FORMAT_R16_SSCALED;
    case 0x55: return VK_FORMAT_R32_UINT;
    case 0x56: return VK_FORMAT_R32_SINT;
    case 0x57: return VK_FORMAT_R8G8_USCALED;
    case 0x58: return VK_FORMAT_R8G8_SSCALED;
    case 0x59: return VK_FORMAT_R16G16_USCALED;
    case 0x5A: return VK_FORMAT_R16G16_SSCALED;
    case 0x5B: return VK_FORMAT_R32G32_UINT;
    case 0x5C: return VK_FORMAT_R32G32_SINT;
    case 0x5D: return VK_FORMAT_R8G8B8_USCALED;
    case 0x5E: return VK_FORMAT_R8G8B8_SSCALED;
    case 0x5F: return VK_FORMAT_R16G16B16_USCALED;
    case 0x60: return VK_FORMAT_R16G16B16_SSCALED;
    case 0x61: return VK_FORMAT_R32G32B32_UINT;
    case 0x62: return VK_FORMAT_R32G32B32_SINT;
    case 0x63: return VK_FORMAT_R8G8B8A8_USCALED;
    case 0x64: return VK_FORMAT_R8G8B8A8_SSCALED;
    case 0x65: return VK_FORMAT_R16G16B16A16_USCALED;
    case 0x66: return VK_FORMAT_R16G16B16A16_SSCALED;
    case 0x67: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x68: return VK_FORMAT_R32G32B32A32_SINT;
    default: {
        static uint8_t told[256];
        if (!told[f & 255]++) LOG("[vk] vertex format nvn %#x not mapped: read as RGBA32F\n", f);
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R8G8B8_USCALED: return VK_FORMAT_R8G8B8A8_USCALED;
    case VK_FORMAT_R8G8B8_SSCALED: return VK_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_R16G16B16_USCALED: return VK_FORMAT_R16G16B16A16_USCALED;
    case VK_FORMAT_R16G16B16_SSCALED: return VK_FORMAT_R16G16B16A16_SSCALED;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
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

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// The draw's states set dynamically with extended dynamic state, baked into the pipeline where the driver lacks it
// (g_vk.eds false: Mali's older drivers); all zero otherwise, so the keys stay as they were. (Depth bias and primitive
// restart: as without extended dynamic state 2, PipeKey.prim_restart.) From LAS Recomp for Android (exec.c PipeFx).
typedef struct {
    uint8_t on, cull, ff, topo, dt, dw, dop, sten;
    uint8_t sop[2][4];  // per face: fail, pass, depth fail, compare
} PipeFx;

// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill, depth_clamp;
    uint8_t prim_restart;  // (no extended dynamic state 2 only: primitive restart is pipeline state then)
    uint32_t write_mask[8];
    PipeFx fx;
} PipeKey;

typedef struct { PipeKey k; uint64_t hash; VkPipeline pipe; _Atomic int state; } Pipe;  // state: 0 building, 1 ready, 2 failed
#define PIPE_TAB 8192
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    PipeKey *k = &pp->k;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->prog->fs, .pName = "main"}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class), .primitiveRestartEnable = k->prim_restart};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    // (no extended dynamic state 2: depth bias always on, "off" being a bias of zero - draw_ sets it either way)
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1,
                                                 .depthClampEnable = k->depth_clamp, .depthBiasEnable = !g_vk.eds2};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    if (k->fx.on) {  // (no extended dynamic state: the draw's own, PipeFx)
        const PipeFx *x = &k->fx;
        ia.topology = (VkPrimitiveTopology)x->topo;
        rs.cullMode = x->cull;
        rs.frontFace = (VkFrontFace)x->ff;
        ds.depthTestEnable = x->dt;
        ds.depthWriteEnable = x->dw;
        ds.depthCompareOp = (VkCompareOp)x->dop;
        ds.stencilTestEnable = x->sten;
        ds.front = (VkStencilOpState){(VkStencilOp)x->sop[0][0], (VkStencilOp)x->sop[0][1], (VkStencilOp)x->sop[0][2], (VkCompareOp)x->sop[0][3]};
        ds.back = (VkStencilOpState){(VkStencilOp)x->sop[1][0], (VkStencilOp)x->sop[1][1], (VkStencilOp)x->sop[1][2], (VkCompareOp)x->sop[1][3]};
    }
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout};
    {   // (no dynamic rendering: against a compatible render pass, rpcompat.c)
        VkRenderPass crp = vk_rp_compat(k->ncolor, k->color, k->depth, k->stencil, ms.rasterizationSamples);
        if (crp) pi.pNext = ri.pNext, pi.renderPass = crp;
    }
#ifdef __ANDROID__
    if (g_vk.exec_stats) pi.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    {   // LARECOMP_LOG_PIPE=1: each pipeline's shaders before the driver compiles them (a crash in its compiler: the
        // last line names the pair; their sources with LARECOMP_SAVE_MSL, build/glsl/<address>_vs|fs.glsl)
        static int logp = -1;
        if (logp < 0) logp = getenv("LARECOMP_LOG_PIPE") != NULL;
        if (logp) LOG("[pipe] compiling vs %llx fs %llx\n", (unsigned long long)k->prog->code[0], (unsigned long long)k->prog->code[1]);
    }
#endif
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pp->pipe);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
#ifdef __ANDROID__
    if (r == VK_SUCCESS && g_vk.exec_stats) {  // (LARECOMP_SHADER_STATS) one line per compiled stage
        uint64_t fsh = hmix(1469598103934665603ull, (const void *)(uintptr_t)(k->prog->code[1] + 0x30), 0x200);
        VkPipelineInfoKHR pinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pp->pipe};
        VkPipelineExecutablePropertiesKHR props[4];
        uint32_t ne = 4;
        for (uint32_t e = 0; e < 4; e++) props[e] = (VkPipelineExecutablePropertiesKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
        if (vkGetPipelineExecutablePropertiesKHR(g_vk.dev, &pinfo, &ne, props) >= 0) {
            for (uint32_t e = 0; e < ne; e++) {
                VkPipelineExecutableInfoKHR ei = {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = pp->pipe, .executableIndex = e};
                VkPipelineExecutableStatisticKHR sts[32];
                uint32_t ns = 32;
                for (uint32_t q = 0; q < 32; q++) sts[q] = (VkPipelineExecutableStatisticKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
                if (vkGetPipelineExecutableStatisticsKHR(g_vk.dev, &ei, &ns, sts) < 0) continue;
                char line[1024];
                int o = snprintf(line, sizeof line, "[stats] fs %016llx vs %llx fs %llx %s:", (unsigned long long)fsh,
                                 (unsigned long long)k->prog->code[0], (unsigned long long)k->prog->code[1], props[e].name);
                for (uint32_t q = 0; q < ns && o < (int)sizeof line - 64; q++) {
                    if (sts[q].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR)
                        o += snprintf(line + o, sizeof line - o, " %s=%llu", sts[q].name, (unsigned long long)sts[q].value.u64);
                    else if (sts[q].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR)
                        o += snprintf(line + o, sizeof line - o, " %s=%lld", sts[q].name, (long long)sts[q].value.i64);
                }
                LOG("%s\n", line);
            }
        }
    }
#endif
    atomic_fetch_add(&stat_compiles, 1);
    if (r != VK_SUCCESS) {  // (each failing key once: its draws are dropped)
        static int n;
        if (n++ < 20) LOG("[vk] graphics pipeline failed (%d): vs %llx fs %llx\n", (int)r,
                          (unsigned long long)k->prog->code[0], (unsigned long long)k->prog->code[1]);
    }
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static TexBackend *feedback_snap[4];  // (the snapshot copy itself: array views of it are made at binding)
static int nfeedback;
static NvnTexture *inplace_tex[4];  // (targets this draw samples in place while writing them: resolve_feedback)
static int ninplace;
static int g_inplace_copy = -1;  // (those targets copied instead; -1: not decided yet this boot - vk_exec_reset)
static uint64_t g_cur_fs_hash;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static bool snap_fb_dirty[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++)
        if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_fb_dirty[i] = false; snap_why[i] = snap_reason; }
}
static void snapshots_mark_fb_dirty(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if (snap_tex[i] == t && snap_ok[i]) snap_fb_dirty[i] = true;
}
static void snapshots_flush_fb_dirty(const char *why) {
    for (int i = 0; i < 8; i++)
        if (snap_fb_dirty[i]) { snap_fb_dirty[i] = false; if (snap_ok[i]) { snap_ok[i] = false; snap_why[i] = why; } }
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

// A shader texture's handle: the bound texture's (64-bit handles from cbuf offset 0x20), or for a bindless access the
// 32-bit handle the shader reads from a constant buffer (translate.c: the cbuf load that defines the handle register)
static uint64_t shader_texture_handle(int stage, const ShaderTexture *st_) {
    if (st_->bindless) {
        const Range *u = st_->bindless_cbuf >= 3 && st_->bindless_cbuf < 21 ? &st.ubuf[stage][st_->bindless_cbuf - 3] : NULL;  // (cbuf_base 3)
        if (!u || !u->addr || (uint64_t)st_->bindless_offset + 4 > u->size) return 0;
        return *(const uint32_t *)(uintptr_t)(u->addr + st_->bindless_offset);
    }
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    return (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
}

static NvnTexEntry *shader_texture_entry(int stage, ShaderTexture *st_) {
    uint64_t h = shader_texture_handle(stage, st_);
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return &((NvnTexEntry *)st.tpool->entries)[tic];
}

// The 3D scene upscaled for this draw (scene_upscale): sampled instead of the scene texture, at its scale.
static struct { NvnTexture *t; TexBackend *img; VkImageView view; float sx, sy; } scene_up;

// That upscaled image seen through the game's own swizzle for the scene texture. BOTW's quick menu (weapons, shields)
// draws its 1280x720 copy of the world full-screen like a scene composite, through a view forcing alpha to one; the
// copy's alpha is 0, so the plain view drew it transparent - a black background.
static VkImageView scene_up_view(const NvnTexEntry *e) {
    const uint8_t *s = e->has_view && (e->view.setmask & 8) ? e->view.swizzle : e->tex->swizzle;
    uint8_t c[4];
    for (int i = 0; i < 4; i++) c[i] = s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
    if (!(c[0] || c[1] || c[2] || c[3])) c[0] = 2, c[1] = 3, c[2] = 4, c[3] = 5;  // (never set: identity)
    if (vk_format_alpha_one(e->tex->format))
        for (int i = 0; i < 4; i++) if (c[i] == 5) c[i] = 1;
    if (c[0] == 2 && c[1] == 3 && c[2] == 4 && c[3] == 5) return scene_up.view;
    // NVN swizzle 0 zero, 1 one, 2..5 r,g,b,a == VkComponentSwizzle - 1
    VkComponentMapping sw = {(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1), (VkComponentSwizzle)(c[2] + 1),
                             (VkComponentSwizzle)(c[3] + 1)};
    return vk_image_view(scene_up.img, VK_IMAGE_VIEW_TYPE_2D, scene_up.img->format, 0, 1, 0, 1, sw, VK_IMAGE_ASPECT_COLOR_BIT);
}
static void scene_upscale(Prog *pr, CmdRec *r);

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    NvnTexEntry *e = shader_texture_entry(stage, st_);
    return e ? e->tex : NULL;
}

static bool view_overlaps_rt(const NvnTexEntry *e, const NvnTextureView *rv, bool has_rv) {
    uint32_t r_lv = has_rv && (rv->setmask & 1) ? rv->base_level : 0;
    uint32_t r_ly = has_rv && (rv->setmask & 2) ? rv->min_layer : 0;
    if (r_lv != 0 || r_ly != 0) return false;
    uint32_t lv0 = e->has_view && (e->view.setmask & 1) ? e->view.base_level : 0;
    uint32_t nlv = e->has_view && (e->view.setmask & 1) && e->view.num_levels ? e->view.num_levels : (e->tex->levels > lv0 ? e->tex->levels - lv0 : 1);
    uint32_t ly0 = e->has_view && (e->view.setmask & 2) ? e->view.min_layer : 0;
    uint32_t nly = e->has_view && (e->view.setmask & 2) && e->view.num_layers ? e->view.num_layers : 1;
    return r_lv >= lv0 && r_lv < lv0 + nlv && r_ly >= ly0 && r_ly < ly0 + nly;
}

static bool is_attachment_entry(const NvnTexEntry *e) {
    if (!e || !e->tex) return false;
    {   // LARECOMP_FEEDBACK_ANY_LAYER=1 (test): an array / mipmapped target sampled at any of its layers or levels
        static int any = -1;
        if (any < 0) any = getenv("LARECOMP_FEEDBACK_ANY_LAYER") != NULL;
        if (any && (e->tex->layout.layers > 1 || e->tex->levels > 1))
            for (int i = 0; i < st.rt.ncolor; i++) if (e->tex == st.rt.color[i]) return true;
    }
    if (e->tex == st.rt.depth && view_overlaps_rt(e, &st.rt.dview, st.rt.has_dview != 0)) return true;
    for (int i = 0; i < st.rt.ncolor; i++)
        if (e->tex == st.rt.color[i] && view_overlaps_rt(e, &st.rt.cview[i], st.rt.has_cview[i] != 0)) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    // Every layer and level of a 2D / array texture. BOTW's terrain builds its height array (1024x1024x8 R16, 4 levels)
    // by sampling the array while it renders into it: each layer from the previous one (fs 91fe757c), each level from
    // the level above (fs 35da1c5a), both through a sampler2DArray. A one-layer copy can't be bound there - those
    // reads got an empty dummy, layers 3-7 and the mips stayed zero, and that ground was drawn flat and low
    // (forests floating below the plateau, Link sunk into the grass).
    uint32_t nl = src->depth > 1 ? 1 : (src->layers ? src->layers : 1), nm = src->levels ? src->levels : 1;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format || d->layers != nl ||
                     d->levels != nm)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = nm, .arrayLayers = nl,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = nl; d->levels = nm;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = nl > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ||
        (b->format != VK_FORMAT_D32_SFLOAT_S8_UINT && b->format != VK_FORMAT_D32_SFLOAT)) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_readback_ready((uint64_t)n * 4);
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

// (debug) LARECOMP_COPY_TRACE=<frame>: draws' attachments / sampled textures, copies and snapshots of that frame ("[ct]")
static bool ct_on(void) {
    static const char *e = (const char *)1;
    if (e == (const char *)1) e = getenv("LARECOMP_COPY_TRACE");
    if (!e) return false;
    static uint64_t memo_f = ~0ull;
    static bool memo;
    uint64_t f = renderer_present_count();
    if (f == memo_f) return memo;
    memo_f = f;
    memo = false;
    for (const char *p = e; *p;) {
        if ((uint64_t)strtoll(p, (char **)&p, 10) == f) { memo = true; break; }
        if (*p == ',') p++; else break;
    }
    if (memo) LOG("[ct] ===== frame %llu\n", (unsigned long long)f);
    return memo;
}
static void ct_draw(Prog *pr, const char *what) {
    LOG("[ct] %s fs %016llx rt", what, (unsigned long long)g_cur_fs_hash);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" c%d=%p/%#x", i, (void *)st.rt.color[i], st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d=%p/%#x%s", (void *)st.rt.depth, st.rt.depth->format, st.ds[0] && st.ds[1] ? "W" : "");
    LOG(" cm %08x vp %.0f %.0f %.0f %.0f sc %d %d %d %d | tex", st.channel_mask, st.viewport[0][0], st.viewport[0][1], st.viewport[0][2],
        st.viewport[0][3], st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < (infos[sg] ? infos[sg]->ntex : 0); i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (t) LOG(" %s%p/%#x", sg ? "" : "v:", (void *)t, t->format);
        }
    LOG("\n");
}

// ================================================================== copy elision (depth copies)
// The game copies its main depth buffer whole, every frame, into textures that are then only sampled while the
// source keeps its depth (the water / transparency depth input), or that get their depth cleared right away
// (stencil kept). At 3x internal resolution each such copy is ~0.15 ms of pure bandwidth. Instead of copying, the
// destination becomes an alias: sampling it (and copying from it) reads the source's image. The alias ends before
// anything changes either side:
//   - the source is about to be overwritten whole (both aspects cleared): the two images swap - the destination
//     keeps the contents without a copy, the source is cleared in the destination's old image;
//   - the destination is about to be overwritten in some aspects (its depth cleared): only the other aspects are
//     copied (stencil, a quarter of the bytes);
//   - anything else (a draw writing depth / stencil, a partial clear, a copy or upload into either, the
//     destination bound as an attachment, a texture freed or reallocated): the real copy is made first.
// Only same-size, same-format, single-level, single-sample depth/stencil images, whole-image copies.
// LARECOMP_VK_COPY_OPT=0: always copy (the old behaviour).
typedef struct { NvnTexture *src, *dst; VkImage simg, dimg; } CopyAlias;
extern uint32_t vk_view_gen;
static CopyAlias aliases[8];
static int naliases;
static uint64_t stat_alias_made, stat_alias_renamed, stat_alias_partial, stat_alias_copied;

static int copy_opt(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_COPY_OPT"); on = e && *e ? atoi(e) : 1; }
    return on;
}

// still valid: both textures keep the images the alias was made with (a resolution change reallocates them)
static bool alias_valid(const CopyAlias *a) {
    const TexBackend *s = a->src->backend, *d = a->dst->backend;
    return s && d && s->image == a->simg && d->image == a->dimg;
}

static void alias_drop(int i) {
    aliases[i] = aliases[--naliases];
    vk_view_gen++;  // (exec.c's per-frame view memo: the destination's views change)
}

// the real copy of the aspects asked for, into the destination's own image
static void alias_copy(CopyAlias *a, VkImageAspectFlags aspects, const char *why) {
    TexBackend *s = a->src->backend, *d = a->dst->backend;
    aspects &= d->aspect;
    if (!aspects) return;
    vk_end_pass();
    st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "copy texture (deferred, %s%s) %ux%u", why, aspects == d->aspect ? "" : ", stencil only", a->dst->width, a->dst->height);
    VkImageCopy c = {.srcSubresource = {aspects, 0, 0, 1}, .dstSubresource = {aspects, 0, 0, 1}, .extent = {d->width, d->height, 1}};
    vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    if (aspects == d->aspect) stat_alias_copied++; else stat_alias_partial++;
    if (ct_on()) LOG("[ct] alias copy %p -> %p aspects %#x (%s)\n", (void *)a->src, (void *)a->dst, aspects, why);
}

// t is about to be written; full: the aspects of it the write overwrites whole (0: unknown / partial)
static void alias_before_write(NvnTexture *t, VkImageAspectFlags full, const char *why) {
    if (!naliases || !t) return;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (!alias_valid(a)) { alias_drop(i); continue; }
        if (a->dst != t) continue;
        // (LARECOMP_VK_COPY_OPT=2: all aspects even then - for comparing the stencil-only copy's cost)
        alias_copy(a, ((TexBackend *)t->backend)->aspect & (copy_opt() == 2 ? ~0u : ~full), why);
        alias_drop(i);
    }
    NvnTexture *heir = NULL;  // t overwritten whole: one destination can take its image instead of a copy
    if (t->backend && (((TexBackend *)t->backend)->aspect & ~full) == 0)
        for (int i = 0; i < naliases && !heir; i++) if (aliases[i].src == t) heir = aliases[i].dst;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (a->src != t || a->dst == heir) continue;
        alias_copy(a, ((TexBackend *)a->dst->backend)->aspect, why);
        alias_drop(i);
    }
    int rename = -1;
    for (int i = 0; i < naliases; i++) if (heir && aliases[i].src == t && aliases[i].dst == heir) rename = i;
    if (rename >= 0) {
        CopyAlias *a = &aliases[rename];
        TexBackend *s = a->src->backend, *d = a->dst->backend;
        vk_end_pass();
        st.rt_dirty = true;
        VkImage im = s->image; s->image = d->image; d->image = im;
        VmaAllocation m = s->mem; s->mem = d->mem; d->mem = m;
        vk_views_forget(s);
        vk_views_forget(d);
        stat_alias_renamed++;
        if (ct_on()) LOG("[ct] alias rename %p -> %p (%s)\n", (void *)a->src, (void *)a->dst, why);
        alias_drop(rename);
    }
}

// the texture whose image holds t's contents (t itself unless it is an alias)
static NvnTexture *alias_read(NvnTexture *t) {
    for (int i = 0; i < naliases; i++)
        if (aliases[i].dst == t) {
            if (alias_valid(&aliases[i])) return aliases[i].src;
            alias_drop(i);
            return t;
        }
    return t;
}

// copy_texture: a whole-image copy that can be an alias instead (true: done)
static bool alias_make(NvnTexture *src, NvnTexture *dst, TexBackend *s, TexBackend *d) {
    if (!copy_opt() || src == dst) return false;
    if (!(s->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) return false;
    if (s->format != d->format || s->width != d->width || s->height != d->height || s->aspect != d->aspect || s->levels != 1 ||
        d->levels != 1 || s->layers != 1 || d->layers != 1 || s->depth != 1 || d->depth != 1 || s->samples != 1 || d->samples != 1 ||
        s->base_type != d->base_type || s->presented || d->presented || !s->image || !d->image || !s->gpu_owned || !d->gpu_owned)
        return false;
    // the destination's previous contents (or alias) are replaced whole; a destination that is itself the source of
    // aliases gets those resolved first
    alias_before_write(dst, d->aspect, "copy dst");
    src = alias_read(src);  // (a copy of an alias: of its source)
    s = src->backend;
    if (src == dst) return true;  // (copied back onto itself)
    if (naliases == (int)(sizeof aliases / sizeof aliases[0])) return false;
    aliases[naliases++] = (CopyAlias){src, dst, s->image, d->image};
    vk_view_gen++;
    stat_alias_made++;
    return true;
}

// textures.c: t is being freed - an alias source hands its contents over first
void vk_alias_texture_destroyed(NvnTexture *t) {
    if (naliases) alias_before_write(t, 0, "freed");
}
void vk_alias_reset(void) { naliases = 0; }  // (reboot: every texture goes)

// (renderer_stats, once per stats interval) copies made aliases / renamed / partly or fully copied later
void vk_alias_stats(void) {
    static int n;
    if (!stat_alias_made || (n++ > 5 && n % 30)) return;
    LOG("[vk] copy alias: %llu made, %llu renamed, %llu stencil-only, %llu copied later (live %d)\n",
        (unsigned long long)stat_alias_made, (unsigned long long)stat_alias_renamed, (unsigned long long)stat_alias_partial,
        (unsigned long long)stat_alias_copied, naliases);
    stat_alias_made = stat_alias_renamed = stat_alias_partial = stat_alias_copied = 0;
}

// draws: attachments written (or a destination bound at all) end the aliases involved
static void alias_draw_check(void) {
    if (!naliases) return;
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) alias_before_write(st.rt.color[i], 0, "color target");
    NvnTexture *d = st.rt.depth;
    if (!d) return;
    bool writes = (st.ds[0] && st.ds[1]) || (st.ds[3] && ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF));
    bool is_dst = false;
    for (int i = 0; i < naliases; i++) is_dst |= aliases[i].dst == d;
    if (writes || is_dst) alias_before_write(d, 0, writes ? "depth write" : "dst bound");
}

// The attachments a draw writes: bit i color target i (its shader outputs it and its channel mask lets it through),
// bit 8 depth / stencil.
static uint32_t draw_writes(const Prog *pr) {
    uint32_t w = 0;
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((pr->fs_out_mask >> (i * 4)) & 0xF) && ((st.channel_mask >> (i * 4)) & 0xF)) w |= 1u << i;
    if (st.rt.depth && ((st.ds[0] && st.ds[1]) || (st.ds[3] && ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF)))) w |= 1u << 8;
    return w;
}

static void resolve_feedback(Prog *pr, uint32_t draw_written) {
    nfeedback = 0;
    ninplace = 0;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    // An attachment only read - not written by this draw nor by an earlier one of its pass - is sampled as it is: the
    // memory holds what the pass loaded, and Vulkan allows that read (no feedback loop without a write). BOTW's decals,
    // light and terrain passes test depth without writing it, or sample a target they mask out: ~15 copies a frame of
    // 1600x900 targets (~2.5 ms of GPU) each time it changed. LARECOMP_FEEDBACK_ALWAYS=1: a copy whenever bound.
    static int always = -1;
    if (always < 0) always = getenv("LARECOMP_FEEDBACK_ALWAYS") != NULL;
    uint32_t written = draw_written | (in_pass && !st.rt_dirty ? pass_written : 0);
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexEntry *e = shader_texture_entry(sg, &infos[sg]->tex[i]);
            if (!is_attachment_entry(e)) continue;
            NvnTexture *t = e->tex;
            if (!always) {
                uint32_t bit = 0;
                if (t == st.rt.depth) bit |= 1u << 8;
                for (int k = 0; k < st.rt.ncolor; k++) if (st.rt.color[k] == t) bit |= 1u << k;
                if (bit && !(written & bit)) {
                    stat_feedback_reads++;
                    if (g_debug_frame && t == st.rt.depth) {  // (debug frames: the depth sampled in place, as it is now)
                        static int nd;
                        char path[128];
                        snprintf(path, sizeof path, "build/fbdepth_%03d_inplace.pgm", nd++);
                        vk_end_pass();
                        debug_save_depth(vk_tex(t, true), path);
                    }
                    continue;
                }
                // A color target the draw writes, sampled in place too (LARECOMP_FEEDBACK_COPY=1: copied, as before).
                // BOTW's terrain decals (fs f984e489, in the G-buffer pass) read and write two of its targets, a
                // barrier every one or two: 32 pairs of 1280x720 copies a frame on the Great Plateau, 11 of its 40 ms
                // of GPU. In place, where a copy was made the pass ends instead, if it wrote the target - begin_pass's
                // barrier makes those writes visible, as the copy had them - and the draws that would have shared the
                // copy (until a barrier, a change of targets or a write by a draw not reading it: the snapshot
                // bookkeeping) share that pass. Those passes of a few draws run in system memory on Turnip (~0.02 ms).
                // Only the writes before the game's last barrier (NVN's rule) left the ground dark: its draws don't
                // fence every write they then read. Sampling a target the pass writes is a feedback loop Vulkan leaves
                // undefined (without VK_EXT_attachment_feedback_loop_layout): on Turnip only, where it was checked
                // frame by frame on the Adreno 830 - Automatic keeps it to the Adreno 8xx (settings.h inplace_reads,
                // the start screen's Driver compatibility; LARECOMP_FEEDBACK_INPLACE=1 on any driver).
                if (g_inplace_copy < 0) {
                    extern bool vk_driver_is_turnip(void);
                    extern bool vk_turnip_path_on(int setting);
                    bool inplace = getenv("LARECOMP_FEEDBACK_INPLACE") || (vk_driver_is_turnip() && vk_turnip_path_on(g_settings.inplace_reads));
                    g_inplace_copy = getenv("LARECOMP_FEEDBACK_COPY") != NULL || !inplace;
                    LOG("[vk] targets a draw writes, sampled by it: %s (%s)\n", g_inplace_copy ? "copies" : "in place",
                        g_settings.inplace_reads < 0 ? "automatic" : "settings");
                }
                extern int vk_ab(const char *what);
                if (!g_inplace_copy && bit && bit < 1u << 8 && AB("copy") != 1) {
                    int si = -1;
                    for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
                    if (si < 0 || !snap_ok[si]) {
                        if (in_pass && !st.rt_dirty && (pass_written & bit)) st.rt_dirty = true, stat_inplace_breaks++;
                        if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
                        snap_ok[si] = true;
                        snap_fb_dirty[si] = false;
                    }
                    if (ninplace < 4) inplace_tex[ninplace++] = t;
                    stat_feedback_reads++;
                    continue;
                }
            }
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            if (!nosnap && si >= 0 && snap_ok[si] && existing) {
                feedback_tex[nfeedback] = t;
                feedback_snap[nfeedback] = &existing->copy;
                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            stat_feedback_copies++;
            if (ct_on()) LOG("[ct] snapshot %p %ux%u (was %s, si %d)\n", (void *)t, src->width, src->height, si >= 0 ? snap_why[si] : "new", si);
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            for (uint32_t lv = 0; lv < dst->levels; lv++) {
                uint32_t w = src->width >> lv, h = src->height >> lv;
                VkImageCopy rg = {.srcSubresource = {src->aspect, lv, 0, dst->layers}, .dstSubresource = {src->aspect, lv, 0, dst->layers},
                                  .extent = {w ? w : 1, h ? h : 1, 1}};
                vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            }
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t;
            feedback_snap[nfeedback] = dst;
            feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            snap_fb_dirty[si] = false;
            st.rt_dirty = true;
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

#ifdef __ANDROID__
// Constant buffers read in place (LARECOMP_UBO_DIRECT=1): with the guest heap placed in GPU memory (vk_heap_place) a
// constant buffer there can be bound as it is, at its offset in the heap buffer, instead of copied for every draw
// (~3 ms of a 60 fps frame in BOTW 1.8.2's world, ~8000 copies). Not the ones the game rewrites inside a command list
// (nvnCommandBufferUpdateUniformBuffer, about one per draw: a draw before the update must not see it, which only a
// copy keeps): their 256-byte blocks are marked when the update is recorded (cmdbuf.c), before the render thread
// translates that list, and stay copied.
enum { UBO_INLINE_SLOTS = 1 << 16 };
static _Atomic uint64_t ubo_inline[UBO_INLINE_SLOTS];  // block index + 1 (open addressing, never removed)
// (cbwin) beside each block, the bytes of it the updates wrote: first | end << 16 (the union of every update's)
static _Atomic uint32_t ubo_inline_span[UBO_INLINE_SLOTS];
static _Atomic uint32_t ubo_inline_gen;  // bumped when a block or its span grows (cbwin_rows' memo)
static int ubo_inline_slot(uint64_t blk) {  // its slot, -1 when not marked, -2 when the table is full
    for (uint32_t i = (uint32_t)(blk * 0x9E3779B97F4A7C15ull >> 48), n = 0; n < UBO_INLINE_SLOTS; i = (i + 1) & (UBO_INLINE_SLOTS - 1), n++) {
        uint64_t v = atomic_load_explicit(&ubo_inline[i], memory_order_acquire);
        if (!v) return -1;
        if (v == blk + 1) return (int)i;
    }
    return -2;
}
static bool ubo_inline_has(uint64_t blk) {
    return ubo_inline_slot(blk) != -1;  // (full: copy everything)
}
void vk_ubo_inline_mark(uint64_t addr, uint64_t size) {
    for (uint64_t blk = addr >> 8; size && blk <= (addr + size - 1) >> 8; blk++) {
        uint64_t lo = addr > blk << 8 ? addr - (blk << 8) : 0, hi = addr + size - (blk << 8);
        if (hi > 256) hi = 256;
        for (uint32_t i = (uint32_t)(blk * 0x9E3779B97F4A7C15ull >> 48), n = 0; n < UBO_INLINE_SLOTS; i = (i + 1) & (UBO_INLINE_SLOTS - 1), n++) {
            uint64_t v = atomic_load_explicit(&ubo_inline[i], memory_order_acquire);
            uint64_t z = 0;
            if (v != blk + 1 && !(!v && atomic_compare_exchange_strong(&ubo_inline[i], &z, blk + 1)) && z != blk + 1) continue;
            uint32_t s = atomic_load_explicit(&ubo_inline_span[i], memory_order_acquire), t;
            do {
                uint32_t a = s ? s & 0xFFFF : (uint32_t)lo, b = s ? s >> 16 : (uint32_t)hi;
                if (lo < a) a = (uint32_t)lo;
                if (hi > b) b = (uint32_t)hi;
                t = a | b << 16;
                if (t == s) break;
            } while (!atomic_compare_exchange_weak(&ubo_inline_span[i], &s, t));
            if (t != s) atomic_fetch_add(&ubo_inline_gen, 1);
            break;
        }
    }
}
static bool ubo_direct(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out);  // (below gmem_buf)
static bool cbwin_bind(int stage, int k, uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out);
static bool gmem_placed_(void);
static uint64_t cbwin_row_(uint64_t addr, uint64_t size);
#endif
// (cbwin) the patch buffers of the draw being bound, vertex and fragment: shader.h maxwell_cb_window
static struct { uint32_t m[4], rows[64][4]; } cbw_patch[2];

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
// BOTW's half-size passes on a scaled scene (from SavageWindRecomp's up_fix, MIT): the draws that bring its half-size
// buffers (lighting, the sun's shadow mask, the clouds) back to the full image pick, per pixel, the half-size texel whose
// depth is nearest (the deferred lighting's fs 966dae9aeabc743a and its variants, the clouds' fs c233c59930c7a676:
// textureGather of the half-size depth, then the sample point moved toward the best texel). The gather reads the scaled
// image's texels; the move is in the game's: the vertex shader takes the texel grid from a constant row (c[3] row 18 =
// 1600, 900, 1/1600, 1/900), the clouds their step from a fragment constant (1/800, 1/450). Scaled - Video > Internal
// resolution above the game's 1600x900 (75% or 100% of a 3200x1440 screen) - each pixel's lighting came from texels off
// at every depth edge: a dark mesh of dotted lines over the ground and along edges, gone at 50% (players, 2026-10-28).
// In draws that gather from a texture at their target's scale, a vertex constant row (w, h, 1/w, 1/h) with (w, h) the
// target's size times 1/4 .. 4 becomes that size times the gathered texture's own scale, and a fragment constant pair
// (1/w, 1/h) of the gathered texture's size its allocated texel. At the game's own size nothing changes.
// LARECOMP_NO_UPSAMPLE_FIX=1: as before.
static struct { uint32_t gw, gh; float sx, sy; uint32_t tw, th, aw, ah; } up_fix;  // (the draw in progress; sx 0: not one)

static bool up_patch(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {  // the vertex stage's texel grid
    const float *g = (const float *)(uintptr_t)addr;
    static float buf[0x10000 / 4];
    static int nlog;
    bool found = false;
    uint64_t nw = (size < sizeof buf ? size : sizeof buf) / 4;
    for (uint64_t i = 0; i + 3 < nw; i += 4) {  // (a vec4: 16-byte aligned)
        float w = g[i], h = g[i + 1];
        if (!(w >= 16 && h >= 16) || fabsf(w * g[i + 2] - 1) > 1e-5f || fabsf(h * g[i + 3] - 1) > 1e-5f) continue;
        bool match = false;
        for (int k = -2; k <= 2 && !match; k++) {
            float f = k < 0 ? 1.0f / (float)(1 << -k) : (float)(1 << k);
            match = w == (float)up_fix.gw * f && h == (float)up_fix.gh * f;
        }
        if (!match) continue;
        if (!found) memcpy(buf, g, nw * 4);
        found = true;
        buf[i] = w * up_fix.sx, buf[i + 1] = h * up_fix.sy;
        buf[i + 2] = g[i + 2] / up_fix.sx, buf[i + 3] = g[i + 3] / up_fix.sy;
        if (nlog < 4) {
            nlog++;
            LOG("[upsample] %ux%u target at %.2fx%.2f: texel grid %gx%g -> %gx%g (vertex constant buffer row %llu)\n", up_fix.gw,
                up_fix.gh, up_fix.sx, up_fix.sy, w, h, buf[i], buf[i + 1], (unsigned long long)(i / 4));
        }
    }
    if (found) *out = ubo_span(vk_transient_copy(buf, nw * 4, 256), nw * 4);
    return found;
}

static bool up_patch_frag(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {  // the fragment stage's step
    if (!up_fix.tw || !up_fix.th || (up_fix.tw == up_fix.aw && up_fix.th == up_fix.ah)) return false;
    const float *g = (const float *)(uintptr_t)addr;
    float tx = 1.0f / (float)up_fix.tw, ty = 1.0f / (float)up_fix.th;
    static float buf[0x10000 / 4];
    static int nlog;
    bool found = false;
    uint64_t nw = (size < sizeof buf ? size : sizeof buf) / 4;
    for (uint64_t i = 0; i + 1 < nw; i += 2) {  // (a vec2: 8-byte aligned)
        if (fabsf(g[i] - tx) > tx * 1e-5f || fabsf(g[i + 1] - ty) > ty * 1e-5f) continue;
        if (!found) memcpy(buf, g, nw * 4);
        found = true;
        buf[i] = 1.0f / (float)up_fix.aw, buf[i + 1] = 1.0f / (float)up_fix.ah;
        if (nlog < 4) {
            nlog++;
            LOG("[upsample] %ux%u texture at %ux%u: fragment texel size %g %g -> %g %g (word %llu)\n", up_fix.tw, up_fix.th,
                up_fix.aw, up_fix.ah, tx, ty, buf[i], buf[i + 1], (unsigned long long)i);
        }
    }
    if (found) *out = ubo_span(vk_transient_copy(buf, nw * 4, 256), nw * 4);
    return found;
}

// nn::ui2d: a pane (its vertex constants f, cb3, at least 0x230 bytes) at the layout's middle in x whose rectangle is
// as wide as the 1280-unit layout, centred - at any height and y (a full screen's black, a band across it). The
// rectangle drawn: cb3[33] (size, then origin), in the textured panes' vertex shaders and the window panes' alike (a
// window's frame pieces each; cb3[11] is the whole window's size - by it, every piece of the menu's 4590-wide windows
// counted, the hearts and the rupee too, and moved out g times farther: they jumped when the menu's page turned).
// Off the middle too when it still covers the wide screen there (half its width past the offset by 960 units: a
// menu's dark bands, 4590 wide, slide with its pages - clipped as narrower panes, they left lighter strips each side).
static bool full_width_pane(const float *f) {
    float ax = fabsf(f[19]);
    return f[132] >= 1279.5f && fabsf(f[134] + f[132] * 0.5f) < 0.5f && (ax < 1.0f || f[132] * 0.5f - ax >= 960.0f);
}

// ... or a piece of a window that is (cb3[11]: the window's size): not clipped to the 16:9 part in the menus - the
// menu's dimming is such a window, drawn in pieces (clipped, the cutout at its sides came back). Not moved by it.
static bool full_width_window(const float *f) {
    float ax = fabsf(f[19]);
    return full_width_pane(f) || (f[44] >= 1279.5f && f[44] < 100000.0f && f[45] > 0 && f[46] == 0 && f[47] == 0 &&
                                  (ax < 1.0f || f[44] * 0.5f - ax >= 960.0f));
}

// LARECOMP_LOG_PANEFIX=1 (diagnosis): each layout draw cbuf_info changes for the wide screen, and why
static void panefix_log(const char *why, const float *f) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_LOG_PANEFIX") != NULL;
    if (!on) return;
    extern bool framepace_in_menu(void);
    extern bool widescreen_in_menu(void);
    LOG("[panefix] frame %llu fs %016llx %s x %.1f r33 %.0f %.0f r11 %.0f %.0f menu %d/%d\n", (unsigned long long)renderer_present_count(),
        (unsigned long long)g_cur_fs_hash, why, f[19], f[132], f[133], f[44], f[45], framepace_in_menu(), widescreen_in_menu());
}

static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    if (g_debug_frame && getenv("LARECOMP_DUMP_CBUF_FS")) {
        // (diagnosis) LARECOMP_DUMP_CBUF_FS=<fs hash>+<fs hash>...: in debug frames, every constant buffer of those
        // fragment shaders' draws, as floats (the first 0x400 bytes)
        static uint64_t want[16];
        static int nwant = -1;
        if (nwant < 0) {
            nwant = 0;
            for (const char *e = getenv("LARECOMP_DUMP_CBUF_FS"); *e && nwant < 16;) {
                want[nwant++] = strtoull(e, (char **)&e, 16);
                while (*e == '+' || *e == ',') e++;
            }
        }
        bool hit = false;
        for (int i = 0; i < nwant; i++) hit |= want[i] == g_cur_fs_hash;
        if (hit) {
            const float *f = (const float *)(uintptr_t)r.addr;
            uint64_t n = size < 0x400 ? size / 4 : 0x100;
            LOG("[cbdump] frame %llu fs %016llx stage %d cb%d size %llx:", (unsigned long long)renderer_present_count(),
                (unsigned long long)g_cur_fs_hash, stage, k, (unsigned long long)size);
            for (uint64_t i = 0; i < n; i++) LOG(" %g", f[i]);
            LOG("\n");
        }
    }
    {   // LARECOMP_LOG_FS_CB=<fs hash>:<stage 0 vs / 1 fs>:<cbuf>:<first row>:<rows> (diagnosis): that cbuf of the first
        // draw of that fragment shader each frame, as floats - a value that jumps for one frame shows here
        static uint64_t want = ~0ull, last_frame = ~0ull;
        static int wstage, wk, wrow, wrows;
        if (want == ~0ull) {
            const char *e = getenv("LARECOMP_LOG_FS_CB");
            want = 0;
            if (e) {
                unsigned long long h;
                if (sscanf(e, "%llx:%d:%d:%d:%d", &h, &wstage, &wk, &wrow, &wrows) == 5) want = h;
            }
        }
        if (want && g_cur_fs_hash == want && stage == wstage && k == wk && renderer_present_count() != last_frame) {
            last_frame = renderer_present_count();
            char line[2048];
            int n = 0;
            const float *f = (const float *)(uintptr_t)r.addr;
            for (int i = wrow * 4; i < (wrow + wrows) * 4 && (uint64_t)(i * 4) < size && n < (int)sizeof line - 32; i++)
                n += snprintf(line + n, sizeof line - n, "%s%g", i % 4 ? " " : " | ", f[i]);
            LOG("[fscb] frame %llu cb%d:%s\n", (unsigned long long)last_frame, k, line);
        }
    }
    {   // LARECOMP_DEBUG_ON_FS_CB=<fs hash>:<stage>:<cbuf>:<float index>:<below> (diagnosis): a debug frame (as F9) the
        // frame after that float of the first draw of that fragment shader drops under <below> - the moment a hit-stop
        // starts (cb10's time step), caught without timing a key press. Three times a run at most.
        static uint64_t want = ~0ull, last_frame = ~0ull;
        static int wstage, wk, wi, fired;
        static float below, prev = 1e30f;
        if (want == ~0ull) {
            const char *e = getenv("LARECOMP_DEBUG_ON_FS_CB");
            want = 0;
            if (e) {
                unsigned long long h;
                if (sscanf(e, "%llx:%d:%d:%d:%f", &h, &wstage, &wk, &wi, &below) == 5) want = h;
            }
        }
        if (want && g_cur_fs_hash == want && stage == wstage && k == wk && renderer_present_count() != last_frame &&
            (uint64_t)(wi * 4 + 4) <= size) {
            last_frame = renderer_present_count();
            float v = ((const float *)(uintptr_t)r.addr)[wi];
            if (v < below && prev >= below && fired < 3) {
                extern volatile int g_debug_frame_request;
                g_debug_frame_request = 1;
                fired++;
                LOG("[fscb] frame %llu cb%d[%d] %g -> %g: debug frame next\n", (unsigned long long)last_frame, k, wi, prev, v);
            }
            prev = v;
        }
    }
    {   // LARECOMP_LOG_CBUF=<fs hash>: the first floats of the draws of that fragment shader get in cb3 (its stage's
        // first game buffer), as copied - a race with the game's writes shows here without a debug frame's timing
        static uint64_t want = ~0ull;
        if (want == ~0ull) want = getenv("LARECOMP_LOG_CBUF") ? strtoull(getenv("LARECOMP_LOG_CBUF"), NULL, 16) : 0;
        if (want && g_cur_fs_hash == want && k == 3 && stage != 0) {
            const float *f = (const float *)(uintptr_t)r.addr;
            LOG("[cbuf] %llx: %.3f %.3f %.3f %.3f + %.3f %.3f %.3f %.3f\n", (unsigned long long)r.addr, f[0], f[1], f[2],
                f[3], f[4], f[5], f[6], f[7]);
        }
        if (want && g_cur_fs_hash == want && k == 3 && stage == 0) {  // (and the vertex stage's, 40 floats)
            const float *f = (const float *)(uintptr_t)r.addr;
            char line[640];
            int n = 0;
            for (int i = 0; i < 40 && i * 4 < (int)size; i++) n += snprintf(line + n, sizeof line - n, "%s%.3f", i % 4 ? " " : " | ", f[i]);
            LOG("[cbuf] vs %llx size %llx:%s\n", (unsigned long long)r.addr, (unsigned long long)size, line);
        }
    }
    if (up_fix.sx > 0 && (stage == 0 || stage == 1)) {  // (a scaled scene's half-size upsample: up_fix - before the
        VkDescriptorBufferInfo d;                       //  in-place reads, which would bind the game's own values)
        if (stage == 0 ? up_patch(r.addr, size, &d) : up_patch_frag(r.addr, size, &d)) return d;
    }
#ifdef __ANDROID__
    {
        VkDescriptorBufferInfo d;
        if (ubo_direct(r.addr, size, &d)) return d;
    }
#endif
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    if (stage == 0 && idx == 0 && size >= 64) {
        // Widescreen: what the HUD's layouts draw through the widened camera moves out towards the screen's edge
        // (widescreen.c widescreen_hud_shift), in its own copy. nn::ui2d's panes: the layout camera's projection in
        // cb3[0..3], the pane's matrix in cb3[4..6] (its x at [4][3]); its text: one matrix, projection times the pane's,
        // whose origin (the text's start) shows where it is - its rows 0 plus a share of row 3 move it on the screen.
        extern float widescreen_hud_shift(float x);
        extern float widescreen_factor(void);
        extern bool widescreen_ui_centred(void);
        const float *f = (const float *)(uintptr_t)r.addr;
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        if (widescreen_ui_centred() && t0 && vk_texture_scale_group(t0) == RS_OUT && f[12] == 0 && f[13] == 0 && f[14] == -1.0f &&
            f[1] == 0 && f[2] == 0) {
            float g = widescreen_factor();
            extern bool framepace_in_menu(void);
            if (g_debug_frame && size >= 0x230 && f[15] == 0)
                LOG("[pane] fs %016llx x %.1f y %.1f m %.3f %.3f proj %.3f %.3f wide %d\n", (unsigned long long)g_cur_fs_hash, f[19],
                    f[23], f[16], f[21], f[0], f[5], g_wide_backdrop);
            if (size >= 0x230 && f[15] == 0 && f[0] > 0 && fabsf(f[5] / f[0] - 16.0f / 9.0f * g) < 0.01f && g_wide_backdrop &&
                fabsf(f[19]) < 1.0f && (g_cur_fs_hash == 0xd4fa32dabc35066cull || g_cur_fs_hash == 0xc94630386244616eull)) {
                // (the blurred world: nn::ui2d's textured pane shader, centred; and the pause menu's layer over it that
                // holds the 3D Link and the menu's darkening, c9463038 - left in the middle 16:9 part, it darkened only
                // that part: an edge each side, 2026-10-28)
                VkSpan c = vk_transient_copy(f, size, 256);  // (the pane's x row, about the layout's middle)
                for (int i = 16; i < 20; i++) ((float *)c.ptr)[i] *= g;
                return ubo_span(c, size);
            } else if (size >= 0x230 && f[15] == 0 && f[0] > 0 && fabsf(f[5] / f[0] - 16.0f / 9.0f * g) < 0.01f &&
                       full_width_pane(f)) {
                panefix_log("full-width", f);
                // A pane as wide as the layout, about its middle (the Game Over screen's black, a menu's dimming, a
                // loading screen's band) covers the widened screen, not only its middle 16:9 part (the world showed at
                // the sides, 2026-10-28), at any scale (the Game Over fade animates it)
                VkSpan c = vk_transient_copy(f, size, 256);
                for (int i = 16; i < 20; i++) ((float *)c.ptr)[i] *= g;
                return ubo_span(c, size);
            } else if (size >= 0x230 && f[15] == 0 && f[0] > 0 && g > 1.001f && fabsf(f[5] / f[0] - 16.0f / 9.0f) < 0.01f) {
                // A layout drawn through a camera of its own left at 16:9 - the inventory's cursor, the loading
                // screens - would stretch across the wide image (the cursor g times farther from the middle than the
                // rest): its projection's x row as the widened camera's, so it lands in the middle 16:9 part like the
                // others. A pane that is the whole layout (the loading screens' black) still covers the screen. The
                // loading screens were drawn stretched and squeezed back into 16:9 with bars by the present - the
                // image shrank to load and stretched back to the world (2026-10-28).
                panefix_log(full_width_pane(f) ? "16:9-cam full-width" : "16:9-cam", f);
                VkSpan c = vk_transient_copy(f, size, 256);
                float *m = (float *)c.ptr;
                for (int i = 0; i < 4; i++) m[i] /= g;
                if (full_width_pane(f))
                    for (int i = 16; i < 20; i++) m[i] *= g;
                return ubo_span(c, size);
            } else if (size >= 0x230 && f[15] == 0 && f[0] > 0 && fabsf(f[5] / f[0] - 16.0f / 9.0f * g) < 0.01f) {
                float d = widescreen_hud_shift(f[19]);
                if (d != 0) {
                    panefix_log("hud-shift", f);
                    VkSpan c = vk_transient_copy(f, size, 256);
                    ((float *)c.ptr)[19] += d;
                    return ubo_span(c, size);
                }
            } else if (f[15] > 1.0f && g > 1.001f && f[0] > 0 && fabsf(f[5] / f[0] - 16.0f / 9.0f) < 0.01f) {
                // Text through a layout camera left at 16:9 (a loading screen's tip): its x row narrowed like the
                // panes' projection above, or it sat apart from its box (2026-10-28)
                VkSpan c = vk_transient_copy(f, size, 256);
                for (int i = 0; i < 4; i++) ((float *)c.ptr)[i] /= g;
                return ubo_span(c, size);
            } else if (f[15] > 1.0f) {
                float d = widescreen_hud_shift(f[3] / f[15] * g * 640.0f);  // (layout x of the origin: 640 g units a side)
                if (d != 0) {
                    VkSpan c = vk_transient_copy(f, size, 256);
                    float *m = (float *)c.ptr, k = d / (640.0f * g);  // (that many NDC units)
                    for (int i = 0; i < 4; i++) m[i] += k * m[12 + i];
                    return ubo_span(c, size);
                }
            }
        }
    }
#ifdef __ANDROID__
    if (si->cbwin_mask >> k & 1u) {
        VkDescriptorBufferInfo d;
        if (cbwin_bind(stage, k, r.addr, size, &d)) return d;
    }
    static int log_cbwin = -1;
    if (log_cbwin < 0) log_cbwin = getenv("LARECOMP_LOG_CBWIN") != NULL;
    if (log_cbwin && (si->cbuf_dynamic >> k & 1u)) {  // (the dynamic ones left copied: why)
        static int n;
        if (n < 40 && !(renderer_present_count() % 97)) {
            n++;
            LOG("[cbwin] stage %d cb%d %llx+%llx mask %x placed %d row %llx\n", stage, k, (unsigned long long)r.addr,
                (unsigned long long)size, si->cbwin_mask, gmem_placed_(), (unsigned long long)cbwin_row_(r.addr, size));
        }
    }
#endif
    int dk = (si->cbuf_dynamic >> k) & 1;
    stat_cb_n[dk]++, stat_cb_bytes[dk] += size;
    if (dk && ct_on()) LOG("[ct] dyncbuf stage %d k %d addr %llx size %llx (bound %llx) fs %016llx\n", stage, k, (unsigned long long)r.addr,
                           (unsigned long long)size, (unsigned long long)r.size, (unsigned long long)g_cur_fs_hash);
    return ubo_span(guest_copy(r.addr, size), size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): the guest heap imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// LARECOMP_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;
#ifdef __ANDROID__
static bool gmem_placed;  // (gmem_buf is the guest heap itself, usable as a uniform buffer: vk_heap_place)

static bool ubo_direct(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_UBO_DIRECT") && atoi(getenv("LARECOMP_UBO_DIRECT"));
    extern int vk_ab(const char *what);
    if (!(on || AB("ubo") == 1) || !gmem_placed || addr < HEAP_BASE || addr + size > HEAP_BASE + gmem_size) return false;
    uint64_t off = addr - HEAP_BASE;
    if ((off & (g_vk.props.limits.minUniformBufferOffsetAlignment - 1)) || size > g_vk.props.limits.maxUniformBufferRange)
        return false;
    for (uint64_t blk = addr >> 8; blk <= (addr + size - 1) >> 8; blk++)
        if (ubo_inline_has(blk)) return false;
    *out = (VkDescriptorBufferInfo){gmem_buf, off, size};
    return true;
}

// Constant buffers patched per draw (shader.h maxwell_cb_window). BOTW binds a 64 KB constant buffer, always at the
// same address and read with register indices, and rewrites 16 bytes of its first 656 (UpdateUniformBuffer, one row
// or another of them) before nearly every draw. The updates keep it out of ubo_direct, and as each draw needs its own
// snapshot, every draw copied all 64 KB (~14 MB a frame). Instead it is bound in place in the placed heap, and the draw
// copies only the rows the updates write, as they are when it runs (the render thread wrote them through:
// CMD_UPDATE_UNIFORM_BUFFER), into its stage's patch buffer, which its shaders read those rows from. Only for a buffer
// whose updated rows fit in 64 in a row, one such buffer per stage; anything else is copied as before.
// LARECOMP_CB_WINDOW=1: on (off by default, below); LARECOMP_AB=<frames>:cbwin alternates it (the shaders keep the test).
// Off by default since 2026-10-27: in play (1.9.0) the water turned white and the terrain showed stretched "curtains",
// and objects far away grew spikes on a hit - the buffer read in place while the CPU already writes the next frame's
// rows there (the sync lag lets the CPU run a frame ahead). LARECOMP_CB_WINDOW=1: on (a test).
static bool cbwin_wanted(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_CB_WINDOW") && atoi(getenv("LARECOMP_CB_WINDOW"));
    return on && gmem_placed;
}
// (the update marks are guest addresses: a reboot's game puts other things there)
RT_ON_RESET(ubo_inline_reset) {
    memset((void *)ubo_inline, 0, sizeof ubo_inline);
    memset((void *)ubo_inline_span, 0, sizeof ubo_inline_span);
    atomic_fetch_add(&ubo_inline_gen, 1);
}

// The rows updates write inside [addr, addr + size): the first one's guest address | (the row count (1..64) - 1) << 48,
// 0 without any (or more than 64 rows apart)
static uint64_t cbwin_rows(uint64_t addr, uint64_t size) {
    static struct { uint64_t addr, size, rows; uint32_t gen; } memo[64];
    uint32_t gen = atomic_load_explicit(&ubo_inline_gen, memory_order_acquire);
    uint32_t h = (uint32_t)((addr ^ size * 31) * 0x9E3779B97F4A7C15ull >> 58);
    if (memo[h].addr == addr && memo[h].size == size && memo[h].gen == gen) return memo[h].rows;
    uint64_t lo = ~0ull, hi = 0;
    bool ok = true;
    for (uint64_t blk = addr >> 8; ok && blk <= (addr + size - 1) >> 8; blk++) {
        int i = ubo_inline_slot(blk);
        if (i == -1) continue;
        uint32_t s = i >= 0 ? atomic_load_explicit(&ubo_inline_span[i], memory_order_acquire) : 0;
        if (!s) ok = false;
        uint64_t l = (blk << 8) + (s & 0xFFFF), e = (blk << 8) + (s >> 16);
        if (l < lo) lo = l;
        if (e > hi) hi = e;
    }
    lo &= ~15ull, hi = (hi + 15) & ~15ull;
    uint64_t rows = ok && hi > lo && hi - lo <= 1024 && lo >= addr && hi <= addr + size ? lo | ((hi - lo) / 16 - 1) << 48 : 0;
    static int nlog;
    if (getenv("LARECOMP_LOG_CBWIN") && nlog < 30 && hi)
        nlog++, LOG("[cbwin] %llx+%llx: updated %llx..%llx%s\n", (unsigned long long)addr, (unsigned long long)size,
                    (unsigned long long)lo, (unsigned long long)hi, rows ? "" : " - copied");
    memo[h].addr = addr, memo[h].size = size, memo[h].rows = rows, memo[h].gen = gen;
    return rows;
}

static bool cbwin_bind(int stage, int k, uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    if ((stage != 0 && stage != 1) || !gmem_placed || cbw_patch[stage].m[0] || AB("cbwin") == 1) return false;
    if (addr < HEAP_BASE || addr + size > HEAP_BASE + gmem_size) return false;
    uint64_t off = addr - HEAP_BASE;
    if ((off & (g_vk.props.limits.minUniformBufferOffsetAlignment - 1)) || size > g_vk.props.limits.maxUniformBufferRange ||
        off > 0xFFFFFFFFull)
        return false;
    uint64_t rows = cbwin_rows(addr, size), first = rows & ((1ull << 48) - 1), n = (rows >> 48) + 1;
    if (!rows) return false;
    memcpy(cbw_patch[stage].rows, (const void *)(uintptr_t)first, n * 16);
    cbw_patch[stage].m[0] = (uint32_t)k + 1;
    cbw_patch[stage].m[1] = (uint32_t)((first - addr) >> 4);
    cbw_patch[stage].m[2] = (uint32_t)n;
    stat_cbwin++;
    *out = (VkDescriptorBufferInfo){gmem_buf, off, size};
    return true;
}
static bool gmem_placed_(void) { return gmem_placed; }
static uint64_t cbwin_row_(uint64_t addr, uint64_t size) { return cbwin_rows(addr, size); }
#endif

static bool gmem_import(uint64_t size) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)HEAP_BASE, &mp) != VK_SUCCESS ||
        !mp.memoryTypeBits)
        return false;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)HEAP_BASE};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, &mem) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (gmem_buf) {  // the heap grew: the old import may still be in use by submitted work
        vk_wait_seq(submit_seq);
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    LOG("[vk] guest heap imported for shader global memory: %llu MB\n", (unsigned long long)(size >> 20));
    return true;
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)

#ifdef __ANDROID__
// Turnip (VK_EXT_map_memory_placed): the guest heap *is* GPU memory. When the game sets its heap size (kernel.c
// SetHeapSize, before it writes there), host-visible device memory is mapped over the heap's range at HEAP_BASE itself
// (replacing the window's anonymous pages) and becomes the global memory buffer: shaders read and write the guest's
// own bytes, as with the desktop's import - no CPU copies (gcopy_prepare), and what a compute pass writes the next
// draw reads (the game's lighting: lights listed by compute, read by the fragment shaders). Freed on reset (vkFreeMemory
// unmaps it; runtime/main.c then remaps the whole window). LARECOMP_VK_NO_PLACED=1: off (CPU copies).
bool vk_heap_place(uint64_t size) {
#ifdef __ANDROID__
    extern void vk_async_drain(void);
    vk_async_drain();  // (the render thread idle: vulkan/async.c)
#endif
    if (!size) return false;
    vk_wait_ready();
    if (gmem_buf) return true;
    // Not placed: the heap stays CPU memory copied for the GPU, and the vertex cache and the texture checks need the
    // OS's write tracking - vk_exec_init turns it on only without placed memory at all, so a Turnip that has it but
    // isn't an Adreno 8xx (Automatic) had neither: every vertex and index buffer copied each frame, ~150 MB, 7-9 fps
    // on an Adreno 730 (issue #31). A page not tracked yet counts as written, so turning it on late is safe.
#define NOT_PLACED() (gcopy_mode ? plat_write_tracking_enable(HEAP_BASE, HEAP_BASE + HEAP_MAX) : (void)0, false)
    if (!g_vk.map_placed || getenv("LARECOMP_VK_NO_PLACED")) return NOT_PLACED();
    extern bool vk_turnip_heap_on(int setting);
    if (!vk_turnip_heap_on(g_settings.gpu_heap)) {
        // (the start screen's Driver compatibility: Automatic is on wherever Turnip places it - vulkan/device.c)
        LOG("[vk] guest heap as GPU memory: off (%s) - shader global memory through CPU copies\n",
            g_settings.gpu_heap < 0 ? "automatic: not Turnip" : "settings");
        return NOT_PLACED();
    }
    {   // The placed heap is committed at once (~3.1 GB for BOTW), where CPU copies commit only the pages the game
        // touches: on a 6 GB phone with it turned on by hand, Android killed the app as the world loaded (issue #83,
        // Adreno 640, 3.6 GB in use). Below ~7 GB of RAM (6 GB phones report ~5.5, 8 GB ones ~7.4): CPU copies whatever
        // the setting says. LARECOMP_HEAP_ANY_RAM=1: placed anyway.
        long total_kb = 0;
        FILE *f = fopen("/proc/meminfo", "r");
        if (f) {
            if (fscanf(f, "MemTotal: %ld kB", &total_kb) != 1) total_kb = 0;
            fclose(f);
        }
        if (total_kb && total_kb < 7L * 1024 * 1024 && !getenv("LARECOMP_HEAP_ANY_RAM")) {
            LOG("[vk] guest heap as GPU memory: off (%ld MB of RAM: it would take %llu MB at once) - shader global memory "
                "through CPU copies\n", total_kb >> 10, (unsigned long long)(size >> 20));
            return NOT_PLACED();
        }
    }
    uint64_t align = g_vk.placed_align > 65536 ? g_vk.placed_align : 65536;
    size = (size + align - 1) & ~(align - 1);
    if ((HEAP_BASE & (align - 1)) || size > HEAP_MAX) {
        LOG("[vk] heap placement: %llx / %llu MB doesn't fit (alignment %llu)\n", (unsigned long long)HEAP_BASE,
            (unsigned long long)(size >> 20), (unsigned long long)align);
        return NOT_PLACED();
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};  // (ubo_direct)
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) return NOT_PLACED();
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    int type = -1;  // host-visible and coherent (the CPU writes, the GPU reads and writes); cached when there is one
    const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if (!(mr.memoryTypeBits & (1u << i)) || (f & need) != need) continue;
        if (type < 0 || (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) type = (int)i;
        if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) break;
    }
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = (uint32_t)type};
    VkResult r = type < 0 ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(g_vk.dev, &ai, NULL, &mem);
    if (r == VK_SUCCESS) r = vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    void *p = NULL;
    if (r == VK_SUCCESS) {
        VkMemoryMapPlacedInfoEXT pl = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = (void *)(uintptr_t)HEAP_BASE};
        VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pl, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT,
                                 .memory = mem, .offset = 0, .size = VK_WHOLE_SIZE};
        r = vkMapMemory2KHR(g_vk.dev, &mi, &p);
    }
    if (r != VK_SUCCESS || p != (void *)(uintptr_t)HEAP_BASE) {
        LOG("[vk] heap placement failed (%d, mapped at %p, type %d): shader global memory through CPU copies\n", r, p, type);
        if (mem) vkFreeMemory(g_vk.dev, mem, NULL);  // (freeing a placed map unmaps it: the window gets its pages back)
        if (p == (void *)(uintptr_t)HEAP_BASE) plat_reset_guest(HEAP_BASE, HEAP_BASE + size);
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        return NOT_PLACED();
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    gmem_placed = true;
    gcopy_mode = false;
    gwin_mode = size > g_vk.props.limits.maxStorageBufferRange && !getenv("LARECOMP_VK_NO_GWIN");
    LOG("[vk] guest heap %llu MB is GPU memory (placed at %llx, memory type %d, flags 0x%x, storage range %llu MB)%s\n",
        (unsigned long long)(size >> 20), (unsigned long long)HEAP_BASE, type, (unsigned)mp.memoryTypes[type].propertyFlags,
        (unsigned long long)(g_vk.props.limits.maxStorageBufferRange >> 20), gwin_mode ? "; draws reach global memory through windows" : "");
    return true;
}
#undef NOT_PLACED
#endif

static VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VK_NO_GMEM") != NULL || !g_vk.host_import;
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    if (!off && want > gmem_size && !gmem_import(want)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
#ifdef __ANDROID__
    if (gmem_buf) {  // (a storage buffer's range is limited: 2 GB on Adreno; the heap may be larger)
        uint64_t range = gmem_size < g_vk.props.limits.maxStorageBufferRange ? gmem_size : g_vk.props.limits.maxStorageBufferRange & ~3ull;
        return (VkDescriptorBufferInfo){gmem_buf, 0, range};
    }
#endif
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// Compute dispatches that use global memory: the imported heap is host memory across PCIe on a discrete GPU, and a
// shader storing to it in a loop crawls (the little chain chomps in Madam MeowMeow's house: ~8 ms a frame on a GTX 1660
// Super and on an RTX 3080 Ti alike, which the game then waits for on its fence - 49-53 fps there on the 1660). Before
// such a dispatch its bound storage buffers (sorted, merged) are copied into a device-local buffer, the address table
// (binding GMAP_BUFFER_INDEX) tells the shader (translate.c, GLSL compute) where each range sits, loads and stores
// inside them use that copy, and a shader that stores gets the ranges copied back to the heap right after it (the
// game alternates its buffer sets per frame: the CPU isn't writing them meanwhile). Accesses outside the table use
// the heap as before. LARECOMP_VK_GMEM_FAST=0: no copy (empty table: everything goes to the heap, the old behaviour).
#define GFAST_SIZE (32ull << 20)
static VkBuffer gfast_buf;
static VmaAllocation gfast_mem;
static uint32_t gmap_tab[(GMAP_MAX + 1) * 4];  // this dispatch's table (stage_set copies it into binding 20)
static VkBufferCopy gfast_cp[GMAP_MAX];       // its copies heap -> gfast_buf (reversed after a storing dispatch)
static int gfast_n;

#ifdef __ANDROID__
static VkDescriptorBufferInfo gfast_win;  // (gfast_prepare, heap placed: a window of the heap instead of a copy)
#endif

static VkDescriptorBufferInfo gfast_info(void) {
#ifdef __ANDROID__
    if ((gcopy_mode || gwin_mode) && gc_size) return (VkDescriptorBufferInfo){gc_span.buf, gc_span.off, gc_size};
    if (gfast_win.buffer) return gfast_win;
#endif
    if (!gfast_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GFAST_SIZE,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &gfast_buf, &gfast_mem, NULL));
    }
    return (VkDescriptorBufferInfo){gfast_buf, 0, GFAST_SIZE};
}

static void gfast_prepare(VkCommandBuffer cb) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_GMEM_FAST"); on = !e || atoi(e) != 0; }
    memset(gmap_tab, 0, sizeof gmap_tab);
    gfast_n = 0;
#ifdef __ANDROID__
    gfast_win = (VkDescriptorBufferInfo){0};
    if (!gcopy_mode) gc_size = 0;  // (a draw's gsnap_prepare copy: not this dispatch's)
#endif
    gmem_info();  // (the heap import, made on first use)
    gfast_info();
    if (!on || !gmem_buf) return;
    struct { uint64_t lo, hi; } r[16];
    int n = 0;
    for (int i = 0; i < 16; i++) {  // heap offsets of the bound storage buffers, 256-aligned
        uint64_t a = st.sbuf[5][i].addr, sz = st.sbuf[5][i].size;
        if (!a || !sz || a < HEAP_BASE || a - HEAP_BASE + sz > gmem_size) continue;
        uint64_t lo = (a - HEAP_BASE) & ~255ull, hi = (a - HEAP_BASE + sz + 255) & ~255ull;
        if (hi > gmem_size) hi = gmem_size;
        int k = n++;
        while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }  // (insertion sort by start)
        r[k].lo = lo; r[k].hi = hi;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {  // merge overlapping / nearby ranges (a small gap costs less than another table entry)
        if (m && r[i].lo <= r[m - 1].hi + 4096) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
#ifdef __ANDROID__
    // The heap placed in GPU memory (vk_heap_place) is already where the GPU works best (one memory, no bus): a
    // copy of the ranges and back, with full barriers around each dispatch, only cost time (~3 ms a frame of the
    // overworld's light lists). Only its descriptor's reach is limited (maxStorageBufferRange, 2 GB on Adreno, the
    // heap is larger): the table points into a window of the heap that starts at the first range instead.
    if (gmem_buf && !g_vk.host_import && m && m <= GMAP_MAX) {
        uint64_t al = g_vk.props.limits.minStorageBufferOffsetAlignment ? g_vk.props.limits.minStorageBufferOffsetAlignment : 256;
        uint64_t base = r[0].lo / al * al;
        uint64_t reach = g_vk.props.limits.maxStorageBufferRange & ~3ull;
        if (reach > gmem_size - base) reach = gmem_size - base;
        if (r[m - 1].hi - base <= reach) {
            for (int i = 0; i < m; i++) {
                gmap_tab[i * 4 + 0] = (uint32_t)r[i].lo;
                gmap_tab[i * 4 + 1] = (uint32_t)(r[i].hi - r[i].lo);
                gmap_tab[i * 4 + 2] = (uint32_t)((r[i].lo - base) >> 2);
            }
            gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
            gfast_win = (VkDescriptorBufferInfo){gmem_buf, base, reach};
            return;  // (gfast_n 0: nothing to copy back)
        }
    }
#endif
    VkBufferCopy cp[16];
    uint64_t off = 0;
    for (int i = 0; i < m; i++) {
        uint64_t len = r[i].hi - r[i].lo;
        if (m > GMAP_MAX || off + len > GFAST_SIZE) return;  // (too much: the heap as before)
        cp[i] = (VkBufferCopy){r[i].lo, off, len};
        gmap_tab[i * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[i * 4 + 1] = (uint32_t)len;
        gmap_tab[i * 4 + 2] = (uint32_t)(off >> 2);
        off += (len + 255) & ~255ull;
    }
    if (!m) return;
    if (getenv("LARECOMP_LOG_GFAST")) {  // (debug) =<shader code address hex>: that shader's first tables
        static int nlog;
        if (st.program->stages[5].code_addr == strtoull(getenv("LARECOMP_LOG_GFAST"), NULL, 16) && nlog++ < 40) {
            LOG("[gfast] cs %llx: %d ranges, %llu KB:", (unsigned long long)st.program->stages[5].code_addr, m, (unsigned long long)(off >> 10));
            for (int i = 0; i < m; i++) LOG(" %llx+%llx", (unsigned long long)(r[i].lo + HEAP_BASE), (unsigned long long)(r[i].hi - r[i].lo));
            LOG("\n");
        }
    }
    vkCmdCopyBuffer(cb, gmem_buf, gfast_buf, (uint32_t)m, cp);
    vk_full_barrier(cb);
    gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
    memcpy(gfast_cp, cp, (size_t)m * sizeof cp[0]);
    gfast_n = m;
}

// After a storing dispatch: its ranges back to the heap (before anything else reads them: draws, the CPU after the fence).
static void gfast_finish(VkCommandBuffer cb) {
#ifdef __ANDROID__
    if (gfast_win.buffer) { vk_full_barrier(cb); return; }  // (stored into the heap itself: visible to what follows)
#endif
    if (!gfast_n) return;
    VkBufferCopy back[GMAP_MAX];
    for (int i = 0; i < gfast_n; i++) back[i] = (VkBufferCopy){gfast_cp[i].dstOffset, gfast_cp[i].srcOffset, gfast_cp[i].size};
    vk_full_barrier(cb);
    vkCmdCopyBuffer(cb, gfast_buf, gmem_buf, (uint32_t)gfast_n, back);
    vk_full_barrier(cb);
    gfast_n = 0;
}

#ifdef __ANDROID__
// ---- global memory without the heap import (Android: neither the Qualcomm driver nor Turnip has
// VK_EXT_external_memory_host). Every stage then reaches global memory through the GMap table (translate.c,
// maxwell_gmem_map_all): before a draw or dispatch whose shaders access it, the storage buffers bound to their stages
// are copied by the CPU into the transient ring (on Adreno the same memory as the CPU's) and the table points there.
// What a storing shader wrote goes back to guest memory once its submission is done (gcopy_retire: from vk_transient
// before the ring reuses a chunk, and after every wait). Without this, global loads read zero: the game's text is
// drawn by a vertex shader that reads its vertices that way (nothing of it showed).
typedef struct { const uint8_t *src; uint64_t addr, len, seq; uint8_t *shadow; const uint64_t *bseq; } GcBack;  // (arena)
static GcBack gc_back[256];
static int gc_nback;
static pthread_mutex_t gc_lock = PTHREAD_MUTEX_INITIALIZER;

static void gcopy_retire(uint64_t done) {
    pthread_mutex_lock(&gc_lock);
    int k = 0;
    for (int i = 0; i < gc_nback; i++) {
        if (gc_back[i].seq <= done) {
            if (!gc_back[i].shadow) {
                memcpy((void *)(uintptr_t)gc_back[i].addr, gc_back[i].src, gc_back[i].len);
                continue;
            }
            // (the arena: 256 bytes at a time, what the GPU changed since the last upload - the shadow - and the CPU
            // didn't: a block the CPU wrote meanwhile stays the CPU's (gres_sync uploads it); copying whole ranges
            // back undid the CPU's writes made since, for good - wrong shadows while moving)
            uint8_t *g = (uint8_t *)(uintptr_t)gc_back[i].addr, *sh = gc_back[i].shadow;
            const uint8_t *src = gc_back[i].src;
            ga_invalidate(src, gc_back[i].len);
            for (uint64_t b = 0; b < gc_back[i].len; b += 256) {
                uint64_t n = gc_back[i].len - b < 256 ? gc_back[i].len - b : 256;
                // (a block the CPU's newer data was uploaded into after this submission: the shadow is that data
                // already while the slot may not have it yet - comparing them took the slot's older bytes for the
                // GPU's and wrote them over the CPU's: the game's allocator lists looped, the game hung)
                if (gc_back[i].bseq[b / 256] > gc_back[i].seq) continue;
                if (!memcmp(src + b, sh + b, n) || memcmp(g + b, sh + b, n)) continue;  // (GPU unchanged / CPU wrote)
                memcpy(g + b, src + b, n);
                memcpy(sh + b, src + b, n);
            }
        } else gc_back[k++] = gc_back[i];
    }
    gc_nback = k;
    pthread_mutex_unlock(&gc_lock);
}


// ---- the arena: shader global memory that stays on the GPU (LARECOMP_VK_NO_GRES=1: off)
// The copies above are taken when a draw is translated, but a compute shader's output only reaches guest memory once
// its submission is done. The game's lighting is such a case: each frame a compute dispatch writes the light list
// (~34 KB) and then over a hundred draws' fragment shaders read it - from copies made before the dispatch had run:
// no lighting on Qualcomm's driver (Turnip reads the heap itself). So every draw and dispatch reaches global memory
// through one buffer, the arena: a range a shader stores to gets a slot there for good (resident), and everything
// after it that touches that range - the dispatch itself next frame, the draws - uses the slot, in GPU order; the
// other ranges are copied per draw into the arena's ring, as before. The CPU's writes to a resident range (the game
// clearing a counter each frame) are found by comparing the range with what was last uploaded, 256 bytes at a time,
// and copied into the slot by the GPU at the start of the submission. What a shader stored also goes back to guest
// memory once its submission is done (the game reads some of it as uniform buffers, vertices, textures), and to the
// range's last-uploaded copy, so that isn't taken for the CPU's write.
typedef struct { uint64_t lo, hi; bool store; } GcRange;  // heap offsets, 256-aligned
#define GA_SIZE (128ull << 20)  // (the minimum maxStorageBufferRange)
#define GA_RES (48ull << 20)    // [0, GA_RES): resident slots; the rest: the ring
#define GA_SEG (8ull << 20)
#define GA_NRES 512
typedef struct { uint64_t lo, hi, off, checked; uint8_t *shadow; bool live; uint64_t *bseq; } GaRes;  // (bseq: per 256 B)
static GaRes ga_res[GA_NRES];
static int ga_nres;
static VkBuffer ga_buf;
static VmaAllocation ga_mem;
static uint8_t *ga_ptr;
static uint64_t ga_res_top, ga_head = GA_RES, ga_seg_seq[GA_SIZE / GA_SEG];
static int gres_state = -1;
static uint64_t stat_gres_res, stat_gres_sync;  // (resident ranges used, CPU-changed bytes uploaded)

// The arena's memory not coherent (Mali: its cached type 0xb; no type is both cached and coherent): what the CPU
// writes there is flushed before the next submission (ga_wrote, ga_flush_dirty), what the GPU stored is invalidated
// before the CPU reads it (ga_invalidate). Without them the GPU read copies still in the CPU's cache and the CPU took
// stale lines for the GPU's results - null pointers, doubled text (#71); 0.2.3 used the coherent type instead, which
// on Mali is uncached: the CPU's reads and writes there crawled, 2-9 fps in the world (#88, #89, #93).
#define GA_NDIRTY 64
static bool ga_noncoherent;
static struct { uint64_t lo, hi; } ga_dirty[GA_NDIRTY];
static int ga_ndirty;

static void ga_flush_dirty(void) {
    if (!ga_ndirty) return;
    VmaAllocation a[GA_NDIRTY];
    VkDeviceSize o[GA_NDIRTY], sz[GA_NDIRTY];
    for (int i = 0; i < ga_ndirty; i++) a[i] = ga_mem, o[i] = ga_dirty[i].lo, sz[i] = ga_dirty[i].hi - ga_dirty[i].lo;
    vmaFlushAllocations(g_vk.vma, (uint32_t)ga_ndirty, a, o, sz);
    ga_ndirty = 0;
}

// The CPU wrote [off, off + len) of the arena (render thread): flushed with the next submission. The ring's copies
// follow each other, so they mostly extend the last range.
static void ga_wrote(uint64_t off, uint64_t len) {
    if (!ga_noncoherent || !len) return;
    if (ga_ndirty && ga_dirty[ga_ndirty - 1].hi == off) { ga_dirty[ga_ndirty - 1].hi = off + len; return; }
    if (ga_ndirty == GA_NDIRTY) ga_flush_dirty();
    ga_dirty[ga_ndirty].lo = off, ga_dirty[ga_ndirty].hi = off + len;
    ga_ndirty++;
}

// The GPU's stores in the arena at p, once their submission is done, before the CPU reads them
static void ga_invalidate(const void *p, uint64_t len) {
    if (ga_noncoherent) vmaInvalidateAllocation(g_vk.vma, ga_mem, (VkDeviceSize)((const uint8_t *)p - ga_ptr), len);
}
static uint64_t stat_gres_fail[4];  // (fell back to the plain copies: 0 no entry/space, 1 table full, 2 ring, 3 too big)

static bool gres_init(void) {
    if (gres_state >= 0) return gres_state;
    gres_state = 0;
    if (getenv("LARECOMP_VK_NO_GRES")) return false;
    if (g_vk.props.limits.maxStorageBufferRange < GA_SIZE) return false;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GA_SIZE,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    // (cached: the CPU reads the GPU's stores back. Adreno's Turnip has a cached coherent type; Mali's cached one
    // isn't coherent - flushed and invalidated by hand, ga_noncoherent)
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .flags = (getenv("LARECOMP_GA_UNCACHED") ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                                                           : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT) |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};  // (LARECOMP_GA_UNCACHED=1, a test)
    if (getenv("LARECOMP_GA_NONCOHERENT")) {  // (a test: Mali's case on an Adreno - a cached type that isn't coherent)
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                ai.memoryTypeBits |= 1u << i;
    }
    VmaAllocationInfo inf;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &ga_buf, &ga_mem, &inf) != VK_SUCCESS || !inf.pMappedData) {
        LOG("[vk] global memory arena: not available\n");
        return false;
    }
    ga_ptr = inf.pMappedData;
    gres_state = 1;
    VkMemoryPropertyFlags mf;
    vmaGetMemoryTypeProperties(g_vk.vma, inf.memoryType, &mf);
    ga_noncoherent = !(mf & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    LOG("[vk] global memory arena: %llu MB (what shaders store stays on the GPU), memory type %u (flags 0x%x%s)\n",
        (unsigned long long)(GA_SIZE >> 20), inf.memoryType, (unsigned)mf, ga_noncoherent ? ": flushed and invalidated" : "");
    return true;
}

// A copy's place in the arena's ring (~0: none - too big, or the ring went round within one submission)
static uint64_t ga_ring(uint64_t size) {
    size = (size + 255) & ~255ull;
    if (size > GA_SEG) return ~0ull;
    if (ga_head / GA_SEG != (ga_head + size - 1) / GA_SEG || ga_head + size > GA_SIZE - 4096) {  // (the last 4 KB: GMISS)
        ga_head = (ga_head / GA_SEG + 1) * GA_SEG;
        if (ga_head >= GA_SIZE) ga_head = GA_RES;
        uint64_t q = ga_seg_seq[ga_head / GA_SEG];
        if (q >= vk_pending_seq()) return ~0ull;
        if (q) vk_wait_seq(q);  // (the segment's last readers done)
    }
    uint64_t off = ga_head;
    ga_head += size;
    ga_seg_seq[off / GA_SEG] = vk_pending_seq();
    return off;
}

// The CPU's writes to a resident range since its last upload, into its slot (by the GPU, first thing in the
// submission: once per frame - its first submission using the range - before any of its uses; once per submission
// compared the learned ranges, hundreds of KB, ~6 times a frame with the chunked submissions: memcmp was ~8% of
// the render thread)
static void gres_sync(GaRes *e) {
    uint64_t pend = vk_pending_seq(), frame = renderer_present_count() + 1;
    if (e->checked == frame) return;
    e->checked = frame;
    const uint8_t *g = (const uint8_t *)(uintptr_t)(HEAP_BASE + e->lo);
    uint64_t len = e->hi - e->lo;
    VkCommandBuffer barrier_cb = VK_NULL_HANDLE;
    for (uint64_t b = 0; b < len;) {
        if (!memcmp(g + b, e->shadow + b, 256)) { b += 256; continue; }
        uint64_t s0 = b;
        while (b < len && memcmp(g + b, e->shadow + b, 256)) b += 256;
        uint64_t n = b - s0;
        VkSpan sp = vk_transient(n, 256);
        memcpy(sp.ptr, g + s0, n);
        memcpy(e->shadow + s0, g + s0, n);
        for (uint64_t k = s0 / 256; k < (s0 + n + 255) / 256; k++) e->bseq[k] = pend;
        VkCommandBuffer up = vk_upload_cmd();
        if (up != barrier_cb) { vk_full_barrier(up); barrier_cb = up; }  // once per upload command buffer
        VkBufferCopy cp = {sp.off, e->off + s0, n};
        vkCmdCopyBuffer(up, sp.buf, ga_buf, 1, &cp);
        stat_gres_sync += n;
    }
}

// The live resident entry covering heap [lo, hi); create: else a new one for it (and any it overlaps)
static GaRes *gres_get(uint64_t lo, uint64_t hi, bool create) {
    for (int i = 0; i < ga_nres; i++)
        if (ga_res[i].live && ga_res[i].lo <= lo && hi <= ga_res[i].hi) return &ga_res[i];
    if (!create) return NULL;
    uint64_t ulo = lo, uhi = hi;
    int over[GA_NRES], no = 0;
    for (bool grew = true; grew;) {  // (the union of the range and the entries it overlaps)
        grew = false;
        no = 0;
        for (int i = 0; i < ga_nres; i++) {
            GaRes *e = &ga_res[i];
            if (!e->live || e->hi <= ulo || e->lo >= uhi) continue;
            over[no++] = i;
            if (e->lo < ulo) { ulo = e->lo; grew = true; }
            if (e->hi > uhi) { uhi = e->hi; grew = true; }
        }
    }
    if (no && in_pass) vk_end_pass();  // (merging copies slot to slot: outside any render pass)
    uint64_t len = uhi - ulo;
    int slot = -1;
    for (int i = 0; i < ga_nres; i++) if (!ga_res[i].live && !ga_res[i].shadow) { slot = i; break; }
    if (slot < 0 && ga_nres < GA_NRES) slot = ga_nres++;
    if (slot < 0 || ga_res_top + len > GA_RES) return NULL;
    GaRes *n = &ga_res[slot];
    uint8_t *shadow = malloc(len);
    uint64_t *bseq = calloc((len + 255) / 256, sizeof *bseq);
    if (!shadow || !bseq) { free(shadow); free(bseq); return NULL; }
    const uint8_t *g = (const uint8_t *)(uintptr_t)(HEAP_BASE + ulo);
    *n = (GaRes){ulo, uhi, ga_res_top, renderer_present_count() + 1, shadow, true, bseq};  // (synced: this frame)
    ga_res_top += (len + 255) & ~255ull;
    memcpy(shadow, g, len);
    memcpy(ga_ptr + n->off, g, len);  // (a new slot: nothing on the GPU uses it yet)
    ga_wrote(n->off, len);
    if (no) {  // the overlapped slots may hold the GPU's results: into the new one, in GPU order
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        for (int k = 0; k < no; k++) {
            GaRes *e = &ga_res[over[k]];
            gres_sync(e);
            VkBufferCopy cp = {e->off, n->off + (e->lo - ulo), e->hi - e->lo};
            vkCmdCopyBuffer(cb, ga_buf, ga_buf, 1, &cp);
            e->live = false;  // (its slot isn't reused: the arena's resident part only grows until a reset)
        }
        vk_full_barrier(cb);
    }
    return n;
}

// This draw's / dispatch's ranges through the arena: false - not possible here, the plain copies instead
// Learned ranges: shaders also reach global memory through pointers, outside the storage buffers bound to them -
// the lighting's draws read the light count, next to a light list a compute dispatch writes, without having the list
// bound (~950k reads a frame, all zero: a dark scene). The shaders record such misses (gmiss, at the arena's end); a
// miss inside a resident range makes that range part of every table from then on, pointing at its slot.
#define GL_MAX 16
static GcRange gl_learned[GL_MAX];
static int gl_n;

static void gres_learn(uint32_t off) {
    for (int i = 0; i < gl_n; i++) if (gl_learned[i].lo <= off && off < gl_learned[i].hi) return;
    GaRes *in = NULL;
    for (int i = 0; i < ga_nres && !in; i++)
        if (ga_res[i].live && ga_res[i].lo <= off && off < ga_res[i].hi) in = &ga_res[i];
    if (!in) {  // (data the CPU writes, reached by pointer - the lights' parameters, objects' data: resident too, a
                // window around it between the ranges already resident - 64 KB before, 256 KB after: the objects'
                // data grows upward as the game allocates (walking around: a miss, and wrong shadows, for every
                // 16 KB until learned); gres_sync keeps it current)
        uint64_t base = off & ~0xFFFFull;
        uint64_t lo = base > 0x10000 ? base - 0x10000 : 0, hi = base + 0x40000;
        if (hi > HEAP_MAX) hi = HEAP_MAX;
        for (int i = 0; i < ga_nres; i++) {
            GaRes *e = &ga_res[i];
            if (!e->live) continue;
            if (e->hi <= off && e->hi > lo) lo = e->hi;
            if (e->lo > off && e->lo < hi) hi = e->lo;
        }
        if (hi <= lo || !(in = gres_get(lo, hi, true))) return;
    }
    if (gl_n == GL_MAX) memmove(gl_learned, gl_learned + 1, sizeof gl_learned[0] * --gl_n);
    gl_learned[gl_n++] = (GcRange){in->lo, in->hi};
    LOG("[vk] global memory: shaders reach heap %x outside their tables - %llx+%llx goes in every table\n", off,
        (unsigned long long)in->lo, (unsigned long long)(in->hi - in->lo));
}

// Load-only arena copies are immutable until their submission retires. Compare against that exact copy;
// no assumption that guest memory remains unchanged between draws.
static uint64_t ga_load_copy(uint64_t at, uint64_t size) {
    static struct { uint64_t at, size, off, seq, gen; bool valid; } cache[64];
    static int reuse = -1;
    if (reuse < 0) { const char *e = getenv("LARECOMP_GCOPY_REUSE"); reuse = !e || atoi(e) != 0; }
    uint64_t seq = vk_pending_seq();
    unsigned slot = (unsigned)((at >> 8) ^ (size >> 8)) & 63;
    const uint8_t *src = (const uint8_t *)(uintptr_t)(HEAP_BASE + at);
    if (reuse && cache[slot].valid && cache[slot].seq == seq && cache[slot].gen == gcopy_gen &&
        cache[slot].at == at && cache[slot].size == size && !memcmp(ga_ptr + cache[slot].off, src, size))
        return cache[slot].off;
    uint64_t off = ga_ring(size);
    if (off == ~0ull) return off;
    memcpy(ga_ptr + off, src, size);
    ga_wrote(off, size);
    cache[slot].at = at; cache[slot].size = size; cache[slot].off = off;
    cache[slot].seq = seq; cache[slot].gen = gcopy_gen; cache[slot].valid = true;
    return off;
}

static bool gres_prepare(const GcRange *r, int m, bool store) {
    if (!gres_init()) return false;
    {   // LARECOMP_GMISS: the arena's state every ~5 s
        static uint64_t last;
        if (getenv("LARECOMP_GMISS") && renderer_present_count() - last >= 300) {
            last = renderer_present_count();
            int live = 0;
            for (int i = 0; i < ga_nres; i++) live += ga_res[i].live;
            LOG("[gres] frame %llu: %d resident ranges (%d entries), %.1f of %llu MB, %d learned, %.1f KB synced; plain copies "
                "instead: %llu no entry/space, %llu table full, %llu ring, %llu too big\n", (unsigned long long)last, live, ga_nres,
                ga_res_top / 1048576.0, (unsigned long long)(GA_RES >> 20), gl_n, stat_gres_sync / 1024.0,
                (unsigned long long)stat_gres_fail[0], (unsigned long long)stat_gres_fail[1], (unsigned long long)stat_gres_fail[2],
                (unsigned long long)stat_gres_fail[3]);
            stat_gres_sync = 0;
            memset(stat_gres_fail, 0, sizeof stat_gres_fail);
        }
    }
    if (maxwell_gmem_miss_log) {  // the accesses outside the tables lately: learned (and logged with LARECOMP_GMISS)
        static uint64_t calls, last_frame;
        if (++calls % 256 == 0 && renderer_present_count() - last_frame >= 30) {
            last_frame = renderer_present_count();
            static int logit = -1;
            if (logit < 0) logit = getenv("LARECOMP_GMISS") != NULL;
            uint32_t *w = (uint32_t *)(ga_ptr + (uint64_t)GMISS_WORD * 4);
            ga_invalidate(w, 4096);
            uint32_t cnt = __atomic_load_n(&w[0], __ATOMIC_RELAXED);
            if (cnt) {
                char b[1024];
                int o = 0, ns = 0;
                uint32_t seen[64];
                for (uint32_t i = 0; i < cnt && i < 64; i++) {
                    uint32_t a = w[1 + i] & ~255u;
                    bool dup = false;
                    for (int k = 0; k < ns; k++) dup |= seen[k] == a;
                    if (dup) continue;
                    seen[ns++] = a;
                    if (o < 980) o += snprintf(b + o, sizeof b - o, " %x", w[1 + i]);
                    gres_learn(w[1 + i]);
                }
                if (logit) LOG("[gmiss] frame %llu: %u accesses outside the tables, at:%s\n", (unsigned long long)last_frame, cnt, b);
                w[0] = 0;
                ga_wrote((uint64_t)GMISS_WORD * 4, 4);
            }
        }
    }
    uint32_t tab[(GMAP_MAX + 1) * 4];
    memset(tab, 0, sizeof tab);
    int n = 0;
#define GRES_ADD(at, len, off)                                                                                        \
    do {                                                                                                               \
        if (n == GMAP_MAX) { stat_gres_fail[1]++; return false; }                                                      \
        tab[n * 4 + 0] = (uint32_t)(at);                                                                               \
        tab[n * 4 + 1] = (uint32_t)(len);                                                                              \
        tab[n * 4 + 2] = (uint32_t)((off) >> 2);                                                                       \
        n++;                                                                                                           \
    } while (0)
    for (int i = 0; i < m; i++) {
        uint64_t lo = r[i].lo, hi = r[i].hi;
        if (r[i].store) {  // a storing shader's bound ranges: resident from now on
            GaRes *e = gres_get(lo, hi, true);
            if (!e) { stat_gres_fail[0]++; return false; }
            gres_sync(e);
            GRES_ADD(lo, hi - lo, e->off + (lo - e->lo));
            stat_gres_res++;
            // and back into guest memory once done, as on Turnip (where the GPU writes the heap itself): the game
            // also reads what its compute shaders wrote as uniform buffers, vertices and textures, which come from
            // guest memory (gcopy_retire, at the game's waits)
            pthread_mutex_lock(&gc_lock);
            if (gc_nback < 256)
                gc_back[gc_nback++] = (GcBack){ga_ptr + e->off + (lo - e->lo), HEAP_BASE + lo, hi - lo, vk_pending_seq(),
                                               e->shadow + (lo - e->lo), e->bseq + (lo - e->lo) / 256};
            pthread_mutex_unlock(&gc_lock);
            continue;
        }
        for (uint64_t at = lo; at < hi;) {  // loads: resident parts from their slots, the rest copied
            GaRes *in = NULL;
            uint64_t next = hi;
            for (int k = 0; k < ga_nres; k++) {
                GaRes *e = &ga_res[k];
                if (!e->live) continue;
                if (e->lo <= at && at < e->hi) { in = e; break; }
                if (e->lo > at && e->lo < next) next = e->lo;
            }
            if (in) {
                uint64_t end = hi < in->hi ? hi : in->hi;
                if (end < hi && n == GMAP_MAX - 1) {  // (one table slot left: copy the whole tail rather than overflow)
                    uint64_t off = ga_load_copy(at, hi - at);
                    if (off == ~0ull) { stat_gres_fail[hi - at > GA_SEG ? 3 : 2]++; return false; }
                    GRES_ADD(at, hi - at, off);
                    break;
                }
                gres_sync(in);
                GRES_ADD(at, end - at, in->off + (at - in->lo));
                stat_gres_res++;
                at = end;
                continue;
            }
            if (next < hi && n == GMAP_MAX - 1) next = hi;
            uint64_t off = ga_load_copy(at, next - at);
            if (off == ~0ull) { stat_gres_fail[next - at > GA_SEG ? 3 : 2]++; return false; }
            GRES_ADD(at, next - at, off);
            at = next;
        }
    }
#undef GRES_ADD
    tab[GMAP_MAX * 4] = (uint32_t)n;
    memcpy(gmap_tab, tab, sizeof tab);
    gc_span = (VkSpan){.buf = ga_buf, .off = 0, .ptr = ga_ptr};
    gc_size = GA_SIZE;
    return true;
}

// The shaders' miss log (binding GMISS_BUFFER_INDEX): the arena's last 4 KB, else (no arena) a scratch buffer
static VkDescriptorBufferInfo gmiss_info(void) {
    if (gres_init()) return (VkDescriptorBufferInfo){ga_buf, (uint64_t)GMISS_WORD * 4, 4096};
    static const uint32_t zero[1024];
    VkSpan sp = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){sp.buf, sp.off, sizeof zero};
}

static void gres_reset(void) {
    pthread_mutex_lock(&gc_lock);
    gc_nback = 0;  // (their shadow / bseq pointers and guest addresses belong to the boot being torn down)
    pthread_mutex_unlock(&gc_lock);
    for (int i = 0; i < ga_nres; i++) { free(ga_res[i].shadow); free(ga_res[i].bseq); }
    memset(ga_res, 0, sizeof ga_res);
    ga_nres = 0;
    ga_res_top = 0;
    ga_head = GA_RES;
    ga_ndirty = 0;
    gl_n = 0;
    memset(ga_seg_seq, 0, sizeof ga_seg_seq);
}

// The storage buffers of the stages in stage_mask (bit = st.sbuf index) into the ring; store: copied back later.
// The heap placed in GPU memory but larger than one storage buffer reaches (maxStorageBufferRange, 2 GB on Adreno;
// BOTW's heap is ~3 GB): a vertex or fragment shader's global loads past that read zero - BOTW's text, whose glyphs a
// vertex shader reads from a buffer ~2.7 GB into the heap, didn't show. Draws then work like gfast_prepare's window:
// the table (GMap) lists the storage buffers bound to the draw's stages (with gcopy_prepare's margin before each), all
// inside one window of the heap that starts at the first of them (GFast's descriptor: gmem_buf from that offset).
// Accesses outside the table go to the heap from its start, as before.
static void gwin_prepare(unsigned stage_mask) {
    memset(gmap_tab, 0, sizeof gmap_tab);
    gfast_n = 0;
    gc_size = 0;
    gmem_info();
    uint64_t reach = g_vk.props.limits.maxStorageBufferRange & ~3ull;
    gfast_win = (VkDescriptorBufferInfo){gmem_buf, 0, reach < gmem_size ? reach : gmem_size};
    if (!gmem_buf) return;
    GcRange r[6 * 16];
    int n = 0;
    for (int s = 0; s < 6; s++) {
        if (!(stage_mask & (1u << s))) continue;
        for (int i = 0; i < 16; i++) {  // heap offsets, 256-aligned, 4 KB of margin before (gcopy_prepare)
            uint64_t a = st.sbuf[s][i].addr, sz = st.sbuf[s][i].size;
            if (!a || !sz || a < HEAP_BASE || a - HEAP_BASE + sz > gmem_size) continue;
            uint64_t lo = (a - HEAP_BASE) & ~255ull, hi = (a - HEAP_BASE + sz + 255) & ~255ull;
            lo = lo > 4096 ? (lo - 4096) & ~255ull : 0;
            if (hi > gmem_size) hi = gmem_size;
            int k = n++;
            while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }
            r[k].lo = lo; r[k].hi = hi;
        }
    }
    int m = 0;
    for (int i = 0; i < n; i++) {  // merged
        if (m && r[i].lo <= r[m - 1].hi + 4096) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
    if (!m) return;
    uint64_t al = g_vk.props.limits.minStorageBufferOffsetAlignment ? g_vk.props.limits.minStorageBufferOffsetAlignment : 256;
    uint64_t base = r[0].lo / al * al;
    if (reach > gmem_size - base) reach = gmem_size - base;
    int k = 0;
    for (int i = 0; i < m && k < GMAP_MAX; i++) {
        if (r[i].hi - base > reach) break;  // (out of the window's reach: the heap from its start, as before)
        gmap_tab[k * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[k * 4 + 1] = (uint32_t)(r[i].hi - r[i].lo);
        gmap_tab[k * 4 + 2] = (uint32_t)((r[i].lo - base) >> 2);
        k++;
    }
    gmap_tab[GMAP_MAX * 4] = (uint32_t)k;
    gfast_win = (VkDescriptorBufferInfo){gmem_buf, base, reach};
}

// The heap placed (gwin_mode), a draw whose only global memory is its vertex shader's loads, a little of it: those
// ranges copied as the draw is recorded, as its constant buffers are, instead of read through the heap's window when
// the GPU runs. BOTW's text draws its glyphs from a buffer the game fills each frame: with the sync lag the CPU was
// already writing the next frame's into it, and the Game Over title showed only "ME" - other glyphs' places - until
// the game left the screen (2026-10-28). false: too much, or nothing bound (gwin_prepare then).
static bool gsnap_prepare(void) {
    memset(gmap_tab, 0, sizeof gmap_tab);
    gfast_n = 0;
    gc_size = 0;
    GcRange r[16];
    int n = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t a = st.sbuf[0][i].addr, sz = st.sbuf[0][i].size;
        if (!a || !sz || a < HEAP_BASE || a - HEAP_BASE + sz > gmem_size) continue;
        uint64_t lo = (a - HEAP_BASE) & ~255ull, hi = (a - HEAP_BASE + sz + 255) & ~255ull;
        if (hi > gmem_size) hi = gmem_size;
        int k = n++;
        while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }
        r[k].lo = lo; r[k].hi = hi;
    }
    int m = 0;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        if (m && r[i].lo <= r[m - 1].hi) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
    for (int i = 0; i < m; i++) total += r[i].hi - r[i].lo;
    if (!m || m > GMAP_MAX || total > (64u << 10)) return false;
    gc_span = vk_transient(total, 256);
    uint64_t off = 0;
    for (int i = 0; i < m; i++) {
        uint64_t len = r[i].hi - r[i].lo;
        memcpy((uint8_t *)gc_span.ptr + off, (const void *)(uintptr_t)(HEAP_BASE + r[i].lo), len);
        gmap_tab[i * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[i * 4 + 1] = (uint32_t)len;
        gmap_tab[i * 4 + 2] = (uint32_t)(off >> 2);
        off += len;
    }
    gc_size = total;
    gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
    return true;
}

static void gcopy_prepare(unsigned stage_mask, bool store) {
    memset(gmap_tab, 0, sizeof gmap_tab);
    gfast_n = 0;
    gc_size = 0;
    GcRange r[6 * 16];
    int n = 0;
    // a margin before each bound range (LARECOMP_GMARGIN bytes, 4 KB): the lighting's shaders read a header 0x100-0x200
    // bytes before the light list's bound address (the light count: ~950k reads a frame outside the tables - zero
    // lights, a dark scene - LARECOMP_GMISS); on Turnip the heap itself is there
    static long margin = -1;
    if (margin < 0) { const char *e = getenv("LARECOMP_GMARGIN"); margin = e ? atol(e) : 4096; }
    for (int s = 0; s < 6; s++) {
        if (!(stage_mask & (1u << s))) continue;
        for (int i = 0; i < 16; i++) {  // heap offsets, 256-aligned
            uint64_t a = st.sbuf[s][i].addr, sz = st.sbuf[s][i].size;
            if (!a || !sz || a < HEAP_BASE || a - HEAP_BASE + sz > HEAP_MAX) continue;
            uint64_t lo = (a - HEAP_BASE) & ~255ull, hi = (a - HEAP_BASE + sz + 255) & ~255ull;
            lo = lo > (uint64_t)margin ? (lo - (uint64_t)margin) & ~255ull : 0;
            int k = n++;
            while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }
            r[k] = (GcRange){lo, hi, store};
        }
    }
    int m = 0;
    for (int i = 0; i < n; i++) {  // merged (nearby bound ranges too: a table entry costs more than a small gap)
        if (m && r[i].lo <= r[m - 1].hi + 4096) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
    if (m > GMAP_MAX) m = GMAP_MAX;
    // The learned ranges (reached through pointers - gres_learn): newest first while table slots remain, never
    // expanding a storing range's write-back or collapsing unrelated heap regions into one multi-megabyte span.
    for (int i = gl_n - 1; i >= 0 && m < GMAP_MAX - 2; i--) {
        uint64_t lo = gl_learned[i].lo, hi = gl_learned[i].hi;
        bool covered = false;
        for (int k = 0; k < m && !covered; k++) {
            if (r[k].lo <= lo && hi <= r[k].hi) { covered = true; break; }
            if (!r[k].store && lo <= r[k].hi + 4096 && hi + 4096 >= r[k].lo) {
                if (lo < r[k].lo) r[k].lo = lo;
                if (hi > r[k].hi) r[k].hi = hi;
                covered = true;
            }
        }
        if (covered) continue;
        int k = m++;
        while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }
        r[k] = (GcRange){lo, hi, false};
    }
    uint64_t total = 0;
    for (int i = 0; i < m; i++) total += (r[i].hi - r[i].lo + 255) & ~255ull;
    {   // LARECOMP_LOG_GCOPY=n: every copy of one frame in n (which stages, storing or not, the ranges) - diagnostics
        static long logn = -1;
        if (logn < 0) { const char *e = getenv("LARECOMP_LOG_GCOPY"); logn = e ? atol(e) : 0; }
        if (logn > 0 && renderer_present_count() % (uint64_t)logn == 0) {
            char b[512];
            int o = 0;
            for (int i = 0; i < m && o < 440; i++)
                o += snprintf(b + o, sizeof b - o, " %llx+%llx", (unsigned long long)r[i].lo, (unsigned long long)(r[i].hi - r[i].lo));
            LOG("[gcopy] frame %llu stages %x %s %d ranges %.1f KB:%s%s\n", (unsigned long long)renderer_present_count(),
                stage_mask, store ? "STORE" : "load", m, total / 1024.0, b, total > CHUNK_SIZE ? " (TOO MUCH: skipped)" : "");
        }
    }
    if (m && gres_prepare(r, m, store)) return;  // (the arena: what shaders store stays on the GPU)
    memset(gmap_tab, 0, sizeof gmap_tab);
    gc_size = 0;
    if (!m || total > CHUNK_SIZE) return;  // (too much: those loads read zero, as without this)
    gc_span = vk_transient(total, 256);
    gc_size = total;
    uint64_t off = 0, seq = vk_pending_seq();
    pthread_mutex_lock(&gc_lock);
    for (int i = 0; i < m; i++) {
        uint64_t len = r[i].hi - r[i].lo;
        memcpy((uint8_t *)gc_span.ptr + off, (const void *)(uintptr_t)(HEAP_BASE + r[i].lo), len);
        gmap_tab[i * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[i * 4 + 1] = (uint32_t)len;
        gmap_tab[i * 4 + 2] = (uint32_t)(off >> 2);
        if (r[i].store && !gres_init() && gc_nback < 256)
            gc_back[gc_nback++] = (GcBack){(const uint8_t *)gc_span.ptr + off, HEAP_BASE + r[i].lo, len, seq, NULL, NULL};
        off += (len + 255) & ~255ull;
    }
    pthread_mutex_unlock(&gc_lock);
    gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
}
#endif

// ---- vertex / index cache
// Reading vertex / index data in place from the imported heap crosses PCIe for every vertex fetched on a discrete GPU
// (outdoors at 3x internal resolution on an RTX 3080 Ti: ~0.7-1.9 ms of GPU a frame; more on PCIe 3.0), and copying it
// per draw instead moves about as much onto the CPU. So each range bound for drawing gets a device-local copy, refreshed
// only where the CPU wrote since (the OS's page write tracking, plat_written_pages, asked at submission: vc_flush): one
// sequential GPU copy of the written pages in the submission's upload buffer (which completes before its draws) instead
// of fetches across the bus. Overlapping ranges are separate entries; the per-page generation keeps them all current.
// Pages the GPU writes itself (a compute dispatch's copy-back, gfast_finish) are never cached: draws over them read the
// heap in place as before, and so does everything when the OS gives no write tracking. LARECOMP_VK_VCACHE=0: off.
#define VC_PAGE 4096ull
#define VC_TAB 8192                   // entries (open addressing)
#define VC_MAX_RANGE (64ull << 20)    // larger bound ranges: read in place
#define VC_BUDGET (768ull << 20)      // device-local bytes kept; the least recently used go first
typedef struct { uint64_t lo, hi, used_seq; uint32_t gen, epoch; VkBuffer buf; VmaAllocation mem; bool gpu; } VcEnt;
static VcEnt vc_tab[VC_TAB];
static uint32_t vc_pend[VC_TAB];  // entries the pending submission draws from (vc_flush brings them up to date)
static int vc_npend;
static uint32_t *vc_page_gen;  // per heap page: the generation of its last CPU write seen (vc_gen when observed)
static uint8_t *vc_page_gpu;   // per heap page: written by the GPU
static uint32_t vc_gen = 1, vc_epoch = 1;  // (epoch: bumped when pages turn GPU-written: entries check again)
static uint64_t vc_bytes;
#define VC_DEAD 1024
static struct { VkBuffer b; VmaAllocation m; uint64_t seq; } vc_dead[VC_DEAD];
static int vc_ndead;
static int vc_on = -1;
// (profiler: draws served / read in place, bytes refreshed, write tracking queries, written pages seen, query time)
uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;

static void vc_free_dead(bool all) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < vc_ndead; i++) {
        if (!all && vc_dead[i].seq > done) { vc_dead[k++] = vc_dead[i]; continue; }
        vmaDestroyBuffer(g_vk.vma, vc_dead[i].b, vc_dead[i].m);
    }
    vc_ndead = k;
}

static bool vc_drop(VcEnt *e) {  // (freed once the submissions that may read it are done)
    if (vc_ndead == VC_DEAD) { vk_wait_seq(vk_pending_seq() - 1); vc_free_dead(false); }
    if (vc_ndead == VC_DEAD) return false;  // (all read by the pending submission: keep it)
    vc_dead[vc_ndead].b = e->buf; vc_dead[vc_ndead].m = e->mem; vc_dead[vc_ndead++].seq = e->used_seq;
    vc_bytes -= e->hi - e->lo;
    memset(e, 0, sizeof *e);
    return true;
}

// The page tables (vc_page_gen / vc_page_gpu) exist whenever the OS gives write tracking: the vertex cache and the
// texture upload checks (vk_pages_changed) share them. Each write tracking query resets the OS bits, so every query
// stamps the pages it saw written with a new generation, and each user compares against the generation it last saw.
static int pt_state = -1;  // 1: tracking, 0: none
static bool pt_init(void) {
    if (pt_state >= 0) return pt_state;
    uint64_t pg = VC_PAGE, one;
    int n = plat_written_pages(HEAP_BASE, VC_PAGE, &one, 1, &pg);  // (is there write tracking?)
    pt_state = n >= 0 && pg == VC_PAGE;
    if (!pt_state) { LOG("[vk] no page write tracking (%d, page %llu)\n", n, (unsigned long long)pg); return false; }
    vc_page_gen = calloc(HEAP_MAX / VC_PAGE, sizeof *vc_page_gen);
    vc_page_gpu = calloc(HEAP_MAX / VC_PAGE, 1);
    vc_page_gen[0] = ++vc_gen;  // (the probe took page 0's tracking)
    return true;
}

// Texture upload checks (textures.c): may guest memory [addr, addr + size) have changed since generation `since`?
// 0: no CPU write since, 1: written, -1: unknown (no tracking, outside the heap, or pages the GPU writes).
// *now: the generation to pass next time.
int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (!size || addr < HEAP_BASE || addr - HEAP_BASE + size > HEAP_MAX || !pt_init()) return -1;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    static uint64_t pages[4096];
    uint64_t pg = VC_PAGE;
    int n = plat_written_pages(HEAP_BASE + lo, hi - lo, pages, 4096, &pg);
    if (n < 0) return -1;
    uint32_t g = ++vc_gen;
    if (n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;
    else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
    *now = g;
    int changed = 0;
    for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) {
        if (vc_page_gpu[p]) return -1;
        if (vc_page_gen[p] > since) changed = 1;
    }
    return changed;
}

// Heap pages [lo, hi) (offsets) written by the GPU: never cached from now on.
static void vc_mark_gpu(uint64_t lo, uint64_t hi) {
    if (!pt_init()) return;
    for (uint64_t p = lo / VC_PAGE; p < (hi + VC_PAGE - 1) / VC_PAGE && p < HEAP_MAX / VC_PAGE; p++) vc_page_gpu[p] = 1;
    vc_epoch++;
}

// A storing shader (stage 0 / 1 / 5) may write any of its bound storage buffers.
// The last store's submission (sync lag), and LARECOMP_LOG_STORES=<frame>: that frame's storing draws and dispatches -
// the stage, its shader's code address, the submission and the storage buffers bound (where its stores may land)
static void note_gmem_store(int stage) {
    atomic_store(&g_gmem_store_seq, vk_pending_seq());
    static long at = -2;
    if (at == -2) at = getenv("LARECOMP_LOG_STORES") ? atol(getenv("LARECOMP_LOG_STORES")) : -1;
    if (at < 0 || (long)renderer_present_count() != at || !st.program) return;
    char line[700];
    int o = snprintf(line, sizeof line, "[stores] frame %ld seq %llu stage %d code %llx:", at, (unsigned long long)vk_pending_seq(),
                     stage, (unsigned long long)st.program->stages[stage].code_addr);
    for (int i = 0; i < 16 && o < (int)sizeof line - 48; i++)
        if (st.sbuf[stage][i].addr)
            o += snprintf(line + o, sizeof line - o, " sb%d %llx+%llx", i, (unsigned long long)st.sbuf[stage][i].addr,
                          (unsigned long long)st.sbuf[stage][i].size);
    LOG("%s\n", line);
}

static void vc_mark_sbufs(int stage) {
    for (int i = 0; i < 16; i++) {
        uint64_t a = st.sbuf[stage][i].addr, sz = st.sbuf[stage][i].size;
        if (a >= HEAP_BASE && sz && a - HEAP_BASE < HEAP_MAX) vc_mark_gpu(a - HEAP_BASE, a - HEAP_BASE + sz);
    }
}

static bool vcache_lookup(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (vc_on < 0) {
        const char *e = getenv("LARECOMP_VK_VCACHE");
        vc_on = !e || atoi(e) != 0;
        if (vc_on && !pt_init()) {
            LOG("[vk] vertex cache off: no page write tracking\n");
            vc_on = 0;
        }
    }
#ifdef __ANDROID__
    bool cpu_fill = !gmem_buf && gcopy_mode;  // (Qualcomm's driver: filled from the CPU - vc_flush)
#else
    bool cpu_fill = false;
#endif
    if (!vc_on || (!gmem_buf && !cpu_fill) || size > VC_MAX_RANGE) return false;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    if (hi > (gmem_buf ? gmem_size : HEAP_MAX)) return false;
    uint64_t pend = vk_pending_seq();
    uint64_t h = (lo * 0x9E3779B97F4A7C15ull) ^ (hi * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VC_TAB - 1), free_slot = ~0u, old_slot = ~0u;
    VcEnt *e = NULL;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VC_TAB - 1)) {
        VcEnt *c = &vc_tab[slot];
        if (c->buf && c->lo == lo && c->hi == hi) { e = c; break; }
        if (!c->buf && free_slot == ~0u) free_slot = slot;
        if (c->buf && c->used_seq < pend && (old_slot == ~0u || c->used_seq < vc_tab[old_slot].used_seq)) old_slot = slot;
    }
    if (!e) {  // a new range: its own device-local buffer (gen 0: copied whole at the submission)
        if (vc_npend == VC_TAB) return false;
        if (vc_ndead) vc_free_dead(false);
        if (free_slot == ~0u) {
            if (old_slot == ~0u || !vc_drop(&vc_tab[old_slot])) return false;
            free_slot = old_slot;
        }
        while (vc_bytes + (hi - lo) > VC_BUDGET) {  // over budget: the least recently used entry goes
            int lru = -1;
            for (int i = 0; i < VC_TAB; i++)
                if (vc_tab[i].buf && vc_tab[i].used_seq < pend && (lru < 0 || vc_tab[i].used_seq < vc_tab[lru].used_seq)) lru = i;
            if (lru < 0 || !vc_drop(&vc_tab[lru])) return false;
        }
        e = &vc_tab[free_slot];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = hi - lo,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &e->buf, &e->mem, NULL) != VK_SUCCESS) { memset(e, 0, sizeof *e); return false; }
        e->lo = lo; e->hi = hi;
        vc_bytes += hi - lo;
    }
    if (e->epoch != vc_epoch) {  // (pages turned GPU-written since: is this range one of them?)
        e->gpu = false;
        for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE && !e->gpu; p++) e->gpu = vc_page_gpu[p];
        e->epoch = vc_epoch;
    }
    if (e->gpu) { prof_vc_bypass++; return false; }
    if (e->used_seq != pend) {  // first use in this submission: brought up to date by vc_flush before it runs
        e->used_seq = pend;
        vc_pend[vc_npend++] = (uint32_t)(e - vc_tab);
    }
    prof_vc_hits++;
    *buf = e->buf;
    *off = addr - HEAP_BASE - lo;
    return true;
}

static int vc_pend_cmp(const void *a, const void *b) {
    uint64_t x = vc_tab[*(const uint32_t *)a].lo, y = vc_tab[*(const uint32_t *)b].lo;
    return x < y ? -1 : x > y;
}

// At submission (vk_submit_extra, before the upload buffer closes): the pages the CPU wrote in the ranges this
// submission draws from - one write tracking query per run of nearby ranges (a query costs about as much as scanning
// ~64 pages: ranges closer than that share one) - and their copies into the cache, ahead of the draws.
void vc_flush(void) {
    if (!vc_npend) return;
    qsort(vc_pend, (size_t)vc_npend, sizeof vc_pend[0], vc_pend_cmp);
    uint32_t g = ++vc_gen;
    static uint64_t pages[4096];
    uint64_t t0 = SDL_GetTicksNS();
    for (int i = 0; i < vc_npend;) {
        uint64_t lo = vc_tab[vc_pend[i]].lo, hi = vc_tab[vc_pend[i]].hi;
        int j = i + 1;
        for (; j < vc_npend && vc_tab[vc_pend[j]].lo <= hi + 64 * VC_PAGE; j++)
            if (vc_tab[vc_pend[j]].hi > hi) hi = vc_tab[vc_pend[j]].hi;
        uint64_t pg = VC_PAGE;
        int n = plat_written_pages(HEAP_BASE + lo, hi - lo, pages, 4096, &pg);
        prof_vc_queries++;
        if (n < 0 || n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;  // (all)
        else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
        if (n > 0) prof_vc_scan_pages += (uint64_t)n;
        i = j;
    }
    prof_vc_scan_ns += SDL_GetTicksNS() - t0;
    VkCommandBuffer cb = vk_upload_cmd();
    for (int i = 0; i < vc_npend; i++) {
        VcEnt *e = &vc_tab[vc_pend[i]];
        VkBufferCopy cp[64];
        int nc = 0;
        for (uint64_t p = e->lo / VC_PAGE; p < e->hi / VC_PAGE; p++) {
            if (e->gen && vc_page_gen[p] <= e->gen) continue;
            uint64_t o = p * VC_PAGE;
            if (nc && cp[nc - 1].srcOffset + cp[nc - 1].size == o) { cp[nc - 1].size += VC_PAGE; continue; }
            if (nc == 64) { nc = 1; cp[0] = (VkBufferCopy){e->lo, 0, e->hi - e->lo}; break; }  // (scattered: the whole range)
            cp[nc++] = (VkBufferCopy){o, o - e->lo, VC_PAGE};
        }
        if (nc && gmem_buf) vkCmdCopyBuffer(cb, gmem_buf, e->buf, (uint32_t)nc, cp);
        else if (nc) {  // (copy mode: no heap buffer - the written pages go through the upload ring)
            uint64_t tot = 0, o = 0;
            for (int k = 0; k < nc; k++) tot += cp[k].size;
            VkSpan sp = vk_transient(tot, 256);
            for (int k = 0; k < nc; k++) {
                memcpy((uint8_t *)sp.ptr + o, (const void *)(uintptr_t)(HEAP_BASE + cp[k].srcOffset), cp[k].size);
                cp[k].srcOffset = sp.off + o;
                o += cp[k].size;
            }
            vkCmdCopyBuffer(cb, sp.buf, e->buf, (uint32_t)nc, cp);
        }
        for (int k = 0; k < nc; k++) prof_vc_copied += cp[k].size;
        e->gen = g;
    }
    vc_npend = 0;
}

// Vertex / index data from the imported guest heap (renderer.m bind_guest reads g_guest_buffer the same way): the
// device-local cache above, else read in place - no per-draw copy, no index scan for the copy size. The game fences its
// CPU writes as on the Switch. Falls back to copies outside the heap or without the import; LARECOMP_VK_COPY_VERTICES=1
// forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
#ifdef __ANDROID__
    if (gcopy_mode && !gmem_buf) return addr + size <= HEAP_BASE + HEAP_MAX && vcache_lookup(addr, size, buf, off);  // (else copied)
#endif
    if (addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) return false;
    if (vcache_lookup(addr, size, buf, off)) return true;
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    g_cs_n = 0;
    gcopy_gen++;
    if (gmem_buf) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    gmem_size = gmem_tried = 0;
    g_inplace_copy = -1;  // (the next boot reads the setting again: resolve_feedback)
#ifdef __ANDROID__
    gcopy_mode = !g_vk.host_import && !getenv("LARECOMP_VK_NO_GCOPY");  // (until the next heap is placed: vk_heap_place)
    gres_reset();  // (the arena's resident ranges: keyed by the old heap's addresses)
#endif
    pthread_mutex_lock(&wb_lock);
    for (int i = 0; i < WB_SLOTS; i++) wb_ent[i].busy = false;
    pthread_mutex_unlock(&wb_lock);
    pthread_mutex_lock(&cq_lock);
    for (int i = 0; i < SUBS; i++) { cq[i].n = 0; cq[i].ts = false; }
    pthread_mutex_unlock(&cq_lock);
    for (int i = 0; i < VC_TAB; i++)  // (the vertex cache: copies of the old pages)
        if (vc_tab[i].buf) vmaDestroyBuffer(g_vk.vma, vc_tab[i].buf, vc_tab[i].mem);
    memset(vc_tab, 0, sizeof vc_tab);
    vc_npend = 0;
    vc_free_dead(true);
    vc_bytes = 0;
    if (vc_page_gpu) memset(vc_page_gpu, 0, HEAP_MAX / VC_PAGE);
    vc_epoch++;
    st_defaults();
}

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    ShaderTexture *st_ = &si->tex[i];
    uint64_t h = shader_texture_handle(stage, st_);
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex) {
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. LARECOMP_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = getenv("LARECOMP_VK_NO_TEX_MEMO") != NULL;
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)e->tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(naliases ? alias_read(e->tex) : e->tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = e->tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(e->tex, &sc[0], &sc[1]);
            if (e->tex == scene_up.t && stage == 1) { view = scene_up_view(e); sc[0] = scene_up.sx; sc[1] = scene_up.sy; }
            wh[0] = e->tex->width; wh[1] = e->tex->height;
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && is_attachment_entry(e) && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_view[k];
                    TexBackend *sn = feedback_snap[k];
                    if (sn && (st_->type == TEXTYPE_2D_ARRAY || sn->layers > 1 || sn->levels > 1)) {
                        // (the entry's own levels / layers, as vk_view takes them, over the full copy)
                        const NvnTextureView *v = e->has_view ? &e->view : NULL;
                        uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0, ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
                        if (lv0 >= sn->levels) lv0 = sn->levels - 1;
                        if (ly0 >= sn->layers) ly0 = sn->layers - 1;
                        uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : sn->levels - lv0;
                        uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : sn->layers - ly0;
                        if (lv0 + nlv > sn->levels) nlv = sn->levels - lv0;
                        if (ly0 + nly > sn->layers) nly = sn->layers - ly0;
                        static const VkComponentMapping idm = {0};
                        VkImageAspectFlags sa2 = sn->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sn->aspect;
                        view = st_->type == TEXTYPE_2D_ARRAY
                             ? vk_image_view(sn, VK_IMAGE_VIEW_TYPE_2D_ARRAY, sn->format, lv0, nlv, ly0, nly, idm, sa2)
                             : vk_image_view(sn, VK_IMAGE_VIEW_TYPE_2D, sn->format, lv0, nlv, ly0, 1, idm, sa2);
                    } else if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);
                }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) {
        NvnSamplerDesc *sd = &((NvnSamplerDesc *)st.spool->entries)[tsc];
        smp = st_->shadow ? vk_sampler(sd) : vk_sampler_plain(sd);
    }
    {   // (diagnosis) LARECOMP_VS_NEAREST=1: the vertex stage's textures through a nearest, clamped sampler - the terrain
        // reads its heights there, and a seam between two tiles that closes with it comes from the filtering
        static int nearest = -1;
        static VkSampler ns;
        if (nearest < 0) nearest = getenv("LARECOMP_VS_NEAREST") != NULL;
        if (nearest && stage == 0 && !st_->shadow) {
            if (!ns) {
                VkSamplerCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                                          .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                          .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 16};
                vkCreateSampler(g_vk.dev, &ci, NULL, &ns);
            }
            smp = ns;
        }
    }
    if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
static float g_mirror_w;  // mirror mode's composite: its render target width (rscale[65].x; 0 otherwise)

static bool is_ubo(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; }

// dyn / ndyn: the set's dynamic uniform buffer offsets, in binding order (its bindings of that type: compile_program)
static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii,
                                 uint32_t *dyn, uint32_t *ndyn) {
#ifdef __ANDROID__
    VkDescriptorSet ds = VK_NULL_HANDLE;  // (allocated at the end, unless this submission already wrote the same set)
#else
    VkDescriptorSet ds = vk_alloc_set(layout);
#endif
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
#ifdef __ANDROID__
    static int planned = -1;
    if (planned < 0) {
        const char *e = getenv("LARECOMP_VK_DESCRIPTOR_PLAN");
        planned = e && atoi(e) != 0;  // experimental: keep the measured stable path as the default
    }
    const DescriptorPlan *plan = &p->descriptor_plans[set];
#endif
#ifdef __ANDROID__
    float rs[66][4];  // (filled when the shader has the table: binding 18 below)
#else
    float rs[66][4] = {{1, 1, 0, 0}};  // ([65]: mirror mode, renderer.m)
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    rs[65][0] = g_mirror_w;
#endif
#ifdef __ANDROID__
    int regular_count = planned ? plan->nregular : p->nbinds;
    for (int i = 0; i < regular_count; i++) {
        VkBindingInfo *b = &p->binds[planned ? plan->regular[i] : i];
#else
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
#endif
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (is_ubo(b->type) && b->binding < 17) {
            uint64_t tc_ = prof > 0 ? plat_now_ns() : 0;
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            if (prof > 0) prof_cbuf += plat_now_ns() - tc_;
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
#ifdef __ANDROID__
            if (b->binding == GMISS_BUFFER_INDEX) bi[n] = gmiss_info();
            else
#endif
            bi[n] = b->binding == GFAST_BUFFER_INDEX ? gfast_info() : gmem_info();
            w[n].pBufferInfo = &bi[n];
#ifdef __ANDROID__
            if (!gmem_buf && b->binding != GFAST_BUFFER_INDEX && b->binding != GMISS_BUFFER_INDEX) {  // (diagnostic: global memory without the heap import)
                static const void *seen[256];
                static int nseen;
                bool known = false;
                for (int k = 0; k < nseen && !known; k++) known = seen[k] == si;
                if (!known && nseen < 256) {
                    seen[nseen++] = si;
                    LOG("[vk] gmem off: stage %d shader %p %s (%d such shaders)\n", stage, (const void *)si,
                        si->uses_gmem_store ? "STORES" : "loads", nseen);
                }
            }
#endif
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
#ifdef __ANDROID__
    int deferred_count = planned ? plan->ndeferred : p->nbinds;
    for (int i = 0; i < deferred_count; i++) {
        VkBindingInfo *b = &p->binds[planned ? plan->deferred[i] : i];
#else
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
#endif
        if (b->set != set || !is_ubo(b->type) ||
            (b->binding != 18 && b->binding != 19 && b->binding != GMAP_BUFFER_INDEX && b->binding != CBPATCH_BUFFER_INDEX)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == CBPATCH_BUFFER_INDEX) {  // the rows cbwin_bind took for this stage (its cbuf_info calls: above)
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_CBP_F : DEDUPE_CBP_V, &cbw_patch[sg], sizeof cbw_patch[sg], 256),
                             sizeof cbw_patch[sg]);
        } else if (b->binding == GMAP_BUFFER_INDEX) {  // (compute) this dispatch's fast-copy table: gfast_prepare
#ifdef __ANDROID__
            bi[n] = ubo_span(gmap_copy(), sizeof gmap_tab);
#else
            bi[n] = ubo_span(vk_transient_copy(gmap_tab, sizeof gmap_tab, 256), sizeof gmap_tab);
#endif
        } else if (b->binding == 18) {
#ifdef __ANDROID__
            memset(rs, 0, sizeof rs);
            rs[0][0] = rs[0][1] = 1;  // ([65]: mirror mode, renderer.m)
            if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
            rs[65][0] = g_mirror_w;
#endif
            for (int t = 0; t < si->ntex && t < 64; t++) { rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; }
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    {   // dynamic uniform buffers: their offsets out of the set (in binding order), the descriptors at offset 0
        uint32_t nd = 0, bind[MAX_BIND];
        for (int i = 0; i < n; i++) {
            if (w[i].descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
            uint32_t k = nd++;
            while (k > 0 && bind[k - 1] > w[i].dstBinding) { bind[k] = bind[k - 1]; if (dyn) dyn[k] = dyn[k - 1]; k--; }
            bind[k] = w[i].dstBinding;
            if (dyn) dyn[k] = (uint32_t)bi[i].offset;
            bi[i].offset = 0;
        }
        if (ndyn) *ndyn = nd;
    }
#ifdef __ANDROID__
    // Draws of one material in a row write the same set over and over (same buffers / offsets / views / samplers): a
    // set written this submission with the same contents (two independent 64-bit hashes of them) is bound again
    // instead of allocating and writing another - the descriptor writes were a good part of the driver's time per
    // draw. Sets live as long as the submission's pools. LARECOMP_NO_SET_CACHE=1: off.
    {
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_SET_CACHE") != NULL;
        uint64_t h1 = (uint64_t)(uintptr_t)layout * 0x9E3779B97F4A7C15ull, h2 = (uint64_t)(uintptr_t)layout ^ 0xC2B2AE3D27D4EB4Full;
#define DS_MIX(v) do { uint64_t v_ = (uint64_t)(v); h1 = (h1 ^ v_) * 0x100000001B3ull; h1 ^= h1 >> 29; \
                       h2 = (h2 + v_) * 0xFF51AFD7ED558CCDull; h2 ^= h2 >> 33; } while (0)
        for (int i = 0; i < n; i++) {
            DS_MIX(w[i].dstBinding | (uint64_t)w[i].descriptorType << 32);
            if (w[i].pImageInfo) { DS_MIX((uintptr_t)w[i].pImageInfo->sampler); DS_MIX((uintptr_t)w[i].pImageInfo->imageView); DS_MIX(w[i].pImageInfo->imageLayout); }
            else { DS_MIX((uintptr_t)w[i].pBufferInfo->buffer); DS_MIX(w[i].pBufferInfo->offset); DS_MIX(w[i].pBufferInfo->range); }
        }
#undef DS_MIX
        static struct { uint64_t seq, h1, h2; VkDescriptorSet ds; } cache[4096];
        uint64_t seq = vk_pending_seq();
        uint32_t slot = (uint32_t)(h1 ^ (h1 >> 32)) & 4095;
        if (!off && cache[slot].seq == seq && cache[slot].h1 == h1 && cache[slot].h2 == h2) {
            prof_set_hits++;
            return cache[slot].ds;
        }
        prof_set_miss++;
        uint64_t tw_ = prof > 0 ? plat_now_ns() : 0;
        ds = vk_alloc_set(layout);
        for (int i = 0; i < n; i++) w[i].dstSet = ds;
        if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
        if (prof > 0) prof_dsw += plat_now_ns() - tw_;
        cache[slot].seq = seq, cache[slot].h1 = h1, cache[slot].h2 = h2, cache[slot].ds = ds;
        return ds;
    }
#else
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    return ds;
#endif
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; uint64_t code_addr; } CsProg;

static void build_compute(void *arg) {
    CsProg *cp = arg;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { atomic_store(&cp->p.state, 2); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&cp->p.state, r == VK_SUCCESS ? 1 : 2);
    if (r != VK_SUCCESS || getenv("LARECOMP_LOG_CS_PIPELINES"))
        LOG("[compute] pipeline %llx: result %d, local %ux%ux%u, %zu SPIR-V words, %d bindings\n",
            (unsigned long long)cp->code_addr, r, cp->local[0], cp->local[1], cp->local[2], n, cp->p.nbinds);
    if (r != VK_SUCCESS && getenv("LARECOMP_SAVE_MSL")) {
        char path[1100];
        snprintf(path, sizeof path, "%s/build/glsl/%llx_cs.spv", rt_data_root(), (unsigned long long)cp->code_addr);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(spv, 4, n, f); fclose(f); }
    }
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    free(spv);
}

static struct CsEnt { uint64_t addr; CsProg *cp; } g_cs_tab[256];  // compute programs by code address

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr) return tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    cp->code_addr = ss->code_addr;
    if (n < 256) { tab[n].addr = ss->code_addr; tab[n].cp = cp; g_cs_n = n + 1; }
    pthread_mutex_lock(&translate_lock);  // (the translator's globals too: the translator thread sets them for graphics)
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
#ifdef __ANDROID__
    maxwell_no_int64 = !g_vk.shader_int64 || g_settings.shader_int32 || getenv("LARECOMP_VK_INT32") != NULL;
    maxwell_gmem_map_all = gcopy_mode || gwin_mode;
    maxwell_gmem_miss_log = gcopy_mode && !getenv("LARECOMP_NO_GMISS");
    if (getenv("LARECOMP_GMISS_STAGES"))  // (a test: which stages log misses - 1 vertex, 2 fragment, 32 compute)
        maxwell_gmem_miss_stages = (unsigned)strtoul(getenv("LARECOMP_GMISS_STAGES"), NULL, 0);
    maxwell_brx_window = !getenv("LARECOMP_BRX_ALL_LABELS");
    maxwell_static_stacks = !getenv("LARECOMP_DYN_STACKS");
    maxwell_structured = structure_on();
    maxwell_no_switch = qcom_no_switch();
    maxwell_no_local_struct = qcom_no_switch() && !getenv("LARECOMP_QCOM_LOCAL");
    maxwell_native_half = !getenv("LARECOMP_BITWISE_HALF");
    maxwell_fast_minmax = !getenv("LARECOMP_SAFE_MATH");
#endif
    ShaderKey k = {0};
    k.glsl = 1;
#ifdef __ANDROID__
    {   // Keep the cached binary lookup for graphics and Turnip; Qualcomm rejects it in large compute shaders.
        extern bool vk_driver_is_qualcomm(void);
        k.gmem_linear_lookup = vk_driver_is_qualcomm();
    }
#endif
    cp->si = maxwell_translate((const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, STAGE_COMPUTE, &k);
    pthread_mutex_unlock(&translate_lock);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)ss->code_addr);
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    run_job(build_compute, cp);
    return cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    {   extern uint64_t nvn_watch_texaddr(void);
        uint64_t wa = nvn_watch_texaddr();
        for (int i = 0; wa && i < 16; i++)
            if (st.sbuf[5][i].addr <= wa && wa < st.sbuf[5][i].addr + st.sbuf[5][i].size)
                LOG("[watch] frame %llu: dispatch %ux%ux%u cs %llx, storage buffer %d %llx+%llx\n", (unsigned long long)renderer_present_count(),
                    gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, i, (unsigned long long)st.sbuf[5][i].addr,
                    (unsigned long long)st.sbuf[5][i].size);
    }
    CsProg *cp = compute_program(st.program);
    if (!cp || atomic_load(&cp->p.state) != 1) { stat_skipped++; return; }
    {   // LARECOMP_SKIP_CS=<code address hex>[,...] (debug): skip those compute shaders' dispatches, to see what they do;
        // "store": every one that stores to global memory
        static uint64_t skip[16];
        static int nskip = -1, skip_store;
        if (nskip < 0) {
            nskip = 0;
            const char *e = getenv("LARECOMP_SKIP_CS");
            if (e && !strcmp(e, "store")) skip_store = 1;
            else
                for (; e && *e && nskip < 16; e += *e == ',') skip[nskip++] = strtoull(e, (char **)&e, 16);
        }
        if (skip_store && cp->si->uses_gmem_store) { stat_skipped++; return; }
        for (int i = 0; i < nskip; i++)
            if (st.program->stages[5].code_addr == skip[i]) { stat_skipped++; return; }
    }
    if (cp->si->uses_gmem && getenv("LARECOMP_LOG_CS_SBUF")) {  // (debug) the storage buffers bound to gmem dispatches
        static uint64_t seen_cs[16];
        static int seen_n[16];
        int k = 0;
        while (k < 15 && seen_cs[k] && seen_cs[k] != st.program->stages[5].code_addr) k++;
        seen_cs[k] = st.program->stages[5].code_addr;
        if (seen_n[k]++ < 24) {
            LOG("[cs] %llx %ux%ux%u:", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
            for (int i = 0; i < 16; i++)
                if (st.sbuf[5][i].addr) LOG(" sb%d %llx+%llx", i, (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size);
            LOG("\n");
        }
    }
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u) cs %llx gmem %d", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2],
            (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    nfeedback = 0;
    if (ct_on()) {
        LOG("[ct] dispatch cs %llx %ux%ux%u | tex", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
        for (int i = 0; i < cp->si->ntex; i++) {
            NvnTexture *t = shader_texture_object(5, &cp->si->tex[i]);
            if (t) LOG(" %s%p/%#x", cp->si->tex[i].is_image ? "I:" : "", (void *)t, t->format);
        }
        LOG("\n");
    }
#ifdef __ANDROID__
    if (gcopy_mode && cp->si->uses_gmem_map) gcopy_prepare(1u << 5, cp->si->uses_gmem_store);
    else
#endif
    if (cp->si->uses_gmem_map) gfast_prepare(cb);
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL, NULL, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    if (cp->si->uses_gmem_map && cp->si->uses_gmem_store) gfast_finish(cb);
    if (cp->si->uses_gmem_store) vc_mark_sbufs(5), note_gmem_store(5);
    stat_dispatches++;
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

static void draw_(CmdRec *r, bool indexed);
// (diagnosis: dark frames) the first skipped draws into a large target - which passes still skip while their shaders
// build: their target's size and whether it has depth
static void skip_note(const char *what) {
    static int n;
    NvnTexture *t = st.rt.ncolor ? st.rt.color[0] : st.rt.depth;
    if (!t || t->width < 640 || n >= 40) return;
    n++;
    LOG("[vk] frame %llu: a draw into %ux%u%s skipped (%s still building)\n", (unsigned long long)renderer_present_count(),
        t->width, t->height, st.rt.depth ? " with depth" : "", what);
}
// (debugging, from SavageWindRecomp's SAVAGEWIND_DEBUG_PICK, MIT) LARECOMP_DEBUG_PICK=<x>,<y>[;<x>,<y>...] (up to 4
// points, in the game's 1600x900 coordinates): in a debug frame each draw copies those pixels of its target afterwards
// (ending its pass), and vk_pick_report logs per point the draws that changed it ("[pick]": the last one is what the
// frame shows there). LARECOMP_DEBUG_PICK_RT=<i>: each pass's target i instead of the first (a G-buffer's other ones).
typedef struct { uint64_t vs, fs, hash; void *img; uint32_t w, h, fmt, no; } PickRec;
static struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; int n, npt, on, rt; int pt[4][2]; uint32_t draws; PickRec rec[4096]; } pick = {.on = -1};

static void pick_after_draw(void) {
    char file_pts[256] = "";
    if (pick.draws == 0) {  // (each debug frame: build/pick.txt, when there, in place of the variable - points picked
        FILE *f = fopen("build/pick.txt", "rb");  //  from a screenshot without starting again; adb can write build/)
        if (f) {
            file_pts[fread(file_pts, 1, sizeof file_pts - 1, f)] = 0;
            fclose(f);
            pick.on = -1, pick.npt = 0;
        }
    }
    if (pick.on < 0) {
        pick.on = 0;
        for (const char *e = file_pts[0] ? file_pts : getenv("LARECOMP_DEBUG_PICK"); e && *e && pick.npt < 4;) {
            if (sscanf(e, "%d,%d", &pick.pt[pick.npt][0], &pick.pt[pick.npt][1]) != 2) break;
            pick.npt++;
            e = strchr(e, ';');
            if (e) e++;
        }
        pick.on = pick.npt > 0;
        const char *rt = file_pts[0] ? strstr(file_pts, "rt=") : NULL;  // (pick.txt: "rt=<i>" anywhere after the points)
        rt = rt ? rt + 3 : getenv("LARECOMP_DEBUG_PICK_RT");
        pick.rt = rt ? atoi(rt) : 0;
        if (pick.rt < 0 || pick.rt > 7) pick.rt = 0;
    }
    pick.draws++;
    const int t = pick.rt;
    if (!pick.on || st.rt.ncolor <= t || !st.rt.color[t] || !st.rt.color[t]->backend || pick.n >= 4096) return;
    TexBackend *b = st.rt.color[t]->backend;
    if ((b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || b->samples > 1 || b->width < 64) return;
    if (!pick.buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096 * 4 * 16, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &pick.buf, &pick.mem, &info));
        pick.ptr = info.pMappedData;
    }
    uint32_t layer = st.rt.has_cview[t] && (st.rt.cview[t].setmask & 2) ? st.rt.cview[t].min_layer : 0;
    uint32_t level = st.rt.has_cview[t] && (st.rt.cview[t].setmask & 1) ? st.rt.cview[t].base_level : 0;
    if (level >= b->levels || layer >= b->layers) return;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    for (int k = 0; k < pick.npt; k++) {
        uint32_t x = (uint32_t)pick.pt[k][0] * b->width / 1600 >> level, y = (uint32_t)pick.pt[k][1] * b->height / 900 >> level;
        memset(pick.ptr + ((size_t)pick.n * 4 + k) * 16, 0, 16);
        if (x >= level_extent(b->width, level) || y >= level_extent(b->height, level)) continue;
        VkBufferImageCopy c = {.bufferOffset = ((VkDeviceSize)pick.n * 4 + k) * 16,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
                               .imageOffset = {(int32_t)x, (int32_t)y, 0}, .imageExtent = {1, 1, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, pick.buf, 1, &c);
    }
    PickRec *pr = &pick.rec[pick.n++];
    pr->vs = st.program->stages[0].code_addr;
    pr->fs = st.program->stages[1].code_addr;
    pr->hash = g_cur_fs_hash;
    pr->img = (void *)b->image;
    pr->w = b->width; pr->h = b->height; pr->fmt = st.rt.color[t]->format; pr->no = pick.draws;
}

void vk_pick_report(void) {
    if (!pick.n) return;
    renderer_flush();
    for (int k = 0; k < pick.npt; k++) {
        LOG("[pick] point %d (%d,%d of 1600x900): draws that changed it\n", k, pick.pt[k][0], pick.pt[k][1]);
        for (int i = 0; i < pick.n; i++) {
            const uint8_t *v = pick.ptr + ((size_t)i * 4 + k) * 16;
            int j = i - 1;  // the previous draw into the same image
            while (j >= 0 && pick.rec[j].img != pick.rec[i].img) j--;
            if (j >= 0 && !memcmp(v, pick.ptr + ((size_t)j * 4 + k) * 16, 16)) continue;
            const uint32_t *u = (const uint32_t *)v;
            LOG("[pick]   draw %u fs %llx hash %016llx vs %llx | target %ux%u f%#x [%p] | %08x %08x %08x %08x\n", pick.rec[i].no,
                (unsigned long long)pick.rec[i].fs, (unsigned long long)pick.rec[i].hash, (unsigned long long)pick.rec[i].vs,
                pick.rec[i].w, pick.rec[i].h, pick.rec[i].fmt, pick.rec[i].img, u[0], u[1], u[2], u[3]);
        }
    }
    pick.n = 0;
    pick.draws = 0;
}

static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    draw_(r, indexed);
    PROF_ADD(prof_draw);
}
static uint64_t prof_tp_;
static void draw_(CmdRec *r, bool indexed) {
    if (prof) prof_tp_ = SDL_GetTicksNS();
    if (!st.program) { stat_skipped++; return; }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
#ifdef __ANDROID__
    if (maxwell_gmem_miss_log)  // (copy mode: early depth tests for the shaders that log misses - ShaderKey)
        fkey.no_ds_write = !st.rt.depth || !((st.ds[0] && st.ds[1]) || (st.ds[3] && ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF)));
#endif
    Prog *pr = get_program(st.program, &vkey, &fkey);
    bool keep = false;  // (a target built once and kept: wait for the shader and pipeline - wait_built)
    {   // (the same targets as the previous draw, same frame: their answer again - reading 6 texture objects a draw
        // showed up)
        static NvnTexture *kc[8], *kd;
        static int kn = -1;
        static bool kkeep;
        static uint64_t kframe = ~0ull;
        uint64_t frame = renderer_present_count();
        if (kn != st.rt.ncolor || memcmp(kc, st.rt.color, sizeof kc) || kd != st.rt.depth || kframe != frame) {
            kn = st.rt.ncolor;
            memcpy(kc, st.rt.color, sizeof kc);
            kd = st.rt.depth;
            kframe = frame;
            kkeep = !st.rt.depth && st.rt.ncolor && wait_no_depth();
            for (int i = 0; i < st.rt.ncolor; i++) {  // (every target: rt_one_off records each one's frame)
                bool once = rt_one_off(st.rt.color[i], frame);
                kkeep |= rt_is_persistent(st.rt.color[i]) | (once && !st.rt.depth);
            }
        }
        keep = kkeep;
    }
    if (pr && keep) wait_built(&pr->state);
    if (!pr || atomic_load(&pr->state) != 1) { stat_skipped++; skip_note("shader"); return; }
    {   // fragment-shader hash: interior backdrop detection (widescreen), skip lists, item twinkle (renderer.m)
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            last_fs = fs;
#ifdef __ANDROID__
            // (memoized per shader address, checked against a copy of its bytes: the byte-serial hash on every
            // fragment-shader change was ~40% of draw_)
            // (1024 slots: BOTW draws a few hundred fragment shaders a frame, and 64 kept colliding)
            static struct { uint64_t addr, hash; uint8_t code[0x200]; } fsh[1024];
            const void *code = (const void *)(uintptr_t)(fs + 0x30);
            unsigned i = (unsigned)((fs * 0x9E3779B97F4A7C15ull) >> 54);
            if (!fs) last_hash = 0;
            else if (fsh[i].addr == fs && !memcmp(fsh[i].code, code, 0x200)) last_hash = fsh[i].hash;
            else {
                last_hash = hmix(1469598103934665603ull, code, 0x200);
                fsh[i].addr = fs, fsh[i].hash = last_hash;
                memcpy(fsh[i].code, code, 0x200);
            }
#else
            last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
#endif
        }
        static uint64_t skip[128];  // (bisecting a frame's shaders: many at once)
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = getenv("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 128;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',' || *p == '+') p++;
            }
        }
        for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { stat_skipped++; return; }
        {   // LARECOMP_SKIPVS=1 (diagnosis): build/skipvs.txt lists vertex shader code addresses ("3da039b00,3da258600";
            // empty: none) whose draws are skipped - re-read twice a second, so a draw can be found by toggling it while
            // the game runs
            static uint64_t vskip[32], vskip_frame = ~0ull;
            static int nvskip, vskip_on = -1;
            if (vskip_on < 0) vskip_on = getenv("LARECOMP_SKIPVS") != NULL;
            uint64_t now = renderer_present_count();
            if (vskip_on && now / 15 != vskip_frame) {
                vskip_frame = now / 15;
                char buf[512] = "";
                FILE *f = fopen("build/skipvs.txt", "rb");
                if (f) { buf[fread(buf, 1, sizeof buf - 1, f)] = 0; fclose(f); }
                int n = 0;
                for (const char *p = buf; *p && n < 32;) {
                    while (*p && !isxdigit((unsigned char)*p)) p++;
                    if (!*p) break;
                    vskip[n++] = strtoull(p, (char **)&p, 16);
                }
                if (n != nvskip) LOG("[vk] skipvs: %d vertex shaders skipped\n", n);
                nvskip = n;
            }
            for (int i = 0; i < nvskip; i++)
                if (st.program->stages[0].code_addr == vskip[i]) { stat_skipped++; return; }
        }
        {   // LARECOMP_LOG_FS_VB=<fs hash>:<stream> (diagnosis): that vertex stream's first 4 floats for the first draw of
            // that fragment shader each frame, as the draw reads them (beside LARECOMP_LOG_FS_CB: a per-instance
            // value and a constant that should change in the same frame)
            static uint64_t want = ~0ull, last_frame = ~0ull;
            static int ws;
            if (want == ~0ull) {
                const char *e = getenv("LARECOMP_LOG_FS_VB");
                unsigned long long h;
                want = e && sscanf(e, "%llx:%d", &h, &ws) == 2 ? h : 0;
            }
            if (want && last_hash == want && renderer_present_count() != last_frame && ws >= 0 && ws < 16 &&
                st.vbuf[ws].addr && st.vbuf[ws].size >= 16) {
                last_frame = renderer_present_count();
                const float *v = (const float *)(uintptr_t)st.vbuf[ws].addr;
                LOG("[fsvb] frame %llu stream %d %llx: %g %g %g %g\n", (unsigned long long)last_frame, ws,
                    (unsigned long long)st.vbuf[ws].addr, v[0], v[1], v[2], v[3]);
            }
        }
        if (g_debug_frame && getenv("LARECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
        {   // Widescreen: a menu's layout pane that shows a picture of the world (the blurred world behind the pause
            // menu: a copy of the scene, a target widened to the window's shape) widens with it - drawn in the middle
            // 16:9 part, its sides were black
            extern bool widescreen_ui_centred(void);
            g_wide_backdrop = false;
            NvnTexture *rt = st.rt.ncolor ? st.rt.color[0] : NULL, *t;
            float sx, sy;
            if (widescreen_ui_centred() && rt && vk_texture_scale_group(rt) == RS_OUT && pr->fi && pr->fi->ntex &&
                (t = shader_texture_object(1, &pr->fi->tex[0])) && t->target == 1) {
                renderer_texture_scale(t, &sx, &sy);
                g_wide_backdrop = sx > sy * 1.01f;
            }
            // A menu layout's pane or text, clipped to the 16:9 screen's part (scissor_rect): the pages slide in and out
            // past its sides when the menu turns to another (Inventory, System), and on the wide screen those sides
            // showed them (2026-10-28) - not the panes as wide as the layout (the bars across the top and bottom, the
            // dimming) nor the pictures of the widened world (the blurred world, the 3D Link) where they sit centred -
            // the page layer slides off the middle with the page (its next page showed past the screen's side)
            g_off_layout = false;
            const Range *u = &st.ubuf[0][0];
            if (widescreen_ui_centred() && rt && vk_texture_scale_group(rt) == RS_OUT && u->addr && u->size >= 64) {
                // (text: one matrix, its constants shorter than a pane's - the labels, not clipped, slid past the sides)
                const float *f = (const float *)(uintptr_t)u->addr;
                bool layout = f[12] == 0 && f[13] == 0 && f[14] == -1.0f;
                bool picture = g_wide_backdrop && u->size >= 0x230 && fabsf(f[19]) < 1.0f;
                g_off_layout = layout && (f[15] > 1.0f || (f[15] == 0 && u->size >= 0x230 && !picture && !full_width_window(f)));
            }
            {   // LARECOMP_LOG_MENUCLIP=1 (diagnosis): the layout draws of the frames where a pane sits past the 16:9 sides
                static int on = -1;
                if (on < 0) on = getenv("LARECOMP_LOG_MENUCLIP") != NULL;
                const float *f = u->addr && u->size >= 0x230 ? (const float *)(uintptr_t)u->addr : NULL;
                extern bool framepace_in_menu(void);
                if (on && rt && f && f[12] == 0 && f[13] == 0 && f[14] == -1.0f && (fabsf(f[19]) > 500.0f || f[15] > 1.0f)) {
                    static uint64_t fr;
                    static int n;
                    if (fr != renderer_present_count()) fr = renderer_present_count(), n = 0;
                    if (n++ < 40)
                        LOG("[mclip] frame %llu fs %016llx rt %ux%u group %d x %.1f w15 %.2f off %d menu %d wide %d\n",
                            (unsigned long long)fr, (unsigned long long)last_hash, rt->width, rt->height,
                            (int)vk_texture_scale_group(rt), f[19], f[15], g_off_layout, framepace_in_menu(), g_wide_backdrop);
                }
            }
        }
        {   // LARECOMP_LOG_FSHASH_FROM=<frame>: each fragment shader drawn from that frame on, once (with its target)
            static long from = -2;
            if (from == -2) from = getenv("LARECOMP_LOG_FSHASH_FROM") ? atol(getenv("LARECOMP_LOG_FSHASH_FROM")) : -1;
            if (from >= 0 && (long)renderer_present_count() >= from) {
                static uint64_t seen[512];
                static int nseen;
                bool f = false;
                for (int i = 0; i < nseen; i++) f |= seen[i] == last_hash;
                if (!f && nseen < 512) {
                    seen[nseen++] = last_hash;
                    NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
                    LOG("[fsnew] frame %llu fs %016llx code %llx vs %llx rt %d %ux%u depth %d poly %02x%02x\n",
                        (unsigned long long)renderer_present_count(), (unsigned long long)last_hash, (unsigned long long)fs,
                        (unsigned long long)st.program->stages[0].code_addr, st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                        st.rt.depth != NULL, st.poly[0], st.poly[1]);
                }
            }
        }
        {   // (LARECOMP_WATCH_TEXADDR + LARECOMP_WATCH_DRAWS=1) each draw into that memory: its vertex constants (bank 3),
            // vertices, view and scissor - where its quad lands
            extern uint64_t nvn_watch_texaddr(void);
            static int wd = -1;
            if (wd < 0) wd = getenv("LARECOMP_WATCH_DRAWS") != NULL;
            NvnTexture *w0 = st.rt.ncolor ? st.rt.color[0] : NULL;
            uint64_t wa = wd ? nvn_watch_texaddr() : 0;
            if (wa && w0 && w0->addr <= wa && wa < w0->addr + tegra_total_size(&w0->layout)) {
                static int nw;
                bool lv0 = !(st.rt.has_cview[0] && st.rt.cview[0].base_level);
                if ((lv0 || getenv("LARECOMP_WATCH_DRAWS_ALL")) && nw++ < 3000) {
                    const float *u = st.ubuf[0][0].addr ? (const float *)(uintptr_t)st.ubuf[0][0].addr : NULL;
                    const float *v = st.vbuf[0].addr ? (const float *)(uintptr_t)st.vbuf[0].addr : NULL;
                    LOG("[wdraw] frame %llu layer %u level %u fs %016llx vp %.0f %.0f %.0f %.0f sc %d %d %d %d | cb3:",
                        (unsigned long long)renderer_present_count(), st.rt.has_cview[0] ? st.rt.cview[0].min_layer : 0,
                        st.rt.has_cview[0] ? st.rt.cview[0].base_level : 0, (unsigned long long)last_hash, st.viewport[0][0],
                        st.viewport[0][1], st.viewport[0][2], st.viewport[0][3], st.scissor[0][0], st.scissor[0][1], st.scissor[0][2],
                        st.scissor[0][3]);
                    LOG(" | ds %02x%02x%02x blend %#x chmask %08x poly %02x%02x depth %s", st.ds[0], st.ds[1], st.ds[2], st.color_state[0],
                        st.channel_mask, st.poly[0], st.poly[1], st.rt.depth ? "yes" : "no");
                    for (int k = 0; u && k < 20; k++) LOG(" %.5g", u[k]);
                    LOG(" | vb:");
                    for (int k = 0; v && k < 16; k++) LOG(" %.4g", v[k]);
                    for (int k = 0; k < pr->fi->ntex && k < 4; k++) {
                        NvnTexture *tt = shader_texture_object(1, &pr->fi->tex[k]);
                        if (tt) LOG(" | ftex%d %ux%ux%u f%#x lv %u @%llx%s", k, tt->width, tt->height, tt->layout.layers, tt->format, tt->levels,
                                    (unsigned long long)tt->addr, tt == w0 ? " (the target)" : "");
                        uint64_t sh = shader_texture_handle(1, &pr->fi->tex[k]);
                        uint32_t tsc = (uint32_t)(sh >> 20) & 0xFFF;
                        if (st.spool && tsc < (uint32_t)st.spool->count) {
                            const NvnSamplerDesc *sd = &((const NvnSamplerDesc *)st.spool->entries)[tsc];
                            LOG(" smp %u wrap %u %u min %u lod %.1f..%.1f", tsc, sd->wrap[0], sd->wrap[1], sd->min_filter, sd->lod_min, sd->lod_max);
                        }
                    }
                    LOG("\n");
                }
            }
        }
        extern void widescreen_backdrop_seen(uint64_t hash);
        if (last_hash) widescreen_backdrop_seen(last_hash);
        extern void widescreen_scene_ubo(const float *u, uint64_t size);  // (the camera's view-projection)
        if (st.ubuf[0][5].addr) widescreen_scene_ubo((const float *)(uintptr_t)st.ubuf[0][5].addr, st.ubuf[0][5].size);
    }
    if (g_debug_frame) debug_draw(pr, r, indexed);
    {   // LARECOMP_LOG_RT=<w>x<h>[:<fmt hex>][+<w>x<h>[:<fmt>]...][,<max>[,<from frame>]]: draws into one color target of
        // those guest sizes (and formats), described as in a debug frame, with the frame number (the pause menu's
        // capture of the world and its blur)
        static int nsz = -1, lw[4], lh[4], lf[4], left;
        static long from;
        if (nsz < 0) {
            nsz = 0;
            const char *e = getenv("LARECOMP_LOG_RT");
            while (e && nsz < 4 && sscanf(e, "%dx%d", &lw[nsz], &lh[nsz]) == 2) {
                const char *colon = strchr(e, ':'), *end = strpbrk(e, "+,");
                lf[nsz] = colon && (!end || colon < end) ? (int)strtol(colon + 1, NULL, 16) : -1;
                nsz++;
                const char *plus = strchr(e, '+'), *comma = strchr(e, ',');
                if (!plus || (comma && comma < plus)) break;
                e = plus + 1;
            }
            const char *comma = e ? strchr(e, ',') : NULL;
            if (comma) sscanf(comma + 1, "%d,%ld", &left, &from);
            if (left <= 0) left = 40;
        }
        bool match = false;
        for (int k = 0; k < nsz && st.rt.ncolor == 1 && st.rt.color[0]; k++)
            match |= (int)st.rt.color[0]->width == lw[k] && (int)st.rt.color[0]->height == lh[k] &&
                     (lf[k] < 0 || (int)st.rt.color[0]->format == lf[k]);
        match &= (long)renderer_present_count() >= from;
        static int rt3d = -1;  // (LARECOMP_LOG_RT3D=1: draws into 3D targets too - colour LUTs rendered slice by slice)
        if (rt3d < 0) rt3d = getenv("LARECOMP_LOG_RT3D") != NULL;
        if (rt3d && st.rt.ncolor >= 1 && st.rt.color[0] && st.rt.color[0]->target == 2) match = true;
        if (match && left > 0 && !g_debug_frame) {
            left--;
            LOG("[logrt] frame %llu fs hash %016llx:\n", (unsigned long long)renderer_present_count(), (unsigned long long)g_cur_fs_hash);
            debug_draw(pr, r, indexed);
        }
    }
    {   // LARECOMP_LOG_UI=<frame>: that frame's 2D-layout draws (nn::ui2d: projection in cb3[0..3], the pane's matrix in
        // cb3[4..6], its quad in cb3[33]) - where each pane is
        static long at = -2;
        if (at == -2) at = getenv("LARECOMP_LOG_UI") ? atol(getenv("LARECOMP_LOG_UI")) : -1;
        const Range *u = &st.ubuf[0][0];
        if (at >= 0 && (long)renderer_present_count() == at && u->addr && u->size >= 80) {
            const float *f = (const float *)(uintptr_t)u->addr;
            if (getenv("LARECOMP_LOG_UI_ALL") && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->width == 1920)
                LOG("[ui-all] fs %016llx vs %llx cb3 size %#llx: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n",
                    (unsigned long long)g_cur_fs_hash, (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)u->size,
                    f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[16], f[17],
                    f[18], f[19]);
            if (u->size >= 0x230 && f[12] == 0 && f[13] == 0 && f[14] == -1.0f && f[15] == 0)
                LOG("[ui] fs %016llx rt %ux%u pane x %.1f y %.1f z %.1f scale %.3f %.3f quad %.1f %.1f %.1f %.1f proj %.4f %.4f\n",
                    (unsigned long long)g_cur_fs_hash, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0,
                    st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0, f[19], f[23], f[27], f[16], f[21],
                    f[33 * 4], f[33 * 4 + 1], f[33 * 4 + 2], f[33 * 4 + 3], f[0], f[5]);
        }
    }
    {   // LARECOMP_FIND_PROJ=<frame>: the constant buffers of that frame's draws holding a 16:9 perspective projection
        // (P[1][1] / P[0][0] = 16/9, five floats apart in either order): where the camera is (widescreen, frame generation)
        static long at = -2;
        if (at == -2) at = getenv("LARECOMP_FIND_PROJ") ? atol(getenv("LARECOMP_FIND_PROJ")) : -1;
        if (at >= 0 && (long)renderer_present_count() == at) {
            static int nlog;
            static uint64_t seen[64];
            static int nseen;
            for (int sg = 0; sg < 2 && nlog < 80; sg++)
                for (int b = 0; b < 18 && nlog < 80; b++) {
                    const Range *u = &st.ubuf[sg][b];
                    if (!u->addr || u->size < 64) continue;
                    const float *f = (const float *)(uintptr_t)u->addr;
                    uint64_t nf = (u->size > 0x1000 ? 0x1000 : u->size) / 4;
                    for (uint64_t k = 0; k + 15 < nf; k++) {
                        float p0 = f[k], p1 = f[k + 5];
                        if (!(p0 > 0.3f && p0 < 6.0f && p1 > p0)) continue;
                        float r = p1 / p0;
                        if (r < 1.76f || r > 1.80f) continue;
                        uint64_t key = (u->addr + k * 4) ^ ((uint64_t)sg << 62);
                        bool dup = false;
                        for (int i = 0; i < nseen; i++) dup |= seen[i] == key;
                        if (dup) continue;
                        if (nseen < 64) seen[nseen++] = key;
                        LOG("[proj] stage %d cbuf %d (+%#llx of %#llx at %llx) fs %016llx: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f\n",
                            sg, b, (unsigned long long)(k * 4), (unsigned long long)u->size, (unsigned long long)u->addr,
                            (unsigned long long)g_cur_fs_hash, f[k], f[k + 1], f[k + 2], f[k + 3], f[k + 4], f[k + 5], f[k + 6],
                            f[k + 7], f[k + 8], f[k + 9], f[k + 10], f[k + 11], f[k + 12], f[k + 13], f[k + 14], f[k + 15]);
                        const uint32_t *w = (const uint32_t *)f;  // (the exact bits: LARECOMP_FIND for the matrix's source)
                        LOG("[proj]   bits %08x %08x %08x %08x and the buffer from +0: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                            w[k], w[k + 5], w[k + 10], w[k + 11], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                        nlog++;
                    }
                }
        }
    }
    if (ct_on()) ct_draw(pr, "draw");
    alias_draw_check();
    // (the attachments this draw writes: once a draw - the targets and masks don't change before it is recorded; three
    // computations of it were ~8% of the render thread on the POCO F7 Ultra)
    const uint32_t draw_w = draw_writes(pr);
    resolve_feedback(pr, draw_w);
    scene_upscale(pr, r);
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) {
        bool fb = false;
        for (int k = 0; k < nfeedback; k++) fb |= feedback_tex[k] == st.rt.depth;
        for (int k = 0; k < ninplace; k++) fb |= inplace_tex[k] == st.rt.depth;
        if (fb) snapshots_mark_fb_dirty(st.rt.depth);
        else snapshots_invalidate(st.rt.depth);
    }
    if (in_pass && (draw_w & pass_dup_null)) st.rt_dirty = true;  // (writes a slot begin_pass left unbound)
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++) {
        if ((draw_w >> i) & 1) {
            bool fb = false;
            for (int k = 0; k < nfeedback; k++) fb |= feedback_tex[k] == st.rt.color[i];
            for (int k = 0; k < ninplace; k++) fb |= inplace_tex[k] == st.rt.color[i];
            if (fb) snapshots_mark_fb_dirty(st.rt.color[i]);
            else snapshots_invalidate(st.rt.color[i]);
        }
    }
    begin_pass();
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { stat_skipped++; return; }
    VkBool32 pre = st.prim_restart && native && (mode == 3 || mode == 5);
    uint8_t pre_key = !g_vk.eds2 && pre;  // (no extended dynamic state 2: in the pipeline, PipeKey.prim_restart)

    uint64_t prof_tk_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_pre += prof_tk_ - prof_tp_;
    // ---- depth / stencil and rasterizer state: set dynamically below, or (no extended dynamic state) baked into the
    // pipeline (PipeKey.fx)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("LARECOMP_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    VkBool32 dw = dt && st.ds[1];
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). LARECOMP_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("LARECOMP_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    bool sten = st.ds[3] && pass_stencil;
    uint32_t sface[2][7] = {{0}};  // per face: fail, pass, depth fail, compare, compare mask, write mask, reference
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            uint32_t v[7] = {stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                             compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF), st.stencil_vmask[f] & 0xFF, st.stencil_mask[f] & 0xFF,
                             st.stencil_ref[f]};
            memcpy(sface[f], v, sizeof v);
        }
    }
    uint8_t cull = st.poly[0];
    if (vkey.flip_x) cull = 0;  // (mirror mode's composite: one full-screen quad, either winding)
    {   // LARECOMP_VK_NO_CULL=1: no face culling at all; =<fs hash>+<fs hash>...: not for those (diagnosis: missing faces)
        static int nn = -1, except;  // (=-<list>: every draw except those)
        static uint64_t hs[256];
        if (nn < 0) {
            nn = 0;
            const char *e = getenv("LARECOMP_VK_NO_CULL");
            if (e && *e == '-') except = 1, e++;
            if (e && !strcmp(e, "1")) nn = 1000;
            else for (const char *p = e; p && *p && nn < 256;) {
                hs[nn++] = strtoull(p, (char **)&p, 16);
                while (*p == '+') p++;
            }
        }
        if (nn == 1000) cull = 0;
        else if (nn) {
            bool in = false;
            for (int i = 0; i < nn; i++) in |= g_cur_fs_hash == hs[i];
            if (in != (except != 0)) cull = 0;
        }
    }
    VkCullModeFlags cm = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // LARECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("LARECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    VkFrontFace ff = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    VkPrimitiveTopology tp = native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    PipeFx fx;
    memset(&fx, 0, sizeof fx);
    if (!g_vk.eds) {
        fx.on = 1, fx.cull = (uint8_t)cm, fx.ff = (uint8_t)ff, fx.topo = (uint8_t)tp;
        fx.dt = dt, fx.dw = (uint8_t)dw, fx.dop = (uint8_t)dop, fx.sten = sten;
        if (sten)
            for (int f = 0; f < 2; f++)
                for (int i = 0; i < 4; i++) fx.sop[f][i] = (uint8_t)sface[f][i];
    }
    // ---- pipeline
    static Pipe *last_pipeline;
    static int reuse = -1, verify;
    if (reuse < 0) {
        const char *e = getenv("LARECOMP_VK_PIPE_REUSE");
        reuse = !e || atoi(e) != 0;
        verify = getenv("LARECOMP_VK_PIPE_VERIFY") != NULL;
    }
    bool hit = reuse && !pipeline_dirty && last_pipeline &&
               last_pipeline->k.prog == pr && last_pipeline->k.topo_class == cls && last_pipeline->k.prim_restart == pre_key &&
               (g_vk.eds || !memcmp(&last_pipeline->k.fx, &fx, sizeof fx));
    Pipe *pp = hit ? last_pipeline : NULL;
    if (!hit || verify) {
        PipeKey k;
        memset(&k, 0, sizeof k);
        k.prog = pr;
        k.prim_restart = pre_key;
        k.fx = fx;
        k.ncolor = pass_ncolor;
        memcpy(k.color, pass_color, sizeof k.color);
        k.depth = pass_depth;
        k.stencil = pass_stencil;
        k.samples = pass_samples;
        for (int i = 0; i < 32; i++) {
            if (!(pr->vi->attr_mask & (1u << i))) continue;
            k.attr_mask |= 1u << i;
            uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
            uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
            if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
                k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
                k.attr_bind[i] = ZERO_BINDING;
                k.used_streams |= 1u << ZERO_BINDING;
                continue;
            }
            k.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
            k.attr_off[i] = off;
            k.attr_bind[i] = stream;
            k.used_streams |= 1u << stream;
        }
        for (int s = 0; s < 16; s++) {
            if (!(k.used_streams & (1u << s))) continue;
            k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
            k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
            if (k.divisor[s] > 1 && !g_vk.divisor) k.divisor[s] = 1;
        }
        for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
            if (!st.rt.color[i] || k.color[i] == VK_FORMAT_UNDEFINED) continue;
            // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
            // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
            k.write_mask[i] = ((pr->fs_out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
            if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
                k.blend_on |= (uint8_t)(1u << i);
                memcpy(k.blend[i], st.blend[i], 8);
            }
        }
        k.a2c = st.ms[0xC] != 0;
        k.topo_class = cls;
        k.poly_fill = st.poly[2] != 1;
        k.depth_clamp = st.depth_clamp;
#ifdef __ANDROID__
        // (consecutive draws mostly share their pipeline: a compare instead of the serial hash of the ~700-byte key)
        static PipeKey last_k;
        static uint64_t last_ph;
        uint64_t ph;
        if (last_ph && !memcmp(&k, &last_k, sizeof k)) ph = last_ph;
        else {
            ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
            if (!ph) ph = 1;
            last_k = k, last_ph = ph;
        }
#else
        uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
        if (!ph) ph = 1;
#endif
        uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
        while (pipe_tab[pslot].key && pipe_tab[pslot].key != ph) pslot = (pslot + 1) & (PIPE_TAB - 1);
        pp = pipe_tab[pslot].pipe;
        if (!pp) {
            pp = calloc(1, sizeof *pp);
            pp->k = k;
            pipe_tab[pslot].key = ph;
            pipe_tab[pslot].pipe = pp;
            run_job(build_pipeline, pp);
        }
        // Verification rebuilds the original key to detect missing invalidations during later device tests.
        if (hit && memcmp(&k, &last_pipeline->k, sizeof k)) {
            LOG("[vk] pipeline reuse: stale key detected\n");
            abort();
        }
        last_pipeline = pp;
        pipeline_dirty = false;
    }
    if (keep) wait_built(&pp->state);
    if (atomic_load(&pp->state) != 1) {
        if (g_debug_frame) LOG("[draw]   skipped: pipeline %s\n", atomic_load(&pp->state) ? "failed" : "still building");
        stat_skipped++;
        skip_note("pipeline");
        return;
    }
    ss_enabled();
    if (ss_need(SS_PIPE, ss.pipe == pp->pipe)) { ss.pipe = pp->pipe; stat_pipe_binds++; vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe); }
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic; without extended dynamic state the calls but the masks' do nothing - device.c - and
    // the pipeline has them, PipeKey.fx)
    if (ss_need(SS_DT, ss.dt == (VkBool32)dt)) { ss.dt = dt; vkCmdSetDepthTestEnable(cb, dt); }
    if (ss_need(SS_DW, ss.dw == dw)) { ss.dw = dw; vkCmdSetDepthWriteEnable(cb, dw); }
    if (ss_need(SS_DOP, ss.dop == dop)) { ss.dop = dop; vkCmdSetDepthCompareOp(cb, dop); }  // (EQUAL's tolerance: above)
    if (ss_need(SS_STEN, ss.sten == (VkBool32)sten)) { ss.sten = sten; vkCmdSetStencilTestEnable(cb, sten); }
    if (sten) {
        for (int f = 0; f < 2; f++) {
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            const uint32_t *v = sface[f];
            if (!ss_need(f ? SS_SFACE1 : SS_SFACE0, !memcmp(v, ss.sface[f], sizeof sface[f]))) continue;
            memcpy(ss.sface[f], v, sizeof sface[f]);
            vkCmdSetStencilOp(cb, face, (VkStencilOp)v[0], (VkStencilOp)v[1], (VkStencilOp)v[2], (VkCompareOp)v[3]);
            vkCmdSetStencilCompareMask(cb, face, v[4]);
            vkCmdSetStencilWriteMask(cb, face, v[5]);
            vkCmdSetStencilReference(cb, face, v[6]);
        }
    }
    if (ss_need(SS_BC, !memcmp(ss.bc, st.blend_color, sizeof ss.bc))) { memcpy(ss.bc, st.blend_color, sizeof ss.bc); vkCmdSetBlendConstants(cb, st.blend_color); }

    // ---- rasterizer (as above: computed before the pipeline)
    if (ss_need(SS_CULL, ss.cull == cm)) { ss.cull = cm; vkCmdSetCullMode(cb, cm); }
    if (ss_need(SS_FF, ss.ff == ff)) { ss.ff = ff; vkCmdSetFrontFace(cb, ff); }
    if (ss_need(SS_TOPO, ss.topo == tp)) { ss.topo = tp; vkCmdSetPrimitiveTopology(cb, tp); }
    if (g_vk.eds2 && ss_need(SS_PRE, ss.pre == pre)) { ss.pre = pre; vkCmdSetPrimitiveRestartEnable(cb, pre); }
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        {   // (diagnosis) LARECOMP_D16_BIAS_SCALE=<k>: the constant bias times k on D16 targets (the shadow maps) - a
            // driver taking D16's unit as 24-bit depth's would leave them 256 times less bias (acne along the terrain)
            static float k16 = -1;
            if (k16 < 0) k16 = getenv("LARECOMP_D16_BIAS_SCALE") ? (float)atof(getenv("LARECOMP_D16_BIAS_SCALE")) : 0;
            if (k16 > 0 && on && st.rt.depth && st.rt.depth->backend &&
                ((TexBackend *)st.rt.depth->backend)->format == VK_FORMAT_D16_UNORM) f[1] *= k16;
        }
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        if (g_vk.eds2 && ss_need(SS_DBE, ss.dbe == (VkBool32)on)) { ss.dbe = on; vkCmdSetDepthBiasEnable(cb, on); }
        if (ss_need(SS_DB, !memcmp(ss.db, f, sizeof f))) { memcpy(ss.db, f, sizeof f); vkCmdSetDepthBias(cb, f[1], f[2], f[0]); }
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    float tint[8] = {0};  // per-draw tint (ShaderInfo.uses_tint; strength 0: untouched): no game feature sets one yet
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (prof) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROF_T0();
#ifdef __ANDROID__
    if (gcopy_mode && (pr->vi->uses_gmem_map || pr->fi->uses_gmem_map))  // (the table both stages' sets use)
        gcopy_prepare((pr->vi->uses_gmem_map ? 1u : 0u) | (pr->fi->uses_gmem_map ? 2u : 0u),
                      pr->vi->uses_gmem_store || pr->fi->uses_gmem_store);
    else if (gwin_mode && (pr->vi->uses_gmem_map || pr->fi->uses_gmem_map)) {
        static int snap = -1;  // (LARECOMP_NO_GSNAP=1: through the window as before)
        if (snap < 0) snap = getenv("LARECOMP_NO_GSNAP") == NULL;
        if (!(snap && pr->vi->uses_gmem_map && !pr->fi->uses_gmem_map && !pr->vi->uses_gmem_store && gsnap_prepare()))
            gwin_prepare((pr->vi->uses_gmem_map ? 1u : 0u) | (pr->fi->uses_gmem_map ? 2u : 0u));
    }
#endif
    uint32_t dyn[64], ndv = 0, ndf = 0;  // (the sets' dynamic uniform buffer offsets: vertex, then fragment)
    memset(cbw_patch, 0, sizeof cbw_patch);  // (cbwin_bind fills them while the sets are written)
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL, dyn, &ndv);
    PROF_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
    }
    up_fix.sx = 0;
    {   // a scaled scene's half-size buffers brought back by nearest depth: the texel grid and step scaled (up_fix)
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_UPSAMPLE_FIX") != NULL;
        bool scaled = !off && st.rt.ncolor && st.rt.color[0] && (pass_sx > 1.001f || pass_sy > 1.001f);
        if (scaled && !pr->fs_gather) {  // (the first gathered texture's slot, from the translated source: 2 + slot)
            const char *p = pr->fi->msl ? strstr(pr->fi->msl, "textureGather(t") : NULL;
            int slot = p ? atoi(p + 15) : -1;
            pr->fs_gather = slot >= 0 && slot < pr->fi->ntex && slot < 64 ? (uint8_t)(2 + slot) : 1;
        }
        int gs = scaled ? pr->fs_gather - 2 : -1;
        NvnTexture *gt = gs >= 0 && fabsf(tex_sc[1][gs][0] - pass_sx) < 0.01f * pass_sx &&
                         fabsf(tex_sc[1][gs][1] - pass_sy) < 0.01f * pass_sy ? shader_texture_object(1, &pr->fi->tex[gs]) : NULL;
        TexBackend *gb = gt ? gt->backend : NULL;
        if (gb && gt->width && gt->height) {
            up_fix.gw = st.rt.color[0]->width, up_fix.gh = st.rt.color[0]->height;
            up_fix.sx = (float)gb->width / (float)gt->width, up_fix.sy = (float)gb->height / (float)gt->height;
            up_fix.tw = gt->width, up_fix.th = gt->height, up_fix.aw = gb->width, up_fix.ah = gb->height;
            ndv = 0;  // (the vertex set again, with its constants patched)
            memset(cbw_patch, 0, sizeof cbw_patch);
            vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL, dyn, &ndv);
        }
    }
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {   // camera-attached effects sized for 16:9 are widened with the view (runtime/hle/widescreen.c)
        extern float widescreen_effect_stretch(uint64_t fs_hash);
        float k = widescreen_effect_stretch(g_cur_fs_hash);
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        if (k > 1.0f) { vx -= vw * (k - 1) / 2; vw *= k; }
        set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (stretch %.3f, fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, k, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = prof ? SDL_GetTicksNS() : 0;
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii, dyn + ndv, &ndf);
    up_fix.sx = 0;
    if (prof) prof_sets += SDL_GetTicksNS() - prof_t1_;
    VkDescriptorSet sets[2] = {vset, fset};
    // (the same sets and offsets as the previous draw - the set cache, stage_set: not bound again)
    uint32_t nd = ndv + ndf;
    if (ss_need(SS_SETS, ss.layout == pr->layout && ss.sets[0] == vset && ss.sets[1] == fset && ss.ndyn == nd &&
                       !memcmp(ss.dyn, dyn, nd * sizeof dyn[0]))) {
        ss.layout = pr->layout, ss.sets[0] = vset, ss.sets[1] = fset;
        ss.ndyn = nd;
        memcpy(ss.dyn, dyn, nd * sizeof dyn[0]);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, nd, dyn);
        stat_set_binds++;
    }
    if (pr->vi->uses_gmem_store) vc_mark_sbufs(0), note_gmem_store(0);
    if (pr->fi->uses_gmem_store) vc_mark_sbufs(1), note_gmem_store(1);

    uint64_t prof_t2_ = prof ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && guest_direct(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
#ifdef __ANDROID__
                // (no primitive restart - most draws: a plain maximum, which the compiler vectorizes; the scalar
                // loop with the restart test was ~9% of the render thread in the copy mode, every indexed draw)
                if (!st.prim_restart) {
                    uint32_t m = 0;
                    if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) m = s[i] > m ? s[i] : m; }
                    else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) m = s[i] > m ? s[i] : m; }
                    if (m > max_index) max_index = m;
                } else
#endif
                if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != (uint16_t)restart) max_index = s[i]; }
                else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != restart) max_index = s[i]; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index && ss_need(SS_IB, ss.ib == ib.buf && ss.io == ib.off && ss.it == itype_vk)) {
        ss.ib = ib.buf; ss.io = ib.off; ss.it = itype_vk;
        vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
    }

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(pp->k.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            // A draw that reads past the stream's bound size (BOTW's lens streaks: a 4-vertex strip over 16-byte
            // streams, one vertex's worth, the corners from the vertex index) reads zeros there on the Switch; bound
            // directly, the heap past the stream (the next buffers' data) bent the quad into a hard-edged band.
            uint64_t reach = 0;
            if (pp->k.stride[s] && !use_index)
                reach = pp->k.divisor[s] ? ((uint64_t)base_inst + inst + pp->k.divisor[s] - 1) / pp->k.divisor[s] * pp->k.stride[s]
                                     : ((uint64_t)first + count) * pp->k.stride[s];
            if (v.addr && v.size && reach > v.size && reach <= (1u << 20)) {
                sp = vk_transient(reach, 16);
                memcpy(sp.ptr, (const void *)(uintptr_t)v.addr, v.size);
                memset((uint8_t *)sp.ptr + v.size, 0, reach - v.size);
                VkDeviceSize zoff = sp.off;
                if (!(ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == sp.buf && ss.vo[s] == zoff)) {
                    ss.vb[s] = sp.buf; ss.vo[s] = zoff; ss.vb_known |= 1u << s;
                    vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &zoff);
                }
                continue;
            }
            if (v.addr && v.size && guest_direct(v.addr, v.size, &dbuf, &doff)) {
                if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == dbuf && ss.vo[s] == doff) continue;
                ss.vb[s] = dbuf; ss.vo[s] = doff; ss.vb_known |= 1u << s;
                vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &dbuf, &doff);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                if (scan_isz == 2) { const uint16_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != (uint16_t)scan_restart) max_index = q[i]; }
                else { const uint32_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != scan_restart) max_index = q[i]; }
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = pp->k.stride[s], need;
            if (!stride) need = 256;
            else if (pp->k.divisor[s]) need = ((uint64_t)base_inst + inst + pp->k.divisor[s] - 1) / pp->k.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else sp = guest_copy(v.addr, need ? need : 16);
        }
        VkDeviceSize off = sp.off;
        if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == sp.buf && ss.vo[s] == off) continue;
        ss.vb[s] = sp.buf; ss.vo[s] = off; ss.vb_known |= 1u << s;
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
    }

    if (prof) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    if (!use_index) vkCmdDraw(cb, count, inst, first, base_inst);
    else vkCmdDrawIndexed(cb, icount, inst, 0, base_vertex, base_inst);
    if (g_debug_frame) pick_after_draw();
    pass_written |= draw_w;
    stat_draws++;
    cum_draws++;
    if (g_gpuprof_frame && gp_n) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
#ifdef __ANDROID__
        static int per_draw = -1;
        if (per_draw < 0) per_draw = getenv("LARECOMP_GPUPROF_DRAWS") != NULL;
        if (per_draw)
            gp_mark(cb, "draw vs %llx fs %llx n %u inst %u%s", (unsigned long long)pr->code[0], (unsigned long long)pr->code[1],
                    use_index ? icount : count, inst, use_index ? " idx" : "");
#endif
    }
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) LOG(" %ux%u/f%#x@%llx(%p)", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format,
                                (unsigned long long)st.rt.color[i]->addr, (void *)st.rt.color[i]);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x\n", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    if (st.color_state[0] & 1)  // (target 0's blend: srcC dstC srcA dstA eqC eqA, as BlendState stores them)
        LOG("[draw]   blend0 %u %u %u %u eq %u %u color %.3f %.3f %.3f %.3f\n", st.blend[0][1], st.blend[0][2],
            st.blend[0][3], st.blend[0][4], st.blend[0][5], st.blend[0][6], st.blend_color[0], st.blend_color[1],
            st.blend_color[2], st.blend_color[3]);
    if (getenv("LARECOMP_DEBUG_TEXSTATE"))  // vertex attributes: guest format, stream, offset, stride
        for (int i = 0; i < 16; i++)
            if (pr->vi->attr_mask & (1u << i)) {
                uint32_t a = i < st.nattrib ? st.attrib[i] : 0;
                uint32_t sn = (a >> 8) & 0x1F;
                LOG("[draw]   attr%d: fmt %#x stream %u off %u stride %u divisor %u buffer %llx size %llu\n", i, a & 0xFF, sn, a >> 13,
                    sn < (uint32_t)st.nstream ? st.stream[sn][0] : 0, sn < (uint32_t)st.nstream ? st.stream[sn][1] : 0,
                    sn < 16 ? (unsigned long long)st.vbuf[sn].addr : 0ull, sn < 16 ? (unsigned long long)st.vbuf[sn].size : 0ull);
            }
    if (getenv("LARECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats (=2: vertex cbufs whole)
        int full = atoi(getenv("LARECOMP_DEBUG_DATA")) >= 2;
        LOG("[draw]   depth range %.9g %.9g offset enables %#x factor %.9g units %.9g clamp %.9g\n",
            st.depth_range[0][0], st.depth_range[0][1], st.poly[3], st.poly_offset[0], st.poly_offset[1], st.poly_offset[2]);
        for (int c = 0; c < 18; c++) {
            if (!((pr->vi->cbuf_mask >> c) & 1)) continue;
            Range r = c >= 3 ? st.ubuf[0][c - 3] : (Range){0, 0};
            if (c == 1) r = (Range){st.program->stages[0].code_addr + st.program->stages[0].const_offset,
                                  st.program->stages[0].const_size};
            if (!r.addr) continue;
            const float *u = (const float *)(uintptr_t)r.addr;
            LOG("[draw]   vcb%d %llx size %u:", c, (unsigned long long)r.addr, (unsigned)r.size);
            for (int k = 0; k < (full ? 16384 : 160) && k * 4 < (int)r.size; k++) LOG(" %.9g", u[k]);
            LOG("\n");
        }
        for (int i = 0; full && i < 16 && i < st.nattrib; i++) {  // integer attributes (bone indices): their range
            uint32_t a = st.attrib[i], f = a & 0xFF, sn = (a >> 8) & 0x1F, n = f == 0x27 ? 4 : f == 0x0F ? 2 : f == 0x03 ? 1 : 0;
            if (!(pr->vi->attr_mask & (1u << i)) || sn >= 16 || sn >= (uint32_t)st.nstream || !st.vbuf[sn].addr) continue;
            uint64_t stride = st.stream[sn][0], nv = stride ? st.vbuf[sn].size / stride : 0;
            const uint8_t *p = (const uint8_t *)(uintptr_t)st.vbuf[sn].addr + (a >> 13);
            if (f == 0x25 || f == 0x0D) {  // (weights: vertices whose weights add up to nothing)
                uint32_t w = f == 0x25 ? 4 : 2;
                uint64_t z = 0, off3 = 0, off4 = 0;
                for (uint64_t v = 0; v < nv; v++) {
                    unsigned s = 0, s3 = 0;
                    for (uint32_t k = 0; k < w; k++) s += p[v * stride + k], s3 += k < 3 ? p[v * stride + k] : 0;
                    z += s == 0;
                    off4 += s < 252 || s > 258;
                    if (w != 4 || (s3 >= 252 && s3 <= 258)) continue;
                    if (off3++ < 4) {  // (the vertex's whole record: its indices beside)
                        LOG("[draw]   attr%d vertex %llu:", i, (unsigned long long)v);
                        for (uint64_t k = 0; k < stride && k < 16; k++) LOG(" %u", ((const uint8_t *)(uintptr_t)st.vbuf[sn].addr)[v * stride + k]);
                        LOG("\n");
                    }
                }
                LOG("[draw]   attr%d unorm: %llu vertices, %llu with all-zero weights, %llu whose first 3 don't add to 1, %llu all 4\n",
                    i, (unsigned long long)nv, (unsigned long long)z, (unsigned long long)off3, (unsigned long long)off4);
                continue;
            }
            if (!n) continue;
            unsigned mx = 0, hist[256] = {0};
            for (uint64_t v = 0; v < nv; v++)
                for (uint32_t k = 0; k < n; k++) { unsigned b = p[v * stride + k]; hist[b]++; if (b > mx) mx = b; }
            LOG("[draw]   attr%d ints: %llu vertices, max %u, values used:", i, (unsigned long long)nv, mx);
            for (unsigned b = 0; b < 256; b++) if (hist[b]) LOG(" %u", b);
            LOG("\n");
        }
        for (int s = 0; s < 8; s++) {  // (2 to 7: per-instance streams - a particle's own values)
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16 && (s < 2 || (uint64_t)k * 4 < st.vbuf[s].size); k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int k = 0; k < 4; k++)  // the vertex stage's storage buffers (LDG / STG through driver cb0 0x110..)
            if (st.sbuf[0][k].addr) {
                const uint32_t *u = (const uint32_t *)(uintptr_t)st.sbuf[0][k].addr;
                LOG("[draw]   vsbuf%d %llx size %llu:", k, (unsigned long long)st.sbuf[0][k].addr, (unsigned long long)st.sbuf[0][k].size);
                for (int j = 0; j < 32 && j * 4 < (int)st.sbuf[0][k].size; j++) LOG(" %08x", u[j]);
                LOG("\n");
            }
        for (int c = 0; c < 18; c++) {
            if (!((pr->fi->cbuf_mask >> c) & 1)) continue;
            Range r = c >= 3 ? st.ubuf[1][c - 3] : (Range){0, 0};  // (hardware bank c: ubuf[c - 3]; 1: the shader's own)
            if (c == 1) r = (Range){st.program->stages[1].code_addr + st.program->stages[1].const_offset,
                                  st.program->stages[1].const_size};
            if (!r.addr) continue;
            const float *u = (const float *)(uintptr_t)r.addr;
            LOG("[draw]   fcb%d %llx size %u:", c, (unsigned long long)r.addr, (unsigned)r.size);
            for (int k = 0; k < 96 && k * 4 < (int)r.size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->vi->ntex && i < 64 && getenv("LARECOMP_DEBUG_TEXSTATE"); i++) {  // vertex textures
        NvnTexture *t = shader_texture_object(0, &pr->vi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   vtex%d: none\n", i); continue; }
        uint64_t n = t->layout.fmt && t->addr ? tegra_total_size(&t->layout) : 0, nz = 0;
        for (uint64_t k = 0; k < n; k += 64) nz += ((const uint8_t *)(uintptr_t)t->addr)[k] != 0;
        LOG("[draw]   vtex%d: %p %ux%ux%u f%#x target %u flags %#x addr %llx: image %d gpu_owned %d uploaded %d dynamic %d, guest nonzero %llu/%llu\n",
            i, (void *)t, t->width, t->height, t->depth, t->format, t->target, t->flags, (unsigned long long)t->addr, b != NULL,
            b ? b->gpu_owned : 0, b ? b->uploaded : 0, b ? b->dynamic : 0, (unsigned long long)nz, (unsigned long long)(n + 63) / 64);
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) {
            const ShaderTexture *stx = &pr->fi->tex[i];
            int slot = ((int)stx->handle_offset - 0x20) / 8;
            uint64_t h = shader_texture_handle(1, stx);
            LOG("[draw]   tex%d: none (handle offset %#x slot %d bindless %d c%u[%#x] (ubuf %llx size %llu) handle %llx, pool %d entries, type %d)\n",
                i, stx->handle_offset, slot, stx->bindless, stx->bindless_cbuf, stx->bindless_offset,
                stx->bindless_cbuf >= 3 && stx->bindless_cbuf < 21 ? (unsigned long long)st.ubuf[1][stx->bindless_cbuf - 3].addr : 0ull,
                stx->bindless_cbuf >= 3 && stx->bindless_cbuf < 21 ? (unsigned long long)st.ubuf[1][stx->bindless_cbuf - 3].size : 0ull, (unsigned long long)h,
                st.tpool ? st.tpool->count : -1, stx->type);
            continue;
        }
        LOG("[draw]   tex%d: %p %ux%u f%#x @%llx lv %u -> %ux%u scale %.4f %.4f%s%s\n", i, (void *)t, t->width, t->height, t->format,
            (unsigned long long)t->addr, b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0,
            b && b->scalable ? " scalable" : "", b && b->presented ? " presented" : "");
        {   // LARECOMP_DUMP_SAMPLED=<w>x<h>:<hex nvn format>: that sampled texture's every layer, level 0, raw (once a
            // debug frame, after the current pass: build/sampled_<w>x<h>_L<layer>.raw)
            // (<w>x<h>:<format>@<hex address>: only the one at that address)
            static int dw = -1, dh, df;
            static unsigned long long da;
            if (dw < 0) { dw = 0; const char *e = getenv("LARECOMP_DUMP_SAMPLED"); if (e) sscanf(e, "%dx%d:%x@%llx", &dw, &dh, &df, &da); }
            extern TexBackend *g_dump_sampled;
            static uint64_t dumped_frame = ~0ull;
            if (dw && b && (int)t->width == dw && (int)t->height == dh && (int)t->format == df && (!da || t->addr == da) &&
                dumped_frame != renderer_present_count()) {
                dumped_frame = renderer_present_count();
                g_dump_sampled = b;
            }
        }
        if (getenv("LARECOMP_DEBUG_TEXSTATE") && !b) {
            uint64_t n = t->layout.fmt && t->addr ? tegra_total_size(&t->layout) : 0, nz = 0;
            for (uint64_t k = 0; k < n; k += 64) nz += ((const uint8_t *)(uintptr_t)t->addr)[k] != 0;
            LOG("[draw]     no image yet: stride %llu flags %#x addr %llx levels %u layout bh %u, guest bytes nonzero %llu/%llu\n",
                (unsigned long long)t->stride, t->flags, (unsigned long long)t->addr, t->levels, t->layout.block_height_log2, (unsigned long long)nz, (unsigned long long)(n + 63) / 64);
        }
        if (getenv("LARECOMP_DEBUG_TEXSTATE") && b && t->addr) {  // where its texels come from: the CPU's memory or GPU work
            uint64_t n = t->layout.fmt ? tegra_total_size(&t->layout) : 0, nz = 0;
            for (uint64_t k = 0; k < n; k += 64) nz += ((const uint8_t *)(uintptr_t)t->addr)[k] != 0;
            LOG("[draw]     flags %#x addr %llx size %llu: gpu_owned %d uploaded %d dynamic %d, guest bytes nonzero %llu/%llu\n",
                t->flags, (unsigned long long)t->addr, (unsigned long long)n, b->gpu_owned, b->uploaded, b->dynamic,
                (unsigned long long)nz, (unsigned long long)(n + 63) / 64);
            const NvnTexEntry *te = shader_texture_entry(1, &pr->fi->tex[i]);
            LOG("[draw]     target %u depth %u layers %u swizzle %u %u %u %u | view %d setmask %#x swizzle %u %u %u %u levels %u+%u "
                "layers %u+%u format %#x target %u ds %u | shader type %d\n",
                t->target, t->depth, b->layers, t->swizzle[0], t->swizzle[1], t->swizzle[2], t->swizzle[3], te ? te->has_view : -1,
                te && te->has_view ? te->view.setmask : 0, te ? te->view.swizzle[0] : 0, te ? te->view.swizzle[1] : 0,
                te ? te->view.swizzle[2] : 0, te ? te->view.swizzle[3] : 0, te ? te->view.base_level : 0, te ? te->view.num_levels : 0,
                te ? te->view.min_layer : 0, te ? te->view.num_layers : 0, te ? te->view.format : 0, te ? te->view.target : 0,
                te ? te->view.ds_mode : 0, pr->fi->tex[i].type);
            uint64_t sh = shader_texture_handle(1, &pr->fi->tex[i]);
            uint32_t tsc = (uint32_t)(sh >> 20) & 0xFFF;
            if (st.spool && tsc < (uint32_t)st.spool->count) {
                const NvnSamplerDesc *sd = &((const NvnSamplerDesc *)st.spool->entries)[tsc];
                LOG("[draw]     sampler %u (handle %llx): min %u mag %u wrap %u %u lod %.3f..%.3f bias %.3f aniso %.1f compare %u/%u border %g %g %g %g\n", tsc,
                    (unsigned long long)sh, sd->min_filter,
                    sd->mag_filter, sd->wrap[0], sd->wrap[1], sd->lod_min, sd->lod_max, sd->lod_bias, sd->max_aniso, sd->compare_mode, sd->compare_func,
                    sd->border[0], sd->border[1], sd->border[2], sd->border[3]);
            }
        }
    }
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    {   extern uint64_t nvn_watch_texaddr(void);
        uint64_t wa = nvn_watch_texaddr();
        if (wa && t && t->addr <= wa && wa < t->addr + tegra_total_size(&t->layout))
            LOG("[watch] frame %llu: buffer %llx -> texture %ux%ux%u f%#x region %d %d %d %d %d %d\n", (unsigned long long)renderer_present_count(),
                (unsigned long long)src, t->width, t->height, t->depth, t->format, dst->region[0], dst->region[1], dst->region[2],
                dst->region[3], dst->region[4], dst->region[5]);
    }
    if (!t || !src) return;
    alias_before_write(t, 0, "upload");
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    {   static int nlog;  // LARECOMP_LOG_READBACK=1: the game's texture readbacks (the save thumbnail is one)
        if (getenv("LARECOMP_LOG_READBACK") && nlog++ < 20) LOG("[vk] texture -> buffer %p %ux%u f%#x flags %#x -> %llx\n", (void *)t, t ? t->width : 0, t ? t->height : 0,
                             t ? t->format : 0, t ? t->flags : 0, (unsigned long long)dst);
    }
    if (!t || !dst || dst < HEAP_BASE) return;
    t = alias_read(t);
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    wait_why = "texture -> buffer readback";
    renderer_flush();  // the CPU may read the result right after the fence
    vk_readback_ready(total);
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    {   static int nlog;
        if (getenv("LARECOMP_LOG_READBACK") && nlog++ < 20) {
            uint64_t nz = 0;
            for (uint64_t k = 0; k < total; k += 4) nz += ((const uint32_t *)out.ptr)[k / 4] != 0;
            LOG("[vk]   read back %llu bytes (%llu words nonzero), gpu_owned %d, aliased %d, level %d region %d %d %d %d %d %d\n",
                (unsigned long long)total, (unsigned long long)nz, b->gpu_owned, t != (NvnTexture *)(uintptr_t)src->tex, level,
                rg[0], rg[1], rg[2], rg[3], rg[4], rg[5]);
        }
    }
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    {   extern uint64_t nvn_watch_texaddr(void);
        NvnTexture *ws = (NvnTexture *)(uintptr_t)src->tex, *wd = (NvnTexture *)(uintptr_t)dst->tex;
        uint64_t wa = nvn_watch_texaddr();
        bool in_s = ws && ws->addr <= wa && wa < ws->addr + tegra_total_size(&ws->layout);
        bool in_d = wd && wd->addr <= wa && wa < wd->addr + tegra_total_size(&wd->layout);
        if (wa && (in_s || in_d))
            LOG("[watch] frame %llu: copy %ux%u f%#x @%llx -> %ux%u f%#x @%llx\n", (unsigned long long)renderer_present_count(),
                ws ? ws->width : 0, ws ? ws->height : 0, ws ? ws->format : 0, ws ? (unsigned long long)ws->addr : 0ull,
                wd ? wd->width : 0, wd ? wd->height : 0, wd ? wd->format : 0, wd ? (unsigned long long)wd->addr : 0ull);
    }
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s->width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s->height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d->width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame || ct_on())
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    if (copy_opt() && sx0 == 0 && sy0 == 0 && dx0 == 0 && dy0 == 0 && sw == (int)s->width && sh == (int)s->height && dw == (int)d->width &&
        dh == (int)d->height && slv == 0 && dlv == 0 && sly == 0 && dly == 0 && alias_make(st_, dt, s, d)) {
        if (ct_on()) LOG("[ct] alias made %p -> %p\n", (void *)st_, (void *)dt);
        return;
    }
    alias_before_write(dt, 0, "copy dst");
    if (naliases) s = alias_read(st_)->backend;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline_a(VkFormat fmt, bool alpha) {  // alpha: the texture's own (debug pass dumps)
    static struct { VkFormat f; bool a; VkPipeline p; } cache[6];
    for (int i = 0; i < 6; i++) if (cache[i].p && cache[i].f == fmt && cache[i].a == alpha) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(alpha ? "present_a.frag" : "present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    {   // (no dynamic rendering: against a compatible render pass, rpcompat.c)
        VkRenderPass crp = vk_rp_compat(1, &fmt, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, ms.rasterizationSamples);
        if (crp) pi.pNext = ri.pNext, pi.renderPass = crp;
    }
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 6; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].a = alpha; cache[i].p = p; break; }
    return p;
}
static VkPipeline present_pipeline(VkFormat fmt) { return present_pipeline_a(fmt, false); }

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
uint32_t vk_dump_depth_layer = ~0u;
static VkImageView raw_view(TexBackend *b) {
    if (vk_dump_depth_layer != ~0u && (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        static const VkComponentMapping id0 = {0};
        return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, vk_dump_depth_layer, 1, id0, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

static VkPipeline post_pipeline(const char *fs, VkFormat fmt) {
    static struct { const char *fs; VkFormat f; VkPipeline p; } cache[16];
    for (int i = 0; i < 16; i++) if (cache[i].p && cache[i].fs == fs && cache[i].f == fmt) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    {   // (no dynamic rendering: against a compatible render pass, rpcompat.c)
        VkRenderPass crp = vk_rp_compat(1, &fmt, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, ms.rasterizationSamples);
        if (crp) pi.pNext = ri.pNext, pi.renderPass = crp;
    }
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 16; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h in format f (kept until the size or format changes). 0, 1: SMAA edges / weights, 2: SMAA
// output, 3, 4: FSR EASU / RCAS (or SGSR) of the presented image; 5, 6: the same of the 3D scene (scene_upscale).
enum { POST_IMAGES = 7 };
static TexBackend *post_image(int i, uint32_t w, uint32_t h, VkFormat f) {
    static TexBackend img[POST_IMAGES];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h || d->format != f)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        // (the scene's EASU output, A2B10G10R10: compressed (UBWC) on Turnip, RCAS's texelFetch of it read 16x4 blocks
        // of zeros - black dashes in the grass, about one a frame on the Adreno 830; uncompressed with a mutable
        // format, as SGSR's single pass and an RGBA8 intermediate showed none either)
        bool plain = i == 5 && f == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .flags = plain ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
                                .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    VkPipeline pipe = post_pipeline(fs, dst->format);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dst->width, dst->height}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vk_begin_rendering(cb, &ri, &dst->format, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {dst->width, dst->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    ss.known &= ~SS_SETS;  // (another layout: the draws' sets are bound again)
    vkCmdDraw(cb, 3, 1, 0, 0);
    vk_end_rendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    VkImageView v = raw_view(b);
    bool smaa = g_settings.antialias == AA_SMAA, fxaa = g_settings.antialias == AA_FXAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool fsr = (g_settings.upscaler == UPSCALE_FSR1 || g_settings.upscaler == UPSCALE_SGSR1) &&
               (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only)
    // The final image (scene already upscaled, and the HUD) below the window - Output resolution 1080p / Auto on a
    // 1440-line screen: the present's own bilinear sampling, whichever upscaler is chosen. FSR 1's two passes ran here
    // again over the whole window (~3 ms of the Adreno 830's frame at 3200x1440), and SGSR 1's one took 1.9 ms against
    // bilinear's 0.14, with no difference to see in the HUD or the scene at a scale of 1.33 (POCO F7 Ultra, 2026-10-26):
    // the scene had its own upscale already. LARECOMP_PRESENT_UPSCALE=sgsr: SGSR 1, =setting: the chosen one (A/B).
    static int pmode = -1;
    if (pmode < 0) {
        const char *e = getenv("LARECOMP_PRESENT_UPSCALE");
        pmode = !e ? UPSCALE_BILINEAR : !strcmp(e, "sgsr") ? UPSCALE_SGSR1 : !strcmp(e, "setting") ? 99 : UPSCALE_BILINEAR;
    }
    int up = pmode == 99 ? g_settings.upscaler : pmode;
    if (pmode == UPSCALE_BILINEAR) fsr = false;
    if (!smaa && !fxaa && !fsr) return v;
    gp_mark(cb, "present: %s%s %ux%u -> %ux%u", smaa ? "smaa " : fxaa ? "fxaa " : "", !fsr ? "" : up == UPSCALE_SGSR1 ? "sgsr1" : "easu+rcas",
            b->width, b->height, ow, oh);
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height, VK_FORMAT_R8G8B8A8_UNORM), *weights = post_image(1, b->width, b->height, VK_FORMAT_R8G8B8A8_UNORM);
        TexBackend *out = post_image(2, b->width, b->height, VK_FORMAT_R8G8B8A8_UNORM);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    } else if (fxaa) {  // FXAA (shaders.c fxaa.frag): one pass
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *out = post_image(2, b->width, b->height, VK_FORMAT_R8G8B8A8_UNORM);
        VkImageView in[3] = {v};
        post_pass(cb, "fxaa.frag", in, 1, out, m, false);
        v = post_view(out);
    }
    if (fsr && up == UPSCALE_SGSR1) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *o = post_image(3, ow, oh, VK_FORMAT_R8G8B8A8_UNORM);
        float ps[8] = {1.0f / iw, 1.0f / ih, iw, ih, (float)ow, (float)oh};
        VkImageView si[3] = {v};
        post_pass(cb, "sgsr1.frag", si, 1, o, ps, false);
        v = post_view(o);
    } else if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh, VK_FORMAT_R8G8B8A8_UNORM), *r = post_image(4, ow, oh, VK_FORMAT_R8G8B8A8_UNORM);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    vk_full_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || g_settings.antialias != psm || fsr != pfs)
            LOG("[post] %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : fxaa ? " fxaa" : "",
                !fsr ? "" : up == UPSCALE_SGSR1 ? " sgsr1" : " fsr1", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = g_settings.antialias; pfs = fsr;
    }
    return v;
}

// ---- the upscaler at the scene -> final image composite (Video > Upscaler: AMD FSR 1, Snapdragon GSR 1; SavageWind-
// Recomp's). The 3D scene's targets are at the internal resolution (textures.c RS_SCENE), the final image at the
// screen's (RS_OUT), and the game's composite draw would scale the scene up bilinearly. Before that draw, EASU + RCAS
// (or SGSR 1) bring the scene to its size in the final image, in the scene's own format (10-bit: no banding); the draw
// then samples that at 1:1 (texture_info). LARECOMP_SCENE_UPSCALE=0: the game's own bilinear composite (A/B).
static void scene_upscale(Prog *pr, CmdRec *r) {
    scene_up.t = NULL;
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_SCENE_UPSCALE") ? atoi(getenv("LARECOMP_SCENE_UPSCALE")) : 1;
    int mode = g_settings.upscaler;
    extern int vk_ab(const char *what);
    if (AB("sgsr") == 1) mode = UPSCALE_SGSR1;
    if (!on || (mode != UPSCALE_FSR1 && mode != UPSCALE_SGSR1)) return;
    NvnTexture *rt = st.rt.ncolor ? st.rt.color[0] : NULL;
    if (!rt || r->x[2] > 6 || !pr->fi->ntex || vk_texture_scale_group(rt) != RS_OUT) return;
    const float *vp = st.viewport[0];
    if (vp[2] < rt->width * 0.5f || vp[3] < rt->height * 0.5f) return;  // (a composite fills most of the image)
    float osx, osy;
    renderer_texture_scale(rt, &osx, &osy);
    uint32_t ow = (uint32_t)lroundf(vp[2] * osx), oh = (uint32_t)lroundf(vp[3] * osy);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        // the scene: a full-size scene target (not one of its reduced steps: bloom, blur)
        if (!t || t->target != 1 || vk_texture_scale_group(t) != RS_SCENE || t->width * 2 < vp[2] || t->height * 2 < vp[3]) continue;
        TexBackend *b = vk_tex(t, false);
        if (!b->image || b->layers != 1 || b->samples != 1 || b->depth != 1 || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) continue;
        if (ow <= b->width + 1 && oh <= b->height + 1) return;  // (at its size in the final image already)
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, b->format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) continue;
        vk_end_pass();
        VkCommandBuffer cb = vk_cmd();
        float iw = (float)b->width, ih = (float)b->height;
        static const VkComponentMapping id = {0};
        VkImageView in[3] = {vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT)};
        TexBackend *o = post_image(6, ow, oh, b->format);
        gp_mark(cb, "scene upscale %ux%u -> %ux%u: %s", b->width, b->height, ow, oh, mode == UPSCALE_SGSR1 ? "sgsr1" : "easu");
        if (mode == UPSCALE_SGSR1) {
            float ps[8] = {1.0f / iw, 1.0f / ih, iw, ih, (float)ow, (float)oh};
            post_pass(cb, "sgsr1.frag", in, 1, o, ps, false);
        } else {
            TexBackend *e = post_image(5, ow, oh, b->format);
            float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
            post_pass(cb, "easu.frag", in, 1, e, pe, false);
            gp_mark(cb, "scene upscale: rcas");
            float pr_[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
            VkImageView ri[3] = {post_view(e)};
            post_pass(cb, "rcas.frag", ri, 1, o, pr_, false);
        }
        vk_full_barrier(cb);
        scene_up.t = t;
        scene_up.img = o;
        scene_up.view = post_view(o);
        scene_up.sx = (float)ow / (float)t->width;
        scene_up.sy = (float)oh / (float)t->height;
        static uint32_t lw, lh, low, loh;
        static int lmode = -1;
        if (b->width != lw || b->height != lh || ow != low || oh != loh || mode != lmode)
            LOG("[post] scene %ux%u -> %s %ux%u (format %d)\n", b->width, b->height, mode == UPSCALE_SGSR1 ? "sgsr1" : "fsr1",
                ow, oh, b->format);
        lw = b->width; lh = b->height; low = ow; loh = oh; lmode = mode;
        return;
    }
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkImageView src = post_process(cb, b, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh);
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    ss.known &= ~SS_SETS;  // (another layout: the draws' sets are bound again)
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
// (debug frame, LARECOMP_DEBUG_RAW_PASSES=1) a color target's texels as they are, level 0 / layer 0: <path>.raw, its
// Vulkan format and size in the log - the PNGs clip HDR values at 1
static void save_texture_raw(TexBackend *b, const char *path) {
    uint32_t bpp = 0;
    switch (b->format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: bpp = 16; break;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: bpp = 4; break;
    case VK_FORMAT_R16_SFLOAT: bpp = 2; break;
    default: return;
    }
    if (!(b->aspect & VK_IMAGE_ASPECT_COLOR_BIT) || b->samples > 1) return;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t size = (uint64_t)b->width * b->height * bpp;
    VkSpan out = vk_readback(size);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_readback_ready(size);
    char rp[300];
    snprintf(rp, sizeof rp, "%s.raw", path);
    FILE *f = fopen(rp, "wb");
    if (f) { fwrite(out.ptr, 1, size, f); fclose(f); }
    LOG("[vk] raw %s: %ux%u vkformat %d\n", rp, b->width, b->height, (int)b->format);
}

void renderer_save_texture_tex(TexBackend *b, const char *path) {
    extern uint32_t vk_dump_depth_layer;
    if (!b || !b->image || ((b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) && vk_dump_depth_layer == ~0u)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    const VkFormat rgba8 = VK_FORMAT_R8G8B8A8_UNORM;
    vk_begin_rendering(cb, &ri, &rgba8, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
    VkPipeline p = present_pipeline_a(VK_FORMAT_R8G8B8A8_UNORM, strstr(path, "pass_") != NULL);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[8] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    ss.known &= ~SS_SETS;  // (another layout: the draws' sets are bound again)
    vkCmdDraw(cb, 3, 1, 0, 0);
    vk_end_rendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_readback_ready((uint64_t)w * h * 4);
    vk_save_png(path, out.ptr, w, h);
    vk_rp_forget_view(tv);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// ================================================================== executor (renderer.m exec_list)
// LARECOMP_VK_SPLIT_US=<us> (Android, async; 3000 on Turnip): a pass translating that long without a submission is
// ended and submitted after a draw, so the GPU starts on it while the rest is translated (the next draw reopens it,
// loading its targets). BOTW's G-buffer is one pass of ~3250 draws, ~10 ms of translation the GPU waited for.
// LARECOMP_AB=<frames>:<switch> (a measurement): the switch alternates every <frames> presented frames - on (1) in
// the odd stretches - so both sides run in the same heat (back-to-back runs on a warm phone differed by more than the
// change); LARECOMP_LOG_PACE tags each frame "ab 0/1". Switches: split (no long-pass split), ubo (constant buffers
// read in place), sgsr (Snapdragon GSR 1 for the scene instead of the setting's upscaler), nobarrier (no barrier at
// the start of a pass: unsafe, what they cost), copy (feedback copies instead of in-place reads), chunk (submissions
// every 0.5 ms of translation instead of LARECOMP_VK_CHUNK_US). -1: not under test.
int vk_ab(const char *what) {
    int n = ab_n;
    static char w[32];
    if (n < 0) {
        const char *e = getenv("LARECOMP_AB"), *c = e ? strchr(e, ':') : NULL;
        n = e ? atoi(e) : 0;
        snprintf(w, sizeof w, "%s", c ? c + 1 : "");
        ab_n = n > 0 ? n : 0;
    }
    if (n <= 0 || (what && strcmp(what, w))) return -1;
    return (int)((renderer_present_count() / (uint64_t)n) & 1);
}

static void split_long_pass(void) {
#ifdef __ANDROID__
    static long split = -1;
    if (split < 0) {  // (3 ms by default; not on Qualcomm's driver, as LARECOMP_VK_CHUNK_US)
        extern bool vk_driver_is_qualcomm(void);
        const char *e = getenv("LARECOMP_VK_SPLIT_US");
        split = e ? atol(e) : vk_driver_is_qualcomm() ? 0 : 3000;
    }
    extern bool vk_async_on(void);
    static unsigned n;  // (the clock every 32 draws: a clock read per draw showed up in the profile)
    if (split > 0 && in_pass && !(++n & 31) && AB("split") != 1 && vk_async_on() && last_submit_ns &&
        SDL_GetTicksNS() - last_submit_ns > (uint64_t)split * 1000)
        renderer_submit();
#endif
}

static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: st.program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_BLEND: { uint8_t *b = x; pipeline_dirty |= memcmp(st.blend[b[0] & 7], b, 8) != 0; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: pipeline_dirty |= memcmp(&st.channel_mask, x, 4) != 0; memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: pipeline_dirty |= memcmp(st.color_state, x, 4) != 0; memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: pipeline_dirty |= st.ms[0xC] != ((uint8_t *)x)[0xC]; memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: pipeline_dirty |= st.poly[2] != ((uint8_t *)x)[2]; memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB: {
            int n = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            pipeline_dirty |= n != st.nattrib || memcmp(st.attrib, x, n * 4) != 0;
            st.nattrib = n;
            memcpy(st.attrib, x, n * 4);
            break;
        }
        case CMD_BIND_VERTEX_STREAM: {
            int n = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            pipeline_dirty |= n != st.nstream || memcmp(st.stream, x, n * 8) != 0;
            st.nstream = n;
            memcpy(st.stream, x, n * 8);
            break;
        }
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_TEXTURES_DEFERRED:  // (stage, first, count, handles): the handles as they are now
            if (r->x[3] >= GUEST_LO && r->x[3] + r->x[2] * 8 <= GUEST_HI)
                for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                    memcpy(&st.tex[r->x[0]][r->x[1] + i], (const uint8_t *)(uintptr_t)r->x[3] + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = getenv("LARECOMP_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) {
                st.rt_dirty = true;
                snapshots_flush_fb_dirty("rt-change");
            }
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_DEPTH_CLAMP: pipeline_dirty |= st.depth_clamp != (r->x[0] != 0); st.depth_clamp = r->x[0] != 0; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            static long clears_from = -1;  // (LARECOMP_LOG_CLEARS=<from frame>)
            if (clears_from < 0) clears_from = getenv("LARECOMP_LOG_CLEARS") ? atol(getenv("LARECOMP_LOG_CLEARS")) : 0x7fffffffL;
            if ((long)renderer_present_count() >= clears_from && index < st.rt.ncolor && st.rt.color[index]) {
                const float *c = x;
                LOG("[cclear] frame %llu idx %d %ux%u f%#x @%llx mask %#x color %.3f %.3f %.3f %.3f\n",
                    (unsigned long long)renderer_present_count(), index, st.rt.color[index]->width, st.rt.color[index]->height,
                    st.rt.color[index]->format, (unsigned long long)st.rt.color[index]->addr, (uint32_t)r->x[2], c[0], c[1], c[2], c[3]);
            }
            if (ct_on() && index < st.rt.ncolor && st.rt.color[index])
                LOG("[ct] clear-color %p mask %#x sc %d %d %d %d\n", (void *)st.rt.color[index], (uint32_t)r->x[2], st.scissor[0][0], st.scissor[0][1],
                    st.scissor[0][2], st.scissor[0][3]);
            if (index < st.rt.ncolor && st.rt.color[index]) alias_before_write(st.rt.color[index], 0, "clear");
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            alias_before_write(t, 0, "clear");
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (ct_on() && st.rt.depth)
                LOG("[ct] clear-depth %p d%d s%d sc %d %d %d %d\n", (void *)st.rt.depth, r->x[0] != 0, r->x[2] != 0, st.scissor[0][0],
                    st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
            if (st.rt.depth && naliases) {  // (copy aliases: which aspects of the depth target this clear replaces whole)
                NvnTexture *d = st.rt.depth;
                bool whole = st.scissor[0][0] <= 0 && st.scissor[0][1] <= 0 && (int64_t)st.scissor[0][0] + st.scissor[0][2] >= (int64_t)d->width &&
                             (int64_t)st.scissor[0][1] + st.scissor[0][3] >= (int64_t)d->height &&
                             !(st.rt.has_dview && (((st.rt.dview.setmask & 1) && st.rt.dview.base_level) ||
                                                   ((st.rt.dview.setmask & 2) && st.rt.dview.min_layer)));
                for (int i = 0; i < st.rt.ncolor; i++)  // (the clear covers the smallest attachment only)
                    if (st.rt.color[i] && (st.rt.color[i]->width < d->width || st.rt.color[i]->height < d->height)) whole = false;
                VkImageAspectFlags full = 0;
                if (whole && r->x[0] != 0) full |= VK_IMAGE_ASPECT_DEPTH_BIT;
                if (whole && renderer_format_has_stencil(d->format) && (r->x[2] & 0xFF) == 0xFF) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (!renderer_format_has_stencil(d->format)) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (ct_on())
                    LOG("[ct] alias clear %p full %#x (whole %d, x0 %llu x1 %#llx x2 %#llx, dview %d/%#x, ncolor %d)\n", (void *)d, full, whole,
                        (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2], st.rt.has_dview,
                        st.rt.dview.setmask, st.rt.ncolor);
                alias_before_write(d, full, "clear");
            }
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); split_long_pass(); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); split_long_pass(); break;
        case CMD_UPDATE_UNIFORM_BUFFER: {
            uint64_t dst = r->x[0] + r->x[2], sz = r->x[3];
            if (ct_on()) LOG("[ct] ubo-update %llx + %llx size %llx%s\n", (unsigned long long)r->x[0], (unsigned long long)r->x[2],
                             (unsigned long long)sz, sz && memcmp((const void *)(uintptr_t)dst, x, sz) ? "" : " (same)");
            if (sz && memcmp((const void *)(uintptr_t)dst, x, sz)) {
                memcpy((void *)(uintptr_t)dst, x, sz);
                guest_copy_invalidate(dst, dst + sz);
            }
            break;
        }
        case CMD_BARRIER: snapshots_flush_fb_dirty("barrier"); break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow: the
            // host clock now, replaced by the GPU's time at this point once the submission is done (cq_resolve).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            int mode = vk_counter_mode();
            // The game's own dynamic resolution (its 3D scene 1600x900 down to 1280x720 in 5% steps) goes by the GPU
            // time these show, and nothing else (its 20 fps step uses its CPU stopwatch: SavageWindRecomp's notes).
            // Its size changes reallocate the scaled targets every few frames (the dark leaves came at those steps):
            // the game is shown no GPU time - every counter of a presented frame the same - and stays at 1600x900;
            // Video > Internal resolution > Dynamic is ours (output_scale.c). The GPU timestamps of our submissions
            // stay (ADPF, the pacer). LARECOMP_GAME_DYNRES=1: the game's own, as before (POCO F7 Ultra, the Great
            // Plateau, 100 s: 105 of 991 frames at 1280x720 and 8 changes with it, none without, the same fps).
            static int game_dynres = -1;
            if (game_dynres < 0) game_dynres = getenv("LARECOMP_GAME_DYNRES") && atoi(getenv("LARECOMP_GAME_DYNRES")) != 0;
            bool frozen = mode == 1 || !game_dynres;
            static uint64_t frame_seen = ~0ull, frame_ts;
            extern uint64_t vk_frame_counter(void);
            if (frozen && vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = ns_to_gpu_ticks(plat_now_ns()); }
            if (!out) break;
            if ((uint64_t)(uintptr_t)out < GUEST_LO || (uint64_t)(uintptr_t)out >= GUEST_HI) {  // (not guest memory)
                static int bad;
                if (bad++ < 5) LOG("[nvn] ReportCounter record %p: destination %p is not guest memory, skipped\n", (void *)r, (void *)out);
                break;
            }
            // Type 1 is SAMPLES_PASSED, the game's occlusion test: a count of 0 told it everything it tested was
            // hidden, and what it thinks hidden it stops animating (birds, smoke and mist far off, frozen in the sky
            // until Link moved). It grows by a lot at every report, so any two give a difference: always visible.
            // (LARECOMP_SAMPLES_PASSED=0: zero, as before)
            static int samples = -1;
            static uint64_t samples_n;
            if (samples < 0) samples = !getenv("LARECOMP_SAMPLES_PASSED") || atoi(getenv("LARECOMP_SAMPLES_PASSED")) != 0;
            out[0] = type == 1 && samples ? (samples_n += 1u << 20) : 0;
            out[1] = frozen ? frame_ts : mode == 3 ? SDL_GetTicksNS() : ns_to_gpu_ticks(plat_now_ns());  // (3 legacy: ns, the old bug)
            static uint64_t *prev_out;  // (frame pacing: the pair's start)
            if (!frozen && (mode == 0 || mode == 2)) out[1] = pace_end_counter(prev_out, out, out[1]);
            prev_out = out;
            {   // (test) LARECOMP_TEST_COUNTER_DELAY_MS=<ms>: stall the host after a frame's first counter - a slow CPU
                // (LARECOMP_TEST_SLOW_AT=<frame>:<count>: only those presented frames, 40 ms unless the delay is set:
                // the game's dynamic resolution drops at a known frame)
                static double delay = -1;
                static uint64_t delayed_frame = ~0ull, slow_at, slow_n;
                if (delay < 0) {
                    delay = getenv("LARECOMP_TEST_COUNTER_DELAY_MS") ? atof(getenv("LARECOMP_TEST_COUNTER_DELAY_MS")) : 0;
                    const char *e = getenv("LARECOMP_TEST_SLOW_AT");
                    if (e && sscanf(e, "%llu:%llu", (unsigned long long *)&slow_at, (unsigned long long *)&slow_n) == 2 && delay <= 0) delay = 40;
                }
                bool in_window = !slow_n || (renderer_present_count() >= slow_at && renderer_present_count() < slow_at + slow_n);
                if (delay > 0 && in_window && vk_frame_counter() != delayed_frame) {
                    delayed_frame = vk_frame_counter();
                    SDL_DelayPrecise((Uint64)(delay * 1e6));
                }
            }
            if (mode == 2 && type == 0 && !frozen) {  // (type 0: timestamp)
                VkCommandBuffer cb = vk_cmd();  // (first: may begin the buffer, which resets the slot's queries)
                CounterQ *q = &cq[cur_sub];
                pthread_mutex_lock(&cq_lock);
                if (q->pool && q->n < CQ_MAX) {
                    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, q->pool, q->n);
#ifdef __ANDROID__
                    if (vk_timing_trace_on()) { q->frame[q->n] = vk_frame_counter(); q->host_ns[q->n] = plat_now_ns(); }
#endif
                    q->out[q->n++] = out;
                }
#ifdef __ANDROID__
                else vk_timing_trace_query_failure(-1, 1, q->seq);
#endif
                pthread_mutex_unlock(&cq_lock);
            }
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            wait_why = "buffer -> buffer copy";
            {   // LARECOMP_LOG_BUFCOPY=1: the first 30 (where in the frame, how large: each waits for the GPU here)
                static int log = -1, n;
                if (log < 0) log = getenv("LARECOMP_LOG_BUFCOPY") != NULL;
                if (log && n++ < 30) {
                    // (whether the GPU writes the source: its bytes before and after the wait)
                    uint64_t h0 = hmix(1469598103934665603ull, (const void *)(uintptr_t)r->x[0], r->x[2] < 4096 ? r->x[2] : 4096);
                    renderer_flush();
                    uint64_t h1 = hmix(1469598103934665603ull, (const void *)(uintptr_t)r->x[0], r->x[2] < 4096 ? r->x[2] : 4096);
                    LOG("[vk] frame %llu: buffer copy %llx -> %llx, %llu bytes; the source %s by the wait\n",
                        (unsigned long long)renderer_present_count(), (unsigned long long)r->x[0], (unsigned long long)r->x[1],
                        (unsigned long long)r->x[2], h0 == h1 ? "unchanged" : "CHANGED");
                }
            }
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        p += r->size;
    }
}

#ifdef __ANDROID__
// LARECOMP_VK_ASYNC=1: queued for the render thread (vulkan/async.c), which runs vk_execute_now
bool vk_async_on(void);
void vk_async_execute(uint64_t handle);
void vk_execute_now(uint64_t handle);
void backend_execute(uint64_t handle) {
    if (vk_async_on()) vk_async_execute(handle);
    else vk_execute_now(handle);
}
void vk_execute_now(uint64_t handle) {
#else
void backend_execute(uint64_t handle) {
#endif
    PROF_T0();
#ifdef __ANDROID__
    game_pending = true;  // (renderer_fence_seq)
#endif
    static int log_lists = -1;  // LARECOMP_LOG_LISTS=1: the command lists of one frame in 120 (draws, passes, time)
    if (log_lists < 0) log_lists = getenv("LARECOMP_LOG_LISTS") != NULL;
    uint64_t ll_d = stat_draws, ll_p = stat_passes, ll_t = log_lists ? plat_now_ns() : 0;
    exec_list(handle, 0);
    if (log_lists && renderer_present_count() % 120 == 0)
        LOG("[lists] frame %llu: list %llx %llu draws %llu passes %.2f ms\n", (unsigned long long)renderer_present_count(),
            (unsigned long long)handle, (unsigned long long)(stat_draws - ll_d), (unsigned long long)(stat_passes - ll_p),
            (plat_now_ns() - ll_t) / 1e6);
    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = getenv("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
    if (sync_submit) renderer_flush();
    else renderer_submit();
#ifdef __ANDROID__
    game_pending = false;
#endif
}
