// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library. Everything else in it stays hidden.
#include "gamelib.h"

extern const uint64_t main_fn_addrs[], sdk_fn_addrs[], main_fn_count, sdk_fn_count;
extern const GuestFn main_fn_ptrs[], sdk_fn_ptrs[];

// Code mods' modules (runtime/codemods.h): generated only when a mod brings one, so weak - absent ones are null
#define EXTRA(n)                                                                                        \
    extern const uint64_t subsdk##n##_fn_addrs[] __attribute__((weak)), subsdk##n##_fn_count __attribute__((weak)); \
    extern const GuestFn subsdk##n##_fn_ptrs[] __attribute__((weak));
EXTRA(1) EXTRA(2) EXTRA(3) EXTRA(4) EXTRA(5) EXTRA(6) EXTRA(7) EXTRA(8) EXTRA(9)
#undef EXTRA

GAMELIB_EXPORT const GameLibTables *botwgame_tables(void) {
    static GameLibTables t = {GAMELIB_ABI,  GAMELIB_ID,   main_fn_addrs,  sdk_fn_addrs,
                              main_fn_ptrs, sdk_fn_ptrs,  &main_fn_count, &sdk_fn_count};
    static int done;
    if (!done) {
        done = 1;
#define EXTRA(n)                                                                                         \
        if (subsdk##n##_fn_addrs && &subsdk##n##_fn_count)                                               \
            t.extra[t.nextra++] = (GameLibModule){"subsdk" #n, subsdk##n##_fn_addrs, subsdk##n##_fn_ptrs, \
                                                  &subsdk##n##_fn_count};
        EXTRA(1) EXTRA(2) EXTRA(3) EXTRA(4) EXTRA(5) EXTRA(6) EXTRA(7) EXTRA(8) EXTRA(9)
#undef EXTRA
    }
    return &t;
}
