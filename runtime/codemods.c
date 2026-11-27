// Code mods: runtime/codemods.h.
#include "codemods.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "mods.h"

#ifdef __ANDROID__
#include <dirent.h>
#endif

bool g_codemods_active;

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

// An NSO header's image end: the data segment's end plus bss, at the Switch's 4 KB pages
static uint64_t nso_end(const uint8_t h[0x40]) {
    uint32_t data_off, data_size, bss;
    memcpy(&data_off, h + 0x34, 4), memcpy(&data_size, h + 0x38, 4), memcpy(&bss, h + 0x3C, 4);
    return ((uint64_t)data_off + data_size + bss + 0xFFF) & ~0xFFFull;
}

static bool read_header(const char *path, uint8_t h[0x60]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(h, 1, 0x60, f) == 0x60 && !memcmp(h, "NSO0", 4);
    fclose(f);
    return ok;
}

uint64_t codemods_main_end(const char *main_path) {
    uint8_t h[0x60];
    return read_header(main_path, h) ? nso_end(h) : 0;
}

uint64_t codemods_module_size(const char *path) {
    uint8_t h[0x60];
    return read_header(path, h) ? nso_end(h) : 0;
}

uint64_t codemods_apply_ips(const char *path, uint8_t *image, uint64_t size) {
    size_t len = 0;
    uint8_t *d = read_all(path, &len);
    if (!d) return 0;
    bool wide = len >= 5 && !memcmp(d, "IPS32", 5);
    if (!wide && (len < 5 || memcmp(d, "PATCH", 5))) {
        LOG("[codemods] %s: not an IPS patch\n", path);
        free(d);
        return 0;
    }
    size_t p = 5, osz = wide ? 4 : 3;
    uint64_t far = 0;
    unsigned nrec = 0, skipped = 0;
    while (p + osz <= len) {
        if (!memcmp(d + p, wide ? "EEOF" : "EOF", osz)) break;
        uint64_t off = 0;
        for (size_t i = 0; i < osz; i++) off = off << 8 | d[p + i];
        if (p + osz + 2 > len) break;
        size_t n = (size_t)d[p + osz] << 8 | d[p + osz + 1];
        p += osz + 2;
        const uint8_t *src = d + p;
        uint8_t fill = 0;
        bool rle = n == 0;
        if (rle) {
            if (p + 3 > len) break;
            n = (size_t)d[p] << 8 | d[p + 1], fill = d[p + 2], p += 3;
        } else {
            if (p + n > len) break;
            p += n;
        }
        if (off < 0x100 || off - 0x100 + n > size) { skipped++; continue; }  // (the NSO header itself, or past the image)
        uint8_t *dst = image + (off - 0x100);
        if (rle) memset(dst, fill, n);
        else memcpy(dst, src, n);
        if (off - 0x100 + n > far) far = off - 0x100 + n;
        nrec++;
    }
    free(d);
    LOG("[codemods] %s: %u changes%s\n", path, nrec, skipped ? " (some outside the module: left out)" : "");
    return far;
}

void codemods_id_suffix(const CodeMods *cm, char *out, size_t cap) {
    if (!cm || !cm->hash) { if (cap) out[0] = 0; return; }
    snprintf(out, cap, "+%016llx", (unsigned long long)cm->hash);
}

#ifdef __ANDROID__
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull;
    return h;
}

static uint64_t hash_file(uint64_t h, const char *name, const char *path) {
    size_t len = 0;
    uint8_t *b = read_all(path, &len);
    h = fnv(h, name, strlen(name) + 1);
    if (b) h = fnv(h, b, len), free(b);
    return h;
}

// "CD57B23FA4BBAD65803D9788C01821EE.ips" for build id cd57b23f...: the hex digits the same, case aside, once the zeros
// at the end are off (emulators name them with 32 or 64 digits)
static bool ips_for(const char *file, const char *bid) {
    size_t l = strlen(file);
    if (l < 5 || strcasecmp(file + l - 4, ".ips")) return false;
    char a[80] = "", b[80] = "";
    snprintf(a, sizeof a, "%.*s", (int)(l - 4), file);
    snprintf(b, sizeof b, "%s", bid);
    for (char *s = a; *s; s++) *s = (char)tolower((unsigned char)*s);
    for (size_t n = strlen(a); n && a[n - 1] == '0';) a[--n] = 0;
    for (size_t n = strlen(b); n && b[n - 1] == '0';) b[--n] = 0;
    return *a && !strcmp(a, b);
}

static int by_str(const void *x, const void *y) { return strcmp((const char *)x, (const char *)y); }

void codemods_find(const char *main_path, CodeMods *out) {
    memset(out, 0, sizeof *out);
    uint8_t h[0x60];
    if (!read_header(main_path, h)) return;
    char bid[65];
    for (int i = 0; i < 32; i++) snprintf(bid + 2 * i, 3, "%02x", h[0x40 + i]);
    uint64_t hash = 0xcbf29ce484222325ull;
    ModInfo m[MODS_MAX];
    int n = mods_list(m, MODS_MAX);
    for (int i = 0; i < n; i++) {
        if (!m[i].on) continue;
        char base[1024], dir[1100];
        if (!mods_base(m[i].name, base, sizeof base)) continue;
        snprintf(dir, sizeof dir, "%s/exefs", base);
        DIR *d = opendir(dir);
        if (!d) continue;
        char names[64][256];
        int nn = 0;
        struct dirent *e;
        while ((e = readdir(d)) && nn < 64)
            if (e->d_name[0] != '.') snprintf(names[nn++], sizeof names[0], "%s", e->d_name);
        closedir(d);
        qsort(names, nn, sizeof names[0], by_str);  // (the same order every time: the fingerprint too)
        for (int k = 0; k < nn; k++) {
            const char *f = names[k];
            char path[1400];
            snprintf(path, sizeof path, "%s/%s", dir, f);
            if (ips_for(f, bid)) {
                if (out->nips == CODEMODS_IPS_MAX) { LOG("[codemods] %s: too many patches, left out\n", path); continue; }
                snprintf(out->ips[out->nips++], sizeof out->ips[0], "%s", path);
                hash = hash_file(hash, f, path);
                LOG("[codemods] %s: a patch for main\n", path);
            } else if (!strncmp(f, "subsdk", 6) && isdigit((unsigned char)f[6]) && !f[7] && f[6] != '0') {
                bool dup = false;
                for (int j = 0; j < out->nmod; j++) dup |= !strcmp(out->mod_name[j], f);
                if (dup || out->nmod == CODEMODS_MOD_MAX || !codemods_module_size(path)) {
                    LOG("[codemods] %s: left out (%s)\n", path, dup ? "another mod has it" : "too many modules, or not an NSO");
                    continue;
                }
                snprintf(out->mod_name[out->nmod], sizeof out->mod_name[0], "%s", f);
                snprintf(out->mod_path[out->nmod++], sizeof out->mod_path[0], "%s", path);
                hash = hash_file(hash, f, path);
                LOG("[codemods] %s: a module\n", path);
            } else if (strcasecmp(f + (strlen(f) > 4 ? strlen(f) - 4 : 0), ".ips") == 0 || !strcmp(f, "main") ||
                       !strcmp(f, "sdk") || !strcmp(f, "rtld") || !strcmp(f, "subsdk0") || strstr(f, ".pchtxt")) {
                LOG("[codemods] %s: not used (another game version's patch, or a replaced game module)\n", path);
            }
        }
    }
    if (out->nips || out->nmod) out->hash = hash ? hash : 1;
}
#else
void codemods_find(const char *main_path, CodeMods *out) {
    (void)main_path;
    memset(out, 0, sizeof *out);
}
#endif
