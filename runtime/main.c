// BOTWRecompAndroid entry: load modules, set up the kernel, run nn::init::Start on the main thread.
#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>  // (main() becomes SDL_main, which SDLActivity runs)
#endif
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gamelib.h"
#include "paths.h"
#include "settings.h"
#include "hle.h"
#include "kernel.h"

const char *g_romfs_dir;
const char *g_romfs_link;

// rtld normally passes these to nn::init::Start: run every module's init/fini arrays.
static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}

// nn::init::Start calls x2 right after nnosInitialize and x3 after nninitStartup, just before
// nnMain: SDK constructors run early, the game's run once nninitStartup has set up its heap.
// rtld runs a module's DT_INIT, which registers it (and its TLS template) with musl via
// __nnmusl_init_dso and then runs its init_array; fall back to the array if there is no DT_INIT.
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, m->init, m->init_array_sz / 8);
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}

static void rtld_init_sdk(Cpu *c) { module_init(c, &g_modules[1]); }
static void rtld_init_main(Cpu *c) { module_init(c, &g_modules[0]); }

// Newer SDKs (BOTW 1.8.2's) also take x4, called if nnMain returns, before nninitFinalizeSdkModule and nnosQuickExit:
// the finalizers of what x3 set up - DT_FINI, else the fini_array backwards (SavageWindRecomp, MIT)
static void rtld_fini_main(Cpu *c) {
    Module *m = &g_modules[0];
    LOG("nnMain returned: fini %s\n", m->name);
    if (m->fini) { guest_call(c, m->fini); return; }
    for (uint64_t i = m->fini_array_sz / 8; i-- > 0;) {
        uint64_t fn = ((uint64_t *)m->fini_array)[i];
        if (fn) guest_call(c, fn);
    }
}

static void call_sym(Cpu *c, const char *name, bool required) {
    uint64_t a = loader_find_symbol(name);
    if (!a) {
        if (required) FATAL("%s not found\n", name);
        return;
    }
    LOG("[boot] %s\n", name);
    guest_call(c, a);
}

// SDKs before nn::init::Start (BOTW 1.0.0, 2017): there rtld ran the startup sequence itself, calling these by name
// (BOTW's rtld from 0x70: its own imports, resolved against the loaded modules) - the main thread starts here with
// rtld's entry registers (x0 = 0: no exception, x1 = the main thread's handle). Module initializers (DT_INIT) run
// together after __nnDetailInitLibc2, in rtld's module list order (by address: main, then sdk). nnosInitialize's
// second argument is the process argument region (right after rtld there); 0 makes it skip the arguments, as on the
// nn::init::Start path.
static void rtld_start_legacy(Cpu *c) {
    uint64_t handle = c->x[1];
    call_sym(c, "__nnDetailInitLibc0", true);
    c->x[0] = handle, c->x[1] = 0;
    call_sym(c, "nnosInitialize", true);
    call_sym(c, "__nnDetailInitLibc1", true);
    call_sym(c, "nndiagStartup", true);
    call_sym(c, "nninitInitializeSdkModule", false);
    call_sym(c, "nninitInitializeAbortObserver", false);
    call_sym(c, "nninitStartup", true);
    call_sym(c, "__nnDetailInitLibc2", true);
    for (int i = 0; i < g_nmodules; i++) module_init(c, &g_modules[i]);
    { extern void watch_arm(void); watch_arm(); }  // (LARECOMP_WATCH from the start)
    call_sym(c, "nnMain", true);
    call_sym(c, "nninitFinalizeSdkModule", false);
    call_sym(c, "nnosQuickExit", true);
}

// Claim the guest address range before anything else can map there. The host allocator places
// regions at random addresses in the same window; on a collision, re-exec for a fresh layout.
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (plat_reserve_guest(GUEST_LO, GUEST_HI)) return;
    const char *tries = getenv("LARECOMP_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("LARECOMP_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

// ------------------------------------------------------------------ boot / in-place reboot
// Restarting the game and going back to the start screen reboot the game inside this process: the window,
// GPU device, settings and shader caches stay. rt_request_reboot (menu, any thread) makes ui_run return on the
// main thread, which then: stops every guest thread (kernel_stop_threads: each unwinds at its next svc / HLE call /
// wait), resets the renderer (textures, address-keyed caches) and all per-boot host state (RT_ON_RESET hooks:
// services, HLE features, audio), empties the kernel, wipes guest memory, and boots again.
static _Atomic int g_reboot;
void rt_request_reboot(int kind) {
    LOG("[reboot] requested (%s)\n", kind == RT_REBOOT_TITLE ? "start screen" : "boot");
    g_reboot = kind;
}
int rt_reboot_requested(void) { return g_reboot; }

// LARECOMP_REBOOT_AT=<frame>:<game | title>[,...] (test): at that presented frame, restart the game in place, or go
// back to the start screen (with LARECOMP_LAUNCHER_AUTOPLAY it plays again).
// Frames count on across reboots. Called by the presenter every frame.
void reboot_test_frame(uint64_t frame) {
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("LARECOMP_REBOOT_AT"); }
    long at;
    char what[128];
    if (!next || !*next || sscanf(next, "%ld:%127[^,]", &at, what) != 2 || (long)frame < at) return;
    next = strchr(next, ',');
    if (next) next++;
    rt_request_reboot(!strcmp(what, "title") ? RT_REBOOT_TITLE : RT_REBOOT_GAME);
}

#ifdef __APPLE__
extern const ResetFn __start_resets[] __asm__("section$start$__DATA$__rt_reset");
extern const ResetFn __stop_resets[] __asm__("section$end$__DATA$__rt_reset");
#elif defined(__linux__)
extern const ResetFn __start_resets[], __stop_resets[];  // (ELF: defined by the linker)
#else
// PE/COFF: .rst$a < .rst$m (entries, hle.h) < .rst$z (the linker sorts by the text after '$')
__attribute__((section(".rst$a"), used)) static const ResetFn __start_resets[1];
__attribute__((section(".rst$z"), used)) static const ResetFn __stop_resets[1];
#endif

void backend_reset(void);             // renderer: GPU idle, game textures and address-keyed caches dropped
void backend_guest_memory_reset(void);  // renderer: guest memory was replaced (re-wrap the heap)
void backend_clear_window(void);      // a black frame while the game is down
void audio_stop(void);                // services/audio.c: its threads read guest memory the renderer's reset may free

static void reboot_idle(void) {  // (main thread, while guest threads stop) keep the window alive
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); exit(0); }
}

static void teardown(void) {
    uint64_t t0 = plat_now_ns();
    kernel_stop_threads(reboot_idle);
    audio_stop();
    backend_clear_window();
    backend_reset();
    const ResetFn *start = __start_resets, *stop = __stop_resets;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    int n = 0;
    for (const ResetFn *p = start; p < stop; p++)
        if (*p) { (*p)(); n++; }
    kernel_reset();
    if (!plat_reset_guest(GUEST_LO, GUEST_HI)) FATAL("cannot reset guest memory\n");
    backend_guest_memory_reset();
    g_nmodules = 0;
    g_reboot = RT_REBOOT_NONE;
    LOG("[reboot] game torn down in %llums (%d reset hooks)\n", (unsigned long long)((plat_now_ns() - t0) / 1000000), n);
}

static void boot(const char *root) {
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    {   // the game files used in place: romfs.link names the player's own romfs/ (no romfs.bin; the movies from there
        // too) - or their own romfs.bin, read as ours would be (the movies taken out of it into romfs/ at setup)
        char link[1024], target[1024] = "";
        snprintf(link, sizeof link, "%s/romfs.link", root);
        FILE *f = fopen(link, "rb");
        if (f) {
            size_t n = fread(target, 1, sizeof target - 1, f);
            fclose(f);
            target[n] = 0;
            while (n && (target[n - 1] == '\n' || target[n - 1] == '\r')) target[--n] = 0;
            struct stat st;
            if (n) g_romfs_link = strdup(target);
            if (n && !(stat(target, &st) == 0 && S_ISREG(st.st_mode))) g_romfs_dir = g_romfs_link;
        }
    }
    if (!g_romfs_dir) g_romfs_dir = strdup(romfs);

    char why[512];
    native_begin_boot();  // (Native game code: the game's own code runs, no botwgame.so - runtime/native.c)
    if (!native_on() && !gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    { extern void settings_load(void); settings_load(); }
    { extern void benchmark_boot(void); benchmark_boot(); }
    kernel_init();
    if (!native_on()) dispatch_init();
    native_prepare();

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    // nn::init::Start(thread_handle, args_region, init_sdk, init_main[, fini_main]): with x4 in newer SDKs (1.8.2)
    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    bool has_fini = start != 0;
    if (!start) start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) {  // an older SDK: rtld's own startup sequence
        LOG("no nn::init::Start: the older SDK's startup (rtld_start_legacy)\n");
        KThread *t = kernel_create_main_thread(hle_register("<rtld_start_legacy>", rtld_start_legacy), stack_size, prio);
        kernel_run_main(t, 0, t->handle, 0, 0, 0);
        return;
    }
    uint64_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t fini_fn = hle_register("<rtld_init_main>", rtld_init_main);
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    uint64_t fini_main = has_fini ? hle_register("<rtld_fini_main>", rtld_fini_main) : 0;
    LOG("nn::init::Start at %llx (%s)\n", (unsigned long long)start, has_fini ? "init, init, fini" : "init, init");
    kernel_run_main(t, t->handle, 0, init_fn, fini_fn, fini_main);
}

int main(int argc, char **argv) {
    plat_init();
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);  // the Windows CRT treats _IOLBF as full buffering
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    const char *root = "extracted";  // (in the data root)
    bool skip_launcher = getenv("LARECOMP_NO_LAUNCHER") || getenv("LARECOMP_INPUT");  // tests boot straight in
    static char arg_root[4096];  // (>= PATH_MAX: Bionic's fortified realpath checks)
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) skip_launcher = true;
        else if (argv[i][0] != '-' && !realpath(argv[i], arg_root)) root = argv[i];  // (relative to where we started)
        else if (argv[i][0] != '-') root = arg_root;
    }
    rt_paths_init();  // the data root becomes the working directory (runtime/paths.h)
    { extern void benchmark_init(void); benchmark_init(); }  // (LARECOMP_BENCHMARK, hle/benchmark.c)
    for (bool title = !skip_launcher;;) {
        if (title) {  // start screen (runtime/ui/launcher.mm): game files setup, settings
            extern void launcher_run(const char *root);
            launcher_run(root);
        }
        boot(root);
        int kind = ui_run();  // window and input on the main thread; returns when a reboot is requested
        teardown();
        title = kind == RT_REBOOT_TITLE;
    }
}
