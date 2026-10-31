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
#define KP 0.5f       // the acceleration's pull on the attitude (rad/s per unit of error)
#define TURN 6.2831853f
static pthread_mutex_t m_lock = PTHREAD_MUTEX_INITIALIZER;
static SixAxisState ring[RING];
static int ring_n, ring_head;            // samples kept, the newest's index
static int64_t m_sampling;
static float q[4] = {1, 0, 0, 0};        // w x y z: the sensor's attitude (sensor -> world)
static bool q_set;                       // (levelled by the first acceleration)
static float m_accel[3] = {0, 0, -1}, m_angle[3];
static uint64_t m_gyro_ts, m_live_ns, m_rest_ns;  // the last angular velocity's own time; host times
static uint64_t pad_until_ns;            // a controller's sensors are in use until then: the phone's wait

// q from the way down a (in G): no yaw - the rotation taking a to the world's (0, 0, -1)
static void level_from(const float a[3]) {
    float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (n < 0.5f) return;
    float ux = a[0] / n, uy = a[1] / n, uz = a[2] / n;
    float w = 1 - uz, x = -uy, y = ux;  // (1 + u.v, u x v) with v = (0, 0, -1)
    float m = sqrtf(w * w + x * x + y * y);
    if (m < 1e-4f) q[0] = 0, q[1] = 1, q[2] = 0, q[3] = 0;  // (upside down: half a turn about x)
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
    for (int i = 0; i < 4; i++) q[i] /= m;
}

// A sample: angular velocity w (rad/s, the Switch's frame) over dt seconds (m_lock held)
static void add_sample(const float w[3], float dt) {
    if (!q_set) level_from(m_accel);
    float wc[3] = {w[0], w[1], w[2]};
    fuse(wc, m_accel, dt);
    ring_head = (ring_head + 1) % RING;
    if (ring_n < RING) ring_n++;
    SixAxisState *s = &ring[ring_head];
    memset(s, 0, sizeof *s);
    s->delta_time = (int64_t)(dt * 1e9f);
    s->sampling_number = ++m_sampling;
    for (int i = 0; i < 3; i++) {
        s->accel[i] = m_accel[i];
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

// SDL's frame (rad/s or m/s^2) in: angular velocity -> a sample, acceleration -> the way down
static void feed(SDL_SensorType type, const float d[3], uint64_t ts) {
    pthread_mutex_lock(&m_lock);
    if (type == SDL_SENSOR_ACCEL) {
        m_accel[0] = -d[0] / 9.80665f, m_accel[1] = d[2] / 9.80665f, m_accel[2] = -d[1] / 9.80665f;
        if (!q_set) level_from(m_accel);
    } else if (type == SDL_SENSOR_GYRO) {
        float dt = m_gyro_ts && ts > m_gyro_ts ? (float)((ts - m_gyro_ts) / 1e9) : 0.005f;
        if (dt > 0.1f) dt = 0.005f;  // (after a pause)
        m_gyro_ts = ts;
        // The gyroscope's bias at rest, learned and taken off: a phone's reads a little turn while it lies still, and the
        // aim drifted (SavageWindRecomp / MacNativeGames: learn it while still). Still: every axis within ~3.4 deg/s of
        // the bias for 1.5 s; then the bias follows slowly. LARECOMP_NO_GYRO_BIAS=1: off.
        static int bias_on = -1;
        static float bias[3];
        static uint64_t still_since;
        if (bias_on < 0) bias_on = getenv("LARECOMP_NO_GYRO_BIAS") == NULL;
        float g[3] = {d[0], d[1], d[2]};
        if (bias_on) {
            float dev = 0;
            for (int i = 0; i < 3; i++) dev = fmaxf(dev, fabsf(d[i] - bias[i]));
            if (dev < 0.06f) {
                if (!still_since) still_since = ts;
                else if (ts - still_since > 1500000000ull) {
                    for (int i = 0; i < 3; i++) bias[i] += (d[i] - bias[i]) * 0.02f;
                    static bool told;
                    if (!told && ts - still_since > 4000000000ull) {
                        told = true;
                        LOG("[motion] gyroscope at rest: bias %.4f %.4f %.4f rad/s taken off\n", bias[0], bias[1], bias[2]);
                    }
                }
            } else {
                still_since = 0;
            }
            for (int i = 0; i < 3; i++) g[i] -= bias[i];
        }
        const float w[3] = {g[0], -g[2], g[1]};
        add_sample(w, dt);
        m_live_ns = SDL_GetTicksNS();
    }
    pthread_mutex_unlock(&m_lock);
}

// The phone's sensors (device axes, portrait) on the screen as it's turned now
static void to_screen(const float d[3], float s[3]) {
    switch (SDL_GetCurrentDisplayOrientation(SDL_GetPrimaryDisplay())) {
    case SDL_ORIENTATION_LANDSCAPE: s[0] = -d[1], s[1] = d[0]; break;          // (its right side up)
    case SDL_ORIENTATION_LANDSCAPE_FLIPPED: s[0] = d[1], s[1] = -d[0]; break;  // (its left side up)
    case SDL_ORIENTATION_PORTRAIT_FLIPPED: s[0] = -d[0], s[1] = -d[1]; break;
    default: s[0] = d[0], s[1] = d[1]; break;
    }
    s[2] = d[2];
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

// A controller that has a gyroscope: its own sensors, in place of the phone's while it sends them
void motion_pad_opened(SDL_Gamepad *g) {
    if (!SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO)) return;
    SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
    if (SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL)) SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
    LOG("[motion] %s: its gyroscope\n", SDL_GetGamepadName(g));
}

void motion_event(const SDL_Event *e) {
    if (!g_settings.motion || ENV("LARECOMP_MOTION_TEST")) return;
    // Settings > Controls > Motion sensor (settings.h motion_source): 1 the phone's alone - for a controller that reports
    // a gyroscope it hasn't got, whose noise passed for a live one and muted the phone's (GameSir X2 Type-C, #115); 2 a
    // controller's alone, taken as it comes; 0 the automatic switch below.
    int source = g_settings.motion_source;
    if (e->type == SDL_EVENT_SENSOR_UPDATE) {
        if (source == 2 || (source == 0 && SDL_GetTicksNS() < pad_until_ns)) return;
        float s[3];
        to_screen(e->sensor.data, s);
        feed(SDL_GetSensorTypeForID(e->sensor.which), s, e->sensor.sensor_timestamp ? e->sensor.sensor_timestamp : SDL_GetTicksNS());
    } else if (e->type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) {
        if (source == 1) return;
        if (source == 2) {
            feed((SDL_SensorType)e->gsensor.sensor, e->gsensor.data,
                 e->gsensor.sensor_timestamp ? e->gsensor.sensor_timestamp : SDL_GetTicksNS());
            return;
        }
        // A controller's sensors replace the phone's only once its gyroscope is alive: some report one that never is (a
        // USB-C controller the phone sits in, GameSir X5 Lite: issue #78), and its zeros held the phone's off. Alive is
        // readings that change from one to the next - a real gyroscope's noise does even lying still - 20 in a row:
        // one stray reading switched to that controller for good (0.2.3), and a speed threshold (0.2.4) never let a
        // slow aim or a resting controller take over (#78, #115). Back to the phone's after 2 s of readings that are
        // zero or repeat: a dead or frozen sensor.
        static bool pad_live;
        static int changing;
        static float last[3];
        static uint64_t dead_ns;
        if ((SDL_SensorType)e->gsensor.sensor == SDL_SENSOR_GYRO) {
            const float *g = e->gsensor.data;
            bool alive = (g[0] != 0.0f || g[1] != 0.0f || g[2] != 0.0f) &&
                         (g[0] != last[0] || g[1] != last[1] || g[2] != last[2]);
            memcpy(last, g, sizeof last);
            uint64_t now = SDL_GetTicksNS();
            if (!pad_live) {
                changing = alive ? changing + 1 : 0;
                if (changing >= 20) {
                    pad_live = true, dead_ns = 0;
                    LOG("[motion] the controller's gyroscope is alive: its sensors in place of the phone's\n");
                }
            } else if (alive) dead_ns = 0;
            else if (!dead_ns) dead_ns = now;
            else if (now - dead_ns > 2000000000) {
                pad_live = false, changing = 0, pad_until_ns = 0;
                LOG("[motion] the controller's gyroscope reads zero or the same: the phone's sensors again\n");
            }
        }
        if (!pad_live) return;
        pad_until_ns = SDL_GetTicksNS() + 500000000;
        feed((SDL_SensorType)e->gsensor.sensor, e->gsensor.data,
             e->gsensor.sensor_timestamp ? e->gsensor.sensor_timestamp : SDL_GetTicksNS());
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
static void own_sample(uint64_t now) {
    float dt = m_rest_ns && now > m_rest_ns ? (float)((now - m_rest_ns) / 1e9) : 1 / 30.0f;
    if (dt > 0.1f) dt = 1 / 30.0f;
    m_rest_ns = now;
    float w[3] = {0, 0, 0};
    if (!test_motion(w) && !g_settings.motion) {  // (off: lying still, the right way up)
        q[0] = 1, q[1] = q[2] = q[3] = 0, q_set = true;
        m_accel[0] = m_accel[1] = 0, m_accel[2] = -1;
    }
    add_sample(w, dt);
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
    uint64_t now = SDL_GetTicksNS();
    pthread_mutex_lock(&m_lock);
    if (!g_settings.motion || ENV("LARECOMP_MOTION_TEST") || now - m_live_ns > 250000000) own_sample(now);
    if (n > ring_n) n = ring_n;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) out[i] = ring[(ring_head - i + RING) % RING];
    static uint64_t logged;
    if (ENV("LARECOMP_LOG_MOTION") && now - logged > 1000000000) {  // (once a second: the newest sample)
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
    ring_n = 0, q_set = false, m_rest_ns = 0;
    memset(m_angle, 0, sizeof m_angle);
    pthread_mutex_unlock(&m_lock);
}
