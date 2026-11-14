// Device-specific performance profile: MediaTek Helio G95 (MT6769G), Mali-G76 MC4, 8 GB RAM.
//
// The goal is the highest frame rate on exactly this SoC, at the game's own resolution, without touching anything
// the player sees (no UI, no name changes). Everything here is invisible except in the frame time.
//
// What the SoC is:
//   2 x Cortex-A76 at up to 2.05 GHz (the cpufreq policy whose cpuinfo_max_freq is highest, CPU 6-7 on MT6769),
//     the only cores fast enough for the game's own thread and the command-list translation;
//   6 x Cortex-A55 at up to 2.0 GHz, fine for audio, input, the UI and the housekeeping;
//   Mali-G76 MC4 on the standard Android driver, where a timeline value read waits for the GPU (exec.c seq_thread).
//
// What this file does:
//   - finds the fast cluster at run time from /sys (cpuinfo_max_freq per CPU), not a hardcoded core list, so a
//     different kernel's numbering still works; on anything that is not a Helio G90 family SoC it changes nothing;
//   - pins the render thread (the async command-list translator: 3.5-5.5 ms of every frame) to the A76 cores when
//     plat_render_thread_pin asks (platform/android.c); the game's threads are left to the scheduler, which the
//     game's job system needs (LARECOMP_PIN's fast main thread raced it, 2026-10-16);
//   - sets a utilization floor (uclamp.min) for every thread the runtime starts, so the governor keeps the cores it
//     runs on at high clocks instead of idling them down between frames - with 8 GB of RAM and one game to run
//     there is no reason to save power in the middle of a frame;
//   - advises the kernel to keep the game's code and the runtime's tables resident (madvise WILLNEED), so no
//     mid-gameplay page faults turn into frame hitches.
//
// Everything can be turned off from the environment: LARECOMP_NO_G95=1 restores the stock behavior.

#define _GNU_SOURCE  // (cpu_set_t, sched_setaffinity, madvise)
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "rt.h"

#ifdef __ANDROID__

#define G95_MAX_CPUS 16

typedef struct {
    int detected;               // -1 not checked, 0 not a G95-class SoC, 1 yes
    int fast_cpu[G95_MAX_CPUS]; // the A76 cores, in order
    int nfast;
    long fast_khz, slow_khz;
} G95Profile;

static G95Profile g_g95 = {.detected = -1};

// The SoC is a Helio G90 family part when the board platform is mt6769 (G95 / G90T / G85) or mt6785 (G96): reading a
// system property needs no permission.
static bool g95_board_is_helio_g9x(void) {
    char buf[PROP_VALUE_MAX];
    const char *keys[] = {"ro.board.platform", "ro.mediatek.platform", "ro.soc.model", NULL};
    for (int i = 0; keys[i]; i++) {
        if (__system_property_get(keys[i], buf) <= 0) continue;
        if (!strncasecmp(buf, "mt6769", 6) || !strncasecmp(buf, "mt6785", 6)) return true;
    }
    return false;
}

// Reads each CPU's top frequency once: on the Helio G95 the A76 policy reports 2050000 kHz, the A55s 2000000 kHz -
// the highest-frequency minority of the cores is the fast cluster, whatever order this kernel numbers them in.
static void g95_find_clusters(G95Profile *p) {
    long khz[G95_MAX_CPUS] = {0};
    int n = 0;
    for (int c = 0; c < G95_MAX_CPUS; c++) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        FILE *f = fopen(path, "re");
        if (!f) break;  // (no such CPU: the end of the list)
        long v = 0;
        if (fscanf(f, "%ld", &v) == 1 && v > 0) khz[c] = v, n = c + 1;
        fclose(f);
    }
    for (int c = 0; c < n; c++)
        if (khz[c] > p->fast_khz) p->fast_khz = khz[c];
    if (p->fast_khz <= 0 || n == 0) return;
    for (int c = 0; c < n; c++) {
        if (khz[c] == p->fast_khz) p->fast_cpu[p->nfast++] = c;
        else if (khz[c] > p->slow_khz) p->slow_khz = khz[c];
    }
    // Only worth using with two real clusters and the fast one a minority: a single cluster (a TV box, an emulator
    // image) has nothing to pin to.
    if (p->nfast == 0 || p->nfast == n) p->nfast = 0;
}

const G95Profile *g95_profile(void) {
    if (g_g95.detected < 0) {
        g_g95.detected = 0;
        if (g95_board_is_helio_g9x()) {
            g95_find_clusters(&g_g95);
            if (g_g95.nfast > 0) g_g95.detected = 1;
        }
        LOG("[g95] %s: %d fast core%s at %ld kHz\n", g_g95.detected == 1 ? "Helio G90 family SoC" :
            "not a Helio G95-class SoC: stock behavior", g_g95.nfast, g_g95.nfast == 1 ? "" : "s", g_g95.fast_khz);
    }
    return &g_g95;
}

bool g95_on(void) { return g95_profile()->detected == 1 && getenv("LARECOMP_NO_G95") == NULL; }

// A utilization floor for the calling thread: whatever the governor thinks of the load, its cores run at least at
// this share of their top clock. SCHED_FLAG_UTIL_CLAMP_MIN keeps the policy and the priority; if the kernel or the
// cgroup refuses, that is logged once and the game runs exactly as before. (Called for every thread that starts
// here: the game's, the render thread's, audio's - with 8 GB of RAM and one game to run, there is no reason for
// the governor to idle a core down in the middle of a frame.)
bool set_uclamp_min(int want, const char *what) {
    struct {
        uint32_t size, policy;
        uint64_t flags;
        int32_t nice;
        uint32_t priority;
        uint64_t runtime, deadline, period;
        uint32_t util_min, util_max;
    } attr = {.size = 56, .flags = 0x08 | 0x10 | 0x20, .util_min = (uint32_t)want};  // (KEEP_POLICY | KEEP_PARAMS |
                                                                                    //  UTIL_CLAMP_MIN; struct ver. 1)
    long r = syscall(__NR_sched_setattr, 0, &attr, 0);
    static atomic_int logged;
    if (!atomic_exchange(&logged, 1))
        LOG("[g95] uclamp.min %d (%s): %s\n", want, what, r ? strerror(errno) : "ok");
    return r == 0;
}

// The render thread (vulkan/async.c) spends its whole frame translating Maxwell command lists into Vulkan. On this
// SoC that belongs on the A76 cores: left to the scheduler it can land on an A55 and the frame waits on it.
bool g95_pin_render_thread(void) {
    if (!g95_on()) return false;
    static int done = -1;
    if (done < 0) {
        const G95Profile *p = g95_profile();
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int i = 0; i < p->nfast; i++) CPU_SET(p->fast_cpu[i], &set);
        int r = sched_setaffinity(0, sizeof set, &set);
        done = r == 0 ? 1 : 0;
        LOG("[g95] render thread on the %d A76 core%s: %s\n", p->nfast, p->nfast == 1 ? "" : "s",
            r ? strerror(errno) : "ok");
    }
    return done == 1;
}

// A utilization floor for every thread that starts here, when the SoC is the one this profile is for. The stock
// experiment's LARECOMP_UCLAMP_MIN=<n> still wins over the default 512.
void g95_uclamp_thread(void) {
    if (!g95_on()) return;
    const char *e = getenv("LARECOMP_UCLAMP_MIN");
    int want = e && atoi(e) > 0 ? (atoi(e) > 1024 ? 1024 : atoi(e)) : 512;
    set_uclamp_min(want, "the G95 profile");
}

// The game's code and the runtime's tables stay resident: a little core faulting a page in takes a long walk
// through the kernel, and that is a hitch. Called for each module once it is in place (native.c native_prepare).
void g95_keep_resident(uint64_t lo, uint64_t size) {
    if (!g95_on()) return;
    static uint64_t advised_hi;
    uint64_t hi = lo + size;
    if (lo < advised_hi) lo = advised_hi;
    if (hi <= advised_hi) return;
    advised_hi = hi;
    madvise((void *)(uintptr_t)lo, hi - lo, MADV_WILLNEED);
    LOG("[g95] code and tables kept resident: %llx..%llx (%llu MB)\n", (unsigned long long)lo,
        (unsigned long long)hi, (unsigned long long)((hi - lo) >> 20));
}

#else  // !__ANDROID__

// (compiled out on the desktop builds: the whole file is about one phone)

#endif
