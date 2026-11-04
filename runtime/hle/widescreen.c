// Widescreen: what the renderers ask about the output's shape. With Video > Aspect ratio "Unlocked", 1.8.2 fills a
// window of another shape: its cameras' sead::PerspectiveProjection objects take the output's aspect where the game
// rebuilds their matrix and where it asks for their aspect (rt_persp_update / rt_persp_aspect below, hooked into its
// game code by tools/patches.py), so the view widens - and its culling with it. The HUD, drawn through such a camera
// too, would sit in the centred 16:9 part; its panes nearer the sides move out to the screen's edges
// (widescreen_hud_shift: vulkan/exec.c cbuf_info moves them, scissor_rect their scissors). The renderers set the wide
// factor (output aspect / 16:9) each frame. 1.0.0 and 1.6.0 have no hooks: 16:9 with bars (LARECOMP_WIDESCREEN=1
// forces the wide targets there for tests: the image stretches); LARECOMP_NO_WIDESCREEN=1: never.
// How the main camera was found (2026-10-23): LARECOMP_FIND_PROJ (vulkan/exec.c) found its projection in the G-buffer
// vertex shaders' first constant buffer (0x920 bytes, P at +0x70: fovy 50 degrees, 16:9, near 1, far 25000), from a
// sead::PerspectiveProjection (vtable 0x101ba4438): +8 mDirty, +9 mDeviceDirty, +0xC mMatrix, +0x4C mDeviceMatrix,
// +0x98 near, far, fovy, sin, cos, tan, +0xB0 mAspect. The game sets 16:9 again before every rebuild (writing the field
// from the render thread, LARECOMP_CAM_PROJ, didn't hold), and the lighting asks for the aspect in between (with the
// matrix alone, trees came out dark with black specks). 1.8.2's one 16:9 constant (main 0x158afc8, read at
// 0x7001381c34: rt_aspect_bits) is some other cameras', not the main one's.
// The title and loading screens keep 16:9 with bars (widescreen_present_crop).
#include <stdbool.h>
#include <stdint.h>

#include <string.h>

#include "../framepace.h"
#include "../hle.h"
#include "../rt.h"
#include "../settings.h"

static float g_wide = 1.0f;   // the output's shape the renderers use: its aspect divided by 16:9 (1 = normal)
static float g_shape = 1.0f;  // the window's, whether widened or not (widescreen_set_shape: the cameras from the start)

void widescreen_set_factor(float f) { g_wide = f; }
float widescreen_factor(void) { return g_wide; }
void widescreen_set_shape(float f) { g_shape = f; }

static bool g_allowed;  // (widescreen_set_allowed: tests)
static volatile bool g_hooked;  // (the game code calls rt_persp_update: built with the hooks)
static int g_boot_aspect = -1;  // (the setting this boot of the game started with)
bool widescreen_allowed(void) {
    // 1.8.2 once its game code shows the hooks (one built before them would stretch the image: it stays 16:9), with
    // the setting the game started with: the layouts' camera takes its aspect once, early on (a copy of a projection
    // built on the stack, never rebuilt), so a change applies when the game starts again (a reboot: widescreen_reset)
    int boot_aspect = g_boot_aspect;
    if (boot_aspect < 0 && g_hooked) g_boot_aspect = boot_aspect = g_settings.aspect;
    bool game = g_game_version >= 182 && g_hooked && boot_aspect == ASPECT_UNLOCKED;
    bool test = (g_allowed || ENV("LARECOMP_WIDESCREEN")) && g_settings.aspect == ASPECT_UNLOCKED;
    return (game || test) && !ENV("LARECOMP_NO_WIDESCREEN");
}
void widescreen_set_allowed(bool on) { g_allowed = on; }

// In the world: the 3D scene was drawn a moment ago (vulkan/exec.c vk_scene_frame), or the game is paused over it
// (pause menu, map). Not on the title screen - its artwork reaches past the 16:9 part while the darker layer over it
// doesn't, an edge on each side - nor on loading screens: those keep 16:9 with bars (widescreen_present_crop) and
// the HUD stays put (widescreen_hud_shift).
static bool in_world(void) {
    extern uint64_t vk_scene_frame(void);
    extern uint64_t renderer_present_count(void);
    if (framepace_on_title()) return false;
    uint64_t seen = vk_scene_frame(), now = renderer_present_count();
    return seen && (now - seen < 20 || framepace_in_menu());
}

// A camera aspect at a patched site: 16:9 times the output's shape (g_wide: 1 unless widened)
uint32_t rt_aspect_bits(int site) {
    float a = 16.0f / 9.0f * (widescreen_allowed() ? g_wide : 1.0f);
    if (ENV("LARECOMP_LOG_ASPECT")) LOG("[widescreen] camera aspect %.4f (site %d)\n", a, site);
    uint32_t u;
    memcpy(&u, &a, 4);
    return u;
}

// sead::PerspectiveProjection::doUpdateMatrix (1.8.2: 0x7000da13f4, tools/patches.py), as it starts building a
// projection's matrix: a 16:9 one takes the window's shape (mAspect at +0xB0) - the game sets 16:9 again before each
// rebuild, so this is where the main camera (and its frustum's culling, from that matrix) widens.
static volatile bool g_widened;  // (rt_persp_update has widened a camera)
static bool is_16_9(float a) { return a > 1.7767f && a < 1.7789f; }
RT_ON_RESET(widescreen_reset) { g_boot_aspect = -1, g_hooked = false, g_widened = false; }  // (Return to start: a new boot)
void rt_persp_update(uint64_t proj) {
    g_hooked = true;
    if (!proj || !widescreen_allowed() || g_shape <= 1.001f) return;
    float *aspect = (float *)(uintptr_t)(proj + 0xB0);
    if (ENV("LARECOMP_LOG_ASPECT")) {  // (each projection rebuilt, once: near, far, fovy, aspect)
        static uint64_t seen[256];
        static int nseen;
        bool found = false;
        for (int i = 0; i < nseen && !found; i++) found = seen[i] == proj;
        if (!found && nseen < 256) {
            seen[nseen++] = proj;
            extern uint64_t renderer_present_count(void);
            const float *p = (const float *)(uintptr_t)(proj + 0x98);
            LOG("[widescreen] frame %llu: projection %llx near %.2f far %.0f fovy %.1f deg aspect %.4f\n",
                (unsigned long long)renderer_present_count(), (unsigned long long)proj, p[0], p[1], p[2] * 57.29578f, *aspect);
        }
    }
    if (!is_16_9(*aspect)) return;
    *aspect = 16.0f / 9.0f * g_shape;
    g_widened = true;
    static int n;
    if (ENV("LARECOMP_LOG_ASPECT") && n++ < 20) LOG("[widescreen] projection %llx: aspect %.4f\n", (unsigned long long)proj, *aspect);
}

// sead::PerspectiveProjection::getAspect (1.8.2: 0x7000da13d8, tools/patches.py): the widened aspect for a 16:9 one,
// also between the game setting 16:9 again and the next rebuild (rt_persp_update), so what asks the projection for its
// aspect agrees with its matrix. The float's bits.
uint32_t rt_persp_aspect(uint64_t proj) {
    float a = *(const float *)(uintptr_t)(proj + 0xB0);
    if (widescreen_allowed() && g_shape > 1.001f && is_16_9(a)) a = 16.0f / 9.0f * g_shape;
    uint32_t u;
    memcpy(&u, &a, 4);
    return u;
}

// The game's cameras are widened (the 1.8.2 hooks above): its HUD, drawn through one, lands in the centred 16:9 part
// of a wide final image (exec.c scissor_rect maps the HUD's scissors there).
bool widescreen_ui_centred(void) { return widescreen_allowed() && g_wide != 1.0f && g_widened; }

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
    static uint64_t last;
    uint64_t now = renderer_present_count();
    if (framepace_in_menu()) last = now + 1;
    return last && now < last + 30;
}

// Playing in the world: not the title or a loading screen, no menu of the game's up (pause, map) - the touch controls'
// icon layout (ui/touch_controls.mm); the classic one stays in the menus
bool widescreen_world_play(void) { return in_world() && !widescreen_in_menu(); }

// The minimap's disc (vulkan/exec.c: its pane's shader) was drawn this frame or in the last few: the minimap is on screen.
// A talk's answers sit where it would be (x 450, y -133 to -237) while it's hidden; moved up with it, they split (#200).
static uint64_t minimap_frame;
void widescreen_minimap_seen(void) {
    extern uint64_t renderer_present_count(void);
    minimap_frame = renderer_present_count();
}

// A talk's box (vulkan/exec.c: its pane, about 0, -235) was drawn this frame or in the last few: its rounded ends
// (x +-284) and the speaker's name stay with it instead of moving out to the screen's edges (widescreen_hud_shift; #200)
static uint64_t talk_frame;
void widescreen_talk_seen(void) {
    extern uint64_t renderer_present_count(void);
    talk_frame = renderer_present_count();
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
    if (x < 290.0f || y > -140.0f || y < -315.0f || !touch_hud_top() || !in_world() || framepace_in_menu()) return 0;
    if (!minimap_frame || renderer_present_count() - minimap_frame > 3) return 0;
    return 490.0f;
}

// Where the minimap is on a w x h screen, moved up as above (its centre at layout 515, -245; about 100 units across):
// a touch there opens the map (ui/touch_controls.mm). false: not up there now.
float widescreen_hud_shift(float x, float y);
bool widescreen_minimap_at(float w, float h, float *x, float *y, float *r) {
    if (widescreen_hud_top(515.0f, -245.0f) == 0) return false;
    float k = h / 720.0f, lx = 515.0f + (widescreen_ui_centred() ? widescreen_hud_shift(515.0f, -245.0f) : 0), ly = -245.0f + 490.0f;
    *x = w * 0.5f + lx * k, *y = h * 0.5f - ly * k, *r = 105.0f * k;
    return true;
}

// A pane as wide as the screen (vulkan/exec.c full_width_pane: a loading screen's band and its black, the Game Over's
// black) was drawn this frame or in the last few: a screen of its own is up, nothing moves out. As a load ends the world
// is drawn again under the loading screen (in_world): its title and tip went out to the left edge, cut, and "Next tip"
// off the right one, for its last second (#195).
static uint64_t band_frame;
void widescreen_band_seen(void) {
    extern uint64_t renderer_present_count(void);
    band_frame = renderer_present_count();
}

// (row: below this y the prompts' row - a pane's centre or a text's origin, widescreen_hud_shift / _text)
static float hud_shift(float x, float y, float row) {
    extern uint64_t renderer_present_count(void);
    if (!widescreen_ui_centred() || widescreen_in_menu() || !in_world()) return 0;
    if (band_frame && renderer_present_count() - band_frame <= 3) return 0;
    // A talk's box on screen: nothing moves out. Its ends (+-284) and the name, and its answers (boxes at 450, texts from
    // 352) split from it (#200); and the windows a talk opens - the stable's horse list and its card, the mane and saddle
    // lists - are pieces whose centres sit at different x, moved by different amounts: the card's background left its
    // frame, the list's names were cut at their start (#231). The game hides the HUD (hearts, minimap) during a talk.
    if (talk_frame && renderer_present_count() - talk_frame <= 3) return 0;
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
    float d = 640.0f * (g_wide - 1.0f) * t;
    return x < 0 ? -d : d;
}
float widescreen_hud_shift(float x, float y) { return hud_shift(x, y, -316.5f); }       // (a pane's centre, a scissor's)
float widescreen_hud_shift_text(float x, float y) { return hud_shift(x, y, -290.0f); }  // (a text's origin)

// A talk's box is on screen (in the world): the touch controls give way to it (ui/touch_controls.mm)
bool widescreen_talk(void) {
    extern uint64_t renderer_present_count(void);
    return talk_frame && renderer_present_count() - talk_frame <= 3 && widescreen_world_play();
}

void widescreen_frame_end(uint64_t frame) { (void)frame; }

// Draws to stretch with the wide view (identified by fragment shader hash): none.
float widescreen_effect_stretch(uint64_t fs_hash) { (void)fs_hash; return 1.0f; }

void widescreen_backdrop_seen(uint64_t hash) { (void)hash; }
void widescreen_scene_ubo(const float *u, uint64_t size) { (void)u, (void)size; }

// Presented part of the image (crop: uv origin and size) and its place in the window: the whole image. On the title
// screen, in the middle 16:9 part of the window with bars: the image's middle 16:9 part (its layout there, through the
// widened layout camera, its artwork past it). Loading screens too are the whole image now: their layout, drawn through
// a 16:9 camera of their own, is narrowed to the middle part as it's drawn (vulkan/exec.c cbuf_info) - squeezed back
// here instead, the image shrank to load and stretched back to the world.
void widescreen_present_crop(uint64_t frame, double shape, float crop[4], double *vx, double *vy, double *vw, double *vh) {
    (void)frame, (void)vy, (void)vh;
    crop[0] = crop[1] = 0, crop[2] = crop[3] = 1;
    if (shape <= 1.001 || !widescreen_allowed() || in_world() || !framepace_on_title()) return;
    crop[2] = (float)(1.0 / shape), crop[0] = (1.0f - crop[2]) * 0.5f;
    double w = *vw / shape;
    *vx += (*vw - w) * 0.5, *vw = w;
}
