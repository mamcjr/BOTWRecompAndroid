// Read-only telemetry for the paired watch. Offsets are for Switch 1.0.0
// (2094f152836683a3632c6153fd84ab80), verified against the local executable.
// /proc/self/mem reads fail safely across guest unload/reboot; the network
// worker never dereferences a guest pointer or calls a guest function.
#ifdef __ANDROID__
#include <jni.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "hle.h"

extern int g_start_screen;
static uint64_t epoch = 1, sequence;
static int memory_fd = -1;
static pthread_once_t memory_once = PTHREAD_ONCE_INIT;
static void open_memory(void) { memory_fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC); }

RT_ON_RESET(watch_map_reset) {
    __atomic_add_fetch(&epoch, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&sequence, 0, __ATOMIC_RELAXED);
}

static bool read_guest(uint64_t address, void *out, size_t size) {
    if (!g_guest_lo || address < GUEST_LO || address > GUEST_HI - size) return false;
    pthread_once(&memory_once, open_memory);
    return memory_fd >= 0 && pread(memory_fd, out, size, (off_t)address) == (ssize_t)size;
}

static bool read_ptr(uint64_t address, uint64_t *out) {
    return read_guest(address, out, sizeof(*out)) && *out >= GUEST_LO && *out < GUEST_HI;
}

static bool supported_revision(void) {
    static const uint32_t offsets[] = {0x7aefdc, 0x7af038, 0x69535c, 0xd596bc};
    static const uint32_t expected[] = {0xf81e0ff3, 0x91022273, 0xf9461508, 0x9000a976};
    for (unsigned i = 0; i < 4; i++) {
        uint32_t word;
        if (!read_guest(MAIN_BASE + offsets[i], &word, 4) || word != expected[i]) return false;
    }
    return true;
}

typedef struct { int32_t size, capacity; uint64_t data; } WatchArray;

// The live TriggerParam contains FlagProxy values. The backing Flag holds the
// name hash; reading the backing value instead would return a stale reference.
static bool flag_array(uint64_t param, unsigned offset, uint32_t hash, unsigned hash_offset, WatchArray *found) {
    WatchArray outer;
    if (!read_guest(param + offset, &outer, sizeof outer) || outer.size < 1 || outer.size > 64) return false;
    uint64_t arrays[64];
    if (!read_guest(outer.data, arrays, outer.size * 8)) return false;
    for (int i = 0; i < outer.size; i++) {
        WatchArray item;
        uint64_t proxy, backing;
        uint32_t name_hash;
        if (!read_guest(arrays[i], &item, sizeof item) || item.size < 1 || item.size > 200) continue;
        if (!read_ptr(item.data, &proxy) || !read_ptr(proxy + 8, &backing)) continue;
        if (read_guest(backing + hash_offset, &name_hash, 4) && name_hash == hash) {
            *found = item;
            return true;
        }
    }
    return false;
}

static bool valid_position(const float p[3]) {
    return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) &&
           fabsf(p[0]) <= 5000 && fabsf(p[2]) <= 5000 && fabsf(p[1]) < 10000;
}

static bool live_flags(uint64_t *live) {
    uint64_t manager, ref;
    return read_ptr(MAIN_BASE + 0x22df9e8, &manager) && read_ptr(manager + 0xbd8, &ref) && read_ptr(ref, live);
}

// Scalar flags are sorted by unsigned CRC32 in this executable.
static bool scalar_flag(uint64_t live, unsigned offset, uint32_t hash, unsigned hash_offset, uint64_t *result) {
    WatchArray a;
    if (!read_guest(live + offset, &a, sizeof a) || a.size < 1 || a.size > 65536 || a.capacity < a.size) return false;
    int lo = 0, hi = a.size - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint64_t proxy, backing;
        uint32_t candidate;
        if (!read_ptr(a.data + mid * 8, &proxy) || !read_ptr(proxy + 8, &backing) ||
            !read_guest(backing + hash_offset, &candidate, 4)) return false;
        if (candidate == hash) { *result = proxy; return true; }
        if (candidate < hash) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

static uint32_t name_hash(const char *name) {
    uint32_t crc = ~0u;
    for (; *name; name++) {
        crc ^= (unsigned char)*name;
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// Read the selected quest's current indicator list, not the MapApp guide arrays:
// those arrays can retain coordinates from a previously selected quest.
static int objectives_json(char *json, size_t capacity, bool *available) {
    uint64_t live, proxy, manager, quest, step, indicator;
    WatchArray quests, steps;
    int32_t selected, current;
    *available = false;
    json[0] = 0;
    if (!live_flags(&live) || !scalar_flag(live, 0x18, name_hash("QuestIndexSetPointGuide"), 0x1c, &proxy) ||
        !read_guest(proxy + 16, &selected, 4)) return 0;
    if (selected < 0) { *available = true; return 0; }
    if (!read_ptr(MAIN_BASE + 0x22ec060, &manager) || !read_guest(manager + 0xb8, &quests, sizeof quests) ||
        quests.size < 1 || quests.size > 1024 || selected >= quests.size ||
        !read_ptr(quests.data + selected * 8, &quest) || !read_guest(quest + 0x140, &current, 4) ||
        !read_guest(quest + 0xa0, &steps, sizeof steps) || steps.size < 1 || steps.size > 64 ||
        current < 0 || current >= steps.size || !read_ptr(steps.data + current * 8, &step)) return 0;
    if (!read_guest(step + 0x78, &indicator, 8)) return 0;
    if (!indicator) { *available = true; return 0; }
    int used = 0, count = 0;
    for (int group = 0; group < 2; group++) {
        WatchArray actors;
        if (!read_guest(indicator + 8 + group * 32, &actors, sizeof actors) || actors.size < 0 || actors.size > 32) return 0;
        for (int i = 0; i < actors.size; i++) {
            uint64_t actor, key;
            float pos[3];
            char off[128] = {0};
            if (!read_ptr(actors.data + i * 8, &actor) || !read_guest(actor + 0x28, pos, sizeof pos) ||
                !read_ptr(actor + 0x40, &key) || !read_guest(key, off, sizeof off) || !memchr(off, 0, sizeof off)) return 0;
            if (*off) {
                unsigned char done;
                if (!scalar_flag(live, 8, name_hash(off), 0xc, &proxy) || !read_guest(proxy + 16, &done, 1)) return 0;
                if (done & 1) continue;
            }
            if (!valid_position(pos) || count == 32) continue;
            int size = snprintf(json + used, capacity - used, "%s{\"id\":%d,\"x\":%.3f,\"z\":%.3f}",
                                count ? "," : "", group * 32 + i, pos[0], pos[2]);
            if (size < 0 || (size_t)size >= capacity - used) return 0;
            used += size; count++;
        }
    }
    // Selection and step can change while the network worker samples them.
    int32_t selected_again, current_again;
    if (!scalar_flag(live, 0x18, name_hash("QuestIndexSetPointGuide"), 0x1c, &proxy) ||
        !read_guest(proxy + 16, &selected_again, 4) || selected_again != selected ||
        !read_guest(quest + 0x140, &current_again, 4) || current_again != current) return 0;
    *available = true;
    return used;
}

static int markers_json(char *json, size_t capacity, bool *available) {
    uint64_t live;
    WatchArray positions, types;
    *available = false;
    json[0] = 0;
    if (!live_flags(&live)) return 0;
    if (!flag_array(live, 0x108, 0xea9def3f, 0x3c, &positions) ||
        !flag_array(live, 0xa8, 0x9383490e, 0x1c, &types) || positions.size != types.size) return 0;
    uint64_t pos_ptrs[200], type_ptrs[200];
    if (!read_guest(positions.data, pos_ptrs, positions.size * 8) || !read_guest(types.data, type_ptrs, types.size * 8)) return 0;
    int used = 0, count = 0;
    for (int i = 0; i < positions.size; i++) {
        float pos[3];
        int32_t type;
        if (!read_guest(pos_ptrs[i] + 16, pos, sizeof pos) || !read_guest(type_ptrs[i] + 16, &type, 4)) return 0;
        if (type < 0 || type > 63 || !valid_position(pos)) continue;
        int size = snprintf(json + used, capacity - used, "%s{\"id\":%d,\"type\":%d,\"x\":%.3f,\"z\":%.3f}",
                            count ? "," : "", i, type, pos[0], pos[2]);
        if (size < 0 || (size_t)size >= capacity - used) return 0;
        used += size;
        count++;
    }
    *available = true;
    return used;
}

JNIEXPORT jstring JNICALL Java_com_zbotw_recomp_WatchMapServer_nativeMapState(JNIEnv *env, jclass cls) {
    (void)cls;
    const char *unavailable = "{\"version\":1,\"status\":\"unavailable\",\"player\":null,\"markers\":[],\"markersAvailable\":false}";
    if (g_start_screen || g_rt_unwind || !supported_revision()) return (*env)->NewStringUTF(env, unavailable);
    uint64_t generation = __atomic_load_n(&epoch, __ATOMIC_RELAXED), info, actor, text;
    float position[3], matrix[12];
    char map[33] = {0};
    if (!read_ptr(MAIN_BASE + 0x22b2c10, &info) || !read_ptr(info + 0x60, &actor) ||
        !read_guest(info + 0x88, position, sizeof position) || !valid_position(position) ||
        !read_ptr(MAIN_BASE + 0x22e4440 + 8, &text) || !read_guest(text, map, 32)) return (*env)->NewStringUTF(env, unavailable);
    for (int i = 0; i < 32 && map[i]; i++) {
        if (!((map[i] >= 'A' && map[i] <= 'Z') || (map[i] >= 'a' && map[i] <= 'z') ||
              (map[i] >= '0' && map[i] <= '9') || map[i] == '_')) return (*env)->NewStringUTF(env, unavailable);
    }
    if (strcmp(map, "MainField")) {
        char indoor[192];
        snprintf(indoor, sizeof indoor, "{\"version\":1,\"status\":\"interior\",\"map\":\"%s\",\"player\":null,\"markers\":[],\"markersAvailable\":false}", map);
        return (*env)->NewStringUTF(env, indoor);
    }
    char heading[32] = "null";
    if (read_guest(actor + 0x398, matrix, sizeof matrix) &&
        fabsf(matrix[3] - position[0]) < .2f && fabsf(matrix[7] - position[1]) < .2f && fabsf(matrix[11] - position[2]) < .2f &&
        isfinite(matrix[2]) && isfinite(matrix[10]) && fabsf(matrix[2] * matrix[2] + matrix[10] * matrix[10] - 1) < .1f) {
        float angle = atan2f(matrix[2], -matrix[10]) * (180.f / 3.14159265359f);
        if (angle < 0) angle += 360;
        snprintf(heading, sizeof heading, "%.2f", angle);
    }
    char markers[14000], objectives[3000], json[24576];
    bool available, objectives_available;
    int length = markers_json(markers, sizeof markers, &available);
    if (!available) length = 0;
    markers[length] = 0;
    int objectives_length = objectives_json(objectives, sizeof objectives, &objectives_available);
    objectives[objectives_available ? objectives_length : 0] = 0;
    uint64_t again;
    if (g_rt_unwind || g_start_screen || generation != __atomic_load_n(&epoch, __ATOMIC_RELAXED) ||
        !read_ptr(MAIN_BASE + 0x22b2c10, &again) || again != info ||
        !read_ptr(info + 0x60, &again) || again != actor) return (*env)->NewStringUTF(env, unavailable);
    snprintf(json, sizeof json,
        "{\"version\":1,\"session\":\"%llu-%llu\",\"sequence\":%llu,\"sampleAgeMs\":0,\"status\":\"ready\",\"map\":\"MainField\","
        "\"player\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"headingDegrees\":%s},\"markersAvailable\":%s,\"markers\":[%s],"
        "\"objectivesAvailable\":%s,\"objectives\":[%s]}",
        (unsigned long long)getpid(), (unsigned long long)generation,
        (unsigned long long)__atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED),
        position[0], position[1], position[2], heading, available ? "true" : "false", markers,
        objectives_available ? "true" : "false", objectives);
    return (*env)->NewStringUTF(env, json);
}
#endif
