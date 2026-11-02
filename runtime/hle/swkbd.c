// Software keyboard (naming Link on a new file, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; our am service has no applets, so the game crashed. ShowKeyboard is replaced here: the overlay
// (runtime/ui/menu.mm menu_keyboard_run) asks for the text - typed directly with a keyboard, an on-screen keyboard
// with a controller - while this (game) thread keeps re-presenting the last frame.
#include <stdio.h>
#include <string.h>

#include "../hle.h"

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);  // runtime/ui/menu.mm

// The next code point of UTF-8 s (advanced past it); a stray byte reads as itself
static uint32_t utf8_next(const char **s) {
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t cp = *p++;
    int more = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
    if (more) cp &= 0x3f >> more;
    for (; more && (*p & 0xc0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3f);
    *s = (const char *)p;
    return cp;
}

static int utf8_put(uint32_t cp, char *o) {  // (bytes written)
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6), o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xe0 | cp >> 12), o[1] = (char)(0x80 | (cp >> 6 & 0x3f)), o[2] = (char)(0x80 | (cp & 0x3f)); return 3; }
    o[0] = (char)(0xf0 | cp >> 18), o[1] = (char)(0x80 | (cp >> 12 & 0x3f)), o[2] = (char)(0x80 | (cp >> 6 & 0x3f));
    o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}
// ShowKeyboardArg (probed 2026-10-08 in Link's Awakening, naming Link; yuzu's SwkbdConfigCommon has the same layout): +0x3ac text max
// length, +0x3b0 min length, +0x3bd isUseUtf8, +0x3c0 / +0x3c4 initial text offset / length (UTF-16) in the work
// buffer at +0x4d0. The answer is UTF-8 when isUseUtf8 is set (that naming), else UTF-16: BOTW's horse names read
// our UTF-8 as UTF-16 - one letter came through, "So" became U+6F53 (shown as nothing) and Cyrillic garbage (#165).
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out), size = LD64(out + 8);
    int max_len = (int)*(const int32_t *)(uintptr_t)(arg + 0x3ac);
    bool utf8 = *(const uint8_t *)(uintptr_t)(arg + 0x3bd) != 0;
    uint32_t off = *(const uint32_t *)(uintptr_t)(arg + 0x3c0), n = *(const uint32_t *)(uintptr_t)(arg + 0x3c4);
    uint64_t wb = LD64(arg + 0x4d0);
    char initial[96] = "";
    int k = 0;
    for (uint32_t i = 0; wb && i < n && k < (int)sizeof initial - 5; i++) {
        uint32_t cp = *(const uint16_t *)(uintptr_t)(wb + off + 2 * i);
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < n) {  // (a surrogate pair)
            uint32_t lo = *(const uint16_t *)(uintptr_t)(wb + off + 2 * (i + 1));
            if (lo >= 0xdc00 && lo < 0xe000) cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00), i++;
        }
        k += utf8_put(cp, initial + k);
    }
    initial[k] = 0;
    char text[96];
    int ok = menu_keyboard_run(initial, max_len, text, sizeof text);
    if (!ok) snprintf(text, sizeof text, "%s", initial);  // cancelled: keep what was there
    LOG("[swkbd] name entry (max %d, %s, initial \"%s\"): %s \"%s\"\n", max_len, utf8 ? "UTF-8" : "UTF-16", initial,
        ok ? "entered" : "cancelled", text);
    if (ptr && size) {
        memset((void *)(uintptr_t)ptr, 0, size);
        if (utf8) snprintf((char *)(uintptr_t)ptr, size, "%s", text);
        else {
            uint16_t *o = (uint16_t *)(uintptr_t)ptr;
            size_t cap = size / 2 - 1, m = 0;  // (UTF-16 units, the terminator left)
            for (const char *s = text; *s && m < cap;) {
                uint32_t cp = utf8_next(&s);
                if (cp < 0x10000) o[m++] = (uint16_t)cp;
                else if (m + 1 < cap) cp -= 0x10000, o[m++] = (uint16_t)(0xd800 + (cp >> 10)), o[m++] = (uint16_t)(0xdc00 + (cp & 0x3ff));
                else break;
            }
        }
    }
    c->x[0] = 0;
}
