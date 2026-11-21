// Native Android unit tests for opt-in Vulkan pipeline prewarming (#294).
// Builds directly against runtime/nvn/vulkan/pipeline_prewarm.inc without game files or a Vulkan driver.
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include "nvn/vulkan/object_cache.h"
#include "settings.h"

#define LOG(...) ((void)0)
#define ZERO_BINDING 16

typedef struct {
    uint64_t vs_h, fs_h, key_h;
    uint32_t nv, nf;
} PwProgId;

typedef struct {
    PwProgId pw_id;
    VkPipelineLayout layout;
    _Atomic int state;
    uint32_t used_s;
    uint64_t used_seq;
} Prog;

typedef struct {
    uint8_t on, cull, ff, topo, dt, dw, dop, sten;
    uint8_t sop[2][4];
} PipeFx;

typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill, depth_clamp;
    uint8_t prim_restart;
    uint32_t write_mask[8];
    PipeFx fx;
} PipeKey;

typedef struct {
    PipeKey k;
    uint64_t hash;
    VkPipeline pipe;
    _Atomic int state;
    uint32_t used_s;
    uint64_t seq;
    bool evicted;
    bool prewarmed;
} Pipe;

static struct {
    VkDevice dev;
    VkPhysicalDeviceProperties props;
    bool dyn_render, eds, eds2, divisor, exec_stats, dyn_vi, local_read;
} g_vk = {
    .props = {.vendorID = 0x13b5, .deviceID = 0xd8300010, .driverVersion = 49,
              .pipelineCacheUUID = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}},
    .dyn_render = true,
    .eds = true,
    .eds2 = true,
    .divisor = true,
};

Settings g_settings;
int g_game_version = 190;
static volatile uint64_t g_scene_frame;
static uint32_t g_exec_s = 10;
static _Atomic int live_gpipes;
static _Atomic size_t pipeline_keys;
static VkObjectCache prog_cache, pipe_cache;

static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tjobs_lock = PTHREAD_MUTEX_INITIALIZER;
static void *jobs_head, *tjobs_head;
static unsigned jobs_active, jobs_workers = 2, tjobs_active;
static bool jobs_started = true;

static bool mock_title, mock_menu, mock_event, mock_sync_shaders, mock_enqueue_busy;
static int mock_reboot, mock_enqueue_calls, mock_run_job_calls, mock_destroy_pipe_calls;
static uint64_t mock_frame = 100, mock_now_ns = 1000000000ull, mock_pend_seq = 1;
static int mock_pipe_keep = 1500;
enum { MG_GPIPE = 2 };
static long long vk_mnow(void) { return 0; }
static void vk_mgrow_add(int w, long long s) { (void)w; (void)s; }
static void vkDestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) {
    (void)d; (void)p; (void)a;
    mock_destroy_pipe_calls++;
}

static _Atomic int pw_env = -2;
static bool prewarm_enabled(void) {
    int e = atomic_load_explicit(&pw_env, memory_order_relaxed);
    if (e == -2) {
        const char *v = getenv("LARECOMP_PIPE_PREWARM");
        e = (v && *v) ? (atoi(v) != 0) : -1;
        atomic_store_explicit(&pw_env, e, memory_order_relaxed);
    }
    return e >= 0 ? e != 0 : g_settings.pipe_prewarm;
}

static bool sync_shaders(void) { return mock_sync_shaders; }
bool framepace_on_title(void) { return mock_title; }
bool framepace_in_menu(void) { return mock_menu; }
bool framepace_in_event(void) { return mock_event; }
static int rt_reboot_requested(void) { return mock_reboot; }
static uint64_t plat_now_ns(void) { return mock_now_ns; }
static uint64_t renderer_present_count(void) { return mock_frame; }
static uint64_t vk_pending_seq(void) { return mock_pend_seq; }
static int pipe_keep(void) { return mock_pipe_keep; }
static uint64_t SDL_GetTicksNS(void) { return mock_now_ns; }

static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) {
        uint64_t w;
        memcpy(&w, b, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; n; n--, b++) h = (h ^ *b) * 0x100000001B3ull;
    return h ^ (h >> 32);
}

static void build_pipeline(void *arg) { (void)arg; }
static bool enqueue_job(void (*fn)(void *), void *arg, bool idle_only) {
    (void)fn; (void)arg; (void)idle_only;
    if (mock_enqueue_busy) return false;
    mock_enqueue_calls++;
    return true;
}
static void run_job(void (*fn)(void *), void *arg) {
    (void)fn; (void)arg;
    mock_run_job_calls++;
}

#include "nvn/vulkan/pipeline_prewarm.inc"

static Prog make_prog(uint64_t id) {
    Prog pr = {0};
    pr.pw_id = (PwProgId){.vs_h = id, .fs_h = id ^ 0x55aa55aaull, .key_h = id * 3u, .nv = 64, .nf = 96};
    pr.layout = (VkPipelineLayout)(uintptr_t)0x1234;
    atomic_store(&pr.state, 1);
    return pr;
}

static PipeKey make_key(Prog *pr, uint32_t stride0) {
    PipeKey k = {0};
    k.prog = pr;
    k.color[0] = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    k.depth = VK_FORMAT_D32_SFLOAT;
    k.ncolor = 1;
    k.samples = 1;
    k.attr_mask = 1;
    k.attr_fmt[0] = VK_FORMAT_R32G32B32_SFLOAT;
    k.attr_bind[0] = 0;
    k.used_streams = 1;
    k.stride[0] = stride0;
    k.topo_class = 2;
    k.poly_fill = 1;
    k.write_mask[0] = 0xF;
    return k;
}

static void free_caches(void) {
    for (size_t i = 0; i < pipe_cache.capacity; i++) free(pipe_cache.entries[i].object);
    free(pipe_cache.entries);
    pipe_cache = (VkObjectCache){0};
    free(prog_cache.entries);
    prog_cache = (VkObjectCache){0};
    atomic_store(&pipeline_keys, 0);
}

int main(void) {
    char tmpdir[] = "/data/local/tmp/pw_unit_XXXXXX";
    char cwd[PATH_MAX];
    assert(getcwd(cwd, sizeof cwd) != NULL);
    mkdir("/data/local/tmp", 0755);
    assert(mkdtemp(tmpdir) != NULL);
    assert(chdir(tmpdir) == 0);

    // 1. Default-off & env override
    unsetenv("LARECOMP_PIPE_PREWARM");
    g_settings.pipe_prewarm = false;
    atomic_store(&pw_env, -2);
    assert(!prewarm_enabled());
    g_settings.pipe_prewarm = true;
    assert(prewarm_enabled());
    setenv("LARECOMP_PIPE_PREWARM", "0", 1);
    atomic_store(&pw_env, -2);
    assert(!prewarm_enabled());
    setenv("LARECOMP_PIPE_PREWARM", "1", 1);
    g_settings.pipe_prewarm = false;
    atomic_store(&pw_env, -2);
    assert(prewarm_enabled());

    // 2. World-only recording + deduplication + cap
    setenv("LARECOMP_PIPE_PREWARM_MAX", "4", 1);
    prewarm_reset();
    Prog p1 = make_prog(0x1111), p2 = make_prog(0x2222);
    PipeKey k1 = make_key(&p1, 16), k2 = make_key(&p1, 24), k3 = make_key(&p2, 32), k4 = make_key(&p2, 40), k5 = make_key(&p2, 48);

    mock_title = true;
    g_scene_frame = 0;
    prewarm_record(&k1);  // title -> ignored
    assert(pw_count == 0);
    mock_title = false;
    g_scene_frame = 110;
    mock_frame = 150;
    prewarm_record(&k1);  // stale scene -> ignored
    assert(pw_count == 0);

    g_scene_frame = 149;  // in world
    prewarm_record(&k1);
    prewarm_record(&k1);  // duplicate ignored
    prewarm_record(&k2);
    prewarm_record(&k3);
    prewarm_record(&k4);
    prewarm_record(&k5);  // capped at 4
    assert(pw_count == 4 && pw_dirty);
    assert(prewarm_save());
    assert(!pw_dirty);

    // 3. Round-trip + malformed/tampered binary rejection
    FILE *f = fopen(PW_PATH, "rb");
    assert(f != NULL);
    uint8_t raw[16384];
    size_t raw_n = fread(raw, 1, sizeof raw, f);
    fclose(f);
    assert(raw_n == sizeof(PwHeader) + 4 * sizeof(PwRecord));

    uint8_t work[16384];
    assert(prewarm_parse_and_validate(raw, 0) == 0);
    assert(prewarm_parse_and_validate(raw, sizeof(PwHeader) - 1) == 0);
    assert(prewarm_parse_and_validate(raw, raw_n - 1) == 0);
    memcpy(work, raw, raw_n);
    work[raw_n] = 0;
    assert(prewarm_parse_and_validate(work, raw_n + 1) == 0);
    memcpy(work, raw, raw_n); work[0] = 'X';
    assert(prewarm_parse_and_validate(work, raw_n) == 0);
    memcpy(work, raw, raw_n); work[8] ^= 1;  // version
    assert(prewarm_parse_and_validate(work, raw_n) == 0);
    memcpy(work, raw, raw_n); work[offsetof(PwHeader, baked_size)] ^= 1;
    assert(prewarm_parse_and_validate(work, raw_n) == 0);
    memcpy(work, raw, raw_n); work[offsetof(PwHeader, driver_version)] ^= 1;
    assert(prewarm_parse_and_validate(work, raw_n) == 0);
    memcpy(work, raw, raw_n); work[offsetof(PwHeader, cache_uuid)] ^= 1;
    assert(prewarm_parse_and_validate(work, raw_n) == 0);
    memcpy(work, raw, raw_n); work[sizeof(PwHeader)] ^= 1;  // payload hash mismatch
    assert(prewarm_parse_and_validate(work, raw_n) == 0);

    // Corrupted payload WITH recomputed valid hash: reject out-of-range samples & duplicates
    memcpy(work, raw, raw_n);
    PwRecord *recs = (PwRecord *)(work + sizeof(PwHeader));
    recs[1] = recs[0];  // duplicate record
    PwHeader *hdr = (PwHeader *)work;
    hdr->payload_hash = hash_words(0xcbf29ce484222325ull, recs, 4 * sizeof(PwRecord));
    assert(prewarm_parse_and_validate(work, raw_n) == 0);

    memcpy(work, raw, raw_n);
    recs = (PwRecord *)(work + sizeof(PwHeader));
    PipeKey bad_k = {0};
    memcpy(&bad_k.color, recs[0].baked, PW_BAKED_SIZE);
    bad_k.samples = 3;  // invalid MSAA count
    memcpy(recs[0].baked, &bad_k.color, PW_BAKED_SIZE);
    hdr = (PwHeader *)work;
    hdr->payload_hash = hash_words(0xcbf29ce484222325ull, recs, 4 * sizeof(PwRecord));
    assert(prewarm_parse_and_validate(work, raw_n) == 0);

    // Toggling local_read invalidates cached prewarm header via vk_flags bit 32
    g_vk.local_read = true;
    assert(prewarm_parse_and_validate(raw, raw_n) == 0);
    g_vk.local_read = false;

    // Valid reload from file
    prewarm_reset();
    prewarm_load();
    assert(pw_count == 4);

    // 4. Loading window + unpause suppression + workers busy + 1-in-flight + headroom
    mock_frame = 160;
    mock_title = false;
    prewarm_tick();
    assert(!pw_loading);  // before title screen: never misclassified as loading

    mock_title = true;
    mock_frame = 170;
    prewarm_tick();
    assert(pw_saw_title && !pw_loading);

    // Register p1, p2 in prog_cache as ready
    vk_object_slot(&prog_cache, 10)->object = &p1;
    vk_object_slot(&prog_cache, 20)->object = &p2;

    // Leave title -> loading screen, test busy-worker guard
    mock_title = false;
    mock_enqueue_busy = true;
    mock_frame = 171;
    prewarm_tick();
    assert(pw_loading);
    assert(pipe_cache.count == 0 && pw_pending == NULL);  // no NULL entry leaked on busy!

    // Unblock worker -> queue 1, hold while state==0, queue 2nd once ready
    mock_enqueue_busy = false;
    mock_frame = 172;
    prewarm_tick();
    assert(mock_enqueue_calls == 1 && pipe_cache.count == 1 && pw_pending != NULL);
    Pipe *first = pw_pending;

    mock_frame = 173;
    prewarm_tick();
    assert(mock_enqueue_calls == 1 && pipe_cache.count == 1);  // still pending -> blocked!

    atomic_store(&first->state, 1);
    mock_frame = 174;
    prewarm_tick();
    assert(mock_enqueue_calls == 2 && pipe_cache.count == 2 && pw_pending != first);
    Pipe *second = pw_pending;
    atomic_store(&second->state, 2);

    // Headroom guard on Mali (keep=1500, headroom=256 -> stop at 1244)
    atomic_store(&live_gpipes, 1244);
    mock_pipe_keep = 1500;
    mock_frame = 175;
    prewarm_tick();
    assert(mock_enqueue_calls == 2 && pipe_cache.count == 2);

    // Uncapped Adreno (keep=0) -> queues cleanly even when live_gpipes is high
    mock_pipe_keep = 0;
    mock_frame = 176;
    prewarm_tick();
    assert(mock_enqueue_calls == 3 && pipe_cache.count == 3);
    atomic_store(&pw_pending->state, 1);

    // First-draw hits & failure fallback
    assert(first->prewarmed && second->prewarmed);
    prewarm_first_draw(first);
    assert(!first->prewarmed && pw_ready_boot == 1 && mock_run_job_calls == 0);
    prewarm_first_draw(second);
    assert(!second->prewarmed && atomic_load(&second->state) == 0 && mock_run_job_calls == 1);

    // Enter world closes loading cleanly
    mock_frame = 177;
    g_scene_frame = 177;
    prewarm_tick();
    assert(!pw_loading);

    // Unpause (menu=1 -> menu=0 with stale g_scene_frame) must NOT start loading
    mock_menu = true;
    mock_frame = 200;
    prewarm_tick();
    assert(!pw_loading);
    mock_menu = false;
    mock_frame = 201;
    prewarm_tick();
    assert(!pw_loading);

    // Fast-travel loading (non-menu frame after unpause with still-stale scene) starts loading cleanly
    mock_frame = 202;
    prewarm_tick();
    assert(pw_loading);

    // Timeout after 30 s
    mock_now_ns += PW_LOAD_TIMEOUT_NS;
    mock_frame = 203;
    prewarm_tick();
    assert(!pw_loading);

    // Non-blocking hash collision handling (state==0 skips without blocking; state==1 destroys & rebuilds)
    Pipe *third = pw_pending;
    assert(third != NULL && third->prewarmed);
    atomic_store(&third->state, 0);
    int run_before = mock_run_job_calls;
    assert(!prewarm_resolve_collision(third, &k2));
    assert(third->prewarmed && mock_run_job_calls == run_before && mock_destroy_pipe_calls == 0);

    atomic_store(&third->state, 1);
    third->pipe = (VkPipeline)(uintptr_t)0x9999;
    atomic_store(&live_gpipes, 5);
    assert(prewarm_resolve_collision(third, &k2));
    assert(!third->prewarmed && third->pipe == VK_NULL_HANDLE && pw_pending == NULL);
    assert(atomic_load(&live_gpipes) == 4 && atomic_load(&third->state) == 0);
    assert(third->k.stride[0] == 24 && mock_destroy_pipe_calls == 1 && mock_run_job_calls == run_before + 1);

    // Reset isolates prewarm scene state without clobbering global g_scene_frame
    assert(g_scene_frame == 177);
    prewarm_reset();
    assert(g_scene_frame == 177 && prewarm_scene_frame() == 0);
    mock_frame = 204;
    g_scene_frame = 204;
    assert(prewarm_scene_frame() == 204);
    assert(pw_pending == NULL && !pw_read && !pw_loading && !pw_saw_title);
    assert(pw_queued_boot == 0 && pw_ready_boot == 0);

    // dyn_vi toggle invalidates saved static-VI prewarm file and rejects non-zero vertex fields
    g_vk.dyn_vi = true;
    assert(prewarm_parse_and_validate(raw, raw_n) == 0);
    PwRecord dyn_rec = {0};
    dyn_rec.prog = (PwProgId){.vs_h = 0x11ull, .fs_h = 0x22ull, .key_h = 0x33ull, .nv = 10, .nf = 10};
    PipeKey dyn_k = {.ncolor = 1, .samples = 1, .topo_class = 2};
    memcpy(dyn_rec.baked, &dyn_k.color, PW_BAKED_SIZE);
    assert(prewarm_valid_record(&dyn_rec));
    dyn_k.attr_mask = 1u;
    memcpy(dyn_rec.baked, &dyn_k.color, PW_BAKED_SIZE);
    assert(!prewarm_valid_record(&dyn_rec));
    dyn_k.attr_mask = 0;
    dyn_k.used_streams = 1u;
    memcpy(dyn_rec.baked, &dyn_k.color, PW_BAKED_SIZE);
    assert(!prewarm_valid_record(&dyn_rec));
    dyn_k.used_streams = 0;
    dyn_k.stride[0] = 32u;
    memcpy(dyn_rec.baked, &dyn_k.color, PW_BAKED_SIZE);
    assert(!prewarm_valid_record(&dyn_rec));

    // dyn_vi = true save + reload round-trip, prewarm_tick hash parity, and true -> false invalidation
    free_caches();
    prewarm_clear_records();
    free(pw_records);
    pw_records = NULL;
    pw_alloc_cap = 0;
    remove(PW_PATH);
    prewarm_reset();
    Prog dp = make_prog(0x7777);
    vk_object_slot(&prog_cache, 77)->object = &dp;
    PipeKey dk_rec = {.prog = &dp, .color = {VK_FORMAT_R8G8B8A8_UNORM}, .depth = VK_FORMAT_D32_SFLOAT, .ncolor = 1, .samples = 1, .topo_class = 2};
    mock_title = false;
    g_scene_frame = 210;
    mock_frame = 210;
    prewarm_record(&dk_rec);
    assert(pw_count == 1 && prewarm_save());
    FILE *df = fopen(PW_PATH, "rb");
    assert(df != NULL);
    uint8_t draw_raw[4096];
    size_t draw_n = fread(draw_raw, 1, sizeof draw_raw, df);
    fclose(df);
    assert(draw_n == sizeof(PwHeader) + sizeof(PwRecord));

    prewarm_reset();
    prewarm_clear_records();
    prewarm_load();
    assert(pw_count == 1 && pw_records != NULL);
    PipeKey dk_draw;
    memset(&dk_draw, 0, sizeof dk_draw);
    memcpy(&dk_draw.color, pw_records[0].baked, PW_BAKED_SIZE);
    dk_draw.prog = &dp;
    uint64_t expected_dh = hash_words(0xcbf29ce484222325ull, &dk_draw, sizeof dk_draw);
    if (!expected_dh) expected_dh = 1;
    mock_title = true;
    mock_frame = 211;
    prewarm_tick();
    mock_title = false;
    g_scene_frame = 210;
    mock_frame = 212;
    prewarm_tick();
    assert(pw_loading && pw_pending != NULL && pw_pending->prewarmed && pw_pending->used_s == g_exec_s);
    assert(pw_cache_find(&pipe_cache, expected_dh) == pw_pending && vk_object_slot(&pipe_cache, expected_dh)->object == pw_pending);

    g_vk.dyn_vi = false;
    assert(prewarm_parse_and_validate(draw_raw, draw_n) == 0);

    free_caches();
    free(pw_records);
    pw_records = NULL;
    remove(PW_PATH);
    rmdir("cache");
    chdir(cwd);
    rmdir(tmpdir);
    puts("PASS: default-off, record/dedup/cap, header+payload validation, loading transitions, guards, reuse, reset");
    return 0;
}
