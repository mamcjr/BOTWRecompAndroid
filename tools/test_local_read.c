// Host (POSIX Clang/GCC) / Android Bionic unit tests for tile-local dynamic rendering local read:
//  1) local_read.h gating (Mali r49+ default-on, Turnip/Qualcomm/older-Mali off, LARECOMP_* overrides) & slot lookup
//  2) translate.c static VS/FS analyzer positive & negative controls + subpassInput/subpassLoad codegen & fail-closed checks
//  3) shaders.c vk_reflect() SPIR-V reflection of OpTypeImage(Dim=SubpassData) -> VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__) && !defined(VK_NO_PROTOTYPES)
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>
#include <shaderc/shaderc.h>

#include "../runtime/nvn/maxwell/translate.c"
#undef F
#undef R
#include "../runtime/nvn/vulkan/local_read.h"
#include "../runtime/nvn/vulkan/shaders.c"

// ---- minimal stubs for shaders.c when compiled standalone (we test vk_reflect, not shaderc/vkCreateShaderModule)
VkG g_vk = {.sync2 = true, .dyn_render = true, .local_read = true};
const char smaa_hlsl_src[] = "";
long long vk_mnow(void) { return 0; }
void vk_mgrow_add(int what, long long since) { (void)what; (void)since; }
void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *fmt, ...) { (void)fmt; abort(); }
void vk_device_fault_report(void) {}
#ifdef __ANDROID__
static VKAPI_ATTR VkResult VKAPI_CALL stub_csm(VkDevice d, const VkShaderModuleCreateInfo *c,
                                                const VkAllocationCallbacks *a, VkShaderModule *m) {
    (void)d; (void)c; (void)a; if (m) *m = VK_NULL_HANDLE; return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL stub_cdsl(VkDevice d, const VkDescriptorSetLayoutCreateInfo *c,
                                                 const VkAllocationCallbacks *a, VkDescriptorSetLayout *l) {
    (void)d; (void)c; (void)a; if (l) *l = VK_NULL_HANDLE; return VK_SUCCESS;
}
PFN_vkCreateShaderModule vkCreateShaderModule = stub_csm;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = stub_cdsl;
#else
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *c,
                                                    const VkAllocationCallbacks *a, VkShaderModule *m) {
    (void)d; (void)c; (void)a; if (m) *m = VK_NULL_HANDLE; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *c,
                                                         const VkAllocationCallbacks *a, VkDescriptorSetLayout *l) {
    (void)d; (void)c; (void)a; if (l) *l = VK_NULL_HANDLE; return VK_SUCCESS;
}
#endif
shaderc_compiler_t shaderc_compiler_initialize(void) { return (shaderc_compiler_t)1; }
void shaderc_compiler_release(shaderc_compiler_t c) { (void)c; }
shaderc_compile_options_t shaderc_compile_options_initialize(void) { return NULL; }
void shaderc_compile_options_release(shaderc_compile_options_t o) { (void)o; }
void shaderc_compile_options_set_target_env(shaderc_compile_options_t o, shaderc_target_env e, uint32_t v) {
    (void)o; (void)e; (void)v;
}
void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t o, shaderc_optimization_level l) {
    (void)o; (void)l;
}
shaderc_compilation_result_t shaderc_compile_into_spv(shaderc_compiler_t c, const char *s, size_t n,
                                                      shaderc_shader_kind k, const char *fn, const char *ep,
                                                      const shaderc_compile_options_t o) {
    (void)c; (void)s; (void)n; (void)k; (void)fn; (void)ep; (void)o;
    return NULL;
}
shaderc_compilation_status shaderc_result_get_compilation_status(const shaderc_compilation_result_t r) {
    (void)r;
    return shaderc_compilation_status_compilation_error;
}
const char *shaderc_result_get_error_message(const shaderc_compilation_result_t r) { (void)r; return ""; }
size_t shaderc_result_get_length(const shaderc_compilation_result_t r) { (void)r; return 0; }
const char *shaderc_result_get_bytes(const shaderc_compilation_result_t r) { (void)r; return NULL; }
void shaderc_result_release(shaderc_compilation_result_t r) { (void)r; }

// ---- Maxwell synthetic instruction encoders (64-bit little-endian words, quad-scheduled every 4 words)
#define PT_BITS (7ull << 16)
#define P0_BITS (0ull << 16)
#define SCHED_OP 0x001fc00000000000ull
#define RZ_REG 255

static uint64_t enc_ld_attr(uint64_t pred, int d, uint32_t addr, int count) {
    return 0xefd8000000000000ull | pred | ((uint64_t)(count - 1) << 47) | ((uint64_t)addr << 20) |
           ((uint64_t)RZ_REG << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_st_attr(uint64_t pred, int s, uint32_t addr, int count) {
    return 0xeff0000000000000ull | pred | ((uint64_t)(count - 1) << 47) | ((uint64_t)addr << 20) |
           ((uint64_t)RZ_REG << 8) | (uint64_t)(s & 255);
}
static uint64_t enc_mov32i(uint64_t pred, int d, uint32_t imm32) {
    return 0x0100000000000000ull | pred | ((uint64_t)imm32 << 20) | (uint64_t)(d & 255);
}
static uint64_t enc_fmul32i(uint64_t pred, int d, int a, uint32_t imm32) {
    return 0x1e00000000000000ull | pred | ((uint64_t)imm32 << 20) | ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_fadd_reg(uint64_t pred, int d, int a, int b, bool neg_a, bool neg_b) {
    return 0x5c58000000000000ull | pred | ((uint64_t)neg_a << 48) | ((uint64_t)neg_b << 45) |
           ((uint64_t)(b & 255) << 20) | ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_fmul_reg(uint64_t pred, int d, int a, int b) {
    return 0x5c68000000000000ull | pred | ((uint64_t)(b & 255) << 20) | ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_fmul_imm(uint64_t pred, int d, int a, uint32_t top20, int sc, bool neg_b, bool sat) {
    return 0x3868000000000000ull | pred | (((uint64_t)top20 >> 19) << 56) | ((uint64_t)sat << 50) |
           ((uint64_t)neg_b << 48) | ((uint64_t)(sc & 7) << 41) |
           (((uint64_t)top20 & 0x7FFFFull) << 20) | ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_mov_reg(uint64_t pred, int d, int b) {
    return 0x5c98000000000000ull | pred | ((uint64_t)(b & 255) << 20) | ((uint64_t)RZ_REG << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_tld4s(uint64_t pred, int d, int d1, int a, int b, bool f16, uint32_t index) {
    return 0xdf00000000000000ull | pred | ((uint64_t)f16 << 55) | ((uint64_t)index << 36) |
           ((uint64_t)(d1 & 255) << 28) | ((uint64_t)(b & 255) << 20) |
           ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_tmml(uint64_t pred, int d, int a, uint32_t index, bool bindless) {
    uint64_t base = bindless ? 0xdf60000000000000ull : 0xdf58000000000000ull;
    return base | pred | (bindless ? 0ull : ((uint64_t)index << 36)) |
           (0x3ull << 31) | (1ull << 29) | ((uint64_t)a << 20) | ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_ffma_imm_half(uint64_t pred, int d, int a, int c_reg, bool neg_c) {
    // 0.5f top 20 bits == 0x3f000u (sign bit at bit 56 == 0, low 19 bits at bit 20 == 0x3f000)
    return 0x3280000000000000ull | pred | ((uint64_t)neg_c << 49) | ((uint64_t)(c_reg & 255) << 39) |
           (0x3f000ull << 20) | ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_ipa(uint64_t pred, int d, int mode, int mul, uint32_t addr) {
    return 0xe00000000000ff00ull | pred | ((uint64_t)mode << 54) | ((uint64_t)addr << 28) |
           ((uint64_t)(mul & 255) << 20) | (uint64_t)(d & 255);
}
static uint64_t enc_mufu_rcp(uint64_t pred, int d, int a) {
    return 0x5080000000000000ull | pred | (4ull << 20) | ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_texs_msel(uint64_t pred, int d, int d1, int a, int b, int info, int msel, bool f32, uint32_t index) {
    return 0xd000000000000000ull | pred | ((uint64_t)f32 << 59) | ((uint64_t)info << 53) | ((uint64_t)msel << 50) |
           ((uint64_t)index << 36) | ((uint64_t)(d1 & 255) << 28) | ((uint64_t)(b & 255) << 20) |
           ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_texs(uint64_t pred, int d, int d1, int a, int b, int info, uint32_t index) {
    return enc_texs_msel(pred, d, d1, a, b, info, 0, false, index);
}
static uint64_t enc_tlds(uint64_t pred, int d, int d1, int a, int b, int info, uint32_t index) {
    return 0xd200000000000000ull | pred | ((uint64_t)info << 53) | ((uint64_t)index << 36) |
           ((uint64_t)(d1 & 255) << 28) | ((uint64_t)(b & 255) << 20) | ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_tld4(uint64_t pred, int d, int a, uint32_t index) {
    return 0xc838000000000000ull | pred | ((uint64_t)index << 36) | ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_txq(uint64_t pred, int d, int a, int qt, uint32_t mask, uint32_t index) {
    return 0xdf48000000000000ull | pred | ((uint64_t)index << 36) | ((uint64_t)mask << 31) |
           ((uint64_t)qt << 22) | ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_tex(uint64_t pred, int d, int a, int b, uint32_t index, int lod_mode) {
    return 0xc038000000000000ull | pred | ((uint64_t)lod_mode << 55) | ((uint64_t)index << 36) |
           (0xFull << 31) | (1ull << 29) | ((uint64_t)(b & 255) << 20) | ((uint64_t)(a & 255) << 8) | (uint64_t)(d & 255);
}
static uint64_t enc_bra(uint64_t pred, int32_t byte_off) {
    return 0xe240000000000000ull | pred | 15ull | (((uint64_t)(uint32_t)byte_off & 0xFFFFFFull) << 20);
}
static uint64_t enc_exit(uint64_t pred) { return 0xe300000000000000ull | pred; }

typedef struct {
    uint8_t blob[4096];
    uint64_t *code;
    size_t nw;
} ProgAsm;

static void asm_init(ProgAsm *a, bool is_fs) {
    memset(a, 0, sizeof *a);
    a->code = (uint64_t *)(a->blob + 0x80);
    if (is_fs) {
        *(uint32_t *)(a->blob + 0x30 + 0x48) = 0x7u;  // omap_target: c0 RGB
        for (int attr = 0; attr < 32; attr++) a->blob[0x30 + 0x18 + attr] = 0xAAu;  // perspective
    }
}
static size_t asm_push(ProgAsm *a, uint64_t op) {
    if ((a->nw % 4u) == 0u) a->code[a->nw++] = SCHED_OP;
    size_t idx = a->nw;
    a->code[a->nw++] = op;
    return idx;
}
static size_t asm_finish(ProgAsm *a) {
    asm_push(a, enc_exit(PT_BITS));
    asm_push(a, enc_bra(PT_BITS, -8));
    return 0x80u + a->nw * sizeof(uint64_t);
}

static void build_vs(ProgAsm *a, bool flip_y_formula, bool use_ffma, bool pred_pos_z_only,
                     bool pred_pos_x, bool cross_blk_st, bool extra_offset) {
    asm_init(a, false);
    // r0..r3 = a0.{x,y,z,w}
    asm_push(a, enc_ld_attr(PT_BITS, 0, 0x80u, 4));
    // Compute r12 = gl_Position (optional predicated r2 update tests BOTW VS pattern)
    asm_push(a, enc_st_attr(pred_pos_x ? P0_BITS : PT_BITS, 0, 0x70u, 1));
    asm_push(a, enc_st_attr(PT_BITS, 1, 0x74u, 1));
    asm_push(a, enc_st_attr(pred_pos_z_only ? P0_BITS : PT_BITS, 2, 0x78u, 1));
    asm_push(a, enc_st_attr(PT_BITS, 3, 0x7cu, 1));
    if (cross_blk_st) {
        // Unconditional jump over a dummy instruction splits basic blocks between pos ST and v0 ST
        asm_push(a, enc_bra(PT_BITS, 8));
        asm_push(a, enc_mov32i(PT_BITS, 20, 0u));
    }
    // v0.x = (r0 + r3) * 0.5, v0.y = ((flip_y_formula ? +r1 : -r1) + r3) * 0.5, v0.w = r3
    if (use_ffma) {
        asm_push(a, enc_fmul32i(PT_BITS, 7, 3, 0x3f000000u));              // r7 = w * 0.5
        asm_push(a, enc_ffma_imm_half(PT_BITS, 4, 0, 7, false));           // r4 = x*0.5 + w*0.5
        asm_push(a, enc_fmul32i(PT_BITS, 8, 1, flip_y_formula ? 0x3f000000u : 0xbf000000u));
        asm_push(a, enc_fadd_reg(PT_BITS, 5, 8, 7, false, false));         // r5 = +/-y*0.5 + w*0.5
    } else {
        asm_push(a, enc_fadd_reg(PT_BITS, 8, 0, 3, false, false));         // r8 = x + w
        asm_push(a, enc_fmul32i(PT_BITS, 4, 8, 0x3f000000u));              // r4 = (x + w)*0.5
        asm_push(a, enc_fadd_reg(PT_BITS, 9, 1, 3, !flip_y_formula, false));
        asm_push(a, enc_fmul32i(PT_BITS, 5, 9, 0x3f000000u));
    }
    if (extra_offset) {
        asm_push(a, enc_mov32i(PT_BITS, 21, 0x3d000000u));
        asm_push(a, enc_fadd_reg(PT_BITS, 4, 4, 21, false, false));
    }
    asm_push(a, enc_st_attr(PT_BITS, 4, 0x80u, 1));
    asm_push(a, enc_st_attr(PT_BITS, 5, 0x84u, 1));
    asm_push(a, enc_st_attr(PT_BITS, 3, 0x8cu, 1));
}

static void build_fs(ProgAsm *a, int variant) {
    // variant:
    //  0 = clean IPA.PASS(v0.x,v0.y,v0.w) -> RCP -> FMUL -> TEXS.2D(slot t0 @ index 8)
    //  1 = clean IPA.MULTIPLY(v0.x,v0.y, rcp_w) -> TEX.2D.LZ
    //  2 = noperspective on v0 (SPH 0x18 = 0xFF)
    //  3 = flat (IPA.CONSTANT) on v0
    //  4 = offset UV after divide
    //  5 = TLDS texelFetch
    //  6 = TLD4 gather
    //  7 = TXQ query
    //  8 = TEX explicit LOD (lod_mode=3)
    //  9 = mixed local t0 (index 8) + non-local read on SAME t0 -> whole slot disqualified
    // 10 = branch label between FMUL and TEXS kills FS_SUV
    asm_init(a, true);
    if (variant == 2) a->blob[0x30 + 0x18] = 0xFFu;
    if (variant == 1) {
        asm_push(a, enc_ipa(PT_BITS, 0, 0, RZ_REG, 0x8cu));       // r0 = v0.w
        asm_push(a, enc_mufu_rcp(PT_BITS, 1, 0));                 // r1 = 1/v0.w
        asm_push(a, enc_ipa(PT_BITS, 2, 1, 1, 0x80u));            // r2 = v0.x * r1
        asm_push(a, enc_ipa(PT_BITS, 3, 1, 1, 0x84u));            // r3 = v0.y * r1
        asm_push(a, enc_tex(PT_BITS, 4, 2, RZ_REG, 8u, 1));       // TEX 2D .LZ -> r4..r7
    } else if (variant == 3) {
        asm_push(a, enc_ipa(PT_BITS, 2, 2, RZ_REG, 0x80u));       // IPA.CONSTANT (flat)
        asm_push(a, enc_ipa(PT_BITS, 3, 2, RZ_REG, 0x84u));
        asm_push(a, enc_texs(PT_BITS, 4, 6, 2, 3, 1, 8u));
    } else {
        asm_push(a, enc_ipa(PT_BITS, 0, 0, RZ_REG, 0x80u));
        asm_push(a, enc_ipa(PT_BITS, 1, 0, RZ_REG, 0x84u));
        asm_push(a, enc_ipa(PT_BITS, 2, 0, RZ_REG, 0x8cu));
        asm_push(a, enc_mufu_rcp(PT_BITS, 3, 2));
        asm_push(a, enc_fmul_reg(PT_BITS, 4, 0, 3));
        asm_push(a, enc_fmul_reg(PT_BITS, 5, 1, 3));
        if (variant == 4) {
            asm_push(a, enc_mov32i(PT_BITS, 6, 0x3d000000u));
            asm_push(a, enc_fadd_reg(PT_BITS, 4, 4, 6, false, false));
        }
        if (variant == 10) {
            asm_push(a, enc_bra(PT_BITS, 8));
            asm_push(a, enc_mov32i(PT_BITS, 20, 0u));
        }
        if (variant == 5) {
            asm_push(a, enc_tlds(PT_BITS, 4, 6, 4, 5, 2, 8u));
        } else if (variant == 6) {
            asm_push(a, enc_tld4(PT_BITS, 4, 4, 8u));
        } else if (variant == 7) {
            asm_push(a, enc_mov32i(PT_BITS, 6, 0u));
            asm_push(a, enc_txq(PT_BITS, 4, 6, 1, 0xFu, 8u));
        } else if (variant == 8) {
            asm_push(a, enc_mov32i(PT_BITS, 6, 0x40000000u));
            asm_push(a, enc_tex(PT_BITS, 4, 4, 6, 8u, 3));
        } else {
            asm_push(a, enc_texs(PT_BITS, 8, 10, 4, 5, 1, 8u));
            if (variant == 9) {
                asm_push(a, enc_mov32i(PT_BITS, 6, 0x3e000000u));
                asm_push(a, enc_fadd_reg(PT_BITS, 4, 4, 6, false, false));
                asm_push(a, enc_texs(PT_BITS, 12, 14, 4, 5, 1, 8u));
            }
            asm_push(a, enc_st_attr(PT_BITS, 8, 0x00u, 3));
        }
    }
}

static void test_gating_and_slots(void) {
    assert(vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-03bet0", true, NULL, false));
    assert(vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r50p0-00eac0", true, NULL, false));
    assert(vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r100p0-00eac0", true, NULL, false));
    assert(!vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r48p0-01eac0", true, NULL, false));
    assert(!vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r44p0-01eac0", true, NULL, false));
    assert(!vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r9p0-00eac0", true, NULL, false));
    assert(!vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, NULL, true, NULL, false));
    assert(!vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "", true, NULL, false));
    assert(!vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "rabc", true, NULL, false));
    assert(!vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-03bet0", false, NULL, false));
    assert(!vk_local_read_want(VK_DRIVER_ID_MESA_TURNIP, "Mesa 25.0", true, NULL, false));
    assert(!vk_local_read_want(VK_DRIVER_ID_QUALCOMM_PROPRIETARY, "Qualcomm 800", true, NULL, false));
    assert(!vk_local_read_want(0u, "Desktop Unknown", true, NULL, false));
    assert(!vk_local_read_want(VK_DRIVER_ID_AMD_PROPRIETARY, "AMD", true, NULL, false));

    // Env overrides and kill-switch priority (including empty string fallback)
    assert(vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-03bet0", true, "", false));
    assert(!vk_local_read_want(VK_DRIVER_ID_MESA_TURNIP, "Mesa 25.0", true, "", false));
    assert(vk_local_read_want(VK_DRIVER_ID_MESA_TURNIP, "Mesa 25.0", true, "1", false));
    assert(!vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-03bet0", true, "0", false));
    assert(!vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-03bet0", true, "1", true));
    assert(!vk_local_read_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-03bet0", true, NULL, true));

    // Slot lookup helper
    uint8_t slots[8] = {1, 3, 6, 0};
    uint8_t atts[8] = {1, 3, 5, 0};
    assert(vk_local_read_slot_att(slots, atts, 0) == 1);
    assert(vk_local_read_slot_att(slots, atts, 2) == 3);
    assert(vk_local_read_slot_att(slots, atts, 5) == 5);
    assert(vk_local_read_slot_att(slots, atts, 1) == -1);
    assert(vk_local_read_slot_att(slots, atts, -1) == -1);
    assert(vk_local_read_slot_att(slots, atts, 64) == -1);
}

static void test_maxwell_analysis_and_codegen(void) {
    ProgAsm a;
    ShaderKey vk = {.glsl = 1, .flip_y = true};
    size_t sz = 0;
    ShaderInfo *si = NULL;

    // 1. VS positive controls (flip_y=1 FFMA & FADD, flip_y=0, predicated gl_Position.z only)
    build_vs(&a, true, false, false, false, false, false);
    sz = asm_finish(&a);
    si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_VERTEX, &vk);
    assert(si && si->msl && !si->error[0] && si->screen_uv_mask == 0x1u);
    maxwell_free(si);

    build_vs(&a, true, true, true, false, false, false);
    sz = asm_finish(&a);
    si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_VERTEX, &vk);
    assert(si && si->msl && !si->error[0] && si->screen_uv_mask == 0x1u);
    maxwell_free(si);

    ShaderKey vk_nofy = {.glsl = 1, .flip_y = false};
    build_vs(&a, false, false, false, false, false, false);
    sz = asm_finish(&a);
    si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_VERTEX, &vk_nofy);
    assert(si && si->msl && !si->error[0] && si->screen_uv_mask == 0x1u);
    maxwell_free(si);

    // 2. VS negative controls: flip_x, wrong Y sign, extra offset, predicated pos.x, cross-block ST
    ShaderKey vk_fx = {.glsl = 1, .flip_y = true, .flip_x = true};
    build_vs(&a, true, false, false, false, false, false);
    sz = asm_finish(&a);
    si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_VERTEX, &vk_fx);
    assert(si && si->msl && si->screen_uv_mask == 0u);
    maxwell_free(si);

    build_vs(&a, false, false, false, false, false, false);  // mismatch with vk.flip_y=true
    sz = asm_finish(&a);
    si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_VERTEX, &vk);
    assert(si && si->msl && si->screen_uv_mask == 0u);
    maxwell_free(si);

    build_vs(&a, true, false, false, false, false, true);   // extra offset
    sz = asm_finish(&a);
    si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_VERTEX, &vk);
    assert(si && si->msl && si->screen_uv_mask == 0u);
    maxwell_free(si);

    build_vs(&a, true, false, false, true, false, false);   // predicated pos.x
    sz = asm_finish(&a);
    si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_VERTEX, &vk);
    assert(si && si->msl && si->screen_uv_mask == 0u);
    maxwell_free(si);

    build_vs(&a, true, false, false, false, true, false);   // cross-block ST
    sz = asm_finish(&a);
    si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_VERTEX, &vk);
    assert(si && si->msl && si->screen_uv_mask == 0u);
    maxwell_free(si);

    // 3. FS positive controls (TEXS 2D and IPA.MULTIPLY -> TEX.2D.LZ) + subpassInput/subpassLoad emission
    ShaderKey fk_base = {.glsl = 1, .flip_y = true};
    for (int v = 0; v <= 1; v++) {
        build_fs(&a, v);
        sz = asm_finish(&a);
        si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_FRAGMENT, &fk_base);
        assert(si && si->msl && !si->error[0] && si->ntex == 1 && si->tex[0].local_read_var == 1u);
        assert(strstr(si->msl, "uniform sampler2D t0;") != NULL);
        assert(strstr(si->msl, "subpassInput") == NULL);
        maxwell_free(si);

        ShaderKey fk_lr = fk_base;
        fk_lr.local_read_slot[0] = 1u;
        fk_lr.local_read_att[0] = 3u;
        si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_FRAGMENT, &fk_lr);
        assert(si && si->msl && !si->error[0]);
        assert(strstr(si->msl, "layout(input_attachment_index = 3, set = 1, binding = 32) uniform subpassInput t0;") != NULL);
        assert(strstr(si->msl, "subpassLoad(t0)") != NULL);
        assert(strstr(si->msl, "tsize0(") == NULL);
        assert(strstr(si->msl, "slin0(") == NULL);
        maxwell_free(si);
    }

    // 4. FS negative controls: noperspective, flat, offset UV, TLDS, TLD4, TXQ, explicit LOD, mixed local/nonlocal, branch merge
    for (int v = 2; v <= 10; v++) {
        build_fs(&a, v);
        sz = asm_finish(&a);
        si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_FRAGMENT, &fk_base);
        assert(si && si->msl && !si->error[0]);
        assert(si->ntex >= 1 && si->tex[0].local_read_var == 0u);
        maxwell_free(si);

        // Fail-closed codegen check on TLDS (5), TLD4 (6), TXQ (7), explicit LOD (8) if local_read_slot is forced
        if (v >= 5 && v <= 8) {
            ShaderKey fk_bad = fk_base;
            fk_bad.local_read_slot[0] = 1u;
            fk_bad.local_read_att[0] = 1u;
            si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_FRAGMENT, &fk_bad);
            assert(si && si->msl == NULL && si->error[0] != 0);
            maxwell_free(si);
        }
    }

    // 5. Partial TEXS write-mask kill precision (must not kill untouched neighbor GPRs holding FS_SUV)
    for (int tc = 0; tc < 3; tc++) {
        asm_init(&a, true);
        asm_push(&a, enc_ipa(PT_BITS, 0, 0, RZ_REG, 0x80u));
        asm_push(&a, enc_ipa(PT_BITS, 1, 0, RZ_REG, 0x84u));
        asm_push(&a, enc_ipa(PT_BITS, 2, 0, RZ_REG, 0x8cu));
        asm_push(&a, enc_mufu_rcp(PT_BITS, 3, 2));
        asm_push(&a, enc_fmul_reg(PT_BITS, 4, 0, 3));
        asm_push(&a, enc_fmul_reg(PT_BITS, 7, 1, 3));
        asm_push(&a, enc_mov32i(PT_BITS, 20, 0u));
        asm_push(&a, enc_mov32i(PT_BITS, 21, 0u));
        if (tc == 0) {
            // 1-comp F32 TEXS (msel=1 -> mask=0x2) writing only r6 (d=6, d1=RZ): must NOT kill r7 (FS_SUV.y)
            asm_push(&a, enc_texs_msel(PT_BITS, 6, RZ_REG, 20, 21, 1, 1, true, 9u));
        } else if (tc == 1) {
            // 3-comp F32 TEXS (msel=0 -> mask=0x7) writing r0,r1,r6 (d=0, d1=6): must NOT kill r7 (FS_SUV.y)
            asm_push(&a, enc_texs_msel(PT_BITS, 0, 6, 20, 21, 1, 0, true, 9u));
        } else {
            // 4-comp F32 TEXS (msel=4 -> mask=0xf) writing r0,r1,r6,r7 (d=0, d1=6): MUST kill r7
            asm_push(&a, enc_texs_msel(PT_BITS, 0, 6, 20, 21, 1, 4, true, 9u));
        }
        asm_push(&a, enc_texs_msel(PT_BITS, 8, 10, 4, 7, 1, 4, true, 8u));
        asm_push(&a, enc_st_attr(PT_BITS, 8, 0x00u, 3));
        sz = asm_finish(&a);
        si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_FRAGMENT, &fk_base);
        assert(si && si->msl && !si->error[0] && si->ntex == 2);
        assert(si->tex[0].local_read_var == 0u);
        assert(si->tex[1].local_read_var == (tc < 2 ? 1u : 0u));
        maxwell_free(si);
    }

    // 6. VS additional positive/negative coverage: FMUL sc=1 * 1.0f (positive), .SAT, FADD ABS, imm zero pos.w, ST 0x6C point_size
    asm_init(&a, false);
    asm_push(&a, enc_ld_attr(PT_BITS, 0, 0x80u, 4));
    asm_push(&a, enc_st_attr(PT_BITS, 0, 0x70u, 4));
    asm_push(&a, enc_fadd_reg(PT_BITS, 8, 0, 3, false, false));
    asm_push(&a, enc_fmul_imm(PT_BITS, 4, 8, 0x3f800u, 1, false, false));  // sc=1 * 1.0f == *0.5f
    asm_push(&a, enc_fadd_reg(PT_BITS, 9, 1, 3, false, false));
    asm_push(&a, enc_fmul_imm(PT_BITS, 5, 9, 0x3f800u, 1, false, false));
    asm_push(&a, enc_st_attr(PT_BITS, 4, 0x80u, 1));
    asm_push(&a, enc_st_attr(PT_BITS, 5, 0x84u, 1));
    asm_push(&a, enc_st_attr(PT_BITS, 3, 0x8cu, 1));
    sz = asm_finish(&a);
    si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_VERTEX, &vk);
    assert(si && si->msl && !si->error[0] && si->screen_uv_mask == 0x1u);
    maxwell_free(si);

    // 6b. VS negative: FMUL.SAT, FADD.ABS, imm +/-0.0f pos.w, writes_point_size (ST 0x6c)
    for (int neg_case = 0; neg_case < 5; neg_case++) {
        asm_init(&a, false);
        asm_push(&a, enc_ld_attr(PT_BITS, 0, 0x80u, 4));
        if (neg_case == 2 || neg_case == 3)
            asm_push(&a, enc_mov32i(PT_BITS, 3, neg_case == 2 ? 0x00000000u : 0x80000000u));  // +0.0f / -0.0f pos.w
        if (neg_case == 4)
            asm_push(&a, enc_st_attr(PT_BITS, 3, 0x6cu, 1));  // gl_PointSize
        asm_push(&a, enc_st_attr(PT_BITS, 0, 0x70u, 4));
        uint64_t fadd_x = enc_fadd_reg(PT_BITS, 8, 0, 3, false, false);
        if (neg_case == 1) fadd_x |= (1ull << 49);  // ABS on A in FADD
        asm_push(&a, fadd_x);
        asm_push(&a, enc_fmul_imm(PT_BITS, 4, 8, 0x3f800u, 1, false, neg_case == 0));  // .SAT when neg_case==0
        asm_push(&a, enc_fadd_reg(PT_BITS, 9, 1, 3, false, false));
        asm_push(&a, enc_fmul32i(PT_BITS, 5, 9, 0x3f000000u));
        asm_push(&a, enc_st_attr(PT_BITS, 4, 0x80u, 1));
        asm_push(&a, enc_st_attr(PT_BITS, 5, 0x84u, 1));
        asm_push(&a, enc_st_attr(PT_BITS, 3, 0x8cu, 1));
        sz = asm_finish(&a);
        si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_VERTEX, &vk);
        assert(si && si->msl && !si->error[0] && si->screen_uv_mask == 0u);
        maxwell_free(si);
    }

    // 7. FS negative: mul_ver mismatch across overwrite of IPA.MULTIPLY multiplier GPR
    asm_init(&a, true);
    asm_push(&a, enc_ipa(PT_BITS, 0, 0, RZ_REG, 0x8cu));
    asm_push(&a, enc_mufu_rcp(PT_BITS, 10, 0));              // r10 = rcp(w)
    asm_push(&a, enc_ipa(PT_BITS, 2, 1, 10, 0x80u));         // r2 = v0.x * r10 (IPA.MULTIPLY stamped with r10's version)
    asm_push(&a, enc_mov32i(PT_BITS, 10, 0x40000000u));      // overwrite r10 -> bumps reg_ver[10]
    asm_push(&a, enc_ipa(PT_BITS, 11, 0, RZ_REG, 0x8cu));
    asm_push(&a, enc_mufu_rcp(PT_BITS, 10, 11));             // new FS_RCP_V3 in r10 has newer reg_ver[10]!
    asm_push(&a, enc_fmul_reg(PT_BITS, 4, 2, 10));           // mul_ver mismatch: must NOT become FS_SUV
    asm_push(&a, enc_ipa(PT_BITS, 5, 1, 10, 0x84u));
    asm_push(&a, enc_texs(PT_BITS, 8, 10, 4, 5, 1, 8u));
    asm_push(&a, enc_st_attr(PT_BITS, 8, 0x00u, 3));
    sz = asm_finish(&a);
    si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_FRAGMENT, &fk_base);
    assert(si && si->msl && !si->error[0] && si->ntex == 1 && si->tex[0].local_read_var == 0u);
    maxwell_free(si);

    // 8. FS negative: TMML, TMML.B, TLD4S, two distinct FS_SUV varyings on same slot, predicated IPA / FMUL / MOV
    for (int fc = 0; fc < 7; fc++) {
        asm_init(&a, true);
        asm_push(&a, enc_ipa(fc == 4 ? P0_BITS : PT_BITS, 0, 0, RZ_REG, 0x80u));
        asm_push(&a, enc_ipa(PT_BITS, 1, 0, RZ_REG, 0x84u));
        asm_push(&a, enc_ipa(PT_BITS, 2, 0, RZ_REG, 0x8cu));
        asm_push(&a, enc_mufu_rcp(PT_BITS, 3, 2));
        asm_push(&a, enc_fmul_reg(fc == 5 ? P0_BITS : PT_BITS, 4, 0, 3));
        asm_push(&a, enc_fmul_reg(PT_BITS, 14, 1, 3));
        if (fc == 0) {
            asm_push(&a, enc_tmml(PT_BITS, 6, 4, 8u, false));
            asm_push(&a, enc_texs(PT_BITS, 8, 10, 4, 14, 1, 8u));
        } else if (fc == 1) {
            asm_push(&a, enc_texs(PT_BITS, 8, 10, 4, 14, 1, 8u));
            asm_push(&a, enc_tmml(PT_BITS, 6, 4, 8u, true));  // TMML.B
        } else if (fc == 2) {
            asm_push(&a, enc_tld4s(PT_BITS, 8, 10, 4, 14, false, 8u));
        } else if (fc == 3) {
            // Second varying v1 screen UV also samples slot index 8 -> mixed attr verdicts -> 0xFF
            asm_push(&a, enc_texs(PT_BITS, 8, 10, 4, 14, 1, 8u));
            asm_push(&a, enc_ipa(PT_BITS, 16, 0, RZ_REG, 0x90u));
            asm_push(&a, enc_ipa(PT_BITS, 17, 0, RZ_REG, 0x94u));
            asm_push(&a, enc_ipa(PT_BITS, 18, 0, RZ_REG, 0x9cu));
            asm_push(&a, enc_mufu_rcp(PT_BITS, 19, 18));
            asm_push(&a, enc_fmul_reg(PT_BITS, 20, 16, 19));
            asm_push(&a, enc_fmul_reg(PT_BITS, 21, 17, 19));
            asm_push(&a, enc_texs(PT_BITS, 12, 14, 20, 21, 1, 8u));
        } else {
            asm_push(&a, enc_mov_reg(fc == 6 ? P0_BITS : PT_BITS, 6, 4));
            asm_push(&a, enc_texs(PT_BITS, 8, 10, 6, 14, 1, 8u));
        }
        asm_push(&a, enc_st_attr(PT_BITS, 8, 0x00u, 3));
        sz = asm_finish(&a);
        si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_FRAGMENT, &fk_base);
        assert(si && si->msl && !si->error[0] && si->ntex >= 1 && si->tex[0].local_read_var == 0u);
        maxwell_free(si);
        if (fc <= 2) {
            ShaderKey fk_bad = fk_base;
            fk_bad.local_read_slot[0] = 1u;
            fk_bad.local_read_att[0] = 0u;
            si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_FRAGMENT, &fk_bad);
            assert(si && si->msl == NULL && si->error[0] != 0);
            maxwell_free(si);
        }
    }

    // 9. Fail-closed codegen when soft_linear collides with local_read_slot
    build_fs(&a, 0);
    sz = asm_finish(&a);
    ShaderKey fk_coll = fk_base;
    fk_coll.soft_linear = 1ull;
    fk_coll.local_read_slot[0] = 1u;
    fk_coll.local_read_att[0] = 0u;
    si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_FRAGMENT, &fk_coll);
    assert(si && si->msl == NULL && si->error[0] != 0);
    maxwell_free(si);
}

static void test_spirv_reflect_input_attachment(void) {
    // Hand-assembled valid minimal SPIR-V binary containing:
    //  id 1: OpTypeImage void Dim=SubpassData(6) depth=0 arrayed=0 ms=0 sampled=2 fmt=Unknown(0) -> t0 (set=1, binding=32)
    //  id 2: OpTypePointer UniformConstant id 1
    //  id 3: OpVariable id 2 UniformConstant (set=1, binding=32)
    //  id 4: OpTypeImage void Dim=2D(1) depth=0 arrayed=0 ms=0 sampled=1 fmt=Unknown(0)
    //  id 5: OpTypeSampledImage id 4
    //  id 6: OpTypePointer UniformConstant id 5
    //  id 7: OpVariable id 6 UniformConstant (set=1, binding=33)
    static const uint32_t spv[] = {
        0x07230203u, 0x00010000u, 0u, 9u, 0u,
        (4u << 16) | 71u, 3u, 34u, 1u,          // OpDecorate %3 DescriptorSet 1
        (4u << 16) | 71u, 3u, 33u, 32u,         // OpDecorate %3 Binding 32
        (4u << 16) | 71u, 7u, 34u, 1u,          // OpDecorate %7 DescriptorSet 1
        (4u << 16) | 71u, 7u, 33u, 33u,         // OpDecorate %7 Binding 33
        (2u << 16) | 19u, 8u,                   // OpTypeVoid %8
        (9u << 16) | 25u, 1u, 8u, 6u, 0u, 0u, 0u, 2u, 0u,   // OpTypeImage %1 Dim=6 (SubpassData)
        (4u << 16) | 32u, 2u, 0u, 1u,           // OpTypePointer %2 -> %1
        (4u << 16) | 59u, 2u, 3u, 0u,           // OpVariable %2 %3 UniformConstant
        (9u << 16) | 25u, 4u, 8u, 1u, 0u, 0u, 0u, 1u, 0u,   // OpTypeImage %4 Dim=1 (2D)
        (3u << 16) | 27u, 5u, 4u,               // OpTypeSampledImage %5 %4
        (4u << 16) | 32u, 6u, 0u, 5u,           // OpTypePointer %6 -> %5
        (4u << 16) | 59u, 6u, 7u, 0u,           // OpVariable %6 %7 UniformConstant
    };
    VkBindingInfo binds[8] = {0};
    int n = vk_reflect(spv, sizeof spv / sizeof spv[0], binds, 8);
    assert(n == 2);
    assert(binds[0].set == 1 && binds[0].binding == 32 && binds[0].type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
    assert(binds[1].set == 1 && binds[1].binding == 33 && binds[1].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
}

int main(int argc, char **argv) {
    test_gating_and_slots();
    test_maxwell_analysis_and_codegen();
    test_spirv_reflect_input_attachment();
    if (argc >= 3 && !strcmp(argv[1], "--dump-glsl")) {
        ProgAsm a;
        build_fs(&a, 0);
        size_t sz = asm_finish(&a);
        ShaderKey fk = {.glsl = 1, .flip_y = true, .local_read_slot = {1}, .local_read_att = {2}};
        ShaderInfo *si = maxwell_translate_bounded(a.blob, sz, 0, STAGE_FRAGMENT, &fk);
        assert(si && si->msl);
        FILE *f = fopen(argv[2], "wb");
        assert(f);
        fputs(si->msl, f);
        fclose(f);
        maxwell_free(si);
    }
    puts("PASS: test_local_read (gating, slots, Maxwell VS/FS analysis, codegen, vk_reflect)");
    return 0;
}
