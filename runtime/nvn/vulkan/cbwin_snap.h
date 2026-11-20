// Submission-scoped constant-buffer snapshot cache + compact patch-buffer dedupe for cbwin.
// Included from runtime/nvn/vulkan/exec.c (with CBWIN_SPAN_TYPE / CbwinSpan = VkSpan)
// and standalone host/Bionic unit tests (tools/test_cbwin_snap.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef CBWIN_SPAN_TYPE
#define CBWIN_SPAN_TYPE
typedef struct {
    uint64_t buf;
    uint64_t off;
    void *ptr;
} CbwinSpan;
#endif

enum {
    CBWIN_CACHE_SETS = 16u,
    CBWIN_CACHE_WAYS = 4u,
    CBWIN_CACHE_SLOTS = CBWIN_CACHE_SETS * CBWIN_CACHE_WAYS,
};

typedef struct {
    uint64_t seq;
    uint64_t addr;
    uint64_t size;
    uint64_t win_lo;
    uint64_t win_hi;
    CbwinSpan span;
} CbwinBaseEntry;

typedef struct {
    CbwinBaseEntry slots[CBWIN_CACHE_SLOTS];
    uint8_t rr[CBWIN_CACHE_SETS];
} CbwinBaseCache;

typedef struct {
    uint32_t m[4];
    uint32_t rows[64][4];
} CbpatchBuf;
_Static_assert(sizeof(CbpatchBuf) == 1040, "CbpatchBuf must match CBP layout (1040 bytes)");

typedef struct {
    struct {
        uint64_t seq;
        uint32_t active;
        uint8_t bytes[sizeof(CbpatchBuf)];
        CbwinSpan span;
    } stage[2];
} CbpatchDedupe;

static inline uint32_t cbwin_base_set_idx(uint64_t addr, uint64_t size) {
    uint64_t h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    return (uint32_t)((h ^ (h >> 32)) >> 4) & (CBWIN_CACHE_SETS - 1u);
}

static inline void cbwin_base_reset(CbwinBaseCache *c) {
    if (c) memset(c, 0, sizeof *c);
}

static inline bool cbwin_base_lookup(const CbwinBaseCache *c, uint64_t seq, uint64_t addr, uint64_t size,
                                     uint64_t win_lo, uint64_t win_hi, CbwinSpan *out) {
    if (!c || !size || !out) return false;
    uint32_t base = cbwin_base_set_idx(addr, size) * CBWIN_CACHE_WAYS;
    for (uint32_t w = 0; w < CBWIN_CACHE_WAYS; w++) {
        const CbwinBaseEntry *e = &c->slots[base + w];
        if (e->size == size && e->seq == seq && e->addr == addr && e->win_lo == win_lo && e->win_hi == win_hi) {
            *out = e->span;
            return true;
        }
    }
    return false;
}

static inline void cbwin_base_insert(CbwinBaseCache *c, uint64_t seq, uint64_t addr, uint64_t size,
                                     uint64_t win_lo, uint64_t win_hi, CbwinSpan span) {
    if (!c || !size) return;
    uint32_t s = cbwin_base_set_idx(addr, size);
    uint32_t base = s * CBWIN_CACHE_WAYS;
    uint32_t slot = UINT32_MAX;
    uint32_t free_slot = UINT32_MAX;
    for (uint32_t w = 0; w < CBWIN_CACHE_WAYS; w++) {
        const CbwinBaseEntry *e = &c->slots[base + w];
        if (e->size == size && e->seq == seq && e->addr == addr) {
            slot = base + w;
            break;
        }
        if (free_slot == UINT32_MAX && (!e->size || e->seq != seq)) {
            free_slot = base + w;
        }
    }
    if (slot == UINT32_MAX) slot = free_slot;
    if (slot == UINT32_MAX) {
        uint32_t v = c->rr[s] & (CBWIN_CACHE_WAYS - 1u);
        c->rr[s] = (uint8_t)((v + 1u) & (CBWIN_CACHE_WAYS - 1u));
        slot = base + v;
    }
    c->slots[slot] = (CbwinBaseEntry){
        .seq = seq,
        .addr = addr,
        .size = size,
        .win_lo = win_lo,
        .win_hi = win_hi,
        .span = span,
    };
}

// Invalidate snapshots overlapping [lo, hi) ONLY when the write falls outside the entry's patched window
// [win_lo, win_hi). Writes contained inside [win_lo, win_hi) are supplied per draw via CBP.
static inline void cbwin_invalidate_outside(CbwinBaseCache *c, uint64_t seq, uint64_t lo, uint64_t hi) {
    if (!c || hi <= lo) return;
    for (uint32_t i = 0; i < CBWIN_CACHE_SLOTS; i++) {
        CbwinBaseEntry *e = &c->slots[i];
        if (!e->size || e->seq != seq) continue;
        uint64_t end = e->addr <= UINT64_MAX - e->size ? e->addr + e->size : UINT64_MAX;
        if (!(e->addr < hi && lo < end)) continue;
        if (lo >= e->win_lo && hi <= e->win_hi) continue;
        e->size = 0;
    }
}

static inline void cbpatch_dedupe_reset(CbpatchDedupe *d) {
    if (d) memset(d, 0, sizeof *d);
}

static inline uint32_t cbpatch_active_bytes(const CbpatchBuf *p) {
    if (!p || !p->m[0]) return 16u;
    uint32_t nrows = p->m[2];
    if (nrows > 64u) nrows = 64u;
    return 16u + nrows * 16u;
}

static inline bool cbpatch_lookup(const CbpatchDedupe *d, int sg, uint64_t seq, const CbpatchBuf *p,
                                  uint32_t *out_active, CbwinSpan *out_span) {
    uint32_t act = cbpatch_active_bytes(p);
    if (out_active) *out_active = act;
    if (!d || (sg != 0 && sg != 1) || !p || !out_span) return false;
    if (d->stage[sg].active == act && d->stage[sg].seq == seq && !memcmp(d->stage[sg].bytes, p, act)) {
        *out_span = d->stage[sg].span;
        return true;
    }
    return false;
}

static inline void cbpatch_commit(CbpatchDedupe *d, int sg, uint64_t seq, const CbpatchBuf *p, uint32_t active,
                                  CbwinSpan span) {
    if (!d || (sg != 0 && sg != 1) || !p || !active || active > sizeof(CbpatchBuf)) return;
    if (span.ptr) memcpy(span.ptr, p, active);
    d->stage[sg].seq = seq;
    d->stage[sg].active = active;
    memcpy(d->stage[sg].bytes, p, active);
    d->stage[sg].span = span;
}
