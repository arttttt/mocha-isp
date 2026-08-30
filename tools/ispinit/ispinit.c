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
typedef int (*NvRmMemHandleAllocAttr_fn)(void *dev, void *attrs, void **out);
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
    printf("[0] requested rate = 0x%x\n", rate);

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

    printf("[3] dlsym: NvRmOpen=%p AllocAttr=%p NvIspOpen=%p "
           "SetConfiguration=%p ProcessFrame=%p SetIspClockRate=%p "
           "GetStatus=%p Close=%p\n",
           nvRmOpen, nvRmMemHandleAllocAttr, nvIspOpen, nvIspSetConfiguration,
           nvIspProcessFrame, nvIspSetIspClockRate, nvIspGetStatus,
           nvIspClose);
    if (nvRmOpen == 0 || nvRmMemHandleAllocAttr == 0 || nvIspOpen == 0 ||
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

    /* [10] one page of memory through AllocAttr(r0=dev, attrs[32], &out);
       the stock dummy's attrs[1]=3, size in attrs[4] (impl-2). No free
       yet: the first submission's state is unknown, an extra release
       would add variables to the experiment. */
    {
        unsigned attrs[32] = {0};
        void *memh = 0;
        unsigned desc[44] = {0}; /* 0xb0 bytes, the stock record size */

        attrs[1] = 3;
        attrs[4] = 4096; /* one page: stride 256 x height 8 = 2048, with headroom */

        printf("[10] NvRmMemHandleAllocAttr(dev=%p, attrs[32], &memh) -> ",
               dev);
        rc = nvRmMemHandleAllocAttr(dev, attrs, &memh);
        printf("rc=0x%x memh=%p\n", rc, memh);

        if (rc != 0 || memh == 0) {
            printf("    alloc failed -- skipping submission, closing\n");
        } else {
            /* [11] the descriptor: seven live words of forty-four */
            desc[0] = 8;          /* height */
            desc[1] = 8;          /* width */
            desc[2] = 0x105a500cu; /* format constant, as-is from the dummy */
            desc[3] = 1;          /* memory type */
            desc[4] = 256;        /* stride, multiple of 64 */
            desc[5] = (unsigned)memh; /* OUR handle */
            desc[9] = 1;          /* 0x24: plane count */

            /* [12] the submission itself. The intent line goes out
               before the call: if the call never returns (a fence wait,
               for instance), the log shows exactly where it stopped. */
            printf("[12] NvIspProcessFrame(hIsp=0x%x, mode=1, desc@stack "
                   "+0x10, aux=zeros) -> calling...\n", hIsp);
            rc = nvIspProcessFrame(hIsp, 1, 0, 0,
                                   0, 0, 0, 0,   /* stack +00..+0c: library ignores */
                                   (unsigned)desc, 0, 0, 0, 0);
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
