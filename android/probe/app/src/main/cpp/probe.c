// BOTWRecompAndroid Android probe (docs/las/ANDROID-PORT.md, Fase 0): where the guest window can go in an app process, and what
// the Vulkan drivers (system, Turnip through adrenotools) offer the backend. JNI entry points for MainActivity.
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <adrenotools/driver.h>

#include "guest_window.h"
#include "sb.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define DESKTOP_LO 0x7000000000ull  // the desktop's fixed window (runtime/rt.h GUEST_LO)

void vk_probe(Sb *b, void *libvulkan);  // vk_probe.c

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

static double gb(uint64_t v) { return (double)v / (1ull << 30); }

// The maps lines overlapping [lo, hi), with their names.
static void maps_overlapping(Sb *b, uint64_t lo, uint64_t hi, int max) {
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return;
    char line[4096];
    int shown = 0, total = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long a, z;
        if (sscanf(line, "%llx-%llx", &a, &z) != 2 || z <= lo || a >= hi) continue;
        if (shown++ < max) sb_printf(b, "    %s%s", line, strchr(line, '\n') ? "" : "\n");
        total++;
    }
    fclose(f);
    if (total > max) sb_printf(b, "    ... e mais %d\n", total - max);
}

static void memory_probe(Sb *b) {
    struct utsname u;
    uname(&u);
    long page = sysconf(_SC_PAGESIZE);
    sb_printf(b, "== Memoria\n");
    sb_printf(b, "kernel: %s %s %s\n", u.sysname, u.release, u.machine);
    sb_printf(b, "pagina: %ld bytes, RAM: %.1f GB\n", page, gb((uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)page));

    enum { MAX_MAPS = 1 << 16 };
    GwRange *maps = malloc(MAX_MAPS * sizeof *maps), holes[64];
    int n = maps ? gw_read_maps(maps, MAX_MAPS) : -1;
    if (n <= 0) {
        sb_printf(b, "ERRO: nao consegui ler /proc/self/maps\n");
        free(maps);
        return;
    }
    uint64_t top = maps[n - 1].hi;
    sb_printf(b, "mapeamentos (adjacentes juntos): %d, menor %#llx, maior fim %#llx -> VA de %d bits\n", n,
              (unsigned long long)maps[0].lo, (unsigned long long)top, 64 - __builtin_clzll(top - 1));

    // The desktop's fixed window: is it free in an app process?
    sb_printf(b, "\njanela fixa do desktop %#llx-%#llx:\n", DESKTOP_LO, DESKTOP_LO + GW_SIZE);
    maps_overlapping(b, DESKTOP_LO, DESKTOP_LO + GW_SIZE, 12);
    void *p = mmap((void *)(uintptr_t)DESKTOP_LO, GW_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == (void *)(uintptr_t)DESKTOP_LO) {
        sb_printf(b, "  reserva fixa: OK (livre neste processo)\n");
        munmap(p, GW_SIZE);
    } else {
        sb_printf(b, "  reserva fixa: FALHOU (%s)\n", p == MAP_FAILED ? strerror(errno) : "endereco diferente");
        if (p != MAP_FAILED) munmap(p, GW_SIZE);
    }

    // Every hole the dynamic placement could use
    int h = gw_holes(maps, n, GW_FLOOR, GW_VA_LIMIT, GW_SIZE, holes, 64);
    sb_printf(b, "\nburacos >= %.1f GB entre 4 GB e 2^39: %d\n", gb(GW_SIZE), h);
    for (int i = 0; i < h; i++)
        sb_printf(b, "  %#012llx-%#012llx  %7.1f GB\n", (unsigned long long)holes[i].lo, (unsigned long long)holes[i].hi,
                  gb(holes[i].hi - holes[i].lo));
    free(maps);

    // The placement itself, and that the window's pages work (first, heap, last)
    uint64_t t0 = now_us();
    uint64_t base = gw_reserve(GW_SIZE, GW_ALIGN, GW_FLOOR, GW_VA_LIMIT);
    uint64_t t1 = now_us();
    if (!base) {
        sb_printf(b, "\nreserva dinamica: FALHOU (nenhum buraco alinhado a 4 GB)\n");
        return;
    }
    sb_printf(b, "\nreserva dinamica: base %#llx (%llu us)\n", (unsigned long long)base, (unsigned long long)(t1 - t0));
    uint64_t probes[3] = {base, base + GW_HEAP_OFF, base + GW_SIZE - (uint64_t)page};
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        volatile uint64_t *q = (volatile uint64_t *)(uintptr_t)probes[i];
        *q = probes[i] ^ 0x5a5a5a5a5a5a5a5aull;
        ok &= *q == (probes[i] ^ 0x5a5a5a5a5a5a5a5aull);
    }
    sb_printf(b, "  escrita/leitura (inicio, heap, fim): %s\n", ok ? "OK" : "FALHOU");
    munmap((void *)(uintptr_t)base, GW_SIZE);
}

JNIEXPORT jstring JNICALL Java_com_zbotw_recomp_probe_MainActivity_memoryProbe(JNIEnv *env, jobject self) {
    (void)self;
    Sb b = {0};
    memory_probe(&b);
    jstring r = (*env)->NewStringUTF(env, sb_str(&b));
    free(b.s);
    return r;
}

// driver_dir == NULL: the system driver. Otherwise the custom driver driver_dir/driver_name (driver_dir ends in '/',
// in the app's internal storage), loaded through adrenotools with the hooks from native_lib_dir.
JNIEXPORT jstring JNICALL Java_com_zbotw_recomp_probe_MainActivity_vulkanProbe(JNIEnv *env, jobject self, jstring jlibdir,
                                                                            jstring jdir, jstring jname) {
    (void)self;
    Sb b = {0};
    const char *libdir = (*env)->GetStringUTFChars(env, jlibdir, NULL);
    const char *dir = jdir ? (*env)->GetStringUTFChars(env, jdir, NULL) : NULL;
    const char *name = jname ? (*env)->GetStringUTFChars(env, jname, NULL) : NULL;
    void *lib;
    if (dir) {
        sb_printf(&b, "== Vulkan: driver customizado %s%s (adrenotools)\n", dir, name);
        lib = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, NULL, libdir, dir, name, NULL, NULL);
        if (!lib) sb_printf(&b, "ERRO: adrenotools_open_libvulkan falhou (veja o logcat, tag AdrenoTools)\n");
    } else {
        sb_printf(&b, "== Vulkan: driver do sistema\n");
        lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) sb_printf(&b, "ERRO: dlopen libvulkan.so: %s\n", dlerror());
    }
    if (lib) vk_probe(&b, lib);  // (not dlclosed: drivers rarely survive being unloaded)
    (*env)->ReleaseStringUTFChars(env, jlibdir, libdir);
    if (dir) (*env)->ReleaseStringUTFChars(env, jdir, dir);
    if (name) (*env)->ReleaseStringUTFChars(env, jname, name);
    jstring r = (*env)->NewStringUTF(env, sb_str(&b));
    free(b.s);
    return r;
}
