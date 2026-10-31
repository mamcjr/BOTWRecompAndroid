// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <vector>

#include "i18n.h"
#include "imgui.h"
#include "option_rows.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
#endif

extern "C" {
#include "../hle.h"
#include "../settings.h"
#include "menu.h"
void input_reset(void);
void controls_bindings_draw(float scale);  // controls_ui.mm
#include "../input_bind.h"
#include "../paths.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_fullscreen_apply{false};

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT } kind;
    ImGuiKey key;
    bool down;
    bool touch;  // (MOUSE_POS / MOUSE_BUTTON) SDL's mouse events made from a finger
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return g_open.load(); }
#ifdef __ANDROID__
extern "C" bool touch_controls_visible(void);  // ui/touch_controls.mm
extern "C" void touch_controls_draw(ImVec2 display, ImFont *font);
#endif
static void set_open(bool open);
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("LARECOMP_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("LARECOMP_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
#ifdef __ANDROID__
static void set_open(bool open);
extern "C" void menu_request_close(void) { set_open(false); }  // (ui/touch_controls.mm: editing the layout)
#endif
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}

// Nintendo layout: the right face button (A) confirms, the bottom one (B) goes back.
static ImGuiKey key_from_pad(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceDown;   // activate
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceRight; // cancel / back
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + A, B deletes, Start / + OK) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
static bool capturing() { return g_open.load() || g_kbd.load() == 1; }
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    static bool back_down, start_down;
    {   // LARECOMP_LOG_POINTER=1 (debugging touch in the menu): the pointer events the menu gets
        static int logp = -1;
        if (logp < 0) logp = getenv("LARECOMP_LOG_POINTER") != NULL;
        if (logp && (e->type == SDL_EVENT_MOUSE_MOTION || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP ||
                     e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_FINGER_UP))
            LOG("[menu] event %#x which %u button %d at %.0f %.0f (open %d)\n", e->type, (unsigned)e->button.which, e->button.button,
                e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_FINGER_UP ? e->tfinger.x : e->button.x,
                e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_FINGER_UP ? e->tfinger.y : e->button.y, (int)g_open.load());
    }
    if (g_open.load() && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing) set_open(!g_open.load());  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.fullscreen = !g_settings.fullscreen;
            g_want_fullscreen_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.scancode == SDL_SCANCODE_ESCAPE) {  // Esc: back out of popups, else close
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        if (down && !typing && ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = key_from_pad(e->gbutton.button);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_POS;
            q.x = e->motion.x * g_pixel_density, q.y = e->motion.y * g_pixel_density;
            q.touch = e->motion.which == SDL_TOUCH_MOUSEID;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.touch = e->button.which == SDL_TOUCH_MOUSEID;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            q.touch = e->button.which == SDL_TOUCH_MOUSEID;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
#ifdef __ANDROID__
    // With a controller, the in-app keyboard only: Android's own keyboard would open over it, and while it is up it
    // takes the controller's stick and d-pad (after naming Link the stick no longer moved him; buttons still worked)
    if (want_text && SDL_HasGamepad()) want_text = false;
    // ... and otherwise only while a text field is being typed in, without the in-app keyboard (the name entry has
    // it): with the menu merely open, Android's keyboard came up over it every time
    extern bool osk_is_open(void);
    if (want_text && (!g_text_busy.load() || osk_is_open())) want_text = false;
#endif
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    if (first || g_want_fullscreen_apply.exchange(false)) {
        first = false;
#ifdef __ANDROID__
        SDL_SetWindowFullscreen(w, true);  // (always: there is no window to go back to)
#else
        SDL_SetWindowFullscreen(w, g_settings.fullscreen);
#endif
    }
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours: the start screen (back to it after a reboot) has its own context
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Baloo 2); everything else keeps g_font
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The start screen's Settings opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
void menu_tabs_focus(bool first_tab) { g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0; }
int g_tab_request_ext = -2;  // (a test's tab: -1 Files / Prelaunch; -2 none)

static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 24); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;
    ImVec4 gold(0.93f, 0.76f, 0.33f, 1), gold_dim(0.62f, 0.49f, 0.20f, 1), green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.08f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.13f, 0.11f, 0.85f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.10f, 0.09f, 0.98f);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.64f, 0.58f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.19f, 0.16f, 1);
    c[ImGuiCol_FrameBgHovered] = green;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.18f, 1);
    c[ImGuiCol_ButtonHovered] = green;
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Header] = green;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.15f, 1);
    c[ImGuiCol_TabHovered] = green;
    c[ImGuiCol_TabSelected] = ImVec4(0.21f, 0.44f, 0.31f, 1);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold_dim;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
#ifdef __ANDROID__  // (the Sheikah look, design/sheikah-settings-v1: charcoal blue, cyan where something is picked)
    ImVec4 teal(0.05f, 0.11f, 0.13f, 0.95f), teal_hot(0.08f, 0.25f, 0.30f, 1), teal_on(0.10f, 0.36f, 0.42f, 1), cyan(0.25f, 0.87f, 0.96f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.065f, 0.075f, 0.92f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.035f, 0.065f, 0.075f, 0.98f);
    c[ImGuiCol_Text] = ImVec4(0.94f, 0.92f, 0.85f, 1);
    c[ImGuiCol_Border] = ImVec4(0.73f, 0.62f, 0.38f, 0.55f);
    c[ImGuiCol_FrameBg] = teal;
    c[ImGuiCol_FrameBgHovered] = teal_hot;
    c[ImGuiCol_FrameBgActive] = teal_on;
    c[ImGuiCol_Button] = teal;
    c[ImGuiCol_ButtonHovered] = teal_hot;
    c[ImGuiCol_ButtonActive] = teal_on;
    c[ImGuiCol_Header] = teal_hot;
    c[ImGuiCol_HeaderHovered] = teal_on;
    c[ImGuiCol_HeaderActive] = teal_on;
    c[ImGuiCol_CheckMark] = cyan;
    c[ImGuiCol_SliderGrab] = ImVec4(0.25f, 0.70f, 0.80f, 1);
    c[ImGuiCol_SliderGrabActive] = cyan;
    c[ImGuiCol_NavCursor] = cyan;
    c[ImGuiCol_Separator] = ImVec4(0.73f, 0.62f, 0.38f, 0.45f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.33f, 0.32f, 0.8f);
    s.FrameRounding = 4; s.ChildRounding = 4; s.WindowRounding = 6;
    s.FrameBorderSize = 1;
#endif
}


#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    for (const char *p : paths) {
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg);
        if (g_font && !g_font_big) { ImFontConfig b = cfg; b.FontNo = 0; g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b); }
    }
    // the menu window itself: Baloo 2 SemiBold, as on the start screen (shipped, OFL), at its Settings window's sizes; the
    // FPS counter, overlays and on-screen keyboard keep the fonts above
    char baloo[1024];
    if (const char *f = getenv("LARECOMP_MENU_FONT")) strlcpy(baloo, f, sizeof baloo);  // (test) another weight / face
    if (getenv("LARECOMP_MENU_FONT") || rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(baloo, 39, &cfg);
        i18n_merge_fonts(io.Fonts, 39);  // (the menu language's other letters: ui/i18n.mm)
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(baloo, 72, &cfg);
        i18n_merge_fonts(io.Fonts, 72);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
    if (!io.Fonts->Build()) {  // (a font ImGui couldn't read left the atlas empty: its upload crashed - ImGui's own font)
        fprintf(stderr, "[ui] the menu's fonts didn't build: ImGui's default font instead\n");
        io.Fonts->Clear();
        g_font = g_font_big = g_menu_font = g_menu_font_big = io.Fonts->AddFontDefault();
    }
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        // (a finger: ImGui's touch source - it gives the item a frame under the pointer before the press; a tab,
        // which needs to have been hovered the frame before (overlap), ignored quick taps as a mouse's clicks)
        case QEvent::MOUSE_POS:
            io.AddMouseSourceEvent(q.touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(q.x, q.y);
            break;
        case QEvent::MOUSE_BUTTON:
            io.AddMouseSourceEvent(q.touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
            io.AddMouseButtonEvent(q.button, q.down);
            break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        }
    }
}

// Section header in the big font (Android: the Sheikah look's heading).
static void header(const char *t) {
#ifdef __ANDROID__
    ui_sheikah_heading(t, ImGui::GetFontSize() / 34);
    return;
#endif
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr(t));
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
}

static void hint(const char *t) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr(t));
    ImGui::PopTextWrapPos();
}

// A row with a label on the left and a choice of options on the right (dpad left/right cycles).
// reserve: room kept free right of the combo (a button on the same line)
static bool choice(const char *label, int *value, const char *const *names, int n, float reserve = 0) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr(label));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
    bool changed = false;
    ImGui::SetNextItemWidth(reserve > 0 ? -reserve : -1);
    if (ImGui::BeginCombo("##c", tr(names[*value]))) {
        for (int i = 0; i < n; i++) {
            ImGui::PushID(i);  // (the shown names may repeat once translated)
            if (ImGui::Selectable(tr(names[i]), i == *value)) { *value = i; changed = true; }
            ImGui::PopID();
            if (i == *value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemFocused() && !ImGui::IsPopupOpen("##c")) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            *value = (*value + 1) % n; changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            *value = (*value + n - 1) % n; changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

static bool toggle(const char *label, bool *v) {
    const char *names[] = {"Off", "On"};
    int i = *v;
    bool ch = choice(label, &i, names, 2);
    *v = i;
    return ch;
}

extern "C" void renderer_get_scene_scale(float *sx, float *sy);  // (vulkan/textures.c: the 3D scene's)
extern "C" void renderer_get_scale(float *sx, float *sy);        // (the final image's)
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    const char *aspects[] = {"Unlocked", "Original 16:9"};
    opt_choice("Aspect ratio", &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               "Unlocked: the view fills the screen, so wider screens show more of Hyrule, and the HUD moves out to its "
               "edges (menus stay in the middle). Game versions 1.8.2 and 1.9.0 (1.0.0 and 1.6.0 keep 16:9 with bars); a change applies "
               "when the game starts again (Quit -> Return to start, then Play). Original 16:9: the game's own framing, "
               "with bars.");
#ifndef __ANDROID__  // (Android: always the whole screen)
    bool fs = g_settings.fullscreen;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. Also: F11, Alt+Enter or Cmd+Ctrl+F.")) {
        g_settings.fullscreen = fs;
        g_want_fullscreen_apply.store(true);
    }
#endif
#ifdef __ANDROID__
    opt_toggle("Show FPS", &g_settings.show_fps, false,
               "Shows the performance overlay: the frame rate and how the phone is doing. Drag it with a finger to move it.");
    {   // (ui/perf_overlay.mm; LASRecompAndroid's)
        static const char *styles[] = {"Full", "Compact"};
        if (!g_settings.show_fps) opt_disable_next();
        opt_choice("Overlay style", &g_settings.overlay_style, styles, 2, 0,
                   "Full: frame rate and frame times, CPU, GPU, memory, temperatures and power. Compact: one line - FPS, "
                   "the app's RAM, the SoC's and the battery's temperature. Either one can be dragged anywhere on the "
                   "screen with a finger.");
    }
#else
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");
#endif
#ifdef __ANDROID__
    opt_toggle("Touch controls", &g_settings.touch_controls, false,
               "On-screen buttons and stick while no controller is connected. The back button opens this menu.");
    {
        // 1.8.2: up to 60 (hle/framepace.c keeps the game's clock right at any rate). 1.0.0 / 1.6.0 never run above
        // 30 - their frame loop picks 2 or 3 vsyncs - so a 60 there counts as 30. (On the start screen the version
        // isn't loaded yet: the 1.8.2 choices.)
        extern int g_game_version;
        bool sixty = g_game_version == 0 || g_game_version >= 182;
        if (!sixty && g_settings.fps_mode > 1) g_settings.fps_mode = 1;
        static const char *fr[] = {"Auto (20 / 30)", "30 fps"};
        static const char *fr60[] = {"Auto (30 / 60)", "30 fps", "60 fps"};
        opt_choice("Frame rate", &g_settings.fps_mode, sixty ? fr60 : fr, sixty ? 3 : 2, 1,
                   sixty ? "Auto: 60 fps where your device keeps up, 30 where it doesn't (the game's clock stays right "
                           "either way). 30 fps: steady and cooler - the phone heats less. 60 fps: always aims for 60 "
                           "(game versions 1.8.2 and 1.9.0; 1.0.0 and 1.6.0 run at 30)."
                         : "Auto: the game picks, as on the Switch, and drops to 20 fps where a frame doesn't fit in 30. "
                           "30 fps: always 30 - where a frame takes longer, the game runs slightly slower instead of "
                           "dropping to 20.");
        if (sixty)
            opt_toggle("Cutscenes at 60 fps (experimental)", &g_settings.event_fps_unlock, false,
                       "In-game cutscenes and talks run at the chosen frame rate instead of 30. Some of them count frames "
                       "and may run shorter or out of step with their sound (at 60 fps the new game's opening gave Link "
                       "control 2 s early). Videos and menus stay at 30. Applies at once.");
    }
#endif

    opt_section("Rendering");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
#ifdef __ANDROID__  // (3x / 4x: 4K / 8K, beyond a phone)
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "1x", "2x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, 2};
#else
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "1x", "2x", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, 2, 3, 4};
#endif
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT;
        for (int i = 0; i < NRES; i++) if (res_mode[i] == g_settings.res_mode) ri = i;
        float sx, sy;
        renderer_get_scene_scale(&sx, &sy);
        char d[640];
        int n = snprintf(d, sizeof d, tr("How sharp the 3D image is (the HUD is drawn at the Output resolution). Match "
                                         "display: the final image's resolution (your screen's, unless Output resolution is "
                                         "lower). Dynamic: as sharp as your GPU keeps at "
                                         "full speed. 1x-2x: the Switch's 900p times that. 75%% / 50%% / 25%%: a share of the "
                                         "display, for slower devices.\n\nRendering the 3D image at %.0f x %.0f."),
                         1600.0 * sx, 900.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC && n > 0 && n < (int)sizeof d)
            snprintf(d + n, sizeof d - n, tr(" Dynamic: %.0f%% of the display right now."), output_scale_dynamic_factor() * 100);
        if (opt_choice("Internal resolution", &ri, res, NRES, RES_MATCH_AT, d, false)) g_settings.res_mode = res_mode[ri];
    }
    {   // bilinear next to each OS's own upscaler (MetalFX on macOS) or AMD FSR 1 and Snapdragon GSR 1 (Vulkan)
#ifdef __APPLE__
        static const char *ups[] = {"Bilinear", "MetalFX"};
        static const int up_mode[] = {UPSCALE_BILINEAR, UPSCALE_METALFX};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Bilinear: plain and soft.";
#else
        static const char *ups[] = {"Bilinear", "AMD FSR 1", "Snapdragon GSR 1"};
        static const int up_mode[] = {UPSCALE_BILINEAR, UPSCALE_FSR1, UPSCALE_SGSR1};
        const char *ud = "How the 3D image is scaled up to the screen when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening, the sharpest, two passes. Snapdragon GSR 1: one light pass, "
                         "softer and more natural, the faster (made for phones, works on any GPU). Bilinear: plain and soft.";
#endif
        enum { NUP = sizeof up_mode / sizeof up_mode[0] };
        int ui = 1;
        for (int i = 0; i < NUP; i++) if (up_mode[i] == g_settings.upscaler) ui = i;
        char udm[768];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, tr("%s\n\nNot used at Match display: the game already renders at your display's resolution."), tr(ud));
            ud = udm;
            opt_disable_next();
        }
#ifdef __ANDROID__
        enum { UP_DEFAULT = 2 };  // (Snapdragon GSR 1: settings.c)
#else
        enum { UP_DEFAULT = 1 };
#endif
        if (opt_choice("Upscaler", &ui, ups, NUP, UP_DEFAULT, ud)) g_settings.upscaler = up_mode[ui];
    }
    {   // the final image's lines (nvn/output_scale.c): the screen's, or fewer, scaled up to it when presented
        static const char *outs[] = {"Auto", "Screen", "1080p", "900p", "720p"};
        static const int out_lines[] = {OUT_AUTO, 0, 1080, 900, 720};
        int oi = 1;
        for (int i = 0; i < 5; i++) if (out_lines[i] == g_settings.out_lines) oi = i;
        float ox, oy;
        renderer_get_scale(&ox, &oy);
        char od[1280];
        int on = snprintf(od, sizeof od, tr("How many pixels the final image has - the 3D scene once scaled up, and the HUD - before "
                                   "it reaches the screen. Screen: your screen's own, the sharpest. 1080p / 900p / 720p: "
                                   "fewer, scaled up at the very end: the game's last passes and the upscaler cost less "
                                   "(most on big and wide screens), the HUD and text are a little softer. Match display "
                                   "(Internal resolution) follows it.\n\nThe final image is %.0f x %.0f."),
                 1920.0 * ox, 1080.0 * oy);
        if (on > 0 && on < (int)sizeof od)
            snprintf(od + on, sizeof od - on, "\n\n%s", tr("Auto: 1080p on screens with more lines than that, where it costs the "
                                                           "GPU the most; the screen's own on the others."));
#ifdef __ANDROID__
        enum { OUT_DEFAULT = 0 };  // (Auto: settings.c)
#else
        enum { OUT_DEFAULT = 1 };
#endif
        if (opt_choice("Output resolution", &oi, outs, 5, OUT_DEFAULT, od, false)) g_settings.out_lines = out_lines[oi];
    }
    static const char *aa[] = {"Off", "SMAA", "FXAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 3, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. FXAA: a single, cheaper pass that also softens the image "
               "a little. Most useful at a lower internal resolution.");
#ifdef __ANDROID__
    opt_toggle("Sync lag", &g_settings.sync_lag, true,
               "The game starts its next frame while the GPU still finishes the last one: a few more fps. What the game reads "
               "back from the GPU then comes a frame late, and Link's legs can sink into the ground while he stands "
               "still. Turn it off if that bothers you. Applies at once.");
    opt_toggle("Native game code (experimental)", &g_settings.native_code, false,
               "Runs the game's own code directly on the phone's CPU instead of the game code built at setup: no build "
               "needed. An experiment - report what goes wrong. Applies the next time the game starts.");
    opt_toggle("32-bit shader math", &g_settings.shader_int32, false,
               "Translates the game's shaders with 32-bit address math, the path used for drivers without 64-bit "
               "integers (Adreno 6xx / 7xx and Mali stock drivers). Try it if lighting, grass or text are missing or wrong "
               "on your GPU. Applies to shaders translated from now on: restart the app for all of them.");
#endif
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Reverb", &g_settings.reverb, true,
               "The game's reverb effect (echo in caves and big rooms).");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's movies (cutscenes played as video). Applies the next time a movie starts.");
    opt_end();
}

// Quit: back to the start screen (in game: the game reboots to it) or out to the desktop. Each asks first (progress
// since the last save is lost); B / Esc or Cancel backs out.
extern "C" void rt_request_reboot(int kind);  // runtime/main.c (RT_REBOOT_TITLE = 2: the start screen)
static void tab_quit(float scale, bool in_game) {
    static int ask;  // 0: the two choices; 1: start screen?; 2: desktop?
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask = 0;
    const char *lost = in_game ? tr("\n\nProgress since your last save is lost.") : "";
    char d1[320], d2[320];
    snprintf(d1, sizeof d1, tr("Leaves the game and goes back to the BOTWRecompAndroid start screen.%s"), lost);
    snprintf(d2, sizeof d2, tr("Closes BOTWRecompAndroid.%s"), lost);
#ifdef __ANDROID__
    const char *quit_app = "Close app", *quit_app_yes = "Close app";
    const char *quit_start = "Return to start", *quit_start_yes = "Return to start";
#else
    const char *quit_app = "Quit to Desktop", *quit_app_yes = "Yes, quit to Desktop";
    const char *quit_start = "Quit to Start Screen", *quit_start_yes = "Yes, quit to Start Screen";
#endif
    opt_begin("##quit", scale);
    opt_section(ask ? "Confirm exit" : "Quit");
    if (!ask) {
        if (!in_game) opt_disable_next();  // (already on it)
        if (opt_action(quit_start, "", in_game ? d1 : "You're on the start screen.")) ask = 1;
        if (opt_action(quit_app, "", d2)) ask = 2;
    } else {
        if (opt_action(ask == 1 ? quit_start_yes : quit_app_yes, "", ask == 1 ? d1 : d2)) {
            settings_save();
            if (ask == 1) {
                rt_request_reboot(2);
                set_open(false);
            } else {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
            ask = 0;
        }
        if (opt_action("Cancel", "", "Stays where you are.")) ask = 0;
    }
    opt_end();
}

static void tab_about(void) {
    header("System language");
    {
        // what the game is told the console's language is: its text follows this (runtime/settings.c)
        char autol[96];
        snprintf(autol, sizeof autol, tr("Auto (%s)"), tr(settings_language_name()));
        const char *names[N_LANGUAGES];
        names[0] = autol;
        for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_names[i];
        if (choice("Language", &g_settings.language, names, N_LANGUAGES)) settings_save();
    }
    hint("The game's text language, as its own system setting, and the app's menus' too (Auto: the phone's). The game's "
         "text changes when the game restarts (Quit -> Title screen, then Play); the menus when the app starts again.");
    ImGui::Spacing();
    header("Controls");
    if (toggle("Motion controls", &g_settings.motion)) settings_save();  // (runtime/hle/motion.c)
    hint("The phone's gyroscope - or a controller's that has one - aims the bow and the Sheikah Slate's camera and tilts "
         "the shrines' motion puzzles, as the Switch's motion controls do. The game's own options can turn motion "
         "aiming off.");
    {
        static const char *const sources[] = {"Auto", "Phone", "Controller"};
        if (choice("Motion sensor", &g_settings.motion_source, sources, 3)) settings_save();  // (runtime/hle/motion.c)
    }
    hint("Auto: a controller's gyroscope once it sends live readings, the phone's otherwise. Phone: always the phone's - "
         "for a controller that says it has a gyroscope but doesn't. Controller: only the controller's.");
    ImGui::Spacing();
    controls_bindings_draw(ImGui::GetFontSize() / 34);
    ImGui::Spacing();
    header("Menu");
#ifdef __ANDROID__
    ImGui::TextUnformatted(tr("Open / close this menu:  the menu button on the screen, the back button, or the controller's "
                              "Home / Xbox / PlayStation button, or Back + Start"));
    ImGui::TextUnformatted(tr("Switch tabs:  L / R        Back:  B"));
#else
    ImGui::TextUnformatted("Open / close this menu:  F1, Esc, the controller's Home / Xbox / PlayStation button, or Back + Start");
    ImGui::TextUnformatted("Switch tabs:  L / R  (Q / E)        Back:  B  (Esc)");
    ImGui::TextUnformatted("Fullscreen:  F11 / Alt+Enter");
#endif
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static int name_filter(ImGuiInputTextCallbackData *d) { return d->EventChar < 0x20 ? 1 : 0; }

#ifdef __ANDROID__
// Touch (no controller): typed with Android's keyboard, which covers the lower half - so at the top, in the menu's
// larger font, with OK / Cancel buttons (there's no Enter / Esc to press; the keyboard's own Enter still confirms)
static void keyboard_draw_touch(ImGuiIO &io, float scale, bool appearing) {
    float w = fminf(io.DisplaySize.x * 0.8f, 1700 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.04f), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * scale, 28 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(24 * scale, 14 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(24 * scale, 20 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12 * scale);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImFont *big = g_menu_font_big ? g_menu_font_big : g_font_big, *small = g_menu_font ? g_menu_font : g_font;
    ImGui::PushFont(big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr("Enter your name"));
    if (appearing) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                     ImGuiInputTextFlags_CallbackCharFilter, name_filter);
    ImGui::PopFont();
    if ((int)strlen(g_kbd_text) > g_kbd_max) g_kbd_text[g_kbd_max] = 0;
    float bw = 340 * scale, bh = 120 * scale, sp = ImGui::GetStyle().ItemSpacing.x;
    // one row: the count at the left, the buttons at the right (placed, not SameLine: the same top for both)
    float y = ImGui::GetCursorPosY(), x0 = ImGui::GetCursorPosX(), xr = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    ImGui::PushFont(big);
    ImGui::SetCursorPos(ImVec2(xr - 2 * bw - sp, y));
    if (ImGui::Button(tr("Cancel"), ImVec2(bw, bh))) g_kbd = 3;
    ImGui::SetCursorPos(ImVec2(xr - bw, y));
    if (ImGui::Button(tr("OK"), ImVec2(bw, bh))) g_kbd = 2;
    ImGui::PopFont();
    ImGui::PushFont(small);
    ImGui::SetCursorPos(ImVec2(x0, y + (bh - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextDisabled("%d / %d", (int)strlen(g_kbd_text), g_kbd_max);
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(5);
}
#endif

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
#ifdef __ANDROID__
    if (!pad) {
        keyboard_draw_touch(io, scale, appearing);
        if (g_kbd.load() != 1) was_open = false;
        return;
    }
#endif
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr("Enter your name"));
    ImGui::PopFont();
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter, name_filter);
        ImGui::PopFont();
        if ((int)strlen(g_kbd_text) > g_kbd_max) g_kbd_text[g_kbd_max] = 0;
        ImGui::TextDisabled("Enter  confirm      Esc  cancel      (%d / %d)", (int)strlen(g_kbd_text), g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) LARECOMP_KBD_AUTO=<text>: answer at once
    if (getenv("LARECOMP_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("LARECOMP_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}

// The tab bar and its pages, into the current window: the in-game menu, and the start screen's Settings (in_game
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: the start screen's own
// first tab, game files and fonts, drawn by launcher.mm).
#ifdef __ANDROID__
extern "C" void mods_tab_draw(float scale);  // ui/mods_tab.mm
#endif
#ifdef __ANDROID__
// The Sheikah menu's tab icons, drawn (r: half their size): files (a folder), video (a screen), audio (a speaker),
// controls (two Joy-Con), mods (a box), quit (a door)
static void settings_icon(ImDrawList *dl, int id, ImVec2 c, float r, ImU32 col, float scale) {
    float t = fmaxf(1.5f, 2.2f * scale);
    switch (id) {
    case -1:
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.55f), ImVec2(c.x + r, c.y + r * 0.75f), col, r * 0.12f, 0, t);
        dl->AddLine(ImVec2(c.x - r, c.y - r * 0.55f), ImVec2(c.x - r * 0.75f, c.y - r * 0.85f), col, t);
        dl->AddLine(ImVec2(c.x - r * 0.75f, c.y - r * 0.85f), ImVec2(c.x - r * 0.1f, c.y - r * 0.85f), col, t);
        dl->AddLine(ImVec2(c.x - r * 0.1f, c.y - r * 0.85f), ImVec2(c.x + r * 0.1f, c.y - r * 0.55f), col, t);
        break;
    case 0:
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.75f), ImVec2(c.x + r, c.y + r * 0.45f), col, r * 0.1f, 0, t);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.45f), ImVec2(c.x, c.y + r * 0.75f), col, t);
        dl->AddLine(ImVec2(c.x - r * 0.45f, c.y + r * 0.8f), ImVec2(c.x + r * 0.45f, c.y + r * 0.8f), col, t);
        break;
    case 1: {
        const ImVec2 sp[] = {{c.x - r, c.y - r * 0.3f}, {c.x - r * 0.55f, c.y - r * 0.3f}, {c.x - r * 0.05f, c.y - r * 0.75f},
                             {c.x - r * 0.05f, c.y + r * 0.75f}, {c.x - r * 0.55f, c.y + r * 0.3f}, {c.x - r, c.y + r * 0.3f}};
        dl->AddConvexPolyFilled(sp, 6, col);
        dl->PathArcTo(ImVec2(c.x - r * 0.05f, c.y), r * 0.5f, -0.9f, 0.9f, 10);
        dl->PathStroke(col, 0, t);
        dl->PathArcTo(ImVec2(c.x - r * 0.05f, c.y), r * 0.9f, -0.9f, 0.9f, 12);
        dl->PathStroke(col, 0, t);
        break;
    }
    case 8:
        for (int side = -1; side <= 1; side += 2) {
            float x0 = side < 0 ? c.x - r : c.x + r * 0.12f, x1 = side < 0 ? c.x - r * 0.12f : c.x + r;
            dl->AddRect(ImVec2(x0, c.y - r * 0.8f), ImVec2(x1, c.y + r * 0.8f), col, r * 0.4f, 0, t);
            dl->AddCircleFilled(ImVec2((x0 + x1) * 0.5f, c.y + side * r * 0.3f), r * 0.17f, col, 10);
        }
        break;
    case 10: {  // (a cube: its three visible faces' edges)
        ImVec2 top(c.x, c.y - r), l(c.x - r * 0.87f, c.y - r * 0.5f), rr(c.x + r * 0.87f, c.y - r * 0.5f), mid(c.x, c.y);
        ImVec2 lb(c.x - r * 0.87f, c.y + r * 0.5f), rb(c.x + r * 0.87f, c.y + r * 0.5f), bot(c.x, c.y + r);
        const ImVec2 hex[] = {top, rr, rb, bot, lb, l};
        dl->AddPolyline(hex, 6, col, ImDrawFlags_Closed, t);
        dl->AddLine(l, mid, col, t), dl->AddLine(rr, mid, col, t), dl->AddLine(mid, bot, col, t);
        break;
    }
    case 9:
        dl->AddRect(ImVec2(c.x - r * 0.9f, c.y - r), ImVec2(c.x + r * 0.2f, c.y + r), col, 0, 0, t);
        dl->AddLine(ImVec2(c.x - r * 0.2f, c.y), ImVec2(c.x + r, c.y), col, t);
        dl->AddLine(ImVec2(c.x + r, c.y), ImVec2(c.x + r * 0.6f, c.y - r * 0.4f), col, t);
        dl->AddLine(ImVec2(c.x + r, c.y), ImVec2(c.x + r * 0.6f, c.y + r * 0.4f), col, t);
        break;
    }
}

// A Sheikah button: dark, a gold frame with cut corners (cyan while pressed or under the cursor), an icon on the left
// (0: back, a chevron; 1: restore, a circling arrow) and the label
static bool sheikah_button(const char *label, ImVec2 size, int icon, float scale) {
    ImGui::PushID(label);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool clicked = ImGui::InvisibleButton("##b", size, ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hot = ImGui::IsItemActive() || (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 b(a.x + size.x, a.y + size.y);
    ui_sheikah_frame_filled(dl, a, b, hot ? IM_COL32(22, 80, 98, 230) : IM_COL32(10, 22, 26, 225), 12 * scale);
    ui_sheikah_frame(dl, a, b, hot ? IM_COL32(65, 222, 246, 240) : IM_COL32(186, 157, 97, 220), 12 * scale, 2.2f * scale);
    float fs = ImGui::GetFontSize(), cy = a.y + size.y * 0.5f, ix = a.x + 34 * scale, r = fs * 0.32f, t = 2.6f * scale;
    ImU32 col = IM_COL32(240, 235, 218, 255);
    if (icon == 0) {
        dl->AddLine(ImVec2(ix + r * 0.4f, cy - r), ImVec2(ix - r * 0.4f, cy), col, t);
        dl->AddLine(ImVec2(ix - r * 0.4f, cy), ImVec2(ix + r * 0.4f, cy + r), col, t);
    } else {
        dl->PathArcTo(ImVec2(ix, cy), r, 0.6f, 5.6f, 16);
        dl->PathStroke(col, 0, t);
        ImVec2 e(ix + r * cosf(0.6f), cy + r * sinf(0.6f));
        dl->AddTriangleFilled(ImVec2(e.x + r * 0.45f, e.y - r * 0.05f), ImVec2(e.x - r * 0.35f, e.y - r * 0.35f),
                              ImVec2(e.x - r * 0.05f, e.y + r * 0.45f), col);
    }
    const std::string shown = ui_upper(tr(label));  // (the label in capitals, in the menu's language)
    float tx = ix + r + 22 * scale, tw = ImGui::CalcTextSize(shown.c_str()).x;
    tx = fmaxf(tx, a.x + (size.x - tw) * 0.5f + r);
    dl->AddText(ImVec2(tx, cy - fs * 0.5f), col, shown.c_str());
    ImGui::PopID();
    return clicked;
}
#endif

static void settings_page(int id, float scale, bool in_game, void (*prelaunch)(float)) {
    switch (id) {
    case -1: prelaunch(scale); break;
    case 0: tab_display(); break;
    case 1: tab_audio(); break;
    case 8: tab_about(); break;
    case 9: tab_quit(scale, in_game); break;
#ifdef __ANDROID__
    case 10: mods_tab_draw(scale); break;  // (ui/mods_tab.mm)
#endif
    }
}

#ifdef __ANDROID__
static bool g_touch_settings_back;
extern "C" bool menu_settings_back_requested() { bool back = g_touch_settings_back; g_touch_settings_back = false; return back; }
#endif

static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[10];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Prelaunch", -1, true};
    // ids (test hooks: LARECOMP_MENU_TAB) as in LAS Recomp, whose game-specific tabs (2-7) are gone
    static const char *names[] = {"Video", "Audio", "", "", "", "", "", "", "Controls", "Quit", "Mods"};
#ifdef __ANDROID__
    static const int order[] = {0, 1, 8, 10, 9};  // Video, Audio, Controls, Mods (runtime/mods.c), Quit
#else
    static const int order[] = {8, 0, 1, 9};  // Controls, Video, Audio, Quit
#endif
    for (int i : order)  // (Quit: in game only - the start screen has its own QUIT)
        if (i != 9 || !prelaunch) tabs[ntabs++] = {names[i], i, true};
#ifdef __ANDROID__
    // The Sheikah look (design/sheikah-settings-v1): the window's gold frame with cut corners, the eye over the title,
    // the pages as tabs along the top (an icon and the name, the open one underlined in cyan), the page (option rows
    // with their description on the right: option_rows.mm), and BACK / RESTORE DEFAULTS under it. The page ids stay
    // (LARECOMP_MENU_TAB, the controller's L / R).
    if (g_tab_request_ext != -2) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -2; }
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab_request && tabs[i].on) g_tab = tabs[i].id;
    bool available = false;
    for (int i = 0; i < ntabs; i++) available |= tabs[i].id == g_tab && tabs[i].on;
    if (!available) g_tab = prelaunch ? -1 : 0;
    bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (!popup && !ui_text_busy()) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? -1 : 0;
        if (step) {
            int index = 0;
            for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) index = i;
            do index = (index + step + ntabs) % ntabs; while (!tabs[index].on);
            g_tab = tabs[index].id;
        }
    }
    const ImU32 gold = IM_COL32(186, 157, 97, 210), gold_dim = IM_COL32(186, 157, 97, 110), cyan = IM_COL32(65, 222, 246, 240);
    const ImU32 ivory = IM_COL32(240, 235, 218, 255);
    ImDrawList *wdl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ui_sheikah_frame(wdl, ImVec2(wp.x + 3 * scale, wp.y + 3 * scale), ImVec2(wp.x + ws.x - 3 * scale, wp.y + ws.y - 3 * scale), gold,
                     22 * scale, 2.5f * scale);
    ui_sheikah_frame(wdl, ImVec2(wp.x + 11 * scale, wp.y + 11 * scale), ImVec2(wp.x + ws.x - 11 * scale, wp.y + ws.y - 11 * scale),
                     IM_COL32(186, 157, 97, 60), 17 * scale, 1);
    {   // the eye, the title between two lines with diamonds
        float cx = wp.x + ws.x * 0.5f, ey = wp.y + 26 * scale;
        ui_sheikah_eye(wdl, ImVec2(cx, ey), 15 * scale, cyan, 2 * scale);
        const std::string title_s = ui_upper(tr("Settings"));
        const char *title = title_s.c_str();
        auto next_char = [](const char *c) {  // (a whole UTF-8 character: Ç, Õ)
            const char *n = c + 1;
            while (((unsigned char)*n & 0xc0) == 0x80) ++n;
            return n;
        };
        ImFont *tf = g_font_big;
        float tsz = 46 * scale, spacing = 4 * scale, tw = 0;
        for (const char *c = title; *c; c = next_char(c)) tw += tf->CalcTextSizeA(tsz, FLT_MAX, 0, c, next_char(c)).x + spacing;
        tw -= spacing;
        float ty = ey + 24 * scale, x = cx - tw * 0.5f;
        for (const char *c = title; *c; c = next_char(c)) {
            wdl->AddText(tf, tsz, ImVec2(x, ty), ivory, c, next_char(c));
            x += tf->CalcTextSizeA(tsz, FLT_MAX, 0, c, next_char(c)).x + spacing;
        }
        float ly = ty + tsz * 0.55f, l0 = tw * 0.5f + 26 * scale, l1 = l0 + 150 * scale;
        for (int side = -1; side <= 1; side += 2) {
            wdl->AddLine(ImVec2(cx + side * l0, ly), ImVec2(cx + side * l1, ly), gold_dim, 1.5f * scale);
            ui_sheikah_diamond(wdl, ImVec2(cx + side * l0, ly), 5 * scale, gold, false);
        }
        ImGui::Dummy(ImVec2(0, ty + tsz + 8 * scale - ImGui::GetCursorScreenPos().y));
    }
    {   // the tabs: centred when they fit, else scrolled sideways
        float th = 64 * scale, fs = ImGui::GetFontSize(), tab_w[16], total = 0;
        char ups[16][48];
        for (int i = 0; i < ntabs && i < 16; i++) {
            const char *nm = tabs[i].id == -1 ? "Files" : tabs[i].name;
            snprintf(ups[i], sizeof ups[i], "%s", ui_upper(tr(nm)).c_str());
            tab_w[i] = fmaxf(190 * scale, ImGui::CalcTextSize(ups[i]).x + fs * 1.6f + 56 * scale);
            total += tab_w[i];
        }
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##tabs", ImVec2(0, th + 14 * scale), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NavFlattened);
        ImGui::PopStyleColor();
        ui_touch_scroll();
        if (total < avail) ImGui::SetCursorPosX((avail - total) * 0.5f);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < ntabs && i < 16; i++) {
            const Tab &t = tabs[i];
            if (i) ImGui::SameLine(0, 0);
            ImGui::PushID(t.id);
            ImVec2 a = ImGui::GetCursorScreenPos();
            if (g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab) || (g_focus_tab == -1 && i == 0)) {
                ImGui::SetKeyboardFocusHere();
                g_focus_tab = -99;
            }
            ImGui::BeginDisabled(!t.on);
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            if (ImGui::InvisibleButton("##tab", ImVec2(tab_w[i], th), ImGuiButtonFlags_EnableNav)) g_tab = t.id;
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::EndDisabled();
            bool open = g_tab == t.id;
            ImU32 col = !t.on ? IM_COL32(120, 120, 112, 255) : open ? cyan : hot ? ivory : IM_COL32(214, 210, 196, 210);
            float isz = fs * 0.62f, cy = a.y + th * 0.5f;
            float content = isz * 2 + 18 * scale + ImGui::CalcTextSize(ups[i]).x, x0 = a.x + (tab_w[i] - content) * 0.5f;
            settings_icon(dl, t.id, ImVec2(x0 + isz, cy), isz, col, scale);
            dl->AddText(ImVec2(x0 + isz * 2 + 18 * scale, cy - fs * 0.5f), col, ups[i]);
            if (open)
                dl->AddLine(ImVec2(a.x + 14 * scale, a.y + th - 2 * scale), ImVec2(a.x + tab_w[i] - 14 * scale, a.y + th - 2 * scale),
                            cyan, 3.5f * scale);
            else if (hot)
                dl->AddLine(ImVec2(a.x + 14 * scale, a.y + th - 2 * scale), ImVec2(a.x + tab_w[i] - 14 * scale, a.y + th - 2 * scale),
                            gold, 2 * scale);
            if (i) dl->AddLine(ImVec2(a.x, a.y + th * 0.22f), ImVec2(a.x, a.y + th * 0.78f), gold_dim, 1.2f * scale);
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImVec2 q = ImGui::GetCursorScreenPos();  // (a line under the tabs, a diamond in the middle)
        float lw = ImGui::GetContentRegionAvail().x;
        wdl->AddLine(ImVec2(q.x, q.y), ImVec2(q.x + lw, q.y), gold_dim, 1.2f * scale);
        ui_sheikah_diamond(wdl, ImVec2(q.x + lw * 0.5f, q.y), 5 * scale, gold, true);
        ImGui::Dummy(ImVec2(lw, 10 * scale));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18 * scale, 18 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(14 * scale, 14 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 22 * scale);
    float bottom = 84 * scale, height = fmaxf(1, ImGui::GetContentRegionAvail().y - bottom);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##touchpage", ImVec2(0, height), ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    ui_touch_scroll();
    ImGui::PushID(g_tab);
    settings_page(g_tab, scale, in_game, prelaunch);
    ImGui::PopID();
    if (const char *t = getenv("LARECOMP_PAGE_SCROLL")) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(t));  // (test)
    ImGui::EndChild();
    {   // BACK on the left; RESTORE DEFAULTS on the right on Video and Audio (asks first)
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8 * scale);
        if (sheikah_button("Back", ImVec2(250 * scale, 62 * scale), 0, scale)) {
            if (prelaunch) g_touch_settings_back = true;
            else set_open(false);
        }
        if (g_tab == 0 || g_tab == 1) {
            float rw = 380 * scale;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - rw);
            if (sheikah_button("Restore defaults", ImVec2(rw, 62 * scale), 1, scale)) ImGui::OpenPopup("##restore");
        }
        ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.035f, 0.065f, 0.075f, 0.97f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(34 * scale, 26 * scale));
        if (ImGui::BeginPopupModal("##restore", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImVec2 pp = ImGui::GetWindowPos(), ps = ImGui::GetWindowSize();
            ui_sheikah_frame(ImGui::GetWindowDrawList(), ImVec2(pp.x + 2, pp.y + 2), ImVec2(pp.x + ps.x - 2, pp.y + ps.y - 2), gold,
                             12 * scale, 2 * scale);
            ImGui::TextUnformatted(tr("Restore the Video and Audio settings to their defaults?"));
            ImGui::TextDisabled("%s", tr("Saves, game files, mods, drivers and the controls stay as they are."));
            ImGui::Dummy(ImVec2(0, 10 * scale));
            if (sheikah_button("Restore", ImVec2(260 * scale, 58 * scale), 1, scale)) {
                settings_reset_defaults();
                settings_save();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (sheikah_button("Cancel", ImVec2(260 * scale, 58 * scale), 0, scale) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleVar(3);
    g_tab_request = -2;
    opt_focus_first(false);
    return;
#endif
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext != -2) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -2; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor on the tab bar goes along (left behind, A on it switched straight back; user 2026-10-11)
            if (focused_tab != -99) g_focus_tab = tabs[i].id, g_focus_frames = 0;
        }
    }
    // Down with the cursor on the tab bar: the page's first row (option rows; other pages keep ImGui's own pick)
    bool down_from_tab = focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible && !any_popup &&
                         (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
                          ImGui::IsKeyPressed(ImGuiKey_DownArrow));
    opt_focus_first(down_from_tab);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[64];
            snprintf(lbl, sizeof lbl, prelaunch ? " %s " : "  %s  ", t.name);
            ImGui::BeginDisabled(!t.on);
            // focus (controller / keyboard): the tab lights up a little with a gold bar along its bottom, instead of
            // ImGui's outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            if (lit) {
                ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.19f, 0.27f, 0.22f, 1));
                ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.19f, 0.27f, 0.22f, 1));
            }
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 8;  // (no Prelaunch in game: Controls, the first tab)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            ImGui::PopStyleColor(lit ? 3 : 1);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (lit) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 4 * scale > 2 ? 4 * scale : 2;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, b.y - h), ImVec2(b.x - 2, b.y),
                                                          IM_COL32(236, 194, 84, 255));  // (the menu's gold)
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
            if (open) {
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                // Down from the tab bar, pages that aren't option rows (Prelaunch, Controls): their first control
                // (option-row pages: opt_focus_first)
                if (down_from_tab && (t.id == -1 || t.id == 8)) {
                    opt_focus_first(false);
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                settings_page(t.id, scale, in_game, prelaunch);
                {   // a finger dragged up / down scrolls the page (ImGui scrolls with a wheel or the scrollbar only); a
                    // vertical drag that started on a button or a row lets go of it (sliders are dragged sideways)
                    ImGuiIO &tio = ImGui::GetIO();
                    static bool drag_scroll;
                    if (!tio.MouseDown[0]) drag_scroll = false;
                    else if (tio.MouseSource == ImGuiMouseSource_TouchScreen && !drag_scroll &&
                             ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                             ImGui::IsMouseDragging(0, 12 * scale)) {
                        ImVec2 d = ImGui::GetMouseDragDelta(0);
                        if (fabsf(d.y) > 2 * fabsf(d.x)) { drag_scroll = true; ImGui::ClearActiveID(); }
                    }
                    if (drag_scroll) ImGui::SetScrollY(ImGui::GetScrollY() - tio.MouseDelta.y);
                }
                static const char *scroll_test = getenv("LARECOMP_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first(false);  // (a page without option rows: nothing took it)
        ImGui::EndTabBar();
    }
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (the start screen has its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, in_game, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load();
    bool kbd = g_kbd.load() == 1;
#ifdef __ANDROID__
    bool touch = touch_controls_visible();
#else
    bool touch = false;
#endif
    if (!open && !kbd && !g_settings.show_fps && !touch) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    double now = SDL_GetTicks() / 1000.0;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    float scale = (float)(th / 1080.0);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        style_scale = scale;
    }
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

#ifdef __ANDROID__
    if (g_settings.show_fps) {  // (the full performance overlay: ui/perf_overlay.mm)
        extern void perf_overlay_draw(ImVec2 display, float scale);
        perf_overlay_draw(io.DisplaySize, scale);
    }
#else
    if (g_settings.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * scale, 16 * scale), 0, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f fps", io.Framerate);
        ImGui::End();
    }
#endif

#ifdef __ANDROID__
    {   // (on top of the game, under the menu: hidden while it is open)
        touch_controls_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    }
#endif

    if (open) {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
        float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;
#ifdef __ANDROID__
        w = io.DisplaySize.x * 0.94f;
#endif  // (all the height: a slim margin)
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
#ifdef __ANDROID__
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.065f, 0.075f, 0.9f));  // (the Sheikah look: tabs_body)
#endif
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
#ifdef __ANDROID__
        ImGui::PopStyleColor();
#endif
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header as on the start screen's Settings: the title, and a close X in the corner (B / Esc too). ("Title
        // screen" - rt_request_reboot(RT_REBOOT_TITLE) - is to find a new home; user 2026-10-11.) Android: the
        // Sheikah frame's own title and BACK (tabs_body)
#ifndef __ANDROID__
        ImGui::PushFont(g_font_big);
        ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr("Settings"));
        ImGui::PopFont();
        {
            float bx = 44 * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }
#endif

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    ImGui::Render();
#ifdef __ANDROID__
    g_text_busy = (open || kbd) && ui_text_busy();  // (the name entry too: Android's keyboard for its field)
#else
    g_text_busy = open && ui_text_busy();
#endif
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);  // inside the window image's rendering
#endif
}
