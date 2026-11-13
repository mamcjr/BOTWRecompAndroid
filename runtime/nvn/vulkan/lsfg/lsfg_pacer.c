// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Ported to C for Banana Native from Eden's src/video_core/renderer_vulkan/present/frame_gen_pacer.cpp: how many frames to generate for each game frame. With a fixed
// multiplier, multiplier - 1 once the game's frame rate is steady; with a target rate, as many as hold that output rate, probing one more at a time and rolling a step back
// when it lowers the game's own rate. Times are seconds on CLOCK_MONOTONIC (0 = unset)
#include "lsfg.h"
#include <math.h>
#include <string.h>

#define INTERVAL_SMOOTHING 0.25f
#define MINIMUM_BASE_RATE 10.0f
#define BURST_CADENCE_RATIO 3.0f
#define BURST_TARGET_RATIO 2.0f
#define PROBE_THROUGHPUT_TOLERANCE 0.95f
#define PROBE_BASE_COLLAPSE_RATIO 0.70f
#define PROBE_MARGINAL_GAIN 1.15f
#define TARGET_SATISFIED_RATIO 0.95f
#define UNLOADED_BASE_RETENTION 0.75f
#define CREDIT_EPSILON 1.0e-4f
#define MAX_PROBE_FAILURES 4u
#define STABILIZATION_DURATION 1.0
#define PROBE_DURATION 1.0
#define DEFICIT_DURATION 1.0
#define PROBE_STEP_DELAY 0.25

static double probe_backoff(uint32_t failures) { return failures == 1 ? 5 : failures == 2 ? 15 : failures == 3 ? 30 : 60; }
static size_t zmin(size_t a, size_t b) { return a < b ? a : b; }
static float fmin_(float a, float b) { return a < b ? a : b; }

void lsfg_pacer_reset(lsfg_pacer *p) { memset(p, 0, sizeof *p); }

static void stabilize(lsfg_pacer *p, double now) {
    p->stable_until = now + STABILIZATION_DURATION; p->probe_until = 0; p->deficit_since = 0;
    p->smoothed_interval = 0; p->output_credit = 0;
}
static void defer_evaluations(lsfg_pacer *p, double amount) {
    if (p->stable_until) p->stable_until += amount;
    if (p->probe_until) p->probe_until += amount;
    if (p->next_probe) p->next_probe += amount;
    p->deficit_since = 0;
}

static void update_limit(lsfg_pacer *p, double now, float base_rate, float target_rate, size_t ceiling) {
    p->limit = zmin(p->limit, ceiling);
    if (p->probe_until) {
        if (now < p->probe_until) return;
        p->probe_until = 0; p->output_credit = 0;
        float previous_output = fmin_(target_rate, p->probe_base_rate * (float)(p->probe_previous_limit + 1));
        float current_output = fmin_(target_rate, base_rate * (float)(p->limit + 1));
        int throughput_regressed = current_output < previous_output * PROBE_THROUGHPUT_TOLERANCE;
        int collapsed_for_marginal_gain = base_rate < p->probe_base_rate * PROBE_BASE_COLLAPSE_RATIO && current_output < previous_output * PROBE_MARGINAL_GAIN;
        int emulation_slowed = p->unloaded_base_rate > 0 && base_rate < p->unloaded_base_rate * UNLOADED_BASE_RETENTION;
        if (throughput_regressed || collapsed_for_marginal_gain || emulation_slowed) {
            p->limit = p->probe_previous_limit;
            p->probe_failures = p->probe_failures + 1 < MAX_PROBE_FAILURES ? p->probe_failures + 1 : MAX_PROBE_FAILURES;
            p->next_probe = now + probe_backoff(p->probe_failures);
            p->deficit_since = 0;
            return;
        }
        p->probe_failures = 0;
        p->next_probe = now + PROBE_STEP_DELAY;
    }
    if (base_rate * (float)(p->limit + 1) >= target_rate * TARGET_SATISFIED_RATIO || p->limit >= ceiling) { p->deficit_since = 0; return; }
    if (!p->deficit_since) { p->deficit_since = now; return; }
    if (now - p->deficit_since < DEFICIT_DURATION) return;
    if (p->next_probe && now < p->next_probe) return;
    p->probe_previous_limit = p->limit; p->probe_base_rate = base_rate;
    p->limit++;
    p->probe_until = now + PROBE_DURATION; p->deficit_since = 0; p->output_credit = 0;
}

lsfg_plan lsfg_pacer_plan(lsfg_pacer *p, size_t capacity, int multiplier, int target_rate, double now) {
    lsfg_plan none = {0, 0};
    if (multiplier < 2) multiplier = 2;
    if (multiplier > 4) multiplier = 4;
    size_t max_gen = target_rate ? 3 : (size_t)(multiplier - 1);   // Settings::FrameGenMaxGenerations
    size_t ceiling = zmin(capacity, max_gen);
    if (!ceiling) { lsfg_pacer_reset(p); return none; }
    size_t previous_generations = p->issued; p->issued = 0;
    if (!p->last_frame) { p->last_frame = now; return none; }
    float interval = (float)(now - p->last_frame);
    p->last_frame = now;
    if (interval <= 0) { stabilize(p, now); return none; }
    float target = (float)target_rate;
    if (p->smoothed_interval > 0) {
        float burst = BURST_CADENCE_RATIO / p->smoothed_interval;
        if (target > 0 && target * BURST_TARGET_RATIO > burst) burst = target * BURST_TARGET_RATIO;
        if (1.0f / interval > burst) { defer_evaluations(p, interval); p->output_credit = 0; return none; }
    }
    if (interval > 1.0f / MINIMUM_BASE_RATE) { stabilize(p, now); return none; }
    p->smoothed_interval = p->smoothed_interval > 0 ? p->smoothed_interval + (interval - p->smoothed_interval) * INTERVAL_SMOOTHING : interval;
    if (!previous_generations) {
        float measured = 1.0f / p->smoothed_interval;
        p->unloaded_base_rate = p->unloaded_base_rate > 0 ? p->unloaded_base_rate + (measured - p->unloaded_base_rate) * INTERVAL_SMOOTHING : measured;
    }
    if (p->stable_until) {
        if (now < p->stable_until) return none;
        p->stable_until = 0;
    }
    if (!target_rate) {
        p->limit = zmin((size_t)(multiplier - 1), ceiling);   // Settings::FrameGenGenerations
        p->output_credit = 0; p->issued = p->limit;
        return (lsfg_plan){p->limit, p->limit > 0};
    }
    update_limit(p, now, 1.0f / p->smoothed_interval, target, ceiling);
    size_t allowed = zmin(p->limit, ceiling);
    float desired_outputs = p->smoothed_interval * target;
    if (!allowed || desired_outputs <= 1.0f) { p->output_credit = 0; return none; }
    p->output_credit += desired_outputs;
    size_t outputs = (size_t)floorf(p->output_credit + CREDIT_EPSILON); if (outputs < 1) outputs = 1;
    size_t generations = zmin(outputs - 1, allowed);
    p->output_credit -= (float)(generations + 1);
    if (p->output_credit < 0) p->output_credit = 0;
    else if (generations == allowed && p->output_credit >= 1.0f) p->output_credit = fmodf(p->output_credit, 1.0f);
    p->issued = generations;
    return (lsfg_plan){generations, 1};
}
