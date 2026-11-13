"""Fault-inject the production pipeline-cache save function; compile for Android.
Usage: python tools/test_pipeline_cache_save.py <NDK llvm bin directory>
Run android/out/test_pipeline_cache_save through adb (no game files required).
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/nvn/vulkan/device.c').read_text()
start = source.index('void vk_pipeline_cache_save(void)')
function = source[start:source.index('// ================================================================== swapchain', start)]
prefix = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn/vulkan/device_compat.h"
static struct { VkDevice dev; VkPipelineCache pcache; } g_vk;
static int queries, allocations, opens, fail_alloc, incomplete;
static size_t cache_size;
static void *test_malloc(size_t n) { allocations++; return fail_alloc ? NULL : malloc(n); }
static FILE *test_open(const char *a, const char *b) { (void)a; (void)b; opens++; return NULL; }
static int test_mkdir(const char *a, int b) { (void)a; (void)b; return 0; }
static VkResult test_get(VkDevice d, VkPipelineCache c, size_t *n, void *p) {
    (void)d; (void)c; queries++;
    if (!p) { *n = cache_size; return VK_SUCCESS; }
    if (incomplete) { *n = cache_size + 64; return VK_INCOMPLETE; }
    memset(p, 0, *n); return VK_SUCCESS;
}
#define vkGetPipelineCacheData test_get
#define malloc test_malloc
#define fopen test_open
#define mkdir test_mkdir
#define LOG(...) ((void)0)
'''
suffix = r'''
static void reset(void) {
    queries = allocations = opens = fail_alloc = incomplete = 0;
    cache_size = 64;
    g_vk.pcache = (VkPipelineCache)(uintptr_t)1;
}
int main(void) {
    reset(); g_vk.pcache = VK_NULL_HANDLE; vk_pipeline_cache_save(); assert(!queries && !allocations && !opens);
    reset(); fail_alloc = 1; vk_pipeline_cache_save(); assert(queries == 1 && allocations == 1 && !opens);
    reset(); cache_size = VK_BACKEND_PIPELINE_CACHE_MAX + 1; vk_pipeline_cache_save(); assert(queries == 1 && !allocations && !opens);
    reset(); incomplete = 1; vk_pipeline_cache_save(); assert(queries == 2 && allocations == 1 && !opens);
    reset(); vk_pipeline_cache_save(); assert(queries == 2 && allocations == 1 && opens == 1);
    puts("PASS: disabled cache, OOM, size limit, growing cache and valid snapshot");
    return 0;
}
'''
out = root / 'android/out'
out.mkdir(parents=True, exist_ok=True)
cfile = out / 'test_pipeline_cache_save.c'
cfile.write_text(prefix + function + suffix)
subprocess.run([str(Path(sys.argv[1]) / 'clang.exe'), '--target=aarch64-linux-android30',
                '-O1', '-Iruntime', '-Iandroid/deps/out/include', str(cfile),
                '-o', str(out / 'test_pipeline_cache_save')], cwd=root, check=True)
