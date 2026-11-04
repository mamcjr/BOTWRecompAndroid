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
typedef struct { int version; uint64_t addr, alt; uint32_t word; int (*fn)(Cpu *); const char *what; } Fix;
static const Fix fixes[] = {  // (canonical addresses, 0x7000000000 + the offset in main)
    {100, 0x70010c1aa0, 0x70010c1ad8, 0xf9400008, fix_clustered_100, "ClusteredRenderer teardown"},
    {100, 0x7000c4f4d4, 0, 0xf9412269, fix_ticks_100, "frame duration"},
    {100, 0x7000d5dce0, 0, 0xf9404500, fix_tag_null, "tag check, no target yet"},
    {160, 0x7001861440, 0x7001861470, 0xf9400008, fix_clustered_160, "ClusteredRenderer teardown"},
    {160, 0x700161495c, 0, 0xf9412668, fix_ticks_160, "frame duration"},
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
