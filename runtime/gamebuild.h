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
    // Fast mode (Android): everything at -O0, no cold-function scan - minutes, not an hour, to first play. The
    // library it leaves is marked (gamelib.c gamelib_is_fast): "<GAMELIB_ID>-fast", and the first play session
    // after it records the functions the player's game entered (fnprof), for the -O1 rebuild that follows on the
    // start screen (runtime/ui/launcher.mm).
    bool fast;
    // Keep the current library until this one is whole: it and its .id are not removed at the start, so a failed
    // build - or the app closed mid-way - changes nothing, and the new library is swapped in by the final rename.
    // The upgrade of a fast library builds this way.
    bool keep_current;
} GameBuildOpts;

// Builds it (a few minutes). false: why says what went wrong (details in the log).
bool gamebuild_run(const char *game_dir, const GameBuildOpts *opts, char *why, size_t why_size);
// The generator and the compiler are there (shipped builds; a source checkout with tools/venv and clang).
bool gamebuild_available(void);

#ifdef __cplusplus
}
#endif
