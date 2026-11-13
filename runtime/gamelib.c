// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "paths.h"
#include "platform.h"
#include "rt.h"

static const GameLibTables *g_tables;
static char g_error[256];

// The library's id file (<library>.id, written next to it by the build): its GAMELIB_ID, read without loading it.
static bool id_matches(const char *lib) {
    char path[1100], id[128] = {0};
    snprintf(path, sizeof path, "%s.id", lib);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0;
    return !strcmp(id, GAMELIB_ID);
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

#ifdef __ANDROID__
#include <sys/stat.h>
static bool copy_to(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", to);
    FILE *out = fopen(tmp, "wb");
    bool ok = out != NULL;
    static char buf[1 << 20];
    for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
    fclose(in);
    if (out) ok &= fclose(out) == 0;
    ok = ok && rename(tmp, to) == 0;
    if (!ok) remove(tmp);
    return ok;
}

// A 1.6.0 / 1.8.2 library in that version's extracted/ (put there over adb: android/push_game.sh) newer than the one in
// internal storage, or another size, is copied in with its id - as BOTWRecompAndroidActivity does 1.0.0's. Once per
// process: a copy that fails isn't tried again every frame.
// LARECOMP_GAME_LIB_SRC=<file> (development): that library instead - extracted/ of a version the app set up belongs to
// the app, and adb can't write there, but it can write to the app's files/ root.
static void sync_from_game_dir(const char *game_dir, const char *lib) {
    char src[1100], dir[1100];
    const char *e = getenv("LARECOMP_GAME_LIB_SRC");
    if (e && *e) snprintf(src, sizeof src, "%s", e);
    else snprintf(src, sizeof src, "%s/%s", game_dir, GAMELIB_NAME);
    struct stat s, d;
    if (stat(src, &s) != 0 || (stat(lib, &d) == 0 && d.st_size == s.st_size && d.st_mtime >= s.st_mtime)) return;
    static char tried[1100];
    if (!strcmp(tried, lib)) return;
    snprintf(tried, sizeof tried, "%s", lib);
    snprintf(dir, sizeof dir, "%s", lib);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0, mkdir(dir, 0755);
    char sid[1200], did[1200];
    snprintf(sid, sizeof sid, "%s.id", src);
    snprintf(did, sizeof did, "%s.id", lib);
    bool ok = copy_to(src, lib) && copy_to(sid, did);
    LOG("[gamelib] %s -> %s: %s\n", src, lib, ok ? "copied" : "copy failed");
}
#endif

// The places to look, in order (gamelib.h). Returns how many were filled.
static int candidates(const char *game_dir, char out[3][1100]) {
    int n = 0;
    const char *env = getenv("LARECOMP_GAME_LIB");
    if (env && *env) snprintf(out[n++], 1100, "%s", env);
    char own[1024];
    if (game_dir && rt_version_game_lib(rt_game_dir_version(game_dir), own, sizeof own)) {  // 1.6.0 / 1.8.2: only their
#ifdef __ANDROID__                                                                         //  own, never 1.0.0's
        sync_from_game_dir(game_dir, own);
#endif
        snprintf(out[n++], 1100, "%s", own);
        return n;
    }
#ifdef __ANDROID__
    {   // 1.0.0's next to "the executable" (the activity copies it there at start; one set up just now, here)
        char exe[1024];
        if (game_dir && rt_game_dir_version(game_dir) == 100 && plat_exe_path(exe, sizeof exe) && strrchr(exe, '/')) {
            *strrchr(exe, '/') = 0;
            snprintf(own, sizeof own, "%s/%s", exe, GAMELIB_NAME);
            sync_from_game_dir(game_dir, own);
        }
    }
#endif
    char exe_side[1024];
    if (rt_resource(GAMELIB_NAME, NULL, exe_side, sizeof exe_side) && exists(exe_side))
        snprintf(out[n++], 1100, "%s", exe_side);
    if (game_dir) snprintf(out[n++], 1100, "%s/%s", game_dir, GAMELIB_NAME);
    return n;
}

bool gamelib_present(const char *game_dir) {
    if (g_tables) return true;
    char c[3][1100];
    int n = candidates(game_dir, c);
    for (int i = 0; i < n; i++)
        if (exists(c[i]) && id_matches(c[i])) return true;
    return false;
}

// Why gamelib_present says no, for the log (#299: the start screen asked for the game code to be built again, and
// nothing said why): none built, or one built by another app (its .id, this app's GAMELIB_ID), or its .id missing
void gamelib_absent_reason(const char *game_dir, char *out, size_t cap) {
    char c[3][1100];
    int n = candidates(game_dir, c);
    snprintf(out, cap, "no game code built (%s)", n ? c[n - 1] : "?");
    for (int i = 0; i < n; i++) {
        if (!exists(c[i])) continue;
        char path[1200], id[128] = {0};
        snprintf(path, sizeof path, "%s.id", c[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { snprintf(out, cap, "%s has no .id (removed after a failed load?)", c[i]); return; }
        size_t k = fread(id, 1, sizeof id - 1, f);
        fclose(f);
        while (k && (id[k - 1] == '\n' || id[k - 1] == '\r')) id[--k] = 0;
        snprintf(out, cap, "%s was built by another app version (its id %s, this app's %s)", c[i], id, GAMELIB_ID);
        return;
    }
}

static const GameLibTables *open_lib(const char *path) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(g_error, sizeof g_error, "can't load %s (error %lu)", path, GetLastError()); return NULL; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        snprintf(g_error, sizeof g_error, "%s", e ? e : path);
        if (e && strstr(e, "cannot locate symbol")) {  // (built wrong - #56, #60: objects with UBSan from an earlier
            char id[1200];                              //  build: its id goes, and the start screen builds it again)
            snprintf(id, sizeof id, "%s.id", path);
            remove(id);
            snprintf(g_error, sizeof g_error, "%.180s\n\nThe game code will be built again: open the app and set up the "
                     "game files.", e);
        }
        return NULL;
    }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(g_error, sizeof g_error, "%s: not game code", path); return NULL; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || strcmp(t->id, GAMELIB_ID)) {
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
        return NULL;  // (left loaded: nothing of it runs)
    }
#ifdef __ANDROID__
    // its addresses follow the guest window wherever it is (gen.py --dyn-base: botwgame_gb)
    void (*set_base)(uint64_t) = (void (*)(uint64_t))dlsym(h, "botwgame_set_guest_base");
    if (!set_base) {
        snprintf(g_error, sizeof g_error, "%s: not built for Android (gen.py --dyn-base)", path);
        return NULL;
    }
    set_base(GUEST_LO);
#endif
    return t;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    static bool tried;
    if (!tried) {
        tried = true;
        snprintf(g_error, sizeof g_error, "the game code hasn't been built: set up the game files on the start screen");
        char c[3][1100];
        int n = candidates(game_dir, c);
        const char *env = getenv("LARECOMP_GAME_LIB");
        for (int i = 0; i < n && !g_tables; i++) {
            if (!exists(c[i])) continue;
            bool given = i == 0 && env && *env;  // LARECOMP_GAME_LIB: loaded as given, the library's own id decides
            if (!given && !id_matches(c[i])) {
                snprintf(g_error, sizeof g_error, "%s was built for another version", c[i]);
                continue;
            }
            g_tables = open_lib(c[i]);
            if (g_tables) LOG("[gamelib] %s (%llu + %llu functions)\n", c[i], (unsigned long long)*g_tables->main_count,
                              (unsigned long long)*g_tables->sdk_count);
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
