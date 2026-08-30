/*
 * bayer_test.c -- positive control for the Bayer generator, host build.
 * Expected values are HAND-DERIVED (independently of the generator):
 *
 * 8x8, stride 256, bpp 2. The first four u32 words all belong to ROW 0
 * (16 pixels bytes = 4 words; stride is 256). Pixels 0-3 sit in
 * quadrant 0, pixels 4-7 in quadrant 1 (+0x100). For RGGB:
 *   row0 = R 0x011 Gr 0x022 | R 0x111 Gr 0x122 (LE words below)
 * and the checksum is 0x6AA0 for every order (permutation invariant).
 */
#include <stdio.h>
#include "bayer_gen.h"

struct expect {
    const char *name;
    unsigned order;
    unsigned w[4];
};

static const struct expect expects[4] = {
    /* x0,x1 | x4,x5 of row 0, packed LE as u32 */
    { "rggb", 0, { 0x00220011u, 0x00220011u, 0x01220111u, 0x01220111u } },
    { "bggr", 1, { 0x00330044u, 0x00330044u, 0x01330144u, 0x01330144u } },
    { "grbg", 2, { 0x00110022u, 0x00110022u, 0x01110122u, 0x01110122u } },
    { "gbrg", 3, { 0x00440033u, 0x00440033u, 0x01440133u, 0x01440133u } },
};

int main(void)
{
    static unsigned char buf[4096];
    int fails = 0;
    int e;

    for (e = 0; e < 4; e++) {
        unsigned sum = bayer_fill(buf, 8, 8, 256, 2, expects[e].order);
        unsigned w[4];
        int k;

        for (k = 0; k < 4; k++)
            w[k] = (unsigned)buf[k * 4] |
                   ((unsigned)buf[k * 4 + 1] << 8) |
                   ((unsigned)buf[k * 4 + 2] << 16) |
                   ((unsigned)buf[k * 4 + 3] << 24);

        printf("%s: checksum=0x%x first=0x%08x 0x%08x 0x%08x 0x%08x\n",
               expects[e].name, sum, w[0], w[1], w[2], w[3]);

        if (sum != 0x6aa0u) {
            printf("  FAIL checksum: expected 0x6AA0\n");
            fails++;
        }
        for (k = 0; k < 4; k++) {
            if (w[k] != expects[e].w[k]) {
                printf("  FAIL word %d: expected 0x%08x\n", k,
                       expects[e].w[k]);
                fails++;
            }
        }
    }

    if (fails != 0) {
        printf("FAILED: %d mismatch(es)\n", fails);
        return 1;
    }
    printf("PASSED: 4 orders, checksums and first words as hand-derived\n");
    return 0;
}
