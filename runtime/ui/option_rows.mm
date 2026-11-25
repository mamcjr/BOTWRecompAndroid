// Two-pane settings pages (see option_rows.h) in the Sheikah look (design/sheikah-settings-v1): rows on the left - the
// label, the value on the right - and the selected row's description on the right, under its name.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner, NavCursorVisible)

#include "i18n.h"
#include "option_rows.h"
#include "osk.h"

static float g_scale = 1;
static std::string g_desc_hover, g_desc_focus, g_desc_sel, g_desc_shown;  // this frame's hovered / focused / selected
static std::string g_name_hover, g_name_focus, g_name_sel, g_name_shown;  // row's description and name, the last shown
static std::string g_desc_first, g_name_first;  // the page's first row (shown until a row is selected or hovered)
static ImGuiID g_sel;          // the row a finger last picked (its description shows; its arrows step it)
static ImGuiID g_page;         // (opt_begin's id: a new page starts without a selection)
static float g_drag = -1;      // a slider row being dragged along its value: where, 0..1 (-1: none)
static ImGuiID g_drag_id;      // (that row)
static bool g_row_dragged;     // (the last row drawn is being dragged this frame: opt_slider / opt_int)

static bool g_next_disabled;
void opt_disable_next(void) { g_next_disabled = true; }
static int g_indent;
static bool g_focus_req, g_take_first;  // (opt_focus_first: requested / this page's first row takes it)
void opt_focus_first(bool on) { g_focus_req = on; }
static bool g_held;  // the last row's left / right has been held a while (steppers go faster)
void opt_indent(int step) { g_indent = g_indent + step < 0 ? 0 : g_indent + step; }

// ------------------------------------------------------------------ the Sheikah look's pieces (menu.mm uses them too)
void ui_sheikah_frame(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float cut, float thickness) {
    const ImVec2 pts[] = {{a.x + cut, a.y}, {b.x - cut, a.y}, {b.x, a.y + cut}, {b.x, b.y - cut},
                          {b.x - cut, b.y}, {a.x + cut, b.y}, {a.x, b.y - cut}, {a.x, a.y + cut}};
    dl->AddPolyline(pts, 8, col, ImDrawFlags_Closed, thickness);
}

void ui_sheikah_frame_filled(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float cut) {
    const ImVec2 pts[] = {{a.x + cut, a.y}, {b.x - cut, a.y}, {b.x, a.y + cut}, {b.x, b.y - cut},
                          {b.x - cut, b.y}, {a.x + cut, b.y}, {a.x, b.y - cut}, {a.x, a.y + cut}};
    dl->AddConvexPolyFilled(pts, 8, col);
}

void ui_sheikah_diamond(ImDrawList *dl, ImVec2 c, float r, ImU32 col, bool filled) {
    const ImVec2 pts[] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
    if (filled) dl->AddConvexPolyFilled(pts, 4, col);
    else dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, fmaxf(1, r * 0.3f));
}

// The Sheikah eye: the lid, the pupil, three lashes above and the tear below
void ui_sheikah_eye(ImDrawList *dl, ImVec2 p, float size, ImU32 col, float thickness) {
    dl->PathLineTo(ImVec2(p.x - size, p.y));
    dl->PathBezierCubicCurveTo(ImVec2(p.x - size * 0.4f, p.y - size * 0.65f), ImVec2(p.x + size * 0.4f, p.y - size * 0.65f),
                               ImVec2(p.x + size, p.y));
    dl->PathBezierCubicCurveTo(ImVec2(p.x + size * 0.4f, p.y + size * 0.65f), ImVec2(p.x - size * 0.4f, p.y + size * 0.65f),
                               ImVec2(p.x - size, p.y));
    dl->PathStroke(col, ImDrawFlags_Closed, thickness);
    dl->AddCircle(p, size * 0.25f, col, 20, thickness);
    dl->AddCircleFilled(p, size * 0.1f, col, 12);
    for (int i = -1; i <= 1; ++i) {
        float x = p.x + i * size * 0.55f;
        dl->AddTriangleFilled(ImVec2(x - size * 0.12f, p.y - size * 0.6f), ImVec2(x + size * 0.12f, p.y - size * 0.6f),
                              ImVec2(x, p.y - size * 0.95f), col);
    }
    dl->AddTriangleFilled(ImVec2(p.x - size * 0.14f, p.y + size * 0.4f), ImVec2(p.x + size * 0.14f, p.y + size * 0.4f),
                          ImVec2(p.x, p.y + size * 1.25f), col);
}

// ◀ / ▶ (the fonts have no arrow glyphs)
static void arrow(ImDrawList *dl, float x, float cy, float s, bool right, ImU32 col) {
    if (right) dl->AddTriangleFilled(ImVec2(x + s, cy), ImVec2(x, cy - s * 0.9f), ImVec2(x, cy + s * 0.9f), col);
    else dl->AddTriangleFilled(ImVec2(x, cy), ImVec2(x + s, cy - s * 0.9f), ImVec2(x + s, cy + s * 0.9f), col);
}

// › (the closed rows' chevron)
static void chevron(ImDrawList *dl, float x, float cy, float s, ImU32 col) {
    dl->AddLine(ImVec2(x - s * 0.5f, cy - s), ImVec2(x + s * 0.5f, cy), col, fmaxf(1.5f, s * 0.28f));
    dl->AddLine(ImVec2(x + s * 0.5f, cy), ImVec2(x - s * 0.5f, cy + s), col, fmaxf(1.5f, s * 0.28f));
}

// A finger dragged up / down scrolls the window it started in (pages, the description pane, a choice's open list),
// and a flick keeps it going, slowing down, as phone lists do - not only the scrollbar, whose grab got small on long
// pages. A new touch stops the glide.
void ui_touch_scroll() {
#ifdef __ANDROID__
    ImGuiIO &io = ImGui::GetIO();
    static ImGuiContext *context;
    static ImGuiID owner;
    static ImVec2 origin;
    static float start_scroll, velocity;  // velocity: px a second, upward finger motion positive
    static bool dragging;
    static ImGuiID glide;                 // the window still gliding after the finger left, and its speed
    static float glide_v;
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    float dt = io.DeltaTime > 0 && io.DeltaTime < 0.1f ? io.DeltaTime : 1.0f / 60;
    if (glide == window->ID && !io.MouseDown[0]) {
        float y = window->Scroll.y + glide_v * dt;
        glide_v *= expf(-dt * 3.5f);
        if (y <= 0 || y >= window->ScrollMax.y || fabsf(glide_v) < 30) glide = 0;
        ImGui::SetScrollY(fminf(fmaxf(y, 0), window->ScrollMax.y));
    }
    if (io.MouseSource != ImGuiMouseSource_TouchScreen) return;
    if (ImGui::IsMouseClicked(0)) glide = 0;  // (a new touch stops any glide)
    if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || (window->Flags & ImGuiWindowFlags_Popup))) {
        context = ImGui::GetCurrentContext(); owner = window->ID;
        origin = io.MousePos; start_scroll = window->Scroll.y; dragging = false; velocity = 0;
    }
    if (context != ImGui::GetCurrentContext() || owner != window->ID) return;
    float dx = io.MousePos.x - origin.x, dy = io.MousePos.y - origin.y;
    if (io.MouseDown[0] && fabsf(dy) > ImGui::GetFontSize() * 0.4f && fabsf(dy) > fabsf(dx) * 1.3f && window->ScrollMax.y > 0 &&
        !g_drag_id)
        dragging = true;
    if (dragging && io.MouseDown[0]) {
        ImGui::ClearActiveID();  // a swipe must not activate the row when the finger lifts
        ImGui::SetScrollY(start_scroll - dy);
        velocity = velocity * 0.6f + (-io.MouseDelta.y / dt) * 0.4f;  // (smoothed over the last frames)
    }
    if (!io.MouseDown[0]) {
        if (dragging && fabsf(velocity) > 200) glide = window->ID, glide_v = velocity;
        owner = 0; dragging = false;
    }
#endif
}

#ifdef __ANDROID__
static void touch_begin(const char *label, const char *desc, bool heading = true) {
    ImGui::PushID(label);
    ImGui::PushTextWrapPos(0);
    if (heading) ImGui::TextUnformatted(tr(label));
    ImGui::PopTextWrapPos();
    if (desc && *desc) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr(desc));
        ImGui::PopTextWrapPos();
    }
    ImGui::BeginDisabled(g_next_disabled);
    g_next_disabled = false;
    if (g_take_first) { ImGui::SetKeyboardFocusHere(); g_take_first = false; }
    ImGui::SetNextItemWidth(-1);
}
static void touch_end() {
    ImGui::EndDisabled();
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::PopID();
}
#endif

static bool g_two_panes;

void opt_begin(const char *id, float scale) {
    g_scale = scale;
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();  // (this frame's Down goes to the first row instead)
    g_indent = 0;
    g_desc_hover.clear(), g_desc_focus.clear(), g_desc_sel.clear();
    g_name_hover.clear(), g_name_focus.clear(), g_name_sel.clear();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushID(id);
    ImGuiID page = ImGui::GetID("##page");
    if (page != g_page) g_page = page, g_sel = 0, g_desc_shown.clear(), g_name_shown.clear();
    // two panes when there is room: the rows, and the description on the right (a narrow window: under the row)
    g_two_panes = avail.x >= 760 * scale;
    float rows_width = g_two_panes ? avail.x * 0.62f : avail.x;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));  // (the frame's own background shows through)
    ImGui::BeginChild("##rows", ImVec2(rows_width, avail.y), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
    ImGui::PopStyleColor();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));  // (rows touch: their lines part them)
    g_desc_first.clear(), g_name_first.clear();
    ui_touch_scroll();
}

void opt_section(const char *title) { ui_sheikah_heading(title, g_scale); }

void ui_sheikah_heading(const char *title, float s) {
    std::string t = ui_upper(tr(title));  // (Portuguese too: the accented letters in capitals)
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1) ImGui::Dummy(ImVec2(0, 14 * s));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize() * 0.78f, w = ImGui::GetContentRegionAvail().x;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    ts.x *= 0.78f;
    dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x + 16 * s, p.y), IM_COL32(200, 172, 106, 230), t.c_str());
    float ly = p.y + fs * 0.55f, lx = p.x + 16 * s + ts.x + 14 * s;
    if (lx < p.x + w - 20 * s) {
        dl->AddLine(ImVec2(lx, ly), ImVec2(p.x + w - 12 * s, ly), IM_COL32(186, 157, 97, 90), 1);
        ui_sheikah_diamond(dl, ImVec2(p.x + w - 12 * s, ly), 3.5f * s, IM_COL32(186, 157, 97, 140), false);
    }
    ImGui::Dummy(ImVec2(w, fs + 8 * s));
}

// One row: draws it, handles A / click / tap and left / right while selected. Returns 2 (A / click), -1 / +1 (left /
// right, or a tap on that side's arrow) or 0. arrows: a value that steps (◀ value ▶ on the selected row). bar: a
// slider's position, 0..1 (-1: none) - dragging along the selected row's value sets it (g_drag).
static int row(const char *label, const std::string &value, bool changed, const char *desc, bool upper = false,
               bool arrows = false, float bar = -1) {
    (void)upper;  // (the Sheikah look writes labels as given; option pages pass them in sentence case)
    ImGui::PushID(label);  // (the English label: the row's id in any language)
    const char *name = tr(label);  // (what is shown: ui/i18n.mm)
    const std::string shown = value.empty() ? value : std::string(tr(value.c_str()));
    if (desc) desc = tr(desc);
    if (g_indent) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + g_indent * 28 * g_scale);  // (an option of the row above)
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = g_scale, fs = ImGui::GetFontSize();
    float w = ImGui::GetContentRegionAvail().x, h = fmaxf(fs * 2.05f, 58 * s);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the row's own frame shows the selection)
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    ImGuiID id = ImGui::GetItemID();
    if (g_take_first) {  // (opt_focus_first)
        g_take_first = false;
        ImGui::SetFocusID(id, ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        ImGui::SetScrollHereY(0);
    }
    bool off = g_next_disabled;  // (greyed out: still selectable, so its description shows, but it can't change)
    g_next_disabled = false;
    ImGuiIO &io = ImGui::GetIO();
    bool touch = io.MouseSource == ImGuiMouseSource_TouchScreen;
    bool hov = ImGui::IsItemHovered() && !touch, foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    bool was_sel = g_sel == id;
    bool sel = was_sel || foc || hov;
    // the value's column: the right part of the row, the value centred in it
    float pad = 18 * s, vx0 = p.x + w * 0.56f, vx1 = p.x + w - pad - fs * 0.5f, cy = p.y + h * 0.5f;
    float aw = fs * 0.36f;
    float lax = vx0 + pad * 0.5f, rax = vx1 - aw - pad * 0.5f;  // (the arrows' x, on the selected row)
    int step = 0;
    if (clicked && !off) {
        float mx = io.MousePos.x;
        bool on_value = mx >= vx0;
        // (a finger on a label just picks the row, and so does the first one on a value that steps: it shows the row's
        // arrows - stepping on, it went right only, as if the chevron were the only way)
        if (!touch || was_sel || (on_value && !arrows) || value.empty()) {
            step = 2;
            if (arrows && (was_sel || !touch)) {
                if (mx <= lax + aw + pad) step = -1;
                else if (mx >= rax - pad) step = 1;
            }
        }
    }
    if (clicked && g_drag_id == id) step = 0;  // (the finger let go of a drag along the value)
    if (clicked) g_sel = id;
    g_held = false;
    if (foc && !off) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) ||
            ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            step = 1;
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft,
                           ImGuiKey_GamepadLStickRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            if (ImGui::GetKeyData(k)->DownDuration > 1.0f) g_held = true;
        // left / right belong to the row (navigation would otherwise try to move sideways)
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            ImGui::SetKeyOwner(k, id);
    }
    // a slider: a finger held on the selected row's value and moved sideways sets it where it is along the bar
    float bx0 = lax + aw + pad, bx1 = rax - pad;
    g_row_dragged = false;
    if (bar >= 0 && !off && ImGui::IsItemActive() && was_sel && io.MouseDown[0] && io.MouseClickedPos[0].x >= vx0 &&
        (g_drag_id == id || fabsf(io.MousePos.x - io.MouseClickedPos[0].x) > fs * 0.3f) && bx1 > bx0) {
        g_drag = fminf(1, fmaxf(0, (io.MousePos.x - bx0) / (bx1 - bx0)));
        g_drag_id = id, g_row_dragged = true;
    } else if (g_drag_id == id && !io.MouseDown[0]) g_drag = -1, g_drag_id = 0;
    if (foc) g_desc_focus = desc ? desc : "", g_name_focus = name;
    if (hov) g_desc_hover = desc ? desc : "", g_name_hover = name;
    if (was_sel || clicked) g_desc_sel = desc ? desc : "", g_name_sel = name;
    if (g_desc_first.empty() && g_name_first.empty()) g_desc_first = desc ? desc : "", g_name_first = name;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 cyan = IM_COL32(65, 222, 246, 235), gold = IM_COL32(186, 157, 97, off ? 60 : 110);
    ImVec2 b(p.x + w, p.y + h - 2 * s);
    if (sel) {
        dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2 * s), b, IM_COL32(22, 107, 131, off ? 30 : 70), 4 * s);
        ui_sheikah_frame(dl, ImVec2(p.x + 2, p.y + 2 * s), b, off ? IM_COL32(65, 222, 246, 90) : cyan, 8 * s, 2 * s);
    } else {
        dl->AddLine(ImVec2(p.x + 10 * s, p.y + h - 1), ImVec2(p.x + w, p.y + h - 1), gold, 1);
        ui_sheikah_diamond(dl, ImVec2(p.x + 6 * s, p.y + h - 1), 3 * s, gold, false);
    }
    float ty = cy - fs * 0.5f;
    ImU32 label_col = off ? IM_COL32(120, 120, 112, 255) : IM_COL32(240, 235, 218, 255);
    // the label, cut short with an ellipsis before the value's column (long names, bigger fonts)
    float room = vx0 - (p.x + pad) - pad * 0.5f;
    const char *end = name;
    while (*end) {
        const char *next = end + 1;
        while (((unsigned char)*next & 0xc0) == 0x80) ++next;
        if (ImGui::CalcTextSize(name, next).x > room) break;
        end = next;
    }
    dl->AddText(ImVec2(p.x + pad, ty), label_col, name, end);
    if (*end) dl->AddText(ImVec2(p.x + pad + ImGui::CalcTextSize(name, end).x, ty), label_col, "...");
    if (!shown.empty()) {
        ImU32 vcol = off ? IM_COL32(120, 120, 112, 255) : changed ? IM_COL32(255, 252, 240, 255) : IM_COL32(214, 210, 196, 255);
        float vw = ImGui::CalcTextSize(shown.c_str()).x;
        float vy = bar >= 0 && sel ? ty - fs * 0.18f : ty;
        dl->AddText(ImVec2((vx0 + vx1 - vw) * 0.5f, vy), vcol, shown.c_str());
        if (sel && arrows && !off) {
            arrow(dl, lax, cy, aw, false, cyan);
            arrow(dl, rax, cy, aw, true, cyan);
        }
        if (bar >= 0 && sel && bx1 > bx0) {  // (a slider: its track under the value, filled to where it is)
            float by = cy + fs * 0.52f, f = g_drag_id == id ? g_drag : bar;
            dl->AddLine(ImVec2(bx0, by), ImVec2(bx1, by), IM_COL32(186, 157, 97, 120), 2 * s);
            dl->AddLine(ImVec2(bx0, by), ImVec2(bx0 + (bx1 - bx0) * f, by), cyan, 3 * s);
            dl->AddCircleFilled(ImVec2(bx0 + (bx1 - bx0) * f, by), 5 * s, cyan, 12);
        }
    }
    if (!sel || !arrows) chevron(dl, p.x + w - pad * 0.9f, cy, fs * 0.22f, off ? IM_COL32(65, 222, 246, 70) : cyan);
    if (!g_two_panes && sel && desc && *desc) {  // (one pane: the description under the selected row)
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.86f, 0.84f, 0.76f, 0.9f), "%s", desc);
        ImGui::PopTextWrapPos();
    }
    ImGui::PopID();
    return step;
}

bool opt_toggle(const char *label, bool *v, bool def, const char *desc) {
    if (!row(label, *v ? "On" : "Off", *v != def, desc)) return false;  // (any step flips it)
    *v = !*v;
    return true;
}

bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    int step = row(label, names[cur], cur != def, desc, true, true);
    if (!step) return false;
    int nv = cur + (step == 2 ? 1 : step);
    nv = wrap ? (nv + n) % n : nv < 0 ? 0 : nv >= n ? n - 1 : nv;
    if (nv == cur) return false;
    *v = nv;
    return true;
}

bool opt_slider(const char *label, float *v, float lo, float hi, float step, float def, const char *fmt, const char *desc) {
    char buf[32];
    snprintf(buf, sizeof buf, fmt, *v);
    float frac = hi > lo ? (*v - lo) / (hi - lo) : 0;
    int s = row(label, buf, fabsf(*v - def) > step * 0.01f, desc, true, true, frac);
    if (g_row_dragged) {  // (dragged along the value: continuous, to the step)
        float n = lo + roundf(g_drag * (hi - lo) / step) * step;
        n = n < lo ? lo : n > hi ? hi : n;
        if (n == *v) return false;
        *v = n;
        return true;
    }
    if (!s) return false;
    float eps = step * 0.01f, n = roundf(*v / step) * step + (s == 2 ? step : s * step);
    if (s == 2 && n > hi + eps) n = lo;  // (A / click past the top wraps to the bottom; left / right stop at the ends)
    *v = n < lo ? lo : n > hi ? hi : n;
    return true;
}

bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc,
             const char *lo_name) {
    char buf[48];
    if (lo_name && *v == lo) snprintf(buf, sizeof buf, "%s", lo_name);
    else snprintf(buf, sizeof buf, fmt, *v);
    float frac = hi > lo ? (float)(*v - lo) / (float)(hi - lo) : 0;
    int s = row(label, buf, *v != def, desc, true, true, frac);
    if (g_row_dragged) {
        int n = lo + (int)roundf(g_drag * (float)(hi - lo) / step) * step;
        n = n < lo ? lo : n > hi ? hi : n;
        if (n == *v) return false;
        *v = n;
        return true;
    }
    if (!s) return false;
    int d = s == 2 ? step : s * step * (g_held && hi - lo > 40 * step ? 10 : 1);
    int n = *v + d;
    if (s == 2 && n > hi) n = lo;  // (A / click past the top wraps; left / right stop at the ends)
    n = n < lo ? lo : n > hi ? hi : n;
    if (n == *v) return false;
    *v = n;
    return true;
}

bool opt_button(const char *label, const char *desc) { return row(label, "", false, desc) == 2; }

bool opt_radio(const char *label, bool selected, const char *desc) {
    bool off = g_next_disabled;
    bool chosen = row(label, "", false, desc) == 2;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float r = ImGui::GetFontSize() * 0.32f, cx = b.x - 40 * g_scale - r, cy = (a.y + b.y) * 0.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = off ? IM_COL32(150, 150, 140, 255) : selected ? IM_COL32(65, 222, 246, 235) : IM_COL32(214, 210, 196, 255);
    dl->AddCircle(ImVec2(cx, cy), r, col, 0, 2.0f);
    if (selected) dl->AddCircleFilled(ImVec2(cx, cy), r * 0.55f, col);
    return chosen && !selected;
}

bool opt_action(const char *label, const char *value, const char *desc, bool upper) {
    return row(label, value ? value : "", false, desc, upper) == 2;
}

static ImGuiID g_text_editing;  // the text row turned into a field (one at a time)
bool opt_text_editing(const char *label) { return g_text_editing && g_text_editing == ImGui::GetID(label); }

bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc) {
#ifdef __ANDROID__
    ImGuiID touch_id = ImGui::GetID(label);
    touch_begin(label, desc);
    bool entered = ui_input_text("##text", buf, cap, ImGuiInputTextFlags_EnterReturnsTrue, tr(hint), label);
    if (ImGui::IsItemActive()) g_text_editing = touch_id;
    else if (g_text_editing == touch_id) g_text_editing = 0;
    touch_end(); return entered;
#endif
    ImGuiID &editing = g_text_editing;
    static int focus;        // frames left to give it the keyboard focus
    ImGuiID id = ImGui::GetID(label);
    if (editing != id) {
        if (row(label, buf[0] ? buf : (hint ? hint : ""), false, desc) == 2) editing = id, focus = 2;
        return false;
    }
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1);
    if (focus > 0) ImGui::SetKeyboardFocusHere(), focus--;
    bool done = ui_input_text("##text", buf, cap, ImGuiInputTextFlags_EnterReturnsTrue, hint, label);
    if (ImGui::IsItemFocused()) g_desc_focus = desc ? desc : "", g_name_focus = label;
    if (done || ui_input_text_cancelled() || (focus == 0 && !ImGui::IsItemActive() && !osk_is_open())) editing = 0;
    ImGui::PopID();
    return done;
}

void opt_end(void) {
    ImGui::PopStyleVar();
    if (const char *t = getenv("LARECOMP_OPT_SCROLL")) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(t));  // (test)
    ImGui::EndChild();
    if (!g_two_panes) { ImGui::PopID(); return; }
    // the description: the hovered row (mouse) wins, else the focused one (controller), else the one a finger picked,
    // else the last shown, else the page's first row
    if (!g_name_hover.empty()) g_desc_shown = g_desc_hover, g_name_shown = g_name_hover;
    else if (!g_name_focus.empty()) g_desc_shown = g_desc_focus, g_name_shown = g_name_focus;
    else if (!g_name_sel.empty()) g_desc_shown = g_desc_sel, g_name_shown = g_name_sel;
    else if (g_name_shown.empty()) g_desc_shown = g_desc_first, g_name_shown = g_name_first;
    float s = g_scale;
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    float gap = 34 * s;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 gold = IM_COL32(186, 157, 97, 150);
    float lx = p.x + gap * 0.5f;  // (the line between the panes, a diamond at each end)
    dl->AddLine(ImVec2(lx, p.y + 8 * s), ImVec2(lx, p.y + avail.y - 8 * s), gold, 1.0f);
    ui_sheikah_diamond(dl, ImVec2(lx, p.y + 6 * s), 4 * s, gold, false);
    ui_sheikah_diamond(dl, ImVec2(lx, p.y + avail.y - 6 * s), 4 * s, gold, false);
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * s, 10 * s));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##desc", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ui_touch_scroll();  // (a long description scrolls with a finger too)
    ImVec2 cp = ImGui::GetWindowPos(), cs = ImGui::GetWindowSize();
    // the watermark first: the Sheikah eye, faint, low in the pane
    ui_sheikah_eye(dl = ImGui::GetWindowDrawList(), ImVec2(cp.x + cs.x * 0.5f, cp.y + cs.y * 0.7f), fminf(cs.x * 0.17f, 70 * s),
                   IM_COL32(45, 150, 180, 45), 2 * s);
    std::string name = ui_upper(g_name_shown.c_str());
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ImVec4(0.90f, 0.85f, 0.70f, 1), "%s", name.c_str());
    ImGui::PopTextWrapPos();
    {
        ImVec2 q = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, y = q.y + 6 * s;
        dl->AddLine(ImVec2(q.x, y), ImVec2(q.x + w, y), gold, 1.0f);
        ui_sheikah_diamond(dl, ImVec2(q.x + w * 0.5f, y), 4 * s, gold, true);
        ImGui::Dummy(ImVec2(w, 18 * s));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.84f, 0.78f, 1));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_desc_shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopID();
}
