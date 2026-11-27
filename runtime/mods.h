// Game mods (Android; LASRecompAndroid's): folders in mods/ in the game's data folder (each game version its own),
// each with a romfs/ tree laid out as emulators take them
// (Eden, Ryujinx: <mod>/romfs/<the game's own paths>). Their files replace the game's when the game starts
// (services/fs.c) - a fan translation, retextures... Every mod found is on unless turned off in the menu's Mods page
// (ui/mods_tab.mm; botwrecompandroid.ini mods_off). Files the game doesn't have are added (services/fs.c
// add_new_files), new folders too.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define MODS_MAX 32
typedef struct {
    char name[96];  // its folder's name
    bool on;
} ModInfo;

// The mods in mods/ (sorted by name), up to max - a folder with romfs/ or exefs/ (code mods: runtime/codemods.h)
int mods_list(ModInfo *out, int max);
// A mod's folder (mods/<name>, or where its .link points)
bool mods_base(const char *name, char *out, size_t cap);
// Turns one on or off (botwrecompandroid.ini: settings_save after)
void mods_set(const char *name, bool on);
// The game opens its RomFS: the files of the mods that are on, looked up by mods_romfs_override from then on
void mods_scan(void);
// Host path of the file replacing RomFS file `path` ("regionUS/USen/message/System.msbt"): the first mod that's on (by
// name) with that file. False: none.
bool mods_romfs_override(const char *path, char *out, size_t cap);
// The files of the mods that are on, after mods_scan: i from 0 to mods_file_count() - 1, sorted by RomFS path, each
// path once (the mod mods_romfs_override would take). False: past the end.
int mods_file_count(void);
bool mods_file(int i, const char **path, const char **host);
// Which image the lookups above are for: the game's (false, the default) or the DLC's - mod folders named for the
// DLC's title (01007EF00011F001), whose files are the DLC's own (services/fs.c)
void mods_select_dlc(bool dlc);
// The mod's own note - a .txt in its folder (Sobre.txt, README.txt...) - its first `cap` bytes, in the menu font's
// characters (ASCII and accented Latin letters; symbols and emoji left out). "" if it has none.
const char *mods_note(const char *name, char *buf, size_t cap);
