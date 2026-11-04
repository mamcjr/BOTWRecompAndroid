// Frame rate for BOTW 1.8.2: 30, 60 or no limit (Video > Frame rate), and the game's clock the same at every one of
// them - from SavageWindRecomp (MIT, runtime/hle/framepace.c), which worked this out for its PC port of 1.8.2.
//
// How BOTW 1.8.2 keeps time (main module, canonical addresses):
//   - Its frame loop (sead GameFrameworkNx, 0x700101287c) asks for a present interval of 2 vsyncs (30 fps), 3 (20 fps)
//     after three slow frames in a row. The frame loop's own vsync wait (0x7000d811fc) only runs for the first frames
//     after start-up; the present keeps the pace.
//   - ksys::VFR (the instance at [0x7001d32010]) turns that interval into the game's time step: one "delta frame" is
//     1/30 s. Every frame VFR::updateInterval (0x7001409230) sets the interval ratio (interval / 2: 1.0 at 30 fps,
//     1.5 at 20 fps), and delta frame = ratio x the slowest active time multiplier (slow motion). About 1600 call
//     sites read it: the time of day, the Blood Moon timer, the Havok step, stamina, animation, AI.
//   - The paused game (pause menu, map: the state at 0x7001cb48a4 is 2, 0 while playing) updates VFR twice a frame.
// So the game already runs at any frame rate as long as VFR is told how long a frame really took. The runtime:
//   - paces the presents at the chosen rate on a steady grid (nvnQueuePresentTexture, framepace_period_ns), and skips
//     the frame loop's vsync wait (rt_frame_wait, tools/patches.py);
//   - tells VFR how long the frame really took (rt_vfr_update, inside updateInterval after the game set its ratio):
//     ratio = seconds x 30, changing by a few percent a frame at most, and whatever it gave more or less than the
//     real time is paid back over the next frames. The interval follows the frame rate averaged over ~20 frames (1
//     above ~43 fps, 2 below ~35, 3 below ~23). A step is 1/20 s at most: frames slower than that for long slow the
//     game down, as on the console, instead of taking steps big enough to break the physics;
//   - holds 30 fps in the menus (title screen, pause menu, map), the events (cutscenes, talks: some of their waits
//     count frames) and while a movie plays (movie.c);
//   - steps the controllers' hold counters by the frame's length (CONTROLLER_STEP).
// Switches:
//   LARECOMP_FPS=<n>             frame rate instead of the setting (0: no limit)
//   LARECOMP_FRAMEPACE=0         the game's own pacing and clock (its 30 / 20 fps, as before)
//   LARECOMP_MOVIE_FPS / LARECOMP_MENU_FPS / LARECOMP_EVENT_FPS=<n>  the frame rate there (default 30; 0: the chosen one)
//   LARECOMP_LOG_FRAMEPACE=<n>   every n clock updates: target, frame length, VFR ratio and interval
//   LARECOMP_NO_CONTROLLER_STEP=1  the controllers count presented frames (as the game does)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"
#include "../settings.h"
#include "../framepace.h"

#ifdef LARECOMP_NO_FFMPEG
static volatile int g_movie_playing;  // (no movie.c without FFmpeg: no movies)
#else
extern volatile int g_movie_playing;  // runtime/hle/movie.c: video decoders running
#endif

// a canonical address of main (0x7000000000 + offset) where the guest window is: 1.8.2's, or 1.9.0's (the same code
// elsewhere: tools/port_addrs.py found each one by the instructions that reach it)
#define GA(a) ((uint64_t)(a) - 0x7000000000ull + GUEST_LO)
#define GV(a182, a190) GA(g_game_version == 190 ? (a190) : (a182))
#define VF(T, base, off) (*(T *)(uintptr_t)((base) + (off)))
#define PAUSE_STATE GV(0x7001cb48a4ull, 0x7001e1611cull)  // (u32: 0 playing, 2 in the pause menu or the map)
#define UI_SCREENS GV(0x7001c05d88ull, 0x7001d66d58ull)

static int env_int(const char *name, int def) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

bool framepace_on(void) {
    static int on = -1;
    if (on < 0) on = g_game_version >= 182 && env_int("LARECOMP_FRAMEPACE", 1) != 0;
    return on;
}

// ------------------------------------------------------------------ the frame rate
// The menus: the game is paused (pause menu, map), or the title screen's menu is up (screen 1 in the UI screen
// manager [0x7001c05d88]: +0x28 count, +0x30 screens by id).
static bool in_menu(void) {
    if (*(uint32_t *)(uintptr_t)PAUSE_STATE) return true;
    uint64_t mgr = *(uint64_t *)(uintptr_t)*(uint64_t *)(uintptr_t)UI_SCREENS;
    return mgr && VF(uint32_t, mgr, 0x28) > 1 && VF(uint64_t, mgr, 0x30) &&
           *(uint64_t *)(uintptr_t)(VF(uint64_t, mgr, 0x30) + 8);
}

// (widescreen.c: the HUD stays in place in the menus)
bool framepace_in_menu(void) { return g_game_version >= 182 && in_menu(); }

// (widescreen.c) the title screen's menu is up - screen 1 in the UI screen manager, as in_menu - and the game isn't paused
bool framepace_on_title(void) {
    if (g_game_version < 182) return false;
    uint64_t holder = *(uint64_t *)(uintptr_t)UI_SCREENS;
    uint64_t mgr = holder ? *(uint64_t *)(uintptr_t)holder : 0;
    bool on = mgr && !*(uint32_t *)(uintptr_t)PAUSE_STATE && VF(uint32_t, mgr, 0x28) > 1 && VF(uint64_t, mgr, 0x30) &&
              *(uint64_t *)(uintptr_t)(VF(uint64_t, mgr, 0x30) + 8);
    static int last = -1;
    if (getenv("LARECOMP_LOG_TITLE") && on != last) {
        last = on;
        LOG("[framepace] title screen %d (screens %u, menu %d)\n", on, mgr ? VF(uint32_t, mgr, 0x28) : 0, in_menu());
    }
    return on;
}

// An event is running: a cutscene, a talk, an item fanfare (ksys::evt::Manager [0x7001cd8db8]: its active event at
// +0x1d2b8). Some of their waits count frames: the new game's opening gave Link control 2 s early at 60 fps.
static bool in_event(void) {
    uint64_t evt = *(uint64_t *)(uintptr_t)GV(0x7001cd8db8ull, 0x7001e3a638ull);
    return evt && *(uint64_t *)(uintptr_t)(evt + 0x1d2b8);
}

int framepace_target(void) {
    static int forced = -2, movie = -2, menu = -2, event = -2;
    if (forced == -2) forced = env_int("LARECOMP_FPS", -1);
    if (movie == -2) movie = env_int("LARECOMP_MOVIE_FPS", 30);
    if (menu == -2) menu = env_int("LARECOMP_MENU_FPS", 30);
    if (event == -2) event = env_int("LARECOMP_EVENT_FPS", -1);  // (-1: the setting - Cutscenes at 60 fps, else 30)
    // (Video > Frame rate: 20, 30, 40 / 45, 60; Auto is 60 here - the clock follows whatever the frames really take.
    // 40 / 45: whichever the screen shows evenly, vulkan/device.c vk_between_fps - the game in its 60 fps way)
    extern int vk_between_fps(void);
    int fps = forced >= 0 ? forced : g_settings.fps_mode == 1 ? 30 : g_settings.fps_mode == 3 ? 20
            : g_settings.fps_mode == 4 ? vk_between_fps() : 60;
    if (movie > 0 && __atomic_load_n(&g_movie_playing, __ATOMIC_RELAXED) > 0 && (fps == 0 || fps > movie)) fps = movie;
    if (menu > 0 && (fps == 0 || fps > menu) && in_menu()) fps = menu;
    int ev = event >= 0 ? event : g_settings.event_fps_unlock ? 0 : 30;
    if (ev > 0 && (fps == 0 || fps > ev) && in_event()) fps = ev;
    return fps;
}

// No limit still stops at 300 fps: shorter steps lose precision in the game's float clocks (the time of day, 0..360,
// takes 1/30 of a minute times the step each frame).
#define FPS_CEILING 300

uint64_t framepace_period_ns(void) {
    int fps = framepace_target();
    return 1000000000ull / (uint64_t)(fps > 0 ? fps : FPS_CEILING);
}

// The frame loop's vsync wait: the present keeps the pace instead (1: skipped; 0: the game's own wait).
int rt_frame_wait(void) { return framepace_on(); }

// ------------------------------------------------------------------ presents
// The controllers (sead::ControllerBase::updateDerivativeParams_, 0x7000d9d8a0) count how long each button has been
// held, in frames, by a step the game keeps at 1 (a float at 0x7001c4ba40, read there only): the runtime sets it to
// the frame's length in console frames, so holds last as long as on the console.
#define CONTROLLER_STEP GV(0x7001c4ba40ull, 0x7001dad0e0ull)
static float g_step = 1;   // the last frame's length between presents, in console frames
static int g_grid_streak;  // presents in a row that kept the pace

void framepace_presented(uint64_t t, bool on_grid) {
    if (!framepace_on()) return;
    static uint64_t prev;
    static double smooth;
    double s = prev && t > prev ? (t - prev) / 1e9 : 1.0 / 30;
    prev = t;
    if (s > 0.25) s = 0.25;
    smooth = smooth > 0 ? smooth + (s - smooth) * 0.25 : s;
    float step = (float)((on_grid ? s : smooth) * 30);
    g_step = step < 0.1f ? 0.1f : step > 1.5f ? 1.5f : step;
    g_grid_streak = on_grid ? g_grid_streak + 1 : 0;
    static int fixed = -1;
    if (fixed < 0) fixed = env_int("LARECOMP_NO_CONTROLLER_STEP", 0);
    if (!fixed) *(float *)(uintptr_t)CONTROLLER_STEP = g_step;
}

// ------------------------------------------------------------------ the game's clock (ksys::VFR)
// VFR fields (1.8.2; the layout of zeldaret's ksys::VFR): +0x28 interval changed, +0x29 interval override on,
// +0x30 new interval ratio, +0x34 interval, +0x3c interval ratio.
#define VFR_CHANGED 0x28
#define VFR_OVERRIDE 0x29
#define VFR_NEW_RATIO 0x30
#define VFR_INTERVAL 0x34
#define VFR_RATIO 0x3c

// The clock's interval (1 / 2 / 3: its 60 / 30 / 20 fps ways), as last set: the frame loop's follows it at 3
// (nvn/objects.c nvnWindowSetPresentInterval)
static uint32_t g_vfr_iv = 2;
uint32_t framepace_interval(void) { return g_vfr_iv; }

void rt_vfr_update(uint64_t vfr) {
    if (!framepace_on()) return;
    static uint64_t last_t;
    static double real, given;  // seconds of real time and of game time handed out (only their gap matters)
    static double smooth;       // seconds between updates, averaged over about 20
    static double last_step;    // the step handed out last, seconds (0: none yet)
    static int last_target = -1;
    static double slow = 1;     // the step in console frames, averaged over about 20 (the interval follows it)
    static uint32_t last_iv;
    static int log_every = -1;
    static uint64_t nframe;
    if (log_every < 0) log_every = env_int("LARECOMP_LOG_FRAMEPACE", 0);
    uint64_t now = plat_now_ns();
    double s = last_t && now > last_t ? (now - last_t) / 1e9 : 1.0 / 30;
    last_t = now;
    float ratio;
    bool paused = *(uint32_t *)(uintptr_t)PAUSE_STATE != 0;
    if (paused) {
        // paused (it updates twice a frame): the frame's length between presents, nothing owed
        ratio = g_step;
        real = given = 0;
        last_target = -1;
    } else {
        if (s > 0.25) s = 0.25;  // (a load: not owed)
        double period = framepace_period_ns() / 1e9;
        int target = framepace_target();
        if (target != last_target) smooth = period, last_step = 0;  // (a new pace: start from it)
        last_target = target;
        // Frame rate changed in the menu: the interval goes to the new one at once - its average took dozens of frames,
        // the frame loop's interval (nvn/objects.c, following this one) and the clock's ratio not matching meanwhile,
        // and Link sank into the ground going from 20 to 30 fps (2026-11-02)
        static int last_mode = -1;
        if (g_settings.fps_mode != last_mode) {
            if (last_mode >= 0) slow = period * 30;
            last_mode = g_settings.fps_mode;
        }
        smooth += (s - smooth) * 0.05;
        real += s;
        // The step changes slowly: the game reads some deltas a frame late, and a step that jumps between frames
        // moves the camera and Link apart. One period once the presents have kept the pace for half a second, else
        // the average of ~20 frames; plus a fiftieth of what is owed; and at most 3% away from the last step.
        double step = (g_grid_streak >= 30 ? period : smooth) + (real - given) * 0.02;
        if (last_step > 0 && step > last_step * 1.03) step = last_step * 1.03;
        if (last_step > 0 && step < last_step * 0.97) step = last_step * 0.97;
        if (step < 1.0 / FPS_CEILING) step = 1.0 / FPS_CEILING;
        if (step > 0.05) step = 0.05;
        last_step = step;
        ratio = (float)(step * 30);
    }
    bool held = VF(uint8_t, vfr, VFR_OVERRIDE) && ratio > 1;  // (stage loading: the game asks for single steps)
    if (held) ratio = 1;
    if (!paused) {
        given += ratio / 30.0;
        if (held || fabs(real - given) > 0.25) real = given;  // (nothing owed for a deliberate slow down)
    }
    // The interval switches some of the game's code between its 60 fps and 30 fps ways: only on a lasting change of
    // the frame rate, never back and forth from one frame to the next.
    slow += (ratio - slow) * 0.05;
    uint32_t iv = g_vfr_iv;
    // (Frame rate 40 / 45: the game's 60 fps way, interval 1, whatever the ratio - at 40 (0.75) the rule below kept 2
    // while the frame loop ran at 1 (nvn/objects.c), the mismatch that sank Link at 20 fps)
    if (g_settings.fps_mode == 4) iv = 1, slow = ratio;
    else if (iv == 1 && slow > 0.85) iv = 2;
    else if (iv == 2 && slow < 0.7) iv = 1;
    else if (iv == 2 && slow > 1.3) iv = 3;
    else if (iv == 3 && slow < 1.15) iv = 2;
    VF(float, vfr, VFR_RATIO) = ratio;
    VF(float, vfr, VFR_NEW_RATIO) = ratio;
    VF(uint32_t, vfr, VFR_INTERVAL) = iv;
    VF(uint8_t, vfr, VFR_CHANGED) = last_iv && iv != last_iv;
    last_iv = g_vfr_iv = iv;
    if (log_every > 0 && nframe++ % (uint64_t)log_every == 0)
        LOG("[framepace] target %d fps, %.2f ms since the last update (avg %.2f), ratio %.3f, interval %u, owed "
            "%.2f ms%s%s\n", framepace_target(), s * 1e3, smooth * 1e3, ratio, iv, (real - given) * 1e3,
            paused ? ", paused" : "", g_movie_playing > 0 ? ", movie" : "");
}
