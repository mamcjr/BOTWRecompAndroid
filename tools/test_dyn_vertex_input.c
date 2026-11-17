// Host/Bionic unit tests for dynamic vertex input (VK_EXT_vertex_input_dynamic_state) descriptor builder.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include "nvn/vulkan/dyn_vi.h"

static VkFormat test_fmt_fn(uint32_t f) {
    return f == 0x25 ? VK_FORMAT_R8G8B8A8_UNORM : f == 0x2E ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32G32B32_SFLOAT;
}

int main(void) {
    VkDynVi vi;
    uint32_t attribs[4] = {
        0x2Eu | (0u << 8) | (0u << 13),   // loc 0: stream 0, off 0
        0x25u | (1u << 8) | (12u << 13),  // loc 1: stream 1, off 12
        0x00u | (0u << 8) | (0u << 13),   // loc 2: fmt 0 -> ZERO_BINDING
        0x2Eu | (16u << 8) | (4u << 13),  // loc 3: stream 16 -> ZERO_BINDING
    };
    uint32_t streams[3][2] = {
        {32, 0},  // stream 0: vertex rate (divisor 0 -> Vk divisor MUST be 1)
        {16, 4},  // stream 1: instance rate divisor 4
        {0, 5},   // stream 2: stride == 0 disables instance rate -> VERTEX rate, divisor 1
    };

    vk_build_dyn_vi(0, 4, attribs, 3, streams, true, test_fmt_fn, &vi, 7);
    assert(vi.gen == 7 && vi.na == 0 && vi.nb == 0 && vi.used_streams == 0);

    vk_build_dyn_vi(0x1Fu, 4, attribs, 3, streams, true, test_fmt_fn, &vi, 8);
    assert(vi.gen == 8 && vi.na == 5 && vi.nb == 3);
    assert(vi.attr_mask == 0x1Fu && vi.stride[0] == 32 && vi.stride[1] == 16 && vi.stride[2] == 0);
    assert(vi.used_streams == ((1u << 0) | (1u << 1) | (1u << ZERO_BINDING)));
    for (uint32_t i = 0; i < vi.na; i++) {
        assert(vi.va[i].sType == VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT);
        assert(vi.va[i].pNext == NULL && vi.va[i].location == i);
    }
    assert(vi.va[0].binding == 0 && vi.va[0].format == VK_FORMAT_R32G32B32A32_SFLOAT && vi.va[0].offset == 0);
    assert(vi.va[1].binding == 1 && vi.va[1].format == VK_FORMAT_R8G8B8A8_UNORM && vi.va[1].offset == 12);
    assert(vi.va[2].binding == ZERO_BINDING && vi.va[2].format == VK_FORMAT_R32G32B32A32_SFLOAT && vi.va[2].offset == 0);
    assert(vi.va[3].binding == ZERO_BINDING && vi.va[3].format == VK_FORMAT_R32G32B32A32_SFLOAT && vi.va[3].offset == 0);
    assert(vi.va[4].binding == ZERO_BINDING && vi.va[4].format == VK_FORMAT_R32G32B32A32_SFLOAT && vi.va[4].offset == 0);
    for (uint32_t i = 0; i < vi.nb; i++) {
        assert(vi.vb[i].sType == VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT);
        assert(vi.vb[i].divisor >= 1u);
    }
    assert(vi.vb[0].binding == 0 && vi.vb[0].stride == 32 && vi.vb[0].inputRate == VK_VERTEX_INPUT_RATE_VERTEX && vi.vb[0].divisor == 1);
    assert(vi.vb[1].binding == 1 && vi.vb[1].stride == 16 && vi.vb[1].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE && vi.vb[1].divisor == 4);
    assert(vi.vb[2].binding == ZERO_BINDING && vi.vb[2].stride == 0 && vi.vb[2].inputRate == VK_VERTEX_INPUT_RATE_VERTEX && vi.vb[2].divisor == 1);

    // Without divisor extension: instance divisor > 1 clamps to 1
    vk_build_dyn_vi(0x1Fu, 4, attribs, 3, streams, false, test_fmt_fn, &vi, 9);
    assert(vi.vb[1].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE && vi.vb[1].divisor == 1 && vi.divisor[1] == 1);

    // have_divisor == true with stream[1][1] == 1 keeps divisor == 1
    streams[1][1] = 1;
    vk_build_dyn_vi(0x1Fu, 4, attribs, 3, streams, true, test_fmt_fn, &vi, 10);
    assert(vi.vb[1].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE && vi.vb[1].divisor == 1);

    // nattrib == 0, nstream == 0 -> all attrs map to ZERO_BINDING
    vk_build_dyn_vi(0x7u, 0, NULL, 0, NULL, true, test_fmt_fn, &vi, 11);
    assert(vi.na == 3 && vi.nb == 1 && vi.vb[0].binding == ZERO_BINDING && vi.vb[0].divisor == 1);

    // stream == nstream & stride == 0 -> VERTEX rate, divisor 1
    uint32_t edge_attrs[2] = {0x2Eu | (2u << 8), 0x2Eu | (3u << 8)};
    vk_build_dyn_vi(0x3u, 2, edge_attrs, 3, streams, true, test_fmt_fn, &vi, 12);
    assert(vi.nb == 2 && vi.vb[0].binding == 2 && vi.vb[0].inputRate == VK_VERTEX_INPUT_RATE_VERTEX && vi.vb[0].divisor == 1);
    assert(vi.vb[1].binding == 3 && vi.vb[1].stride == 0 && vi.vb[1].inputRate == VK_VERTEX_INPUT_RATE_VERTEX && vi.vb[1].divisor == 1);

    // Max 32 attributes across all 16 streams + ZERO_BINDING = 17 bindings
    uint32_t max_attrs[32];
    uint32_t max_streams[16][2];
    for (int i = 0; i < 16; i++) {
        max_streams[i][0] = 16;
        max_streams[i][1] = 0;
    }
    for (int i = 0; i < 32; i++)
        max_attrs[i] = (i == 31) ? (0x2Eu | (16u << 8)) : (0x2Eu | ((uint32_t)(i & 15) << 8));
    vk_build_dyn_vi(0xFFFFFFFFu, 32, max_attrs, 16, max_streams, true, test_fmt_fn, &vi, 13);
    assert(vi.na == 32 && vi.nb == 17 && vi.used_streams == 0x1FFFFu);

    // Driver allowlist policy: only Turnip and ARM proprietary Mali r49+ enable dyn_vi by default.
    assert(vk_arm_driver_major("v1.r49p1-03bet0-b327f60") == 49);
    assert(vk_arm_driver_major("v1.r50p0-00eac0") == 50);
    assert(vk_arm_driver_major("r44p1") == 44);
    assert(vk_arm_driver_major("foor99p1") == -1);   // preceded by alnum, not delimiter
    assert(vk_arm_driver_major("arm_r49p1") == 49);  // underscore / dot delimiters allowed
    assert(vk_arm_driver_major("v1.r49p") == -1);
    assert(vk_arm_driver_major("1r99p1") == -1);
    assert(vk_arm_driver_major("r49p0") == 49);
    assert(vk_arm_driver_major("v1.r49x1") == -1);
    assert(vk_arm_driver_major("") == -1);
    assert(vk_arm_driver_major(NULL) == -1);

    assert(vk_dyn_vi_driver_default(VK_DRIVER_ID_MESA_TURNIP, "Turnip Adreno (TM) 830") == true);
    assert(vk_dyn_vi_driver_default(VK_DRIVER_ID_MESA_TURNIP, NULL) == true);
    assert(vk_dyn_vi_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1-03bet0") == true);
    assert(vk_dyn_vi_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r52p0-01eac0") == true);
    assert(vk_dyn_vi_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r48p1-01eac0") == false);
    assert(vk_dyn_vi_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r44p1-01eac0") == false);
    assert(vk_dyn_vi_driver_default(VK_DRIVER_ID_ARM_PROPRIETARY, NULL) == false);
    assert(vk_dyn_vi_driver_default(VK_DRIVER_ID_QUALCOMM_PROPRIETARY, "v1.r50p0") == false);
    assert(vk_dyn_vi_driver_default(VK_DRIVER_ID_MESA_RADV, "Mesa 25.0") == false);
    assert(vk_dyn_vi_driver_default(VK_DRIVER_ID_MESA_PANVK, "Mesa 25.0") == false);
    assert(vk_dyn_vi_driver_default(VK_DRIVER_ID_SAMSUNG_PROPRIETARY, "Xclipse 940") == false);

    // Override semantics: LARECOMP_NO_DYN_VI beats everything; LARECOMP_DYN_VI=1/0 overrides driver default
    assert(vk_dyn_vi_want(VK_DRIVER_ID_QUALCOMM_PROPRIETARY, "", true, NULL, false) == false);
    assert(vk_dyn_vi_want(VK_DRIVER_ID_QUALCOMM_PROPRIETARY, "", true, "1", false) == true);
    assert(vk_dyn_vi_want(VK_DRIVER_ID_QUALCOMM_PROPRIETARY, "", false, "1", false) == true);
    assert(vk_dyn_vi_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r44p1", true, "1", false) == true);
    assert(vk_dyn_vi_want(VK_DRIVER_ID_MESA_TURNIP, "", true, "", false) == true);
    assert(vk_dyn_vi_want(VK_DRIVER_ID_MESA_TURNIP, "", true, "0", false) == false);
    assert(vk_dyn_vi_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1", true, NULL, false) == true);
    assert(vk_dyn_vi_want(VK_DRIVER_ID_ARM_PROPRIETARY, "v1.r49p1", false, NULL, false) == false);
    assert(vk_dyn_vi_want(VK_DRIVER_ID_MESA_TURNIP, "", true, "1", true) == false);

    puts("PASS: test_dyn_vertex_input (vk_build_dyn_vi + driver policy)");
    return 0;
}
