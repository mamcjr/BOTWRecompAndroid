// The selected rune and the runes the player has, read from the game's flags (ksys::gdt::Manager in zeldaret/botw,
// gdtManager.h): the touch controls' power button shows it, and their rune fan picks one (ui/touch_controls.mm).
// The manager is a singleton (sInstance: build/find_gdt.py, its 0xdc8-byte allocation; 1.6.0's by the store after its
// inlined constructor). Its flag buffers (TriggerParam*) are a few pointers near its end (1.9.0: +0xbf8 the copy, +0xc00
// the live one): each holds sead::PtrArrays of flags - the bools at +8, the s32s at +0x18 (count, capacity, pointers).
// A FlagS32 has its value at +0x18 and its name's CRC32 at +0x1C; a FlagBool its value at +0xB and hash at +0xC.
// Flags are found by their name's hash, once, and checked again on every read (a reboot rebuilds them):
//   a439d800 (its name unknown): the selected rune - 0 remote bomb, 1 the cube one, 2 magnesis, 3 stasis, 4 cryonis,
//   5 camera, 6 Master Cycle Zero, 7 amiibo (2026-11-02: watched changing with the rune wheel on 1.9.0);
//   IsGet_Obj_<rune>: the runes the player has.
#include <stdbool.h>
#include <stdint.h>

#include "../hle.h"
#include "../platform.h"
#include "../rt.h"

#define RUNE_FLAG 0xa439d800u
static const uint32_t rune_got[8] = {0x191bccf9, 0x3e4a4bd5, 0x795e7bbc, 0x7504085d,  // IsGet_Obj_RemoteBomb, ..Cube,
                                     0x5992b256, 0xf7dd3e03, 0xc9328299, 0x89ebcae4}; // ..Magnetglove, StopTimer,
                                                                                    // IceMaker, Camera, Motorcycle, AmiiboItem

static bool in_window(uint64_t p) { return p >= GUEST_LO && p + 0x1000 < GUEST_HI && !(p & 7); }
#define RD32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define RD64(a) (*(volatile uint64_t *)(uintptr_t)(a))

static uint64_t gdt_instance_ref(void) {
    switch (g_game_version) {
    case 160: return 0x7002cd1700ull;
    case 182: return 0x7001cd9908ull;
    case 190: return 0x7001e3b188ull;
    default: return 0;
    }
}

// The flag of that hash in a TriggerParam's array at +arr (its hash at +hoff): its address, or 0
static uint64_t find_flag(uint64_t tp, int arr, int hoff, uint32_t hash) {
    int32_t n = (int32_t)RD32(tp + arr);
    uint64_t p = RD64(tp + arr + 8);
    if (n <= 0 || n > 400000 || !in_window(p)) return 0;  // (the bools: tens of thousands)
    for (int i = 0; i < n; i++) {
        uint64_t f = RD64(p + 8 * (uint64_t)i);
        if (in_window(f) && RD32(f + hoff) == hash) return f;
    }
    return 0;
}

static uint64_t rune_flag, got_flag[8];
static uint64_t last_try_ms;

// The live flag buffer: the first TriggerParam pointer past +0xb00 whose s32 array holds the rune flag (1.9.0: +0xc00;
// the one at +0xbf8 has other flags)
static bool lookup(uint64_t now_ms) {
    if (now_ms - last_try_ms < 2000) return false;  // (not every frame while there's no game)
    last_try_ms = now_ms;
    uint64_t ref = gdt_instance_ref();
    if (!ref) return false;
    uint64_t inst = RD64(ref - 0x7000000000ull + GUEST_LO);
    if (!in_window(inst)) return false;
    uint64_t found = 0;
    for (int o = 0xb00; o < 0xd80 && !found; o += 8) {
        uint64_t p = RD64(inst + o);
        if (in_window(p) && (rune_flag = find_flag(p, 0x18, 0x1C, RUNE_FLAG))) found = p;
    }
    if (!found) return false;
    for (int i = 0; i < 8; i++) got_flag[i] = find_flag(found, 0x08, 0x0C, rune_got[i]);
    return true;
}

// The selected rune (0-7, as above), or -1 unknown. *owned: a bit per rune the player has (all 8 if unknown).
int gdt_rune(uint32_t *owned) {
    *owned = 0xFF;
    if (!rune_flag || RD32(rune_flag + 0x1C) != RUNE_FLAG) {
        rune_flag = 0;
        if (!lookup(plat_now_ns() / 1000000)) return -1;
    }
    uint32_t m = 0;
    bool all = true;
    for (int i = 0; i < 8; i++) {
        if (!got_flag[i] || RD32(got_flag[i] + 0xC) != rune_got[i]) { all = false; break; }
        if (*(volatile uint8_t *)(uintptr_t)(got_flag[i] + 0xB)) m |= 1u << i;
    }
    if (m & 1) m |= 2;  // (the cube bomb comes with the round one: its own IsGet flag stays false, 2026-11-02)
    if (all && m) *owned = m;
    int32_t v = (int32_t)RD32(rune_flag + 0x18);
    static int32_t last_v = -2;
    static uint32_t last_m;
    if (v != last_v || *owned != last_m) {  // (the log: what the touch controls go by)
        last_v = v, last_m = *owned;
        LOG("[gdt] rune %d, runes %#x%s\n", v, *owned, all ? "" : " (which ones not read: all shown)");
    }
    return v >= 0 && v < 8 ? v : -1;
}
