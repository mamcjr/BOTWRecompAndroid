// BC1 / BC3 / BC5_UNORM -> ASTC 4x4 (bc1_astc.h). Analytical per-block mapping with zero search:
// - BC1 -> ASTC 4x4 RGB (CEM 8) or dual-plane RGBA (CEM 12) when 3-colour mode has transparent texels.
// - BC3 -> ASTC 4x4 RGBA: constant alpha (CEM 8/12), constant RGB + 8-level alpha (CEM 12 QUANT_192),
//          or dual-plane RGBA (CEM 12 QUANT_48 + QUANT_4); RGB always uses 4-colour mode.
// - BC5_UNORM -> ASTC 4x4 LA (CEM 4, L=BC5.R, A=BC5.G): constant void-extent, single-varying channel
//          (QUANT_256 + QUANT_16), smooth dual-plane (QUANT_256 + QUANT_5), or steep dual-plane (QUANT_64 + QUANT_6).
// Encoding per the Khronos ASTC spec; checked against runtime/nvn/astc.c (bit-exact with ARM's decoder).
#include "bc1_astc.h"

#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

typedef struct { uint64_t w[2]; } Bits;
static inline void put(Bits *b, int pos, int n, uint32_t v) {  // bits [pos, pos + n), LSB first
    for (int i = 0; i < n; i++, pos++)
        if (v >> i & 1) b->w[pos >> 6] |= 1ull << (pos & 63);
}

// Smallest T (0..255) for 5 trits and smallest Q (0..127) for 3 quints per ASTC spec C.2.12,
// plus reverse quantization maps for QUANT_48, QUANT_64, and QUANT_192 matching runtime/nvn/astc.c.
static uint8_t g_trit_code[243];
static uint8_t g_quint_code[125];
static uint8_t g_q48_sym[256], g_q48_unq[256];
static uint8_t g_q64_sym[256], g_q64_unq[256];
static uint8_t g_q192_sym[256], g_q192_unq[256];

static const uint8_t k_cunq_48[48] = {
    0,255,16,239,32,223,48,207,65,190,81,174,97,158,113,142,5,250,21,234,38,217,54,201,
    70,185,86,169,103,152,119,136,11,244,27,228,43,212,59,196,76,179,92,163,108,147,124,131,
};
static const uint8_t k_cunq_64[64] = {
    0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,65,69,73,77,81,85,89,93,97,101,105,109,113,117,121,125,
    130,134,138,142,146,150,154,158,162,166,170,174,178,182,186,190,195,199,203,207,211,215,219,223,227,
    231,235,239,243,247,251,255,
};
static const uint8_t k_cunq_192[192] = {
    0,255,4,251,8,247,12,243,16,239,20,235,24,231,28,227,32,223,36,219,40,215,44,211,48,207,52,203,56,
    199,60,195,64,191,68,187,72,183,76,179,80,175,84,171,88,167,92,163,96,159,100,155,104,151,108,147,
    112,143,116,139,120,135,124,131,1,254,5,250,9,246,13,242,17,238,21,234,25,230,29,226,33,222,37,218,
    41,214,45,210,49,206,53,202,57,198,61,194,65,190,69,186,73,182,77,178,81,174,85,170,89,166,93,162,
    97,158,101,154,105,150,109,146,113,142,117,138,121,134,125,130,2,253,6,249,10,245,14,241,18,237,22,
    233,26,229,30,225,34,221,38,217,42,213,46,209,50,205,54,201,58,197,62,193,66,189,70,185,74,181,78,
    177,82,173,86,169,90,165,94,161,98,157,102,153,106,149,110,145,114,141,118,137,122,133,126,129,
};

static void init_qmap(const uint8_t *tbl, int n, uint8_t sym_out[256], uint8_t unq_out[256]) {
    for (int v = 0; v < 256; v++) {
        int best_s = 0, best_d = 1000;
        for (int s = 0; s < n; s++) {
            int d = abs((int)tbl[s] - v);
            if (d < best_d) { best_d = d; best_s = s; }
        }
        sym_out[v] = (uint8_t)best_s;
        unq_out[v] = tbl[best_s];
    }
}

static void tables_init(void) {
    uint8_t seen_t[243] = {0};
    for (int T = 0; T < 256; T++) {
        int t[5], C;
        if ((T >> 2 & 7) == 7) {
            C = (T >> 5 & 7) << 2 | (T & 3);
            t[4] = 2, t[3] = 2;
        } else {
            C = T & 31;
            if ((T >> 5 & 3) == 3) t[4] = 2, t[3] = T >> 7 & 1;
            else t[4] = T >> 7 & 1, t[3] = T >> 5 & 3;
        }
        if ((C & 3) == 3) t[2] = 2, t[1] = C >> 4 & 1, t[0] = (C >> 3 & 1) << 1 | (C >> 2 & 1 & ~(C >> 3) & 1);
        else if ((C >> 2 & 3) == 3) t[2] = 2, t[1] = 2, t[0] = C & 3;
        else t[2] = C >> 4 & 1, t[1] = C >> 2 & 3, t[0] = (C >> 1 & 1) << 1 | (C & 1 & ~(C >> 1) & 1);
        int i = t[0] + 3 * t[1] + 9 * t[2] + 27 * t[3] + 81 * t[4];
        if (!seen_t[i]) seen_t[i] = 1, g_trit_code[i] = (uint8_t)T;
    }
    uint8_t seen_q[125] = {0};
    for (int Q = 0; Q < 128; Q++) {
        int q[3];
        if ((Q >> 1 & 3) == 3 && (Q >> 5 & 3) == 0) {
            uint32_t q0 = (uint32_t)(Q & 1);
            q[2] = (int)(q0 << 2 | ((Q >> 4 & 1) & ~q0 & 1) << 1 | ((Q >> 3 & 1) & ~q0 & 1));
            q[1] = q[0] = 4;
        } else {
            uint32_t C;
            if ((Q >> 1 & 3) == 3) { q[2] = 4; C = (uint32_t)((Q >> 3 & 3) << 3 | (~(Q >> 5) & 3) << 1 | (Q & 1)); }
            else { q[2] = Q >> 5 & 3; C = (uint32_t)(Q & 0x1F); }
            if ((C & 7) == 5) { q[1] = 4; q[0] = (int)(C >> 3 & 3); }
            else { q[1] = (int)(C >> 3 & 3); q[0] = (int)(C & 7); }
        }
        int i = q[0] + 5 * q[1] + 25 * q[2];
        if (i < 125 && !seen_q[i]) seen_q[i] = 1, g_quint_code[i] = (uint8_t)Q;
    }
    init_qmap(k_cunq_48, 48, g_q48_sym, g_q48_unq);
    init_qmap(k_cunq_64, 64, g_q64_sym, g_q64_unq);
    init_qmap(k_cunq_192, 192, g_q192_sym, g_q192_unq);
}
static pthread_once_t g_tables_once = PTHREAD_ONCE_INIT;

static int put_ise(Bits *b, int pos, const uint8_t *sym, int n, int nb, int trits, int quints) {
    int p = pos;
    if (trits) {
        static const uint8_t tb[5] = {2, 2, 1, 2, 1}, ts[5] = {0, 2, 4, 5, 7};
        uint32_t mask = (1u << nb) - 1u;
        for (int g = 0; g < n; g += 5) {
            int k = n - g < 5 ? n - g : 5, idx = 0;
            for (int j = k - 1; j >= 0; j--) idx = idx * 3 + (sym[g + j] >> nb);
            uint32_t T = g_trit_code[idx];
            for (int j = 0; j < k; j++) {
                if (nb) put(b, p, nb, sym[g + j] & mask), p += nb;
                put(b, p, tb[j], T >> ts[j]), p += tb[j];
            }
        }
    } else if (quints) {
        static const uint8_t qb[3] = {3, 2, 2}, qs[3] = {0, 3, 5};
        uint32_t mask = (1u << nb) - 1u;
        for (int g = 0; g < n; g += 3) {
            int k = n - g < 3 ? n - g : 3, idx = 0;
            for (int j = k - 1; j >= 0; j--) idx = idx * 5 + (sym[g + j] >> nb);
            uint32_t Q = g_quint_code[idx];
            for (int j = 0; j < k; j++) {
                if (nb) put(b, p, nb, sym[g + j] & mask), p += nb;
                put(b, p, qb[j], Q >> qs[j]), p += qb[j];
            }
        }
    } else {
        for (int i = 0; i < n; i++) put(b, p, nb, sym[i]), p += nb;
    }
    return p - pos;
}

// Weights stored bit-reversed from bit 127 down. Returns length in bits.
static int put_weights_ise(Bits *b, const uint8_t *w, int n, int nb, int trits, int quints) {
    Bits s = {{0, 0}};
    int len = put_ise(&s, 0, w, n, nb, trits, quints);
    for (int i = 0; i < len; i++)
        if (s.w[i >> 6] >> (i & 63) & 1) b->w[(127 - i) >> 6] |= 1ull << ((127 - i) & 63);
    return len;
}

static int unq7(int q) { return q << 1 | q >> 6; }  // (a 7-bit endpoint back to 8 bits: bit replication)
static int q7(int e) {  // (the nearest 7-bit endpoint)
    int q = e >> 1;
    return q < 127 && unq7(q + 1) - e < e - unq7(q) ? q + 1 : q;
}

static void expand565(uint32_t c, int e[3]) {  // (bcdec's expansion: the colours it decoded before)
    e[0] = (int)(((c >> 11 & 31) * 527 + 23) >> 6);
    e[1] = (int)(((c >> 5 & 63) * 259 + 33) >> 6);
    e[2] = (int)(((c & 31) * 527 + 23) >> 6);
}

static void bc1_one(const uint8_t *in, uint8_t *out) {
    uint32_t c0 = in[0] | in[1] << 8, c1 = in[2] | in[3] << 8;
    uint32_t idx = in[4] | in[5] << 8 | in[6] << 16 | (uint32_t)in[7] << 24;
    int e0[3], e1[3];
    expand565(c0, e0);
    expand565(c1, e1);
    bool four = c0 > c1, clear = false;
    if (!four)
        for (int i = 0; i < 16; i++) clear |= (idx >> 2 * i & 3) == 3;
    Bits b = {{0, 0}};
    uint8_t w[32];
    if (!clear) {
        // Index -> weight: 4 colours 0, 3, 1, 2 (of 0..3); 3 colours 0, 2, 1 (of 0..2). The second endpoint's sum must
        // be at least the first's (else the decoder blue-contracts): swapped with the weights mirrored if not.
        static const uint8_t map4[4] = {0, 3, 1, 2}, map3[4] = {0, 2, 1, 0};
        int top = four ? 3 : 2;
        bool swap = e1[0] + e1[1] + e1[2] < e0[0] + e0[1] + e0[2];
        for (int i = 0; i < 16; i++) {
            int q = (four ? map4 : map3)[idx >> 2 * i & 3];
            w[i] = (uint8_t)(swap ? top - q : q);
        }
        put(&b, 0, 11, four ? 0x42 : 0x51);  // (4x4 weights, one plane; 4 levels / 3 levels)
        put(&b, 13, 4, 8);                   // (one partition: bits 11-12 zero; CEM 8)
        const int *a = swap ? e1 : e0, *z = swap ? e0 : e1;
        for (int c = 0; c < 3; c++) put(&b, 17 + 16 * c, 8, (uint32_t)a[c]), put(&b, 25 + 16 * c, 8, (uint32_t)z[c]);
        put_weights_ise(&b, w, 16, four ? 2 : 0, !four, 0);
    } else {
        // Dual plane: RGB on plane 1 (a transparent texel on the darker endpoint), alpha on plane 2. 52 weight bits
        // leave 57 for 8 endpoint values: 7 bits each (bit replication back to 8).
        int q0[3], q1[3], u0 = 0, u1 = 0;
        for (int c = 0; c < 3; c++) {
            q0[c] = q7(e0[c]), q1[c] = q7(e1[c]);
            u0 += unq7(q0[c]), u1 += unq7(q1[c]);
        }
        bool swap = u1 < u0;
        static const uint8_t map3[3] = {0, 2, 1};
        for (int i = 0; i < 16; i++) {
            int k = idx >> 2 * i & 3;
            int q = k == 3 ? (u0 <= u1 ? 0 : 2) : map3[k];
            w[2 * i] = (uint8_t)(swap ? 2 - q : q);
            w[2 * i + 1] = k == 3 ? 0 : 2;
        }
        put(&b, 0, 11, 0x451);  // (4x4 weights, two planes, 3 levels)
        put(&b, 13, 4, 12);     // (CEM 12: RGBA direct)
        const int *a = swap ? q1 : q0, *z = swap ? q0 : q1;
        for (int c = 0; c < 3; c++) put(&b, 17 + 14 * c, 7, (uint32_t)a[c]), put(&b, 24 + 14 * c, 7, (uint32_t)z[c]);
        put(&b, 59, 7, 0), put(&b, 66, 7, 127);  // (alpha 0 .. 255)
        int len = put_weights_ise(&b, w, 32, 0, 1, 0);
        put(&b, 128 - len - 2, 2, 3);  // (the second plane's component: alpha)
    }
    memcpy(out, b.w, 16);
}

// Unsigned BC4/BC3-alpha 8-byte block -> 16 exact texels and actual [lo, hi].
static void decode_bc4u_block(const uint8_t *in, uint8_t vals[16], uint8_t *lo_out, uint8_t *hi_out) {
    uint8_t lut[8];
    uint32_t a0 = in[0], a1 = in[1];
    lut[0] = (uint8_t)a0;
    lut[1] = (uint8_t)a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; i++) lut[1 + i] = (uint8_t)(((7 - i) * a0 + i * a1 + 3) / 7);
    } else {
        for (int i = 1; i <= 4; i++) lut[1 + i] = (uint8_t)(((5 - i) * a0 + i * a1 + 2) / 5);
        lut[6] = 0;
        lut[7] = 255;
    }
    uint64_t idx = 0;
    for (int i = 7; i >= 2; i--) idx = (idx << 8) | in[i];
    uint8_t lo = 255, hi = 0;
    for (int i = 0; i < 16; i++, idx >>= 3) {
        uint8_t v = lut[idx & 7u];
        vals[i] = v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    *lo_out = lo;
    *hi_out = hi;
}

static inline int pick_weight(int v, int e0, int e1, const uint8_t *wt, int nwt) {
    if (v <= e0) return 0;
    if (v >= e1) return nwt - 1;
    int best_k = 0, best_d = 1000;
    for (int k = 0; k < nwt; k++) {
        int dec = (e0 * (64 - wt[k]) + e1 * wt[k] + 32) >> 6;
        int d = abs(dec - v);
        if (d < best_d) { best_d = d; best_k = k; }
    }
    return best_k;
}

static void bc3_one(const uint8_t *in, uint8_t *out) {
    static const uint8_t map4[4] = {0, 3, 1, 2};
    static const uint8_t wt4[4] = {0, 21, 43, 64};
    static const uint8_t wt8[8] = {0, 9, 18, 27, 37, 46, 55, 64};

    uint8_t avals[16], alo, ahi;
    decode_bc4u_block(in, avals, &alo, &ahi);

    // BC3 RGB always operates in 4-colour mode regardless of c0 <= c1.
    const uint8_t *rgb = in + 8;
    uint32_t c0 = rgb[0] | rgb[1] << 8, c1 = rgb[2] | rgb[3] << 8;
    uint32_t idx = rgb[4] | rgb[5] << 8 | rgb[6] << 16 | (uint32_t)rgb[7] << 24;
    int e0[3], e1[3];
    expand565(c0, e0);
    expand565(c1, e1);

    uint32_t k0 = idx & 3u;
    bool same_idx = true;
    for (int i = 1; i < 16; i++) {
        if ((idx >> 2 * i & 3u) != k0) { same_idx = false; break; }
    }
    bool const_rgb = (c0 == c1) || same_idx;

    Bits b = {{0, 0}};
    uint8_t w[32];
    if (alo == ahi) {
        // Constant alpha: single-plane QUANT_256 + QUANT_4 RGB weights.
        bool swap = e1[0] + e1[1] + e1[2] < e0[0] + e0[1] + e0[2];
        for (int i = 0; i < 16; i++) {
            int q = map4[idx >> 2 * i & 3u];
            w[i] = (uint8_t)(swap ? 3 - q : q);
        }
        const int *a = swap ? e1 : e0, *z = swap ? e0 : e1;
        put(&b, 0, 11, 0x42);  // (4x4, 1 plane, QUANT_4)
        if (alo == 255) {
            put(&b, 13, 4, 8);
            for (int c = 0; c < 3; c++) put(&b, 17 + 16 * c, 8, (uint32_t)a[c]), put(&b, 25 + 16 * c, 8, (uint32_t)z[c]);
        } else {
            put(&b, 13, 4, 12);
            for (int c = 0; c < 3; c++) put(&b, 17 + 16 * c, 8, (uint32_t)a[c]), put(&b, 25 + 16 * c, 8, (uint32_t)z[c]);
            put(&b, 65, 8, alo), put(&b, 73, 8, alo);
        }
        put_weights_ise(&b, w, 16, 2, 0, 0);
    } else if (const_rgb) {
        // Constant RGB + varying alpha: single-plane QUANT_8 (8-level weights), CEM 12 QUANT_192.
        static const uint8_t rgb_w[4] = {0, 3, 1, 2};
        int rw = (c0 == c1) ? 0 : rgb_w[k0];
        uint8_t crgb[3];
        for (int c = 0; c < 3; c++) {
            int v;
            if (rw == 0) v = e0[c];
            else if (rw == 3) v = e1[c];
            else v = (e0[c] * (3 - rw) + e1[c] * rw + 1) / 3;
            crgb[c] = g_q192_sym[v];
        }
        uint8_t qa0 = g_q192_sym[alo], qa1 = g_q192_sym[ahi];
        int ua0 = g_q192_unq[alo], ua1 = g_q192_unq[ahi];
        uint8_t ep[8] = {crgb[0], crgb[0], crgb[1], crgb[1], crgb[2], crgb[2], qa0, qa1};
        for (int i = 0; i < 16; i++) {
            w[i] = (uint8_t)pick_weight(avals[i], ua0, ua1, wt8, 8);
        }
        put(&b, 0, 11, 0x53);  // (4x4, 1 plane, QUANT_8)
        put(&b, 13, 4, 12);    // (CEM 12: RGBA direct)
        put_ise(&b, 17, ep, 8, 6, 1, 0);
        put_weights_ise(&b, w, 16, 3, 0, 0);
    } else {
        // General case: dual-plane QUANT_4 (wbits=64), CEM 12 QUANT_48 (45 bits), CCS=3 at bit 62.
        int q0[3], q1[3], u0 = 0, u1 = 0;
        for (int c = 0; c < 3; c++) {
            q0[c] = g_q48_sym[e0[c]], q1[c] = g_q48_sym[e1[c]];
            u0 += g_q48_unq[e0[c]], u1 += g_q48_unq[e1[c]];
        }
        bool swap = u1 < u0;
        uint8_t qa0 = g_q48_sym[alo], qa1 = g_q48_sym[ahi];
        int ua0 = g_q48_unq[alo], ua1 = g_q48_unq[ahi];
        const int *a = swap ? q1 : q0, *z = swap ? q0 : q1;
        uint8_t ep[8] = {
            (uint8_t)a[0], (uint8_t)z[0], (uint8_t)a[1], (uint8_t)z[1],
            (uint8_t)a[2], (uint8_t)z[2], qa0, qa1
        };
        for (int i = 0; i < 16; i++) {
            int q = map4[idx >> 2 * i & 3u];
            w[2 * i] = (uint8_t)(swap ? 3 - q : q);
            w[2 * i + 1] = (uint8_t)pick_weight(avals[i], ua0, ua1, wt4, 4);
        }
        put(&b, 0, 11, 0x442);  // (4x4, dual-plane, QUANT_4)
        put(&b, 13, 4, 12);     // (CEM 12: RGBA direct)
        put_ise(&b, 17, ep, 8, 4, 1, 0);
        put(&b, 62, 2, 3);      // (CCS = 3: plane 2 is alpha)
        put_weights_ise(&b, w, 32, 2, 0, 0);
    }
    memcpy(out, b.w, 16);
}

static void bc5u_one(const uint8_t *in, uint8_t *out) {
    static const uint8_t wt5[5] = {0, 16, 32, 48, 64};
    static const uint8_t wt6[6] = {0, 12, 25, 39, 52, 64};
    static const uint8_t sym6[6] = {0, 2, 4, 5, 3, 1};
    static const uint8_t wt16[16] = {0, 4, 8, 12, 17, 21, 25, 29, 35, 39, 43, 47, 52, 56, 60, 64};

    uint8_t xvals[16], yvals[16], xlo, xhi, ylo, yhi;
    decode_bc4u_block(in, xvals, &xlo, &xhi);
    decode_bc4u_block(in + 8, yvals, &ylo, &yhi);

    Bits b = {{0, 0}};
    if (xlo == xhi && ylo == yhi) {
        // Constant LA block: LDR Void-Extent (exact 0 error).
        uint16_t rx = (uint16_t)xlo * 257u, ry = (uint16_t)ylo * 257u;
        b.w[0] = 0xFFFFFFFFFFFFFDFCull;
        b.w[1] = (uint64_t)rx | ((uint64_t)rx << 16) | ((uint64_t)rx << 32) | ((uint64_t)ry << 48);
    } else if (xlo == xhi || ylo == yhi) {
        // One channel varies: single-plane CEM 4 QUANT_256 + QUANT_16 (16-level weights for both 6- and 8-level modes).
        const uint8_t *vv = xlo != xhi ? xvals : yvals;
        int vlo = xlo != xhi ? xlo : ylo, vhi = xlo != xhi ? xhi : yhi;
        uint8_t w[16];
        for (int i = 0; i < 16; i++)
            w[i] = (uint8_t)pick_weight(vv[i], vlo, vhi, wt16, 16);
        put(&b, 0, 11, 0x242);  // (4x4, 1 plane, QUANT_16)
        put(&b, 13, 4, 4);      // (CEM 4: LA direct)
        put(&b, 17, 8, xlo), put(&b, 25, 8, xhi);
        put(&b, 33, 8, ylo), put(&b, 41, 8, yhi);
        put_weights_ise(&b, w, 16, 4, 0, 0);
    } else {
        int dx = xhi - xlo, dy = yhi - ylo;
        uint8_t w[32];
        if (dx <= 48 && dy <= 48) {
            // Smooth dual-plane CEM 4: QUANT_256 exact 8-bit endpoints + QUANT_5 quint weights (126 <= 128 bits).
            for (int i = 0; i < 16; i++) {
                w[2 * i] = (uint8_t)pick_weight(xvals[i], xlo, xhi, wt5, 5);
                w[2 * i + 1] = (uint8_t)pick_weight(yvals[i], ylo, yhi, wt5, 5);
            }
            put(&b, 0, 11, 0x452);  // (4x4, dual-plane, QUANT_5)
            put(&b, 13, 4, 4);      // (CEM 4: LA direct)
            put(&b, 17, 8, xlo), put(&b, 25, 8, xhi);
            put(&b, 33, 8, ylo), put(&b, 41, 8, yhi);
            put(&b, 51, 2, 3);      // (CCS = 3: plane 2 is alpha = logical G)
            put_weights_ise(&b, w, 32, 0, 0, 1);
        } else {
            // Steep dual-plane CEM 4: QUANT_64 6-bit endpoints + QUANT_6 1b+trit weights (127 <= 128 bits).
            int ux0 = g_q64_unq[xlo], ux1 = g_q64_unq[xhi];
            int uy0 = g_q64_unq[ylo], uy1 = g_q64_unq[yhi];
            for (int i = 0; i < 16; i++) {
                w[2 * i] = sym6[pick_weight(xvals[i], ux0, ux1, wt6, 6)];
                w[2 * i + 1] = sym6[pick_weight(yvals[i], uy0, uy1, wt6, 6)];
            }
            put(&b, 0, 11, 0x443);  // (4x4, dual-plane, QUANT_6)
            put(&b, 13, 4, 4);      // (CEM 4: LA direct)
            put(&b, 17, 6, g_q64_sym[xlo]), put(&b, 23, 6, g_q64_sym[xhi]);
            put(&b, 29, 6, g_q64_sym[ylo]), put(&b, 35, 6, g_q64_sym[yhi]);
            put(&b, 42, 2, 3);      // (CCS = 3: plane 2 is alpha = logical G)
            put_weights_ise(&b, w, 32, 1, 1, 0);
        }
    }
    memcpy(out, b.w, 16);
}

void bc1_to_astc(const uint8_t *bc1, uint8_t *astc, uint64_t n) {
    pthread_once(&g_tables_once, tables_init);
    for (uint64_t i = 0; i < n; i++) bc1_one(bc1 + 8 * i, astc + 16 * i);
}

void bc3_to_astc(const uint8_t *bc3, uint8_t *astc, uint64_t n) {
    pthread_once(&g_tables_once, tables_init);
    for (uint64_t i = 0; i < n; i++) bc3_one(bc3 + 16 * i, astc + 16 * i);
}

void bc5_unorm_to_astc_la(const uint8_t *bc5, uint8_t *astc, uint64_t n) {
    pthread_once(&g_tables_once, tables_init);
    for (uint64_t i = 0; i < n; i++) bc5u_one(bc5 + 16 * i, astc + 16 * i);
}
