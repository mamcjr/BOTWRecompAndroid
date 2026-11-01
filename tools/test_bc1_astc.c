// Test of runtime/nvn/bc1_astc.c: random BC1 blocks decoded by bcdec, and converted to ASTC 4x4 then decoded by
// runtime/nvn/astc.c (bit-exact with ARM's decoder) - the largest difference per kind of block, LDR and sRGB.
// Build (an Android NDK clang, run on the phone):
//   $NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang -O2 -I runtime/nvn \
//     tools/test_bc1_astc.c runtime/nvn/bc1_astc.c runtime/nvn/astc.c -o test_bc1_astc
//   adb push test_bc1_astc /data/local/tmp && adb shell /data/local/tmp/test_bc1_astc [blocks]
#include "../runtime/nvn/astc.h"
#include "../runtime/nvn/bc1_astc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "../runtime/nvn/bcdec.h"

static uint64_t g_rng = 1;
static uint64_t rnd(void) {  // splitmix64
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    const char *kind[3] = {"4 colours", "3 colours", "3 colours + transparent"};
    int worst[2][3] = {{0}}, alpha_bad[2][3] = {{0}}, illegal = 0;
    long count[3] = {0};
    for (long i = 0; i < n; i++) {
        uint8_t bc[8], astc[16], ref[64], out[64];
        uint64_t r = rnd();
        memcpy(bc, &r, 8);
        if (i % 4 == 1) bc[2] = bc[0], bc[3] = bc[1];  // (some c0 == c1)
        if (i % 4 == 2) {  // (near endpoints: small steps)
            bc[2] = bc[0] ^ (uint8_t)(r >> 60), bc[3] = bc[1];
        }
        uint32_t c0 = bc[0] | bc[1] << 8, c1 = bc[2] | bc[3] << 8, idx;
        memcpy(&idx, bc + 4, 4);
        int k = c0 > c1 ? 0 : 1;
        if (k == 1)
            for (int t = 0; t < 16; t++) if ((idx >> 2 * t & 3) == 3) k = 2;
        count[k]++;
        bcdec_bc1(bc, ref, 16);
        bc1_to_astc(bc, astc, 1);
        for (int s = 0; s < 2; s++) {
            if (!astc_decode_block(astc, 4, 4, s, out)) {
                if (illegal++ < 5) printf("illegal block (kind %d): bc1 %016llx\n", k, (unsigned long long)r);
                continue;
            }
            for (int t = 0; t < 16; t++) {
                if (ref[4 * t + 3] != out[4 * t + 3]) alpha_bad[s][k]++;
                if (ref[4 * t + 3] == 0) continue;  // (transparent: only its alpha counts)
                for (int c = 0; c < 3; c++) {
                    int d = abs(ref[4 * t + c] - out[4 * t + c]);
                    if (d > worst[s][k]) worst[s][k] = d;
                }
            }
        }
    }
    int fail = illegal > 0;
    for (int k = 0; k < 3; k++)
        for (int s = 0; s < 2; s++) {
            printf("%-24s %s: %ld blocks, largest colour difference %d, alpha wrong on %d texels\n", kind[k],
                   s ? "sRGB" : "LDR ", count[k], worst[s][k], alpha_bad[s][k]);
            if (worst[s][k] > 3 || alpha_bad[s][k]) fail = 1;
        }
    printf("%s (%d illegal blocks)\n", fail ? "FAIL" : "PASS", illegal);
    return fail;
}
