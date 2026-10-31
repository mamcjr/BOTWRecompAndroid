// Maxwell (GM20B) shader translation to Metal Shading Language (macOS) or Vulkan GLSL (Windows; compiled to
// SPIR-V at runtime with shaderc). ShaderKey.glsl selects the dialect; the MSL output does not depend on it.
//
// GLSL contract (what the Vulkan renderer binds):
//  - `#version 460`, entry point `main`. Descriptor set: vertex 0, fragment 1, compute 0.
//  - hardware cbuf k -> binding k: `layout(std140, set=S, binding=k) uniform CB<k> { uvec4 cb<k>[N]; };`
//    (anonymous block). N = max(cbuf_size[k] / 16, highest constant-index vec4 + 1), and at least 4096 when
//    cbuf_dynamic has bit k (the renderer binds up to 64 KB). Register-indexed reads are clamped to N - 1.
//  - global memory (LDG/STG) -> binding GMEM_BUFFER_INDEX (17):
//    `layout(std430, set=S, binding=17) [readonly] buffer GMem { uint w[]; } gmem_;`, 32-bit words relative to
//    maxwell_gmem_base. `readonly` unless the shader stores (needs vertexPipeline/fragmentStoresAndAtomics then);
//    byte / short stores are atomicAnd + atomicOr on the containing word.
//  - rscale table -> binding 18: `uniform RScale { vec4 rscale[65]; };` (std140, the MSL buffer 18 contents).
//  - per-draw tint -> binding 19: `uniform Tint { vec4 ltint[2]; };` (fragment, when uses_tint).
//  - texture slot i (ShaderInfo.tex[i]) -> binding 32 + i, a combined image sampler: sampler1D/2D/3D/Cube/
//    1DArray/2DArray/CubeArray, or its *Shadow variant when tex[i].shadow.
//  - vertex inputs `layout(location=i) in vec4|ivec4|uvec4 a<i>;` (key->attr_kind); varyings: location i =
//    generic attribute i, `vec4 v<i>` (fragment side flat / noperspective as in MSL); `invariant gl_Position`;
//    gl_PointSize when writes_point_size. Fragment outputs `layout(location=rt) out vec4|ivec4|uvec4 c<rt>;`,
//    gl_FragDepth when writes_depth.
//  - clip space exactly as the MSL output (key->flip_y / depth_minus_one epilogue unchanged): the renderer uses
//    a negative-height viewport so Vulkan matches Metal's y-up NDC, clip z in [0, 1].
//  - compute: `layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;` (spec constants 0..2).
//  - extensions: GL_EXT_shader_explicit_arithmetic_types_int64 when maxwell_no_int64 is false;
//    otherwise carry/address math uses 32-bit words without the Int64 SPIR-V capability.
//    GL_KHR_shader_subgroup_basic/_shuffle/_shuffle_relative (SHFL, FSWZADD lane: fragment/compute subgroup
//    support), GL_EXT_texture_shadow_lod (explicit-LOD array / cube shadow lookups).
//  - not in GLSL: the maxwell_dbg_vs position dump (buffer 29) is MSL only.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { STAGE_VERTEX = 0, STAGE_FRAGMENT = 1, STAGE_GEOMETRY = 2, STAGE_TESS_CTRL = 3, STAGE_TESS_EVAL = 4,
       STAGE_COMPUTE = 5 };

enum { TEXTYPE_1D, TEXTYPE_2D, TEXTYPE_3D, TEXTYPE_CUBE, TEXTYPE_2D_ARRAY, TEXTYPE_CUBE_ARRAY, TEXTYPE_1D_ARRAY,
       TEXTYPE_BUFFER, TEXTYPE_2D_MS };

typedef struct {
    uint8_t attr_kind[32];  // vertex inputs: 0 float, 1 signed int, 2 unsigned int
    uint8_t rt_kind[8];     // fragment outputs: 0 float, 1 signed int, 2 unsigned int
    bool depth_minus_one;   // NVN depth mode [-1,1]: remap clip z
    bool flip_y;            // lower-left window origin
    bool flip_x;            // mirror the image left to right (mirror mode's scene composite). (In the padding
                            // after flip_y: keys without it hash as before - shader caches stay valid.)
    bool no_ds_write;       // (Android's copy mode, fragment) the draw writes no depth / stencil: early depth tests are
                            // exact even with a discard (in the padding too)
    uint32_t vs_out_mask;   // fragment: varyings the vertex shader provides
    uint8_t glsl;           // emit Vulkan GLSL (see the contract above) instead of MSL
    bool gmem_linear_lookup;  // Qualcomm compute: cached linear GMap lookup avoids its compiler's binary-search failure
} ShaderKey;

typedef struct {
    uint32_t handle_offset;  // byte offset of the texture handle in the bound texture cbuf
    uint8_t type;            // TEXTYPE_*
    bool shadow, is_array, is_image, bindless;
    uint32_t bindless_cbuf, bindless_offset;
} ShaderTexture;

typedef struct {
    int stage;
    char *msl;              // complete MSL or GLSL source (MSL entry "main0", GLSL "main"); NULL on failure
    uint32_t cbuf_mask;     // hardware constant buffers read
    uint32_t cbuf_size[18];
    uint32_t cbuf_dynamic;  // cbufs indexed with a register: bind the whole buffer
    uint32_t cbwin_mask;    // (maxwell_cb_window) cbufs whose reads may take their rows from the patch buffer
    int ntex;
    ShaderTexture tex[64];
    uint32_t attr_mask;     // vertex: input attributes; fragment: input varyings
    uint32_t out_mask;      // vertex: output varyings; fragment: render targets written
    bool writes_depth, uses_discard, writes_point_size;
    bool uses_gmem;         // global memory access: bind the guest heap at GMEM_BUFFER_INDEX
    bool uses_gmem_map;     // (GLSL compute only) gmem loads look their address up in GMap (uniform, binding
                            // GMAP_BUFFER_INDEX) and read GFast (storage, GFAST_BUFFER_INDEX) - a device-local copy of the
                            // dispatch's storage buffers - when found, stores too (copied back after the dispatch).
                            // runtime/nvn/vulkan/exec.c
    bool uses_gmem_store;   // writes global memory (STG)
    bool uses_tint;        // fragment: reads the per-draw tint (buffer 19, float4[2]) applied to color target 0
    bool uses_rscale;       // reads the resolution-scale table (fragment/compute buffer 18): float4 [0] = render
                            // target scale, [1 + i] = allocation scale of texture i
    uint32_t local_mem;     // bytes of local memory
    uint32_t num_insts;
    char error[256];        // set when translation fails
} ShaderInfo;

// Global memory (LDG/STG) is addressed through one buffer that wraps the guest heap.
#define GMEM_BUFFER_INDEX 17
// (GLSL compute, Vulkan) the fast copy of a dispatch's storage buffers: its address table and its data
#define GMAP_BUFFER_INDEX 20
#define GFAST_BUFFER_INDEX 21
#define GMAP_MAX 32  // ranges in the table (GMap { uvec4 gmap[GMAP_MAX + 1]; }: [i] = addr lo, addr hi, size, word offset;
                     // [GMAP_MAX].x = count). 32: a draw's own ranges and the 16 learned ones (vulkan/exec.c gres_learn)
                     // - at 16 they overflowed after loads, and the draw fell back to the CPU copying it all (#89)
extern uint64_t maxwell_gmem_base, maxwell_gmem_size;
// (GLSL) every stage's global memory through the GMap table, not only compute: set by a backend that can't wrap the
// guest heap in a buffer (Android: no VK_EXT_external_memory_host; vulkan/exec.c copies the bound ranges instead)
extern bool maxwell_gmem_map_all;
// Vulkan drivers without shaderInt64: preserve address/carry semantics using 32-bit words.
extern bool maxwell_no_int64;
// (diagnostics, Android copy mode) a mapped access outside the table records its heap offset at the end of the gfast_
// buffer (the arena: exec.c) instead of reading the heap binding, which isn't there
extern bool maxwell_gmem_miss_log;
extern unsigned maxwell_gmem_miss_stages;  // (the stages that log: bit = STAGE_*; a store in a shader costs - Adreno)
#define GMISS_WORD ((128u << 20) / 4u - 1024u)  // (the arena's last 4 KB: count, then up to 64 offsets)
// ... through a binding of their own, writable (GMiss: those 4 KB): the arena (GFast) stays readonly in a shader that
// only reads - writable, Adreno's fast read path was gone (the lit pass ~4x slower per pixel)
#define GMISS_BUFFER_INDEX 22
// A BRX (jump table) labels only the instructions up to the join pushed before it, not the whole shader (set by the
// Android backend: Adreno's compiler turns every label into a block boundary)
extern bool maxwell_brx_window;
// (GLSL) SSY / PBK / PCNT targets resolved while translating: SYNC / BRK / CONT jump to constants, and the state
// machine has no stack arrays (48 dynamically indexed words: registers or scratch memory on a mobile GPU). Shaders
// where a target isn't the same on every path keep the stacks. Set by the Android backend.
extern bool maxwell_static_stacks;
// (GLSL, with maxwell_static_stacks) shaders with branches as if / else / for instead of the `switch (pc)` state
// machine when their control flow allows it (translate.c structure()). Set by the Android backend.
extern bool maxwell_structured;
// (maxwell_structured) no GLSL switch for a jump table: such a shader keeps its state machine (Qualcomm's compiler)
extern bool maxwell_no_switch;
// (maxwell_structured) a shader with local memory keeps its state machine (Qualcomm's compiler crashed on one)
extern bool maxwell_no_local_struct;
// (GLSL) fragment and compute shaders convert halves with unpackHalf2x16 instead of the bitwise h2f (vertex shaders
// keep it: NVIDIA ran math after unpacked halves in fp16, and depth passes disagreed). Set by the Android backend.
extern bool maxwell_native_half;
// (GLSL vertex / fragment) the game cbufs indexed with a register (3..16) read up to 64 of their 16-byte rows from a
// patch buffer when the draw says so - `uniform CBP { uvec4 cbp_m; uvec4 cbp[64]; };` at CBPATCH_BUFFER_INDEX: cbp_m.x
// the cbuf (index + 1, 0: none), .y its first row there, .z how many. The runtime binds such a buffer in place and
// copies only the rows the game rewrites between draws (vulkan/exec.c cbwin_bind).
extern bool maxwell_cb_window;
#define CBPATCH_BUFFER_INDEX 23
// (GLSL) fmin / fmax / saturate as GLSL min / max / clamp: the GPU returns the non-NaN operand and saturates NaN to 0
// (Adreno, as D3D requires) - what the NaN-testing helpers do. Set by the Android backend.
extern bool maxwell_fast_minmax;
// Debug: when translating the shader whose code starts at maxwell_dbg_code, output register
// maxwell_dbg_reg as captured after the instruction at maxwell_dbg_pc (greyscale, all targets).
extern const uint8_t *maxwell_dbg_code;
extern uint32_t maxwell_dbg_pc;
extern int maxwell_dbg_reg;
// Debug: the vertex shader at maxwell_dbg_vs writes each vertex's position to buffer 18 (slot iid*8+vid).
extern const uint8_t *maxwell_dbg_vs;
#define DBG_VS_BUFFER_INDEX 29

// code: shader binary (with the 0x50-byte SPH header for graphics stages)
ShaderInfo *maxwell_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
// Constants immediately following code, with their actual readable byte count (zero disables table inspection).
ShaderInfo *maxwell_translate_bounded(const uint8_t *code, size_t max_size, size_t const_size, int stage, const ShaderKey *key);
void maxwell_free(ShaderInfo *s);
// Disassemble for debugging (writes text to f).
void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file);
