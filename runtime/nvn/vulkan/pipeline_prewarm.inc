// Opt-in graphics pipeline prewarming during Android loading screens (#294).
// Included from runtime/nvn/vulkan/exec.c (#ifdef __ANDROID__) and tools/test_pipeline_prewarm.c.
// All profile, cache, and queue state runs on the render owner except Prog.pw_id write in compile_program().

#define PW_MAGIC "BOTWPW01"
enum {
    PW_VERSION = 1,
    PW_DEFAULT_CAP = 256,
    PW_MAX_CAP = 512,
    PW_HEADROOM = 256,
    PW_IDX_BUCKETS = 1024,
};
#define PW_LOAD_TIMEOUT_NS   30000000000ull
#define PW_FLUSH_INTERVAL_NS 10000000000ull
#define PW_BAKED_OFF         offsetof(PipeKey, color)
#define PW_BAKED_SIZE        ((uint32_t)(sizeof(PipeKey) - PW_BAKED_OFF))
#define PW_PATH              "cache/pipeline-prewarm.bin"
#define PW_TMP_PATH          "cache/pipeline-prewarm.bin.tmp"

extern bool framepace_on_title(void);
extern bool framepace_in_menu(void);
extern bool framepace_in_event(void);

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t baked_size;
    uint32_t count;
    uint32_t vk_flags;
    int32_t game_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_version;
    uint8_t cache_uuid[VK_UUID_SIZE];
    uint64_t payload_hash;
} PwHeader;
_Static_assert(sizeof(PwHeader) == 64, "PwHeader must stay 64 bytes without padding");

typedef struct {
    PwProgId prog;
    uint8_t baked[sizeof(PipeKey) - offsetof(PipeKey, color)];
} PwRecord;

typedef struct {
    uint64_t key;
    uint16_t slot_plus_1;
} PwRecBucket;

typedef struct {
    uint64_t vs_h;
    uint16_t head_plus_1;
} PwProgBucket;

#ifndef PW_PENDING_DECLARED
static Pipe *pw_pending;
#endif
static PwRecord *pw_records;
static unsigned pw_alloc_cap, pw_count, pw_cap_val;
static PwRecBucket pw_rec_set[PW_IDX_BUCKETS];
static PwProgBucket pw_prog_idx[PW_IDX_BUCKETS];
static uint16_t pw_next_plus_1[PW_MAX_CAP];
static uint8_t pw_attempted[PW_MAX_CAP];

static bool pw_read, pw_dirty, pw_saw_title, pw_loading, pw_prev_menu;
static bool pw_logged_loaded;
static unsigned pw_logged_loads;
static uint64_t pw_frame = UINT64_MAX, pw_reset_frame, pw_load_start_ns, pw_first_dirty_ns;
static unsigned pw_queued_load, pw_queued_boot, pw_seen_boot, pw_ready_boot;

static uint64_t prewarm_scene_frame(void) {
    uint64_t s = g_scene_frame;
    return s > pw_reset_frame ? s : 0;
}

static uint32_t prewarm_vk_flags(void) {
    return (uint32_t)((g_vk.dyn_render ? 1u : 0u) |
                      (g_vk.eds ? 2u : 0u) |
                      (g_vk.eds2 ? 4u : 0u) |
                      (g_vk.divisor ? 8u : 0u) |
                      (g_vk.dyn_vi ? 16u : 0u));
}

static bool prewarm_supported(void) {
    return g_game_version >= 182 && !g_vk.exec_stats && !sync_shaders();
}

static unsigned prewarm_cap(void) {
    if (!pw_cap_val) {
        unsigned c = PW_DEFAULT_CAP;
        const char *e = getenv("LARECOMP_PIPE_PREWARM_MAX");
        if (e && *e) {
            long v = strtol(e, NULL, 10);
            if (v >= 1 && v <= (long)PW_MAX_CAP) c = (unsigned)v;
        }
        pw_cap_val = c;
    }
    return pw_cap_val;
}

static bool prewarm_ensure_storage(void) {
    unsigned cap = prewarm_cap();
    if (pw_records && pw_alloc_cap == cap) return true;
    free(pw_records);
    pw_records = calloc(cap, sizeof *pw_records);
    pw_alloc_cap = pw_records ? cap : 0;
    return pw_records != NULL;
}

static void prewarm_clear_records(void) {
    pw_count = 0;
    memset(pw_rec_set, 0, sizeof pw_rec_set);
    memset(pw_prog_idx, 0, sizeof pw_prog_idx);
    memset(pw_next_plus_1, 0, sizeof pw_next_plus_1);
    memset(pw_attempted, 0, sizeof pw_attempted);
}

static uint64_t pw_record_hash(const PwRecord *r) {
    uint64_t h = hash_words(0xcbf29ce484222325ull, r, sizeof *r);
    return h ? h : 1;
}

static int pw_find_record(const PwRecord *r, uint64_t key) {
    unsigned b = (unsigned)((key ^ (key >> 32)) & (PW_IDX_BUCKETS - 1));
    while (pw_rec_set[b].slot_plus_1) {
        if (pw_rec_set[b].key == key) {
            unsigned idx = pw_rec_set[b].slot_plus_1 - 1;
            if (idx < pw_count && memcmp(&pw_records[idx], r, sizeof *r) == 0)
                return (int)idx;
        }
        b = (b + 1) & (PW_IDX_BUCKETS - 1);
    }
    return -1;
}

static void pw_index_append(const PwRecord *r) {
    if (!prewarm_ensure_storage() || pw_count >= pw_alloc_cap) return;
    unsigned idx = pw_count++;
    pw_records[idx] = *r;

    uint64_t key = pw_record_hash(r);
    unsigned b = (unsigned)((key ^ (key >> 32)) & (PW_IDX_BUCKETS - 1));
    while (pw_rec_set[b].slot_plus_1) b = (b + 1) & (PW_IDX_BUCKETS - 1);
    pw_rec_set[b].key = key;
    pw_rec_set[b].slot_plus_1 = (uint16_t)(idx + 1);

    unsigned pb = (unsigned)((r->prog.vs_h ^ (r->prog.vs_h >> 32)) & (PW_IDX_BUCKETS - 1));
    while (pw_prog_idx[pb].vs_h && pw_prog_idx[pb].vs_h != r->prog.vs_h)
        pb = (pb + 1) & (PW_IDX_BUCKETS - 1);
    pw_prog_idx[pb].vs_h = r->prog.vs_h;
    pw_next_plus_1[idx] = pw_prog_idx[pb].head_plus_1;
    pw_prog_idx[pb].head_plus_1 = (uint16_t)(idx + 1);
}

static uint16_t pw_find_prog_head(uint64_t vs_h) {
    unsigned b = (unsigned)((vs_h ^ (vs_h >> 32)) & (PW_IDX_BUCKETS - 1));
    while (pw_prog_idx[b].vs_h) {
        if (pw_prog_idx[b].vs_h == vs_h) return pw_prog_idx[b].head_plus_1;
        b = (b + 1) & (PW_IDX_BUCKETS - 1);
    }
    return 0;
}

static bool prewarm_valid_record(const PwRecord *r) {
    if (!r->prog.vs_h || !r->prog.nv || !r->prog.nf) return false;
    PipeKey k;
    memset(&k, 0, sizeof k);
    memcpy(&k.color, r->baked, PW_BAKED_SIZE);
    if (k.ncolor > 8) return false;
    if (k.samples < 1 || k.samples > 64 || (k.samples & (k.samples - 1)) != 0) return false;
    if (k.used_streams & ~((1u << 17) - 1u)) return false;
    uint32_t c_mask = k.ncolor < 8 ? ((1u << k.ncolor) - 1u) : 0xFFu;
    if (k.blend_on & ~c_mask) return false;
    for (int i = 0; i < 32; i++) {
        if ((k.attr_mask & (1u << i)) && k.attr_bind[i] > ZERO_BINDING) return false;
    }
    if (g_vk.dyn_vi) {
        if (k.attr_mask || k.used_streams) return false;
        for (int i = 0; i < 32; i++)
            if (k.attr_fmt[i] || k.attr_off[i] || k.attr_bind[i]) return false;
        for (int i = 0; i < 17; i++)
            if (k.stride[i] || k.divisor[i]) return false;
    }
    if (k.topo_class > 2 || k.poly_fill > 1 || k.depth_clamp > 1 ||
        k.prim_restart > 1 || k.a2c > 1 || k.fx.on > 1) return false;
    if (g_vk.eds && k.fx.on) return false;
    if (k.fx.on) {
        if (k.fx.cull > 3 || k.fx.ff > 1 || k.fx.topo > 10 ||
            k.fx.dt > 1 || k.fx.dw > 1 || k.fx.dop > 7 || k.fx.sten > 1)
            return false;
        for (int f = 0; f < 2; f++) {
            if (k.fx.sop[f][0] > 7 || k.fx.sop[f][1] > 7 ||
                k.fx.sop[f][2] > 7 || k.fx.sop[f][3] > 7)
                return false;
        }
    }
    return true;
}

static unsigned prewarm_parse_and_validate(const uint8_t *data, size_t size) {
    prewarm_clear_records();
    if (!data || size < sizeof(PwHeader)) return 0;
    PwHeader hdr;
    memcpy(&hdr, data, sizeof hdr);
    if (memcmp(hdr.magic, PW_MAGIC, 8) != 0 ||
        hdr.version != PW_VERSION ||
        hdr.baked_size != PW_BAKED_SIZE ||
        hdr.count == 0 ||
        hdr.count > PW_MAX_CAP)
        return 0;
    size_t expected = sizeof(PwHeader) + (size_t)hdr.count * sizeof(PwRecord);
    if (size != expected) return 0;
    if (hdr.vk_flags != prewarm_vk_flags() ||
        hdr.game_version != g_game_version ||
        hdr.vendor_id != g_vk.props.vendorID ||
        hdr.device_id != g_vk.props.deviceID ||
        hdr.driver_version != g_vk.props.driverVersion ||
        memcmp(hdr.cache_uuid, g_vk.props.pipelineCacheUUID, VK_UUID_SIZE) != 0)
        return 0;

    const uint8_t *payload = data + sizeof(PwHeader);
    uint64_t ph = hash_words(0xcbf29ce484222325ull, payload, (size_t)hdr.count * sizeof(PwRecord));
    if (ph != hdr.payload_hash) return 0;
    if (!prewarm_ensure_storage()) return 0;

    for (uint32_t i = 0; i < hdr.count; i++) {
        PwRecord rec;
        memcpy(&rec, payload + (size_t)i * sizeof(PwRecord), sizeof rec);
        if (!prewarm_valid_record(&rec) || pw_find_record(&rec, pw_record_hash(&rec)) >= 0) {
            prewarm_clear_records();
            return 0;
        }
        if (pw_count < pw_alloc_cap) pw_index_append(&rec);
    }
    return pw_count;
}

static void prewarm_load(void) {
    pw_read = true;
    FILE *f = fopen(PW_PATH, "rb");
    if (!f) return;
    size_t max_sz = sizeof(PwHeader) + (size_t)PW_MAX_CAP * sizeof(PwRecord);
    uint8_t *buf = malloc(max_sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t n = fread(buf, 1, max_sz + 1, f);
    bool io_err = ferror(f) != 0;
    fclose(f);
    if (!io_err && n <= max_sz) prewarm_parse_and_validate(buf, n);
    free(buf);
}

static bool prewarm_save(void) {
    if (!pw_dirty || !pw_records || !pw_count) {
        pw_dirty = false;
        pw_first_dirty_ns = 0;
        return true;
    }
    mkdir("cache", 0755);
    size_t payload_sz = (size_t)pw_count * sizeof(PwRecord);
    PwHeader hdr = {0};
    memcpy(hdr.magic, PW_MAGIC, 8);
    hdr.version = PW_VERSION;
    hdr.baked_size = PW_BAKED_SIZE;
    hdr.count = pw_count;
    hdr.vk_flags = prewarm_vk_flags();
    hdr.game_version = g_game_version;
    hdr.vendor_id = g_vk.props.vendorID;
    hdr.device_id = g_vk.props.deviceID;
    hdr.driver_version = g_vk.props.driverVersion;
    memcpy(hdr.cache_uuid, g_vk.props.pipelineCacheUUID, VK_UUID_SIZE);
    hdr.payload_hash = hash_words(0xcbf29ce484222325ull, pw_records, payload_sz);

    FILE *f = fopen(PW_TMP_PATH, "wb");
    if (!f) return false;
    bool ok = fwrite(&hdr, sizeof hdr, 1, f) == 1 &&
              fwrite(pw_records, sizeof(PwRecord), pw_count, f) == pw_count &&
              fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(PW_TMP_PATH, PW_PATH) != 0) {
        remove(PW_TMP_PATH);
        return false;
    }
    pw_dirty = false;
    pw_first_dirty_ns = 0;
    return true;
}

static void prewarm_record(const PipeKey *key) {
    if (!prewarm_supported() || !key || !key->prog || !key->prog->pw_id.vs_h) return;
    uint64_t scene = prewarm_scene_frame();
    if (framepace_on_title() || !scene || renderer_present_count() - scene >= 20) return;
    if (!pw_read) prewarm_load();
    if (!prewarm_ensure_storage() || pw_count >= pw_alloc_cap) return;

    PwRecord rec = {0};
    rec.prog = key->prog->pw_id;
    memcpy(rec.baked, &key->color, PW_BAKED_SIZE);
    if (!prewarm_valid_record(&rec)) return;
    if (pw_find_record(&rec, pw_record_hash(&rec)) >= 0) return;

    pw_index_append(&rec);
    if (!pw_dirty) {
        pw_dirty = true;
        pw_first_dirty_ns = plat_now_ns();
    }
}

static bool prewarm_workers_idle(void) {
    pthread_mutex_lock(&jobs_lock);
    bool w_idle = !jobs_head && (!jobs_started || jobs_active < jobs_workers);
    pthread_mutex_unlock(&jobs_lock);
    if (!w_idle) return false;
    pthread_mutex_lock(&tjobs_lock);
    bool t_idle = !tjobs_head && !tjobs_active;
    pthread_mutex_unlock(&tjobs_lock);
    return t_idle;
}

static void *pw_cache_find(const VkObjectCache *cache, uint64_t key) {
    if (!cache->capacity) return NULL;
    size_t slot = vk_object_bucket(key, cache->capacity);
    while (cache->entries[slot].key && cache->entries[slot].key != key)
        slot = (slot + 1) & (cache->capacity - 1);
    return cache->entries[slot].key == key ? cache->entries[slot].object : NULL;
}

static void prewarm_tick(void) {
    if (!prewarm_supported() || rt_reboot_requested()) return;
    if (!pw_read) {
        prewarm_load();
        if (!pw_logged_loaded && pw_count > 0) {
            pw_logged_loaded = true;
            LOG("[vk] pipeline prewarming: loaded %u recorded pipelines\n", pw_count);
        }
    }

    uint64_t now = plat_now_ns();
    bool title = framepace_on_title();
    bool menu = framepace_in_menu();
    bool event = framepace_in_event();
    bool just_left_menu = pw_prev_menu && !menu;
    pw_prev_menu = menu;
    if (title) pw_saw_title = true;
    uint64_t scene = prewarm_scene_frame();
    uint64_t frame = renderer_present_count();
    bool in_world = !title && scene && frame - scene < 20;
    bool want_loading = (pw_saw_title || scene) && !title && !menu && !event && !in_world &&
                        !(just_left_menu && scene);

    if (want_loading && !pw_loading) {
        if (pw_dirty) prewarm_save();
        pw_loading = true;
        pw_load_start_ns = now;
        pw_queued_load = 0;
    } else if (pw_loading && (!want_loading || now - pw_load_start_ns >= PW_LOAD_TIMEOUT_NS)) {
        pw_loading = false;
        if ((pw_queued_load > 0 || pw_logged_loads == 0) && pw_count > 0 && pw_logged_loads < 20) {
            pw_logged_loads++;
            LOG("[vk] pipeline prewarming: %u queued while loading (%u recorded)\n", pw_queued_load, pw_count);
        }
    }

    if (!pw_loading) {
        if (pw_dirty && pw_first_dirty_ns && now - pw_first_dirty_ns >= PW_FLUSH_INTERVAL_NS)
            prewarm_save();
        return;
    }
    if (!pw_count) return;
    if (pw_frame == frame) return;
    pw_frame = frame;

    if (pw_pending && !pw_pending->evicted && atomic_load(&pw_pending->state) == 0) return;
    pw_pending = NULL;

    int keep = pipe_keep();
    if (keep > 0) {
        size_t live = (size_t)atomic_load(&live_gpipes);
        if (live + PW_HEADROOM >= (size_t)keep || pipe_cache.count + PW_HEADROOM >= (size_t)keep) return;
    }
    if (!prewarm_workers_idle()) return;

    for (size_t j = 0; j < prog_cache.capacity; j++) {
        Prog *pr = prog_cache.entries[j].object;
        if (!pr || atomic_load(&pr->state) != 1 || !pr->pw_id.vs_h || !pr->layout) continue;
        for (uint16_t cur = pw_find_prog_head(pr->pw_id.vs_h); cur; cur = pw_next_plus_1[cur - 1]) {
            unsigned idx = (unsigned)(cur - 1);
            if (pw_attempted[idx]) continue;
            const PwRecord *rec = &pw_records[idx];
            if (memcmp(&rec->prog, &pr->pw_id, sizeof rec->prog) != 0) continue;

            PipeKey k;
            memset(&k, 0, sizeof k);
            memcpy(&k.color, rec->baked, PW_BAKED_SIZE);
            k.prog = pr;
            uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
            if (!ph) ph = 1;
            if (pw_cache_find(&pipe_cache, ph)) {
                pw_attempted[idx] = 1;
                continue;
            }
            Pipe *p = calloc(1, sizeof *p);
            if (!p) return;
            p->k = k;
            p->used_s = g_exec_s;
            p->seq = vk_pending_seq();
            p->prewarmed = true;
            if (!enqueue_job(build_pipeline, p, true)) {
                free(p);
                return;
            }
            vk_object_slot(&pipe_cache, ph)->object = p;
            atomic_store(&pipeline_keys, pipe_cache.count);
            pr->used_s = g_exec_s;
            pr->used_seq = vk_pending_seq();
            pw_pending = p;
            pw_attempted[idx] = 1;
            pw_queued_load++;
            pw_queued_boot++;
            return;
        }
    }
}

static void prewarm_first_draw(Pipe *pp) {
    pp->prewarmed = false;
    int st = atomic_load(&pp->state);
    pw_seen_boot++;
    if (st == 1) {
        pw_ready_boot++;
        if (pw_ready_boot == 1) {
            LOG("[vk] pipeline prewarming: %u/%u ready at first draw (%u recorded)\n",
                pw_ready_boot, pw_queued_boot, pw_count);
        }
    } else if (st == 2) {
        atomic_store(&pp->state, 0);
        run_job(build_pipeline, pp);
    }
}

static bool prewarm_resolve_collision(Pipe *pp, const PipeKey *k) {
    int pst = atomic_load(&pp->state);
    if (!pst) return false;
    if (pp == pw_pending) pw_pending = NULL;
    if (pp->pipe) {
        long long mg0 = vk_mnow();
        vkDestroyPipeline(g_vk.dev, pp->pipe, NULL);
        vk_mgrow_add(MG_GPIPE, mg0);
        pp->pipe = VK_NULL_HANDLE;
        if (pst == 1) atomic_fetch_sub(&live_gpipes, 1);
    }
    pp->k = *k;
    pp->prewarmed = false;
    pp->evicted = false;
    pp->used_s = g_exec_s;
    pp->seq = vk_pending_seq();
    atomic_store(&pp->state, 0);
    run_job(build_pipeline, pp);
    return true;
}

static void prewarm_reset(void) {
    if (pw_dirty) prewarm_save();
    if (pw_queued_boot > 0) {
        LOG("[vk] pipeline prewarming: boot summary queued %u, seen %u, ready_at_first_draw %u (%u recorded)\n",
            pw_queued_boot, pw_seen_boot, pw_ready_boot, pw_count);
    }
    pw_pending = NULL;
    pw_read = false;
    pw_saw_title = false;
    pw_loading = false;
    pw_prev_menu = false;
    pw_logged_loaded = false;
    pw_logged_loads = 0;
    pw_frame = UINT64_MAX;
    pw_reset_frame = renderer_present_count();
    pw_load_start_ns = 0;
    pw_first_dirty_ns = 0;
    pw_queued_load = pw_queued_boot = pw_seen_boot = pw_ready_boot = 0;
    memset(pw_attempted, 0, sizeof pw_attempted);
    pw_cap_val = 0;
    atomic_store_explicit(&pw_env, -2, memory_order_relaxed);
}
