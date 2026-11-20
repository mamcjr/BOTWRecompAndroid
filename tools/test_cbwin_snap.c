// Standalone regression tests for transient-snapshot cbwin cache + compact CBP dedupe.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/vulkan/cbwin_snap.h"

int main(void) {
    CbwinBaseCache cache;
    cbwin_base_reset(&cache);

    uint64_t seq = 10;
    uint64_t addr = 0x200000000ull, sz = 0x10000ull;
    uint64_t wlo = addr + 0x40, whi = addr + 0x200;

    // 1. Miss -> insert -> hit with exact (seq, addr, size, win_lo, win_hi).
    cbwin_base_reset(NULL);
    cbpatch_dedupe_reset(NULL);
    CbwinSpan out = {0};
    assert(!cbwin_base_lookup(NULL, seq, addr, sz, wlo, whi, &out));
    assert(!cbwin_base_lookup(&cache, seq, addr, 0, wlo, whi, &out));
    assert(!cbwin_base_lookup(&cache, seq, addr, sz, wlo, whi, NULL));
    assert(!cbwin_base_lookup(&cache, seq, addr, sz, wlo, whi, &out));
    uint8_t backing_a[16] = {1};
    CbwinSpan sp1 = {.buf = 0x1111, .off = 0x1000, .ptr = backing_a};
    cbwin_base_insert(NULL, seq, addr, sz, wlo, whi, sp1);
    cbwin_base_insert(&cache, seq, addr, 0, wlo, whi, sp1);
    cbwin_invalidate_outside(NULL, seq, addr, addr + sz);
    cbwin_base_insert(&cache, seq, addr, sz, wlo, whi, sp1);
    assert(cbwin_base_lookup(&cache, seq, addr, sz, wlo, whi, &out));
    assert(out.buf == sp1.buf && out.off == sp1.off && out.ptr == sp1.ptr);

    // 2. Write strictly inside [win_lo, win_hi) preserves base snapshot.
    cbwin_invalidate_outside(&cache, seq, addr + 0x40, addr + 0x50);
    cbwin_invalidate_outside(&cache, seq, addr + 0x1f0, addr + 0x200);
    assert(cbwin_base_lookup(&cache, seq, addr, sz, wlo, whi, &out));

    // 3. Mismatched window bounds miss without returning old snapshot.
    assert(!cbwin_base_lookup(&cache, seq, addr, sz, wlo, whi + 16, &out));

    // 4. Write outside [win_lo, win_hi) - or straddling its edge - invalidates base snapshot; hi <= lo is a no-op.
    cbwin_invalidate_outside(&cache, seq, addr + 0x1f8, addr + 0x208);  // straddles end
    assert(!cbwin_base_lookup(&cache, seq, addr, sz, wlo, whi, &out));

    cbwin_base_insert(&cache, seq, addr, sz, wlo, whi, sp1);
    cbwin_invalidate_outside(&cache, seq, wlo - 8, wlo + 8);  // straddles win_lo
    assert(!cbwin_base_lookup(&cache, seq, addr, sz, wlo, whi, &out));

    cbwin_base_insert(&cache, seq, addr, sz, wlo, whi, sp1);
    cbwin_invalidate_outside(&cache, seq, addr + 0x50, addr + 0x50);  // empty hi == lo no-op
    cbwin_invalidate_outside(&cache, seq, addr + 0x50, addr + 0x40);  // inverted hi < lo no-op
    assert(cbwin_base_lookup(&cache, seq, addr, sz, wlo, whi, &out));
    cbwin_invalidate_outside(&cache, seq, addr, addr + 0x10);  // before win_lo
    assert(!cbwin_base_lookup(&cache, seq, addr, sz, wlo, whi, &out));

    // 5. Disjoint writes and writes under different gcopy_gen / seq do not invalidate live snapshot.
    cbwin_base_insert(&cache, seq, addr, sz, wlo, whi, sp1);
    cbwin_invalidate_outside(&cache, seq, addr - 0x100, addr);       // touching start below
    cbwin_invalidate_outside(&cache, seq, addr + sz, addr + sz + 16); // just past end
    cbwin_invalidate_outside(&cache, seq + 1, addr, addr + sz);       // different seq/gen
    assert(cbwin_base_lookup(&cache, seq, addr, sz, wlo, whi, &out));
    // Advancing gcopy_gen << 40 invalidates via seq tag mismatch.
    uint64_t next_gen_seq = seq + (1ull << 40);
    assert(!cbwin_base_lookup(&cache, next_gen_seq, addr, sz, wlo, whi, &out));

    // 6. Deterministic 4-way set associativity + collision eviction in one target set.
    cbwin_base_reset(&cache);
    const uint32_t target_set = cbwin_base_set_idx(0x300000000ull, 0x10000);
    uint64_t keys[5];
    int nk = 0;
    for (uint64_t step = 0; nk < 5; step++) {
        uint64_t cand = 0x300000000ull + (step << 12);
        if (cbwin_base_set_idx(cand, 0x10000) == target_set) keys[nk++] = cand;
    }
    for (int k = 0; k < 4; k++) {
        CbwinSpan s = {.buf = 0x2000u + (uint64_t)k, .off = (uint64_t)k * 0x400u, .ptr = NULL};
        cbwin_base_insert(&cache, seq, keys[k], 0x10000, keys[k], keys[k] + 0x100, s);
    }
    for (int k = 0; k < 4; k++) {
        assert(cbwin_base_lookup(&cache, seq, keys[k], 0x10000, keys[k], keys[k] + 0x100, &out));
        assert(out.buf == 0x2000u + (uint64_t)k && out.off == (uint64_t)k * 0x400u);
    }
    // 5th distinct key in the same set must evict way 0 (keys[0]) while preserving keys[1..3].
    CbwinSpan s4 = {.buf = 0x2004u, .off = 0x1000u, .ptr = NULL};
    cbwin_base_insert(&cache, seq, keys[4], 0x10000, keys[4], keys[4] + 0x100, s4);
    assert(!cbwin_base_lookup(&cache, seq, keys[0], 0x10000, keys[0], keys[0] + 0x100, &out));
    for (int k = 1; k < 5; k++) {
        assert(cbwin_base_lookup(&cache, seq, keys[k], 0x10000, keys[k], keys[k] + 0x100, &out));
        assert(out.buf == 0x2000u + (uint64_t)k && out.off == (uint64_t)k * 0x400u);
    }

    // 6b. cbwin_base_insert overwrites existing (seq, addr, size) in place even when an earlier way is free.
    cbwin_base_reset(&cache);
    CbwinSpan sp_k0 = {.buf = 0x2100u, .off = 0x100u, .ptr = NULL};
    CbwinSpan sp_k1 = {.buf = 0x2101u, .off = 0x200u, .ptr = NULL};
    CbwinSpan sp_k1_upd = {.buf = 0x2109u, .off = 0x900u, .ptr = NULL};
    cbwin_base_insert(&cache, seq, keys[0], 0x10000, keys[0], keys[0] + 0x100, sp_k0);
    cbwin_base_insert(&cache, seq, keys[1], 0x10000, keys[1], keys[1] + 0x100, sp_k1);
    cbwin_invalidate_outside(&cache, seq, keys[0] + 0x200, keys[0] + 0x210);  // frees way 0 while way 1 holds keys[1]
    assert(!cbwin_base_lookup(&cache, seq, keys[0], 0x10000, keys[0], keys[0] + 0x100, &out));
    cbwin_base_insert(&cache, seq, keys[1], 0x10000, keys[1], keys[1] + 0x200, sp_k1_upd);  // must overwrite way 1, not duplicate in way 0
    for (int k = 2; k < 5; k++) {
        CbwinSpan sk = {.buf = 0x2100u + (uint64_t)k, .off = (uint64_t)k * 0x100u, .ptr = NULL};
        cbwin_base_insert(&cache, seq, keys[k], 0x10000, keys[k], keys[k] + 0x100, sk);
    }
    assert(!cbwin_base_lookup(&cache, seq, keys[1], 0x10000, keys[1], keys[1] + 0x100, &out));
    assert(cbwin_base_lookup(&cache, seq, keys[1], 0x10000, keys[1], keys[1] + 0x200, &out));
    assert(out.buf == sp_k1_upd.buf && out.off == sp_k1_upd.off);
    for (int k = 2; k < 5; k++) {
        assert(cbwin_base_lookup(&cache, seq, keys[k], 0x10000, keys[k], keys[k] + 0x100, &out));
        assert(out.buf == 0x2100u + (uint64_t)k);
    }

    // 7. CbpatchDedupe: m[0]==0 -> 16B active prefix; m[0]!=0 -> 16 + m[2]*16 B; full 1040B buffer safety.
    CbpatchDedupe dedupe;
    cbpatch_dedupe_reset(&dedupe);
    CbpatchBuf patch = {0};
    uint32_t act = 0;
    CbwinSpan cbp_out = {0};
    assert(cbpatch_active_bytes(&patch) == 16u);
    assert(!cbpatch_lookup(&dedupe, 0, seq, &patch, &act, &cbp_out));
    assert(act == 16u);

    uint8_t *gpu_buf = calloc(1, sizeof(CbpatchBuf));
    assert(gpu_buf != NULL);
    memset(gpu_buf + 16, 0xAA, sizeof(CbpatchBuf) - 16);
    patch.rows[0][0] = 0xDEADBEEFu;  // past active 16 bytes when m[0]==0: must still dedupe!
    CbwinSpan gpu_sp = {.buf = 0x3001, .off = 0x200, .ptr = gpu_buf};
    cbpatch_commit(&dedupe, 0, seq, &patch, act, gpu_sp);
    assert(*(uint32_t *)gpu_buf == 0u && gpu_buf[16] == 0xAAu);  // only active 16 bytes copied!
    patch.rows[0][0] = 0xCAFEBABEu;
    assert(cbpatch_lookup(&dedupe, 0, seq, &patch, &act, &cbp_out));
    assert(cbp_out.buf == gpu_sp.buf && cbp_out.off == gpu_sp.off);

    // Active patch with 3 rows (64 bytes active) misses old inactive dedupe and updates on row changes only within n*16.
    memset(&patch, 0, sizeof patch);
    patch.m[0] = 4;  // k + 1
    patch.m[1] = 2;
    patch.m[2] = 3;  // 3 rows -> 64 active bytes
    patch.rows[2][3] = 0x12345678u;
    patch.rows[10][0] = 0x99999999u;  // past 3 rows -> ignored!
    assert(!cbpatch_lookup(&dedupe, 0, seq, &patch, &act, &cbp_out));
    assert(act == 64u);
    uint8_t *gpu_buf2 = calloc(1, sizeof(CbpatchBuf));
    assert(gpu_buf2 != NULL);
    memset(gpu_buf2 + 64, 0x55, sizeof(CbpatchBuf) - 64);
    CbwinSpan gpu_sp2 = {.buf = 0x3002, .off = 0x800, .ptr = gpu_buf2};
    cbpatch_commit(&dedupe, 0, seq, &patch, act, gpu_sp2);
    assert(memcmp(gpu_buf2, &patch, 64) == 0 && gpu_buf2[64] == 0x55u);
    patch.rows[10][0] = 0x11111111u;  // outside active 64B: still hits!
    assert(cbpatch_lookup(&dedupe, 0, seq, &patch, &act, &cbp_out));
    patch.rows[2][3] = 0x87654321u;   // inside active 64B: misses!
    assert(!cbpatch_lookup(&dedupe, 0, seq, &patch, &act, &cbp_out));
    // Stage 1 stays independent of Stage 0; seq change invalidates both.
    assert(!cbpatch_lookup(&dedupe, 1, seq, &patch, &act, &cbp_out));
    cbpatch_commit(&dedupe, 1, seq, &patch, act, gpu_sp2);
    assert(cbpatch_lookup(&dedupe, 1, seq, &patch, &act, &cbp_out));
    assert(!cbpatch_lookup(&dedupe, 1, seq + 1, &patch, &act, &cbp_out));

    // cbpatch_active_bytes edge cases (NULL, m[0]!=0 & m[2]==0, m[2]==64, m[2]>64 clamp) + NULL/bounds guards.
    assert(cbpatch_active_bytes(NULL) == 16u);
    CbpatchBuf p_edge = {0};
    p_edge.m[0] = 4u; p_edge.m[2] = 0u;
    assert(cbpatch_active_bytes(&p_edge) == 16u);
    p_edge.m[2] = 64u;
    assert(cbpatch_active_bytes(&p_edge) == sizeof(CbpatchBuf));
    p_edge.m[2] = 100u;
    assert(cbpatch_active_bytes(&p_edge) == sizeof(CbpatchBuf));

    assert(!cbpatch_lookup(NULL, 0, seq, &patch, &act, &cbp_out));
    assert(!cbpatch_lookup(&dedupe, -1, seq, &patch, &act, &cbp_out));
    assert(!cbpatch_lookup(&dedupe, 2, seq, &patch, &act, &cbp_out));
    assert(!cbpatch_lookup(&dedupe, 0, seq, NULL, &act, &cbp_out));
    assert(!cbpatch_lookup(&dedupe, 0, seq, &patch, &act, NULL));
    cbpatch_commit(NULL, 0, seq, &patch, 16u, gpu_sp2);
    cbpatch_commit(&dedupe, -1, seq, &patch, 16u, gpu_sp2);
    cbpatch_commit(&dedupe, 0, seq, NULL, 16u, gpu_sp2);
    cbpatch_commit(&dedupe, 0, seq, &patch, 0u, gpu_sp2);
    cbpatch_commit(&dedupe, 0, seq, &patch, (uint32_t)sizeof(CbpatchBuf) + 1u, gpu_sp2);

    free(gpu_buf);
    free(gpu_buf2);
    puts("PASS: test_cbwin_snap (base snapshot cache, outside invalidation, gcopy_gen, 4-way collision, compact cbp dedupe)");
    return 0;
}
