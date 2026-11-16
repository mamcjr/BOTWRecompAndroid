// 3D reverb (effect type 5) and the auxiliary buffer (type 2), included by audio.c.
//
// Adapted from AurelioB/totk-android (games/totk/runtime/audio_reverb.c and audio_mixer.c mix_aux), MIT license,
// Copyright (c) 2026 MacNativeGames contributors; the reverb network is in turn based on Ryujinx.Audio's
// Reverb3dState/Command, DelayLine3d and DecayDelay, Copyright (c) Ryujinx Team and Contributors, MIT license.
// See NOTICE.md. Changes here: invalid parameters skip the effect instead of stopping the game; samples are this
// mixer's PCM16-scale floats (the network is linear, so the scale is the same either way); the delay lines wrap at
// their buffer's size (r3_delay_update).
//
// BOTW 1.9.0 puts two auxiliary buffers (type 2) on mix 1; no 3D reverb (type 5) showed up in its logs, but the SDK has
// it. Before, both were skipped, so the aux's output buffers got nothing back from the game's own processing.

typedef struct {
    uint8_t input[6], output[6];
    uint16_t channel_max, channels;
    uint32_t reserved, sample_rate;
    float room_hf, hf_reference, decay_time, hf_decay_ratio, room_gain, reflections_gain, reverb_gain, diffusion,
        reflection_delay, reverb_delay, density, dry_gain;
    uint8_t status;
} Reverb3dParam;
_Static_assert(__builtin_offsetof(Reverb3dParam, status) == 72, "reverb3d wire layout");

typedef struct { float *data; unsigned cap, delay, read, write; } R3Delay;
typedef struct {
    Reverb3dParam p;
    R3Delay fdn[4], allpass1[4], allpass2[4], pre, center;
    unsigned early_time[20], reflection_time;
    float pre_previous, pre_gain, pre_target, early_gain, late_gain, diffusion;
    float direct[4], current[4], output[4], history[4];
} Reverb3d;

static const float r3_fdn_min[4] = {5, 6, 13, 14}, r3_fdn_max[4] = {45.704f, 82.782f, 149.94f, 271.58f};
static const float r3_ap1_max[4] = {17, 13, 9, 7}, r3_ap2_max[4] = {19, 11, 10, 6};
static const float r3_early_times[20] = {.017136f, .059154f, .16173f, .39019f, .42526f, .45541f, .68974f,
                                         .74591f,  .83384f,  .8595f,  0,       .075024f, .16879f, .2999f,
                                         .33744f,  .3719f,   .59901f, .71674f, .81786f,  .85166f};
static const float r3_early_gains[20] = {.67096f, .61027f, 1,      .35680f, .68361f, .65978f, .51939f,
                                         .24712f, .45945f, .45021f, .64196f, .54879f, .92925f, .38270f,
                                         .72867f, .69794f, .5464f,  .24563f, .45214f, .44042f};

static unsigned r3_samples(unsigned khz, float ms) { return (unsigned)nearbyintf(khz * ms); }
static int r3_delay_set(R3Delay *d, unsigned n) {
    if (n > d->cap) return 0;
    d->delay = n;
    d->write = (d->read + n) % (d->cap + 1);
    return 1;
}
static int r3_delay_init(R3Delay *d, unsigned khz, float ms) {
    d->cap = r3_samples(khz, ms);
    d->read = 0;
    d->data = calloc(d->cap + 1, sizeof(float));
    return d->data && r3_delay_set(d, d->cap);
}
static float r3_delay_update(R3Delay *d, float v) {
    // (the indexes wrap at the buffer's size, cap + 1, as the taps and the delay setting do: the reference wraps them
    // at cap, which left the first sample written at [cap] for good - an echo of it every 400 ms through the taps)
    d->data[d->write] = v;
    float out = d->data[d->read];
    if (++d->read > d->cap) d->read = 0;
    if (++d->write > d->cap) d->write = 0;
    return out;
}
static float r3_delay_tap(const R3Delay *d, unsigned at) {
    int i = (int)d->write - (int)at - 1;
    if (i < 0) i += (int)d->cap + 1;
    return d->data[i];
}
static float r3_allpass(R3Delay *d, float x, float gain) {
    float v = x - gain * d->data[d->read];
    return r3_delay_update(d, v) + gain * v;
}
static float r3_gain10(float x) { return x >= 0 ? 1 : x <= -5.3f ? 0 : powf(10, x); }
static float r3_radians(float x) { return x * 3.14159265358979323846f / 180; }
static int r3_valid(const Reverb3dParam *p) {
    return (p->channels == 1 || p->channels == 2 || p->channels == 4 || p->channels == 6) && p->sample_rate >= 1000 &&
           p->sample_rate <= 192000 && p->decay_time > 0 && p->hf_decay_ratio > 0 && p->density >= 0 &&
           p->density <= 100 && p->reflection_delay >= 0 && p->reverb_delay >= 0 &&
           p->reflection_delay + p->reverb_delay <= .4f;
}
static void r3_free(Reverb3d *s) {
    if (!s) return;
    for (int i = 0; i < 4; i++) free(s->fdn[i].data), free(s->allpass1[i].data), free(s->allpass2[i].data);
    free(s->pre.data), free(s->center.data), free(s);
}
static int r3_update(Reverb3d *s, const Reverb3dParam *p) {
    if (!r3_valid(p)) return 0;
    s->p = *p;
    unsigned khz = p->sample_rate / 1000;
    memset(s->history, 0, sizeof s->history);
    s->pre_previous = 0;
    s->early_gain = r3_gain10(fminf(p->room_gain + p->reflections_gain, 5000) / 2000);
    s->late_gain = r3_gain10(fminf(p->room_gain + p->reverb_gain, 5000) / 2000);
    float hf = r3_gain10(p->room_hf / 2000);
    if (hf < 1) {
        float a = 1 - hf, b = 2 - 2 * hf * cosf(r3_radians(256 * p->hf_reference / p->sample_rate));
        s->pre_gain = (b - sqrtf(b * b - 4 * a * a)) / (2 * a);
        s->pre_target = 1 - s->pre_gain;
    } else {
        s->pre_gain = 0;
        s->pre_target = 1;
    }
    s->reflection_time = r3_samples(khz, 1000 * (p->reflection_delay + p->reverb_delay));
    if (s->reflection_time > s->pre.cap) s->reflection_time = s->pre.cap;
    s->diffusion = .6f * p->diffusion * .01f;
    for (int i = 0; i < 4; i++) {
        if (!r3_delay_set(&s->fdn[i], r3_samples(khz, r3_fdn_min[i] + p->density / 100 * (r3_fdn_max[i] - r3_fdn_min[i]))))
            return 0;
        float n = (float)(s->fdn[i].delay + s->allpass1[i].delay + s->allpass2[i].delay);
        float a = -60 * n / (p->decay_time * p->sample_rate), b = a / p->hf_decay_ratio;
        float angle = r3_radians(128 * .5f * p->hf_reference / p->sample_rate), c = cosf(angle) / sinf(angle);
        float d = r3_gain10((b - a) / 40), e = r3_gain10((b + a) / 40) * .7071f;
        s->direct[i] = e * (d * c + 1) / (c + d);
        s->current[i] = e * (1 - d * c) / (c + d);
        s->output[i] = (c - d) / (c + d);
    }
    for (int i = 0; i < 20; i++) {
        unsigned n = r3_samples(khz, p->reflection_delay * 1000 + r3_early_times[i] * 1000 * (p->reverb_delay * .9998f + .02f));
        s->early_time[i] = n < s->pre.cap ? n : s->pre.cap;
    }
    return 1;
}
static Reverb3d *r3_create(const Reverb3dParam *p) {
    if (!r3_valid(p)) return NULL;
    Reverb3d *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    unsigned khz = p->sample_rate / 1000;
    int ok = 1;
    for (int i = 0; i < 4; i++)
        ok &= r3_delay_init(&s->fdn[i], khz, r3_fdn_max[i]) & r3_delay_init(&s->allpass1[i], khz, r3_ap1_max[i]) &
              r3_delay_init(&s->allpass2[i], khz, r3_ap2_max[i]);
    ok &= r3_delay_init(&s->pre, khz, 400) & r3_delay_init(&s->center, khz, 5);
    if (!ok || !r3_update(s, p)) {
        r3_free(s);
        return NULL;
    }
    return s;
}
static void r3_process(Reverb3d *s, float *const input[6], float *const output[6], int frames) {
    static const unsigned stereo[20] = {0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1};
    static const unsigned quad[20] = {0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0, 3, 3, 3};
    // (the reference's legacy surround channel order: L, R, BL, BR, C, LFE)
    static const unsigned surround[20] = {4, 5, 0, 5, 0, 5, 1, 5, 1, 5, 1, 5, 1, 5, 2, 5, 2, 5, 2, 5};
    const int channels = s->p.channels;
    for (int k = 0; k < frames; k++) {
        float dry[6], early[6] = {0}, fb[4], v[4], sum = 0, tap = r3_delay_tap(&s->pre, s->reflection_time);
        for (int i = 0; i < 20; i++) {
            unsigned out = channels == 1 ? 0 : channels == 2 ? stereo[i] : channels == 4 ? quad[i] : surround[i];
            float x = r3_delay_tap(&s->pre, s->early_time[i]) * r3_early_gains[i];
            early[out] += x;
            if (channels == 6) early[out] += x;
        }
        for (int c = 0; c < channels; c++) dry[c] = input[c][k], sum += dry[c];
        s->pre_previous = sum * s->pre_target + s->pre_previous * s->pre_gain;
        r3_delay_update(&s->pre, s->pre_previous);
        for (int i = 0; i < 4; i++) {
            float x = s->fdn[i].data[s->fdn[i].read];
            fb[i] = x * s->direct[i] + s->history[i];
            s->history[i] = x * s->current[i] + fb[i] * s->output[i];
        }
        const float feedback[4] = {fb[2] + fb[1], -fb[0] - fb[3], fb[0] - fb[3], fb[1] - fb[2]};
        for (int i = 0; i < 4; i++) {
            v[i] = r3_allpass(&s->allpass2[i], r3_allpass(&s->allpass1[i], tap * s->late_gain + feedback[i], s->diffusion),
                              -.9f * s->diffusion);
            r3_delay_update(&s->fdn[i], v[i]);
        }
        for (int c = 0; c < channels; c++) {
            if (channels == 6 && c == 4)
                output[c][k] += early[c] * s->early_gain + r3_delay_update(&s->center, (v[2] - v[3]) * .5f) + dry[c] * s->p.dry_gain;
            else
                output[c][k] = early[c] * s->early_gain + v[c == 5 ? 3 : c] + dry[c] * s->p.dry_gain;
        }
        if (channels == 1) output[0][k] += v[1];
    }
}

// ---- auxiliary buffer: the game's own effects. Two rings in guest memory (send: the mix's samples out to the game;
// return: what it gives back), each a 0x80-byte header (CPU 0x40, then DSP 0x40: only the DSP's cursor is ours to
// move) followed by int32 samples, one channel's frame after another.
typedef struct {
    uint8_t input[24], output[24];
    uint32_t channels, sample_rate, capacity, channel_max;
    uint64_t send_info, send_storage, return_info, return_storage;
} AuxParam;
_Static_assert(__builtin_offsetof(AuxParam, send_info) == 64, "auxiliary effect wire layout");

static void aux_process(const AuxParam *p, bool enabled, float *const *in, float *const *out, uint32_t N) {
    if (!p->send_info || !p->return_info || !p->capacity || !p->channels || p->channels > 24 ||
        p->capacity > 16u * 1024 * 1024)
        return;
    uint8_t *send = (uint8_t *)(uintptr_t)p->send_info, *ret = (uint8_t *)(uintptr_t)p->return_info;
    plat_write_prepare(p->send_info + 64, 64 + (uint64_t)p->capacity * 4);
    plat_write_prepare(p->return_info + 64, 64);
    uint32_t *sh = (uint32_t *)(send + 64), *rh = (uint32_t *)(ret + 64);
    int32_t *sd = (int32_t *)(send + 128), *rd = (int32_t *)(ret + 128);
    uint32_t wr = sh[1], rdo = rh[0], total = p->channels * N;
    for (uint32_t c = 0; c < p->channels; c++) {
        if (!enabled) {
            if (in[c] != out[c]) memmove(out[c], in[c], N * sizeof(float));
            continue;
        }
        uint64_t w = (uint64_t)wr + c * N, at = (uint64_t)rdo + c * N;
        for (uint32_t k = 0; k < N; k++) {
            float x = truncf(in[c][k]);
            int32_t v = !isfinite(x) ? 0 : x >= 2147483647.0f ? INT32_MAX : x <= -2147483648.0f ? INT32_MIN : (int32_t)x;
            in[c][k] = (float)v;
            if (w <= p->capacity) sd[(w + k) % p->capacity] = v;
        }
        for (uint32_t k = 0; k < N; k++) out[c][k] = at <= p->capacity ? (float)rd[(at + k) % p->capacity] : 0;
    }
    if (!enabled) {
        sh[0] = sh[1] = sh[3] = 0;
        rh[0] = rh[1] = rh[3] = 0;
        return;
    }
    uint64_t last = (uint64_t)(p->channels - 1) * N;  // (as the reference: the last channel's start in range)
    if (wr + last <= p->capacity) sh[1] = (wr + total) % p->capacity;
    if (rdo + last <= p->capacity) rh[0] = (rdo + total) % p->capacity;
}
