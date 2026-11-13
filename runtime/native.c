// Running the game's own ARM64 code on the phone's CPU (LARECOMP_NATIVE=1, a prototype: docs/EXECUCAO-NATIVA.md).
//
// The modules are loaded and relocated at their guest addresses as always (loader.c); guest addresses are host
// addresses, so the game's code can run there as it is. Only what can't run outside the Switch is rewritten, in place:
//   svc #n              -> a stub (in a patch area after the module) that calls native_svc_gate: the registers go into
//                          the thread's Cpu, rt_svc runs on the host stack, the registers come back
//   mrs/msr tpidr_el0   -> the guest's TPIDR_EL0 in its Cpu (bionic's TLS stays in the real register)
//   mrs tpidrro_el0     -> the guest's TPIDRRO_EL0 (its ThreadLocalRegion) from its Cpu
//   mrs cntpct_el0      -> mrs cntvct_el0 (the virtual counter: readable by apps; 19.2 MHz on Snapdragon, as the Switch)
// x18 is left alone: the game uses it as an ordinary register (~38,000 places), and Linux keeps it across context
// switches; the gates give the host its own x18 back while C runs (bionic's shadow call stack lives there).
// Host functions (HLE: NVN, rtld's callbacks, imports) keep their fake addresses at HLE_BASE, now real code: each
// 16-byte slot jumps to native_hle_gate with its index. A stub finds the thread's Cpu from TPIDR_EL0 through
// g_native_tab (native_gate.S). Everything the runtime does with a Cpu (kernel.c, services, NVN) is unchanged.
#if defined(__aarch64__) && defined(__ANDROID__)
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <pthread.h>

#include "hle.h"
#include "kernel.h"
#include "rt.h"
#include "settings.h"

typedef struct { uint64_t tp; Cpu *cpu; } NativeTabEnt;
// Open addressing from the hashed slot (native_gate.S: the same hash; up to 64 probes, past the end: no wrap). tp 0:
// never used, 1: a thread that ended (probing goes on past it). One slot per thread collided within a minute.
#define TAB_N 4096
#define TAB_PROBES 64
__attribute__((visibility("hidden"), aligned(64))) NativeTabEnt g_native_tab[TAB_N + TAB_PROBES];
static pthread_mutex_t tab_lock = PTHREAD_MUTEX_INITIALIZER;

void native_enter(Cpu *c, uint64_t addr);  // native_gate.S
extern char native_svc_gate[], native_hle_gate[];

// Chosen per boot (main.c boot): the setting (Native game code), or LARECOMP_NATIVE=0/1 over it
static bool g_on;
bool native_wanted(void) {
    const char *e = getenv("LARECOMP_NATIVE");
    return e ? atoi(e) != 0 : g_settings.native_code;
}
void native_begin_boot(void) { g_on = native_wanted(); }
bool native_on(void) { return g_on; }

static uint64_t tp_now(void) { uint64_t v; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
static unsigned tab_idx(uint64_t tp) { return (unsigned)(((tp ^ (tp >> 12)) >> 12) & 4095); }

// The calling host thread runs guest code with this Cpu (its slot: the thread's TPIDR_EL0)
static NativeTabEnt *tab_find(uint64_t tp) {
    NativeTabEnt *e = &g_native_tab[tab_idx(tp)];
    for (int k = 0; k < TAB_PROBES && e[k].tp; k++)
        if (e[k].tp == tp) return &e[k];
    return NULL;
}
static void native_bind(Cpu *c) {
    uint64_t tp = tp_now();
    NativeTabEnt *e = tab_find(tp);
    if (e && e->cpu == c) return;
    pthread_mutex_lock(&tab_lock);
    if (!e) {
        NativeTabEnt *s = &g_native_tab[tab_idx(tp)];
        for (int k = 0; k < TAB_PROBES && !e; k++)
            if (s[k].tp <= 1) e = &s[k];  // (never used, or a thread that ended)
        if (!e) FATAL("[native] no free slot for a thread (%d probes)\n", TAB_PROBES);
    }
    e->cpu = c;
    __atomic_store_n(&e->tp, tp, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&tab_lock);
}
void native_thread_end(void) {
    if (!native_on()) return;
    pthread_mutex_lock(&tab_lock);
    NativeTabEnt *e = tab_find(tp_now());
    if (e) __atomic_store_n(&e->tp, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&tab_lock);
}

void native_call(Cpu *c, uint64_t addr) {
    native_bind(c);
    uint64_t lr = c->x[30];
    native_enter(c, addr);
    c->x[30] = lr;
}

// ---- the gates' C side (on the host stack; c holds the guest's registers)
void native_svc_c(Cpu *c, uint32_t imm) { rt_svc(c, imm, c->x[30]); }

HostFn hle_slot_fn(uint64_t pc);  // dispatch.c
void native_hle_c(Cpu *c, uint32_t i) {
    rt_unwind_point();
    HostFn f = hle_slot_fn(HLE_BASE + 16ull * i);
    f(c);
}

// ---- the rewriter
static uint32_t enc_b(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)(to - from);
    if (d & 3 || d < -(1ll << 27) || d >= (1ll << 27)) FATAL("[native] branch %llx -> %llx out of range\n", from, to);
    return 0x14000000u | ((uint32_t)(d >> 2) & 0x3FFFFFFu);
}
static uint32_t enc_ldr_lit(int rt, uint64_t from, uint64_t to) { return 0x58000000u | (((uint32_t)((to - from) >> 2) & 0x7FFFF) << 5) | rt; }
static uint32_t enc_stp_pre(int t1, int t2, int n, int off) { return 0xA9800000u | (((uint32_t)(off / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1; }
static uint32_t enc_ldp_post(int t1, int t2, int n, int off) { return 0xA8C00000u | (((uint32_t)(off / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1; }
static uint32_t enc_stp(int t1, int t2, int n, int off) { return 0xA9000000u | (((uint32_t)(off / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1; }
static uint32_t enc_ldp(int t1, int t2, int n, int off) { return 0xA9400000u | (((uint32_t)(off / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1; }
static uint32_t enc_str(int t, int n, int off) { return 0xF9000000u | ((uint32_t)(off / 8) << 10) | (n << 5) | t; }
static uint32_t enc_ldr(int t, int n, int off) { return 0xF9400000u | ((uint32_t)(off / 8) << 10) | (n << 5) | t; }
#define SP 31
#define MRS_TPIDR 0xD53BD040u
#define MRS_TPIDRRO 0xD53BD060u
#define MSR_TPIDR 0xD51BD040u
#define MRS_CNTPCT 0xD53BE020u
#define MRS_CNTVCT 0xD53BE040u

// One stub per site, 80 bytes (16-aligned: the literal at +40 / +72). The svc's frame: [sp] x16, x17, [sp + 16] x30,
// [sp + 24] the svc number, [sp + 32] a third register for native_svc_gate.
static uint64_t stub_svc(uint32_t *s, uint64_t at, uint64_t site, uint32_t imm) {
    s[0] = 0xD100C3FFu;                    // sub sp, sp, #48
    s[1] = enc_stp(16, 17, SP, 0);         // stp x16, x17, [sp]
    s[2] = enc_str(30, SP, 16);            // str x30, [sp, #16]
    s[3] = 0xD2800011u | (imm << 5);       // movz x17, #imm
    s[4] = enc_ldr_lit(16, at + 16, at + 40);
    s[5] = 0xD63F0200u;                    // blr x16
    s[6] = enc_ldp(16, 17, SP, 0);         // ldp x16, x17, [sp]
    s[7] = enc_ldr(30, SP, 16);            // ldr x30, [sp, #16]
    s[8] = 0x9100C3FFu;                    // add sp, sp, #48
    s[9] = enc_b(at + 36, site + 4);
    memcpy(&s[10], &(uint64_t){(uint64_t)(uintptr_t)native_svc_gate}, 8);
    return 80;
}
// mrs xN, tpidr_el0 / tpidrro_el0 (load) or msr tpidr_el0, xN (store): xN <-> [Cpu + off], the Cpu from g_native_tab
static uint64_t stub_tls(uint32_t *s, uint64_t at, uint64_t site, int n, int off, bool store) {
    int t[3], k = 0;  // (three temporaries, none of them xN)
    for (int r = 14; r <= 17 && k < 3; r++)
        if (r != n) t[k++] = r;
    int a = t[0], b = t[1], c = t[2];
    s[0] = enc_stp_pre(a, b, SP, -32);
    s[1] = enc_str(c, SP, 16);
    s[2] = MRS_TPIDR | a;                                        // mrs xa, tpidr_el0 (the host's: the key)
    s[3] = 0xCA400000u | (a << 16) | (12 << 10) | (a << 5) | b;  // eor xb, xa, xa, lsr #12
    s[4] = 0xD3400000u | (12 << 16) | (23 << 10) | (b << 5) | b;  // ubfx xb, xb, #12, #12
    s[5] = enc_ldr_lit(c, at + 20, at + 72);                    // ldr xc, =g_native_tab
    s[6] = 0x8B000000u | (b << 16) | (4 << 10) | (c << 5) | b;   // add xb, xc, xb, lsl #4
    s[7] = enc_ldr(c, b, 0);                                    // 1: ldr xc, [xb] (the slot's thread)
    s[8] = 0xCA000000u | (a << 16) | (c << 5) | c;              // eor xc, xc, xa
    s[9] = 0xB4000000u | (3 << 5) | c;                          // cbz xc, 2f (+12)
    s[10] = 0x91004000u | (b << 5) | b;                         // add xb, xb, #16
    s[11] = 0x17FFFFFCu;                                        // b 1b (-16)
    s[12] = enc_ldr(a, b, 8);                                   // 2: ldr xa, [xb, #8] (the Cpu)
    s[13] = store ? enc_str(n, a, off) : enc_ldr(n, a, off);
    s[14] = enc_ldr(c, SP, 16);
    s[15] = enc_ldp_post(a, b, SP, 32);
    s[16] = enc_b(at + 64, site + 4);
    s[17] = 0xD503201Fu;                                        // nop (the literal 8-aligned)
    memcpy(&s[18], &(uint64_t){(uint64_t)(uintptr_t)g_native_tab}, 8);
    return 80;
}

// ---- the game's fixes (tools/patches.py, the same ones at the same addresses: there they are C in place of one
// instruction of the generated code). Here a stub calls native_patch_gate with the fix's number; its handler works on
// the Cpu and says how to go on: 0 run the original instruction (from the stub) and go on after it, 1 the fix's
// other address, 2 return from the function (ret), 3 go on after the instruction without running it.
extern char native_patch_gate[];
int64_t rt_frame_ticks(int64_t ticks);      // nvn/objects.c
int rt_frame_wait(void);                    // hle/framepace.c
void rt_vfr_update(uint64_t vfr);
void rt_persp_update(uint64_t proj);        // hle/widescreen.c
uint32_t rt_persp_aspect(uint64_t proj);
static int fix_clustered_100(Cpu *c) {  // act::ClusteredRenderer's teardown with a null display-list heap
    if (c->x[0]) return 0;
    c->x[21] = c->x[20] + 0xc60;
    return 1;
}
static int fix_clustered_160(Cpu *c) {
    if (c->x[0]) return 0;
    c->x[8] = *(const uint32_t *)(uintptr_t)(c->x[20] + 0xc9c);
    return 1;
}
static int fix_ticks_100(Cpu *c) { c->x[9] = (uint64_t)rt_frame_ticks(*(const int64_t *)(uintptr_t)(c->x[19] + 0x240)); return 3; }
static int fix_ticks_160(Cpu *c) { c->x[8] = (uint64_t)rt_frame_ticks(*(const int64_t *)(uintptr_t)(c->x[19] + 0x248)); return 3; }
static int fix_frame_wait(Cpu *c) { (void)c; return rt_frame_wait() ? 2 : 0; }
static int fix_vfr(Cpu *c) { rt_vfr_update(c->x[19]); return 0; }
static int fix_persp_matrix(Cpu *c) { rt_persp_update(c->x[0]); return 0; }
static int fix_persp_aspect(Cpu *c) { c->v[0] = rt_persp_aspect(c->x[0]); return 3; }
// An actor's tag check (`if (!a) return 0; return hasTag(a->[0x570]->[0x88], hash)`) reached with the actor's +0x570
// still null - a rune's target (Magnesis, Stasis) being made or let go: the game read address 0x88 and Native mode
// crashed (#235, #241, #248). The same answer as for no actor. Not only null: opening the Slate's camera it was -1
// (#247, 1.8.2) - anything outside the game's memory.
static int fix_tag_null(Cpu *c) {
    if (c->x[8] >= GUEST_LO && c->x[8] < GUEST_HI) return 0;
    c->x[0] = 0;
    return 2;
}
// #333: observe the task's registered queue node before setup consumes it and before
// cancellation removes it. Opt-in only; replay the original instructions unchanged.
typedef struct { uint64_t before, after, thread, when; unsigned phase; int core; } Task160Event;
typedef struct { uint64_t task; unsigned sequence; Task160Event events[4]; } Task160History;
static Task160History task160_history[2048];
static pthread_mutex_t task160_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned task160_slot(uint64_t task) { return (unsigned)((task >> 4) ^ (task >> 16)) & 2047; }
static int trace_task160_write(uint64_t task, uint64_t after, unsigned phase) {
    if (task < GUEST_LO || task > GUEST_HI - 0x198) return 0;
    uint64_t before = *(const uint64_t *)(uintptr_t)(task + 0x128);
    KThread *t = kernel_current_thread();
    Task160Event e = {before, after, t ? t->id : 0, plat_now_ns(), phase,
                     t ? __atomic_load_n(&t->core, __ATOMIC_ACQUIRE) : -1};
    pthread_mutex_lock(&task160_lock);
    Task160History *h = &task160_history[task160_slot(task)];
    if (h->task != task) { h->task = task; h->sequence = 0; }
    h->events[h->sequence++ & 3] = e;
    pthread_mutex_unlock(&task160_lock);
    return 0;
}
static void trace_task160_history(uint64_t task) {
    Task160History h;
    pthread_mutex_lock(&task160_lock);
    h = task160_history[task160_slot(task)];
    pthread_mutex_unlock(&task160_lock);
    if (h.task != task) { LOG("[task160] node history unavailable (no record or slot reused)\n"); return; }
    for (unsigned i = h.sequence > 4 ? h.sequence - 4 : 0; i < h.sequence; i++) {
        Task160Event e = h.events[i & 3];
        LOG("[task160] node history task=%llx phase=%u before=%llx after=%llx thread=%llu core=%d time=%llu\n",
            (unsigned long long)task, e.phase, (unsigned long long)e.before, (unsigned long long)e.after,
            (unsigned long long)e.thread, e.core, (unsigned long long)e.when);
    }
}
static int trace_task160_reset(Cpu *c) { return trace_task160_write(c->x[19], 0, 1); }
static int trace_task160_assign(Cpu *c) { return trace_task160_write(c->x[28] - 0x128, c->x[26], 2); }
static void trace_task160_rejection(Cpu *c, const char *phase) {
    uint64_t task = c->x[19];
    uint32_t request[8] = {0}; uint64_t node = 0, node_fields[12] = {0};
    if (task >= GUEST_LO && task <= GUEST_HI - 0x198) {
        memcpy(request, (const void *)(uintptr_t)(task + 0x130), sizeof request);
        memcpy(&node, (const void *)(uintptr_t)(task + 0x128), sizeof node);
    }
    if (node >= GUEST_LO && node <= GUEST_HI - sizeof node_fields)
        memcpy(node_fields, (const void *)(uintptr_t)node, sizeof node_fields);
    KThread *t = kernel_current_thread();
    LOG("[task160] rejection %s task=%llx node=%llx manager=%llx list=%llx thread=%llu core=%d\n",
        phase, (unsigned long long)task, (unsigned long long)node, (unsigned long long)c->x[21],
        (unsigned long long)c->x[25], (unsigned long long)(t ? t->id : 0),
        t ? __atomic_load_n(&t->core, __ATOMIC_ACQUIRE) : -1);
    LOG("[task160] request +130=%x/%x/%x/%x/%x/%x/%x/%x; x8-x15=%llx/%llx/%llx/%llx/%llx/%llx/%llx/%llx\n",
        request[0], request[1], request[2], request[3], request[4], request[5], request[6], request[7],
        (unsigned long long)c->x[8], (unsigned long long)c->x[9], (unsigned long long)c->x[10],
        (unsigned long long)c->x[11], (unsigned long long)c->x[12], (unsigned long long)c->x[13],
        (unsigned long long)c->x[14], (unsigned long long)c->x[15]);
    for (unsigned i = 0; i < 12; i += 4)
        LOG("[task160] node +%x=%llx/%llx/%llx/%llx\n", i * 8,
            (unsigned long long)node_fields[i], (unsigned long long)node_fields[i+1],
            (unsigned long long)node_fields[i+2], (unsigned long long)node_fields[i+3]);
}
static int trace_task160_reject(Cpu *c) { trace_task160_rejection(c, "before removal"); return 0; }
static int trace_task160_evict(Cpu *c) {
    trace_task160_rejection(c, "before zeroing");
    return trace_task160_write(c->x[28] - 0x128, 0, 3);
}
static int trace_task160_remove(Cpu *c) { return trace_task160_write(c->x[20], 0, 4); }
static int trace_task160_destroy(Cpu *c) { return trace_task160_write(c->x[19], 0, 5); }
static void trace_task160(uint64_t manager, uint64_t task, uint64_t lr, const char *phase) {
    static unsigned count;
    if (task < GUEST_LO || task > GUEST_HI - 0x198) return;
    uint64_t node = *(const uint64_t *)(uintptr_t)(task + 0x128);
    if (__atomic_fetch_add(&count, 1, __ATOMIC_RELAXED) < 40 || !node)
        LOG("[task160] %s manager=%llx task=%llx node=%llx owner=%llx flags=%x lr=%llx\n", phase,
            (unsigned long long)manager, (unsigned long long)task, (unsigned long long)node,
            (unsigned long long)*(const uint64_t *)(uintptr_t)(task + 0x150),
            *(const uint8_t *)(uintptr_t)(task + 0x192), (unsigned long long)lr);
    if (!node) trace_task160_history(task);
}
static int trace_task160_setup(Cpu *c) { trace_task160(c->x[21], c->x[19], c->x[30], "setup"); return 0; }
static int trace_task160_cancel(Cpu *c) { trace_task160(c->x[0], c->x[1], c->x[30], "cancel"); return 0; }
typedef struct { uint64_t descriptor, buffer, caller; int bound; } Output160History;
static Output160History output160_history[1024];
static pthread_mutex_t output160_lock = PTHREAD_MUTEX_INITIALIZER;
static void output160_event(uint64_t descriptor, uint64_t buffer, uint64_t caller, int bound) {
    unsigned slot = (unsigned)((descriptor >> 4) ^ (descriptor >> 16)) & 1023;
    pthread_mutex_lock(&output160_lock);
    output160_history[slot] = (Output160History){descriptor, buffer, caller, bound};
    pthread_mutex_unlock(&output160_lock);
}
static int trace_output160_construct(Cpu *c) { output160_event(c->x[19], 0, c->x[30], 0); return 0; }
static int trace_output160_bind(Cpu *c) { output160_event(c->x[0], c->x[1], c->x[30], 1); return 0; }
typedef struct { Cpu *cpu; uint64_t sp, descriptor, input, target; } Cursor160History;
static Cursor160History cursor160_history[1024];
// Bounded observation only: preserve the game's pool selection/release behavior.
typedef struct { uint64_t pool, buffer, index, cpu; unsigned phase; } Pool160Event;
static Pool160Event pool160_events[64];
static unsigned pool160_sequence;
static void trace_pool160_event(Cpu *c, uint64_t pool, uint64_t buffer, uint64_t index, unsigned phase) {
    Pool160Event recent[64];
    unsigned total = 0;
    int count = 0; uint64_t buffers = 0, flags = 0;
    if (pool < GUEST_LO || pool > GUEST_HI - 0x92) return;
    const uint8_t *p = (const uint8_t *)(uintptr_t)pool;
    memcpy(&count, p + 0x58, 4);
    // A pool of zero means this object needs no scratch buffer; zero is normal.
    bool failed = !buffer && phase == 1 && count > 0;
    pthread_mutex_lock(&output160_lock);
    pool160_events[pool160_sequence++ & 63] = (Pool160Event){pool, buffer, index, (uint64_t)(uintptr_t)c, phase};
    if (failed) {
        total = pool160_sequence < 64 ? pool160_sequence : 64;
        for (unsigned i = 0; i < total; i++) recent[i] = pool160_events[(pool160_sequence - total + i) & 63];
    }
    pthread_mutex_unlock(&output160_lock);
    if (!total) return;
    if (pool >= GUEST_LO && pool <= GUEST_HI - 0x92) {
        memcpy(&buffers, p + 0x50, 8); memcpy(&flags, p + 0x60, 8);
        LOG("[pool160] null selection pool=%llx count=%d buffers=%llx flags=%llx cpu=%llx\n",
            (unsigned long long)pool, count, (unsigned long long)buffers,
            (unsigned long long)flags, (unsigned long long)(uintptr_t)c);
        if (count > 0 && count <= 64 && buffers >= GUEST_LO && buffers <= GUEST_HI - (uint64_t)count * 8 &&
            flags >= GUEST_LO && flags <= GUEST_HI - (uint64_t)count)
            for (int i = 0; i < count; i++) {
                uint64_t b; memcpy(&b, (const void *)(uintptr_t)(buffers + (uint64_t)i * 8), 8);
                LOG("[pool160] slot=%d buffer=%llx busy=%u\n", i, (unsigned long long)b,
                    *(const uint8_t *)(uintptr_t)(flags + i));
            }
    }
    for (unsigned i = 0; i < total; i++) if (recent[i].pool == pool)
        LOG("[pool160] history phase=%u pool=%llx buffer=%llx index=%llu cpu=%llx\n", recent[i].phase,
            (unsigned long long)pool, (unsigned long long)recent[i].buffer,
            (unsigned long long)recent[i].index, (unsigned long long)recent[i].cpu);
}
static int trace_pool160_select(Cpu *c) { trace_pool160_event(c, c->x[19], c->x[8], c->x[22], 1); return 0; }
static int trace_pool160_release(Cpu *c) { trace_pool160_event(c, c->x[20], c->x[21], c->x[9], 2); return 0; }
static int trace_pool160_configure(Cpu *c) { trace_pool160_event(c, c->x[0], c->x[1], c->x[2], 3); return 0; }
// Experimental #333 guard: keep one workspace owner's configure/use/release
// transaction together. Only installed with LARECOMP_FIX_WORKSPACE160=1.
// Bounded stripes may serialize unrelated owners on a hash collision.
#define WORKSPACE160_GUARDS 256
void kernel_blocking_begin(void);
void kernel_blocking_end(void);
static pthread_mutex_t workspace160_guards[WORKSPACE160_GUARDS];
static pthread_once_t workspace160_once = PTHREAD_ONCE_INIT;
static void workspace160_init(void) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) || pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE))
        FATAL("workspace160: cannot initialize guard attributes\n");
    for (unsigned i = 0; i < WORKSPACE160_GUARDS; i++)
        if (pthread_mutex_init(&workspace160_guards[i], &attr)) FATAL("workspace160: cannot initialize guard\n");
    pthread_mutexattr_destroy(&attr);
}
static pthread_mutex_t *workspace160_guard(uint64_t owner) {
    return &workspace160_guards[((owner >> 4) ^ (owner >> 16)) & (WORKSPACE160_GUARDS - 1)];
}
static int fix_workspace160_enter(Cpu *c) {
    pthread_once(&workspace160_once, workspace160_init);
    pthread_mutex_t *guard = workspace160_guard(c->x[0]);
    int err = pthread_mutex_trylock(guard);
    if (err == EBUSY) {
        // A thread waiting here must not retain guest core 0: the current
        // workspace owner may need that core to finish and release the guard.
        kernel_blocking_begin();
        err = pthread_mutex_lock(guard);
        kernel_blocking_end();
        static unsigned waits;
        unsigned n = __atomic_fetch_add(&waits, 1, __ATOMIC_RELAXED);
        if (n < 16 || !(n & 255))
            LOG("[workspace160] serialized overlap owner=%llx waits=%u\n", (unsigned long long)c->x[0], n + 1);
    }
    if (err) FATAL("workspace160: cannot acquire guard (%d)\n", err);
    return 0;
}
static int fix_workspace160_leave(Cpu *c) {
    // x19 still holds the owner here, before the epilogue restores registers.
    int err = pthread_mutex_unlock(workspace160_guard(c->x[19]));
    if (err) FATAL("workspace160: cannot release guard (%d)\n", err);
    return 0;
}
// Observe the indirect-call family reached after the workspace candidate. Do
// not replace the target or suppress an invalid call; preserve the evidence.
static int trace_virtual160(Cpu *c, uint64_t object, const char *phase) {
    static unsigned samples;
    uint64_t target = c->x[8];
    bool suspect = !((target >= MAIN_BASE && target < SDK_BASE + 0x1000000) ||
                     (target >= HLE_BASE && target < HLE_BASE + 0x14000));
    if (!suspect && __atomic_fetch_add(&samples, 1, __ATOMIC_RELAXED) >= 16) return 0;
    KThread *t = kernel_current_thread();
    uint64_t fields[4] = {0};
    if (object >= GUEST_LO && object <= GUEST_HI - sizeof fields)
        memcpy(fields, (const void *)(uintptr_t)object, sizeof fields);
    LOG("[virtual160] %s suspect=%d object=%llx target=%llx fields=%llx/%llx/%llx/%llx owner=%llx lr=%llx thread=%llu core=%d\n",
        phase, suspect, (unsigned long long)object, (unsigned long long)target,
        (unsigned long long)fields[0], (unsigned long long)fields[1],
        (unsigned long long)fields[2], (unsigned long long)fields[3],
        (unsigned long long)c->x[19], (unsigned long long)c->x[30],
        (unsigned long long)(t ? t->id : 0), t ? __atomic_load_n(&t->core, __ATOMIC_ACQUIRE) : -1);
    return 0;
}
static int trace_virtual160_create(Cpu *c) { return trace_virtual160(c, c->x[21], "create"); }
static int trace_virtual160_start(Cpu *c) { return trace_virtual160(c, c->x[0], "start"); }
static int trace_virtual160_drop(Cpu *c) { return trace_virtual160(c, c->x[20], "drop"); }
static int trace_virtual160_update(Cpu *c) { return trace_virtual160(c, c->x[20], "update"); }
static int trace_cursor160_entry(Cpu *c) {
    if (!c->x[2]) LOG("[cursor160] null workspace object=%llx owner=%llx caller=%llx\n",
        (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[30]);
    return 0;
}
static int trace_cursor160_before(Cpu *c) {
    unsigned slot = (unsigned)((uintptr_t)c >> 6) & 1023;
    pthread_mutex_lock(&output160_lock);
    cursor160_history[slot] = (Cursor160History){c, c->sp, c->x[0], c->x[20], c->x[8]};
    pthread_mutex_unlock(&output160_lock);
    return 0;
}
static int trace_cursor160_after(Cpu *c) {
    if (c->x[0]) return 0;
    unsigned slot = (unsigned)((uintptr_t)c >> 6) & 1023;
    pthread_mutex_lock(&output160_lock);
    Cursor160History h = cursor160_history[slot];
    pthread_mutex_unlock(&output160_lock);
    if (h.cpu == c && h.sp == c->sp)
        LOG("[cursor160] zero result descriptor=%llx input=%llx target=%llx\n",
            (unsigned long long)h.descriptor, (unsigned long long)h.input, (unsigned long long)h.target);
    else LOG("[cursor160] zero result: call history unavailable\n");
    return 0;
}
static void trace_output160(uint64_t descriptor, uint64_t source, uint64_t lr, const char *phase) {
    static unsigned count;
    if (descriptor < GUEST_LO || descriptor > GUEST_HI - 0x24) {
        LOG("[output160] %s invalid descriptor=%llx lr=%llx\n", phase,
            (unsigned long long)descriptor, (unsigned long long)lr);
        return;
    }
    const uint64_t *d = (const uint64_t *)(uintptr_t)descriptor;
    uint64_t output = d[2];
    if (!output) {
        unsigned slot = (unsigned)((descriptor >> 4) ^ (descriptor >> 16)) & 1023;
        pthread_mutex_lock(&output160_lock);
        Output160History h = output160_history[slot];
        pthread_mutex_unlock(&output160_lock);
        LOG("[output160] null history descriptor=%llx last=%s buffer=%llx caller=%llx\n",
            (unsigned long long)descriptor, h.descriptor != descriptor ? "unknown (not recorded or slot reused)"
            : h.bound ? "buffer assigned" : "constructed, no later assignment recorded",
            (unsigned long long)(h.descriptor == descriptor ? h.buffer : 0),
            (unsigned long long)(h.descriptor == descriptor ? h.caller : 0));
    }
    if (__atomic_fetch_add(&count, 1, __ATOMIC_RELAXED) < 16 || !output)
        LOG("[output160] %s descriptor=%llx source=%llx output=%llx fields=%llx/%llx/%llx/%x lr=%llx\n",
            phase, (unsigned long long)descriptor, (unsigned long long)source, (unsigned long long)output,
            (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[3],
            *(const uint32_t *)(uintptr_t)(descriptor + 0x20), (unsigned long long)lr);
}
static int trace_output160_entry(Cpu *c) { trace_output160(c->x[2], c->x[0], c->x[30], "entry"); return 0; }
static int trace_output160_load(Cpu *c) { trace_output160(c->x[20], c->x[24], c->x[30], "load (x30 is scratch)"); return 0; }
typedef struct { int version; uint64_t addr, alt; uint32_t word; int (*fn)(Cpu *); const char *what; } Fix;
static const Fix fixes[] = {  // (canonical addresses, 0x7000000000 + the offset in main)
    {100, 0x70010c1aa0, 0x70010c1ad8, 0xf9400008, fix_clustered_100, "ClusteredRenderer teardown"},
    {100, 0x7000c4f4d4, 0, 0xf9412269, fix_ticks_100, "frame duration"},
    {100, 0x7000d5dce0, 0, 0xf9404500, fix_tag_null, "tag check, no target yet"},
    {160, 0x7001861440, 0x7001861470, 0xf9400008, fix_clustered_160, "ClusteredRenderer teardown"},
    {160, 0x700161495c, 0, 0xf9412668, fix_ticks_160, "frame duration"},
    {160, 0x7001296cfc, 0, 0xf9400388, trace_task160_setup, "task queue setup trace"},
    {160, 0x7001297080, 0, 0xf81d0ff5, trace_task160_cancel, "task queue cancellation trace"},
    {160, 0x70012962f4, 0, 0xf900967f, trace_task160_reset, "task node reset trace"},
    {160, 0x700129663c, 0, 0xf900039a, trace_task160_assign, "task node assignment trace"},
    {160, 0x7001296cc0, 0, 0xf900039f, trace_task160_evict, "task node eviction trace"},
    {160, 0x7001296cb0, 0, 0x320003e2, trace_task160_reject, "task rejection context trace"},
    {160, 0x700129714c, 0, 0xf900969f, trace_task160_remove, "task node removal trace"},
    {160, 0x700128ff20, 0, 0xf900967f, trace_task160_destroy, "task node teardown trace"},
    {160, 0x700128ff7c, 0, 0xf900967f, trace_task160_destroy, "task node teardown reset trace"},
    {160, 0x7001db55ec, 0, 0xd10643ff, trace_output160_entry, "output buffer entry trace"},
    {160, 0x7001db5a4c, 0, 0xf9400a8a, trace_output160_load, "output buffer load trace"},
    {160, 0x7001e0df64, 0, 0xb900226a, trace_output160_construct, "output buffer construction trace"},
    {160, 0x7001e0e03c, 0, 0x3941612b, trace_output160_bind, "output buffer binding trace"},
    {160, 0x7001da0d90, 0, 0xa9bb67fa, trace_cursor160_entry, "workspace entry trace"},
    {160, 0x7001da0f00, 0, 0xaa1403e1, trace_cursor160_before, "workspace cursor input trace"},
    {160, 0x7001da0f08, 0, 0xb9403308, trace_cursor160_after, "workspace cursor result trace"},
    {160, 0x7001da402c, 0, 0xf90013e8, trace_pool160_select, "workspace pool selection trace"},
    {160, 0x7001da3ef0, 0, 0xf9403288, trace_pool160_release, "workspace pool release trace"},
    {160, 0x7001da3d14, 0, 0xf81c0ff7, trace_pool160_configure, "workspace pool configuration trace"},
    {160, 0x7001da3f1c, 0, 0xd10283ff, fix_workspace160_enter, "workspace transaction guard entry"},
    {160, 0x7001da41e0, 0, 0xfd401be8, fix_workspace160_leave, "workspace transaction guard exit"},
    {160, 0x7001339db4, 0, 0xaa1503e0, trace_virtual160_create, "object create call trace"},
    {160, 0x700133becc, 0, 0xaa0003f4, trace_virtual160_start, "object start call trace"},
    {160, 0x700133beec, 0, 0xaa1403e0, trace_virtual160_drop, "object drop call trace"},
    {160, 0x700133e564, 0, 0xaa1403e0, trace_virtual160_update, "object update call trace"},
    {182, 0x7000d811fc, 0, 0xa9bd7bfd, fix_frame_wait, "frame wait"},
    {182, 0x70014092f4, 0, 0xb9407a68, fix_vfr, "VFR ratio"},
    {182, 0x7000da13f4, 0, 0xbd409800, fix_persp_matrix, "projection matrix"},
    {182, 0x7000da13d8, 0, 0xbd40b000, fix_persp_aspect, "projection aspect"},
    {182, 0x70010f7eb8, 0, 0xf9404500, fix_tag_null, "tag check, no target yet"},
    {190, 0x7000e9487c, 0, 0xa9bd7bfd, fix_frame_wait, "frame wait"},
    {190, 0x700151d494, 0, 0xb9407a68, fix_vfr, "VFR ratio"},
    {190, 0x7000eb4a44, 0, 0xbd409800, fix_persp_matrix, "projection matrix"},
    {190, 0x7000eb4a28, 0, 0xbd40b000, fix_persp_aspect, "projection aspect"},
    {190, 0x700120bc48, 0, 0xf9404500, fix_tag_null, "tag check, no target yet"},
};
int native_patch_c(Cpu *c, uint32_t i) { return fixes[i].fn(c); }

static uint64_t stub_fix(uint32_t *s, uint64_t at, uint64_t site, uint32_t id, uint32_t orig, uint64_t alt) {
    const uint32_t pop[3] = {enc_ldp(16, 17, SP, 0), enc_ldr(30, SP, 16), 0x9100C3FFu};  // (the frame back, add sp #48)
    s[0] = 0xD100C3FFu;                         // sub sp, sp, #48
    s[1] = enc_stp(16, 17, SP, 0);
    s[2] = enc_str(30, SP, 16);
    s[3] = 0xD2800011u | (id << 5);             // movz x17, #id
    s[4] = enc_ldr_lit(16, at + 16, at + 112);
    s[5] = 0xD63F0200u;                         // blr x16 (native_patch_gate: the way on at [sp + 24])
    s[6] = enc_ldr(16, SP, 24);
    s[7] = 0xB4000000u | (15 << 5) | 16;        // cbz x16, 22 (+60)
    s[8] = 0x37080000u | (5 << 5) | 16;         // tbnz w16, #1, 13 (+20)
    memcpy(&s[9], pop, 12);                     // 1: the fix's other address
    s[12] = alt ? enc_b(at + 48, alt) : 0xD4200000u;
    s[13] = 0x37000000u | (5 << 5) | 16;        // tbnz w16, #0, 18 (+20)
    memcpy(&s[14], pop, 12);                    // 2: return
    s[17] = 0xD65F03C0u;                        // ret
    memcpy(&s[18], pop, 12);                    // 3: on, without the instruction
    s[21] = enc_b(at + 84, site + 4);
    memcpy(&s[22], pop, 12);                    // 0: the instruction, then on
    s[25] = orig;
    s[26] = enc_b(at + 104, site + 4);
    s[27] = 0xD503201Fu;                        // nop (the literal 8-aligned)
    memcpy(&s[28], &(uint64_t){(uint64_t)(uintptr_t)native_patch_gate}, 8);
    return 128;
}

static void prepare_module(Module *m) {
    uint32_t *text = (uint32_t *)(uintptr_t)(m->base + m->text_off);
    uint64_t nw = m->text_size / 4, nsites = 0;
    for (uint64_t i = 0; i < nw; i++) {
        uint32_t w = text[i];
        if ((w & 0xFFE0001Fu) == 0xD4000001u || (w & ~31u) == MRS_TPIDR || (w & ~31u) == MRS_TPIDRRO || (w & ~31u) == MSR_TPIDR)
            nsites++;
    }
    bool is_main = m->base == MAIN_BASE;
    uint64_t nfix = sizeof fixes / sizeof *fixes;
    uint64_t area = (m->base + m->size + 0xFFFF) & ~0xFFFFull, asize = (nsites * 80 + nfix * 128 + 0x3FFF) & ~0x3FFFull;
    if (!asize) asize = 0x4000;
    plat_commit(area, asize);
    uint64_t at = area;
    unsigned nsvc = 0, ntls = 0, ncnt = 0;
    for (uint64_t i = 0; i < nw; i++) {
        uint32_t w = text[i];
        uint64_t site = (uint64_t)(uintptr_t)&text[i];
        uint64_t len = 0;
        if ((w & 0xFFE0001Fu) == 0xD4000001u) len = stub_svc((uint32_t *)(uintptr_t)at, at, site, (w >> 5) & 0xFFFF), nsvc++;
        else if ((w & ~31u) == MRS_TPIDRRO) len = stub_tls((uint32_t *)(uintptr_t)at, at, site, w & 31, 288, false), ntls++;
        else if ((w & ~31u) == MRS_TPIDR) len = stub_tls((uint32_t *)(uintptr_t)at, at, site, w & 31, 280, false), ntls++;
        else if ((w & ~31u) == MSR_TPIDR) len = stub_tls((uint32_t *)(uintptr_t)at, at, site, w & 31, 280, true), ntls++;
        else if ((w & ~31u) == MRS_CNTPCT) { text[i] = MRS_CNTVCT | (w & 31); ncnt++; continue; }
        if (!len) continue;
        text[i] = enc_b(site, at);
        at += len;
    }
    unsigned nfixed = 0;
    for (uint32_t f = 0; is_main && f < nfix; f++) {
        if (fixes[f].version != g_game_version) continue;
        if ((fixes[f].fn == trace_task160_setup || fixes[f].fn == trace_task160_cancel) &&
            !getenv("LARECOMP_TRACE_TASK160")) continue;
        if ((fixes[f].fn == trace_task160_reset || fixes[f].fn == trace_task160_assign ||
             fixes[f].fn == trace_task160_evict || fixes[f].fn == trace_task160_remove ||
             fixes[f].fn == trace_task160_destroy || fixes[f].fn == trace_task160_reject) && !ENV("LARECOMP_TRACE_TASK160")) continue;
        if ((fixes[f].fn == trace_output160_entry || fixes[f].fn == trace_output160_load ||
             fixes[f].fn == trace_output160_construct || fixes[f].fn == trace_output160_bind) &&
            !getenv("LARECOMP_TRACE_WRITE160")) continue;
        if ((fixes[f].fn == trace_cursor160_entry || fixes[f].fn == trace_cursor160_before ||
             fixes[f].fn == trace_cursor160_after) && !getenv("LARECOMP_TRACE_WRITE160")) continue;
        if ((fixes[f].fn == trace_pool160_select || fixes[f].fn == trace_pool160_release ||
             fixes[f].fn == trace_pool160_configure) && !getenv("LARECOMP_TRACE_WRITE160")) continue;
        if ((fixes[f].fn == fix_workspace160_enter || fixes[f].fn == fix_workspace160_leave) &&
            !ENV("LARECOMP_FIX_WORKSPACE160")) continue;
        if ((fixes[f].fn == trace_virtual160_create || fixes[f].fn == trace_virtual160_start ||
             fixes[f].fn == trace_virtual160_drop || fixes[f].fn == trace_virtual160_update) &&
            !ENV("LARECOMP_TRACE_VIRTUAL160")) continue;
        uint64_t site = MAIN_BASE + (fixes[f].addr - 0x7000000000ull);
        uint64_t alt = fixes[f].alt ? MAIN_BASE + (fixes[f].alt - 0x7000000000ull) : 0;
        uint32_t *w = (uint32_t *)(uintptr_t)site;
        if (*w != fixes[f].word) FATAL("[native] fix '%s' at %llx: %08x, not %08x\n", fixes[f].what,
                                       (unsigned long long)fixes[f].addr, *w, fixes[f].word);
        at += stub_fix((uint32_t *)(uintptr_t)at, at, site, f, *w, alt);
        *w = enc_b(site, at - 128);
        nfixed++;
    }
    uint64_t t0 = m->base + m->text_off, tsz = (m->text_size + 0xFFF) & ~0xFFFull;
    __builtin___clear_cache((char *)(uintptr_t)t0, (char *)(uintptr_t)(t0 + tsz));
    __builtin___clear_cache((char *)(uintptr_t)area, (char *)(uintptr_t)(area + asize));
    if (mprotect((void *)(uintptr_t)t0, tsz, PROT_READ | PROT_EXEC) || mprotect((void *)(uintptr_t)area, asize, PROT_READ | PROT_EXEC))
        FATAL("[native] %s: the code can't be made executable (errno %d)\n", m->name, errno);
    LOG("[native] %s: %u svc, %u TLS, %u counter reads rewritten, %u fixes; stubs at %llx (%llu KB)\n", m->name, nsvc,
        ntls, ncnt, nfixed, (unsigned long long)area, (unsigned long long)(asize >> 10));
}

// The HLE slots as code: slot i = movz x17, #i; b trampoline (-> native_hle_gate)
static void prepare_hle(void) {
    uint64_t n = 4096, tramp = HLE_BASE + 16 * n, size = 16 * n + 0x4000;
    plat_commit(HLE_BASE, size);
    for (uint64_t i = 0; i < n; i++) {
        uint32_t *s = (uint32_t *)(uintptr_t)(HLE_BASE + 16 * i);
        s[0] = 0xD2800011u | ((uint32_t)i << 5);
        s[1] = enc_b(HLE_BASE + 16 * i + 4, tramp);
        s[2] = s[3] = 0xD4200000u;  // brk #0
    }
    uint32_t *t = (uint32_t *)(uintptr_t)tramp;
    t[0] = enc_ldr_lit(16, tramp, tramp + 8);
    t[1] = 0xD61F0200u;  // br x16
    memcpy(&t[2], &(uint64_t){(uint64_t)(uintptr_t)native_hle_gate}, 8);
    __builtin___clear_cache((char *)(uintptr_t)HLE_BASE, (char *)(uintptr_t)(HLE_BASE + size));
    if (mprotect((void *)(uintptr_t)HLE_BASE, size, PROT_READ | PROT_EXEC))
        FATAL("[native] the HLE slots can't be made executable (errno %d)\n", errno);
}

void native_prepare(void) {
    if (!native_on()) return;
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    LOG("[native] the game's code runs on the CPU (counter %llu Hz%s)\n", (unsigned long long)f,
        f == 19200000 ? ", as the Switch's" : ": NOT the Switch's 19.2 MHz - the game's timing will be off");
    memset(g_native_tab, 0, sizeof g_native_tab);
    for (int i = 0; i < g_nmodules; i++) prepare_module(&g_modules[i]);
    prepare_hle();
}

// The Switch's tick: the counter the rewritten reads see (rt_ticks, kernel.c)
uint64_t native_ticks(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
#else
#include "rt.h"
bool native_wanted(void) { return false; }
void native_begin_boot(void) {}
bool native_on(void) { return false; }
void native_prepare(void) {}
void native_call(Cpu *c, uint64_t addr) { (void)c; (void)addr; }
void native_thread_end(void) {}
uint64_t native_ticks(void) { return 0; }
#endif
