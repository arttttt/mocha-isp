/*
 * ispinit -- raise the ISP by hand and give it back, from a shell.
 *
 * The first thing we do ourselves instead of watching. The sequence is
 * minimal on purpose (the contract from the lead, 2026-08-30):
 *
 *   NvRmOpen(&dev)         -- stub in libnvrm.so: writes 1 to *out, returns 0.
 *                             So dev is the VALUE 1, not an address; the live
 *                             hook log confirms the stock passes r0=1 into
 *                             NvIspOpen.
 *   NvIspOpen(dev, 1, &h)  -- instance 1 for the first instance (stock opens
 *                             1 then 2). On failure it cleans up after
 *                             itself; we free nothing by hand, NvIspClose
 *                             owns the teardown, including the host1x
 *                             channel.
 *   NvIspSetIspClockRate(h, 0x003fffff) -- nonzero on purpose. impl-2: on a
 *                             fresh context the rate cache is zero, so
 *                             GetStatus(6) alone would read that same zero
 *                             and we could not tell a raised ISP from an
 *                             unraised one. The stock itself calls this with
 *                             r1=0, which is a no-op by cache equality.
 *   NvIspGetStatus(h, 6, &v, &size) -- size=4 in, actual size out; after the
 *                             clock call it must print the written rate,
 *                             not zero.
 *   NvIspClose(h)          -- nothing released manually: a manual release
 *                             would be a double free.
 *
 * Every step prints its result BEFORE the next call that may not return;
 * stdout is unbuffered, so whatever ran reaches the shell even mid-crash.
 *
 * Exit is a normal return from main. Nobody has established that the
 * kernel reclaims the channel behind a killed process, so we do not kill
 * the process and do not leave via a signal.
 *
 * Built WITH crt0 and libc (build-ispinit.sh), not -nostdlib: the -nostdlib
 * build died inside the linker on the very first dlopen -- loading a
 * library whose dependencies include libc runs constructors against a
 * process whose libc was never initialised. This is a separate process, so
 * the mediaserver rule (no symbol versioning, ever) does not extend here:
 * if it does not load, it does not load, nothing breaks.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bayer_gen.h"

/*
 * dlopen flags are the bionic 4.4 values, NOT the NDK header values: on
 * this device RTLD_NOW is 0 and RTLD_GLOBAL is 2, while the r21e header
 * constants encode modern bionic and would read as RTLD_GLOBAL here.
 * Verified on the device; see the long comment in shim/src/nvisp_shim.c.
 * We want NOW | LOCAL, which is 0.
 */
#define ISPINIT_DLOPEN_FLAGS 0

static const char nvrm_path[] = "libnvrm.so";
static const char nvisp_path[] = "/system/vendor/lib/libnvisp_v3.real.so";

/*
 * The stock's configuration words, verbatim from the live dump
 * (out/hook-config.txt, identical for both instances):
 *   mode 1, size 64: sixteen words of format selectors;
 *   mode 2, size 4:  the single word 2.
 * Cross-checked twice: the hook dump and the lead's mail agree word for
 * word. A typo here is a wrong format with no visible cause.
 */
static const unsigned cfg_mode1[16] = {
    0x00000001, 0x00000007, 0x00000009, 0x0000000a,
    0x00000003, 0x00000000, 0x00000006, 0x00000008,
    0x00000011, 0x0000000f, 0x0000000c, 0x0000000e,
    0x0000000b, 0x00000000, 0x00000010, 0x0000000d,
};
static const unsigned cfg_mode2 = 2;


/* --- the five calls --- */

typedef int (*NvRmOpen_fn)(void **out);
typedef int (*NvRmMemHandleCreate_fn)(unsigned param0, void **out,
                                      unsigned size);
typedef int (*NvRmMemHandleAllocAttr_fn)(unsigned size, void *attrs,
                                         void **out);
typedef int (*NvIspOpen_fn)(unsigned devHandle, unsigned instance,
                            unsigned *hIsp);
typedef int (*NvIspSetConfiguration_fn)(unsigned hIsp, unsigned mode,
                                        const void *buf, unsigned *size);
typedef int (*NvIspSetIspClockRate_fn)(unsigned hIsp, unsigned rate);
typedef int (*NvIspGetStatus_fn)(unsigned hIsp, unsigned statusId,
                                 void *value, unsigned *size);
typedef int (*NvIspClose_fn)(unsigned hIsp);

/*
 * ProcessFrame: 4 register args + 9 stack words. The hook's live stack
 * (caller-sp coordinates) shows the five pointers at +0x10..+0x20 and
 * four words the library never reads at +0x00..+0x0c -- in C terms the
 * stack argument area starts at +0x00, so the descriptors are
 * parameters 9..13 and the unread words are parameters 5..8, mirrored
 * as zeros here (the stock's 3280/2460 there were session sensor
 * geometry; we encode nothing of the session).
 */
typedef int (*NvIspProcessFrame_fn)(unsigned hIsp, unsigned mode,
                                    unsigned r2, unsigned r3,
                                    unsigned s5, unsigned s6,
                                    unsigned s7, unsigned s8,
                                    unsigned inDesc, unsigned aux1,
                                    unsigned aux2, unsigned aux3,
                                    unsigned aux4);

int main(int argc, char **argv)
{
    void *nvrm;
    void *nvisp;
    NvRmOpen_fn nvRmOpen;
    NvRmMemHandleCreate_fn nvRmMemHandleCreate;
    NvRmMemHandleAllocAttr_fn nvRmMemHandleAllocAttr;
    NvIspOpen_fn nvIspOpen;
    NvIspSetConfiguration_fn nvIspSetConfiguration;
    NvIspProcessFrame_fn nvIspProcessFrame;
    NvIspSetIspClockRate_fn nvIspSetIspClockRate;
    NvIspGetStatus_fn nvIspGetStatus;
    NvIspClose_fn nvIspClose;
    void *dev = 0;
    unsigned hIsp = 0;
    unsigned value = 0;
    unsigned size = 4;
    unsigned rate = 0x003fffffu;
    unsigned order;
    unsigned a1_val;         /* AllocAttr's first argument override; the
                                 working value is the device VALUE (1 in
                                 our runs), used when unset */
    int a1_set = 0;
    unsigned attr_val[8];
    unsigned attr_set[8] = {0};
    int aux_on[3] = {1, 1, 1}; /* slots +18, +1c, +20 */
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);

    /*
     * Optional rate argument: "ispinit [rate]". Control experiment -- the
     * first run read 600000 (0x927c0) after we asked for the max, which
     * proves life only if a smaller request comes back as requested.
     * strtol, no frills; anything unparseable keeps the default.
     */
    if (argc > 1) {
        char *end = 0;
        long v = strtol(argv[1], &end, 0);
        if (end != argv[1] && *end == '\0' && v > 0)
            rate = (unsigned)v;
    }

    /* mosaic order, CLI-switchable like the rate: our sensor is SRGGB10
       per the kernel, but the ISP-visible order depends on readout
       mirroring, so we will be cycling through them. Default RGGB when
       the argument is absent; an explicit but unknown name is an error. */
    order = 0;
    if (argc > 2) {
        int k, found = 0;
        for (k = 0; k < 4; k++) {
            if (strcmp(argv[2], order_names[k]) == 0) {
                order = (unsigned)k;
                found = 1;
            }
        }
        if (!found) {
            printf("[0] bad order '%s', use rggb|bggr|grbg|gbrg\n", argv[2]);
            return 1;
        }
    }

    /* [attr=]idx:val allocation-attribute overrides and the buffer SIZE,
       several overrides allowed (hex or decimal). A token WITH ':' is an
       attribute override (idx 0..7; attrs[0] is a pointer -- overridable
       but almost certainly wrong); a token WITHOUT ':' is the buffer
       size. Unspecified values stay per the recipe. */
    for (int ai = 3; ai < argc; ai++) {
        char *tok = argv[ai];
        char *colon;
        char *e1 = 0, *e2 = 0;
        long idx, val;

        if (strncmp(tok, "aux=", 4) == 0) {
            /* aux=<hexslot>:on|off -- fill one of the three remaining
               stack slots (+18, +1c, +20) with a zeroed buffer, or pass
               a deliberate NULL; default is all on */
            char *colon = strchr(tok + 4, ':');
            long slot;
            int *flag;
            if (colon == 0) {
                printf("[0] bad aux '%s', use aux=<hexslot>:on|off\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            slot = strtol(tok + 4, &e1, 16); /* slots are hex offsets:
                                                18, 1c, 20 ("0x" ok too) */
            if (e1 == tok + 4 || *e1 != '\0') {
                printf("[0] bad aux slot '%s'\n", argv[ai]);
                return 1;
            }
            if (slot == 0x18)
                flag = &aux_on[0];
            else if (slot == 0x1c)
                flag = &aux_on[1];
            else if (slot == 0x20)
                flag = &aux_on[2];
            else {
                printf("[0] unknown aux slot 0x%lx, use 18|1c|20\n", slot);
                return 1;
            }
            if (strcmp(colon + 1, "on") == 0)
                *flag = 1;
            else if (strcmp(colon + 1, "off") == 0)
                *flag = 0;
            else {
                printf("[0] bad aux state '%s', use on|off\n", colon + 1);
                return 1;
            }
            continue;
        }
        if (strncmp(tok, "attr=", 5) == 0)
            tok += 5;
        colon = strchr(tok, ':');
        if (colon == 0) {
            /* no colon: override of AllocAttr's first argument */
            val = strtol(tok, &e2, 0);
            if (e2 == tok || *e2 != '\0' || val <= 0) {
                printf("[0] bad value '%s', use a positive number\n",
                       argv[ai]);
                return 1;
            }
            a1_val = (unsigned)val;
            a1_set = 1;
            continue;
        }
        *colon = '\0';
        idx = strtol(tok, &e1, 0);
        val = strtol(colon + 1, &e2, 0);
        if (e1 == tok || *e1 != '\0' || e2 == colon + 1 || *e2 != '\0' ||
            idx < 0 || idx > 7 || val < 0 || val > 0xffffffffl) {
            printf("[0] bad attr '%s', use [attr=]idx:val, idx 0..7\n",
                   argv[ai]);
            return 1;
        }
        attr_set[idx] = 1;
        attr_val[idx] = (unsigned)val;
    }
    printf("[0] requested rate = 0x%x, order = %s\n", rate,
           order_names[order]);

    /*
     * [gen] Generator self-test on a HOST staging buffer: FIRST, before
     * anything that can crash -- it needs nothing but malloc, and its
     * whole point is to be visible when everything else dies. 8x8
     * stride 256 bpp 2 (the dummy geometry, deliberate): for RGGB the
     * first four u32 words must be
     *   00220011 00220011 01220111 01220111
     * (row 0 spans four WORDS because stride is 256, not 16: pixels 4-7
     * sit in quadrant 1, +0x100) and the checksum must be 0x6AA0 --
     * permutation invariant, the same for all four orders. The numbers
     * are asserted on the host by bayer_test.sh.
     */
    {
        unsigned char *stage = malloc(4096);
        if (stage == 0) {
            printf("[gen] malloc failed -- generator untested\n");
        } else {
            unsigned sum = bayer_fill(stage, 8, 8, 256, 2, order);
            unsigned w[4];
            int k;
            for (k = 0; k < 4; k++)
                w[k] = (unsigned)stage[k * 4] |
                       ((unsigned)stage[k * 4 + 1] << 8) |
                       ((unsigned)stage[k * 4 + 2] << 16) |
                       ((unsigned)stage[k * 4 + 3] << 24);
            printf("[gen] bayer_fill(8x8, stride=256, bpp=2, order=%s) "
                   "checksum=0x%x first=0x%08x 0x%08x 0x%08x 0x%08x\n",
                   order_names[order], sum, w[0], w[1], w[2], w[3]);
            free(stage);
        }
    }

    /* [1] libnvrm -- gives us NvRmOpen, the only way to a device handle */
    printf("[1] dlopen(\"%s\") -> ", nvrm_path);
    nvrm = dlopen(nvrm_path, ISPINIT_DLOPEN_FLAGS);
    printf("%p\n", nvrm);
    if (nvrm == 0) {
        printf("    dlerror: %s\n", dlerror());
        return 1;
    }

    /* [2] the stock ISP library, by its explicit deployed path
       (the shim occupies libnvisp_v3.so; the stock one is .real) */
    printf("[2] dlopen(\"%s\") -> ", nvisp_path);
    nvisp = dlopen(nvisp_path, ISPINIT_DLOPEN_FLAGS);
    printf("%p\n", nvisp);
    if (nvisp == 0) {
        printf("    dlerror: %s\n", dlerror());
        return 1;
    }

    /* [3] the symbols; a miss here is printed and fatal */
    nvRmOpen = (NvRmOpen_fn)dlsym(nvrm, "NvRmOpen");
    nvRmMemHandleCreate =
        (NvRmMemHandleCreate_fn)dlsym(nvrm, "NvRmMemHandleCreate");
    nvRmMemHandleAllocAttr =
        (NvRmMemHandleAllocAttr_fn)dlsym(nvrm, "NvRmMemHandleAllocAttr");
    nvIspOpen = (NvIspOpen_fn)dlsym(nvisp, "NvIspOpen");
    nvIspSetConfiguration =
        (NvIspSetConfiguration_fn)dlsym(nvisp, "NvIspSetConfiguration");
    nvIspProcessFrame =
        (NvIspProcessFrame_fn)dlsym(nvisp, "NvIspProcessFrame");
    nvIspSetIspClockRate =
        (NvIspSetIspClockRate_fn)dlsym(nvisp, "NvIspSetIspClockRate");
    nvIspGetStatus = (NvIspGetStatus_fn)dlsym(nvisp, "NvIspGetStatus");
    nvIspClose = (NvIspClose_fn)dlsym(nvisp, "NvIspClose");

    printf("[3] dlsym: NvRmOpen=%p HandleCreate=%p AllocAttr=%p NvIspOpen=%p "
           "SetConfiguration=%p ProcessFrame=%p SetIspClockRate=%p "
           "GetStatus=%p Close=%p\n",
           nvRmOpen, nvRmMemHandleCreate, nvRmMemHandleAllocAttr, nvIspOpen,
           nvIspSetConfiguration,
           nvIspProcessFrame, nvIspSetIspClockRate, nvIspGetStatus,
           nvIspClose);
    if (nvRmOpen == 0 || nvRmMemHandleCreate == 0 ||
        nvRmMemHandleAllocAttr == 0 || nvIspOpen == 0 ||
        nvIspSetConfiguration == 0 || nvIspProcessFrame == 0 ||
        nvIspSetIspClockRate == 0 || nvIspGetStatus == 0 ||
        nvIspClose == 0) {
        printf("    dlerror: %s\n", dlerror());
        return 1;
    }

    /* [4] device handle: the VALUE lands in dev (the stub writes 1) */
    printf("[4] NvRmOpen(&dev) -> ");
    rc = nvRmOpen(&dev);
    printf("rc=%d dev=%p\n", rc, dev);
    if (rc != 0)
        return 1;

    /* [5] open instance 1. On failure the library cleans up after
       itself; we release nothing here, that would be a double free. */
    printf("[5] NvIspOpen(dev=%p, instance=1, &hIsp) -> ", dev);
    rc = nvIspOpen((unsigned)dev, 1, &hIsp);
    printf("rc=0x%x hIsp=0x%x\n", (unsigned)rc, hIsp);
    if (rc != 0)
        return 1;

    /* [6] configure, mode 1: the stock's sixteen selector words, size 64
       in. The size AFTER the call matters: rc 10 means the library
       disagreed and wrote the size it wants -- the fix-and-retry
       mechanism. We print what it said instead of guessing. */
    size = 64;
    printf("[6] NvIspSetConfiguration(hIsp=0x%x, mode=1, cfg16, &size=64) "
           "-> ", hIsp);
    rc = nvIspSetConfiguration(hIsp, 1, cfg_mode1, &size);
    printf("rc=0x%x size-after=%u\n", (unsigned)rc, size);

    /* [7] configure, mode 2: the single word 2, size 4 in */
    size = 4;
    printf("[7] NvIspSetConfiguration(hIsp=0x%x, mode=2, &cfg_mode2, "
           "&size=4) -> ", hIsp);
    rc = nvIspSetConfiguration(hIsp, 2, &cfg_mode2, &size);
    printf("rc=0x%x size-after=%u\n", (unsigned)rc, size);

    /* [8] set the requested rate: the zero cache would answer the next
       read with zero and hide whether the ISP is alive at all; the max
       is clipped by the clock tree (600000 of 0x3fffff), and a smaller
       request must come back as requested -- that is the control */
    printf("[8] NvIspSetIspClockRate(hIsp=0x%x, rate=0x%x) -> ", hIsp, rate);
    rc = nvIspSetIspClockRate(hIsp, rate);
    printf("rc=0x%x\n", (unsigned)rc);

    /* [9] status id 6, 4 bytes in, actual size out; must be the rate
       just written, not zero */
    size = 4;
    printf("[9] NvIspGetStatus(hIsp=0x%x, id=6, &value, size=4) -> ", hIsp);
    rc = nvIspGetStatus(hIsp, 6, &value, &size);
    printf("rc=0x%x size=%u value=0x%x\n", (unsigned)rc, size, value);

    /*
     * [10..12] submit the placeholder frame. First reproduce what the
     * hardware already accepted (the stock's 8x8 dummy: format 0x105a500c
     * from the stock pool, stride 256, one plane), with OUR buffer and
     * OUR handle; geometry and format change one step at a time after
     * this works. The numbers 8x8 are copied deliberately as known-good,
     * the handle is ours from the allocation -- never copied, it lives
     * one run.
     */

    /*
     * [10] the WORKING ATTRIBUTES -- read from INTERCEPTION, not derived.
     * nvrmlog caught the stock's live calls in our own process (ten
     * CREATE/ALLOC/MMAP triples, rc=0, identical attrs every time):
     *
     *   arg1      = 0x1      the device VALUE. Three derived answers --
     *                        pointer, Create handle, bare size -- were
     *                        all wrong; this one reproduced rc=0 first try.
     *   attrs[0]  = 0        no tag list at all
     *   attrs[1]  = 0        not the tagged path
     *   attrs[2]  = 0x20     alignment
     *   attrs[3]  = 2        memory type (2, not 1)
     *   attrs[4]  = 0x40000  256 KiB -- the size DOES live in attrs[4]
     *
     * CLI overrides still land on top of these defaults; they exist for
     * exactly this kind of one-run experiment. (The two-step Create+Alloc
     * experiment lives in git history, commit be257b4.)
     */
    {
        unsigned attrs_base[8] = { 0, 0, 0x20, 2, 0x40000u, 0, 0, 0 };
        unsigned attrs_in[8];
        unsigned attrs_out[8];
        void *memh_in = 0;
        void *memh_out = 0;
        unsigned desc_in[44] = {0};  /* 0xb0 bytes each, the stock
                                        record size */
        unsigned desc_out[44] = {0};
        unsigned aux18[44] = {0};  /* stack +18 */
        unsigned aux1c[44] = {0};  /* stack +1c */
        unsigned aux20[44] = {0};  /* stack +20 */
        unsigned a1;
        int k;

        for (k = 0; k < 8; k++) {
            attrs_in[k] = attrs_base[k];
            attrs_out[k] = attrs_base[k];
        }
        for (k = 0; k < 8; k++) {
            if (attr_set[k]) {
                attrs_in[k] = attr_val[k];
                attrs_out[k] = attr_val[k];
            }
        }

        printf("[10] attrs in|out:");
        for (k = 0; k < 8; k++)
            printf(" [%d]=0x%x|0x%x%s", k, attrs_in[k], attrs_out[k],
                   attr_set[k] ? "*" : "");
        printf("\n");

        a1 = a1_set ? a1_val : (unsigned)dev;
        printf("[10] NvRmMemHandleAllocAttr(a1=0x%x, attrs, &memh_in) "
               "[input] -> ", a1);
        rc = nvRmMemHandleAllocAttr(a1, attrs_in, &memh_in);
        printf("rc=0x%x memh_in=%p\n", (unsigned)rc, memh_in);

        if (rc == 0 && memh_in != 0) {
            printf("[10] NvRmMemHandleAllocAttr(a1=0x%x, attrs, &memh_out) "
                   "[output] -> ", a1);
            rc = nvRmMemHandleAllocAttr(a1, attrs_out, &memh_out);
            printf("rc=0x%x memh_out=%p\n", (unsigned)rc, memh_out);
        }

        if (rc != 0 || memh_in == 0 || memh_out == 0) {
            printf("    alloc failed -- skipping submission, closing\n");
        } else {
            /* [11] two descriptors, same layout as the stock dummy:
               seven live words of forty-four; identical 8x8 geometry is
               a deliberate first try -- the output requirements are
               unknown, and a refusal would be informative. */
            desc_in[0] = 8;           /* height */
            desc_in[1] = 8;           /* width */
            desc_in[2] = 0x105a500cu; /* format constant, from the dummy */
            desc_in[3] = 1;           /* memory type */
            desc_in[4] = 256;         /* stride, multiple of 64 */
            desc_in[5] = (unsigned)memh_in;  /* OUR input handle */
            desc_in[9] = 1;           /* 0x24: plane count */

            desc_out[0] = 8;
            desc_out[1] = 8;
            desc_out[2] = 0x105a500cu;
            desc_out[3] = 1;
            desc_out[4] = 256;
            desc_out[5] = (unsigned)memh_out; /* OUR output handle */
            desc_out[9] = 1;

            printf("[11] desc_in=%p (h=8 w=8 fmt=0x105a500c stride=256 "
                   "memh=0x%x planes=1)\n", desc_in, desc_in[5]);
            printf("[11] desc_out=%p (h=8 w=8 fmt=0x105a500c stride=256 "
                   "memh=0x%x planes=1)\n", desc_out, desc_out[5]);

            /* [12] the submission itself. The intent line goes out
               before the call: if the call never returns (a fence wait,
               for instance), the log shows exactly where it stopped. */
            printf("[12] aux slots: +18=%p(%s) +1c=%p(%s) +20=%p(%s)\n",
                   aux_on[0] ? (void *)aux18 : (void *)0,
                   aux_on[0] ? "on" : "off",
                   aux_on[1] ? (void *)aux1c : (void *)0,
                   aux_on[1] ? "on" : "off",
                   aux_on[2] ? (void *)aux20 : (void *)0,
                   aux_on[2] ? "on" : "off");
            printf("[12] NvIspProcessFrame(hIsp=0x%x, mode=1, "
                   "in@+0x10=%p, out@+0x14=%p) -> calling...\n",
                   hIsp, desc_in, desc_out);
            rc = nvIspProcessFrame(hIsp, 1, 0, 0,
                                   0, 0, 0, 0,   /* stack +00..+0c: library ignores */
                                   (unsigned)desc_in, (unsigned)desc_out,
                                   aux_on[0] ? (unsigned)aux18 : 0,
                                   aux_on[1] ? (unsigned)aux1c : 0,
                                   aux_on[2] ? (unsigned)aux20 : 0);
            printf("    ProcessFrame returned rc=0x%x\n", (unsigned)rc);
        }
    }

    /* [13] hand it back, completely: NvIspClose releases everything
       itself, including the host1x channel. */
    printf("[13] NvIspClose(hIsp=0x%x) -> ", hIsp);
    rc = nvIspClose(hIsp);
    printf("rc=0x%x\n", (unsigned)rc);

    printf("done\n");
    return 0;
}
