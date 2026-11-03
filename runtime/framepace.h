// Frame rate and the game's clock for BOTW 1.8.2 (runtime/hle/framepace.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The frame rate the runtime paces to now, frames per second (0: no limit): the setting (Video > Frame rate), 30
// in the menus, the events and while a movie plays.
int framepace_target(void);
// The time between presents for it, nanoseconds (no limit: 1/300 s).
uint64_t framepace_period_ns(void);
// true: the runtime paces the presents and sets the game's clock (1.8.2; LARECOMP_FRAMEPACE=0: the game's own pacing)
bool framepace_on(void);
// 1.8.2: the game is paused (pause menu, map) or the title screen's menu is up
bool framepace_in_menu(void);
// 1.8.2: the title screen's menu is up
bool framepace_on_title(void);
// The present (nvn/objects.c): a frame went out at t (plat_now_ns), on its pace's grid or not.
void framepace_presented(uint64_t t, bool on_grid);

// Called by the game code (tools/patches.py): the frame loop's vsync wait (1: skip it, 0: run the game's own), and
// its clock's update (x19 of ksys::VFR::updateInterval).
int rt_frame_wait(void);
void rt_vfr_update(uint64_t vfr);
// The game's clock's interval as the runtime last set it (1, 2, 3: its 60, 30, 20 fps ways)
uint32_t framepace_interval(void);
