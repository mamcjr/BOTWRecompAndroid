// Policy and persistent worker pool for parallel CPU BC1-BC7 / BC1->ASTC texture decoding on BC-less GPUs.
// Included from runtime/nvn/vulkan/textures.c and tools/test_bc_parallel.c.
#pragma once

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#if !defined(VK_VERSION_1_0)
#include <vulkan/vulkan_core.h>
#endif

#ifdef __ANDROID__
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

enum {
    VK_BC_PARALLEL_DEFAULT_THREADS = 4,
    VK_BC_PARALLEL_MAX_THREADS = 8,
};

static inline int vk_bc_parallel_threads(const char *env_val, bool no_env, bool cpu_bc_device, bool *from_env) {
    if (from_env) *from_env = false;
    if (no_env) return 1;
    int def = cpu_bc_device ? VK_BC_PARALLEL_DEFAULT_THREADS : 1;
    if (env_val && *env_val) {
        char *end = NULL;
        errno = 0;
        long n = strtol(env_val, &end, 10);
        if (end == env_val) return def;
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
        if (*end != '\0') return def;
        if (from_env) *from_env = true;
        if (errno == ERANGE) return n == LONG_MIN ? 1 : VK_BC_PARALLEL_MAX_THREADS;
        if (n <= 1) return 1;
        if (n > VK_BC_PARALLEL_MAX_THREADS) return VK_BC_PARALLEL_MAX_THREADS;
        return (int)n;
    }
    return def;
}

static inline bool vk_bc_parallel_should_split(VkFormat bc, uint32_t wb, uint32_t hb, bool is_astc, int threads) {
    if (threads <= 1 || wb == 0 || hb == 0) return false;
    uint64_t blocks = (uint64_t)wb * hb;
    if (is_astc) return blocks >= 4096;
    bool heavy = bc == VK_FORMAT_BC6H_UFLOAT_BLOCK || bc == VK_FORMAT_BC6H_SFLOAT_BLOCK ||
                 bc == VK_FORMAT_BC7_UNORM_BLOCK || bc == VK_FORMAT_BC7_SRGB_BLOCK;
    if (heavy) return blocks >= 512 && hb >= 4;
    return blocks >= 2048 && hb >= 4;
}

static inline uint32_t vk_bc_chunk_size(uint32_t total, int threads, uint32_t min_chunk) {
    if (total == 0) return 0;
    if (min_chunk == 0) min_chunk = 1;
    if (threads <= 1) return total;
    uint64_t target = (uint64_t)(uint32_t)threads * 16u;
    uint32_t chunk = (uint32_t)(((uint64_t)total + target - 1u) / target);
    if (chunk < min_chunk) chunk = min_chunk;
    if (chunk > total) chunk = total;
    return chunk;
}

static inline uint64_t vk_bc_select_fast_cpus(const long *mhz, int n, uint64_t allowed_mask, int threads) {
    if (!mhz || n <= 0) return 0;
    if (n > 64) n = 64;
    long min_mhz = LONG_MAX, max_mhz = 0;
    int n_allowed = 0;
    for (int c = 0; c < n; c++) {
        if (!(allowed_mask & (1ull << c)) || mhz[c] <= 0) continue;
        if (mhz[c] < min_mhz) min_mhz = mhz[c];
        if (mhz[c] > max_mhz) max_mhz = mhz[c];
        n_allowed++;
    }
    if (n_allowed < 4 || min_mhz >= max_mhz) return 0;
    uint64_t fast = 0;
    int n_fast = 0;
    for (int c = 0; c < n; c++) {
        if ((allowed_mask & (1ull << c)) && mhz[c] > min_mhz) {
            fast |= 1ull << c;
            n_fast++;
        }
    }
    return (n_fast >= 3 && n_fast >= threads) ? fast : 0;
}

#ifdef __ANDROID__
extern void plat_thread_unpin(void) __attribute__((weak));
#endif

static inline void vk_bc_worker_setup(int threads) {
#ifdef __ANDROID__
    prctl(PR_SET_NAME, "vk-bc", 0, 0, 0);
    if (plat_thread_unpin) plat_thread_unpin();
    unsigned long allowed[16] = {0};
    if (syscall(__NR_sched_getaffinity, 0, sizeof allowed, allowed) < 0) return;
    uint64_t allowed_mask = 0;
    if (sizeof(unsigned long) == 8) {
        for (size_t i = 1; i < sizeof allowed / sizeof allowed[0]; i++) {
            if (allowed[i] != 0) return;
        }
        allowed_mask = (uint64_t)allowed[0];
    } else {
        for (size_t i = 2; i < sizeof allowed / sizeof allowed[0]; i++) {
            if (allowed[i] != 0) return;
        }
        allowed_mask = (uint64_t)allowed[0] | ((uint64_t)allowed[1] << 32);
    }
    long mhz[64] = {0};
    for (int c = 0; c < 64; c++) {
        if (!(allowed_mask & (1ull << c))) continue;
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        FILE *f = fopen(path, "re");
        if (!f) continue;
        if (fscanf(f, "%ld", &mhz[c]) != 1) mhz[c] = 0;
        fclose(f);
    }
    uint64_t fast = vk_bc_select_fast_cpus(mhz, 64, allowed_mask, threads);
    if (fast) {
        unsigned long mask[16] = {0};
        if (sizeof(unsigned long) == 8) {
            mask[0] = (unsigned long)fast;
        } else {
            mask[0] = (unsigned long)(uint32_t)fast;
            mask[1] = (unsigned long)(uint32_t)(fast >> 32);
        }
        syscall(__NR_sched_setaffinity, 0, sizeof mask, mask);
    }
#else
    (void)threads;
#endif
}

typedef void (*VkBcTaskFn)(void *ctx, uint32_t start, uint32_t count);

typedef struct VkBcPool VkBcPool;

typedef struct {
    VkBcPool *pool;
    int id;
    uint64_t start_gen;
    int threads;
} VkBcWorkerArg;

// Persistent worker pool for CPU BC texture decode; caller invokes vk_bc_pool_run from a single owner thread.
struct VkBcPool {
    pthread_mutex_t mu;
    pthread_cond_t work_cv;
    pthread_cond_t done_cv;
    bool init;
    bool init_failed;
    bool spawn_failed;
    bool stop;
    int nworkers;
    int threads;
    uint64_t gen;
    int remaining;
    _Atomic uint64_t next;
    uint32_t chunk;
    VkBcTaskFn fn;
    void *ctx;
    uint32_t total;
    pthread_t th[VK_BC_PARALLEL_MAX_THREADS - 1];
    VkBcWorkerArg warg[VK_BC_PARALLEL_MAX_THREADS - 1];
};

static inline void vk_bc_pool_drain(VkBcPool *p, VkBcTaskFn fn, void *ctx, uint32_t total, uint32_t chunk) {
    for (;;) {
        uint64_t s = atomic_fetch_add_explicit(&p->next, chunk, memory_order_relaxed);
        if (s >= (uint64_t)total) break;
        uint32_t c = (uint32_t)((uint64_t)total - s);
        if (c > chunk) c = chunk;
        fn(ctx, (uint32_t)s, c);
    }
}

static inline void *vk_bc_pool_worker(void *arg) {
    VkBcWorkerArg *wa = (VkBcWorkerArg *)arg;
    VkBcPool *p = wa->pool;
    int id = wa->id;
    uint64_t seen = wa->start_gen;
    vk_bc_worker_setup(wa->threads);
    pthread_mutex_lock(&p->mu);
    for (;;) {
        while (!p->stop && p->gen == seen) pthread_cond_wait(&p->work_cv, &p->mu);
        if (p->stop) break;
        seen = p->gen;
        if (id >= p->threads) continue;
        VkBcTaskFn fn = p->fn;
        void *ctx = p->ctx;
        uint32_t total = p->total;
        uint32_t chunk = p->chunk;
        pthread_mutex_unlock(&p->mu);

        vk_bc_pool_drain(p, fn, ctx, total, chunk);

        pthread_mutex_lock(&p->mu);
        if (--p->remaining == 0) pthread_cond_signal(&p->done_cv);
    }
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

static inline void vk_bc_pool_run(VkBcPool *p, VkBcTaskFn fn, void *ctx, uint32_t total, uint32_t min_chunk,
                                  int threads) {
    if (!fn || total == 0) return;
    if (threads > VK_BC_PARALLEL_MAX_THREADS) threads = VK_BC_PARALLEL_MAX_THREADS;
    if (!p || p->init_failed || threads <= 1 || total <= 1) {
        fn(ctx, 0, total);
        return;
    }
    if (!p->init) {
        if (pthread_mutex_init(&p->mu, NULL) != 0) {
            p->init_failed = true;
            fn(ctx, 0, total);
            return;
        }
        if (pthread_cond_init(&p->work_cv, NULL) != 0) {
            pthread_mutex_destroy(&p->mu);
            p->init_failed = true;
            fn(ctx, 0, total);
            return;
        }
        if (pthread_cond_init(&p->done_cv, NULL) != 0) {
            pthread_cond_destroy(&p->work_cv);
            pthread_mutex_destroy(&p->mu);
            p->init_failed = true;
            fn(ctx, 0, total);
            return;
        }
        p->init = true;
    }
    int want_workers = threads - 1;
    while (p->nworkers < want_workers && !p->spawn_failed) {
        int idx = p->nworkers;
        p->warg[idx] = (VkBcWorkerArg){.pool = p, .id = idx + 1, .start_gen = p->gen, .threads = threads};
        if (pthread_create(&p->th[idx], NULL, vk_bc_pool_worker, &p->warg[idx]) != 0) {
            p->spawn_failed = true;
            break;
        }
        p->nworkers++;
    }
    int active_threads = threads < p->nworkers + 1 ? threads : p->nworkers + 1;
    if (active_threads <= 1) {
        fn(ctx, 0, total);
        return;
    }
    uint32_t chunk = vk_bc_chunk_size(total, active_threads, min_chunk);

    pthread_mutex_lock(&p->mu);
    p->fn = fn;
    p->ctx = ctx;
    p->total = total;
    p->chunk = chunk;
    p->threads = active_threads;
    p->remaining = active_threads - 1;
    atomic_store_explicit(&p->next, 0, memory_order_relaxed);
    p->gen++;
    pthread_cond_broadcast(&p->work_cv);
    pthread_mutex_unlock(&p->mu);

    vk_bc_pool_drain(p, fn, ctx, total, chunk);

    pthread_mutex_lock(&p->mu);
    while (p->remaining > 0) pthread_cond_wait(&p->done_cv, &p->mu);
    pthread_mutex_unlock(&p->mu);
}

static inline void vk_bc_pool_destroy(VkBcPool *p) {
    if (!p) return;
    if (p->init) {
        pthread_mutex_lock(&p->mu);
        p->stop = true;
        pthread_cond_broadcast(&p->work_cv);
        pthread_mutex_unlock(&p->mu);
        for (int i = 0; i < p->nworkers; i++) pthread_join(p->th[i], NULL);
        pthread_cond_destroy(&p->work_cv);
        pthread_cond_destroy(&p->done_cv);
        pthread_mutex_destroy(&p->mu);
    }
    *p = (VkBcPool){0};
}
