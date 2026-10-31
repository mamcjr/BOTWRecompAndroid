// The menu's Mods page (Android; LASRecompAndroid's): the game mods in mods/ (runtime/mods.c), each on or off, its own
// note (its .txt: the authors, the credits) as the description, and Import a mod folder
// (BOTWRecompAndroidActivity.importMod: the Android folder picker; the folder is copied into this game version's
// mods/). On or off applies when the game starts again.
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "i18n.h"
#include "imgui.h"
#include "option_rows.h"

extern "C" {
#include "../mods.h"
#include "../paths.h"
#include "../settings.h"
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_msg;  // the last import's outcome (set from the activity's thread)
static bool g_busy;

extern "C" JNIEXPORT void JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeModImportDone(JNIEnv *env, jclass,
                                                                                                          jstring message) {
    const char *text = env->GetStringUTFChars(message, NULL);
    pthread_mutex_lock(&g_lock);
    g_msg = text ? text : "";
    g_busy = false;
    pthread_mutex_unlock(&g_lock);
    if (text) env->ReleaseStringUTFChars(message, text);
}

static void import_mod(void) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return;
    jclass c = env->GetObjectClass(act);
    // (Game Files > Game files: Use them in place - the mod read where it is too, mods/<name>.link: runtime/mods.c)
    jmethodID m = env->GetStaticMethodID(c, g_settings.files_in_place ? "importModInPlace" : "importMod", "(Ljava/lang/String;)V");
    bool called = false;
    if (m) {
        char dir[1200];  // (this game version's data folder: each version reads its own mods)
        snprintf(dir, sizeof dir, "%s/mods", rt_data_root());
        jstring d = env->NewStringUTF(dir);
        env->CallStaticVoidMethod(c, m, d);
        env->DeleteLocalRef(d);
        called = true;
    }
    if (env->ExceptionCheck()) env->ExceptionClear(), called = false;
    env->DeleteLocalRef(c);
    env->DeleteLocalRef(act);
    pthread_mutex_lock(&g_lock);
    g_busy = called;
    g_msg = called ? "" : "0|Could not open the folder picker.";
    pthread_mutex_unlock(&g_lock);
}

extern "C" const char *fs_aoc_image(void);  // (runtime/services/fs.c: the DLC image, NULL when not set up)

extern "C" void mods_tab_draw(float scale) {
    opt_begin("##mods", scale);
    opt_section("Mods");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Mods replace the game's files: each is a folder with a romfs folder inside, laid out as for "
                                 "Yuzu, Eden or Ryujinx (made for this game version). Turning one on or off applies when the "
                                 "game starts again: Quit -> Return to start, then Play. Only files the game has are "
                                 "replaced."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    static ModInfo mods[MODS_MAX];
    static int n = -1;
    static double at;
    double now = SDL_GetTicks() / 1000.0;
    if (n < 0 || now - at > 1) n = mods_list(mods, MODS_MAX), at = now;  // (once a second: a copy can land meanwhile)
    if (!n) ImGui::TextDisabled("%s", tr("No mods yet."));
    for (int i = 0; i < n; i++) {
        static char notes[MODS_MAX][1600];
        static char note_of[MODS_MAX][96];
        if (strcmp(note_of[i], mods[i].name)) {  // (read once per mod)
            mods_note(mods[i].name, notes[i], sizeof notes[i]);
            snprintf(note_of[i], sizeof note_of[i], "%s", mods[i].name);
        }
        bool on = mods[i].on;
        if (opt_toggle(mods[i].name, &on, true, notes[i])) {
            mods_set(mods[i].name, on);
            mods[i].on = on;
            settings_save();
        }
    }
    {   // A mod made for the DLC (its folder for the DLC's title, as Relics of the Past ships) with no DLC set up: the
        // game closes at the title screen (#65 - reproduced on 1.6.0, fine with the DLC)
        bool dlc_mod = false;
        for (int i = 0; i < n; i++) dlc_mod |= mods[i].on && strcasestr(mods[i].name, "01007EF00011F001") != NULL;
        if (dlc_mod && !fs_aoc_image()) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                               tr("A mod for the DLC is on, but the DLC isn't set up (Settings > Files > DLC). Mods made "
                                  "with it, like Relics of the Past, close the game at the title screen without it."));
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::Spacing();
    pthread_mutex_lock(&g_lock);
    bool busy = g_busy;
    std::string msg = g_msg;
    pthread_mutex_unlock(&g_lock);
    ImGui::BeginDisabled(busy);
    if (opt_button("Import a mod folder", "Pick the mod's folder (the one with romfs inside, already extracted from its "
                                          "ZIP / RAR): it is copied into the app, for the game version running now."))
        import_mod();
    ImGui::EndDisabled();
    if (busy) ImGui::TextDisabled("%s", tr("Copying the mod..."));
    else if (msg.size() > 1) {  // (from the activity: "1|<name>" imported, "0|<reason>" failed, "2|" cancelled)
        char line[400];
        if (msg[0] == '1') snprintf(line, sizeof line, tr("Imported %s. It is on, from the next start of the game."), msg.c_str() + 2);
        else if (msg[0] == '2') snprintf(line, sizeof line, "%s", tr("Import cancelled."));
        else snprintf(line, sizeof line, tr("Import failed: %s"), tr(msg.c_str() + 2));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(line);
        ImGui::PopTextWrapPos();
    }
    opt_end();
}
#endif
