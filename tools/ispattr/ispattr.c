/*
 * ispattr — which attributes does the ISP library accept?
 *
 * The demosaic is not switched on by a register. Its settings object holds a
 * byte at offset zero, and the library's own copy routine skips the whole
 * nine-word coefficient block when that byte is clear -- so what turns the
 * stage on is a field, set through the library, and applied by it. That is
 * why it never appears in a command trace: a trace shows the result, not the
 * decision, and no amount of poking registers was going to find it.
 *
 * This asks the library directly. Create a settings object, then walk the
 * attribute numbers and see which ones it accepts. The ones that answer are
 * the surface we can actually drive; among them is the one we want.
 *
 * Nothing here touches the hardware: it opens the library, allocates a
 * settings object and asks questions.
 *
 * Build: tools/ispattr/build-ispattr.sh (on the build server)
 * Usage: ./ispattr [--instance=N] [--max=N] [--set]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

int main(int argc, char **argv)
{
    int instance = 2;          /* ISP-B; the other tool uses 1 for ISP-A */
    unsigned maxattr = 256;
    int do_set = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--instance=", 11) == 0) instance = atoi(a + 11);
        else if (strncmp(a, "--max=", 6) == 0)  maxattr = (unsigned)atoi(a + 6);
        else if (strcmp(a, "--set") == 0)       do_set = 1;
        else { printf("unknown option %s\n", a); return 1; }
    }

    void *rm = dlopen("libnvrm.so", RTLD_NOW);
    void *isp = dlopen("libnvisp_v3.so", RTLD_NOW);
    if (!rm || !isp) { printf("dlopen: %s\n", dlerror()); return 1; }

    int (*NvRmOpen)(void **, uint32_t) = dlsym(rm, "NvRmOpen");
    int (*NvIspOpen)(void *, int, void **) = dlsym(isp, "NvIspOpen");
    int (*NvIspClose)(void *) = dlsym(isp, "NvIspClose");
    int (*HwSettingsCreate)(void *, void **) =
        dlsym(isp, "NvIspHwSettingsCreate");
    /* Five arguments, not four. The code moves r1 into r7 as the attribute
     * number, shifts r2 and r3 down, and picks a fifth off the stack --
     * then does `attr - 1` against 15 and jumps through a table, so the
     * numbers it knows are 1 to 16 and nothing else. Passing four left the
     * fifth as stack rubbish, which is what crashed it. */
    int (*HwSettingsSetAttribute)(void *, uint32_t, uint32_t, uint32_t,
                                  uint32_t) =
        dlsym(isp, "NvIspHwSettingsSetAttribute");
    int (*HwSettingsGetAttribute)(void *, uint32_t, void *, uint32_t) =
        dlsym(isp, "NvIspHwSettingsGetAttribute");
    int (*HwSettingsApply)(void *, void *) =
        dlsym(isp, "NvIspHwSettingsApply");
    int (*HwSettingsDestroy)(void *) = dlsym(isp, "NvIspHwSettingsDestroy");

    printf("symbols: RmOpen=%p IspOpen=%p Create=%p Set=%p Get=%p Apply=%p\n",
           (void *)NvRmOpen, (void *)NvIspOpen, (void *)HwSettingsCreate,
           (void *)HwSettingsSetAttribute, (void *)HwSettingsGetAttribute,
           (void *)HwSettingsApply);
    if (!NvRmOpen || !NvIspOpen || !HwSettingsCreate ||
        !HwSettingsSetAttribute) {
        printf("the library is missing what this needs\n");
        return 1;
    }

    void *hRm = 0;
    int rc = NvRmOpen(&hRm, 0);
    printf("NvRmOpen: rc=%d handle=%p\n", rc, hRm);
    if (!hRm) return 1;

    void *hIsp = 0;
    rc = NvIspOpen(hRm, instance, &hIsp);
    printf("NvIspOpen(instance %d): rc=%d handle=%p\n", instance, rc, hIsp);
    if (!hIsp) return 1;

    /* Configure the session before asking for settings. The other tool that
     * drives this library does it in this order, and a settings object made
     * against an unconfigured session is a fair suspect for a handler that
     * dies rather than answers. The selector set is the one the camera
     * stack sends. */
    int (*NvIspSetConfiguration)(void *, int, void *, unsigned *) =
        dlsym(isp, "NvIspSetConfiguration");
    if (NvIspSetConfiguration) {
        uint32_t cfg[16] = {
            1, 7, 9, 0xa, 3, 0, 6, 8, 0x11, 0xf, 0xc, 0xe, 0xb, 0, 0x10, 0xd
        };
        unsigned sz = sizeof cfg;
        int crc = NvIspSetConfiguration(hIsp, 1, cfg, &sz);
        printf("NvIspSetConfiguration: rc=%d\n", crc);
    }

    void *hSet = 0;
    rc = HwSettingsCreate(hIsp, &hSet);
    printf("HwSettingsCreate: rc=%d handle=%p\n", rc, hSet);
    if (!hSet) { if (NvIspClose) NvIspClose(hIsp); return 1; }

    /* Reading is a dead end: three different names in this library share
     * one address, which is the shape of a stub that answers nothing. The
     * write is the real entry point, so probe that -- an attribute number
     * the library knows will be accepted, one it does not will be refused,
     * and the pattern of the two maps the surface. */
    if (do_set) {
        /* Attribute 8 is the demosaic. Its handler sits in the slot at
         * +0x12a4, and the case that loads that slot is the one attribute 8
         * jumps to. The handler wants the third argument zero, the fourth a
         * pointer to a sixty-four byte block and the fifth a pointer to its
         * size -- it compares that size against 0x40 and, if it disagrees,
         * writes 0x40 back and returns, which is the library telling the
         * caller how big the block should have been.
         *
         * Inside the block: a byte at zero that decides whether the stage
         * runs at all, a pointer at four to the nine coefficient words, and
         * a second such pair at 0x18 and 0x1c. */
        static uint32_t coeff[9] = {
            0x3f3fcff3, 0x00000000, 0x04c1304c, 0x08220882, 0x00000000,
            0x03d0f43d, 0x08621886, 0x01204812, 0x06e1b86e
        };
        /* The fields at +0x04 and +0x1c are COUNTS, not pointers, and the
         * handler insists on nine and sixteen -- when they disagree it
         * writes the right number back and returns ten, which is exactly
         * the answer we were getting while passing addresses there. The
         * data itself is inline: the first array is taken from +0x08 and
         * the second from +0x20. */
        /* Declared as sixty-four bytes because that is what the handler
         * demands, but allocated with room to spare: past the count checks
         * it reads the arrays inline, and sixteen entries starting at +0x20
         * do not fit in sixty-four. Reading into our own slack is harmless;
         * reading off the end of a tight buffer is what just killed it. */
        /* And the fields at +0x08 and +0x20 are pointers after all -- to
         * arrays of FLOATS. The routine at 0x1db4 walks them looking for
         * the largest magnitude and scales the lot into fixed point, which
         * is where the register words come from. That is why a zero there
         * killed the process: it was dereferenced.
         *
         * So the coefficients are given as nine and sixteen real numbers
         * and the library does the conversion. Ones to begin with -- what
         * matters first is whether the stage comes on at all. */
        static float c9[9]  = { 1, 1, 1, 1, 1, 1, 1, 1, 1 };
        static float c16[16] = { 1, 1, 1, 1, 1, 1, 1, 1,
                                 1, 1, 1, 1, 1, 1, 1, 1 };
        uint8_t st[256];
        memset(st, 0, sizeof st);
        st[0] = 1;                                   /* run the stage */
        uint32_t nine = 9, sixteen = 16;
        memcpy(st + 0x04, &nine, 4);
        memcpy(st + 0x08, &(void *){ c9 }, 4);
        st[0x18] = 1;
        memcpy(st + 0x1c, &sixteen, 4);
        memcpy(st + 0x20, &(void *){ c16 }, 4);
        (void)coeff;

        printf("block: enable=%u count=%u ptr=%08x  enable2=%u count2=%u"
               " ptr2=%08x\n", st[0], *(uint32_t *)(st + 4),
               *(uint32_t *)(st + 8), st[0x18], *(uint32_t *)(st + 0x1c),
               *(uint32_t *)(st + 0x20));
        fflush(stdout);

        uint32_t size = 0x40;
        int src = HwSettingsSetAttribute(hSet, 8, 0, (uint32_t)(uintptr_t)st,
                                         (uint32_t)(uintptr_t)&size);
        printf("demosaic attribute: rc=%d, size now %u\n", src, size);

        if (src == 0 && HwSettingsApply) {
            int arc = HwSettingsApply(hIsp, hSet);
            printf("HwSettingsApply: rc=%d\n", arc);
        }
    } else {
        printf("attributes the library accepts (it knows 1..16):\n");
        for (unsigned id = 1; id <= 16 && id <= maxattr; id++) {
            fflush(stdout);
            int src = HwSettingsSetAttribute(hSet, id, 0, 0, 0);
            printf("  attr %2u: rc=%d\n", id, src);
            fflush(stdout);
        }
    }
    (void)HwSettingsGetAttribute;

    if (HwSettingsDestroy) HwSettingsDestroy(hSet);
    if (NvIspClose) NvIspClose(hIsp);
    return 0;
}
