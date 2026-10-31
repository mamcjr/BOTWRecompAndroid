// User settings (botwrecompandroid.ini next to the executable's working directory), edited from the in-game menu.
#pragma once
#include <stdbool.h>

enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1 };
enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12 };
// (11 was 67% of the display: larecomp.ini "67%" now loads as RES_Q75)
enum { OUT_AUTO = -1 };  // (Settings.out_lines)
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* AMD FSR 1 */,
       UPSCALE_SGSR1 = 3 /* Vulkan: Snapdragon Game Super Resolution 1 */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1, AA_FXAA = 2 };
// System language served to the game (runtime/services/am.c GetDesiredLanguage, services/hid_set.c GetLanguageCode):
// the game picks its text language from it, as it would from the Switch's own system setting. LANG_AUTO follows the
// host locale ($LC_ALL / $LC_MESSAGES / $LANG); the rest are the codes of the languages the game has text for.
enum { LANG_AUTO = 0, N_LANGUAGES = 13 };
extern const char *const g_language_codes[N_LANGUAGES];  // what the game is told: "ja", "en-US", "es", ...
extern const char *const g_language_names[N_LANGUAGES];  // for the menu ("Auto", "Japanese", ..., "Spanish")

typedef struct {
    int aspect;          // ASPECT_*: unlocked = fill the window (wider/taller views), original = 16:9 with bars
    bool fullscreen;
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: Windows)
    int out_lines;       // Output resolution: the final image (scene + HUD) at most this many lines, scaled up to the
                         // screen at the end (0: the screen's own; 1080 / 900 / 720; OUT_AUTO: 1080 on taller
                         // screens, Android's default) - nvn/output_scale.c
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    float volume;        // master volume, 1 = 100%
    bool reverb;         // room reverb effect
    bool movie_audio;
    bool show_fps;
    bool motion;          // the phone's (or a controller's) gyroscope as the controller's motion sensor (hle/motion.c)
#ifdef __ANDROID__
    bool touch_controls;  // on-screen controls while no controller is connected (ui/touch_controls.mm)
    int overlay_style;    // Show FPS's overlay (ui/perf_overlay.mm): 0 full, 1 compact (one line)
    float overlay_pos[2]; // its top left, a share of the screen, dragged there with a finger (negative: the corner)
    bool event_fps_unlock;  // in-game cutscenes and talks at the chosen frame rate, not 30 (hle/framepace.c)
    int fps_mode;         // 0: the game's own choice (30 / 60, the frame pacer), 1: 30 fps, 2: 60 fps
    bool shader_int32;    // shader address math in 32-bit words even where the driver offers 64-bit (vulkan/exec.c)
    bool native_code;     // Android: the game's own code on the CPU, no build of it (runtime/native.c); off by default
    bool files_in_place;  // Android: a setup reads the picked romfs/ where it is, no copy (services/fs.c); off by default
    bool sync_lag;        // Android: the game's next frame overlaps the GPU's last (nvn/objects.c nvnSyncWait); on by default
    // Driver compatibility (the start screen's Settings > Prelaunch, ui/launcher.mm; from LASRecompAndroid), for GPUs
    // where a Turnip path draws wrong: read when the game starts (gpu_heap, inplace_reads) or when the app starts
    // (turnip_lrz, log_file). -1 Automatic: on for the Adreno 8xx GPUs these paths were made on, off elsewhere
    // (vulkan/device.c vk_adreno_8xx); 1 on, 0 off.
    int gpu_heap;         // Turnip: the game's memory mapped as GPU memory (vulkan/exec.c vk_heap_place); off: CPU copies
                          // (its Automatic: on for any Turnip that can place it - vulkan/device.c vk_turnip_heap_on)
    int inplace_reads;    // Turnip: draws sample targets they write as they are (vulkan/exec.c resolve_feedback); off: copies
    int turnip_lrz;       // Turnip's low-resolution Z; off: TU_DEBUG=nolrz before the driver loads (vulkan/device.c)
    bool log_file;        // the log in a file as well (platform/android.c), to send with a report
    char gpu_model[64];   // the GPU of the last start ("Adreno (TM) 830v1"): turnip_lrz's Automatic, before the driver
#define TOUCH_NCTL 19  // (ui/touch_controls.mm: LAS's 12, then the right stick, L3 / R3 and the d-pad)
    float touch_pos[TOUCH_NCTL][2];  // each on-screen control's place, a share of the screen (negative: default)
    float touch_scale, touch_opacity;
    // FPS-style touch play (ui/touch_controls.mm): the camera by dragging a finger (its speed tilts the right stick)
    // instead of holding a stick, at touch_drag_sens; the sticks drawn only while a finger holds them; each stick's
    // area a whole half of the screen
    bool touch_drag_camera, touch_sticks_hidden, touch_half_areas;
    float touch_drag_sens;
    float stick_deadzone;  // a controller's sticks: a round dead zone (share of the travel, 0-0.4), rescaled past it
                           // (hle/input.c): worn sticks that drift. 0: the game's own only
#endif
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    bool aspect_asked;   // the start screen's one-time "authentic 16:9 or unlocked?" question was answered
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
    char mods_off[512];  // game mods turned off (runtime/mods.c: their folder names, separated by '|')
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
// The menu's RESTORE DEFAULTS: the Video and Audio options back to the defaults (the language, the controls, the touch
// layout, mods and the one-time questions stay)
void settings_reset_defaults(void);
// The language code to serve the game ("es", "en-US", ...): the setting, or the host locale for LANG_AUTO
const char *settings_language(void);
// That code's name for the menu ("Spanish")
const char *settings_language_name(void);
// Android: the language of the menus' own text (ui/i18n.mm), from the phone's first preferred language: "pt", "es", "fr",
// "de", "it", "nl", "ru", "ja", "ko", "zh-Hans", "zh-Hant" (the game's languages), else "en". LARECOMP_HOST_LANG=<code>
// (tests). Elsewhere "en".
const char *settings_menu_language(void);
bool settings_host_portuguese(void);  // (settings_menu_language is "pt")
