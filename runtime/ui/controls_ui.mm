// Control remapping editor (runtime/input_bind.h), shared by the in-game menu's Controls tab (menu.mm) and the start
// screen's Controls page (launcher.mm): a table of Switch inputs with the controller button and the key bound to
// each. Select a binding, then press the new button / key (Esc or 5 s without a press cancels; a button already in
// use swaps over). Drawn in whichever ImGui context is current.
#include <SDL3/SDL.h>
#include <stdio.h>

#include "i18n.h"
#include "imgui.h"

extern "C" {
#include "../input_bind.h"
#include "../settings.h"
}

static void cell(int dev, int slot, int fam, int cap_dev, int cap_slot, int ms_left) {
    char lbl[96];
    bool waiting = cap_dev == dev && cap_slot == slot;
    if (waiting) snprintf(lbl, sizeof lbl, tr("Press %s...  %d##%d_%d"), tr(dev == BIND_DEV_PAD ? "a button" : "a key"),
                          (ms_left + 999) / 1000, dev, slot);
    else snprintf(lbl, sizeof lbl, "%s##%d_%d", dev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[slot], fam) :
                                                 bind_key_label(g_bind_key[slot]), dev, slot);
    // changed from the default: gold text; the same button / key on two inputs (a hand-edited larecomp.ini): red
    const int16_t *t = dev == BIND_DEV_PAD ? g_bind_pad : g_bind_key;
    int n = dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT, pushed = 0;
    bool dup = false;
    for (int i = 0; i < n; i++) dup |= i != slot && t[i] == t[slot] && t[slot] > (dev == BIND_DEV_PAD ? -1 : 0);
    if (waiting) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.12f, 1)); pushed++; }
    else if (dup) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.12f, 1)); pushed++; }
    if (!bind_is_default_slot(dev, slot)) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.82f, 0.38f, 1)); pushed++; }
    if (ImGui::Button(lbl, ImVec2(-FLT_MIN, 0))) {
        if (waiting) bind_capture_cancel();
        else bind_capture_begin(dev, slot);
    }
    ImGui::PopStyleColor(pushed);
    if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);  // (a capture started by a test hook)
}

#ifdef __ANDROID__
extern "C" bool touch_edit_available(void);  // ui/touch_controls.mm
extern "C" void touch_edit_begin_layout(int icons);  // (1 the action layout, 0 the classic one)
extern "C" void touch_layout_reset(void);

// No controller: the on-screen controls' layout instead of bindings (Android has no keyboard to bind, as a rule)
static void touch_layout_draw(void) {
    ImGui::TextUnformatted(tr("No controller connected: the on-screen controls"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Move the buttons and the stick where your thumbs like them. Connect a controller to remap "
                                 "its buttons instead."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    bool can = touch_edit_available();
    ImGui::BeginDisabled(!can);
    if (g_settings.touch_scheme == 1) {  // (two layouts: the icons' in the world, the classic one in the game's menus)
        if (ImGui::Button(tr("Edit the action layout"), ImVec2(-1, 0))) touch_edit_begin_layout(1);
        if (ImGui::Button(tr("Edit the classic layout"), ImVec2(-1, 0))) touch_edit_begin_layout(0);
    } else if (ImGui::Button(tr("Edit touch layout"), ImVec2(-1, 0))) touch_edit_begin_layout(0);
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("While editing, touch a button to select it, then - and + change its own size."));
    ImGui::PopTextWrapPos();
    bool def = true;
    for (int i = 0; i < TOUCH_NCTL; i++) def &= g_settings.touch_pos[i][0] < 0;
    for (int i = 0; i < TOUCH_NCTL2; i++) def &= g_settings.touch_pos2[i][0] < 0;
    ImGui::BeginDisabled(def);
    if (ImGui::Button(tr("Reset layout to defaults"), ImVec2(-1, 0))) touch_layout_reset();
    ImGui::EndDisabled();
    if (!can) ImGui::TextDisabled("%s", tr(g_settings.touch_controls ? "Get into the game to edit the layout."
                                                                     : "Turn on Touch controls (Video tab) to edit the layout."));
    ImGui::Spacing();
    float sz = g_settings.touch_scale * 100, op = g_settings.touch_opacity * 100;
    ImGui::TextUnformatted(tr("Button size"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##Size", &sz, 60, 160, "%.0f%%")) g_settings.touch_scale = sz / 100;
    if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
    ImGui::TextUnformatted(tr("Opacity"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##Opacity", &op, 20, 100, "%.0f%%")) g_settings.touch_opacity = op / 100;
    if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
    // FPS-style play (ui/touch_controls.mm drag_frame): the camera by dragging, the sticks hidden, larger areas
    auto hint = [](const char *s) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr(s));
        ImGui::PopTextWrapPos();
    };
    ImGui::Spacing();
    bool icons = g_settings.touch_scheme == 1;
    if (ImGui::Checkbox(tr("Action icons in the world"), &icons)) g_settings.touch_scheme = icons ? 1 : 0, settings_save();
    hint("While playing in the world, buttons drawn as what they do (attack, jump, the bow...) instead of A, B, ZR. The "
         "game's menus keep the classic buttons. Edit the layout while playing to move them.");
    if (icons) {  // (the bow and throw buttons' drag: ui/touch_controls.mm recompute)
        float as = (g_settings.touch_aim_sens > 0.1f ? g_settings.touch_aim_sens : 1.0f) * 100;
        ImGui::TextUnformatted(tr("Aim sensitivity (bow, throw)"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##AimSens", &as, 25, 200, "%.0f%%")) g_settings.touch_aim_sens = as / 100;
        if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
        if (ImGui::Checkbox(tr("Simple controls in talks"), &g_settings.touch_talk)) settings_save();
        hint("In a conversation only the stick, A and B stay on the screen. Off: all the buttons stay.");
    }
    ImGui::Spacing();
    if (ImGui::Checkbox(tr("Camera by dragging"), &g_settings.touch_drag_camera)) settings_save();
    hint("The camera turns while a finger moves on the right half of the screen and stops when it stops, as in phone "
         "shooters, instead of a stick held to one side.");
    if (g_settings.touch_drag_camera || g_settings.touch_scheme == 1) {  // (the action layout's camera drags too: #199)
        float sens = g_settings.touch_drag_sens * 100;
        ImGui::TextUnformatted(tr("Drag sensitivity"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##DragSens", &sens, 25, 400, "%.0f%%")) g_settings.touch_drag_sens = sens / 100;
        if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
    }
    if (ImGui::Checkbox(tr("Sticks only while touched"), &g_settings.touch_sticks_hidden)) settings_save();
    hint("The sticks are drawn only while a finger holds them, for a cleaner screen.");
    if (ImGui::Checkbox(tr("Larger stick areas"), &g_settings.touch_half_areas)) settings_save();
    hint("Each stick starts anywhere on its half of the screen, not only in the lower part. The buttons stay in front.");
}
#endif

extern "C" void controls_bindings_draw(float scale) {
    (void)scale;
#ifdef __ANDROID__
    if (!SDL_HasGamepad()) { touch_layout_draw(); return; }
    const bool keys = false;  // (bindings: the controller's only)
#else
    const bool keys = true;
#endif
    int fam = bind_label_family();
    static const char *const fam_name[] = {"Nintendo", "Xbox", "PlayStation"};
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    const char *pad_name = ids && n > 0 ? SDL_GetGamepadNameForID(ids[0]) : NULL;
    char line[200];
    if (n > 0) snprintf(line, sizeof line, tr("Controller: %s  (%s button names)"), pad_name ? pad_name : tr("connected"), fam_name[fam % 3]);
    else snprintf(line, sizeof line, tr("No controller connected  (%s button names shown)"), fam_name[fam % 3]);
    SDL_free(ids);
    ImGui::TextUnformatted(line);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr(keys ? "Select a binding, then press the new button or key. Esc (or 5 seconds) cancels. A button "
                               "that's already in use swaps with the old one."
                             : "Select a binding, then press the new button. 5 seconds without a press cancel. A button "
                               "that's already in use swaps with the old one."));
    ImGui::PopTextWrapPos();

    int cd, cs, ms;
    bind_capture_active(&cd, &cs, &ms);
    // a finished capture: say what happened, keep larecomp.ini current
    static int seen_serial = -1;
    static char msg[200];
    int rdev, rslot, rother, serial;
    int kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen_serial < 0) seen_serial = serial;
    if (serial != seen_serial) {
        seen_serial = serial;
        const char *now = rslot < 0 ? "" : rdev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[rslot], fam) : bind_key_label(g_bind_key[rslot]);
        if (kind == 5) snprintf(msg, sizeof msg, tr("%s bindings reset to the defaults."), tr(rdev == BIND_DEV_PAD ? "Controller" : "Keyboard"));
        else if (kind == 1) snprintf(msg, sizeof msg, tr("%s is now %s."), bind_input_name(rslot), now);
        else if (kind == 2)
            snprintf(msg, sizeof msg, tr("%s is now %s; %s moved to %s (swapped)."), bind_input_name(rslot), now,
                     bind_input_name(rother), rdev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[rother], fam) : bind_key_label(g_bind_key[rother]));
        else if (kind == 3) snprintf(msg, sizeof msg, tr("Cancelled: %s unchanged."), bind_input_name(rslot));
        else if (kind == 4) snprintf(msg, sizeof msg, tr("Nothing pressed: %s unchanged."), bind_input_name(rslot));
        if (kind == 1 || kind == 2 || kind == 5) settings_save();
    }
    if (msg[0]) ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", msg);

    {   // the sticks' dead zone (hle/input.c): for worn sticks that drift
        float dz = g_settings.stick_deadzone * 100;
        ImGui::TextUnformatted(tr("Stick dead zone"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##Deadzone", &dz, 0, 40, dz < 0.5f ? tr("Off") : "%.0f%%")) g_settings.stick_deadzone = dz / 100;
        if (ImGui::IsItemDeactivatedAfterEdit()) settings_save();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("A stick held this close to the centre counts as centred, for worn sticks that move Link "
                                     "or the camera on their own (drift). Off: only the game's own."));
        ImGui::PopTextWrapPos();
    }

    if (ImGui::BeginTable("##binds", keys ? 3 : 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Switch", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("Controller"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
        if (keys) ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableHeadersRow();
        // in the order a player thinks of them: face buttons, shoulders, Plus / Minus, d-pad, sticks
        static const int order[] = {BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS,
                                    BIND_MINUS, BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT, BIND_LSTICK, BIND_RSTICK,
                                    BIND_LS_UP, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT, BIND_RS_UP, BIND_RS_DOWN,
                                    BIND_RS_LEFT, BIND_RS_RIGHT};
        for (int slot : order) {
            if (!keys && slot >= BIND_PAD_COUNT) continue;  // (stick directions: keyboard only)
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(bind_input_name(slot));
            ImGui::TableNextColumn();
            if (slot < BIND_PAD_COUNT) cell(BIND_DEV_PAD, slot, fam, cd, cs, ms);
            else ImGui::TextDisabled("%s", tr(slot < BIND_RS_UP ? "Left stick" : "Right stick"));
            if (!keys) continue;
            ImGui::TableNextColumn();
            cell(BIND_DEV_KEY, slot, fam, cd, cs, ms);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(bind_is_default(BIND_DEV_PAD));
    if (ImGui::Button(tr("Reset controller to defaults"))) { bind_capture_cancel(); bind_reset(BIND_DEV_PAD); }
    ImGui::EndDisabled();
    if (keys) {
        ImGui::SameLine();
        ImGui::BeginDisabled(bind_is_default(BIND_DEV_KEY));
        if (ImGui::Button("Reset keyboard to defaults")) { bind_capture_cancel(); bind_reset(BIND_DEV_KEY); }
        ImGui::EndDisabled();
    }
    ImGui::PushTextWrapPos(0);
#ifdef __ANDROID__
    ImGui::TextDisabled("%s", tr("Bindings apply to the game only. Menus keep the fixed layout (right face button select, bottom "
                        "face button back), and the in-game menu always opens with the back button or the menu button "
                        "on the screen, or the controller's Back + Start held together, or its Home / Guide button - so "
                        "this page can't be locked out. The game's button icons follow the controller bindings."));
#else
    ImGui::TextDisabled("Bindings apply to the game only. Menus keep the fixed layout (right face button / Enter "
                        "select, bottom face button / Esc back), and the in-game menu always opens with F1 or Esc, or "
                        "the controller's Back + Start held together, or its Home / Guide button - so this page can't be "
                        "locked out. The game's button icons follow the controller bindings.");
#endif
    ImGui::PopTextWrapPos();
}
