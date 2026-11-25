#!/usr/bin/env python3
"""Exercise production cache identity and SPIR-V framing checks on the PC (NDK + Node)."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'build/shader-cache-tests'
out.mkdir(parents=True, exist_ok=True)
bin_dir = Path(sys.argv[1])
(out / 'test.c').write_text(r'''
#include "shader_cache.h"
int check(void) {
    uint32_t spv[] = {
        0x07230203,0x00010000,0,5,0,
        0x00020011,1, 0x0003000e,0,1,
        0x0005000f,5,3,0x6e69616d,0,
        0x00060010,3,17,1,1,1,
        0x00020013,1, 0x00030021,2,1,
        0x00050036,1,3,0,2, 0x000200f8,4, 0x000100fd, 0x00010038
    };
    size_t n = sizeof spv / 4;
    if (!vk_shader_cache_framed(spv, n)) return 1;
    if (vk_shader_cache_framed(0, n)) return 2;
    if (vk_shader_cache_framed(spv, 5)) return 3;
    spv[0] = 0;
    if (vk_shader_cache_framed(spv, n)) return 4;
    spv[0] = 0x07230203; spv[1] = 0x00010700;
    if (vk_shader_cache_framed(spv, n)) return 5;
    spv[1] = 0;
    if (vk_shader_cache_framed(spv, n)) return 6;
    spv[1] = 0x00010001;
    if (vk_shader_cache_framed(spv, n)) return 7;
    spv[1] = 0x00010600; spv[3] = 0;
    if (vk_shader_cache_framed(spv, n)) return 8;
    spv[3] = 5; spv[4] = 1;
    if (vk_shader_cache_framed(spv, n)) return 9;
    spv[4] = 0; spv[5] = 17;
    if (vk_shader_cache_framed(spv, n)) return 10;
    spv[5] = 0xffff0011;
    if (vk_shader_cache_framed(spv, n)) return 11;
    spv[5] = 0x00020011;
    if (vk_shader_cache_framed(spv, 9)) return 12;
    if (vk_shader_cache_framed(spv, 16u*1024*1024/4+1)) return 13;
    if (vk_shader_source_hash("",0,false) != 0xcff29ce484222325ull) return 14;
    uint64_t normal = vk_shader_source_hash("void main(){}",0,false);
    if (normal == vk_shader_source_hash("void main(){}",0,true)) return 15;
    if (normal == vk_shader_source_hash("void main(){}",1,false)) return 16;
    if (normal == vk_shader_source_hash("void main(){ }",0,false)) return 17;
    if (normal != vk_shader_source_hash_ex("void main(){}",0,false,0)) return 18;
    if (normal == vk_shader_source_hash_ex("void main(){}",0,false,1)) return 19;
    if (vk_shader_source_hash_ex("void main(){}",0,false,1) == vk_shader_source_hash_ex("void main(){}",0,false,2)) return 20;
    if (vk_shader_source_hash_ex("void main(){}",0,false,1) == vk_shader_source_hash_ex("void main(){}",0,false,3)) return 21;
    if (vk_shader_source_hash_ex("void main(){}",0,false,2) == vk_shader_source_hash_ex("void main(){}",0,false,3)) return 22;
    return 0;
}
''')
subprocess.run([str(bin_dir / 'clang.exe'), '--target=wasm32', '-ffreestanding', '-O2', '-nostdlib',
                '-I', str(root / 'runtime/nvn/vulkan'), '-c', str(out / 'test.c'), '-o', str(out / 'test.o')], check=True)
subprocess.run([str(bin_dir / 'ld.lld.exe'), '-flavor', 'wasm', '--no-entry', '--export=check',
                str(out / 'test.o'), '-o', str(out / 'test.wasm')], check=True)
(out / 'run.cjs').write_text('''
const fs = require('fs');
WebAssembly.instantiate(fs.readFileSync(process.argv[2])).then(({instance}) => {
  const code = instance.exports.check();
  if (code) throw new Error(`Shader cache check ${code} failed`);
  console.log('22 shader cache checks passed (production C)');
}).catch(e => {console.error(e); process.exit(1);});
''')
subprocess.run(['node', str(out / 'run.cjs'), str(out / 'test.wasm')], check=True)
