#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_bind.h"
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <sys/system_properties.h>
#endif

#define SETTINGS_FILE settings_file()

// LARECOMP_SETTINGS=<path>: another settings file (test runs never touch the player's larecomp.ini)
static const char *settings_file(void) {
    const char *e = getenv("LARECOMP_SETTINGS");
    if (e && *e) return e;
    static int moved;  // (the project's former name: the player's settings carry over, once)
    if (!moved) {
        moved = 1;
        FILE *f = fopen("botwrecompandroid.ini", "rb");
        if (f) fclose(f);
        else rename("botwrecomp.ini", "botwrecompandroid.ini");
    }
    return "botwrecompandroid.ini";
}

Settings g_settings = {
    .aspect = ASPECT_UNLOCKED,
    .fullscreen = false,
    .volume = 1.0f,
    .reverb = true,
    .movie_audio = true,
    .motion = true,
#ifdef __ANDROID__
    .show_fps = true,  // (the performance overlay, ui/perf_overlay.mm: on until turned off in the menu)
    .touch_controls = true,
    .overlay_pos = {-1, -1},
    .touch_pos = {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
                  {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}},
    .touch_scale = 1.0f,
    .touch_opacity = 1.0f,
    .touch_drag_sens = 1.0f,
    .gpu_heap = -1,
    .inplace_reads = -1,
    .turnip_lrz = -1,
    .event_fps_unlock = false,  // (some events count frames: hle/framepace.c in_event)
    .fps_mode = 1,  // (30: the game's own choice dropped the overworld to 20, where the GPU's clocks settled low)
    .res_mode = 1,  // (1x: the Switch's 900p scene, the HUD at the screen's resolution - Match display drew the scene at
                    // 2560x1440 on the POCO F7 Ultra: 22 fps)
#else
    .show_fps = false,
#endif
#ifdef __ANDROID__
    .upscaler = UPSCALE_SGSR1,  // (one pass: ~1 ms on the Adreno 830 to 2560x1440, FSR 1's two ~2.7 ms; 40 fps against 38)
    .native_code = false,       // (an experiment: runtime/native.c)
    .sync_lag = true,           // (a few more fps; the CPU's reads of GPU results a frame old: nvn/objects.c nvnSyncWait)
    .out_lines = OUT_AUTO,      // (POCO F7 Ultra, 3200x1440, 1.8.2's Great Plateau: GPU 22.8 ms a frame at the screen's,
                                // 18.6 at 1080 lines with a larger scene - the final image's passes over 2.2x the pixels)
#else
    .upscaler = UPSCALE_SHARP,  // (MetalFX on macOS, AMD FSR 1 on Windows)
#endif
};

// The game's own text languages: its RomFS carries these per region (regionUS/USen|USes|USfr, regionEU/EUen|EUes|...,
// regionJP/JPja, regionKR/KRko, regionTW/TWzh, regionCN/CNzh), and the game picks one from the system language it
// reads back (runtime/services/am.c), exactly as it does on the console. Codes as the Switch reports them.
// Brazilian Portuguese (regionUS/USpt, Pack/Bootup_USpt.pack) came with the game's v1.8.2 (runtime/language.c in
// SavageWindRecomp); the older versions are told English instead.
// Chinese is zh-Hans / zh-Hant, as GetDesiredLanguage answers since system 4.0 (and Eden): the older zh-CN / zh-TW
// gave Japanese text (issue #15).
const char *const g_language_codes[N_LANGUAGES] = {"auto", "ja", "en-US", "es", "fr", "de", "it", "nl", "ru", "ko",
                                                   "zh-Hans", "zh-Hant", "pt-BR"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English", "Spanish", "French", "German",
                                                   "Italian", "Dutch", "Russian", "Korean", "Chinese (Simplified)",
                                                   "Chinese (Traditional)", "Portuguese (Brazil)"};

// The app's own menus (ui/i18n.mm): the game's Language when one is chosen - one setting for both, read once (the menus
// change when the app starts again) - else the phone's first preferred language, which is the app's own when Android
// 13+ has one set for it (res/xml/locales_config.xml)
const char *settings_menu_language(void) {
#ifdef __ANDROID__
    static const char *lang;
    if (!lang) {
        char l[16] = "", country[16] = "";
        const char *e = getenv("LARECOMP_HOST_LANG");
        int chosen = g_settings.language;
        if (!(e && *e) && chosen > LANG_AUTO && chosen < N_LANGUAGES) {
            static const char *const menu[N_LANGUAGES] = {"en", "ja", "en", "es", "fr", "de", "it", "nl", "ru", "ko",
                                                          "zh-Hans", "zh-Hant", "pt"};  // (g_language_codes' order)
            return lang = menu[chosen];
        }
        if (e && *e) {
            snprintf(l, sizeof l, "%s", e);
            char *dash = strchr(l, '-');
            if (dash) snprintf(country, sizeof country, "%s", dash + 1), *dash = 0;
        } else {  // (the phone's first preferred language, through SDL)
            int n = 0;
            SDL_Locale **loc = SDL_GetPreferredLocales(&n);
            if (loc && n > 0 && loc[0]->language) {
                snprintf(l, sizeof l, "%s", loc[0]->language);
                if (loc[0]->country) snprintf(country, sizeof country, "%s", loc[0]->country);
            }
            SDL_free(loc);
        }
        static const char *const own[] = {"pt", "es", "fr", "de", "it", "nl", "ru", "ja", "ko"};
        lang = "en";
        for (size_t i = 0; i < sizeof own / sizeof *own; i++)
            if (!strcmp(l, own[i])) lang = own[i];
        if (!strcmp(l, "zh"))  // (Taiwan, Hong Kong, Macau: traditional characters)
            lang = !strcmp(country, "TW") || !strcmp(country, "HK") || !strcmp(country, "MO") || !strcmp(country, "Hant")
                       ? "zh-Hant" : "zh-Hans";
    }
    return lang;
#else
    return "en";
#endif
}

bool settings_host_portuguese(void) { return !strcmp(settings_menu_language(), "pt"); }

// LANG_AUTO: the host's own language (LANG=es_ES.UTF-8 -> es), en-US when it is none of the game's. Linux only:
// macOS / Windows apps get no locale variables from the system, so Auto is English there (pick one in settings).
static const char *host_language(void) {
#ifndef __linux__
    return "en-US";
#endif
#ifdef __ANDROID__
    // (Android: no locale variables. The app's preferred language first - the phone's, or the app's own one set in
    // Android 13+'s per-app languages - then the system's as a property, "pt-BR", "es-US", "zh-Hant-TW")
    static char prop[PROP_VALUE_MAX];
    if (!prop[0]) {
        int n = 0;
        SDL_Locale **loc = SDL_GetPreferredLocales(&n);
        if (loc && n > 0 && loc[0]->language)
            snprintf(prop, sizeof prop, "%s%s%s", loc[0]->language, loc[0]->country ? "-" : "",
                     loc[0]->country ? loc[0]->country : "");
        SDL_free(loc);
    }
    if (!prop[0] && __system_property_get("persist.sys.locale", prop) <= 0) __system_property_get("ro.product.locale", prop);
    const char *e = prop;
#else
    const char *e = getenv("LC_ALL");
#endif
#ifndef __ANDROID__
    if (!e || !*e) e = getenv("LC_MESSAGES");
    if (!e || !*e) e = getenv("LANG");
#endif
    if (!e || !*e || !strcmp(e, "C") || !strcmp(e, "POSIX")) return "en-US";
    char l[8] = {0};
    size_t n = 0;
    for (const char *p = e; *p && *p != '.' && *p != '@' && *p != '_' && *p != '-' && n < sizeof l - 1; p++) l[n++] = *p;
    static const struct { const char *loc, *code; } map[] = {{"ja", "ja"}, {"ko", "ko"}, {"ru", "ru"}, {"nl", "nl"},
                                                             {"de", "de"}, {"it", "it"}, {"fr", "fr"}, {"es", "es"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(l, map[i].loc)) return map[i].code;
    if (!strcmp(l, "zh"))  // (TW / HK, Hant: traditional)
        return strstr(e, "TW") || strstr(e, "HK") || strstr(e, "Hant") ? "zh-Hant" : "zh-Hans";
    if (!strcmp(l, "pt")) return "pt-BR";  // (the only Portuguese the game has)
    return "en-US";
}

const char *settings_language(void) {
    int i = g_settings.language;
    const char *code = i > LANG_AUTO && i < N_LANGUAGES ? g_language_codes[i] : host_language();
    extern int g_game_version;
    if (!strcmp(code, "pt-BR") && g_game_version && g_game_version < 182) return "en-US";  // (no Portuguese before 1.8.2)
    return code;
}

const char *settings_language_name(void) {
    const char *c = settings_language();
    for (int i = 0; i < N_LANGUAGES; i++)
        if (!strcmp(c, g_language_codes[i])) return g_language_names[i];
    return "English";
}

static Settings g_defaults;  // (the defaults above, kept before the file overrides them: settings_reset_defaults)
static bool g_have_defaults;

void settings_reset_defaults(void) {
    if (!g_have_defaults) return;
    const Settings *d = &g_defaults;
    g_settings.aspect = d->aspect;
    g_settings.fullscreen = d->fullscreen;
    g_settings.res_mode = d->res_mode;
    g_settings.upscaler = d->upscaler;
    g_settings.out_lines = d->out_lines;
    g_settings.antialias = d->antialias;
    g_settings.volume = d->volume;
    g_settings.reverb = d->reverb;
    g_settings.movie_audio = d->movie_audio;
    g_settings.show_fps = d->show_fps;
    g_settings.motion = d->motion;
    g_settings.motion_source = d->motion_source;
#ifdef __ANDROID__
    g_settings.touch_controls = d->touch_controls;
    g_settings.overlay_style = d->overlay_style;
    g_settings.fps_mode = d->fps_mode;
    g_settings.event_fps_unlock = d->event_fps_unlock;
    g_settings.shader_int32 = d->shader_int32;
    g_settings.sync_lag = d->sync_lag;
    g_settings.native_code = d->native_code;
    g_settings.files_in_place = d->files_in_place;
#endif
}

void settings_load(void) {
    static bool loaded;
    if (!g_have_defaults) g_defaults = g_settings, g_have_defaults = true;
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
#ifdef __ANDROID__
    char line[640];  // (touch_layout: 24 numbers)
#else
    char line[256];
#endif
    bool tip_key = false, aspect_key = false, output_key = false;
    while (fgets(line, sizeof line, f)) {
        char key[64];
#ifdef __ANDROID__
        char val[512] = "";  // (a key with nothing after "=": mods_off when every mod is on)
        if (sscanf(line, " %63[^= ] = %511[^\n]", key, val) < 1) continue;
#else
        char val[128] = "";
        if (sscanf(line, " %63[^= ] = %127[^\n]", key, val) < 1) continue;
#endif
        if (!strcmp(key, "aspect")) g_settings.aspect = !strcmp(val, "original") ? ASPECT_ORIGINAL : ASPECT_UNLOCKED;
        else if (!strcmp(key, "fullscreen")) g_settings.fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
        else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
        else if (!strcmp(key, "motion")) g_settings.motion = atoi(val) != 0;
        else if (!strcmp(key, "motion_source")) g_settings.motion_source = atoi(val) >= 0 && atoi(val) <= 2 ? atoi(val) : 0;
#ifdef __ANDROID__
        else if (!strcmp(key, "overlay")) g_settings.overlay_style = !strcmp(val, "compact");
        else if (!strcmp(key, "overlay_pos")) sscanf(val, "%f,%f", &g_settings.overlay_pos[0], &g_settings.overlay_pos[1]);
#endif
#ifdef __ANDROID__
        else if (!strcmp(key, "touch_controls")) g_settings.touch_controls = atoi(val) != 0;
        else if (!strcmp(key, "cutscenes_60")) g_settings.event_fps_unlock = atoi(val) != 0;
        else if (!strcmp(key, "frame_rate")) g_settings.fps_mode = !strcmp(val, "30") ? 1 : !strcmp(val, "60") ? 2 : 0;
        else if (!strcmp(key, "shader_int32")) g_settings.shader_int32 = atoi(val) != 0;
        else if (!strcmp(key, "sync_lag")) g_settings.sync_lag = atoi(val) != 0;
        else if (!strcmp(key, "native_code")) g_settings.native_code = atoi(val) != 0;
        else if (!strcmp(key, "files_in_place")) g_settings.files_in_place = atoi(val) != 0;
        else if (!strcmp(key, "gpu_heap")) g_settings.gpu_heap = !strcmp(val, "auto") ? -1 : atoi(val) != 0;
        else if (!strcmp(key, "inplace_reads")) g_settings.inplace_reads = !strcmp(val, "auto") ? -1 : atoi(val) != 0;
        else if (!strcmp(key, "turnip_lrz")) g_settings.turnip_lrz = !strcmp(val, "auto") ? -1 : atoi(val) != 0;
        else if (!strcmp(key, "log_file")) g_settings.log_file = atoi(val) != 0;
        else if (!strcmp(key, "gpu_model")) snprintf(g_settings.gpu_model, sizeof g_settings.gpu_model, "%s", val);
        else if (!strcmp(key, "touch_scale")) g_settings.touch_scale = (float)atof(val);
        else if (!strcmp(key, "touch_opacity")) g_settings.touch_opacity = (float)atof(val);
        else if (!strcmp(key, "touch_drag_camera")) g_settings.touch_drag_camera = atoi(val) != 0;
        else if (!strcmp(key, "touch_drag_sens")) g_settings.touch_drag_sens = (float)atof(val);
        else if (!strcmp(key, "stick_deadzone")) g_settings.stick_deadzone = fminf(fmaxf((float)atof(val), 0), 0.4f);
        else if (!strcmp(key, "touch_sticks_hidden")) g_settings.touch_sticks_hidden = atoi(val) != 0;
        else if (!strcmp(key, "touch_half_areas")) g_settings.touch_half_areas = atoi(val) != 0;
        else if (!strcmp(key, "touch_layout")) {  // x,y per control (a share of the screen; negative: default)
            char *q = val;
            for (int i = 0; i < TOUCH_NCTL * 2 && *q; i++) {
                g_settings.touch_pos[i / 2][i % 2] = strtof(q, &q);
                while (*q == ',' || *q == ' ') q++;
            }
        }
#endif
        else if (!strcmp(key, "resolution")) {
            int n = atoi(val);
            g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : (!strcmp(val, "75%") || !strcmp(val, "67%")) ? RES_Q75 :
                                  !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 :
                                  (n >= 1 && n <= 4) ? n : n > 4 ? 4 : RES_MATCH;  // (5x-8x were removed)
        }
        else if (!strcmp(key, "upscaler"))  // (metalfx / fsr1: this OS's sharp one)
            g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR
#ifndef __APPLE__
                                : !strcmp(val, "sgsr1") ? UPSCALE_SGSR1
#endif
                                : UPSCALE_SHARP;
        else if (!strcmp(key, "output")) {  // ("auto", "screen", or the final image's lines: 1080 / 900 / 720)
            int n = atoi(val);
            g_settings.out_lines = !strcmp(val, "auto") ? OUT_AUTO : n == 1080 || n == 900 || n == 720 ? n : 0;
            output_key = true;
        }
        else if (!strcmp(key, "output_resolution") && !output_key) {  // (0.1.11 and before: "screen" was only the default,
            int n = atoi(val);                                         // written by every save - read as Auto now)
            g_settings.out_lines = n == 1080 || n == 900 || n == 720 ? n : g_defaults.out_lines;
        }
        else if (!strcmp(key, "antialias")) g_settings.antialias = !strcmp(val, "off") ? AA_OFF : !strcmp(val, "fxaa") ? AA_FXAA : AA_SMAA;
        else if (!strcmp(key, "language")) {  // ("auto" or one of the game's codes; anything else: auto)
            g_settings.language = LANG_AUTO;
            const char *v = !strcmp(val, "zh-CN") ? "zh-Hans" : !strcmp(val, "zh-TW") ? "zh-Hant" : val;  // (saved before #15)
            for (int i = 1; i < N_LANGUAGES; i++)
                if (!strcmp(v, g_language_codes[i])) g_settings.language = i;
        }
        else if (!strcmp(key, "menu_tip_seen")) g_settings.menu_tip_seen = atoi(val) != 0, tip_key = true;
        else if (!strcmp(key, "mods_off")) snprintf(g_settings.mods_off, sizeof g_settings.mods_off, "%s", val);
        else if (!strcmp(key, "aspect_asked")) g_settings.aspect_asked = atoi(val) != 0, aspect_key = true;
        else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c
    }
    fclose(f);
    if (!tip_key) g_settings.menu_tip_seen = true;  // (a settings file from before the note: not a first launch)
    if (!aspect_key) g_settings.aspect_asked = true;  // (likewise: the aspect ratio was chosen already)
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    LOG("[settings] loaded %s\n", SETTINGS_FILE);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# BOTWRecompAndroid settings (edited from the in-game menu: F1, Esc, or Minus+Plus on a controller)\n");
    fprintf(f, "aspect = %s\n", g_settings.aspect == ASPECT_ORIGINAL ? "original" : "unlocked");
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    fprintf(f, "motion = %d\nmotion_source = %d\n", g_settings.motion, g_settings.motion_source);
#ifdef __ANDROID__
    fprintf(f, "overlay = %s\n", g_settings.overlay_style == 1 ? "compact" : "full");
    fprintf(f, "overlay_pos = %.4f,%.4f\n", g_settings.overlay_pos[0], g_settings.overlay_pos[1]);
#endif
#ifdef __ANDROID__
    fprintf(f, "touch_controls = %d\n", g_settings.touch_controls);
    fprintf(f, "frame_rate = %s\n", g_settings.fps_mode == 1 ? "30" : g_settings.fps_mode == 2 ? "60" : "auto");
    fprintf(f, "cutscenes_60 = %d\n", g_settings.event_fps_unlock);
    fprintf(f, "shader_int32 = %d\nsync_lag = %d\nnative_code = %d\nfiles_in_place = %d\n", g_settings.shader_int32,
            g_settings.sync_lag, g_settings.native_code, g_settings.files_in_place);
    static const char *const tri[] = {"auto", "0", "1"};  // (-1 / 0 / 1)
#define TRI(v) tri[(v) < 0 ? 0 : (v) ? 2 : 1]
    fprintf(f, "gpu_heap = %s\ninplace_reads = %s\nturnip_lrz = %s\nlog_file = %d\ngpu_model = %s\n", TRI(g_settings.gpu_heap),
            TRI(g_settings.inplace_reads), TRI(g_settings.turnip_lrz), g_settings.log_file, g_settings.gpu_model);
#undef TRI
    fprintf(f, "touch_drag_camera = %d\ntouch_drag_sens = %.2f\ntouch_sticks_hidden = %d\ntouch_half_areas = %d\n",
            g_settings.touch_drag_camera, g_settings.touch_drag_sens, g_settings.touch_sticks_hidden,
            g_settings.touch_half_areas);
    fprintf(f, "stick_deadzone = %.2f\n", g_settings.stick_deadzone);
    fprintf(f, "touch_scale = %.2f\ntouch_opacity = %.2f\ntouch_layout = ", g_settings.touch_scale, g_settings.touch_opacity);
    for (int i = 0; i < TOUCH_NCTL; i++)
        fprintf(f, "%s%.4f,%.4f", i ? "," : "", g_settings.touch_pos[i][0], g_settings.touch_pos[i][1]);
    fprintf(f, "\n");
#endif
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_METALFX ? "metalfx" : g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" :
                                   g_settings.upscaler == UPSCALE_SGSR1 ? "sgsr1" : "bilinear");
    if (g_settings.out_lines > 0) fprintf(f, "output = %d\n", g_settings.out_lines);
    else fprintf(f, "output = %s\n", g_settings.out_lines == OUT_AUTO ? "auto" : "screen");
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : g_settings.antialias == AA_FXAA ? "fxaa" : "off");
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "language = %s\n", g_language_codes[g_settings.language > LANG_AUTO && g_settings.language < N_LANGUAGES
                                                     ? g_settings.language : LANG_AUTO]);
    fprintf(f, "aspect_asked = %d\n", g_settings.aspect_asked);
    fprintf(f, "menu_tip_seen = %d\n", g_settings.menu_tip_seen);
    fprintf(f, "mods_off = %s\n", g_settings.mods_off);
    bind_settings_save(f);
    fclose(f);
}
