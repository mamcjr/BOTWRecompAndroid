#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __ANDROID__
// Freeze watchdog (watchdog.c): where a thread is blocked, a relaxed store at each blocking call. Render thread and every
// other thread (the game's) have a place each. vk_wd_enter returns what was there, for vk_wd_leave to put back.
enum {
    WD_NONE, WD_R_WAIT_GAME, WD_R_TRANSLATE, WD_R_PRESENT_REQ, WD_R_FENCE_REQ, WD_R_CALL_REQ, WD_R_SUBMIT, WD_R_SEQ_WAIT,
    WD_R_ACQUIRE, WD_R_QUEUE_PRESENT, WD_R_SWAPCHAIN, WD_R_BUILD_WAIT, WD_GPU_WAIT, WD_G_QUEUE_FULL, WD_G_WAIT_ID, WD_G_DRAIN, WD_G_SYNCWAIT,
    WD_R_BC_DECODE, WD_STAGES
};
typedef struct { uint32_t stage, outer; uint64_t ns, arg; } WdMark;
WdMark vk_wd_enter(uint32_t stage, uint64_t arg);
void vk_wd_leave(WdMark m);
void vk_wd_set(uint32_t stage, uint64_t arg);   // (no restore: the render thread's loop)
void vk_watchdog_start(void);
bool nvn_background_paused(void);               // objects.c
void vk_async_wd_info(uint64_t *queued, uint64_t *done, int *presents);  // async.c
void vk_wd_gpu_info(uint64_t *submitted, uint64_t *done, uint64_t *submit_age_ns, bool *waiter);  // exec.c
uint64_t vk_wd_timeline_value(void);            // exec.c: asks the driver
uint64_t vk_wd_frames_done(void);               // device.c: presents the render thread finished
uint64_t vk_wd_frames_asked(void);              // device.c: presents the game's thread handed over
void vk_device_fault_probe(void);               // device.c: the fault report, on demand
#endif
