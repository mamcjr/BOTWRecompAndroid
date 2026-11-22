// Native Android regressions for runtime/hle/motion.c and pad_close/reset lifetimes.
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "../runtime/hle/motion.c"
#include "../runtime/settings.c"

// Minimal SDL3 & runtime stubs so motion.c and settings.c can be tested standalone without linking libSDL3.so.
struct SDL_Gamepad {
    SDL_JoystickID id;
    Uint16 vendor, product;
    SDL_GamepadType type;
    const char *name;
    const char *path;
    float rate;
    bool has_gyro, has_accel;
};

int g_game_version = 190;
bool bind_settings_load(const char *k, const char *v) { (void)k; (void)v; return false; }
void bind_settings_save(FILE *f) { (void)f; }
SDL_Locale **SDL_GetPreferredLocales(int *count) { if (count) *count = 0; return NULL; }

static uint64_t g_ticks_ns = 1000000000ull;
static SDL_DisplayOrientation g_display_orientation = SDL_ORIENTATION_LANDSCAPE;

uint64_t renderer_present_count(void) { return 0; }
Uint64 SDL_GetTicksNS(void) { return g_ticks_ns; }
const char *SDL_GetError(void) { return "stub"; }
bool SDL_InitSubSystem(SDL_InitFlags flags) { (void)flags; return true; }
SDL_SensorID *SDL_GetSensors(int *count) { if (count) *count = 0; return NULL; }
SDL_SensorType SDL_GetSensorTypeForID(SDL_SensorID id) { return id == 2 ? SDL_SENSOR_ACCEL : SDL_SENSOR_GYRO; }
SDL_Sensor *SDL_OpenSensor(SDL_SensorID id) { (void)id; return NULL; }
const char *SDL_GetSensorNameForID(SDL_SensorID id) { (void)id; return "stub"; }
void SDL_free(void *mem) { free(mem); }
SDL_DisplayID SDL_GetPrimaryDisplay(void) { return 1; }
SDL_DisplayOrientation SDL_GetCurrentDisplayOrientation(SDL_DisplayID d) { (void)d; return g_display_orientation; }
SDL_DisplayOrientation SDL_GetNaturalDisplayOrientation(SDL_DisplayID d) { (void)d; return SDL_ORIENTATION_PORTRAIT; }
#ifdef __ANDROID__
void *SDL_GetAndroidJNIEnv(void) { return NULL; }
void *SDL_GetAndroidActivity(void) { return NULL; }
#endif

SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *g) { return g ? g->id : 0; }
Uint16 SDL_GetGamepadVendor(SDL_Gamepad *g) { return g ? g->vendor : 0; }
Uint16 SDL_GetGamepadProduct(SDL_Gamepad *g) { return g ? g->product : 0; }
const char *SDL_GetGamepadPath(SDL_Gamepad *g) { return g ? g->path : NULL; }
const char *SDL_GetGamepadName(SDL_Gamepad *g) { return g && g->name ? g->name : "stub"; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *g) { return g ? g->type : SDL_GAMEPAD_TYPE_UNKNOWN; }
float SDL_GetGamepadSensorDataRate(SDL_Gamepad *g, SDL_SensorType t) { (void)t; return g ? g->rate : 0.0f; }
bool SDL_GamepadHasSensor(SDL_Gamepad *g, SDL_SensorType t) {
    if (!g) return false;
    return t == SDL_SENSOR_GYRO ? g->has_gyro : t == SDL_SENSOR_ACCEL ? g->has_accel : false;
}
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *g, SDL_SensorType t, bool en) { (void)g; (void)t; (void)en; return true; }

static SDL_Gamepad make_nintendo_android_pad(SDL_JoystickID id) {
    return (SDL_Gamepad){
        .id = id,
        .vendor = 0x057e,
        .product = 0x2009,
        .type = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,
        .name = "Nintendo Switch Pro Controller",
        .path = NULL,
        .rate = 0.0f,
        .has_gyro = true,
        .has_accel = true,
    };
}

static void push_pad_sensor(SDL_JoystickID id, SDL_SensorType type, float x, float y, float z,
                            uint64_t sensor_ts, uint64_t host_ts) {
    g_ticks_ns = host_ts;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_GAMEPAD_SENSOR_UPDATE;
    e.gsensor.timestamp = host_ts;
    e.gsensor.which = id;
    e.gsensor.sensor = (Sint32)type;
    e.gsensor.data[0] = x;
    e.gsensor.data[1] = y;
    e.gsensor.data[2] = z;
    e.gsensor.sensor_timestamp = sensor_ts;
    motion_event(&e);
}

static void push_phone_sensor(SDL_SensorType type, float x, float y, float z,
                              uint64_t sensor_ts, uint64_t host_ts) {
    g_ticks_ns = host_ts;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_SENSOR_UPDATE;
    e.sensor.timestamp = host_ts;
    e.sensor.which = (SDL_SensorID)(type == SDL_SENSOR_GYRO ? 1 : 2);
    e.sensor.data[0] = x;
    e.sensor.data[1] = y;
    e.sensor.data[2] = z;
    e.sensor.sensor_timestamp = sensor_ts;
    motion_event(&e);
}

static SixAxisState read_latest(void) {
    SixAxisState out[4];
    memset(out, 0, sizeof out);
    Cpu c;
    memset(&c, 0, sizeof c);
    c.x[0] = (uintptr_t)out;
    c.x[1] = 1;
    hid_sixaxis_states(&c);
    assert(c.x[0] == 1);
    return out[0];
}

static float vec_angle_deg(const float a[3], const float b[3]) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    float na = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    float nb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    float c = dot / (na * nb);
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c) * (180.0f / 3.14159265f);
}

// 1. Nintendo Android (057e:2009, path == NULL): (-y,z,-x) auto-remap on Android (and non-remap on non-Android).
static void test_nintendo_android_axes(void) {
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad pad = make_nintendo_android_pad(1);
    motion_pad_opened(&pad);

    uint64_t t = 1000000000ull;
    const float g = 9.80665f;
#ifdef __ANDROID__
    assert(m_pads[0].nintendo_android);
    push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, 0.0f, g, t, t);
    push_pad_sensor(1, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    SixAxisState s = read_latest();
    assert(fabsf(s.accel[0]) < 1e-4f && fabsf(s.accel[1]) < 1e-4f && fabsf(s.accel[2] + 1.0f) < 1e-4f);
    assert(fabsf(s.dir[1][0]) < 1e-3f && fabsf(s.dir[1][1] - 1.0f) < 1e-3f && fabsf(s.dir[1][2]) < 1e-3f);

    // Pitch nose up 30 deg over 1.0 s (in hid-nintendo, nose up is negative rotation around +Y left).
    const float target_deg = 30.0f;
    const float omega = target_deg * 3.14159265f / 180.0f;
    float theta = 0.0f;
    for (int i = 0; i < 100; i++) {
        t += 10000000ull;
        theta += omega * 0.01f;
        push_pad_sensor(1, SDL_SENSOR_ACCEL, sinf(theta) * g, 0.0f, cosf(theta) * g, t, t);
        push_pad_sensor(1, SDL_SENSOR_GYRO, 0.0f, -omega, 0.0f, t, t);
        s = read_latest();
        if (i == 50) assert(s.gyro[0] > 0.075f && fabsf(s.gyro[1]) < 1e-4f && fabsf(s.gyro[2]) < 1e-4f);
    }
    for (int i = 0; i < 200; i++) {
        t += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_ACCEL, sinf(theta) * g, 0.0f, cosf(theta) * g, t, t);
        push_pad_sensor(1, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    }
    s = read_latest();
    assert(fabsf(s.angle[0] - target_deg / 360.0f) < 0.003f);
    assert(fabsf(s.angle[1]) < 1e-3f && fabsf(s.angle[2]) < 1e-3f);
    float expected_y[3] = {0.0f, cosf(theta), sinf(theta)};
    assert(vec_angle_deg(s.dir[1], expected_y) < 1.0f);

    // Roll right 30 deg from level (in hid-nintendo, +X forward rotation rolls right wing down; Switch accel x points right).
    motion_reset();
    t += 20000000ull;
    push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, 0.0f, g, t, t);
    push_pad_sensor(1, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    theta = 0.0f;
    for (int i = 0; i < 100; i++) {
        t += 10000000ull;
        theta += omega * 0.01f;
        // As pad rolls right (+Y in Switch frame), +X dips toward gravity; in hid-nintendo, Switch +X is +Y_nin.
        push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, sinf(theta) * g, cosf(theta) * g, t, t);
        push_pad_sensor(1, SDL_SENSOR_GYRO, omega, 0.0f, 0.0f, t, t);
        s = read_latest();
        if (i == 50) assert(s.gyro[1] > 0.075f && fabsf(s.gyro[0]) < 1e-4f && fabsf(s.gyro[2]) < 1e-4f);
    }
    for (int i = 0; i < 200; i++) {
        t += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, sinf(theta) * g, cosf(theta) * g, t, t);
        push_pad_sensor(1, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    }
    s = read_latest();
    assert(fabsf(s.angle[1] - target_deg / 360.0f) < 0.003f);
    float expected_x[3] = {cosf(theta), 0.0f, -sinf(theta)};
    assert(vec_angle_deg(s.dir[0], expected_x) < 1.0f);

    // Yaw left (+Z in hid-nintendo -> +z in Switch) has zero bleed into pitch/roll.
    t += 10000000ull;
    push_pad_sensor(1, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.6f, t, t);
    s = read_latest();
    assert(s.gyro[2] > 0.09f && fabsf(s.gyro[0]) < 1e-4f && fabsf(s.gyro[1]) < 1e-4f);
#else
    assert(!m_pads[0].nintendo_android);
    push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, t, t);
    push_pad_sensor(1, SDL_SENSOR_GYRO, 0.5f, 0.0f, 0.0f, t, t);
    SixAxisState s = read_latest();
    assert(fabsf(s.accel[0]) < 1e-4f && fabsf(s.accel[1]) < 1e-4f && fabsf(s.accel[2] + 1.0f) < 1e-4f);
    assert(fabsf(s.gyro[0] - 0.5f / TURN) < 1e-4f);
#endif
}

// 2. Negative controls: non-Nintendo, USB HIDAPI Switch, Switch 2, Retro NSO un-remapped; motion=false is a no-op.
static void test_negative_controls_and_motion_off(void) {
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    const float g = 9.80665f;
    uint64_t t = 1000000000ull;
    SDL_Gamepad non_remap[] = {
        {.id = 2, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
         .name = "DualSense", .path = NULL, .rate = 250.0f, .has_gyro = true, .has_accel = true},
        {.id = 3, .vendor = 0x057e, .product = 0x2009, .type = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,
         .name = "Switch Pro USB", .path = "/dev/hidraw0", .rate = 200.0f, .has_gyro = true, .has_accel = true},
        {.id = 4, .vendor = 0x057e, .product = 0x2069, .type = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,
         .name = "Switch 2 Pro", .path = NULL, .rate = 200.0f, .has_gyro = true, .has_accel = true},
        {.id = 5, .vendor = 0x057e, .product = 0x2017, .type = SDL_GAMEPAD_TYPE_STANDARD,
         .name = "NSO SNES", .path = NULL, .rate = 60.0f, .has_gyro = true, .has_accel = true},
    };
    for (int i = 0; i < (int)(sizeof(non_remap) / sizeof(non_remap[0])); i++) {
        motion_pad_opened(&non_remap[i]);
        assert(!m_pads[0].nintendo_android);
        t += 10000000ull;
        push_pad_sensor(non_remap[i].id, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, t, t);
        push_pad_sensor(non_remap[i].id, SDL_SENSOR_GYRO, 0.5f, 0.0f, 0.0f, t, t);
        SixAxisState s = read_latest();
        assert(fabsf(s.accel[0]) < 1e-4f && fabsf(s.accel[1]) < 1e-4f && fabsf(s.accel[2] + 1.0f) < 1e-4f);
        assert(fabsf(s.gyro[0] - 0.5f / TURN) < 1e-4f);
        motion_pad_closed(non_remap[i].id);
    }

    // Motion controls disabled: pad/phone events are ignored and hid_sixaxis_states emits level rest.
    g_settings.motion = false;
    motion_pad_opened(&non_remap[0]);
    t += 50000000ull;
    push_pad_sensor(2, SDL_SENSOR_ACCEL, g, 0.0f, 0.0f, t, t);
    push_pad_sensor(2, SDL_SENSOR_GYRO, 1.5f, -1.0f, 0.5f, t, t);
    SixAxisState off_s = read_latest();
    assert(fabsf(off_s.gyro[0]) < 1e-5f && fabsf(off_s.gyro[1]) < 1e-5f && fabsf(off_s.gyro[2]) < 1e-5f);
    assert(fabsf(off_s.dir[1][1] - 1.0f) < 1e-5f && fabsf(off_s.accel[2] + 1.0f) < 1e-5f);
}

// 3a. LARECOMP_PAD_GYRO=x,y,z rollback overrides Nintendo Android auto-remap.
static void test_env_pad_gyro_rollback(void) {
    setenv("LARECOMP_PAD_GYRO", "x,y,z", 1);
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad pad = make_nintendo_android_pad(1);
    motion_pad_opened(&pad);
    uint64_t t = 1000000000ull;
    push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, 9.80665f, 0.0f, t, t);
    push_pad_sensor(1, SDL_SENSOR_GYRO, 0.5f, 0.0f, 0.0f, t, t);
    SixAxisState s = read_latest();
    assert(fabsf(s.accel[0]) < 1e-4f && fabsf(s.accel[1]) < 1e-4f && fabsf(s.accel[2] + 1.0f) < 1e-4f);
    assert(fabsf(s.gyro[0] - 0.5f / TURN) < 1e-4f);
}

// 3b. LARECOMP_PAD_GYRO=-y,z,-x signed spec remaps DualSense cleanly.
static void test_env_pad_gyro_signed(void) {
    setenv("LARECOMP_PAD_GYRO", "-y,z,-x", 1);
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad ps = {.id = 2, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
                      .name = "DualSense", .path = NULL, .rate = 250.0f, .has_gyro = true, .has_accel = true};
    motion_pad_opened(&ps);
    uint64_t t = 1000000000ull;
    push_pad_sensor(2, SDL_SENSOR_ACCEL, 0.0f, 0.0f, 9.80665f, t, t);
    push_pad_sensor(2, SDL_SENSOR_GYRO, 0.0f, -0.5f, 0.0f, t, t);
    SixAxisState s = read_latest();
    assert(fabsf(s.accel[2] + 1.0f) < 1e-4f && fabsf(s.gyro[0] - 0.5f / TURN) < 1e-4f);
}

// 3c. Invalid LARECOMP_PAD_GYRO="x,y,z,garbage" logs failure and falls back cleanly.
static void test_env_pad_gyro_invalid_fallback(void) {
    setenv("LARECOMP_PAD_GYRO", "x,y,z,garbage", 1);
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad pad = make_nintendo_android_pad(1);
    motion_pad_opened(&pad);
    assert(!parse_pad_gyro_env(NULL, NULL));
#ifdef __ANDROID__
    uint64_t t = 1000000000ull;
    push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, 0.0f, 9.80665f, t, t);
    push_pad_sensor(1, SDL_SENSOR_GYRO, 0.0f, -0.5f, 0.0f, t, t);
    SixAxisState s = read_latest();
    assert(fabsf(s.accel[2] + 1.0f) < 1e-4f && fabsf(s.gyro[0] - 0.5f / TURN) < 1e-4f);
#else
    uint64_t t = 1000000000ull;
    push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, 9.80665f, 0.0f, t, t);
    push_pad_sensor(1, SDL_SENSOR_GYRO, 0.5f, 0.0f, 0.0f, t, t);
    SixAxisState s = read_latest();
    assert(fabsf(s.accel[2] + 1.0f) < 1e-4f && fabsf(s.gyro[0] - 0.5f / TURN) < 1e-4f);
#endif
}

// 4. pad_gyro_axes (0..3) resets bias/have_accel/q_set safely even when GYRO precedes ACCEL, and permutes gyro+accel.
static void test_settings_pad_gyro_axes_and_reset(void) {
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad ps = {.id = 2, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
                      .name = "DualSense", .path = NULL, .rate = 250.0f, .has_gyro = true, .has_accel = true};
    motion_pad_opened(&ps);
    uint64_t t = 1000000000ull;
    const float g = 9.80665f;
    for (int i = 0; i < 160; i++) {
        t += 12000000ull;
        push_pad_sensor(2, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, t, t);
        push_pad_sensor(2, SDL_SENSOR_GYRO, 0.01f, -0.02f, 0.015f, t, t);
    }
    assert(m_pads[0].bias.ready && q_set);

    // Tilt 45 deg so q != identity.
    t += 12000000ull;
    push_pad_sensor(2, SDL_SENSOR_ACCEL, 0.0f, 0.7071f * g, -0.7071f * g, t, t);
    for (int i = 0; i < 40; i++) {
        t += 12000000ull;
        push_pad_sensor(2, SDL_SENSOR_GYRO, 0.7854f, 0.0f, 0.0f, t, t);
    }
    SixAxisState tilted = read_latest();
    assert(fabsf(tilted.dir[1][2]) > 0.3f);

    // Switch pad_gyro_axes and intentionally push GYRO BEFORE ACCEL (SDL HIDAPI order).
    // Must reset bias, clear have_accel/q_set, NOT re-level from stale accel, then re-level when new ACCEL arrives.
    g_settings.pad_gyro_axes = 1;  // y,x,z: pd = (d[1], d[0], d[2]) -> Switch w=(pd[0], -pd[2], pd[1]), a=(-pd[0], pd[2], -pd[1])
    t += 12000000ull;
    push_pad_sensor(2, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    assert(!m_pads[0].bias.ready && !m_pads[0].have_accel && !q_set);
    t += 12000000ull;
    push_pad_sensor(2, SDL_SENSOR_ACCEL, g, 0.0f, 0.0f, t, t);  // y,x,z -> pd=(0, g, 0) -> Switch accel=(0,0,-g)
    push_pad_sensor(2, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    SixAxisState s = read_latest();
    assert(q_set && fabsf(s.dir[1][2]) < 0.05f);

    // Verify all 4 setting permutations transform both accel and gyro identically.
    const float ax = 1.0f * g, ay = 2.0f * g, az = 3.0f * g;
    const float gx = 0.2f, gy = 0.4f, gz = 0.6f;

    g_settings.pad_gyro_axes = 1;  // y,x,z -> (2, 1, 3) -> Switch a=(-2, 3, -1), g=(0.4, -0.6, 0.2)
    t += 12000000ull;
    push_pad_sensor(2, SDL_SENSOR_ACCEL, ax, ay, az, t, t);
    push_pad_sensor(2, SDL_SENSOR_GYRO, gx, gy, gz, t, t);
    s = read_latest();
    assert(fabsf(s.accel[0] + 2.0f) < 1e-4f && fabsf(s.accel[1] - 3.0f) < 1e-4f && fabsf(s.accel[2] + 1.0f) < 1e-4f);
    assert(fabsf(s.gyro[0] - 0.4f / TURN) < 1e-4f && fabsf(s.gyro[1] + 0.6f / TURN) < 1e-4f && fabsf(s.gyro[2] - 0.2f / TURN) < 1e-4f);

    g_settings.pad_gyro_axes = 2;  // z,y,x -> (3, 2, 1) -> Switch a=(-3, 1, -2), g=(0.6, -0.2, 0.4)
    t += 12000000ull;
    push_pad_sensor(2, SDL_SENSOR_ACCEL, ax, ay, az, t, t);
    push_pad_sensor(2, SDL_SENSOR_GYRO, gx, gy, gz, t, t);
    s = read_latest();
    assert(fabsf(s.accel[0] + 3.0f) < 1e-4f && fabsf(s.accel[1] - 1.0f) < 1e-4f && fabsf(s.accel[2] + 2.0f) < 1e-4f);
    assert(fabsf(s.gyro[0] - 0.6f / TURN) < 1e-4f && fabsf(s.gyro[1] + 0.2f / TURN) < 1e-4f && fabsf(s.gyro[2] - 0.4f / TURN) < 1e-4f);

    g_settings.pad_gyro_axes = 3;  // x,z,y -> (1, 3, 2) -> Switch a=(-1, 2, -3), g=(0.2, -0.4, 0.6)
    t += 12000000ull;
    push_pad_sensor(2, SDL_SENSOR_ACCEL, ax, ay, az, t, t);
    push_pad_sensor(2, SDL_SENSOR_GYRO, gx, gy, gz, t, t);
    s = read_latest();
    assert(fabsf(s.accel[0] + 1.0f) < 1e-4f && fabsf(s.accel[1] - 2.0f) < 1e-4f && fabsf(s.accel[2] + 3.0f) < 1e-4f);
    assert(fabsf(s.gyro[0] - 0.2f / TURN) < 1e-4f && fabsf(s.gyro[1] + 0.4f / TURN) < 1e-4f && fabsf(s.gyro[2] - 0.6f / TURN) < 1e-4f);

    g_settings.pad_gyro_axes = 0;  // x,y,z -> (1, 2, 3) -> Switch a=(-1, 3, -2), g=(0.2, -0.6, 0.4)
    t += 12000000ull;
    push_pad_sensor(2, SDL_SENSOR_ACCEL, ax, ay, az, t, t);
    push_pad_sensor(2, SDL_SENSOR_GYRO, gx, gy, gz, t, t);
    s = read_latest();
    assert(fabsf(s.accel[0] + 1.0f) < 1e-4f && fabsf(s.accel[1] - 3.0f) < 1e-4f && fabsf(s.accel[2] + 2.0f) < 1e-4f);
    assert(fabsf(s.gyro[0] - 0.2f / TURN) < 1e-4f && fabsf(s.gyro[1] + 0.6f / TURN) < 1e-4f && fabsf(s.gyro[2] - 0.4f / TURN) < 1e-4f);
}

static void push_mapped_pad(bool nintendo_mapped, SDL_SensorType type, float x, float y, float z, uint64_t ts,
                            uint64_t h) {
    if (nintendo_mapped) {
        if (type == SDL_SENSOR_ACCEL) push_pad_sensor(1, type, -z, 0.0f, y, ts, h);
        else push_pad_sensor(1, type, 0.0f, -x, 0.0f, ts, h);
    } else {
        push_pad_sensor(1, type, x, y, z, ts, h);
    }
}

static void run_192x_pitch_and_hold(bool nintendo_pad, float *angle_out, float *expected_turns_out,
                                    float *max_rebound_deg_out) {
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad pad = nintendo_pad ? make_nintendo_android_pad(1) : (SDL_Gamepad){
        .id = 1, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
        .name = "Clock Test Pad", .path = NULL, .rate = 0.0f, .has_gyro = true, .has_accel = true,
    };
    motion_pad_opened(&pad);
    bool nintendo_mapped = nintendo_pad && m_pads[0].nintendo_android;

    const float g = 9.80665f;
    uint64_t host = 1000000000ull;
    uint64_t hw = 5000000000ull;
    // Exact 8BitDo Pro 2 BT packet pattern: 22.0 ms hw (3.666 + 3.666 + 14.666 ms) per 11.458 ms host (~1.92x).
    const uint64_t hw_step[3] = {3666667ull, 3666667ull, 14666666ull};
    const uint64_t host_step[3] = {50000ull, 50000ull, 11358333ull};

    while (host < 1600000000ull) {
        for (int k = 0; k < 3; k++) {
            hw += hw_step[k];
            host += host_step[k];
            push_mapped_pad(nintendo_mapped, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, hw, host);
            push_mapped_pad(nintendo_mapped, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, hw, host);
        }
    }

    SixAxisState base_s = read_latest();
    float base_angle = base_s.angle[0];
    uint64_t pitch_start = host;
    const float omega = 60.0f * 3.14159265f / 180.0f;
    float theta = 0.0f;
    uint64_t prev_host = host;
    while (host - pitch_start < 750000000ull) {
        for (int k = 0; k < 3; k++) {
            hw += hw_step[k];
            host += host_step[k];
            float real_dt = (float)((host - prev_host) / 1e9);
            prev_host = host;
            theta += omega * real_dt;
            push_mapped_pad(nintendo_mapped, SDL_SENSOR_ACCEL, 0.0f, cosf(theta) * g, -sinf(theta) * g, hw, host);
            push_mapped_pad(nintendo_mapped, SDL_SENSOR_GYRO, omega, 0.0f, 0.0f, hw, host);
        }
    }
    SixAxisState after_pitch = read_latest();
    *angle_out = after_pitch.angle[0] - base_angle;
    *expected_turns_out = theta / TURN;

    float target_y[3] = {0.0f, cosf(theta), sinf(theta)};
    float max_err = 0.0f;
    uint64_t hold_start = host;
    while (host - hold_start < 5000000000ull) {
        for (int k = 0; k < 3; k++) {
            hw += hw_step[k];
            host += host_step[k];
            push_mapped_pad(nintendo_mapped, SDL_SENSOR_ACCEL, 0.0f, cosf(theta) * g, -sinf(theta) * g, hw, host);
            push_mapped_pad(nintendo_mapped, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, hw, host);
            SixAxisState cur = read_latest();
            float err = vec_angle_deg(cur.dir[1], target_y);
            if (err > max_err) max_err = err;
        }
    }
    *max_rebound_deg_out = max_err;
}

// 5a. 1.92x MSC_TIMESTAMP skew fixed automatically on Nintendo Android (and via LARECOMP_CLOCK_SCALE=1 on host).
static void test_clock_skew_1_92x_fixed(void) {
    setenv("LARECOMP_NO_CLOCK_SCALE", "0", 1);
#ifndef __ANDROID__
    setenv("LARECOMP_CLOCK_SCALE", "1", 1);
#endif
    float angle = 0.0f, expected_turns = 0.0f, max_err = 0.0f;
    run_192x_pitch_and_hold(true, &angle, &expected_turns, &max_err);
    assert(fabsf(m_pads[0].clock.scale - 1.92f) < 0.05f);
    assert(m_pads[0].clock.scale_logged);
    assert(fabsf(angle - expected_turns) < 0.004f);
    assert(max_err < 1.5f);
}

// 5b. Negative control with LARECOMP_NO_CLOCK_SCALE=1 reproduces ~1.92x overshoot and >20 deg rebound.
static void test_clock_skew_negative_control(void) {
    setenv("LARECOMP_NO_CLOCK_SCALE", "1", 1);
    float angle = 0.0f, expected_turns = 0.0f, max_err = 0.0f;
    run_192x_pitch_and_hold(true, &angle, &expected_turns, &max_err);
    assert(angle > expected_turns * 1.75f);
    assert(max_err > 20.0f);
}

// 5c. Non-Nintendo pads stay unscaled by default, while LARECOMP_CLOCK_SCALE=1 opts them in cleanly.
static void test_clock_scale_non_nintendo_unscaled(void) {
    float angle = 0.0f, expected_turns = 0.0f, max_err = 0.0f;
    run_192x_pitch_and_hold(false, &angle, &expected_turns, &max_err);
    assert(m_pads[0].clock.scale == 0.0f && !m_pads[0].clock.scale_logged);
    assert(angle > expected_turns * 1.75f);
    assert(max_err > 20.0f);
}

static void test_clock_scale_env_opt_in(void) {
    setenv("LARECOMP_CLOCK_SCALE", "1", 1);
    float angle = 0.0f, expected_turns = 0.0f, max_err = 0.0f;
    run_192x_pitch_and_hold(false, &angle, &expected_turns, &max_err);
    assert(fabsf(m_pads[0].clock.scale - 1.92f) < 0.05f && m_pads[0].clock.scale_logged);
    assert(fabsf(angle - expected_turns) < 0.004f && max_err < 1.5f);
}

// 6. Sane 1.0x clock with 3-sample burst (dt_host == 0 inside burst) + mid-burst 32-bit MSC_TIMESTAMP wrap.
static void test_sane_clock_burst_and_wrap(void) {
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;
#ifndef __ANDROID__
    setenv("LARECOMP_CLOCK_SCALE", "1", 1);
#endif

    SDL_Gamepad pad = make_nintendo_android_pad(3);
    motion_pad_opened(&pad);

    uint64_t host = 1000000000ull;
    uint64_t hw = 1000000000ull;
    for (int p = 0; p < 100; p++) {
        host += 15000000ull;
        for (int k = 0; k < 3; k++) {
            hw += 5000000ull;
            push_pad_sensor(3, SDL_SENSOR_GYRO, 0.0f, -0.5f, 0.0f, hw, host);
            SixAxisState cur = read_latest();
            // Even during initial 250 ms warmup bursts (k=1,2 where dt_host==0), delta_time must stay > 0.
            assert(cur.delta_time > 0 && cur.delta_time <= 20000000);
        }
    }
    assert(m_pads[0].clock.scale == 1.0f && m_pads[0].clock.scale_logged);
    SixAxisState s = read_latest();
    int axis = m_pads[0].nintendo_android ? 0 : 2;
    float expect_sign = m_pads[0].nintendo_android ? 1.0f : -1.0f;
    assert(fabsf(s.angle[axis] - expect_sign * (0.5f / TURN) * (100 * 0.015f)) < 0.002f);

    // Position hw so 32-bit MSC_TIMESTAMP wrap crosses specifically on p=1, k=1 (dt_host == 0 mid-burst).
    const uint64_t wrap_limit = (1ull << 32) * 1000ull;
    host += 120000000ull;
    hw = wrap_limit - 22000000ull;
    host += 15000000ull;
    push_pad_sensor(3, SDL_SENSOR_GYRO, 0.0f, -0.5f, 0.0f, hw, host);  // gap reset -> dt = 0.005 s
    float pre_wrap_angle = read_latest().angle[axis];
    int wrap_samples = 0;
    for (int p = 0; p < 40; p++) {
        host += 15000000ull;
        for (int k = 0; k < 3; k++) {
            hw = (hw + 5000000ull) % wrap_limit;
            push_pad_sensor(3, SDL_SENSOR_GYRO, 0.0f, -0.5f, 0.0f, hw, host);
            SixAxisState cur = read_latest();
            assert(cur.delta_time > 0 && cur.delta_time <= 20000000);
            wrap_samples++;
        }
    }
    assert(wrap_samples == 120);
    s = read_latest();
    assert(fabsf((s.angle[axis] - pre_wrap_angle) - expect_sign * (0.5f / TURN) * (40 * 0.015f)) < 0.002f);
    assert(m_pads[0].clock.scale == 1.0f);
}

// 7. GyroBias host-time lock, slow-aim immunity, accel check, gap reset, post-lock tracking, and 6 s recovery.
static void test_bias_precision_immunity_and_recovery(void) {
    setenv("LARECOMP_CLOCK_SCALE", "1", 1);
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad pad = {
        .id = 1, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
        .name = "Bias Test Pad", .path = NULL, .rate = 0.0f, .has_gyro = true, .has_accel = true,
    };
    motion_pad_opened(&pad);

    const float g = 9.80665f;
    const float off[3] = {-0.0094f, 0.0058f, -0.0187f};
    uint64_t host = 1000000000ull;
    uint64_t hw = 2000000000ull;
    const uint64_t hw_step[3] = {3666667ull, 3666667ull, 14666666ull};
    const uint64_t host_step[3] = {50000ull, 50000ull, 11358333ull};

    // Accel jitter > 0.035 G must reset transient bias accumulation even if gyro stays flat.
    for (int i = 0; i < 60; i++) {
        host += 10000000ull;
        hw += 19200000ull;
        push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, g, (i & 1) ? 0.4f : -0.4f, hw, host);
        push_pad_sensor(1, SDL_SENSOR_GYRO, off[0], off[1], off[2], hw, host);
    }
    assert(m_pads[0].bias.count <= 1 && !m_pads[0].bias.ready);

    // 1.4 s real host_ts (hw_ts advanced ~2.69 s): must NOT lock prematurely!
    uint64_t win_start = host;
    int idx = 0;
    while (host - win_start < 1400000000ull) {
        for (int k = 0; k < 3; k++) {
            hw += hw_step[k];
            host += host_step[k];
            float sgn = (idx & 1) ? 1.0f : -1.0f;
            idx++;
            push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, hw, host);
            push_pad_sensor(1, SDL_SENSOR_GYRO,
                            off[0] + sgn * 0.003f, off[1] - sgn * 0.002f, off[2] + sgn * 0.0025f, hw, host);
        }
    }
    assert(!m_pads[0].bias.ready);

    // Pause gap > 250 ms after count >= 20 must reset transient window and NOT prematurely lock across gap.
    host += 1600000000ull;
    hw += 1600000000ull;
    push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, hw, host);
    push_pad_sensor(1, SDL_SENSOR_GYRO, off[0], off[1], off[2], hw, host);
    assert(!m_pads[0].bias.ready && m_pads[0].bias.count == 1);

    // Hold 1.7 s real host_ts -> locks exact mean in m_pads[0].bias.
    win_start = host;
    while (host - win_start < 1700000000ull) {
        for (int k = 0; k < 3; k++) {
            hw += hw_step[k];
            host += host_step[k];
            float sgn = (idx & 1) ? 1.0f : -1.0f;
            idx++;
            push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, hw, host);
            push_pad_sensor(1, SDL_SENSOR_GYRO,
                            off[0] + sgn * 0.003f, off[1] - sgn * 0.002f, off[2] + sgn * 0.0025f, hw, host);
        }
    }
    assert(m_pads[0].bias.ready);
    for (int i = 0; i < 3; i++) assert(fabsf(m_pads[0].bias.bias[i] - off[i]) < 0.002f);

    // Hold 10 s at rest: total angle drift must stay < 0.5 deg.
    SixAxisState s0 = read_latest();
    uint64_t rest_start = host;
    while (host - rest_start < 10000000000ull) {
        for (int k = 0; k < 3; k++) {
            hw += hw_step[k];
            host += host_step[k];
            float sgn = (idx & 1) ? 1.0f : -1.0f;
            idx++;
            push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, hw, host);
            push_pad_sensor(1, SDL_SENSOR_GYRO,
                            off[0] + sgn * 0.003f, off[1] - sgn * 0.002f, off[2] + sgn * 0.0025f, hw, host);
        }
    }
    SixAxisState s1 = read_latest();
    float drift_turns = sqrtf((s1.angle[0] - s0.angle[0]) * (s1.angle[0] - s0.angle[0]) +
                              (s1.angle[1] - s0.angle[1]) * (s1.angle[1] - s0.angle[1]) +
                              (s1.angle[2] - s0.angle[2]) * (s1.angle[2] - s0.angle[2]));
    assert(drift_turns * 360.0f < 0.5f);

    // Slow 2.0 deg/s (0.0349 rad/s) bow pan for 3.0 s must NOT corrupt learned controller bias.
    float saved_bias[3] = {m_pads[0].bias.bias[0], m_pads[0].bias.bias[1], m_pads[0].bias.bias[2]};
    uint64_t pan_start = host;
    const float slow_pan = 2.0f * 3.14159265f / 180.0f;
    while (host - pan_start < 3000000000ull) {
        for (int k = 0; k < 3; k++) {
            hw += hw_step[k];
            host += host_step[k];
            push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, hw, host);
            push_pad_sensor(1, SDL_SENSOR_GYRO, off[0], off[1], off[2] + slow_pan, hw, host);
        }
    }
    for (int i = 0; i < 3; i++) assert(fabsf(m_pads[0].bias.bias[i] - saved_bias[i]) < 0.001f);

    // Post-lock gentle drift tracking within tol_g (> 3.0 s still).
    uint64_t track_start = host;
    const float shifted[3] = {off[0] + 0.006f, off[1], off[2]};
    while (host - track_start < 6500000000ull) {
        for (int k = 0; k < 3; k++) {
            hw += hw_step[k];
            host += host_step[k];
            push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, hw, host);
            push_pad_sensor(1, SDL_SENSOR_GYRO, shifted[0], shifted[1], shifted[2], hw, host);
        }
    }
    assert(fabsf(m_pads[0].bias.bias[0] - shifted[0]) < 0.003f);

    // 6 s dead stillness recovery from larger thermal shift (> tol_g = 0.015 rad/s, still <= 0.08 rad/s).
    const float thermal[3] = {off[0] + 0.030f, off[1] - 0.025f, off[2] + 0.028f};
    uint64_t rec_start = host;
    while (host - rec_start < 6200000000ull) {
        for (int k = 0; k < 3; k++) {
            hw += hw_step[k];
            host += host_step[k];
            push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, hw, host);
            push_pad_sensor(1, SDL_SENSOR_GYRO, thermal[0], thermal[1], thermal[2], hw, host);
        }
    }
    for (int i = 0; i < 3; i++) assert(fabsf(m_pads[0].bias.bias[i] - thermal[i]) < 0.002f);
}

// 8a. Phone 180-deg display flip keeps raw MEMS bias intact (zero 2x drift & zero permanent lockout).
static void test_phone_180_flip_bias_invariance(void) {
    g_settings.motion = true;
    g_settings.motion_source = 1;
    g_settings.motion_flat = false;
    g_display_orientation = SDL_ORIENTATION_LANDSCAPE;  // turns == 1

    const float g = 9.80665f;
    const float phone_off[3] = {0.025f, -0.018f, 0.012f};  // > 0.0075 rad/s so old screen-frame bug would lock out!
    uint64_t t = 1000000000ull;
    for (int i = 0; i < 180; i++) {
        t += 10000000ull;
        push_phone_sensor(SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, t, t);
        push_phone_sensor(SDL_SENSOR_GYRO, phone_off[0], phone_off[1], phone_off[2], t, t);
    }
    assert(m_phone_bias.ready);
    assert(fabsf(m_phone_bias.bias[0] - phone_off[0]) < 0.002f);
    assert(fabsf(m_phone_bias.bias[1] - phone_off[1]) < 0.002f);

    // Flip display 180 deg to LANDSCAPE_FLIPPED (turns == 3); advance > 250 ms for screen_turns cache + > 400 ms quiet.
    g_display_orientation = SDL_ORIENTATION_LANDSCAPE_FLIPPED;
    t += 300000000ull;
    push_phone_sensor(SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, t, t);
    t += 500000000ull;
    push_phone_sensor(SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, t, t);
    push_phone_sensor(SDL_SENSOR_GYRO, phone_off[0], phone_off[1], phone_off[2], t, t);
    SixAxisState s0 = read_latest();
    assert(fabsf(s0.gyro[0]) < 0.001f && fabsf(s0.gyro[1]) < 0.001f && fabsf(s0.gyro[2]) < 0.001f);

    for (int i = 0; i < 200; i++) {
        t += 10000000ull;
        push_phone_sensor(SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, t, t);
        push_phone_sensor(SDL_SENSOR_GYRO, phone_off[0], phone_off[1], phone_off[2], t, t);
    }
    SixAxisState s1 = read_latest();
    assert(fabsf(s1.angle[0] - s0.angle[0]) < 0.001f);
    assert(fabsf(s1.angle[1] - s0.angle[1]) < 0.001f);
}

// 8b. LARECOMP_NO_GYRO_BIAS=1 disables bias subtraction cleanly.
static void test_env_no_gyro_bias(void) {
    setenv("LARECOMP_NO_GYRO_BIAS", "1", 1);
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad ps = {.id = 1, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
                      .name = "PS5", .path = NULL, .rate = 250.0f, .has_gyro = true, .has_accel = true};
    motion_pad_opened(&ps);
    uint64_t t = 1000000000ull;
    for (int i = 0; i < 200; i++) {
        t += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, 9.80665f, 0.0f, t, t);
        push_pad_sensor(1, SDL_SENSOR_GYRO, 0.02f, 0.0f, 0.0f, t, t);
    }
    assert(!m_pads[0].bias.ready);
    SixAxisState s = read_latest();
    assert(fabsf(s.gyro[0] - 0.02f / TURN) < 1e-4f);
}

// 9. Multi-pad arbitration, clean close, post-close event rejection, and phone attitude safety on unrelated close.
static void test_multi_pad_and_close_isolation(void) {
    setenv("LARECOMP_CLOCK_SCALE", "1", 1);
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad p1 = {.id = 1, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
                      .name = "Pad 1 (1.92x)", .path = NULL, .rate = 0.0f, .has_gyro = true, .has_accel = true};
    SDL_Gamepad p2 = {.id = 2, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
                      .name = "Pad 2 (1.0x)", .path = NULL, .rate = 200.0f, .has_gyro = true, .has_accel = true};
    motion_pad_opened(&p1);
    motion_pad_opened(&p2);

    const float g = 9.80665f;
    uint64_t host = 1000000000ull, hw1 = 2000000000ull, hw2 = 3000000000ull;
    // Interleave Pad 1 (1.92x) and Pad 2 (1.0x) every 10 ms: Pad 1 keeps lock & clock scale; Pad 2 doesn't wipe it!
    for (int i = 0; i < 180; i++) {
        host += 10000000ull;
        hw1 += 19200000ull;
        hw2 += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, hw1, host);
        push_pad_sensor(1, SDL_SENSOR_GYRO, 0.01f, 0.0f, 0.0f, hw1, host);
        push_pad_sensor(2, SDL_SENSOR_ACCEL, g, 0.0f, 0.0f, hw2, host);
        push_pad_sensor(2, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, hw2, host);
    }
    assert(m_active_pad_id == 1);
    assert(fabsf(m_pads[0].clock.scale - 1.92f) < 0.05f);
    assert(m_pads[0].bias.ready && fabsf(m_pads[0].bias.bias[0] - 0.01f) < 0.002f);

    // Stop Pad 1 for 0.6 s, turn Pad 2 with GYRO before ACCEL -> cleanly switches active pad without 1.92x bleed.
    host += 600000000ull;
    hw2 += 600000000ull;
    push_pad_sensor(2, SDL_SENSOR_GYRO, 0.6f, 0.0f, 0.0f, hw2, host);
    assert(m_active_pad_id == 2 && !m_pads[1].have_accel && !q_set);
    push_pad_sensor(2, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, hw2, host);
    assert(m_pads[1].have_accel && q_set && m_pads[1].clock.scale == 0.0f);

    // Close active Pad 2 and push stale buffered events for Pad 2 -> strictly ignored, Pad 1 unharmed.
    motion_pad_closed(2);
    assert(m_active_pad_id == 0);
    int64_t samp_before = m_sampling;
    push_pad_sensor(2, SDL_SENSOR_GYRO, 0.9f, 0.0f, 0.0f, hw2 + 10000000ull, host + 10000000ull);
    assert(m_active_pad_id == 0 && m_sampling == samp_before);

    // Auto mode: close active live Pad 1 while dead Pad 3 is still open -> Pad 3 must NOT inherit live=true!
    motion_pad_closed(1);
    g_settings.motion_source = 0;
    SDL_Gamepad dead_pad = {.id = 3, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
                            .name = "Dead Grip", .path = NULL, .rate = 200.0f, .has_gyro = true, .has_accel = true};
    motion_pad_opened(&p1);
    motion_pad_opened(&dead_pad);
    for (int i = 0; i < 30; i++) {
        host += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_GYRO, 0.6f + 0.01f * (i & 1), 0.0f, 0.0f, host, host);
    }
    assert(m_pads[0].live && m_active_pad_id == 1 && pad_until_ns > host);
    motion_pad_closed(1);
    assert(m_active_pad_id == 0 && pad_until_ns == 0 && !m_pads[1].live);

    // Auto mode: two live pads (e.g. BT Joy-Con L + R); Pad 1 resting jitter stays live, turning Pad 2 takes over.
    motion_pad_closed(3);
    motion_pad_opened(&p1);
    motion_pad_opened(&p2);
    for (int i = 0; i < 30; i++) {
        host += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_GYRO, 0.6f + 0.01f * (i & 1), 0.0f, 0.0f, host, host);
    }
    assert(m_pads[0].live && m_active_pad_id == 1);
    host += 600000000ull;
    for (int i = 0; i < 30; i++) {
        host += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_GYRO, 0.002f * ((i & 1) ? 1 : -1), 0.0f, 0.0f, host, host);
        push_pad_sensor(2, SDL_SENSOR_GYRO, 0.7f + 0.01f * (i & 1), 0.0f, 0.0f, host, host);
    }
    assert(m_pads[1].live && m_active_pad_id == 2);

    // Disconnecting a non-gyro pad while phone sensor is active must NOT clear phone q_set or snap attitude.
    // LANDSCAPE (turns == 1): upright gravity is +X raw (g, 0, 0), and nose-up pitch around +X_screen is -Y raw gyro.
    g_settings.motion_source = 1;
    g_display_orientation = SDL_ORIENTATION_LANDSCAPE;
    host += 10000000ull;
    push_phone_sensor(SDL_SENSOR_ACCEL, g, 0.0f, 0.0f, host, host);
    for (int i = 0; i < 40; i++) {
        host += 10000000ull;
        push_phone_sensor(SDL_SENSOR_ACCEL, 0.7071f * g, 0.0f, -0.7071f * g, host, host);
        push_phone_sensor(SDL_SENSOR_GYRO, 0.0f, -0.7854f, 0.0f, host, host);
    }
    SixAxisState before_close = read_latest();
    assert(q_set && fabsf(before_close.dir[1][2]) > 0.3f);
    motion_pad_closed(99);
    assert(q_set);
    SixAxisState after_close = read_latest();
    assert(fabsf(after_close.dir[1][2] - before_close.dir[1][2]) < 1e-4f);
}

// 10. Auto mode quantized wakeup, #115 noise cap, 2s timeout + orphan accel drop, and NaN/Inf rejection.
static void test_auto_mode_and_nan_guards(void) {
    g_settings.motion = true;
    g_settings.motion_source = 0;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad pad = make_nintendo_android_pad(1);
    motion_pad_opened(&pad);
    uint64_t t = 1000000000ull;

    // All-zero fake stream -> never live.
    for (int i = 0; i < 100; i++) {
        t += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    }
    assert(!m_pads[0].live);

    // #115 sub-threshold noise caps changing at 20; 25 frozen repeats then decay it back to 0 so 1 spike stays dead.
    for (int i = 0; i < 100; i++) {
        t += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_GYRO, (i & 1) ? 0.05f : 0.04f, 0.0f, 0.0f, t, t);
    }
    assert(m_pads[0].changing <= 20 && !m_pads[0].turned && !m_pads[0].live);
    for (int i = 0; i < 25; i++) {
        t += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_GYRO, 0.6f, 0.0f, 0.0f, t, t);
    }
    assert(!m_pads[0].live && m_pads[0].changing == 0);

    // Real quantized gyro (80% changing, 20% repeating) -> wakes cleanly.
    float val = 0.6000f;
    int step = 0;
    for (int i = 0; i < 60; i++) {
        t += 10000000ull;
        if (i % 5 != 4) {
            step++;
            val = (step & 1) ? 0.6012f : 0.6000f;
        }
        push_pad_sensor(1, SDL_SENSOR_GYRO, val, 0.0f, 0.0f, t, t);
    }
    assert(m_pads[0].live && pad_until_ns > t);

    // NaN and Inf packets must be rejected completely without poisoning q, bias, angle, or sampling count.
    int64_t samp_ok = m_sampling;
    push_pad_sensor(1, SDL_SENSOR_ACCEL, NAN, 0.0f, 9.80665f, t + 5000000ull, t + 5000000ull);
    push_pad_sensor(1, SDL_SENSOR_GYRO, INFINITY, -INFINITY, NAN, t + 10000000ull, t + 10000000ull);
    push_phone_sensor(SDL_SENSOR_GYRO, NAN, 0.0f, 0.0f, t + 15000000ull, t + 15000000ull);
    assert(m_sampling == samp_ok);
    SixAxisState safe = read_latest();
    assert(isfinite(safe.angle[0]) && isfinite(safe.dir[1][1]) && !isnan(m_pads[0].bias.bias[0]));

    // Freeze gyro to a constant value for > 2.0 s -> must revert cleanly back to phone (!live, pad_until_ns == 0).
    uint64_t freeze_start = t;
    while (t - freeze_start < 2100000000ull) {
        t += 20000000ull;
        push_pad_sensor(1, SDL_SENSOR_GYRO, val, 0.0f, 0.0f, t, t);
    }
    assert(!m_pads[0].live && pad_until_ns == 0);

    // Wake again, then stop gyro altogether while accel continues -> orphan accel must expire live too.
    for (int i = 0; i < 30; i++) {
        t += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_GYRO, (i & 1) ? 0.61f : 0.60f, 0.0f, 0.0f, t, t);
    }
    assert(m_pads[0].live && pad_until_ns > t);
    for (int i = 0; i < 220; i++) {
        t += 10000000ull;
        push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, 0.0f, 9.80665f, t, t);
    }
    assert(!m_pads[0].live && pad_until_ns == 0);

    motion_pad_closed(1);
    assert(m_active_pad_id == 0 && !m_pads[0].id);
}

// 11. hid_sixaxis_states underflow guard + motion_reset retains open controller registration & allows clean resume.
static void test_sixaxis_underflow_and_reset_persistence(void) {
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad pad = make_nintendo_android_pad(1);
    motion_pad_opened(&pad);
    uint64_t t = 2000000000ull;
    push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.0f, 0.0f, 9.80665f, t, t);
    push_pad_sensor(1, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    int64_t sampling_before = m_sampling;

#ifndef _WIN32
    int log_pipe[2];
    assert(pipe(log_pipe) == 0);
    int saved_err = dup(STDERR_FILENO);
    dup2(log_pipe[1], STDERR_FILENO);
    close(log_pipe[1]);
#endif

    // Simulate now < m_live_ns (event thread updated m_live_ns slightly ahead of game read).
    g_ticks_ns = m_live_ns - 50000000ull;
    SixAxisState s = read_latest();
    assert(s.attributes == 1 && m_sampling == sampling_before);  // must NOT trigger own_sample via unsigned underflow

#ifndef _WIN32
    fflush(stderr);
    dup2(saved_err, STDERR_FILENO);
    close(saved_err);
    char buf[1024] = {0};
    ssize_t nread = read(log_pipe[0], buf, sizeof(buf) - 1);
    close(log_pipe[0]);
    if (nread > 0) assert(strstr(buf, "no gyroscope readings for 1 s") == NULL);
#endif

    // In-place reboot reset must clear transient state while keeping m_pads[] active.
    g_ticks_ns = m_live_ns + 10000000ull;
    motion_reset();
    assert(ring_n == 0 && !q_set && m_live_ns == 0 && !m_pads[0].clock.has_prev && m_pads[0].bias.still_since == 0);
    assert(m_pads[0].id == 1);

    // Push GYRO before ACCEL after reset: must integrate without stale level until non-default ACCEL arrives.
    const float g = 9.80665f;
    t += 10000000ull;
    push_pad_sensor(1, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    assert(!q_set);
    push_pad_sensor(1, SDL_SENSOR_ACCEL, 0.6f * g, 0.0f, 0.8f * g, t, t);
    assert(q_set);
    t += 10000000ull;
    push_pad_sensor(1, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    s = read_latest();
#ifdef __ANDROID__
    assert(fabsf(s.accel[0]) < 1e-4f && fabsf(s.accel[1] + 0.6f) < 1e-4f && fabsf(s.accel[2] + 0.8f) < 1e-4f);
#else
    assert(fabsf(s.accel[0] + 0.6f) < 1e-4f && fabsf(s.accel[1] - 0.8f) < 1e-4f && fabsf(s.accel[2]) < 1e-4f);
#endif
}

// 12. Accelerometer-only phone (no gyroscope) and >250 ms gyro pause keep following gravity via own_sample().
static void test_phone_accel_only_tilt(void) {
    g_settings.motion = true;
    g_settings.motion_source = 0;
    g_settings.motion_flat = false;
    g_display_orientation = SDL_ORIENTATION_LANDSCAPE;  // turns == 1

    const float g = 9.80665f;
    uint64_t t = 1000000000ull;

    // Phone without gyroscope: only ACCEL events arrive; initial sample levels q_set = true.
    push_phone_sensor(SDL_SENSOR_ACCEL, g, 0.0f, 0.0f, t, t);
    g_ticks_ns = t;
    SixAxisState s0 = read_latest();
    assert(q_set && fabsf(s0.dir[1][1] - 1.0f) < 1e-3f && fabsf(s0.dir[1][2]) < 1e-3f);

    // Tilt phone 45 deg nose-up; Mahony accel gain KP=0.5 rad/s (tau=2 s) converges within < 2 deg over 8 s.
    for (int i = 0; i < 240; i++) {
        t += 33333333ull;
        push_phone_sensor(SDL_SENSOR_ACCEL, 0.70710678f * g, 0.0f, -0.70710678f * g, t, t);
        g_ticks_ns = t;
        read_latest();
    }
    SixAxisState s1 = read_latest();
    float expected_y[3] = {0.0f, 0.70710678f, 0.70710678f};
    assert(vec_angle_deg(s1.dir[1], expected_y) < 2.0f);

    // Gyro pause > 250 ms while accel tilts back to upright must also level smoothly via own_sample().
    t += 33333333ull;
    push_phone_sensor(SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    t += 300000000ull;
    for (int i = 0; i < 240; i++) {
        t += 33333333ull;
        push_phone_sensor(SDL_SENSOR_ACCEL, g, 0.0f, 0.0f, t, t);
        g_ticks_ns = t;
        read_latest();
    }
    SixAxisState s2 = read_latest();
    float upright_y[3] = {0.0f, 1.0f, 0.0f};
    assert(vec_angle_deg(s2.dir[1], upright_y) < 2.0f);

    // Source=Controller only: stale phone accel must NOT pull own_sample() attitude.
    g_settings.motion_source = 2;
    m_phone_accel[0] = 0.0f; m_phone_accel[1] = 1.0f; m_phone_accel[2] = 0.0f;
    for (int i = 0; i < 30; i++) {
        t += 33333333ull;
        g_ticks_ns = t;
        read_latest();
    }
    SixAxisState s3 = read_latest();
    assert(vec_angle_deg(s3.dir[1], upright_y) < 3.0f);

    // motion_reset() clears m_phone_have_accel / m_last_phone across multiple steps.
    g_settings.motion_source = 1;
    motion_reset();
    assert(!m_phone_have_accel && m_last_phone == -1);
    m_phone_accel[0] = 0.0f; m_phone_accel[1] = 1.0f; m_phone_accel[2] = 0.0f;
    for (int i = 0; i < 30; i++) {
        t += 33333333ull;
        g_ticks_ns = t;
        read_latest();
    }
    SixAxisState s4 = read_latest();
    assert(fabsf(s4.dir[1][1] - 1.0f) < 1e-4f);
}

// 13. Controller gyro-pause accel fallback via own_sample(), motion_source=1 isolation, and pad-close cleanup.
static void test_pad_gyro_pause_and_close_fallback(void) {
    g_settings.motion = true;
    g_settings.motion_source = 2;
    g_settings.pad_gyro_axes = 0;

    SDL_Gamepad pad = {
        .id = 7, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
        .name = "DualSense Pause", .path = NULL, .rate = 250.0f, .has_gyro = true, .has_accel = true,
    };
    motion_pad_opened(&pad);
    const float g = 9.80665f;
    uint64_t t = 2000000000ull;

    // Level upright via ACCEL + GYRO, then pause GYRO > 250 ms while tilting controller 45 deg nose-up.
    push_pad_sensor(7, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, t, t);
    push_pad_sensor(7, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    t += 300000000ull;
    for (int i = 0; i < 240; i++) {
        t += 33333333ull;
        push_pad_sensor(7, SDL_SENSOR_ACCEL, 0.0f, 0.70710678f * g, -0.70710678f * g, t, t);
        g_ticks_ns = t;
        read_latest();
    }
    SixAxisState s_tilted = read_latest();
    float expected_y[3] = {0.0f, 0.70710678f, 0.70710678f};
    assert(vec_angle_deg(s_tilted.dir[1], expected_y) < 2.0f);

    // Switching motion_source=1 (Phone) ignores controller accel during own_sample().
    g_settings.motion_source = 1;
    m_pads[0].accel[0] = 0.0f; m_pads[0].accel[1] = 0.0f; m_pads[0].accel[2] = -1.0f;
    for (int i = 0; i < 30; i++) {
        t += 33333333ull;
        g_ticks_ns = t;
        read_latest();
    }
    SixAxisState s_phone_only = read_latest();
    assert(vec_angle_deg(s_phone_only.dir[1], expected_y) < 2.5f);

    // Restoring Controller mode resumes controller accel leveling, then closing pad stops fusing its accel.
    g_settings.motion_source = 2;
    for (int i = 0; i < 240; i++) {
        t += 33333333ull;
        push_pad_sensor(7, SDL_SENSOR_ACCEL, 0.0f, g, 0.0f, t, t);
        g_ticks_ns = t;
        read_latest();
    }
    SixAxisState s_upright = read_latest();
    float upright_y[3] = {0.0f, 1.0f, 0.0f};
    assert(vec_angle_deg(s_upright.dir[1], upright_y) < 2.0f);

    motion_pad_closed(7);
    assert(m_active_pad_id == 0 && !m_pads[0].have_accel);
    m_pads[0].accel[0] = 0.0f; m_pads[0].accel[1] = 1.0f; m_pads[0].accel[2] = 0.0f;
    for (int i = 0; i < 30; i++) {
        t += 33333333ull;
        g_ticks_ns = t;
        read_latest();
    }
    SixAxisState s_closed = read_latest();
    assert(vec_angle_deg(s_closed.dir[1], s_upright.dir[1]) < 1e-2f);
}

// 14. Toggling motion false->true & switching Controller(2)->Auto(0) clear stale accel/clock/live state cleanly.
static void test_motion_toggle_and_source_2_to_0(void) {
    const float g = 9.80665f;
    uint64_t t = 1000000000ull;

    g_settings.motion = true;
    g_settings.motion_source = 1;
    g_settings.motion_flat = false;
    g_display_orientation = SDL_ORIENTATION_LANDSCAPE;

    // Tilt phone 45 deg, then toggle motion OFF (read once) and back ON at level orientation.
    for (int i = 0; i < 120; i++) {
        t += 33333333ull;
        push_phone_sensor(SDL_SENSOR_ACCEL, 0.70710678f * g, 0.0f, -0.70710678f * g, t, t);
        g_ticks_ns = t;
        read_latest();
    }
    g_settings.motion = false;
    t += 33333333ull;
    g_ticks_ns = t;
    SixAxisState off_s = read_latest();
    assert(!q_set && !m_phone_have_accel && m_last_phone == -1 && m_active_pad_id == 0);
    assert(fabsf(off_s.dir[1][1] - 1.0f) < 1e-5f);

    // Re-enable motion and check 30 steps without new events: must NOT pull toward old tilted m_phone_accel.
    g_settings.motion = true;
    for (int i = 0; i < 30; i++) {
        t += 33333333ull;
        g_ticks_ns = t;
        read_latest();
    }
    SixAxisState s_clean = read_latest();
    assert(fabsf(s_clean.dir[1][1] - 1.0f) < 1e-4f);

    // First new phone ACCEL event after re-enable must level immediately (q_set false -> true).
    t += 33333333ull;
    push_phone_sensor(SDL_SENSOR_ACCEL, 0.70710678f * g, 0.0f, -0.70710678f * g, t, t);
    assert(q_set);
    SixAxisState s_leveled = read_latest();
    float expected_y[3] = {0.0f, 0.70710678f, 0.70710678f};
    assert(vec_angle_deg(s_leveled.dir[1], expected_y) < 1.0f);

    // Controller(2) -> Auto(0): non-live active pad drops stale accel and resets clock/bias before wakeup.
    motion_reset();
    g_settings.motion_source = 2;
    SDL_Gamepad pad = {
        .id = 8, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
        .name = "Source Switch Pad", .path = NULL, .rate = 250.0f, .has_gyro = true, .has_accel = true,
    };
    motion_pad_opened(&pad);
    t += 50000000ull;
    push_pad_sensor(8, SDL_SENSOR_ACCEL, 0.0f, 0.70710678f * g, -0.70710678f * g, t, t);
    push_pad_sensor(8, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    assert(m_active_pad_id == 8 && m_pads[0].have_accel && !m_pads[0].live);

    // Switch Controller -> Auto while streaming constant zero gyro: first sample clears m_active_pad_id/clock/accel.
    g_settings.motion_source = 0;
    for (int i = 0; i < 30; i++) {
        t += 33333333ull;
        g_ticks_ns = t;
        SixAxisState s_idle = read_latest();
        assert(fabsf(s_idle.dir[1][2] - 0.70710678f) < 0.05f);  // unchanged, not pulled once reset/non-live
    }
    t += 10000000ull;
    push_pad_sensor(8, SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    assert(m_active_pad_id == 0 && !m_pads[0].have_accel && !m_pads[0].clock.has_prev && !m_pads[0].live);

    // Stream moving samples over ~90ms (<100ms clock gap): clock resets cleanly on sample 20 (live flip), no 90ms dt jump!
    for (int i = 0; i < 20; i++) {
        t += 4500000ull;
        push_pad_sensor(8, SDL_SENSOR_GYRO, 0.60f + 0.01f * (i & 1), 0.0f, 0.0f, t, t);
    }
    assert(m_pads[0].live && m_active_pad_id == 8);
    SixAxisState s_wake = read_latest();
    assert(s_wake.delta_time == 5000000);  // first live sample dt clamped to fresh 5 ms, NOT 90 ms jump!
}

// 15. Nintendo Android (-y,z,-x) + non-zero pad_gyro_axes composition + Auto orphan accel 0.5..2s window.
static void test_nintendo_axes_composition_and_orphan_window(void) {
    g_settings.motion = true;
    g_settings.motion_source = 0;
    g_settings.pad_gyro_axes = 1;  // y,x,z applied AFTER (-y,z,-x) -> (z, -y, -x) on Android

    SDL_Gamepad pad = make_nintendo_android_pad(9);
    motion_pad_opened(&pad);
    uint64_t t = 1000000000ull;
    const float g = 9.80665f;

#ifdef __ANDROID__
    // Raw Nintendo sample (dx=1, dy=2, dz=3): norm = (-2, 3, -1); y,x,z -> pd = (3, -2, -1).
    // Switch gyro = (pd[0], -pd[2], pd[1]) = (3, 1, -2); Switch accel = (-pd[0], pd[2], -pd[1]) = (-3, -1, 2).
    for (int i = 0; i < 22; i++) {
        t += 10000000ull;
        push_pad_sensor(9, SDL_SENSOR_GYRO, 0.6f + 0.01f * (i & 1), 0.0f, 0.0f, t, t);
    }
    assert(m_pads[0].live);
    t += 10000000ull;
    push_pad_sensor(9, SDL_SENSOR_ACCEL, 1.0f * g, 2.0f * g, 3.0f * g, t, t);
    push_pad_sensor(9, SDL_SENSOR_GYRO, 0.2f, 0.4f, 0.6f, t, t);
    // Expected: norm_g=(-0.4, 0.6, -0.2); pd_g=(0.6, -0.4, -0.2) -> Switch g=(0.6, 0.2, -0.4).
    SixAxisState s = read_latest();
    assert(fabsf(s.accel[0] + 3.0f) < 1e-4f && fabsf(s.accel[1] + 1.0f) < 1e-4f && fabsf(s.accel[2] - 2.0f) < 1e-4f);
    assert(fabsf(s.gyro[0] - 0.6f / TURN) < 1e-4f && fabsf(s.gyro[1] - 0.2f / TURN) < 1e-4f &&
           fabsf(s.gyro[2] + 0.4f / TURN) < 1e-4f);
#else
    for (int i = 0; i < 22; i++) {
        t += 10000000ull;
        push_pad_sensor(9, SDL_SENSOR_GYRO, 0.6f + 0.01f * (i & 1), 0.0f, 0.0f, t, t);
    }
#endif

    // Orphan controller ACCEL between 0.5 s and 2.0 s must NOT steal m_last_phone=1 back to m_last_phone=0.
    t += 600000000ull;  // > 500 ms so pad_until_ns expires and phone can take over
    g_display_orientation = SDL_ORIENTATION_LANDSCAPE;
    push_phone_sensor(SDL_SENSOR_ACCEL, g, 0.0f, 0.0f, t, t);
    push_phone_sensor(SDL_SENSOR_GYRO, 0.0f, 0.0f, 0.0f, t, t);
    assert(m_last_phone == 1);
    t += 200000000ull;  // 800 ms since last pad gyro (> 500 ms, < 2000 ms)
    push_pad_sensor(9, SDL_SENSOR_ACCEL, g, 0.0f, 0.0f, t, t);
    assert(m_last_phone == 1);  // still phone!
}

// 16. Legacy 0.4.12 pad_gyro_axes one-time migration end-to-end through real settings_load() / settings_save().
static void make_temp_ini_path(char *buf, size_t cap) {
#ifdef _WIN32
    snprintf(buf, cap, "_test_motion_mig.ini");
#else
    const char *td = getenv("TMPDIR");
    if (!td || !*td || access(td, W_OK) != 0) td = access("/tmp", W_OK) == 0 ? "/tmp" : "/data/local/tmp";
    snprintf(buf, cap, "%s/test_motion_mig_%ld.ini", td, (long)getpid());
#endif
}

static void read_file_str(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    assert(f != NULL);
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
}

static void test_legacy_axes_migration(void) {
    char ini[256];
    make_temp_ini_path(ini, sizeof ini);
    FILE *f = fopen(ini, "w");
    assert(f != NULL);
    fprintf(f, "pad_gyro_axes = 2\n");
    fclose(f);

    setenv("LARECOMP_SETTINGS", ini, 1);
    settings_load();
    assert(g_settings.pad_gyro_axes == 2 && g_settings.pad_gyro_axes_rev == 1);

    // Unrelated settings_save() must NOT stamp pad_gyro_axes_rev = 2 before a Nintendo pad is seen.
    g_settings.volume = 0.5f;
    settings_save();
    char buf[8192];
    read_file_str(ini, buf, sizeof buf);
    assert(strstr(buf, "pad_gyro_axes = 2\n") != NULL);
    assert(strstr(buf, "pad_gyro_axes_rev") == NULL);

    g_settings.motion = true;
    g_settings.motion_source = 2;

    // Non-Nintendo pad keeps its legacy swap and does not bump revision or write rev=2.
    SDL_Gamepad ps5 = {
        .id = 10, .vendor = 0x054c, .product = 0x0ce6, .type = SDL_GAMEPAD_TYPE_PS5,
        .name = "DualSense", .path = NULL, .rate = 250.0f, .has_gyro = true, .has_accel = true,
    };
    motion_pad_opened(&ps5);
    assert(g_settings.pad_gyro_axes == 2 && g_settings.pad_gyro_axes_rev == 1);

    // Nintendo Android pad resets legacy swap (rev < 2) to 0 ("As reported"), sets rev = 2, and saves INI.
    SDL_Gamepad nin = make_nintendo_android_pad(11);
    motion_pad_opened(&nin);
#ifdef __ANDROID__
    assert(g_settings.pad_gyro_axes == 0 && g_settings.pad_gyro_axes_rev == PAD_GYRO_AXES_REV);
    read_file_str(ini, buf, sizeof buf);
    assert(strstr(buf, "pad_gyro_axes = 0\n") != NULL);
    assert(strstr(buf, "pad_gyro_axes_rev = 2\n") != NULL);
#else
    assert(g_settings.pad_gyro_axes == 2 && g_settings.pad_gyro_axes_rev == 1);
#endif

    // User choosing a swap in UI (settings_set_pad_gyro_axes) sets rev = 2 and persists across Nintendo opens.
    settings_set_pad_gyro_axes(2);
    assert(g_settings.pad_gyro_axes == 2 && g_settings.pad_gyro_axes_rev == PAD_GYRO_AXES_REV);
    read_file_str(ini, buf, sizeof buf);
    assert(strstr(buf, "pad_gyro_axes = 2\n") != NULL);
    assert(strstr(buf, "pad_gyro_axes_rev = 2\n") != NULL);
    motion_pad_opened(&nin);
    assert(g_settings.pad_gyro_axes == 2 && g_settings.pad_gyro_axes_rev == PAD_GYRO_AXES_REV);
    remove(ini);
}

// 17. Legacy INI with pad_gyro_axes = 0 loads as rev = 2, saves rev = 2, and resets cleanly via settings_reset_defaults.
static void test_settings_migration_zero_and_reset(void) {
    char ini[256];
    make_temp_ini_path(ini, sizeof ini);
    FILE *f = fopen(ini, "w");
    assert(f != NULL);
    fprintf(f, "pad_gyro_axes = 0\n");
    fclose(f);

    setenv("LARECOMP_SETTINGS", ini, 1);
    settings_load();
    assert(g_settings.pad_gyro_axes == 0 && g_settings.pad_gyro_axes_rev == PAD_GYRO_AXES_REV);
    settings_save();
    char buf[8192];
    read_file_str(ini, buf, sizeof buf);
    assert(strstr(buf, "pad_gyro_axes = 0\n") != NULL);
    assert(strstr(buf, "pad_gyro_axes_rev = 2\n") != NULL);

    settings_set_pad_gyro_axes(3);
    assert(g_settings.pad_gyro_axes == 3 && g_settings.pad_gyro_axes_rev == PAD_GYRO_AXES_REV);
    settings_reset_defaults();
    assert(g_settings.pad_gyro_axes == 0 && g_settings.pad_gyro_axes_rev == PAD_GYRO_AXES_REV);
    remove(ini);
}

static void run_isolated(const char *name, void (*fn)(void)) {
#ifndef _WIN32
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        fn();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: %s (status=%d)\n", name, status);
        exit(1);
    }
#else
    fn();
#endif
    printf("PASS: %s\n", name);
}

int main(void) {
    run_isolated("nintendo_android_axes", test_nintendo_android_axes);
    run_isolated("negative_controls_and_motion_off", test_negative_controls_and_motion_off);
    run_isolated("env_pad_gyro_rollback", test_env_pad_gyro_rollback);
    run_isolated("env_pad_gyro_signed", test_env_pad_gyro_signed);
    run_isolated("env_pad_gyro_invalid_fallback", test_env_pad_gyro_invalid_fallback);
    run_isolated("settings_pad_gyro_axes_and_reset", test_settings_pad_gyro_axes_and_reset);
    run_isolated("clock_skew_1_92x_fixed", test_clock_skew_1_92x_fixed);
    run_isolated("clock_skew_negative_control", test_clock_skew_negative_control);
    run_isolated("clock_scale_non_nintendo_unscaled", test_clock_scale_non_nintendo_unscaled);
    run_isolated("clock_scale_env_opt_in", test_clock_scale_env_opt_in);
    run_isolated("sane_clock_burst_and_wrap", test_sane_clock_burst_and_wrap);
    run_isolated("bias_precision_immunity_and_recovery", test_bias_precision_immunity_and_recovery);
    run_isolated("phone_180_flip_bias_invariance", test_phone_180_flip_bias_invariance);
    run_isolated("env_no_gyro_bias", test_env_no_gyro_bias);
    run_isolated("multi_pad_and_close_isolation", test_multi_pad_and_close_isolation);
    run_isolated("auto_mode_and_nan_guards", test_auto_mode_and_nan_guards);
    run_isolated("sixaxis_underflow_and_reset_persistence", test_sixaxis_underflow_and_reset_persistence);
    run_isolated("phone_accel_only_tilt", test_phone_accel_only_tilt);
    run_isolated("pad_gyro_pause_and_close_fallback", test_pad_gyro_pause_and_close_fallback);
    run_isolated("motion_toggle_and_source_2_to_0", test_motion_toggle_and_source_2_to_0);
    run_isolated("nintendo_axes_composition_and_orphan_window", test_nintendo_axes_composition_and_orphan_window);
    run_isolated("legacy_axes_migration", test_legacy_axes_migration);
    run_isolated("settings_migration_zero_and_reset", test_settings_migration_zero_and_reset);
    puts("PASS: all motion regression tests");
    return 0;
}
