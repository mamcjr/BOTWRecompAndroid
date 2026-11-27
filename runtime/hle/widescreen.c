// Widescreen: what the renderers ask about the output's shape. With Video > Aspect ratio "Unlocked", 1.8.2 fills a
// window of another shape: its cameras' sead::PerspectiveProjection objects take the output's aspect where the game
// rebuilds their matrix and where it asks for their aspect (rt_persp_update / rt_persp_aspect below, hooked into its
// game code by tools/patches.py), so a wider view widens horizontally and a narrower landscape view opens vertically
// (Vert+, expanding fovy in rt_persp_update while fitting the 16:9 UI layout to the width). The HUD's panes nearer
// the edges move out to the screen's edges (widescreen_hud_shift / widescreen_hud_anchor in vulkan/exec.c). 1.0.0 and
// 1.6.0 have no hooks: 16:9 with bars (LARECOMP_WIDESCREEN=1 forces the wide targets there for tests);
// LARECOMP_NO_WIDESCREEN=1: never; LARECOMP_NARROW_HOR_MINUS=1: keep original vertical FOV in narrow mode.
// How the main camera was found (2026-10-23): LARECOMP_FIND_PROJ (vulkan/exec.c) found its projection in the G-buffer
// vertex shaders' first constant buffer (0x920 bytes, P at +0x70: fovy 50 degrees, 16:9, near 1, far 25000), from a
// sead::PerspectiveProjection (vtable 0x101ba4438): +8 mDirty, +9 mDeviceDirty, +0xC mMatrix, +0x4C mDeviceMatrix,
// +0x98 near, far, fovy, sin, cos, tan, +0xB0 mAspect. The game sets 16:9 again before every rebuild (writing the field
// from the render thread, LARECOMP_CAM_PROJ, didn't hold), and the lighting asks for the aspect in between (with the
// matrix alone, trees came out dark with black specks). 1.8.2's one 16:9 constant (main 0x158afc8, read at
// 0x7001381c34: rt_aspect_bits) is some other cameras', not the main one's.
// Loading screens keep 16:9 proportions via fitted layout scaling; title and scope use 16:9 crop with bars
// (widescreen_present_crop).
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../framepace.h"
#include "../hle.h"
#include "../rt.h"
#include "../settings.h"

#define NARROW_MIN (9.0f / 16.0f)

static _Atomic float g_wide = 1.0f;   // the output's shape the renderers use: its aspect divided by 16:9 (1 = normal)
static _Atomic float g_shape = 1.0f;  // the window's, whether widened or not (widescreen_set_shape: the cameras from the start)

static float load_wide(void) { return atomic_load_explicit(&g_wide, memory_order_relaxed); }
static float load_shape(void) { return atomic_load_explicit(&g_shape, memory_order_relaxed); }

void widescreen_set_factor(float f) { atomic_store_explicit(&g_wide, f, memory_order_relaxed); }
float widescreen_factor(void) { return load_wide(); }
void widescreen_set_shape(float f) { atomic_store_explicit(&g_shape, f, memory_order_relaxed); }

static bool hor_minus_env(void) { return ENV("LARECOMP_NARROW_HOR_MINUS"); }
static bool shape_vert_plus(float shape) {
    return isfinite(shape) && shape >= NARROW_MIN && shape < 0.999f && !hor_minus_env();
}
static bool shape_reshaped(float shape) {
    return isfinite(shape) && ((shape >= NARROW_MIN && shape < 0.999f) || shape > 1.001f);
}

static bool frame_recent(uint64_t now, uint64_t seen, uint64_t max_age) {
    return seen && (now <= seen || now - seen <= max_age);
}

static bool g_allowed;              // (widescreen_set_allowed: tests)
static _Atomic bool g_hooked;       // (the game code calls rt_persp_update: built with the hooks)
static _Atomic int g_boot_aspect = -1;  // (the setting this boot of the game started with)
bool widescreen_allowed(void) {
    // 1.8.2 once its game code shows the hooks (one built before them would stretch the image: it stays 16:9), with
    // the setting the game started with: the layouts' camera takes its aspect once, early on (a copy of a projection
    // built on the stack, never rebuilt), so a change applies when the game starts again (a reboot: widescreen_reset)
    int boot_aspect = atomic_load_explicit(&g_boot_aspect, memory_order_acquire);
    bool hooked = atomic_load_explicit(&g_hooked, memory_order_acquire);
    if (boot_aspect < 0 && hooked) {
        int want = g_settings.aspect;
        if (atomic_compare_exchange_strong_explicit(&g_boot_aspect, &boot_aspect, want, memory_order_acq_rel,
                                                    memory_order_acquire))
            boot_aspect = want;
    }
    bool game = g_game_version >= 182 && hooked && boot_aspect == ASPECT_UNLOCKED;
    bool test = (g_allowed || ENV("LARECOMP_WIDESCREEN")) && g_settings.aspect == ASPECT_UNLOCKED;
    return (game || test) && !ENV("LARECOMP_NO_WIDESCREEN");
}
void widescreen_set_allowed(bool on) { g_allowed = on; }
bool widescreen_narrow_vert_plus(void) { return widescreen_allowed() && shape_vert_plus(load_wide()); }

// In the world: the 3D scene was drawn a moment ago (vulkan/exec.c vk_scene_frame), or the game is paused over it
// (pause menu, map). Not on the title screen - its artwork reaches past the 16:9 part while the darker layer over it
// doesn't, an edge on each side - nor on loading screens: those keep 16:9 proportions via layout scaling and the HUD
// stays put (widescreen_hud_shift / widescreen_hud_anchor).
static bool in_world(void) {
    extern uint64_t vk_scene_frame(void);
    extern uint64_t renderer_present_count(void);
    if (framepace_on_title()) return false;
    uint64_t seen = vk_scene_frame(), now = renderer_present_count();
    return seen && ((now <= seen || now - seen < 20) || framepace_in_menu());
}

// A camera aspect at a patched site: 16:9 times the output's shape (g_wide: 1 unless widened)
uint32_t rt_aspect_bits(int site) {
    float a = 16.0f / 9.0f * (widescreen_allowed() ? load_wide() : 1.0f);
    if (ENV("LARECOMP_LOG_ASPECT")) LOG("[widescreen] camera aspect %.4f (site %d)\n", a, site);
    uint32_t u;
    memcpy(&u, &a, 4);
    return u;
}

// sead::PerspectiveProjection::doUpdateMatrix (1.8.2: 0x7000da13f4, tools/patches.py; x0 = projection, vtable entry 11):
// before near/far/tan/aspect are read, apply output shape. Narrow Vert+ scales tan(fovy/2) by 1/g_shape so horizontal
// FOV matches 16:9 while LRU-tracking orig_fovy / scaled_fovy / scaled_tan per object to prevent double-scaling and
// restore cleanly on dynamic resize back to 16:9 or ultrawide.
static _Atomic bool g_widened;  // (rt_persp_update has reshaped a camera)
static bool is_16_9(float a) { return a > 1.7767f && a < 1.7789f; }

enum { VERT_SLOTS = 256 };
typedef struct {
    uint64_t proj, last_use;
    float near_z, far_z, orig_fovy, scaled_sin, scaled_cos, scaled_tan, scaled_aspect;
} VertProj;
static pthread_mutex_t vert_mu = PTHREAD_MUTEX_INITIALIZER;
static VertProj seen_vert[VERT_SLOTS];
static uint64_t vert_tick;
static int vert_count;
static _Atomic bool vert_used;
static uint64_t log_seen[256];
static int log_nseen, log_vert_n, log_wide_n;

static _Atomic uint64_t minimap_frame, talk_frame, band_frame, scope_frame, map_frame, menu_last;

RT_ON_RESET(widescreen_reset) {
    atomic_store_explicit(&g_wide, 1.0f, memory_order_relaxed);
    atomic_store_explicit(&g_boot_aspect, -1, memory_order_release);
    atomic_store_explicit(&g_hooked, false, memory_order_release);
    atomic_store_explicit(&g_widened, false, memory_order_release);
    atomic_store_explicit(&minimap_frame, 0, memory_order_release);
    atomic_store_explicit(&talk_frame, 0, memory_order_release);
    atomic_store_explicit(&band_frame, 0, memory_order_release);
    atomic_store_explicit(&scope_frame, 0, memory_order_release);
    atomic_store_explicit(&map_frame, 0, memory_order_release);
    atomic_store_explicit(&menu_last, 0, memory_order_release);
    pthread_mutex_lock(&vert_mu);
    memset(seen_vert, 0, sizeof seen_vert);
    vert_tick = 0;
    vert_count = 0;
    memset(log_seen, 0, sizeof log_seen);
    log_nseen = log_vert_n = log_wide_n = 0;
    atomic_store_explicit(&vert_used, false, memory_order_release);
    pthread_mutex_unlock(&vert_mu);
}

static bool trig_scaled_match(const VertProj *e, const float *p) {
    return fabsf(p[3] - e->scaled_sin) < 1e-4f && fabsf(p[4] - e->scaled_cos) < 1e-4f &&
           fabsf(p[5] - e->scaled_tan) < 1e-4f;
}

void rt_persp_update(uint64_t proj) {
    atomic_store_explicit(&g_hooked, true, memory_order_release);
    if (!proj || !widescreen_allowed()) return;
    float shape = load_shape();
    float *p = (float *)(uintptr_t)(proj + 0x98);  // near, far, fovy, sin, cos, tan, aspect (+0xB0)
    if (!shape_vert_plus(shape) && atomic_load_explicit(&vert_used, memory_order_acquire)) {
        pthread_mutex_lock(&vert_mu);
        for (int i = 0; i < VERT_SLOTS; i++) {
            if (seen_vert[i].proj == proj) {
                bool trig_match = trig_scaled_match(&seen_vert[i], p);
                bool nf_match = seen_vert[i].near_z == p[0] && seen_vert[i].far_z == p[1];
                if (trig_match) {
                    float orig = (isfinite(p[2]) && p[2] > 0.01f && p[2] < 3.0f) ? p[2] : seen_vert[i].orig_fovy;
                    float half = orig * 0.5f;
                    p[3] = sinf(half);
                    p[4] = cosf(half);
                    p[5] = tanf(half);
                }
                if ((nf_match || trig_match) && fabsf(p[6] - seen_vert[i].scaled_aspect) < 0.002f) p[6] = 16.0f / 9.0f;
                seen_vert[i].proj = 0;
                if (vert_count > 0) vert_count--;
                if (vert_count == 0) atomic_store_explicit(&vert_used, false, memory_order_release);
                break;
            }
        }
        pthread_mutex_unlock(&vert_mu);
    }
    if (!shape_reshaped(shape)) return;
    if (ENV("LARECOMP_LOG_ASPECT")) {  // (each projection rebuilt, once: near, far, fovy, aspect)
        bool log_new = false;
        float nz = 0, fz = 0, deg = 0, asp = 0;
        pthread_mutex_lock(&vert_mu);
        bool found = false;
        for (int i = 0; i < log_nseen && !found; i++) found = log_seen[i] == proj;
        if (!found && log_nseen < 256) {
            log_seen[log_nseen++] = proj;
            log_new = true;
            nz = p[0], fz = p[1], deg = p[2] * 57.29578f, asp = p[6];
        }
        pthread_mutex_unlock(&vert_mu);
        if (log_new) {
            extern uint64_t renderer_present_count(void);
            LOG("[widescreen] frame %llu: projection %llx near %.2f far %.0f fovy %.1f deg aspect %.4f\n",
                (unsigned long long)renderer_present_count(), (unsigned long long)proj, nz, fz, deg, asp);
        }
    }
    if (shape_vert_plus(shape)) {
        float want_aspect = 16.0f / 9.0f * shape;
        bool is169 = is_16_9(p[6]);
        pthread_mutex_lock(&vert_mu);
        int idx = -1;
        for (int i = 0; i < VERT_SLOTS; i++) {
            if (seen_vert[i].proj == proj) { idx = i; break; }
        }
        float orig_fovy = p[2];
        bool apply = false;
        if (is169) {
            apply = true;
        } else if (idx >= 0) {
            if (fabsf(p[6] - want_aspect) < 0.002f) {
                bool same_nf = seen_vert[idx].near_z == p[0] && seen_vert[idx].far_z == p[1];
                bool trig_ok = trig_scaled_match(&seen_vert[idx], p) && fabsf(p[2] - seen_vert[idx].orig_fovy) <= 1e-4f;
                if (same_nf && !trig_ok) {
                    apply = true;
                } else if (trig_ok) {
                    seen_vert[idx].near_z = p[0];
                    seen_vert[idx].far_z = p[1];
                    seen_vert[idx].last_use = ++vert_tick;
                } else {
                    // Unscaled stack camera (e.g. native 1:1 / 4:3 matching screen aspect) reused slot with different near/far.
                    seen_vert[idx].proj = 0;
                    if (vert_count > 0) vert_count--;
                    if (vert_count == 0) atomic_store_explicit(&vert_used, false, memory_order_release);
                }
            } else if (fabsf(p[6] - seen_vert[idx].scaled_aspect) < 0.002f) {
                apply = true;
            } else {
                // Address reused by a projection with an unrelated aspect (e.g. native 1:1 / 4:3); drop stale entry.
                seen_vert[idx].proj = 0;
                if (vert_count > 0) vert_count--;
                if (vert_count == 0) atomic_store_explicit(&vert_used, false, memory_order_release);
            }
        }
        if (!apply || !isfinite(orig_fovy) || orig_fovy <= 0.01f || orig_fovy >= 3.0f) {
            pthread_mutex_unlock(&vert_mu);
            return;
        }
        float tan_new = tanf(orig_fovy * 0.5f) / shape;
        float half_new = atanf(tan_new);
        if (half_new > 1.49f) {
            half_new = 1.49f;
            tan_new = tanf(half_new);
        }
        float sin_new = sinf(half_new);
        float cos_new = cosf(half_new);
        float fovy_new = 2.0f * half_new;
        float nz = p[0], fz = p[1];
        // Leave stored fovy/mAngle (p[2]) untouched so game zoom/paraglider/cutscene blends reading fovy cannot creep.
        p[3] = sin_new;
        p[4] = cos_new;
        p[5] = tan_new;
        p[6] = want_aspect;
        atomic_store_explicit(&g_widened, true, memory_order_release);
        if (idx < 0) {
            int free_idx = -1, lru_idx = 0;
            uint64_t lru_tick = UINT64_MAX;
            for (int i = 0; i < VERT_SLOTS; i++) {
                if (!seen_vert[i].proj) { free_idx = i; break; }
                if (seen_vert[i].last_use < lru_tick) lru_tick = seen_vert[i].last_use, lru_idx = i;
            }
            if (free_idx >= 0) { idx = free_idx; vert_count++; }
            else idx = lru_idx;
        }
        seen_vert[idx] = (VertProj){proj, ++vert_tick, nz, fz, orig_fovy, sin_new, cos_new, tan_new, want_aspect};
        atomic_store_explicit(&vert_used, true, memory_order_release);
        bool do_log = ENV("LARECOMP_LOG_ASPECT") && log_vert_n < 20 && (++log_vert_n, true);
        pthread_mutex_unlock(&vert_mu);
        if (do_log)
            LOG("[widescreen] projection %llx: aspect %.4f fovy %.1f -> %.1f deg (mAngle preserved)\n",
                (unsigned long long)proj, want_aspect, orig_fovy * 57.29578f, fovy_new * 57.29578f);
        return;
    }
    if (!is_16_9(p[6])) return;
    float want = 16.0f / 9.0f * shape;
    p[6] = want;
    atomic_store_explicit(&g_widened, true, memory_order_release);
    if (ENV("LARECOMP_LOG_ASPECT")) {
        pthread_mutex_lock(&vert_mu);
        bool do_log = log_wide_n < 20 && (++log_wide_n, true);
        pthread_mutex_unlock(&vert_mu);
        if (do_log) LOG("[widescreen] projection %llx: aspect %.4f\n", (unsigned long long)proj, want);
    }
}

// sead::PerspectiveProjection::getAspect (1.8.2: 0x7000da13d8, tools/patches.py): the reshaped aspect for a 16:9 one,
// also between the game setting 16:9 again and the next rebuild (rt_persp_update), so what asks the projection for its
// aspect agrees with its matrix. The float's bits.
uint32_t rt_persp_aspect(uint64_t proj) {
    if (!proj) return 0;
    float a = *(const float *)(uintptr_t)(proj + 0xB0);
    float shape = load_shape();
    if (widescreen_allowed()) {
        float want = shape_reshaped(shape) ? 16.0f / 9.0f * shape : 16.0f / 9.0f;
        if (is_16_9(a)) {
            if (shape_reshaped(shape)) a = want;
        } else if (fabsf(a - want) >= 0.002f && atomic_load_explicit(&vert_used, memory_order_acquire)) {
            pthread_mutex_lock(&vert_mu);
            for (int i = 0; i < VERT_SLOTS; i++) {
                if (seen_vert[i].proj == proj && fabsf(a - seen_vert[i].scaled_aspect) < 0.002f) {
                    a = want;
                    break;
                }
            }
            pthread_mutex_unlock(&vert_mu);
        }
    }
    uint32_t u;
    memcpy(&u, &a, 4);
    return u;
}

// The game's cameras are reshaped (the 1.8.2 hooks above): its HUD, drawn through one, lands in the centred 16:9 part
// of a wide final image (or fitted to width in a narrow landscape one).
static bool ui_centred_wide(float wide) {
    return widescreen_allowed() && isfinite(wide) && wide != 1.0f &&
           atomic_load_explicit(&g_widened, memory_order_acquire);
}
bool widescreen_ui_centred(void) { return ui_centred_wide(load_wide()); }

// The HUD out to the screen's edges: how far a pane of the game's 1280x720 layouts moves, from its centre x in
// layout units (-640..640). The widened layout camera draws the layout in the middle 16:9 part; panes nearer its sides
// than its middle (the hearts, the location name, the minimap and its gauges) move out by that part's distance to the
// screen edge, the middle ones (dialogs, prompts) stay, a blend in between. Only in the world, not in its menus
// (pause, map), whose panes make up grids across the middle. Panes placed over things in the world (an enemy's health
// bar) move the same way.
// A menu open, or until 30 frames after: turning from one of its pages to another the game says it isn't for some
// frames - the hearts and rupees moved out to the screen's edges and the pages past its sides showed (2026-10-28;
// 8 frames weren't enough)
bool widescreen_in_menu(void) {
    extern uint64_t renderer_present_count(void);
    extern uint64_t vk_scene_frame(void);
    uint64_t now = renderer_present_count();
    if (framepace_in_menu()) atomic_store_explicit(&menu_last, now + 1, memory_order_release);
    if (ENV("LARECOMP_LOG_MENU")) {  // (a test: per frame, the game's menu flag and whether the world was drawn)
        static _Atomic uint64_t logged;
        uint64_t prev = atomic_load_explicit(&logged, memory_order_relaxed);
        if (now != prev && atomic_compare_exchange_strong_explicit(&logged, &prev, now, memory_order_relaxed,
                                                                   memory_order_relaxed))
            LOG("[widescreen] frame %llu: menu %d, world drawn at %llu\n", (unsigned long long)now, framepace_in_menu(),
                (unsigned long long)vk_scene_frame());
    }
    uint64_t last = atomic_load_explicit(&menu_last, memory_order_acquire);
    if (!last || now >= last + 30) return false;
    // The world drawn again since: the menu has closed, not turned a page (its pages don't draw the 3D scene). Waiting
    // the whole 30 frames, the HUD sat in the 16:9 middle for a second after each pause, then jumped out to the edges
    // (#261, #259). LARECOMP_MENU_WAIT_FULL=1: the old wait.
    if (!ENV("LARECOMP_MENU_WAIT_FULL") && vk_scene_frame() >= last) return false;
    return true;
}

// Playing in the world: not the title or a loading screen, no menu of the game's up (pause, map) - the touch controls'
// icon layout (ui/touch_controls.mm); the classic one stays in the menus
bool widescreen_world_play(void) { return in_world() && !widescreen_in_menu(); }

// The minimap's disc (vulkan/exec.c: its pane's shader) was drawn this frame or in the last few: the minimap is on screen.
// A talk's answers sit where it would be (x 450, y -133 to -237) while it's hidden; moved up with it, they split (#200).
void widescreen_minimap_seen(void) {
    extern uint64_t renderer_present_count(void);
    atomic_store_explicit(&minimap_frame, renderer_present_count(), memory_order_release);
}

// A talk's box (vulkan/exec.c: its pane, about 0, -235) was drawn this frame or in the last few: its rounded ends
// (x +-284) and the speaker's name stay with it instead of moving out to the screen's edges (widescreen_hud_shift; #200)
void widescreen_talk_seen(void) {
    extern uint64_t renderer_present_count(void);
    atomic_store_explicit(&talk_frame, renderer_present_count(), memory_order_release);
}

// The minimap group up at the top right while the on-screen controls are up (ui/touch_controls.mm touch_hud_top: their
// buttons sat over it) and the player is in the world with the minimap on screen: a pane of the game's 1280x720 layouts in
// its lower right (x > 290, y -140 to -315: the minimap, the time, temperature and noise meters, the Sheikah Sensor;
// 2026-11-02, LARECOMP_LOG_PANES) goes up this many layout units (y up); 0 elsewhere. vulkan/exec.c moves those panes,
// their texts and scissors. Not lower: the scope's and the camera's frames have pieces along the screen's bottom edge
// (y about -345), which went up with the minimap - a bar across the scope's view (#230, #226), the camera frame's corner
// cut in half (#220). Right after a menu (the pause, the inventory) too: widescreen_world_play waits 30 frames, and the
// minimap showed down in its usual place for a second, then jumped up (#229, #220); its own disc tells it's back.
float widescreen_hud_top(float x, float y) {
    extern bool touch_hud_top(void);
    extern uint64_t renderer_present_count(void);
    if (x < 290.0f || y > -140.0f || y < -315.0f || !touch_hud_top()) return 0;
    // Not before 1.8.2: the menus (pause, map) are told apart only on 1.8.2 and 1.9.0 (framepace.c PAUSE_STATE,
    // UI_SCREENS; tools/port_addrs.py finds no 1.6.0 counterpart), and on 1.6.0 the group stayed up over the map and
    // the menus, then didn't come back (#273)
    if (g_game_version < 182) return 0;
    uint64_t now = renderer_present_count();
    uint64_t mf = atomic_load_explicit(&minimap_frame, memory_order_acquire);
    int why = !in_world() ? 1 : framepace_in_menu() ? 2 : !frame_recent(now, mf, 3) ? 3 : 0;
    if (ENV("LARECOMP_LOG_HUDTOP")) {  // (a test: each change of the group's place, and why)
        static int last = -1;
        if (why != last) {
            static const char *const name[] = {"up", "down: not in the world", "down: a menu", "down: no minimap disc"};
            LOG("[widescreen] frame %llu: minimap group %s (pane %.0f, %.0f; disc last at frame %llu)\n",
                (unsigned long long)now, name[why], x, y, (unsigned long long)mf);
            last = why;
        }
    }
    return why ? 0 : 490.0f;
}

// A pane as wide as the screen (vulkan/exec.c full_width_pane: a loading screen's band and its black, the Game Over's
// black) was drawn this frame or in the last few: a screen of its own is up, nothing moves out. As a load ends the world
// is drawn again under the loading screen (in_world): its title and tip went out to the left edge, cut, and "Next tip"
// off the right one, for its last second (#195).
void widescreen_band_seen(void) {
    extern uint64_t renderer_present_count(void);
    atomic_store_explicit(&band_frame, renderer_present_count(), memory_order_release);
}

// Vertical edge anchor for narrow landscape screens (g < 1): fitted 16:9 UI edge groups sit 360*(1/g - 1) layout
// units inside the top/bottom edges; push perimeter world HUD groups out while keeping menus, dialogs, talks,
// loading bands, and scope intact.
static float hud_anchor_wide(float x, float y, float row, float wide) {
    extern uint64_t renderer_present_count(void);
    if (!isfinite(wide) || wide < NARROW_MIN || wide >= 0.999f || !ui_centred_wide(wide) || !in_world()) return 0;
    if (widescreen_in_menu()) return 0;
    uint64_t now = renderer_present_count();
    if (frame_recent(now, atomic_load_explicit(&band_frame, memory_order_acquire), 3)) return 0;
    if (frame_recent(now, atomic_load_explicit(&talk_frame, memory_order_acquire), 3)) return 0;
    if (frame_recent(now, atomic_load_explicit(&scope_frame, memory_order_acquire), 3)) return 0;
    float e = 360.0f * (1.0f / wide - 1.0f);
    if (y < -360.5f || y > 360.5f || fabsf(x) >= 640.0f) return 0;
    if (x >= 290.0f && y >= -315.0f && y <= -140.0f) return widescreen_hud_top(x, y) != 0 ? e : -e;
    if (y >= 260.0f && (x < -120.0f || x > 290.0f)) return e;
    if (y >= 170.0f && x < -460.0f) return e;
    if (y < -316.5f || (y < row && x > -440.0f && x < 440.0f)) return -e;
    return 0;
}
float widescreen_hud_anchor(float x, float y) { return hud_anchor_wide(x, y, -316.5f, load_wide()); }
float widescreen_hud_anchor_text(float x, float y) { return hud_anchor_wide(x, y, -290.0f, load_wide()); }

// (row: below this y the prompts' row - a pane's centre or a text's origin, widescreen_hud_shift / _text)
static float hud_shift_wide(float x, float y, float row, float wide) {
    extern uint64_t renderer_present_count(void);
    if (!ui_centred_wide(wide) || wide < 0.999f || widescreen_in_menu() || !in_world()) return 0;
    uint64_t now = renderer_present_count();
    if (frame_recent(now, atomic_load_explicit(&band_frame, memory_order_acquire), 3)) return 0;
    // A talk's box on screen: nothing moves out. Its ends (+-284) and the name, and its answers (boxes at 450, texts from
    // 352) split from it (#200); and the windows a talk opens - the stable's horse list and its card, the mane and saddle
    // lists - are pieces whose centres sit at different x, moved by different amounts: the card's background left its
    // frame, the list's names were cut at their start (#231). The game hides the HUD (hearts, minimap) during a talk.
    if (frame_recent(now, atomic_load_explicit(&talk_frame, memory_order_acquire), 3)) return 0;
    // The button prompts' row along the bottom (y about -320; on a horse "Reset Camera ZL ... Yah! A", from x -250 to
    // 365) is centred by the game: its texts (moved by their origin) and their dark shadows (by their centre) fell either
    // side of the blend below and parted - "Yah!" off its shadow (2026-11-03). Its icons' panes sit at -318; the weather
    // forecast's, in the minimap's group, at -302 to -315 (x 326-428) - under one line for both, the forecast stayed
    // behind as the rest of the group went out to the edge (#243). So panes below -316.5, texts' origins below -290.
    if (y < row && x > -440.0f && x < 440.0f) return 0;
    float ax = x < 0 ? -x : x, t = (ax - 180.0f) / (260.0f - 180.0f);
    if (t <= 0) return 0;
    if (t > 1) t = 1;
    t = t * t * (3 - 2 * t);
    float d = 640.0f * (wide - 1.0f) * t;
    return x < 0 ? -d : d;
}
float widescreen_hud_shift(float x, float y) { return hud_shift_wide(x, y, -316.5f, load_wide()); }       // (a pane's centre, a scissor's)
float widescreen_hud_shift_text(float x, float y) { return hud_shift_wide(x, y, -290.0f, load_wide()); }  // (a text's origin)

// Where the minimap is on a w x h screen, moved up as above (its centre at layout 515, -245; about 100 units across):
// a touch there opens the map (ui/touch_controls.mm). false: not up there now.
bool widescreen_minimap_at(float w, float h, float *x, float *y, float *r) {
    if (widescreen_hud_top(515.0f, -245.0f) == 0) return false;
    float wide = load_wide();
    bool centred = ui_centred_wide(wide);
    float k = h / 720.0f, lx = 515.0f + (centred ? hud_shift_wide(515.0f, -245.0f, -316.5f, wide) : 0),
          ly = -245.0f + 490.0f + hud_anchor_wide(515.0f, -245.0f, -316.5f, wide);
    if (centred && isfinite(wide) && wide >= NARROW_MIN && wide < 0.999f)
        k *= wide;  // follow fitted width scaling on narrow screens
    *x = w * 0.5f + lx * k, *y = h * 0.5f - ly * k, *r = 105.0f * k;
    return true;
}

// A talk's box is on screen (in the world): the touch controls give way to it (ui/touch_controls.mm)
bool widescreen_talk(void) {
    extern uint64_t renderer_present_count(void);
    uint64_t tf = atomic_load_explicit(&talk_frame, memory_order_acquire);
    return frame_recent(renderer_present_count(), tf, 3) && widescreen_world_play();
}

void widescreen_frame_end(uint64_t frame) { (void)frame; }

// Draws to stretch with the wide view (identified by fragment shader hash): none.
float widescreen_effect_stretch(uint64_t fs_hash) { (void)fs_hash; return 1.0f; }

void widescreen_backdrop_seen(uint64_t hash) { (void)hash; }
void widescreen_scene_ubo(const float *u, uint64_t size) { (void)u, (void)size; }

// Presented part of the image (crop: uv origin and size) and its place in the window: the whole image. On the title
// screen, in the middle 16:9 part of the window with bars (pillarboxed if wider, letterboxed if narrower): the image's
// 16:9 part. Loading screens keep 16:9 proportions via layout scaling; the scope's view uses the same 16:9 crop with bars (#262, #349).
void widescreen_scope_seen(void) {
    extern uint64_t renderer_present_count(void);
    if (!ENV("LARECOMP_NO_SCOPE_BARS") && !framepace_in_menu())
        atomic_store_explicit(&scope_frame, renderer_present_count(), memory_order_release);
}

// The map terrain pane was drawn recently while a game menu is up. Share that signal between
// the terrain, markers, scissors and final presentation so they agree on the map's proportions.
void widescreen_map_seen(void) {
    extern uint64_t renderer_present_count(void);
    atomic_store_explicit(&map_frame, renderer_present_count(), memory_order_release);
}

bool widescreen_map_open(float wide) {
    extern uint64_t renderer_present_count(void);
    uint64_t seen = atomic_load_explicit(&map_frame, memory_order_acquire);
    return shape_reshaped(wide) && frame_recent(renderer_present_count(), seen, 2) && widescreen_in_menu();
}

bool widescreen_cropped(void) {
    extern uint64_t renderer_present_count(void);
    if (!widescreen_allowed()) return false;
    uint64_t sf = atomic_load_explicit(&scope_frame, memory_order_acquire);
    bool scope = frame_recent(renderer_present_count(), sf, 3);
    float wide = load_wide();
    return scope || framepace_on_title() || (g_settings.center_world_map && wide < 0.999f && widescreen_map_open(wide));
}

void widescreen_present_crop(uint64_t frame, double shape, float crop[4], double *vx, double *vy, double *vw, double *vh) {
    (void)frame;
    crop[0] = crop[1] = 0, crop[2] = crop[3] = 1;
    if (!isfinite(shape) || (shape >= 0.999 && shape <= 1.001) || !widescreen_cropped()) return;
    if (shape > 1.001) {
        crop[2] = (float)(1.0 / shape), crop[0] = (1.0f - crop[2]) * 0.5f;
        double w = *vw / shape;
        *vx += (*vw - w) * 0.5, *vw = w;
    } else if (shape >= 9.0 / 16.0 && shape < 0.999) {
        crop[3] = (float)shape, crop[1] = (1.0f - crop[3]) * 0.5f;
        double h = *vh * shape;
        *vy += (*vh - h) * 0.5, *vh = h;
    }
}
