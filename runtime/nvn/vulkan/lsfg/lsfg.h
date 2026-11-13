// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// Lossless Scaling frame generation (LSFG) for the Vulkan replay, ported to C for Banana Native from Eden (src/video_core/frame_gen, src/video_core/renderer_vulkan/present/
// frame_gen*, lsfg_*), which builds on lsfg-vk. This folder is GPL-3.0-or-later (LICENSE.md here); an APK built with it (NX_LSFG, the default) is GPL-3.0-or-later as a whole.
// The shaders come from the user's own Lossless.dll at run time: never bundled.
#pragma once
#include <stddef.h>
#include <stdint.h>

// ---------- The DLL (lsfg_dll.c) ----------
enum { LSFG_OK, LSFG_NOT_INSTALLED, LSFG_UNREADABLE, LSFG_NOT_PE, LSFG_MISSING_SHADERS, LSFG_TRANSLATION_FAILED };
enum { LSFG_SH_MIPMAPS = 255, LSFG_SH_GENERATE = 256, LSFG_NATIVE_FP16_OFFSET = 49, LSFG_NMODULES = 25 };
// The performance shader set (the only one Eden uses): alpha 290..293, beta 298..302, gamma 280, 282..285, delta 280, 286..289, 281, 294..297
typedef struct { uint32_t id, nwords; uint32_t *words; } lsfg_module;
typedef struct { int n; lsfg_module m[LSFG_NMODULES]; } lsfg_modules;
int lsfg_dll_load(const char *path, lsfg_modules *out);   // LSFG_OK: out holds the 25 modules, bindings renumbered (lsfg_modules_free)
int lsfg_dll_validate(const char *path);
void lsfg_modules_free(lsfg_modules *m);
const lsfg_module *lsfg_module_get(const lsfg_modules *m, uint32_t id);
const char *lsfg_status_name(int status);

// ---------- The pacer (lsfg_pacer.c): how many frames to generate for the next game frame ----------
typedef struct {
    double last_frame, stable_until, probe_until, next_probe, deficit_since;   // seconds (CLOCK_MONOTONIC); 0 = unset
    float smoothed_interval, output_credit, probe_base_rate, unloaded_base_rate;
    size_t issued, probe_previous_limit, limit; uint32_t probe_failures;
} lsfg_pacer;
typedef struct { size_t generations; int warm; } lsfg_plan;
// multiplier 2..4 (images per game frame with target_rate 0), target_rate 0 or the output rate to hold (the multiplier then follows it)
lsfg_plan lsfg_pacer_plan(lsfg_pacer *p, size_t capacity, int multiplier, int target_rate, double now);
void lsfg_pacer_reset(lsfg_pacer *p);

// ---------- The chain (lsfg_chain.c), Vulkan; needs the replay's device (vka.h) ----------
#ifdef VK_VERSION_1_0
enum { LSFG_MAX_GENERATIONS = 3, LSFG_GENERATION_SLOTS = LSFG_MAX_GENERATIONS * (LSFG_MAX_GENERATIONS + 1) / 2 };
typedef struct lsfg_chain lsfg_chain;
// The chain for frames of extent ext (RGBA8 UNORM inputs); flow_scale 0.25..1 is the optical flow's resolution. outs[g] are the images generation g writes (storage, RGBA8)
lsfg_chain *lsfg_chain_new(const lsfg_modules *mods, VkExtent2D ext, float flow_scale, VkImage outs[LSFG_MAX_GENERATIONS], VkImageView out_views[LSFG_MAX_GENERATIONS]);
void lsfg_chain_free(lsfg_chain *c);   // the GPU must be done with it
VkImage lsfg_chain_input(lsfg_chain *c, uint64_t count);   // the image game frame count is copied into (GENERAL layout)
void lsfg_chain_shared(lsfg_chain *c, VkCommandBuffer cb, uint64_t count);                            // after the copy: mipmaps, alpha, beta of frame count
void lsfg_chain_generate(lsfg_chain *c, VkCommandBuffer cb, uint64_t count, size_t ngen, size_t gen);  // generation gen of ngen between frames count - 1 and count, into outs[gen]
#endif
