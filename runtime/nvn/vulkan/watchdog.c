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
#include <unistd.h>

#include "vk.h"
#include "../../platform.h"
#include "../../rt.h"

extern pthread_t g_vk_render_thread;  // (async.c)
extern bool g_vk_render_started;

// Slot 0 is the render thread; every other thread that marks a call gets a slot of its own the first time (the game
// has several: one slot shared by all of them mixed their marks - one thread's enter, another's leave - and the first
// 0.4.6 logs of #401 showed a 76 s "drain" that could not have been one). Past WD_SLOTS - 1 threads the last is shared.
#define WD_SLOTS 17
static struct { _Atomic uint32_t stage, outer; _Atomic uint64_t ns, arg; _Atomic int tid; } wd[WD_SLOTS];
static _Atomic int wd_used = 1;
static __thread int wd_mine = -1;

static int wd_slot(void) {
    if (g_vk_render_started && pthread_equal(pthread_self(), g_vk_render_thread)) return 0;
    if (wd_mine < 0) {
        int i = atomic_fetch_add_explicit(&wd_used, 1, memory_order_relaxed);
        wd_mine = i < WD_SLOTS ? i : WD_SLOTS - 1;
        atomic_store_explicit(&wd[wd_mine].tid, gettid(), memory_order_relaxed);
    }
    return wd_mine;
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
    [WD_R_BUILD_WAIT] = "waiting for a shader or pipeline build",
    [WD_R_BC_DECODE] = "waiting for parallel BC texture decode",
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

// Core 0 (kernel.c): who holds it, since when, who waits for it - and where the holder is. In #401's 0.4.7 logs the
// main thread held core 0 in no SVC and no marked wait while a 100 ms sleep on core 0 had waited 10 s behind it: its
// pc and frames (asked twice, 5 s apart: the same place = stuck there, a different one = a loop) tell a spin in the
// game's code (guest addresses, 0x1_0000_0000 and up in native mode) from a host call (named through dladdr).
#include <dlfcn.h>
static void where(char *out, size_t cap, uint64_t a) {
    Dl_info di;
    if (a && dladdr((void *)(uintptr_t)a, &di) && di.dli_fname) {
        const char *f = strrchr(di.dli_fname, '/');
        if (di.dli_sname && di.dli_saddr)
            snprintf(out, cap, "%llx (%s+%llx, %s)", (unsigned long long)a, di.dli_sname,
                     (unsigned long long)(a - (uint64_t)(uintptr_t)di.dli_saddr), f ? f + 1 : di.dli_fname);
        else
            snprintf(out, cap, "%llx (%s+%llx)", (unsigned long long)a, f ? f + 1 : di.dli_fname,
                     (unsigned long long)(a - (uint64_t)(uintptr_t)di.dli_fbase));
    } else
        snprintf(out, cap, "%llx%s", (unsigned long long)a, a >= 0x100000000ull && a < 0x8000000000ull ? " (guest?)" : "");
}
static void log_core0(uint64_t now) {
    (void)now;
    extern bool kernel_core0_info(uint64_t *id, uint64_t *held_ns, uint64_t *waiters, int max, int *nwaiters);
    extern bool kernel_core0_where(uint64_t *pc, uint64_t *lr, uint64_t *frames, int max, int *nframes);
    uint64_t id = 0, held = 0, w[16];
    int nw = 0;
    char line[512];
    if (!kernel_core0_info(&id, &held, w, 16, &nw)) {
        LOG("[watchdog]   core 0: nobody holds it (%d queued)\n", nw);
        return;
    }
    int n = snprintf(line, sizeof line, "[watchdog]   core 0: t%llu holds it for %.1f s; queued:", (unsigned long long)id, held / 1e9);
    for (int i = 0; i < nw && n > 0 && (size_t)n < sizeof line - 24; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, " t%llu", (unsigned long long)w[i]);
    if (!nw && n > 0 && (size_t)n < sizeof line - 8) snprintf(line + n, sizeof line - (size_t)n, " none");
    LOG("%s\n", line);
    uint64_t pc = 0, lr = 0, fr[16];
    int nf = 0;
    if (!kernel_core0_where(&pc, &lr, fr, 16, &nf)) {
        LOG("[watchdog]   core 0's holder did not answer the signal\n");
        return;
    }
    char a[200], b[200];
    where(a, sizeof a, pc), where(b, sizeof b, lr);
    LOG("[watchdog]   t%llu is at pc %s, lr %s\n", (unsigned long long)id, a, b);
    for (int i = 0; i < nf; i++) {
        where(a, sizeof a, fr[i]);
        LOG("[watchdog]     #%d %s\n", i, a);
    }
}

static void report(uint64_t now, double stalled, bool second) {
    uint64_t done = vk_wd_frames_done(), asked = vk_wd_frames_asked(), q = 0, qd = 0, sub = 0, gdone = 0, age = 0;
    int presents = 0;
    bool waiter = false;
    vk_async_wd_info(&q, &qd, &presents);
    vk_wd_gpu_info(&sub, &gdone, &age, &waiter);
    char r[256], buf[1536];
    describe(r, sizeof r, 0, now);
    int n = snprintf(buf, sizeof buf,
        "[watchdog] %s: no frame finished for %.1f s (frames rendered %llu, handed over by the game %llu)\n"
        "[watchdog]   render thread: %s\n"
        "[watchdog]   async queue: %llu request%s not run yet, %d present%s in flight\n"
        "[watchdog]   GPU: submitted seq %llu, last known done %llu (%s), last submit %.1f s ago\n",
        second ? "still stalled" : "STALL", stalled, (unsigned long long)done, (unsigned long long)asked, r,
        (unsigned long long)(q > qd ? q - qd : 0), q > qd + 1 ? "s" : "", presents, presents == 1 ? "" : "s",
        (unsigned long long)sub, (unsigned long long)gdone, waiter ? "from the waiter thread" : "from our own waits",
        age / 1e9);
    if (n > 0) LOG("%s", buf);
    int used = atomic_load_explicit(&wd_used, memory_order_relaxed), marked = 0;
    for (int i = 1; i < used && i < WD_SLOTS; i++) {  // (the other threads in a marked call; the rest are elsewhere)
        if (atomic_load_explicit(&wd[i].stage, memory_order_relaxed) == WD_NONE) continue;
        describe(r, sizeof r, i, now);
        LOG("[watchdog]   thread %d: %s\n", atomic_load_explicit(&wd[i].tid, memory_order_relaxed), r);
        marked++;
    }
    if (!marked) LOG("[watchdog]   other threads: none in a marked call\n");
    // Where each of the game's threads is (kernel.c): the svc it waits in, for how long, and on what. In the 0.4.6 logs
    // of #401 the render thread waited for the game and the GPU was done: the game's own threads had stopped.
    extern void kernel_log_threads(uint64_t svc_ns);
    kernel_log_threads(2000000000ull);
    log_core0(now);
    extern void kernel_log_stuck_stacks(void (*fmt)(char *out, size_t cap, uint64_t a));
    kernel_log_stuck_stacks(where);  // (the others in a host function or holding a core's turn: where they are)
    fflush(stderr);  // (the log pump reads the pipe: nothing stays in stdio)
}

static void *watch_main(void *arg) {
    (void)arg;
    const char *e = getenv("LARECOMP_WATCHDOG_S");
    double limit = e && atof(e) > 0 ? atof(e) : 5.0;
    uint64_t last_frames = 0, since = plat_now_ns();  // (since: when the frame count last moved, as seen here)
    bool reported = false, second = false, rebooting = false;
    for (;;) {
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        uint64_t now = plat_now_ns(), frames = vk_wd_frames_done();
        // (an in-place reboot - Restart, back to the start screen - stops the frames on purpose until the new boot draws:
        // two 0.4.6 reports, #488 and #489, came from that, 5 s after "[reboot] requested")
        if (rt_reboot_requested() || g_rt_unwind) rebooting = true;
        else if (rebooting && frames != last_frames) rebooting = false;
        // (nothing to judge before the first frame: the start - shader compiles, the load - is the game's own time;
        // and in the background the game stops at its present on purpose, nvn_background)
        if (!frames || frames != last_frames || nvn_background_paused() || rebooting) {
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
