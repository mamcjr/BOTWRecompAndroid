// NVN command buffers: commands are recorded as CmdRec records into a host shadow of the game's own memory, so the
// game's memory management and out-of-memory callbacks work as designed.
//
// Where: in NVN a command handle refers to the list in *control* memory (nvnCommandBufferAddControlMemory), and that
// is the memory whose lifetime is the list's: BOTW gives a small command buffer its own control memory per object
// (kept as long as the list is called) but the same scratch command memory every time. So records go to the control
// memory when the command buffer has some, else to its command memory. Command memory then is only accounted.
//
// How much: our records are bigger than NVN's own tokens, which the game sizes its memory for, so each byte of the
// game's memory stands for SHADOW_K bytes of a host shadow of it (shadow_at: by 64 KB page of guest address). A
// command buffer reports its use in the game's bytes (ours / SHADOW_K); when the game reuses a piece of its memory,
// the shadow of that piece is reused too. A list never crosses a shadow page in one piece: a jump continues it.
// A command handle is the (host) address of a list's first record.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"

#define SHADOW_K 8                                                  // shadow bytes per byte of the game's memory
#define SHADOW_PAGE 0x10000ull                                      // (game bytes)
#define JUMP_RESERVE ((sizeof(CmdRec) + SHADOW_K - 1) / SHADOW_K)  // (game bytes) the chaining jump at a chunk's end

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

NVN(nvnCommandBufferInitialize) {
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    RET(1);
}
NVN(nvnCommandBufferFinalize) { free(cb_of(A(0))); SET_HOST(A(0), 0); }
NVN(nvnCommandBufferSetDebugLabel) { (void)c; }

static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

// ---- shadow pages: guest page -> host block of SHADOW_PAGE * SHADOW_K bytes (allocated at first use, kept)
// Asked for every record the game's threads write, several at once: a lookup takes no lock - slots are only ever
// added (mem published before page). The lock is only for adding one: under it every record contended (futex wakes and
// bionic's contention trace writes, ~15% of the game's threads, the critical path of the frame; simpleperf
// 2026-10-31). No per-thread cache of the last page: a thread-local costs a dynamic TLS resolve in this library.
enum { SHADOW_SLOTS = 1 << 16 };
static struct { uint64_t page; uint8_t *mem; } g_shadow[SHADOW_SLOTS];
static pthread_mutex_t g_shadow_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t *shadow_find(uint64_t page, uint32_t *at) {
    uint32_t i = (uint32_t)(page * 0x9E3779B97F4A7C15ull >> 48) & (SHADOW_SLOTS - 1);
    for (uint64_t p; (p = __atomic_load_n(&g_shadow[i].page, __ATOMIC_ACQUIRE)); i = (i + 1) & (SHADOW_SLOTS - 1))
        if (p == page) return g_shadow[i].mem;
    *at = i;
    return NULL;
}

static uint64_t shadow_at(uint64_t guest) {
    uint64_t page = guest / SHADOW_PAGE + 1;  // (+1: 0 marks an empty slot)
    uint32_t i;
    uint8_t *m = shadow_find(page, &i);
    if (!m) {
        pthread_mutex_lock(&g_shadow_lock);
        if (!(m = shadow_find(page, &i))) {  // (still missing: added at the empty slot it ended on)
            m = malloc(SHADOW_PAGE * SHADOW_K + sizeof(CmdRec));
            if (!m) FATAL("nvn: no memory for a command shadow page\n");
            g_shadow[i].mem = m;
            __atomic_store_n(&g_shadow[i].page, page, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&g_shadow_lock);
    }
    return (uint64_t)(uintptr_t)m + guest % SHADOW_PAGE * SHADOW_K;
}

// The chunk records go to: cb->gmem (guest address; 0: host memory at cb->mem), cb->size and cb->used in game bytes.
// (the page last written kept in the command buffer: a page's shadow never moves, so it can't go stale)
static uint64_t write_at(NvnCmdBuf *cb) {
    if (!cb->gmem) return cb->mem + cb->used * SHADOW_K;
    uint64_t guest = cb->gmem + cb->used, page = guest / SHADOW_PAGE + 1;
    if (page != cb->shadow_page) cb->shadow_mem = shadow_at(guest - guest % SHADOW_PAGE), cb->shadow_page = page;
    return cb->shadow_mem + guest % SHADOW_PAGE * SHADOW_K;
}

// The list being recorded goes on at `to` (a jump to it).
static void jump_from_here(NvnCmdBuf *cb, uint64_t to) {
    if (cb->recording && (cb->gmem || cb->mem)) raw_rec(write_at(cb), CMD_JUMP, sizeof(CmdRec))->x[0] = to;
}

static void set_chunk(NvnCmdBuf *cb, uint64_t gmem, uint64_t mem, uint64_t size) {
    if (gmem) {
        uint64_t in_page = gmem % SHADOW_PAGE;
        if (in_page + JUMP_RESERVE > SHADOW_PAGE) {
            uint64_t skip = SHADOW_PAGE - in_page;
            gmem += skip;
            size = size > skip ? size - skip : 0;
        }
    }
    if (cb->recording && ENV("LARECOMP_CHECK_HANDLES")) {
        static int n;
        if (n++ < 40) LOG("[nvn] command buffer %llx: new memory %llx (host %llx) while recording, %llu of %llu used at %llx\n",
                          (unsigned long long)cb->guest, (unsigned long long)gmem, (unsigned long long)mem,
                          (unsigned long long)cb->used, (unsigned long long)cb->size, (unsigned long long)cb->gmem);
    }
    jump_from_here(cb, gmem ? shadow_at(gmem) : mem);
    cb->gmem = gmem;
    cb->mem = mem;
    cb->size = size;
    cb->used = 0;
}

NVN(nvnCommandBufferAddCommandMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    if (ENV("LARECOMP_CHECK_HANDLES")) {
        static int n;
        if (n++ < 300) LOG("[nvn] AddCommandMemory %llx: pool %llx +%llx size %llx (lr %llx)\n", (unsigned long long)A(0),
                           (unsigned long long)A(1), (unsigned long long)A(2), (unsigned long long)A(3), (unsigned long long)c->x[30]);
    }
    cb->cmd_size = A(3), cb->cmd_used = 0;
    if (!cb->has_ctrl) set_chunk(cb, HOST(NvnPool, A(1))->storage + A(2), 0, A(3));
}
NVN(nvnCommandBufferAddControlMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    if (ENV("LARECOMP_CHECK_HANDLES")) {
        static int n;
        if (n++ < 300) LOG("[nvn] AddControlMemory %llx: %llx size %llx (lr %llx)\n", (unsigned long long)A(0), (unsigned long long)A(1),
                           (unsigned long long)A(2), (unsigned long long)c->x[30]);
    }
    cb->has_ctrl = true;
    set_chunk(cb, A(1), 0, A(2));
}
static uint64_t chunk_free(const NvnCmdBuf *cb) { return cb->size > cb->used + JUMP_RESERVE ? cb->size - cb->used - JUMP_RESERVE : 0; }
NVN(nvnCommandBufferGetCommandMemorySize) { NvnCmdBuf *cb = cb_of(A(0)); RET(cb->has_ctrl ? cb->cmd_size : cb->size); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { NvnCmdBuf *cb = cb_of(A(0)); RET(cb->has_ctrl ? cb->cmd_used : cb->used); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->has_ctrl ? cb->cmd_size - cb->cmd_used : chunk_free(cb));
}
NVN(nvnCommandBufferGetControlMemorySize) { NvnCmdBuf *cb = cb_of(A(0)); RET(cb->has_ctrl ? cb->size : 0x10000); }
NVN(nvnCommandBufferGetControlMemoryUsed) { NvnCmdBuf *cb = cb_of(A(0)); RET(cb->has_ctrl ? cb->used : 0); }
NVN(nvnCommandBufferGetControlMemoryFree) { NvnCmdBuf *cb = cb_of(A(0)); RET(cb->has_ctrl ? chunk_free(cb) : 0x10000); }
NVN(nvnCommandBufferSetMemoryCallback) { cb_of(A(0))->callback = A(1); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { (void)c; }

// Memory of our own, when the game's runs out and its callback adds none (BOTW: an empty callback on small command
// buffers). Lists live as long as the game keeps calling them, so these chunks are never freed; LARECOMP_LOG_CMDMEM
// logs each one.
static void add_host_memory(NvnCmdBuf *cb, uint32_t need) {
    static _Atomic uint64_t total;
    uint64_t size = need + JUMP_RESERVE > 0x800 ? need + JUMP_RESERVE : 0x800;  // (game bytes)
    uint8_t *mem = malloc(size * SHADOW_K);
    if (!mem) FATAL("nvn: out of command memory (host, %llu bytes)\n", (unsigned long long)(size * SHADOW_K));
    total += size * SHADOW_K;
    if (ENV("LARECOMP_LOG_CMDMEM"))
        LOG("[nvn] command buffer %llx: %llu bytes of host command memory (%llu KB in all)\n",
            (unsigned long long)cb->guest, (unsigned long long)(size * SHADOW_K), (unsigned long long)(total / 1024));
    set_chunk(cb, 0, (uint64_t)(uintptr_t)mem, size);
}

// Reserve a record. May call back into the guest for more memory, so callers snapshot their
// arguments first.
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    uint32_t real = (uint32_t)((sizeof(CmdRec) + extra + 7) & ~7u);
    uint32_t size = (real + SHADOW_K - 1) / SHADOW_K;  // (game bytes)
    for (int tries = 0; (!cb->gmem && !cb->mem) || cb->used + size + JUMP_RESERVE > cb->size; tries++) {
        if (!cb->callback || tries == 2) {
            add_host_memory(cb, size);
            break;
        }
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = cb->guest;
        c->x[1] = cb->has_ctrl;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY / _CONTROL_MEMORY
        c->x[2] = size + JUMP_RESERVE;
        c->x[3] = cb->callback_data;
        guest_call(c, cb->callback);
        memcpy(c->x, saved, sizeof saved);
    }
    if (cb->gmem) {  // (a record doesn't cross a shadow page: the rest of the page is skipped)
        uint64_t in_page = (cb->gmem + cb->used) % SHADOW_PAGE;
        if (in_page + size + JUMP_RESERVE > SHADOW_PAGE) {
            uint64_t skip = SHADOW_PAGE - in_page;
            if (cb->used + skip + size + JUMP_RESERVE <= cb->size) {
                raw_rec(write_at(cb), CMD_JUMP, sizeof(CmdRec))->x[0] = shadow_at(cb->gmem + cb->used + skip);
                cb->used += skip;
            } else {
                add_host_memory(cb, size);
            }
        }
    }
    CmdRec *r = raw_rec(write_at(cb), op, real);
    cb->used += size;
    return r;
}

NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    cb->recording = true;
    cb->start = (uint64_t)(uintptr_t)rec(c, cb, CMD_BARRIER, 0);  // makes sure memory exists; the list starts here
}
// LARECOMP_CHECK_HANDLES: the last lists recorded (where they start and end in the shadow, by whom, from which game
// memory), to tell who recorded over a list that is called later
typedef struct { uint64_t start, end, cb, gmem, gused, lr, seq; } ListRec;
enum { HIST = 1 << 16 };
static ListRec g_hist[HIST];
static _Atomic uint64_t g_hist_n;
static void hist_report(uint64_t h) {
    uint64_t n = g_hist_n, found = ~0ull;
    for (uint64_t k = n; k-- > (n > HIST ? n - HIST : 0);)
        if (g_hist[k % HIST].start == h) { found = k; break; }
    if (found == ~0ull) { LOG("[nvn]   %llx: not among the last lists recorded\n", (unsigned long long)h); return; }
    const ListRec *l = &g_hist[found % HIST];
    LOG("[nvn]   %llx: list #%llu by %llx (lr %llx), game memory %llx+%llx, %llu lists ago; recorded over by:\n",
        (unsigned long long)h, (unsigned long long)found, (unsigned long long)l->cb, (unsigned long long)l->lr,
        (unsigned long long)l->gmem, (unsigned long long)l->gused, (unsigned long long)(n - found));
    int shown = 0;
    for (uint64_t k = found + 1; k < n && shown < 6; k++) {
        const ListRec *o = &g_hist[k % HIST];
        if (o->start < l->end && o->end > l->start) {
            LOG("[nvn]     #%llu by %llx (lr %llx): %llx-%llx, game memory %llx+%llx\n", (unsigned long long)k,
                (unsigned long long)o->cb, (unsigned long long)o->lr, (unsigned long long)o->start, (unsigned long long)o->end,
                (unsigned long long)o->gmem, (unsigned long long)o->gused);
            shown++;
        }
    }
}

NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    rec(c, cb, CMD_END, 0);
    cb->recording = false;
    if (ENV("LARECOMP_CHECK_HANDLES")) {
        uint64_t k = g_hist_n++;
        ListRec *l = &g_hist[k % HIST];
        l->start = cb->start, l->end = write_at(cb), l->cb = cb->guest, l->gmem = cb->gmem, l->gused = cb->used;
        l->lr = c->x[30], l->seq = k;
    }
    if (ENV("LARECOMP_CHECK_HANDLES")) {
        static int n;
        if (n++ < 400) LOG("[nvn] EndRecording %llx -> %llx (lr %llx)\n", (unsigned long long)cb->guest,
                           (unsigned long long)cb->start, (unsigned long long)c->x[30]);
    }
    RET(cb->start);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }

// ---- generic recorders
typedef struct { uint64_t x[8]; float f[4]; } Snap;
static Snap snap(Cpu *c) {
    Snap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (int i = 0; i < 4; i++) s.f[i] = *(float *)&c->v[i];
    return s;
}
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, Snap *s) {
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, uint32_t op, int arg, uint32_t len) {
    Snap s = snap(c);
    CmdRec *r = record(c, op, len, &s);
    if (s.x[arg] && len) memcpy(r->extra, (void *)(uintptr_t)s.x[arg], len);
    return r;
}
#define PLAIN(name, op) NVN(name) { Snap s = snap(c); record(c, op, 0, &s); }

// ---- state binding
NVN(nvnCommandBufferBindProgram) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnProgram, s.x[1]);  // host program
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4); }
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8); }

// (cmd, first, count, const NVNbufferRange*) and the single-binding forms
NVN(nvnCommandBufferBindVertexBuffers) { record_copy(c, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferBindVertexBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
}
// (cmd, stage, first, count, const NVNbufferRange*)
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindUniformBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindStorageBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
// (cmd, stage, first, count, const handle*)
NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
// BindTexturesDeferred(cmd, stage, first, count, handles): the handles are read when the command runs, not now -
// BOTW 1.8.2 binds its deferred lighting's inputs this way (normals, albedo, depth, light buffers; SavageWindRecomp)
NVN(nvnCommandBufferBindTexturesDeferred) { Snap s = snap(c); record(c, CMD_BIND_TEXTURES_DEFERRED, 0, &s); }
NVN(nvnCommandBufferBindTexture) {
    {   extern int nvn_watch_texid(void);
        if ((int)(A(3) & 0xFFFFF) == nvn_watch_texid())
            LOG("[watch] BindTexture stage %llu slot %llu handle %llx (lr %llx)\n", (unsigned long long)A(1), (unsigned long long)A(2), (unsigned long long)A(3), (unsigned long long)c->x[30]);
    }
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindImages) { record_copy(c, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindImage) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_IMAGES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindSeparateTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
// (not translated yet: logged, the first calls and a count - does the game override a slot's sampler this way?)
NVN(nvnCommandBufferBindSeparateSamplers) {
    static unsigned n;
    if (n++ < 20 || (n & (n - 1)) == 0)
        LOG("[nvn] BindSeparateSamplers #%u stage %llu first %llu count %llu: %llx...\n", n, (unsigned long long)A(1), (unsigned long long)A(2),
            (unsigned long long)A(3), A(3) ? (unsigned long long)*(const uint64_t *)AP(4) : 0ull);
}
NVN(nvnCommandBufferBindSeparateSampler) {
    static unsigned n;
    if (n++ < 20 || (n & (n - 1)) == 0)
        LOG("[nvn] BindSeparateSampler #%u stage %llu slot %llu handle %llx\n", n, (unsigned long long)A(1), (unsigned long long)A(2),
            (unsigned long long)A(3));
}
NVN(nvnCommandBufferBindTransformFeedbackBuffers) { (void)c; }
NVN(nvnCommandBufferBindTransformFeedbackBuffer) { (void)c; }
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; }

NVN(nvnCommandBufferSetTexturePool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}
NVN(nvnCommandBufferSetSamplerPool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}

// SetRenderTargets(cmd, n, colors[], colorViews[], depth, depthView)
// extra: { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
NVN(nvnCommandBufferSetRenderTargets) {
    Snap s = snap(c);
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    const uint64_t *colors = (const uint64_t *)(uintptr_t)s.x[2], *views = (const uint64_t *)(uintptr_t)s.x[3];
    for (int i = 0; i < n; i++) {
        rt.tex[i] = colors && colors[i] ? (uint64_t)(uintptr_t)HOST(NvnTexture, colors[i]) : 0;
        if (views && views[i]) { rt.has_view[i] = 1; memcpy(&rt.views[i], (void *)(uintptr_t)views[i], sizeof(NvnTextureView)); }
    }
    rt.depth = s.x[4] ? (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[4]) : 0;
    if (s.x[5]) { rt.has_dview = 1; memcpy(&rt.dview, (void *)(uintptr_t)s.x[5], sizeof(NvnTextureView)); }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
}

// viewports/scissors/depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetViewport) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetScissor) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8); }
NVN(nvnCommandBufferSetDepthRange) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16); }
NVN(nvnCommandBufferSetStencilRef) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 2; }
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART)
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_ALPHA_REF)
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_DEPTH_BOUNDS)
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET)
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_LINE_WIDTH)
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE)
NVN(nvnCommandBufferSetRenderEnableConditional) { (void)c; }
PLAIN(nvnCommandBufferSetDepthClamp, CMD_SET_DEPTH_CLAMP)
NVN(nvnCommandBufferSetRasterizerDiscard) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterDilate) { (void)c; }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; }
NVN(nvnCommandBufferSetPatchSize) { (void)c; }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; }
NVN(nvnCommandBufferSaveZCullData) { (void)c; }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; }

static uint64_t copy_row_stride, copy_image_stride;
RT_ON_RESET(cmdbuf_reset) { copy_row_stride = copy_image_stride = 0; }
NVN(nvnCommandBufferSetCopyRowStride) {
    Snap s = snap(c);
    copy_row_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferSetCopyImageStride) {
    Snap s = snap(c);
    copy_image_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { record_copy(c, CMD_CLEAR_COLOR, 2, 16); }
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 1; }
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 2; }
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL)
// ClearTexture(cmd, texture, view, region, color, mask)
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;
static void clear_texture(Cpu *c, int kind) {
    Snap s = snap(c);
    ClearTexRec ct = {0};
    ct.tex = (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[1]);
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, (void *)(uintptr_t)s.x[2], sizeof ct.view); }
    if (s.x[3]) memcpy(ct.region, (void *)(uintptr_t)s.x[3], sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, (void *)(uintptr_t)s.x[4], 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, 1); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, 2); }
PLAIN(nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER)
// DiscardColor(cmdbuf, index): the target's contents become undefined. On the Switch a discarded (compressed)
// target reads back cleared; areas the game then leaves undrawn (e.g. a pit after a floor tile falls away) stay
// black there. Treated as a clear to (0,0,0,0) (LARECOMP_KEEP_DISCARD=1: old no-op behaviour).
NVN(nvnCommandBufferDiscardColor) {
    static int keep = -1, logged;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_DISCARD") != NULL;
    if (logged < 5) { logged++; LOG("[nvn] DiscardColor index %llu%s\n", (unsigned long long)c->x[1], keep ? " (kept)" : " -> clear"); }
    if (keep) return;
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; }

// ---- draws (all integer arguments fit in x1..x7)
NVN(nvnCommandBufferDrawArrays) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); r->x[3] = 0; r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) { Snap s = snap(c); record(c, CMD_DRAW_ARRAYS, 0, &s); }
// Debug: LARECOMP_TRACE_COUNT=<n> logs the guest call chain of indexed draws with n indices.
static void trace_draw(Cpu *c, uint64_t count) {
    static long want = -2;
    if (want == -2) want = getenv("LARECOMP_TRACE_COUNT") ? atol(getenv("LARECOMP_TRACE_COUNT")) : -1;
    if (want < 0 || (long)count != want) return;
    static int n;
    if (n++ > 40) return;
    LOG("[trace] draw count %llu lr %llx chain:", (unsigned long long)count, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= GUEST_LO && fp < GUEST_LO + 0x1000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}

NVN(nvnCommandBufferDrawElements) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
}
NVN(nvnCommandBufferDrawElementsBaseVertex) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[5] = 0; r->x[6] = 1;
}
NVN(nvnCommandBufferDrawElementsInstanced) { trace_draw(c, c->x[3]); Snap s = snap(c); record(c, CMD_DRAW_ELEMENTS, 0, &s); }
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT)
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT)
NVN(nvnCommandBufferMultiDrawArraysIndirectCount) { (void)c; }
NVN(nvnCommandBufferMultiDrawElementsIndirectCount) { (void)c; }
NVN(nvnCommandBufferDrawTransformFeedback) { (void)c; }
NVN(nvnCommandBufferDrawTexture) { (void)c; }
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH)
PLAIN(nvnCommandBufferDispatchComputeIndirect, CMD_DISPATCH_INDIRECT)
NVN(nvnCommandBufferBeginTransformFeedback) { (void)c; }
NVN(nvnCommandBufferEndTransformFeedback) { (void)c; }
NVN(nvnCommandBufferPauseTransformFeedback) { (void)c; }
NVN(nvnCommandBufferResumeTransformFeedback) { (void)c; }

// ---- copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    t->tex = tex ? (uint64_t)(uintptr_t)HOST(NvnTexture, tex) : 0;
    if (view) { t->has_view = 1; memcpy(&t->view, (void *)(uintptr_t)view, sizeof t->view); }
    if (region) memcpy(t->region, (void *)(uintptr_t)region, sizeof t->region);
}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToBuffer) {  // (cmd, src, srcView, srcRegion, dst, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAIN(nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER)
NVN(nvnCommandBufferDownsample) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; }
// UpdateUniformBuffer(cmd, buffer, size, offset, dataSize, data)
NVN(nvnCommandBufferUpdateUniformBuffer) {
    static uint64_t watch = ~0ull;  // LARECOMP_LOG_UBO_UPDATE=<hex address>: the updates that write it (caller, source)
    if (watch == ~0ull) watch = getenv("LARECOMP_LOG_UBO_UPDATE") ? strtoull(getenv("LARECOMP_LOG_UBO_UPDATE"), NULL, 16) : 0;
    if (watch && A(1) + A(3) <= watch && watch < A(1) + A(3) + A(4)) {
        static int n;
        if (n++ % 30 == 0 && n < 600) {
            const float *f = (const float *)(uintptr_t)(A(5) + (watch - A(1) - A(3)));
            LOG("[nvn] ubo update %llx+%#llx size %#llx from %llx (lr %llx): %g %g %g %g\n", (unsigned long long)A(1),
                (unsigned long long)A(3), (unsigned long long)A(4), (unsigned long long)A(5), (unsigned long long)c->x[30],
                f[0], f[1], f[2], f[3]);
        }
    }
#ifdef __ANDROID__
    extern void vk_ubo_inline_mark(uint64_t addr, uint64_t size);
    vk_ubo_inline_mark(A(1) + A(3), A(4));  // (that range stays copied per draw: vulkan/exec.c ubo_direct)
#endif
    record_copy(c, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4));
}

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) {
    // work executes at submit time, so the sync is satisfied once this list has run
    Snap s = snap(c);
    record(c, CMD_FENCE_SYNC, 0, &s);
}
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC)
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER)
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT)
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT)
// ReportCounter(cmd, type, address): the counter goes to that GPU address, which here is the guest address of the
// memory (nvnMemoryPoolGetBufferAddress). An address outside guest memory is logged (once per caller) and dropped.
NVN(nvnCommandBufferReportCounter) {
    Snap s = snap(c);
    if (s.x[2] && (s.x[2] < GUEST_LO || s.x[2] >= GUEST_HI)) {
        static uint64_t seen[8];
        int k = 0;
        while (k < 8 && seen[k] && seen[k] != c->x[30]) k++;
        if (k < 8 && !seen[k]) {
            seen[k] = c->x[30];
            LOG("[nvn] ReportCounter type %llu to %llx: not guest memory, dropped (lr %llx)\n",
                (unsigned long long)s.x[1], (unsigned long long)s.x[2], (unsigned long long)c->x[30]);
        }
        s.x[2] = 0;
    }
    record(c, CMD_REPORT_COUNTER, 0, &s);
}
NVN(nvnCommandBufferReportValue) { (void)c; }
NVN(nvnCommandBufferResetCounter) { (void)c; }
// A command handle is the address of a list's first record (a CMD_BARRIER: nvnCommandBufferBeginRecording). Handles
// that aren't one are logged (LARECOMP_CHECK_HANDLES=1).
void nvn_check_handles(Cpu *c, const char *what, int n, const uint64_t *h) {
    if (!ENV("LARECOMP_CHECK_HANDLES")) return;
    for (int i = 0; i < n; i++) {
        bool guest = h[i] >= GUEST_LO && h[i] < GUEST_HI;
        bool host = h[i] >= 0x6000000000ull;  // (malloc'd overflow memory: high host addresses)
        if ((!guest && !host) || ((const CmdRec *)(uintptr_t)h[i])->op != CMD_BARRIER) {
            static int n_logged;
            if (n_logged++ < 20) {
                LOG("[nvn] %s: handle %d of %d is %llx, not a command list (lr %llx)\n", what, i, n,
                    (unsigned long long)h[i], (unsigned long long)c->x[30]);
                hist_report(h[i]);
            }
        }
    }
}
// LARECOMP_CHECK_HANDLES: a submitted list's records are walked first; the first that isn't one is logged with the
// records before it.
static bool plausible(uint64_t a) { return (a >= GUEST_LO && a < GUEST_HI) || a >= 0x6000000000ull; }
void nvn_check_list(uint64_t h, int depth) {
    if (!ENV("LARECOMP_CHECK_HANDLES") || depth > 4) return;
    uint64_t p = h, prev[8] = {0};
    int np = 0;
    for (int i = 0; i < 1000000; i++) {
        if (!plausible(p)) { LOG("[nvn] list %llx: record %d at %llx: bad address\n", (unsigned long long)h, i, (unsigned long long)p); break; }
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        bool bad = r->op >= CMD_COUNT || (r->op != CMD_END && r->op != CMD_JUMP && (r->size < sizeof(CmdRec) || r->size > 0x100000));
        if (bad) {
            LOG("[nvn] list %llx: record %d at %llx: op %u size %u; before:", (unsigned long long)h, i, (unsigned long long)p, r->op, r->size);
            for (int k = 0; k < np && k < 8; k++) {
                const CmdRec *q = (const CmdRec *)(uintptr_t)prev[(np - 1 - k) % 8];
                LOG(" [%llx op %u size %u]", (unsigned long long)prev[(np - 1 - k) % 8], q->op, q->size);
            }
            LOG("\n");
            return;
        }
        prev[np++ % 8] = p;
        if (r->op == CMD_END) return;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (r->op == CMD_CALL) {
            const uint64_t *hh = (const uint64_t *)r->extra;
            for (uint64_t k = 0; k < r->x[0] && k < 64; k++) {
                if (!plausible(hh[k])) LOG("[nvn] list %llx: record %d (call) handle %llu is %llx\n", (unsigned long long)h, i, (unsigned long long)k, (unsigned long long)hh[k]);
                else nvn_check_list(hh[k], depth + 1);
            }
        }
        p += r->size;
    }
}

// CopyCommands copies the lists' commands into this one: the game may reuse their memory right away (BOTW does).
// (CallCommands above only refers to them.) The lists' leading barrier (nvnCommandBufferBeginRecording) is left out.
static void copy_list(Cpu *c, NvnCmdBuf *cb, uint64_t p) {
    bool first = true;
    for (int guard = 0; guard < 10000000 && p; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op == CMD_END) return;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        uint32_t size = r->size;
        if (!size) return;
        if (!(first && r->op == CMD_BARRIER)) {
            CmdRec *d = rec(c, cb, r->op, size - (uint32_t)sizeof(CmdRec));
            memcpy(d, r, size);
        }
        first = false;
        p += size;
    }
}
// CallCommands too: the lists it calls live in control memory, which NVN reads as this is recorded; BOTW records
// over them before this list is submitted. (LARECOMP_CALL_REFS=1: refer to them, as before)
NVN(nvnCommandBufferCallCommands) {
    int n = (int)A(1);
    nvn_check_handles(c, "CallCommands", n, AP(2));
    if (ENV("LARECOMP_CALL_REFS")) { record_copy(c, CMD_CALL, 2, (uint32_t)n * 8); return; }
    uint64_t self = A(0), handles[64];
    if (n > 64) n = 64;
    memcpy(handles, AP(2), n * sizeof *handles);
    for (int i = 0; i < n; i++) copy_list(c, cb_of(self), handles[i]);
}
NVN(nvnCommandBufferCopyCommands) {
    int n = (int)A(1);
    nvn_check_handles(c, "CopyCommands", n, AP(2));
    uint64_t self = A(0), handles[64];
    if (n > 64) n = 64;
    memcpy(handles, AP(2), n * sizeof *handles);  // (rec may call the guest back: its registers change)
    for (int i = 0; i < n; i++) copy_list(c, cb_of(self), handles[i]);
}
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; }
