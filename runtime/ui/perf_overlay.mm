// Performance overlay on Android (the in-game menu's Show FPS, ui/menu.mm; on by default there), like the ones Android
// emulators show. Two modes, a double tap on it switches between them (ui/touch_controls.mm), a finger drags it anywhere
// on the screen (g_settings.overlay_pos) - LASRecompAndroid's, with Swap2-Native's overlay's items and settings:
// - Compact: one short item a line (Swap2-Native's): the throttle dot and the frame rate (with frame generation on, the
//   game's -> the shown, and "LSFG"), the frame time, the GPU's time a frame, the CPU clock caps, the battery, memory.
// - Detailed: the frame time graph, CPU clusters and this app's CPU share, ADPF, the GPU and how much of the time the CPU
//   waits on it, memory, temperatures, battery power and Android's thermal state; the same items' lines too.
// Settings (Video): size 40-150 %, opacity 20-100 %, and a switch per item (g_settings.overlay_*). The readings: runtime/
// platform/android.c plat_device_stats (the battery and thermal state from the activity, reportDeviceStats), the CPU caps
// from cpufreq (each cluster's scaling_max_freq / cpuinfo_max_freq), frame generation from vulkan/lsfg/lsfg_present.c.
//
// The throttle dot, worst of three readings: red for thermal status MODERATE or worse, thermal headroom >= 1.0 (SEVERE),
// or the main CPU cluster (the one with the most cores) capped below 65 % of its top clock; yellow for status LIGHT,
// headroom >= 0.85, or a cap below 85 %; green otherwise; grey when nothing could be read (Swap2-Native's rule).
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "imgui.h"

extern "C" {
#include "platform.h"
#include "../settings.h"
uint64_t vk_cum_wait_ns(void);  // vulkan/exec.c
const char *vk_gpu_desc(void);  // vulkan/device.c
double vk_frame_gpu_ms(void);   // vulkan/exec.c
uint64_t renderer_present_count(void);  // vulkan/device.c: the game's frames
int vk_fg_status(const char **why);     // vulkan/lsfg/lsfg_present.c: 1 generating, 2 on and waiting, 0 off, -1 unavailable
}

#define FT_N 120  // frame times in the graph

static ImVec4 grade(float v, float good, float ok) {  // green / yellow / red
    return v >= good ? ImVec4(0.45f, 0.95f, 0.45f, 1) : v >= ok ? ImVec4(1.0f, 0.85f, 0.3f, 1) : ImVec4(1.0f, 0.4f, 0.35f, 1);
}
static const ImVec4 DOT[3] = {ImVec4(0.30f, 0.69f, 0.31f, 1), ImVec4(1.0f, 0.76f, 0.03f, 1), ImVec4(0.96f, 0.26f, 0.21f, 1)};

// Where it was last drawn (pixels: x, y, w, h): ui/touch_controls.mm drags it with a finger (g_settings.overlay_pos)
float g_perf_rect[4];

static void place(ImVec2 display, float scale) {  // its saved place, kept on the screen at its last size
    ImVec2 p(16 * scale, 16 * scale);
    if (g_settings.overlay_pos[0] >= 0 && g_settings.overlay_pos[1] >= 0)
        p = ImVec2(g_settings.overlay_pos[0] * display.x, g_settings.overlay_pos[1] * display.y);
    p.x = fminf(fmaxf(p.x, 0), fmaxf(display.x - g_perf_rect[2], 0));
    p.y = fminf(fmaxf(p.y, 0), fmaxf(display.y - g_perf_rect[3], 0));
    ImGui::SetNextWindowPos(p);
}

static void record(void) {
    ImVec2 p = ImGui::GetWindowPos(), z = ImGui::GetWindowSize();
    g_perf_rect[0] = p.x, g_perf_rect[1] = p.y, g_perf_rect[2] = z.x, g_perf_rect[3] = z.y;
}

// The CPU clusters' caps (cpufreq policies, read once; the main cluster - most cores - first): kHz, 0 unreadable
#define NCL 4
static struct { char dir[64]; int cores; long max_khz, cap_khz; } cl[NCL];
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
            long mx = read_long(path);
            if (mx <= 0) continue;
            snprintf(cl[ncl].dir, sizeof cl[ncl].dir, "/sys/devices/system/cpu/cpufreq/policy%d", p);
            cl[ncl].max_khz = mx;
            snprintf(path, sizeof path, "%s/related_cpus", cl[ncl].dir);
            int cores = 0;
            FILE *f = fopen(path, "re");
            if (f) {
                int c;
                while (fscanf(f, "%d", &c) == 1) cores++;
                fclose(f);
            }
            cl[ncl].cores = cores ? cores : 1;
            ncl++;
        }
        for (int i = 0; i < ncl; i++)  // (main cluster first: most cores, then the slower one)
            for (int j = i + 1; j < ncl; j++)
                if (cl[j].cores > cl[i].cores || (cl[j].cores == cl[i].cores && cl[j].max_khz < cl[i].max_khz)) {
                    auto t = cl[i];
                    cl[i] = cl[j], cl[j] = t;
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

static int throttle_level(const PlatDeviceStats &ds) {  // -1 unknown, 0 green, 1 yellow, 2 red
    float cap = ncl > 0 && cl[0].cap_khz > 0 && cl[0].max_khz > 0 ? (float)cl[0].cap_khz / (float)cl[0].max_khz : NAN;
    if (ds.thermal < 0 && ds.headroom <= 0 && isnan(cap)) return -1;
    if (ds.thermal >= 2 || ds.headroom >= 1.0f || cap < 0.65f) return 2;
    if (ds.thermal >= 1 || ds.headroom >= 0.85f || cap < 0.85f) return 1;
    return 0;
}

static void dot(int level) {  // the throttle dot: a filled circle the height of a line (the font may lack the glyph)
    float h = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec4 c = level >= 0 ? DOT[level] : ImVec4(0.6f, 0.6f, 0.6f, 1);
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.4f, p.y + h * 0.5f), h * 0.32f, ImGui::GetColorU32(c));
    ImGui::Dummy(ImVec2(h * 0.8f, h));
}

void perf_overlay_draw(ImVec2 display, float scale) {
    // frame times: this runs once per presented frame (frame generation's frames too)
    static float ft[FT_N];
    static int nft, head;
    static uint64_t last_ns;
    uint64_t now = SDL_GetTicksNS();
    if (last_ns) {
        ft[head] = (float)((now - last_ns) / 1e6);
        head = (head + 1) % FT_N;
        if (nft < FT_N) nft++;
    }
    last_ns = now;

    // the rest twice a second
    static uint64_t next_ns, t0, wait0, game0;
    static int frames;
    static float shown, game_fps, avg_ms, max_ms, wait_pct, gpu_ms;
    static int fg = 0, level = -1;
    static PlatDeviceStats ds;
    frames++;
    if (now >= next_ns) {
        uint64_t w = vk_cum_wait_ns(), g = renderer_present_count();
        if (t0) {
            double dt = (now - t0) / 1e9;
            shown = (float)(frames / dt);
            game_fps = (float)((g - game0) / dt);
            wait_pct = (float)((w - wait0) / 1e9 / dt * 100.0);
            if (wait_pct > 100) wait_pct = 100;
        }
        wait0 = w, game0 = g;
        frames = 0;
        t0 = now;
        next_ns = now + 500000000ull;
        float s = 0, mx = 0;
        for (int i = 0; i < nft; i++) { s += ft[i]; if (ft[i] > mx) mx = ft[i]; }
        avg_ms = nft ? s / nft : 0;
        max_ms = mx;
        gpu_ms = (float)vk_frame_gpu_ms();
        plat_device_stats(&ds);
        read_caps();
        level = throttle_level(ds);
        fg = vk_fg_status(NULL);
    }

    int items = g_settings.overlay_items;
    float size = (g_settings.overlay_scale >= 40 && g_settings.overlay_scale <= 150 ? g_settings.overlay_scale : 100) / 100.0f;
    float alpha = (g_settings.overlay_opacity >= 20 && g_settings.overlay_opacity <= 100 ? g_settings.overlay_opacity : 100) / 100.0f;
    bool gen = fg == 1 || fg == 2;
    float fps = gen ? game_fps : shown;  // (the game's own rate: with frame generation the shown one is higher)
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    place(display, scale);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##perf", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(0.62f * size);
    long total = mem_total_mb();
    bool low = total > 0 && ds.avail_mb > 0 && (long)ds.avail_mb < total / 12;  // (Android's own low-memory line is near there)

    if (g_settings.overlay_style == 1) {  // compact: one short item a line (Swap2-Native's)
        bool first = true;
        if (items & OVERLAY_THROTTLE) {
            dot(level);
            first = false;
        }
        if (items & OVERLAY_FPS) {
            if (!first) ImGui::SameLine();
            if (gen) ImGui::TextColored(grade(fps, 55, 28), "%.0f->%.0f fps", fps, shown);  // (→)
            else ImGui::TextColored(grade(fps, 55, 28), "%.0f fps", fps);
            ImGui::Text("%.1f ms", fps > 0 ? 1000.0f / fps : 0.0f);
            if (gen) ImGui::Text("LSFG");
            else if (fg < 0 && g_settings.lsfg) ImGui::TextDisabled("no LSFG");
        }
        if ((items & OVERLAY_GPU) && gpu_ms > 0) ImGui::Text("GPU %.1f ms", gpu_ms);
        if (items & OVERLAY_CPU) {
            int n = 0;
            for (int i = 0; i < ncl; i++) {
                if (cl[i].cap_khz <= 0) continue;
                ImGui::Text(n++ ? "%.1f/%.1f GHz" : "CPU %.1f/%.1f", cl[i].cap_khz / 1e6, cl[i].max_khz / 1e6);
            }
            if (!n) ImGui::TextDisabled("CPU cap ?");
        }
        if ((items & OVERLAY_BATTERY) && ds.batt_pct >= 0) {
            if (ds.batt_temp_c > 0) ImGui::Text("%.0f\xc2\xb0" "C %.0f%%%s", ds.batt_temp_c, ds.batt_pct, ds.plugged > 0 ? " +" : "");
            else ImGui::Text("%.0f%%%s", ds.batt_pct, ds.plugged > 0 ? " +" : "");
        }
        if (items & OVERLAY_MEMORY) {
            ImGui::Text("App %.1f GiB", ds.rss_mb / 1024.0f);
            if (total > 0) {
                ImGui::Text("RAM %.1f/%.1f GiB", (total - (long)ds.avail_mb) / 1024.0f, total / 1024.0f);
                if (low) {
                    ImGui::SameLine();
                    ImGui::TextColored(DOT[2], "low");
                }
            }
        }
        record();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // detailed
    if (items & OVERLAY_THROTTLE) {
        dot(level);
        ImGui::SameLine();
    }
    if (items & OVERLAY_FPS) {
        if (gen) ImGui::TextColored(grade(fps, 55, 28), "%.0f -> %.0f FPS", fps, shown);
        else ImGui::TextColored(grade(fps, 55, 28), "%.0f FPS", fps);
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f ms", fps > 0 ? 1000.0f / fps : 0.0f);
        if (gen) {
            ImGui::SameLine();
            ImGui::Text(" LSFG");
        } else if (fg < 0 && g_settings.lsfg) {
            ImGui::SameLine();
            ImGui::TextDisabled(" no LSFG");
        }
        float graph[FT_N];  // (oldest first)
        for (int i = 0; i < nft; i++) graph[i] = ft[(head - nft + i + FT_N) % FT_N];
        ImGui::PushStyleColor(ImGuiCol_PlotLines, grade(fps, 55, 28));
        ImGui::PlotLines("##ft", graph, nft, 0, NULL, 0.0f, 100.0f, ImVec2(260 * scale * size, 44 * scale * size));
        ImGui::PopStyleColor();
        ImGui::TextDisabled("frame avg %.1f ms  max %.0f ms", avg_ms, max_ms);
    } else if (items & OVERLAY_THROTTLE) {
        ImGui::NewLine();
    }

    if (items & OVERLAY_CPU) {
        char cpu[96] = "";
        for (int i = 0, n = 0; i < ds.ncluster && n < (int)sizeof cpu - 8; i++)
            n += snprintf(cpu + n, sizeof cpu - n, "%s%u", i ? " / " : "", ds.cluster_mhz[i]);
        ImGui::Text("CPU %s MHz", cpu);
        ImGui::SameLine();
        ImGui::TextDisabled("app %.0f%%", ds.cpu_pct);
        char caps[96] = "";
        for (int i = 0, n = 0; i < ncl && n < (int)sizeof caps - 12; i++)
            if (cl[i].cap_khz > 0)
                n += snprintf(caps + n, sizeof caps - n, "%s%.1f/%.1f", n ? "  " : "", cl[i].cap_khz / 1e6, cl[i].max_khz / 1e6);
        if (caps[0]) ImGui::TextDisabled("    caps %s GHz", caps);

        PlatAdpfStats adpf;
        plat_adpf_stats(&adpf);
        if (adpf.state > 0) {
            ImGui::TextColored(grade(adpf.target_ms - adpf.work_ms, 3.0f, 0.0f), "ADPF on (%d thr%s)",
                               adpf.threads, adpf.sessions > 1 ? ", split" : "");
            if (adpf.errors > 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1), "err %d (%d)", adpf.errors, adpf.last_error);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("tgt %.1f ms", adpf.target_ms);
            if (adpf.render_ms > 0) {
                ImGui::TextDisabled("    game %.1f ms (cpu %d)  render %.1f ms (cpu %d)",
                                    adpf.game_ms, adpf.game_cpu, adpf.render_ms, adpf.render_cpu);
            } else {
                ImGui::TextDisabled("    game %.1f ms (cpu %d)", adpf.game_ms, adpf.game_cpu);
            }
        } else if (adpf.state == 0) {
            ImGui::TextDisabled("ADPF: init...");
        } else {
            ImGui::TextDisabled("ADPF: %s", adpf.state == -2 ? "disabled" : "unavailable");
        }
    }
    if (items & OVERLAY_GPU) {
        ImGui::Text("GPU %s", vk_gpu_desc());
        if (gpu_ms > 0) {
            ImGui::TextColored(grade(100 - wait_pct, 50, 20), "    waiting on the GPU %.0f%%", wait_pct);
            ImGui::SameLine();
            ImGui::TextDisabled("%.1f ms a frame", gpu_ms);
        } else {
            ImGui::TextColored(grade(100 - wait_pct, 50, 20), "    waiting on the GPU %.0f%%", wait_pct);
        }
    }
    if (items & OVERLAY_MEMORY) {
        ImGui::Text("RAM app %u MB", ds.rss_mb);
        ImGui::SameLine();
        if (total > 0) ImGui::TextDisabled("free %u of %ld MB", ds.avail_mb, total);
        else ImGui::TextDisabled("free %u MB", ds.avail_mb);
        if (low) {
            ImGui::SameLine();
            ImGui::TextColored(DOT[2], "low");
        }
    }
    if (items & OVERLAY_BATTERY) {
        if (ds.soc_temp_c > 0) {
            ImGui::TextColored(grade(100 - ds.soc_temp_c, 35, 20), "SoC %.0f\xc2\xb0" "C", ds.soc_temp_c);
            ImGui::SameLine();
        }
        if (ds.batt_temp_c > 0) ImGui::TextColored(grade(100 - ds.batt_temp_c, 60, 55), "Battery %.0f\xc2\xb0" "C", ds.batt_temp_c);
        else ImGui::NewLine();
        if (ds.batt_pct >= 0) {
            if (ds.watts > 0) { ImGui::Text("Power %.1f W", ds.watts); ImGui::SameLine(); }
            ImGui::TextDisabled("battery %.0f%%%s", ds.batt_pct, ds.plugged > 0 ? " +" : "");
        }
    }
    if (items & OVERLAY_THROTTLE) {
        static const char *const thermal[] = {"normal", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
        if (ds.thermal >= 0 && ds.thermal <= 6) {
            ImGui::TextColored(grade((float)(6 - ds.thermal), 6, 4), "Thermal: %s", thermal[ds.thermal]);
            if (ds.headroom > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("(headroom %.2f)", ds.headroom);
            }
        }
    }
    record();
    ImGui::End();
    ImGui::PopStyleVar();
}
#endif
