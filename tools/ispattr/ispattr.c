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

/* The mapping that holds an address, so a snapshot can be taken without
 * running off the end of it. */
static int map_range(const void *p, uintptr_t *start, uintptr_t *end)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t a = (uintptr_t)p;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        uintptr_t s, e;
        if (sscanf(line, "%lx-%lx", (unsigned long *)&s,
                   (unsigned long *)&e) != 2) continue;
        if (a >= s && a < e) { *start = s; *end = e; found = 1; break; }
    }
    fclose(f);
    return found;
}

/* What the library keeps for each block of registers it will push: the
 * method it goes to, how many words, whether it has been touched, and the
 * words themselves. Finding the one the demosaic call dirtied is the whole
 * point -- it is the register write we could never see in a trace. */
static void report_blocks(const uint8_t *before, const uint8_t *after,
                          uintptr_t base, size_t len)
{
    int shown = 0;
    for (size_t off = 0; off + 16 <= len; off += 4) {
        if (memcmp(before + off, after + off, 4) == 0) continue;
        /* Walk back to a plausible header: a method under 0x1000 with a
         * sane count, whose data region covers this change. */
        for (size_t h = (off > 0x400 ? off - 0x400 : 0); h <= off; h += 4) {
            uint16_t method = *(const uint16_t *)(after + h);
            uint16_t count  = *(const uint16_t *)(after + h + 2);
            uint8_t dirty   = after[h + 6];
            if (method == 0 || method >= 0x1000) continue;
            if (count == 0 || count > 512) continue;
            if (h + 12 + 4u * count > len) continue;
            if (off < h + 12 || off >= h + 12 + 4u * count) continue;
            if (dirty != 1 || after[h + 7] > 8) continue;
            printf("\nblock at +%04zx: method=0x%03x count=%u dirty=%u"
                   " mode=%u  (addr %08lx)\n", h, method, count, dirty,
                   after[h + 7], (unsigned long)(base + h));
            const uint32_t *w = (const uint32_t *)(after + h + 12);
            for (unsigned i = 0; i < count; i++)
                printf("  [%2u] %08x%s", i, w[i],
                       (i % 4 == 3 || i + 1 == count) ? "\n" : "");
            shown++;
            off = h + 12 + 4u * count;   /* skip past what we just printed */
            break;
        }
        if (shown >= 12) break;
    }
    if (shown) return;

    /* No recognisable header: fall back to reporting where the bytes moved,
     * as runs, so the shape of the change is still visible. */
    printf("no tagged block matched; changed runs:\n");
    size_t off = 0;
    int runs = 0;
    while (off + 4 <= len && runs < 24) {
        if (memcmp(before + off, after + off, 4) == 0) { off += 4; continue; }
        size_t s = off;
        while (off + 4 <= len && memcmp(before + off, after + off, 4) != 0)
            off += 4;
        /* What sits just before a change is the header that names it: the
         * method the words are destined for, and how many of them. */
        size_t ctx = s > 0x20 ? s - 0x20 : 0;
        printf("  before +%06zx:", s);
        for (size_t i = ctx; i < s; i += 4)
            printf(" %08x", *(const uint32_t *)(after + i));
        printf("\n");
        printf("  +%06zx (%08lx) %zu words:", s, (unsigned long)(base + s),
               (off - s) / 4);
        for (size_t i = s; i < off && i < s + 32; i += 4)
            printf(" %08x", *(const uint32_t *)(after + i));
        printf("%s\n", (off - s) > 32 ? " ..." : "");
        runs++;
    }
    if (!runs) printf("  (nothing changed at all)\n");
}

int main(int argc, char **argv)
{
    int instance = 2;          /* ISP-B; the other tool uses 1 for ISP-A */
    unsigned maxattr = 256;
    int do_set = 0, do_apply = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--instance=", 11) == 0) instance = atoi(a + 11);
        else if (strncmp(a, "--max=", 6) == 0)  maxattr = (unsigned)atoi(a + 6);
        else if (strcmp(a, "--set") == 0)       do_set = 1;
        else if (strcmp(a, "--apply") == 0)     { do_set = 1; do_apply = 1; }
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
        /* FOUR pointers in each half, not one. Nine and sixteen are the
         * lengths of the arrays those pointers reach, not a count of
         * anything inside the block -- which is why the block fits in
         * sixty-four bytes after all, and why filling only the first
         * pointer left three null ones to be dereferenced.
         *
         * Four three-by-three matrices and four four-by-four ones: one set
         * per Bayer phase, going by the company they keep in the symbol
         * table. Identity to start with -- a one in the middle and nothing
         * else -- because the question is whether the stage comes on, not
         * yet what it computes. The library normalises these to ten-bit
         * fixed point and refuses anything above about eight. */
        static float m[4][9], k[4][16], k2[4][16];
        for (int q = 0; q < 4; q++) {
            memset(m[q], 0, sizeof m[q]);
            memset(k[q], 0, sizeof k[q]);
            memset(k2[q], 0, sizeof k2[q]);
            /* Distinct per phase, so the converted words are recognisable
             * on the other side rather than four identical runs. */
            m[q][4]  = 1.0f / (float)(1 << q);
            k[q][5]  = 1.0f / (float)(1 << q);
            k2[q][5] = 1.0f / (float)(1 << q);
        }

        uint8_t st[0x40];
        memset(st, 0, sizeof st);
        st[0] = 1;                                   /* run the stage */
        uint32_t nine = 9, sixteen = 16;
        memcpy(st + 0x04, &nine, 4);
        for (int q = 0; q < 4; q++)
            memcpy(st + 0x08 + 4 * q, &(void *){ m[q] }, 4);
        st[0x18] = 1;
        memcpy(st + 0x1c, &sixteen, 4);
        for (int q = 0; q < 4; q++)
            memcpy(st + 0x20 + 4 * q, &(void *){ k[q] }, 4);
        /* And a third set at +0x30: the handler reads pointers from there
         * too, which is where it died last -- so the tail is not padding,
         * and three groups of four pointers fill the sixty-four bytes
         * exactly. */
        for (int q = 0; q < 4; q++)
            memcpy(st + 0x30 + 4 * q, &(void *){ k2[q] }, 4);
        (void)coeff;

        printf("block: enable=%u count=%u ptr=%08x  enable2=%u count2=%u"
               " ptr2=%08x\n", st[0], *(uint32_t *)(st + 4),
               *(uint32_t *)(st + 8), st[0x18], *(uint32_t *)(st + 0x1c),
               *(uint32_t *)(st + 0x20));
        fflush(stdout);

        /* Photograph the settings object's whole mapping either side of the
         * call. The library converts our floats to its own fixed point and
         * files them in a block tagged with the method they go to -- and
         * that tag is the answer we have been hunting in register dumps
         * that could never contain it. */
        uintptr_t ms = 0, me = 0;
        uint8_t *before = 0, *after = 0;
        size_t snap = 0;
        /* The whole mapping, not just what follows the handle: the blocks
         * are allocated in their own right and can as easily sit below it. */
        void *watch = 0;
        if (map_range(hSet, &ms, &me)) {
            watch = (void *)ms;
            snap = me - ms;
            if (snap > (8u << 20)) snap = 8u << 20;
            before = malloc(snap);
            after = malloc(snap);
            printf("watching %zu bytes of %08lx..%08lx (handle at %p)\n",
                   snap, (unsigned long)ms, (unsigned long)me, hSet);
        }

        /* Nothing between the two photographs but the call itself -- our own
         * printing runs through a buffer on this very heap, and it was that
         * churn, not the library, filling the last report. */
        uint32_t size = 0x40;
        fflush(stdout);
        if (before && after) memcpy(before, watch, snap);
        int src = HwSettingsSetAttribute(hSet, 8, 0, (uint32_t)(uintptr_t)st,
                                         (uint32_t)(uintptr_t)&size);
        if (before && after) memcpy(after, watch, snap);

        printf("demosaic attribute: rc=%d, size now %u\n", src, size);
        if (before && after)
            report_blocks(before, after, (uintptr_t)watch, snap);

        /* The attribute is accepted. Applying is a separate question: the
         * name that looks right, NvIspHwSettingsApply, shares an address
         * with two getters, which is the shape of a stub. The emitter that
         * actually walks the shadow blocks and pushes them is
         * NvCameraHwSettingsApply, so try that -- and only when asked,
         * since a wrong guess at its arguments takes the process down and
         * the attribute call above is worth keeping. */
        if (src == 0 && do_apply) {
            int (*CameraApply)(void *, void *) =
                dlsym(isp, "NvCameraHwSettingsApply");
            printf("emitter at %p\n", (void *)CameraApply);
            fflush(stdout);
            if (CameraApply) {
                int arc = CameraApply(hSet, hIsp);
                printf("NvCameraHwSettingsApply: rc=%d\n", arc);
            }
        }
        (void)HwSettingsApply;
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
