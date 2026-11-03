// On-screen controls on Android (docs/las/ANDROID-PORT.md, Fase 4): both sticks, A / B / X / Y, L / R / ZL / ZR,
// L3 / R3 (the sticks' clicks), the d-pad, Minus / Plus and a button for the in-game menu, drawn over the game with
// ImGui (ui/menu.mm menu_render) while no controller is connected. Fingers come from SDL's touch events (ui_run,
// runtime/nvn/vulkan/device.c); what they hold joins the controller's buttons and sticks in runtime/hle/input.c
// (g_touch_buttons, g_touch_lx / ly, g_touch_rx / ry). Both sticks float: the left one starts where a finger lands in
// the left part of the screen, the right one (BOTW's camera) where it lands on the right, off the buttons. With the
// menu or the name entry up, touches stay mouse clicks for the menu. The Android back button opens the menu too.
// FPS-style options (Controls tab): the camera by dragging (drag_frame), the sticks drawn only while touched, and each
// stick's area a whole half of the screen.
// The layout is the player's (the Controls tab, ui/controls_ui.mm): each control's place (g_settings.touch_pos, a
// share of the screen; negative: the default), their size and opacity. Editing it (touch_edit_begin) closes the menu
// and lets the controls be dragged; a bar on the screen saves, resets or cancels.
// The icon layout (g_settings.touch_scheme 1; design/touch-controls-concept/IMPLEMENTACAO-TOUCH-V1.md): while playing in
// the world (hle/widescreen.c widescreen_world_play), buttons drawn as what they do - the game's own item icons, read
// from its RomFS (services/fs.c fs_romfs_read), and assets/artwork/touch-actions-v1.png - at places of their own
// (g_settings.touch_pos2). Some are gestures: the bow, weapon and shield hold their button while the same finger's
// drag tilts the right stick (aiming, the game's pickers); the power taps L, or held opens the rune wheel (d-pad up).
// The game's menus keep the classic layout.
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "i18n.h"
#include "imgui.h"

extern "C" {
#include "settings.h"
#include "paths.h"
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
bool menu_input_captured(void);  // ui/menu.mm
void menu_test_open(void);       // ui/menu.mm: opens the in-game menu
void menu_request_close(void);   // ui/menu.mm
_Atomic uint64_t g_touch_buttons;  // (runtime/hle/input.c) Switch button bits held on the screen
_Atomic int32_t g_touch_lx, g_touch_ly;  // left stick, -32767..32767, y up
_Atomic int32_t g_touch_rx, g_touch_ry;  // right stick
bool widescreen_world_play(void);                       // hle/widescreen.c
bool widescreen_minimap_at(float w, float h, float *x, float *y, float *r);
bool widescreen_talk(void);
uint8_t *fs_romfs_read(const char *path, size_t *size);  // services/fs.c
}

// the Switch button bits (runtime/hle/input.c)
enum { TB_A = 0, TB_B, TB_X, TB_Y, TB_LSTICK, TB_RSTICK, TB_L, TB_R, TB_ZL, TB_ZR, TB_PLUS, TB_MINUS,
       TB_LEFT, TB_UP, TB_RIGHT, TB_DOWN };
enum { C_NONE = -1, C_STICK = 100, C_MENU = 101, C_DONE = 102, C_RESET = 103, C_CANCEL = 104, C_RSTICK = 105 };
// the controls, in g_settings.touch_pos order (TOUCH_NCTL entries; LAS's first twelve keep their places in saved layouts)
enum { K_A, K_B, K_X, K_Y, K_L, K_ZL, K_R, K_ZR, K_MINUS, K_PLUS, K_MENU, K_STICK,
       K_RSTICK, K_LS, K_RS, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_N };
_Static_assert(K_N == TOUCH_NCTL, "settings.h TOUCH_NCTL");

// The icon layout's controls, in g_settings.touch_pos2 order (TOUCH_NCTL2 entries): what each holds (a Switch button
// bit, -1 none), its default place (a share of the screen) and radius (a share of its height)
enum { I_MOVE, I_CROUCH, I_WEAPON, I_SHIELD, I_WHISTLE, I_SLATE, I_SCOPE, I_INVENTORY, I_ATTACK, I_POWER, I_BLOCK, I_BOW,
       I_JUMP, I_SPRINT, I_A, I_R, I_MENU, I_PUSH, I_PULL, I_MLEFT, I_MRIGHT, I_CANCEL, I_ARROWS, I_ALBUM, I_N };
_Static_assert(I_N == TOUCH_NCTL2, "settings.h TOUCH_NCTL2");
static const struct { int bit; float x, y, r; } IC[I_N] = {
    {-1, 0.09f, 0.77f, 0},               // the left stick's resting place
    {TB_LSTICK, 0.19f, 0.895f, 0.046f},  // crouch
    {TB_RIGHT, 0.0544f, 0.5638f, 0.05f}, // weapon: held, the right stick picks one in the game's bar
    {TB_LEFT, 0.1167f, 0.565f, 0.05f},   // shield: the same
    {TB_DOWN, 0.46f, 0.90f, 0.043f},     // whistle
    {TB_RSTICK, 0.54f, 0.90f, 0.043f},   // the Sheikah Slate: its scope (R3) - the map opens from the minimap
    {-1, 0.62f, 0.90f, 0},               // (was the scope: kept for saved layouts, not shown)
    {TB_PLUS, 0.89f, 0.32f, 0.045f},     // inventory
    {TB_Y, 0.83f, 0.70f, 0.11f},         // attack: the largest
    {TB_L, 0.83f, 0.4781f, 0.0624f},     // power: L on a tap; held, the rune fan
    {TB_ZL, 0.94f, 0.635f, 0.052f},      // shield up / lock on
    {TB_ZR, 0.72f, 0.635f, 0.07f},       // bow: held, the same finger's drag aims (the phone with motion controls on)
    {TB_X, 0.72f, 0.80f, 0.05f},         // jump
    {TB_B, 0.83f, 0.875f, 0.05f},        // sprint
    {TB_A, 0.94f, 0.80f, 0.05f},         // interact (talk, pick up, open)
    {TB_R, 0.94f, 0.47f, 0.045f},        // throw: held, the same finger's drag aims (as the bow)
    {C_MENU, 0.5f, 0.06f, 0.045f},       // the app's menu (the Sheikah eye)
    {TB_UP, 0.735f, 0.375f, 0.04f},      // Magnesis: the d-pad, a cross (only with Magnesis selected: shown2) - up
    {TB_DOWN, 0.735f, 0.505f, 0.04f},    //   pushes the object away, down pulls it closer
    {TB_LEFT, 0.695f, 0.44f, 0.04f},     //   left
    {TB_RIGHT, 0.775f, 0.44f, 0.04f},    //   right
    {TB_B, 0.085f, 0.40f, 0.05f},        // cancel (B): only while the bow or a throw is held - the left thumb's (shown2)
    {TB_LEFT, 0.19f, 0.40f, 0.05f},      // arrows: only while the bow is held - held, the game's arrow picker (the d-pad's
                                         //   left with the bow drawn), picked by the same finger's drag as the weapons
    {TB_MINUS, 0.735f, 0.44f, 0.045f},   // the album (Minus): only with the camera rune selected, where Magnesis's cross
                                         //   sits - the map moved to the minimap and left the camera without it (#202)
};
enum { C_ICON = 300, C_EDIT2 = 400 };  // (a finger's ctl: an icon control, C_ICON + I_*; dragged while editing, C_EDIT2 + I_*)
#define POWER_HOLD_MS 280  // (the power held this long: the rune wheel, and no L)
#define PULSE_MS 100       // (a button tapped shorter stays held this long: the game reads it at least once)

typedef struct { float x, y, r; int bit; const char *label; bool rect; float w, h; int k; } Ctl;

static pthread_mutex_t t_lock = PTHREAD_MUTEX_INITIALIZER;
// (icon gestures: sx / sy where the finger landed, t0 when, wheel: the power held into the rune wheel)
static struct { SDL_FingerID id; int ctl; float x, y, dx, dy, sx, sy; uint64_t t0; bool wheel; } fingers[10];
static uint64_t pulse_until[16];  // (icon layout) button bits still held after a short tap, until then (ms)
static uint64_t keep_icons_until;  // (icon layout) kept through a game menu until then (ms): mode_check
// Shield surfing (the game's: shield up, jump, then A in the air): the shield button tapped twice and held - the
// second touch jumps (X) and presses A a moment later, the shield up while it's held
#define SURF_TAP_MS 300  // (the second touch this soon after the first let go)
#define SURF_A_MS 150    // (A this long after the jump: in the air)
#define PARRY_MS 600     // (a parry, a slide on the held shield: the next no sooner - a controller's A mashed: 0.45-0.63 s)
static uint64_t block_up_ms, surf_a_at;
static bool icons_on;  // the icon layout is the one in use (as of the last event or frame)
static int nfingers;
// the sticks (0 left, 1 right): where each started and where its knob is (pixels)
static struct { bool on; float cx, cy, kx, ky; } stick[2];
static float disp_w = 1, disp_h = 1;  // the window in pixels (as last drawn)
static atomic_bool used;  // a touch since the last controller: show the controls
static atomic_bool hide_auto;  // a controller is connected (as of the last event or check)
static atomic_bool editing;
static float edit_pos[K_N][2];  // (editing) the layout being edited
static bool edit_icons;          // (editing) the icon layout, in edit_pos2
static float edit_pos2[I_N][2];
static float edit_size[K_N], edit_size2[I_N];  // (editing) each control's own size (g_settings.touch_size / _size2)
static int edit_sel = -1;  // (editing) the control the bar's - / + resize: its entry in the layout edited
static _Atomic uint64_t last_draw_ms;

// The icon layout is in use: its setting on, playing in the world (or editing it)
static bool icons_wanted(void) {
    if (editing) return edit_icons;
    return g_settings.touch_scheme == 1 && widescreen_world_play();
}

static void default_pos(int k, float *x, float *y) {  // in pixels
    float w = disp_w, h = disp_h, u = h / 100.0f, gap = 13.0f * u, fx = w - 20 * u, fy = h - 27 * u, sw = 15 * u;
    float dx = 15 * u, dy = h - 58 * u, dg = 7.5f * u;  // the d-pad: above the left stick
    switch (k) {
    case K_A: *x = fx + gap, *y = fy; break;  // face buttons: a diamond, Switch layout (A east)
    case K_B: *x = fx, *y = fy + gap; break;
    case K_X: *x = fx, *y = fy - gap; break;
    case K_Y: *x = fx - gap, *y = fy; break;
    case K_L: *x = 4 * u + sw / 2, *y = 20 * u; break;  // shoulders: L / ZL top left, R / ZR top right
    case K_ZL: *x = 4 * u + sw / 2, *y = 10 * u; break;
    case K_R: *x = w - 4 * u - sw / 2, *y = 20 * u; break;
    case K_ZR: *x = w - 4 * u - sw / 2, *y = 10 * u; break;
    case K_MINUS: *x = w / 2 - 9 * u, *y = h - 7 * u; break;
    case K_PLUS: *x = w / 2 + 9 * u, *y = h - 7 * u; break;
    case K_MENU: *x = w / 2, *y = 6 * u; break;
    case K_RSTICK: *x = w - 62 * u, *y = h - 27 * u; break;  // the right stick's resting place
    case K_LS: *x = 40 * u, *y = h - 8 * u; break;  // the sticks' clicks, under each stick's resting place
    case K_RS: *x = w - 50 * u, *y = h - 8 * u; break;
    case K_UP: *x = dx, *y = dy - dg; break;
    case K_DOWN: *x = dx, *y = dy + dg; break;
    case K_LEFT: *x = dx - dg, *y = dy; break;
    case K_RIGHT: *x = dx + dg, *y = dy; break;
    default: *x = 20 * u, *y = h - 27 * u; break;  // the left stick's resting place
    }
}

static void pos_of(int k, float *x, float *y) {
    const float *p = editing ? edit_pos[k] : g_settings.touch_pos[k];
    if (p[0] >= 0 && p[1] >= 0) *x = p[0] * disp_w, *y = p[1] * disp_h;
    else default_pos(k, x, y);
}

static float size_scale(void) { return g_settings.touch_scale > 0.2f ? g_settings.touch_scale : 1.0f; }
// each control's own size, times size_scale (a classic layout entry / an icon layout entry)
static float size_of(int k) { float s = editing ? edit_size[k] : g_settings.touch_size[k]; return s > 0.1f ? s : 1.0f; }
static float size_of2(int k) { float s = editing ? edit_size2[k] : g_settings.touch_size2[k]; return s > 0.1f ? s : 1.0f; }
static float stick_radius(void) { return disp_h * 0.13f * size_scale(); }
static float stick_r(int s) {  // (a stick's own: the left one's size in either layout, the classic right one's)
    return stick_radius() * (icons_on ? (s ? 1.0f : size_of2(I_MOVE)) : size_of(s ? K_RSTICK : K_STICK));
}

static void pos_of2(int k, float *x, float *y) {  // (icon layout) in pixels
    const float *p = editing ? edit_pos2[k] : g_settings.touch_pos2[k];
    if (p[0] >= 0 && p[1] >= 0) *x = p[0] * disp_w, *y = p[1] * disp_h;
    else *x = IC[k].x * disp_w, *y = IC[k].y * disp_h;
}
static float radius2(int k) { return IC[k].r * disp_h * size_scale() * size_of2(k); }
static bool shown2(int k);
static bool talk_on(void);

enum { C_SMALLER = 106, C_BIGGER = 107 };
static bool is_bar(int bit) { return (bit >= C_DONE && bit <= C_CANCEL) || bit == C_SMALLER || bit == C_BIGGER; }
static int edit_bar(Ctl *c) {  // (editing) the bar: save, defaults, cancel - and with a control selected, - / + its size
    float u = disp_h / 100.0f, bw = 16 * u, bh = 8 * u, y = 30 * u;
    c[0] = (Ctl){disp_w / 2 - 18 * u, y, 0, C_DONE, "Done", true, bw, bh, -1};
    c[1] = (Ctl){disp_w / 2, y, 0, C_RESET, "Default", true, bw, bh, -1};
    c[2] = (Ctl){disp_w / 2 + 18 * u, y, 0, C_CANCEL, "Cancel", true, bw, bh, -1};
    if (edit_sel < 0) return 3;
    c[3] = (Ctl){disp_w / 2 - 6 * u, y + 11 * u, 0, C_SMALLER, "-", true, 10 * u, bh, -1};
    c[4] = (Ctl){disp_w / 2 + 6 * u, y + 11 * u, 0, C_BIGGER, "+", true, 10 * u, bh, -1};
    return 5;
}

static int layout(Ctl *c) {  // in pixels of the window (landscape)
    float u = disp_h / 100.0f, z0 = size_scale();
    static const struct { int k, bit; const char *label; int shape; } T[] = {
        {K_A, TB_A, "A", 0}, {K_B, TB_B, "B", 0}, {K_X, TB_X, "X", 0}, {K_Y, TB_Y, "Y", 0},
        {K_L, TB_L, "L", 1}, {K_ZL, TB_ZL, "ZL", 1}, {K_R, TB_R, "R", 1}, {K_ZR, TB_ZR, "ZR", 1},
        {K_MINUS, TB_MINUS, "-", 2}, {K_PLUS, TB_PLUS, "+", 2}, {K_MENU, C_MENU, "=", 2},
        {K_LS, TB_LSTICK, "L3", 2}, {K_RS, TB_RSTICK, "R3", 2},
        {K_UP, TB_UP, "^", 3}, {K_DOWN, TB_DOWN, "v", 3}, {K_LEFT, TB_LEFT, "<", 3}, {K_RIGHT, TB_RIGHT, ">", 3}};
    int n = 0;
    for (const auto &t : T) {
        float x, y, z = z0 * size_of(t.k);
        float br = 7.5f * u * z, sw = 15 * u * z, sh = 7 * u * z, sr = 4.5f * u * z, dr = 3.8f * u * z;
        pos_of(t.k, &x, &y);
        c[n++] = t.shape == 1 ? (Ctl){x, y, 0, t.bit, t.label, true, sw, sh, t.k}
                              : (Ctl){x, y, t.shape == 2 ? sr : t.shape == 3 ? dr : br, t.bit, t.label, false, 0, 0, t.k};
    }
    if (editing) n += edit_bar(c + n);
    return n;
}

static bool in_ctl(const Ctl *c, float x, float y) {
    float u = disp_h / 100.0f;
    if (c->rect) return fabsf(x - c->x) <= c->w / 2 + u && fabsf(y - c->y) <= c->h / 2 + u;
    float dx = x - c->x, dy = y - c->y, r = c->r * 1.25f;  // (a bit larger than drawn: thumbs)
    return dx * dx + dy * dy <= r * r;
}

// (icon layout) the control under (x, y): the edit bar, the nearest icon control (in a circle a bit larger than drawn),
// else a stick's area as in the classic layout. *k: its entry in touch_pos2.
static int hit2(float x, float y, int *k) {
    if (editing) {
        Ctl c[5];
        for (int i = 0, n = edit_bar(c); i < n; i++)
            if (fabsf(x - c[i].x) <= c[i].w / 2 && fabsf(y - c[i].y) <= c[i].h / 2) { *k = -1; return c[i].bit; }
    }
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < I_N; i++) {
        if (i == I_MOVE || !shown2(i)) continue;
        float cx, cy, r = radius2(i) * 1.25f;  // (thumbs)
        pos_of2(i, &cx, &cy);
        float d = ((x - cx) * (x - cx) + (y - cy) * (y - cy)) / (r * r);
        if (d <= 1 && d < bd) bd = d, best = i;
    }
    if (best >= 0) { *k = best; return best == I_MENU ? C_MENU : C_ICON + best; }
    float R = stick_r(0);
    if (editing) {  // the left stick's resting place can be dragged
        float sx, sy;
        pos_of2(I_MOVE, &sx, &sy);
        *k = I_MOVE;
        if ((x - sx) * (x - sx) + (y - sy) * (y - sy) <= R * R) return C_STICK;
        *k = -1;
        return C_NONE;
    }
    bool half = g_settings.touch_half_areas, right_half = true;  // (the camera by dragging, on the whole right half)
    *k = I_MOVE;
    if (half ? x < disp_w * 0.5f : x < disp_w * 0.45f && y > disp_h * 0.30f) return C_STICK;
    *k = -1;
    if (right_half ? x >= disp_w * 0.5f : x > disp_w * 0.50f && y > disp_h * 0.25f) return C_RSTICK;  // (the camera)
    return C_NONE;
}

static int hit(float x, float y, int *k) {  // the control under (x, y) (*k: its layout entry)
    // (the minimap, up top with the on-screen controls: a double tap opens the map - Minus. One tap goes on to what's
    // under it - the classic layout's R and ZR sit there, and a touch meant for them opened the map: #209, #218)
    float mx, my, mr;
    static uint64_t map_tap_ms;
    if (!editing && widescreen_minimap_at(disp_w, disp_h, &mx, &my, &mr) && (x - mx) * (x - mx) + (y - my) * (y - my) <= mr * mr) {
        uint64_t now = SDL_GetTicks();
        if (map_tap_ms && now - map_tap_ms < 350) {
            map_tap_ms = 0, *k = -1;
            return TB_MINUS;
        }
        map_tap_ms = now;
    }
    if (icons_on) return hit2(x, y, k);
    Ctl c[32];
    int n = layout(c);
    for (int i = n; i-- > 0;)  // (the bar first)
        if (in_ctl(&c[i], x, y)) { *k = c[i].k; return c[i].bit; }
    if (editing) {  // the sticks' resting places can be dragged
        for (int s = 0; s < 2; s++) {
            float sx, sy;
            pos_of(s ? K_RSTICK : K_STICK, &sx, &sy);
            if ((x - sx) * (x - sx) + (y - sy) * (y - sy) <= stick_r(s) * stick_r(s)) { *k = s ? K_RSTICK : K_STICK; return s ? C_RSTICK : C_STICK; }
        }
        *k = -1;
        return C_NONE;
    }
    // (Larger areas: each a whole half of the screen, the buttons still in front; the drag camera has no stick of its own
    // to stay near, so it takes the whole right half too)
    bool half = g_settings.touch_half_areas, right_half = half || g_settings.touch_drag_camera;
    *k = K_STICK;
    if (half ? x < disp_w * 0.5f : x < disp_w * 0.45f && y > disp_h * 0.30f) return C_STICK;
    *k = K_RSTICK;
    if (right_half ? x >= disp_w * 0.5f : x > disp_w * 0.50f && y > disp_h * 0.25f) return C_RSTICK;  // (the camera)
    *k = -1;
    return C_NONE;
}

static void stick_update(int s) {  // (t_lock held)
    float R = stick_r(s);
    float dx = stick[s].kx - stick[s].cx, dy = stick[s].ky - stick[s].cy, d = sqrtf(dx * dx + dy * dy);
    if (d > R) { dx *= R / d, dy *= R / d; stick[s].kx = stick[s].cx + dx, stick[s].ky = stick[s].cy + dy; }
    int32_t x = (int32_t)(dx / R * 32767), y = (int32_t)(-dy / R * 32767);
    if (s) g_touch_rx = x, g_touch_ry = y;
    else g_touch_lx = x, g_touch_ly = y;
}

// The camera by dragging (g_settings.touch_drag_camera; FPS-style, a Reddit request): the finger's speed, not where it
// is, tilts the right stick - the camera turns while the finger moves and stops with it. Its travel since the last frame
// (drag_dx / dy, pixels) becomes the tilt once a frame (drag_frame): a full tilt at 80% of the screen's height a second
// (times touch_drag_sens), a little smoothed (touch events and frames come at different rates), and past the stick's
// first 15% (the game's dead zone: slow drags would turn nothing).
static float drag_dx, drag_dy, drag_vx, drag_vy;
static uint64_t drag_last_ns;

// (the icon layout: always - no right stick drawn, the camera dragged anywhere on the right half)
static bool drag_camera(void) { return (g_settings.touch_drag_camera || icons_on) && !editing; }

static void drag_frame(void) {  // (t_lock held) once a presented frame
    uint64_t now = SDL_GetTicksNS();
    double dt = drag_last_ns ? (double)(now - drag_last_ns) / 1e9 : 0;
    drag_last_ns = now;
    if (!drag_camera() || !stick[1].on) { drag_vx = drag_vy = drag_dx = drag_dy = 0; return; }
    if (dt <= 0 || dt > 0.25) dt = 1.0 / 30;
    float sens = g_settings.touch_drag_sens > 0.1f ? g_settings.touch_drag_sens : 1.0f;
    float full = disp_h * 0.8f / sens;  // (pixels a second for a full tilt)
    float vx = (float)(drag_dx / dt) / full, vy = (float)(-drag_dy / dt) / full;
    drag_dx = drag_dy = 0;
    drag_vx = 0.5f * drag_vx + 0.5f * vx, drag_vy = 0.5f * drag_vy + 0.5f * vy;
    float m = sqrtf(drag_vx * drag_vx + drag_vy * drag_vy), out = m < 0.01f ? 0 : fminf(1.0f, 0.15f + 0.85f * m);
    float k = m > 0 ? out / m : 0;
    g_touch_rx = (int32_t)(drag_vx * k * 32767), g_touch_ry = (int32_t)(drag_vy * k * 32767);
}

static void stick_release(int s) {  // (t_lock held)
    stick[s].on = false;
    if (s) g_touch_rx = 0, g_touch_ry = 0, drag_dx = drag_dy = drag_vx = drag_vy = 0;
    else g_touch_lx = 0, g_touch_ly = 0;
}

static bool aiming;  // (icon layout) a gesture finger tilts the right stick

// The rune wheel in steps: the game's moves one rune a tilt of the right stick, and a tilt that followed the finger
// overshot. Each WHEEL_STEP of the screen's width the power's finger slides sideways is one short tilt that way.
#define WHEEL_STEP 0.06f
#define FLICK_MS 110   // (a step's tilt, then that much more upright before the next)
static float wheel_base;  // (the finger's x as of the last step)
static int wheel_pending, flick_dir;
static uint64_t flick_until, flick_gap_until;
static void wheel_steps(float x, bool start, uint64_t now) {  // (t_lock held) -> g_touch_rx / ry
    float step = WHEEL_STEP * disp_w;
    if (start) wheel_base = x, wheel_pending = 0;
    while (x - wheel_base >= step) wheel_pending++, wheel_base += step;
    while (wheel_base - x >= step) wheel_pending--, wheel_base -= step;
    if (now >= flick_gap_until && wheel_pending) {
        flick_dir = wheel_pending > 0 ? 1 : -1;
        wheel_pending -= flick_dir;
        flick_until = now + FLICK_MS, flick_gap_until = flick_until + FLICK_MS;
    }
    g_touch_rx = now < flick_until ? flick_dir * 32767 : 0, g_touch_ry = 0;
}

// The rune fan, where the game's flags are read (hle/gdt_runes.c: the selected rune, the ones the player has): the
// power held shows the player's runes in an arc above it; let go over one, the game's wheel is opened (d-pad up) and
// stepped a rune at a time until the game's own selection is that one, then let go. Without them (1.0.0): the wheel in
// steps above.
extern "C" int gdt_rune(uint32_t *owned);
static const char *const rune_item[8] = {"Obj_RemoteBombBall", "Obj_RemoteBombCube", "Obj_Magnetglove", "Obj_StopTimer",
                                         "Obj_IceMaker", "Obj_Camera", "Obj_Motorcycle", "Obj_AmiiboItem"};
static int rune_now = -1;  // the game's selected rune (0-7; -1 unknown)

static bool bow_held;  // (icon layout) a finger holds the bow or throw (recompute)
static bool bow_drawn;  // (icon layout) a finger holds the bow (recompute)

// (icon layout) a control on screen: Magnesis's d-pad (up pushes, down pulls; #199) only while it's the rune selected,
// the cancel only while the bow or a throw is held (or editing the layout); the others always, but the old scope's place
static bool shown2(int k) {
    if (IC[k].r == 0) return false;
    if (talk_on()) return k == I_A || k == I_SPRINT || k == I_MENU;  // (a talk: A, B, the app's menu - and the stick)
    if (k >= I_PUSH && k <= I_MRIGHT) return editing || rune_now == 2;
    if (k == I_CANCEL) return editing || bow_held;
    if (k == I_ARROWS) return editing || bow_drawn;
    if (k == I_ALBUM) return editing || rune_now == 5;
    return true;
}

// A talk (hle/widescreen.c widescreen_talk: its box on screen) in the icon layout: the controls give way to it but the
// stick, A and B (its answers picked with them, as on a controller: tapping them sent the game past the stable's horse list)
static bool talk_on(void) { return icons_on && !editing && g_settings.touch_talk && widescreen_talk(); }
static uint32_t runes_owned = 0xFF;
static bool fan_open;
static int fan_items[8], fan_n, fan_sel = -1;  // (open) the runes shown, the one under the finger
static int rune_goal = -1;                     // (the game's wheel being stepped to it)
static uint64_t goal_start, goal_done;

// The fan: a half ring above the power button, from its left round to its right, one slice a rune (in the game's order)
#define FAN_PI 3.14159265f
static void fan_ring(float *cx, float *cy, float *ri, float *ro) {
    float rp = radius2(I_POWER);
    pos_of2(I_POWER, cx, cy);
    *ri = rp * 1.2f, *ro = rp * 3.2f;
}

static void fan_update(float fx, float fy) {  // (t_lock held) the runes shown and the slice the finger points at
    fan_n = 0;
    for (int i = 0; i < 8; i++) if (runes_owned >> i & 1) fan_items[fan_n++] = i;
    float cx, cy, ri, ro;
    fan_ring(&cx, &cy, &ri, &ro);
    float dx = fx - cx, dy = fy - cy, a = atan2f(dy, dx);  // (screen y down: the upper half is -pi..0)
    fan_sel = -1;
    if (dx * dx + dy * dy < ri * ri * 0.36f || fan_n == 0) return;  // (still on the button: none)
    if (a > 0) a = dx < 0 ? -FAN_PI : 0;  // (below the button: the nearer end)
    int s = (int)((a + FAN_PI) / (FAN_PI / fan_n));
    fan_sel = s < 0 ? 0 : s >= fan_n ? fan_n - 1 : s;
}

// (t_lock held) stepping the game's wheel to rune_goal: d-pad up held, then as many flicks as the goal is places away
// in the wheel (the player's runes, in order) - the game's selection flag changes only once the wheel closes, so it
// can't be watched on the way - a moment more, then let go
static int goal_steps;
static bool goal_step(uint64_t now) {
    if (rune_goal < 0) return false;
    if (now - goal_start > 4000) { rune_goal = -1; return false; }  // (safety)
    if (goal_steps == 0) {
        if (!goal_done) goal_done = now + 250;
        if (now >= goal_done) { rune_goal = -1; g_touch_rx = 0; return false; }
    } else if (now - goal_start > 250 && now >= flick_gap_until) {  // (the wheel open first)
        flick_dir = goal_steps > 0 ? 1 : -1;
        goal_steps -= flick_dir;
        flick_until = now + FLICK_MS, flick_gap_until = flick_until + FLICK_MS;
    }
    g_touch_rx = now < flick_until ? flick_dir * 32767 : 0, g_touch_ry = 0;
    return true;
}
static int wheel_place(int rune) {  // (its place among the player's runes)
    int p = 0;
    for (int i = 0; i < rune; i++) p += runes_owned >> i & 1;
    return p;
}

static void recompute(void) {  // (t_lock held; also once a frame: the power's hold, the taps' pulses)
    uint64_t b = 0, now = SDL_GetTicks();
    int aim = -1;  // (icon layout) the finger whose drag tilts the right stick: the bow, throw, weapon, shield
    int wheel = -1;  // (the power's, held into the rune wheel: wheel_steps)
    bool wheel_start = false;
    if (icons_on) rune_now = gdt_rune(&runes_owned);
    fan_open = false;
    bow_held = bow_drawn = false;
    int pick = -1;  // (a picker's finger: its drag picks, before the bow's aims)
    for (int i = 0; i < nfingers && !editing; i++) {
        int c = fingers[i].ctl;
        if (c >= 0 && c < 16) b |= 1ull << c;
        if (c < C_ICON || c >= C_ICON + I_N) continue;
        int k = c - C_ICON;
        if (k == I_POWER) {
            bool start = !fingers[i].wheel && now - fingers[i].t0 >= POWER_HOLD_MS;
            if (start) fingers[i].wheel = true;
            if (fingers[i].wheel && rune_now >= 0 && rune_goal < 0) fan_open = true, fan_update(fingers[i].x, fingers[i].y);
            else if (fingers[i].wheel && rune_now < 0) b |= 1ull << TB_UP, wheel = i, wheel_start = start;
            continue;
        }
        if (IC[k].bit >= 0 && IC[k].bit < 16) b |= 1ull << IC[k].bit;
        if (k == I_WEAPON || k == I_SHIELD || k == I_ARROWS) aim = pick = i;
        if ((k == I_BOW || k == I_R) && !g_settings.motion) aim = i;  // (with motion controls on, the phone aims)
        if (k == I_BOW || k == I_R) bow_held = true;
        if (k == I_BOW) bow_drawn = true;
    }
    if (pick >= 0) aim = pick;
    if (surf_a_at && now >= surf_a_at) pulse_until[TB_A] = now + PULSE_MS, surf_a_at = 0;  // (shield surfing: A in the air)
    for (int i = 0; i < 16; i++) if (pulse_until[i] > now) b |= 1ull << i;
    bool goal = goal_step(now);
    if (goal) b |= 1ull << TB_UP;
    g_touch_buttons = b;
    if (goal) { aiming = true; return; }
    if (wheel >= 0) { wheel_steps(fingers[wheel].x, wheel_start, now); aiming = true; return; }
    // Another finger on the right half (the camera's) while one holds a control: the game's pickers (weapon, shield)
    // take its drag as a held stick, from where it landed - the camera's speed-driven tilt picked nothing; with the
    // bow or a throw it keeps turning the camera (drag_frame)
    int ak = aim >= 0 ? fingers[aim].ctl - C_ICON : -1;
    bool picker = ak == I_WEAPON || ak == I_SHIELD || ak == I_ARROWS;
    float ox = 0, oy = 0;  // (the drag that tilts it)
    if (aim >= 0 && stick[1].on && picker) ox = stick[1].kx - stick[1].cx, oy = stick[1].ky - stick[1].cy;
    else if (aim >= 0 && !stick[1].on) ox = fingers[aim].x - fingers[aim].sx, oy = fingers[aim].y - fingers[aim].sy;
    else aim = -1;
    if (aim >= 0) {  // the drag from where the finger landed: full at 70% of a stick's radius, past a small dead zone
        // (the bow and a throw: full at 1.3 stick radii, over touch_aim_sens - 0.7 turned the aim too fast; the pickers: 0.7)
        float sens = g_settings.touch_aim_sens > 0.1f ? g_settings.touch_aim_sens : 1.0f;
        float R = stick_radius() * (picker ? 0.7f : 1.3f / sens), dx = ox / R, dy = oy / R;
        float m = sqrtf(dx * dx + dy * dy), k = m < 0.1f ? 0 : fminf(1, (m - 0.1f) / 0.9f) / m;
        g_touch_rx = (int32_t)(dx * k * 32767), g_touch_ry = (int32_t)(-dy * k * 32767);
        aiming = true;
    } else if (aiming) {
        aiming = false;
        if (!stick[1].on) g_touch_rx = 0, g_touch_ry = 0;
    }
}

// (icon layout) a finger off an icon control: a tap shorter than PULSE_MS keeps its button that long; the power's
// tap is L, sent now (none when it was held into the rune wheel, nor when the touch was cancelled)
static void icon_release(int f, bool cancelled) {  // (t_lock held)
    int c = fingers[f].ctl;
    if (editing || c < C_ICON || c >= C_ICON + I_N) return;
    int k = c - C_ICON;
    uint64_t now = SDL_GetTicks();
    if (k == I_POWER) {
        if (!fingers[f].wheel && !cancelled) pulse_until[TB_L] = now + PULSE_MS;
        if (fingers[f].wheel && fan_open && !cancelled && fan_sel >= 0 && fan_items[fan_sel] != rune_now) {  // (the fan's pick)
            rune_goal = fan_items[fan_sel], goal_start = now, goal_done = 0, flick_until = flick_gap_until = 0;
            goal_steps = wheel_place(rune_goal) - wheel_place(rune_now);
            fprintf(stderr, "[touch] rune fan: %d -> %d (%d steps)\n", rune_now, rune_goal, goal_steps);
        }
        fan_open = false;
    }
    else if (IC[k].bit >= 0 && IC[k].bit < 16 && now - fingers[f].t0 < PULSE_MS) pulse_until[IC[k].bit] = fingers[f].t0 + PULSE_MS;
    // (mode_check: the icon layout stays until the game is back in the world - at most 5 s - unless a real menu follows)
    if (k == I_WEAPON || k == I_SHIELD || k == I_ARROWS || (k == I_POWER && fingers[f].wheel)) keep_icons_until = now + 5000;
    if (k == I_INVENTORY) keep_icons_until = 0;
    if (k == I_BLOCK) block_up_ms = now;  // (a second touch soon: shield surfing)
}

static void release_all(void) {
    pthread_mutex_lock(&t_lock);
    nfingers = 0;
    stick_release(0);
    stick_release(1);
    memset(pulse_until, 0, sizeof pulse_until), surf_a_at = 0;
    aiming = false, fan_open = false, rune_goal = -1;
    g_touch_buttons = 0;
    pthread_mutex_unlock(&t_lock);
}

// The layout in use changes (into the world, into a menu): nothing held over from the other one. But the game's
// pickers held open from the icon layout (weapon, shield, the rune wheel) count as its menus: the icon layout stays
// while a finger is on one of its controls, and after a picker's is lifted until the game is back in the world (icon_release)
static void mode_check(void) {
    bool m = icons_wanted();
    if (icons_on && !m && !editing) {
        bool held = false;
        pthread_mutex_lock(&t_lock);
        for (int i = 0; i < nfingers; i++) held |= fingers[i].ctl >= C_ICON && fingers[i].ctl < C_ICON + I_N;
        pthread_mutex_unlock(&t_lock);
        if (held || rune_goal >= 0 || SDL_GetTicks() < keep_icons_until) return;  // (also the fan stepping the wheel)
    }
    if (m && rune_goal < 0) keep_icons_until = 0;  // (back in the world: the picker has closed)
    if (m != icons_on) release_all(), icons_on = m;
}

// The on-screen controls are up (no controller, the setting on): the game's minimap group goes up to the top right,
// out of the buttons' way (hle/widescreen.c widescreen_hud_top); with a controller it stays where the game puts it
extern "C" bool touch_hud_top(void) { return !hide_auto && g_settings.touch_controls; }

// ---- layout editing (the Controls tab)
extern "C" bool touch_edit_available(void) {  // the overlay is up (in game): there is something to drag
    return SDL_GetTicks() - last_draw_ms < 500 && g_settings.touch_controls;
}

// Editing a layout (the Controls tab): icons 1 the icon layout, 0 the classic one, -1 the one on the screen
extern "C" void touch_edit_begin_layout(int icons) {
    memcpy(edit_pos, g_settings.touch_pos, sizeof edit_pos);
    memcpy(edit_pos2, g_settings.touch_pos2, sizeof edit_pos2);
    memcpy(edit_size, g_settings.touch_size, sizeof edit_size);
    memcpy(edit_size2, g_settings.touch_size2, sizeof edit_size2);
    edit_icons = icons >= 0 ? icons == 1 : g_settings.touch_scheme == 1 && widescreen_world_play();
    edit_sel = -1;
    release_all();
    editing = true;
    icons_on = edit_icons;
    used = true;
    menu_request_close();
}
extern "C" void touch_edit_begin(void) { touch_edit_begin_layout(-1); }

static void edit_end(int how) {  // C_DONE / C_RESET / C_CANCEL; C_SMALLER / C_BIGGER: the selected control's size
    if (how == C_SMALLER || how == C_BIGGER) {
        if (edit_sel < 0) return;
        float *s = edit_icons ? &edit_size2[edit_sel] : &edit_size[edit_sel];
        float v = (*s > 0.1f ? *s : 1.0f) + (how == C_BIGGER ? 0.1f : -0.1f);
        *s = v < 0.5f ? 0.5f : v > 2.0f ? 2.0f : v;
        return;
    }
    if (how == C_RESET) {  // (positions and sizes of the layout being edited)
        if (edit_icons) for (int k = 0; k < I_N; k++) edit_pos2[k][0] = edit_pos2[k][1] = -1, edit_size2[k] = 0;
        else for (int k = 0; k < K_N; k++) edit_pos[k][0] = edit_pos[k][1] = -1, edit_size[k] = 0;
        return;  // (still editing: the defaults on the screen)
    }
    if (how == C_DONE) {
        if (edit_icons) memcpy(g_settings.touch_pos2, edit_pos2, sizeof edit_pos2), memcpy(g_settings.touch_size2, edit_size2, sizeof edit_size2);
        else memcpy(g_settings.touch_pos, edit_pos, sizeof edit_pos), memcpy(g_settings.touch_size, edit_size, sizeof edit_size);
        settings_save();
    }
    release_all();
    editing = false;
    edit_sel = -1;
}

extern "C" void touch_layout_reset(void) {
    for (int k = 0; k < K_N; k++) g_settings.touch_pos[k][0] = g_settings.touch_pos[k][1] = -1, g_settings.touch_size[k] = 0;
    for (int k = 0; k < I_N; k++) g_settings.touch_pos2[k][0] = g_settings.touch_pos2[k][1] = -1, g_settings.touch_size2[k] = 0;
    settings_save();
}

extern "C" bool touch_controls_visible(void) {
    // a controller connected before the game started (its ADDED event went to the start screen): checked here too,
    // a few times a second
    static _Atomic uint64_t checked_ms;
    uint64_t now = SDL_GetTicks();
    if (now - checked_ms > 250) {
        checked_ms = now;
        bool pad = SDL_HasGamepad();
        if (pad && !hide_auto) release_all();
        hide_auto = pad;
    }
    return editing || (!hide_auto && !menu_input_captured() && g_settings.touch_controls);
}

// ---- the performance overlay (ui/perf_overlay.mm), dragged with a finger anywhere on the screen - with or without the
// on-screen controls (a controller connected) - and kept there (g_settings.overlay_pos); LASRecompAndroid's
extern float g_perf_rect[4];  // where it was last drawn (pixels)
static SDL_FingerID overlay_finger;
static bool overlay_dragging;
static float overlay_dx, overlay_dy;

static bool overlay_drag(const SDL_Event *e, float x, float y) {  // true: the finger is the overlay's
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        const float *r = g_perf_rect;
        if (overlay_dragging || !g_settings.show_fps || r[2] <= 0 || x < r[0] || x > r[0] + r[2] || y < r[1] || y > r[1] + r[3])
            return false;
        overlay_dragging = true, overlay_finger = e->tfinger.fingerID, overlay_dx = r[0] - x, overlay_dy = r[1] - y;
        return true;
    }
    if (!overlay_dragging || e->tfinger.fingerID != overlay_finger) return false;
    float nx = (x + overlay_dx) / disp_w, ny = (y + overlay_dy) / disp_h;  // (perf_overlay.mm keeps it on the screen)
    g_settings.overlay_pos[0] = nx < 0 ? 0 : nx > 1 ? 1 : nx;
    g_settings.overlay_pos[1] = ny < 0 ? 0 : ny > 1 ? 1 : ny;
    if (e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) overlay_dragging = false, settings_save();
    return true;
}

// SDL events (the UI thread): true when the event was for the on-screen controls.
extern "C" bool touch_controls_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED: hide_auto = true; release_all(); return false;
    case SDL_EVENT_GAMEPAD_REMOVED: hide_auto = SDL_HasGamepad(); return false;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_AC_BACK && !e->key.repeat) {
            if (editing) { edit_end(C_CANCEL); return true; }
            if (!menu_input_captured()) { menu_test_open(); return true; }
        }
        return false;
    case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_CANCELED: break;
    default: return false;
    }
    if (!editing && !menu_input_captured() && overlay_drag(e, e->tfinger.x * disp_w, e->tfinger.y * disp_h)) return true;
    if (!editing && (menu_input_captured() || !g_settings.touch_controls || hide_auto)) { release_all(); return false; }  // (the menu's clicks)
    used = true;
    float x = e->tfinger.x * disp_w, y = e->tfinger.y * disp_h;
    mode_check();
    pthread_mutex_lock(&t_lock);
    int f = -1;
    for (int i = 0; i < nfingers; i++) if (fingers[i].id == e->tfinger.fingerID) f = i;
    int act = C_NONE;  // (after the lock: menu / the edit bar)
    if (e->type == SDL_EVENT_FINGER_DOWN && f < 0 && nfingers < 10) {
        int k, ctl = hit(x, y, &k);
        int s = ctl == C_STICK ? 0 : ctl == C_RSTICK ? 1 : -1;
        if (s >= 0 && stick[s].on && !editing) ctl = C_NONE, s = -1;  // (one finger per stick)
        float cx, cy;
        if (k >= 0 && icons_on) pos_of2(k, &cx, &cy);
        else if (k >= 0) pos_of(k, &cx, &cy);
        else cx = x, cy = y;
        fingers[nfingers] = {e->tfinger.fingerID, ctl, x, y, cx - x, cy - y, x, y, SDL_GetTicks(), false};
        if (!editing && ctl == C_ICON + I_BLOCK && SDL_GetTicks() - block_up_ms < SURF_TAP_MS) {  // (shield surfing)
            uint64_t now = SDL_GetTicks();
            pulse_until[TB_X] = now + PULSE_MS, surf_a_at = now + SURF_A_MS;
        }
        if (editing && k >= 0 && ctl != C_NONE && !is_bar(ctl))  // (dragging layout entry k, now the one - / + resize)
            fingers[nfingers].ctl = (icons_on ? C_EDIT2 : 200) + k, edit_sel = k;
        nfingers++;
        {   // LARECOMP_DEBUG_TAP=1 (diagnosis on a phone, no F9): a third finger down dumps the next frame (as F9)
            static int tap = -1;
            if (tap < 0) tap = getenv("LARECOMP_DEBUG_TAP") != NULL;
            extern volatile int g_debug_frame_request;  // (hle/input.c)
            if (tap && nfingers == 3) g_debug_frame_request = 1, fprintf(stderr, "[touch] debug frame requested\n");
        }
        if (!editing && s >= 0) {
            stick[s].on = true;
            stick[s].cx = stick[s].kx = x, stick[s].cy = stick[s].ky = y;
            if (s == 1 && drag_camera()) drag_dx = drag_dy = drag_vx = drag_vy = 0;
            else stick_update(s);
        }
        if (ctl == C_MENU && !editing) act = C_MENU;
        if (is_bar(ctl)) act = ctl;
    } else if (e->type == SDL_EVENT_FINGER_MOTION && f >= 0) {
        float px = fingers[f].x, py = fingers[f].y;
        fingers[f].x = x, fingers[f].y = y;
        if (fingers[f].ctl == C_ICON + I_BLOCK && !editing) {
            // Parry (the game's: A with the shield up): the finger slid some way, any way - the shield stays up; again
            // from where it got to, as it slides again - not sooner than PARRY_MS after the last: a parry each slide would
            // beat any timing
            static uint64_t parry_ms;
            uint64_t now = SDL_GetTicks();
            float dx = x - fingers[f].sx, dy = y - fingers[f].sy, r = radius2(I_BLOCK) * 0.6f;
            if (dx * dx + dy * dy > r * r) {
                if (now - parry_ms > PARRY_MS) pulse_until[TB_A] = now + PULSE_MS, parry_ms = now;
                fingers[f].sx = x, fingers[f].sy = y;
            }
        }
        if (fingers[f].ctl == C_RSTICK && drag_camera()) {  // (its travel: drag_frame turns it into the tilt)
            drag_dx += x - px, drag_dy += y - py;
            stick[1].kx = x, stick[1].ky = y;
        } else if (fingers[f].ctl >= C_EDIT2 || (fingers[f].ctl >= 200 && fingers[f].ctl < 200 + K_N)) {
            // (editing) the control follows the finger, kept on the screen
            bool two = fingers[f].ctl >= C_EDIT2;
            float *p = two ? edit_pos2[fingers[f].ctl - C_EDIT2] : edit_pos[fingers[f].ctl - 200];
            float nx = (x + fingers[f].dx) / disp_w, ny = (y + fingers[f].dy) / disp_h;
            p[0] = nx < 0.02f ? 0.02f : nx > 0.98f ? 0.98f : nx;
            p[1] = ny < 0.03f ? 0.03f : ny > 0.97f ? 0.97f : ny;
        } else if ((fingers[f].ctl == C_STICK || fingers[f].ctl == C_RSTICK) && !editing) {
            int s = fingers[f].ctl == C_RSTICK;
            stick[s].kx = x, stick[s].ky = y;
            stick_update(s);
        }
    } else if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) && f >= 0) {
        if ((fingers[f].ctl == C_STICK || fingers[f].ctl == C_RSTICK) && !editing) stick_release(fingers[f].ctl == C_RSTICK);
        icon_release(f, e->type == SDL_EVENT_FINGER_CANCELED);
        fingers[f] = fingers[--nfingers];
    }
    recompute();
    pthread_mutex_unlock(&t_lock);
    if (act == C_MENU) { release_all(); menu_test_open(); }
    else if (act >= C_DONE) edit_end(act);
    return true;
}

// One persistent atlas in the menu's ImGui context (the context survives game reboots).
// Transparent frame holes let the existing fill and opacity settings show through.
static ImTextureID touch_skin;
static void load_touch_skin(void) {
    static bool attempted;
    if (attempted) return;
    attempted = true;
    char path[1100];
    if (!rt_resource("sheikah-touch-atlas.png", "assets/artwork/sheikah-touch-atlas-v1.png", path, sizeof path)) return;
    SDL_Surface *source = SDL_LoadPNG(path);
    SDL_Surface *rgba = source ? SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (source) SDL_DestroySurface(source);
    if (!rgba) return;
    std::vector<uint8_t> pixels((size_t)rgba->w * rgba->h * 4);
    for (int y = 0; y < rgba->h; y++)
        memcpy(pixels.data() + (size_t)y * rgba->w * 4,
               (const uint8_t *)rgba->pixels + (size_t)y * rgba->pitch, (size_t)rgba->w * 4);
    touch_skin = (ImTextureID)vk_imgui_texture_rgba(pixels.data(), rgba->w, rgba->h);
    if (touch_skin) fprintf(stderr, "[touch] Sheikah skin loaded (%dx%d)\n", rgba->w, rgba->h);
    SDL_DestroySurface(rgba);
}

// ---- the icon layout's pictures. The actions' (jump, sprint, crouch, whistle, inventory): one strip of five 256-pixel
// squares (assets/artwork/touch-actions-v1.png, from design/touch-controls-concept's atlas)
enum { ACT_JUMP, ACT_SPRINT, ACT_CROUCH, ACT_WHISTLE, ACT_BAG, ACT_N };
static ImTextureID act_tex;
static ImTextureID texture_from_png(const char *name, const char *dev_rel) {
    char path[1100];
    if (!rt_resource(name, dev_rel, path, sizeof path)) return 0;
    SDL_Surface *source = SDL_LoadPNG(path);
    SDL_Surface *rgba = source ? SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (source) SDL_DestroySurface(source);
    if (!rgba) return 0;
    std::vector<uint8_t> pixels((size_t)rgba->w * rgba->h * 4);
    for (int y = 0; y < rgba->h; y++)
        memcpy(pixels.data() + (size_t)y * rgba->w * 4, (const uint8_t *)rgba->pixels + (size_t)y * rgba->pitch, (size_t)rgba->w * 4);
    ImTextureID t = (ImTextureID)vk_imgui_texture_rgba(pixels.data(), rgba->w, rgba->h);
    SDL_DestroySurface(rgba);
    return t;
}

// The game's own item icons (UI/StockItem/<item>.sbitemico in its RomFS: a Yaz0-compressed BNTX of one RGBA8 texture,
// 144x144, in the GPU's block-linear layout), read from the player's game files - never shipped
enum { GI_SWORD, GI_BOW, GI_SHIELD, GI_SLATE, GI_BOMB, GI_N };
static const char *const gi_item[GI_N] = {"Weapon_Sword_070", "Weapon_Bow_036", "Weapon_Shield_030", "Obj_DRStone_Get",
                                          "Obj_RemoteBombBall"};
static ImTextureID gi_tex[GI_N];

static std::vector<uint8_t> yaz0(const uint8_t *s, size_t n) {
    std::vector<uint8_t> out;
    if (n < 16 || memcmp(s, "Yaz0", 4)) return out;
    size_t size = (size_t)s[4] << 24 | (size_t)s[5] << 16 | (size_t)s[6] << 8 | s[7], i = 16;
    if (size > (64u << 20)) return out;
    out.resize(size);
    size_t o = 0;
    while (o < size && i < n) {
        uint8_t code = s[i++];
        for (int b = 0; b < 8 && o < size && i < n; b++, code <<= 1) {
            if (code & 0x80) { out[o++] = s[i++]; continue; }
            if (i + 1 >= n) break;
            size_t b1 = s[i++], b2 = s[i++], dist = ((b1 & 0xF) << 8 | b2) + 1, len = b1 >> 4;
            if (!len) { if (i >= n) break; len = (size_t)s[i++] + 0x12; } else len += 2;
            if (dist > o) { out.clear(); return out; }
            for (; len-- && o < size; o++) out[o] = out[o - dist];
        }
    }
    if (o < size) out.clear();
    return out;
}

// A BNTX's first texture as RGBA (its RGBA8 formats only)
static bool bntx_rgba(const std::vector<uint8_t> &f, std::vector<uint8_t> &px, uint32_t *w, uint32_t *h) {
    size_t base = 0;
    while (base + 8 <= f.size() && memcmp(&f[base], "BNTX\0\0\0\0", 8)) base++;
    auto u32 = [&](size_t o) { uint32_t v = 0; if (base + o + 4 <= f.size()) memcpy(&v, &f[base + o], 4); return v; };
    auto u64 = [&](size_t o) { uint64_t v = 0; if (base + o + 8 <= f.size()) memcpy(&v, &f[base + o], 8); return v; };
    if (base + 0x30 > f.size()) return false;
    uint64_t ti = u64(u64(0x28));
    if (!ti || base + ti + 0x78 > f.size() || memcmp(&f[base + ti], "BRTI", 4)) return false;
    uint32_t fmt = u32(ti + 0x1C), bh = u32(ti + 0x34);
    *w = u32(ti + 0x24), *h = u32(ti + 0x28);
    uint64_t data = u64(u64(ti + 0x70));
    if ((fmt >> 8) != 0x0B || !*w || !*h || *w > 1024 || *h > 1024 || bh > 5) return false;  // (R8G8B8A8)
    uint32_t pitch = *w * 4, wg = (pitch + 63) / 64;
    px.assign((size_t)pitch * *h, 0);
    for (uint32_t y = 0; y < *h; y++)
        for (uint32_t x = 0; x < pitch; x++) {  // (x in bytes: the GOB layout, as tools/export_watch_map.py)
            uint32_t gy = y >> 3;
            uint64_t a = ((uint64_t)(gy >> bh) * wg + (x >> 6)) * (512u << bh) + (gy & ((1u << bh) - 1)) * 512 +
                         ((x & 63) >> 5) * 256 + ((y & 7) >> 1) * 64 + (((x & 63) >> 4) & 1) * 32 + (y & 1) * 16 + (x & 15);
            if (base + data + a >= f.size()) return false;
            px[(size_t)y * pitch + x] = f[base + data + a];
        }
    return true;
}

static ImTextureID decode_item_icon(const char *item) {  // (UI/StockItem/<item>.sbitemico; 0: not there, not decoded)
    char path[128];
    snprintf(path, sizeof path, "UI/StockItem/%s.sbitemico", item);
    size_t n = 0;
    uint8_t *raw = fs_romfs_read(path, &n);
    if (!raw) return 0;
    std::vector<uint8_t> f = yaz0(raw, n), px;
    free(raw);
    uint32_t w, h;
    return bntx_rgba(f, px, &w, &h) ? (ImTextureID)vk_imgui_texture_rgba(px.data(), (int)w, (int)h) : 0;
}

// The equipped weapon, bow and shield (hle/pouch.c), by actor name - their icons read once each and kept (a DLC item,
// in the DLC's own files: none, the fixed icon stays)
extern "C" bool pouch_equipped(char out[4][64]);
static char equipped[4][64];
static ImTextureID item_icon(const char *item) {
    static struct { char name[64]; ImTextureID tex; } cache[64];
    static int n;
    if (!item[0]) return 0;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, item)) return cache[i].tex;
    ImTextureID t = decode_item_icon(item);
    if (n < 64) snprintf(cache[n].name, sizeof cache[n].name, "%s", item), cache[n++].tex = t;
    return t;
}
static void poll_equipped(void) {  // twice a second
    static uint64_t last;
    uint64_t now = SDL_GetTicks();
    if (now - last < 500) return;
    last = now;
    char e[4][64] = {};
    if (!pouch_equipped(e) || !memcmp(e, equipped, sizeof e)) return;
    memcpy(equipped, e, sizeof equipped);
    fprintf(stderr, "[touch] equipped: %s, %s, %s, %s\n", e[0][0] ? e[0] : "-", e[1][0] ? e[1] : "-", e[2][0] ? e[2] : "-",
            e[3][0] ? e[3] : "-");
}

static void load_icon_art(void) {
    static bool act_tried;
    if (!act_tried) {
        act_tried = true;
        act_tex = texture_from_png("touch-actions.png", "assets/artwork/touch-actions-v1.png");
    }
    // (the game's: once its RomFS is open - a few tries, a second apart)
    static int tries;
    static uint64_t last_ms;
    uint64_t now = SDL_GetTicks();
    if (tries >= 5 || now - last_ms < 1000) return;
    bool missing = false;
    for (int i = 0; i < GI_N; i++) missing |= !gi_tex[i];
    if (!missing) return;
    last_ms = now, tries++;
    for (int i = 0; i < GI_N; i++) {
        if (gi_tex[i]) continue;
        gi_tex[i] = decode_item_icon(gi_item[i]);
        fprintf(stderr, "[touch] icon %s: %s\n", gi_item[i], gi_tex[i] ? "loaded" : "not there yet");
    }
}

enum { SKIN_BUTTON, SKIN_SHOULDER, SKIN_STICK, SKIN_KNOB };
static bool skin_image(ImDrawList *dl, int sprite, ImVec2 a, ImVec2 b, ImU32 tint) {
    if (!touch_skin) return false;
    // Pixel bounds of the four independent drawings in the original 1254-square atlas.
    static const float bounds[4][4] = {{24, 26, 602, 562}, {633, 185, 1247, 412},
                                      {22, 617, 606, 1187}, {706, 677, 1175, 1138}};
    const float *uv = bounds[sprite];
    dl->AddImage(touch_skin, a, b, ImVec2(uv[0] / 1254, uv[1] / 1254),
                 ImVec2(uv[2] / 1254, uv[3] / 1254), tint);
    return true;
}

static void sheikah_eye(ImDrawList *dl, ImVec2 p, float r, ImU32 color) {
    float w = r * 0.55f, h = r * 0.22f, stroke = fmaxf(1, r * 0.06f);
    dl->PathLineTo(ImVec2(p.x - w, p.y));
    dl->PathBezierCubicCurveTo(ImVec2(p.x - w * 0.35f, p.y - h * 1.8f),
                              ImVec2(p.x + w * 0.35f, p.y - h * 1.8f), ImVec2(p.x + w, p.y));
    dl->PathBezierCubicCurveTo(ImVec2(p.x + w * 0.35f, p.y + h * 1.8f),
                              ImVec2(p.x - w * 0.35f, p.y + h * 1.8f), ImVec2(p.x - w, p.y));
    dl->PathStroke(color, ImDrawFlags_Closed, stroke);
    dl->AddCircle(p, r * 0.15f, color, 16, stroke);
    for (int i = -1; i <= 1; i++) {
        float x = p.x + i * r * 0.28f, y = p.y - r * 0.38f;
        dl->AddTriangleFilled(ImVec2(x - r * 0.08f, y), ImVec2(x + r * 0.08f, y),
                              ImVec2(x, y - r * 0.18f), color);
    }
    dl->AddTriangleFilled(ImVec2(p.x - r * 0.09f, p.y + h), ImVec2(p.x + r * 0.09f, p.y + h),
                          ImVec2(p.x, p.y + r * 0.66f), color);
}

// Two curved arrows around a control (the weapon and shield pickers): a swap
static void swap_arrows(ImDrawList *dl, ImVec2 p, float r, ImU32 col) {
    float t = fmaxf(1.5f, r * 0.09f), rr = r * 0.86f;
    for (int i = 0; i < 2; i++) {
        float a0 = i ? 0.35f : 3.49f, a1 = a0 + 1.6f;  // (radians: one upper right, one lower left)
        dl->PathArcTo(p, rr, a0, a1, 16);
        dl->PathStroke(col, 0, t);
        float ex = p.x + cosf(a1) * rr, ey = p.y + sinf(a1) * rr, tx = -sinf(a1), ty = cosf(a1), h = r * 0.2f;
        dl->AddTriangleFilled(ImVec2(ex + tx * h, ey + ty * h), ImVec2(ex + cosf(a1) * h * 0.6f, ey + sinf(a1) * h * 0.6f),
                              ImVec2(ex - cosf(a1) * h * 0.6f, ey - sinf(a1) * h * 0.6f), col);
    }
}

// The scope (R3): a spyglass
static void spyglass(ImDrawList *dl, ImVec2 p, float r, ImU32 col) {
    float t = fmaxf(1.5f, r * 0.1f);
    dl->AddCircle(ImVec2(p.x - r * 0.12f, p.y - r * 0.12f), r * 0.32f, col, 24, t);
    dl->AddLine(ImVec2(p.x + r * 0.12f, p.y + r * 0.12f), ImVec2(p.x + r * 0.45f, p.y + r * 0.45f), col, t * 1.6f);
}

// (icon layout) its controls and the left stick; the right one only while a finger holds it
static void draw_icons(ImDrawList *dl, ImFont *font, float alpha, float op, ImU32 fill, ImU32 fill_on, ImU32 line, ImU32 text) {
    load_icon_art();
    poll_equipped();
    uint32_t pressed = 0;  // (the controls a finger is on)
    pthread_mutex_lock(&t_lock);
    for (int i = 0; i < nfingers; i++)
        if (fingers[i].ctl >= C_ICON && fingers[i].ctl < C_ICON + I_N) pressed |= 1u << (fingers[i].ctl - C_ICON);
    pthread_mutex_unlock(&t_lock);
    ImU32 img = IM_COL32(255, 255, 255, (int)(235 * alpha)), cyan = IM_COL32(66, 215, 245, (int)(220 * alpha));
    for (int k = 0; k < I_N; k++) {
        if (k == I_MOVE || !shown2(k)) continue;
        float x, y, r = radius2(k);
        pos_of2(k, &x, &y);
        ImVec2 c(x, y), a(x - r, y - r), b(x + r, y + r);
        dl->AddCircleFilled(c, r, pressed >> k & 1 ? fill_on : fill, 40);
        if (!skin_image(dl, SKIN_BUTTON, a, b, line)) dl->AddCircle(c, r, line, 40, fmaxf(1, r * 0.04f));
        if (k == I_MENU) { sheikah_eye(dl, c, r, IM_COL32(66, 215, 245, (int)(200 * alpha))); continue; }
        if (k == I_CANCEL) {  // (a cross)
            float t = r * 0.38f, w = fmaxf(2.0f, r * 0.12f);
            ImU32 red = IM_COL32(240, 90, 80, (int)(235 * alpha));
            dl->AddLine(ImVec2(x - t, y - t), ImVec2(x + t, y + t), red, w);
            dl->AddLine(ImVec2(x - t, y + t), ImVec2(x + t, y - t), red, w);
            continue;
        }
        if (k >= I_PUSH && k <= I_MRIGHT) {  // (Magnesis's d-pad: an arrow its way, the magnet's cyan)
            static const float dir[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};  // (I_PUSH, I_PULL, I_MLEFT, I_MRIGHT)
            float dx = dir[k - I_PUSH][0], dy = dir[k - I_PUSH][1], t = r * 0.45f;
            dl->AddTriangleFilled(ImVec2(x + dx * t, y + dy * t), ImVec2(x - dx * t * 0.6f - dy * t, y - dy * t * 0.6f + dx * t),
                                  ImVec2(x - dx * t * 0.6f + dy * t, y - dy * t * 0.6f - dx * t), cyan);
            continue;
        }
        int act = k == I_SPRINT && talk_on() ? -1 : k == I_JUMP ? ACT_JUMP : k == I_SPRINT ? ACT_SPRINT : k == I_CROUCH ? ACT_CROUCH : k == I_WHISTLE ? ACT_WHISTLE
                : k == I_INVENTORY ? ACT_BAG : -1;
        int gi = k == I_ATTACK || k == I_WEAPON ? GI_SWORD : k == I_BOW ? GI_BOW : k == I_BLOCK || k == I_SHIELD ? GI_SHIELD
               : k == I_SLATE ? GI_SLATE : k == I_POWER ? GI_BOMB : -1;
        float box = r * (k == I_BOW ? 0.5f : k == I_WEAPON || k == I_SHIELD || k == I_ARROWS ? 0.52f : 0.66f);  // (half the picture's side)
        ImVec2 ia(x - box, y - box), ib(x + box, y + box);
        // (the equipped item's icon where there is one: the weapon picker shows the bow while it's drawn)
        int eq = k == I_ATTACK || k == I_WEAPON ? (k == I_WEAPON && (pressed >> I_BOW & 1) ? 1 : 0) : k == I_BOW ? 1
               : k == I_BLOCK || k == I_SHIELD ? 2 : k == I_ARROWS ? 3 : -1;
        ImTextureID it = eq >= 0 ? item_icon(equipped[eq]) : 0;
        if (k == I_POWER && rune_now >= 0) it = item_icon(rune_item[rune_now]);  // (the selected rune)
        if (act >= 0 && act_tex) dl->AddImage(act_tex, ia, ib, ImVec2(act / (float)ACT_N, 0), ImVec2((act + 1) / (float)ACT_N, 1), img);
        else if (it) dl->AddImage(it, ia, ib, ImVec2(0, 0), ImVec2(1, 1), img);
        else if (gi >= 0 && gi_tex[gi]) dl->AddImage(gi_tex[gi], ia, ib, ImVec2(0, 0), ImVec2(1, 1), img);
        else if (k == I_SCOPE) spyglass(dl, c, r, text);
        else {  // (no picture: interact and throw, or the game's icons not read yet)
            static const char *const label[I_N] = {"", "L3", ">", "<", "v", "R3", "", "+", "Y", "L", "ZL", "ZR", "X", "B", "A", "R", "", "^", "v", "<", ">", "B", "<", "-"};
            float fs = r * 0.9f;
            ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label[k]);
            dl->AddText(font, fs, ImVec2(x - ts.x / 2, y - ts.y / 2), text, label[k]);
        }
        if (k == I_WEAPON || k == I_SHIELD || k == I_ARROWS) swap_arrows(dl, c, r, cyan);
    }
    // the rune fan, while the power is held
    pthread_mutex_lock(&t_lock);
    bool fo = fan_open;
    int n = fan_n, sel = fan_sel, items[8];
    memcpy(items, fan_items, sizeof items);
    pthread_mutex_unlock(&t_lock);
    if (fo && n > 0) {  // a half ring of slices, each a rune's icon at its middle
        float cx, cy, ri, ro;
        fan_ring(&cx, &cy, &ri, &ro);
        float span = FAN_PI / n, gap = 0.03f;
        ImU32 rim = IM_COL32(255, 255, 255, (int)(150 * alpha)), shade = IM_COL32(20, 30, 33, (int)(170 * alpha));
        for (int i = 0; i < n; i++) {
            float a0 = -FAN_PI + i * span + gap, a1 = -FAN_PI + (i + 1) * span - gap;
            ImU32 col = i == sel ? fill_on : shade;
            const int seg = 12;
            for (int s = 0; s < seg; s++) {  // (quads: an annular sector isn't convex)
                float b0 = a0 + (a1 - a0) * s / seg, b1 = a0 + (a1 - a0) * (s + 1) / seg;
                dl->AddQuadFilled(ImVec2(cx + cosf(b0) * ri, cy + sinf(b0) * ri), ImVec2(cx + cosf(b0) * ro, cy + sinf(b0) * ro),
                                  ImVec2(cx + cosf(b1) * ro, cy + sinf(b1) * ro), ImVec2(cx + cosf(b1) * ri, cy + sinf(b1) * ri), col);
            }
            dl->PathArcTo(ImVec2(cx, cy), ro, a0, a1, seg);
            dl->PathArcTo(ImVec2(cx, cy), ri, a1, a0, seg);
            dl->PathStroke(items[i] == rune_now ? cyan : rim, ImDrawFlags_Closed, fmaxf(1.5f, ri * (items[i] == rune_now ? 0.05f : 0.025f)));
            float am = (a0 + a1) * 0.5f, rm = (ri + ro) * 0.5f, x = cx + cosf(am) * rm, y = cy + sinf(am) * rm;
            float b = fminf((ro - ri) * 0.36f, rm * span * 0.42f) * (i == sel ? 1.15f : 1.0f);
            if (ImTextureID it = item_icon(rune_item[items[i]]))
                dl->AddImage(it, ImVec2(x - b, y - b), ImVec2(x + b, y + b), ImVec2(0, 0), ImVec2(1, 1), img);
        }
    }
    (void)op;
}

// Drawn with the in-game overlay (ui/menu.mm menu_render), between ImGui's NewFrame and Render.
extern "C" void touch_controls_draw(ImVec2 display, ImFont *font) {
    disp_w = display.x, disp_h = display.y;
    last_draw_ms = SDL_GetTicks();
    mode_check();
    pthread_mutex_lock(&t_lock);
    drag_frame();
    recompute();  // (the power held into the rune wheel, the taps' pulses ending)
    pthread_mutex_unlock(&t_lock);
    if (!touch_controls_visible()) return;
    load_touch_skin();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    Ctl c[32];
    int n = icons_on ? (editing ? edit_bar(c) : 0) : layout(c);  // (the icon layout's controls: draw_icons)
    uint64_t held = g_touch_buttons;
    float u = disp_h / 100.0f;
    float op = g_settings.touch_opacity > 0.05f ? g_settings.touch_opacity : 1.0f;
    float alpha = (used || editing ? 1.0f : 0.6f) * op;
    // Alpha values and idle fading are unchanged from the original touch controls.
    ImU32 fill = IM_COL32(20, 30, 33, (int)(40 * alpha)), fill_on = IM_COL32(45, 205, 235, (int)(120 * op));
    ImU32 line = IM_COL32(255, 255, 255, (int)(110 * alpha)), text = IM_COL32(240, 235, 219, (int)(200 * alpha));
    if (editing) {
        dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 90));
        const char *tip = tr("Drag the controls to move them");
        float fs = 4.5f * u;
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, tip);
        dl->AddText(font, fs, ImVec2(disp_w / 2 - ts.x / 2, 20 * u), IM_COL32(255, 255, 255, 230), tip);
    }
    if (icons_on) draw_icons(dl, font, alpha, op, fill, fill_on, line, text);
    for (int i = 0; i < n; i++) {
        bool bar = is_bar(c[i].bit);
        bool on = c[i].bit >= 0 && c[i].bit < 16 && (held >> c[i].bit & 1);
        float fs = (c[i].rect ? c[i].h * (bar ? 0.45f : 0.48f) : c[i].r * 0.9f);
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, c[i].label);
        ImU32 f = bar ? IM_COL32(60, 90, 70, 230) : on ? fill_on : fill, l = bar ? IM_COL32(236, 194, 84, 255) : line;
        ImU32 tx = bar ? IM_COL32(255, 255, 255, 255) : text;
        if (c[i].rect) {
            ImVec2 a(c[i].x - c[i].w / 2, c[i].y - c[i].h / 2), b(c[i].x + c[i].w / 2, c[i].y + c[i].h / 2);
            dl->AddRectFilled(a, b, f, 1.5f * u);
            if (bar || !skin_image(dl, SKIN_SHOULDER, a, b, l))
                dl->AddRect(a, b, l, 1.5f * u, 0, 0.3f * u);
        } else {
            dl->AddCircleFilled(ImVec2(c[i].x, c[i].y), c[i].r, f, 32);
            ImVec2 a(c[i].x - c[i].r, c[i].y - c[i].r), b(c[i].x + c[i].r, c[i].y + c[i].r);
            if (!skin_image(dl, SKIN_BUTTON, a, b, l))
                dl->AddCircle(ImVec2(c[i].x, c[i].y), c[i].r, l, 32, 0.3f * u);
        }
        if (c[i].bit == C_MENU) {
            sheikah_eye(dl, ImVec2(c[i].x, c[i].y), c[i].r, IM_COL32(66, 215, 245, (int)(200 * alpha)));
        } else if (c[i].bit >= TB_LEFT && c[i].bit <= TB_DOWN) {  // d-pad: a triangle pointing its way
            static const float dir[4][2] = {{-1, 0}, {0, -1}, {1, 0}, {0, 1}};  // LEFT, UP, RIGHT, DOWN
            const float *d = dir[c[i].bit - TB_LEFT];
            float r = c[i].r * 0.5f, px = -d[1], py = d[0];
            ImVec2 tip(c[i].x + d[0] * r, c[i].y + d[1] * r);
            ImVec2 b1(c[i].x - d[0] * r * 0.6f + px * r * 0.8f, c[i].y - d[1] * r * 0.6f + py * r * 0.8f);
            ImVec2 b2(c[i].x - d[0] * r * 0.6f - px * r * 0.8f, c[i].y - d[1] * r * 0.6f - py * r * 0.8f);
            dl->AddTriangleFilled(tip, b1, b2, tx);
        } else {
            dl->AddText(font, fs, ImVec2(c[i].x - ts.x / 2, c[i].y - ts.y / 2), tx, c[i].label);
        }
    }
    // the sticks: where each started (or its resting place) and the knob - none for the drag camera, and only while a
    // finger holds them with "only while touched" (but always while editing the layout: they're dragged into place)
    for (int s = 0; s < 2; s++) {
        if (s == 1 && drag_camera()) continue;
        pthread_mutex_lock(&t_lock);
        bool on = stick[s].on;
        if (g_settings.touch_sticks_hidden && !on && !editing) { pthread_mutex_unlock(&t_lock); continue; }
        if (icons_on && s == 1 && !on) { pthread_mutex_unlock(&t_lock); continue; }  // (the icon layout's: only while held)
        float R = stick_r(s);
        float rx, ry;
        if (icons_on) pos_of2(I_MOVE, &rx, &ry);
        else pos_of(s ? K_RSTICK : K_STICK, &rx, &ry);
        float cx = on ? stick[s].cx : rx, cy = on ? stick[s].cy : ry, kx = on ? stick[s].kx : cx, ky = on ? stick[s].ky : cy;
        pthread_mutex_unlock(&t_lock);
        dl->AddCircleFilled(ImVec2(cx, cy), R, fill, 48);
        if (!skin_image(dl, SKIN_STICK, ImVec2(cx - R, cy - R), ImVec2(cx + R, cy + R), line))
            dl->AddCircle(ImVec2(cx, cy), R, line, 48, 0.3f * u);
        float kr = R * 0.45f;
        ImU32 knob = IM_COL32(255, 255, 255, (int)(on ? 120 * op : 70 * alpha));
        if (!skin_image(dl, SKIN_KNOB, ImVec2(kx - kr, ky - kr), ImVec2(kx + kr, ky + kr), knob))
            dl->AddCircleFilled(ImVec2(kx, ky), kr, on ? fill_on : IM_COL32(20, 30, 33, (int)(70 * alpha)), 32);
    }
    if (editing && edit_sel >= 0) {  // the control the bar's - / + resize: a gold ring
        ImU32 gold = IM_COL32(236, 194, 84, 255);
        float x, y, r = 0;
        if (icons_on) {
            pos_of2(edit_sel, &x, &y);
            r = edit_sel == I_MOVE ? stick_r(0) : radius2(edit_sel);
        } else if (edit_sel == K_STICK || edit_sel == K_RSTICK) {
            pos_of(edit_sel, &x, &y);
            r = stick_r(edit_sel == K_RSTICK);
        } else {
            for (int i = 0; i < n; i++)
                if (c[i].k == edit_sel) x = c[i].x, y = c[i].y, r = c[i].rect ? fmaxf(c[i].w, c[i].h) * 0.6f : c[i].r;
        }
        if (r > 0) dl->AddCircle(ImVec2(x, y), r * 1.12f, gold, 48, fmaxf(2, 0.5f * u));
    }
}
#endif
