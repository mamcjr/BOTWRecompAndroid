// Guest address -> recompiled function lookup, and host (HLE) function registry.
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "gamelib.h"
#include "hle.h"
#include "kernel.h"

typedef struct { const uint64_t *addrs; const GuestFn *fns; uint64_t count, lo, hi; } FnTable;
static FnTable tables[2];

// HLE stubs live at fake addresses HLE_BASE + 16*i
#define MAX_HLE 4096
typedef struct { const char *name; HostFn fn; } HleSlot;
static HleSlot hle_slots[MAX_HLE];
static int hle_count;

// Overrides of recompiled guest functions (by address)
#define MAX_OVERRIDE 1024
static struct { uint64_t addr; HostFn fn; const char *name; } overrides[MAX_OVERRIDE];
static int override_count;

// Host implementations registered by name (runtime/hle/*.c fill this via HLE_FUNC)
typedef struct { const char *name; HostFn fn; } HleImpl;
#ifdef __APPLE__
extern const HleImpl __start_hle_impls[] __asm__("section$start$__DATA$__hle_impls");
extern const HleImpl __stop_hle_impls[] __asm__("section$end$__DATA$__hle_impls");
#elif defined(__linux__)
extern const HleImpl __start_hle_impls[], __stop_hle_impls[];  // (ELF: defined by the linker)
#else
// PE/COFF: the linker sorts .hle$a < .hle$m (entries, hle.h) < .hle$z by the text after '$'.
__attribute__((section(".hle$a"), used)) static const HleImpl __start_hle_impls[1];
__attribute__((section(".hle$z"), used)) static const HleImpl __stop_hle_impls[1];
#endif

HostFn hle_lookup_impl(const char *name) {
    const char *skip = getenv("LARECOMP_HLE_SKIP");  // debugging: disable HLE for names containing this
    if (skip && strstr(name, skip)) return NULL;
    const HleImpl *start = __start_hle_impls, *stop = __stop_hle_impls;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const HleImpl *p = start; p < stop; p++)
        if (p->name && !strcmp(p->name, name)) return p->fn;
    return NULL;
}

static void hle_unimplemented(Cpu *c);

uint64_t hle_register(const char *name, HostFn fn) {
    for (int i = 0; i < hle_count; i++)
        if (!strcmp(hle_slots[i].name, name)) return HLE_BASE + 16ull * i;
    if (hle_count == MAX_HLE) FATAL("too many HLE stubs\n");
    hle_slots[hle_count].name = strdup(name);
    hle_slots[hle_count].fn = fn;
    return HLE_BASE + 16ull * hle_count++;
}

void hle_override(uint64_t addr, HostFn fn, const char *name) {
    if (override_count == MAX_OVERRIDE) FATAL("too many overrides\n");
    overrides[override_count].addr = addr;
    overrides[override_count].fn = fn;
    overrides[override_count].name = name;
    override_count++;
}

static __thread uint64_t unimpl_pc;
static int unimpl_counts[MAX_HLE];
// A call through a null function pointer (pc below 0x10000), most often one the null-page read just gave as zero
// (platform/android.c null_read: "the game's code read address 0x10") - returns 0, as the reads do. The Slate's
// camera, taking photos: a read at 0x10 and then a call to 0, FATAL "no function at 0" (#300, Recompiled mode).
// LARECOMP_NO_NULL_CALLS=1: the FATAL, as before.
static void null_call(Cpu *c) {
    static int n;
    if (n++ < 10) LOG("[dispatch] the game's code called address 0: returned 0 (lr %llx)\n", (unsigned long long)c->x[30]);
    c->x[0] = 0;
}

static void hle_unimplemented(Cpu *c) {
    int i = (int)((unimpl_pc - HLE_BASE) / 16);
    if (unimpl_counts[i]++ < 3)
        LOG("[hle] UNIMPLEMENTED %s (x0=%llx x1=%llx lr=%llx) -> returning 0\n", hle_slots[i].name, c->x[0],
            c->x[1], c->x[30]);
    c->x[0] = 0;
}

uint64_t guest_sym(const char *name) {
    // small cache: callers pass string literals, so pointer identity is a good key
    static struct { const char *name; uint64_t addr; } cache[64];
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64 && cache[i].name; i++)
        if (cache[i].name == name) { uint64_t a = cache[i].addr; pthread_mutex_unlock(&m); return a; }
    pthread_mutex_unlock(&m);
    uint64_t a = loader_find_symbol(name);
    if (!a) FATAL("guest symbol %s not found\n", name);
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64; i++) if (!cache[i].name) { cache[i].name = name; cache[i].addr = a; break; }
    pthread_mutex_unlock(&m);
    return a;
}

#ifdef __ANDROID__
static uint64_t hle_next;  // (HLE_HEAP moves with the window: the first hle_alloc sets it)
#else
static uint64_t hle_next = HLE_HEAP;
#endif
RT_ON_RESET(hle_heap_reset) { hle_next = HLE_HEAP; }  // (guest memory, wiped by the reboot)

void *hle_alloc(uint64_t size) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
#ifdef __ANDROID__
    if (!hle_next) hle_next = HLE_HEAP;
#endif
    uint64_t a = hle_next;
    hle_next += (size + 63) & ~63ull;
    if (hle_next > HLE_HEAP + HLE_HEAP_SIZE) FATAL("HLE heap exhausted\n");
    pthread_mutex_unlock(&m);
    plat_commit(a, size);
    memset((void *)a, 0, size);
    return (void *)a;
}

void dispatch_init(void) {  // (after gamelib_load)
    const GameLibTables *g = gamelib_tables();
    uint64_t nm = *g->main_count, ns = *g->sdk_count;
#ifdef __ANDROID__
    // The library lists the desktop window's addresses (gen.py --dyn-base): moved to where the window is (rt.h).
    static uint64_t *moved[2];
    const uint64_t *src[2] = {g->main_addrs, g->sdk_addrs}, cnt[2] = {nm, ns};
    for (int t = 0; t < 2; t++) {
        free(moved[t]);
        moved[t] = malloc(cnt[t] * sizeof *moved[t]);
        if (!moved[t]) FATAL("out of memory\n");
        for (uint64_t i = 0; i < cnt[t]; i++) moved[t][i] = src[t][i] - GUEST_CANON + GUEST_LO;
    }
    tables[0] = (FnTable){moved[0], g->main_fns, nm, moved[0][0], moved[0][nm - 1]};
    tables[1] = (FnTable){moved[1], g->sdk_fns, ns, moved[1][0], moved[1][ns - 1]};
#else
    tables[0] = (FnTable){g->main_addrs, g->main_fns, nm, g->main_addrs[0], g->main_addrs[nm - 1]};
    tables[1] = (FnTable){g->sdk_addrs, g->sdk_fns, ns, g->sdk_addrs[0], g->sdk_addrs[ns - 1]};
#endif
}

GuestFn dispatch_lookup(uint64_t addr) {
    for (int t = 0; t < 2; t++) {
        FnTable *ft = &tables[t];
        if (addr < ft->lo || addr > ft->hi) continue;
        uint64_t lo = 0, hi = ft->count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (ft->addrs[mid] < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < ft->count && ft->addrs[lo] == addr) return ft->fns[lo];
    }
    return NULL;
}

// Small direct-mapped cache in front of the binary search
#ifdef __ANDROID__
// (larger, and indexed with higher pc bits folded in: functions 16 KB apart evicted each other - about a fifth of
// rt_dispatch's samples were the binary search)
#define DCACHE 16384
#define DC_IDX(pc) ((unsigned)(((pc) >> 2) ^ ((pc) >> 16)) & (DCACHE - 1))
#else
#define DCACHE 4096
#define DC_IDX(pc) ((unsigned)((pc) >> 2) & (DCACHE - 1))
#endif
typedef struct { uint64_t addr; GuestFn fn; } DcEnt;
static __thread DcEnt dcache[DCACHE];
#ifdef __ANDROID__
// A guest thread's cache lives in its KThread (Cpu.thread): the runtime is a dlopened library on Android, where
// every __thread access goes through the dynamic TLS resolver - with one per indirect call that was ~5% of the
// game thread (simpleperf). Other threads (no KThread) keep the TLS one.
static DcEnt *dc_of(Cpu *c) {
    KThread *t = c->thread;
    if (!t) return dcache;
    if (!t->dcache) t->dcache = calloc(DCACHE, sizeof(DcEnt));
    return t->dcache ? (DcEnt *)t->dcache : dcache;
}
#endif

static GuestFn dispatch_slow(Cpu *c) {
    uint64_t pc = c->pc;
    if (pc >= HLE_BASE && pc < HLE_BASE + HLE_SIZE) {
        rt_unwind_point();  // (HLE calls aren't cached below, so every one passes here: a reboot's safe point)
        int i = (int)((pc - HLE_BASE) / 16);
        if (i < hle_count) {
            if (hle_slots[i].fn) return hle_slots[i].fn;
            unimpl_pc = pc;
            return hle_unimplemented;
        }
    }
    for (int i = 0; i < override_count; i++)
        if (overrides[i].addr == pc) return overrides[i].fn;
    GuestFn f = dispatch_lookup(pc);
    if (!f && pc < 0x10000 && !getenv("LARECOMP_NO_NULL_CALLS")) return null_call;  // (not cached: rare)
    if (!f) {
        uint64_t off;
        const char *s = loader_symbol_at(c->x[30], &off);
        FATAL("no function at %llx (lr=%llx %s+%llx)\n", pc, c->x[30], s ? s : "?", off);
    }
    unsigned h = DC_IDX(pc);
#ifdef __ANDROID__
    DcEnt *dc = dc_of(c);
    dc[h].addr = pc;
    dc[h].fn = f;
#else
    dcache[h].addr = pc;
    dcache[h].fn = f;
#endif
    return f;
}

#ifdef __ANDROID__
__attribute__((noinline)) static void dispatch_miss(Cpu *c) {
    GuestFn f = dispatch_slow(c);
    __attribute__((musttail)) return f(c);
}
#endif

// Always tail-calls, so chains of indirect jumps don't grow the host stack.
#ifdef __ANDROID__
// (the hit path without a stack frame or stack protector: the miss and the TLS cache are dispatch_miss's)
__attribute__((no_stack_protector))
#endif
void rt_dispatch(Cpu *c) {
    unsigned h = DC_IDX(c->pc);
#ifdef __ANDROID__
    KThread *t = c->thread;
    const DcEnt *dc;
    if (__builtin_expect(t && (dc = t->dcache) && dc[h].addr == c->pc, 1)) {
        GuestFn f = dc[h].fn;
        __attribute__((musttail)) return f(c);
    }
    __attribute__((musttail)) return dispatch_miss(c);
#else
    GuestFn f = dcache[h].addr == c->pc ? dcache[h].fn : dispatch_slow(c);
    __attribute__((musttail)) return f(c);
#endif
}

// The host function at an HLE address (native.c native_hle_c: the slots are code there)
HostFn hle_slot_fn(uint64_t pc) {
    int i = (int)((pc - HLE_BASE) / 16);
    if (i < 0 || i >= hle_count) FATAL("[native] call to HLE slot %d of %d\n", i, hle_count);
    if (hle_slots[i].fn) return hle_slots[i].fn;
    unimpl_pc = pc;
    return hle_unimplemented;
}

void guest_call(Cpu *c, uint64_t addr) {
    if (native_on()) { native_call(c, addr); return; }
    uint64_t lr = c->x[30];
    c->x[30] = 0;
    c->pc = addr;
    rt_dispatch(c);
    c->x[30] = lr;
}

// Host pc -> recompiled guest function containing it ("f_<addr>"), for hosts without symbol lookup.
static int cmp_host(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) {
    static uint64_t (*byhost)[2];  // {host address, guest address}, sorted by host address
    static uint64_t n;
    static __thread char name[32];
    if (!byhost) {
        uint64_t total = tables[0].count + tables[1].count, k = 0;
        uint64_t (*v)[2] = malloc(total * sizeof *v);
        if (!v) return NULL;
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < tables[t].count; i++) {
                v[k][0] = (uint64_t)(uintptr_t)tables[t].fns[i];
                v[k++][1] = tables[t].addrs[i];
            }
        qsort(v, k, sizeof *v, cmp_host);
        n = k;
        byhost = v;
    }
    uint64_t lo = 0, hi = n;  // last entry with host address <= pc
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (byhost[mid][0] <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || pc - byhost[lo - 1][0] > 0x100000) return NULL;
    snprintf(name, sizeof name, "f_%llx", (unsigned long long)byhost[lo - 1][1]);
    if (offset) *offset = pc - byhost[lo - 1][0];
    return name;
}

static const char *host_sym(uint64_t pc, uint64_t *offset) {
    // (game functions first: they live in botwgame.dll, which the OS lookup can only name as a whole)
    const char *s = rt_host_fn_name(pc, offset);
    return s ? s : plat_symbol(pc, offset);
}

// One crash report at a time: two of the game's threads crashed together (issue #24: the same null pointer, 33 ms
// apart), their lines interleaved, and the first one's abort cut both off in the host backtrace, before the guest
// registers. Another thread waits here for that abort; the same thread (a fault inside its own report) goes on.
static void fatal_gate(void) {
    static uintptr_t owner;  // (plain: clang's __atomic builtins take no _Atomic operand)
    uintptr_t me = (uintptr_t)pthread_self(), none = 0;
    if (__atomic_compare_exchange_n(&owner, &none, me, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) || none == me) return;
    for (;;) plat_sleep_until_ns(plat_now_ns() + 1000000000ull);
}

void rt_fatal_trace(void) {
    fatal_gate();
    Cpu *c = kernel_current_cpu();
    if (c) {  // (first: the host backtrace below is slow, and the guest's registers name the game's caller)
        fprintf(stderr, "guest: sp=%llx lr=%llx\n", c->sp, c->x[30]);
        for (int i = 0; i < 31; i += 4)
            fprintf(stderr, "  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", i, c->x[i], i + 1,
                    c->x[i + 1], i + 2, c->x[i + 2], i + 3, c->x[i + 3]);
        // the game's callers, by its frame chain (x29: [fp] the caller's frame, [fp + 8] the return address) - they map
        // to the game's code without the phone's own build of it. (Once: a fault while walking it ends up here again.)
        static volatile int walked;
        if (!walked++) {
            uint64_t fp = c->x[29];
            fprintf(stderr, "guest frames:");
            for (int i = 0; i < 24 && fp >= GUEST_LO && fp + 16 <= GUEST_HI && !(fp & 7); i++) {
                uint64_t next = *(const uint64_t *)(uintptr_t)fp, ret = *(const uint64_t *)(uintptr_t)(fp + 8);
                fprintf(stderr, " %llx", (unsigned long long)ret);
                if (next <= fp) break;
                fp = next;
            }
            fprintf(stderr, "\n");
        }
    }
    fprintf(stderr, "host backtrace (guest functions are f_<addr>):\n");
    plat_print_backtrace();
    if (c) {
        // LARECOMP_CRASH_MEM=<reg>:<hex offset>:<hex length>[:<hex deref offset>]: guest memory at x<reg> + offset at
        // the crash (with a 4th field: first follow the pointer at x<reg> + that offset), 8 bytes a line
        const char *e = getenv("LARECOMP_CRASH_MEM");
        unsigned reg = 0;
        unsigned long long off = 0, len = 0, deref = ~0ull;
        if (e && sscanf(e, "%u:%llx:%llx:%llx", &reg, &off, &len, &deref) >= 3 && reg < 31) {
            uint64_t base = c->x[reg];
            if (deref != ~0ull && base >= GUEST_LO && base + deref < GUEST_HI) base = *(const uint64_t *)(uintptr_t)(base + deref);
            fprintf(stderr, "guest memory at %llx + %#llx:\n", (unsigned long long)base, off);
            for (unsigned long long k = 0; k < len && base + off + k + 8 <= GUEST_HI && base + off >= GUEST_LO; k += 8)
                fprintf(stderr, "  +%#06llx %016llx\n", off + k, (unsigned long long)*(const uint64_t *)(uintptr_t)(base + off + k));
        }
    }
    plat_fatal_notice();
    // our crash handler also catches SIGABRT: without this abort() re-entered it forever (a 5 GB log)
    signal(SIGABRT, SIG_DFL);
    abort();
}

void rt_udf(Cpu *c, uint64_t pc) {
    (void)c;
    FATAL("undefined instruction / trap at %llx\n", pc);
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    FATAL("brk #%u at %llx\n", imm, pc);
}

// ---- crash reporting

// Debug data watchpoint: the page is protected; accesses in [lo, hi) are attributed to the host
// function (f_<guest addr>) doing them, then the page is reopened until the next re-arm.
uint64_t g_watch_lo, g_watch_hi;
static uint64_t watch_page;
static void *watch_thread(void *a) {
#ifdef __ANDROID__
    plat_thread_unpin();  // (not the presenting thread's fast cores: platform.h)
#endif
    (void)a;
    int r = plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_NONE);
    fprintf(stderr, "[watch] page %llx protect -> %d\n", (unsigned long long)watch_page, r);
    for (;;) { plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_NONE); usleep(100); }
    return NULL;
}
void rt_range_watch(uint64_t lo, uint64_t hi);
void watch_arm(void) {
    {   // LARECOMP_WATCH_READ_ADDR=<hex>:<len>[+<hex>:<len>...]: read watch over those guest bytes (rt_range_watch)
        static int done;
        const char *e = getenv("LARECOMP_WATCH_READ_ADDR");
        if (e && !done) {
            done = 1;
            while (*e) {
                char *q;
                uint64_t a = strtoull(e, &q, 16), n = *q == ':' ? strtoull(q + 1, &q, 0) : 8;
                rt_range_watch(a, a + n);
                e = *q == '+' ? q + 1 : q;
                if (*q != '+') break;
            }
        }
    }
    static int env_done;
    if (!env_done) {  // LARECOMP_WATCH=<offset in main, hex>[,<length>]: watch those bytes of main's image
        env_done = 1;
        const char *e = getenv("LARECOMP_WATCH");
        if (e && !g_watch_lo) {
            char *q;
            uint64_t off = strtoull(e, &q, 16), len = *q == ',' ? strtoull(q + 1, NULL, 0) : 8;
            g_watch_lo = MAIN_BASE + off, g_watch_hi = g_watch_lo + len;
            LOG("[watch] main+%llx (%llu bytes) at %llx\n", (unsigned long long)off, (unsigned long long)len,
                (unsigned long long)g_watch_lo);
        }
    }
    if (!g_watch_lo || watch_page) return;
    watch_page = g_watch_lo & ~(PLAT_PAGE - 1);
    pthread_t t;
    pthread_create(&t, NULL, watch_thread, NULL);
}

// Read watch over a whole range (rt_range_watch: e.g. a texture the GPU renders into, LARECOMP_WATCH_READ_TEX in
// nvn/objects.c): its pages are closed, each access is attributed once per function, the faulting page reopens and a
// thread closes the range again every 50 ms.
#define RW_MAX 16
static uint64_t rw_lo[RW_MAX], rw_hi[RW_MAX], rw_exact_lo[RW_MAX], rw_exact_hi[RW_MAX];
static int rw_n;
static volatile int rw_armed;
static void *range_watch_thread(void *a) {
#ifdef __ANDROID__
    plat_thread_unpin();
#endif
    (void)a;  // (armed late: the loader fills it through the kernel too, which would just fail - LARECOMP_WATCH_READ_DELAY s)
    const char *d = getenv("LARECOMP_WATCH_READ_DELAY");
    sleep(d ? atoi(d) : 60);
    LOG("[rwatch] armed (%d ranges)\n", rw_n);
    rw_armed = 1;
    const char *pe = getenv("LARECOMP_WATCH_READ_PERIOD_US");  // (a busy page reopens at once: re-close it more often)
    long period = pe ? atol(pe) : 50000;
    for (;;) {
        for (int i = 0; i < rw_n; i++) plat_protect(rw_lo[i], rw_hi[i] - rw_lo[i], PLAT_PROT_NONE);
        if (period > 0) usleep((useconds_t)period); else sched_yield();
    }
    return NULL;
}
void rt_range_watch(uint64_t lo, uint64_t hi) {
    uint64_t elo = lo, ehi = hi;
    lo &= ~(uint64_t)(PLAT_PAGE - 1), hi = (hi + PLAT_PAGE - 1) & ~(uint64_t)(PLAT_PAGE - 1);
    for (int i = 0; i < rw_n; i++)
        if (rw_lo[i] == lo) return;
    if (rw_n == RW_MAX || rw_armed) return;  // (a range added after arming: its pages may be the loader's right now)
    rw_lo[rw_n] = lo, rw_hi[rw_n] = hi, rw_exact_lo[rw_n] = elo, rw_exact_hi[rw_n] = ehi;
    LOG("[rwatch] range %d: %llx..%llx\n", rw_n, (unsigned long long)lo, (unsigned long long)hi);
    if (rw_n++ == 0) {
        pthread_t t;
        pthread_create(&t, NULL, range_watch_thread, NULL);
    }
}
int g_fault_wnr = -1;  // (set by the platform's handler when it knows: 1 a write, 0 a read)
static bool range_watch_fault(uint64_t fa, uint64_t pc) {
    int r = -1;
    for (int i = 0; i < rw_n; i++)
        if (fa >= rw_lo[i] && fa < rw_hi[i]) { r = i; break; }
    if (r < 0) return false;
    if (fa < rw_exact_lo[r] || fa >= rw_exact_hi[r]) {  // (a neighbour on the same page: just reopen it)
        plat_protect(fa & ~(uint64_t)(PLAT_PAGE - 1), PLAT_PAGE, PLAT_PROT_RW);
        return true;
    }
    static char seen[128][48];
    static int nseen, nhits;
    const char *nm = host_sym(pc, NULL);
    if (!nm) nm = "?";
    nhits++;
    bool found = false;
    char key[48];
    snprintf(key, sizeof key, "%.40s/%d/%c", nm, r, g_fault_wnr == 1 ? 'w' : g_fault_wnr == 0 ? 'r' : '?');
    for (int i = 0; i < nseen; i++) found |= !strcmp(seen[i], key);
    if (!found && nseen < 128) {
        snprintf(seen[nseen++], 48, "%s", key);
        KThread *kt = kernel_current_thread();
        uint64_t lib = 0;
#ifndef _WIN32
        Dl_info di;
        if (dladdr((void *)(uintptr_t)pc, &di)) lib = pc - (uint64_t)(uintptr_t)di.dli_fbase;
#endif
        LOG("[rwatch] range %d +%#llx %s by %s (host pc %llx, library +%llx) thread t%llu hit %d\n", r, (unsigned long long)(fa - rw_exact_lo[r]),
            g_fault_wnr == 1 ? "write" : g_fault_wnr == 0 ? "read" : "access", nm, (unsigned long long)pc, (unsigned long long)lib, (unsigned long long)(kt ? kt->id : 0), nhits);
        if (getenv("LARECOMP_WATCH_REGS")) {  // (the guest registers at that access: x0-x30)
            Cpu *gc = kernel_current_cpu();
            for (int i = 0; gc && i < 31; i += 4)
                LOG("[rwatch]   x%-2d %016llx x%-2d %016llx x%-2d %016llx x%-2d %016llx\n", i, (unsigned long long)gc->x[i], i + 1,
                    (unsigned long long)gc->x[i + 1], i + 2, (unsigned long long)gc->x[i + 2], i + 3,
                    (unsigned long long)(i + 3 < 31 ? gc->x[i + 3] : 0));
        }
    }
    plat_protect(fa & ~(uint64_t)(PLAT_PAGE - 1), PLAT_PAGE, PLAT_PROT_RW);
    return true;
}

bool rt_fault_watch(uint64_t fa, uint64_t pc) {
    if (range_watch_fault(fa, pc)) return true;
    if (!watch_page || fa < watch_page || fa >= watch_page + PLAT_PAGE) return false;
    if (fa >= g_watch_lo && fa < g_watch_hi) {
        static char seen[64][48];
        static int nseen;
        const char *nm = host_sym(pc, NULL);
        if (!nm) nm = "?";
        char key[48];  // (each function + offset once)
        snprintf(key, sizeof key, "%.36s+%llx", nm, (unsigned long long)(fa - g_watch_lo));
        bool found = false;
        for (int i = 0; i < nseen; i++) found |= !strcmp(seen[i], key);
        if (!found && nseen < 64) {
            snprintf(seen[nseen++], 48, "%s", key);
            KThread *kt = kernel_current_thread();
            uint64_t lib = 0;
#ifndef _WIN32
            Dl_info di;
            if (dladdr((void *)(uintptr_t)pc, &di)) lib = pc - (uint64_t)(uintptr_t)di.dli_fbase;
#endif
            LOG("[watch] +%#llx by %s (host pc %llx, library +%llx) thread t%llu\n", (unsigned long long)(fa - g_watch_lo), nm,
                (unsigned long long)pc, (unsigned long long)lib, (unsigned long long)(kt ? kt->id : 0));
        }
    }
    plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_RW);
    return true;
}

void rt_fault_report(const char *what, uint64_t fa, uint64_t pc, uint64_t lr) {
    fatal_gate();
    uint64_t po = 0, lo = 0;
    const char *ps = host_sym(pc, &po), *ls = host_sym(lr, &lo);
    if (pc >= GUEST_LO && pc < GUEST_HI) {  // (the game's own code, LARECOMP_NATIVE: named by the modules' symbols)
        ps = loader_symbol_at(pc, &po);
        if (lr >= GUEST_LO && lr < GUEST_HI) ls = loader_symbol_at(lr, &lo);
        fprintf(stderr, "\n*** %s accessing %#llx in the game's code\n    guest pc %llx (main+%llx, %s+%llx)\n",
                what, (unsigned long long)fa, (unsigned long long)pc, (unsigned long long)(pc - MAIN_BASE), ps ? ps : "?",
                (unsigned long long)po);
    }
    fprintf(stderr, "\n*** %s accessing %#llx\n    host pc %llx (%s+%llx)\n    host lr %llx (%s)\n", what,
            (unsigned long long)fa, (unsigned long long)pc, ps ? ps : "?", (unsigned long long)po,
            (unsigned long long)lr, ls ? ls : "?");
    rt_fatal_trace();
}

void crash_handler_install(void) { plat_crash_handler_install(); }
