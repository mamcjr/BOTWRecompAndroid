// Android: the game's command lists translated on a render thread of their own (LARECOMP_VK_ASYNC=0: off).
//
// The game's thread records its commands into its own command memory (cmdbuf.c) and, at submit, used to translate
// them to Vulkan right there: 3.5-5.5 ms of each frame, a third of a 60 fps frame on top of the game's own work. Here
// the game's thread only queues the request and goes on; the render thread runs the same translation in the same order.
// That is the contract a real GPU has too: the game reuses command memory, and rewrites what the GPU reads, only after
// waiting on a fence - so the fences cover the translation: a fence gets a logical number when it is queued (or when
// its command list is), and a wait first waits for the render thread to reach it, then for the GPU as before.
//
// Requests: EXEC (a command list), PRESENT (the window's half of backend_present: the game's half - pacing, input,
// tests - stays on the game's thread), FENCE (nvnQueueFenceSync). Everything else that reaches into the renderer from
// the game's thread (a texture destroyed, a movie frame, the heap, a reset) first waits for the queue to empty
// (vk_async_drain): those are rare, and so they run as before, with the render thread idle.
#ifdef __ANDROID__
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../nvn.h"
#include "../../platform.h"
#include "../../rt.h"

enum { REQ_EXEC, REQ_PRESENT, REQ_FENCE };
typedef struct { int type; uint64_t a; void *p; uint64_t id; } Req;

#define QN 4096  // requests in flight (a frame has a few dozen)
static Req q[QN];
static uint64_t q_head, q_tail;   // q_tail: next id to queue (ids start at 1), q_head: next to run
static uint64_t q_done;           // the last id the render thread finished
static int q_presents;            // PRESENT requests queued and not finished (the game's lead)
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_more = PTHREAD_COND_INITIALIZER, q_less = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t p_lock = PTHREAD_MUTEX_INITIALIZER;  // producers: an id, its list's fences and its push together
pthread_t g_vk_render_thread;  // (also read by device.c renderer_present_count, many times a draw)
bool g_vk_render_started;
static int on = -1;

// The game's thread may run this many frames ahead of the render thread (the translation of one frame overlaps the
// game's work on the next; more only adds latency). LARECOMP_VK_ASYNC_AHEAD=n.
static int ahead = 1;

// The render thread's work for its last frame (requests run between two presents, ns): the frame pacer's other half
static uint64_t frame_work, last_frame_work, frame_cpu, last_frame_cpu;
uint64_t vk_async_frame_work_ns(void) { return __atomic_load_n(&last_frame_work, __ATOMIC_RELAXED); }
uint64_t vk_async_frame_cpu_ns(void) { return __atomic_load_n(&last_frame_cpu, __ATOMIC_RELAXED); }

void kernel_blocking_begin(void);        // kernel.c: core 0 to the game's other threads while a guest thread waits here
void kernel_blocking_end(void);
void vk_execute_now(uint64_t handle);    // exec.c: translate and submit a command list
void vk_present_now(NvnTexture *t);      // device.c: the window's half of backend_present
void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
uint64_t renderer_fence_seq(void);

bool vk_async_on(void) {
    if (on < 0) {
        const char *e = getenv("LARECOMP_VK_ASYNC");  // (on by default: =0 turns it off)
        on = !e || atoi(e) != 0;
        const char *a = getenv("LARECOMP_VK_ASYNC_AHEAD");
        if (a && atoi(a) > 0) ahead = atoi(a);
        if (on) LOG("[vk] async: command lists translated on a render thread (%d frame%s ahead)\n", ahead, ahead > 1 ? "s" : "");
    }
    return on;
}

bool vk_async_is_render_thread(void) { return g_vk_render_started && pthread_equal(pthread_self(), g_vk_render_thread); }

// Thread CPU time excludes GPU fences, swapchain acquisition and time descheduled by Android.
static uint64_t render_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *render_main(void *arg) {
    (void)arg;
    extern void plat_thread_unpin(void);
    plat_thread_unpin();
    // Keep the established ADPF signal by default. CPU-only timing is an explicit experiment:
    // excluding descheduling/waits can understate the work seen by Android's frequency governor.
    const char *timing = getenv("LARECOMP_ADPF_CPU_TIME");
    bool cpu_timing = timing && atoi(timing) != 0;
    pthread_mutex_lock(&q_lock);
    for (;;) {
        while (q_head == q_tail) pthread_cond_wait(&q_more, &q_lock);
        Req r = q[q_head % QN];
        pthread_mutex_unlock(&q_lock);
        uint64_t t0 = plat_now_ns(), cpu0 = cpu_timing ? render_cpu_ns() : 0;
        extern void vk_tl(const char *what, uint64_t a, uint64_t b);  // (exec.c: LARECOMP_FRAME_TIMELINE)
        vk_tl(r.type == REQ_EXEC ? "render: list start" : r.type == REQ_PRESENT ? "render: present start" : "render: fence", r.id, 0);
        if (r.type == REQ_EXEC) { vk_execute_now(r.a); free(r.p); }  // (r.p: the list copy, vk_async_execute)
        else if (r.type == REQ_PRESENT) vk_present_now((NvnTexture *)r.p);
        if (r.type != REQ_FENCE) vk_tl(r.type == REQ_EXEC ? "render: list end" : "render: present end", r.id, 0);
        else if (r.type == REQ_FENCE) nvn_sync_set_seq(r.a, renderer_fence_seq());
        uint64_t elapsed = plat_now_ns() - t0;
        frame_cpu += cpu_timing ? render_cpu_ns() - cpu0 : elapsed;
        frame_work += elapsed;
        if (r.type == REQ_PRESENT) {
            __atomic_store_n(&last_frame_work, frame_work, __ATOMIC_RELAXED);
            extern void plat_render_work(uint64_t work_ns, uint64_t target_ns);
            __atomic_store_n(&last_frame_cpu, frame_cpu, __ATOMIC_RELAXED);
            plat_render_work(frame_cpu, 16666667ull);  // (ADPF: clocks for 60 fps on this thread too)
            frame_work = frame_cpu = 0;
        }
        pthread_mutex_lock(&q_lock);
        q_head++;
        q_done = r.id;
        if (r.type == REQ_PRESENT) q_presents--;
        pthread_cond_broadcast(&q_less);
    }
    return NULL;
}

static uint64_t push(int type, uint64_t a, void *p) {
    pthread_mutex_lock(&q_lock);
    if (!g_vk_render_started) {
        g_vk_render_started = true;
        pthread_create(&g_vk_render_thread, NULL, render_main, NULL);
    }
    if (q_tail - q_head >= QN || (type == REQ_PRESENT && q_presents >= ahead)) {
        pthread_mutex_unlock(&q_lock);
        kernel_blocking_begin();
        pthread_mutex_lock(&q_lock);
        while (q_tail - q_head >= QN || (type == REQ_PRESENT && q_presents >= ahead)) pthread_cond_wait(&q_less, &q_lock);
        pthread_mutex_unlock(&q_lock);
        kernel_blocking_end();
        pthread_mutex_lock(&q_lock);
    }
    uint64_t id = ++q_tail;  // (ids 1, 2, ...: the request at index id - 1)
    {
        extern void vk_tl(const char *what, uint64_t a, uint64_t b);
        vk_tl(type == REQ_EXEC ? "game: list sent" : type == REQ_PRESENT ? "game: present sent" : "game: fence sent", id, 0);
    }
    q[(id - 1) % QN] = (Req){type, a, p, id};
    if (type == REQ_PRESENT) q_presents++;
    pthread_cond_signal(&q_more);
    pthread_mutex_unlock(&q_lock);
    return id;
}

// Until the render thread has finished request `id` (0: nothing to wait for).
void vk_async_wait_id(uint64_t id) {
    if (!id || !g_vk_render_started || vk_async_is_render_thread()) return;
    pthread_mutex_lock(&q_lock);
    while (q_done < id) pthread_cond_wait(&q_less, &q_lock);
    pthread_mutex_unlock(&q_lock);
}

// Until every request queued so far has run (the render thread is idle after it).
void vk_async_drain(void) {
    if (!g_vk_render_started || vk_async_is_render_thread()) return;
    pthread_mutex_lock(&q_lock);
    uint64_t id = q_tail;
    while (q_done < id) pthread_cond_wait(&q_less, &q_lock);
    pthread_mutex_unlock(&q_lock);
}

// The fences a command list signals (CMD_FENCE_SYNC, in it or in the lists it calls) get the list's request: a wait
// on one first waits for the render thread to have translated it (its GPU submission is only known then).
static void mark_fences(uint64_t p, uint64_t id, int depth) {
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op == CMD_END) return;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (r->op == CMD_CALL && depth < 8) {
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0]; i++) mark_fences(h[i], id, depth + 1);
        } else if (r->op == CMD_FENCE_SYNC && r->x[1]) {
            NvnSync *s = HOST(NvnSync, r->x[1]);
            if (!s) { s = calloc(1, sizeof *s); SET_HOST(r->x[1], s); }
            s->signaled = 1;
            s->lseq = id;
        }
        if (!r->size) return;
        p += r->size;
    }
}

// A copy of a submitted list, the lists it calls inlined: NVN reads the lists (in control memory) when they are
// submitted, so a game may record over them right after (BOTW does), while the render thread runs them later.
typedef struct { uint8_t *b; size_t n, cap; } Snap;
static void snap_add(Snap *s, uint64_t p, int depth) {
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op == CMD_END) return;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (!r->size) return;
        if (r->op == CMD_CALL && depth < 8) {
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0]; i++) snap_add(s, h[i], depth + 1);
        } else {
            if (s->n + r->size > s->cap) {
                s->cap = (s->n + r->size) * 2 > 0x10000 ? (s->n + r->size) * 2 : 0x10000;
                s->b = realloc(s->b, s->cap);
                if (!s->b) FATAL("vk: out of memory for a command list copy\n");
            }
            memcpy(s->b + s->n, r, r->size);
            s->n += r->size;
        }
        p += r->size;
    }
}
static uint8_t *snap_list(uint64_t handle) {
    Snap s = {0};
    snap_add(&s, handle, 0);
    s.b = realloc(s.b, s.n + sizeof(CmdRec));
    if (!s.b) FATAL("vk: out of memory for a command list copy\n");
    memset(s.b + s.n, 0, sizeof(CmdRec));  // CMD_END
    ((CmdRec *)(s.b + s.n))->size = sizeof(CmdRec);
    return s.b;
}

void vk_async_execute(uint64_t handle) {
    uint8_t *copy = getenv("LARECOMP_NO_LIST_COPY") ? NULL : snap_list(handle);
    if (copy) handle = (uint64_t)(uintptr_t)copy;
    pthread_mutex_lock(&p_lock);
    pthread_mutex_lock(&q_lock);
    uint64_t next = q_tail + 1;  // (the id the push gets: the list's fences carry it before the render thread sees them)
    pthread_mutex_unlock(&q_lock);
    mark_fences(handle, next, 0);
    push(REQ_EXEC, handle, copy);  // (the render thread frees the copy once run)
    pthread_mutex_unlock(&p_lock);
}

void vk_async_present(NvnTexture *t) {
    pthread_mutex_lock(&p_lock);
    push(REQ_PRESENT, 0, t);
    pthread_mutex_unlock(&p_lock);
}

uint64_t vk_async_fence(uint64_t guest_sync) {
    pthread_mutex_lock(&p_lock);
    uint64_t id = push(REQ_FENCE, guest_sync, NULL);
    pthread_mutex_unlock(&p_lock);
    return id;
}
#else
typedef int vk_async_unused;  // (Android only)
#endif
