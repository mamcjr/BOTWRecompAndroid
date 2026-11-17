// Android: a freeze watchdog, diagnostics only - it never aborts or changes what the game does.
//
// Issue #401 (Mali-G1-Ultra): after 4-10 minutes in a busy scene the game stops; the log's frame lines end, a few
// unrelated lines follow, then nothing - no device lost, no fence timeout, no crash - and nobody can reproduce it. So
// the next log has to say where it stops. A thread of its own wakes every second and looks at what the other threads
// leave behind: the count of presents the render thread finished, a stage marker each blocking call sets (a relaxed
// store and one clock read, at a submit or a wait - not per draw), the queue between the game's thread and the render
// thread, and the GPU's submission numbers. Once no frame has finished for LARECOMP_WATCHDOG_S seconds (default 5) it
// writes what it sees, once; it logs again when frames come back. It takes no lock the stuck threads might hold: it
// only reads atomics (a plain counter read the same way: a torn or stale value is just a worse diagnostic).
// LARECOMP_WATCHDOG=0: off.
#ifdef __ANDROID__
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vk.h"
#include "../../platform.h"
#include "../../rt.h"

extern pthread_t g_vk_render_thread;  // (async.c)
extern bool g_vk_render_started;

// Slot 0 is the render thread, slot 1 every other thread (the game's; without the async render thread, the one thread
// that does both, which is then slot 1).
static struct { _Atomic uint32_t stage, outer; _Atomic uint64_t ns, arg; } wd[2];

static int wd_slot(void) {
    return g_vk_render_started && pthread_equal(pthread_self(), g_vk_render_thread) ? 0 : 1;
}

WdMark vk_wd_enter(uint32_t stage, uint64_t arg) {
    int i = wd_slot();
    WdMark m = {atomic_load_explicit(&wd[i].stage, memory_order_relaxed), atomic_load_explicit(&wd[i].outer, memory_order_relaxed),
                atomic_load_explicit(&wd[i].ns, memory_order_relaxed), atomic_load_explicit(&wd[i].arg, memory_order_relaxed)};
    atomic_store_explicit(&wd[i].outer, m.stage, memory_order_relaxed);  // (what this one is inside of)
    atomic_store_explicit(&wd[i].arg, arg, memory_order_relaxed);
    atomic_store_explicit(&wd[i].ns, plat_now_ns(), memory_order_relaxed);
    atomic_store_explicit(&wd[i].stage, stage, memory_order_relaxed);
    return m;
}

void vk_wd_leave(WdMark m) {
    int i = wd_slot();
    atomic_store_explicit(&wd[i].outer, m.outer, memory_order_relaxed);
    atomic_store_explicit(&wd[i].arg, m.arg, memory_order_relaxed);
    atomic_store_explicit(&wd[i].ns, m.ns, memory_order_relaxed);  // (the outer one's own start: it is still in it)
    atomic_store_explicit(&wd[i].stage, m.stage, memory_order_relaxed);
}

void vk_wd_set(uint32_t stage, uint64_t arg) {
    int i = wd_slot();
    atomic_store_explicit(&wd[i].outer, WD_NONE, memory_order_relaxed);
    atomic_store_explicit(&wd[i].arg, arg, memory_order_relaxed);
    atomic_store_explicit(&wd[i].ns, plat_now_ns(), memory_order_relaxed);
    atomic_store_explicit(&wd[i].stage, stage, memory_order_relaxed);
}

static const char *const stage_name[WD_STAGES] = {
    [WD_NONE] = "none (running, or not in a marked call)",
    [WD_R_WAIT_GAME] = "waiting for the game's thread to send a request",
    [WD_R_TRANSLATE] = "translating a command list",
    [WD_R_PRESENT_REQ] = "running a present request",
    [WD_R_FENCE_REQ] = "running a fence request",
    [WD_R_CALL_REQ] = "running a call request",
    [WD_R_SUBMIT] = "vkQueueSubmit",
    [WD_R_SEQ_WAIT] = "seq_wait_done (the waiter thread's word on a submission)",
    [WD_R_ACQUIRE] = "vkAcquireNextImageKHR",
    [WD_R_QUEUE_PRESENT] = "vkQueuePresentKHR",
    [WD_R_SWAPCHAIN] = "swapchain / surface made again (vkDeviceWaitIdle)",
    [WD_GPU_WAIT] = "waiting for the GPU (vk_wait_seq)",
    [WD_G_QUEUE_FULL] = "waiting for room in the render thread's queue (push)",
    [WD_G_WAIT_ID] = "waiting for the render thread to reach a request",
    [WD_G_DRAIN] = "waiting for the render thread to empty its queue",
    [WD_G_SYNCWAIT] = "nvnSyncWait",
};

static const char *name(uint32_t s) { return s < WD_STAGES && stage_name[s] ? stage_name[s] : "?"; }

// Where one slot is, as text ("vkQueuePresentKHR for 4.9 s, inside X").
static void describe(char *out, size_t cap, int i, uint64_t now) {
    uint32_t st = atomic_load_explicit(&wd[i].stage, memory_order_relaxed);
    uint32_t outer = atomic_load_explicit(&wd[i].outer, memory_order_relaxed);
    uint64_t ns = atomic_load_explicit(&wd[i].ns, memory_order_relaxed), arg = atomic_load_explicit(&wd[i].arg, memory_order_relaxed);
    if (st == WD_NONE) { snprintf(out, cap, "%s", name(st)); return; }
    int n = snprintf(out, cap, "%s (arg %llu) since %.1f s ago", name(st), (unsigned long long)arg, now > ns ? (now - ns) / 1e9 : 0.0);
    if (outer != WD_NONE && n > 0 && (size_t)n < cap) snprintf(out + n, cap - (size_t)n, ", inside: %s", name(outer));
}

// The driver's own word, on a thread of its own: these calls go into the driver, which may be what is stuck, and the
// watchdog goes on watching (and tells when frames come back) whatever they do. After a stall of 10 s, once.
static void *probe_main(void *arg) {
    (void)arg;
    uint64_t v = vk_wd_timeline_value();
    LOG("[watchdog] the driver's timeline semaphore reads %llu\n", (unsigned long long)v);
    fflush(stderr);
    vk_device_fault_probe();
    fflush(stderr);
    return NULL;
}

static void report(uint64_t now, double stalled, bool second) {
    uint64_t done = vk_wd_frames_done(), asked = vk_wd_frames_asked(), q = 0, qd = 0, sub = 0, gdone = 0, age = 0;
    int presents = 0;
    bool waiter = false;
    vk_async_wd_info(&q, &qd, &presents);
    vk_wd_gpu_info(&sub, &gdone, &age, &waiter);
    char r[256], g[256], buf[1536];
    describe(r, sizeof r, 0, now);
    describe(g, sizeof g, 1, now);
    int n = snprintf(buf, sizeof buf,
        "[watchdog] %s: no frame finished for %.1f s (frames rendered %llu, handed over by the game %llu)\n"
        "[watchdog]   render thread: %s\n"
        "[watchdog]   game thread(s): %s\n"
        "[watchdog]   async queue: %llu request%s not run yet, %d present%s in flight\n"
        "[watchdog]   GPU: submitted seq %llu, last known done %llu (%s), last submit %.1f s ago\n",
        second ? "still stalled" : "STALL", stalled, (unsigned long long)done, (unsigned long long)asked, r, g,
        (unsigned long long)(q > qd ? q - qd : 0), q > qd + 1 ? "s" : "", presents, presents == 1 ? "" : "s",
        (unsigned long long)sub, (unsigned long long)gdone, waiter ? "from the waiter thread" : "from our own waits",
        age / 1e9);
    if (n > 0) LOG("%s", buf);
    fflush(stderr);  // (the log pump reads the pipe: nothing stays in stdio)
}

static void *watch_main(void *arg) {
    (void)arg;
    const char *e = getenv("LARECOMP_WATCHDOG_S");
    double limit = e && atof(e) > 0 ? atof(e) : 5.0;
    uint64_t last_frames = 0, since = plat_now_ns();  // (since: when the frame count last moved, as seen here)
    bool reported = false, second = false;
    for (;;) {
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        uint64_t now = plat_now_ns(), frames = vk_wd_frames_done();
        // (nothing to judge before the first frame: the start - shader compiles, the load - is the game's own time;
        // and in the background the game stops at its present on purpose, nvn_background)
        if (!frames || frames != last_frames || nvn_background_paused()) {
            if (reported && frames != last_frames)
                LOG("[watchdog] frames resumed after %.1f s (frame %llu)\n", (now - since) / 1e9, (unsigned long long)frames), fflush(stderr);
            if (reported && frames == last_frames) continue;  // (paused during a stall: keep the state until frames come)
            last_frames = frames, since = now, reported = second = false;
            continue;
        }
        double stalled = (now - since) / 1e9;
        if (stalled >= limit && !reported) {
            reported = true;
            report(now, stalled, false);
        } else if (reported && !second && stalled >= (limit * 2 > 10.0 ? limit * 2 : 10.0)) {
            second = true;
            report(now, stalled, true);
            pthread_t t;
            if (pthread_create(&t, NULL, probe_main, NULL) == 0) pthread_detach(t);
        }
    }
    return NULL;
}

void vk_watchdog_start(void) {
    static bool started;
    const char *e = getenv("LARECOMP_WATCHDOG");
    if (started || (e && atoi(e) == 0)) return;
    started = true;
    pthread_t t;
    if (pthread_create(&t, NULL, watch_main, NULL) == 0) pthread_detach(t);
}
#else
typedef int vk_watchdog_unused;  // (Android only)
#endif
