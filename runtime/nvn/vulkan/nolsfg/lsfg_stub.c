// Frame generation left out of this build (CMake -DBOTW_LSFG=OFF): the present's calls answer "not in this build".
// (The LSFG files, runtime/nvn/vulkan/lsfg, are GPL-3.0-or-later; this stub is MIT as the rest.)
#include "../vk.h"
int vk_fg_status(const char **why) {
    if (why) *why = "not in this build";
    return -1;
}
int vk_fg_frame(TexBackend *game, TexBackend **gen, uint64_t *hold_ns, uint64_t vsync_ns) {
    (void)game, (void)gen, (void)vsync_ns;
    *hold_ns = 0;
    return 0;
}
const char *vk_fg_dll_path(void) { return NULL; }
