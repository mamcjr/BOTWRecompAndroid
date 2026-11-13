"""Compile the production command-copy helper with a source-recycling callback.
Usage: python tools/test_command_snapshot.py <NDK llvm bin directory>
Run android/out/test_command_snapshot through adb.
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/nvn/cmdbuf.c').read_text()
start = source.index('static void copy_lists(')
helper = source[start:source.index('// CallCommands too:', start)]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint64_t x[32]; } Cpu;
typedef struct { int unused; } NvnCmdBuf;
typedef struct { uint32_t op, size; uint64_t x[7]; float f[4]; uint8_t extra[]; } CmdRec;
enum { CMD_END, CMD_JUMP, CMD_CALL, CMD_BARRIER, CMD_TEST, CMD_COUNT };
#define FATAL(...) abort()
#define LOG(...) ((void)0)
#define ENV(...) 0
static void hist_report(uint64_t h) { (void)h; }
static CmdRec first[22], continuation[6], later[12], output[64];
static unsigned recorded;
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    (void)c; (void)cb; assert(op == CMD_TEST && extra == 0);
    if (!recorded) {
        // Allocation callback reuses both this handle and a later handle's source memory.
        memset(first, 0xa5, sizeof first);
        memset(continuation, 0xa5, sizeof continuation);
        memset(later, 0xa5, sizeof later);
    }
    assert(recorded < 64);
    return &output[recorded++];
}
'''
suffix = r'''
static void item(CmdRec *r, unsigned value) {
    *r = (CmdRec){.op=CMD_TEST, .size=sizeof(CmdRec)}; r->x[0]=value;
}
int main(void) {
    Cpu c = {0}; NvnCmdBuf cb = {0};
    first[0] = (CmdRec){.op=CMD_BARRIER, .size=sizeof(CmdRec)};
    for (unsigned i=0;i<20;i++) item(&first[i+1],i);
    first[21] = (CmdRec){.op=CMD_JUMP, .size=sizeof(CmdRec)};
    first[21].x[0]=(uintptr_t)continuation;
    for (unsigned i=0;i<5;i++) item(&continuation[i],20+i);
    continuation[5].op=CMD_END;
    later[0] = (CmdRec){.op=CMD_BARRIER, .size=sizeof(CmdRec)};
    for (unsigned i=0;i<10;i++) item(&later[i+1],100+i);
    later[11].op=CMD_END;
    uint64_t handles[]={(uintptr_t)first,(uintptr_t)later};
    copy_lists(&c,&cb,2,handles);
    assert(recorded==35);
    for (unsigned i=0;i<35;i++) {
        assert(output[i].op==CMD_TEST && output[i].size==sizeof(CmdRec));
        assert(output[i].x[0]==(i<25?i:100+i-25));
    }
    copy_lists(&c,&cb,0,NULL); assert(recorded==35);
    puts("PASS: recycled source, multiple lists, jumps, leading barriers, heap snapshot and empty input");
    return 0;
}
'''
out = root / 'android/out'
out.mkdir(parents=True, exist_ok=True)
cfile = out / 'test_command_snapshot.c'
cfile.write_text(prefix + helper + suffix)
subprocess.run([str(Path(sys.argv[1]) / 'clang.exe'), '--target=aarch64-linux-android30',
                '-O1', str(cfile), '-o', str(out / 'test_command_snapshot')], cwd=root, check=True)
