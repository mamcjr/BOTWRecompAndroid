// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// Ported to C for Banana Native from Eden's src/video_core/frame_gen/lossless_dll.cpp and lsfg_translate.cpp: the user's own Lossless.dll (Lossless Scaling) is a PE image
// whose RCDATA resources hold the frame generation shaders as SPIR-V. The performance set's modules (255, 256, 280..302) are taken from their native-FP16 twins
// (resource ID + 49) and their bindings renumbered in (set, binding) order, as the descriptor set layouts of lsfg_chain.c expect. Unlike Eden there is no translated-module
// cache: reading and renumbering the image takes a few milliseconds. Nothing of the DLL is ever part of this repository or the APK
#include "lsfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOS_MAGIC 0x5A4Du
#define PE_SIGNATURE 0x00004550u
#define PE32_MAGIC 0x010Bu
#define PE32_PLUS_MAGIC 0x020Bu
#define DOS_LFANEW_OFFSET 0x3C
#define COFF_HEADER_SIZE 20
#define OPTIONAL_HEADER_SIZE_OFFSET 16
#define SECTION_HEADER_SIZE 40
#define DATA_DIRECTORY_ENTRY_SIZE 8
#define DATA_DIRECTORY_OFFSET_PE32 96
#define DATA_DIRECTORY_OFFSET_PE32_PLUS 112
#define RESOURCE_DATA_DIRECTORY_INDEX 2
#define RESOURCE_DIRECTORY_SIZE 16
#define RESOURCE_NAMED_COUNT_OFFSET 12
#define RESOURCE_ID_COUNT_OFFSET 14
#define RESOURCE_ENTRY_SIZE 8
#define RESOURCE_SUBDIRECTORY_FLAG 0x80000000u
#define RESOURCE_TYPE_RCDATA 10
#define SPIRV_MAGIC 0x07230203u

typedef struct { const uint8_t *p; size_t n; } img_t;
typedef struct { uint32_t va, vsize, raw, rsize; } section_t;

static int rd16(const img_t *im, size_t off, uint16_t *v) { if (off > im->n || im->n - off < 2) return 0; memcpy(v, im->p + off, 2); return 1; }
static int rd32(const img_t *im, size_t off, uint32_t *v) { if (off > im->n || im->n - off < 4) return 0; memcpy(v, im->p + off, 4); return 1; }

static const uint32_t *ids_all(int *n) {
    static uint32_t ids[LSFG_NMODULES]; static int k;
    if (!k) { ids[k++] = LSFG_SH_MIPMAPS; ids[k++] = LSFG_SH_GENERATE; for (uint32_t id = 280; id <= 302; id++) ids[k++] = id; }
    *n = k; return ids;
}

static size_t rva_to_off(const section_t *s, int ns, uint32_t rva, int *ok) {
    for (int i = 0; i < ns; i++) {
        uint32_t span = s[i].vsize > s[i].rsize ? s[i].vsize : s[i].rsize;
        if (!span || rva < s[i].va) continue;
        uint32_t rel = rva - s[i].va;
        if (rel < span) { *ok = 1; return (size_t)s[i].raw + rel; }
    }
    *ok = 0; return 0;
}

// The RCDATA leaves by name ID: found[id] = the first language's data
typedef struct { uint32_t id; const uint8_t *p; size_t n; } res_t;
static int res_dir(const img_t *im, size_t dir, uint32_t (*ent)[2], int max) {
    uint16_t nn, ni; if (!rd16(im, dir + RESOURCE_NAMED_COUNT_OFFSET, &nn) || !rd16(im, dir + RESOURCE_ID_COUNT_OFFSET, &ni)) return -1;
    int t = nn + ni; if (t > max) t = max;
    for (int i = 0; i < t; i++) { size_t o = dir + RESOURCE_DIRECTORY_SIZE + (size_t)i * RESOURCE_ENTRY_SIZE; if (!rd32(im, o, &ent[i][0]) || !rd32(im, o + 4, &ent[i][1])) return -1; }
    return t;
}
static int collect_rcdata(const img_t *im, const section_t *sec, int ns, size_t base, res_t *out, int max) {
    static uint32_t te[4096][2], ne[4096][2], le[64][2];
    int nt = res_dir(im, base, te, 4096), k = 0; if (nt < 0) return -1;
    for (int a = 0; a < nt; a++) {
        if ((te[a][0] & RESOURCE_SUBDIRECTORY_FLAG) || te[a][0] != RESOURCE_TYPE_RCDATA || !(te[a][1] & RESOURCE_SUBDIRECTORY_FLAG)) continue;
        int nn = res_dir(im, base + (te[a][1] & ~RESOURCE_SUBDIRECTORY_FLAG), ne, 4096); if (nn < 0) return -1;
        for (int b = 0; b < nn; b++) {
            if ((ne[b][0] & RESOURCE_SUBDIRECTORY_FLAG) || !(ne[b][1] & RESOURCE_SUBDIRECTORY_FLAG)) continue;
            int nl = res_dir(im, base + (ne[b][1] & ~RESOURCE_SUBDIRECTORY_FLAG), le, 64); if (nl < 0) return -1;
            for (int c = 0; c < nl; c++) {
                if (le[c][1] & RESOURCE_SUBDIRECTORY_FLAG) continue;
                size_t leaf = base + le[c][1]; uint32_t rva, sz; int ok;
                if (!rd32(im, leaf, &rva) || !rd32(im, leaf + 4, &sz) || !sz) continue;
                size_t off = rva_to_off(sec, ns, rva, &ok);
                if (!ok || off > im->n || im->n - off < sz) continue;
                if (k < max) out[k++] = (res_t){ne[b][0], im->p + off, sz};
                break;
            }
        }
    }
    return k;
}

static int is_spirv(const uint8_t *p, size_t n) { uint32_t m; if (n < 20 || n % 4) return 0; memcpy(&m, p, 4); return m == SPIRV_MAGIC; }

// The bindings in (set, binding) order become 0, 1, 2, ...: each pass's set layout is the list of its descriptors in that order (lsfg_chain.c)
typedef struct { uint32_t set, binding; size_t lit; } slot_t;
static int slot_cmp(const void *a, const void *b) {
    const slot_t *x = a, *y = b;
    if (x->set != y->set) return x->set < y->set ? -1 : 1;
    if (x->binding != y->binding) return x->binding < y->binding ? -1 : 1;
    return x->lit < y->lit ? -1 : x->lit > y->lit;   // stable
}
static void renumber(uint32_t *w, size_t nw) {
    enum { MAXS = 256 };
    slot_t s[MAXS]; int n = 0;
    struct { uint32_t id, set; } sets[MAXS]; int nsets = 0;
    for (size_t o = 5; o + 1 <= nw;) {
        uint32_t len = w[o] >> 16, op = w[o] & 0xffff;
        if (!len || o + len > nw) return;
        if (op == 54) break;   // OpFunction: no decorations after it
        if (op == 71 && len >= 4) {   // OpDecorate
            if (w[o + 2] == 34 && nsets < MAXS) { sets[nsets].id = w[o + 1]; sets[nsets++].set = w[o + 3]; }   // DescriptorSet
            else if (w[o + 2] == 33 && n < MAXS) s[n++] = (slot_t){0, w[o + 3], o + 3};                         // Binding
        }
        o += len;
    }
    for (int i = 0; i < n; i++) { uint32_t id = w[s[i].lit - 2]; for (int j = 0; j < nsets; j++) if (sets[j].id == id) { s[i].set = sets[j].set; break; } }
    qsort(s, (size_t)n, sizeof *s, slot_cmp);
    for (int i = 0; i < n; i++) w[s[i].lit] = (uint32_t)i;
}

static int read_file(const char *path, uint8_t **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return LSFG_NOT_INSTALLED;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return LSFG_UNREADABLE; }
    uint8_t *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return LSFG_UNREADABLE; }
    fclose(f); *out = b; *n = (size_t)sz; return LSFG_OK;
}

static int parse(const img_t *im, res_t *res, int *nres) {
    uint16_t dos; uint32_t pe, sig;
    if (!rd16(im, 0, &dos) || dos != DOS_MAGIC || !rd32(im, DOS_LFANEW_OFFSET, &pe) || !rd32(im, pe, &sig) || sig != PE_SIGNATURE) return LSFG_NOT_PE;
    size_t opt = (size_t)pe + 4 + COFF_HEADER_SIZE; uint16_t om, nsec, osz;
    if (!rd16(im, opt, &om) || (om != PE32_MAGIC && om != PE32_PLUS_MAGIC)) return LSFG_NOT_PE;
    size_t dd = opt + (om == PE32_MAGIC ? DATA_DIRECTORY_OFFSET_PE32 : DATA_DIRECTORY_OFFSET_PE32_PLUS);
    if (!rd16(im, (size_t)pe + 4 + 2, &nsec) || !rd16(im, (size_t)pe + 4 + OPTIONAL_HEADER_SIZE_OFFSET, &osz)) return LSFG_NOT_PE;
    section_t sec[96]; int ns = nsec > 96 ? 96 : nsec;
    size_t tab = (size_t)pe + 4 + COFF_HEADER_SIZE + osz;
    for (int i = 0; i < ns; i++) {
        size_t o = tab + (size_t)i * SECTION_HEADER_SIZE;
        if (!rd32(im, o + 8, &sec[i].vsize) || !rd32(im, o + 12, &sec[i].va) || !rd32(im, o + 16, &sec[i].rsize) || !rd32(im, o + 20, &sec[i].raw)) return LSFG_NOT_PE;
    }
    uint32_t rrva; if (!rd32(im, dd + RESOURCE_DATA_DIRECTORY_INDEX * DATA_DIRECTORY_ENTRY_SIZE, &rrva) || !rrva) return LSFG_MISSING_SHADERS;
    int ok; size_t base = rva_to_off(sec, ns, rrva, &ok); if (!ok) return LSFG_NOT_PE;
    *nres = collect_rcdata(im, sec, ns, base, res, 4096);
    if (*nres < 0) return LSFG_MISSING_SHADERS;
    int nid; const uint32_t *ids = ids_all(&nid);
    for (int i = 0; i < nid; i++) {
        int hit = 0;
        for (int j = 0; j < *nres && !hit; j++) if (res[j].id == ids[i] + LSFG_NATIVE_FP16_OFFSET && is_spirv(res[j].p, res[j].n)) hit = 1;
        if (!hit) return LSFG_MISSING_SHADERS;
    }
    return LSFG_OK;
}

int lsfg_dll_load(const char *path, lsfg_modules *out) {
    memset(out, 0, sizeof *out);
    uint8_t *b; size_t n; int r = read_file(path, &b, &n);
    if (r) return r;
    img_t im = {b, n};
    static res_t res[4096]; int nres = 0;
    r = parse(&im, res, &nres);
    if (!r) {
        int nid; const uint32_t *ids = ids_all(&nid);
        for (int i = 0; i < nid && !r; i++) {
            const res_t *hit = NULL;
            for (int j = 0; j < nres && !hit; j++) if (res[j].id == ids[i] + LSFG_NATIVE_FP16_OFFSET) hit = &res[j];
            uint32_t *w = hit ? malloc(hit->n) : NULL;
            if (!w) { r = LSFG_TRANSLATION_FAILED; break; }
            memcpy(w, hit->p, hit->n);
            renumber(w, hit->n / 4);
            out->m[out->n++] = (lsfg_module){ids[i], (uint32_t)(hit->n / 4), w};
        }
    }
    free(b);
    if (r) lsfg_modules_free(out);
    return r;
}

int lsfg_dll_validate(const char *path) {
    uint8_t *b; size_t n; int r = read_file(path, &b, &n);
    if (r) return r;
    img_t im = {b, n}; static res_t res[4096]; int nres;
    r = parse(&im, res, &nres);
    free(b);
    return r;
}

void lsfg_modules_free(lsfg_modules *m) { for (int i = 0; i < m->n; i++) free(m->m[i].words); memset(m, 0, sizeof *m); }

const lsfg_module *lsfg_module_get(const lsfg_modules *m, uint32_t id) { for (int i = 0; i < m->n; i++) if (m->m[i].id == id) return &m->m[i]; return NULL; }

const char *lsfg_status_name(int s) {
    switch (s) {
    case LSFG_OK: return "ok";
    case LSFG_NOT_INSTALLED: return "no Lossless.dll";
    case LSFG_UNREADABLE: return "Lossless.dll cannot be read";
    case LSFG_NOT_PE: return "not a Windows DLL (PE image)";
    case LSFG_MISSING_SHADERS: return "the DLL lacks the frame generation shaders (another Lossless Scaling version?)";
    case LSFG_TRANSLATION_FAILED: return "a shader could not be taken over";
    default: return "unknown";
    }
}
