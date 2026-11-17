// Builds the game code library on the player's machine (runtime/gamebuild.h).
#include "gamebuild.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"

#ifdef __ANDROID__
// Android (docs/las/ANDROID-PORT.md, Fase 6): the generator runs on the app's Python (libpymain.so, its standard library
// and the generator in LARECOMP_GEN_DIR, android/deps/build_gen_deps.sh) and zig compiles for aarch64-linux-musl: the
// C library's headers only - the library links -nostdlib against libmain.so, and Android's linker resolves its libc
// calls. -O1 as on the desktop (on the phone a build takes minutes; -O2 measured no faster in game).
// -mllvm -dag-maps-huge-region=500: the schedulers' dependency graph of huge runs of stores was quadratic in LAS's
// code (main_012.c 4.3 s -> 0.7 s, 806 of 812 objects byte-identical: LAS Recomp for Android d2da382, from
// LASRecomp-Android-Plus, MIT). BOTW 1.8.2's files (NDK clang on the PC): the same objects, the same times - kept
// for a file of that kind.
// -fno-sanitize=undefined -fno-stack-protector -g0: zig cc reads -O0 as its Debug mode and then adds UBSan
// (-fsanitize=undefined), -fstack-protector-strong and DWARF debug info on its own. 1.6.0's main_536.c (a giant
// function, at -O0: HEAVY_LINES) took 6-8 GB on the POCO F7 Ultra for over 20 minutes, until Android ended the app
// ("swap is low and thrashing", 2026-10-27) - on the PC's NDK clang with those flags, 23 GB and a failure; without
// them 1.45 GB in 19 s. None of it belongs in the game code (UBSan also checks every operation at run time).
#define GAMELIB_CFLAGS "-target aarch64-linux-musl -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -fPIC -mllvm -dag-maps-huge-region=500 -fno-sanitize=undefined -fno-stack-protector -g0"
#define GAMELIB_SHARED "-target aarch64-linux-musl -shared -nostdlib -Wl,-z,max-page-size=16384"
#endif
// From the Makefile: the flags the game code is compiled and linked with (as `make` builds it in a source checkout)
#ifndef GAMELIB_CFLAGS
#define GAMELIB_CFLAGS "-O1 -fno-strict-aliasing -fvisibility=hidden"
#endif
#ifndef GAMELIB_SHARED
#define GAMELIB_SHARED "-shared"
#endif

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// ------------------------------------------------------------------ child processes
// Every one running, so that quitting mid-build ends them too (atexit) instead of leaving compilers behind.
#define MAX_PROCS 80
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static PlatProc g_procs[MAX_PROCS];
static int g_nprocs;
static bool g_quitting;

static void end_all(void) {
    pthread_mutex_lock(&g_plock);
    g_quitting = true;
    for (int i = 0; i < g_nprocs; i++) plat_proc_kill(g_procs[i]);
    g_nprocs = 0;
    pthread_mutex_unlock(&g_plock);
}

static PlatProc spawn(char **argv, const char *log) {
    pthread_mutex_lock(&g_plock);
    PlatProc p = g_quitting ? 0 : plat_spawn((const char *const *)argv, log, false);
    if (p && g_nprocs < MAX_PROCS) g_procs[g_nprocs++] = p;
    pthread_mutex_unlock(&g_plock);
    return p;
}

// plat_proc_wait without blocking, then forgotten once it has ended
static int finished(PlatProc p) {
    pthread_mutex_lock(&g_plock);
    int r = plat_proc_wait(p, false);
    if (r >= 0)
        for (int i = 0; i < g_nprocs; i++)
            if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; break; }
    pthread_mutex_unlock(&g_plock);
    return r;
}

static void stop(PlatProc p) {  // ends one that is still running, then forgotten
    pthread_mutex_lock(&g_plock);
    bool ours = false;
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; ours = true; break; }
    pthread_mutex_unlock(&g_plock);
    if (ours) plat_proc_kill(p);
}

// why: what failed, and how the process ended (r from finished / wait_for); the setup screen shows it (issues #29, #36:
// a hint about free space alone read as "the phone has 2 GB left")
static void failed_why(char *why, size_t why_size, const char *what, int r) {
    if (r == 128 + 9)
        snprintf(why, why_size, "%s: Android stopped the compiler, usually for lack of free memory (RAM)", what);
    else if (r > 128 && r < 255)
        snprintf(why, why_size, "%s: the compiler crashed (signal %d)", what, r - 128);
    else
        snprintf(why, why_size, "%s (exit code %d)", what, r);
}

static void sleep_ms(int ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000ull); }

static int wait_for(PlatProc p, const GameBuildOpts *o) {
    int r;
    while ((r = finished(p)) < 0) {
        // asked from outside while it links (PLAY / Later): the linker is ended at once, not waited out - the
        // objects stay, the next build links again from them, and the UI thread's join is over in a moment,
        // not the link's minutes (a stoppable link, so no app-not-responding)
        if (o && o->stopped && o->stopped()) { stop(p); return 128 + 9; }
        sleep_ms(20);
    }
    return r;
}

// ------------------------------------------------------------------ argument lists
typedef struct { char **v; int n, cap; } Args;
static void add(Args *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = (char **)realloc(a->v, sizeof *a->v * a->cap);
    }
    a->v[a->n++] = strdup(s);
    a->v[a->n] = NULL;
}
static void add_split(Args *a, const char *flags) {  // (space-separated, no quoting)
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", flags);
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) add(a, t);
}
static void args_free(Args *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

// ------------------------------------------------------------------ the tools
typedef struct {
    char gen[1100], gen_script[1100];  // generator: botwgen, or (source checkout) the venv's python + tools/gen.py
    char cc[1100];                     // compiler: zig (as `zig cc`), or LARECOMP_CC / clang
    bool zig;
    char inc[1100], entry[1100], hle[1100];  // headers the game code includes, tools/gamelib_entry.c, HLE names
#ifdef _WIN32
    char implib[1100];  // the .exe's import library (the runtime functions the game code calls)
    char zig_rt[1100];  // the release's zig: its runtime built ahead of time (zig/rt, tools/zig_slim.sh), "" = none
#endif
} Tools;

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

#ifdef __ANDROID__
// The app's: the launchers and libraries in nativeLibraryDir (the only place Android runs binaries from), the rest in
// LARECOMP_GEN_DIR (the activity unpacks it from the APK's assets)
static bool find_tools(Tools *t) {
    memset(t, 0, sizeof *t);
    const char *lib = getenv("LARECOMP_NATIVE_LIB_DIR"), *gen = getenv("LARECOMP_GEN_DIR");
    if (!lib || !*lib || !gen || !*gen) return false;
    snprintf(t->gen, sizeof t->gen, "%s/libpymain.so", lib);
    snprintf(t->gen_script, sizeof t->gen_script, "%s/tools/gen.py", gen);
    snprintf(t->cc, sizeof t->cc, "%s/libzig.so", lib);
    t->zig = true;
    snprintf(t->inc, sizeof t->inc, "%s/gamelib-src", gen);
    snprintf(t->entry, sizeof t->entry, "%s/gamelib-src/gamelib_entry.c", gen);
    snprintf(t->hle, sizeof t->hle, "%s/gamelib-src/hle_symbols.txt", gen);
    return file_exists(t->gen) && file_exists(t->gen_script) && file_exists(t->cc) && file_exists(t->entry) && file_exists(t->hle);
}

// The generator's environment: its Python (home, the bindings and tools), the native libraries it loads
static void android_env(const char *work) {
    const char *lib = getenv("LARECOMP_NATIVE_LIB_DIR"), *gen = getenv("LARECOMP_GEN_DIR");
    char v[2400];
    snprintf(v, sizeof v, "%s/python", gen);
    setenv("PYTHONHOME", v, 1);
    snprintf(v, sizeof v, "%s/site:%s/tools", gen, gen);
    setenv("PYTHONPATH", v, 1);
    setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
    setenv("LD_LIBRARY_PATH", lib, 1);
    setenv("LIBCAPSTONE_PATH", lib, 1);
    setenv("LIBUNICORN_PATH", lib, 1);
    setenv("LIBLZ4_PATH", lib, 1);
    setenv("TMPDIR", work, 1);
    setenv("HOME", work, 1);
    snprintf(v, sizeof v, "%s/zig/lib", gen);  // (zig looks for its lib/ next to itself: it's in nativeLibraryDir)
    setenv("ZIG_LIB_DIR", v, 1);
}
#else
static bool find_tools(Tools *t) {
    memset(t, 0, sizeof *t);
    const char *e = getenv("LARECOMP_GEN");
    if (e && *e) snprintf(t->gen, sizeof t->gen, "%s", e);
    else if (!rt_resource("botwgen" SEP "botwgen" PLAT_EXE_SUFFIX, NULL, t->gen, sizeof t->gen) &&
             !(rt_resource(NULL, "tools/" PLAT_VENV_PYTHON, t->gen, sizeof t->gen) &&
               rt_resource(NULL, "tools/gen.py", t->gen_script, sizeof t->gen_script)))
        return false;
    e = getenv("LARECOMP_CC");
    if (e && *e) snprintf(t->cc, sizeof t->cc, "%s", e);
    else if (rt_resource("zig" SEP "zig" PLAT_EXE_SUFFIX, NULL, t->cc, sizeof t->cc)) t->zig = true;
#ifndef _WIN32
    else if (rt_dev_mode() && file_exists("/usr/bin/clang")) snprintf(t->cc, sizeof t->cc, "/usr/bin/clang");
#endif
    else return false;
    if (!rt_resource("gamelib-src" SEP "cpu.h", "runtime/cpu.h", t->inc, sizeof t->inc) ||
        !rt_resource("gamelib-src" SEP "gamelib_entry.c", "tools/gamelib_entry.c", t->entry, sizeof t->entry) ||
        !rt_resource("gamelib-src" SEP "hle_symbols.txt", "runtime/hle/hle_symbols.txt", t->hle, sizeof t->hle))
        return false;
    char *s = strrchr(t->inc, '/'), *b = strrchr(t->inc, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
#ifdef _WIN32
    if (!rt_resource("botwrecompandroid.dll.a", "build/botwrecompandroid.dll.a", t->implib, sizeof t->implib)) return false;
    t->zig_rt[0] = 0;
    if (t->zig && rt_resource("zig" SEP "rt" SEP "link.rsp", NULL, t->zig_rt, sizeof t->zig_rt)) {
        char *s = strrchr(t->zig_rt, '/'), *b = strrchr(t->zig_rt, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
        else t->zig_rt[0] = 0;
    }
#endif
    return true;
}
#endif

bool gamebuild_available(void) {
    Tools t;
    return find_tools(&t);
}

static void compiler(Args *a, const Tools *t) {
    add(a, t->cc);
    if (t->zig) add(a, "cc");
}

// ------------------------------------------------------------------ generator progress (its log)
typedef struct {
    const char *name;
    char log[1100], out[1100];
    PlatProc p;
    int exit;                // -1 while running
    int files;               // chunk files written so far (complete)
    unsigned long long ci, n;  // functions written / all
} Gen;

static void read_gen_log(Gen *g) {
    FILE *f = fopen(g->log, "rb");
    if (!f) return;
    static char buf[1 << 16];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *last = NULL;
    for (const char *p = strstr(buf, ": progress "); p; p = strstr(p + 1, ": progress ")) last = p;
    unsigned long long ci, all;
    int files;
    if (last && sscanf(last, ": progress %llu/%llu files %d", &ci, &all, &files) == 3) {
        g->ci = ci, g->n = all;
        if (files > g->files) g->files = files;
    }
}

static void log_tail(const char *path, char *out, size_t n) {  // the last line of a log (an error message)
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    char *nl = strrchr(buf, '\n');
    snprintf(out, n, "%s", nl ? nl + 1 : buf);
}

// ------------------------------------------------------------------ compiling
typedef struct {
    char src[1100], obj[1100], log[1100];
    unsigned long long bytes;
    bool o0;  // (a file with a giant function: compiled at -O0 - mark_cold)
    bool alone, marked;  // (a giant function's file: compiled with no other compiler running; mark_cold done)
} Unit;

typedef struct {
    Unit *u;
    int n, cap;
} Units;

static void add_unit(Units *us, const char *src, const char *work, const char *stem) {
    if (us->n == us->cap) {
        us->cap = us->cap ? us->cap * 2 : 64;
        us->u = (Unit *)realloc(us->u, sizeof *us->u * us->cap);
    }
    Unit *u = &us->u[us->n++];
    snprintf(u->src, sizeof u->src, "%s", src);
    snprintf(u->obj, sizeof u->obj, "%s" SEP "%s.o", work, stem);
    snprintf(u->log, sizeof u->log, "%s" SEP "%s.log", work, stem);
    struct stat st;
    u->bytes = stat(src, &st) == 0 ? (unsigned long long)st.st_size : 0;
    u->o0 = u->alone = u->marked = false;
}

// The flags the game code was built with (GAMELIB_CFLAGS, part of its id). zig cc reads -march=<cpu> with its own CPU
// names (x86_64_v3) and takes "-<feature>" off the end, so x86-64-v3 would be CPU "x86": the same CPU as -mcpu there.
static void add_cflags(Args *a, const Tools *t) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", GAMELIB_CFLAGS);
    for (char *f = strtok(buf, " "); f; f = strtok(NULL, " ")) {
        if (t->zig && !strncmp(f, "-march=", 7)) {
            char cpu[128];
            snprintf(cpu, sizeof cpu, "-mcpu=%s", f + 7);
            for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
            add(a, cpu);
        } else {
            add(a, f);
        }
    }
}

// The fast (-O0, Android-only) build's two bits of state, at file scope: the mode flag (opts->fast, read by
// start_compile) and the marker a fast library carries. A fast library is marked, not silent: its .id file is
// "<GAMELIB_ID>-fast" and its own GameLibTables->id says the same (gamelib.c id_matches / gamelib_fast_present).
// A distinct marker is what makes the scheme safe - with the same id as a full one it would look complete and never
// be rebuilt.
static bool g_fast_build;  // (opts->fast: everything at -O0 - the quick first build a phone set up with)
#define GAMELIB_ID_FAST GAMELIB_ID "-fast"

#ifdef __ANDROID__
// Hot / cold (LASRecompAndroid's): compiling the generated C at -O1 is most of the setup's time on a phone, and most of
// the game's functions never run in a play session. gamelib-src/hot_functions_<version>.txt (tools/hot_functions.py:
// the functions sessions of that game version entered, as hex addresses - versions differ in what lives where, so the
// game's version comes from main's build id) keeps those at -O1; every other function in a generated file gets
// optnone - the -O0 code generator, about 2.3x faster to compile. No list (or LARECOMP_GAMEBUILD_HOT=0): all at -O1.
// A file with a giant function (over HEAVY_LABELS labels: a jump table's function has one per instruction) is compiled
// at -O0, hot or not, as android/build_gamelib.sh does on the PC: clang spent hours on each at -O1 (v1.6.0), and
// optnone isn't enough there - -O1 still runs its loop and block frequency analyses over the giant graph (v1.6.0's
// main_1022.c, NDK clang on the PC: 135-277 s with the function optnone, 15 s for the file at -O0). The lists aren't
// part of the game code's id (GAMELIB_ID): they change how fast it is built, not what it does.
#define HEAVY_LABELS 15000
// ... or over HEAVY_LINES lines: 1.6.0's f_7000fb62f0 has 14,525 labels but 167,749 lines (main_536.c, 12 MB of C).
// At -O1 it took the PC's NDK clang 526 s and 1.1 GB; on a phone that is most of an hour holding over a gigabyte while
// the other compilers moved the bar from ~53% to ~80%, where Android ended the app for low memory (issues #25, #47,
// #51; a Galaxy S21 on Reddit). At -O0: 21 s, though still 1.45 GB - so such a file is also compiled alone (Unit.alone).
// The next largest function of any version: 12,249 labels, 24,513 lines (1.0.0); the next largest compile 348 MB.
#define HEAVY_LINES 60000
static uint64_t *g_hot;
static size_t g_nhot;
static unsigned long long g_cold_fns, g_heavy_fns, g_all_fns, g_cold_files;

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void load_hot(const char *path) {
    free(g_hot), g_hot = NULL, g_nhot = 0, g_cold_fns = g_heavy_fns = g_all_fns = g_cold_files = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t cap = 0;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n'))  // (a longer line - a comment: its rest isn't another address)
            for (int ch; (ch = fgetc(f)) != EOF && ch != '\n';) {}
        if (line[0] == '#') continue;
        // "<address>" as the generated code names it, or "main <offset>" / "sdk <offset>" (the module's base plus that)
        const char *p = line;
        uint64_t base = 0;
        if (!strncmp(p, "main ", 5)) base = 0x7000000000ull, p += 5;
        else if (!strncmp(p, "sdk ", 4)) base = 0x7004000000ull, p += 4;
        char *end;
        uint64_t a = base + strtoull(p, &end, 16);
        if (end == p) continue;
        if (g_nhot == cap) g_hot = realloc(g_hot, sizeof *g_hot * (cap = cap ? cap * 2 : 4096));
        g_hot[g_nhot++] = a;
    }
    fclose(f);
    qsort(g_hot, g_nhot, sizeof *g_hot, cmp_u64);
}

// Whether the line at p starts a function definition ("void f_<address>(Cpu *c) {", not a declaration "...;")
static bool def_at(const char *p, uint64_t *addr) {
    if (strncmp(p, "void f_", 7)) return false;
    char *end;
    *addr = strtoull(p + 7, &end, 16);
    return !strncmp(end, "(Cpu *c) {", 10);
}

// The next function definition after the one at p (d: the file's start - its first line counts too): its line's start
static const char *next_def(const char *d, const char *p, uint64_t *addr) {
    if (p == d && def_at(p, addr)) return p;
    while ((p = strstr(p + (p == d ? 0 : 1), "\nvoid f_"))) {
        p++;  // (the line's start)
        if (def_at(p, addr)) return p;
    }
    return NULL;
}

// A generated file's cold functions get optnone, in place (left as it is when there are none). *o0: compile the file at
// -O0 - it has a giant function, or none of its functions is hot (the same code as all of them optnone, but -O1 would
// still run its analyses over them: 3 chunks of 1.8.2's main, NDK clang on the PC, -O1 9.1 s, -O0 4.4 s, 87% optnone
// 6.9 s).
static bool mark_cold(const char *src, bool *o0, bool *heavy) {
    *o0 = *heavy = false;
    FILE *f = fopen(src, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = n > 0 ? malloc((size_t)n + 2) : NULL;
    bool ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(d); return false; }
    d[n] = d[n + 1] = 0;  // (a label test reads two past a newline)
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.cold", src);
    FILE *o = NULL;
    static const char attr[] = "__attribute__((optnone, noinline)) ";
    const char *copied = d;
    uint64_t a, an;
    unsigned defs = 0, hot = 0;
    for (const char *p = next_def(d, d, &a), *q; p && ok; p = q, a = an) {
        q = next_def(d, p + 1, &an);
        const char *stop = q ? q : d + n;
        unsigned labels = 0, lines = 0;
        for (const char *l = p; (l = memchr(l, '\n', (size_t)(stop - l))) && labels <= HEAVY_LABELS && lines <= HEAVY_LINES;
             l++)
            labels += l[1] == 'L' && l[2] == '_', lines++;
        g_all_fns++, defs++;
        if (labels > HEAVY_LABELS || lines > HEAVY_LINES) g_heavy_fns++, *o0 = *heavy = true;
        bool cold = g_nhot && !bsearch(&a, g_hot, g_nhot, sizeof *g_hot, cmp_u64);
        if (!cold) { hot++; continue; }
        g_cold_fns++;
        if (!o && !(o = fopen(tmp, "wb"))) { ok = false; break; }
        ok = fwrite(copied, 1, (size_t)(p - copied), o) == (size_t)(p - copied) && fputs(attr, o) >= 0;
        copied = p;
    }
    if (g_nhot && defs && !hot) {  // (all cold: the file at -O0 as it is)
        *o0 = true;
        g_cold_files++;
        if (o) fclose(o), remove(tmp);
        free(d);
        return ok;
    }
    if (!o) { free(d); return ok; }  // (nothing to mark)
    ok = ok && fwrite(copied, 1, (size_t)(d + n - copied), o) == (size_t)(d + n - copied);
    if (fclose(o) != 0) ok = false;
    free(d);
    if (ok && rename(tmp, src) == 0) return true;
    remove(tmp);
    return false;
}

// The hot list for the game in game_dir: its version from exefs/main's build id (runtime/loader.c)
static void load_hot_for(const char *game_dir) {
    const char *e = getenv("LARECOMP_GAMEBUILD_HOT"), *gen = getenv("LARECOMP_GEN_DIR");
    char path[1200], nso[1100];
    uint8_t head[0x48] = {0};
    snprintf(nso, sizeof nso, "%s/exefs/main", game_dir);
    FILE *f = fopen(nso, "rb");
    int version = f && fread(head, 1, sizeof head, f) == sizeof head ? game_version_of_build(head + 0x40) : 0;
    if (f) fclose(f);
    snprintf(path, sizeof path, "%s/gamelib-src/hot_functions_%d.txt", gen ? gen : ".", version);
    load_hot(e && strcmp(e, "0") ? e : e ? "" : path);
    LOG("[gamebuild] game version %d, hot functions: %zu%s; files with a function over %d labels: -O0\n", version, g_nhot,
        g_nhot ? " (the rest compiled with optnone)" : " (all at -O1)", HEAVY_LABELS);
}
#endif

static PlatProc start_compile(const Tools *t, const Unit *u) {
    Args a = {0};
    compiler(&a, t);
    add_cflags(&a, t);
    if (u->o0 || g_fast_build) add(&a, "-O0");  // (after GAMELIB_CFLAGS' -O1: the last one counts)
    char inc[1200];
    snprintf(inc, sizeof inc, "-I%s", t->inc);
    add(&a, inc);
    // (a fast library's own id says fast: a marker to upgrade, not a finished library - gamelib.c id_matches. The
    // id is part of GAMELIB_ID's hash in a full build's C too: a fast one isn't that library)
    if (g_fast_build) add(&a, "-DGAMELIB_ID=\"" GAMELIB_ID_FAST "\"");
    else add(&a, "-DGAMELIB_ID=\"" GAMELIB_ID "\"");
    add(&a, "-c");
    add(&a, u->src);
    add(&a, "-o");
    add(&a, u->obj);
    PlatProc p = spawn(a.v, u->log);
    args_free(&a);
    return p;
}

// ------------------------------------------------------------------ going on after the app closed
// What the objects in .gamebuild were compiled from: this builder (GAMELIB_ID), its compile flags and the game code's
// build ids. The flags too: 0.2.1 took them from zig's Debug mode (UBSan at -O0) without a new GAMELIB_ID, so a build
// 0.2.0 had begun went on with its objects and the library wanted __ubsan_* symbols (issues #56, #60). The mode too:
// a fast build's objects are -O0 with the fast id - a full build must not go on from them (it would leave a mixed
// library whose .id says complete), and a fast one not from a full build's.
static void build_stamp(const char *game_dir, bool fast, char *out, size_t n) {
    uint32_t h = 2166136261u;  // (FNV-1a)
    for (const char *s = GAMELIB_CFLAGS; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    int k = snprintf(out, n, "%s%s %08x", GAMELIB_ID, fast ? "-fast" : "", h);
    static const char *const mods[2] = {"main", "sdk"};
    for (int m = 0; m < 2 && k > 0 && (size_t)k < n; m++) {
        char p[1100];
        uint8_t id[8] = {0};
        snprintf(p, sizeof p, "%s" SEP "exefs" SEP "%s", game_dir, mods[m]);
        FILE *f = fopen(p, "rb");
        if (f) {
            if (fseek(f, 0x40, SEEK_SET) != 0 || fread(id, 1, sizeof id, f) != sizeof id) memset(id, 0, sizeof id);
            fclose(f);
        }
        k += snprintf(out + k, n - (size_t)k, " %s %02x%02x%02x%02x%02x%02x%02x%02x", mods[m], id[0], id[1], id[2], id[3],
                      id[4], id[5], id[6], id[7]);
    }
}

static bool same_text(const char *path, const char *text) {
    char buf[256] = "";
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return !strcmp(buf, text);
}

static bool unit_done(const Unit *u) {
    char ok[1200];
    snprintf(ok, sizeof ok, "%s.ok", u->obj);
    struct stat st;
    return stat(u->obj, &st) == 0 && st.st_size > 0 && stat(ok, &st) == 0;
}

static void mark_done(const Unit *u) {
    char ok[1200];
    snprintf(ok, sizeof ok, "%s.ok", u->obj);
    FILE *f = fopen(ok, "wb");
    if (f) fclose(f);
}

#ifdef __ANDROID__
// Another compiler now? 8 at once closed the app on a 12 GB tablet after ~20 minutes (issue #18) and drew 11 W from a
// phone until it overheated (issue #25): one more starts only with 1.5 GB of memory available, a second after the last
// one, and on a hot phone no more than half of them run - Android's thermal status moderate or worse, its headroom
// near throttling, or the battery past 42 degrees (the activity's, once a second; a CPU zone read 70 degrees on the
// POCO F7 Ultra from the build's first second, so it doesn't count). One always runs. The same floors in a fast
// (-O0) build: its compilers are lighter, but the giant files' peak is what killed the 4 GB phones (#25/#47/#51),
// whatever the mode.
static bool may_start_another(int nrun, int jobs) {
    static uint64_t last_start;
    static int last_cap = -1;  // (0: waiting for memory)
    uint64_t now = plat_now_ns();
    if (nrun == 0) { last_start = now; return true; }
    if (now - last_start < 1000000000ull) return false;
    PlatDeviceStats s;
    plat_device_stats(&s);
    bool low = s.avail_mb && s.avail_mb < (last_cap == 0 ? 1792u : 1536u);  // (on again with 256 MB more: no flapping)
    bool hot = s.thermal >= 2 || s.headroom >= 0.95f || s.batt_temp_c > 42;
    int cap = low ? 0 : hot ? (jobs + 1) / 2 : jobs;
    if (cap != last_cap) {
        LOG("[gamebuild] %s %d compilers (%u MB available, thermal status %d, headroom %.2f, battery %.0f degrees)\n",
            low ? "no more than the" : "up to", low ? nrun : cap, s.avail_mb, s.thermal, s.headroom, s.batt_temp_c);
        last_cap = cap;
    }
    if (low || nrun >= cap) return false;
    last_start = now;
    return true;
}
#endif

// ------------------------------------------------------------------ the build
bool gamebuild_run(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    g_fast_build = o->fast;
    Tools t;
    if (!find_tools(&t)) {
        snprintf(why, why_size, "the game code builder is missing from this copy of BOTWRecompAndroid");
        return false;
    }
    char work[1100], lib[1100], tmp[1100];
    snprintf(work, sizeof work, "%s" SEP ".gamebuild", game_dir);
    snprintf(lib, sizeof lib, "%s" SEP "%s", game_dir, GAMELIB_NAME);
    snprintf(tmp, sizeof tmp, "%s" SEP "%s.tmp", work, GAMELIB_NAME);
#ifdef __ANDROID__
    // the library in internal storage, next to "the executable" where runtime/gamelib.c finds it (external storage is
    // mounted noexec: dlopen refuses it) - the temporary file there too (a rename can't cross the two). 1.6.0's and
    // 1.8.2's in their own place there (paths.c rt_version_game_lib).
    {
        char exe[1100], own[1024];
        if (rt_version_game_lib(rt_game_dir_version(game_dir), own, sizeof own)) {
            snprintf(lib, sizeof lib, "%s", own);
            snprintf(tmp, sizeof tmp, "%s.tmp", own);
            snprintf(exe, sizeof exe, "%s", own);
            char *sl = strrchr(exe, '/');
            if (sl) *sl = 0, mkdir(exe, 0755);
        } else if (plat_exe_path(exe, sizeof exe)) {
            char *sl = strrchr(exe, '/');
            if (sl) *sl = 0;
            snprintf(lib, sizeof lib, "%s/%s", exe, GAMELIB_NAME);
            snprintf(tmp, sizeof tmp, "%s/%s.tmp", exe, GAMELIB_NAME);
        }
    }
#endif
    // A build the system stopped (the app closed: issues #18, #25) goes on where it was: the objects compiled for the same
    // game code by the same builder stay in .gamebuild (each with a .ok beside it once whole); only the rest is compiled.
    // The mode is part of the stamp: a fast build's -O0 objects are never gone on from by a full one (a mixed library,
    // its .id saying complete), nor a full one's by a fast.
    char stamp[256], stamp_path[1200];
    build_stamp(game_dir, o->fast, stamp, sizeof stamp);
    snprintf(stamp_path, sizeof stamp_path, "%s" SEP "stamp.txt", work);
    bool resume = same_text(stamp_path, stamp);
    if (!resume) rt_remove_game_file(game_dir, ".gamebuild");
    if (!o->keep_current) {
        // The old library and its .id go only when they are to be replaced by this build's result: a build that keeps
        // the current one (the fast library's -O1 upgrade) leaves them in place until the new library is whole - a
        // failed upgrade then changes nothing, and Play during one keeps the library that loaded.
        rt_remove_game_file(game_dir, GAMELIB_NAME);
        rt_remove_game_file(game_dir, GAMELIB_NAME ".id");
    }
    mkdir(work, 0755);
    {
        FILE *sf = fopen(stamp_path, "wb");
        if (sf) fputs(stamp, sf), fclose(sf);
    }
    int resumed = 0;
#ifdef __ANDROID__
    android_env(work);
    if (!o->fast) load_hot_for(game_dir);
    {   // (the previous one in internal storage: a failed build must not leave it looking current)
        if (!o->keep_current) {
            char idp[1200];
            snprintf(idp, sizeof idp, "%s.id", lib);
            remove(lib), remove(idp);
        }
    }
#endif
    if (t.zig) {  // its caches in the work folder too (deleted below), not ~/.cache: hundreds of MB, used once
        char zc[1200];
        snprintf(zc, sizeof zc, "%s" SEP "zig-cache", work);
#ifdef _WIN32
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", zc);
        _putenv_s("ZIG_LOCAL_CACHE_DIR", zc);
#else
        setenv("ZIG_GLOBAL_CACHE_DIR", zc, 1);
        setenv("ZIG_LOCAL_CACHE_DIR", zc, 1);
#endif
    }
    LOG("[gamebuild] generator %s%s%s, compiler %s%s, %d jobs\n", t.gen, t.gen_script[0] ? " " : "", t.gen_script,
        t.cc, t.zig ? " cc" : "", o->jobs);

    // 1. the generator, both modules at once; their finished files are compiled while it goes on
    Gen gens[2] = {{.name = "main"}, {.name = "sdk"}};
    static const char *const base[2] = {"0x7000000000", "0x7004000000"};
    bool ok = true;
    for (int i = 0; i < 2 && ok; i++) {
        Gen *g = &gens[i];
        g->exit = -1;
        snprintf(g->log, sizeof g->log, "%s" SEP "gen_%s.log", work, g->name);
        snprintf(g->out, sizeof g->out, "%s" SEP "%s", work, g->name);
        remove(g->log);  // (a stopped run's log: its file count isn't this run's - read before the new one replaced it)
        char nso[1100];
        snprintf(nso, sizeof nso, "%s" SEP "exefs" SEP "%s", game_dir, g->name);
        Args a = {0};
        add(&a, t.gen);
        if (t.gen_script[0]) add(&a, t.gen_script);
        add(&a, g->name);
        add(&a, nso);
        add(&a, base[i]);
        add(&a, g->out);
        if (i == 1) { add(&a, "--hle"); add(&a, t.hle); }
#ifdef __ANDROID__
        add(&a, "--simd"); add(&a, "asm"); add(&a, "--dyn-base");  // (as android/build_gamelib.sh: the window moves)
#endif
        g->p = spawn(a.v, g->log);
        args_free(&a);
        if (!g->p) { snprintf(why, why_size, "the game code builder didn't start"); ok = false; }
    }

    Units units = {0};
    int queued[2] = {0, 0};  // chunk files of each module put in units so far
    bool tables[2] = {false, false};
    int next = 0, jobs = o->jobs < 1 ? 1 : o->jobs > 64 ? 64 : o->jobs;
    struct { PlatProc p; int unit; } run[64];
    int nrun = 0;
    unsigned long long done_bytes = 0;
    double shown = 0;
    {
        add_unit(&units, t.entry, work, "gamelib_entry");
    }
    while (ok) {
        // asked from outside (PLAY / Later / a setup needs the builders): stop where this is - the compilers end,
        // the work folder stays, the next build goes on from it
        if (o->stopped && o->stopped()) {
            snprintf(why, why_size, "postponed");
            ok = false;
            break;
        }
        // generator progress, newly finished files
        bool gens_done = true;
        double gen_frac = 0;
        unsigned long long written = 0;
        for (int i = 0; i < 2; i++) {
            Gen *g = &gens[i];
            if (g->exit < 0) {
                read_gen_log(g);
                int r = finished(g->p);
                if (r >= 0) {
                    g->exit = r;
                    read_gen_log(g);
                    if (r != 0) {
                        char last[300];
                        log_tail(g->log, last, sizeof last);
                        LOG("[gamebuild] generator (%s) failed: %s\n", g->name, last);
                        snprintf(why, why_size, "the game code couldn't be prepared");
                        ok = false;
                    }
                }
            }
            if (g->exit != 0) gens_done = false;
            for (; queued[i] < g->files; queued[i]++) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_%03d", g->name, queued[i]);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
            }
            if (g->exit == 0 && !tables[i]) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_table", g->name);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
                tables[i] = true;
            }
            gen_frac += g->exit == 0 ? 0.5 : g->n ? 0.5 * (double)g->ci / (double)g->n : 0;
        }
        if (!ok) break;
        for (int k = 0; k < units.n; k++) written += units.u[k].bytes;

        // compiles, in the order the generator wrote them (the largest ready first - LAS Recomp for Android d2da382 -
        // started the biggest files together while the generator still ran: 0.1.8-beta's setups crashed halfway
        // through on phones where 0.1.7's went through, issue #17)
        while (nrun < jobs && next < units.n) {
            if (resume && unit_done(&units.u[next])) {  // (compiled before the app closed)
                done_bytes += units.u[next].bytes;
                if (next > 0) remove(units.u[next].src);
                next++, resumed++;
                continue;
            }
            // (a file the generator's log names but that isn't there yet: waited for while the generator runs - a
            // resumed build once read the stopped run's log and compiled sdk_013.c before it was written again: zig's
            // "file_open FileNotFound", the setup failed a second in, every time; issues #52, #92)
            if (next > 0 && !gens_done && !file_exists(units.u[next].src)) break;
            if (!units.u[next].bytes) {
                struct stat st;
                if (stat(units.u[next].src, &st) == 0) units.u[next].bytes = (unsigned long long)st.st_size;
            }
#ifdef __ANDROID__
            if (!may_start_another(nrun, jobs)) break;
#endif
#ifdef __ANDROID__  // (unit 0 is the shipped entry file; a file that can't be marked just stays at -O1)
            Unit *nu = &units.u[next];
            if (next > 0 && !nu->marked) {  // (fast too: no hot list is loaded, so nothing is marked cold - but the
                nu->marked = true;       //  giant files are still found, and they build alone: Unit.alone, the floors)
                if (!mark_cold(nu->src, &nu->o0, &nu->alone))
                    LOG("[gamebuild] couldn't mark cold functions in %s\n", nu->src);
                if (nu->alone) LOG("[gamebuild] %s has a giant function: at -O0, no other compiler beside it\n", nu->src);
            }
            // (a giant function's file waits for the others to finish, and nothing starts beside it: HEAVY_LINES)
            bool alone_running = false;
            for (int k = 0; k < nrun; k++) alone_running |= units.u[run[k].unit].alone;
            if (alone_running || (nu->alone && nrun > 0)) break;
#endif
            PlatProc p = start_compile(&t, &units.u[next]);
            if (!p) { snprintf(why, why_size, "the compiler didn't start"); ok = false; break; }
            run[nrun].p = p, run[nrun].unit = next++;
            nrun++;
        }
        for (int k = 0; k < nrun; k++) {
            int r = finished(run[k].p);
            if (r < 0) continue;
            Unit *u = &units.u[run[k].unit];
            if (r != 0) {
                char last[300];
                log_tail(u->log, last, sizeof last);
                LOG("[gamebuild] compiling %s failed: %s\n", u->src, last);
                failed_why(why, why_size, "the game code didn't compile", r);
                ok = false;
            } else {
                mark_done(u);
            }
            done_bytes += u->bytes;
            if (run[k].unit > 0) remove(u->src);  // (generated C, done with: not kept. Unit 0 is the shipped entry file)
            run[k--] = run[--nrun];
        }
        if (!ok) break;
        if (gens_done && next == units.n && nrun == 0) break;

        // progress: the generator's share (its functions written), the compiler's (bytes of C compiled, of an
        // estimate of all of it while the generator still writes)
        double est_total = gens_done ? (double)written : gen_frac > 0.02 ? (double)written / gen_frac : 0;
        double comp_frac = est_total > 0 ? (double)done_bytes / est_total : 0;
        if (comp_frac > 1) comp_frac = 1;
        double f = 0.2 * gen_frac + 0.75 * comp_frac;
        double secs = (double)(plat_now_ns() - t0) / 1e9;
        if (gen_frac == 0) f = 0.05 * (secs / (secs + 20));  // (the generator's first pass: no count yet)
        if (f > shown) shown = f;
        if (o->progress) o->progress(shown, gen_frac < 1 && !gens_done ? "Reading the game code..." : "Recompiling...");
        sleep_ms(50);
    }
    for (int k = 0; k < nrun; k++) stop(run[k].p);  // (a failure: the rest is of no use - the failure shows at once)
    for (int i = 0; i < 2; i++)
        if (gens[i].p && gens[i].exit < 0) stop(gens[i].p);

    // 2. link
    if (ok && o->stopped && o->stopped()) ok = false;  // (the link is minutes of its own: not worth it when stopped)
    if (ok) {
        if (o->progress) o->progress(shown > 0.95 ? shown : 0.95, "Finishing...");
        char rsp[1100], log[1100];
        snprintf(rsp, sizeof rsp, "%s" SEP "link.rsp", work);
        snprintf(log, sizeof log, "%s" SEP "link.log", work);
        FILE *f = fopen(rsp, "wb");
        if (f) {
            for (int k = 0; k < units.n; k++) {  // (forward slashes: the response file's backslashes are escapes)
                fputc('"', f);
                for (const char *c = units.u[k].obj; *c; c++) fputc(*c == '\\' ? '/' : *c, f);
                fputs("\"\n", f);
            }
            fclose(f);
        }
        Args a = {0};
        char at[1200];
        snprintf(at, sizeof at, "@%s", rsp);
#ifdef _WIN32
        if (t.zig_rt[0]) {  // the linker itself, with the runtime built ahead of time (its flags and libraries: link.rsp)
            char opt[1200];
            add(&a, t.cc);
            add(&a, "lld-link");
            snprintf(opt, sizeof opt, "@%s" SEP "link.rsp", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-LIBPATH:%s", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-OUT:%s", tmp);
            add(&a, opt);
            add(&a, at);
            add(&a, t.implib);
        } else
#endif
        {
            compiler(&a, &t);
            add_split(&a, GAMELIB_SHARED);
            add(&a, "-o");
            add(&a, tmp);
            add(&a, at);
#ifdef _WIN32
            add(&a, t.implib);
#endif
#ifdef __ANDROID__
            char lm[1200];  // (the runtime functions it calls: libmain.so as its DT_NEEDED)
            snprintf(lm, sizeof lm, "%s/libmain.so", getenv("LARECOMP_NATIVE_LIB_DIR"));
            add(&a, lm);
#endif
        }
        PlatProc p = f ? spawn(a.v, log) : 0;
        args_free(&a);
        int r = p ? wait_for(p, o) : 255;
        if (r == 128 + 9 && o->stopped && o->stopped()) {  // (ended by a stop, not the system: not a failure -
            LOG("[gamebuild] link postponed\n");           //  the objects stay, the next one links again)
            snprintf(why, why_size, "postponed");
            ok = false;
        } else if (r != 0) {
            char last[300];
            log_tail(log, last, sizeof last);
            LOG("[gamebuild] linking failed: %s\n", last);
            failed_why(why, why_size, "the game code didn't link", r);
            ok = false;
        }
    }
    if (ok) {
        // A keep-current build (the fast library's -O1 upgrade) swaps the new library in only here, whole: until then
        // the one that loaded is what runs - a failed build, or the app closed mid-way, changes nothing. The rename
        // replaces the old file in one step (nothing is removed first: a kill between a remove and a rename would
        // leave no library at all); the .id lands after it, of the build that just finished (fast: "<id>-fast").
        char idp[1100];
        snprintf(idp, sizeof idp, "%s.id", lib);
        FILE *f = NULL;
        const char *id = o->fast ? GAMELIB_ID_FAST : GAMELIB_ID;
        if (rename(tmp, lib) != 0 || !(f = fopen(idp, "wb")) || fprintf(f, "%s\n", id) < 0) {
            snprintf(why, why_size, "the game code couldn't be saved");
            ok = false;
        }
        if (f) fclose(f);
    }
    free(units.u);
    if (resumed) LOG("[gamebuild] %d files compiled before the app closed: not again\n", resumed);
    // the C, the objects, the logs - kept after a failure, for the next build to go on from
    if (ok) rt_remove_game_file(game_dir, ".gamebuild");
#ifdef __ANDROID__
    LOG("[gamebuild] %s build: %llu of %llu functions cold (optnone, or %llu files all cold at -O0), %llu giant (their "
        "files at -O0)\n", g_fast_build ? "fast -O0" : "full", g_cold_fns, g_all_fns, g_cold_files, g_heavy_fns);
#endif
    LOG("[gamebuild] %s in %.0fs\n", ok ? "done" : "failed", (double)(plat_now_ns() - t0) / 1e9);
    if (ok && o->progress) o->progress(1, "Finishing...");
    return ok;
}
