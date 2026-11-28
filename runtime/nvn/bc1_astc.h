// BC1 / BC3 / BC5_UNORM blocks -> ASTC 4x4 blocks, for GPUs that sample ASTC but not BC (Mali):
// the same 4x4 footprint in 16 bytes instead of 64-byte RGBA8 or 32-byte RG8.
#pragma once
#include <stdint.h>

// n BC1 blocks (8 bytes each) -> n ASTC 4x4 blocks (16 bytes each), texel for texel what bcdec_bc1 decodes (within
// ASTC's interpolation rounding, a step or two of 255); a transparent texel of the 3-colour mode keeps alpha 0, its
// colour the darker endpoint's instead of black
void bc1_to_astc(const uint8_t *bc1, uint8_t *astc, uint64_t n);

// n BC3 blocks (16 bytes each) -> n ASTC 4x4 RGBA blocks (16 bytes each). RGB always uses 4-colour mode; alpha handles
// both 8-level (a0 > a1) and 6-level + 0/255 cutout (a0 <= a1) modes.
void bc3_to_astc(const uint8_t *bc3, uint8_t *astc, uint64_t n);

// n BC5_UNORM blocks (16 bytes each) -> n ASTC 4x4 LA (CEM 4) blocks (16 bytes each), storing logical R in L (R,G,B)
// and logical G in A; consumers remap view swizzles via vk_bc5_la_remap_swizzle().
void bc5_unorm_to_astc_la(const uint8_t *bc5, uint8_t *astc, uint64_t n);
