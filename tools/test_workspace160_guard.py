"""Exercise the production experimental #333 guard on Android, without game files.
Usage: python tools/test_workspace160_guard.py <NDK llvm bin directory>
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/native.c').read_text()
start = source.index('#define WORKSPACE160_GUARDS')
helper = source[start:source.index('// Observe the indirect-call family', start)]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
typedef struct { uint64_t x[32]; } Cpu;
#define FATAL(...) abort()
#define LOG(...) ((void)0)
static atomic_int begins, ends;
void kernel_blocking_begin(void) { atomic_fetch_add(&begins, 1); }
void kernel_blocking_end(void) { atomic_fetch_add(&ends, 1); }
'''
suffix = r'''
static atomic_int inside, ready, release_worker;
static void *contender(void *arg) {
    Cpu c = {0}; c.x[0] = c.x[19] = (uintptr_t)arg;
    atomic_store(&ready, 1);
    fix_workspace160_enter(&c);
    assert(atomic_fetch_add(&inside, 1) == 0);
    assert(atomic_fetch_sub(&inside, 1) == 1);
    fix_workspace160_leave(&c);
    return NULL;
}
static void *independent(void *arg) {
    Cpu c = {0}; c.x[0] = c.x[19] = (uintptr_t)arg;
    fix_workspace160_enter(&c);
    atomic_store(&ready, 1);
    while (!atomic_load(&release_worker)) usleep(100);
    fix_workspace160_leave(&c);
    return NULL;
}
int main(void) {
    Cpu c = {0}; c.x[0] = c.x[19] = 0x3457954a0ULL;
    fix_workspace160_enter(&c);
    fix_workspace160_enter(&c); // nested same-thread owner remains legal
    fix_workspace160_leave(&c);
    atomic_store(&inside, 1);
    pthread_t th; assert(!pthread_create(&th, NULL, contender, (void *)(uintptr_t)c.x[0]));
    while (!atomic_load(&ready) || !atomic_load(&begins)) usleep(100);
    assert(atomic_load(&inside) == 1);
    atomic_store(&inside, 0);
    fix_workspace160_leave(&c);
    pthread_join(th, NULL);
    assert(atomic_load(&begins) == atomic_load(&ends));
    assert(atomic_load(&begins) > 0);
    uint64_t other = c.x[0] + 16;
    assert(workspace160_guard(other) != workspace160_guard(c.x[0]));
    fix_workspace160_enter(&c);
    atomic_store(&ready, 0);
    assert(!pthread_create(&th, NULL, independent, (void *)(uintptr_t)other));
    while (!atomic_load(&ready)) usleep(100); // unrelated owner runs while ours is held
    atomic_store(&release_worker, 1);
    pthread_join(th, NULL);
    fix_workspace160_leave(&c);
    puts("PASS: same-owner exclusion, recursive entry, independent owners and balanced blocking hooks");
    return 0;
}
'''
out = root / 'android/out'
out.mkdir(parents=True, exist_ok=True)
cfile = out / 'test_workspace160_guard.c'
cfile.write_text(prefix + helper + suffix)
subprocess.run([str(Path(sys.argv[1]) / 'clang.exe'), '--target=aarch64-linux-android30',
                '-O1', str(cfile), '-o', str(out / 'test_workspace160_guard')], check=True)
