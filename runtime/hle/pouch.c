// The player's equipped weapon, bow, shield and arrows, read from the game's inventory (uking::ui::PauseMenuDataMgr in
// zeldaret/botw, uiPauseMenuDataMgr.h): the touch controls show their icons (ui/touch_controls.mm).
// The manager is a singleton (sInstance, found by its 0x44808-byte allocation: build/find_pmdm.py). The object holds
// its items' list at +0x68 (a sead::OffsetList: the head's prev / next, its count, then the node's offset in an item:
// 8) and the 420 items' storage at +0x98, 0x298 bytes each. A PouchItem (as the game has it, 2026-11-02): its vtable,
// the list node at +8, mType at +0x18 (0 sword, 1 bow, 2 arrow, 3 shield), mEquipped at +0x24, mName (a
// sead::FixedSafeString<64>) its text at +0x3C.
// The game changes the list on its own threads while this reads it: every node is checked to be one of the storage's
// items before it's read, so a list caught mid-change gives up (false) instead of following a stale pointer.
// 1.6.0, 1.8.2 and 1.9.0 (the same layout); 1.0.0's differs and isn't read. The runes aren't pouch items (its key
// items are the Slate, the champions' abilities, proofs, horse gear): the one selected is elsewhere.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../rt.h"

#define PM_LIST 0x68
#define PM_ITEMS 0x98
#define PM_ITEM_SIZE 0x298
#define PM_NITEMS 420
#define PI_NODE 0x8
#define PI_TYPE 0x18
#define PI_EQUIPPED 0x24
#define PI_NAME 0x3C

static uint64_t pmdm_instance_ref(void) {  // where sInstance is (canonical, 0x7000000000 + offset)
    switch (g_game_version) {
    case 160: return 0x7002ca6d50ull;
    case 182: return 0x7001cb6cf8ull;
    case 190: return 0x7001e18570ull;
    default: return 0;
    }
}

// out[0] the equipped sword's actor name, out[1] the bow's, out[2] the shield's, out[3] the arrows' ("" none). false: not read (no game
// running, another version, the list changing).
bool pouch_equipped(char out[4][64]) {
    for (int i = 0; i < 4; i++) out[i][0] = 0;
    uint64_t ref = pmdm_instance_ref();
    if (!ref) return false;
    uint64_t inst = *(volatile uint64_t *)(uintptr_t)(ref - 0x7000000000ull + GUEST_LO);
    if (inst < GUEST_LO || inst + PM_ITEMS + PM_NITEMS * PM_ITEM_SIZE > GUEST_HI) return false;
    uint64_t head = inst + PM_LIST, lo = inst + PM_ITEMS, hi = lo + PM_NITEMS * PM_ITEM_SIZE;
    if (*(volatile int32_t *)(uintptr_t)(head + 0x14) != PI_NODE) return false;  // (the list's node offset: not this layout)
    uint64_t node = *(volatile uint64_t *)(uintptr_t)(head + 8);
    for (int n = 0; node != head; n++) {
        uint64_t item = node - PI_NODE;
        if (n > PM_NITEMS || item < lo || item >= hi || (item - lo) % PM_ITEM_SIZE) return false;
        int32_t type = *(volatile int32_t *)(uintptr_t)(item + PI_TYPE);
        uint8_t equipped = *(volatile uint8_t *)(uintptr_t)(item + PI_EQUIPPED);
        int slot = type == 0 ? 0 : type == 1 ? 1 : type == 3 ? 2 : type == 2 ? 3 : -1;
        if (equipped && slot >= 0) {
            const char *s = (const char *)(uintptr_t)(item + PI_NAME);
            int k = 0;
            for (; k < 63 && s[k]; k++) {
                char ch = s[k];
                if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
                out[slot][k] = ch;
            }
            out[slot][k] = 0;
        }
        node = *(volatile uint64_t *)(uintptr_t)(node + 8);
    }
    return true;
}
