// Start screen, shown before the game boots (from LAS Recomp's, styled after Dusklight's launcher): PLAY, SETTINGS,
// QUIT, game-files status at the lower left. Nothing of the game ships with BOTWRecompAndroid: on first launch the user picks
// the folder with their own game files (v1.0.0, 1.6.0 or 1.8.2), we tell the version by the main module's build id,
// copy the files into that version's folder (runtime/paths.h) and build the game's data image; PLAY stays disabled
// until that worked. A version set up beside the one running becomes the one played: the app starts again in its
// folder (Game Files page / Settings > Prelaunch: the versions set up, to switch between). The background uses the
// bundled artwork, with a frame from the user's game files as a fallback for packages without the artwork.
//
// Skipped with --play, LARECOMP_NO_LAUNCHER=1, or any scripted test run (LARECOMP_INPUT): tests boot straight in.
// The in-game menu's "Title screen" comes back here without leaving the process (runtime/main.c reboots the game).
#ifdef __APPLE__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <atomic>

#include "i18n.h"
#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner)
#include "option_rows.h"
#include "osk.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
#endif
#include "imgui_impl_sdl3.h"

extern "C" {
#ifndef LARECOMP_NO_FFMPEG  // (Android, for now: docs/las/ANDROID-PORT.md, Fase 2)
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#endif
#include "platform.h"
#include "paths.h"
#include "gamebuild.h"
#include "gamelib.h"
#include "romfs_build.h"
#include "settings.h"
#include "input_bind.h"
int game_version_of_build(const uint8_t id[8]);  // runtime/loader.c: 100, 160, 182; 0 another one
void controls_bindings_draw(float scale);  // ui/controls_ui.mm
void ui_window_init(void);
SDL_Window *ui_window(void);
#ifdef __APPLE__
void *ui_metal_layer(void);
void *ui_metal_device(void);
#else
// runtime/nvn/vulkan: window frames and ImGui plumbing
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
#endif
}
#ifdef __APPLE__
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// (test) BGRA texture -> PNG (the game renderer's saver isn't up yet before the game boots)
static void save_png(id<MTLTexture> tex, const char *path) {
    size_t w = tex.width, h = tex.height;
    std::vector<uint8_t> px(w * h * 4);
    [tex getBytes:px.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) {  // (NULL when the file can't be created, e.g. its folder isn't there: CFRelease(NULL) traps)
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    } else fprintf(stderr, "[launcher] can't write %s\n", path);
    CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
}
#endif

// The release tag this build is (also names the release files; Makefile / tools/bundle_macos.sh / make_dmg.sh read
// it): beta_<n> until 1.0, then v<major>.<minor>.<patch>.
#define LARECOMP_VERSION "alpha_0"

// ------------------------------------------------------------------ update check
// The start screen asks GitHub for the project's releases once per launch (a background curl, which macOS and
// Windows 10+ ship) and offers the best one if it outranks this build: any v1.0.0+ above every beta, betas by
// number; other tags (the alpha_* ones) are ignored. Offline / no curl / anything unexpected: nothing is shown.
#define RELEASES_URL "https://api.github.com/repos/mamcjr/BOTWRecompAndroid/releases?per_page=100"
#define RELEASE_PAGE "https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/"

static long long tag_rank(const std::string &t) {
    int n, a, b, c;
    char end;
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1 && n > 0) return n;
    if (sscanf(t.c_str(), "v%d.%d.%d%c", &a, &b, &c, &end) == 3 && a >= 1 && b >= 0 && c >= 0 && b < 4096 && c < 4096)
        return (1LL << 48) + ((long long)a << 24) + ((long long)b << 12) + c;
    return -1;
}

// "Beta 2" / "1.0.0" (with_word: "Version 1.0.0")
static std::string tag_label(const std::string &t, bool with_word) {
    int n;
    char end;
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1) return "Beta " + std::to_string(n);
    std::string v = t.size() > 1 && t[0] == 'v' ? t.substr(1) : t;
    return with_word ? "Version " + v : v;
}

static std::string g_update_tag;  // a newer release's tag (g_update_lock)
static pthread_mutex_t g_update_lock = PTHREAD_MUTEX_INITIALIZER;

static void *update_thread(void *) {
    std::string body;
    if (const char *t = getenv("LARECOMP_UPDATE_TAGS")) {  // (test) "beta_2,v1.0.0": these releases, no network
        for (const char *p = t; *p;) {
            const char *c = strchr(p, ',');
            body += "\"tag_name\": \"" + std::string(p, c ? c - p : strlen(p)) + "\"\n";
            p = c ? c + 1 : p + strlen(p);
        }
    } else {
#ifdef _WIN32
        const char *sr = getenv("SystemRoot");
        std::string curl = std::string(sr ? sr : "C:\\Windows") + "\\System32\\curl.exe";
#else
        std::string curl = "/usr/bin/curl";
#endif
        std::string out = std::string(rt_data_root()) + "/cache/releases.json";
        mkdir((std::string(rt_data_root()) + "/cache").c_str(), 0755);
        const char *av[] = {curl.c_str(), "-fsSL", "--max-time", "15", "-H", "Accept: application/vnd.github+json",
                            "-A", "BOTWRecompAndroid/" LARECOMP_VERSION, RELEASES_URL, NULL};
        PlatProc p = plat_spawn(av, out.c_str(), false);
        if (!p || plat_proc_wait(p, true) != 0) { remove(out.c_str()); return NULL; }
        if (FILE *f = fopen(out.c_str(), "rb")) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0 && body.size() < (8u << 20)) body.append(buf, n);
            fclose(f);
        }
        remove(out.c_str());
    }
    std::string best;
    long long best_rank = tag_rank(LARECOMP_VERSION);
    for (size_t at = 0; (at = body.find("\"tag_name\"", at)) != std::string::npos;) {
        size_t q1 = body.find('"', body.find(':', at) + 1), q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string tag = body.substr(q1 + 1, q2 - q1 - 1);
        if (tag_rank(tag) > best_rank) best_rank = tag_rank(tag), best = tag;
        at = q2;
    }
    if (!best.empty()) fprintf(stderr, "[launcher] update available: %s (this build %s)\n", best.c_str(), LARECOMP_VERSION);
    pthread_mutex_lock(&g_update_lock);
    g_update_tag = best;
    pthread_mutex_unlock(&g_update_lock);
    return NULL;
}

// ------------------------------------------------------------------ game files
static std::string g_root;  // extracted game: <root>/exefs, <root>/romfs

static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// The game version of a main module (game_version_of_build: 100, 160, 182; 0 another one), -1 when it isn't there
static int main_version(const std::string &main_path) {
    FILE *f = fopen(main_path.c_str(), "rb");
    if (!f) return -1;
    uint8_t id[8] = {0};
    fseek(f, 0x40, SEEK_SET);
    size_t n = fread(id, 1, 8, f);
    fclose(f);
    return n == 8 ? game_version_of_build(id) : 0;
}

static const char *version_name(int v) { return v == 190 ? "1.9.0" : v == 182 ? "1.8.2" : v == 160 ? "1.6.0" : "1.0.0"; }

// The player's romfs/ the files of root use in place (root/romfs.link: main.c reads it too), "" when they're copied
static std::string romfs_link_target(const std::string &root) {
    FILE *f = fopen((root + "/romfs.link").c_str(), "rb");
    if (!f) return "";
    char t[1024];
    size_t n = fread(t, 1, sizeof t - 1, f);
    fclose(f);
    while (n && (t[n - 1] == '\n' || t[n - 1] == '\r')) n--;
    return std::string(t, n);
}

// The copied game files alone ("" = there), without the game code built from them: of a version this app runs, the
// one whose folder root is (runtime/paths.h)
static std::string check_copied_files(const std::string &root) {
    int v = main_version(root + "/exefs/main"), want = rt_game_dir_version(root.c_str());
    if (v < 0) return "No game files";
    if (!v || (want && v != want)) return "Wrong game version (needs v1.0.0, v1.6.0, v1.8.2 or v1.9.0)";
    // (the game reads its data through romfs.bin alone: on Android it's built straight from the picked folder, and romfs/
    // holds only the movies, opened by path)
    std::string linked = romfs_link_target(root);
    struct stat lst;
    if (!linked.empty() && stat(linked.c_str(), &lst) == 0 && S_ISREG(lst.st_mode)) {  // (in place: the player's romfs.bin)
        char bad[128];
        return romfs_image_ok(linked.c_str(), bad, sizeof bad) ? "" : "The game files folder was moved or can't be read";
    }
    if (!linked.empty())  // (used in place: the player's own romfs/, runtime/services/fs.c)
        return file_exists(linked + "/System/Version.txt") ? "" : "The game files folder was moved or can't be read";
    if (!file_exists(root + "/romfs.bin")) return "Game data missing";
    char why[128];  // (a copy cut short: the game would abort at start, runtime/romfs_build.c)
    if (!romfs_image_ok((root + "/romfs.bin").c_str(), why, sizeof why)) return "Game data incomplete: set up the game files again";
    return "";
}

extern "C" bool native_wanted(void);  // (Native game code: nothing to build - runtime/native.c)

// "" = ready to play, else what is wrong
static std::string check_game_files(const std::string &root) {
    std::string why = check_copied_files(root);
    if (why.empty() && !native_wanted() && !gamelib_present(root.c_str())) return "The game code needs building";
    return why;
}

// The game versions this app runs, and which of them are set up (their copied files there; checked again once a second)
static const int k_versions[] = {100, 160, 182, 190};
enum { N_VERSIONS = 4 };
static bool version_set_up(int i) {
    static bool set_up[N_VERSIONS];
    static double next;
    double now = ImGui::GetTime();
    if (now >= next) {
        next = now + 1.0;
        for (int k = 0; k < N_VERSIONS; k++) {
            char r[1024];
            set_up[k] = rt_version_root(k_versions[k], r, sizeof r) && check_copied_files(std::string(r) + "/extracted").empty();
        }
    }
    return set_up[i];
}

// ------------------------------------------------------------------ setup (background thread)
// The player brings their own game files: a folder holding exefs/ and romfs/. They're copied into the data root (APFS
// clones / hard links where the file system allows) and the data image the game reads is built from romfs/
// (runtime/romfs_build.c).
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_src;           // the picked folder
static std::string g_dest;          // the setup's: the picked files' version's extracted/ ("": this run's, g_root)
static int g_set_up_version;        // a setup that worked: the version it set up (another than this run's: a restart into it)
static std::string g_job;           // current setup step / result for the status line
static std::string g_job_hint;      // a failure: what to do about it (shown under it)
static std::string g_job_detail;    // a failure: its cause as the code saw it, untranslated ("can't read Pack/...")
static std::string g_fail_detail;   // (that cause, from the import thread, until its failure is set)
static bool g_busy, g_job_failed;
static bool g_confirm_delete;       // game-files page: "Delete the game files?" shown

static void set_job(const std::string &s, bool failed = false, const std::string &hint = "") {
    pthread_mutex_lock(&g_lock);
    g_job = s;
    g_job_failed = failed;
    g_job_hint = hint;
    g_job_detail.clear();
    pthread_mutex_unlock(&g_lock);
}

// A failure's cause on the screen too: before, "Setup didn't finish" alone, the cause only in Android's log
// (RedMagic 11 Air, 2026-10-24: a player can't read that)
static void set_job_detail(const std::string &d) {
    if (d.empty()) return;
    fprintf(stderr, "[launcher] setup failed: %s\n", d.c_str());
    pthread_mutex_lock(&g_lock);
    g_job_detail = d;
    pthread_mutex_unlock(&g_lock);
}

// Setup failures: a short headline and what to do next (also LARECOMP_LAUNCHER_SHOT=<f>:fail<1-6>, screenshots)
enum { FAIL_NOT_FOUND = 1, FAIL_UNUSABLE, FAIL_COPY, FAIL_VERSION, FAIL_MISSING, FAIL_BUILD };
static void set_failure(int k) {
    static const char *const head[] = {"", "No game files found in that folder", "Those files can't be used",
                                       "Setup didn't finish", "Wrong version of the game", "Game files incomplete",
                                       "The game code didn't build"};
    static const char *const hint[] = {
        "",
        "Choose the folder that holds the game files.",
        "Choose the folder that holds the game files.",
        "Make sure there's about 30 GB free and try again.",
        "BOTWRecompAndroid runs versions 1.0.0, 1.6.0, 1.8.2 and 1.9.0 of the game.",
        "Some files are missing. Choose the folder again; if it happens again, dump the game again.",
        "Try again: the build goes on where it stopped. If it keeps failing, report it on GitHub with the log."};
    if (k >= 1 && k <= 6) set_job(head[k], true, hint[k]);
}

// The romfs/ found against the exefs/ beside it (romfs_check_set): false = it doesn't go with it, the failure set (the
// versions only in its detail: the hints are translated whole)
static bool romfs_check_result(int r, const char *why, int nfiles, const char *version_txt) {
    if (r == ROMFS_SET_OTHER_VERSION) {
        set_job("Wrong version of the game", true,
                "The game data (romfs) and the game code (exefs) are from different versions. Choose a folder where both "
                "come from the same version.");
    } else if (r == ROMFS_SET_MISSING) {
        set_failure(FAIL_MISSING);
    } else if (r == ROMFS_SET_DAMAGED) {  // (a romfs.bin brought ready-made, romfs_check_image)
        set_job("Game data incomplete: copy romfs.bin again whole", true,
                "The romfs.bin in that folder can't be read whole. Copy it again from where it was made, or make it again.");
    } else {
        if (nfiles >= 0) fprintf(stderr, "[launcher] romfs: %d files, Version.txt %.16s\n", nfiles, version_txt[0] ? version_txt : "-");
        return true;
    }
    set_job_detail(why);
    return false;
}

static bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// exefs: the folder with main and main.npdm; romfs: the folder with Pack (the game's data root). Found
// in the picked folder or a few levels below it (dump tools nest them).
static void find_game(const std::string &dir, int depth, std::string &exefs, std::string &romfs, bool &unusable) {
    if (depth > 4 || (!exefs.empty() && !romfs.empty())) return;
    if (exefs.empty() && file_exists(dir + "/main") && file_exists(dir + "/main.npdm")) exefs = dir;
    if (romfs.empty() && is_dir(dir + "/Pack")) romfs = dir;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> sub;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n[0] == '.') continue;
        std::string ext = n.size() > 4 ? n.substr(n.size() - 4) : "";
        for (auto &ch : ext) ch = (char)tolower((unsigned char)ch);
        if (ext == ".xci" || ext == ".nsp" || ext == ".nca" || ext == ".nsz" || ext == ".xcz") unusable = true;
        if (n != "Pack" && is_dir(dir + "/" + n)) sub.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(sub.begin(), sub.end());
    for (auto &s : sub) find_game(s, depth + 1, exefs, romfs, unusable);
}

// progress (the overlay's bar): the romfs copy, then the image build - each about the size of romfs/ (Android: exefs/
// copied, then the image read straight from the picked romfs/ - one pass)
static std::atomic<unsigned long long> g_done, g_total;
// The bar: copying and the data image are the first tenth, building the game code (runtime/gamebuild.c) the rest
static unsigned long long g_phase_base;
static std::atomic<float> g_build_frac{-1};  // -1: not building
static bool g_copy_skipped;                  // (only the game code to build: the bar is all of that)
static void setup_progress(uint64_t done, uint64_t) { g_done = g_phase_base + done; }
#ifdef __ANDROID__
static void android_call(const char *method);
static void android_call_int(const char *method, int v);
#endif
static void build_progress(double f, const char *step) {
    g_build_frac = (float)f;
#ifdef __ANDROID__
    static int pct = -1;  // (the foreground service's notification, a whole percent at a time)
    int p = (int)(f * 100);
    if (p != pct) pct = p, android_call_int("buildServiceProgress", p);
#endif
    static const char *last;
    if (step != last) last = step, set_job(step);
}
static float extract_progress(void) {
    unsigned long long t = g_total, d = g_done;
    float b = g_build_frac, f = b >= 0 ? (g_copy_skipped ? b : 0.1f + 0.9f * b) : t ? 0.1f * (float)((double)d / (double)t) : 0;
    return f < 0 ? 0 : f > 0.99f ? 0.99f : f;
}

// Builds the game code from the copied files in dir (a few minutes); false = failed (the job line says so)
static bool build_game_code(const std::string &dir) {
    if (native_wanted()) return true;  // (the game's own code runs as it is)
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    GameBuildOpts o = {build_progress, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1};
    char why[256] = "";
    g_build_frac = 0;
#ifdef __ANDROID__
    android_call("buildServiceStart");  // (BuildService: on with the screen off or the app in the background)
#endif
    bool ok = gamebuild_run(dir.c_str(), &o, why, sizeof why);
#ifdef __ANDROID__
    android_call("buildServiceStop");
#endif
    if (!ok) {
        fprintf(stderr, "[launcher] game code: %s\n", why);
        std::string detail = why;
        struct statvfs fs;  // (free space named only when it is short)
        if (statvfs(dir.c_str(), &fs) == 0) {
            double gb = (double)fs.f_bavail * (double)fs.f_frsize / (1024.0 * 1024.0 * 1024.0);
            if (gb < 2.0) {
                char sp[96];
                snprintf(sp, sizeof sp, "; only %.1f GB free on the phone", gb);
                detail += sp;
            }
        }
        set_failure(FAIL_BUILD);
        set_job_detail(detail);
    }
    return ok;
}

// The setup's folder: the picked files' version's extracted/ (g_dest), else this run's
static std::string setup_dest(void) {
    pthread_mutex_lock(&g_lock);
    std::string d = g_dest.empty() ? g_root : g_dest;
    pthread_mutex_unlock(&g_lock);
    return d;
}

static void *extract_thread(void *) {
    std::string src, dest = setup_dest();
    pthread_mutex_lock(&g_lock);
    src = g_src;
    pthread_mutex_unlock(&g_lock);
    g_done = 0, g_total = 0, g_phase_base = 0, g_build_frac = -1, g_copy_skipped = false;
    std::string exefs, romfs;
    bool unusable = false;  // (game packages rather than its files)
    bool ok = false;
#ifdef __ANDROID__
    extern std::atomic<bool> g_android_imported;
    if (g_android_imported.exchange(false)) {  // (the activity copied exefs/ and built romfs.bin: below) the code -
        // unless the picked folder brought it built already for this app (its id: runtime/gamelib.c)
        if (gamelib_present(dest.c_str())) fprintf(stderr, "[launcher] game code from the picked folder\n");
        else if (!build_game_code(dest)) goto done;
        goto check;
    }
#endif
    if (src.empty() && check_copied_files(dest).empty()) {  // copied already (an update needs new game code)
        g_copy_skipped = true;
        if (!build_game_code(dest)) goto done;
        goto check;
    }
    set_job("Looking for the game files...");
    find_game(src, 0, exefs, romfs, unusable);
    if (exefs.empty() || romfs.empty()) { set_failure(unusable ? FAIL_UNUSABLE : FAIL_NOT_FOUND); goto done; }
    {   // into that version's own folder (runtime/paths.h)
        int v = main_version(exefs + "/main");
        char vr[1024];
        if (v <= 0 || !rt_version_root(v, vr, sizeof vr)) { set_failure(FAIL_VERSION); goto done; }
        char why[256] = "";
        int r = romfs_check_dir(v, romfs.c_str(), why, sizeof why);
        if (!romfs_check_result(r, why, -1, "")) goto done;  // (before an earlier setup is removed)
        mkdir(vr, 0755);
        dest = std::string(vr) + "/extracted";
    }
    {
        // (the picked folder can't be BOTWRecompAndroid's own copy: setting up again deletes that first)
        char a[4096], b[4096];
        mkdir(dest.c_str(), 0755);
        if (realpath(exefs.c_str(), a) && realpath(dest.c_str(), b) && !strncmp(a, b, strlen(b)) &&
            (a[strlen(b)] == '/' || a[strlen(b)] == '\\' || !a[strlen(b)])) {
            set_failure(FAIL_NOT_FOUND);
            goto done;
        }
        rt_remove_game_files_for_setup(dest.c_str());  // (an earlier or interrupted setup's; its compiled objects stay)
        mkdir(dest.c_str(), 0755);
        unsigned long long rbytes = romfs_dir_bytes(romfs.c_str());
        g_total = 2 * rbytes;
        set_job("Copying the game files...");
        if (!romfs_copy_tree(exefs.c_str(), (dest + "/exefs").c_str(), NULL) ||
            !romfs_copy_tree(romfs.c_str(), (dest + "/romfs").c_str(), setup_progress)) {
            set_failure(FAIL_COPY);
            goto done;
        }
        g_phase_base = rbytes, g_done = rbytes;
        set_job("Building the game data image...");
        char why[256] = "";
        if (!romfs_build((dest + "/romfs").c_str(), (dest + "/romfs.bin").c_str(), setup_progress, why, sizeof why)) {
            fprintf(stderr, "[launcher] romfs image: %s\n", why);
            set_failure(FAIL_COPY);
            set_job_detail(why);
            goto done;
        }
        if (!build_game_code(dest)) goto done;
    }
check:
    {
        std::string why = check_game_files(dest);
        ok = why.empty();
        if (ok) set_job("Game files ready.");
        else set_failure(why.find("version") != std::string::npos ? FAIL_VERSION : FAIL_MISSING);
    }
done:
    pthread_mutex_lock(&g_lock);
    g_busy = false;
    g_set_up_version = ok ? rt_game_dir_version(dest.c_str()) : 0;
    g_dest.clear();
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

#ifdef __ANDROID__
// ------------------------------------------------------------------ Android: the system's pickers (SAF)
// Android has no POSIX path to a folder the player picks: BOTWRecompAndroidActivity opens the system's folder picker, finds
// exefs/ and romfs/ in the picked folder (a few levels down too, as find_game), tells the game version with
// nativeGameVersion, copies exefs/ into that version's folder (nativeImportBegin: g_dest) and builds romfs.bin straight
// from the picked romfs/ (nativeBuildRomfs:
// no copy of its ~15 GB first; only the movies, opened by path, stay loose - LASRecompAndroid's), then the setup goes
// on as on the desktop with the game code (runtime/gamebuild.c, on the phone itself). The Vulkan driver is
// picked the same way (a Turnip .zip, copied to the drivers folder the activity loads from at start).
#include <jni.h>
std::atomic<bool> g_android_imported{false};
static std::string g_driver_msg;
static bool g_driver_restart;  // (the message asks for a restart: a Restart now button beside it)

static void android_call(const char *method) {  // a static method of the activity
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return;
    jclass c = env->GetObjectClass(act);
    jmethodID m = env->GetStaticMethodID(c, method, "()V");
    if (m) env->CallStaticVoidMethod(c, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(c);
    env->DeleteLocalRef(act);
}

static void android_call_int(const char *method, int v) {  // (one int argument)
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return;
    jclass c = env->GetObjectClass(act);
    jmethodID m = env->GetStaticMethodID(c, method, "(I)V");
    if (m) env->CallStaticVoidMethod(c, m, (jint)v);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(c);
    env->DeleteLocalRef(act);
}

// A content:// URI the SDL dialog picked (Import fonts): its name, and a copy of it at an absolute path
// (BOTWRecompAndroidActivity documentName / copyDocument)
static std::string android_document_name(const std::string &uri) {
    std::string v;
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return v;
    jclass c = env->GetObjectClass(act);
    jmethodID m = env->GetStaticMethodID(c, "documentName", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring u = env->NewStringUTF(uri.c_str());
    jstring s = m ? (jstring)env->CallStaticObjectMethod(c, m, u) : NULL;
    if (env->ExceptionCheck()) { env->ExceptionClear(); s = NULL; }
    if (s) {
        const char *t = env->GetStringUTFChars(s, NULL);
        if (t) v = t, env->ReleaseStringUTFChars(s, t);
        env->DeleteLocalRef(s);
    }
    env->DeleteLocalRef(u);
    env->DeleteLocalRef(c);
    env->DeleteLocalRef(act);
    return v;
}

static bool android_copy_document(const std::string &uri, const std::string &dst) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return false;
    jclass c = env->GetObjectClass(act);
    jmethodID m = env->GetStaticMethodID(c, "copyDocument", "(Ljava/lang/String;Ljava/lang/String;)Z");
    jstring u = env->NewStringUTF(uri.c_str()), d = env->NewStringUTF(dst.c_str());
    bool ok = m && env->CallStaticBooleanMethod(c, m, u, d);
    if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; }
    env->DeleteLocalRef(u);
    env->DeleteLocalRef(d);
    env->DeleteLocalRef(c);
    env->DeleteLocalRef(act);
    return ok;
}

// The log file (Driver compatibility; runtime/platform/android.c): written to a folder the player picks
// (BOTWRecompAndroidActivity pickLogFolder / openLog), where a file manager can open it - Android/data/ is out of its reach
extern "C" void android_log_file(bool on);
extern "C" bool vk_adreno_8xx(const char *name);  // (Driver compatibility's Automatic: runtime/nvn/vulkan/device.c)
extern "C" bool vk_turnip_heap_on(int setting);
extern "C" bool vk_lrz_at_start(void);
extern "C" const char *vk_gpu_name(void);
extern "C" const char *vk_driver_rejected(void);  // (runtime/nvn/vulkan/android_driver.c)
extern "C" const char *vk_driver_desc(void);      // (its own name and version: runtime/nvn/vulkan/device.c)
extern "C" bool vk_driver_is_turnip(void);
extern "C" const char *vk_gpu_desc(void);  // ("Adreno (TM) 830 - Turnip")
static std::string g_log_folder;  // its name ("Download/BOTWRecomp"), "" with none
static bool g_log_folder_read;

static std::string app_version() {  // BOTWRecompAndroidActivity.appVersion, asked once ("0.2.0-beta")
    static std::string v;
    static bool read;
    if (read) return v;
    read = true;
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return v;
    jclass c = env->GetObjectClass(act);
    jmethodID m = env->GetStaticMethodID(c, "appVersion", "()Ljava/lang/String;");
    jstring s = m ? (jstring)env->CallStaticObjectMethod(c, m) : NULL;
    if (env->ExceptionCheck()) { env->ExceptionClear(); s = NULL; }
    if (s) {
        const char *t = env->GetStringUTFChars(s, NULL);
        if (t) v = t, env->ReleaseStringUTFChars(s, t);
        env->DeleteLocalRef(s);
    }
    env->DeleteLocalRef(c);
    env->DeleteLocalRef(act);
    return v;
}

static void log_folder_read() {  // (BOTWRecompAndroidActivity.logFolderName, once: then from each pick)
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return;
    jclass c = env->GetObjectClass(act);
    jmethodID m = env->GetStaticMethodID(c, "logFolderName", "()Ljava/lang/String;");
    jstring s = m ? (jstring)env->CallStaticObjectMethod(c, m) : NULL;
    if (env->ExceptionCheck()) { env->ExceptionClear(); s = NULL; }
    if (s) {
        const char *t = env->GetStringUTFChars(s, NULL);
        pthread_mutex_lock(&g_lock);
        g_log_folder = t ? t : "";
        pthread_mutex_unlock(&g_lock);
        if (t) env->ReleaseStringUTFChars(s, t);
        env->DeleteLocalRef(s);
    }
    env->DeleteLocalRef(c);
    env->DeleteLocalRef(act);
}

extern "C" JNIEXPORT void JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeLogFolderPicked(JNIEnv *env, jclass, jstring name) {
    const char *t = env->GetStringUTFChars(name, NULL);
    pthread_mutex_lock(&g_lock);
    if (t && *t) g_log_folder = t;  // ("": cancelled - the folder before stays)
    pthread_mutex_unlock(&g_lock);
    if (t) env->ReleaseStringUTFChars(name, t);
    if (g_settings.log_file) android_log_file(true);  // (the file moves to the folder, with this run's lines so far)
}

// Saves (Settings > Saves; BOTWRecompAndroidActivity importSave / exportSave): this version's save_0, and the result
static std::string g_save_msg;  // (g_lock) the last import's or export's, shown under the buttons
static bool g_save_failed, g_save_busy;

extern "C" JNIEXPORT jstring JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeSaveDir(JNIEnv *env, jclass) {
    char r[1024];
    std::string d = rt_version_root(rt_game_version(), r, sizeof r) ? std::string(r) + "/save/save_0" : "";
    return env->NewStringUTF(d.c_str());
}

extern "C" JNIEXPORT void JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeSaveDone(JNIEnv *env, jclass,
                                                                                                    jint result, jstring detail) {
    const char *t = env->GetStringUTFChars(detail, NULL);
    std::string d = t ? t : "";
    if (t) env->ReleaseStringUTFChars(detail, t);
    char msg[600];
    switch (result) {
    case 0:
        if (d.empty()) snprintf(msg, sizeof msg, "%s", tr("Saves imported."));
        else snprintf(msg, sizeof msg, tr("Saves imported. The ones before are kept beside them as %s."), d.c_str());
        break;
    case 1: snprintf(msg, sizeof msg, tr("Saves exported to %s."), d.c_str()); break;
    case 2: msg[0] = 0; break;
    case 3: snprintf(msg, sizeof msg, "%s", tr("No BOTW save there: look for the one that holds 0, 1, 2... and option.sav.")); break;
    default: snprintf(msg, sizeof msg, tr("That didn't work: %s"), d.c_str()); break;
    }
    pthread_mutex_lock(&g_lock);
    g_save_msg = msg, g_save_failed = result >= 3, g_save_busy = false;
    pthread_mutex_unlock(&g_lock);
}

// The game version of the picked main module's first bytes (game_version_of_build: 0 = not one this app runs)
extern "C" JNIEXPORT jint JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeGameVersion(JNIEnv *env, jclass, jbyteArray head) {
    if (env->GetArrayLength(head) < 0x48) return 0;
    jbyte b[8];
    env->GetByteArrayRegion(head, 0x40, 8, b);
    return game_version_of_build((const uint8_t *)b);
}

// The picked folder is searched (the activity's import thread, through the file provider: it can take a while) - before,
// nothing on the screen moved until the copy began (issue #17). nativeImportBegin or nativeImportDone follows.
extern "C" JNIEXPORT void JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeImportLooking(JNIEnv *, jclass) {
    pthread_mutex_lock(&g_lock);
    g_busy = true;
    pthread_mutex_unlock(&g_lock);
    set_job("Looking for the game files...");
}

// The import of that version's files begins: its own extracted/ (runtime/paths.h), emptied of an earlier setup - its
// path back, for the activity to copy exefs/ into
extern "C" JNIEXPORT jstring JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeImportBegin(JNIEnv *env, jclass, jstring name,
                                                                                                         jint version) {
    const char *n = env->GetStringUTFChars(name, NULL);
    char vr[1024] = "";
    bool own = rt_version_root(version, vr, sizeof vr);
    std::string dest = own ? std::string(vr) + "/extracted" : g_root;
    pthread_mutex_lock(&g_lock);
    g_busy = true;
    g_src = n ? n : "";
    g_dest = dest;
    pthread_mutex_unlock(&g_lock);
    if (n) env->ReleaseStringUTFChars(name, n);
    rt_remove_game_files_for_setup(dest.c_str());  // (an earlier or interrupted setup's; its compiled objects stay)
    if (own) mkdir(vr, 0755);
    mkdir(dest.c_str(), 0755);
    g_done = 0, g_total = 0, g_phase_base = 0, g_build_frac = -1, g_copy_skipped = false;
    set_job("Copying the game files...");
    return env->NewStringUTF(dest.c_str());
}

// The picked romfs/'s files (relative paths) and its System/Version.txt's first bytes against the picked exefs/'s
// version, before nativeImportBegin: a folder that can't be used leaves an earlier setup alone. false = the failure is
// set (nativeImportDone(6) follows)
extern "C" JNIEXPORT jboolean JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeCheckRomfs(JNIEnv *env, jclass, jint version,
                                                                                                         jobjectArray paths, jstring version_txt) {
    int n = env->GetArrayLength(paths);
    std::vector<std::string> keep(n);
    std::vector<const char *> p(n);
    for (int i = 0; i < n; i++) {
        jstring s = (jstring)env->GetObjectArrayElement(paths, i);
        const char *c = env->GetStringUTFChars(s, NULL);
        keep[i] = c ? c : "";
        if (c) env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
        p[i] = keep[i].c_str();
    }
    const char *t = version_txt ? env->GetStringUTFChars(version_txt, NULL) : NULL;
    std::string txt = t ? t : "";
    if (t) env->ReleaseStringUTFChars(version_txt, t);
    char why[256] = "";
    int r = romfs_check_set(version, p.data(), n, txt.c_str(), why, sizeof why);
    while (!txt.empty() && (txt.back() == '\n' || txt.back() == '\r' || txt.back() == ' ')) txt.pop_back();
    return romfs_check_result(r, why, n, txt.c_str());
}

static bool keep_loose(const char *rel);

// A romfs.bin brought ready-made (the picked folder: exefs/ and romfs.bin - made on a PC from the game's own files, no
// 41,000 files to copy through the file provider): nativeCheckRomfs for it, its descriptor (closed here) read from its
// own tables and System/Version.txt. false = the failure is set (nativeImportDone(6) follows)
extern "C" JNIEXPORT jboolean JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeCheckRomfsImage(JNIEnv *, jclass,
                                                                                                              jint version, jint fd) {
    char why[256] = "";
    int r = romfs_check_image(version, fd, why, sizeof why);
    close(fd);
    if (r != ROMFS_SET_OK) fprintf(stderr, "[launcher] romfs.bin: %s\n", why);
    return romfs_check_result(r, why, -1, "");
}

// The few files the runtime opens by path (keep_loose: the movies), out of a ready-made romfs.bin (its descriptor,
// closed here) into this setup's romfs/. NULL, or why it failed.
extern "C" JNIEXPORT jstring JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeImageLoose(JNIEnv *env, jclass, jint fd) {
    set_job("Taking the movies out of the game data...");
    std::string romfs = setup_dest() + "/romfs";
    mkdir(romfs.c_str(), 0755);
    char why[256] = "";
    bool ok = romfs_image_extract(fd, romfs.c_str(), keep_loose, why, sizeof why);
    close(fd);
    if (ok) return NULL;
    fprintf(stderr, "[launcher] romfs.bin's movies: %s\n", why);
    pthread_mutex_lock(&g_lock);
    g_fail_detail = why;  // (nativeImportDone(5) follows)
    pthread_mutex_unlock(&g_lock);
    return env->NewStringUTF(why);
}

extern "C" JNIEXPORT void JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeImportProgress(JNIEnv *, jclass, jlong done, jlong total) {
    g_total = (unsigned long long)total;  // (exefs/ copied, then the data image read straight from romfs/: one pass)
    g_done = (unsigned long long)done;
}

// The few romfs/ files the runtime opens by path rather than through romfs.bin: the movies (hle/movie.c). Only these
// stay loose.
static bool keep_loose(const char *rel) {
    size_t n = strlen(rel);
    return !strncmp(rel, "Movie/", 6) || (n > 5 && !strcmp(rel + n - 5, ".webm"));
}

// Called by the activity's import thread with romfs/'s files (relative paths, sizes): romfs.bin straight from the
// picked folder - each file's descriptor from openRomfsFile(index) - and the loose few beside it. Returns NULL, or why
// it failed.
struct RomfsJni { JNIEnv *env; jobject act; jmethodID open; };
static int romfs_jni_open(void *ctx, int index) {
    RomfsJni *j = (RomfsJni *)ctx;
    int fd = j->env->CallIntMethod(j->act, j->open, (jint)index);
    if (j->env->ExceptionCheck()) { j->env->ExceptionClear(); return -1; }
    return fd;
}
extern "C" JNIEXPORT jstring JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeBuildRomfs(
        JNIEnv *env, jobject act, jobjectArray paths, jlongArray sizes) {
    int n = env->GetArrayLength(paths);
    std::vector<std::string> keep_alive(n);
    std::vector<RomfsEntry> entries(n);
    std::vector<jlong> sz(n);
    if (n) env->GetLongArrayRegion(sizes, 0, n, sz.data());
    for (int i = 0; i < n; i++) {
        jstring s = (jstring)env->GetObjectArrayElement(paths, i);
        const char *c = env->GetStringUTFChars(s, NULL);
        keep_alive[i] = c ? c : "";
        if (c) env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
        entries[i] = {keep_alive[i].c_str(), (uint64_t)sz[i]};
    }
    jclass cls = env->GetObjectClass(act);
    RomfsJni j = {env, act, env->GetMethodID(cls, "openRomfsFile", "(I)I")};
    env->DeleteLocalRef(cls);
    if (!j.open) { env->ExceptionClear(); return env->NewStringUTF("Internal error: openRomfsFile missing."); }
    set_job("Building the game data image...");
    g_phase_base = g_done;  // (exefs/ copied: the image's bytes follow)
    char why[256] = "";
    std::string dest = setup_dest(), romfs = dest + "/romfs";
    mkdir(romfs.c_str(), 0755);
    bool ok = romfs_build_entries(entries.data(), n, romfs_jni_open, &j, (dest + "/romfs.bin").c_str(),
                                  romfs.c_str(), keep_loose, setup_progress, why, sizeof why);
    if (ok) return NULL;
    fprintf(stderr, "[launcher] romfs image: %s\n", why);
    const char *what = why[0] ? why : "Could not build the game data image. Check free storage and try again.";
    pthread_mutex_lock(&g_lock);
    g_fail_detail = what;  // (nativeImportDone(5) follows)
    pthread_mutex_unlock(&g_lock);
    return env->NewStringUTF(what);
}

// The DLC (Settings > DLC; BOTWRecompAndroidActivity importDlc): its image built as romfs.bin is, at
// <1.0.0's data root>/dlc/aoc.bin (services/fs.c fs_aoc_image), the movies loose in dlc/romfs (hle/movie.c)
static std::string g_dlc_msg;  // (g_lock) the last install's result
extern "C" const char *fs_aoc_image(void);  // (runtime/services/fs.c: the DLC image, NULL when not set up)
static bool g_dlc_failed, g_dlc_busy;
static std::atomic<float> g_dlc_frac{-1};
static std::string dlc_dir(void) {
    char r[1024];
    return rt_version_root(100, r, sizeof r) ? std::string(r) + "/dlc" : std::string();
}
static void dlc_progress(uint64_t done, uint64_t total) { g_dlc_frac = total ? (float)((double)done / (double)total) : 0; }
extern "C" JNIEXPORT jstring JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeBuildDlc(
        JNIEnv *env, jobject act, jobjectArray paths, jlongArray sizes) {
    int n = env->GetArrayLength(paths);
    std::vector<std::string> keep_alive(n);
    std::vector<RomfsEntry> entries(n);
    std::vector<jlong> sz(n);
    if (n) env->GetLongArrayRegion(sizes, 0, n, sz.data());
    for (int i = 0; i < n; i++) {
        jstring s = (jstring)env->GetObjectArrayElement(paths, i);
        const char *c = env->GetStringUTFChars(s, NULL);
        keep_alive[i] = c ? c : "";
        if (c) env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
        entries[i] = {keep_alive[i].c_str(), (uint64_t)sz[i]};
    }
    jclass cls = env->GetObjectClass(act);
    RomfsJni j = {env, act, env->GetMethodID(cls, "openRomfsFile", "(I)I")};
    env->DeleteLocalRef(cls);
    if (!j.open) { env->ExceptionClear(); return env->NewStringUTF("Internal error: openRomfsFile missing."); }
    std::string dir = dlc_dir(), romfs = dir + "/romfs";
    if (dir.empty()) return env->NewStringUTF("no data folder");
    mkdir(dir.c_str(), 0755);
    mkdir(romfs.c_str(), 0755);
    g_dlc_frac = 0;
    char why[256] = "";
    bool ok = romfs_build_entries(entries.data(), n, romfs_jni_open, &j, (dir + "/aoc.bin").c_str(), romfs.c_str(),
                                  keep_loose, dlc_progress, why, sizeof why);
    g_dlc_frac = -1;
    if (ok) return NULL;
    fprintf(stderr, "[launcher] DLC image: %s\n", why);
    return env->NewStringUTF(why[0] ? why : "Could not build the DLC's data image. Check free storage and try again.");
}

extern "C" JNIEXPORT void JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeDlcDone(JNIEnv *env, jclass,
                                                                                                   jint result, jstring detail) {
    const char *t = env->GetStringUTFChars(detail, NULL);
    std::string d = t ? t : "";
    if (t) env->ReleaseStringUTFChars(detail, t);
    char msg[600];
    switch (result) {
    case 0: snprintf(msg, sizeof msg, "%s", tr("DLC installed. Start the game: the title shows Master Mode.")); break;
    case 2: msg[0] = 0; break;
    case 3: snprintf(msg, sizeof msg, "%s", tr("No DLC there: choose the folder with the DLC's romfs (System/AocVersion.txt and Pack).")); break;
    case 5: snprintf(msg, sizeof msg, "%s", tr("That's only the Champions' Ballad's unlock: choose the larger pack (01007EF00011F001), "
                                               "which holds both DLCs.")); break;
    default: snprintf(msg, sizeof msg, tr("That didn't work: %s"), d.c_str()); break;
    }
    fprintf(stderr, "[launcher] DLC setup: %d %s\n", (int)result, d.c_str());
    pthread_mutex_lock(&g_lock);
    g_dlc_msg = msg, g_dlc_failed = result >= 3, g_dlc_busy = false;
    pthread_mutex_unlock(&g_lock);
}

static void remove_tree(const std::string &p) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = opendir(p.c_str())) {
            while (struct dirent *e = readdir(d))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) remove_tree(p + "/" + e->d_name);
            closedir(d);
        }
        rmdir(p.c_str());
    } else {
        remove(p.c_str());
    }
}

// The import thread's cause of a failure (an exception: "IOException: ..."), ahead of nativeImportDone
extern "C" JNIEXPORT void JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeImportDetail(JNIEnv *env, jclass,
                                                                                                         jstring what) {
    const char *c = what ? env->GetStringUTFChars(what, NULL) : NULL;
    pthread_mutex_lock(&g_lock);
    g_fail_detail = c ? c : "";
    pthread_mutex_unlock(&g_lock);
    if (c) env->ReleaseStringUTFChars(what, c);
}

// An earlier version copied all of romfs/ beside romfs.bin (its ~15 GB twice): once the image is there, only the loose
// few stay (keep_loose).
static void prune_loose(const std::string &dir, const std::string &rel) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string p = dir + "/" + e->d_name, r = rel.empty() ? e->d_name : rel + "/" + e->d_name;
        struct stat st;
        if (lstat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            prune_loose(p, r);
            rmdir(p.c_str());  // (only when it was left empty)
        } else if (!keep_loose(r.c_str())) {
            remove(p.c_str());
        }
    }
    closedir(d);
}
static void *prune_thread(void *) {
    prune_loose(g_root + "/romfs", "");
    fprintf(stderr, "[launcher] loose game files trimmed to what the runtime opens by path\n");
    return NULL;
}
static void android_prune_loose(void) {  // (start screen, game files ready)
    static bool done;
    if (done) return;
    done = true;
    struct stat st;
    if (stat((g_root + "/romfs.bin").c_str(), &st) != 0 || st.st_size < (1ll << 30)) return;
    pthread_t t;
    if (pthread_create(&t, NULL, prune_thread, NULL) == 0) pthread_detach(t);
}

// 0 copied, 1 cancelled, 2 no game files found, 3 game packages instead of files, 4 another version, 5 copy failed,
// 6 the romfs/ doesn't go with the exefs/ (nativeCheckRomfs set the failure)
extern "C" JNIEXPORT void JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeImportDone(JNIEnv *, jclass, jint result) {
    if (result == 0) {
        g_android_imported = true;
        pthread_t t;
        pthread_create(&t, NULL, extract_thread, NULL);
        pthread_detach(t);
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_busy = false;
    g_dest.clear();
    std::string detail = g_fail_detail;
    g_fail_detail.clear();
    pthread_mutex_unlock(&g_lock);
    if (result == 1 || result == 6) return;
    set_failure(result == 2 ? FAIL_NOT_FOUND : result == 3 ? FAIL_UNUSABLE : result == 4 ? FAIL_VERSION : FAIL_COPY);
    if (result == 5 && detail.rfind("can't read ", 0) == 0)  // (not the free space: issue #17 read it as 30 GB asked for)
        set_job("Setup didn't finish", true,
                "A file in the chosen folder couldn't be read. Copy the whole game folder again and try again.");
    set_job_detail(detail);
}

extern "C" JNIEXPORT void JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeDriverPicked(JNIEnv *env, jclass, jstring name) {
    const char *n = env->GetStringUTFChars(name, NULL);
    pthread_mutex_lock(&g_lock);
    char m[512];
    snprintf(m, sizeof m, tr("%s installed. Restart BOTWRecompAndroid to use it."), n ? n : "");
    g_driver_msg = n && !strcmp(n, "-") ? tr("The system's driver from the next start on. Restart BOTWRecompAndroid to use it.")
                   : n && *n                ? m
                                            : tr("That file isn't a driver.");
    g_driver_restart = n && *n;
    pthread_mutex_unlock(&g_lock);
    if (n) env->ReleaseStringUTFChars(name, n);
}
#endif

// ------------------------------------------------------------------ folder dialog (SDL calls back on any thread)
static void SDLCALL picked(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_src = files[0];
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------------ the user's own Nintendo fonts (not shipped)
// runtime/hle/pl.c loads fonts/ first, else a close substitute (Hiragino Sans). Import copies recognized files only.
static const char *FONT_STD[] = {"nintendo_udsg-r_std_003.bfttf", "nintendo_udsg-r_std_003.ttf",
                                 "nintendo_udsgr_std_003.ttf", "nintendo_udsgr_std_003.bfttf"};
static const char *FONT_EXT[] = {"nintendo_ext_003.bfttf", "nintendo_ext_003.ttf"};
static const char *FONT_CJK[] = {"nintendo_udsg-r_org_zh-cn_003.bfttf", "nintendo_udsg-r_org_zh-cn_003.ttf",  // (pl.c)
                                 "nintendo_udsg-r_ext_zh-cn_003.bfttf", "nintendo_udsg-r_ext_zh-cn_003.ttf",
                                 "nintendo_udjxh-db_zh-tw_003.bfttf",   "nintendo_udjxh-db_zh-tw_003.ttf",
                                 "nintendo_udsg-r_ko_003.bfttf",        "nintendo_udsg-r_ko_003.ttf"};
static std::vector<std::string> g_font_picks;
static std::string g_font_msg;

static bool have_font(const char *const *names, int n) {
    for (int i = 0; i < n; i++)
        if (file_exists(std::string("fonts/") + names[i])) return true;
    return false;
}

static void SDLCALL picked_fonts(void *, const char *const *files, int) {
    if (!files) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; files[i]; i++) g_font_picks.push_back(files[i]);
    pthread_mutex_unlock(&g_lock);
}

static void import_fonts() {
    pthread_mutex_lock(&g_lock);
    std::vector<std::string> picks;
    picks.swap(g_font_picks);
    pthread_mutex_unlock(&g_lock);
    if (picks.empty()) return;
    mkdir("fonts", 0755);
    int ok = 0, skipped = 0;
    for (auto &src : picks) {
        std::string base = src.substr(src.find_last_of('/') + 1), low;
#ifdef __ANDROID__
        // Android's dialog hands back content:// URIs, which end in a document id ("primary%3ADownload%2F..." or a
        // number) and which fopen can't open: the name and the copy come through the activity (issue #122)
        bool uri = !src.compare(0, 10, "content://");
        if (uri) base = android_document_name(src);
#endif
        low = base;
        for (auto &c : low) c = (char)tolower(c);
        bool known = false;
        for (auto n : FONT_STD) known |= low == n;
        for (auto n : FONT_EXT) known |= low == n;
        for (auto n : FONT_CJK) known |= low == n;
        if (!known) { skipped++; continue; }
#ifdef __ANDROID__
        if (uri) {
            char cwd[1024];
            if (getcwd(cwd, sizeof cwd) && android_copy_document(src, std::string(cwd) + "/fonts/" + low)) ok++;
            continue;
        }
#endif
        FILE *in = fopen(src.c_str(), "rb"), *out = fopen(("fonts/" + low).c_str(), "wb");
        if (in && out) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
            ok++;
        }
        if (in) fclose(in);
        if (out) fclose(out);
    }
    g_font_msg = std::to_string(ok) + " font file(s) imported" + (skipped ? ", " + std::to_string(skipped) + " not recognized" : "") +
                 ". Used from the next game start.";
}

static void remove_fonts() {
    for (auto n : FONT_STD) unlink((std::string("fonts/") + n).c_str());
    for (auto n : FONT_EXT) unlink((std::string("fonts/") + n).c_str());
    for (auto n : FONT_CJK) unlink((std::string("fonts/") + n).c_str());
    g_font_msg = "Imported fonts removed: the substitute font is used.";
}

// ------------------------------------------------------------------ background: bundled artwork, then a movie frame
#ifdef __APPLE__
static id<MTLTexture> g_bg;
#define BG_TEXID ((ImTextureID)(__bridge void *)g_bg)
typedef id<MTLDevice> GpuDevice;
#else
static uint64_t g_bg;  // ImGui texture id (Vulkan descriptor set)
#define BG_TEXID ((ImTextureID)g_bg)
typedef void *GpuDevice;
#endif
static int g_bg_w, g_bg_h;

static void load_background(GpuDevice dev) {
    if (g_bg) return;
    char artwork[1100];
    if (rt_resource("launcher-background.png", "assets/artwork/great-plateau-background-v2.png", artwork, sizeof artwork)) {
        SDL_Surface *src = SDL_LoadPNG(artwork);
        SDL_Surface *rgba = src ? SDL_ConvertSurface(src, SDL_PIXELFORMAT_RGBA32) : NULL;
        if (src) SDL_DestroySurface(src);
        if (rgba) {
#ifdef __APPLE__
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:rgba->w height:rgba->h mipmapped:NO];
            g_bg = [dev newTextureWithDescriptor:td];
            [g_bg replaceRegion:MTLRegionMake2D(0, 0, rgba->w, rgba->h) mipmapLevel:0
                     withBytes:rgba->pixels bytesPerRow:rgba->pitch];
#else
            // SDL surfaces may pad rows; the upload helper takes tightly packed RGBA pixels.
            std::vector<uint8_t> pixels((size_t)rgba->w * rgba->h * 4);
            for (int y = 0; y < rgba->h; y++)
                memcpy(pixels.data() + (size_t)y * rgba->w * 4, (const uint8_t *)rgba->pixels + (size_t)y * rgba->pitch,
                       (size_t)rgba->w * 4);
            g_bg = vk_imgui_texture_rgba(pixels.data(), rgba->w, rgba->h);
#endif
            g_bg_w = rgba->w, g_bg_h = rgba->h;
            SDL_DestroySurface(rgba);
            if (g_bg) return;
        }
    }
#ifdef LARECOMP_NO_FFMPEG
    (void)dev;
#else
    std::string path = g_root + "/romfs/Movie/Demo101_0.webm";  // (to choose: which movie and frame look best)
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path.c_str(), NULL, NULL) < 0) return;
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si >= 0 && avformat_find_stream_info(fmt, NULL) >= 0) {
        AVStream *st = fmt->streams[si];
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        AVCodecContext *cc = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(cc, st->codecpar);
        if (codec && avcodec_open2(cc, codec, NULL) == 0) {
            AVPacket *pkt = av_packet_alloc();
            AVFrame *fr = av_frame_alloc();
            int64_t want = (int64_t)(6.0 / av_q2d(st->time_base));  // a few seconds in: the island, not black
            av_seek_frame(fmt, si, want, AVSEEK_FLAG_BACKWARD);
            bool got = false;
            while (!got && av_read_frame(fmt, pkt) >= 0) {
                if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) == 0)
                    while (avcodec_receive_frame(cc, fr) == 0)
                        if (fr->best_effort_timestamp >= want) { got = true; break; }
                av_packet_unref(pkt);
            }
            if (got) {
                int w = fr->width, h = fr->height;
                std::vector<uint8_t> rgba((size_t)w * h * 4);
                SwsContext *sws = sws_getContext(w, h, (AVPixelFormat)fr->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
                uint8_t *dst[1] = {rgba.data()};
                int dst_ls[1] = {w * 4};
                sws_scale(sws, fr->data, fr->linesize, 0, h, dst, dst_ls);
                sws_freeContext(sws);
#ifdef __APPLE__
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:w height:h mipmapped:NO];
                g_bg = [dev newTextureWithDescriptor:td];
                [g_bg replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:w * 4];
#else
                (void)dev;
                g_bg = vk_imgui_texture_rgba(rgba.data(), w, h);
#endif
                g_bg_w = w, g_bg_h = h;
            }
            av_frame_free(&fr);
            av_packet_free(&pkt);
        }
        avcodec_free_context(&cc);
    }
    avformat_close_input(&fmt);
#endif
}

// ------------------------------------------------------------------ screen
static ImFont *g_font_title, *g_font_menu, *g_font_small;
enum Page { MAIN, SETTINGS, SETUP };  // SETTINGS: the in-game menu's tabs (runtime/ui/menu.mm)
static int g_refocus = MAIN;  // back on the main page: select the item of the page we came from (controllers)

// A big menu line: gold + a marker when focused / hovered (keyboard, gamepad or mouse), grey when disabled
static bool g_menu_small;  // sub pages: buttons in the small font
void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float));  // menu.mm
void menu_tabs_focus(bool first_tab);
extern int g_tab_request_ext;
extern "C" void menu_apply_window_requests(void *sdl_window);
extern "C" int g_start_screen;  // runtime/hle/game.c: the start screen is up (no game loaded: don't read its memory)
static float g_menu_gap = 0.35f;  // space under each item, in line heights

// The first PLAY ever (settings.menu_tip_seen): a note on opening the BOTWRecompAndroid menu in game (it isn't obvious),
// OK starts the game. Returns true once OK is pressed.
static bool g_tip_test;  // (LARECOMP_LAUNCHER_SHOT=<frame>:tip)
// First PLAY ever, before the menu note: authentic 16:9 or the unlocked (fill the window) aspect ratio.
static bool aspect_prompt(float s) {
    bool done = false;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fminf(ds.x * 0.8f, 760 * s), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 28 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * s);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.03f, 0.04f, 0.05f, 0.94f));
    bool open = ImGui::BeginPopupModal("##aspect", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (open) {
        ImGui::SetWindowFontScale(1.25f);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(tr("How would you like to play?"));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("You can change this any time in Settings."));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14 * s));
        float bw = 300 * s, gap = 20 * s;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 2 * bw - gap) * 0.5f);
        int pick = -1;
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(tr("Authentic 16:9"), ImVec2(bw, 44 * s))) pick = ASPECT_ORIGINAL;
        ImGui::SameLine(0, gap);
        if (ImGui::Button(tr("Unlocked (fill the screen)"), ImVec2(bw, 44 * s))) pick = ASPECT_UNLOCKED;
        if (pick >= 0) {
            g_settings.aspect = pick;
            g_settings.aspect_asked = true;
            settings_save();
            ImGui::CloseCurrentPopup();
            done = true;
        }
        ImGui::EndPopup();
    }
    return done;
}

static bool menu_tip(float s) {
    bool go = false;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fminf(ds.x * 0.8f, 760 * s), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 28 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * s);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.03f, 0.04f, 0.05f, 0.94f));
    bool open = ImGui::BeginPopupModal("##menutip", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (open) {
        ImGui::SetWindowFontScale(1.25f);
        // the controller's home button first (what players reach for: the PS button on a DualSense), the combo second
        static const char *const home[] = {"Home button", "Xbox button", "PlayStation button"};  // (bind_label_family)
        static const char *const pad[] = {"Minus + Plus", "View + Menu", "Create + Options"};
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(tr("BOTWRecompAndroid has its own menu for settings and controls."));
        ImGui::Spacing();
        int fam = bind_label_family() % 3;
#ifdef __ANDROID__
        ImGui::Text(tr("Open it in game with the menu button on the screen or the back button, or on a controller with the "
                       "%s (or %s together)."), tr(home[fam]), tr(pad[fam]));
#else
        ImGui::Text(tr("Open it in game with F1 or Esc, or on a controller with the %s (or %s together)."), tr(home[fam]), tr(pad[fam]));
#endif
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14 * s));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 180 * s) * 0.5f);
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(tr("OK"), ImVec2(180 * s, 44 * s)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
            g_settings.menu_tip_seen = true;
            settings_save();
            ImGui::CloseCurrentPopup();
            go = true;
        }
        ImGui::EndPopup();
    }
    return go;
}

// Start-screen text: a soft drop shadow, then the text with a top-to-bottom colour gradient (top == bottom: flat)
static void text_fx(ImDrawList *dl, ImFont *font, float size, ImVec2 p, ImU32 top, ImU32 bottom, const char *t,
                    float shadow) {
    if (shadow > 0)
        for (float d : {shadow, shadow * 0.5f}) dl->AddText(font, size, ImVec2(p.x + d, p.y + d), IM_COL32(4, 8, 30, 110), t);
    int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, size, p, top, t);
    if (top == bottom) return;
    float y0 = p.y + size * 0.2f, y1 = p.y + size * 0.85f;
    for (int i = v0; i < dl->VtxBuffer.Size; i++) {
        float f = (dl->VtxBuffer[i].pos.y - y0) / (y1 - y0);
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        ImVec4 a = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
        dl->VtxBuffer[i].col = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f,
                                                                     a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f));
    }
}

// A four-point sparkle (PLAY's twinkle): k = 0..1 grows and fades it
static void sparkle(ImDrawList *dl, ImVec2 c, float r, float k, float spin) {
    if (k <= 0) return;
    ImVec2 pts[8];
    for (int i = 0; i < 8; i++) {
        float a = spin + i * 3.14159265f / 4, rr = (i & 1 ? r * 0.22f : r) * k;
        pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
    }
    dl->AddCircleFilled(c, r * 0.55f * k, IM_COL32(255, 230, 150, (int)(60 * k)));  // soft glow
    dl->AddConvexPolyFilled(pts, 8, IM_COL32(255, 248, 215, (int)(235 * k)));
}

// split .. split_end: that part of the label in its own colour (PLAY's choice: Vanilla in vanilla cream)
static bool menu_item(const char *label, bool enabled, bool *hovered = NULL, const char *split = NULL, ImU32 col2 = 0,
                      const char *split_end = NULL) {
    ImGui::PushFont(g_menu_small ? g_font_small : g_font_menu);
    ImGui::BeginDisabled(!enabled);
    ImVec2 at = ImGui::GetCursorScreenPos();
    // (the ID from the label's fixed part: a new ID loses the selection)
    ImGui::PushID(label, split ? split : label + strlen(label));
    if (!split) label = tr(label);  // (shown in the menu's language; the id stays the English one)
    ImVec2 sz = ImGui::CalcTextSize(label);
    // (EnableNav: otherwise an InvisibleButton can't be reached with the d-pad / arrow keys - ImGui 1.91)
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the hover band shows the selection, no box)
    bool clicked = ImGui::InvisibleButton("##b", ImVec2(sz.x + 40, sz.y + 6), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hot = ImGui::IsItemFocused() || ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered) *hovered = hot;
    ImU32 col = !enabled ? IM_COL32(170, 175, 200, 140) : hot ? IM_COL32(255, 214, 64, 255) : IM_COL32(250, 250, 255, 255);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot && enabled) {  // a shadow band behind the item (like the version corner's), feathered, with a warm glow
        float x0 = ImGui::GetWindowPos().x, x1 = at.x + sz.x * 0.75f, x2 = at.x + sz.x + sz.y * 3.5f;
        float f = sz.y * 0.22f, y0 = at.y - f * 0.4f, y1 = y0 + f, y3 = at.y + sz.y + f * 0.2f, y2 = y3 - f;
        ImU32 c = IM_COL32(6, 10, 34, 150), g = IM_COL32(255, 190, 80, 34), n = IM_COL32(6, 10, 34, 0), ng = IM_COL32(255, 190, 80, 0);
        for (int pass = 0; pass < 2; pass++) {  // dark band, then the glow over it
            ImU32 a = pass ? g : c, z = pass ? ng : n;
            dl->AddRectFilledMultiColor(ImVec2(x0, y1), ImVec2(x1, y2), a, a, a, a);
            dl->AddRectFilledMultiColor(ImVec2(x1, y1), ImVec2(x2, y2), a, z, z, a);
            dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), z, z, a, a);  // (feathered top / bottom)
            dl->AddRectFilledMultiColor(ImVec2(x1, y0), ImVec2(x2, y1), z, z, z, a);
            dl->AddRectFilledMultiColor(ImVec2(x0, y2), ImVec2(x1, y3), a, a, z, z);
            dl->AddRectFilledMultiColor(ImVec2(x1, y2), ImVec2(x2, y3), a, z, z, z);
        }
    }
    ImU32 col_b = hot && enabled ? IM_COL32(255, 160, 50, 255) : col;
    if (split && enabled) {
        std::string head(label, split - label);
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), at, col, col_b, head.c_str(), sz.y * 0.05f);
        std::string mid = split_end ? std::string(split, split_end - split) : std::string(split);
        ImVec2 p2(at.x + ImGui::CalcTextSize(label, split).x, at.y);
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), p2, col2, col2, mid.c_str(), sz.y * 0.05f);
        if (split_end)
            text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + ImGui::CalcTextSize(label, split_end).x, at.y),
                    col, col, split_end, sz.y * 0.05f);
    } else
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), at, col, col_b, label, enabled ? sz.y * 0.05f : 0);
    if (g_menu_gap >= 0) ImGui::Dummy(ImVec2(0, sz.y * g_menu_gap));  // line spacing
    else ImGui::SetCursorPosY(ImGui::GetCursorPosY() + sz.y * g_menu_gap);  // (tighter than the font's line height)
    ImGui::EndDisabled();
    ImGui::PopFont();
    return clicked;
}

// Controllers, as in the in-game menu (runtime/ui/menu.mm): read straight from SDL, Nintendo layout - the right face
// button (A) confirms, the bottom one (B) goes back; d-pad / left stick move. (ImGui's own SDL gamepad handling is
// switched off: it used the Xbox layout and, per the user, didn't work on the start screen.)
static ImGuiKey launcher_pad_key(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceDown;   // A: activate
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceRight; // B: back
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

static void launcher_pad_open_all(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

// true when the event was a controller event (handled here)
static bool launcher_pad_event(const SDL_Event &e) {
    ImGuiIO &io = ImGui::GetIO();
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        SDL_OpenGamepad(e.gdevice.which);
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        ImGuiKey k = launcher_pad_key(e.gbutton.button);
#ifdef __ANDROID__
        // ImGui ignores A while its nav cursor is hidden, as it starts out (PLAY focused at launch): A did nothing until
        // the d-pad had moved the selection. A controller press shows it, so this very press acts
        if (k != ImGuiKey_None && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) ImGui::SetNavCursorVisible(true);
#endif
        if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        float v = e.gaxis.value / 32767.0f;
        // the left stick also works as the d-pad (ImGui moves between items with the d-pad only; the stick scrolls):
        // pressed past half way, released under 0.3
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            static bool held[4];  // left, right, up, down
            bool x = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
            int neg = x ? 0 : 2, pos = neg + 1;
            const ImGuiKey keys[4] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadUp,
                                      ImGuiKey_GamepadDpadDown};
            for (int i : {neg, pos}) {
                float m = i == neg ? -v : v;
                bool now = held[i] ? m > 0.3f : m > 0.5f;
                if (now != held[i]) { held[i] = now; io.AddKeyEvent(keys[i], now); }
            }
        }
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, v > 0.3f, v > 0.3f ? v : 0);
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, v > 0.3f, v > 0.3f ? v : 0);
        }
        return true;
    }
    default: return false;
    }
}

// Returns when PLAY is chosen; QUIT exits.
// Settings' first tab, on the start screen only: the extracted game files (size, delete) and the user's own Switch
// fonts (import / back to the substitute)
// Open a folder in Finder / Explorer (relative paths are in the data root, the working directory)
static void reveal_folder(const std::string &dir) {
    std::string path = dir;
    bool abs = !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'));
    if (!abs) path = std::string(rt_data_root()) + "/" + path;
    std::string url = "file://";
    if (path[0] != '/') url += "/";  // (Windows: file:///C:/...)
    for (char ch : path) {
        if (ch == '\\') ch = '/';
        if (ch == ' ') url += "%20";
        else if (ch == '%') url += "%25";
        else if (ch == '#') url += "%23";
        else url += ch;
    }
    SDL_OpenURL(url.c_str());
}

// Another game version from the next start on (runtime/paths.h). Android starts again in it at once (launcher_run, after
// a moment for the line saying so); the desktop asks for a restart.
static int g_switching_to;  // (Android: the version the app is about to start again in)
static double g_switch_at;
static void switch_to_version(int v) {
    if (!rt_select_game_version(v)) {
        set_job("Couldn't switch the game version", true, "BOTWRecompAndroid can't write to its data folder.");
        return;
    }
#ifdef __ANDROID__
    g_switching_to = v;
    g_switch_at = ImGui::GetTime() + 0.6;
#else
    set_job(std::string(tr("Restart BOTWRecompAndroid to play version")) + " " + version_name(v) + ".");
#endif
}

// The setup's Game code (Native or Recompiled) into the version it set up: each version keeps its own settings, so a
// setup of 1.8.2 from 1.9.0's page with Native chosen restarted into 1.8.2's - or a copy of 1.0.0's - with Recompiled,
// and the start screen asked to build the game code the setup had just skipped (players' reports, 2026-10-28)
static void carry_game_code(int version) {
    char vr[1024], main_root[1024];
    if (!rt_version_root(version, vr, sizeof vr) || !rt_version_root(100, main_root, sizeof main_root)) return;
    std::string path = std::string(vr) + "/botwrecompandroid.ini", text, line;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) f = fopen((std::string(main_root) + "/botwrecompandroid.ini").c_str(), "rb");  // (as paths.c version_seed)
    if (f) {
        for (int ch; (ch = fgetc(f)) != EOF;) {
            line += (char)ch;
            if (ch != '\n') continue;
            if (line.compare(0, 11, "native_code")) text += line;
            line.clear();
        }
        if (!line.empty() && line.compare(0, 11, "native_code")) text += line + "\n";
        fclose(f);
    }
    text += std::string("native_code = ") + (g_settings.native_code ? "1" : "0") + "\n";
    f = fopen(path.c_str(), "wb");
    if (f) fputs(text.c_str(), f), fclose(f);
}

// The Game Files page's game versions: the one running, the others set up (choosing one starts the app again in it),
// the ones not set up yet greyed - the files folder below sets them up. focus: the running one takes the cursor.
static void version_rows(bool busy, bool focus) {
    static const char *const notes[N_VERSIONS] = {
        "The game's first release, without updates.",
        "An update of the game.",
        "An update of the game, in Brazilian Portuguese too. 60 fps and widescreen (Video settings) work in this one.",
        "The game's last update (2025), in Brazilian Portuguese too. Being brought up: expect problems."};
    opt_section("Game version");
    int cur = rt_game_version();
    for (int i = 0; i < N_VERSIONS; i++) {
        int v = k_versions[i];
        bool set_up = version_set_up(i);
        std::string label = std::string(tr("Version")) + " " + version_name(v);
        std::string desc = std::string(tr(notes[i])) + "\n\n" +
                           tr(!set_up ? "Not set up yet: choose the folder with this version's game files below."
                                      : "Which version PLAY starts. Each one keeps its own game files, saves and settings; "
                                        "1.6.0, 1.8.2 and 1.9.0 start from a copy of 1.0.0's saves and settings. Choosing another "
                                        "one starts BOTWRecompAndroid again with it.");
        if (busy || g_switching_to || (!set_up && v != cur)) opt_disable_next();
        if (opt_radio(label.c_str(), v == cur, desc.c_str()) && v != cur && set_up) switch_to_version(v);
        if (focus && v == cur) ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    }
}

static SDL_Window *g_launcher_win;
static bool g_files_deleted;  // set here: the start screen goes back to setting the game files up
static bool g_open_files;     // Settings > Prelaunch: the Game Files page (versions, setup) instead
static void prelaunch_tab(float sc) {
    const ImVec4 gold(0.93f, 0.76f, 0.33f, 1);
    auto heading = [&](const char *t) {
#ifdef __ANDROID__  // (the Sheikah look of the menu's pages)
        ui_sheikah_heading(t, sc);
        return;
#endif
        ImGui::PushFont(g_font_menu);
        ImGui::TextColored(gold, "%s", tr(t));
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
    };
#ifdef __ANDROID__
    heading("Vulkan driver");
    {
        const char *d = getenv("LARECOMP_VK_DRIVER");
        const char *rejected = vk_driver_rejected();  // (chosen, but the system's ran: android_driver.c)
        std::string pkg = tr("the system's driver");
        if (d && *d && !*rejected) {  // (the package's name: its folder, files/drivers/<name>/<library>)
            pkg = d;
            size_t slash = pkg.rfind('/');
            if (slash != std::string::npos) pkg.erase(slash);
            slash = pkg.rfind('/');
            if (slash != std::string::npos) pkg.erase(0, slash + 1);
        }
        ImGui::Text(tr("In use: %s"), pkg.c_str());
        ImGui::PushTextWrapPos(0);
        if (*vk_driver_desc()) ImGui::TextDisabled("%s", vk_driver_desc());  // (its own name and version)
        if (*rejected)
            ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.3f, 1.0f), tr("%s couldn't be used on this GPU: choose a driver made for it."),
                               rejected);
        ImGui::TextDisabled("%s", tr("The game runs on the system's driver, lighting included. On Adreno GPUs, Mesa's Turnip "
                                     "driver runs it faster: choose a Turnip driver package (.zip, as for adrenotools)."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(tr("Choose driver"))) android_call("pickDriver");
        if (d && *d) {  // (a custom driver in use: back to the system's)
            ImGui::SameLine();
            if (ImGui::Button(tr("Use the system driver"))) android_call("useSystemDriver");
        }
        pthread_mutex_lock(&g_lock);
        std::string msg = g_driver_msg;
        bool restart = g_driver_restart;
        pthread_mutex_unlock(&g_lock);
        if (!msg.empty()) {
            ImGui::Text("%s", msg.c_str());
            if (restart) {
                ImGui::SameLine();
                if (ImGui::Button(tr("Restart now"))) android_call("restartApp");
            }
        }
    }
    ImGui::Spacing();
    ImGui::Spacing();
    heading("Driver compatibility");
    {   // (runtime/settings.h gpu_heap / inplace_reads / turnip_lrz / log_file: for GPUs where a Turnip path draws wrong;
        // LASRecompAndroid's, with BOTW's in-place reads of draw targets)
        bool changed = false;
        const bool a8 = vk_adreno_8xx(vk_gpu_name());  // (Automatic: on for Adreno 8xx only)
        auto tri = [&](const char *id, int *v, bool auto_on) {  // Automatic / On / Off: -1 / 1 / 0; auto_on: Automatic's choice
            bool ch = false;
            ImGui::PushID(id);
            ch |= ImGui::RadioButton(tr(auto_on ? "Automatic (on for this GPU)" : "Automatic (off for this GPU)"), v, -1);
            ImGui::SameLine();
            ch |= ImGui::RadioButton(tr("On"), v, 1);
            ImGui::SameLine();
            ch |= ImGui::RadioButton(tr("Off"), v, 0);
            ImGui::PopID();
            return ch;
        };
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("For GPUs where the game draws wrong with Turnip (missing text, odd colors, black "
                                  "patches). Automatic suits most phones: the fastest paths on Adreno 8xx GPUs, the safer "
                                  "ones on the others."));
        ImGui::Text(tr("This GPU: %s"), vk_gpu_desc());
        if (!vk_driver_is_turnip())
            ImGui::TextDisabled("%s", tr("These options are for Turnip: the driver in use takes the safer paths anyway."));
        ImGui::Spacing();
        ImGui::Text("%s", tr("Game memory on the GPU (Turnip)"));
        changed |= tri("heap", &g_settings.gpu_heap, vk_turnip_heap_on(-1));  // (any Turnip that places it)
        ImGui::TextDisabled("%s", tr("On: the game's memory is mapped straight into the GPU's, the fastest way. Off: what each "
                                  "frame uses is copied instead - it draws right where the fast way doesn't, but this game "
                                  "runs at about half the frame rate (15 fps instead of 35 on an Adreno 830). Automatic: on "
                                  "wherever Turnip can map it - without it an Adreno 7xx shows no text. Applies when the "
                                  "game starts."));
        ImGui::Spacing();
        ImGui::Text("%s", tr("Draw-target reads in place (Turnip)"));
        changed |= tri("inplace", &g_settings.inplace_reads, a8);
        ImGui::TextDisabled("%s", tr("On: decals and effects read the image they draw on as it is, the fastest way. Off: a copy "
                                  "of it each time, slower (fewer fps) but by the Vulkan rules. Automatic: on for Adreno 8xx "
                                  "GPUs, off for the others. Applies when the game starts."));
        ImGui::Spacing();
        ImGui::Text("%s", tr("Turnip low-resolution Z (LRZ)"));
        changed |= tri("lrz", &g_settings.turnip_lrz, a8);
        ImGui::TextDisabled("%s", tr("Turnip's early depth test, a little faster. Off can fix missing or broken drawing on "
                                  "some Adreno GPUs. Automatic: on for Adreno 8xx GPUs, off for the others. Applies when "
                                  "BOTWRecompAndroid restarts."));
        bool lrz_now = g_settings.turnip_lrz < 0 ? a8 : g_settings.turnip_lrz != 0;
        if (lrz_now != vk_lrz_at_start()) {  // (Turnip reads it once, when the app starts)
            ImGui::Text("%s", tr("Restart BOTWRecompAndroid to apply it."));
            ImGui::SameLine();
            ImGui::PushID("lrz");  // (the driver's own Restart now may be showing too)
            if (ImGui::Button(tr("Restart now"))) {
                settings_save();
                android_call("restartApp");
            }
            ImGui::PopID();
        }
        if (!g_log_folder_read) {
            g_log_folder_read = true;
            log_folder_read();
        }
        pthread_mutex_lock(&g_lock);
        std::string folder = g_log_folder;
        pthread_mutex_unlock(&g_lock);
        ImGui::Spacing();
        if (ImGui::Checkbox(tr("Save a log file"), &g_settings.log_file)) {
            changed = true;
            if (g_settings.log_file && folder.empty()) android_call("pickLogFolder");  // (then nativeLogFolderPicked)
            else android_log_file(g_settings.log_file);
        }
        ImGui::TextDisabled("%s", tr("Writes what the app reports (GPU, driver, errors) to botwrecomp-log.txt in a folder you "
                                  "choose (create one, e.g. Download/BOTWRecomp), to open on the phone and send with a "
                                  "bug report. Turn it on and play until the problem shows; after a crash, the run "
                                  "before is kept as botwrecomp-log-previous.txt."));
        if (folder.empty()) ImGui::TextDisabled("%s", tr("No folder chosen: the log stays in the app's own folder, which "
                                                         "Android only lets a computer (adb) read."));
        else ImGui::Text(tr("Log folder: %s"), folder.c_str());
        ImGui::PushID("logdir");
        if (ImGui::Button(tr(folder.empty() ? "Choose folder" : "Change folder"))) android_call("pickLogFolder");
        ImGui::PopID();
        ImGui::PopTextWrapPos();
        if (changed) settings_save();
    }
    ImGui::Spacing();
    ImGui::Spacing();
#endif
    heading("Game files");
    ImGui::Text(tr("Game version: %s"), version_name(rt_game_version()));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Versions 1.0.0, 1.6.0, 1.8.2 and 1.9.0 can be set up side by side, each with its own saves."));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(tr("Versions and setup"))) g_open_files = true;
    static double gb = -1;
    if (gb < 0) gb = rt_game_files_size(g_root.c_str()) / 1e9;
#ifdef __ANDROID__
    // (no file manager to show the folder in)
#elif defined(_WIN32)
    if (ImGui::Button("Show in Explorer")) reveal_folder(g_root);
#elif defined(__linux__)
    if (ImGui::Button("Open folder")) reveal_folder(g_root);
#else
    if (ImGui::Button("Show in Finder")) reveal_folder(g_root);
#endif
#ifndef __ANDROID__
    ImGui::SameLine();
#endif
    static bool shot_del = getenv("LARECOMP_LAUNCHER_SHOT") && strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":askdelete");
    if (ImGui::Button(tr("Delete game files")) || shot_del) ImGui::OpenPopup("##delfiles"), shot_del = false;  // (test: :askdelete)
    if (ImGui::BeginPopupModal("##delfiles", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushFont(g_font_menu);
        ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "%s", tr("Are you sure?"));
        ImGui::PopFont();
        ImGui::Text(tr("This deletes BOTWRecompAndroid's copy of the game files (%.1f GB)."), gb);
        ImGui::TextDisabled("%s", tr("Saves and settings are kept. Your own game files aren't touched."));
        if (rt_dev_mode()) {  // (a source checkout: the files are the project's own extracted/)
            bool abs = !g_root.empty() && (g_root[0] == '/' || (g_root.size() > 1 && g_root[1] == ':'));
            std::string where = abs ? g_root : std::string(rt_data_root()) + "/" + g_root;
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Dev checkout: this is %s", where.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button(tr("Delete"), ImVec2(220 * sc, 0))) {
            bool ok = rt_remove_game_files(g_root.c_str());  // (runtime/paths.c: only what the extraction made)
            set_job(ok ? "Game files deleted." : "Could not delete the game files (not in BOTWRecompAndroid's data folder?).", !ok);
            g_files_deleted = true, gb = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(tr("Cancel"), ImVec2(220 * sc, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
#ifdef __ANDROID__
    ImGui::Spacing();
    ImGui::Spacing();
    heading("Saves");
    {
        pthread_mutex_lock(&g_lock);
        std::string msg = g_save_msg;
        bool failed = g_save_failed, sbusy = g_save_busy;
        pthread_mutex_unlock(&g_lock);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("The saves are in the app's own folder, out of file managers' reach. Export saves this "
                                     "version's as one .zip where you choose (to keep, edit, send or move them). Import "
                                     "replaces them with the saves in a .zip - one exported here, or any holding 0, 1, 2... "
                                     "and option.sav; Import from folder takes an emulator's save folder. The ones before "
                                     "are kept beside them. A save from a newer version of the game may not load in an "
                                     "older one."));
        if (sbusy) ImGui::BeginDisabled();
        if (ImGui::Button(tr("Export saves"))) {
            pthread_mutex_lock(&g_lock);
            g_save_busy = true, g_save_msg.clear();
            pthread_mutex_unlock(&g_lock);
            android_call("pickSaveExport");  // (then nativeSaveDone)
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Import saves"))) {
            pthread_mutex_lock(&g_lock);
            g_save_busy = true, g_save_msg.clear();
            pthread_mutex_unlock(&g_lock);
            android_call("pickSaveImport");
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Import from folder"))) {
            pthread_mutex_lock(&g_lock);
            g_save_busy = true, g_save_msg.clear();
            pthread_mutex_unlock(&g_lock);
            android_call("pickSaveImportFolder");
        }
        if (sbusy) ImGui::EndDisabled();
        if (!msg.empty()) {
            if (failed) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", msg.c_str());
            else ImGui::Text("%s", msg.c_str());
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    heading("DLC");
    {
        pthread_mutex_lock(&g_lock);
        std::string msg = g_dlc_msg;
        bool failed = g_dlc_failed, dbusy = g_dlc_busy;
        pthread_mutex_unlock(&g_lock);
        bool have = fs_aoc_image() != NULL;
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("The Expansion Pass - The Master Trials and The Champions' Ballad - for game versions "
                                     "1.6.0, 1.8.2 and 1.9.0. Choose the folder with your own DLC's extracted romfs: the "
                                     "larger pack (01007EF00011F001, 7,094 files, about 1.4 GB) holds both; the other "
                                     "one is only the Ballad's unlock and isn't needed. The DLC isn't included with "
                                     "BOTWRecompAndroid."));
        ImGui::Text("%s", tr(have ? "Installed." : "Not installed."));
        float f = g_dlc_frac;
        if (dbusy && f >= 0) ImGui::Text(tr("Installing the DLC... %d%%"), (int)(f * 100));
        if (dbusy || g_busy) ImGui::BeginDisabled();
        if (ImGui::Button(tr(have ? "Install again" : "Install DLC"))) {
            pthread_mutex_lock(&g_lock);
            g_dlc_busy = true, g_dlc_msg.clear();
            pthread_mutex_unlock(&g_lock);
            android_call("pickDlcFolder");  // (then importDlc -> nativeBuildDlc -> nativeDlcDone)
        }
        if (have) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Remove DLC"))) {
                remove_tree(dlc_dir());
                pthread_mutex_lock(&g_lock);
                g_dlc_msg = tr("DLC removed."), g_dlc_failed = false;
                pthread_mutex_unlock(&g_lock);
            }
        }
        if (dbusy || g_busy) ImGui::EndDisabled();
        if (!msg.empty()) {
            if (failed) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", msg.c_str());
            else ImGui::Text("%s", msg.c_str());
        }
        ImGui::PopTextWrapPos();
    }
#endif
    ImGui::Spacing();
    ImGui::Spacing();
    heading("Game font");
    import_fonts();
    bool fs = have_font(FONT_STD, 4), fe = have_font(FONT_EXT, 2);
    ImGui::Text(tr("Text: %s    Button icons: %s"), tr(fs ? "Nintendo font (imported)" : "substitute (Hiragino Sans)"),
                tr(fe ? "imported" : "substitute"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Fonts aren't included. For the original look, dump them from your own Switch and import: "
                                 "nintendo_udsg-r_std_003.bfttf (text) and nintendo_ext_003.bfttf (button icons). "
                                 ".ttf versions with the same names work too."));
    ImGui::PopTextWrapPos();
    if (!g_font_msg.empty()) ImGui::Text("%s", tr(g_font_msg.c_str()));
    static const SDL_DialogFileFilter font_f[] = {{"Switch fonts", "bfttf;ttf"}};
    if (ImGui::Button(tr("Import fonts"))) SDL_ShowOpenFileDialog(picked_fonts, NULL, g_launcher_win, font_f, 1, NULL, true);
    if (fs || fe) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Use substitute font"))) remove_fonts();
    }
}

extern "C" void launcher_run(const char *root) {
    g_root = root;
    g_start_screen = 1;
    static bool update_checked;  // (once per launch, not again after a "Title screen" reboot)
#ifdef __ANDROID__
    update_checked = true;  // (the PC releases: not for this app, and no curl on the phone)
#endif
    if (!update_checked && !getenv("LARECOMP_NO_UPDATE_CHECK")) {
        update_checked = true;
        pthread_t t;
        if (pthread_create(&t, NULL, update_thread, NULL) == 0) pthread_detach(t);
    }
    ui_window_init();
    SDL_Window *win = ui_window();
    g_launcher_win = win;
#ifdef __APPLE__
    CAMetalLayer *layer = (__bridge CAMetalLayer *)ui_metal_layer();
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    id<MTLCommandQueue> queue = [dev newCommandQueue];
#else
    GpuDevice dev = NULL;
#endif

    IMGUI_CHECKVERSION();
    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();  // the in-game menu's, when back here after a reboot
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    // fonts belong to this context: back here via "Title screen" (in-place reboot) the previous run's are gone with
    // its context, so load them again (a stale pointer here aborted in PushFont - user, 0.1 RC)
    g_font_small = g_font_menu = g_font_title = NULL;
    // Baloo 2 SemiBold (OFL, shipped: third_party/fonts/Baloo2) - rounded, game-like; it runs small, hence the sizes
    char baloo[1024];
    std::string own = rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo) ? baloo : "";
    const char *fonts[] = {getenv("LARECOMP_LAUNCHER_FONT") ? getenv("LARECOMP_LAUNCHER_FONT") : "",  // (test)
                           own.c_str(), "fonts/nintendo_udsgr_std_003.ttf", "/System/Library/Fonts/Avenir Next.ttc",
                           "/System/Library/Fonts/Helvetica.ttc", "C:/Windows/Fonts/segoeui.ttf"};
    static const ImWchar ranges[] = {0x20, 0xFF, 0x2039, 0x203A, 0x2192, 0x2192, 0x2713, 0x2713, 0x25B8, 0x25B8, 0};  // + ‹ › → ✓ ▸
    for (const char *p : fonts) {
        if (g_font_menu || !*p || !file_exists(p)) continue;
        bool b2 = p == own.c_str() || getenv("LARECOMP_LAUNCHER_FONT");
        // (each followed by the menu language's other letters, if it needs any: ui/i18n.mm - not the title's, the
        // app's name)
        g_font_small = io.Fonts->AddFontFromFileTTF(p, 30 * (b2 ? 1.75f : 1), NULL, ranges);
        i18n_merge_fonts(io.Fonts, 30 * (b2 ? 1.75f : 1));
        g_font_menu = io.Fonts->AddFontFromFileTTF(p, 52 * (b2 ? 1.85f : 1), NULL, ranges);
        i18n_merge_fonts(io.Fonts, 52 * (b2 ? 1.85f : 1));
        g_font_title = io.Fonts->AddFontFromFileTTF(p, 92 * (b2 ? 2.3f : 1), NULL, ranges);
    }
    if (!g_font_menu) g_font_small = g_font_menu = g_font_title = io.Fonts->AddFontDefault();
    if (!io.Fonts->Build()) {  // (a font ImGui couldn't read left the atlas empty: its upload crashed - ImGui's own font)
        fprintf(stderr, "[ui] the start screen's fonts didn't build: ImGui's default font instead\n");
        io.Fonts->Clear();
        g_font_small = g_font_menu = g_font_title = io.Fonts->AddFontDefault();
    }
    ImGuiStyle &st = ImGui::GetStyle();
    st.Colors[ImGuiCol_Header] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.08f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.14f);
    st.Colors[ImGuiCol_NavCursor] = ImVec4(0.95f, 0.78f, 0.35f, 1);
    st.FrameRounding = st.WindowRounding = 10;
#ifdef __APPLE__
    ImGui_ImplSDL3_InitForMetal(win);
    ImGui_ImplMetal_Init(dev);
#else
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();
#endif
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, NULL, 0);  // (controllers: launcher_pad_event)
    launcher_pad_open_all();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    std::string status = check_game_files(g_root);
#ifdef __ANDROID__
    if (status.empty()) android_prune_loose();
#endif
    load_background(dev);
    Page page = status.empty() ? MAIN : SETUP;
    bool play = false;
    // LARECOMP_LAUNCHER_SHOT=<frame>[:<page>] (test): save build/launcher.png at that frame and exit; page: settings[:<tab>] / askdelete / setup
    long shot_at = getenv("LARECOMP_LAUNCHER_SHOT") ? atol(getenv("LARECOMP_LAUNCHER_SHOT")) : 0, frame = 0;
    if (shot_at) {
#ifdef __APPLE__
        layer.framebufferOnly = NO;
#endif
        const char *pg = strchr(getenv("LARECOMP_LAUNCHER_SHOT"), ':');
        if (pg && (!strncmp(pg + 1, "settings", 8) || !strcmp(pg + 1, "askdelete"))) page = SETTINGS, menu_tabs_focus(true);
        if (pg && !strcmp(pg + 1, "setup")) page = SETUP;
        if (pg && !strncmp(pg + 1, "fail", 4)) page = SETUP, set_failure(atoi(pg + 5));  // (the failure messages)
        if (pg && !strncmp(pg + 1, "settings", 8) && pg[9] == ':') g_tab_request_ext = atoi(pg + 10);  // settings:<tab>
        if (pg && !strcmp(pg + 1, "delete")) page = SETUP, g_confirm_delete = true;
        if (pg && !strcmp(pg + 1, "tip")) g_settings.menu_tip_seen = false, g_tip_test = true;
        if (pg && !strcmp(pg + 1, "aspect")) g_settings.aspect_asked = g_settings.menu_tip_seen = false, g_tip_test = true;
    }

    static int runs;  // (test: =once - only the first time, so a "Title screen" reboot lands on the start screen)
    const char *ap = getenv("LARECOMP_LAUNCHER_AUTOPLAY");
    if (ap && status.empty() && !(runs && !strcmp(ap, "once"))) play = true;  // (test: PLAY at once)
    runs++;
    if (const char *t = getenv("LARECOMP_LAUNCHER_EXTRACT")) {  // (test) <folder>: set the game files up, log, exit
        g_src = t;
        g_busy = true;
        extract_thread(NULL);
        fprintf(stderr, "[launcher] setup: %s -> %s\n", g_job.c_str(), check_game_files(g_root).empty() ? "READY" : "NOT READY");
        exit(0);
    }
    if (const char *t = getenv("LARECOMP_LAUNCHER_SETUP")) {  // (test) <folder>: Set up started on the start screen
        g_src = t;
        g_busy = true;
        page = SETUP;
        pthread_t th;
        pthread_create(&th, NULL, extract_thread, NULL);
        pthread_detach(th);
    }
    if (getenv("LARECOMP_LAUNCHER_DELETE")) {  // (test) what DELETE GAME FILES does, logged; then the start screen
        double gb = rt_game_files_size(g_root.c_str()) / 1e9;
        bool ok = rt_remove_game_files(g_root.c_str());
        status = check_game_files(g_root);
        fprintf(stderr, "[launcher] delete %.1f GB: %s -> %s\n", gb, ok ? "ok" : "refused", status.c_str());
        if (!status.empty()) { g_bg = 0; page = SETUP; }
    }
    while (!play) {
        menu_apply_window_requests(win);  // (the Video tab's Fullscreen)
        if (g_files_deleted) {  // (Settings > Prelaunch deleted them)
            g_files_deleted = false;
            status = check_game_files(g_root);
            if (!status.empty()) { g_bg = 0; bind_capture_cancel(); settings_save(); page = SETUP; }
        }
        if (g_open_files) {  // (Settings > Prelaunch: Versions and setup)
            g_open_files = false;
            bind_capture_cancel();
            settings_save();
            page = SETUP;
        }
#ifdef __ANDROID__
        if (g_switching_to && ImGui::GetTime() >= g_switch_at) {  // another game version: the app again, in its folder
            g_switch_at = 1e30;
            settings_save();
            android_call("restartApp");
        }
#endif
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (page == SETTINGS && bind_capture_event(&e)) continue;  // (waiting for a new binding: controls_ui.mm)
            if (!launcher_pad_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) exit(0);
        }
        // extraction finished: re-check, load the background
        pthread_mutex_lock(&g_lock);
        bool busy = g_busy;
        pthread_mutex_unlock(&g_lock);
        static bool was_busy;
        if (was_busy && !busy) {
            pthread_mutex_lock(&g_lock);
            int set_up = g_set_up_version;
            g_set_up_version = 0;
            bool failed = g_job_failed;
            pthread_mutex_unlock(&g_lock);
            if (set_up && set_up != rt_game_version()) {  // (set up beside this one: played now)
                carry_game_code(set_up);
                switch_to_version(set_up);
            }
            status = check_game_files(g_root);
            // (a failure stays on the page with its cause: with this version's files ready, a setup of another folder
            // that failed went back to the start screen without a word)
            if (status.empty() && !g_switching_to && !failed) { load_background(dev); page = MAIN; }
        }
        was_busy = busy;

#ifdef __APPLE__
        int w, h;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        layer.drawableSize = CGSizeMake(w, h);
        @autoreleasepool {
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) { SDL_Delay(16); continue; }
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.04, 0.05, 0.06, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            ImGui_ImplMetal_NewFrame(rp);
#else
        {
            // one frame per refresh of the window's display: the swapchain presents without waiting (mailbox, for the
            // game's own pacing: device.c), so the start screen alone would draw as fast as the GPU goes
            static uint64_t next_frame;
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
            uint64_t period = (uint64_t)(1e9 / (dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60.0f)), now = plat_now_ns();
            bool on_time = next_frame > now && next_frame - now <= period;  // (else late, or the rate changed: start over)
            if (on_time) plat_sleep_until_ns(next_frame);
            next_frame = (on_time ? next_frame : now) + period;
            static const float clear_col[4] = {0.04f, 0.05f, 0.06f, 1};
            if (!vk_frame_begin(clear_col)) { SDL_Delay(16); continue; }
            ImGui_ImplVulkan_NewFrame();
#endif
            ImGui_ImplSDL3_NewFrame();
            // the backend (manual mode, no gamepads of its own) clears HasGamepad every frame, and ImGui then drops the
            // controller presses launcher_pad_event fed in and turns gamepad navigation off: set it again while a
            // controller is connected
            if (SDL_HasGamepad()) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            ImGui::NewFrame();

            ImVec2 ds = io.DisplaySize;
            float s = ds.y / 720.0f;  // layout scale (fonts are built for ~2x)
            ImGui::GetIO().FontGlobalScale = s * 0.5f;
            ImDrawList *bg = ImGui::GetBackgroundDrawList();
            if (g_bg) {  // cover the window, keep the aspect
                float ia = (float)g_bg_w / g_bg_h, wa = ds.x / ds.y;
                ImVec2 uv0(0, 0), uv1(1, 1);
                if (wa > ia) { float v = ia / wa; uv0.y = (1 - v) / 2; uv1.y = 1 - uv0.y; }
                else { float u = wa / ia; uv0.x = (1 - u) / 2; uv1.x = 1 - uv0.x; }
                bg->AddImage(BG_TEXID, ImVec2(0, 0), ds, uv0, uv1);
            } else {
                bg->AddRectFilledMultiColor(ImVec2(0, 0), ds, IM_COL32(18, 34, 40, 255), IM_COL32(40, 70, 64, 255),
                                            IM_COL32(12, 20, 24, 255), IM_COL32(8, 12, 16, 255));
            }
            {  // a shadow banner fading in from the left behind the menu, and one from the right behind the version
                const float stops[][2] = {{0.0f, 225}, {0.3f, 215}, {0.44f, 165}, {0.55f, 95}, {0.64f, 35}, {0.74f, 0}};
                for (int i = 0; i + 1 < (int)(sizeof stops / sizeof stops[0]); i++) {
                    ImU32 a = IM_COL32(6, 10, 34, (int)stops[i][1]), b = IM_COL32(6, 10, 34, (int)stops[i + 1][1]);
                    bg->AddRectFilledMultiColor(ImVec2(ds.x * stops[i][0], 0), ImVec2(ds.x * stops[i + 1][0], ds.y), a, b, b, a);
                }
                float y0 = ds.y - 190 * s, y1 = ds.y - 135 * s, y2 = ds.y - 40 * s, y3 = ds.y, x0r = ds.x * 0.5f;
                ImU32 c0 = IM_COL32(6, 10, 34, 0), c1 = IM_COL32(6, 10, 34, 235);
                bg->AddRectFilledMultiColor(ImVec2(x0r, y0), ImVec2(ds.x, y1), c0, c0, c1, c0);  // (feathered top)
                bg->AddRectFilledMultiColor(ImVec2(x0r, y1), ImVec2(ds.x, y2), c0, c1, c1, c0);
                bg->AddRectFilledMultiColor(ImVec2(x0r, y2), ImVec2(ds.x, y3), c0, c1, c0, c0);  // (feathered bottom)
            }

            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ds);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImGui::SetScrollX(0);  // (the start screen never scrolls; a stray nav move mustn't shift it)
            ImGui::SetScrollY(0);
            float x0 = 90 * s;
            ImGui::SetCursorPos(ImVec2(x0, 120 * s));
            ImGui::PushFont(g_font_title);
            if (page == MAIN) {  // gold to orange, shadowed (not behind the Settings / Game Files windows)
                const char *t = "BOTWRecompAndroid";
                ImVec2 p = ImGui::GetCursorScreenPos();
                text_fx(ImGui::GetWindowDrawList(), ImGui::GetFont(), ImGui::GetFontSize(), p, IM_COL32(255, 232, 120, 255),
                        IM_COL32(255, 138, 48, 255), t, 3 * s);
                ImGui::Dummy(ImGui::CalcTextSize(t));
            }
            ImGui::PopFont();
#ifdef __ANDROID__
            if (page == MAIN) {  // "Android" under the title, after the Android robot's head (Google, CC BY 3.0), in its green
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImGui::PushFont(g_font_menu);
                float fs = ImGui::GetFontSize();
                ImGui::SetCursorPosX(x0);
                ImVec2 p = ImGui::GetCursorScreenPos();
                p.y -= fs * 0.3f;  // (the title's line runs deep below its letters)
                float r = fs * 0.34f;  // the head: a half disc on its flat side, two antennae, two eyes
                ImVec2 c(p.x + 8 * s + r, p.y + fs * 0.66f);
                const ImU32 green = IM_COL32(61, 220, 132, 255), shade = IM_COL32(4, 8, 30, 150);
                auto head = [&](float dx, float dy, ImU32 col, bool eyes) {
                    ImVec2 o(c.x + dx, c.y + dy);
                    dl->PathArcTo(o, r, IM_PI, 2 * IM_PI, 32);
                    dl->PathFillConvex(col);
                    for (float sg : {-1.0f, 1.0f})
                        dl->AddLine(ImVec2(o.x + sg * r * 0.45f, o.y - r * 0.78f), ImVec2(o.x + sg * r * 0.7f, o.y - r * 1.22f),
                                    col, r * 0.09f);
                    if (eyes)
                        for (float sg : {-1.0f, 1.0f})
                            dl->AddCircleFilled(ImVec2(o.x + sg * r * 0.42f, o.y - r * 0.42f), r * 0.1f, IM_COL32(16, 24, 48, 255));
                };
                head(2.5f * s, 2.5f * s, shade, false);
                head(0, 0, green, true);
                const char *t = "Android";
                ImVec2 tp(c.x + r + fs * 0.28f, p.y);
                for (float d : {2.5f, 1.5f}) dl->AddText(ImGui::GetFont(), fs, ImVec2(tp.x + d * s, tp.y + d * s), shade, t);
                dl->AddText(ImGui::GetFont(), fs, tp, green, t);
                ImGui::PopFont();
            }
#endif
            ImGui::SetCursorPos(ImVec2(x0, 285 * s));
            // typing in a text field / the on-screen keyboard (osk.h): B / Esc are for it, not "back" (from the frame
            // before too, so the Esc that ends the typing doesn't also leave the page)
            static bool typed_before;
            bool typing = typed_before || ui_text_busy();

            if (page == MAIN) {
                {  // the four items close together, the block anchored where QUIT always was (the gap to the status
                   // line stays)
                    ImGui::PushFont(g_font_menu);
                    float h = ImGui::CalcTextSize("A").y, sp = ImGui::GetStyle().ItemSpacing.y;
                    ImGui::PopFont();
                    auto pitch = [&](float gap) { return h + 6 + gap * h + (gap >= 0 ? 2 : 1) * sp; };
                    g_menu_gap = -0.18f;
                    ImGui::SetCursorPosY(285 * s + 3 * pitch(0.35f) - 3 * pitch(g_menu_gap));
                }
                ImGui::SetCursorPosX(x0);
                static bool focused_once;
                if (!focused_once) { ImGui::SetKeyboardFocusHere(); focused_once = true; }
                ImVec2 play_at = ImGui::GetCursorScreenPos();
                bool pressed = menu_item("PLAY", status.empty());
                if (status.empty()) {  // PLAY twinkles now and then while the game is ready
                    ImGui::PushFont(g_font_menu);
                    float w = ImGui::CalcTextSize(tr("PLAY")).x, h = ImGui::CalcTextSize(tr("PLAY")).y;
                    ImGui::PopFont();
                    float t = (float)ImGui::GetTime(), ph = fmodf(t, 3.6f), ph2 = fmodf(t + 0.45f, 3.6f);
                    float k1 = ph < 0.9f ? sinf(ph / 0.9f * 3.14159265f) : 0, k2 = ph2 < 0.7f ? sinf(ph2 / 0.7f * 3.14159265f) : 0;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    sparkle(dl, ImVec2(play_at.x + w + h * 0.05f, play_at.y + h * 0.22f), h * 0.2f, k1, t * 0.6f);
                    sparkle(dl, ImVec2(play_at.x + w - h * 0.32f, play_at.y + h * 0.08f), h * 0.11f, k2 * 0.8f, -t * 0.8f);
                }
                // first PLAY ever: the aspect ratio question, then how to reach our menu, then the game
                if (pressed) {
                    if (!g_settings.aspect_asked) ImGui::OpenPopup("##aspect");
                    else if (!g_settings.menu_tip_seen) ImGui::OpenPopup("##menutip");
                    else play = true;
                }
                if (g_tip_test) ImGui::OpenPopup(g_settings.aspect_asked ? "##menutip" : "##aspect"), g_tip_test = false;
                if (aspect_prompt(s)) {
                    if (g_settings.menu_tip_seen) play = true;
                    else ImGui::OpenPopup("##menutip");
                }
                if (menu_tip(s)) play = true;
                ImGui::SetCursorPosX(x0);
                if (g_refocus == SETTINGS) ImGui::SetKeyboardFocusHere(), g_refocus = MAIN;  // (back from Settings: it
                if (menu_item("SETTINGS", true)) {                                             //  stays selected)
                    static bool opened;
                    menu_tabs_focus(!opened);  // (first time this session: Prelaunch; later: the selected tab)
                    opened = true;
                    page = SETTINGS;
                }
                if (!status.empty() || busy) {  // (once set up, the game files are kept up in Settings > Prelaunch)
                    ImGui::SetCursorPosX(x0);
                    if (g_refocus == SETUP) ImGui::SetKeyboardFocusHere(), g_refocus = MAIN;
                    if (menu_item("SET UP GAME FILES", !busy)) page = SETUP;
                }
                ImGui::SetCursorPosX(x0);
                if (menu_item("QUIT", true)) exit(0);
                g_menu_gap = 0.35f;
            } else if (page == SETTINGS) {
                // the in-game menu's tabs (runtime/ui/menu.mm), in a window like the in-game one; tabs that need a
                // loaded game say so
                float sc = ds.y / 1080.0f, w = fminf(ds.x * 0.9f, 1500 * sc), h = ds.y - 2 * 18 * sc;  // (all the height: a slim margin)
#ifdef __ANDROID__
                w = ds.x * 0.94f;  // (touch: the category rail and full-width rows, menu.mm)
#endif
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ds.x,
                                                          ImGui::GetWindowPos().y + ds.y), IM_COL32(0, 0, 0, 110));
                ImVec2 wp = ImGui::GetWindowPos();
                ImGui::SetCursorScreenPos(ImVec2(wp.x + (ds.x - w) * 0.5f, wp.y + (ds.y - h) * 0.5f));
#ifdef __ANDROID__  // (the Sheikah look, menu.mm tabs_body: its own frame, title and BACK)
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.065f, 0.075f, 0.94f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6 * sc);
#else
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.09f, 0.08f, 0.96f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * sc);
#endif
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 24 * sc));
                ImGui::BeginChild("##settings", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NavFlattened);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                ImGui::PushFont(g_font_small);
                bool back = false;
#ifndef __ANDROID__
                ImGui::PushFont(g_font_menu);
                ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr("Settings"));
                ImGui::PopFont();
                {  // close: a drawn X in the corner (B / Esc too)
                    float bx = 44 * sc;
                    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                    back = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
                    ImGui::PopStyleColor();
                    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
                    ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
                    float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * sc * 1.5f;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * sc);
                    dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
                    dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
                }
#endif
                bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);  // (before the pages run, like the menu)
                menu_tabs_draw(sc, g_font_small, g_font_menu, false, prelaunch_tab);
#ifdef __ANDROID__
                extern bool menu_settings_back_requested();
                back |= menu_settings_back_requested();
#endif
                int cap_dev;
                bind_capture_active(&cap_dev, NULL, NULL);
                if (!popup && !typing && cap_dev < 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    back = true;
                ImGui::PopFont();
                ImGui::EndChild();
                if (back) {
                    bind_capture_cancel();
                    settings_save();
                    page = MAIN, g_refocus = SETTINGS;
                }
            } else {
                // SETUP: the game files, in a window like Settings (option rows: runtime/ui/option_rows.h) - choose the
                // folder with the game files, set up; once set up, delete. Each row's explanation on the right.
                float sc = ds.y / 1080.0f, w = fminf(ds.x * 0.9f, 1500 * sc), h = ds.y - 2 * 18 * sc;
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ds.x,
                                                          ImGui::GetWindowPos().y + ds.y), IM_COL32(0, 0, 0, 110));
                ImVec2 wp = ImGui::GetWindowPos();
                ImGui::SetCursorScreenPos(ImVec2(wp.x + (ds.x - w) * 0.5f, wp.y + (ds.y - h) * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.09f, 0.08f, 0.96f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * sc);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 24 * sc));
                ImGui::BeginChild("##setup", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NavFlattened);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                ImGui::PushFont(g_font_small);
                ImGui::PushFont(g_font_menu);
                ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr("Game Files"));
                ImGui::PopFont();
                bool back = false;
                if (!busy) {  // close: the drawn X in the corner, as Settings (B / Esc too)
                    float bx = 44 * sc;
                    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                    back = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
                    ImGui::PopStyleColor();
                    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
                    ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
                    float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * sc * 1.5f;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * sc);
                    dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
                    dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
                }
                ImGui::Spacing();

                pthread_mutex_lock(&g_lock);
                std::string src = g_src, job = g_job, job_hint = g_job_hint, job_detail = g_job_detail;
                bool failed = g_job_failed;
                pthread_mutex_unlock(&g_lock);
                bool ready = status.empty() && !busy;  // (set up: the window offers to delete them)
                static double files_gb = -1;
                // chosen: "Selected" with a green check (long paths ran over the label); the path is in the
                // explanation, and choosing again opens the picker again
                std::string src_v = src.empty() ? "Choose..." : "Selected";
                auto check_left_of_value = [&](const std::string &v) {
                    if (v != "Selected") return;
                    ImVec2 rb = ImGui::GetItemRectMax(), ra = ImGui::GetItemRectMin();
                    float fs = ImGui::GetFontSize(), vw = ImGui::CalcTextSize(tr(v.c_str())).x;
                    float x = rb.x - 16 * sc - vw - fs * 0.95f, cy = (ra.y + rb.y) * 0.5f, u = fs * 0.62f;
                    ImU32 g = IM_COL32(120, 220, 120, 255);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    dl->AddLine(ImVec2(x, cy), ImVec2(x + u * 0.35f, cy + u * 0.35f), g, fs * 0.11f);
                    dl->AddLine(ImVec2(x + u * 0.35f, cy + u * 0.35f), ImVec2(x + u, cy - u * 0.4f), g, fs * 0.11f);
                };
                std::string src_d = tr("The folder with your own game folders (exefs/romfs, or exefs and a romfs.bin) of The Legend of Zelda: Breath of "
                                       "the Wild for Nintendo Switch: version 1.0.0, 1.6.0, 1.8.2 or 1.9.0 (the last two in "
                                       "Brazilian Portuguese). The version is told from the files, and each one is set up in a place of "
                                       "its own, beside the others. The game isn't included with BOTWRecompAndroid.");
                if (!src.empty()) src_d += std::string(tr("\n\nChosen: ")) + src;
                // the step that's running / its result, under the rows and in the Set up row's explanation
                const std::string &job_line = job;
                bool copied = check_copied_files(g_root).empty();  // (here, the game code to build again: an app update)
                std::string ex_d = tr(native_wanted() ? "Copies the game files BOTWRecompAndroid needs (native game code: "
                                                        "nothing to build). Your own files aren't changed."
                                      : copied ? "The game files are already here: this builds the game code from them again, as "
                                                 "BOTWRecompAndroid needs after an update. It may take a while."
                                               : "Copies the game files BOTWRecompAndroid needs and recompiles the game from them. "
                                                 "This only needs to be done once and may take a while. Your own files aren't changed.");
                if (!job.empty()) ex_d += "\n\n" + std::string(tr(job.c_str())) + (job_hint.empty() ? "" : ".\n" + std::string(tr(job_hint.c_str())));
                if (failed && !job_detail.empty()) ex_d += "\n(" + job_detail + ")";

                // the controller's cursor starts on the first row (not the X), a few frames while the window lays out
                static int seen_frame = -10, focus_first;  // (not drawn last frame: the window has just opened)
                if (seen_frame != ImGui::GetFrameCount() - 1) focus_first = 4;
                seen_frame = ImGui::GetFrameCount();
                opt_begin("##files", sc);
                version_rows(busy, focus_first > 0 && focus_first--);
                // the folder: this version's files when they aren't set up, else another version's (or this one's again)
                opt_section("Your copy of the game");
#ifdef __ANDROID__
                {   // how the game's code runs - chosen before the folder (picking it sets the game up at once): built
                    // from the files (recompiled) or the game's own on the CPU (native: no build; runtime/native.c)
                    static const char *const modes[] = {"Recompiled", "Native (experimental)"};
                    int m = g_settings.native_code ? 1 : 0;
                    if (busy) opt_disable_next();
                    opt_choice("Game code", &m, modes, 2, 0,
                               "Recompiled: the setup rebuilds the game's code for this phone from your files (once, it may "
                               "take a while). Set up this way, the game can still run natively: Video > Native game code.\n\n"
                               "Native: the game's own code runs directly on the phone's CPU, so the setup only copies the "
                               "files - no build. An experiment. Switching to Recompiled later means building the game code "
                               "then.");
                    if ((m == 1) != g_settings.native_code) {
                        g_settings.native_code = m == 1;
                        settings_save();
                        status = check_game_files(g_root);
                    }
                }
                {   // copied, or read where they are (the romfs/: services/fs.c romfs.link; the exefs/ is copied, small)
                    static const char *const ways[] = {"Copy them", "Use them in place"};
                    int w = g_settings.files_in_place ? 1 : 0;
                    if (busy) opt_disable_next();
                    opt_choice("Game files", &w, ways, 2, 0,
                               "Copy them: the setup copies the game data (about 16 GB) into BOTWRecompAndroid's folder - "
                               "your folder can be moved or deleted afterwards.\n\n"
                               "Use them in place: the game reads its data from your folder, no copy - it saves the space "
                               "and the wait. It needs the permission to access all files (Android asks), and the folder "
                               "must stay where it is: moved or deleted, the game files have to be set up again.");
                    if ((w == 1) != g_settings.files_in_place) {
                        g_settings.files_in_place = w == 1;
                        settings_save();
                    }
                }
#endif
                if (busy) opt_disable_next();
#ifdef __ANDROID__
                if (opt_action("Game files folder", src_v.c_str(), src_d.c_str(), true))
                    android_call(g_settings.files_in_place ? "pickGameFolderInPlace" : "pickGameFolder");
#else
                if (opt_action("Game files folder", src_v.c_str(), src_d.c_str(), true))
                    SDL_ShowOpenFolderDialog(picked, NULL, win, NULL, false);
#endif
                check_left_of_value(src_v);
                if (!ready || !src.empty()) {  // (Android: picking the folder sets it up at once)
                    opt_section("Set up");
                    if (busy || (src.empty() && !copied)) opt_disable_next();
                    if (opt_action("Set up game files", busy ? "Working..." : "", ex_d.c_str(), true)) {
                        pthread_mutex_lock(&g_lock);
                        g_busy = true;
                        pthread_mutex_unlock(&g_lock);
                        pthread_t t;
                        pthread_create(&t, NULL, extract_thread, NULL);
                        pthread_detach(t);
                    }
                    files_gb = -1;  // (re-measured after a setup)
                }
                if (ready) {
                    // DELETE: frees BOTWRecompAndroid's copy (runtime/paths.c removes only what the setup made, only inside
                    // the data root). Saves, settings and the user's own files stay.
                    if (files_gb < 0) files_gb = rt_game_files_size(g_root.c_str()) / 1e9;
                    char gb[32];
                    snprintf(gb, sizeof gb, "%.1f GB", files_gb);
                    opt_section("Game files");
                    if (!g_confirm_delete) {
                        if (opt_action("Delete game files", gb, "Frees the space BOTWRecompAndroid's copy of the game files takes. "
                                       "Saves and settings are kept, and your own game files aren't touched: you can "
                                       "set the game files up again from them.", true))
                            g_confirm_delete = true;
                    } else {
                        if (opt_action("Yes, delete them", gb, "Deletes the extracted game files now.", true)) {
                            bool ok = rt_remove_game_files(g_root.c_str());
                            status = check_game_files(g_root);
                            if (ok) set_job("Game files deleted.");
                            else set_job("Couldn't delete the game files", true, "They aren't in BOTWRecompAndroid's data folder, "
                                         "so they were left alone.");
                            if (!status.empty()) g_bg = 0;
                            g_confirm_delete = false, files_gb = -1;
                        }
                        if (opt_action("Cancel", "", "Keeps the game files.", true)) g_confirm_delete = false;
                    }
                }
                if (!job_line.empty()) {  // the current step / result
                    ImGui::Dummy(ImVec2(0, 14 * sc));
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextColored(failed ? ImVec4(1, 0.5f, 0.4f, 1) : busy ? ImVec4(0.93f, 0.76f, 0.33f, 1)
                                              : ImVec4(0.45f, 0.95f, 0.55f, 1), "%s", tr(job_line.c_str()));
                    if (failed && !job_hint.empty())  // (what to do next, in the rows' own text colour)
                        ImGui::TextColored(ImVec4(0.86f, 0.85f, 0.80f, 1), "%s", tr(job_hint.c_str()));
                    if (failed && !job_detail.empty()) ImGui::TextDisabled("(%s)", job_detail.c_str());  // (its cause)
                    ImGui::PopTextWrapPos();
                }
                opt_end();
                if (g_confirm_delete && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                         ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    g_confirm_delete = false;  // (B / Esc: back out of the question first)
                else if (!busy && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                              ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    back = true;
                ImGui::PopFont();
                ImGui::EndChild();
                if (back && !busy) {
                    settings_save();
                    g_confirm_delete = false;
                    page = MAIN, g_refocus = SETUP;
                }
                static bool test_busy = getenv("LARECOMP_LAUNCHER_SHOT") && strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":extracting");
                if (busy || test_busy) {  // extracting: everything dimmed, a progress bar in front (input waits)
                    ImDrawList *fg = ImGui::GetForegroundDrawList();
                    ImVec2 o = ImGui::GetMainViewport()->Pos;
                    fg->AddRectFilled(o, ImVec2(o.x + ds.x, o.y + ds.y), IM_COL32(0, 0, 0, 170));
                    float f = test_busy ? 0.42f : extract_progress();
                    // (while the game code is built: what's happening, on whose files - runtime/gamebuild.c)
                    bool recompiling = g_build_frac >= 0;
                    const char *title = tr(recompiling ? "Recompiling the game" : "Setting up the game files");
                    const char *const notes[] = {tr("This only needs to be done once and may take a while.")};
                    int nnotes = recompiling ? 1 : 0;
                    float tfs = g_font_menu->FontSize * 0.75f, sfs = g_font_small->FontSize * 0.85f;
                    ImVec2 ts = g_font_menu->CalcTextSizeA(tfs, FLT_MAX, 0, title);
                    float line_h = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, "A").y;
                    float pw = fminf(ds.x * 0.6f, 900 * sc), bar_y = 36 * sc + ts.y + 30 * sc;
                    for (int i = 0; i < nnotes; i++)  // (wide enough for its text, within the window)
                        pw = fmaxf(pw, g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, notes[i]).x + 96 * sc);
                    pw = fminf(fmaxf(pw, ts.x + 96 * sc), ds.x - 32 * sc);
                    float ph = bar_y + 24 * sc + 26 * sc + line_h + (nnotes ? 14 * sc + nnotes * line_h : 0) + 40 * sc;
                    ImVec2 a(o.x + (ds.x - pw) * 0.5f, o.y + (ds.y - ph) * 0.5f), b(a.x + pw, a.y + ph);
                    fg->AddRectFilled(a, b, IM_COL32(15, 23, 20, 250), 14 * sc);
                    fg->AddRect(a, b, IM_COL32(236, 194, 84, 140), 14 * sc, 0, 1.5f * sc);
                    fg->AddText(g_font_menu, tfs, ImVec2((a.x + b.x - ts.x) * 0.5f, a.y + 36 * sc), IM_COL32(236, 194, 84, 255), title);
                    ImVec2 ba(a.x + 48 * sc, a.y + bar_y), bb(b.x - 48 * sc, a.y + bar_y + 24 * sc);
                    fg->AddRectFilled(ba, bb, IM_COL32(255, 255, 255, 25), 12 * sc);
                    if (f > 0) fg->AddRectFilled(ba, ImVec2(ba.x + (bb.x - ba.x) * f, bb.y), IM_COL32(236, 194, 84, 255), 12 * sc);
                    char line[200];
                    snprintf(line, sizeof line, "%s   %d%%", tr(test_busy ? "Setting up the game files..." : job.c_str()),
                             (int)(f * 100));
                    ImVec2 ls = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, line);
                    fg->AddText(g_font_small, sfs, ImVec2((a.x + b.x - ls.x) * 0.5f, bb.y + 26 * sc), IM_COL32(220, 216, 205, 255), line);
                    for (int i = 0; i < nnotes; i++) {
                        ImVec2 ns = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, notes[i]);
                        fg->AddText(g_font_small, sfs, ImVec2((a.x + b.x - ns.x) * 0.5f, bb.y + 26 * sc + (i + 1) * line_h + 14 * sc),
                                    IM_COL32(150, 148, 140, 255), notes[i]);
                    }
                }
            }

            if (g_switching_to) {  // (Android) starting again in another game version: everything dimmed, saying so
                ImDrawList *fg = ImGui::GetForegroundDrawList();
                ImVec2 o = ImGui::GetMainViewport()->Pos;
                fg->AddRectFilled(o, ImVec2(o.x + ds.x, o.y + ds.y), IM_COL32(0, 0, 0, 170));
                std::string t = std::string(tr("Starting version")) + " " + version_name(g_switching_to) + "...";
                float tfs = g_font_menu->FontSize * 0.75f;
                ImVec2 ts = g_font_menu->CalcTextSizeA(tfs, FLT_MAX, 0, t.c_str());
                fg->AddText(g_font_menu, tfs, ImVec2(o.x + (ds.x - ts.x) * 0.5f, o.y + (ds.y - ts.y) * 0.5f),
                            IM_COL32(236, 194, 84, 255), t.c_str());
            }

            // status (lower left) and version (lower right) - not under the Settings / Game Files windows
            if (page == MAIN) {
            ImGui::PushFont(g_font_small);
            ImGui::SetCursorPos(ImVec2(x0, ds.y - 118 * s));
            if (status.empty()) {
                {  // a drawn check mark (the Switch font has no ✓)
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float u = ImGui::GetTextLineHeight();
                    ImU32 g = IM_COL32(140, 230, 140, 255);
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + u * 0.1f, p.y + u * 0.55f), ImVec2(p.x + u * 0.35f, p.y + u * 0.8f), g, u * 0.12f);
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + u * 0.35f, p.y + u * 0.8f), ImVec2(p.x + u * 0.85f, p.y + u * 0.2f), g, u * 0.12f);
                    ImGui::SetCursorPosX(x0 + u * 1.3f);
                }
                ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1), "%s", tr("Game files ready."));
                ImGui::SetCursorPosX(x0);
                ImGui::TextColored(ImVec4(0.66f, 0.76f, 0.98f, 1), "Nintendo Switch  \xc2\xb7  v%s", version_name(rt_game_version()));
            } else {
                ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "!  %s", tr(busy ? "Setting up the game files..." : status.c_str()));
                ImGui::SetCursorPosX(x0);
                ImGui::TextDisabled("%s", tr("Set up the game files to play."));
            }
            if (const char *f = getenv("LARECOMP_LAUNCHER_FONT")) {  // (test) which typeface this window shows
                const char *b = strrchr(f, '/');
                ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetWindowPos().x + 20 * s, ImGui::GetWindowPos().y + 16 * s),
                                                    IM_COL32(255, 255, 255, 255), b ? b + 1 : f);
            }
            {  // version, and a newer release when the update check found one (lower right). The background is
               // always the title movie's still: bright clouds here, under a shadow banner from the right
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImFont *font = ImGui::GetFont();
                float fs = ImGui::GetFontSize(), right = ImGui::GetWindowPos().x + ds.x - 60 * s;
                auto text = [&](ImVec2 p, float size, ImU32 col, const char *t, bool bold) {
                    for (float d : {2.5f, 1.5f, 0.8f}) dl->AddText(font, size, ImVec2(p.x + d * s, p.y + d * s), IM_COL32(4, 8, 30, 150), t);
                    dl->AddText(font, size, p, col, t);
                    if (bold) dl->AddText(font, size, ImVec2(p.x + 0.7f * s, p.y), col, t);
                };
#ifdef __ANDROID__
                std::string ver = app_version();  // this app's versionName, not the PC version's tag
                if (!ver.empty()) ver = tag_label(ver, true);  // "Version 0.2.0-beta", as the PC version shows its own
#else
                std::string ver = tag_label(LARECOMP_VERSION, true);
#endif
                float vs = fs * 1.4f, y = ImGui::GetWindowPos().y + ds.y - 134 * s;
                ImVec2 vsz = font->CalcTextSizeA(vs, FLT_MAX, 0, ver.c_str());
                text(ImVec2(right - vsz.x, y), vs, IM_COL32(255, 255, 255, 255), ver.c_str(), false);
                pthread_mutex_lock(&g_update_lock);
                std::string upd = g_update_tag;
                pthread_mutex_unlock(&g_update_lock);
                if (!upd.empty()) {
                    const char *arrow = font->FindGlyphNoFallback(0x2192) ? " \xe2\x86\x92" : " \xe2\x96\xb8";
                    std::string pre = "Update available!  ", link = "DOWNLOAD " + tag_label(upd, false) + arrow;
                    float wp = font->CalcTextSizeA(fs, FLT_MAX, 0, pre.c_str()).x;
                    ImVec2 lsz = font->CalcTextSizeA(fs, FLT_MAX, 0, link.c_str());
                    ImVec2 lp(right - lsz.x, y + vsz.y + 2 * s);
                    text(ImVec2(lp.x - wp, lp.y), fs, IM_COL32(200, 214, 250, 255), pre.c_str(), false);
                    ImGui::SetCursorScreenPos(lp);
                    bool click = ImGui::InvisibleButton("update", lsz);
                    bool hover = ImGui::IsItemHovered();
                    // the menu's hover colours (yellow to orange), lighter while the link itself is hovered
                    ImU32 col = hover ? IM_COL32(255, 238, 150, 255) : IM_COL32(255, 214, 64, 255);
                    ImU32 col_b = hover ? IM_COL32(255, 196, 110, 255) : IM_COL32(255, 160, 50, 255);
                    text_fx(dl, font, fs, lp, col, col_b, link.c_str(), 1.5f * s);
                    text_fx(dl, font, fs, ImVec2(lp.x + 0.7f * s, lp.y), col, col_b, link.c_str(), 0);  // (faux bold)
                    if (hover) {
                        dl->AddLine(ImVec2(lp.x, lp.y + lsz.y), ImVec2(lp.x + lsz.x, lp.y + lsz.y), col, 1.5f * s);
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                    if (click) SDL_OpenURL((RELEASE_PAGE + upd).c_str());
                }
            }
            ImGui::PopFont();
            }
            ImGui::End();
            ImGui::PopStyleColor();
            osk_draw(g_font_menu, s * 0.7f);  // (a text field activated with the controller)
            typed_before = ui_text_busy();

            ImGui::Render();
#ifdef __APPLE__
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
            [enc endEncoding];
            id<MTLTexture> shot = nil;
            if (shot_at && ++frame == shot_at) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drawable.texture.pixelFormat
                                             width:drawable.texture.width height:drawable.texture.height mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                shot = [dev newTextureWithDescriptor:td];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:drawable.texture toTexture:shot];
                [bl endEncoding];
            }
            [cb presentDrawable:drawable];
            [cb commit];
            if (shot) {
                [cb waitUntilCompleted];
                save_png(shot, getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/launcher.png");
                exit(0);
            }
#else
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
            bool shot = shot_at && ++frame == shot_at;
            vk_frame_end(shot ? getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/launcher.png" : NULL);
            if (shot) exit(0);
#endif
        }
    }
    // play: hand the window to the game
    settings_save();
#ifdef __APPLE__
    ImGui_ImplMetal_Shutdown();
#else
    vk_imgui_backend_shutdown();
#endif
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev_ctx);
    g_bg = 0;
    g_start_screen = 0;
}
