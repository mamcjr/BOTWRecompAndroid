// Builds the game code library (runtime/gamelib.h) on the player's machine, from their own game files: the generator
// (tools/gen.py; shipped frozen as botwgen/) turns exefs/main and exefs/sdk into C, the bundled compiler (zig/) builds
// that into <game_dir>/botwgame.*. The C and the objects are deleted afterwards.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // now and then, from the calling thread: how far the whole build is (0..1) and what it's doing
    void (*progress)(double fraction, const char *step);
    int jobs;  // compiler processes at once (>= 1)
    // Fast mode (Android): everything at -O0 - minutes, not an hour, to first play. The library it leaves is marked
    // (gamelib.c gamelib_fast_present): "<GAMELIB_ID>-fast", and the start screen upgrades it in the background
    // (runtime/ui/launcher.mm) with everything a full build does - the app's own shipped hot list (load_hot_for),
    // the cold marking, the memory floors - only the codegen level differs. The scan for the giant files runs in
    // this mode too (they build alone: Unit.alone); with no hot list loaded, nothing is marked cold, only detected.
    bool fast;
    // Keep the current library until this one is whole: it and its .id are not removed at the start, so a failed
    // build - or the app closed mid-way - changes nothing, and the new library is swapped in by the final rename.
    // The upgrade of a fast library builds this way.
    bool keep_current;
    // Asked from outside, between units: true = stop launching compilers, end the ones running and return false
    // with why "postponed" - the work folder stays, so the next build goes on from where this stopped. The
    // background upgrade is stopped this way (the player pressed PLAY or Later, or a setup needs the builders);
    // NULL when nothing can stop this build. Read from another thread: keep it atomic.
    bool (*stopped)(void);
} GameBuildOpts;

// Builds it (a few minutes). false: why says what went wrong (details in the log).
bool gamebuild_run(const char *game_dir, const GameBuildOpts *opts, char *why, size_t why_size);
// The generator and the compiler are there (shipped builds; a source checkout with tools/venv and clang).
bool gamebuild_available(void);

#ifdef __cplusplus
}
#endif
