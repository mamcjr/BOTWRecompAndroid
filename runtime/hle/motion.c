// Motion controls: the phone's gyroscope and accelerometer - or a controller's, when it has them - as the Switch
// controller's six-axis sensor (nn::hid::GetSixAxisSensorStates): aiming the bow and the Sheikah Slate's camera, the
// shrines' motion puzzles. Settings > Controls > Motion controls (g_settings.motion) off: the controller lies still.
//
// Frames: SDL gives a sensor as a device held in front of you - x right, y up, z toward you (a phone in its natural
// portrait: turned to the screen here, the game being in landscape). The Switch's six-axis frame is x right, y
// forward, z up (yuzu's mapping of SDL controllers): angular velocity (x, -z, y) in turns per second, acceleration
// -(x, -z, y) in G (at rest it points down). The direction matrix follows the angular velocity, kept level by the
// acceleration (a Mahony filter): its world is z up, with the yaw the sensor started at.
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "../hle.h"
#include "../settings.h"

typedef struct {
    int64_t delta_time;   // ns since the sample before
    int64_t sampling_number;
    float accel[3];       // G
    float gyro[3];        // turns per second
    float angle[3];       // turns: the angular velocity summed
    float dir[3][3];      // the sensor's x, y and z axes in the world (lying still, the right way up: identity)
    uint32_t attributes;  // 1: connected
    uint32_t reserved;
} SixAxisState;
_Static_assert(sizeof(SixAxisState) == 0x60, "nn::hid::SixAxisSensorState");

#define RING 32
#define MAX_MOTION_PADS 16
#define KP 0.5f       // the acceleration's pull on the attitude (rad/s per unit of error)
#define TURN 6.2831853f

typedef struct {
    uint64_t prev_hw, prev_host;
    uint64_t win_hw, win_host;
    int win_n;
    float scale;
    bool has_prev;
    bool scale_logged;
} PadClock;

typedef struct {
    float bias[3];
    double sum[3], accel_sum[3];
    int count, accel_count;
    uint64_t still_since, last_host;
    bool ready, has_accel, near_bias, told;
} GyroBias;

typedef struct {
    SDL_JoystickID id;
    bool nintendo_android;
    PadClock clock;
    GyroBias bias;
    float accel[3];
    bool have_accel;
    // Auto-mode (motion_source == 0) per-pad liveness, protected under m_lock:
    bool live, turned;
    int changing;
    float last_gyro[3];
    uint64_t dead_ns, last_gyro_ns;
} MotionPad;

static pthread_mutex_t m_lock = PTHREAD_MUTEX_INITIALIZER;
static SixAxisState ring[RING];
static int ring_n, ring_head;            // samples kept, the newest's index
static int64_t m_sampling;
static float q[4] = {1, 0, 0, 0};        // w x y z: the sensor's attitude (sensor -> world)
static bool q_set;                       // (levelled by the first acceleration)
static float m_last_accel[3] = {0, 0, -1}, m_angle[3];
static uint64_t m_phone_gyro_ts, m_live_ns, m_rest_ns;  // phone gyro timestamp; host times
static uint64_t pad_until_ns;            // event thread only: controller active until then (phone waits)
static GyroBias m_phone_bias;
static float m_phone_accel[3] = {0, 0, -1}, m_phone_raw_accel[3] = {0, 0, -1};
static bool m_phone_have_accel;
static MotionPad m_pads[MAX_MOTION_PADS];
static SDL_JoystickID m_active_pad_id;
static uint64_t m_active_pad_gyro_ns, m_active_pad_turn_ns;
static int m_pad_axes_seen = -1;
static int m_last_flat = -1, m_last_phone = -1;

static bool vec3_finite(const float v[3]) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static bool env_enabled(const char *v) {
    return v && *v && strcmp(v, "0") != 0;
}

// q from the way down a (in G): no yaw - the rotation taking a to the world's (0, 0, -1)
static void level_from(const float a[3]) {
    float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!(n >= 0.5f)) return;
    float ux = a[0] / n, uy = a[1] / n, uz = a[2] / n;
    float w = 1 - uz, x = -uy, y = ux;  // (1 + u.v, u x v) with v = (0, 0, -1)
    float m = sqrtf(w * w + x * x + y * y);
    if (!isfinite(m) || m < 1e-4f) q[0] = 0, q[1] = 1, q[2] = 0, q[3] = 0;  // (upside down: half a turn about x)
    else q[0] = w / m, q[1] = x / m, q[2] = y / m, q[3] = 0;
    q_set = true;
}

// One step of the attitude: w (rad/s, the sensor's axes), pulled toward the measured down a
static void fuse(float w[3], const float a[3], float dt) {
    float qw = q[0], x = q[1], y = q[2], z = q[3];
    float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (n > 0.75f && n < 1.25f) {  // (not while shaken)
        // the world's down seen from the sensor: -(R's third row)
        float vx = -2 * (x * z - qw * y), vy = -2 * (y * z + qw * x), vz = -(1 - 2 * (x * x + y * y));
        float ax = a[0] / n, ay = a[1] / n, az = a[2] / n;
        w[0] += KP * (ay * vz - az * vy);
        w[1] += KP * (az * vx - ax * vz);
        w[2] += KP * (ax * vy - ay * vx);
    }
    q[0] += 0.5f * dt * (-x * w[0] - y * w[1] - z * w[2]);
    q[1] += 0.5f * dt * (qw * w[0] + y * w[2] - z * w[1]);
    q[2] += 0.5f * dt * (qw * w[1] - x * w[2] + z * w[0]);
    q[3] += 0.5f * dt * (qw * w[2] + x * w[1] - y * w[0]);
    float m = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(isfinite(m) && m > 1e-6f)) q[0] = 1, q[1] = q[2] = q[3] = 0;
    else for (int i = 0; i < 4; i++) q[i] /= m;
}

// A sample: angular velocity w (rad/s, the Switch's frame) over dt seconds (m_lock held)
static void add_sample(const float w[3], const float accel[3], bool have_accel, float dt) {
    if (!vec3_finite(w)) return;
    float wc[3] = {w[0], w[1], w[2]};
    if (have_accel && vec3_finite(accel)) {
        memcpy(m_last_accel, accel, sizeof m_last_accel);
        if (!q_set) level_from(accel);
        fuse(wc, accel, dt);
    } else {
        // Pure gyro integration avoids leveling from stale accel after a source/axis/reset change.
        static const float zero_a[3] = {0, 0, 0};
        fuse(wc, zero_a, dt);
    }
    ring_head = (ring_head + 1) % RING;
    if (ring_n < RING) ring_n++;
    SixAxisState *s = &ring[ring_head];
    memset(s, 0, sizeof *s);
    s->delta_time = (int64_t)(dt * 1e9f);
    s->sampling_number = ++m_sampling;
    for (int i = 0; i < 3; i++) {
        s->accel[i] = m_last_accel[i];
        s->gyro[i] = w[i] / TURN;
        m_angle[i] += s->gyro[i] * dt;
        s->angle[i] = m_angle[i];
    }
    float qw = q[0], x = q[1], y = q[2], z = q[3];
    float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - qw * z), 2 * (x * z + qw * y)},
                     {2 * (x * y + qw * z), 1 - 2 * (x * x + z * z), 2 * (y * z - qw * x)},
                     {2 * (x * z - qw * y), 2 * (y * z + qw * x), 1 - 2 * (x * x + y * y)}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) s->dir[i][j] = r[j][i];  // (row i: the sensor's axis i, a column of R)
    s->attributes = 1;
}

// Some Android kernel drivers (notably hid-nintendo) emit 32-bit MSC_TIMESTAMP clocks whose rate differs from real
// wall time (e.g. ~1.92x on 8BitDo Pro 2), inflating integrated gyro angles and causing Mahony leveling to snap back
// once motion stops. Measure hw_ts / host_ts over short windows and scale dt back to real seconds (m_lock held).
// Scoped by default to the Nintendo Android IMU path; LARECOMP_CLOCK_SCALE=1 enables it for any controller, and
// LARECOMP_NO_CLOCK_SCALE=1 disables scaling and falls back to raw sensor timestamps.
static float pad_clock_dt(PadClock *c, uint64_t hw_ts, uint64_t host_ts) {
    if (!c->has_prev || host_ts < c->prev_host || (host_ts - c->prev_host) > 100000000ull) {
        c->prev_hw = hw_ts;
        c->prev_host = host_ts;
        c->win_hw = c->win_host = 0;
        c->win_n = 0;
        c->has_prev = true;
        return 0.005f;
    }
    uint64_t dt_host_ns = host_ts - c->prev_host;
    float dt_host = (float)(dt_host_ns / 1e9);
    // 32-bit MSC_TIMESTAMP (us * 1000) wraps every ~4294.97 s, often mid-burst where dt_host == 0.
    const uint64_t wrap_ns = (1ull << 32) * 1000ull;
    if (hw_ts && c->prev_hw && hw_ts < c->prev_hw && c->prev_hw <= wrap_ns) {
        uint64_t wrapped_hw = (wrap_ns - c->prev_hw) + hw_ts;
        c->prev_hw = hw_ts;
        c->prev_host = host_ts;
        c->win_hw = c->win_host = 0;
        c->win_n = 0;
        if (wrapped_hw > 0 && wrapped_hw <= 250000000ull) {
            float dt_hw = (float)(wrapped_hw / 1e9);
            float dt = c->scale > 0.0f ? dt_hw / c->scale : (dt_host > 0.0f ? fminf(dt_host, dt_hw) : dt_hw);
            return (dt > 0.0f && dt <= 0.1f) ? dt : 0.005f;
        }
        return (dt_host > 0.0f && dt_host <= 0.1f) ? dt_host : 0.005f;
    }
    if (!hw_ts || !c->prev_hw || hw_ts <= c->prev_hw || (hw_ts - c->prev_hw) > 250000000ull) {
        c->prev_hw = hw_ts;
        c->prev_host = host_ts;
        c->win_hw = c->win_host = 0;
        c->win_n = 0;
        return (dt_host > 0.0f && dt_host <= 0.1f) ? dt_host : 0.005f;
    }
    uint64_t dt_hw_ns = hw_ts - c->prev_hw;
    c->prev_hw = hw_ts;
    c->prev_host = host_ts;
    c->win_hw += dt_hw_ns;
    c->win_host += dt_host_ns;
    c->win_n++;
    uint64_t min_host_ns = c->scale == 0.0f ? 250000000ull : 500000000ull;
    int min_n = c->scale == 0.0f ? 8 : 10;
    if (c->win_host >= min_host_ns && c->win_n >= min_n) {
        double r = (double)c->win_hw / (double)c->win_host;
        if (r > 0.2 && r < 5.0) {
            if (fabs(r - 1.0) < 0.08) r = 1.0;
            c->scale = c->scale == 0.0f ? (float)r : c->scale * 0.7f + (float)r * 0.3f;
            if (!c->scale_logged) {
                c->scale_logged = true;
                LOG("[motion] controller sensor clock ratio %.2fx real time (%s)\n", c->scale,
                    c->scale != 1.0f ? "scaling dt; disable with LARECOMP_NO_CLOCK_SCALE=1" : "unscaled");
            }
        }
        c->win_hw = c->win_host = 0;
        c->win_n = 0;
    }
    float dt_hw = (float)(dt_hw_ns / 1e9);
    // During initial 250 ms warmup (scale == 0), bound by hw delta when dt_host == 0 inside USB bursts.
    float dt = c->scale > 0.0f ? dt_hw / c->scale : (dt_host > 0.0f ? fminf(dt_host, dt_hw) : dt_hw);
    return (dt > 0.0f && dt <= 0.1f) ? dt : 0.005f;
}

static void bias_reset_transient(GyroBias *b) {
    b->still_since = 0;
    b->last_host = 0;
    b->count = 0;
    b->accel_count = 0;
    b->has_accel = false;
    b->near_bias = false;
    memset(b->sum, 0, sizeof b->sum);
    memset(b->accel_sum, 0, sizeof b->accel_sum);
}

// Learns resting gyro zero-rate bias using real host timestamps (SavageWindRecomp / MacNativeGames: learn it while
// still) so accelerated controller clocks cannot compress stillness windows. Checks running means with gyro + accel
// stability (so slow bow aiming does not skew bias), keeps gentle post-lock tracking within tol_g, and allows a strict
// 6 s re-lock if an initial lock or thermal drift shifted.
static void bias_update(GyroBias *b, const float d[3], const float accel[3], bool have_accel,
                        uint64_t host_ts, float dt, bool phone) {
    for (int i = 0; i < 3; i++) {
        // 0.08 rad/s (~4.6 deg/s) excludes intentional motion while keeping any plausible MEMS zero-rate offset.
        if (!(fabsf(d[i]) <= 0.08f)) {
            bias_reset_transient(b);
            return;
        }
    }
    if (b->count > 0 && (host_ts < b->last_host || host_ts - b->last_host > 250000000ull))
        bias_reset_transient(b);

    const float tol_g = phone ? 0.020f : 0.015f;  // handheld phones carry slightly more sensor noise than a rested pad
    const float tol_a = phone ? 0.035f : 0.025f;
    // 4 mrad/s Schmitt hysteresis avoids flickering between track and recover modes near the tol_g boundary.
    const float close_lim = (b->ready && b->count > 0 && b->near_bias) ? tol_g + 0.004f : tol_g;
    bool close = !b->ready;
    if (b->ready) {
        close = true;
        for (int i = 0; i < 3; i++) {
            if (!(fabsf(d[i] - b->bias[i]) <= close_lim)) { close = false; break; }
        }
    }
    if (b->count > 0 && b->near_bias != close)
        bias_reset_transient(b);

    if (b->count == 0) {
        for (int i = 0; i < 3; i++) {
            b->sum[i] = d[i];
            if (have_accel) b->accel_sum[i] = accel[i];
        }
        b->count = 1;
        b->accel_count = have_accel ? 1 : 0;
        b->has_accel = have_accel;
        b->near_bias = close;
        b->still_since = host_ts;
        b->last_host = host_ts;
        return;
    }
    if (!b->has_accel && have_accel) {
        memset(b->accel_sum, 0, sizeof b->accel_sum);
        b->accel_count = 0;
        b->has_accel = true;
    }
    const float stab_g = (close && !b->ready && !phone) ? 0.010f : (close ? tol_g : 0.008f);
    const float stab_a = (close && !b->ready && !phone) ? 0.020f : (close ? tol_a : 0.015f);
    bool still = true;
    for (int i = 0; i < 3; i++) {
        float mean_g = (float)(b->sum[i] / b->count);
        if (!(fabsf(d[i] - mean_g) <= stab_g)) { still = false; break; }
        if (have_accel && b->has_accel && b->accel_count > 0) {
            float mean_a = (float)(b->accel_sum[i] / b->accel_count);
            if (!(fabsf(accel[i] - mean_a) <= stab_a)) { still = false; break; }
        }
    }
    if (!still) {
        for (int i = 0; i < 3; i++) {
            b->sum[i] = d[i];
            if (have_accel) b->accel_sum[i] = accel[i];
        }
        b->count = 1;
        b->accel_count = have_accel ? 1 : 0;
        b->has_accel = have_accel;
        b->near_bias = close;
        b->still_since = host_ts;
        b->last_host = host_ts;
        return;
    }
    for (int i = 0; i < 3; i++) {
        b->sum[i] += d[i];
        if (have_accel) b->accel_sum[i] += accel[i];
    }
    b->count++;
    if (have_accel) b->accel_count++;
    b->last_host = host_ts;

    uint64_t elapsed = host_ts >= b->still_since ? host_ts - b->still_since : 0;
    if (!b->ready) {
        if (elapsed > 1500000000ull && b->count >= 20) {
            for (int i = 0; i < 3; i++) b->bias[i] = (float)(b->sum[i] / b->count);
            b->ready = true;
            if (!b->told) {
                b->told = true;
                LOG("[motion] %s gyroscope at rest: bias %.4f %.4f %.4f rad/s taken off\n",
                    phone ? "phone" : "controller", b->bias[0], b->bias[1], b->bias[2]);
            }
        }
    } else if (!close) {
        // Recover cleanly after 6 s of tight dead stillness if an earlier lock occurred in motion or thermally drifted.
        if (elapsed > 6000000000ull && b->count >= 60) {
            for (int i = 0; i < 3; i++) b->bias[i] = (float)(b->sum[i] / b->count);
            bias_reset_transient(b);
            LOG("[motion] %s gyroscope resting bias updated: %.4f %.4f %.4f rad/s\n",
                phone ? "phone" : "controller", b->bias[0], b->bias[1], b->bias[2]);
        }
    } else if (elapsed > 3000000000ull && b->count >= 20 && dt > 0.0f) {
        float alpha = fminf(dt * 0.25f, 0.05f);
        for (int i = 0; i < 3; i++) b->bias[i] += (d[i] - b->bias[i]) * alpha;
    }
}

#ifdef __ANDROID__
// Display.getRotation() of the activity's display, through JNI: 0-3, or -1 if it can't be had
static int android_rotation(void) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return -1;
    if ((*env)->PushLocalFrame(env, 8) != 0) {
        (*env)->ExceptionClear(env);
        return -1;
    }
    int r = -1;
    jobject act = (jobject)SDL_GetAndroidActivity();  // (a local reference: the frame's pop releases it)
    jobject wm = NULL, disp = NULL;
    jmethodID m = NULL;
    if (act) m = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, act), "getWindowManager", "()Landroid/view/WindowManager;");
    if (m && !(*env)->ExceptionCheck(env)) wm = (*env)->CallObjectMethod(env, act, m);
    m = NULL;
    if (wm && !(*env)->ExceptionCheck(env)) m = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, wm), "getDefaultDisplay", "()Landroid/view/Display;");
    if (m && !(*env)->ExceptionCheck(env)) disp = (*env)->CallObjectMethod(env, wm, m);
    m = NULL;
    if (disp && !(*env)->ExceptionCheck(env)) m = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, disp), "getRotation", "()I");
    if (m && !(*env)->ExceptionCheck(env)) r = (*env)->CallIntMethod(env, disp, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env), r = -1;
    (*env)->PopLocalFrame(env, NULL);
    return r >= 0 && r <= 3 ? r : -1;
}

static bool is_switch1_nintendo_pid(uint16_t product) {
    return product == 0x2006 || product == 0x2007 || product == 0x2008 ||
           product == 0x2009 || product == 0x200e;
}
#endif

// The screen's turn from the display's natural orientation, in quarter turns counterclockwise (Display.getRotation()):
// the turn that takes the sensors' axes - which follow the natural orientation - onto the screen's. Not SDL's display
// orientation: SDL counts a quarter turn more on a display whose natural orientation is landscape (SDL_android.c
// onNativeRotationChanged), and takes that natural orientation once, in SDLActivity.onCreate - the activity lives
// through folding and unfolding (configChanges). On a foldable's inner screen, landscape by nature (Galaxy Z Fold 8,
// Pixel Fold), rotation 0 read as LANDSCAPE and turned the motion a quarter too far: turning the phone tilted the aim.
// Asked of Android at most every 250 ms (folding changes it with no event here); SDL's orientation only without JNI.
static int screen_turns(void) {
    static int turns = -1;
    static uint64_t asked_ns;
    uint64_t now = SDL_GetTicksNS();
    if (turns >= 0 && now - asked_ns < 250000000ull) return turns;
    asked_ns = now;
    int r = -1;
#ifdef __ANDROID__
    r = android_rotation();
#endif
    if (r < 0) {
        SDL_DisplayID d = SDL_GetPrimaryDisplay();
        SDL_DisplayOrientation o = SDL_GetCurrentDisplayOrientation(d);
        r = o == SDL_ORIENTATION_LANDSCAPE ? 1 : o == SDL_ORIENTATION_PORTRAIT_FLIPPED ? 2 : o == SDL_ORIENTATION_LANDSCAPE_FLIPPED ? 3 : 0;
        if (SDL_GetNaturalDisplayOrientation(d) == SDL_ORIENTATION_LANDSCAPE) r = (r + 3) % 4;
    }
    if (r != turns) LOG("[motion] the screen is turned %d degrees from its natural orientation\n", r * 90);
    turns = r;
    return turns;
}

// The phone's sensors (device axes, as the display's natural orientation) on the screen as it's turned now
static void to_screen_turns(const float d[3], int turns, float s[3]) {
    switch (turns) {
    case 1: s[0] = -d[1], s[1] = d[0]; break;   // (a quarter counterclockwise: a portrait phone in landscape, its right side up)
    case 3: s[0] = d[1], s[1] = -d[0]; break;   // (a quarter clockwise: its left side up)
    case 2: s[0] = -d[0], s[1] = -d[1]; break;
    default: s[0] = d[0], s[1] = d[1]; break;   // (as by nature: a foldable's landscape inner screen held normally too)
    }
    s[2] = d[2];
}

// SDL's frame (rad/s or m/s^2) in: angular velocity -> a sample, acceleration -> the way down
// phone: the phone's own sensors, rotated onto the screen after raw bias subtraction so 180-deg flips keep the
// hardware MEMS bias intact. Held upright they turn as a controller lying flat would - the Switch's axes (x, -z, y)
// of the screen's; with Motion like the Switch's screen (#211, #247: "tilting turns the aim, turning tilts it")
// they are the screen's own, as the console's in handheld mode.
static void feed_phone(SDL_SensorType type, const float d[3], int turns, uint64_t sensor_ts, uint64_t host_ts) {
    pthread_mutex_lock(&m_lock);
    if (!vec3_finite(d)) {
        pthread_mutex_unlock(&m_lock);
        return;
    }
    if (m_last_phone != 1) {
        if (m_last_phone >= 0) q_set = false;
        m_phone_gyro_ts = 0;
        m_phone_have_accel = false;
        bias_reset_transient(&m_phone_bias);
        m_last_phone = 1;
    }
    bool flat = g_settings.motion_flat;
    if (m_last_flat != (int)flat) {
        if (m_last_flat >= 0) q_set = false, m_phone_have_accel = false;
        m_last_flat = (int)flat;
    }
    if (type == SDL_SENSOR_ACCEL) {
        for (int i = 0; i < 3; i++) m_phone_raw_accel[i] = d[i] / 9.80665f;
        m_phone_have_accel = true;
        float s[3];
        to_screen_turns(d, turns, s);
        if (flat) m_phone_accel[0] = -s[0] / 9.80665f, m_phone_accel[1] = -s[1] / 9.80665f, m_phone_accel[2] = -s[2] / 9.80665f;
        else m_phone_accel[0] = -s[0] / 9.80665f, m_phone_accel[1] = s[2] / 9.80665f, m_phone_accel[2] = -s[1] / 9.80665f;
        memcpy(m_last_accel, m_phone_accel, sizeof m_last_accel);
        if (!q_set) level_from(m_phone_accel);
    } else if (type == SDL_SENSOR_GYRO) {
        uint64_t ts = sensor_ts ? sensor_ts : host_ts;
        float dt = m_phone_gyro_ts && ts > m_phone_gyro_ts ? (float)((ts - m_phone_gyro_ts) / 1e9) : 0.005f;
        if (!(dt > 0.0f && dt <= 0.1f)) dt = 0.005f;  // (after a pause)
        m_phone_gyro_ts = ts;
        // Estimate/subtract phone bias in raw device frame before to_screen_turns() so screen rotation never flips sign.
        // LARECOMP_NO_GYRO_BIAS=1: off.
        float raw_corr[3] = {d[0], d[1], d[2]};
        if (!env_enabled(ENV("LARECOMP_NO_GYRO_BIAS"))) {
            bias_update(&m_phone_bias, d, m_phone_raw_accel, m_phone_have_accel, host_ts, dt, true);
            for (int i = 0; i < 3; i++) raw_corr[i] -= m_phone_bias.bias[i];
        }
        float g[3];
        to_screen_turns(raw_corr, turns, g);
        const float w[3] = {g[0], flat ? g[1] : -g[2], flat ? g[2] : g[1]};
        add_sample(w, m_phone_accel, m_phone_have_accel, dt);
        m_live_ns = SDL_GetTicksNS();
    }
    pthread_mutex_unlock(&m_lock);
}

// Feed controller sample with m_lock held.
static void feed_pad_locked(MotionPad *p, SDL_SensorType type, const float pd[3], uint64_t sensor_ts, uint64_t host_ts) {
    if (!vec3_finite(pd)) return;
    if (m_last_phone != 0) {
        if (m_last_phone >= 0) q_set = false;
        p->clock.has_prev = false;
        p->clock.win_hw = p->clock.win_host = 0;
        p->clock.win_n = 0;
        bias_reset_transient(&p->bias);
        p->have_accel = false;
        m_last_phone = 0;
    }
    if (type == SDL_SENSOR_ACCEL) {
        p->accel[0] = -pd[0] / 9.80665f, p->accel[1] = pd[2] / 9.80665f, p->accel[2] = -pd[1] / 9.80665f;
        p->have_accel = true;
        memcpy(m_last_accel, p->accel, sizeof m_last_accel);
        if (!q_set) level_from(p->accel);
    } else if (type == SDL_SENSOR_GYRO) {
        float dt;
        if ((p->nintendo_android || env_enabled(ENV("LARECOMP_CLOCK_SCALE"))) &&
            !env_enabled(ENV("LARECOMP_NO_CLOCK_SCALE"))) {
            dt = pad_clock_dt(&p->clock, sensor_ts, host_ts);
        } else {
            uint64_t ts = sensor_ts ? sensor_ts : host_ts;
            dt = p->clock.has_prev && p->clock.prev_hw && ts > p->clock.prev_hw
                     ? (float)((ts - p->clock.prev_hw) / 1e9) : 0.005f;
            p->clock.prev_hw = ts;
            p->clock.prev_host = host_ts;
            p->clock.has_prev = true;
            if (!(dt > 0.0f && dt <= 0.1f)) dt = 0.005f;
        }
        // Per-pad resting bias subtraction. LARECOMP_NO_GYRO_BIAS=1: off.
        float g[3] = {pd[0], pd[1], pd[2]};
        if (!env_enabled(ENV("LARECOMP_NO_GYRO_BIAS"))) {
            bias_update(&p->bias, pd, p->accel, p->have_accel, host_ts, dt, false);
            for (int i = 0; i < 3; i++) g[i] -= p->bias.bias[i];
        }
        const float w[3] = {g[0], -g[2], g[1]};
        add_sample(w, p->accel, p->have_accel, dt);
        m_live_ns = SDL_GetTicksNS();
    }
}

// (the event thread: runtime/hle/input.c) The phone's gyroscope and accelerometer, once the setting is on
void motion_open(void) {
    static bool opened;
    if (opened || !g_settings.motion) return;
    opened = true;
    if (!SDL_InitSubSystem(SDL_INIT_SENSOR)) { LOG("[motion] sensors: %s\n", SDL_GetError()); return; }
    int n = 0;
    SDL_SensorID *ids = SDL_GetSensors(&n);
    bool gyro = false, accel = false;
    for (int i = 0; i < n; i++) {
        SDL_SensorType t = SDL_GetSensorTypeForID(ids[i]);
        if ((t == SDL_SENSOR_GYRO && !gyro) || (t == SDL_SENSOR_ACCEL && !accel)) {
            if (!SDL_OpenSensor(ids[i])) continue;
            if (t == SDL_SENSOR_GYRO) gyro = true;
            else accel = true;
            LOG("[motion] %s: %s\n", t == SDL_SENSOR_GYRO ? "gyroscope" : "accelerometer", SDL_GetSensorNameForID(ids[i]));
        }
    }
    SDL_free(ids);
    if (!gyro) LOG("[motion] no gyroscope among %d sensors: motion controls only from a controller that has one\n", n);
}

static bool parse_pad_gyro_env(int axis[3], int sign[3]) {
    static int cached = -1;
    static int c_axis[3], c_sign[3];
    if (cached < 0) {
        const char *raw = ENV("LARECOMP_PAD_GYRO");
        int a[3] = {0}, s[3] = {1, 1, 1}, n = 0;
        const char *p = raw;
        while (p && *p && n < 3) {
            int neg = 0;
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            if (*p == '-') neg = 1, p++;
            else if (*p == '+') p++;
            if (*p < 'x' || *p > 'z') { n = -1; break; }
            a[n] = *p - 'x', s[n] = neg ? -1 : 1, n++, p++;
        }
        while (p && (*p == ' ' || *p == '\t')) p++;
        if (n == 3 && (!p || !*p) && a[0] != a[1] && a[1] != a[2] && a[0] != a[2]) {
            for (int i = 0; i < 3; i++) c_axis[i] = a[i], c_sign[i] = s[i];
            cached = 1;
            LOG("[motion] the controller's axes taken as %s (LARECOMP_PAD_GYRO)\n", raw);
        } else {
            cached = 0;
            if (raw && *raw) LOG("[motion] LARECOMP_PAD_GYRO=%s not understood (three of x, y, z, each with an optional -)\n", raw);
        }
    }
    if (cached && axis && sign)
        for (int i = 0; i < 3; i++) axis[i] = c_axis[i], sign[i] = c_sign[i];
    return cached != 0;
}

static MotionPad *pad_find_locked(SDL_JoystickID id) {
    if (!id) return NULL;
    for (int i = 0; i < MAX_MOTION_PADS; i++)
        if (m_pads[i].id == id) return &m_pads[i];
    return NULL;
}

// A controller's sensors in another axis order: LARECOMP_PAD_GYRO=a,b,c (each x, y or z, with an optional -), the axes
// SDL reports for the controller's x, y and z - e.g. "z,y,x" swaps pitch and roll (#338). Gyroscope and accelerometer
// alike. Android's Switch 1 hid-nintendo path omits SDL's (-y,z,-x) Nintendo->SDL conversion, so normalize it first
// before applying Settings > Controls > Controller gyro axes; a valid LARECOMP_PAD_GYRO still overrides raw axes directly.
static void pad_axes_locked(MotionPad *p, const float d[3], float out[3]) {
    // (Settings > Controls > Controller gyro axes, settings.h pad_gyro_axes, #511 #521: used when the variable isn't set)
    static const char *const setting_order[4] = {"x,y,z", "y,x,z", "z,y,x", "x,z,y"};
    int env_axis[3], env_sign[3];
    if (parse_pad_gyro_env(env_axis, env_sign)) {
        for (int i = 0; i < 3; i++) out[i] = env_sign[i] * d[env_axis[i]];
        return;
    }
    int want = g_settings.pad_gyro_axes;
    if (want < 0 || want > 3) want = 0;
    if (m_pad_axes_seen >= 0 && want != m_pad_axes_seen) {
        for (int i = 0; i < MAX_MOTION_PADS; i++) {
            memset(&m_pads[i].bias, 0, sizeof m_pads[i].bias);
            m_pads[i].have_accel = false;
        }
        if (m_last_phone == 0) m_last_phone = -1, q_set = false;
    }
    if (want != m_pad_axes_seen) {
        if (want || m_pad_axes_seen >= 0) LOG("[motion] the controller's axes taken as %s (Settings)\n", setting_order[want]);
        m_pad_axes_seen = want;
    }
    bool nintendo = p->nintendo_android;
    float norm[3] = {nintendo ? -d[1] : d[0], nintendo ? d[2] : d[1], nintendo ? -d[0] : d[2]};
    const char *o = setting_order[want];
    for (int i = 0; i < 3; i++) out[i] = norm[o[i * 2] - 'x'];
}

// A controller that has a gyroscope: its own sensors, in place of the phone's while it sends them
void motion_pad_opened(SDL_Gamepad *g) {
    if (!SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO)) return;
    SDL_JoystickID id = SDL_GetGamepadID(g);
    uint16_t vendor = SDL_GetGamepadVendor(g);
    uint16_t product = SDL_GetGamepadProduct(g);
    const char *path = SDL_GetGamepadPath(g);
    const char *pad_name = SDL_GetGamepadName(g);
    if (!pad_name) pad_name = "(unnamed)";
    LOG("[motion] controller %s: vendor %04x product %04x, type %d, path %s, sensor rate %.0f Hz\n",
        pad_name, vendor, product, (int)SDL_GetGamepadType(g),
        path ? path : "-", SDL_GetGamepadSensorDataRate(g, SDL_SENSOR_GYRO));
    bool nintendo_android = false;
#ifdef __ANDROID__
    // On Android, Switch 1 kernel hid-nintendo controllers have path == NULL (SensorManager) and miss SDL's (-y,z,-x) remap (#338).
    nintendo_android = vendor == 0x057e && !path && is_switch1_nintendo_pid(product);
#endif
    bool was_active = false;
    bool has_env_override = false;
    pthread_mutex_lock(&m_lock);
    if (id) {
        int slot = -1, fallback = -1;
        for (int i = 0; i < MAX_MOTION_PADS; i++) {
            if (m_pads[i].id == id) { slot = i; break; }
            if (slot < 0 && !m_pads[i].id) slot = i;
            if (fallback < 0 && m_pads[i].id != m_active_pad_id) fallback = i;
        }
        if (slot < 0) slot = fallback >= 0 ? fallback : 0;
        memset(&m_pads[slot], 0, sizeof m_pads[slot]);
        m_pads[slot].id = id;
        m_pads[slot].nintendo_android = nintendo_android;
        if (m_active_pad_id == id) {
            was_active = true;
            m_active_pad_id = 0, m_active_pad_gyro_ns = 0, m_active_pad_turn_ns = 0;
            if (m_last_phone == 0) m_last_phone = -1, q_set = false;
        }
    }
    if (nintendo_android) has_env_override = parse_pad_gyro_env(NULL, NULL);
    pthread_mutex_unlock(&m_lock);
    if (was_active) pad_until_ns = 0;  // event thread only
    int prev_axes = 0;
    if (nintendo_android && settings_migrate_pad_gyro_axes(&prev_axes)) {
        LOG("[motion] %s: reset legacy Controller gyro axes (%d -> 0, As reported; Nintendo Android axes are now normalized)\n",
            pad_name, prev_axes);
    }
    // (Motion sensor: Phone - the controller's left off: a USB-C controller the phone sits in, its sensors on, and the
    // phone's aim stopped after a few shots until the controller was plugged in again, #78)
    if (g_settings.motion_source == 1) { LOG("[motion] %s: its gyroscope left off (Phone)\n", pad_name); return; }
    SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
    if (SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL)) SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
    LOG("[motion] %s: its gyroscope\n", pad_name);
    if (nintendo_android && !has_env_override)
        LOG("[motion] %s: Nintendo Android IMU frame (-y,z,-x; override with LARECOMP_PAD_GYRO)\n", pad_name);
}

// Removes a closed controller slot cleanly without disturbing the phone's attitude or other open controllers.
void motion_pad_closed(SDL_JoystickID id) {
    if (!id) return;
    bool was_active = false;
    pthread_mutex_lock(&m_lock);
    MotionPad *p = pad_find_locked(id);
    if (!p) {
        pthread_mutex_unlock(&m_lock);
        return;
    }
    memset(p, 0, sizeof *p);
    if (id == m_active_pad_id) {
        was_active = true;
        m_active_pad_id = 0;
        m_active_pad_gyro_ns = 0;
        m_active_pad_turn_ns = 0;
        if (m_last_phone == 0) m_last_phone = -1, q_set = false;
    }
    pthread_mutex_unlock(&m_lock);
    if (was_active) pad_until_ns = 0;  // event thread only
}

static bool pad_auto_alive_locked(MotionPad *p, SDL_SensorType type, const float raw[3], uint64_t now, bool *clear_pad_until) {
    // A controller's sensors replace the phone's only once its gyroscope is alive: some report one that never is (a
    // USB-C controller the phone sits in, GameSir X5 Lite: issue #78), and its zeros held the phone's off. Alive is
    // readings that change from one to the next (decrementing slowly on quantized repeats so coarse-LSB pads like
    // 8BitDo Pro 2 can still wake Auto mode), capped at 20 so sub-threshold noise (#115) cannot latch a frozen sensor.
    if (type == SDL_SENSOR_GYRO) {
        p->last_gyro_ns = now;
        bool zero = raw[0] == 0.0f && raw[1] == 0.0f && raw[2] == 0.0f;
        bool alive = !zero && (raw[0] != p->last_gyro[0] || raw[1] != p->last_gyro[1] || raw[2] != p->last_gyro[2]);
        memcpy(p->last_gyro, raw, sizeof p->last_gyro);
        bool was_turned = p->turned;
        if (!p->turned && raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2] > 0.25f) {
            p->turned = true;
            LOG("[motion] the controller's gyroscope turned (%.2f %.2f %.2f rad/s)\n", raw[0], raw[1], raw[2]);
        }
        if (!p->live) {
            // Cap at 19 before turning so a single glitch after #115 noise cannot wake, and 20 overall for quick decay.
            int cap = was_turned ? 20 : 19;
            if (alive) { if (p->changing < cap) p->changing++; }
            else if (zero) p->changing = 0;
            else if (p->changing > 0) p->changing--;
            if (p->changing >= 20 && p->turned) {
                p->live = true, p->dead_ns = 0;
                if (!m_active_pad_id)
                    LOG("[motion] the controller's gyroscope is alive: its sensors in place of the phone's\n");
            }
        } else if (alive) {
            p->dead_ns = 0;
        } else if (!p->dead_ns) {
            p->dead_ns = now;
        } else if (now > p->dead_ns && now - p->dead_ns > 2000000000ull) {
            p->live = false, p->changing = 0, p->dead_ns = 0;
            if (p->id == m_active_pad_id) *clear_pad_until = true;
            LOG("[motion] the controller's gyroscope reads zero or the same: the phone's sensors again\n");
        }
    } else if (type == SDL_SENSOR_ACCEL) {
        // Ignore orphan accel if this pad's gyro stopped delivering events so it cannot stall Auto fallback to phone.
        if (!p->last_gyro_ns || (now > p->last_gyro_ns && now - p->last_gyro_ns > 500000000ull)) {
            if (p->live && p->last_gyro_ns && now > p->last_gyro_ns && now - p->last_gyro_ns > 2000000000ull) {
                p->live = false, p->changing = 0, p->dead_ns = 0;
                if (p->id == m_active_pad_id) *clear_pad_until = true;
                LOG("[motion] the controller's gyroscope reads zero or the same: the phone's sensors again\n");
            }
            return false;
        }
    }
    return p->live;
}

void motion_event(const SDL_Event *e) {
    bool motion = g_settings.motion;
    if (!motion || ENV("LARECOMP_MOTION_TEST")) {
        if (!motion) pad_until_ns = 0;
        return;
    }
    // Settings > Controls > Motion sensor (settings.h motion_source): 1 the phone's alone - for a controller that reports
    // a gyroscope it hasn't got, whose noise passed for a live one and muted the phone's (GameSir X2 Type-C, #115); 2 a
    // controller's alone, taken as it comes; 0 the automatic switch below.
    int source = g_settings.motion_source;
    if (source != 0) pad_until_ns = 0;
    if (e->type == SDL_EVENT_SENSOR_UPDATE) {
        if (source == 2 || (source == 0 && SDL_GetTicksNS() < pad_until_ns)) return;
        if (!vec3_finite(e->sensor.data)) return;
        int turns = screen_turns();
        SDL_SensorType type = SDL_GetSensorTypeForID(e->sensor.which);
        uint64_t now = SDL_GetTicksNS();
        {   // Turning the phone round (landscape to the other landscape): the screen's axes flip part way, when Android
            // turns the game, and what the sensors read on the way (the hand's tilt, the way down sweeping round) went into
            // the attitude in one frame before and the other after - the aim further up with each turn. Held against the
            // screen, the phone is as it was before the half turn: while it turns fast about the screen's normal (over ~115
            // deg/s) and for 0.4 s after the screen turned, its readings are left out - the attitude kept as it was.
            static uint64_t quiet_until;
            static int last_turns = -1;
            if (last_turns >= 0 && turns != last_turns) quiet_until = now + 400000000ull;
            last_turns = turns;
            // z is invariant under to_screen_turns(), so raw data[2] measures turn rate about the screen normal.
            if (type == SDL_SENSOR_GYRO && fabsf(e->sensor.data[2]) > 2.0f) quiet_until = now + 250000000ull;
            if (now < quiet_until) return;
        }
        uint64_t host_ts = e->sensor.timestamp ? e->sensor.timestamp : now;
        uint64_t sensor_ts = e->sensor.sensor_timestamp ? e->sensor.sensor_timestamp : host_ts;
        feed_phone(type, e->sensor.data, turns, sensor_ts, host_ts);
    } else if (e->type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) {
        if (source == 1 || !vec3_finite(e->gsensor.data)) return;
        SDL_JoystickID id = e->gsensor.which;
        SDL_SensorType type = (SDL_SensorType)e->gsensor.sensor;
        uint64_t now = SDL_GetTicksNS();
        uint64_t host_ts = e->gsensor.timestamp ? e->gsensor.timestamp : now;
        const float *raw = e->gsensor.data;
        bool extend_pad_until = false, clear_pad_until = false;

        pthread_mutex_lock(&m_lock);
        MotionPad *p = pad_find_locked(id);
        if (!p) {
            pthread_mutex_unlock(&m_lock);
            return;
        }
        if (source == 0 && !pad_auto_alive_locked(p, type, raw, now, &clear_pad_until)) {
            bool drop_active = clear_pad_until || (!p->live && p->id == m_active_pad_id);
            if (drop_active) {
                p->clock.has_prev = false;
                p->clock.win_hw = p->clock.win_host = 0;
                p->clock.win_n = 0;
                bias_reset_transient(&p->bias);
                p->have_accel = false;
                m_active_pad_id = 0, m_active_pad_gyro_ns = 0, m_active_pad_turn_ns = 0;
                if (m_last_phone == 0) m_last_phone = -1, q_set = false;
            }
            pthread_mutex_unlock(&m_lock);
            if (drop_active) pad_until_ns = 0;  // event thread only
            return;
        }
        MotionPad *active = pad_find_locked(m_active_pad_id);
        bool p_turning = type == SDL_SENSOR_GYRO && raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2] > 0.25f;
        if (!active || active->id != p->id) {
            // Arbitrate between multi-open controllers (e.g. Joy-Cons or handheld+BT) so resting pads never reset state.
            bool can_take = !active || (source == 0 && !active->live) ||
                            (now > m_active_pad_gyro_ns && now - m_active_pad_gyro_ns > 500000000ull) ||
                            (p_turning && (!m_active_pad_turn_ns ||
                             (now > m_active_pad_turn_ns && now - m_active_pad_turn_ns > 500000000ull)));
            if (!can_take) {
                pthread_mutex_unlock(&m_lock);
                return;
            }
            p->clock.has_prev = false;
            p->clock.win_hw = p->clock.win_host = 0;
            p->clock.win_n = 0;
            bias_reset_transient(&p->bias);
            p->have_accel = false;
            if (m_last_phone == 0) m_last_phone = -1, q_set = false;
            m_active_pad_id = p->id;
            m_active_pad_gyro_ns = now;
            m_active_pad_turn_ns = p_turning ? now : 0;
        }
        if (type == SDL_SENSOR_GYRO) {
            m_active_pad_gyro_ns = now;
            if (p_turning) m_active_pad_turn_ns = now;
            if (source == 0) extend_pad_until = true;
        }
        float pd[3];
        pad_axes_locked(p, raw, pd);
        feed_pad_locked(p, type, pd, e->gsensor.sensor_timestamp, host_ts);
        pthread_mutex_unlock(&m_lock);

        if (extend_pad_until) pad_until_ns = now + 500000000ull;  // event thread only
    }
}

// LARECOMP_MOTION_TEST=gx,gy,gz[:from:frames]: that angular velocity (turns per second, the Switch's frame) in place
// of the sensors', from that presented frame on for that many (all along without them)
static bool test_motion(float w[3]) {
    const char *e = ENV("LARECOMP_MOTION_TEST");
    if (!e) return false;
    float g[3] = {0};
    unsigned long from = 0, len = ~0ul;
    if (sscanf(e, "%f,%f,%f:%lu:%lu", &g[0], &g[1], &g[2], &from, &len) < 3) return false;
    extern uint64_t renderer_present_count(void);
    uint64_t f = renderer_present_count();
    for (int i = 0; i < 3; i++) w[i] = f >= from && f - from < len ? g[i] * TURN : 0;
    return true;
}

// A read with nothing new from the sensors (none, the setting off, or a test): one sample of its own
static void own_sample(uint64_t now, bool motion, int source) {
    float dt = m_rest_ns && now > m_rest_ns ? (float)((now - m_rest_ns) / 1e9) : 1 / 30.0f;
    if (!(dt > 0.0f && dt <= 0.1f)) dt = 1 / 30.0f;
    m_rest_ns = now;
    float w[3] = {0, 0, 0};
    bool testing = test_motion(w);
    if (testing) {
        add_sample(w, m_last_accel, true, dt);
        return;
    }
    if (!motion) {  // (off: lying still, the right way up; clear cached accel/pad state for clean re-enable)
        q[0] = 1, q[1] = q[2] = q[3] = 0, q_set = false;
        m_last_accel[0] = m_last_accel[1] = 0, m_last_accel[2] = -1;
        m_phone_have_accel = false;
        m_last_phone = -1;
        m_live_ns = 0;
        m_phone_gyro_ts = 0;
        bias_reset_transient(&m_phone_bias);
        m_active_pad_id = 0, m_active_pad_gyro_ns = 0, m_active_pad_turn_ns = 0;
        for (int i = 0; i < MAX_MOTION_PADS; i++) {
            m_pads[i].have_accel = false;
            m_pads[i].clock.has_prev = false;
            m_pads[i].clock.win_hw = m_pads[i].clock.win_host = 0;
            m_pads[i].clock.win_n = 0;
            m_pads[i].live = false;
            m_pads[i].changing = 0;
            m_pads[i].dead_ns = 0;
            m_pads[i].last_gyro_ns = 0;
            bias_reset_transient(&m_pads[i].bias);
        }
        add_sample(w, m_last_accel, false, dt);
        return;
    }
    const float *accel = m_last_accel;
    bool have_accel = false;
    if (m_last_phone == 1 && m_phone_have_accel && source != 2) {
        accel = m_phone_accel;
        have_accel = true;
    } else if (m_last_phone == 0 && source != 1) {
        MotionPad *ap = pad_find_locked(m_active_pad_id);
        if (ap && ap->have_accel && (source != 0 || ap->live)) {
            accel = ap->accel;
            have_accel = true;
        }
    }
    add_sample(w, accel, have_accel, dt);
}

// int GetSixAxisSensorHandles(SixAxisSensorHandle *out, int count, const NpadIdType &, NpadStyleSet): one sensor, two
// for a pair of Joy-Con (both read the same here)
HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
         hid_sixaxis_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)(int32_t)c->x[1];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2], style = (uint32_t)c->x[3];
    int k = style & 4 ? 2 : 1;  // (JoyDual: left and right)
    if (n > k) n = k;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) out[i] = (uint32_t)i << 16 | (id & 0xFF) << 8 | 3;  // device, player, FullKey
    static int nlog;
    if (nlog++ < 4) LOG("[motion] sensor handles: player %#x style %#x -> %d\n", id, style, n);
    c->x[0] = (uint64_t)n;
}

HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_sixaxis_start) {
    static int nlog;
    if (nlog++ < 4) LOG("[motion] sensor %#x started\n", *(uint32_t *)(uintptr_t)c->x[0]);
}

HLE_FUNC("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE",
         hid_sixaxis_drift) { (void)c; }

// int GetSixAxisSensorStates(SixAxisSensorState *out, int count, const SixAxisSensorHandle &): the newest first
HLE_FUNC("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", hid_sixaxis_states) {
    SixAxisState *out = (SixAxisState *)(uintptr_t)c->x[0];
    int n = (int)(int32_t)c->x[1];
    pthread_mutex_lock(&m_lock);
    bool motion = g_settings.motion;
    int source = g_settings.motion_source;
    uint64_t now = SDL_GetTicksNS();
    if (!motion || ENV("LARECOMP_MOTION_TEST") || !m_live_ns ||
        (now > m_live_ns && now - m_live_ns > 250000000ull))
        own_sample(now, motion, source);
    {   // (the log: a sensor that stops sending while the game reads it - aiming that stops working, #78)
        static bool quiet;
        static int nlog;
        bool q_quiet = motion && m_live_ns && now > m_live_ns && now - m_live_ns > 1000000000ull;
        if (q_quiet != quiet && nlog < 40) {
            nlog++;
            LOG(q_quiet ? "[motion] no gyroscope readings for 1 s while the game reads the sensor (%s)\n"
                        : "[motion] gyroscope readings again (%s)\n",
                source == 1 ? "Phone" : source == 2 ? "Controller" : "Auto");
        }
        quiet = q_quiet;
    }
    if (n > ring_n) n = ring_n;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) out[i] = ring[(ring_head - i + RING) % RING];
    static uint64_t logged;
    if (ENV("LARECOMP_LOG_MOTION") && now >= logged && now - logged > 1000000000ull) {  // (once a second: the newest sample)
        logged = now;
        const SixAxisState *s = &ring[ring_head];
        LOG("[motion] #%lld dt %.1f ms accel %.2f %.2f %.2f gyro %.3f %.3f %.3f angle %.3f %.3f %.3f y-axis %.2f %.2f %.2f "
            "(%d of %d)\n", (long long)s->sampling_number, s->delta_time / 1e6, s->accel[0], s->accel[1], s->accel[2],
            s->gyro[0], s->gyro[1], s->gyro[2], s->angle[0], s->angle[1], s->angle[2], s->dir[1][0], s->dir[1][1],
            s->dir[1][2], n, (int)(int32_t)c->x[1]);
    }
    pthread_mutex_unlock(&m_lock);
    c->x[0] = (uint64_t)n;
}

RT_ON_RESET(motion_reset) {
    pthread_mutex_lock(&m_lock);
    ring_n = 0;
    q[0] = 1, q[1] = q[2] = q[3] = 0, q_set = false;
    m_last_accel[0] = m_last_accel[1] = 0, m_last_accel[2] = -1;
    m_live_ns = 0, m_rest_ns = 0, m_phone_gyro_ts = 0;
    m_phone_have_accel = false;
    m_last_flat = -1, m_last_phone = -1;
    m_active_pad_id = 0, m_active_pad_gyro_ns = 0, m_active_pad_turn_ns = 0;
    memset(m_angle, 0, sizeof m_angle);
    bias_reset_transient(&m_phone_bias);
    for (int i = 0; i < MAX_MOTION_PADS; i++) {
        memset(&m_pads[i].clock, 0, sizeof m_pads[i].clock);
        bias_reset_transient(&m_pads[i].bias);
        m_pads[i].have_accel = false;
        m_pads[i].dead_ns = 0;
        m_pads[i].last_gyro_ns = 0;
    }
    pthread_mutex_unlock(&m_lock);
    pad_until_ns = 0;  // event thread resets / RT_ON_RESET runs while runtime is quiescent
}
