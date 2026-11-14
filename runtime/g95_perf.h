// Device-specific performance profile (Helio G95: runtime/g95_perf.c). Every call is a no-op on other SoCs.
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool g95_on(void);
bool g95_pin_render_thread(void);      // the render thread -> the A76 cores (called from platform/android.c)
void g95_uclamp_thread(void);          // a utilization floor for the calling thread (every thread that starts here)
void g95_keep_resident(uint64_t lo, uint64_t size);  // madvise WILLNEED the game's code and the runtime's tables
bool set_uclamp_min(int want, const char *what);     // (shared with platform/android.c's own uclamp experiment)
