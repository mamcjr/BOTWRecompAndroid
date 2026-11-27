// Code mods (exefs mods, as emulators take them: <mod>/exefs/<main's build id>.ips and extra modules like subsdk6 -
// exlaunch's). The game code is recompiled, so a mod's code goes in when the game code is built: its IPS patches into
// main before the generator reads it (tools/nso.py), its modules generated as modules of their own (gen.py --sweep),
// placed right after main as the Switch's loader does - where an exlaunch patch looks for them. At run time the same
// patches go into main's memory (the code the mod reads as data, and Native mode's code) and the modules are loaded
// there too (runtime/loader.c). The library's id gets their fingerprint, so another set of code mods rebuilds the game
// code (runtime/gamelib.c, gamebuild.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CODEMODS_IPS_MAX 16
#define CODEMODS_MOD_MAX 2  // (runtime/rt.h g_modules: main, sdk and these)

typedef struct {
    int nips;
    char ips[CODEMODS_IPS_MAX][1024];  // IPS / IPS32 patches for main, in the mods' order (by name)
    int nmod;
    char mod_name[CODEMODS_MOD_MAX][16];     // "subsdk6"...
    char mod_path[CODEMODS_MOD_MAX][1024];   // its file
    uint64_t hash;                           // of all of them (0: no code mods)
} CodeMods;

#ifdef __cplusplus
extern "C" {
#endif

// Code mods went into this boot's modules (runtime/loader.c): Native mode leaves its own fixes out where they changed code
extern bool g_codemods_active;
// The code mods that are on (mods/, runtime/mods.h), for the main NSO at main_path (its build id picks the patches)
void codemods_find(const char *main_path, CodeMods *out);
// What the game code library's id gets after GAMELIB_ID: "" with no code mods, else "+<16 hex digits>"
void codemods_id_suffix(const CodeMods *cm, char *out, size_t cap);
// An IPS / IPS32 patch into a module loaded at `image` (`size` bytes; its offsets are the NSO's with the 0x100-byte
// header in front). Returns the end (offset in the image) of the furthest write, 0 if nothing was written.
uint64_t codemods_apply_ips(const char *path, uint8_t *image, uint64_t size);
// Where the first extra module goes: the end of main's image (data + bss), at the Switch's page size
uint64_t codemods_main_end(const char *main_path);
// The size an extra module takes (its image, page-aligned)
uint64_t codemods_module_size(const char *path);

#ifdef __cplusplus
}
#endif
