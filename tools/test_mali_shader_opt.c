// Unit and corpus tests for Mali shader/SPIR-V optimizations (Phase 1):
//   - mali_shader_opt.h policy gating (!BC && ASTC, independent env overrides)
//   - Maxwell -> GLSL cleanup & discard passes in translate.c (STAGE_VERTEX & MSL invariance)
//   - Linear SPIR-V cleanup & OpDemoteToHelperInvocation -> OpTerminateInvocation passes + negative controls
//   - Batch mode: test_mali_shader_opt --batch <in_dir> <out_dir>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "../runtime/nvn/maxwell/shader.h"
#include "../runtime/nvn/vulkan/mali_shader_opt.h"
#include "../runtime/nvn/vulkan/spirv_opt_mali.h"

static void put_u32_le(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put_u64_le(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

// Build a synthetic NVN shader binary (0x30 NVN hdr + 0x50 SPH + code at 0x80).
static size_t build_nv_shader(uint8_t *buf, size_t cap, uint32_t omap_target, bool omap_depth,
                              uint32_t nopersp_mask, const uint64_t *insns, size_t n_insns) {
    memset(buf, 0, cap);
    put_u32_le(buf + 0x30 + 0x48, omap_target);
    put_u32_le(buf + 0x30 + 0x4C, omap_depth ? 2u : 0u);
    for (int i = 0; i < 32; i++) {
        if ((nopersp_mask >> i) & 1u) buf[0x30 + 0x18 + i] = 0xFFu;
    }
    size_t slot = 0;
    for (size_t k = 0; k < n_insns; k++) {
        if (slot % 4 == 0) {
            put_u64_le(buf + 0x80 + slot * 8, 0x001fc00000000000ull);
            slot++;
        }
        put_u64_le(buf + 0x80 + slot * 8, insns[k]);
        slot++;
    }
    if (slot % 4 == 0) {
        put_u64_le(buf + 0x80 + slot * 8, 0x001fc00000000000ull);
        slot++;
    }
    put_u64_le(buf + 0x80 + slot * 8, 0xe300000000070000ull);  // EXIT
    slot++;
    if (slot % 4 == 0) {
        put_u64_le(buf + 0x80 + slot * 8, 0x001fc00000000000ull);
        slot++;
    }
    put_u64_le(buf + 0x80 + slot * 8, 0xe240000000000000ull | (0xFFFFF8ull << 20) | (7ull << 16) | 15ull);  // BRA -8 (self)
    slot++;
    return 0x80 + slot * 8 + 64;
}

static void test_policy(void) {
    const char *why = NULL;
    // Turnip / desktop: has_bc = true -> always off even with env = "1"
    assert(!vk_mali_spirv_cleanup_want(true, true, NULL, &why));
    assert(!vk_mali_spirv_cleanup_want(true, true, "1", &why));
    assert(!vk_mali_discard_opt_want(true, true, NULL, &why));
    assert(!vk_mali_discard_opt_want(true, true, "1", &why));
    // Missing ASTC -> off
    assert(!vk_mali_spirv_cleanup_want(false, false, "1", &why));
    assert(!vk_mali_discard_opt_want(false, false, "1", &why));
    // Mali (!BC && ASTC) default ON, independent env override OFF/ON
    assert(vk_mali_spirv_cleanup_want(false, true, NULL, &why));
    assert(!vk_mali_spirv_cleanup_want(false, true, "0", &why));
    assert(vk_mali_spirv_cleanup_want(false, true, "1", &why));
    assert(vk_mali_discard_opt_want(false, true, NULL, &why));
    assert(!vk_mali_discard_opt_want(false, true, "0", &why));
    assert(vk_mali_discard_opt_want(false, true, "1", &why));
}

static void test_translate_cleanups_and_discard(void) {
    uint8_t blob[4096];
    // Test 1: dead .CC, IPA.MUL 1/w fold, FSETP PT/PT, as_u/as_f removal, entry KIL hoisting,
    //         noperspective preservation, STAGE_VERTEX invariance, and MSL byte-identity.
    //   IPA r0, a[0x7c] (mode=0): 0xe00000000000ff00 | (0x7c<<28) | (PT<<16) | 0 -> r0 = pos.w
    //   MUFU.RCP r1, r0 (fn=4):   0x5080000000000000 | (4<<20) | (PT<<16) | (0<<8) | 1
    //   IPA.MUL r2, a[0x80], r1:  0xe00000000000ff00 | (1<<54) | (0x80<<28) | (1<<20) | (PT<<16) | 2
    //   IPA.MUL r3, a[0x90], r1 (noperspective): must keep in.v1[0] * uintBitsToFloat/as_type(r1)
    //   IADD.CC r4, r2, r3:       0x5c10000000000000 | (1<<47) | (PT<<16) | (3<<20) | (2<<8) | 4
    //   FSETP.LT PT, PT, r4, r4:  0x5bb0000000000000 | (1<<48) | (PT<<16) | (4<<20) | (4<<8) | (PT<<3) | PT
    //   @p0 KIL:                  0xe330000000000000 | (0<<16)
    const uint64_t code1[] = {
        0xe00000000000ff00ull | (0x7cull << 28) | (7ull << 16) | 0ull,
        0x5080000000000000ull | (4ull << 20) | (7ull << 16) | (0ull << 8) | 1ull,
        0xe00000000000ff00ull | (1ull << 54) | (0x80ull << 28) | (1ull << 20) | (7ull << 16) | 2ull,
        0xe00000000000ff00ull | (1ull << 54) | (0x90ull << 28) | (1ull << 20) | (7ull << 16) | 3ull,
        0x5c10000000000000ull | (1ull << 47) | (7ull << 16) | (3ull << 20) | (2ull << 8) | 4ull,
        0x5bb0000000000000ull | (1ull << 48) | (7ull << 16) | (4ull << 20) | (4ull << 8) | (7ull << 3) | 7ull,
        0xe330000000000000ull | (0ull << 16),
    };
    size_t sz = build_nv_shader(blob, sizeof blob, 0xF, false, (1u << 1), code1, sizeof code1 / sizeof code1[0]);

    // MSL invariance (both VS & FS)
    ShaderKey mkey = {.glsl = 0, .vs_out_mask = 3};
    maxwell_mali_cleanup = false; maxwell_mali_discard_opt = false;
    ShaderInfo *msl_base = maxwell_translate_bounded(blob, sz, 0, STAGE_FRAGMENT, &mkey);
    maxwell_mali_cleanup = true; maxwell_mali_discard_opt = true;
    ShaderInfo *msl_opt = maxwell_translate_bounded(blob, sz, 0, STAGE_FRAGMENT, &mkey);
    assert(msl_base->msl && msl_opt->msl && strcmp(msl_base->msl, msl_opt->msl) == 0);
    maxwell_free(msl_base);
    maxwell_free(msl_opt);

    // STAGE_VERTEX GLSL must stay byte-identical even if flags are set (preserves depth pre-pass invariance).
    // Note: maxwell_translate_bounded reads attrs via LD/ST attr (0xefd8/0xeff0); include LD attr + IADD.CC + ST attr(gl_Position.x).
    const uint64_t vs_code[] = {
        0xefd800000000ff00ull | (0x80ull << 20) | (7ull << 16) | 0ull,                             // LD a[0x80], r0 (ia=RZ)
        0x5c10000000000000ull | (1ull << 47) | (7ull << 16) | (0ull << 20) | (0ull << 8) | 2ull,  // IADD.CC r2, r0, r0
        0xeff0000000000000ull | (0x70ull << 20) | (7ull << 16) | 2ull,                             // ST a[0x70], r2
    };
    size_t vsz = build_nv_shader(blob, sizeof blob, 0, false, 0, vs_code, sizeof vs_code / sizeof vs_code[0]);
    ShaderKey vs_key = {.glsl = 1};
    maxwell_mali_cleanup = false; maxwell_mali_discard_opt = false;
    ShaderInfo *vs_base = maxwell_translate_bounded(blob, vsz, 0, STAGE_VERTEX, &vs_key);
    maxwell_mali_cleanup = true; maxwell_mali_discard_opt = true;
    ShaderInfo *vs_opt = maxwell_translate_bounded(blob, vsz, 0, STAGE_VERTEX, &vs_key);
    assert(vs_base->msl && vs_opt->msl && strcmp(vs_base->msl, vs_opt->msl) == 0);
    assert(strstr(vs_opt->msl, "invariant gl_Position;\nprecise gl_Position;") != NULL);
    maxwell_free(vs_base);
    maxwell_free(vs_opt);

    sz = build_nv_shader(blob, sizeof blob, 0xF, false, (1u << 1), code1, sizeof code1 / sizeof code1[0]);
    ShaderKey gkey = {.glsl = 1, .flip_y = true, .vs_out_mask = 3};
    maxwell_static_stacks = true; maxwell_structured = true;
    maxwell_mali_cleanup = false; maxwell_mali_discard_opt = false;
    ShaderInfo *gl_base = maxwell_translate_bounded(blob, sz, 0, STAGE_FRAGMENT, &gkey);
    maxwell_mali_cleanup = true; maxwell_mali_discard_opt = true;
    ShaderInfo *gl_opt = maxwell_translate_bounded(blob, sz, 0, STAGE_FRAGMENT, &gkey);
    assert(gl_base->msl && gl_opt->msl);
    assert(strstr(gl_base->msl, "uint cc = 0u") != NULL);
    assert(strstr(gl_opt->msl, "uint cc = 0u") == NULL);  // dead .CC removed
    assert(strstr(gl_opt->msl, "as_u(") == NULL);         // no as_u function calls
    assert(strstr(gl_opt->msl, "as_f(") == NULL);         // no as_f function calls
    assert(strstr(gl_opt->msl, "v0[0] * gl_FragCoord.w") == NULL);  // IPA.MUL 1/w collapsed
    assert(strstr(gl_opt->msl, "v1[0] * as_type<float>(r1)") != NULL ||
           strstr(gl_opt->msl, "v1[0] * uintBitsToFloat(r1)") != NULL);  // noperspective preserved!
    // Entry KIL hoisted above independent ALU (discard appears before v0[0])
    const char *p_disc = strstr(gl_opt->msl, "discard;");
    const char *p_v0 = strstr(gl_opt->msl, "v0[0]");
    assert(p_disc && p_v0 && p_disc < p_v0);
    maxwell_free(gl_base);
    maxwell_free(gl_opt);

    // Test 2: Mysterious Woods mist special case: IPA.MUL a[0xc0], r5 where r5 = 1/(v4.w * pos.w), NOT 1/pos.w!
    // Must keep `* in.pos.w * uintBitsToFloat(r5)` exact and never collapse to `in.v4[0]`.
    const uint64_t code_mist[] = {
        0xe00000000000ff00ull | (0x7cull << 28) | (7ull << 16) | 0ull,                                // IPA r0 = pos.w
        0xe00000000000ff00ull | (0xccull << 28) | (7ull << 16) | 1ull,                                // IPA r1 = a[0xcc] = v4.w / pos.w
        0x5c68000000000000ull | (7ull << 16) | (1ull << 20) | (0ull << 8) | 2ull,                     // FMUL r2, r0, r1 (= v4.w * pos.w...)
        0x5080000000000000ull | (4ull << 20) | (7ull << 16) | (2ull << 8) | 5ull,                     // MUFU.RCP r5, r2
        0xe00000000000ff00ull | (1ull << 54) | (0xc0ull << 28) | (5ull << 20) | (7ull << 16) | 3ull,  // IPA.MUL r3, a[0xc0], r5
    };
    size_t msz = build_nv_shader(blob, sizeof blob, 0xF, false, 0, code_mist, sizeof code_mist / sizeof code_mist[0]);
    ShaderKey mfkey = {.glsl = 1, .vs_out_mask = 0x3Fu};
    ShaderInfo *gl_mist = maxwell_translate_bounded(blob, msz, 0, STAGE_FRAGMENT, &mfkey);
    assert(gl_mist->msl && strstr(gl_mist->msl, "v4[0] * gl_FragCoord.w") != NULL);
    maxwell_free(gl_mist);

    // Test 2b: TEXS overwriting r1 via secondary destination d1 invalidates rcp_w[r1],
    // so subsequent IPA.MUL a[0x80], r1 must NOT drop `* gl_FragCoord.w`.
    const uint64_t code_texs_clobber[] = {
        0xe00000000000ff00ull | (0x7cull << 28) | (7ull << 16) | 0ull,                                // IPA r0 = pos.w
        0x5080000000000000ull | (4ull << 20) | (7ull << 16) | (0ull << 8) | 1ull,                     // MUFU.RCP r1, r0
        0xd000000000000000ull | (1ull << 59) | (1ull << 53) | (4ull << 50) |
            (1ull << 28) | (5ull << 20) | (7ull << 16) | (4ull << 8) | 6ull,                          // TEXS d0=r6, d1=r1, info=2D
        0xe00000000000ff00ull | (1ull << 54) | (0x80ull << 28) | (1ull << 20) | (7ull << 16) | 3ull,  // IPA.MUL r3, a[0x80], r1
    };
    size_t tsz = build_nv_shader(blob, sizeof blob, 0xF, false, 0, code_texs_clobber,
                                 sizeof code_texs_clobber / sizeof code_texs_clobber[0]);
    ShaderInfo *gl_texs = maxwell_translate_bounded(blob, tsz, 0, STAGE_FRAGMENT, &mfkey);
    assert(gl_texs->msl && strstr(gl_texs->msl, "v0[0] * gl_FragCoord.w") != NULL);
    maxwell_free(gl_texs);

    // Test 3: unreachable KIL is pruned -> uses_discard == false
    uint64_t code_unreach[3] = {
        0xe240000000000000ull | (8ull << 20) | (7ull << 16) | 15ull,  // BRA +8 -> EXIT
        0xe330000000000000ull | (7ull << 16),                         // unreachable KIL
        0xe300000000070000ull,                                        // EXIT
    };
    sz = build_nv_shader(blob, sizeof blob, 0xF, false, 0, code_unreach, 3);
    ShaderInfo *gl_u = maxwell_translate_bounded(blob, sz, 0, STAGE_FRAGMENT, &gkey);
    assert(!gl_u->uses_discard && strstr(gl_u->msl, "discard;") == NULL);
    maxwell_free(gl_u);
    maxwell_mali_cleanup = false; maxwell_mali_discard_opt = false;
}

static void test_spirv_passes_synthetic(void) {
    // Minimal valid SPIR-V 1.6 fragment shader:
    //   - Scalar Function var %v: dead overwrite + store-to-load forwarding + bitcast round-trip fold
    //   - SelectionMerge -> %then (OpDemoteToHelperInvocation; FAdd in tail; OpBranch %merge) + %else
    //   - %merge: OpPhi (%21 %then, %11 %else), OpReturn
    uint32_t raw[] = {
        0x07230203u, 0x00010600u, 0u, 32u, 0u,
        (2u << 16) | 17u, 1u,                       // OpCapability Shader
        (2u << 16) | 17u, 5379u,                    // OpCapability DemoteToHelperInvocation
        (6u << 16) | 11u, 1u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0u, // GLSL.std.450
        (3u << 16) | 14u, 0u, 1u,                   // OpMemoryModel Logical GLSL450
        (5u << 16) | 15u, 4u, 2u, 0x6e69616du, 0u,  // OpEntryPoint Fragment %2 "main"
        (3u << 16) | 16u, 2u, 7u,                   // OriginUpperLeft
        (2u << 16) | 19u, 3u,                       // %3 = void
        (3u << 16) | 33u, 4u, 3u,                   // %4 = fn void()
        (2u << 16) | 20u, 5u,                       // %5 = bool
        (4u << 16) | 21u, 6u, 32u, 0u,              // %6 = uint
        (3u << 16) | 22u, 7u, 32u,                  // %7 = float
        (4u << 16) | 32u, 8u, 7u, 6u,               // %8 = ptr Function uint
        (3u << 16) | 41u, 5u, 9u,                   // %9 = true
        (4u << 16) | 43u, 6u, 10u, 0x3f800000u,     // %10 = uint(1.0f)
        (4u << 16) | 43u, 7u, 11u, 0x3f800000u,     // %11 = float 1.0f
        (4u << 16) | 43u, 7u, 12u, 0x00000000u,     // %12 = float 0.0f
        (5u << 16) | 54u, 3u, 2u, 0u, 4u,           // %2 = OpFunction
        (2u << 16) | 248u, 13u,                     // %13 = OpLabel
        (4u << 16) | 59u, 8u, 14u, 7u,              // %14 = OpVariable Function uint
        (3u << 16) | 62u, 14u, 10u,                 // OpStore %14 %10 (dead store)
        (4u << 16) | 124u, 6u, 15u, 11u,            // %15 = OpBitcast uint %11
        (3u << 16) | 62u, 14u, 15u,                 // OpStore %14 %15
        (4u << 16) | 61u, 6u, 16u, 14u,             // %16 = OpLoad uint %14 -> %15
        (4u << 16) | 124u, 7u, 17u, 16u,            // %17 = OpBitcast float %16 -> %11
        (3u << 16) | 247u, 20u, 0u,                 // OpSelectionMerge %20 None
        (4u << 16) | 250u, 9u, 18u, 19u,            // OpBranchConditional %9 %18 %19
        (2u << 16) | 248u, 18u,                     // %18 = OpLabel
        (1u << 16) | 5380u,                         // OpDemoteToHelperInvocation
        (5u << 16) | 129u, 7u, 21u, 17u, 12u,       // %21 = FAdd float %17 %12 (in tail!)
        (2u << 16) | 249u, 20u,                     // OpBranch %20
        (2u << 16) | 248u, 19u,                     // %19 = OpLabel
        (2u << 16) | 249u, 20u,                     // OpBranch %20
        (2u << 16) | 248u, 20u,                     // %20 = OpLabel
        (7u << 16) | 245u, 7u, 22u, 21u, 18u, 11u, 19u, // %22 = OpPhi %7 (%21 %18, %11 %19)
        (1u << 16) | 253u,                          // OpReturn
        (1u << 16) | 56u,                           // OpFunctionEnd
    };
    size_t nw = sizeof raw / 4;
    uint32_t *buf = malloc(sizeof raw);
    memcpy(buf, raw, sizeof raw);
    assert(spirv_mali_demote_to_terminate(&buf, &nw));
    assert(spirv_mali_cleanup(&buf, &nw));
    assert(mali_spv_validate_stream(buf, nw, NULL));

    bool saw_term = false, saw_demote = false, saw_cap_demote = false, saw_bitcast = false, saw_var = false;
    for (size_t p = 5; p < nw;) {
        uint32_t wc = buf[p] >> 16;
        uint16_t op = (uint16_t)(buf[p] & 0xFFFFu);
        if (op == MALI_SPV_OP_TERMINATE_INVOCATION) saw_term = true;
        if (op == MALI_SPV_OP_DEMOTE_TO_HELPER) saw_demote = true;
        if (op == MALI_SPV_OP_CAPABILITY && buf[p + 1] == MALI_SPV_OP_CAP_DEMOTE) saw_cap_demote = true;
        if (op == MALI_SPV_OP_BITCAST) saw_bitcast = true;
        if (op == MALI_SPV_OP_VARIABLE) saw_var = true;
        p += wc;
    }
    assert(saw_term && !saw_demote && !saw_cap_demote && !saw_bitcast && !saw_var);
    free(buf);
}

static size_t count_spv_op(const uint32_t *w, size_t nw, uint16_t target_op) {
    size_t c = 0;
    for (size_t p = 5; p < nw;) {
        uint32_t wc = w[p] >> 16;
        if ((uint16_t)(w[p] & 0xFFFFu) == target_op) c++;
        p += wc;
    }
    return c;
}

static void test_spirv_negative_controls_and_bounds(void) {
    // 1. Degenerate / empty / malformed buffers must fail safely without OOB reads.
    uint32_t empty5[5] = {0x07230203u, 0x00010600u, 0u, 8u, 0u};
    size_t nw5 = 5;
    uint32_t *p5 = malloc(sizeof empty5);
    memcpy(p5, empty5, sizeof empty5);
    assert(!spirv_mali_demote_to_terminate(&p5, &nw5));
    assert(!spirv_mali_cleanup(&p5, &nw5));
    free(p5);
    uint32_t trunc_hdr[3] = {0x07230203u, 0x00010600u, 0u};
    size_t nwt = 3;
    uint32_t *pt = malloc(sizeof trunc_hdr);
    memcpy(pt, trunc_hdr, sizeof trunc_hdr);
    assert(!spirv_mali_demote_to_terminate(&pt, &nwt));
    assert(!spirv_mali_cleanup(&pt, &nwt));
    free(pt);
    assert(!spirv_mali_cleanup(NULL, NULL));

    // 2. Intra-block cleanup negative controls:
    //    (a) Full reset on every OpLabel: store in %16 must NOT forward into merge block %20.
    //    (b) Full reset on OpFunctionCall: store to %33 before Call %34 must NOT forward to Load %35 after Call.
    //    (c) vec4 temp promotion turns Store vec4 -> AccessChain -> Load into OpCompositeExtract
    //        and removes dead vec4 variable %26.
    uint32_t mod_neg[] = {
        0x07230203u, 0x00010600u, 0u, 40u, 0u,
        (2u << 16) | 17u, 1u,
        (3u << 16) | 14u, 0u, 1u,
        (7u << 16) | 15u, 4u, 2u, 0x6e69616du, 0u, 29u, 32u, // EntryPoint %2 "main" %29 %32
        (3u << 16) | 16u, 2u, 7u,
        (2u << 16) | 19u, 3u,                       // void
        (3u << 16) | 33u, 4u, 3u,                   // fn void()
        (2u << 16) | 20u, 5u,                       // bool
        (4u << 16) | 21u, 6u, 32u, 0u,              // uint
        (3u << 16) | 22u, 7u, 32u,                  // float
        (4u << 16) | 23u, 8u, 7u, 4u,               // vec4
        (4u << 16) | 32u, 9u, 7u, 6u,               // ptr Fn uint
        (4u << 16) | 32u, 10u, 7u, 8u,              // ptr Fn vec4
        (4u << 16) | 32u, 11u, 7u, 7u,              // ptr Fn float
        (4u << 16) | 32u, 30u, 3u, 7u,              // ptr Output float
        (4u << 16) | 32u, 31u, 3u, 6u,              // ptr Output uint
        (4u << 16) | 59u, 30u, 29u, 3u,             // %29 Output float
        (4u << 16) | 59u, 31u, 32u, 3u,             // %32 Output uint
        (3u << 16) | 41u, 5u, 12u,                  // true
        (4u << 16) | 43u, 6u, 13u, 42u,             // uint 42
        (4u << 16) | 43u, 6u, 14u, 0u,              // uint 0
        (4u << 16) | 43u, 7u, 15u, 0x3f800000u,     // float 1.0
        (7u << 16) | 44u, 8u, 25u, 15u, 15u, 15u, 15u, // vec4(1.0)
        // Helper %34: void helper()
        (5u << 16) | 54u, 3u, 34u, 0u, 4u,
        (2u << 16) | 248u, 37u,
        (1u << 16) | 253u,
        (1u << 16) | 56u,
        // Main %2
        (5u << 16) | 54u, 3u, 2u, 0u, 4u,           // %2 main
        (2u << 16) | 248u, 16u,                     // %16 entry
        (4u << 16) | 59u, 9u, 17u, 7u,              // %17 Var Fn uint (cross-block: NOT eliminated)
        (4u << 16) | 59u, 9u, 33u, 7u,              // %33 Var Fn uint (across OpFunctionCall: NOT forwarded!)
        (4u << 16) | 59u, 10u, 26u, 7u,             // %26 Var Fn vec4 (promoted & eliminated!)
        (3u << 16) | 62u, 17u, 13u,                 // OpStore %17 %13 in entry block
        (3u << 16) | 62u, 33u, 13u,                 // OpStore %33 %13 before Call
        (4u << 16) | 57u, 3u, 35u, 34u,             // %35 = OpFunctionCall %34
        (4u << 16) | 61u, 6u, 36u, 33u,             // %36 = OpLoad %33 after Call -> MUST remain!
        (3u << 16) | 62u, 26u, 25u,                 // OpStore %26 %25 (vec4)
        (5u << 16) | 65u, 11u, 27u, 26u, 14u,       // %27 = AccessChain %26[0]
        (4u << 16) | 61u, 7u, 28u, 27u,             // %28 = OpLoad %27 -> OpCompositeExtract!
        (3u << 16) | 62u, 29u, 28u,                 // OpStore %29 %28 (keeps %28 live!)
        (3u << 16) | 247u, 20u, 0u,
        (4u << 16) | 250u, 12u, 18u, 19u,
        (2u << 16) | 248u, 18u,
        (2u << 16) | 249u, 20u,
        (2u << 16) | 248u, 19u,
        (2u << 16) | 249u, 20u,
        (2u << 16) | 248u, 20u,                     // %20 merge
        (4u << 16) | 61u, 6u, 21u, 17u,             // OpLoad %17 across OpLabel -> MUST remain!
        (5u << 16) | 128u, 6u, 38u, 21u, 36u,       // %38 = IAdd %21 + %36 (keeps both %21 and %36 live!)
        (3u << 16) | 62u, 32u, 38u,                 // OpStore %32 %38
        (1u << 16) | 253u,
        (1u << 16) | 56u,
    };
    size_t nwn = sizeof mod_neg / 4;
    uint32_t *bufn = malloc(sizeof mod_neg);
    memcpy(bufn, mod_neg, sizeof mod_neg);
    assert(spirv_mali_cleanup(&bufn, &nwn));
    assert(mali_spv_validate_stream(bufn, nwn, NULL));
    assert(count_spv_op(bufn, nwn, MALI_SPV_OP_COMPOSITE_EXTRACT) == 1);  // vec4 temp promoted
    assert(count_spv_op(bufn, nwn, MALI_SPV_OP_VARIABLE) == 4);           // 2 Output vars + %17 + %33; %26 eliminated!
    assert(count_spv_op(bufn, nwn, MALI_SPV_OP_LOAD) == 2);               // %36 (post-Call) and %21 (post-Label) both kept!
    free(bufn);

    // 3. Downstream taint negative controls for OpDemoteToHelperInvocation:
    //    %then does OpDemoteToHelperInvocation -> %merge contains taint op -> MUST NOT convert!
    const uint16_t taint_ops[] = {
        207u, // OpDPdx
        89u,  // OpImageSampleDrefImplicitLod
        91u,  // OpImageSampleProjImplicitLod
        105u, // OpImageQueryLod
        365u, // OpGroupNonUniformQuadBroadcast
    };
    for (size_t t = 0; t < sizeof taint_ops / sizeof taint_ops[0]; t++) {
        uint16_t top = taint_ops[t];
        uint32_t taint_wc = (top == 89u || top == 365u) ? 6u : (top == 91u || top == 105u) ? 5u : 4u;
        uint32_t mod_t[] = {
            0x07230203u, 0x00010600u, 0u, 32u, 0u,
            (2u << 16) | 17u, 1u,
            (2u << 16) | 17u, 5379u,
            (3u << 16) | 14u, 0u, 1u,
            (5u << 16) | 15u, 4u, 2u, 0x6e69616du, 0u,
            (3u << 16) | 16u, 2u, 7u,
            (2u << 16) | 19u, 3u,
            (3u << 16) | 33u, 4u, 3u,
            (2u << 16) | 20u, 5u,
            (3u << 16) | 22u, 7u, 32u,
            (3u << 16) | 41u, 5u, 9u,
            (4u << 16) | 43u, 7u, 11u, 0x3f800000u,
            (5u << 16) | 54u, 3u, 2u, 0u, 4u,
            (2u << 16) | 248u, 13u,
            (3u << 16) | 247u, 20u, 0u,
            (4u << 16) | 250u, 9u, 18u, 19u,
            (2u << 16) | 248u, 18u,
            (1u << 16) | 5380u,                     // OpDemoteToHelperInvocation
            (2u << 16) | 249u, 20u,
            (2u << 16) | 248u, 19u,
            (2u << 16) | 249u, 20u,
            (2u << 16) | 248u, 20u,
            (taint_wc << 16) | top, 7u, 21u, 11u, 11u, 11u,
            (1u << 16) | 253u,
            (1u << 16) | 56u,
        };
        uint32_t compact[64];
        size_t n_hdr = sizeof(mod_t) / 4 - 8;  // up to merge OpLabel inclusive
        for (size_t i = 0; i < n_hdr; i++) compact[i] = mod_t[i];
        size_t o = n_hdr;
        compact[o++] = (taint_wc << 16) | top;
        for (uint32_t k = 1; k < taint_wc; k++) compact[o++] = 11u;
        compact[o++] = (1u << 16) | 253u;
        compact[o++] = (1u << 16) | 56u;
        uint32_t *buft = malloc(o * 4);
        memcpy(buft, compact, o * 4);
        size_t nwt2 = o;
        assert(!spirv_mali_demote_to_terminate(&buft, &nwt2));
        assert(count_spv_op(buft, nwt2, MALI_SPV_OP_DEMOTE_TO_HELPER) == 1);
        assert(count_spv_op(buft, nwt2, MALI_SPV_OP_TERMINATE_INVOCATION) == 0);
        free(buft);
    }

    // 4. Transitive callee taint propagation (OpGroupNonUniformQuadBroadcast=365 inside helper %25).
    uint32_t mod_callee[] = {
        0x07230203u, 0x00010600u, 0u, 32u, 0u,
        (2u << 16) | 17u, 1u,
        (2u << 16) | 17u, 5379u,
        (3u << 16) | 14u, 0u, 1u,
        (5u << 16) | 15u, 4u, 2u, 0x6e69616du, 0u,
        (3u << 16) | 16u, 2u, 7u,
        (2u << 16) | 19u, 3u,
        (3u << 16) | 33u, 4u, 3u,
        (2u << 16) | 20u, 5u,
        (3u << 16) | 22u, 7u, 32u,
        (3u << 16) | 41u, 5u, 9u,
        (4u << 16) | 43u, 7u, 11u, 0x3f800000u,
        (5u << 16) | 54u, 3u, 25u, 0u, 4u,           // helper %25
        (2u << 16) | 248u, 26u,
        (6u << 16) | 365u, 7u, 27u, 11u, 11u, 11u,   // OpGroupNonUniformQuadBroadcast
        (1u << 16) | 253u,
        (1u << 16) | 56u,
        (5u << 16) | 54u, 3u, 2u, 0u, 4u,            // main %2
        (2u << 16) | 248u, 13u,
        (3u << 16) | 247u, 20u, 0u,
        (4u << 16) | 250u, 9u, 18u, 19u,
        (2u << 16) | 248u, 18u,
        (1u << 16) | 5380u,
        (2u << 16) | 249u, 20u,
        (2u << 16) | 248u, 19u,
        (2u << 16) | 249u, 20u,
        (2u << 16) | 248u, 20u,
        (4u << 16) | 57u, 3u, 28u, 25u,             // OpFunctionCall %25
        (1u << 16) | 253u,
        (1u << 16) | 56u,
    };
    size_t nwc = sizeof mod_callee / 4;
    uint32_t *bufc = malloc(sizeof mod_callee);
    memcpy(bufc, mod_callee, sizeof mod_callee);
    assert(!spirv_mali_demote_to_terminate(&bufc, &nwc));
    free(bufc);

    // 5. OpIsHelperInvocationEXT (5381) keeps Cap Demote & SPV_EXT_demote_to_helper_invocation alive.
    uint32_t mod_helper_ext[] = {
        0x07230203u, 0x00010600u, 0u, 32u, 0u,
        (2u << 16) | 17u, 1u,
        (2u << 16) | 17u, 5379u,
        (10u << 16) | 10u, 0x5f565053u, 0x5f545845u, 0x6f6d6564u, 0x745f6574u,
            0x65685f6fu, 0x7265706cu, 0x766e695fu, 0x7461636fu, 0x006e6f69u, // SPV_EXT_demote_to_helper_invocation
        (3u << 16) | 14u, 0u, 1u,
        (5u << 16) | 15u, 4u, 2u, 0x6e69616du, 0u,
        (3u << 16) | 16u, 2u, 7u,
        (2u << 16) | 19u, 3u,
        (3u << 16) | 33u, 4u, 3u,
        (2u << 16) | 20u, 5u,
        (3u << 16) | 41u, 5u, 9u,
        (5u << 16) | 54u, 3u, 2u, 0u, 4u,
        (2u << 16) | 248u, 13u,
        (3u << 16) | 5381u, 5u, 10u,                // %10 = OpIsHelperInvocationEXT in entry block
        (3u << 16) | 247u, 20u, 0u,
        (4u << 16) | 250u, 9u, 18u, 19u,
        (2u << 16) | 248u, 18u,
        (1u << 16) | 5380u,                         // converts cleanly to OpTerminateInvocation
        (2u << 16) | 249u, 20u,
        (2u << 16) | 248u, 19u,
        (2u << 16) | 249u, 20u,
        (2u << 16) | 248u, 20u,
        (1u << 16) | 253u,
        (1u << 16) | 56u,
    };
    size_t nwhe = sizeof mod_helper_ext / 4;
    uint32_t *bufhe = malloc(sizeof mod_helper_ext);
    memcpy(bufhe, mod_helper_ext, sizeof mod_helper_ext);
    assert(spirv_mali_demote_to_terminate(&bufhe, &nwhe));
    assert(count_spv_op(bufhe, nwhe, MALI_SPV_OP_TERMINATE_INVOCATION) == 1);
    assert(count_spv_op(bufhe, nwhe, MALI_SPV_OP_EXTENSION) == 1);
    assert(count_spv_op(bufhe, nwhe, MALI_SPV_OP_CAPABILITY) == 2);
    free(bufhe);

    // 6. OpPhi forbidden-ID / keep_pairs==0 rejection in spirv_mali_demote_to_terminate:
    //    Tail after demote defines %21 which is read in %20 outside OpPhi's stripped %18 pair -> MUST reject!
    uint32_t mod_forb[] = {
        0x07230203u, 0x00010600u, 0u, 32u, 0u,
        (2u << 16) | 17u, 1u,
        (2u << 16) | 17u, 5379u,
        (3u << 16) | 14u, 0u, 1u,
        (5u << 16) | 15u, 4u, 2u, 0x6e69616du, 0u,
        (3u << 16) | 16u, 2u, 7u,
        (2u << 16) | 19u, 3u,
        (3u << 16) | 33u, 4u, 3u,
        (2u << 16) | 20u, 5u,
        (3u << 16) | 22u, 7u, 32u,
        (3u << 16) | 41u, 5u, 9u,
        (4u << 16) | 43u, 7u, 11u, 0x3f800000u,
        (5u << 16) | 54u, 3u, 2u, 0u, 4u,
        (2u << 16) | 248u, 13u,
        (3u << 16) | 247u, 20u, 0u,
        (4u << 16) | 250u, 9u, 18u, 19u,
        (2u << 16) | 248u, 18u,
        (1u << 16) | 5380u,
        (5u << 16) | 129u, 7u, 21u, 11u, 11u,       // %21 defined in tail!
        (2u << 16) | 249u, 20u,
        (2u << 16) | 248u, 19u,
        (2u << 16) | 249u, 20u,
        (2u << 16) | 248u, 20u,
        (5u << 16) | 129u, 7u, 22u, 21u, 11u,       // non-Phi use of %21 in %20 -> forbidden!
        (1u << 16) | 253u,
        (1u << 16) | 56u,
    };
    size_t nwf = sizeof mod_forb / 4;
    uint32_t *buff = malloc(sizeof mod_forb);
    memcpy(buff, mod_forb, sizeof mod_forb);
    assert(!spirv_mali_demote_to_terminate(&buff, &nwf));
    free(buff);

    // 7. OpAtomicStore (228) must keep its OpAccessChain %Pointer operand alive across pure DCE!
    uint32_t mod_atomic[] = {
        0x07230203u, 0x00010600u, 0u, 32u, 0u,
        (2u << 16) | 17u, 1u,
        (3u << 16) | 14u, 0u, 1u,
        (6u << 16) | 15u, 4u, 2u, 0x6e69616du, 0u, 12u,
        (3u << 16) | 16u, 2u, 7u,
        (2u << 16) | 19u, 3u,
        (3u << 16) | 33u, 4u, 3u,
        (4u << 16) | 21u, 6u, 32u, 0u,              // uint32 %6
        (4u << 16) | 43u, 6u, 7u, 0u,               // uint 0
        (4u << 16) | 43u, 6u, 8u, 1u,               // uint 1
        (4u << 16) | 32u, 9u, 12u, 6u,              // ptr StorageBuffer uint %9
        (3u << 16) | 29u, 10u, 6u,                  // RuntimeArray uint %10
        (3u << 16) | 30u, 11u, 10u,                 // Struct { RuntimeArray } %11
        (4u << 16) | 32u, 13u, 12u, 11u,            // ptr StorageBuffer Struct %13
        (4u << 16) | 59u, 13u, 12u, 12u,            // %12 = StorageBuffer var
        (5u << 16) | 54u, 3u, 2u, 0u, 4u,
        (2u << 16) | 248u, 14u,
        (6u << 16) | 65u, 9u, 15u, 12u, 7u, 7u,    // %15 = OpAccessChain %12[0][0]
        (5u << 16) | 228u, 15u, 8u, 7u, 8u,         // OpAtomicStore %15 %8 %7 %8
        (1u << 16) | 253u,
        (1u << 16) | 56u,
    };
    size_t nwa = sizeof mod_atomic / 4;
    uint32_t *bufa = malloc(sizeof mod_atomic);
    memcpy(bufa, mod_atomic, sizeof mod_atomic);
    assert(!spirv_mali_cleanup(&bufa, &nwa));
    assert(count_spv_op(bufa, nwa, MALI_SPV_OP_ACCESS_CHAIN) == 1);
    assert(count_spv_op(bufa, nwa, 228u) == 1);
    free(bufa);
}

static int run_batch_file(const char *in_path, const char *out_path,
                          size_t *n_demote_chg, size_t *n_cleanup_chg,
                          uint64_t *words_before, uint64_t *words_after) {
    FILE *f = fopen(in_path, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 20 || (sz % 4) != 0) { fclose(f); return 1; }
    size_t nw = (size_t)sz / 4;
    uint32_t *w = malloc((size_t)sz);
    if (!w || fread(w, 1, (size_t)sz, f) != (size_t)sz) { free(w); fclose(f); return 1; }
    fclose(f);

    *words_before += nw;
    if (spirv_mali_demote_to_terminate(&w, &nw)) (*n_demote_chg)++;
    if (spirv_mali_cleanup(&w, &nw)) (*n_cleanup_chg)++;
    *words_after += nw;

    FILE *fo = fopen(out_path, "wb");
    if (!fo || fwrite(w, 4, nw, fo) != nw) {
        if (fo) fclose(fo);
        free(w);
        return 1;
    }
    fclose(fo);
    free(w);
    return 0;
}

static int run_batch_dir(const char *in_dir, const char *out_dir) {
    size_t files = 0, dem_chg = 0, cln_chg = 0, errs = 0;
    uint64_t w_before = 0, w_after = 0;
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*.spv", in_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "FindFirstFileA failed: %s\n", pattern); return 2; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char inp[1024], outp[1024];
        snprintf(inp, sizeof inp, "%s\\%s", in_dir, fd.cFileName);
        snprintf(outp, sizeof outp, "%s\\%s", out_dir, fd.cFileName);
        files++;
        if (run_batch_file(inp, outp, &dem_chg, &cln_chg, &w_before, &w_after) != 0) errs++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(in_dir);
    if (!d) return 2;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nl = strlen(ent->d_name);
        if (nl < 5 || strcmp(ent->d_name + nl - 4, ".spv") != 0) continue;
        char inp[1024], outp[1024];
        snprintf(inp, sizeof inp, "%s/%s", in_dir, ent->d_name);
        snprintf(outp, sizeof outp, "%s/%s", out_dir, ent->d_name);
        files++;
        if (run_batch_file(inp, outp, &dem_chg, &cln_chg, &w_before, &w_after) != 0) errs++;
    }
    closedir(d);
#endif
    printf("BATCH files=%zu demote_changed=%zu cleanup_changed=%zu words_before=%llu words_after=%llu errs=%zu\n",
           files, dem_chg, cln_chg, (unsigned long long)w_before, (unsigned long long)w_after, errs);
    return errs ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--batch")) return run_batch_dir(argv[2], argv[3]);
    test_policy();
    test_translate_cleanups_and_discard();
    test_spirv_passes_synthetic();
    test_spirv_negative_controls_and_bounds();
    puts("mali_shader_opt tests passed");
    return 0;
}
