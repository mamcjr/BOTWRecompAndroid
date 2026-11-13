// Performance overlay on Android (the in-game menu's Show FPS, ui/menu.mm; on by default there): Swap2-Native's
// (android/.../PerformanceOverlay.java), drawn the same, on the same canvas units. A double tap on it switches the two
// modes (ui/touch_controls.mm), a finger drags it anywhere on the screen (g_settings.overlay_pos):
// - Compact, one line (520 x 34; 600 with the TURNIP badge): the frame rate (with frame generation, the game's > the
//   shown), the frame time, the app's RAM, the SoC's and the battery's temperature.
// - Detailed (270 x 286): the GPU's maker (or TURNIP) and name, the frame rate and time, charts of the frame rate, frame
//   time and RAM (96 samples of 500 ms: 48 s), the GPU's time a frame, the CPU clusters' clock caps, the temperatures
//   and battery level, the phone's free memory and Android's thermal state.
// Settings (Video): size 40-150 % (75 % Swap2's), opacity 20-100 %, and a switch per item (g_settings.overlay_*): an
// item switched off reads "--", as there. The readings: runtime/platform/android.c plat_device_stats (the battery and
// thermal state from the activity, reportDeviceStats), the CPU caps from cpufreq (each cluster's scaling_max_freq),
// frame generation from vulkan/lsfg/lsfg_present.c.
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"

extern "C" {
#include "platform.h"
#include "../settings.h"
const char *vk_gpu_desc(void);  // vulkan/device.c: "<device> - <driver>"
double vk_frame_gpu_ms(void);   // vulkan/exec.c
uint64_t renderer_present_count(void);  // vulkan/device.c: the game's frames
int vk_fg_status(const char **why);     // vulkan/lsfg/lsfg_present.c: 1 generating, 2 on and waiting, 0 off, -1 unavailable
}

extern ImFont *g_overlay_font;  // ui/menu.mm: Android's Roboto (Swap2-Native's draws with Android's sans-serif)
extern float g_screen_dp;       // ui/menu.mm: the screen's px a dp, and its height in pixels
extern int g_screen_h_px;

// Where it was last drawn (pixels: x, y, w, h): ui/touch_controls.mm drags it with a finger (g_settings.overlay_pos)
float g_perf_rect[4];

static void place(ImVec2 display, ImVec2 size) {  // its saved place, kept on the screen
    ImVec2 p(display.x - size.x - 16, 16);  // (Swap2's: the top right)
    if (g_settings.overlay_pos[0] >= 0 && g_settings.overlay_pos[1] >= 0)
        p = ImVec2(g_settings.overlay_pos[0] * display.x, g_settings.overlay_pos[1] * display.y);
    p.x = fminf(fmaxf(p.x, 0), fmaxf(display.x - size.x, 0));
    p.y = fminf(fmaxf(p.y, 0), fmaxf(display.y - size.y, 0));
    ImGui::SetNextWindowPos(p);
    ImGui::SetNextWindowSize(size);
}

// The CPU clusters' caps (cpufreq policies, read once, in order): kHz, 0 unreadable
#define NCL 4
static struct { char dir[64]; long cap_khz; } cl[NCL];
static int ncl = -1;
static long read_long(const char *path) {
    FILE *f = fopen(path, "re");
    long v = -1;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1) v = -1;
        fclose(f);
    }
    return v;
}
static void read_caps(void) {
    if (ncl < 0) {
        ncl = 0;
        for (int p = 0; p < 16 && ncl < NCL; p++) {
            char path[96];
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq", p);
            if (read_long(path) <= 0) continue;
            snprintf(cl[ncl].dir, sizeof cl[ncl].dir, "/sys/devices/system/cpu/cpufreq/policy%d", p);
            ncl++;
        }
    }
    for (int i = 0; i < ncl; i++) {
        char path[96];
        snprintf(path, sizeof path, "%s/scaling_max_freq", cl[i].dir);
        cl[i].cap_khz = read_long(path);
    }
}

static long mem_total_mb(void) {
    static long t = -1;
    if (t < 0) {
        long kb = 0;
        FILE *f = fopen("/proc/meminfo", "re");
        if (f) {
            if (fscanf(f, "MemTotal: %ld kB", &kb) != 1) kb = 0;
            fclose(f);
        }
        t = kb >> 10;
    }
    return t;
}

// Drawing on Swap2's canvas: o its top left, u pixels a unit; text at a baseline, as Android's drawText
static ImDrawList *dl;
static ImFont *font;
static ImVec2 o;
static float u;
static ImU32 argb(uint32_t c) { return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24); }
static void rect(float x0, float y0, float x1, float y1, float r, uint32_t c) {
    dl->AddRectFilled(ImVec2(o.x + x0 * u, o.y + y0 * u), ImVec2(o.x + x1 * u, o.y + y1 * u), argb(c), r * u);
}
static float text_w(const char *s, float size) { return font->CalcTextSizeA(size * u, FLT_MAX, 0, s).x / u; }
static void text(const char *s, float x, float y, float size, uint32_t c) {
    float px = size * u;
    dl->AddText(font, px, ImVec2(o.x + x * u, o.y + y * u - font->Ascent * px / font->FontSize), argb(c), s);
}
static void badge(const char *label, float x, float y, float w, uint32_t fill, uint32_t c) {
    rect(x, y, x + w, y + 20, 5, fill);
    text(label, x + 6, y + 14, 10, c);
}
static const char *number(char *b, float n) {  // one decimal, "--" when unknown
    if (isfinite(n)) snprintf(b, 16, "%.1f", n);
    else strcpy(b, "--");
    return b;
}

#define HN 96  // chart samples (every 500 ms: 48 s)
static float hist[3][HN];
static int hcount, hnext;
static void chart(int metric, float y, uint32_t color, const char *label) {
    rect(12, y, 258, y + 34, 4, 0xFF1B293D);
    float mx = 1;
    for (int i = 0; i < hcount; i++)
        if (isfinite(hist[metric][i])) mx = fmaxf(mx, hist[metric][i]);
    ImVec2 pts[HN];
    int n = 0;
    for (int i = 0; i <= hcount; i++) {
        float v = i < hcount ? hist[metric][(hnext - hcount + i + HN) % HN] : NAN;
        if (isfinite(v)) {
            pts[n++] = ImVec2(o.x + (14 + 242 * i / 95.0f) * u, o.y + (y + 32 - 28 * v / mx) * u);
        } else {
            if (n > 1) dl->AddPolyline(pts, n, argb(color), 0, 1.3f * u);
            n = 0;
        }
    }
    text(label, 17, y + 11, 8, 0xFFAFC0D6);
}

void perf_overlay_draw(ImVec2 display, float scale) {
    // the readings, twice a second (Swap2's samples)
    static uint64_t next_ns, t0, game0, last_sample;
    static int frames;
    static float shown = NAN, game_fps = NAN, gpu_ms = NAN;
    static int fg = 0;
    static PlatDeviceStats ds;
    uint64_t now = SDL_GetTicksNS();
    int items = g_settings.overlay_items;
    frames++;
    if (now >= next_ns) {
        uint64_t g = renderer_present_count();
        if (t0) {
            double dt = (now - t0) / 1e9;
            shown = (float)(frames / dt);
            game_fps = (float)((g - game0) / dt);
        }
        game0 = g, frames = 0, t0 = now;
        next_ns = now + 500000000ull;
        double gm = vk_frame_gpu_ms();
        gpu_ms = gm > 0 ? (float)gm : NAN;
        plat_device_stats(&ds);
        read_caps();
        fg = vk_fg_status(NULL);
        bool gen = fg == 1 || fg == 2;
        float fps = gen ? game_fps : shown;
        if (last_sample && now - last_sample > 2000000000ull) hcount = hnext = 0;  // (hidden a while: a new history)
        last_sample = now;
        hist[0][hnext] = items & OVERLAY_FPS ? fps : NAN;
        hist[1][hnext] = items & OVERLAY_FPS && fps > 0 ? 1000.0f / fps : NAN;
        hist[2][hnext] = items & OVERLAY_MEMORY ? (float)ds.rss_mb : NAN;
        hnext = (hnext + 1) % HN;
        if (hcount < HN) hcount++;
    }

    // what is shown (an item switched off reads "--")
    bool gen = fg == 1 || fg == 2;
    float fps = NAN, presented = NAN, ms = NAN, gms = NAN, soc = NAN, bat = NAN;
    int level = -1, thermal = -1;
    long rss = -1, total = 0, avail = 0;
    char cpu[64] = "--";
    if (items & OVERLAY_FPS) {
        fps = gen ? game_fps : shown, presented = shown;
        ms = fps > 0 ? 1000.0f / fps : NAN;
    }
    if (items & OVERLAY_GPU) gms = gpu_ms;
    if (items & OVERLAY_MEMORY) rss = ds.rss_mb, total = mem_total_mb(), avail = ds.avail_mb;
    if (items & OVERLAY_BATTERY) {
        soc = ds.soc_temp_c > 0 ? ds.soc_temp_c : NAN, bat = ds.batt_temp_c > 0 ? ds.batt_temp_c : NAN;
        level = ds.batt_pct >= 0 ? (int)lroundf(ds.batt_pct) : -1;
    }
    if (items & OVERLAY_THROTTLE) thermal = ds.thermal;
    if (items & OVERLAY_CPU) {
        int n = 0;
        for (int i = 0; i < ncl; i++)
            if (cl[i].cap_khz > 0) n += snprintf(cpu + n, sizeof cpu - n, "%s%ld", n ? " / " : "", cl[i].cap_khz / 1000);
        if (n) snprintf(cpu + n, sizeof cpu - n, " MHz");
    }

    // the GPU: its name, maker and driver (Turnip, Mesa's, is marked: it changes what runs)
    static char gpu[96];
    static const char *brand = "GPU";
    static bool turnip;
    if (!gpu[0]) {
        const char *d = vk_gpu_desc();
        const char *sep = strstr(d, " - ");
        if (d[0]) {
            snprintf(gpu, sizeof gpu, "%.*s", sep ? (int)(sep - d) : (int)strlen(d), d);
            turnip = sep && strstr(sep, "Turnip");
            char low[96];
            for (int i = 0; i < (int)sizeof low; i++) low[i] = (char)tolower((unsigned char)gpu[i]);
            brand = strstr(low, "adreno") ? "QUALCOMM" : strstr(low, "mali") || strstr(low, "immortalis") ? "ARM"
                    : strstr(low, "nvidia") ? "NVIDIA" : strstr(low, "xclipse") ? "SAMSUNG" : "GPU";
        }
    }

    bool detail = g_settings.overlay_style == 0;
    float size = (g_settings.overlay_scale >= 40 && g_settings.overlay_scale <= 150 ? g_settings.overlay_scale : 75) / 100.0f;
    float alpha = (g_settings.overlay_opacity >= 20 && g_settings.overlay_opacity <= 100 ? g_settings.overlay_opacity : 100) / 100.0f;
    char a[16], b[16], s[160], f[48];
    if (isfinite(presented) && presented > fps + 1) snprintf(f, sizeof f, "%s > %s FPS", number(a, fps), number(b, presented));
    else snprintf(f, sizeof f, "%s FPS", number(a, fps));
    char ram[24];
    if (rss < 0) strcpy(ram, "--");
    else snprintf(ram, sizeof ram, "%ld MB", rss);

    font = g_overlay_font ? g_overlay_font : ImGui::GetFont();
    // compact: Swap2's items, each as wide as its widest reading (so the panel keeps its size) and a gap apart
    static const float CGAP = 14;
    char cs[5][48];
    float cw[5], csz[5] = {15, 13, 13, 13, 13};
    static const uint32_t ccol[5] = {0xFF6DE6EF, 0xFFFFFFFF, 0xFFE3EAF5, 0xFFFFD477, 0xFFFFD477};
    const char *widest[5] = {gen ? "88.8 > 88.8 FPS" : "88.8 FPS", "888.8 ms", "RAM 8888 MB", "SoC 88.8\xc2\xb0", "BAT 88.8\xc2\xb0"};
    snprintf(cs[0], sizeof cs[0], "%s", f);
    snprintf(cs[1], sizeof cs[1], "%s ms", number(a, ms));
    snprintf(cs[2], sizeof cs[2], "RAM %s", ram);
    snprintf(cs[3], sizeof cs[3], "SoC %s\xc2\xb0", number(a, soc));
    snprintf(cs[4], sizeof cs[4], "BAT %s\xc2\xb0", number(a, bat));
    float cx = 12;
    for (int i = 0; i < 5; i++) {
        cw[i] = fmaxf(text_w(cs[i], csz[i]), text_w(widest[i], csz[i]));
        cx += cw[i] + CGAP;
    }
    float W = detail ? 270 : cx - CGAP + 12 + (turnip ? 66 + CGAP - 4 : 0), H = detail ? 286 : 34;
    // a unit is a dp, as Swap2's (this draws at the swapchain's size, the screen's scaled by display.y / its height)
    u = (g_screen_dp > 0 ? g_screen_dp : 2.75f) * size * (g_screen_h_px > 0 ? display.y / g_screen_h_px : scale);
    u = fminf(u, fminf(display.x / W, display.y / H));
    place(display, ImVec2(W * u, H * u));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##perf", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    dl = ImGui::GetWindowDrawList();
    o = ImGui::GetWindowPos();
    g_perf_rect[0] = o.x, g_perf_rect[1] = o.y, g_perf_rect[2] = W * u, g_perf_rect[3] = H * u;

    rect(0, 0, W, H, 10, 0xE6101829);
    rect(0, 8, 3, H - 8, 2, 0xFF50DBEB);
    if (!detail) {
        float x = 12;
        for (int i = 0; i < 5; i++) {
            text(cs[i], x, 23, csz[i], ccol[i]);
            x += cw[i] + CGAP;
        }
        if (turnip) badge("TURNIP", x - 4, 7, 66, 0xFF4A2A12, 0xFFFF9F43);
    } else {
        // (the maker's name in a badge, not a recreation of its logo)
        if (turnip) badge("TURNIP", 178, 9, 82, 0xFF4A2A12, 0xFFFF9F43);
        else {
            rect(178, 9, 260, 29, 5, 0xFF203A50);
            text(brand, 184, 23, 10, 0xFF6DE6EF);
        }
        text("BOTW / MONITOR", 12, 23, 10, 0xFF92A5BC);
        char title[96];
        snprintf(title, sizeof title, "%s", gpu[0] ? gpu : "GPU");
        for (int n = (int)strlen(title); n > 1 && text_w(title, 12) > 245;) title[--n] = 0;
        text(title, 12, 47, 12, 0xFFFFFFFF);
        text(f, 12, 72, 19, 0xFF6DE6EF);
        snprintf(s, sizeof s, "%s ms", number(a, ms));
        text(s, 175, 72, 15, 0xFFFFFFFF);
        chart(0, 82, 0xFF50DBEB, "FPS");
        chart(1, 123, 0xFFFFCE78, "ms");
        chart(2, 164, 0xFFA699FF, "RAM MB");
        snprintf(s, sizeof s, "GPU %s ms  |  RAM %s", number(a, gms), ram);
        text(s, 12, 212, 11, 0xFFE3EAF5);
        snprintf(s, sizeof s, "CPU cap %s%s", cpu, turnip ? "  |  Turnip (Mesa)" : "");
        text(s, 12, 229, 10, turnip ? 0xFFFF9F43 : 0xFFE3EAF5);
        char lv[8];
        if (level < 0) strcpy(lv, "--");
        else snprintf(lv, sizeof lv, "%d%%", level);
        snprintf(s, sizeof s, "SoC %s\xc2\xb0  BAT %s\xc2\xb0  %s", number(a, soc), number(b, bat), lv);
        text(s, 12, 246, 11, 0xFFFFD477);
        char fr[32], th[8];
        if (total <= 0) strcpy(fr, "--");
        else snprintf(fr, sizeof fr, "%.1f / %.1f GB", avail / 1024.0f, total / 1024.0f);
        if (thermal < 0) strcpy(th, "--");
        else snprintf(th, sizeof th, "%d", thermal);
        snprintf(s, sizeof s, "Free RAM %s  T:%s", fr, th);
        text(s, 12, 263, 10, 0xFFE3EAF5);
        text("500 ms samples \xc2\xb7 48 s history", 12, 279, 9, 0xFF92A5BC);
    }
    ImGui::Dummy(ImVec2(W * u, H * u));
    ImGui::End();
    ImGui::PopStyleVar(3);
}
#endif
