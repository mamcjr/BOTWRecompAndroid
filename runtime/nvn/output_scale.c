// Output resolution, shared by the Metal and Vulkan backends (each calls output_scale() once per presented frame).
// BOTW draws its 3D scene into targets of 1600x900 (docked; its own dynamic resolution goes down to 1280x720) and
// composes it into its final 1920x1080 image with the HUD. The final image's targets are allocated at (sx, sy) times
// their size - the window's resolution, so the HUD is sharp - and the scene's at (scene_sx, scene_sy) times theirs
// (vulkan/textures.c RS_OUT / RS_SCENE); the image is then drawn into (vx, vy, vw, vh) of the window. Video tab
// "Internal resolution" (g_settings.res_mode) sets the scene's, "Output resolution" (g_settings.out_lines) caps the
// final image's (else the window's):
//   Match display  the scene at the size it takes in the final image
//   1x .. 4x       N times the Switch's docked scene: 900 lines x N; capped to the GPU's 16384-pixel texture limit
//   .75x / .50x / .25x  of Match display
//   Dynamic        Match display, lowered in steps while the GPU can't hold the frame rate and raised again when it
//                  can (each change reallocates the render targets: a short hitch, so it changes rarely, with hysteresis)
// Test overrides: LARECOMP_SCALE=<f> (the scene's height = 900 x f), LARECOMP_WIDE_TEST=<f> (forced shape).
#include <math.h>
#include <stdlib.h>

#include "../hle.h"
#include "../settings.h"
#include "output_scale.h"
#include "../framepace.h"

#define MAX_TEX 16384.0
#define SCENE_LINES 900.0  // (BOTW's docked 3D scene: 1600x900)

// Dynamic resolution: a factor on the Match display scale, 0.5 .. 1 in 0.05 steps.
static double g_dyn = 1.0;
static double g_dyn_gpu_sum, g_dyn_dt_sum, g_last_t;
static int g_dyn_n, g_dyn_gpu_n, g_dyn_good;
static double g_dyn_cool_until, g_dyn_probe_wait = 5.0, g_dyn_probe_at;
static double g_dyn_last_raise = -100;

static void dyn_update(double now, double gpu_ms) {
    double dt = g_last_t > 0 ? (now - g_last_t) * 1000.0 : 16.7;
    g_last_t = now;
    if (dt > 250) return;  // (loading, menus, a stall: not the GPU's steady state)
    double want = framepace_on() && framepace_target() > 0 ? 1000.0 / framepace_target() : 1000.0 / 30;  // (ms a frame)
#ifdef __ANDROID__
    // the frame rate it keeps: 60 with Frame rate at 60 fps, else 30 - Auto's 60 comes by itself where the GPU has room.
    // (Aiming at Auto's tries for 60 took the scale to its floor while the game stayed at 30: POCO F7 Ultra, 2026-10-25)
    want = g_settings.fps_mode == 2 || g_settings.fps_mode == 4 ? want : g_settings.fps_mode == 3 ? 1000.0 / 20 : 1000.0 / 30;  // (45: its 22 ms)
#endif
    g_dyn_dt_sum += dt;
    g_dyn_n++;
    if (gpu_ms >= 0) { g_dyn_gpu_sum += gpu_ms; g_dyn_gpu_n++; }
    if (g_dyn_n < 30) return;  // decide every ~half second
    double avg_dt = g_dyn_dt_sum / g_dyn_n, avg_gpu = g_dyn_gpu_n ? g_dyn_gpu_sum / g_dyn_gpu_n : -1;
    g_dyn_dt_sum = g_dyn_gpu_sum = 0;
    g_dyn_n = g_dyn_gpu_n = 0;
    if (now < g_dyn_cool_until) return;
    double prev = g_dyn;
    // With the GPU's time: slow frames lower the scale only while the GPU is busy most of them - else the CPU holds
    // them, and a smaller image doesn't help (on the POCO 36 ms frames with 25 ms of GPU kept lowering it)
    bool slow = avg_gpu >= 0 ? avg_gpu > want * 0.87 || (avg_dt > want * 1.08 && avg_gpu > want * 0.7)
                             : avg_dt > want * 1.04;
    if (slow) {
        g_dyn = fmax(0.5, g_dyn - (avg_dt > want * 1.3 ? 0.1 : 0.05));
        g_dyn_good = 0;
        if (now - g_dyn_last_raise < 3.0) g_dyn_probe_wait = fmin(g_dyn_probe_wait * 2, 60.0);  // raised too eagerly
    } else if (g_dyn < 1.0) {
        g_dyn_good++;
        // with GPU timing: raise when there is clear headroom; without (Vulkan): probe upwards now and then
        // (with it too, a raise that had to be taken back soon waits longer before the next: g_dyn_probe_wait - the
        // POCO swung 0.55 <-> 0.70 every couple of seconds, each change reallocating the targets)
        bool room = avg_gpu >= 0 ? avg_gpu < want * 0.63 && g_dyn_good >= 4 && now >= g_dyn_probe_at
                                 : now >= g_dyn_probe_at && g_dyn_good >= 2 * (int)g_dyn_probe_wait;
        if (room) { g_dyn = fmin(1.0, g_dyn + 0.05); g_dyn_good = 0; g_dyn_last_raise = now; g_dyn_probe_at = now + g_dyn_probe_wait; }
    }
    g_dyn = round(g_dyn * 20) / 20;
    if (g_dyn != prev) {
        g_dyn_cool_until = now + 1.5;
        LOG("[dynres] %.2f -> %.2f (frame %.1f ms, gpu %.1f ms)\n", prev, g_dyn, avg_dt, avg_gpu);
    }
}

double output_scale_dynamic_factor(void) { return g_dyn; }

void output_scale(double dw, double dh, bool reshape_allowed, double now, double gpu_ms, OutScale *o) {
    double fit = fmin(dw / 1920.0, dh / 1080.0);
    static double forced = -1, wide_test = -1;
    if (forced < 0) forced = getenv("LARECOMP_SCALE") ? atof(getenv("LARECOMP_SCALE")) : 0;
    if (wide_test < 0) wide_test = getenv("LARECOMP_WIDE_TEST") ? atof(getenv("LARECOMP_WIDE_TEST")) : 0;
    double shape = wide_test > 0 ? wide_test : (dw / dh) / (16.0 / 9.0);
    bool reshape = reshape_allowed && fabs(shape - 1.0) > 0.005;
    if (!reshape) shape = 1.0;
    // the final image: the window's resolution, in units of the Switch's 1080 lines
    double h = fmax(reshape ? dh / 1080.0 : fit, 0.1);
    double sx = h * shape, sy = h, h_screen = h;
    bool out_auto = false;
    {   // Video tab "Output resolution" (g_settings.out_lines): the final image at most that many lines, scaled up to the
        // window when presented - the game's last passes and the scene's upscale over fewer pixels, the HUD softer
        // (LARECOMP_OUT_LINES=<n> for tests: the same, over the setting)
        static double env_lines = -1;
        if (env_lines < 0) env_lines = getenv("LARECOMP_OUT_LINES") ? atof(getenv("LARECOMP_OUT_LINES")) : 0;
        out_auto = env_lines <= 0 && g_settings.out_lines == OUT_AUTO;  // (Auto: 1080 lines at most)
        double out_lines = env_lines > 0 ? env_lines : out_auto ? 1080 : g_settings.out_lines;
        if (out_lines > 0 && sy * 1080.0 > out_lines) { double k = out_lines / (sy * 1080.0); sx *= k; sy *= k; }
    }
    h = sy;  // (Match display below: the final image's size, which is the screen's unless Output resolution is lower)
    double cap = fmin(MAX_TEX / (1920.0 * sx), MAX_TEX / (1080.0 * sy));
    if (cap < 1.0) { sx *= cap; sy *= cap; }
    o->sx = sx; o->sy = sy; o->shape = shape;
    // the 3D scene, in units of its own 900 lines: the size it takes in the final image (Match display), or N times
    // the Switch's
    double hs;
    int mode = g_settings.res_mode;
    if (forced > 0) hs = forced;
    else if (mode >= 1 && mode <= 8) hs = mode;
    else {
        hs = h * 1080.0 / SCENE_LINES;  // Match display
        // (Auto output: the shares below are of the screen's lines, as they were before it - only the final image is
        // smaller - and at most the scene's size in it: 50% on a 1440-line screen stays 720 lines, 75% is 1080)
        double base = out_auto ? h_screen * 1080.0 / SCENE_LINES : hs, share = 1.0;
        if (mode == RES_DYNAMIC) { dyn_update(now, gpu_ms); share = g_dyn; }
        else if (mode == RES_Q75) share = 0.75;
        else if (mode == RES_Q25) share = 0.25;
        else if (mode == RES_Q50) share = 0.5;
        if (mode != RES_MATCH) hs = fmin(base * share, hs);
    }
    hs = fmax(hs, 0.1);
    o->scene_sx = hs * shape; o->scene_sy = hs;
    double scap = fmin(MAX_TEX / (1920.0 * o->scene_sx), MAX_TEX / (1080.0 * o->scene_sy));
    if (scap < 1.0) { o->scene_sx *= scap; o->scene_sy *= scap; }
    // where the image goes in the window: fills it when reshaped, else a fitted 16:9 box
    if (reshape) { o->vw = dw; o->vh = dh; }
    else { o->vw = 1920.0 * fit; o->vh = 1080.0 * fit; }
    if (wide_test > 0) {
        double f2 = fmin(dw / (1920.0 * wide_test), dh / 1080.0);
        o->vw = 1920.0 * wide_test * f2; o->vh = 1080.0 * f2;
    }
    o->vx = (dw - o->vw) / 2; o->vy = (dh - o->vh) / 2;
}
