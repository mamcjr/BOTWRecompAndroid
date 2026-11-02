// See mods.h. Android only (this fork's scope): elsewhere there are none.
#include "mods.h"

#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "hle.h"
#include "settings.h"

static bool is_off(const char *name) {  // (mods_off: names separated by '|')
    size_t n = strlen(name);
    for (const char *s = g_settings.mods_off; *s;) {
        const char *e = strchr(s, '|');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == n && !strncmp(s, name, n)) return true;
        if (!e) break;
        s = e + 1;
    }
    return false;
}

static int by_name(const void *a, const void *b) { return strcmp(((const ModInfo *)a)->name, ((const ModInfo *)b)->name); }

// A mod's folder: mods/<name>, or the folder mods/<name>.link names (a mod used in place - Mods > Import a mod folder
// with Game Files > Game files: Use them in place; BOTWRecompAndroidActivity.importMod). false: neither is there.
static bool mod_base(const char *name, char *out, size_t cap) {
    struct stat sb;
    snprintf(out, cap, "mods/%s", name);
    char p[1200];
    snprintf(p, sizeof p, "%s/romfs", out);
    if (!stat(p, &sb) && S_ISDIR(sb.st_mode)) return true;
    snprintf(p, sizeof p, "mods/%s.link", name);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    snprintf(p, sizeof p, "%s/romfs", out);
    return n && !stat(p, &sb) && S_ISDIR(sb.st_mode);
}

int mods_list(ModInfo *out, int max) {
    DIR *d = opendir("mods");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        char name[sizeof out->name], base[1024];
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l >= sizeof name) continue;
        snprintf(name, sizeof name, "%s", e->d_name);
        if (l > 5 && !strcmp(name + l - 5, ".link")) name[l - 5] = 0;  // (in place)
        if (!mod_base(name, base, sizeof base)) continue;
        bool dup = false;  // (a folder and a .link of one name: once)
        for (int k = 0; k < n && !dup; k++) dup = !strcmp(out[k].name, name);
        if (dup) continue;
        snprintf(out[n].name, sizeof out[n].name, "%s", name);
        out[n].on = !is_off(name);  // (by the mod's name: a .link's file name stayed on for good - #167, #190)
        n++;
    }
    closedir(d);
    qsort(out, n, sizeof *out, by_name);
    return n;
}

void mods_set(const char *name, bool on) {
    char list[sizeof g_settings.mods_off] = "";
    for (const char *s = g_settings.mods_off; *s;) {  // the others as they were
        const char *e = strchr(s, '|');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l && !(l == strlen(name) && !strncmp(s, name, l))) {
            size_t k = strlen(list);
            if (k + l + 2 < sizeof list) snprintf(list + k, sizeof list - k, "%s%.*s", k ? "|" : "", (int)l, s);
        }
        if (!e) break;
        s = e + 1;
    }
    size_t k = strlen(list);
    if (!on && k + strlen(name) + 2 < sizeof list) snprintf(list + k, sizeof list - k, "%s%s", k ? "|" : "", name);
    snprintf(g_settings.mods_off, sizeof g_settings.mods_off, "%s", list);
}

// ---- the files of the mods that are on: (path in the RomFS, host path), sorted by path; the first mod wins
typedef struct { char *rel, *host; int rank; } ModFile;  // (rank: the mod's place by name)
// two lists: the game's image and the DLC's (folders named for the DLC's title, 01007EF00011F001); `cur` picks the one
// the lookups below use (mods_select_dlc)
static struct { ModFile *f; int n, cap; } lists[2];
static int cur;
#define files (lists[cur].f)
#define nfiles (lists[cur].n)
#define capfiles (lists[cur].cap)

void mods_select_dlc(bool dlc) { cur = dlc ? 1 : 0; }

static void walk(const char *root, const char *rel, int rank) {
    char dirp[1200];
    snprintf(dirp, sizeof dirp, "%s/%s", root, rel);
    DIR *d = opendir(dirp);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char r2[1024], h2[1200];
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(h2, sizeof h2, "%s/%s", root, r2);
        struct stat sb;
        if (stat(h2, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) { walk(root, r2, rank); continue; }
        if (nfiles == capfiles) { capfiles = capfiles ? capfiles * 2 : 64; files = realloc(files, sizeof *files * capfiles); }
        files[nfiles++] = (ModFile){strdup(r2), strdup(h2), rank};
    }
    closedir(d);
}

static int by_rel(const void *a, const void *b) {
    int c = strcmp(((const ModFile *)a)->rel, ((const ModFile *)b)->rel);
    return c ? c : ((const ModFile *)a)->rank - ((const ModFile *)b)->rank;
}

void mods_scan(void) {
    int was = cur;
    for (cur = 0; cur < 2; cur++) {
        for (int i = 0; i < nfiles; i++) free(files[i].rel), free(files[i].host);
        nfiles = 0;
    }
    ModInfo m[MODS_MAX];
    int n = mods_list(m, MODS_MAX);
    for (int i = 0; i < n; i++) {
        if (!m[i].on) { LOG("[mods] %s: off\n", m[i].name); continue; }
        // (a folder named for the DLC's title is a mod of the DLC's own files - Map/MainField's units, its packs -
        // which go into the DLC's image, not over the game's files of the same paths. Relics of the Past ships one
        // beside the game's, issue #65)
        cur = strcasestr(m[i].name, "01007EF00011F001") ? 1 : 0;
        int before = nfiles;
        char base[1024], root[1100];
        if (!mod_base(m[i].name, base, sizeof base)) continue;
        snprintf(root, sizeof root, "%s/romfs", base);
        walk(root, "", i);
        LOG("[mods] %s: %d files%s\n", m[i].name, nfiles - before, cur ? " (the DLC's)" : "");
    }
    for (cur = 0; cur < 2; cur++) qsort(files, nfiles, sizeof *files, by_rel);
    cur = was;
}

bool mods_romfs_override(const char *path, char *out, size_t cap) {
    int lo = 0, hi = nfiles - 1, at = -1;
    while (lo <= hi) {  // (the first entry with this path: the lowest-ranked mod)
        int mid = (lo + hi) / 2, c = strcmp(path, files[mid].rel);
        if (c <= 0) { if (!c) at = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    if (at < 0) return false;
    snprintf(out, cap, "%s", files[at].host);
    return true;
}

int mods_file_count(void) { return nfiles; }

bool mods_file(int i, const char **path, const char **host) {
    if (i < 0 || i >= nfiles) return false;
    bool repeat = i > 0 && !strcmp(files[i].rel, files[i - 1].rel);  // (a later mod's copy: the first one wins)
    *path = repeat ? NULL : files[i].rel;
    *host = files[i].host;
    return true;
}

const char *mods_note(const char *name, char *buf, size_t cap) {
    buf[0] = 0;
    char dirp[1024], pick[1400] = "";
    if (!mod_base(name, dirp, sizeof dirp)) return buf;
    DIR *d = opendir(dirp);
    if (!d) return buf;
    struct dirent *e;
    int best = -1;
    while ((e = readdir(d))) {  // a .txt; the one that looks like the mod's own note first
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".txt")) continue;
        char low[256];
        snprintf(low, sizeof low, "%s", e->d_name);
        for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
        int score = strstr(low, "sobre") || strstr(low, "about") || strstr(low, "readme") || strstr(low, "credit") ? 1 : 0;
        if (score > best) best = score, snprintf(pick, sizeof pick, "%s/%s", dirp, e->d_name);
    }
    closedir(d);
    if (!pick[0]) return buf;
    FILE *f = fopen(pick, "rb");
    if (!f) return buf;
    char raw[2048];
    size_t n = fread(raw, 1, sizeof raw - 1, f), o = 0;
    fclose(f);
    for (size_t i = 0; i < n && o + 3 < cap;) {  // ASCII and U+0080..U+00FF only (the menu font's)
        unsigned char c = (unsigned char)raw[i];
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (i + len > n) break;
        if (c < 0x80) { if (c != '\r') buf[o++] = (char)c; }
        else if (len == 2 && (c == 0xC2 || c == 0xC3)) buf[o++] = (char)c, buf[o++] = raw[i + 1];
        i += len;
    }
    buf[o] = 0;
    // (symbols dropped at a line's start leave a space: trim them)
    char *w = buf;
    for (char *r = buf; *r;) {
        if ((r == buf || r[-1] == '\n') && *r == ' ') { r++; continue; }
        *w++ = *r++;
    }
    *w = 0;
    return buf;
}
#else
int mods_list(ModInfo *out, int max) { (void)out, (void)max; return 0; }
void mods_set(const char *name, bool on) { (void)name, (void)on; }
void mods_scan(void) {}
bool mods_romfs_override(const char *path, char *out, size_t cap) { (void)path, (void)out, (void)cap; return false; }
int mods_file_count(void) { return 0; }
void mods_select_dlc(bool dlc) { (void)dlc; }
bool mods_file(int i, const char **path, const char **host) { (void)i, (void)path, (void)host; return false; }
const char *mods_note(const char *name, char *buf, size_t cap) { (void)name; if (cap) buf[0] = 0; return buf; }
#endif
