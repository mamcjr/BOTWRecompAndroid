#!/usr/bin/env python3
"""Compare the production descriptor plan with the legacy two-pass descriptor selection.

Uses Android NDK clang/ld.lld and Node.js; no device or Vulkan driver is used.
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'build' / 'descriptor-plan-tests'
out.mkdir(parents=True, exist_ok=True)
bin_dir = Path(sys.argv[1])
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
start = source.index('typedef struct { uint8_t regular[MAX_BIND]')
helper = source[start:source.index('\n#endif', start)]
prefix = r'''
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
#define MAX_BIND 96
#define GMAP_BUFFER_INDEX 20
#define CBPATCH_BUFFER_INDEX 23
enum { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       UNUSED_STORAGE_IMAGE, UNUSED_SAMPLER };
typedef struct { uint32_t set, binding, type; } VkBindingInfo;
'''
test = r'''
static unsigned rng;
static unsigned next(void) { rng = rng * 1664525u + 1013904223u; return rng; }
int check(unsigned seed, int count, unsigned set) {
    VkBindingInfo binds[MAX_BIND], original[MAX_BIND];
    rng = seed;
    for (int i = 0; i < count; i++) {
        binds[i] = (VkBindingInfo){(next() >> 16) % 3, (next() >> 16) % 100, (next() >> 16) % 6};
        original[i] = binds[i];
    }
    // Include all special bindings and boundary cases regularly, across both sets.
    const unsigned boundary[] = {0, 16, 17, 18, 19, 20, 21, 22, 23, 31, 32, 95};
    for (int i = 0; i < count && i < 12; i++) {
        binds[i].binding = boundary[i]; binds[i].type = seed % 6; binds[i].set = set;
        original[i] = binds[i];
    }
    int legacy[MAX_BIND], n = 0;
    for (int i = 0; i < count; i++) {
        const VkBindingInfo *b = &binds[i];
        if (b->set != set) continue;
        int ubo = b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                  b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) legacy[n++] = i;
        else if (ubo && b->binding < 17) legacy[n++] = i;
        else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) legacy[n++] = i;
    }
    for (int i = 0; i < count; i++) {
        const VkBindingInfo *b = &binds[i];
        int ubo = b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                  b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        if (b->set != set || !ubo ||
            (b->binding != 18 && b->binding != 19 &&
             b->binding != GMAP_BUFFER_INDEX && b->binding != CBPATCH_BUFFER_INDEX)) continue;
        legacy[n++] = i;
    }
    struct { unsigned before; DescriptorPlan plan; unsigned after; } checked;
    checked.before = 0x12345678; checked.after = 0xabcdef01;
    descriptor_plan(binds, count, set, &checked.plan);
    DescriptorPlan *p = &checked.plan;
    if (checked.before != 0x12345678 || checked.after != 0xabcdef01) return 1;
    if (p->nregular + p->ndeferred != n) return 2;
    for (int i = 0; i < n; i++)
        if (legacy[i] != (i < p->nregular ? p->regular[i] : p->deferred[i - p->nregular])) return 3;
    for (int i = 0; i < count; i++)
        if (binds[i].set != original[i].set || binds[i].binding != original[i].binding || binds[i].type != original[i].type) return 4;
    // Rebuilding another set must clear a plan's prior counts.
    descriptor_plan(binds, 0, set, p);
    if (p->nregular || p->ndeferred) return 5;
    return 0;
}
'''
(out / 'test.c').write_text(prefix + helper + test)
subprocess.run([str(bin_dir / 'clang.exe'), '--target=wasm32', '-O2', '-nostdlib', '-c',
                str(out / 'test.c'), '-o', str(out / 'test.o')], check=True)
subprocess.run([str(bin_dir / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', '--export=check',
                str(out / 'test.o'), '-o', str(out / 'test.wasm')], check=True)
(out / 'run.cjs').write_text(r'''
const fs = require('fs');
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({instance}) => {
  let cases = 0;
  for (let seed = 0; seed < 160; seed++) for (let count = 0; count <= 96; count++) for (let set = 0; set < 2; set++) {
    const code = instance.exports.check(seed, count, set);
    if (code) throw new Error(`seed=${seed} count=${count} set=${set} code=${code}`);
    cases++;
  }
  console.log(`${cases} descriptor-plan equivalence cases passed`);
}).catch(e => { console.error(e); process.exit(1); });
''')
subprocess.run(['node', str(out / 'run.cjs'), str(out / 'test.wasm')], check=True)
