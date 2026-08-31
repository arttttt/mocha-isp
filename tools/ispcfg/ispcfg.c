/*
 * ispcfg — bring the ISP pipeline up through libnvisp_v3, then get out of
 * the way.
 *
 * Everything the reprocess tool can write from a command gather leaves the
 * memory path monochrome: a synthetic bayer frame whose bands light only
 * the red sites and only the blue sites comes back byte-identical, with the
 * output value tracking nothing but how many pixels are lit. The one place
 * in the library's surface that selects a KIND OF PROCESSING rather than a
 * pixel format is NvIspSetConfiguration — mode 1 carries sixteen stage
 * selectors, and mode 2 is sent for the sensor pipeline and skipped for the
 * YUV one. If the demosaic is switched on anywhere reachable, it is there.
 *
 * This program does that and nothing else. Hardware state belongs to the
 * block, not to a channel, so a frame submitted afterwards by the reprocess
 * tool runs on whatever configuration this left behind. --hold keeps the
 * library's handles open meanwhile, in case its teardown undoes the setup.
 *
 * Build: tools/ispcfg/build-ispcfg.sh (on the build server)
 * Usage: ./ispcfg [--w0=N] [--mode1-only] [--yuv] [--hold=SECONDS]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>

/* The selector sets the camera stack sends, as read out of libnvmm.
 * The sensor pipeline fills twelve selectors; the YUV one fills four and
 * skips mode 2 entirely -- that difference is the whole experiment. */
static const uint32_t CFG_PRIMARY[16] = {
    1, 7, 9, 0xa, 3, 0, 6, 8, 0x11, 0xf, 0xc, 0xe, 0xb, 0, 0x10, 0xd
};
static const uint32_t CFG_YUV[16] = {
    1, 0, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0xd
};

int main(int argc, char **argv)
{
    uint32_t w0 = 1;            /* call sites pass 1; 2 is the other branch */
    int mode1_only = 0, use_yuv = 0, hold = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--w0=", 5) == 0)          w0 = (uint32_t)strtoul(a + 5, 0, 0);
        else if (strcmp(a, "--mode1-only") == 0)  mode1_only = 1;
        else if (strcmp(a, "--yuv") == 0)         use_yuv = 1;
        else if (strncmp(a, "--hold=", 7) == 0)   hold = atoi(a + 7);
        else {
            printf("usage: %s [--w0=N] [--mode1-only] [--yuv] [--hold=SECONDS]\n",
                   argv[0]);
            return 1;
        }
    }

    void *rm = dlopen("libnvrm.so", RTLD_NOW);
    if (!rm) { printf("dlopen libnvrm.so: %s\n", dlerror()); return 1; }
    void *isp = dlopen("libnvisp_v3.so", RTLD_NOW);
    if (!isp) { printf("dlopen libnvisp_v3.so: %s\n", dlerror()); return 1; }

    int (*NvRmInit)(void **) = dlsym(rm, "NvRmInit");
    int (*NvRmOpen)(void **, uint32_t) = dlsym(rm, "NvRmOpen");
    int (*NvIspOpen)(void *, int, void **) = dlsym(isp, "NvIspOpen");
    int (*NvIspSetConfiguration)(void *, int, void *, unsigned *) =
        dlsym(isp, "NvIspSetConfiguration");

    printf("symbols: NvRmInit=%p NvRmOpen=%p NvIspOpen=%p SetConfiguration=%p\n",
           (void *)NvRmInit, (void *)NvRmOpen, (void *)NvIspOpen,
           (void *)NvIspSetConfiguration);
    if (!NvIspOpen || !NvIspSetConfiguration) {
        printf("libnvisp_v3 does not export what we need -- stopping\n");
        return 1;
    }

    /* Old NvRm exposes both spellings depending on the build; take whichever
     * is there rather than guessing which one this device ships. */
    void *hRm = 0;
    int rc = -1;
    if (NvRmInit)       rc = NvRmInit(&hRm);
    else if (NvRmOpen)  rc = NvRmOpen(&hRm, 0);
    printf("NvRm init: rc=%d handle=%p\n", rc, hRm);
    if (rc != 0 || !hRm) { printf("no NvRm handle -- stopping\n"); return 1; }

    void *hIsp = 0;
    rc = NvIspOpen(hRm, 1, &hIsp);          /* instance 1 = ISP-A */
    printf("NvIspOpen(instance=1): rc=%d handle=%p\n", rc, hIsp);
    if (rc != 0 || !hIsp) { printf("no ISP handle -- stopping\n"); return 1; }

    uint32_t cfg1[16];
    memcpy(cfg1, use_yuv ? CFG_YUV : CFG_PRIMARY, sizeof cfg1);
    cfg1[0] = w0;
    unsigned sz = sizeof cfg1;
    rc = NvIspSetConfiguration(hIsp, 1, cfg1, &sz);
    printf("SetConfiguration mode=1 (%s, w0=%u): rc=%d\n",
           use_yuv ? "YUV" : "primary", w0, rc);

    if (!mode1_only && !use_yuv) {
        uint32_t cfg2 = 2;
        sz = sizeof cfg2;
        rc = NvIspSetConfiguration(hIsp, 2, &cfg2, &sz);
        printf("SetConfiguration mode=2: rc=%d\n", rc);
    } else {
        printf("SetConfiguration mode=2: skipped\n");
    }

    if (hold > 0) {
        printf("holding the handles open for %d s -- run the frame now\n", hold);
        fflush(stdout);
        sleep(hold);
    }
    printf("=== done ===\n");
    return 0;
}
