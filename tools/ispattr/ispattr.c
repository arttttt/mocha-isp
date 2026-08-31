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
        uint8_t st[0x40];
        memset(st, 0, sizeof st);
        st[0] = 1;                                   /* run the stage */
        memcpy(st + 4, &(void *){ coeff }, 4);
        /* The second pair is left alone. Filling it with the same pointer
         * was a guess, and the handler rejects the block on a check deeper
         * than the shape ones -- so give it only what we know. */

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
