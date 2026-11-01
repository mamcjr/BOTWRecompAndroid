// BC1 blocks -> ASTC 4x4 blocks, for GPUs that sample ASTC but not BC (Mali): the same 4x4 footprint in 16 bytes,
// where the decode into RGBA8 took 64 (8x a BC1 texture's size; BOTW's BC1 textures went from ~150 MB to ~1.1 GB)
#pragma once
#include <stdint.h>

// n BC1 blocks (8 bytes each) -> n ASTC 4x4 blocks (16 bytes each), texel for texel what bcdec_bc1 decodes (within
// ASTC's interpolation rounding, a step or two of 255); a transparent texel of the 3-colour mode keeps alpha 0, its
// colour the darker endpoint's instead of black
void bc1_to_astc(const uint8_t *bc1, uint8_t *astc, uint64_t n);
