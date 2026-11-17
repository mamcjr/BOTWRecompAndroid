// The built-in benchmark, Android and game version 1.9.0 only. LARECOMP_BENCHMARK=<scene file>: each scene runs from a
// scratch copy of the player's save (position, time, Blood Moon and hearts edited in the copy, then a reboot in place and
// the game's own load), holds its time and weather, turns the camera to its angles with the stick, warms up, measures,
// and <scene file>.out/ gets report.json, report.csv and frames.csv. Without the variable none of this runs.
// tools/benchmark-scenes.txt is a set to start from.
//
// The scene file:
//   format = 1
//   repeats = 3
//   warmup_s = 20       (at least)
//   calm_s = 20         (and until this long without a shader compile or a draw skipped for one: past the load's burst,
//                       whose gaps were up to 12 s; later ones come by ones and twos, a minute or more apart...)
//   warmup_max_s = 180  (... or this long, whichever comes first)
//   window_s = 45
//   [scene]
//   name = plateau
//   pos = -1038 251 1815
//   time = 180
//   weather = 0
//   yaw = 40
//   pitch = 0
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "../benchmark_perf.h"
#include "../hle.h"
#include "../framepace.h"
#include "../rt.h"
#include "../settings.h"
#include "benchmark_commit.h"

#define GA(a) ((uint64_t)(a) - 0x7000000000ull + GUEST_LO)
#define RD8(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define RD32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define RD64(a) (*(volatile uint64_t *)(uintptr_t)(a))
#define RDF(a) (*(volatile float *)(uintptr_t)(a))

// 1.9.0's globals and fields, found on the device (step 11a)
#define WORLD_MGR 0x7001e57580ull     // ksys::world::Manager
#define W_WEATHER 0x7d1               // mWeatherType (with the timer and the flags beside it, setWeatherType's writes)
#define W_WEATHER_TIMER 0x794
#define W_WEATHER_7D5 0x7d5
#define W_WEATHER_DEMO 0x7d6
#define W_MAIN_FIELD 0x7d9
#define W_MGRS 0x5c0                  // its sub-managers: {u32 n, cap; ptr *}, the first is the TimeMgr
#define TM_TIME 0xb8                  // mTime, 0..360; mNewTime (-1: none) and mTimeStep after it
#define TM_NEW_TIME 0xc0
#define TM_STEP 0xc4
#define PLAYER_INFO 0x7001e13ed0ull   // uking::act::PlayerInfo
#define PI_POS 0x88
#define CAMERA_MGR 0x7001e39d10ull    // its look-at camera: +0x28+0xd0 an index at +0xc, pointers from +0x10
#define CAM_POS 0x38
#define CAM_AT 0x44

// Flags in game_data.sav: a 12-byte header, then pairs of crc32(name) and value
#define H_POS 0xa40ba103u   // PlayerSavePos (three pairs)
#define H_TIME 0x9c79a188u  // WM_Time
#define H_BTIMER 0x4234e199u  // WM_BloodyMoonTimer
#define H_BDAY 0xcda06bceu    // WM_BloodyDay
#define H_HART 0xbe9bc993u    // CurrentHart
#define H_MAXHART 0x2906f327u // MaxHartValue

#define MAX_SCENES 16
#define MAX_REPEATS 10
#define TIMEOUT_LOAD_S 240
#define TIMEOUT_ARRIVE_S 15
#define TIMEOUT_CAMERA_S 30
#define TIMEOUT_EVENT_S 30
#define SETTLE_S 3             // after the load, before the stick: the camera finishes its own move
#define ARRIVE_TOLERANCE 1.0   // units in X and Z (the probe's error: 0.02)
#define MOVE_TOLERANCE 3.0     // in the window: Link stands still
#define CAM_TOLERANCE 0.5      // degrees, the stick stops there
#define CAM_DRIFT 2.0          // degrees, the window's end against the target
#define TIME_DRIFT 0.01        // of the held time, at the window's end
#define TARGET_FPS 30

bool g_benchmark;

typedef struct { char name[32]; float pos[3], time, yaw, pitch; int weather; } Scene;
typedef struct { double p50, p95, p99; } Pct;
typedef struct { float batt_temp, batt_pct, watts, soc_temp, headroom; int thermal, plugged; } Power;
typedef struct {
    int scene, repeat;
    bool ok;
    char step[24], why[120];
    double load_s, camera_s, warmup_s, fps, low1, low01, hz_start, hz_end, cam_err, moved;
    Pct frame, gpu, cpu, game, render;
    BenchStats trace;
    double cooling_s;
    int thermal_before, thermal_after;
    int frames, over50;
    uint64_t skipped, compiles, skipped_building;
    Power start, end;
} Result;

static Scene scenes[MAX_SCENES];
static int nscenes, repeats = 1, warmup_s = 20, calm_s = 20, warmup_max_s = 180, window_s = 45;
static int thermal_max = -1, thermal_wait_s = 300;
static double cooling_s;
static uint64_t cool_poll;
static int thermal_before = -1, thermal_after = -1;
static bool previous_long;
static char out_dir[1024], source[1024];
static Result results[MAX_SCENES * MAX_REPEATS];
static int nresults;
static int cur_scene, cur_repeat;
static const char *cur_step = "start";
static const char *cache_at_start = "unknown";  // cold or warm: SPIR-V cache misses before the first window
static float (*win)[5];  // period, busy, game, render, GPU: ms a frame (-1: not known, a frame too long for the pacer)
static int win_cap;
static volatile int win_n;
static volatile bool win_on;

// ------------------------------------------------------------------ the report
static double median(double *v, int n) {
    for (int i = 1; i < n; i++) {
        double x = v[i];
        int j = i;
        for (; j > 0 && v[j - 1] > x; j--) v[j] = v[j - 1];
        v[j] = x;
    }
    return n ? (n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2) : 0;
}

static void json_str(FILE *f, const char *key, const char *v, const char *end) {
    fprintf(f, "\"%s\":\"", key);
    for (; v && *v; v++) {
        if (*v == '"' || *v == '\\') fputc('\\', f);
        if ((unsigned char)*v >= 0x20) fputc(*v, f);
    }
    fprintf(f, "\"%s", end);
}

static void read_power(Power *p) {
    PlatDeviceStats s;
    plat_device_stats(&s);
    *p = (Power){s.batt_temp_c, s.batt_pct, s.watts, s.soc_temp_c, s.headroom, s.thermal, s.plugged};
}

static void json_power(FILE *f, const char *key, const Power *p, const char *end) {
    fprintf(f, "\"%s\":{\"battery_c\":%.1f,\"battery_pct\":%.0f,\"watts\":%.2f,\"soc_c\":%.1f,\"thermal\":%d,\"headroom\":%.2f,\"plugged\":%d}%s",
            key, p->batt_temp, p->batt_pct, p->watts, p->soc_temp, p->thermal, p->headroom, p->plugged, end);
}

static void json_pct(FILE *f, const char *key, const Pct *p) {
    fprintf(f, "\"%s\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f},", key, p->p50, p->p95, p->p99);
}

static const char *prop(const char *name) {
    static char v[8][PROP_VALUE_MAX];
    static int i;
    char *s = v[i++ & 7];
    s[0] = 0;
    __system_property_get(name, s);
    return s;
}

// What ran: no serial numbers, no build fingerprint
static void json_passport(FILE *f) {
    extern const char *vk_gpu_desc(void), *vk_driver_desc(void), *launcher_app_version(void);
    static Power power;
    static int spirv = -1;
    static long pipelines = -1;
    if (spirv < 0) {
        read_power(&power);
        spirv = 0;
        DIR *d = opendir("cache/spirv");
        for (struct dirent *e; d && (e = readdir(d));) spirv += e->d_name[0] != '.';
        if (d) closedir(d);
        struct stat sb;
        pipelines = stat("cache/vk_pipelines.bin", &sb) ? 0 : (long)sb.st_size;
    }
    fprintf(f, "\"passport\":{");
    json_str(f, "gpu", vk_gpu_desc(), ",");
    json_str(f, "driver", vk_driver_desc(), ",");
    json_str(f, "android", prop("ro.build.version.release"), ",");
    json_str(f, "android_api", prop("ro.build.version.sdk"), ",");
    json_str(f, "model", prop("ro.product.model"), ",");
    json_str(f, "soc", prop("ro.soc.model"), ",");
    json_str(f, "apk", launcher_app_version(), ",");
    json_str(f, "commit", BENCHMARK_COMMIT, ",");  // (benchmark_commit.h: written on every build)
    json_str(f, "game_version", "1.9.0", ",");
    json_str(f, "game_code", native_on() ? "native" : "recompiled", ",");
    fprintf(f, "\"settings\":{\"fps_mode\":%d,\"res_mode\":%d,\"upscaler\":%d,\"out_lines\":%d,\"antialias\":%d,\"aspect\":%d,"
               "\"sync_lag\":%d,\"event_fps_unlock\":%d,\"touch_controls\":%d,\"show_fps\":%d},",
            g_settings.fps_mode, g_settings.res_mode, g_settings.upscaler, g_settings.out_lines, g_settings.antialias, g_settings.aspect,
            g_settings.sync_lag, g_settings.event_fps_unlock, g_settings.touch_controls, g_settings.show_fps);
    json_str(f, "shader_cache_at_start", cache_at_start, ",");
    fprintf(f, "\"cache\":{\"spirv_files\":%d,\"pipeline_bytes\":%ld},", spirv, pipelines);
    json_power(f, "power", &power, ",");
    fprintf(f, "\"pass\":\"cap30\"},\n");
}

static void write_report(const char *status) {
    char path[1200], tmp[1200];
    snprintf(tmp, sizeof tmp, "%s/report.json.tmp", out_dir);
    FILE *f = fopen(tmp, "w");
    if (!f) { LOG("[benchmark] can't write %s (errno %d)\n", tmp, errno); return; }
    fprintf(f, "{\"report_format\":2,\"scene_format\":1,");
    json_str(f, "status", status, ",");
    if (nscenes && !strcmp(status, "running")) {
        fprintf(f, "\"at\":{\"scene\":\"%s\",\"repeat\":%d,", scenes[cur_scene].name, cur_repeat);
        json_str(f, "step", cur_step, "},");
    }
    fprintf(f, "\"repeats\":%d,\"warmup_s\":%d,\"calm_s\":%d,\"warmup_max_s\":%d,\"window_s\":%d,\n", repeats, warmup_s, calm_s, warmup_max_s, window_s);
    fprintf(f, "\"thermal_max\":%d,\"thermal_wait_s\":%d,", thermal_max, thermal_wait_s);
    if (g_game_version == 190) json_passport(f);
    fprintf(f, "\"results\":[\n");
    for (int i = 0; i < nresults; i++) {
        const Result *r = &results[i];
        fprintf(f, "{\"scene\":\"%s\",\"repeat\":%d,", scenes[r->scene].name, r->repeat);
        json_str(f, "status", r->ok ? "ok" : "harness_error", ",");
        fprintf(f, "\"cooling_s\":%.3f,\"thermal_before\":%d,\"thermal_after\":%d,", r->cooling_s, r->thermal_before, r->thermal_after);
        if (!r->ok) {
            json_str(f, "step", r->step, ",");
            json_str(f, "reason", r->why, r->frames ? "," : i + 1 < nresults ? "},\n" : "}\n");
            if (!r->frames) continue;
        }
        fprintf(f, "\"load_s\":%.1f,\"camera_s\":%.1f,\"warmup_s\":%.1f,\"frames\":%d,\"fps\":%.2f,\"low_1pct_fps\":%.2f,\"low_01pct_fps\":%.2f,\"over_50ms\":%d,",
                r->load_s, r->camera_s, r->warmup_s, r->frames, r->fps, r->low1, r->low01, r->over50);
        bench_trace_json(f, &r->trace);
        json_pct(f, "game_period_ms", &r->frame);
        json_pct(f, "frame_ms", &r->frame);
        json_pct(f, "gpu_ms", &r->gpu);
        json_pct(f, "cpu_ms", &r->cpu);
        json_pct(f, "game_thread_ms", &r->game);
        json_pct(f, "render_thread_ms", &r->render);
        fprintf(f, "\"skipped_draws\":%llu,\"skipped_building\":%llu,\"shader_compiles\":%llu,\"display_hz_start\":%.1f,\"display_hz_end\":%.1f,\"camera_err_deg\":%.2f,\"moved\":%.2f,",
                (unsigned long long)r->skipped, (unsigned long long)r->skipped_building, (unsigned long long)r->compiles, r->hz_start, r->hz_end, r->cam_err, r->moved);
        json_power(f, "power_start", &r->start, ",");
        json_power(f, "power_end", &r->end, "");
        fprintf(f, "}%s\n", i + 1 < nresults ? "," : "");
    }
    fprintf(f, "],\n\"summary\":[\n");
    for (int s = 0; s < nscenes; s++) {  // (the repeats that ran to their end: median, and the spread as a share of it)
        double gpu[MAX_REPEATS], frame[MAX_REPEATS], lo = 0, hi = 0;
        int n = 0;
        for (int i = 0; i < nresults; i++) {
            if (results[i].scene != s || !results[i].ok) continue;
            double g = results[i].gpu.p50;
            lo = n && lo < g ? lo : g, hi = n && hi > g ? hi : g;
            frame[n] = results[i].frame.p50;
            gpu[n++] = g;
        }
        double m = median(gpu, n);
        fprintf(f, "{\"scene\":\"%s\",\"ok\":%d,\"gpu_p50_median\":%.3f,\"gpu_p50_spread_pct\":%.2f,\"frame_p50_median\":%.3f}%s\n",
                scenes[s].name, n, m, m > 0 ? (hi - lo) / m * 100 : 0, median(frame, n), s + 1 < nscenes ? "," : "");
    }
    fprintf(f, "]}\n");
    fclose(f);
    snprintf(path, sizeof path, "%s/report.json", out_dir);
    rename(tmp, path);

    snprintf(tmp, sizeof tmp, "%s/report.csv.tmp", out_dir);
    if (!(f = fopen(tmp, "w"))) return;
    fprintf(f, "scene,repeat,status,step,frames,fps,frame_p50,frame_p95,frame_p99,low_1pct_fps,low_01pct_fps,over_50ms,gpu_p50,gpu_p95,gpu_p99,"
               "cpu_p50,cpu_p95,cpu_p99,skipped_draws,skipped_building,shader_compiles,warmup_s,display_hz,thermal_start,thermal_end\n");
    for (int i = 0; i < nresults; i++) {
        const Result *r = &results[i];
        fprintf(f, "%s,%d,%s,%s,%d,%.2f,%.3f,%.3f,%.3f,%.2f,%.2f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%.1f,%.1f,%d,%d\n",
                scenes[r->scene].name, r->repeat, r->ok ? "ok" : "harness_error", r->ok ? "" : r->step, r->frames, r->fps, r->frame.p50, r->frame.p95,
                r->frame.p99, r->low1, r->low01, r->over50, r->gpu.p50, r->gpu.p95, r->gpu.p99, r->cpu.p50, r->cpu.p95, r->cpu.p99,
                (unsigned long long)r->skipped, (unsigned long long)r->skipped_building, (unsigned long long)r->compiles, r->warmup_s, r->hz_end, r->start.thermal, r->end.thermal);
    }
    fclose(f);
    snprintf(path, sizeof path, "%s/report.csv", out_dir);
    rename(tmp, path);
}

// ------------------------------------------------------------------ the scene file
static bool parse_scenes(const char *path, char *err, size_t nerr) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, nerr, "can't read the scene file %s (errno %d)", path, errno); return false; }
    char line[256];
    int format = 0, have = 0;
    enum { HAVE_NAME = 1, HAVE_POS = 2, HAVE_TIME = 4, HAVE_WEATHER = 8, HAVE_YAW = 16, HAVE_PITCH = 32, HAVE_ALL = 63 };
    Scene *s = NULL;
    bool ok = true;
    for (int n = 1; ok && fgets(line, sizeof line, f); n++) {
        char *c = strchr(line, '#');
        if (c) *c = 0;
        char key[32], val[128];
        val[0] = 0;
        if (sscanf(line, " [%31[a-z]]", key) == 1 && !strcmp(key, "scene")) {
            if (s && have != HAVE_ALL) { snprintf(err, nerr, "scene %s: a key is missing (line %d)", s->name, n); ok = false; break; }
            if (nscenes == MAX_SCENES) { snprintf(err, nerr, "more than %d scenes", MAX_SCENES); ok = false; break; }
            s = &scenes[nscenes++];
            have = 0;
            continue;
        }
        if (sscanf(line, " %31[a-z_0-9] = %127[^\r\n]", key, val) != 2) {
            char blank[2];
            if (sscanf(line, " %1s", blank) == 1) { snprintf(err, nerr, "line %d: not key = value", n); ok = false; }
            continue;
        }
        for (size_t e = strlen(val); e && val[e - 1] == ' '; e--) val[e - 1] = 0;
        if (!s) {
            if (!strcmp(key, "format")) format = atoi(val);
            else if (!strcmp(key, "repeats")) repeats = atoi(val);
            else if (!strcmp(key, "warmup_s")) warmup_s = atoi(val);
            else if (!strcmp(key, "calm_s")) calm_s = atoi(val);
            else if (!strcmp(key, "warmup_max_s")) warmup_max_s = atoi(val);
            else if (!strcmp(key, "thermal_max") || !strcmp(key, "thermal_wait_s")) {
                char *end; errno = 0; long value = strtol(val, &end, 10);
                if (errno || *end || value < -1 || value > 3600) { snprintf(err, nerr, "line %d: bad %s", n, key); ok = false; }
                else if (!strcmp(key, "thermal_max")) thermal_max = (int)value;
                else thermal_wait_s = (int)value;
            }
            else if (!strcmp(key, "window_s")) window_s = atoi(val);
            else { snprintf(err, nerr, "line %d: unknown key %s", n, key); ok = false; }
            continue;
        }
        if (!strcmp(key, "name")) {
            ok = sscanf(val, "%31[A-Za-z0-9_-]", s->name) == 1 && !strcmp(s->name, val);
            have |= HAVE_NAME;
        } else if (!strcmp(key, "pos")) {
            ok = sscanf(val, "%f %f %f", &s->pos[0], &s->pos[1], &s->pos[2]) == 3 && isfinite(s->pos[0]) && isfinite(s->pos[1]) && isfinite(s->pos[2]);
            have |= HAVE_POS;
        } else if (!strcmp(key, "time")) {
            s->time = (float)atof(val);
            ok = s->time >= 0 && s->time < 360;
            have |= HAVE_TIME;
        } else if (!strcmp(key, "weather")) {
            s->weather = atoi(val);
            ok = s->weather >= 0 && s->weather < 9;
            have |= HAVE_WEATHER;
        } else if (!strcmp(key, "yaw")) {
            s->yaw = (float)atof(val);
            ok = fabsf(s->yaw) <= 360;
            have |= HAVE_YAW;
        } else if (!strcmp(key, "pitch")) {
            s->pitch = (float)atof(val);
            ok = fabsf(s->pitch) <= 360;
            have |= HAVE_PITCH;
        } else {
            ok = false;
        }
        if (!ok) snprintf(err, nerr, "line %d: bad %s", n, key);
    }
    fclose(f);
    if (!ok) return false;
    if (format != 1) { snprintf(err, nerr, "format %d: only format = 1 is known", format); return false; }
    if (!nscenes || have != HAVE_ALL) { snprintf(err, nerr, "no scenes, or the last one has a key missing"); return false; }
    if (repeats < 1 || repeats > MAX_REPEATS || warmup_s < 0 || warmup_s > 600 || calm_s < 0 || calm_s > 600 || warmup_max_s < warmup_s ||
        warmup_max_s > 1800 || window_s < 5 || window_s > 600) {
        snprintf(err, nerr, "repeats 1-%d, warmup_s and calm_s 0-600, warmup_max_s up to 1800 and not under warmup_s, window_s 5-600", MAX_REPEATS);
        return false;
    }
    if (thermal_max < -1 || thermal_max > 6 || thermal_wait_s < 1 || thermal_wait_s > 3600) {
        snprintf(err, nerr, "thermal_max -1 (disabled) or 0-6; thermal_wait_s 1-3600"); return false;
    }
    return true;
}

// ------------------------------------------------------------------ the scratch save
extern void copy_tree(const char *src, const char *dst);  // services/fs.c: this only reads the player's save

static bool set_flag(uint8_t *d, size_t n, uint32_t hash, const uint32_t *v, int count) {
    int found = 0;
    for (size_t o = 12; o + 8 <= n && found < count; o += 8)
        if (*(uint32_t *)(d + o) == hash) *(uint32_t *)(d + o + 4) = v[found++];
    return found == count;
}

static bool get_flag(const uint8_t *d, size_t n, uint32_t hash, uint32_t *v) {
    for (size_t o = 12; o + 8 <= n; o += 8)
        if (*(const uint32_t *)(d + o) == hash) { *v = *(const uint32_t *)(d + o + 4); return true; }
    return false;
}

// Every slot of the copy gets the scene (the game continues from whichever is the latest); a flag not found: nothing to run
static bool edit_slot(const char *path, const Scene *s, char *why, size_t nwhy) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, nwhy, "can't read the copy of game_data.sav"); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 12 ? malloc((size_t)n) : NULL;
    bool ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    uint32_t zero = 0, hart = 0, pos[3], time;
    memcpy(pos, s->pos, sizeof pos);
    memcpy(&time, &s->time, 4);
    ok = ok && set_flag(d, (size_t)n, H_POS, pos, 3) && set_flag(d, (size_t)n, H_TIME, &time, 1) &&
         set_flag(d, (size_t)n, H_BTIMER, &zero, 1) && set_flag(d, (size_t)n, H_BDAY, &zero, 1) &&
         get_flag(d, (size_t)n, H_MAXHART, &hart) && set_flag(d, (size_t)n, H_HART, &hart, 1);
    if (!ok) snprintf(why, nwhy, "a flag isn't in game_data.sav: not a save of this game's format");
    if (ok) {
        f = fopen(path, "wb");
        ok = f && fwrite(d, 1, (size_t)n, f) == (size_t)n;
        if (f) ok = !fclose(f) && ok;
        if (!ok) snprintf(why, nwhy, "can't write the copy");
    }
    free(d);
    return ok;
}

static bool prepare_save(const Scene *s, char *why, size_t nwhy) {
    char dir[1200], from[1200], path[1300];
    snprintf(dir, sizeof dir, "%s/save", out_dir);
    plat_remove_tree(dir);
    mkdir(dir, 0755);
    snprintf(from, sizeof from, "%s/save_0", source);
    snprintf(path, sizeof path, "%s/save_0", dir);
    copy_tree(from, path);
    DIR *d = opendir(path);
    if (!d) { snprintf(why, nwhy, "no save_0 in %s", source); return false; }
    int slots = 0;
    bool ok = true;
    for (struct dirent *e; ok && (e = readdir(d));) {
        char sav[1600];
        struct stat sb;
        snprintf(sav, sizeof sav, "%s/%s/game_data.sav", path, e->d_name);
        if (e->d_name[0] == '.' || stat(sav, &sb)) continue;
        ok = edit_slot(sav, s, why, nwhy);
        slots++;
    }
    closedir(d);
    if (ok && !slots) { snprintf(why, nwhy, "no game_data.sav in %s", from); return false; }
    return ok;
}

// ------------------------------------------------------------------ start, and each boot
static void stop_run(const char *status, const char *message) {
    nscenes = 0;
    write_report(status);
    FATAL("Benchmark: %s\n", message);
}

static bool under(const char *dir, const char *p) {
    size_t n = strlen(dir);
    return !strncmp(dir, p, n) && (p[n] == '/' || !p[n]);
}

// Before the start screen and anything that opens a save or the settings
void benchmark_init(void) {
    const char *path = getenv("LARECOMP_BENCHMARK");
    if (!path || !*path) return;
    g_benchmark = true;
    const char *from = getenv("LARECOMP_SAVE_DIR");  // (the save this run would have used)
    snprintf(source, sizeof source, "%s", from && *from ? from : "save");
    if (strlen(path) > 900 || strlen(source) > 900) stop_run("bad_output", "a path over 900 characters");
    snprintf(out_dir, sizeof out_dir, "%s.out", path);
    {   // the output beside the scene file, and the save never under one another
        char a[4096], b[4096];  // (>= PATH_MAX: Bionic's fortified realpath checks)
        if (realpath(source, a) && realpath(path, b) && under(a, b)) stop_run("bad_save", "the scene file is inside the save's folder");
    }
    mkdir(out_dir, 0755);
    char err[300] = "", old[1200];
    snprintf(old, sizeof old, "%s/frames.csv", out_dir);  // (a run before this one; it's appended to, scene by scene)
    remove(old);
    if (!parse_scenes(path, err, sizeof err)) stop_run("bad_scene_file", err);
    {
        char a[4096], b[4096];
        if (realpath(source, a) && realpath(out_dir, b) && under(b, a)) stop_run("bad_save", "the save is inside the output folder");
    }
    // The game plays the copy, the settings are a copy too: the player's never change
    static char save_dir[1100], settings[1100];
    snprintf(save_dir, sizeof save_dir, "%s/save", out_dir);
    setenv("LARECOMP_SAVE_DIR", save_dir, 1);
    extern const char *settings_file(void);
    snprintf(settings, sizeof settings, "%s/settings.ini", out_dir);
    FILE *in = fopen(settings_file(), "rb"), *o = fopen(settings, "wb");
    if (!o) stop_run("bad_output", "can't write to the output folder (<scene file>.out)");
    char buf[4096];
    for (size_t n; in && (n = fread(buf, 1, sizeof buf, in)) > 0;) fwrite(buf, 1, n, o);
    if (in) fclose(in);
    fprintf(o, "\nframe_rate = 30\ntouch_controls = 0\nshow_fps = 0\naspect_asked = 1\nmenu_tip_seen = 1\n");  // (later lines win)
    fclose(o);
    setenv("LARECOMP_SETTINGS", settings, 1);
    setenv("LARECOMP_LAUNCHER_AUTOPLAY", "1", 1);
    write_report("running");
}

static uint64_t time_mgr;
static volatile bool hold_time, hold_weather;
static volatile bool time_written;
static enum { S_COOL, S_TITLE, S_ARRIVE, S_SETTLE, S_CAMERA, S_WARMUP, S_WINDOW, S_MEASURED, S_DONE } state;
static uint64_t boot_t, phase_t, press_t, event_t, warm_t, calm_t, warm_dirty;
static int good;
static uint64_t skipped_t0, compiles_t0, building_t0;
static double hz_start, load_s, camera_s, warm_s;
static float pos0[3];
static Power power_start;

static void set_step(const char *step) {
    cur_step = step;
    write_report("running");
}

static void next_or_exit(void);
static _Atomic uint64_t reboot_deadline;

// Called by the main thread while guest threads unwind; frame hooks no longer run here.
void benchmark_reboot_poll(void) {
    uint64_t deadline = atomic_load(&reboot_deadline);
    if (!deadline || plat_now_ns() < deadline) return;
    Result *r = &results[nresults++];
    *r = (Result){.scene = cur_scene, .repeat = cur_repeat};
    snprintf(r->step, sizeof r->step, "reboot");
    snprintf(r->why, sizeof r->why, "guest threads did not stop within 60 s");
    LOG("[benchmark] harness_error at reboot: %s\n", r->why);
    write_report("harness_error");
    // A live guest thread still owns old game memory: do not reset it or boot another scene.
    exit(EXIT_FAILURE);
}

// In boot(), the game's code loaded, none of it running: the scene's save goes in place
void benchmark_boot(void) {
    if (!g_benchmark) return;
    atomic_store(&reboot_deadline, 0);
    if (g_game_version != 190) {
        char m[100];
        snprintf(m, sizeof m, "unsupported version %d.%d.%d: it runs on game version 1.9.0 only", g_game_version / 100, g_game_version / 10 % 10, g_game_version % 10);
        stop_run("unsupported_version", m);
    }
    for (;;) {
        char why[120] = "";
        hold_time = hold_weather = win_on = false;
        time_mgr = 0, state = thermal_max < 0 ? S_TITLE : S_COOL, boot_t = phase_t = press_t = event_t = 0, good = 0;
        if (prepare_save(&scenes[cur_scene], why, sizeof why)) break;
        Result *r = &results[nresults++];
        *r = (Result){.scene = cur_scene, .repeat = cur_repeat};
        snprintf(r->step, sizeof r->step, "save");
        snprintf(r->why, sizeof r->why, "%s", why);
        LOG("[benchmark] %s, repeat %d: harness_error at save: %s\n", scenes[cur_scene].name, cur_repeat, why);
        next_or_exit();
    }
    LOG("[benchmark] scene %s, repeat %d: from a scratch copy of %s\n", scenes[cur_scene].name, cur_repeat, source);
    bench_trace_begin();
    previous_long = false;
    cooling_s = 0; cool_poll = 0; thermal_before = thermal_after = -1;
    set_step(thermal_max < 0 ? "title" : "cooling");
}

// ------------------------------------------------------------------ reads
static bool ok_ptr(uint64_t p) { return p >= GUEST_LO && p + 0x1000 < GUEST_HI && !(p & 7); }
static uint64_t read_ptr(uint64_t a) { uint64_t p = RD64(a); return ok_ptr(p) ? p : 0; }

static uint64_t camera(void) {
    uint64_t cm = read_ptr(GA(CAMERA_MGR));
    if (!cm) return 0;
    uint64_t b = cm + 0x28 + 0xd0;
    uint32_t i = RD32(b + 0xc);
    if (i == 0xffffffffu) return 0;
    return read_ptr(i < 2 ? b + 0x10 + 8 * (uint64_t)i : b + 0x10);
}

static bool cam_angles(double *yaw, double *pitch) {
    uint64_t c = camera();
    if (!c) return false;
    double dx = RDF(c + CAM_AT) - RDF(c + CAM_POS), dy = RDF(c + CAM_AT + 4) - RDF(c + CAM_POS + 4), dz = RDF(c + CAM_AT + 8) - RDF(c + CAM_POS + 8);
    double l = sqrt(dx * dx + dy * dy + dz * dz);
    if (l < 1e-4) return false;
    *yaw = atan2(dx, dz) * 180 / M_PI;
    *pitch = asin(dy / l) * 180 / M_PI;
    return true;
}

static double wrap180(double a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }

// ------------------------------------------------------------------ the holds, each clock update
void benchmark_vfr(void) {
    uint64_t w = read_ptr(GA(WORLD_MGR));
    if (!w) return;
    if (hold_time && time_mgr && !time_written) {  // (once: the game takes the new time; a step of 0 keeps it there)
        RDF(time_mgr + TM_NEW_TIME) = scenes[cur_scene].time;
        RDF(time_mgr + TM_STEP) = 0;
        time_written = true;
    }
    if (hold_weather) {
        int v = scenes[cur_scene].weather;
        RD8(w + W_WEATHER) = (uint8_t)v;
        RD32(w + W_WEATHER_TIMER) = 4;
        RD8(w + W_WEATHER_7D5) = 0;
        RD8(w + W_WEATHER_DEMO) = 1;
    }
}

// (vulkan/exec.c) each presented frame
void benchmark_frame(double period, double busy, double game, double render, double gpu) {
    // The GPU accumulator spans the hitch and the next frame; neither is a per-frame sample.
    bool long_frame = period > 100;
    if (previous_long) gpu = -1;
    previous_long = long_frame;
    if (long_frame || gpu <= 0) gpu = -1;
    bench_trace_frame(cur_step, win_on, period, busy, game, render, gpu);
    if (!win_on || win_n >= win_cap) return;
    float *r = win[win_n];
    r[0] = (float)period, r[1] = (float)busy, r[2] = (float)game, r[3] = (float)render, r[4] = (float)gpu;
    win_n = win_n + 1;
}

// ------------------------------------------------------------------ the sequence, each presented frame
static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float *g_sort;
static Pct percentiles(int column) {
    int n = 0;
    for (int i = 0; i < win_n; i++)
        if (win[i][column] >= 0) g_sort[n++] = win[i][column];
    qsort(g_sort, (size_t)n, sizeof *g_sort, cmp_float);
    if (!n) return (Pct){-1, -1, -1};
    return (Pct){g_sort[(n - 1) * 50 / 100], g_sort[(n - 1) * 95 / 100], g_sort[(n - 1) * 99 / 100]};
}

// The mean of the slowest share of the frames, as a frame rate. (g_sort: the periods, in order)
static double low_fps(double share) {
    int n = win_n, k = (int)ceil(n * share);
    double sum = 0;
    if (k < 1) k = 1;
    for (int i = 0; i < k; i++) sum += g_sort[n - 1 - i];
    return sum > 0 ? 1000.0 * k / sum : 0;
}

static void next_or_exit(void) {
    if (++cur_scene == nscenes) cur_scene = 0, cur_repeat++;
    if (cur_repeat < repeats) return;
    cur_scene = 0;
    write_report("done");
    LOG("[benchmark] done: %s/report.json\n", out_dir);
    exit(0);  // (the pipeline cache is kept: device.c quick_exit_handler)
}

static void end_scene(Result *r, bool ok, const char *step, const char *why) {
    hold_time = hold_weather = win_on = false;
    if (!r) {
        r = &results[nresults];
        *r = (Result){.scene = cur_scene, .repeat = cur_repeat};
    }
    if (!r->frames) bench_trace_finish(out_dir, scenes[cur_scene].name, cur_repeat, &r->trace);
    r->cooling_s = cooling_s; r->thermal_before = thermal_before; r->thermal_after = thermal_after;
    nresults++;
    r->ok = ok;
    if (!ok) {
        snprintf(r->step, sizeof r->step, "%s", step);
        snprintf(r->why, sizeof r->why, "%s", why);
        LOG("[benchmark] %s, repeat %d: harness_error at %s: %s\n", scenes[r->scene].name, r->repeat, step, why);
    } else {
        LOG("[benchmark] %s, repeat %d: ok, GPU p50 %.2f ms, frame p99 %.2f ms, %llu skipped for a compile, %llu compiles\n", scenes[r->scene].name, r->repeat, r->gpu.p50,
            r->frame.p99, (unsigned long long)r->skipped_building, (unsigned long long)r->compiles);
    }
    state = S_DONE;
    next_or_exit();  // (the last one ends here)
    atomic_store(&reboot_deadline, plat_now_ns() + 60000000000ull);
    cur_step = "reboot";
    write_report("running");
    rt_request_reboot(RT_REBOOT_GAME);
}

extern uint64_t vk_skipped_total(void), vk_compiles_total(void), vk_skipped_building_total(void), vk_spirv_misses(void);

static Result *measure(void) {
    extern double vk_display_hz(void);
    Result *r = &results[nresults];
    *r = (Result){.scene = cur_scene, .repeat = cur_repeat, .load_s = load_s, .camera_s = camera_s, .warmup_s = warm_s, .frames = win_n, .start = power_start, .hz_start = hz_start};
    r->frame = percentiles(0);
    r->low1 = low_fps(0.01);  // (percentiles left the periods sorted)
    r->low01 = low_fps(0.001);
    double sum = 0;
    for (int i = 0; i < win_n; i++) {
        sum += win[i][0];
        r->over50 += win[i][0] > 50;
    }
    r->fps = sum > 0 ? 1000.0 * win_n / sum : 0;
    r->cpu = percentiles(1);
    r->game = percentiles(2);
    r->render = percentiles(3);
    r->gpu = percentiles(4);
    r->skipped = vk_skipped_total() - skipped_t0;
    r->compiles = vk_compiles_total() - compiles_t0;
    r->skipped_building = vk_skipped_building_total() - building_t0;
    r->hz_end = vk_display_hz();
    read_power(&r->end);
    bench_trace_finish(out_dir, scenes[cur_scene].name, cur_repeat, &r->trace);
    return r;
}

void benchmark_input(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    extern bool widescreen_world_play(void);
    extern bool framepace_in_event(void);
    *b = 0, *lx = *ly = *rx = *ry = 0;
    if (state == S_DONE || !nscenes) return;
    const Scene *s = &scenes[cur_scene];
    uint64_t t = plat_now_ns();
    if (!boot_t) boot_t = t;
    double since = (t - phase_t) / 1e9;
    uint64_t pi = read_ptr(GA(PLAYER_INFO)), w = read_ptr(GA(WORLD_MGR));
    float pos[3] = {0, 0, 0};
    if (pi) pos[0] = RDF(pi + PI_POS), pos[1] = RDF(pi + PI_POS + 4), pos[2] = RDF(pi + PI_POS + 8);
    bool title = framepace_on_title(), world = widescreen_world_play(), event = framepace_in_event();
    double yaw = 0, pitch = 0, ey = 0, ep = 0;
    bool angles = cam_angles(&yaw, &pitch);
    if (angles) ey = wrap180(s->yaw - yaw), ep = s->pitch - pitch;

    switch (state) {
    case S_COOL: {
        if (!phase_t) phase_t = t;
        cooling_s = (t - phase_t) / 1e9;
        if (title && t >= cool_poll) {
            Power power; read_power(&power);
            cool_poll = t + 1000000000ull;
            if (thermal_before < 0) thermal_before = power.thermal;
            thermal_after = power.thermal;
            int decision = bench_cooling_check(thermal_max, power.thermal, cooling_s, thermal_wait_s);
            if (decision == -1) { end_scene(NULL, false, "cooling", "thermal status unavailable"); break; }
            if (decision == 1) {
                state = S_TITLE; boot_t = t; phase_t = t; set_step("title"); break;
            }
        }
        if ((t - phase_t) / 1e9 >= thermal_wait_s) end_scene(NULL, false, "cooling", "thermal wait timed out");
        break;
    }
    case S_TITLE:  // A on the title screen (Continue) until the game has loaded; the count starts at the first in-world frame
        if (title && t - press_t > 2000000000ull) press_t = t;
        if (title && t - press_t < 330000000ull) *b = 1;
        if (!title && world && !event && fabs(pos[0]) + fabs(pos[2]) > 1) {
            load_s = (t - boot_t) / 1e9;
            phase_t = t;
            state = S_ARRIVE;
            hold_weather = true;
            set_step("arrive");
        } else if ((t - boot_t) / 1e9 > TIMEOUT_LOAD_S) {
            end_scene(NULL, false, "load", title ? "still on the title screen" : "the world didn't come up");
        }
        break;
    case S_ARRIVE:
        if (w && pi && fabs(pos[0] - s->pos[0]) < ARRIVE_TOLERANCE && fabs(pos[2] - s->pos[2]) < ARRIVE_TOLERANCE && RD8(w + W_MAIN_FIELD)) {
            uint64_t arr = RD32(w + W_MGRS) ? read_ptr(w + W_MGRS + 8) : 0;
            time_mgr = arr ? read_ptr(arr) : 0;
            float now = time_mgr ? RDF(time_mgr + TM_TIME) : -1;
            if (now < 0 || now >= 360) { end_scene(NULL, false, "hold_time", "no time manager where it was expected"); break; }
            time_written = false, hold_time = true;
            phase_t = t;
            state = S_SETTLE;
        } else if (since > TIMEOUT_ARRIVE_S) {
            end_scene(NULL, false, "arrive", w && !RD8(w + W_MAIN_FIELD) ? "not on the main field: a save in a shrine or a DLC trial?" : "Link isn't at the scene's position");
        }
        break;
    case S_SETTLE:
        if (since >= SETTLE_S) {
            phase_t = t;
            state = S_CAMERA;
            set_step("camera");
        }
        break;
    case S_CAMERA:  // the stick, closer to the angles the smaller; below 11000 the pitch doesn't move (step 11a)
        if (angles) {
            bool py = fabs(ey) < CAM_TOLERANCE, pp = fabs(ep) < CAM_TOLERANCE;
            good = py && pp ? good + 1 : 0;
            *rx = py ? 0 : -(int32_t)copysign(fmin(32767, 5000 + fabs(ey) * 700), ey);
            *ry = pp ? 0 : (int32_t)copysign(fmin(32767, 11000 + fabs(ep) * 1500), ep);
        }
        if (good >= 10) {
            camera_s = since;
            phase_t = warm_t = calm_t = t;
            warm_dirty = vk_skipped_building_total() + vk_compiles_total();
            state = S_WARMUP;
            set_step("warmup");
        } else if (since > TIMEOUT_CAMERA_S) {
            char why[120];
            snprintf(why, sizeof why, "off by %.1f, %.1f degrees after %d s (inverted camera in the save's options?)", ey, ep, TIMEOUT_CAMERA_S);
            end_scene(NULL, false, "camera", why);
        }
        break;
    case S_WARMUP:  // (a short event, about a second, comes 6-7 s after every load: the warm-up counts from its end)
        if (!world) { end_scene(NULL, false, "warmup", "left the world"); break; }
        if (event) {
            if (!event_t) event_t = t;
            if ((t - event_t) / 1e9 > TIMEOUT_EVENT_S) end_scene(NULL, false, "warmup", "an event didn't end");
            phase_t = t;
            break;
        }
        event_t = 0;
        uint64_t dirty = vk_skipped_building_total() + vk_compiles_total();
        if (dirty != warm_dirty) warm_dirty = dirty, calm_t = t;
        warm_s = (t - warm_t) / 1e9;
        if ((since >= warmup_s && (t - calm_t) / 1e9 >= calm_s) || warm_s >= warmup_max_s) {
            extern double vk_display_hz(void);
            int cap = window_s * 130 + 64;  // (more than the panel's 120 Hz could present)
            if (cap > win_cap) {
                free(win), free(g_sort);
                win = malloc(sizeof(float[5]) * (size_t)cap);
                g_sort = malloc(sizeof(float) * (size_t)cap);
                if (!win || !g_sort) stop_run("out_of_memory", "cannot allocate the measurement window");
                win_cap = cap;
            }
            memcpy(pos0, pos, sizeof pos0);
            set_step("window");
            read_power(&power_start);
            hz_start = vk_display_hz();
            skipped_t0 = vk_skipped_total(), compiles_t0 = vk_compiles_total();
            building_t0 = vk_skipped_building_total();
            if (!strcmp(cache_at_start, "unknown")) cache_at_start = vk_spirv_misses() ? "cold" : "warm";
            win_n = 0, win_on = true;
            phase_t = plat_now_ns();
            state = S_WINDOW;
        }
        break;
    case S_WINDOW: {
        const char *bad = event ? "an event started" : !world ? "left the world" : framepace_target() != TARGET_FPS ? "the frame rate target isn't 30" : NULL;
        if (bad) end_scene(NULL, false, "window", bad);
        else if (since >= window_s) { win_on = false; state = S_MEASURED; phase_t = t; set_step("drain"); }
        break;
    }
    case S_MEASURED: {
        if (since < 1) break;  // drain delayed display-timing feedback for the window
        if (!win_n) { end_scene(NULL, false, "window", "no frames recorded"); break; }
        Result *r = measure();
        r->cam_err = hypot(ey, ep);
        r->moved = hypot(pos[0] - pos0[0], pos[2] - pos0[2]);
        double drift = fabs(RDF(time_mgr + TM_TIME) - s->time);
        if (!angles) end_scene(r, false, "check", "the camera can't be read");
        else if (r->moved > MOVE_TOLERANCE) end_scene(r, false, "check", "Link moved in the window");
        else if (r->cam_err > CAM_DRIFT) end_scene(r, false, "check", "the camera drifted in the window");
        else if (drift > TIME_DRIFT) end_scene(r, false, "check", "the time of day didn't hold");
        else if (!w || RD8(w + W_WEATHER) != (uint8_t)s->weather) end_scene(r, false, "check", "the weather didn't hold");
        else end_scene(r, true, "", "");
        break;
    }
    case S_DONE:
        break;
    }
}
